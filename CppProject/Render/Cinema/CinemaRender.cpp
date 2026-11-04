#include "CinemaRender.hpp"
#include "CinemaOracle.hpp"

#include "CinemaDenoise.hpp"
#include "CinemaGpu.hpp"
#include "CinemaGpu12.hpp"
#include "CinemaKernelM0.hpp"
#include "CinemaTrimSource.hpp"  // TRIM: composicion del kernel por conjunto de efectos
#include "CinemaScene.hpp"

#include "Asset/Asset.hpp"
#include "Asset/Surface.hpp"
#include "Render/FrameBuffer.hpp"
#include "Render/GraphicsApiHandler.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QImage>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// CinemaRender: implementacion (contrato en CinemaRender.hpp). Despacha el
// kernel M0 de CinemaKernelM0.hpp sobre la escena subida por CinemaScene.
//
// Politica de errores (misma que CinemaScene/CinemaGpu): nunca crashear.
// Todo fallo emite WARNING y deja un codigo estable en el estado.

namespace CppProject
{
	namespace
	{
		// -----------------------------------------------------------------
		// cbuffer PTFrame: espejo CPU byte a byte del cbuffer HLSL de
		// CinemaKernelM0.hpp (272 B exactos). Solo float4x4/float4/uint4:
		// ningun escalar suelto, ningun miembro cruza registro de 16 B.
		// -----------------------------------------------------------------
		struct PTFrameCpu
		{
			float invViewProj[16];      //   0: inversa de view_proj, memcpy TAL CUAL (sin transponer)
			float camPos[4];            //  64: xyz camara + near
			float sunDirCosAng[4];      //  80: xyz HACIA el sol (unitaria) + cos(radio angular)
			float sunRadiance[4];       //  96: radiancia del DISCO solar (dividida por omega) + far
			float skyZenith[4];         // 112: color lineal del cenit
			float skyHorizon[4];        // 128: color lineal del horizonte
			float ambientNightAlpha[4]; // 144: ambient lineal (suelo del domo) + night_alpha
			float fogParams0[4];        // 160: R12 niebla -> fogColor lineal (horizonte) + fogDistance
			float fogParams1[4];        // 176: R12 niebla -> fogSize, fogHeight, show (0/1), 0
			uint32_t counts[4];         // 192: numLights (M1), numEmiTris (M1), maxBounces, flagsFrame FRAMEF_*
			uint32_t tile[4];           // 208: offset x, offset y del tile (ABSOLUTO, en pixeles), sampleIdx, semilla
			float resClamp[4];          // 224: width, height, clamp de indirecta (0 = off), escala de emisivos
			float dofParams[4];         // 240: R6 DoF -> focusDist (mundo), radio de lente (mundo), dofOn (0/1), umbral adaptive
			float fogParams2[4];        // 256: R12 niebla -> fogColorSun lineal (lobulo Mie) + gamma del espacio de mezcla
			float lensParams[4];        // 272: DOF-FIEL A1 -> palas (0/<3 disco), rotacion (rad), ratio anamorfico, bias apodizacion
			float camFwd[4];            // 288: DOF-FIEL A2 -> eje optico unitario (xyz) + reserva (w)
			float bandParams[4];        // 304: CAPAS SLICING -> t0, t1 (0 = +inf), on (0/1), reserva
			// TRIM: el tiempo del efecto viaja como CONSTANTE de frame. No
			// como contenido de textura: la firma de escena hashea texId, asi
			// que regenerar texturas por frame dispararia el rebuild de
			// BVH/atlas (~430 ms/frame). Es P5 escrito en el layout.
			float trimTime[4];          // 320: tick0, uTime, loopL, sceneLight
			float trimNoise[4];         // 336: rect del ruido en su slice del atlas
			float trimNoiseSize[4];     // 352: noiseSize (escalar), slice, trim_emissive, bodyScale
		};

		static_assert(sizeof(PTFrameCpu) == CinemaKernelM0::PTFRAME_SIZE, "PTFrame must be 368 bytes (kernel contract)");
		static_assert(offsetof(PTFrameCpu, camPos) == 64, "PTFrame camPos offset");
		static_assert(offsetof(PTFrameCpu, sunRadiance) == 96, "PTFrame sunRadiance offset");
		static_assert(offsetof(PTFrameCpu, counts) == 192, "PTFrame counts offset");
		static_assert(offsetof(PTFrameCpu, tile) == 208, "PTFrame tile offset");
		static_assert(offsetof(PTFrameCpu, resClamp) == 224, "PTFrame res_clamp offset");
		static_assert(offsetof(PTFrameCpu, dofParams) == 240, "PTFrame dofParams offset");
		static_assert(offsetof(PTFrameCpu, fogParams2) == 256, "PTFrame fogParams2 offset");
		static_assert(offsetof(PTFrameCpu, lensParams) == 272, "PTFrame lensParams offset");
		static_assert(offsetof(PTFrameCpu, camFwd) == 288, "PTFrame camFwd offset");
		static_assert(offsetof(PTFrameCpu, bandParams) == 304, "PTFrame bandParams offset");
		static_assert(offsetof(PTFrameCpu, trimTime) == 320, "PTFrame trimTime offset");
		static_assert(offsetof(PTFrameCpu, trimNoise) == 336, "PTFrame trimNoise offset");
		static_assert(offsetof(PTFrameCpu, trimNoiseSize) == 352, "PTFrame trimNoiseSize offset");

		// -----------------------------------------------------------------
		// Constantes del host
		// -----------------------------------------------------------------

		// Tile de dispatch anti-TDR (diseno 2.4). RONDA 7 (perf): el resultado
		// por pixel depende SOLO de pixelAbs = tile.xy + dtid (CSPathTrace de
		// CinemaKernelM0), NUNCA del tamano de tile -> cualquier particion es
		// bit-identica. El 256px + Flush cada 8 original era anti-TDR
		// ultra-conservador: 40 dispatches y ~5 drenajes de pipeline por sample
		// a 1080p, y cada dispatch al MISMO acumulador UAV mete una barrera
		// automatica del driver D3D11 (burbuja de GPU). Tiles mas grandes =
		// muchas menos barreras/burbujas sin tocar la matematica. Presupuesto
		// aproximado por TIEMPO via pixeles: ~1 Mpix/tile (medido ~50 ns/pix en
		// una RTX 4060 a 3 bounces -> ~50 ms/tile, 40x bajo el TDR de 2 s) y
		// Flush por presupuesto de pixeles encolados.
		const IntType CINEMA_TILE_SIZE_DEFAULT = 1024;   // 1 Mpix/tile
		const IntType CINEMA_FLUSH_MPX_DEFAULT = 4;      // Flush cada ~4 Mpix

		// Overrides opcionales por variable de entorno, SOLO para el perfilado
		// A/B de la ronda 7 (MI3_CINEMA_TILE = lado del tile en px;
		// MI3_CINEMA_FLUSHMPX = Mpix entre Flush). Ausente/0 = default. No
		// alteran la matematica: solo el particionado (bit-identico). Cacheado.
		IntType CinemaEnvInt(const char* name, IntType def)
		{
			const char* v = std::getenv(name);
			if (!v || !*v)
				return def;
			long long n = atoll(v);
			return n > 0 ? (IntType)n : def;
		}

		// M6: override del backend de trazado por variable de entorno
		// MI3_CINEMA_BACKEND = dx11 | dxr. Devuelve: 0 = auto (default de fase B:
		// DXR cuando esta disponible, si no D3D11), 1 = forzar DXR, -1 = forzar
		// D3D11. Para A/B y debugging.
		IntType CinemaBackendOverride()
		{
			const char* v = std::getenv("MI3_CINEMA_BACKEND");
			if (!v || !*v)
				return 0;
			if (!strcmp(v, "dxr") || !strcmp(v, "DXR") || !strcmp(v, "dx12") || !strcmp(v, "d3d12"))
				return 1;
			if (!strcmp(v, "dx11") || !strcmp(v, "DX11") || !strcmp(v, "d3d11"))
				return -1;
			return 0;
		}
		// R9 (FIX 1/5): backend de trazado que ELEGIRA el proximo Begin. Prediccion
		// estable (mismo criterio que CinemaRenderBegin) para (a) el defer del BVH
		// del preview (CinemaSceneBuild solo salta el arbol si sera DXR) y (b) el
		// presupuesto de batching del export (pt_backend). DXR si el device esta
		// disponible y no se forzo dx11 por env; si no, D3D11.
		BoolType CinemaBackendWantsDxrImpl()
		{
			if (CinemaBackendOverride() == -1)
				return false;
			return CinemaGpu12Available(nullptr);
		}

		// FIX-A (sun_softness): el radio angular del sol YA NO es una constante.
		// Viene de background_sunlight_angle por pt_env_set y lo sanea/deriva
		// CinemaSceneEnvSet (env.sunAngleDeg = DIAMETRO en grados, env.sunCosAng,
		// env.sunOmega). Este valor solo queda como FALLBACK para el env invalido
		// (pt_env_set nunca llamado): 2 grados de RADIO era el M0 historico, asi
		// que una escena sin entorno sigue viendose exactamente igual que antes.
		const float CINEMA_SUN_ANGLE_DEG = 2.0f;
		// Cotas de resolucion: dimension maxima de recurso D3D11 y 64 Mpix
		// (3 buffers float4 de 64 Mpix = 3 GB de VRAM: siempre error antes)
		const IntType CINEMA_MAX_DIM = 16384;
		const IntType CINEMA_MAX_PIXELS = 64ll * 1024 * 1024;

		const float CINEMA_PI = 3.14159265358979f;

		// Nivel de exposicion base del Cinema (knob de calibracion de escena,
		// M2). El AgX del raster lleva un ancla fija x1.7 porque sus unidades
		// de luz son display-tuned ("sun strength 1" = brillo pleno) y quedan
		// ~2/3 EV bajas al linealizar; aqui la radiancia del acumulador es
		// fisica de verdad, asi que NO se aplica ese ancla. Si el primer A/B
		// contra el raster sale oscuro, se calibra SUBIENDO este unico factor
		// de escena (no el exposure del usuario, que sigue siendo su knob
		// multiplicador lineal). Solo afecta al camino AgX (tonemap 1): el
		// resolve sRGB simple (tonemap 0) queda intacto para los goldens
		// M0/M1 y el arnes de paridad.
		const float CINEMA_EXPOSURE_BASE = 1.0f;

		// -----------------------------------------------------------------
		// Estado del render
		// -----------------------------------------------------------------
		// DIAGNOSTICO (G4): ver CinemaRenderSetEmiCdf.
		BoolType g_emiCdfOn = true;

		struct RenderState
		{
			QString state = "idle"; // idle|ready|sampling|finished|error
			QString error;
			IntType width = 0, height = 0;
			uint32_t seed = 0;
			IntType sampleIdx = 0; // samples acumulados desde el ultimo Begin
			RealType msLastSample = 0.0;
			RealType msFinish = 0.0;
			// R10 (FIX 1): coste del ultimo CinemaRenderBegin (reset de camara). Con
			// buffers ya reservados y escena cacheada es solo CSClear + contadores;
			// el preview interactivo lo lee (pt_ms_begin) para verificar el reset
			// BARATO (<2 ms objetivo). El primer Begin de una acumulacion incluye la
			// compilacion lazy de kernels; los siguientes (mover camara) no.
			RealType msBegin = 0.0;
			QString denoiser = "off"; // motor usado en el ultimo Finish: oidn|atrous|off
			RealType msDenoise = 0.0;
			IntType tonemap = 0;        // look de color del ultimo Finish (0..4)
			RealType saturation = 1.0;  // saturacion post-tonemap del ultimo Finish

			// FONDO DE PELICULA: copia EXACTA del criterio con el que se encendio
			// FRAMEF_BACKDROP en el ultimo sample. El resolve compone mirando
			// ESTE flag y no uno recalculado, para que kernel y resolve no puedan
			// discrepar nunca (si discreparan, el fondo saldria negro o el cielo
			// saldria sin componer). Lo fija CinemaRenderSample, no FillFrame: el
			// pase de sonda de profundidad tambien llama a FillFrame y lo haria
			// mentir.
			BoolType backdropOn = false;

			// M7 adaptive sampling: umbral de ruido del render en curso (0 = OFF).
			// adaptive = true si alguna llamada a Sample lo activo (> 0): en ese
			// caso el resolve divide por el CONTEO POR PIXEL (gVarCount.z); con
			// adaptive false se usa el spp global (camino BIT-IDENTICO). activePixels
			// = pixeles NO convergidos del ultimo sondeo (CinemaRenderActiveCount).
			BoolType adaptive = false;
			RealType noiseThreshold = 0.0;
			IntType activePixels = -1;

			// M6 fase A: backend de trazado activo para el render EN CURSO. Lo fija
			// CinemaRenderBegin (D3D12/DXR si esta disponible y elegido, si no
			// D3D11). Sample/Finish/Parity enrutan readback/dispatch por este flag.
			// El camino D3D11 queda byte a byte identico cuando es false.
			BoolType useDxr = false;

			// DOF-FIEL D1: resultado del ultimo RunAutofocusProbe, consultable via
			// pt_dof_info() (op headless cinema.dof). dofMode vacio = nunca corrio
			// un autofocus en este proceso (DoF off o manual). mode: region|frame|
			// aabb|off.
			QString dofMode;
			RealType dofFocus = 0.0;
			RealType dofAperture = 0.0;
			IntType dofRegionHits = 0;
			IntType dofFrameHits = 0;

		#if API_D3D11
			// Kernels y recursos fijos: compilados/creados UNA vez (lazy) y
			// reutilizados entre renders (el fuente HLSL es constante)
			ID3D11ComputeShader* csPathTrace = nullptr;
			ID3D11ComputeShader* csClear = nullptr;
			// TRIM: clave del CONJUNTO de efectos con el que se compilaron los
			// kernels de arriba. 0 = ninguno (el fuente de siempre). Si el
			// conjunto de la escena cambia, hay que recompilar.
			uint64_t trimKey = 0;
			ID3D11SamplerState* sampPoint = nullptr;
			ID3D11Buffer* cbFrame = nullptr;
			// RONDA 7: query de evento para drenar la GPU al final de un batch
			// multi-spp (auto-pacing; ver CinemaRenderSample). Best-effort: si
			// no se pudo crear, se cae a solo Flush.
			ID3D11Query* syncEvent = nullptr;

			// Recursos por resolucion. Los tres UAVs son buffers
			// estructurados float4 de w*h elementos, pix = y * width + x
			// (CinemaKernelM0.hpp: typed UAV load de float4 es ilegal en
			// D3D11 FL11, NO son texturas)
			ID3D11Buffer* accum = nullptr;
			ID3D11Buffer* aovAlbedo = nullptr;
			ID3D11Buffer* aovNormalDepth = nullptr;
			ID3D11UnorderedAccessView* accumUav = nullptr;
			ID3D11UnorderedAccessView* aovAlbedoUav = nullptr;
			ID3D11UnorderedAccessView* aovNormalDepthUav = nullptr;
			// M7 adaptive: buffer estructurado float4 por pixel (u3): x = media de
			// luminancia (Welford), y = M2, z = conteo de samples del pixel, w =
			// marca de convergencia. Entra en el MISMO kernel (fuente compartida).
			ID3D11Buffer* varCount = nullptr;
			ID3D11UnorderedAccessView* varCountUav = nullptr;
		#endif
		};

		RenderState render;

		// CAPAS SLICING: cache del ULTIMO mapa de profundidad guardado (mismas
		// distancias, mismo near/far que el _depth.json que se acaba de
		// escribir). Es la UNICA fuente de los cortes y de las estadisticas del
		// manifest: por construccion no puede haber inconsistencia de unidades.
		std::vector<float> depthMapDist;   // por pixel, -1 = cielo
		IntType depthMapW = 0, depthMapH = 0;
		float   depthMapNear = 0.f, depthMapFar = 0.f, depthMapFarPlane = 0.f;

		// CAPAS N: percentil determinista (std::nth_element) sobre una copia
		// mutable del vector de distancias. p en [0,1]; el vector queda
		// parcialmente reordenado tras la llamada. Duplicado LITERAL de
		// CinemaScene.cpp (esta en un anonymous namespace de ese .cpp, no
		// visible desde aqui).
		float CapasPercentile(std::vector<float>& v, float p)
		{
			if (v.empty())
				return 0.f;
			IntType n = (IntType)v.size();
			IntType idx = (IntType)(p * (float)(n - 1) + 0.5f);
			if (idx < 0) idx = 0;
			if (idx > n - 1) idx = n - 1;
			std::nth_element(v.begin(), v.begin() + (size_t)idx, v.end());
			return v[(size_t)idx];
		}

		// -----------------------------------------------------------------
		// Helpers
		// -----------------------------------------------------------------

		RealType MsSince(const std::chrono::steady_clock::time_point& t)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
		}

		inline float Clamp01f(float v)
		{
			return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
		}

		// Encode sRGB exacto (inversa del SrgbToLinear de CinemaScene):
		// c <= 0.0031308 -> 12.92 * c; si no -> 1.055 * c^(1/2.4) - 0.055.
		// Version float [0,1] (para poder aplicar la saturacion post-tonemap
		// ANTES de cuantizar); EncodeSrgb8 = esta + cuantizacion.
		inline float EncodeSrgbF(float linear)
		{
			float c = Clamp01f(linear);
			float srgb = (c <= 0.0031308f) ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
			return Clamp01f(srgb);
		}

		inline uchar EncodeSrgb8(float linear)
		{
			return (uchar)(EncodeSrgbF(linear) * 255.f + 0.5f);
		}

		// Inversa exacta de EncodeSrgbF (EOTF sRGB). La usa el OVER del FONDO DE
		// PELICULA: componer se hace EN LUZ, no en codigos de pantalla. Mezclar
		// codigos oscurece de mas -- una sombra que tapa el 50 % de la luz sobre
		// un fondo 247 sale 187 en lineal y 123 en codigos, que son dos imagenes
		// distintas. Es tambien lo que hace cualquier compositor serio.
		inline float DecodeSrgbF(float srgb)
		{
			float c = Clamp01f(srgb);
			return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		// Hombro suave "sRGB shoulder" (tonemap 3): hombro exponencial C1 con
		// rodilla t = 0.5.
		//   x <= t: IDENTIDAD EXACTA (los medios [0, 0.5] lineal quedan
		//           byte a byte como el sRGB simple; el Reinhard extendido
		//           que hubo aqui antes hundia los medios: -19% en 0.25,
		//           -31% en 0.5, inaceptable).
		//   x > t:  x' = 1 - (1-t) * exp((t-x)/(1-t)).
		// Continuidad: f(t) = t y f'(t) = 1 (C1 en la rodilla, sin codo
		// visible). Monotona SIEMPRE: f' = exp((t-x)/(1-t)) > 0. Acotada
		// por 1 asintoticamente: NUNCA supera 1, ni siquiera antes del
		// encode (nada que recortar). f(1) = 0.816, f(2) = 0.975,
		// f(4) = 0.9995 (ya cuantiza a 255: blanco efectivo ~2 stops sobre
		// gris medio, como el punto de blanco w = 4 del diseno original).
		// Se aplica POR CANAL (no sobre luma) a proposito: bajo la rodilla
		// conserva EXACTA la saturacion del sRGB simple (identidad) y en
		// las altas cada canal rueda a 1, o sea el highlight rueda a
		// BLANCO (desaturacion solo en highlights: comportamiento filmico
		// correcto, no desatura antes de tiempo). Entrada: lineal ya
		// multiplicado por exposure; la salida se encoda sRGB.
		inline float ShoulderCurve(float x)
		{
			if (x < 0.f)
				x = 0.f;
			const float t = 0.5f;
			if (x <= t)
				return x;
			return 1.f - (1.f - t) * std::exp((t - x) / (1.f - t));
		}

		// Soft-clip por canal (auxiliar del tonemap 4, hombro preservando
		// color): identidad exacta hasta 0.98 y hombro exponencial C1
		// asintotico a 1 por encima:
		//   x <= 0.98: x
		//   x >  0.98: 0.98 + 0.02 * (1 - exp((0.98 - x) / 0.02))
		// Continuidad C1 en 0.98 (f(0.98) = 0.98, f'(0.98) = 1), monotona
		// (f' = exp((0.98-x)/0.02) > 0) y NUNCA supera 1. Solo recorta el
		// exceso extremo que deja el escalado por luma del tonemap 4 (canales
		// > 1 en colores saturados brillantes): la desaturacion que introduce
		// es minima y confinada al ultimo 2 por ciento del rango.
		inline float SoftClipChannel(float x)
		{
			const float k = 0.98f;
			if (x <= k)
				return x;
			return k + 0.02f * (1.f - std::exp((k - x) / 0.02f));
		}

		// Saturacion clasica post-tonemap (M4, look de color): en espacio
		// display [0,1], luma Rec.709, c' = luma + (c - luma) * saturation,
		// clamp [0,1]. saturation = 1 es identidad EXACTA via early-out (no
		// altera ni un byte: preserva los goldens de todos los tonemaps).
		// OJO: el early-out NO es cosmetico -- en float, luma + (c - luma)
		// no siempre re-redondea al c original, y a un LSB de una frontera
		// de cuantizacion eso cambiaria un byte. Con sat > 1 un canal puede
		// salirse de [0,1]; el clamp es POR CANAL: en el borde del gamut
		// puede desplazar el hue un pelo (aceptable y estandar para un
		// control de saturacion display-referred; un clamp que preserve
		// hue exigiria proyectar hacia la luma y aplanaria highlights).
		// El alpha no se toca. Comun a los cinco tonemaps.
		inline void ApplySaturation(float& r, float& g, float& b, float sat)
		{
			if (sat == 1.f)
				return;
			float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			r = Clamp01f(luma + (r - luma) * sat);
			g = Clamp01f(luma + (g - luma) * sat);
			b = Clamp01f(luma + (b - luma) * sat);
		}

		// -----------------------------------------------------------------
		// AgX (M2): aproximacion "AgX minimal" de Troy Sobotka (variante
		// Benjamin Wrensch / iolite, la que aproxima el view transform
		// default de Blender 4.x). MISMAS matrices inset/outset, mismo
		// encode log2 sobre 16.5 stops y mismo polinomio sigmoide que el
		// raster next-gen: porte 1:1 de agxInset/agxContrast/agxOutset de
		// GmProject/shaders/shader_high_samples_unpack/
		// shader_high_samples_unpack.fsh (mantener AMBOS sincronizados).
		// Entrada: Rec.709 lineal referido a escena, sin clamp superior.
		// Salida: display-encoded 2.2 (NO aplicar OETF sRGB encima).
		// -----------------------------------------------------------------

		// Polinomio de 6o orden de la sigmoide de contraste AgX
		inline float AgxContrast(float x)
		{
			float x2 = x * x;
			float x4 = x2 * x2;
			return 15.5f * x4 * x2
				- 40.14f * x4 * x
				+ 31.96f * x4
				- 6.868f * x2 * x
				+ 0.4298f * x2
				+ 0.1191f * x
				- 0.00232f;
		}

		// Encode log2 de un canal ya inset: clamp a [min_ev, max_ev],
		// normalizacion a [0,1] y sigmoide
		inline float AgxLog2Channel(float v)
		{
			const float minEv = -12.47393f;
			const float maxEv = 4.026069f;
			v = std::log2(v < 1.0e-10f ? 1.0e-10f : v);
			v = v < minEv ? minEv : (v > maxEv ? maxEv : v);
			v = (v - minEv) / (maxEv - minEv);
			return AgxContrast(v);
		}

		// Look "Punchy" AgX (M4): porte 1:1 de agxLookPunchy del raster
		// (shader_high_samples_unpack.fsh) -- version domada 1.3/1.3, no la
		// 1.35/1.4 de Blender. Se aplica sobre el color ya inset+sigmoide,
		// ENTRE la sigmoide y el outset, exactamente donde el shader lo hace
		// cuando uTonemapper == 4: pow 1.3 por canal (clamp a 0) y realce de
		// saturacion 1.3 en torno a la luma Rec.709 del mismo espacio outset.
		inline void AgxLookPunchy(float& r, float& g, float& b)
		{
			float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			float pr = std::pow(r < 0.f ? 0.f : r, 1.3f);
			float pg = std::pow(g < 0.f ? 0.f : g, 1.3f);
			float pb = std::pow(b < 0.f ? 0.f : b, 1.3f);
			r = luma + 1.3f * (pr - luma);
			g = luma + 1.3f * (pg - luma);
			b = luma + 1.3f * (pb - luma);
		}

		// Transform AgX completo in-place: inset -> log2+sigmoide -> [look
		// Punchy opcional] -> outset. Los coeficientes de matriz son
		// IDENTICOS a los del shader raster (expandidos, sin mat3). punchy =
		// false replica el look Base (tonemap 1, byte a byte como M2); punchy
		// = true intercala AgxLookPunchy entre sigmoide y outset (tonemap 2),
		// igual que el shader con uTonemapper == 4.
		inline void AgxApply(float& r, float& g, float& b, BoolType punchy)
		{
			// Matriz inset
			float ir = r * 0.842479062253094f  + g * 0.0784335999999992f + b * 0.0792237451477643f;
			float ig = r * 0.0423282422610123f + g * 0.878468636469772f  + b * 0.0791661274605434f;
			float ib = r * 0.0423756549057051f + g * 0.0784336f          + b * 0.879142973793104f;

			ir = AgxLog2Channel(ir);
			ig = AgxLog2Channel(ig);
			ib = AgxLog2Channel(ib);

			// Look "Punchy" (tonemap 2): entre sigmoide y outset, igual que
			// agxLookPunchy del raster cuando uTonemapper == 4
			if (punchy)
				AgxLookPunchy(ir, ig, ib);

			// Matriz outset (inversa del inset)
			r = ir *  1.19687900512017f   + ig * -0.0980208811401368f + ib * -0.0990297440797205f;
			g = ir * -0.0528968517574562f + ig *  1.15190312990417f   + ib * -0.0989611768448433f;
			b = ir * -0.0529716355144438f + ig * -0.0980434501171241f + ib *  1.15107367264116f;
		}

	#if API_D3D11

		UINT GroupsFor(IntType pixels)
		{
			return (UINT)((pixels + (IntType)CinemaKernelM0::THREADGROUP_SIZE - 1)
				/ (IntType)CinemaKernelM0::THREADGROUP_SIZE);
		}

		void ReleaseTargets()
		{
			releaseAndReset(render.accumUav);
			releaseAndReset(render.aovAlbedoUav);
			releaseAndReset(render.aovNormalDepthUav);
			releaseAndReset(render.varCountUav);
			releaseAndReset(render.accum);
			releaseAndReset(render.aovAlbedo);
			releaseAndReset(render.aovNormalDepth);
			releaseAndReset(render.varCount);
			render.width = 0;
			render.height = 0;
		}

		// Compila los dos entry points del kernel M0 y crea el sampler point
		// y el cbuffer PTFrame. Lazy: solo la primera vez (o al reintentar
		// tras un fallo parcial). Los kernels sobreviven entre renders.
		BoolType EnsureKernels()
		{
			// TRIM (D10): el kernel se COMPONE con solo los efectos que la
			// escena usa. Conjunto vacio -> CinemaKernelM0::SOURCE tal cual ->
			// el texto es byte a byte el de siempre. Si el conjunto cambia
			// (editar el efecto de una pieza), hay que RECOMPILAR: es el coste
			// nuevo que introduce la especializacion, y el gate lo rinde.
			uint64_t trimKey = CinemaTrimSetKey(CinemaSceneTrimSlots());
			if (trimKey != render.trimKey && render.csPathTrace)
			{
				releaseAndReset(render.csPathTrace);
				releaseAndReset(render.csClear);
			}
			render.trimKey = trimKey;
			const char* src = CinemaTrimComposeSource(CinemaSceneTrimSlots());

			if (!render.csPathTrace)
				render.csPathTrace = CinemaGpu::CreateCS(src,
					CinemaKernelM0::ENTRY_PATHTRACE, "cinema_m0_pathtrace");
			if (!render.csClear)
				render.csClear = CinemaGpu::CreateCS(src,
					CinemaKernelM0::ENTRY_CLEAR, "cinema_m0_clear");

			if (!render.sampPoint)
			{
				// s0 del kernel: MIN_MAG_MIP_POINT, address CLAMP (contrato
				// de bindings de CinemaKernelM0.hpp)
				D3D11_SAMPLER_DESC desc = {};
				desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
				desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
				desc.MaxLOD = D3D11_FLOAT32_MAX;
				HRESULT result = D3DDevice->CreateSamplerState(&desc, &render.sampPoint);
				if (FAILED(result))
				{
					WARNING("CreateSamplerState failed (HRESULT " + NumStr((IntType)result) + ")");
					render.sampPoint = nullptr;
				}
			}

			if (!render.cbFrame)
				render.cbFrame = CinemaGpu::CreateConstantBuffer(CinemaKernelM0::PTFRAME_SIZE, nullptr);

			// Query de evento para el auto-pacing del batch (best-effort: su
			// ausencia NO es fatal, el fallback es solo Flush).
			if (!render.syncEvent)
			{
				D3D11_QUERY_DESC qd = {};
				qd.Query = D3D11_QUERY_EVENT;
				HRESULT qr = D3DDevice->CreateQuery(&qd, &render.syncEvent);
				if (FAILED(qr))
				{
					WARNING("CreateQuery(EVENT) failed (HRESULT " + NumStr((IntType)qr) + ") -- batch usara solo Flush");
					render.syncEvent = nullptr;
				}
			}

			return render.csPathTrace && render.csClear && render.sampPoint && render.cbFrame;
		}

		// FONDO DE PELICULA: unico sitio donde se decide si el ajuste ESTA ACTIVO.
		// Lo consultan FillFrame (para el bit FRAMEF_BACKDROP) y CinemaRenderSample
		// (para dejarlo apuntado y que el resolve componga con el MISMO criterio).
		//   - Solo con film OPACO: con "Remove background" el alpha manda y el
		//     fondo no debe ensuciarlo ni tocar la sombra del catcher.
		//   - Solo con env valido: sin pt_env_set no hay ajuste que aplicar.
		//   - backdrop 0 = OFF = el motor de siempre, bit a bit.
		BoolType CinemaBackdropActive(BoolType alphaFilm)
		{
			const CinemaEnvState& e = CinemaSceneEnv();
			return !alphaFilm && e.valid && e.backdrop > 0;
		}

		// Llena el cbuffer PTFrame leyendo camara y entorno de CinemaScene.
		// tile.xy queda a 0: lo fija el bucle de tiles de cada sample.
		void FillFrame(PTFrameCpu& frame, IntType bounces, RealType clampIndirect,
			RealType emissiveScale, BoolType alphaFilm, RealType noiseThreshold)
		{
			const CinemaCameraState& cam = CinemaSceneCamera();
			const CinemaEnvState& env = CinemaSceneEnv();

			memset(&frame, 0, sizeof(frame));

			// Inversa de view_proj memcpy TAL CUAL, sin transponer (nota de
			// convencion de matriz en CinemaKernelM0.hpp)
			memcpy(frame.invViewProj, cam.invViewProj, sizeof(frame.invViewProj));
			frame.camPos[0] = cam.pos[0];
			frame.camPos[1] = cam.pos[1];
			frame.camPos[2] = cam.pos[2];
			frame.camPos[3] = cam.nearPlane;

			// R6: DoF fisico. dofOn/apertureRadius ya vienen saneados por
			// CinemaSceneCameraSet (apertureRadius 0 y dofOn 0 => rayo pinhole
			// identico byte a byte, el kernel salta el muestreo de lente).
			frame.dofParams[0] = cam.focusDist;
			frame.dofParams[1] = cam.apertureRadius;
			frame.dofParams[2] = cam.dofOn ? 1.f : 0.f;
			// M7 adaptive: umbral de ruido en dofParams.w (0 = OFF = bit-identico;
			// el kernel no hace early-out ni marca convergencia con 0).
			frame.dofParams[3] = noiseThreshold > 0.0 ? (float)noiseThreshold : 0.f;

			// DOF-FIEL A0/A1/A5: apertura poligonal. Con la camara por defecto
			// (blades=0, ratio=0, bias=0) es {0,0,0,0}: el kernel recorre
			// EXACTAMENTE el mismo camino que el disco uniforme de siempre.
			frame.lensParams[0] = cam.blades;
			frame.lensParams[1] = cam.lensRot;
			frame.lensParams[2] = cam.lensRatio;
			frame.lensParams[3] = cam.lensBias;
			// DOF-FIEL A0/A2: eje optico unitario (para refocalizar por PLANO).
			frame.camFwd[0] = cam.camFwd[0];
			frame.camFwd[1] = cam.camFwd[1];
			frame.camFwd[2] = cam.camFwd[2];
			frame.camFwd[3] = 0.f;

			// Sol: direccion re-normalizada (defensivo) + disco angular del ENTORNO
			// (FIX-A: antes era la constante CINEMA_SUN_ANGLE_DEG y sun_softness no
			// hacia nada -- dos renders con 0.526 y 8 salian byte a byte iguales).
			float sd[3] = { env.sunDir[0], env.sunDir[1], env.sunDir[2] };
			float len = std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
			if (len > 1.0e-6f)
			{
				sd[0] /= len;
				sd[1] /= len;
				sd[2] /= len;
			}
			else
			{
				sd[0] = 0.f;
				sd[1] = 0.f;
				sd[2] = 1.f;
			}
			// FIX-A: cosAng y omega los calcula UNA vez CinemaSceneEnvSet, con la
			// MISMA formula float que el kernel, y los lee TAMBIEN el oraculo: no
			// queda ninguna expresion duplicada que pueda desincronizarse (que era
			// el riesgo real para cinema.parity). Con env invalido (sin pt_env_set)
			// se cae al radio historico de 2 grados: escena sin entorno = pixel de
			// antes, byte a byte.
			float cosAng, omega;
			if (env.valid)
			{
				cosAng = env.sunCosAng;
				omega = env.sunOmega;
			}
			else
			{
				cosAng = std::cos(CINEMA_SUN_ANGLE_DEG * CINEMA_PI / 180.f);
				omega = 2.f * CINEMA_PI * (1.f - cosAng);
			}
			frame.sunDirCosAng[0] = sd[0];
			frame.sunDirCosAng[1] = sd[1];
			frame.sunDirCosAng[2] = sd[2];
			frame.sunDirCosAng[3] = cosAng;

			// La radiancia del env es la irradiancia efectiva del sol "delta"
			// del raster (color lineal x strength). El kernel integra un
			// disco de angulo solido omega y pondera la NEE por omega, asi
			// que aqui se DIVIDE por omega: la energia total del disco iguala
			// la del sol delta (contrato del cbuffer en CinemaKernelM0.hpp).
			// FIX-A: ... SALVO en el regimen DELTA. Cuando el disco es tan pequeno
			// que el kernel calcula omega <= 1e-6 (CinemaKernelM0.hpp:2673) cambia
			// de rama: sunWeight = 1 y sunRadiance se interpreta como la
			// IRRADIANCIA ya integrada. Dividir alli metia una radiancia de 1e8 en
			// el cbuffer -- con sun_softness = 0, cinema.parity daba rmse 107001.
			// El umbral es el MISMO 1e-6 que usan kernel y oraculo.
			// Puente display->radiometrico: en el raster "sun strength 1" es
			// dominante, pero radiometricamente E_sol=1 pierde contra el domo
			// de cielo (E_cielo ~ pi * radiancia_media ~ 1.5). En la realidad
			// el sol aporta 5-10x el cielo: escala fija en M0 (knob de
			// proyecto project_cinema_* en M4).
			const float CINEMA_SUN_IRRADIANCE_SCALE = 5.f;
			float sunDiv = (omega > 1.0e-6f) ? omega : 1.f;
			frame.sunRadiance[0] = env.sunRadiance[0] * CINEMA_SUN_IRRADIANCE_SCALE / sunDiv;
			frame.sunRadiance[1] = env.sunRadiance[1] * CINEMA_SUN_IRRADIANCE_SCALE / sunDiv;
			frame.sunRadiance[2] = env.sunRadiance[2] * CINEMA_SUN_IRRADIANCE_SCALE / sunDiv;
			frame.sunRadiance[3] = cam.farPlane;

			for (IntType c = 0; c < 3; c++)
			{
				frame.skyZenith[c] = env.skyZenith[c];
				frame.skyHorizon[c] = env.skyHorizon[c];
				frame.ambientNightAlpha[c] = env.ambient[c];
				frame.fogParams0[c] = env.fogColor[c];
				frame.fogParams2[c] = env.fogColorSun[c];
			}
			frame.ambientNightAlpha[3] = env.nightAlpha;

			// R12 niebla ATMOSFERICA (perspectiva aerea) EN EL KERNEL, sobre el
			// hit primario real: formula exacta del raster (shader_high_fog.fsh)
			// + inscatter bicolor del apply. fogParams2.w = gamma del espacio de
			// mezcla (2.2 = el espacio display en el que mezcla el raster; 0 =
			// mezcla lineal pura). Con show = 0 el kernel ni lo evalua.
			frame.fogParams0[3] = env.fogDistance;
			frame.fogParams1[0] = env.fogSize;
			frame.fogParams1[1] = env.fogHeight;
			frame.fogParams1[2] = env.fogShow ? 1.f : 0.f;
			// F5 (C1): la antigua reserva de fogParams1.w lleva ahora el peso del
			// grupo ENTORNO de la NEE (project_cinema_env_light, ya saneado por
			// CinemaSceneEnvSet). 0 = OFF -> el kernel no toca nada y el camino es
			// BIT-IDENTICO. Sin env valido (sin pt_env_set) tambien 0.
			frame.fogParams1[3] = env.valid ? env.envLight : 0.f;
			frame.fogParams2[3] = CinemaSceneFogMixGamma();

			// counts: el kernel M0 no muestrea luces ni emisivos por NEE; M1
			// pasara CinemaSceneLights().size() y el tamano de la CDF emisiva
			frame.counts[0] = CinemaSceneGetGpu().numLights;
			frame.counts[1] = g_emiCdfOn ? CinemaSceneGetGpu().numEmiTris : 0u;
			IntType b = bounces < 1 ? 1 : (bounces > 8 ? 8 : bounces);
			frame.counts[2] = (uint32_t)b;
			frame.counts[3] = alphaFilm ? CinemaKernelM0::FRAMEF_ALPHA_FILM : 0u;
			// R14: camara ORTOGRAFICA -> el kernel arranca el rayo primario en el
			// plano near del pixel en vez de en camPos (rayos paralelos). Sin el
			// flag el camino es BIT-IDENTICO al de siempre.
			if (cam.ortho)
				frame.counts[3] |= CinemaKernelM0::FRAMEF_ORTHO;
			// FONDO DE PELICULA: el bit SOLO se enciende con film OPACO. Con
			// "Remove background" (alphaFilm) el camino ya es holdout y el fondo
			// no debe ensuciar ni el alpha ni la sombra del catcher: con
			// transparent = 1 este flag NO LLEGA NUNCA al kernel, asi que el
			// render sale BIT-IDENTICO tenga el fondo encendido o apagado (esa es
			// la prueba de que el fondo no ilumina, y sale por construccion). Con
			// el ajuste en 0 (el default) tampoco llega: bit-identico a secas.
			if (CinemaBackdropActive(alphaFilm))
				frame.counts[3] |= CinemaKernelM0::FRAMEF_BACKDROP;

			// TRIM: constantes del efecto. El tiempo llega YA resuelto desde
			// GML como funcion pura del timeline (tick0 + uTime/3 = la MISMA
			// formula del raster), asi que es CONSTANTE durante toda la
			// acumulacion de samples de un frame -- que es lo que hace que dos
			// procesos distintos den el mismo PNG.
			{
				const CinemaTrimState& trim = CinemaSceneTrim();
				frame.trimTime[0] = trim.tick0;
				frame.trimTime[1] = trim.uTime;
				frame.trimTime[2] = trim.loopL;
				frame.trimTime[3] = trim.sceneLight;
				frame.trimNoise[0] = trim.noiseRect[0];
				frame.trimNoise[1] = trim.noiseRect[1];
				frame.trimNoise[2] = trim.noiseRect[2];
				frame.trimNoise[3] = trim.noiseRect[3];
				frame.trimNoiseSize[0] = trim.noiseSize;
				frame.trimNoiseSize[1] = (float)trim.noiseSlice;
				frame.trimNoiseSize[2] = trim.emissive;
				frame.trimNoiseSize[3] = trim.bodyScale;
				if (trim.ldrClip)
					frame.counts[3] |= CinemaKernelM0::FRAMEF_TRIM_LDR_CLIP;
			}

			frame.tile[0] = 0;
			frame.tile[1] = 0;
			frame.tile[2] = (uint32_t)render.sampleIdx;
			frame.tile[3] = render.seed;

			frame.resClamp[0] = (float)render.width;
			frame.resClamp[1] = (float)render.height;
			frame.resClamp[2] = clampIndirect > 0.0 ? (float)clampIndirect : 0.f;
			frame.resClamp[3] = emissiveScale > 0.0 ? (float)emissiveScale : 0.f;

			// CAPAS SLICING: ventana de profundidad de la pasada (OFF = ceros =
			// camino bit-identico). El pase de sonda (FRAMEF_DEPTH_PROBE) la
			// APAGA a proposito: el _depth.png es SIEMPRE de la escena completa.
			if (CinemaSceneBandWindowActive())
			{
				frame.bandParams[0] = CinemaSceneBandWindowT0();
				float t1 = CinemaSceneBandWindowT1();
				frame.bandParams[1] = (t1 >= 1.0e29f) ? 0.f : t1;
				frame.bandParams[2] = 1.f;
			}
		}

		// DOF-FIEL A3/Cambio 12: UN pase de sonda FRAMEF_DEPTH_PROBE (pinhole, sin
		// DoF, sin shading: solo el primer hit) que sirve a los DOS backends (DXR
		// y D3D11 compute) -- antes solo DXR tenia sonda (RunAutofocusProbeDxr) y
		// el D3D11 se quedaba con el foco de fallback de GML. NO toca el
		// acumulador: el llamador re-clarea si hace falta (ReclearAfterProbe).
		// aov queda con w = distancia RADIAL del primer hit (-1 = cielo).
	#if API_D3D11
		bool RunDepthProbe(std::vector<float>& aov, IntType w, IntType h)
		{
			aov.assign((size_t)w * h * 4, 0.f);

			PTFrameCpu pf;
			FillFrame(pf, 1, 0.0, 5.0, false, 0.0);
			pf.bandParams[0] = 0.f; pf.bandParams[1] = 0.f; pf.bandParams[2] = 0.f; // sonda SIN ventana
			pf.dofParams[2] = 0.f; // pinhole (sin DoF en la sonda)
			pf.counts[3] |= CinemaKernelM0::FRAMEF_DEPTH_PROBE;

			IntType tileSize = CinemaEnvInt("MI3_CINEMA_TILE", CINEMA_TILE_SIZE_DEFAULT);
			IntType flushBudgetPix = CinemaEnvInt("MI3_CINEMA_FLUSHMPX", CINEMA_FLUSH_MPX_DEFAULT) * 1024 * 1024;

			if (render.useDxr)
			{
				if (!CinemaGpu12Sample(&pf, 0, 1, tileSize, flushBudgetPix))
					return false;
				if (!CinemaGpu12Readback(nullptr, nullptr, aov.data()))
					return false;
				return true;
			}

			const CinemaSceneGpu& sceneGpu = CinemaSceneGetGpu();
			if (!sceneGpu.IsValid() || !render.csPathTrace || !render.cbFrame)
				return false;
			ID3D11ShaderResourceView* srvs[8] =
			{
				sceneGpu.vertsSrv, sceneGpu.trisSrv, sceneGpu.nodesSrv,
				sceneGpu.materialsSrv, sceneGpu.atlasSrv,
				sceneGpu.lightsSrv, sceneGpu.emiTriIdxSrv, sceneGpu.emiCdfSrv
			};
			ID3D11UnorderedAccessView* uavs[4] = { render.accumUav, render.aovAlbedoUav, render.aovNormalDepthUav, render.varCountUav };
			D3DContext->CSSetShader(render.csPathTrace, nullptr, 0);
			D3DContext->CSSetConstantBuffers(0, 1, &render.cbFrame);
			D3DContext->CSSetShaderResources(0, 8, srvs);
			D3DContext->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
			D3DContext->CSSetSamplers((UINT)CinemaKernelM0::SLOT_SAMPLER_POINT, 1, &render.sampPoint);
			for (IntType ty = 0; ty < h; ty += tileSize)
				for (IntType tx = 0; tx < w; tx += tileSize)
				{
					IntType tileW = w - tx < tileSize ? w - tx : tileSize;
					IntType tileH = h - ty < tileSize ? h - ty : tileSize;
					pf.tile[0] = (uint32_t)tx;
					pf.tile[1] = (uint32_t)ty;
					CinemaGpu::UpdateConstantBuffer(render.cbFrame, &pf, CinemaKernelM0::PTFRAME_SIZE);
					D3DContext->Dispatch(GroupsFor(tileW), GroupsFor(tileH), 1);
				}
			ID3D11ShaderResourceView* nullSrvs[8] = {};
			ID3D11UnorderedAccessView* nullUavs[4] = {};
			ID3D11Buffer* nullCb = nullptr;
			ID3D11SamplerState* nullSampler = nullptr;
			D3DContext->CSSetShaderResources(0, 8, nullSrvs);
			D3DContext->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
			D3DContext->CSSetConstantBuffers(0, 1, &nullCb);
			D3DContext->CSSetSamplers((UINT)CinemaKernelM0::SLOT_SAMPLER_POINT, 1, &nullSampler);
			D3DContext->CSSetShader(nullptr, nullptr, 0);
			if (!CinemaGpu::ReadbackBuffer(render.aovNormalDepth, aov.data(), (size_t)w * h * 4 * sizeof(float)))
				return false;
			return true;
		}

		// Re-clarea el frame tras el pase de sonda (que escribio el AOV/accum),
		// para que la acumulacion real arranque limpia (bit-identico a no
		// sondear). Equivalente a los dos caminos de Begin (CinemaGpu12Begin en
		// DXR, CSClear en D3D11).
		void ReclearAfterProbe(IntType w, IntType h)
		{
			if (render.useDxr)
			{
				CinemaGpu12Begin(w, h);
				return;
			}
			PTFrameCpu frame = {};
			frame.resClamp[0] = (float)w;
			frame.resClamp[1] = (float)h;
			CinemaGpu::UpdateConstantBuffer(render.cbFrame, &frame, CinemaKernelM0::PTFRAME_SIZE);
			ID3D11UnorderedAccessView* uavs[4] = { render.accumUav, render.aovAlbedoUav, render.aovNormalDepthUav, render.varCountUav };
			CinemaGpu::Dispatch(render.csClear, render.cbFrame, nullptr, 0, uavs, 4, GroupsFor(w), GroupsFor(h));
		}

		// DOF-FIEL A2: rayo PINHOLE (SIN jitter, centro de pixel) para (px,py),
		// misma convencion NDC que el kernel (x en [-1,1] izq->der, y en [-1,1]
		// abajo->arriba). Usado solo para convertir la distancia RADIAL de la
		// sonda a PLANAR (planar = t_radial * dot(dir, camFwd)); el jitter
		// subpixel es irrelevante a esta escala (mediana sobre una rejilla).
		void PinholeRayDirCpu(const CinemaCameraState& cam, IntType px, IntType py, IntType w, IntType h, float dir[3])
		{
			float ndcX = ((float(px) + 0.5f) / (float)w) * 2.f - 1.f;
			float ndcY = 1.f - ((float(py) + 0.5f) / (float)h) * 2.f;
			const float* m = cam.invViewProj;
			auto unproj4 = [m](float nx, float ny, float nz, float nw, float out[4])
			{
				out[0] = m[0] * nx + m[4] * ny + m[8] * nz + m[12] * nw;
				out[1] = m[1] * nx + m[5] * ny + m[9] * nz + m[13] * nw;
				out[2] = m[2] * nx + m[6] * ny + m[10] * nz + m[14] * nw;
				out[3] = m[3] * nx + m[7] * ny + m[11] * nz + m[15] * nw;
			};
			float pNear[4], pFar[4];
			unproj4(ndcX, ndcY, 0.f, 1.f, pNear);
			unproj4(ndcX, ndcY, 1.f, 1.f, pFar);
			float nx = pNear[0] / pNear[3], ny = pNear[1] / pNear[3], nz = pNear[2] / pNear[3];
			float fx = pFar[0] / pFar[3], fy = pFar[1] / pFar[3], fz = pFar[2] / pFar[3];
			float dx = fx - nx, dy = fy - ny, dz = fz - nz;
			float len = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (len > 1.0e-9f) { dir[0] = dx / len; dir[1] = dy / len; dir[2] = dz / len; }
			else { dir[0] = 0.f; dir[1] = 0.f; dir[2] = 1.f; }
		}

		// DOF-FIEL A3 (Cambio 13): cadena de fallback SIN constantes arbitrarias.
		// Reemplaza a RunAutofocusProbeDxr; sirve a los DOS backends via
		// RunDepthProbe. NIVEL 1 (region 12% central) -> NIVEL 2 (encuadre
		// completo) -> NIVEL 3 (AABB de la escena) -> apagar el DoF si no hay
		// NADA que enfocar. Guarda el resultado en render.dof* (consultable via
		// pt_dof_info / cinema.dof, D1).
		void RunAutofocusProbe(IntType w, IntType h)
		{
			const CinemaCameraState& cam = CinemaSceneCamera();
			if (!cam.dofOn || !cam.autofocus)
				return;

			QString mode = "off";
			float focus = 0.f;
			IntType regionHits = 0, frameHits = 0;

			std::vector<float> aov;
			if (RunDepthProbe(aov, w, h))
			{
				// NIVEL 1 (REGION): rejilla 16x16 sobre la ventana central de
				// +-6% del ancho/alto (= 12% x 12% del encuadre). Solo hits;
				// convertidos de RADIAL a PLANAR (A2) antes de la mediana.
				std::vector<float> regionPlanar;
				regionPlanar.reserve(256);
				IntType cx = w / 2, cy = h / 2;
				IntType hw = w * 6 / 100; if (hw < 1) hw = 1;
				IntType hh = h * 6 / 100; if (hh < 1) hh = 1;
				for (IntType j = 0; j < 16; j++)
				for (IntType i = 0; i < 16; i++)
				{
					IntType px = cx - hw + (IntType)((i + 0.5f) * (2.f * hw) / 16.f);
					IntType py = cy - hh + (IntType)((j + 0.5f) * (2.f * hh) / 16.f);
					if (px < 0) px = 0; if (px >= w) px = w - 1;
					if (py < 0) py = 0; if (py >= h) py = h - 1;
					float d = aov[((size_t)py * w + px) * 4 + 3];
					if (d > 0.f)
					{
						float dir[3];
						PinholeRayDirCpu(cam, px, py, w, h, dir);
						float planar = d * (dir[0] * cam.camFwd[0] + dir[1] * cam.camFwd[1] + dir[2] * cam.camFwd[2]);
						regionPlanar.push_back(planar);
					}
				}
				regionHits = (IntType)regionPlanar.size();
				if (regionHits >= 8)
				{
					std::sort(regionPlanar.begin(), regionPlanar.end());
					focus = regionPlanar[regionPlanar.size() / 2];
					mode = "region";
				}
				else
				{
					// NIVEL 2 (ESCENA): rejilla de paso fijo sobre TODO el
					// encuadre (maximo 64x64 muestras). Mediana de TODOS los
					// hits del primer sample (nunca range/3).
					std::vector<float> framePlanar;
					framePlanar.reserve(64 * 64);
					IntType stepX = w / 64; if (stepX < 1) stepX = 1;
					IntType stepY = h / 64; if (stepY < 1) stepY = 1;
					for (IntType py = 0; py < h; py += stepY)
					for (IntType px = 0; px < w; px += stepX)
					{
						float d = aov[((size_t)py * w + px) * 4 + 3];
						if (d > 0.f)
						{
							float dir[3];
							PinholeRayDirCpu(cam, px, py, w, h, dir);
							float planar = d * (dir[0] * cam.camFwd[0] + dir[1] * cam.camFwd[1] + dir[2] * cam.camFwd[2]);
							framePlanar.push_back(planar);
						}
					}
					frameHits = (IntType)framePlanar.size();
					if (frameHits >= 8)
					{
						std::sort(framePlanar.begin(), framePlanar.end());
						focus = framePlanar[framePlanar.size() / 2];
						mode = "frame";
					}
				}
			}

			if (mode == "off")
			{
				// NIVEL 3 (GEOMETRIA): sin ningun hit en pantalla o sin sonda
				// -> centro del AABB de la escena proyectado sobre el eje optico.
				const CinemaBvh& bvh = CinemaSceneGetBvh();
				if (bvh.IsValid() && !bvh.nodes.empty())
				{
					const PTNode& root = bvh.nodes[0];
					float ccx = 0.5f * (root.bminx + root.bmaxx);
					float ccy = 0.5f * (root.bminy + root.bmaxy);
					float ccz = 0.5f * (root.bminz + root.bmaxz);
					float d = (ccx - cam.pos[0]) * cam.camFwd[0] + (ccy - cam.pos[1]) * cam.camFwd[1] + (ccz - cam.pos[2]) * cam.camFwd[2];
					if (d > cam.nearPlane && d < cam.farPlane)
					{
						focus = d;
						mode = "aabb";
					}
				}
			}

			// Siempre re-clarear (el pase de sonda escribio el AOV/accum), sea
			// cual sea el desenlace: la acumulacion real debe arrancar limpia.
			ReclearAfterProbe(w, h);

			float aper = 0.f;
			if (mode == "off")
			{
				CinemaSceneCameraDisableDof();
				WARNING("Cinema autofocus: sin geometria visible -> DoF desactivado en este render");
			}
			else
			{
				CinemaSceneCameraApplyAutofocus((RealType)focus);
				aper = CinemaSceneCamera().apertureRadius;
			}

			render.dofMode = mode;
			render.dofFocus = (RealType)focus;
			render.dofAperture = (RealType)aper;
			render.dofRegionHits = regionHits;
			render.dofFrameHits = frameHits;

			DEBUG("Cinema autofocus: mode=" + mode + " focus=" + NumStr((RealType)focus) + " region_hits="
				+ NumStr(regionHits) + "/256 frame_hits=" + NumStr(frameHits) + " aperture=" + NumStr((RealType)aper));
		}
	#endif

		// Camino de upload compartido con CinemaFinishTest (el que quedo
		// VERIFICADO por readback en el paso 2 de M0): SubmitBatch + desligar
		// RTVs + UpdateSubresource sobre el d3dColorTex USAGE_DEFAULT del
		// surface. pixelsRgba8 = w*h*4 bytes RGBA8 straight-alpha del surface
		// destino; si el framebuffer es HDR (RGBA32F) se expande a float.
		BoolType UploadToSurface(IntType surfId, const uchar* pixelsRgba8)
		{
			Surface* surf = FindSurface(surfId);
			if (!surf || !surf->frameBuffer)
			{
				WARNING("Cinema upload: surface " + QString::number(surfId) + " invalido");
				return false;
			}
			FrameBuffer* fb = surf->frameBuffer;
			if (!fb->d3dColorTex || !fb->valid)
			{
				WARNING("Cinema upload: framebuffer sin textura de color");
				return false;
			}
			IntType w = surf->size.width();
			IntType h = surf->size.height();
			if (w <= 0 || h <= 0 || !pixelsRgba8)
				return false;

			// Vaciar cualquier draw batcheado pendiente antes de tocar la textura
			GFX->SubmitBatch();

			// Desligar CUALQUIER render target actual: UpdateSubresource sobre
			// una textura ligada como RTV es un hazard (el runtime puede
			// descartar o diferir la escritura). Se restaura el estado en el
			// siguiente BeginUse/surface_set_target del motor.
			ID3D11RenderTargetView* nullRtv[1] = { nullptr };
			D3DContext->OMSetRenderTargets(1, nullRtv, nullptr);

			if (fb->hdr)
			{
				// RGBA32F: expandir el resolve de 8 bits a float
				QVector<float> px((int)(w * h * 4));
				float* fp = px.data();
				for (IntType i = 0; i < w * h * 4; i++)
					fp[i] = (float)pixelsRgba8[i] / 255.f;
				D3DContext->UpdateSubresource(fb->d3dColorTex, 0, nullptr, px.constData(), (UINT)(w * 16), 0);
			}
			else
				D3DContext->UpdateSubresource(fb->d3dColorTex, 0, nullptr, pixelsRgba8, (UINT)(w * 4), 0);

			return true;
		}

	#endif // API_D3D11
	}

	// R9 (FIX 1/5): backend que ELEGIRA el proximo Begin (ver impl). Publico
	// para pt_backend (presupuesto de export) y para el defer del preview.
	BoolType CinemaRenderWantsDxr()
	{
		return CinemaBackendWantsDxrImpl();
	}

	// M8-B ("escena viva"): intenta un update TRANSFORM-ONLY del frame RECIEN
	// capturado (draws frescos en CinemaScene) contra el snapshot del ultimo build.
	// Devuelve:
	//   "rebuild"  -> el caller (render_cinema.gml) debe hacer pt_build completo
	//                 (topologia distinta, backend D3D11, o el update GPU fallo).
	//   "static"   -> nada cambio; sin trabajo.
	//   "transform"-> instancias + TLAS + gEmiO2W/gLights actualizados en µs.
	// Solo el backend DXR (TLAS por hardware) soporta el camino vivo; en D3D11 se
	// degrada al rebuild actual (aceptable: es el hardware sin DXR).
	static double g_lastSceneUpdateMs = 0.0; // classify (CPU) + update GPU

	QString CinemaRenderSceneUpdate()
	{
	#if !API_D3D11
		return QString("rebuild");
	#else
		g_lastSceneUpdateMs = 0.0;
		if (!CinemaRenderWantsDxr())
			return QString("rebuild");
		auto tUpd = std::chrono::steady_clock::now();
		std::vector<CinemaInstXform> worlds, emiO2W;
		std::vector<int> changedDraws;
		CinemaLiveClass cls = CinemaSceneClassifyLive(worlds, emiO2W, changedDraws);
		if (cls == CINEMA_LIVE_REBUILD)
			return QString("rebuild");
		if (cls == CINEMA_LIVE_STATIC)
		{
			g_lastSceneUpdateMs = MsSince(tUpd);
			return QString("static");
		}
		if (cls == CINEMA_LIVE_GEOMETRY)
		{
			// M8-C: bend deformo verts -> update PARCIAL (verts + BLAS de esas partes
			// + TLAS). Si falla -> rebuild seguro.
			if (!CinemaGpu12UpdateGeometry(worlds, emiO2W, changedDraws))
				return QString("rebuild");
			g_lastSceneUpdateMs = MsSince(tUpd);
			return QString("geometry");
		}
		if (!CinemaGpu12UpdateInstances(worlds, emiO2W))
			return QString("rebuild"); // fallback seguro si el update GPU falla
		g_lastSceneUpdateMs = MsSince(tUpd);
		return QString("transform");
	#endif
	}

	// Ms (wall, con drain de GPU) del ultimo update en vivo: clasificacion CPU +
	// reescritura de instancias + TLAS rebuild + re-subida de gEmiO2W/gLights
	// (gate de coste M8-B). NO incluye el walk de captura (eso lo mide GML).
	RealType CinemaRenderMsUpdate()
	{
		return (RealType)g_lastSceneUpdateMs;
	}

	// ---------------------------------------------------------------------
	// Begin
	// ---------------------------------------------------------------------

	QString CinemaRenderBegin(IntType w, IntType h, IntType seed)
	{
	#if !API_D3D11
		(void)w; (void)h; (void)seed;
		render.state = "error";
		render.error = "cinema_requires_d3d11";
		return render.error;
	#else
		auto tBegin = std::chrono::steady_clock::now();
		auto fail = [&](const QString& error) -> QString
		{
			render.state = "error";
			render.error = error;
			WARNING("Cinema render begin failed: " + error);
			return error;
		};

		if (!CinemaGpuSupported())
			return fail("cinema_requires_fl11");
		if (w < 1 || h < 1 || w > CINEMA_MAX_DIM || h > CINEMA_MAX_DIM || w * h > CINEMA_MAX_PIXELS)
			return fail("invalid_size");
		if (!CinemaSceneGetGpu().IsValid())
			return fail("scene_not_built");
		if (!EnsureKernels())
			return fail("kernel_compile_failed");

		render.error.clear();

		// -----------------------------------------------------------------
		// M6 fase A: seleccion de backend. Default = D3D11 (el DXR se activa con
		// MI3_CINEMA_BACKEND=dxr hasta que la fase B lo valide como default). Si
		// se pide DXR y el device D3D12/dxc estan disponibles y la escena sube y
		// clarea OK, corre por D3D12; cualquier fallo cae LIMPIO a D3D11. El log
		// dice que backend corre y por que.
		// -----------------------------------------------------------------
		// Fase B: DXR es el DEFAULT cuando esta disponible (validado: >=3x vs
		// D3D11 a 1080p/4K, paridad verde, casos custom A/B rmse<0.001). Solo se
		// usa D3D11 si el usuario lo fuerza (MI3_CINEMA_BACKEND=dx11 -> -1) o si el
		// device D3D12/dxc/BLAS no estan disponibles (fallback limpio).
		IntType backendOv = CinemaBackendOverride();
		render.useDxr = false;
		if (backendOv != -1)
		{
			QString why;
			if (CinemaGpu12Available(&why) && CinemaGpu12EnsureScene() && CinemaGpu12Begin(w, h))
			{
				render.useDxr = true;
				render.width = w;
				render.height = h;
				DEBUG("Cinema backend: DXR (" + CinemaGpu12BackendInfo() + ")");
			}
			else
			{
				QString reason = why.isEmpty() ? QString("begin_failed") : why;
				if (backendOv == 1)
					WARNING("Cinema backend: DXR pedido pero no disponible (" + reason + ") -> fallback D3D11");
				else
					DEBUG("Cinema backend: DXR no disponible (" + reason + ") -> D3D11");
			}
		}

		if (!render.useDxr)
		{
			DEBUG("Cinema backend: D3D11 (motivo: " + QString(backendOv == -1 ? "override dx11" : "DXR no disponible") + ")");

			// M8-B (§5): si venimos de una sesion DXR con updates en vivo, los verts
			// MUNDO del CPU estan desfasados. Antes de trazar por el BVH de CPU,
			// re-aplanar los draws frescos (build completo) -> mundo consistente.
			{
				QString rws = CinemaSceneRebuildIfWorldStale();
				if (!rws.isEmpty())
					return fail(rws);
			}

			// R9 (FIX 1): el camino D3D11 traza por el BVH de CPU. Si el build fue
			// diferido (preview que esperaba DXR pero acabo en D3D11), construir el
			// arbol AHORA (reordena + re-sube). Raro: solo si el device DXR no
			// estaba disponible al Begin pese a que el preview lo predijo.
			if (CinemaSceneIsBvhDeferred())
			{
				QString efb = CinemaSceneEnsureFullBuild();
				if (!efb.isEmpty())
					return fail(efb);
			}

			// (Re)crear acumulador y AOVs solo si cambia la resolucion
			if (w != render.width || h != render.height || !render.accumUav)
			{
				ReleaseTargets();
				UINT count = (UINT)(w * h);
				UINT stride = 4 * sizeof(float); // RWStructuredBuffer<float4>
				render.accum = CinemaGpu::CreateStructuredBuffer(stride, count, nullptr, true);
				render.aovAlbedo = CinemaGpu::CreateStructuredBuffer(stride, count, nullptr, true);
				render.aovNormalDepth = CinemaGpu::CreateStructuredBuffer(stride, count, nullptr, true);
				render.varCount = CinemaGpu::CreateStructuredBuffer(stride, count, nullptr, true); // M7 adaptive (u3)
				render.accumUav = render.accum ? CinemaGpu::CreateBufferUAV(render.accum, count) : nullptr;
				render.aovAlbedoUav = render.aovAlbedo ? CinemaGpu::CreateBufferUAV(render.aovAlbedo, count) : nullptr;
				render.aovNormalDepthUav = render.aovNormalDepth ? CinemaGpu::CreateBufferUAV(render.aovNormalDepth, count) : nullptr;
				render.varCountUav = render.varCount ? CinemaGpu::CreateBufferUAV(render.varCount, count) : nullptr;

				if (!render.accumUav || !render.aovAlbedoUav || !render.aovNormalDepthUav || !render.varCountUav)
				{
					ReleaseTargets();
					return fail("gpu_alloc_failed");
				}
				render.width = w;
				render.height = h;
			}
		}

		render.seed = (uint32_t)seed;
		render.sampleIdx = 0;
		render.msLastSample = 0.0;
		render.msFinish = 0.0;
		render.denoiser = "off";
		render.msDenoise = 0.0;
		render.adaptive = false;      // M7: se activa en Sample si llega umbral > 0
		render.noiseThreshold = 0.0;
		render.activePixels = -1;

		if (!render.useDxr)
		{
			// CSClear del frame ENTERO: ignora tile y lee width/height de
			// res_clamp, asi que el cbuffer debe llevar la resolucion ya puesta
			PTFrameCpu frame = {};
			frame.resClamp[0] = (float)w;
			frame.resClamp[1] = (float)h;
			CinemaGpu::UpdateConstantBuffer(render.cbFrame, &frame, CinemaKernelM0::PTFRAME_SIZE);

			// Orden de UAVs = contrato u0/u1/u2/u3 (SLOT_UAV_* de CinemaKernelM0.hpp):
			// acumulador / AOV albedo / AOV normal+depth / varCount adaptive
			ID3D11UnorderedAccessView* uavs[4] = { render.accumUav, render.aovAlbedoUav, render.aovNormalDepthUav, render.varCountUav };
			CinemaGpu::Dispatch(render.csClear, render.cbFrame, nullptr, 0, uavs, 4, GroupsFor(w), GroupsFor(h));
		}

		// R9 (FIX 6) / DOF-FIEL A3: autofocus por sonda de profundidad (una vez
		// por Begin, consistente para todos los samples de esta acumulacion).
		// Solo si DoF con CAM_DOF_DEPTH==0; re-clarea al terminar. Sirve a los
		// DOS backends (antes solo DXR). Ver RunAutofocusProbe.
		RunAutofocusProbe(w, h);

		render.state = "ready";
		render.msBegin = MsSince(tBegin);
		DEBUG("Cinema render begin: " + NumStr(w) + "x" + NumStr(h) + ", seed " + NumStr(seed)
			+ (render.useDxr ? " [DXR]" : " [D3D11]") + " (" + NumStr(render.msBegin) + " ms)");
		return QString();
	#endif // API_D3D11
	}

	// ---------------------------------------------------------------------
	// Sample
	// ---------------------------------------------------------------------

	QString CinemaRenderSample(IntType bounces, RealType clampIndirect,
		RealType emissiveScale, BoolType alphaFilm, IntType sppCount, RealType noiseThreshold)
	{
	#if !API_D3D11
		(void)bounces; (void)clampIndirect; (void)emissiveScale; (void)alphaFilm; (void)sppCount; (void)noiseThreshold;
		render.state = "error";
		render.error = "cinema_requires_d3d11";
		return render.error;
	#else
		auto fail = [&](const QString& error) -> QString
		{
			render.state = "error";
			render.error = error;
			WARNING("Cinema render sample failed: " + error);
			return error;
		};

		// "finished" tambien es valido: el preview progresivo resuelve y
		// SIGUE acumulando (seccion 11.1 del diseno)
		if (render.state != "ready" && render.state != "sampling" && render.state != "finished")
			return fail("render_not_begun");

		const CinemaSceneGpu& sceneGpu = CinemaSceneGetGpu();
		if (!sceneGpu.IsValid())
			return fail("scene_not_built");
		if (!CinemaSceneCamera().valid)
			return fail("camera_not_set");
		if (!CinemaSceneEnv().valid && render.sampleIdx == 0)
			WARNING("Cinema render: no environment set (pt_env_set missing?), sol y cielo negros");

		// RONDA 7 (attack A, batching multi-spp): sppCount pasadas COMPLETAS del
		// frame DENTRO del native, avanzando tile.z = sampleIdx como siempre.
		// Elimina el viaje GML<->C++<->app-frame del editor entre samples en el
		// export (el gran overhead: ~266 ms/sample de frame del editor). La
		// semantica de acumulacion y el RNG por sample quedan IDENTICOS
		// (tile.z = 0,1,2,...,N-1 igual que sin batch; el acumulador suma en el
		// mismo orden creciente de sample) -> bit-identico. Editor interactivo
		// pasa 1 (responsividad del viewport).
		if (sppCount < 1)
			sppCount = 1;

		// M7 adaptive: umbral > 0 activa el modo para TODO el render (el resolve
		// pasa a dividir por conteo por pixel). El oraculo/paridad SIEMPRE llama
		// con 0 (adaptive off), asi que nunca se espeja nada al oraculo.
		render.noiseThreshold = noiseThreshold;
		if (noiseThreshold > 0.0)
			render.adaptive = true;

		auto tSample = std::chrono::steady_clock::now();

		PTFrameCpu frame;
		FillFrame(frame, bounces, clampIndirect, emissiveScale, alphaFilm, noiseThreshold);
		// FONDO DE PELICULA: se apunta el MISMO criterio que acaba de decidir el
		// bit del cbuffer, para que el resolve componga exactamente cuando el
		// kernel hizo holdout. Aqui y no en FillFrame: la sonda de profundidad
		// tambien llama a FillFrame (con alphaFilm false) y lo dejaria mintiendo.
		render.backdropOn = CinemaBackdropActive(alphaFilm);

		IntType tileSize = CinemaEnvInt("MI3_CINEMA_TILE", CINEMA_TILE_SIZE_DEFAULT);
		IntType flushBudgetPix = CinemaEnvInt("MI3_CINEMA_FLUSHMPX", CINEMA_FLUSH_MPX_DEFAULT) * 1024 * 1024;

		// M6 fase A: camino DXR (D3D12). El MISMO kernel (traversal por software
		// aun) sobre el device D3D12: mismo PTFrame, misma particion en tiles
		// (tile.xy offset absoluto + tile.z sampleIdx -> bit-identico), barreras
		// UAV entre samples. Readback y resolve CPU siguen igual (Finish/Parity).
		if (render.useDxr)
		{
			if (!CinemaGpu12Sample(&frame, render.sampleIdx, sppCount, tileSize, flushBudgetPix))
				return fail("dxr_sample_failed");
			render.sampleIdx += sppCount;
			render.msLastSample = MsSince(tSample);
			render.state = "sampling";
			return QString();
		}

		// SRVs de escena t0..t7 (orden del kernel: SLOT_SRV_VERTS/TRIS/NODES/
		// MATERIALS/ATLAS/LIGHTS/EMITRI/EMICDF). Los slots M1 pueden ser null
		// con counts 0: el kernel gatea todo acceso por counts.
		ID3D11ShaderResourceView* srvs[8] =
		{
			sceneGpu.vertsSrv, sceneGpu.trisSrv, sceneGpu.nodesSrv,
			sceneGpu.materialsSrv, sceneGpu.atlasSrv,
			sceneGpu.lightsSrv, sceneGpu.emiTriIdxSrv, sceneGpu.emiCdfSrv
		};
		// UAVs u0/u1/u2/u3 = acumulador / AOV albedo / AOV normal+depth / varCount (M7)
		ID3D11UnorderedAccessView* uavs[4] = { render.accumUav, render.aovAlbedoUav, render.aovNormalDepthUav, render.varCountUav };

		// RONDA 7 (attacks B + D/E): ligar shader/cbuffer/SRVs/UAVs/sampler UNA
		// vez para TODO el lote (antes CinemaGpu::Dispatch religaba y desligaba
		// los 11 recursos por tile -> ~960 llamadas de estado/sample). Los tiles
		// mas grandes (1 Mpix) reducen 10x el numero de dispatches y por tanto de
		// barreras UAV automaticas del driver entre dispatches (la burbuja real).
		// (tileSize/flushBudgetPix ya calculados arriba, compartidos con el camino DXR.)

		D3DContext->CSSetShader(render.csPathTrace, nullptr, 0);
		D3DContext->CSSetConstantBuffers(0, 1, &render.cbFrame);
		D3DContext->CSSetShaderResources(0, 8, srvs);
		D3DContext->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		D3DContext->CSSetSamplers((UINT)CinemaKernelM0::SLOT_SAMPLER_POINT, 1, &render.sampPoint);

		// 1 dispatch = 1 tile de tileSize^2 px x 1 spp. tile.xy = offset ABSOLUTO
		// en pixeles del frame (el RNG usa coordenadas absolutas: sin costuras).
		// tile.z = sampleIdx de ESTA pasada. Flush por presupuesto de pixeles
		// encolados (anti-TDR: cada dispatch <= 1 Mpix y cada submission <= unos
		// Mpix, muy por debajo del limite de 2 s).
		IntType pixelsSinceFlush = 0;
		for (IntType s = 0; s < sppCount; s++)
		{
			frame.tile[2] = (uint32_t)(render.sampleIdx + s);
			for (IntType ty = 0; ty < render.height; ty += tileSize)
			{
				for (IntType tx = 0; tx < render.width; tx += tileSize)
				{
					IntType tileW = render.width - tx < tileSize ? render.width - tx : tileSize;
					IntType tileH = render.height - ty < tileSize ? render.height - ty : tileSize;

					frame.tile[0] = (uint32_t)tx;
					frame.tile[1] = (uint32_t)ty;
					CinemaGpu::UpdateConstantBuffer(render.cbFrame, &frame, CinemaKernelM0::PTFRAME_SIZE);
					D3DContext->Dispatch(GroupsFor(tileW), GroupsFor(tileH), 1);

					pixelsSinceFlush += tileW * tileH;
					if (pixelsSinceFlush >= flushBudgetPix)
					{
						D3DContext->Flush();
						pixelsSinceFlush = 0;
					}
				}
			}
		}

		// Desligar TODO una vez (mismo saneo de hazards que hacia
		// CinemaGpu::Dispatch por tile): dejar SRVs/UAVs del CS ligados
		// colisionaria con las pasadas raster del editor que leen/escriben esos
		// recursos.
		ID3D11ShaderResourceView* nullSrvs[8] = {};
		ID3D11UnorderedAccessView* nullUavs[4] = {};
		ID3D11Buffer* nullCb = nullptr;
		ID3D11SamplerState* nullSampler = nullptr;
		D3DContext->CSSetShaderResources(0, 8, nullSrvs);
		D3DContext->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
		D3DContext->CSSetConstantBuffers(0, 1, &nullCb);
		D3DContext->CSSetSamplers((UINT)CinemaKernelM0::SLOT_SAMPLER_POINT, 1, &nullSampler);
		D3DContext->CSSetShader(nullptr, nullptr, 0);
		if (pixelsSinceFlush)
			D3DContext->Flush();

		// Auto-pacing del batch (attack A): con batch > 1 se drena la GPU al
		// terminar la llamada. Sin esto el CPU encolaria miles de dispatches
		// mientras la GPU va detras (el preview backoff ya no drena seguido) y
		// crece una cola desbocada; ademas hace que el app-frame del editor dure
		// ~batch*ms_gpu, amortizando su overhead sobre todo el batch (el objetivo
		// del attack A). NO cambia el resultado (una sincronizacion no es
		// matematica). En batch = 1 (viewport interactivo) NO se sincroniza:
		// responsividad, comportamiento identico al de antes.
		if (sppCount > 1 && render.syncEvent)
		{
			D3DContext->End(render.syncEvent);
			D3DContext->Flush();
			BOOL done = FALSE;
			while (D3DContext->GetData(render.syncEvent, &done, sizeof(done),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE)
				std::this_thread::yield();
		}

		render.sampleIdx += sppCount;
		render.msLastSample = MsSince(tSample);
		render.state = "sampling";
		return QString();
	#endif // API_D3D11
	}

	// ---------------------------------------------------------------------
	// Finish
	// ---------------------------------------------------------------------

	// Paridad GPU vs oraculo CPU (arnes M1): renderiza AMBOS a la misma
	// resolucion/semilla y compara RMSE en lineal. Devuelve "rmse=X ok=0|1"
	// o un codigo de error. Pensado para resoluciones chicas (320x180).
	QString CinemaRenderParity(IntType w, IntType h, IntType spp, IntType seed,
		IntType bounces, RealType clampIndirect, RealType emissiveScale, RealType rmseThreshold)
	{
	#if API_D3D11
		QString err = CinemaRenderBegin(w, h, seed);
		if (!err.isEmpty())
			return err;
		for (IntType i = 0; i < spp; i++)
		{
			// M7: paridad SIEMPRE con adaptive off (umbral 0) -> el oraculo CPU no
			// se toca y el acumulador GPU queda bit-identico al camino previo.
			err = CinemaRenderSample(bounces, clampIndirect, emissiveScale, false, 1, 0.0);
			if (!err.isEmpty())
				return err;
		}
		// Media lineal del acumulador GPU (mismo espacio que el oraculo). El
		// backend (D3D11 o DXR) lo elige CinemaRenderBegin via env; el oraculo
		// CPU es el mismo arbitro para ambos (rmse<threshold gate de la regresion).
		IntType numFloats = w * h * 4;
		QVector<float> accumData(numFloats);
		if (render.useDxr)
		{
			if (!CinemaGpu12Readback(accumData.data(), nullptr, nullptr))
				return QString("readback_failed");
		}
		else if (!CinemaGpu::ReadbackBuffer(render.accum, accumData.data(), numFloats * sizeof(float)))
			return QString("readback_failed");
		// NORMALIZACION DEL FILM: la MISMA que el resolve de produccion
		// (CinemaRenderFinish), literalmente las mismas dos lineas. Es la unica
		// eleccion honesta: lo que este arnes debe vigilar es la imagen que SALE
		// del render, no una cantidad intermedia que nunca ve nadie.
		//   rgb   = suma_color / max(suma_alpha, 1)   -> DES-PREMULTIPLICADO
		//   alpha = suma_alpha / spp                  -> cobertura media
		// Historia (que no se repita): el lado GPU dividia por la suma de alphas
		// y el ORACULO por el numero de samples. Con alpha 1 por sample -- toda
		// la geometria anterior a la cobertura -- las dos expresiones dan lo
		// MISMO y la paridad estuvo verde anos. Al aparecer cobertura parcial en
		// el film (capas de skin con alfa), la razon GPU/CPU pasa a ser 1/E[alpha]
		// y el arnes se puso rojo comparando dos cantidades distintas, con el
		// kernel y el oraculo correctos. Si se toca la normalizacion del resolve
		// de produccion, HAY QUE TOCAR ESTAS DOS LINEAS Y LAS DEL ORACULO.
		QVector<float> gpuMean(numFloats);
		float invSppP = (spp > 0) ? 1.f / (float)spp : 0.f;
		for (IntType i = 0; i < w * h; i++)
		{
			float sumA = std::isfinite(accumData[i * 4 + 3]) ? accumData[i * 4 + 3] : 0.f;
			float weight = sumA > 1.f ? sumA : 1.f;
			gpuMean[i * 4 + 0] = accumData[i * 4 + 0] / weight;
			gpuMean[i * 4 + 1] = accumData[i * 4 + 1] / weight;
			gpuMean[i * 4 + 2] = accumData[i * 4 + 2] / weight;
			gpuMean[i * 4 + 3] = sumA * invSppP;
		}
		err = CinemaOracleRender(w, h, spp, seed, bounces, clampIndirect, emissiveScale, nullptr);
		if (!err.isEmpty())
			return QString("oracle_") + err;
		CinemaOracleCompareResult cmp = CinemaOracleCompare(gpuMean.constData(), w, h, (float)rmseThreshold);
		// Volcado de las DOS imagenes de la paridad (sRGB 8 bit) junto al log:
		// con ok=0, el rmse dice "hay divergencia" pero no DONDE ni de que
		// signo. parity_gpu/parity_cpu/parity_diff (el diff x10 para que se
		// lea) lo ensenan de un vistazo. Son 4 PNG chicos por corrida.
		// El CUARTO, parity_alpha, es el canal que nadie miraba: R = cobertura
		// GPU, G = cobertura del oraculo, B = |diferencia| x10. Un pixel azul
		// ahi es una divergencia SOLO de alpha, invisible en los otros tres.
		{
			IntType ow = 0, oh = 0;
			const float* ora = CinemaOracleLastImage(&ow, &oh);
			if (ora && ow == w && oh == h)
			{
				QImage imgG(w, h, QImage::Format_RGBA8888);
				QImage imgC(w, h, QImage::Format_RGBA8888);
				QImage imgD(w, h, QImage::Format_RGBA8888);
				QImage imgA(w, h, QImage::Format_RGBA8888);
				auto enc = [](float v) -> int {
					float c = v <= 0.f ? 0.f : (v >= 1.f ? 1.f : v);
					c = (c <= 0.0031308f) ? c * 12.92f : (1.055f * std::pow(c, 1.f / 2.4f) - 0.055f);
					int b = (int)(c * 255.f + 0.5f);
					return b < 0 ? 0 : (b > 255 ? 255 : b);
				};
				auto lin8 = [](float v) -> int {
					float c = v <= 0.f ? 0.f : (v >= 1.f ? 1.f : v);
					return (int)(c * 255.f + 0.5f);
				};
				for (IntType y = 0; y < h; y++)
					for (IntType x = 0; x < w; x++)
					{
						size_t i = (size_t)y * (size_t)w + (size_t)x;
						const float* g = gpuMean.constData() + i * 4;
						const float* o = ora + i * 4;
						imgG.setPixel((int)x, (int)y, qRgba(enc(g[0]), enc(g[1]), enc(g[2]), 255));
						imgC.setPixel((int)x, (int)y, qRgba(enc(o[0]), enc(o[1]), enc(o[2]), 255));
						imgD.setPixel((int)x, (int)y, qRgba(
							enc(std::fabs(g[0] - o[0]) * 10.f),
							enc(std::fabs(g[1] - o[1]) * 10.f),
							enc(std::fabs(g[2] - o[2]) * 10.f), 255));
						imgA.setPixel((int)x, (int)y, qRgba(
							lin8(g[3]), lin8(o[3]),
							lin8(std::fabs(g[3] - o[3]) * 10.f), 255));
					}
				QString dir = QCoreApplication::applicationDirPath() + "/Data/";
				imgG.save(dir + "parity_gpu.png");
				imgC.save(dir + "parity_cpu.png");
				imgD.save(dir + "parity_diff.png");
				imgA.save(dir + "parity_alpha.png");

				// Los cuatro PNG son sRGB de 8 bit: CLAMPAN a 1.0. En una escena
				// HDR (una sala blanca muy iluminada) los dos lados saturan a
				// blanco y el volcado dice "identico" mientras el rmse grita. El
				// .bin lleva los MISMOS floats que compara CinemaOracleCompare,
				// sin clamp ni sRGB, para poder medir la divergencia de verdad:
				//   "MI3PAR\0" | w:i32 | h:i32 | gpu rgba f32 | oraculo rgba f32
				QFile raw(dir + "parity_raw.bin");
				if (raw.open(QIODevice::WriteOnly))
				{
					int32_t hdr[2] = { (int32_t)w, (int32_t)h };
					raw.write("MI3PAR\0", 7);
					raw.write((const char*)hdr, sizeof(hdr));
					raw.write((const char*)gpuMean.constData(), (qint64)numFloats * sizeof(float));
					raw.write((const char*)ora, (qint64)numFloats * sizeof(float));
					raw.close();
				}
			}
		}
		return QString("rmse=%1 ok=%2 maxDiff=%3 at=%4,%5 ch=%6")
			.arg(cmp.rmse, 0, 'g', 6).arg(cmp.ok ? 1 : 0)
			.arg(cmp.maxDiff, 0, 'g', 4).arg(cmp.maxX).arg(cmp.maxY)
			.arg(QString(QChar(cmp.maxC >= 0 && cmp.maxC < 4 ? "rgba"[cmp.maxC] : '?')));
	#else
		(void)w; (void)h; (void)spp; (void)seed; (void)bounces;
		(void)clampIndirect; (void)emissiveScale; (void)rmseThreshold;
		return QString("cinema_requires_d3d11");
	#endif
	}

	QString CinemaRenderFinish(IntType surfId, RealType exposure, IntType tonemap,
		IntType denoiseMode, RealType saturation)
	{
	#if !API_D3D11
		(void)surfId; (void)exposure; (void)tonemap; (void)denoiseMode; (void)saturation;
		render.state = "error";
		render.error = "cinema_requires_d3d11";
		return render.error;
	#else
		auto fail = [&](const QString& error) -> QString
		{
			render.state = "error";
			render.error = error;
			WARNING("Cinema render finish failed: " + error);
			return error;
		};

		if (render.sampleIdx < 1 || (!render.useDxr && !render.accum))
			return fail("no_samples");

		Surface* surf = FindSurface(surfId);
		if (!surf || !surf->frameBuffer || !surf->frameBuffer->d3dColorTex || !surf->frameBuffer->valid)
			return fail("invalid_surface");
		if (surf->size.width() != render.width || surf->size.height() != render.height)
			return fail("surface_size_mismatch");

		auto tFinish = std::chrono::steady_clock::now();

		IntType numPix = render.width * render.height;
		size_t numFloats = (size_t)numPix * 4;

		// Readback de acumulador + AOVs (staging de BUFFER, no de textura).
		// Los AOVs no entran en el resolve M0 pero son parte del contrato
		// del render (M3: entradas del denoiser y del fog post); leerlos ya
		// valida el camino completo de readback.
		std::vector<float> accumData(numFloats);
		std::vector<float> aovAlbedoData(numFloats);
		std::vector<float> aovNormalDepthData(numFloats);
		if (render.useDxr)
		{
			// M6 fase A: readback del acumulador + AOVs del backend D3D12. El
			// resto del Finish (denoise/fog/resolve/upload) es identico: opera
			// sobre los mismos float4 premultiplicados.
			if (!CinemaGpu12Readback(accumData.data(), aovAlbedoData.data(), aovNormalDepthData.data()))
				return fail("readback_failed");
		}
		else if (!CinemaGpu::ReadbackBuffer(render.accum, accumData.data(), numFloats * sizeof(float)) ||
			!CinemaGpu::ReadbackBuffer(render.aovAlbedo, aovAlbedoData.data(), numFloats * sizeof(float)) ||
			!CinemaGpu::ReadbackBuffer(render.aovNormalDepth, aovNormalDepthData.data(), numFloats * sizeof(float)))
			return fail("readback_failed");

		// -----------------------------------------------------------------
		// Denoise (M3, diseno seccion 9): sobre la radiancia LINEAL MEDIA
		// (color = accum.rgb / peso, MISMA division que el resolve de
		// abajo), guiado por el albedo medio (AOV u1: rgb = suma, w = num
		// samples) y la normal mapeada + depth medios (AOV u2: xyz = suma
		// de normal, w = suma de t). SIEMPRE antes del fog post (M3) y del
		// tonemap. denoiseMode: 0 = off (el resolve queda byte a byte
		// identico al camino M0-M2), 1 = auto (OIDN -> a-trous), 2 =
		// a-trous forzado. Si el denoise falla del todo se sigue con el
		// color sin denoise (CinemaDenoise ya emitio el WARNING).
		// -----------------------------------------------------------------
		render.denoiser = "off";
		render.msDenoise = 0.0;
		std::vector<float> denoisedData;
		BoolType haveDenoised = false;
		if (denoiseMode != 0)
		{
			auto tDenoise = std::chrono::steady_clock::now();

			std::vector<float> meanColor(numFloats);
			std::vector<float> meanAlbedo(numFloats);
			std::vector<float> meanNormalDepth(numFloats);
			float invSppD = 1.f / (float)render.sampleIdx;

			#pragma OPENMP_FOR
			for (IntType i = 0; i < numPix; i++)
			{
				const float* a = accumData.data() + (size_t)i * 4;
				const float* al = aovAlbedoData.data() + (size_t)i * 4;
				const float* nd = aovNormalDepthData.data() + (size_t)i * 4;

				// color medio: mismas guardas NaN y mismo peso que el
				// resolve (sumA = suma de alphas, straight-alpha)
				float sumA = std::isfinite(a[3]) ? a[3] : 0.f;
				float weight = sumA > 1.f ? sumA : 1.f;
				for (IntType c = 0; c < 3; c++)
				{
					float v = a[c] / weight;
					meanColor[(size_t)i * 4 + c] = std::isfinite(v) ? v : 0.f;
				}
				meanColor[(size_t)i * 4 + 3] = sumA;

				// albedo medio: el AOV acumula 1 muestra por sample (w =
				// num samples, tambien en pixeles de cielo)
				float alW = std::isfinite(al[3]) && al[3] > 1.f ? al[3] : 1.f;
				for (IntType c = 0; c < 3; c++)
				{
					float v = al[c] / alW;
					meanAlbedo[(size_t)i * 4 + c] = std::isfinite(v) ? v : 0.f;
				}
				meanAlbedo[(size_t)i * 4 + 3] = 1.f;

				// normal media re-normalizada (el promedio de normales de
				// un pixel con borde no es unitario) + depth media.
				// M7 adaptive: divisor por CONTEO POR PIXEL (= samples que tomo el
				// pixel = w del AOV albedo, que incrementa 1 por sample igual que
				// gVarCount.z; el early-out salta ambos). Sin adaptive es el spp
				// global invSppD (BIT-IDENTICO). La normal se re-normaliza abajo, asi
				// que el divisor solo afecta al depth; se hace bien por consistencia.
				float invNd = render.adaptive ? (al[3] > 1.f ? 1.f / al[3] : 1.f) : invSppD;
				float nx = std::isfinite(nd[0]) ? nd[0] * invNd : 0.f;
				float ny = std::isfinite(nd[1]) ? nd[1] * invNd : 0.f;
				float nz = std::isfinite(nd[2]) ? nd[2] * invNd : 0.f;
				float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (nl > 1.0e-6f)
				{
					nx /= nl;
					ny /= nl;
					nz /= nl;
				}
				else
				{
					nx = 0.f;
					ny = 0.f;
					nz = 0.f; // pixel de cielo puro: "sin normal"
				}
				float depth = std::isfinite(nd[3]) ? nd[3] * invNd : 0.f;
				meanNormalDepth[(size_t)i * 4 + 0] = nx;
				meanNormalDepth[(size_t)i * 4 + 1] = ny;
				meanNormalDepth[(size_t)i * 4 + 2] = nz;
				meanNormalDepth[(size_t)i * 4 + 3] = depth;
			}

			denoisedData.resize(numFloats);
			QString used = CinemaDenoiseRun(render.width, render.height,
				meanColor.data(), meanAlbedo.data(), meanNormalDepth.data(),
				denoisedData.data(), denoiseMode == 1);
			if (!used.isEmpty())
			{
				render.denoiser = used;
				haveDenoised = true;
			}
			render.msDenoise = MsSince(tDenoise);
			DEBUG("Cinema denoise: " + (used.isEmpty() ? QString("FALLIDO (sin denoise)") : used)
				+ " (" + NumStr(render.msDenoise) + " ms)");
		}

		// R12: el fog POST de M3 (analitico sobre el depth AOV) esta ELIMINADO.
		// La niebla vive AHORA EN EL KERNEL, sobre el hit primario REAL (tras
		// ghost-texels y cutouts pasantes), que era justo el fallo estructural
		// que lo mantuvo apagado: el depth medio del AOV venia inflado por los
		// texels transparentes. No queda ningun segundo camino de niebla.

		// Resolve CPU (OpenMP; seccion 10 del diseno, recortada a M2):
		//   color = acum.rgb / max(peso, 1) * exposure  (lineal; el peso es
		//           la suma de alphas: en pixeles de cobertura parcial esto
		//           des-premultiplica y deja color straight-alpha). En un
		//           render OPACO el kernel garantiza alpha 1 por sample, asi
		//           que el peso ES el spp y esto es la media de siempre; solo
		//           el film transparente ve cobertura parcial aqui. Cuando el
		//           kernel dejaba escapar alphas < 1 en renders opacos, esta
		//           division les inflaba el brillo por 1/E[alpha] (el halo de
		//           la silueta): ver t33_alpha_invariante.
		//   tonemap 0: -> encode sRGB exacto -> RGBA8 (camino M0/M1: goldens
		//              y arnes de paridad, byte a byte identico al anterior)
		//   tonemap 1: -> x CINEMA_EXPOSURE_BASE -> AgX look Base (la salida
		//              queda display-encoded 2.2 como en el raster; la unica
		//              correccion post seria la DESVIACION de gamma
		//              pow(val, 2.2/gamma), que con el gamma fijo 2.2 de M2
		//              es no-op: el knob de gamma de proyecto llega en M4)
		//              -> RGBA8
		//   alpha = cobertura media (acum.w / spp), en AMBOS caminos: el
		//           tonemap solo toca RGB, el film straight-alpha se respeta
		// exposure es multiplicador LINEAL puro pre-tonemap (nada de x/(1+x)
		// ni EV); el denoise ya corrio ANTES (arriba) sobre la radiancia
		// lineal media, y el fog post (M3) entra entre denoise y exposure.
		// Con denoise activo el rgb sale de denoisedData (ya es la media
		// lineal: NO se vuelve a dividir por el peso); el alpha sale
		// SIEMPRE del acumulador (el denoise no toca la cobertura).
		QVector<uchar> pixels((int)(numPix * 4));
		uchar* dst = pixels.data();
		const float* acc = accumData.data();
		const float* dn = haveDenoised ? denoisedData.data() : nullptr;
		float invSpp = 1.f / (float)render.sampleIdx;
		float exposureMul = exposure > 0.0 ? (float)exposure : 0.f;
		// tonemap: 0 = sRGB simple, 1 = AgX Base, 2 = AgX Punchy, 3 = sRGB
		// con hombro suave, 4 = hombro preservando color. Los desconocidos
		// degradan a sRGB simple.
		BoolType useAgx = (tonemap == 1 || tonemap == 2);
		BoolType punchy = (tonemap == 2);
		BoolType softShoulder = (tonemap == 3);
		BoolType colorShoulder = (tonemap == 4);
		if (useAgx)
			exposureMul *= CINEMA_EXPOSURE_BASE;
		// Saturacion post-tonemap (default 1 = identidad byte a byte)
		float satMul = saturation > 0.0 ? (float)saturation : 0.f;

		// --- FONDO DE PELICULA (cinema_backdrop) ---------------------------
		// Un color que la CAMARA ve y que NO ilumina. El kernel ya trajo el film
		// en HOLDOUT (FRAMEF_BACKDROP: el cielo no pinta ni cubre, y el catcher
		// escribe solo su alpha), asi que aqui solo queda el operador OVER:
		//     out = color_straight * a + fondo * (1 - a)
		// DESPUES del tonemap y de la saturacion, y mezclando EN LUZ (los dos
		// lados se decodifican a lineal con DecodeSrgbF, se mezclan y se vuelven
		// a encodar). Es literalmente lo que hace un compositor externo con esta
		// pasada -- y justo por eso el color que el usuario eligio sale byte a
		// byte (un F7F6F6 sale 247/246/246) y la sombra del catcher sale con la
		// misma densidad que le pondria el compositor. Mezclar en CODIGOS de
		// pantalla, que es el atajo facil, oscureceria la sombra de mas.
		//   backdropOn se decide con el MISMO criterio que el flag del kernel
		//   (film opaco + ajuste > 0). Si por lo que sea no casaran, el peor caso
		//   seria un fondo negro (holdout sin composicion) o un cielo sin
		//   componer: por eso se lee del mismo sitio, CinemaSceneEnv().
		// El alpha de salida se fuerza a 255: el render opaco sigue siendo opaco.
		const CinemaEnvState& fenv = CinemaSceneEnv();
		BoolType backdropOn = render.backdropOn;
		BoolType backdropRamp = backdropOn && fenv.backdrop >= 2;
		float bdA[3] = { fenv.backdropColor[0], fenv.backdropColor[1], fenv.backdropColor[2] };
		float bdB[3] = { fenv.backdropColor2[0], fenv.backdropColor2[1], fenv.backdropColor2[2] };
		// Rampa LINEAL EN PANTALLA. Angulo en grados con el eje Y hacia ABAJO
		// (como los pixeles): 0 = izquierda->derecha, 90 = arriba->abajo, 30 =
		// diagonal hacia abajo-derecha. t = 0 en la esquina de la que sale el
		// color A y t = 1 en la contraria; la normalizacion por (|cos| + |sin|)
		// hace que la rampa recorra EXACTAMENTE de esquina a esquina sea cual sea
		// el angulo, y como usa UV normalizadas es independiente de resolucion.
		const float PI_F = 3.14159265358979f;
		float bdRad = fenv.backdropAngle * PI_F / 180.f;
		float bdDx = std::cos(bdRad), bdDy = std::sin(bdRad);
		float bdExt = std::fabs(bdDx) + std::fabs(bdDy);
		if (bdExt < 1.0e-6f)
			bdExt = 1.f;
		// TILED EXPORT: la rampa se mide contra el LIENZO COMPLETO, no contra esta
		// superficie. Cada tile del export es un render entero en su propio target
		// (export_update.gml), asi que sin esto el degradado volveria a empezar en
		// cada tile y saldria a franjas. Sin tiling (fullW/H = 0) esto degrada
		// EXACTAMENTE al caso de siempre: origen 0,0 y lienzo = la superficie.
		IntType bdFullW = (fenv.tileFullW > 0) ? fenv.tileFullW : render.width;
		IntType bdFullH = (fenv.tileFullH > 0) ? fenv.tileFullH : render.height;
		IntType bdOx = (fenv.tileFullW > 0) ? fenv.tileX0 : 0;
		IntType bdOy = (fenv.tileFullH > 0) ? fenv.tileY0 : 0;
		float bdInvW = (bdFullW > 1) ? 1.f / (float)(bdFullW - 1) : 0.f;
		float bdInvH = (bdFullH > 1) ? 1.f / (float)(bdFullH - 1) : 0.f;
		IntType bdW = render.width;

		#pragma OPENMP_FOR
		for (IntType i = 0; i < numPix; i++)
		{
			const float* a = acc + (size_t)i * 4;
			uchar* p = dst + (size_t)i * 4;
			// Guardas NaN/inf del resolve: el kernel ya sanea el acumulador
			// (criterio (e) de M0), pero un readback corrupto (device removed,
			// fallo de driver) colaria aqui; Clamp01f NO filtra NaN (ambas
			// comparaciones son false y lo devuelve tal cual) y el cast a
			// uchar de un NaN es UB. AgxApply tampoco filtra NaN (el floor
			// 1e-10 del log2 usa una comparacion que un NaN esquiva): sanear
			// SIEMPRE antes del tonemap.
			float sumA = std::isfinite(a[3]) ? a[3] : 0.f;
			float weight = sumA > 1.f ? sumA : 1.f;
			float r, g, b;
			if (dn)
			{
				// media lineal ya denoised (y saneada por CinemaDenoise)
				const float* d = dn + (size_t)i * 4;
				r = d[0] * exposureMul;
				g = d[1] * exposureMul;
				b = d[2] * exposureMul;
			}
			else
			{
				r = a[0] / weight * exposureMul;
				g = a[1] / weight * exposureMul;
				b = a[2] / weight * exposureMul;
			}
			r = std::isfinite(r) ? r : 0.f;
			g = std::isfinite(g) ? g : 0.f;
			b = std::isfinite(b) ? b : 0.f;
			if (useAgx)
			{
				// lineal -> AgX (Base o Punchy); la salida ya viene
				// display-encoded 2.2: cuantizar directo, SIN OETF sRGB
				// (seria doble encode)
				AgxApply(r, g, b, punchy);
			}
			else if (softShoulder)
			{
				// lineal -> hombro suave por canal -> encode sRGB. Conserva
				// la saturacion del sRGB simple en los medios (por canal)
				r = EncodeSrgbF(ShoulderCurve(r));
				g = EncodeSrgbF(ShoulderCurve(g));
				b = EncodeSrgbF(ShoulderCurve(b));
			}
			else if (colorShoulder)
			{
				// Hombro preservando color (tonemap 4): la MISMA ShoulderCurve
				// del tonemap 3, pero aplicada sobre la luma Rec.709 y no por
				// canal, escalando el RGB por t/luma: el ratio de color se
				// conserva EXACTO al rodar (los highlights saturados no ruedan
				// a blanco cremoso como en el 3). Como el ratio preservado
				// puede dejar canales > 1 (p.ej. rojo saturado brillante),
				// soft-clip final POR CANAL (SoftClipChannel, identidad hasta
				// 0.98) antes del encode sRGB.
				float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
				float scale = (luma > 1.0e-6f) ? ShoulderCurve(luma) / luma : 1.f;
				r = EncodeSrgbF(SoftClipChannel(r * scale));
				g = EncodeSrgbF(SoftClipChannel(g * scale));
				b = EncodeSrgbF(SoftClipChannel(b * scale));
			}
			else
			{
				// sRGB simple (tonemap 0 y desconocidos)
				r = EncodeSrgbF(r);
				g = EncodeSrgbF(g);
				b = EncodeSrgbF(b);
			}
			// Saturacion post-tonemap en espacio display (comun a todos los
			// caminos; satMul = 1 -> identidad exacta, goldens intactos)
			ApplySaturation(r, g, b, satMul);
			// M7 adaptive: cobertura media / CONTEO POR PIXEL (w del AOV albedo).
			// Sin adaptive, spp global invSpp (mismo valor y misma expresion =
			// BIT-IDENTICO). Sin esto, un pixel que paro pronto saldria casi
			// transparente (sumA/spp_global en vez de sumA/samples_del_pixel).
			float aovCnt = aovAlbedoData[(size_t)i * 4 + 3];
			float invCov = render.adaptive ? (aovCnt > 1.f ? 1.f / aovCnt : 1.f) : invSpp;
			float cov = Clamp01f(sumA * invCov);
			// FONDO DE PELICULA: OVER en display sobre el color del usuario. Con
			// el ajuste apagado esta rama NO se toca y el resolve queda BYTE A
			// BYTE como siempre (ni una multiplicacion de mas).
			if (backdropOn)
			{
				// Color del fondo en DISPLAY. La rampa se interpola en codigos de
				// pantalla (que es como se elige un degradado: los dos extremos
				// son los colores del selector y el paso visual es uniforme), y
				// SOLO despues se pasa a luz para componer.
				float bc[3] = { bdA[0], bdA[1], bdA[2] };
				if (backdropRamp)
				{
					IntType px = (i % bdW) + bdOx;
					IntType py = (i / bdW) + bdOy;
					float u = (float)px * bdInvW - 0.5f;
					float v = (float)py * bdInvH - 0.5f;
					float t = Clamp01f(0.5f + (u * bdDx + v * bdDy) / bdExt);
					bc[0] = bdA[0] + (bdB[0] - bdA[0]) * t;
					bc[1] = bdA[1] + (bdB[1] - bdA[1]) * t;
					bc[2] = bdA[2] + (bdB[2] - bdA[2]) * t;
				}
				if (cov <= 0.f)
				{
					// Fondo puro: el color del usuario TAL CUAL, sin pasar por
					// lineal y volver (el ida y vuelta en float podria mover un
					// byte en el borde de cuantizacion). Es el 60 % del cuadro en
					// un hero de producto, asi que ademas se ahorra el pow.
					r = bc[0]; g = bc[1]; b = bc[2];
				}
				else
				{
					// OVER EN LUZ (espacio lineal), no en codigos: ver DecodeSrgbF.
					float ia = 1.f - cov;
					r = EncodeSrgbF(DecodeSrgbF(r) * cov + DecodeSrgbF(bc[0]) * ia);
					g = EncodeSrgbF(DecodeSrgbF(g) * cov + DecodeSrgbF(bc[1]) * ia);
					b = EncodeSrgbF(DecodeSrgbF(b) * cov + DecodeSrgbF(bc[2]) * ia);
				}
				// El render opaco sigue siendo opaco: el fondo ES la imagen.
				cov = 1.f;
			}
			p[0] = (uchar)(Clamp01f(r) * 255.f + 0.5f);
			p[1] = (uchar)(Clamp01f(g) * 255.f + 0.5f);
			p[2] = (uchar)(Clamp01f(b) * 255.f + 0.5f);
			p[3] = (uchar)(cov * 255.f + 0.5f);
		}

		if (!UploadToSurface(surfId, pixels.constData()))
			return fail("upload_failed");

		render.tonemap = tonemap;
		render.saturation = saturation;
		render.msFinish = MsSince(tFinish);
		render.state = "finished";
		DEBUG("Cinema render finish: " + NumStr(render.sampleIdx) + " spp -> surface "
			+ NumStr(surfId) + " (" + NumStr(render.msFinish) + " ms)");
		return QString();
	#endif // API_D3D11
	}

	// F2/R17 (rehecho en la ronda de fixes): mapa de PROFUNDIDAD del ultimo
	// render RESUELTO. La version anterior derivaba el mapa del AOV normal+depth
	// ACUMULADO y tenia dos defectos medidos en la escena de aceptacion:
	//  (1) el cielo acumula farPlane en el MISMO canal w que los hits, asi que
	//      todo pixel de cobertura parcial (bordes AA, bokeh del DoF, horizonte)
	//      salia sesgado hacia el fondo (el pilar a 96 u media 155 u);
	//  (2) la normalizacion min-max con un suelo infinito manda far a ~farPlane
	//      (9990 u) y comprime la escena util en gray 253-255: PNG casi blanco,
	//      inservible para el desplazamiento de parallax.
	// AHORA:
	//  - La FUENTE es un pase de SONDA dedicado (FRAMEF_DEPTH_PROBE, la misma
	//    sonda del autofocus R9): un rayo PINHOLE por pixel, distancia EXACTA
	//    del primer hit (cielo = -1). Sin acumulacion -> sin promedio -> sin
	//    sesgo de cobertura por construccion. El mapa queda sin AA ni DoF (el
	//    Z clasico de cualquier DCC: el Z no se antialiasea).
	//  - La CODIFICACION pasa a DISPARIDAD (1/d) lineal entre near y far:
	//    gray = 255 * (1/d - 1/far) / (1/near - 1/far). Robusta a un far
	//    enorme por construccion (el suelo del horizonte tiende a gray 0
	//    suavemente y los objetos cercanos conservan el contraste aunque
	//    far sea ~farPlane) y ademas es la magnitud FISICA del parallax
	//    (el desplazamiento aparente es proporcional a 1/d).
	//    Reconstruccion: d = 1 / ((gray/255) * (1/near - 1/far) + 1/far).
	// El pase de sonda escribe SOLO el AOV normal+depth; el acumulador y el
	// beauty NO se tocan -> el render queda byte-identico con el pase activado
	// o no. Debe llamarse tras el pt_render_finish final (que ya consumio los
	// AOV para el denoise; esta llamada los invalida) y antes del siguiente
	// pt_render_begin (los UAV siguen vivos).
	QString CinemaRenderSaveDepth(const QString& filename)
	{
	#if !API_D3D11
		(void)filename;
		return QString("error:cinema_requires_d3d11");
	#else
		if (render.sampleIdx < 1)
			return QString("error:no_samples");

		IntType w = render.width, h = render.height;
		IntType numPix = w * h;

		// --- pase de sonda (Cambio 12: factorizado en RunDepthProbe, mismo
		// PTFrame que el render -- misma camara/escena, que no han cambiado
		// entre el finish y esta llamada -- DoF apagado/pinhole y
		// noiseThreshold 0 dentro de FillFrame). Sirve a los DOS backends.
		std::vector<float> aovNormalDepthData;
		if (!RunDepthProbe(aovNormalDepthData, w, h))
			return QString("error:probe_failed");

		float farPlane = CinemaSceneCamera().farPlane;

		std::vector<float> depth((size_t)numPix);
		std::vector<BoolType> isSky((size_t)numPix);
		BoolType haveHit = false;
		// NOTA: "near"/"far" son macros vacias heredadas de windows.h (modelo de
		// memoria 16-bit) -- usar depthNear/depthFar para no chocar con ellas.
		float depthNear = 0.f, depthFar = 0.f;
		IntType skyCount = 0;
		for (IntType i = 0; i < numPix; i++)
		{
			// La sonda escribe la distancia del primer hit o -1 si el rayo se
			// escapa; un hit mas alla de farPlane*0.999 (el suelo infinito en el
			// horizonte extremo) se clasifica cielo igual que antes, y de paso
			// garantiza far <= far_plane en el sidecar.
			float d = aovNormalDepthData[(size_t)i * 4 + 3];
			BoolType sky = !(d > 0.f) || d >= farPlane * 0.999f;
			isSky[(size_t)i] = sky;
			if (sky)
			{
				depth[(size_t)i] = -1.f;
				skyCount++;
			}
			else
			{
				depth[(size_t)i] = d;
				if (!haveHit) { depthNear = d; depthFar = d; haveHit = true; }
				else { if (d < depthNear) depthNear = d; if (d > depthFar) depthFar = d; }
			}
		}
		if (!haveHit)
		{
			depthNear = 0.f;
			depthFar = farPlane;
		}

		// Codificacion en DISPARIDAD (ver contrato arriba). depthNear > 0
		// siempre que haya hits (t > PT_T_EPS); el guard de invRange cubre la
		// escena degenerada de un solo plano de profundidad (gray 255).
		float invNear2 = haveHit ? 1.f / depthNear : 1.f;
		float invFar2 = haveHit ? 1.f / depthFar : 0.f;
		float invRange = invNear2 - invFar2;
		if (invRange < 1e-12f)
			invRange = 1e-12f;

		QImage img(w, h, QImage::Format_RGBA8888);
		for (IntType y = 0; y < h; y++)
			for (IntType x = 0; x < w; x++)
			{
				size_t i = (size_t)y * (size_t)w + (size_t)x;
				uchar gray = 0;
				if (!isSky[i])
				{
					float v = 255.f * ((1.f / depth[i] - invFar2) / invRange);
					int gi = (int)(v + 0.5f);
					gray = (uchar)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
				}
				img.setPixel((int)x, (int)y, qRgba(gray, gray, gray, 255));
			}
		if (!img.save(filename))
			return QString("error:save_failed");

		float skyFraction = numPix > 0 ? (float)skyCount / (float)numPix : 0.f;

		// CAPAS SLICING: cachear el mapa recien calculado (unica fuente de los
		// cortes y de las estadisticas del manifest -- ver estado del modulo).
		depthMapDist = depth;             // ya trae -1 en el cielo
		depthMapW = w; depthMapH = h;
		depthMapNear = depthNear; depthMapFar = depthFar; depthMapFarPlane = farPlane;

		return QString("{\"near\":%1,\"far\":%2,\"far_plane\":%3,\"sky_fraction\":%4,\"width\":%5,\"height\":%6,"
			"\"encoding\":\"gray8 linear in disparity (1/world distance); 255=near, 0=far; sky=0; source=pinhole first-hit probe (no AA/DoF); d = 1/((gray/255)*(1/near-1/far)+1/far)\",\"units\":\"mi3_world_units\"}")
			.arg(depthNear, 0, 'g', 6).arg(depthFar, 0, 'g', 6).arg(farPlane, 0, 'g', 6)
			.arg(skyFraction, 0, 'g', 4).arg(w).arg(h);
	#endif // API_D3D11
	}

	// CAPAS SLICING (S2): histograma en disparidad del ULTIMO mapa de
	// profundidad cacheado por CinemaRenderSaveDepth.
	QString CinemaRenderDepthHist(IntType bins)
	{
		if (depthMapDist.empty())
			return QString("error:no_depth");
		if (bins < 16 || bins > 1024 || depthMapNear <= 0.f || depthMapFar <= depthMapNear)
			return QString("error:bad_arg");

		float invNear = 1.f / depthMapNear, invFar = 1.f / depthMapFar;
		float invRange = invNear - invFar;
		if (invRange < 1.0e-12f)
			return QString("error:bad_arg");

		std::vector<IntType> hist((size_t)bins, 0);
		IntType hits = 0, sky = 0;
		for (float d : depthMapDist)
		{
			if (!(d > 0.f))
			{
				sky++;
				continue;
			}
			float u = (1.f / d - invFar) / invRange;
			u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
			IntType b = (IntType)(u * (float)bins);
			if (b >= bins) b = bins - 1;
			if (b < 0) b = 0;
			hist[(size_t)b]++;
			hits++;
		}

		QString out = "ok w=" + QString::number(depthMapW) + " h=" + QString::number(depthMapH)
			+ " near=" + QString::number(depthMapNear, 'g', 6) + " far=" + QString::number(depthMapFar, 'g', 6)
			+ " hits=" + QString::number(hits) + " sky=" + QString::number(sky) + " bins=" + QString::number(bins) + ";";
		for (IntType b = 0; b < bins; b++)
			out += (b > 0 ? "," : "") + QString::number(hist[(size_t)b]);
		return out;
	}

	// CAPAS SLICING (S3): estadisticas por CAPA sobre el mapa cacheado.
	QString CinemaRenderDepthBands(const QString& cutsCsv)
	{
		if (depthMapDist.empty())
			return QString("error:no_depth");

		std::vector<float> cuts;
		{
			IntType pos = 0, len = cutsCsv.length();
			while (pos < len)
			{
				IntType comma = (IntType)cutsCsv.indexOf(',', pos);
				QString token = (comma < 0) ? cutsCsv.mid(pos) : cutsCsv.mid(pos, comma - pos);
				pos = (comma < 0) ? len : comma + 1;
				if (token.isEmpty())
					continue;
				bool okv = false;
				float v = token.toFloat(&okv);
				if (!okv || v <= 0.f)
					return QString("error:bad_arg");
				cuts.push_back(v);
			}
		}
		for (size_t i = 1; i < cuts.size(); i++)
			if (!(cuts[i] > cuts[i - 1]))
				return QString("error:bad_arg");

		IntType M = (IntType)cuts.size();
		IntType N = M + 1;
		if (N < 1 || N > 12)
			return QString("error:bad_arg");

		// Ventanas [t0_K,t1_K) de la banda K (0 = la mas lejana), identico a la
		// definicion de la planificacion: K0=[s_M,+inf), K=[s_{M-K},s_{M-K+1}),
		// K=N-1=[0,s_1). Aqui basta con K = M - count(cuts <= d) (mismo criterio
		// que CinemaSceneObjectBands).
		std::vector<std::vector<float>> bandDist((size_t)N);
		std::vector<IntType> bandPx((size_t)N, 0), bandSky((size_t)N, 0);
		for (float d : depthMapDist)
		{
			if (!(d > 0.f))
			{
				// cielo: pertenece SIEMPRE a la banda 0 (telon)
				bandPx[0]++;
				bandSky[0]++;
				continue;
			}
			// K = M - count(cuts <= d)
			IntType cnt = 0;
			for (IntType c = 0; c < M; c++)
				if (cuts[(size_t)c] <= d)
					cnt++;
			IntType K = M - cnt;
			bandPx[(size_t)K]++;
			bandDist[(size_t)K].push_back(d);
		}

		QString out = "ok bands=" + QString::number(N);
		for (IntType K = 0; K < N; K++)
		{
			if (bandPx[(size_t)K] == 0)
			{
				out += ";" + QString::number(K) + ",0,0,0,0,0,0";
				continue;
			}
			std::vector<float>& v = bandDist[(size_t)K];
			float d02 = 0.f, d50 = 0.f, d80 = 0.f, d98 = 0.f;
			if (!v.empty())
			{
				std::vector<float> tmp;
				tmp = v; d02 = CapasPercentile(tmp, 0.02f);
				tmp = v; d50 = CapasPercentile(tmp, 0.50f);
				tmp = v; d80 = CapasPercentile(tmp, 0.80f);
				tmp = v; d98 = CapasPercentile(tmp, 0.98f);
			}
			out += ";" + QString::number(K) + "," + QString::number(bandPx[(size_t)K]) + ","
				+ QString::number(bandSky[(size_t)K]) + ","
				+ QString::number(d02, 'g', 6) + "," + QString::number(d50, 'g', 6) + ","
				+ QString::number(d80, 'g', 6) + "," + QString::number(d98, 'g', 6);
		}
		return out;
	}

	// T6: concordancia POR RAYO entre la sonda de objetos (CPU, BVH) y el mapa de
	// profundidad (GPU, pinhole full-res). Comparar near/far de las dos sondas es
	// comparar EXTREMOS de dos rejillas de distinta densidad y no significa nada
	// (medido: la sintetica da near identico 24.09/24.09 y far 5549.89 vs 29947.4
	// = ratio 0.19 SOLO por densidad de muestreo). Aqui se compara rayo a rayo.
	//
	// Para cada pixel (x,y) de la sonda se toma el pixel MAS CERCANO del mapa
	// (misma convencion de centro de pixel x/probeW <-> mx/mapW que usan las dos
	// sondas); se acepta como CONCORDANTE si d_probe cae dentro del [min,max] de
	// la vecindad 3x3 del mapa con margen del 2%. Un rayo de CIELO en la sonda
	// concuerda si su vecindad 3x3 en el mapa es TODO cielo (interpretacion
	// minima: a 320x180 contra full-res, el borde cielo/geometria es ruido de
	// muestreo esperado, no fuga de ventana). med_ratio es la MEDIANA de
	// d_probe/mediana(vecindad) sobre los pares con impacto en AMBAS sondas:
	// robusta a outliers, detecta un sesgo de ESCALA aunque el 2% por-rayo lo
	// tumbe entero (es justo el tipo de numero que habria destapado la
	// inconsistencia de unidades original con un solo dato).
	// Salida: "ok agree=<frac> med_ratio=<r> near_ratio=<r> co=<n> mapped=<n>
	//          sky_probe=<n> sky_map=<n>"
	// Errores: "error:no_depth" (sin mapa cacheado), "error:no_probe" (sin
	// cache de sonda, dimensiones invalidas o desajustadas).
	QString CinemaRenderProbeAgree()
	{
		if (depthMapDist.empty() || depthMapW <= 0 || depthMapH <= 0)
			return QString("error:no_depth");

		const std::vector<IntType>* probeObj = nullptr;
		const std::vector<float>* probeDist = nullptr;
		IntType pw = 0, ph = 0;
		CinemaSceneGetProbeCache(&probeObj, &probeDist, &pw, &ph);
		if (!probeObj || !probeDist || pw <= 0 || ph <= 0 ||
			(IntType)probeObj->size() != pw * ph || (IntType)probeDist->size() != pw * ph)
			return QString("error:no_probe");

		IntType mw = depthMapW, mh = depthMapH;
		IntType rays = pw * ph;
		IntType co = 0, mapped = 0, skyProbe = 0, skyMap = 0;
		std::vector<float> ratios;
		ratios.reserve((size_t)rays);
		float probeNear = 0.f;
		BoolType haveProbeNear = false;

		std::vector<float> nbr;
		nbr.reserve(9);
		for (IntType y = 0; y < ph; y++)
		{
			IntType my = (IntType)(((float)y + 0.5f) / (float)ph * (float)mh);
			if (my < 0) my = 0;
			if (my >= mh) my = mh - 1;
			for (IntType x = 0; x < pw; x++)
			{
				IntType idx = y * pw + x;
				IntType objId = (*probeObj)[(size_t)idx];
				float pd = (*probeDist)[(size_t)idx];
				BoolType probeSky = (objId < 0);

				if (!probeSky && (!haveProbeNear || pd < probeNear))
				{
					probeNear = pd;
					haveProbeNear = true;
				}

				IntType mx = (IntType)(((float)x + 0.5f) / (float)pw * (float)mw);
				if (mx < 0) mx = 0;
				if (mx >= mw) mx = mw - 1;

				nbr.clear();
				for (IntType dy = -1; dy <= 1; dy++)
				{
					IntType ny = my + dy;
					if (ny < 0 || ny >= mh) continue;
					for (IntType dx = -1; dx <= 1; dx++)
					{
						IntType nx = mx + dx;
						if (nx < 0 || nx >= mw) continue;
						float dm = depthMapDist[(size_t)ny * (size_t)mw + (size_t)nx];
						if (dm > 0.f)
							nbr.push_back(dm);
					}
				}
				BoolType mapCenterSky = !(depthMapDist[(size_t)my * (size_t)mw + (size_t)mx] > 0.f);
				if (mapCenterSky) skyMap++;

				if (probeSky)
				{
					skyProbe++;
					if (nbr.empty())
						co++;   // cielo de la sonda concuerda con cielo del mapa
					continue;
				}

				if (nbr.empty())
					continue;   // sonda impacta, vecindad del mapa TODO cielo: no comparable

				mapped++;
				std::sort(nbr.begin(), nbr.end());
				float nmin = nbr.front(), nmax = nbr.back();
				float nmed = nbr[nbr.size() / 2];
				float dlo = nmin * 0.98f, dhi = nmax * 1.02f;
				if (pd >= dlo && pd <= dhi)
					co++;
				if (nmed > 0.f)
					ratios.push_back(pd / nmed);
			}
		}

		float agree = rays > 0 ? (float)co / (float)rays : 0.f;
		float medRatio = 1.f;
		if (!ratios.empty())
		{
			std::sort(ratios.begin(), ratios.end());
			medRatio = ratios[ratios.size() / 2];
		}
		float nearRatio = (haveProbeNear && depthMapNear > 0.f) ? probeNear / depthMapNear : 0.f;

		return QString("ok agree=") + QString::number(agree, 'g', 6)
			+ " med_ratio=" + QString::number(medRatio, 'g', 6)
			+ " near_ratio=" + QString::number(nearRatio, 'g', 6)
			+ " co=" + QString::number(co)
			+ " mapped=" + QString::number(mapped)
			+ " sky_probe=" + QString::number(skyProbe)
			+ " sky_map=" + QString::number(skyMap);
	}

	// V1(b)+V2: estadisticas por capa medidas sobre los PNG ENTREGADOS y el mapa
	// de profundidad cacheado (el de la pasada 0; sólo lo reescribe
	// CinemaRenderSaveDepth, que no vuelve a correr en este export). Es la UNICA
	// medida de "lo que se ha entregado de verdad": la red de seguridad
	// post-render (SS3.4) y el manifest (SS4.2) consumen la MISMA cadena, asi que
	// es imposible que discrepen.
	//
	// top(p) = indice de la capa MAS CERCANA con alpha > 128 en p (K mayor = mas
	// cerca; K = 0 es el telon), -1 si ninguna la cubre. Percentiles con
	// CapasPercentile (nearest-rank redondeado): la MISMA convencion que
	// check_capasd2.py, que es lo que hace que U7' pase por construccion.
	//
	// Salida: "ok n=<N> w=<W> h=<H> geom=<n> sky=<n>" y por capa
	//   ";<K>,<alpha_px>,<top_px>,<top_px_geom>,<top_sky_px>,<d02>,<d50>,<d80>,<d98>"
	// Errores: "error:no_depth" (sin mapa cacheado), "error:bad_arg",
	//          "error:no_file:<K>" (PNG ausente/ilegible),
	//          "error:dims:<K>" (PNG con otras dimensiones que el mapa).
	QString CinemaRenderLayersTopStats(const QString& base, IntType n)
	{
		if (depthMapDist.empty() || depthMapW <= 0 || depthMapH <= 0)
			return QString("error:no_depth");
		if (n < 1 || n > 12)
			return QString("error:bad_arg");

		const IntType W = depthMapW, H = depthMapH;
		const size_t NPX = (size_t)W * (size_t)H;
		if (depthMapDist.size() != NPX)
			return QString("error:no_depth");

		std::vector<int32_t> top(NPX, -1);
		std::vector<IntType> alphaPx((size_t)n, 0);

		// pasada 1: una capa cada vez (nunca N imagenes a la vez en RAM); de lejos
		// a cerca, la ultima que cubre el pixel GANA.
		for (IntType K = 0; K < n; K++)
		{
			QString num = QString::number(K);
			while (num.length() < 2)
				num = QString("0") + num;
			QImage img(base + "_layer" + num + ".png");
			if (img.isNull())
				return QString("error:no_file:") + QString::number(K);
			if ((IntType)img.width() != W || (IntType)img.height() != H)
				return QString("error:dims:") + QString::number(K);
			img = img.convertToFormat(QImage::Format_RGBA8888);
			IntType cnt = 0;
			for (IntType y = 0; y < H; y++)
			{
				const uchar* row = img.constScanLine((int)y);
				for (IntType x = 0; x < W; x++)
				{
					if (row[(size_t)x * 4 + 3] > 128)
					{
						cnt++;
						top[(size_t)y * (size_t)W + (size_t)x] = (int32_t)K;
					}
				}
			}
			alphaPx[(size_t)K] = cnt;
		}

		// pasada 2: reparto por top() contra el mapa
		std::vector<IntType> topPx((size_t)n, 0), topGeom((size_t)n, 0), topSky((size_t)n, 0);
		std::vector<std::vector<float>> vals((size_t)n);
		IntType geom = 0, sky = 0;
		for (size_t i = 0; i < NPX; i++)
		{
			float d = depthMapDist[i];
			BoolType hasGeom = (d > 0.f);
			if (hasGeom) geom++; else sky++;
			int32_t K = top[i];
			if (K < 0)
				continue;
			topPx[(size_t)K]++;
			if (hasGeom)
			{
				topGeom[(size_t)K]++;
				vals[(size_t)K].push_back(d);
			}
			else
				topSky[(size_t)K]++;
		}

		QString out = "ok n=" + QString::number(n) + " w=" + QString::number(W)
			+ " h=" + QString::number(H) + " geom=" + QString::number(geom)
			+ " sky=" + QString::number(sky);
		for (IntType K = 0; K < n; K++)
		{
			float d02 = 0.f, d50 = 0.f, d80 = 0.f, d98 = 0.f;
			std::vector<float>& v = vals[(size_t)K];
			if (!v.empty())
			{
				std::vector<float> tmp;
				tmp = v; d02 = CapasPercentile(tmp, 0.02f);
				tmp = v; d50 = CapasPercentile(tmp, 0.50f);
				tmp = v; d80 = CapasPercentile(tmp, 0.80f);
				tmp = v; d98 = CapasPercentile(tmp, 0.98f);
			}
			out += ";" + QString::number(K) + "," + QString::number(alphaPx[(size_t)K])
				+ "," + QString::number(topPx[(size_t)K])
				+ "," + QString::number(topGeom[(size_t)K])
				+ "," + QString::number(topSky[(size_t)K])
				+ "," + QString::number(d02, 'g', 6) + "," + QString::number(d50, 'g', 6)
				+ "," + QString::number(d80, 'g', 6) + "," + QString::number(d98, 'g', 6);
		}
		return out;
	}

	// ---------------------------------------------------------------------
	// Status
	// ---------------------------------------------------------------------

	QString CinemaRenderStatus()
	{
		QString out;
		out += "state=" + render.state;
		out += " error=" + (render.error.isEmpty() ? QString("none") : render.error);
		out += " spp=" + QString::number(render.sampleIdx);
		out += " width=" + QString::number(render.width);
		out += " height=" + QString::number(render.height);
		out += " seed=" + QString::number(render.seed);
		out += " ms_begin=" + QString::number(render.msBegin, 'f', 2);
		out += " ms_sample=" + QString::number(render.msLastSample, 'f', 1);
		out += " ms_finish=" + QString::number(render.msFinish, 'f', 1);
		// Report the backend that actually completed the last Begin, not the
		// preferred backend for the next render. This survives a DXR fallback.
		out += " backend=" + QString(render.useDxr ? "DXR" : "D3D11");
		out += " denoiser=" + render.denoiser;
		out += " ms_denoise=" + QString::number(render.msDenoise, 'f', 1);
		out += " tonemap=" + QString::number(render.tonemap);
		out += " saturation=" + QString::number(render.saturation, 'f', 2);
		out += " adaptive=" + QString::number(render.adaptive ? 1 : 0);
		out += " active=" + QString::number(render.activePixels);
		return out;
	}

	// DOF-FIEL D1: ver contrato en CinemaRender.hpp. "planar=1" es constante
	// (A2 mide siempre en distancia planar, nunca radial).
	QString CinemaRenderDofInfo()
	{
		QString out;
		out += "mode=" + (render.dofMode.isEmpty() ? QString("none") : render.dofMode);
		out += " focus=" + QString::number(render.dofFocus, 'f', 4);
		out += " aperture=" + QString::number(render.dofAperture, 'f', 4);
		out += " region_hits=" + QString::number(render.dofRegionHits);
		out += " frame_hits=" + QString::number(render.dofFrameHits);
		out += " planar=1";
		return out;
	}

	// R10 (FIX 1/2): getters de timing para el preview interactivo (natives
	// pt_ms_begin / pt_ms_sample). Ver RenderState::msBegin / msLastSample.
	RealType CinemaRenderMsBegin() { return render.msBegin; }
	RealType CinemaRenderMsSample() { return render.msLastSample; }
	RealType CinemaRenderMsFinish() { return render.msFinish; }

	// ---------------------------------------------------------------------
	// M7 adaptive: terminacion temprana global + heatmap del conteo
	// ---------------------------------------------------------------------

	// Cuenta los pixeles NO convergidos (gVarCount.w < 0.5). Devuelve -1 si no hay
	// render adaptive en curso (o falla el readback). render_cinema lo sondea desde
	// GML a intervalos geometricos: cuando llega a 0, todos convergieron y el render
	// puede pararse aunque falten samples del presupuesto. Un readback (stall) por
	// sondeo, acotado a ~log2(presupuesto) llamadas -> coste despreciable frente al
	// tiempo ahorrado.
	IntType CinemaRenderActiveCount()
	{
	#if API_D3D11
		if (!render.adaptive || render.width < 1 || render.height < 1)
			return -1;
		IntType numPix = render.width * render.height;
		size_t numFloats = (size_t)numPix * 4;
		std::vector<float> vc(numFloats);
		if (render.useDxr)
		{
			if (!CinemaGpu12Readback(nullptr, nullptr, nullptr, vc.data()))
				return -1;
		}
		else if (!render.varCount || !CinemaGpu::ReadbackBuffer(render.varCount, vc.data(), numFloats * sizeof(float)))
			return -1;
		IntType active = 0;
		for (IntType i = 0; i < numPix; i++)
			if (!(vc[(size_t)i * 4 + 3] >= 0.5f)) // NaN-safe: NaN cuenta como activo
				active++;
		render.activePixels = active;
		return active;
	#else
		return -1;
	#endif
	}

	// Vuelca el CONTEO POR PIXEL de samples como heatmap RGBA8 al surface (evidencia
	// visual del gasto adaptive). Colormap jet: azul = pocas samples, rojo = muchas,
	// normalizado a maxCount (<= 0 => usa el spp maximo del render). Debe llamarse
	// TRAS el render y ANTES del siguiente Begin (el buffer aun tiene los conteos).
	QString CinemaRenderHeatmap(IntType surfId, RealType maxCount)
	{
	#if API_D3D11
		if (render.width < 1 || render.height < 1)
			return QString("no_render");
		Surface* surf = FindSurface(surfId);
		if (!surf || surf->size.width() != render.width || surf->size.height() != render.height)
			return QString("invalid_surface");
		IntType numPix = render.width * render.height;
		size_t numFloats = (size_t)numPix * 4;
		std::vector<float> vc(numFloats);
		if (render.useDxr)
		{
			if (!CinemaGpu12Readback(nullptr, nullptr, nullptr, vc.data()))
				return QString("readback_failed");
		}
		else if (!render.varCount || !CinemaGpu::ReadbackBuffer(render.varCount, vc.data(), numFloats * sizeof(float)))
			return QString("readback_failed");

		float invMax = 1.f / (float)(maxCount > 0.0 ? maxCount : (render.sampleIdx > 0 ? render.sampleIdx : 1));
		QVector<uchar> px((int)(numPix * 4));
		uchar* dst = px.data();
		RealType sumSpp = 0.0;
		std::vector<float> counts((size_t)numPix);
		float minSpp = 1.0e30f, maxSpp = 0.f;
		for (IntType i = 0; i < numPix; i++)
		{
			float cnt = vc[(size_t)i * 4 + 2];
			if (!std::isfinite(cnt) || cnt < 0.f)
				cnt = 0.f;
			sumSpp += cnt;
			counts[(size_t)i] = cnt;
			if (cnt < minSpp) minSpp = cnt;
			if (cnt > maxSpp) maxSpp = cnt;
			float t = cnt * invMax;
			t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
			// jet: rampas cruzadas por canal (azul->cian->verde->amarillo->rojo)
			float r = 1.5f - std::fabs(4.f * t - 3.f);
			float g = 1.5f - std::fabs(4.f * t - 2.f);
			float b = 1.5f - std::fabs(4.f * t - 1.f);
			uchar* p = dst + (size_t)i * 4;
			p[0] = (uchar)(Clamp01f(r) * 255.f + 0.5f);
			p[1] = (uchar)(Clamp01f(g) * 255.f + 0.5f);
			p[2] = (uchar)(Clamp01f(b) * 255.f + 0.5f);
			p[3] = 255;
		}
		if (!UploadToSurface(surfId, px.constData()))
			return QString("upload_failed");
		double meanSpp = numPix > 0 ? sumSpp / (double)numPix : 0.0;
		float medSpp = 0.f;
		if (numPix > 0)
		{
			size_t mid = (size_t)numPix / 2;
			std::nth_element(counts.begin(), counts.begin() + mid, counts.end());
			medSpp = counts[mid];
		}
		DEBUG("Cinema heatmap: spp medio " + QString::number(meanSpp, 'f', 1)
			+ " mediana " + QString::number(medSpp, 'f', 0)
			+ " min " + QString::number(minSpp, 'f', 0) + " max " + QString::number(maxSpp, 'f', 0)
			+ " (norm " + QString::number(1.0 / invMax, 'f', 0) + ")");
		return QString("mean_spp=%1 median=%2 min=%3 max=%4")
			.arg(meanSpp, 0, 'f', 2).arg(medSpp, 0, 'f', 0)
			.arg(minSpp, 0, 'f', 0).arg(maxSpp, 0, 'f', 0);
	#else
		(void)surfId; (void)maxCount;
		return QString("cinema_requires_d3d11");
	#endif
	}

	// ---------------------------------------------------------------------
	// Herramientas de diagnostico (paso 2 de M0)
	// ---------------------------------------------------------------------

	// Sonda de diagnostico: lee el pixel (10,10) del surface y lo loggea con
	// una etiqueta. Devuelve "r,g,b" o "" si el surface no existe.
	QString CinemaProbe(IntType surfId, const QString& tag)
	{
	#if API_D3D11
		Surface* surf = FindSurface(surfId);
		if (!surf || !surf->frameBuffer || !surf->frameBuffer->d3dColorTex)
		{
			DEBUG("CinemaProbe[" + tag + "]: surface " + QString::number(surfId) + " invalido");
			return QString();
		}
		FrameBuffer* fb = surf->frameBuffer;
		IntType w = surf->size.width(), h = surf->size.height();
		if (w < 16 || h < 16)
			return QString();
		GFX->SubmitBatch();
		QVector<uchar> chk(w * h * fb->GetPixelSize(true));
		fb->CopyColorData(chk.data());
		QString out;
		if (fb->hdr)
		{
			const float* fp = (const float*)chk.constData();
			IntType i = (10 * w + 10) * 4;
			out = QString("%1,%2,%3").arg(fp[i]).arg(fp[i+1]).arg(fp[i+2]);
		}
		else
		{
			IntType i = (10 * w + 10) * 4;
			out = QString("%1,%2,%3").arg(chk[i]).arg(chk[i+1]).arg(chk[i+2]);
		}
		DEBUG("CinemaProbe[" + tag + "]: surf " + QString::number(surfId) + " px(10,10)=" + out
			+ " tex=" + QString::number((quintptr)fb->d3dColorTex, 16));
		return out;
	#else
		(void)surfId; (void)tag;
		return QString();
	#endif
	}

	BoolType CinemaFinishTest(IntType surfId)
	{
	#if API_D3D11
		Surface* surf = FindSurface(surfId);
		if (!surf || !surf->frameBuffer)
		{
			WARNING("CinemaFinishTest: surface " + QString::number(surfId) + " invalido");
			return false;
		}
		FrameBuffer* fb = surf->frameBuffer;
		IntType w = surf->size.width();
		IntType h = surf->size.height();
		if (w <= 0 || h <= 0)
			return false;

		// Patron determinista en RGBA8; UploadToSurface lo expande a float
		// si el framebuffer es HDR (mismo camino compartido con
		// CinemaRenderFinish)
		QVector<uchar> px((int)(w * h * 4));
		for (IntType y = 0; y < h; y++)
			for (IntType x = 0; x < w; x++)
			{
				IntType i = (y * w + x) * 4;
				uchar checker = (((x / 32) + (y / 32)) & 1) ? 255 : 64;
				px[(int)i + 0] = (uchar)(x * 255 / (w > 1 ? w - 1 : 1));
				px[(int)i + 1] = (uchar)(y * 255 / (h > 1 ? h - 1 : 1));
				px[(int)i + 2] = checker;
				px[(int)i + 3] = 255;
			}

		if (!UploadToSurface(surfId, px.constData()))
			return false;

		// Verificacion inmediata: readback del pixel (10,10) y comparacion con
		// el patron esperado - el upload se AUTOVERIFICA (paranoia del paso de
		// humo M0; se puede quitar cuando pt_finish real este asentado).
		if (w > 10 && h > 10)
		{
			QVector<uchar> chk(w * h * fb->GetPixelSize(true));
			fb->CopyColorData(chk.data());
			IntType cx = 10, cy = 10;
			bool ok;
			if (fb->hdr)
			{
				const float* fp = (const float*)chk.constData();
				float r = fp[(cy * w + cx) * 4];
				ok = fabsf(r - (float)cx / (float)(w - 1)) < 0.01f;
			}
			else
			{
				uchar r = chk[(cy * w + cx) * 4];
				uchar esperado = (uchar)(cx * 255 / (w - 1));
				ok = (r >= esperado - 2 && r <= esperado + 2);
			}
			if (!ok)
			{
				WARNING("CinemaFinishTest: VERIFICACION FALLIDA - el upload no quedo en la textura");
				return false;
			}
		}

		DEBUG("CinemaFinishTest: gradiente subido y VERIFICADO en surface " + QString::number(surfId)
			+ " (" + QString::number(w) + "x" + QString::number(h) + (fb->hdr ? " hdr)" : ")"));
		return true;
	#else
		(void)surfId;
		return false;
	#endif
	}

	void CinemaRenderSetEmiCdf(BoolType on)
	{
		g_emiCdfOn = on;
	}
}
