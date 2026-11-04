#include "CinemaScene.hpp"

#include "CinemaGpu.hpp"
#include "CinemaKernelM0.hpp"
#include "CinemaTrimGen.hpp"  // TRIM: cotas de potencia del catalogo (CDF de emisores)
#include "CinemaOracle.hpp"   // V3 (T6, Cambio 22): CinemaOracleTraceCameraRay para la sonda

#include "Asset/Asset.hpp"
#include "Asset/Shader.hpp"
#include "Asset/Sprite.hpp"
#include "Asset/Surface.hpp"
#include "Asset/VertexBuffer.hpp"
#include "Render/FrameBuffer.hpp"
#include "Render/GraphicsApiHandler.hpp"
#include "Render/Texture.hpp"
#include "Render/TexturePage.hpp"

#include <QPair>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

// R18: presupuesto de memoria (RAM fisica libre + working set del proceso).
#if OS_WINDOWS
	#include <windows.h>
	#include <psapi.h>
	#ifdef _MSC_VER
		#pragma comment(lib, "psapi.lib")
	#endif
#endif

// CinemaScene: implementacion (ver CinemaScene.hpp para el contrato).
//
// Politica de errores (misma que CinemaGpu): nunca crashear. Todo fallo
// libera lo que alojo, emite WARNING y deja un codigo de error estable
// consultable via CinemaSceneStatus().

namespace CppProject
{
	// Los strides que sube este modulo son el contrato del kernel: verificados
	// aqui contra CinemaKernelM0.hpp ademas de los static_assert de tamano
	// absoluto de CinemaBvh.hpp / CinemaScene.hpp.
	static_assert(sizeof(PTVert) == CinemaKernelM0::PTVERT_STRIDE, "PTVert stride mismatch with kernel");
	static_assert(sizeof(PTTri) == CinemaKernelM0::PTTRI_STRIDE, "PTTri stride mismatch with kernel");
	static_assert(sizeof(PTNode) == CinemaKernelM0::PTNODE_STRIDE, "PTNode stride mismatch with kernel");
	static_assert(sizeof(PTMaterial) == CinemaKernelM0::PTMATERIAL_STRIDE, "PTMaterial stride mismatch with kernel");
	// M1: los tres buffers nuevos del build (luces + CDF emisiva) tambien se
	// suben con sizeof() como stride; verificados contra el contrato t5/t6/t7.
	static_assert(sizeof(PTLight) == CinemaKernelM0::PTLIGHT_STRIDE, "PTLight stride mismatch with kernel");
	static_assert(sizeof(uint32_t) == CinemaKernelM0::PTEMITRI_STRIDE, "emiTriIdx stride mismatch with kernel");
	static_assert(sizeof(float) == CinemaKernelM0::PTEMICDF_STRIDE, "emiCdf stride mismatch with kernel");

	BoolType cinemaSceneCaptureActive = false;
	BoolType cinemaRetainVbuffers = false;
	// CAPAS N v2 (D2): cache POR RAYO de la ultima CinemaSceneObjectDepths, la
	// consume CinemaSceneBandDepths. capasProbeObj[i] = objectId del primer
	// impacto del rayo i (-1 = cielo; 0 = impacto defensivo fuera de rango, va
	// a telon); capasProbeDist[i] = distancia radial (0 si cielo). Un elemento
	// por rayo, SIEMPRE, en el mismo orden (y,x) que el mapa de profundidad.
	// Se invalida en CinemaSceneCaptureBegin (empieza un build nuevo: la
	// geometria puede cambiar). Declarada aqui (no junto a
	// CinemaSceneObjectDepths, mas abajo en el archivo) para que
	// CinemaSceneCaptureBegin pueda limpiarla. ~460 KB, vive hasta el proximo
	// build.
	static std::vector<IntType> capasProbeObj;
	static std::vector<float>   capasProbeDist;
	// T6: dimensiones de la sonda que llena la cache de arriba (CinemaSceneGetProbeCache).
	static IntType capasProbeW = 0, capasProbeH = 0;
	// Sonda de alfa (ver ClassifyAlphaRegion): lineas ya emitidas por build.
	static IntType cinemaAlphaProbeLines = 0;
	// R13: readback de GPU como fallback del flatten (ON por defecto; el toggle
	// existe para el A/B de verificacion y como valvula de emergencia).
	BoolType cinemaReadbackVbuffers = true;
	// R18: radio de recorte por distancia (0 = OFF, el default -> bit-identico).
	static RealType cinemaClipRadius = 0.0;
	// R18 (harness): presupuesto de memoria forzado en MB (0 = medir el real).
	static RealType cinemaMemLimitMB = 0.0;

	// CAPAS SLICING (S1): ventana de profundidad del segmento de CAMARA para la
	// pasada de capa en curso. La fija GML por pasada (pt_band_window) y la leen
	// FillFrame (GPU) y CinemaOracleRender (CPU). OFF por defecto => todo el
	// camino de render queda BIT-IDENTICO.
	static BoolType cinemaBandOn = false;
	static float    cinemaBandT0 = 0.f;
	static float    cinemaBandT1 = 0.f;   // 0 o negativo = +infinito

	// CAPAS SLICING: API de la ventana de profundidad (declarada en
	// CinemaScene.hpp). No se resetea en CinemaSceneCaptureBegin ni en pt_build:
	// la ventana es estado de PASADA, no de escena. Quien la apaga es GML
	// (mi3_export_parallax_begin/apply) con pt_band_window(0, 0).
	void CinemaSceneBandWindowSet(float t0, float t1)
	{
		if (t0 <= 0.f && t1 <= 0.f) { cinemaBandOn = false; cinemaBandT0 = 0.f; cinemaBandT1 = 0.f; return; }
		if (t1 > 0.f && t0 > 0.f && t1 <= t0)
		{
			WARNING("Cinema band window: ventana vacia (t0=" + QString::number(t0) + " t1=" + QString::number(t1) + ") -- ignorada");
			cinemaBandOn = false; cinemaBandT0 = 0.f; cinemaBandT1 = 0.f; return;
		}
		cinemaBandOn = true;
		cinemaBandT0 = t0 > 0.f ? t0 : 0.f;
		cinemaBandT1 = t1 > 0.f ? t1 : 0.f;
	}
	BoolType CinemaSceneBandWindowActive() { return cinemaBandOn; }
	float    CinemaSceneBandWindowT0()     { return cinemaBandT0; }
	float    CinemaSceneBandWindowT1()     { return cinemaBandT1 > 0.f ? cinemaBandT1 : 1.0e30f; }

	// R18: memoria del sistema/proceso para el guard de presupuesto. Sin psapi:
	// GlobalMemoryStatusEx (kernel32) da la RAM fisica libre; el working set del
	// proceso sale de K32GetProcessMemoryInfo, que en Win7+ vive en kernel32.
	IntType CinemaSceneMemAvailableMB()
	{
	#if OS_WINDOWS
		MEMORYSTATUSEX ms;
		ms.dwLength = sizeof(ms);
		if (GlobalMemoryStatusEx(&ms))
			return (IntType)(ms.ullAvailPhys / (1024ull * 1024ull));
	#endif
		return -1;
	}

	// R18: PRESUPUESTO de memoria del build, en MB. No es la RAM libre a secas:
	//   - el techo DURO es el commit disponible (cuando se agota, la siguiente
	//     reserva lanza bad_alloc y la app muere: eso es lo que hay que evitar);
	//   - por debajo de el se permite morder una parte de la RAM total ademas de
	//     la libre (Windows desaloja caches y working sets ajenos sin drama),
	//     pero no toda: un build que se coma la maquina entera la deja inusable.
	// Devuelve -1 si no se puede consultar (entonces no se aplica el guard).
	static IntType CinemaSceneMemBudgetMB()
	{
		if (cinemaMemLimitMB > 0.0)
			return (IntType)cinemaMemLimitMB;
	#if OS_WINDOWS
		MEMORYSTATUSEX ms;
		ms.dwLength = sizeof(ms);
		if (GlobalMemoryStatusEx(&ms))
		{
			uint64_t mb = 1024ull * 1024ull;
			uint64_t availPhys = ms.ullAvailPhys / mb;
			uint64_t totalPhys = ms.ullTotalPhys / mb;
			uint64_t availCommit = ms.ullAvailPageFile / mb;
			uint64_t budget = availPhys + totalPhys / 4;
			if (budget > availCommit)
				budget = availCommit;
			return (IntType)budget;
		}
	#endif
		return -1;
	}

	IntType CinemaSceneMemProcessMB()
	{
	#if OS_WINDOWS
		PROCESS_MEMORY_COUNTERS pmc;
		pmc.cb = sizeof(pmc);
		if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
			return (IntType)((uint64_t)pmc.WorkingSetSize / (1024ull * 1024ull));
	#endif
		return -1;
	}

	// R18: VRAM local libre en MB (-1 si el runtime no expone IDXGIAdapter3).
	// Misma consulta que el native gml gpu_memory_available().
	static IntType CinemaGpuAvailableMB()
	{
	#if API_D3D11
		if (GFX && GFX->dxgiAdapter3)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO info;
			if (SUCCEEDED(GFX->dxgiAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				return (IntType)((info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0) / (1024 * 1024));
		}
	#endif
		return -1;
	}

	void CinemaSceneSetMemLimitMB(RealType mb)
	{
		cinemaMemLimitMB = (mb > 0.0) ? mb : 0.0;
	}

	void CinemaSceneSetClipRadius(RealType radius)
	{
		cinemaClipRadius = (radius > 0.0) ? radius : 0.0;
	}

	RealType CinemaSceneGetClipRadius()
	{
		return cinemaClipRadius;
	}

	namespace
	{
		const IntType CINEMA_ATLAS_SIZE = 4096;
		// Cota de seguridad: los indices de PTTri son uint32 y el presupuesto
		// VRAM del diseno es de unos pocos millones de tris.
		const IntType CINEMA_MAX_ELEMENTS = 100000000;

		// Region de una textura dentro del atlas. ox/oy/sx/sy remapean las
		// coordenadas ORIGINALES de la textura (0-1 sobre su imagen fuente)
		// al slice: uvSlice = (ox, oy) + uvTextura * (sx, sy).
		struct AtlasEntry
		{
			BoolType valid = false;
			uint32_t slice = 0;
			float ox = 0.f, oy = 0.f, sx = 0.f, sy = 0.f;
		};

		struct SceneState
		{
			// Captura
			IntType sectionTag = CINEMA_SECTION_DEFAULT;
			IntType camInvisible = 0; // R15: holdout del timeline en curso (pt_cam_invisible)
			IntType camMatte = 0;     // F1: holdout que OCLUYE del timeline en curso (pt_cam_matte)
			// F1 4a: shadow catcher del timeline en curso (pt_shadow_catcher)
			IntType shadowCatcher = 0;
			float shadowOpacity = 1.f;
			IntType objectLayer = 0;  // COBERTURA: capa de objeto en curso (pt_object_layer)
			IntType objectId = 0;     // CAPAS N: id de OBJETO en curso (pt_object_id)
			// FIX-B: uEmissive del draw en curso (pt_emissive). El shader de la
			// captura no declara ese uniform, asi que el valor NO se puede leer del
			// shader: viaja por estado de captura como la seccion.
			float emissive = 0.f;
			// TRIM: efecto en curso (pt_trim). Se resetea en CaptureBegin.
			IntType trimFx = 0;
			IntType trimOutline = 0;
			float trimIntensity = 1.f;
			float trimOrigin[3] = { 0.f, 0.f, 0.f };
			IntType trimMaskTexId = -1;
			float trimMaskUvRect[4] = { 0.f, 0.f, 1.f, 1.f };
			// TRIM: estado de ESCENA (pt_trim_scene, una vez por frame).
			CinemaTrimState trim;
			std::vector<uint32_t> trimSlots;
			std::vector<PTDraw> draws;
			IntType skippedNoShader = 0;

			// Entradas de escena (natives)
			CinemaEnvState env;
			CinemaCameraState camera;
			std::vector<PTLight> lights;

			// Escena aplanada (CPU)
			std::vector<PTVert> verts;
			std::vector<PTMaterial> materials;
			std::vector<PTDrawRange> ranges;
			CinemaBvh bvh;
			// R9 (FIX 1): el ultimo build salto el arbol BVH de CPU (preview DXR).
			// bvh.tris queda en ORDEN DE ENTRADA con un unico nodo dummy; el oraculo
			// y el fallback D3D11 llaman a CinemaSceneEnsureFullBuild() para
			// completar el reorden antes de trazar por el BVH de CPU.
			BoolType bvhDeferred = false;

			// CDF de triangulos emisores para NEE (M1). Arrays paralelos:
			// emiTriIdx indexa bvh.tris (el array REORDENADO que ve la GPU);
			// emiCdf es la CDF acumulada por potencia SIN normalizar
			// (emiCdf.back() == emiPower). Sin emissive_scale.
			std::vector<uint32_t> emiTriIdx;
			std::vector<float> emiCdf;
			float emiPower = 0.f;

			// M8-A ("escena viva", fase A): representacion en espacio LOCAL +
			// instancias TLAS para el backend DXR (toggle MI3_CINEMA_INSTANCED).
			// Es PARALELA a la escena MUNDO de arriba, que sigue intacta como
			// fuente del oraculo, del fallback D3D11 y de la CDF emisiva. Solo la
			// puebla el build con el toggle ON; solo la consume CinemaGpu12.
			//   vertsLocal: pos/normal/tangente LOCALES (UV/color/emissive = verts).
			//   trisLocal : winding FUENTE (SIN el swap de espejo), orden de ENTRADA
			//               por-draw contiguo (mismos rangos que 'ranges' -> una BLAS
			//               por draw desde su sub-rango). El espejo vive en la matriz
			//               de la instancia; el kernel lo compensa por det<0.
			//   emi*Local : CDF emisiva sobre trisLocal, area en MUNDO (via verts);
			//               emiO2W = object->world por emisor (el sampling de NEE no
			//               pasa por RayQuery y necesita su transform aparte).
			BoolType instanced = false;
			std::vector<PTVert> vertsLocal;
			std::vector<PTTri> trisLocal;
			std::vector<uint32_t> emiTriIdxLocal;
			std::vector<float> emiCdfLocal;
			std::vector<CinemaInstXform> emiO2W;

			// M8-B ("escena viva", fase B): snapshot del ULTIMO build instanciado
			// para clasificar un frame nuevo (marker distinto, misma geometria) como
			// transform-only vs rebuild SIN re-flatten. El Paso 0 midio que el
			// vbufferId ROTA cada captura (los buffers se regeneran por frame), asi
			// que el matching NO puede usar vbufferId: se hace por ORDINAL de draw
			// (el walk es determinista) + verificacion de campos ESTABLES (seccion,
            // cullMode). liveInstWorld/liveInstSection son paralelos a 'ranges'
			// (una entrada por instancia); liveEmiInstance mapea cada emisor de la
			// CDF a su instancia (para refrescar gEmiO2W al moverse). liveStructHash
			// es un hash order-dependiente de (seccion,cullMode) sobre TODOS los
			// draws capturados: si cambia -> topologia distinta -> rebuild seguro.
			std::vector<CinemaInstXform> liveInstWorld; // world por instancia (== ranges)
			std::vector<IntType> liveInstSection;       // sectionTag por instancia
			std::vector<uint64_t> liveInstGeoFp;        // M8-C: fingerprint de geometria por instancia
			std::vector<uint32_t> liveEmiInstance;      // instancia duena de cada emisor (== emiO2W)
			size_t liveDrawCount = 0;                   // nº de draws capturados en el build
			uint64_t liveStructHash = 0;                // hash estable (seccion+cull) de todos los draws
			size_t liveLightCount = 0;                  // nº de luces en el build
			uint64_t liveLightHash = 0;                 // hash del contenido de las luces
			BoolType worldStale = false;                // verts MUNDO desfasados tras un update en vivo
			RealType lastVertexAo = 0;                  // vertexAo del ultimo build (para re-flatten defensivo)

			// Atlas (CPU). packSlice/shelf* son el estado del shelf-packer de
			// imagenes sueltas; las TexturePages completas van a slice propio.
			QVector<QImage> slices;
			IntType packSlice = -1;
			IntType shelfX = 0, shelfY = 0, shelfH = 0;
			QHash<IntType, AtlasEntry> texEntries;      // texId -> region en el atlas
			QHash<IntType, QImage> resolvedImages;      // texId -> imagen fuente (para scans de alpha)
			// Clave EXACTA (texId, region empaquetada en 16 bits por coordenada):
			// un hash con colisiones aqui asignaria flags de alpha equivocados.
			QHash<QPair<IntType, quint64>, uint32_t> alphaClassCache;
			// Media sRGB de la region de albedo, empaquetada 0xRRGGBB (para la
			// potencia de la CDF emisiva; misma clave exacta que el alpha cache)
			QHash<QPair<IntType, quint64>, uint32_t> meanRgbCache;
			// Regiones ALPHA_BLEND ya blanqueadas en el atlas (ver
			// WhitenTransparentTexels; misma clave exacta que el alpha cache)
			QSet<QPair<IntType, quint64>> whitenedRegions;
			AtlasEntry whiteEntry;                      // parche blanco para texIds irresolubles

			// Render target activo cuando empezo la captura: el hook SOLO
			// registra draws dirigidos a este target. Cualquier draw bajo un
			// surface_set_target distinto durante la captura es trabajo
			// offscreen (p.ej. background_sky_update_clouds rasteriza su
			// textura a una surface temporal para leer los pixeles), NO
			// geometria de escena: se deja pasar al rasterizador real.
			// Sin este guard, ese draw 2D se registraba como un quad gigante
			// (el "emisor blanco") y la surface temporal quedaba VACIA,
			// corrompiendo el vbuffer de nubes.
			const void* captureTarget = nullptr;

			// Estado / stats
			QString state = "empty";
			QString error;
			CinemaSceneStats stats;

		#if API_D3D11
			CinemaSceneGpu gpu;
		#endif
		};

		SceneState scene;

		// -----------------------------------------------------------------
		// Helpers de color y codificacion de vertice
		// -----------------------------------------------------------------

		inline float Clamp01(float v)
		{
			return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
		}

		inline float SrgbToLinear(float c)
		{
			return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		// Color GML 0xBBGGRR (byte 0 = rojo) -> lineal x escala
		void GmlColorToLinear(IntType color, RealType scale, float out[3])
		{
			out[0] = SrgbToLinear((float)(color & 255) / 255.f) * (float)scale;
			out[1] = SrgbToLinear((float)((color >> 8) & 255) / 255.f) * (float)scale;
			out[2] = SrgbToLinear((float)((color >> 16) & 255) / 255.f) * (float)scale;
		}

		// FONDO DE PELICULA: color GML 0xBBGGRR -> DISPLAY [0,1], SIN linealizar.
		// El fondo de pelicula se compone en el resolve, DESPUES del tonemap y de
		// la saturacion, asi que el numero que el usuario eligio en el selector es
		// literalmente el byte que sale en el PNG. Linealizar aqui seria meterlo
		// en el camino de la radiancia, que es justo lo que este mando NO hace.
		void GmlColorToDisplay(IntType color, float out[3])
		{
			out[0] = Clamp01((float)(color & 255) / 255.f);
			out[1] = Clamp01((float)((color >> 8) & 255) / 255.f);
			out[2] = Clamp01((float)((color >> 16) & 255) / 255.f);
		}

		// Decodificacion EXACTA del encode del motor (Vertex::SetNormal:
		// byte = (n + 1) / 2 * 255). El kernel HLSL (DecodeNormal888) y el
		// oraculo CPU usan AHORA esta MISMA escala: antes dividian por 127 --
		// otra recta -- y los tres lados reconstruian normales distintas a
		// partir de los mismos bytes. La escala del raster (UNPACK_VERTEX_NORMAL
		// en Vertex.hpp, byte/127 - 1) es OTRA convencion y no se toca: es del
		// motor de rasterizado, no de Cinema.
		inline void DecodePacked888(uint32_t packed, float out[3])
		{
			out[0] = (float)(packed & 255u) * (2.f / 255.f) - 1.f;
			out[1] = (float)((packed >> 8u) & 255u) * (2.f / 255.f) - 1.f;
			out[2] = (float)((packed >> 16u) & 255u) * (2.f / 255.f) - 1.f;
		}

		// M8-B: FNV-1a helpers para las firmas del clasificador del camino vivo.
		inline uint64_t FnvU32(uint64_t h, uint32_t v)
		{
			for (int b = 0; b < 4; b++) { h ^= (uint64_t)((v >> (b * 8)) & 0xFFu); h *= 1099511628211ull; }
			return h;
		}
		inline uint64_t FnvF(uint64_t h, float f)
		{
			uint32_t u; memcpy(&u, &f, 4); return FnvU32(h, u);
		}
		// Firma de TOPOLOGIA de los draws: order-dependiente sobre campos ESTABLES
		// entre frames (seccion + cullMode). NO incluye vbufferId ni texId (el Paso 0
		// midio que ROTAN cada captura) ni la matriz de mundo (esa es justo lo que un
		// frame de animacion cambia). Igual entre dos markers -> misma topologia.
		inline uint64_t CinemaDrawTopologyHash(const std::vector<PTDraw>& draws)
		{
			uint64_t h = 1469598103934665603ull;
			// R15: camInvisible entra en la firma (bit 16) -- cambiar el holdout
			// es un cambio de MATERIAL, y el camino vivo (M8) solo re-sube
			// transforms: sin esto, tocar el toggle en el editor no repintaba.
			// F1: camMatte (bit 17), mismo motivo.
			// F4: texRepeat[0] (bit 18) -- ahora decide MATF_WRAP_UV, asi que
			// cambiarlo es un cambio de MATERIAL y el camino vivo (que solo
			// re-sube transforms) tiene que rendirse y reconstruir.
			for (size_t d = 0; d < draws.size(); d++)
			{
				h = FnvU32(h, (uint32_t)((draws[d].sectionTag & 0xFF) | ((draws[d].cullMode & 0xFF) << 8)
					| ((draws[d].camInvisible & 0x1) << 16)
					| ((draws[d].camMatte & 0x1) << 17)
					| ((draws[d].texRepeat[0] & 0x1) << 18)
					| ((draws[d].shadowCatcher & 0x1) << 19)));  // F1 4a
				// F1 4a: la OPACIDAD de la sombra tambien es material (va en
				// PTMaterial::shadowOpacity): animarla por keyframe tiene que
				// rendir el camino vivo M8, que solo re-sube transforms.
				h = FnvF(h, draws[d].shadowOpacity);
				// FIX-B: emissive entra en la firma. Antes era SIEMPRE 0 (el uniform
				// no llegaba) y por tanto constante; ahora es un mando del usuario que
				// decide PTMaterial::emissive Y la CDF de emisores (Paso 4b del
				// build). El camino vivo M8 solo re-sube transforms: sin esto, subir
				// EMISSIVE en el editor no encenderia la luz.
				h = FnvF(h, draws[d].emissive);
			}
			return h;
		}
		// Firma del contenido de las luces (posicion/color/rango/spot): detecta luces
		// animadas (mover una luz sin cambiar el nº de objetos = camino vivo, re-sube
		// gLights). NO incluye ids inestables.
		inline uint64_t CinemaLightsHash(const std::vector<PTLight>& lights)
		{
			uint64_t h = 1469598103934665603ull;
			for (size_t i = 0; i < lights.size(); i++)
			{
				const PTLight& l = lights[i];
				const float* f = &l.posx;
				for (int k = 0; k < 14; k++) h = FnvF(h, f[k]); // posx..fadeSize (14 floats)
				h = FnvU32(h, l.type);
			}
			return h;
		}

		// M8-C: fingerprint de CONTENIDO de la geometria FUENTE de un draw
		// (posiciones + normal + tangente + nº de verts/tris), INDEPENDIENTE del
		// vbufferId (que ROTA cada captura). Es la pieza que le faltaba al clasificador
		// de M8-B: el BEND de una parte REGENERA su vbuffer con vertices deformados
		// (model_shape_generate_block subdivide el bloque y curva los segmentos), asi
		// que dos frames con distinto bend tienen la MISMA firma de topologia
		// (seccion+cull) y la MISMA (o distinta) matriz de mundo, pero geometria LOCAL
		// distinta -> M8-B lo clasificaba transform-only y dejaba la malla del build
		// anterior (obsoleta). Comparar esta firma por ordinal detecta el cambio.
		//
		// Coste: hash FNV-1a de 5 words/vert. Para mallas enormes (scenery importado)
		// se ESTRATIFICA con un stride que acota ~2048 muestras/malla: el bend mueve
		// segmentos contiguos (muchos verts), asi que cualquier muestreo razonable lo
		// capta; y una edicion real de un solo vertice pasa por history_amount ->
		// rebuild total, no por el camino vivo. Devuelve nº de verts/tris FUENTE en
		// outVerts/outTris (para decidir parcial vs rebuild si el count cambia).
		inline uint64_t CinemaDrawGeoFingerprint(const PTDraw& draw, IntType* outVerts, IntType* outTris)
		{
			IntType nv = 0, nt = 0;
			uint64_t h = 1469598103934665603ull;
			VertexBuffer* buffer = FindVertexBuffer(draw.vbufferId);
			if (buffer)
			{
				for (Mesh<>* mesh : buffer->meshes)
				{
					if (!mesh || !mesh->numVertices || !mesh->numIndices)
						continue;
					if (mesh->vertexData.Size() < mesh->numVertices ||
						mesh->indexData.Size() < mesh->numIndices)
						continue;
					const Vertex* sv = mesh->vertexData.Data();
					IntType step = mesh->numVertices > 2048 ? (mesh->numVertices / 2048) : 1;
					for (IntType v = 0; v < mesh->numVertices; v += step)
					{
						const Vertex& s = sv[v];
						h = FnvF(h, s.x); h = FnvF(h, s.y); h = FnvF(h, s.z);
						h = FnvU32(h, s.normal); h = FnvU32(h, s.tangent);
					}
					nv += mesh->numVertices;
					nt += mesh->numIndices / 3;
				}
			}
			h = FnvU32(h, (uint32_t)nv);
			h = FnvU32(h, (uint32_t)nt);
			if (outVerts) *outVerts = nv;
			if (outTris) *outTris = nt;
			return h;
		}

		// Encode identico al del motor (Vertex::SetNormal/SetTangent)
		inline uint32_t EncodePacked888(const float v[3])
		{
			uint32_t bx = (uint32_t)(Clamp01((v[0] + 1.f) * 0.5f) * 255.f + 0.5f);
			uint32_t by = (uint32_t)(Clamp01((v[1] + 1.f) * 0.5f) * 255.f + 0.5f);
			uint32_t bz = (uint32_t)(Clamp01((v[2] + 1.f) * 0.5f) * 255.f + 0.5f);
			if (bx > 255u) bx = 255u;
			if (by > 255u) by = 255u;
			if (bz > 255u) bz = 255u;
			return bx | (by << 8u) | (bz << 16u);
		}

		// Politica de AO de vertice (seccion 7.1 / FIX 2 ronda 2): desvanece la
		// MAGNITUD acromatica del AO horneado preservando el CROMA (tinte de bioma).
		// Extraida para que el re-flatten PARCIAL de M8-C (bend) produzca bytes
		// IDENTICOS al flatten del build (gate bit-identico).
		inline uint32_t ApplyVertexAoColor(uint32_t c, BoolType applyAoPolicy, float aoMix)
		{
			if (applyAoPolicy && aoMix < 1.f)
			{
				float r = (float)(c & 255u);
				float g = (float)((c >> 8u) & 255u);
				float b = (float)((c >> 16u) & 255u);
				float vmax = r > g ? r : g;
				if (b > vmax) vmax = b;
				if (vmax > 0.f)
				{
					float vNew = 255.f + (vmax - 255.f) * aoMix;
					float scale = vNew / vmax;
					uint32_t rr = (uint32_t)(r * scale + 0.5f);
					uint32_t gg = (uint32_t)(g * scale + 0.5f);
					uint32_t bb = (uint32_t)(b * scale + 0.5f);
					c = (c & 0xFF000000u) | (rr > 255u ? 255u : rr)
					  | ((gg > 255u ? 255u : gg) << 8u)
					  | ((bb > 255u ? 255u : bb) << 16u);
				}
			}
			return c;
		}

		// Plegado de uDefaultEmissive en el byte emissive del vertice + bits wave/sss
		// (idem: compartido por FlattenDraw y el re-flatten parcial de M8-C).
		inline uint32_t FoldEmissiveDataFlags(uint32_t data, float defEmissive)
		{
			float e = (float)((data >> 8u) & 255u) * defEmissive;
			uint32_t eByte = (uint32_t)(e + 0.5f);
			if (eByte > 255u)
				eByte = 255u;
			return (data & 7u) | (eByte << 8u);
		}

		// -----------------------------------------------------------------
		// Lectura de uniforms por nombre en el instante del draw. Los valores
		// no estaticos viven en el slot actual del batch buffer
		// (batchBufferObjectIndex no avanza durante la captura: el hook
		// retorna antes de SubmitObject); los estaticos en staticBufferData.
		// -----------------------------------------------------------------

		const char* UniformBase(Shader* shader, const Shader::UniformState& uni)
		{
			if (uni.isStatic)
				return shader->staticBufferData;
			if (!shader->batchBufferData)
				return nullptr;
			return shader->batchBufferData + shader->batchBufferObjectIndex * shader->batchBufferObjectSize;
		}

		float ReadUniformFloat(Shader* shader, const char* name, float def)
		{
			IntType idx = shader->GetUniformIndex(StringType(name));
			if (idx < 0)
				return def;
			Shader::UniformState uni = shader->uniforms.value(idx);
			const char* base = UniformBase(shader, uni);
			if (!base)
				return def;
			if (uni.type == Shader::FLOAT)
			{
				float v;
				memcpy(&v, base + uni.bufferOffset, sizeof(float));
				return v;
			}
			if (uni.type == Shader::INT)
			{
				int32_t v;
				memcpy(&v, base + uni.bufferOffset, sizeof(int32_t));
				return (float)v;
			}
			return def;
		}

		IntType ReadUniformInt(Shader* shader, const char* name, IntType def)
		{
			IntType idx = shader->GetUniformIndex(StringType(name));
			if (idx < 0)
				return def;
			Shader::UniformState uni = shader->uniforms.value(idx);
			const char* base = UniformBase(shader, uni);
			if (!base)
				return def;
			if (uni.type == Shader::INT)
			{
				int32_t v;
				memcpy(&v, base + uni.bufferOffset, sizeof(int32_t));
				return (IntType)v;
			}
			if (uni.type == Shader::FLOAT)
			{
				float v;
				memcpy(&v, base + uni.bufferOffset, sizeof(float));
				return (IntType)v;
			}
			return def;
		}

		// Deja out intacto (defaults del PTDraw) si el uniform no existe
		void ReadUniformVec4(Shader* shader, const char* name, float out[4])
		{
			IntType idx = shader->GetUniformIndex(StringType(name));
			if (idx < 0)
				return;
			Shader::UniformState uni = shader->uniforms.value(idx);
			if (uni.type != Shader::VEC4)
				return;
			const char* base = UniformBase(shader, uni);
			if (!base)
				return;
			memcpy(out, base + uni.bufferOffset, 4 * sizeof(float));
		}

		// -----------------------------------------------------------------
		// Atlas
		// -----------------------------------------------------------------

	#if API_D3D11
		// Resuelve un id de textura ligada (Shader::samplerState.currentTexId,
		// ya resuelto a Texture/FrameBuffer por SubmitTexture) a su QImage
		// fuente. La clase Texture NO guarda copia CPU: se busca el dueno.
		BoolType ResolveTexImage(IntType texId, QImage& out)
		{
			if (texId <= 0)
				return false;

			// TexturePages (personajes/sprites): QImage permanente de la pagina
			for (TexturePage* page : TexturePage::pages)
			{
				if (page && page->texture && page->texture->d3dSRVId == texId && !page->image.isNull())
				{
					out = page->image;
					return true;
				}
			}

			// Frames de Sprite con imagen propia (block sheets compuestos via
			// Surface -> Sprite(Surface*), ground, texturas de modelo sueltas)
			for (Sprite* sprite : Sprite::allSprites)
			{
				if (!sprite)
					continue;
				for (Sprite::Frame* frame : sprite->frames)
				{
					if (frame && frame->texture && frame->texture->d3dSRVId == texId && !frame->image.isNull())
					{
						out = frame->image;
						return true;
					}
				}
			}

			// Surfaces ligadas directo (surface_get_texture -> FrameBuffer):
			// readback GPU una vez (patron Surface::ToImage)
			for (IntType i = 0; i < Asset::assets.Size(); i++)
			{
				Asset* asset = Asset::assets.Value(i);
				if (!asset || asset->assetId != ID_Surface)
					continue;
				Surface* surf = (Surface*)asset;
				if (surf->frameBuffer && surf->frameBuffer->GetColorTexId() == texId)
				{
					out = surf->ToImage();
					return true;
				}
			}

			return false;
		}

		// Readback GPU de la textura REALMENTE bindeada al shader por texId, en
		// vez de la imagen CPU fuente (page->image / frame->image = el PNG de
		// disco). Cualquier textura GENERADA EN RUNTIME sobre la GPU (skins de
		// armadura, tintes, trims re-bakeados, sheets recompuestos) esta SIN
		// reflejar en la copia CPU; el raster muestrea la GPU, asi que Cinema
		// debe hacer lo mismo para pintar los MISMOS texeles. Se lee el texId
		// exacto que la captura vio bindeado (Texture::d3dIdSRVMap cubre tanto
		// Texture como los FrameBuffer de las surfaces). Staging + CopyResource
		// + Map del mip 0 (patron FrameBuffer::CopyData). Solo RGBA8
		// (UNORM/_SRGB/TYPELESS: mismos bytes, el atlas ya trabaja en sRGB);
		// comprimidos/float (HDR) -> false, y el llamador cae al camino CPU.
		// Una copia por textura unica por build (AtlasAddTexture deduplica por
		// texId y cachea en resolvedImages). Sin hilos nuevos: corre en el hilo
		// de render con el contexto inmediato. No toca kernel ni oraculo: la
		// paridad CPU/GPU se conserva porque el oraculo lee este MISMO atlas CPU.
		BoolType ReadbackGpuTexImage(IntType texId, QImage& out)
		{
			if (texId <= 0)
				return false;

			ID3D11ShaderResourceView* srv = Texture::d3dIdSRVMap.value(texId, nullptr);
			if (!srv)
				return false;

			ID3D11Resource* res = nullptr;
			srv->GetResource(&res);
			if (!res)
				return false;

			ID3D11Texture2D* tex = nullptr;
			res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
			res->Release();
			if (!tex)
				return false;

			D3D11_TEXTURE2D_DESC desc = {};
			tex->GetDesc(&desc);

			// Solo RGBA8: los bytes de UNORM/_SRGB/TYPELESS son identicos y el
			// atlas ya interpreta sRGB. Cualquier otra cosa (BC*, R32F HDR de
			// los FrameBuffers) -> fallback al camino CPU con aviso una vez por
			// textura (AtlasAddTexture ya deduplica por texId).
			if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
				&& desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
				&& desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS)
			{
				WARNING("Cinema atlas: texture id " + NumStr(texId)
					+ " GPU format " + NumStr((IntType)desc.Format)
					+ " no es RGBA8, usando imagen CPU");
				tex->Release();
				return false;
			}
			if (desc.Width == 0 || desc.Height == 0)
			{
				tex->Release();
				return false;
			}

			// Staging con la MISMA desc (mip 0 completo); se copia todo el
			// recurso y se mapea el subrecurso 0 (mip 0, ArraySlice 0).
			D3D11_TEXTURE2D_DESC sd = desc;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;

			ID3D11Texture2D* staging = nullptr;
			if (D3DCheckErrorSoft(D3DDevice->CreateTexture2D(&sd, nullptr, &staging)) || !staging)
			{
				tex->Release();
				return false;
			}

			D3DContext->CopyResource(staging, tex);

			D3D11_MAPPED_SUBRESOURCE map = {};
			if (D3DCheckErrorSoft(D3DContext->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
			{
				staging->Release();
				tex->Release();
				return false;
			}

			QImage img((int)desc.Width, (int)desc.Height, QImage::Format_RGBA8888);
			const IntType rowBytes = (IntType)desc.Width * 4;
			for (IntType y = 0; y < (IntType)desc.Height; y++)
				memcpy(img.scanLine((int)y),
					   (const uchar*)map.pData + (size_t)y * map.RowPitch,
					   (size_t)rowBytes);

			D3DContext->Unmap(staging, 0);
			staging->Release();
			tex->Release();

			out = img;
			return true;
		}

		// R13: FALLBACK UNIVERSAL del flatten. El motor libera la copia CPU de
		// los vbuffers que no caben en un batch (Mesh::CreateBuffers(true) desde
		// VertexBuffer::Complete, y VertexBuffer::Write tras escribir el
		// .meshcache), asi que un scenery recien importado -- la ISLA del
		// usuario -- llega al build sin vertexData/indexData y el build tolerante
		// lo OMITIA (WARNING "N draws sin copia CPU"). Pero los bytes siguen
		// VIVOS en la GPU: los buffers D3D11 del Mesh son USAGE_DEFAULT, asi que
		// se pueden copiar a un staging y mapear. Este helper reconstruye
		// vertexData/indexData EXACTAMENTE con el layout que el flatten ya
		// consume (Vertex de 36 B y uint32 de indice: los mismos bytes que
		// CreateBuffers subio con CreateBuffer + D3D11_SUBRESOURCE_DATA), asi
		// que el draw se procesa NORMAL y los fingerprints de M8-C hashean la
		// misma fuente. Mismo patron que ReadbackGpuTexImage (atlas, ronda 3):
		// staging + CopyResource + Map en el hilo de render con el contexto
		// inmediato, sin hilos nuevos. La copia se deja PERMANENTE en el Mesh:
		// una vez por mesh (los builds/scrubs siguientes salen gratis) y el
		// camino vivo (fingerprints C1, ReflattenDrawLocal) la ve igual que si
		// nunca se hubiera liberado. NO cambia el raster: la elegibilidad de
		// batch se decide por numIndices, jamas por la presencia de la copia CPU.
		// Cualquier fallo devuelve false sin tocar nada mas -> el llamador cae al
		// WARNING historico. Devuelve los bytes reconstruidos en outBytes.
		BoolType ReadbackMeshCpuData(Mesh<>* mesh, int64_t& outBytes)
		{
			outBytes = 0;
			if (!cinemaReadbackVbuffers || !mesh || !mesh->numVertices || !mesh->numIndices)
				return false;

			// Sin buffers GPU no hay de donde leer (p.ej. CreateBuffers soft-fail
			// por falta de VRAM: el raster tambien se salta esa malla).
			if (!mesh->d3dVertexBuffer || !mesh->d3dIndexBuffer)
				return false;

			const IntType vBytes = mesh->numVertices * (IntType)sizeof(Vertex);
			const IntType iBytes = mesh->numIndices * (IntType)sizeof(uint32_t);

			D3D11_BUFFER_DESC vDesc = {}, iDesc = {};
			mesh->d3dVertexBuffer->GetDesc(&vDesc);
			mesh->d3dIndexBuffer->GetDesc(&iDesc);
			// El buffer GPU debe cubrir lo que el flatten va a leer; si no casa,
			// el layout no es el esperado -> no arriesgar bytes basura.
			if ((IntType)vDesc.ByteWidth < vBytes || (IntType)iDesc.ByteWidth < iBytes)
			{
				WARNING("Cinema readback: buffer GPU mas pequeno que la malla ("
					+ NumStr((IntType)vDesc.ByteWidth) + "/" + NumStr(vBytes) + " v, "
					+ NumStr((IntType)iDesc.ByteWidth) + "/" + NumStr(iBytes) + " i)");
				return false;
			}

			auto copyOut = [](ID3D11Buffer* src, UINT byteWidth, void* dst, IntType bytes) -> BoolType
			{
				D3D11_BUFFER_DESC sd = {};
				sd.ByteWidth = byteWidth;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				sd.StructureByteStride = 0;

				ID3D11Buffer* staging = nullptr;
				if (D3DCheckErrorSoft(D3DDevice->CreateBuffer(&sd, nullptr, &staging)) || !staging)
					return false;

				D3DContext->CopyResource(staging, src);

				D3D11_MAPPED_SUBRESOURCE map = {};
				if (D3DCheckErrorSoft(D3DContext->Map(staging, 0, D3D11_MAP_READ, 0, &map)) || !map.pData)
				{
					staging->Release();
					return false;
				}

				memcpy(dst, map.pData, (size_t)bytes);
				D3DContext->Unmap(staging, 0);
				staging->Release();
				return true;
			};

			// Solo se reconstruye lo que falta (una malla puede tener una mitad
			// viva). Alloc sin clear: se sobrescribe entera con el memcpy.
			if (mesh->vertexData.Size() < mesh->numVertices)
			{
				mesh->vertexData.Alloc(mesh->numVertices, false);
				if (!mesh->vertexData.data
					|| !copyOut(mesh->d3dVertexBuffer, vDesc.ByteWidth, mesh->vertexData.data, vBytes))
				{
					mesh->vertexData.FreeData(); // estado limpio = "sigue faltando"
					return false;
				}
				outBytes += (int64_t)vBytes;
			}

			if (mesh->indexData.Size() < mesh->numIndices)
			{
				mesh->indexData.Alloc(mesh->numIndices, false);
				if (!mesh->indexData.data
					|| !copyOut(mesh->d3dIndexBuffer, iDesc.ByteWidth, mesh->indexData.data, iBytes))
				{
					mesh->indexData.FreeData();
					return false;
				}
				outBytes += (int64_t)iBytes;
			}

			return true;
		}
	#endif

		void AtlasNewPackSlice()
		{
			QImage img(CINEMA_ATLAS_SIZE, CINEMA_ATLAS_SIZE, QImage::Format_RGBA8888);
			img.fill(Qt::transparent);
			scene.slices.append(img);
			scene.packSlice = scene.slices.size() - 1;
			scene.shelfX = 0;
			scene.shelfY = 0;
			scene.shelfH = 0;
		}

		// Shelf-packer simple (CINEMA_DESIGN.md 4.5): filas de altura variable,
		// 1 px de separacion contra el sangrado por redondeo de floats.
		BoolType AtlasAllocRect(IntType w, IntType h, IntType& outSlice, IntType& outX, IntType& outY)
		{
			if (w <= 0 || h <= 0 || w > CINEMA_ATLAS_SIZE || h > CINEMA_ATLAS_SIZE)
				return false;

			if (scene.packSlice < 0)
				AtlasNewPackSlice();

			if (scene.shelfX + w > CINEMA_ATLAS_SIZE)
			{
				scene.shelfY += scene.shelfH;
				scene.shelfX = 0;
				scene.shelfH = 0;
			}

			if (scene.shelfY + h > CINEMA_ATLAS_SIZE)
				AtlasNewPackSlice();

			outSlice = scene.packSlice;
			outX = scene.shelfX;
			outY = scene.shelfY;
			scene.shelfX += w + 1;
			if (h + 1 > scene.shelfH)
				scene.shelfH = h + 1;
			return true;
		}

		void AtlasBlit(QImage& dst, const QImage& src, IntType x, IntType y)
		{
			IntType rowBytes = (IntType)src.width() * 4;
			for (IntType row = 0; row < src.height(); row++)
				memcpy(dst.scanLine((int)(y + row)) + x * 4, src.constScanLine((int)row), (size_t)rowBytes);
		}

		void AtlasAddWhite()
		{
			QImage white(8, 8, QImage::Format_RGBA8888);
			white.fill(QColor(255, 255, 255, 255));

			IntType slice = 0, x = 0, y = 0;
			if (!AtlasAllocRect(8, 8, slice, x, y))
				return; // imposible: primera reserva

			AtlasBlit(scene.slices[(int)slice], white, x, y);
			scene.whiteEntry.valid = true;
			scene.whiteEntry.slice = (uint32_t)slice;
			// Escala 0: el remap colapsa CUALQUIER UV al centro del parche
			// (una UV fuera de [0,1] jamas sale del blanco)
			scene.whiteEntry.ox = ((float)x + 4.f) / (float)CINEMA_ATLAS_SIZE;
			scene.whiteEntry.oy = ((float)y + 4.f) / (float)CINEMA_ATLAS_SIZE;
			scene.whiteEntry.sx = 0.f;
			scene.whiteEntry.sy = 0.f;
		}

	#if API_D3D11
		AtlasEntry AtlasAddTexture(IntType texId)
		{
			if (scene.texEntries.contains(texId))
				return scene.texEntries.value(texId);

			AtlasEntry entry;
			QImage img;
			// Preferir la textura GPU realmente bindeada (texturas generadas en
			// runtime: armaduras/tintes/sheets recompuestos); fallback al camino
			// CPU y, si tampoco resuelve, al parche blanco (historico).
			if (!ReadbackGpuTexImage(texId, img) && !ResolveTexImage(texId, img))
			{
				if (texId > 0)
				{
					scene.stats.texturesMissing++;
					WARNING("Cinema atlas: no CPU image for texture id " + NumStr(texId) + ", using white");
				}
				scene.texEntries.insert(texId, entry);
				return entry;
			}

			if (img.format() != QImage::Format_RGBA8888)
				img = img.convertToFormat(QImage::Format_RGBA8888);

			// Mas grande que un slice: reescalar preservando aspecto (el
			// troceo con uvRect remapeada queda como refinamiento M4)
			if (img.width() > CINEMA_ATLAS_SIZE || img.height() > CINEMA_ATLAS_SIZE)
			{
				WARNING("Cinema atlas: texture id " + NumStr(texId) + " is "
					+ NumStr((IntType)img.width()) + "x" + NumStr((IntType)img.height())
					+ ", scaling down to fit " + NumStr(CINEMA_ATLAS_SIZE));
				img = img.scaled((int)CINEMA_ATLAS_SIZE, (int)CINEMA_ATLAS_SIZE,
					Qt::KeepAspectRatio, Qt::FastTransformation);
				if (img.format() != QImage::Format_RGBA8888)
					img = img.convertToFormat(QImage::Format_RGBA8888);
			}

			scene.resolvedImages.insert(texId, img);

			if (img.width() == CINEMA_ATLAS_SIZE && img.height() == CINEMA_ATLAS_SIZE)
			{
				// TexturePage completa (o sheet 4096): slice dedicado, cero
				// copia (QImage comparte el buffer de la pagina)
				scene.slices.append(img);
				entry.valid = true;
				entry.slice = (uint32_t)(scene.slices.size() - 1);
				entry.ox = 0.f;
				entry.oy = 0.f;
				entry.sx = 1.f;
				entry.sy = 1.f;
			}
			else
			{
				IntType slice = 0, x = 0, y = 0;
				if (!AtlasAllocRect(img.width(), img.height(), slice, x, y))
				{
					scene.stats.texturesMissing++;
					scene.texEntries.insert(texId, entry);
					return entry;
				}
				AtlasBlit(scene.slices[(int)slice], img, x, y);
				entry.valid = true;
				entry.slice = (uint32_t)slice;
				entry.ox = (float)x / (float)CINEMA_ATLAS_SIZE;
				entry.oy = (float)y / (float)CINEMA_ATLAS_SIZE;
				entry.sx = (float)img.width() / (float)CINEMA_ATLAS_SIZE;
				entry.sy = (float)img.height() / (float)CINEMA_ATLAS_SIZE;
			}

			scene.texEntries.insert(texId, entry);
			return entry;
		}

		// Region de textura fuente que un draw realmente muestrea via su
		// uvRect (para sprites en pagina: el rect del sprite, no la pagina
		// entera), en texeles [x0,x1) x [y0,y1), siempre no vacia.
		void TexRegionBounds(const QImage& img, const float rect[4],
			IntType& x0, IntType& y0, IntType& x1, IntType& y1)
		{
			IntType w = img.width(), h = img.height();
			x0 = (IntType)std::floor((RealType)rect[0] * w);
			y0 = (IntType)std::floor((RealType)rect[1] * h);
			x1 = (IntType)std::ceil((RealType)(rect[0] + rect[2]) * w);
			y1 = (IntType)std::ceil((RealType)(rect[1] + rect[3]) * h);
			x0 = std::max((IntType)0, std::min(x0, w - 1));
			y0 = std::max((IntType)0, std::min(y0, h - 1));
			x1 = std::max(x0 + 1, std::min(x1, w));
			y1 = std::max(y0 + 1, std::min(y1, h));
		}

		// Region exacta en 16 bits por coordenada (texturas <= 65535 px;
		// el maximo real de D3D11 es 16384). Cero colisiones posibles.
		quint64 TexRegionKey(IntType x0, IntType y0, IntType x1, IntType y1)
		{
			return ((quint64)(quint16)x0 << 48) | ((quint64)(quint16)y0 << 32)
			     | ((quint64)(quint16)x1 << 16) | (quint64)(quint16)y1;
		}

		// Clasificacion de alpha por REGION de la textura fuente.
		// bit 0: algun texel no opaco (< 250) -> candidato a ALPHA_TEST
		// bit 1: hay translucidez INTENCIONAL -> ALPHA_BLEND exclusivo
		//        (personajes/modelos/items). Dos criterios, en OR:
		//        (1) los texels de alpha INTERMEDIO [8..247] superan el 0.5
		//            por ciento de los texels no-cero (alpha >= 8): manchas
		//            translucidas grandes;
		//        (2) EXISTE al menos un texel intermedio INTERIOR, es decir
		//            con sus 4 vecinos (dentro de la region) no transparentes
		//            (alpha >= 8). Ver la nota "detalle pequeno" mas abajo.
		// bit 2: ALGUN texel intermedio [8..247] (la semantica previa al
		//        ratio, sin umbral) -> la conservan las secciones de BLOQUE,
		//        que mantienen el comportamiento TEST|BLEND historico
		// Distincion (fix "goggles translucidos de skin"):
		// (a) region con SOLO texels ~0/~255 (< 8 o > 247) = cutout puro
		//     -> ALPHA_TEST como siempre;
		// (b) region con alpha intermedio real -> ALPHA_BLEND: el path
		//     thin-surface del kernel maneja la transmision tintada POR
		//     TEXEL (goggles al 30 por ciento salen como cristal tintado,
		//     los texels opacos siguen opacos, y los texels con alpha ~0 se
		//     blanquean en el atlas -- WhitenTransparentTexels -- para que
		//     transmitan del todo en vez de tintar con su RGB fuente, que
		//     suele ser negro). El umbral del 0.5 por ciento sobre texels
		//     no-cero evita que unos pocos texels de borde antialiasado
		//     conviertan cutouts legitimos en blend.
		// DETALLE PEQUENO (fix de los cristales de unas gafas en la capa 2 de
		// una skin): el ratio SOLO no basta. Una skin 64x64 con 8 texels de
		// lente a alpha 114 da 8/1919 = 0.417 por ciento < 0.5 -> caia a
		// ALPHA_TEST y el cutout 0.5 del kernel BORRABA justo esos 8 texels
		// (114/255 = 0.447 < 0.5): las lentes desaparecian y se veia la cara
		// de debajo. El criterio geometrico distingue lo que el ratio no
		// puede: un texel de BORDE ANTIALIASADO siempre toca un texel
		// transparente (alpha < 8) -- es la frontera del cutout --, mientras
		// que un texel translucido INTENCIONAL esta rodeado de material
		// (opaco o translucido). Basta con que exista UNO para que la region
		// se trate como blend; los texels opacos de la region siguen opacos
		// (aEff >= 0.999 -> difuso estandar) y los de alpha ~0 siguen siendo
		// texels fantasma en la traversal, asi que la region no pierde nada.
		uint32_t ClassifyAlphaRegion(IntType texId, const float rect[4])
		{
			QImage img = scene.resolvedImages.value(texId);
			if (img.isNull())
				return 0;

			IntType x0, y0, x1, y1;
			TexRegionBounds(img, rect, x0, y0, x1, y1);
			QPair<IntType, quint64> key(texId, TexRegionKey(x0, y0, x1, y1));
			if (scene.alphaClassCache.contains(key))
				return scene.alphaClassCache.value(key);

			// Scan completo (sin early-out: el ratio necesita los conteos);
			// el resultado queda cacheado por region exacta como antes.
			// midInterior: texels intermedios cuyos 4 vecinos DENTRO de la
			// region no son transparentes (alpha >= 8). Los vecinos fuera de
			// la region no cuentan (una pagina de sprites tiene otros sprites
			// al lado: mirarlos daria falsos negativos/positivos).
			uint32_t bits = 0;
			uint64_t nonZero = 0, mid = 0, midInterior = 0;
			for (IntType y = y0; y < y1; y++)
			{
				const uchar* line = img.constScanLine((int)y);
				const uchar* lineUp = (y > y0) ? img.constScanLine((int)(y - 1)) : nullptr;
				const uchar* lineDn = (y + 1 < y1) ? img.constScanLine((int)(y + 1)) : nullptr;
				for (IntType x = x0; x < x1; x++)
				{
					uchar a = line[x * 4 + 3];
					if (a < 250)
						bits |= 1u;
					if (a >= 8)
					{
						nonZero++;
						if (a <= 247)
						{
							mid++;
							BoolType interior = true;
							if (x > x0 && line[(x - 1) * 4 + 3] < 8)
								interior = false;
							if (interior && x + 1 < x1 && line[(x + 1) * 4 + 3] < 8)
								interior = false;
							if (interior && lineUp && lineUp[x * 4 + 3] < 8)
								interior = false;
							if (interior && lineDn && lineDn[x * 4 + 3] < 8)
								interior = false;
							if (interior)
								midInterior++;
						}
					}
				}
			}
			// mid / nonZero > 0.005  <=>  mid * 200 > nonZero (entero exacto)
			if (mid * 200 > nonZero || midInterior > 0)
				bits |= 2u;
			if (mid > 0)
				bits |= 4u;

			// SONDA de alfa: que translucidez entra por region y que decide
			// la clasificacion. Es la linea que localiza en UNA corrida los
			// bugs de "mi textura semitransparente no se ve" (bits sin el 2 =
			// cutout 0.5 = los texels intermedios se BORRAN). Solo regiones
			// con algun texel no opaco y como mucho 24 lineas por build; se
			// emite en el cache-miss, coste despreciable.
			if (bits != 0 && cinemaAlphaProbeLines < 24)
			{
				cinemaAlphaProbeLines++;
				DEBUG("Cinema alpha probe: tex=" + NumStr((IntType)texId)
					+ " region=" + NumStr((IntType)x0) + "," + NumStr((IntType)y0)
					+ ".." + NumStr((IntType)x1) + "," + NumStr((IntType)y1)
					+ " nonZero=" + NumStr((IntType)nonZero)
					+ " mid=" + NumStr((IntType)mid)
					+ " midInt=" + NumStr((IntType)midInterior)
					+ " ratio_ppm=" + NumStr((IntType)(nonZero ? (mid * 1000000ull / nonZero) : 0ull))
					+ " bits=" + NumStr((IntType)bits));
			}

			scene.alphaClassCache.insert(key, bits);
			return bits;
		}

		// Complemento OBLIGATORIO de la clasificacion ALPHA_BLEND por scan:
		// el path cristal del kernel (y TraceShadow) ponderan la transmision
		// por texel.rgb * (1 - aEff). En texels con alpha ~0 el RGB fuente
		// suele ser NEGRO (los editores de skins exportan 0,0,0,0), asi que
		// sin retoque las zonas 100 por ciento transparentes de una region
		// ALPHA_BLEND transmitirian negro (cristal opaco negro y sombra
		// total) en vez de "transmitir del todo". Fix host-side (el kernel
		// no se toca): blanquear el RGB de los texels con alpha < 8 SOLO
		// dentro de la region blend, sobre el atlas ya blitteado; el
		// glassTint queda blanco * (1 - 0) = transmision perfecta y la
		// sombra deja de ocluir. Los texels con alpha >= 8 conservan su RGB
		// (goggles = cristal tintado). Escribir en el QImage del slice es
		// seguro: copy-on-write de Qt separa el buffer si lo comparte una
		// TexturePage del motor (la pagina original queda intacta), y el
		// upload GPU (paso 5) ocurre despues de TODOS los BuildMaterial.
		// Nota: las regiones cutout (ALPHA_TEST) no se tocan -- alli el RGB
		// bajo alpha 0 nunca se muestrea (texel fantasma). meanRgbCache lee
		// la imagen FUENTE (resolvedImages), no el atlas: la CDF emisiva no
		// cambia. Deduplicado por region exacta (whitenedRegions);
		// idempotente si dos regiones se solapan.
		void WhitenTransparentTexels(IntType texId, const float rect[4])
		{
			QImage img = scene.resolvedImages.value(texId);
			if (img.isNull())
				return;

			AtlasEntry entry = scene.texEntries.value(texId);
			if (!entry.valid || entry.sx <= 0.f || entry.sy <= 0.f)
				return;
			if ((IntType)entry.slice >= (IntType)scene.slices.size())
				return;

			IntType x0, y0, x1, y1;
			TexRegionBounds(img, rect, x0, y0, x1, y1);
			QPair<IntType, quint64> key(texId, TexRegionKey(x0, y0, x1, y1));
			if (scene.whitenedRegions.contains(key))
				return;
			scene.whitenedRegions.insert(key);

			// Pixel (0,0) de la imagen resuelta dentro del slice: ox/oy se
			// computaron como x/ATLAS_SIZE exacto, el redondeo lo invierte
			IntType ax = (IntType)std::lround((RealType)entry.ox * CINEMA_ATLAS_SIZE);
			IntType ay = (IntType)std::lround((RealType)entry.oy * CINEMA_ATLAS_SIZE);

			QImage& slice = scene.slices[(int)entry.slice];
			for (IntType y = y0; y < y1; y++)
			{
				IntType sy = ay + y;
				if (sy < 0 || sy >= CINEMA_ATLAS_SIZE)
					continue;
				uchar* line = slice.scanLine((int)sy);
				for (IntType x = x0; x < x1; x++)
				{
					IntType sx = ax + x;
					if (sx < 0 || sx >= CINEMA_ATLAS_SIZE)
						continue;
					uchar* px = line + sx * 4;
					if (px[3] < 8)
					{
						px[0] = 255;
						px[1] = 255;
						px[2] = 255;
					}
				}
			}
		}

		// Albedo medio LINEAL aprox de un draw para la potencia de la CDF
		// emisiva: media de los bytes sRGB de la region de albedo fuente
		// (linealizada TRAS promediar: aproximacion documentada, evita un pow
		// por texel) x uBlendColor lineal del material. Aproximaciones
		// asumidas (solo pesan en la DISTRIBUCION del muestreo NEE, nunca en
		// la energia de la imagen, que la pone EvalSurface por texel): media
		// sin ponderar por alpha del texel y sin el color de vertice
		// (normalmente blanco). Textura irresoluble (parche blanco): solo el
		// blend. Cacheada por region exacta (misma clave que el alpha cache).
		void MeanAlbedoLinear(const PTDraw& draw, const PTMaterial& mat, float out[3])
		{
			out[0] = mat.blendR;
			out[1] = mat.blendG;
			out[2] = mat.blendB;

			QImage img = scene.resolvedImages.value(draw.texId[0]);
			if (img.isNull())
				return;

			IntType x0, y0, x1, y1;
			TexRegionBounds(img, draw.uvRect[0], x0, y0, x1, y1);
			QPair<IntType, quint64> key(draw.texId[0], TexRegionKey(x0, y0, x1, y1));

			uint32_t packed;
			if (scene.meanRgbCache.contains(key))
				packed = scene.meanRgbCache.value(key);
			else
			{
				uint64_t sumR = 0, sumG = 0, sumB = 0;
				for (IntType y = y0; y < y1; y++)
				{
					const uchar* line = img.constScanLine((int)y);
					for (IntType x = x0; x < x1; x++)
					{
						sumR += line[x * 4 + 0];
						sumG += line[x * 4 + 1];
						sumB += line[x * 4 + 2];
					}
				}
				uint64_t count = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0); // > 0: region no vacia
				packed = ((uint32_t)(sumR / count) << 16)
				       | ((uint32_t)(sumG / count) << 8)
				       | (uint32_t)(sumB / count);
				scene.meanRgbCache.insert(key, packed);
			}

			out[0] *= SrgbToLinear((float)((packed >> 16) & 255u) / 255.f);
			out[1] *= SrgbToLinear((float)((packed >> 8) & 255u) / 255.f);
			out[2] *= SrgbToLinear((float)(packed & 255u) / 255.f);
		}
	#endif // API_D3D11

		// -----------------------------------------------------------------
		// Materiales
		// -----------------------------------------------------------------

		// Componer el remap capturado (uv local -> region de textura) con el
		// remap de atlas (textura -> slice): out = atlas(captura(uv)).
		void ComposeUvRect(const AtlasEntry& e, const float cap[4], float out[4])
		{
			out[0] = e.ox + cap[0] * e.sx;
			out[1] = e.oy + cap[1] * e.sy;
			out[2] = cap[2] * e.sx;
			out[3] = cap[3] * e.sy;
		}

	#if API_D3D11
		// TRIM: cota ESTATICA de potencia de un material con efecto, para la
		// CDF de emisores. max(gain) sale del catalogo (lo mide el generador
		// sobre su rejilla determinista, 240 muestras) y viaja en
		// CinemaTrimGen. nocturne de dia: intensity puede ser 0 -> cota 0
		// EXACTA -> esos tris ni entran a la lista (cero rayos gastados en un
		// emisor apagado).
		float TrimPowerBound(const PTMaterial& mat)
		{
			uint32_t fx = mat.trimPack & 255u;
			if (fx == 0u)
				return 0.f;
			const CinemaTrimGen::Effect* table = CinemaTrimGen::Table();
			for (unsigned int i = 0; i < CinemaTrimGen::COUNT; i++)
			{
				if (table[i].slot != fx)
					continue;
				float bound = table[i].gainMax * table[i].cinemaGain
					* mat.trimOrigin[3] * scene.trim.emissive;
				return bound > 0.f ? bound : 0.f;
			}
			return 0.f;
		}

		PTMaterial BuildMaterial(const PTDraw& draw)
		{
			PTMaterial mat = {};
			uint32_t flags = 0;

			// Albedo (siempre; texId invalido -> parche blanco)
			AtlasEntry albedo = AtlasAddTexture(draw.texId[0]);
			const AtlasEntry& useAlbedo = albedo.valid ? albedo : scene.whiteEntry;
			ComposeUvRect(useAlbedo, draw.uvRect[0], mat.uvRect);
			mat.sliceAlbedo = useAlbedo.slice;

			if (albedo.valid)
			{
				uint32_t alphaBits = ClassifyAlphaRegion(draw.texId[0], draw.uvRect[0]);
				BoolType cloudSection = draw.sectionTag == CINEMA_SECTION_CLOUDS;
				BoolType blockSection =
					draw.sectionTag == CINEMA_SECTION_BLOCK ||
					draw.sectionTag == CINEMA_SECTION_SCENERY ||
					draw.sectionTag == CINEMA_SECTION_GROUND;
				if (cloudSection)
				{
					// Nubes: thin-translucent tipo HOJA (PT_KIND_LEAF via
					// MATF_SUBSURFACE en el switch de abajo). Mantener SOLO
					// ALPHA_TEST para los huecos cutout; NUNCA ALPHA_BLEND, que
					// las clasificaria como cristal -> panza inferior facetada y
					// oscura con sombra dura. El color de vertice (coltop/bottom/
					// sides) ya multiplica el albedo, sin politica de AO.
					if (alphaBits & 1u)
						flags |= CinemaKernelM0::MATF_ALPHA_TEST;
				}
				else if (blockSection)
				{
					// Bloques/scenery/ground: comportamiento historico
					// INTACTO (TEST y BLEND pueden coexistir, bit 2 = la
					// semantica pre-ratio "algun texel intermedio"). Los
					// sheets de bloques mezclan cutout (hojas, flores) y
					// cristal tintado (stained glass, alpha >= 0.5) en la
					// misma region: el cutout 0.5 del kernel + glass para
					// los texels que sobreviven ya era el comportamiento
					// correcto y los goldens de bloques dependen de el.
					if (alphaBits & 1u)
						flags |= CinemaKernelM0::MATF_ALPHA_TEST;
					if (alphaBits & 4u)
						flags |= CinemaKernelM0::MATF_ALPHA_BLEND;
				}
				else if (alphaBits & 2u)
				{
					// Personajes/modelos/items, EXCLUYENTE: si la region
					// tiene alpha intermedio real (bit 1, ratio > 0.5 por
					// ciento) va a ALPHA_BLEND y NO a ALPHA_TEST -- el
					// cutout 0.5 borraria justo esos texels translucidos
					// (goggles). Los texels alpha ~0 de la region se
					// blanquean en el atlas: sin eso serian cristal NEGRO
					// (su RGB fuente suele ser 0,0,0) en vez de transmitir.
					flags |= CinemaKernelM0::MATF_ALPHA_BLEND;
					// COBERTURA: si el draw es CAPA DE OBJETO (pt_object_layer:
					// partes de personaje/modelo, items, texto, primitivas), su
					// alpha es COBERTURA, no transmision -> MATF_COVERAGE y el
					// kernel usa PT_KIND_COVER (difuso con peso aEff + pasar de
					// largo con peso 1-aEff). La geometria de block sheet
					// (cristal de bloque, hielo) NO lleva el bit y conserva el
					// cristal TENIDO; el agua va por MATF_WATER, que el kernel
					// evalua ANTES que la cobertura.
					if (draw.objectLayer && !draw.isWater)
						flags |= CinemaKernelM0::MATF_COVERAGE;
					WhitenTransparentTexels(draw.texId[0], draw.uvRect[0]);
				}
				else if (alphaBits & 1u)
					flags |= CinemaKernelM0::MATF_ALPHA_TEST;
			}

			// Material / normal map solo si el raster los usaria (mismos
			// gates que el shader high: uMaterialFormat / uUseNormalMap)
			if (draw.materialFormat != 0 && draw.texId[1] > 0)
			{
				AtlasEntry matTex = AtlasAddTexture(draw.texId[1]);
				if (matTex.valid)
				{
					flags |= CinemaKernelM0::MATF_HAS_MATTEX;
					// M2: formato del decode (contrato MATF_MAT_LABPBR de
					// CinemaKernelM0.hpp). Bit puesto = LabPBR (uMaterialFormat
					// 2: rough = (1-R)^2, metal = G > 0.898); limpio = SEUS
					// (formato 1: rough = 1-R, metal = G). Sin este bit TODO
					// material map decodificaria como SEUS.
					if (draw.materialFormat == 2)
						flags |= CinemaKernelM0::MATF_MAT_LABPBR;
					ComposeUvRect(matTex, draw.uvRect[1], mat.uvRectMat);
					mat.sliceMat = matTex.slice;
				}
			}
			if (draw.useNormalMap != 0 && draw.texId[2] > 0)
			{
				AtlasEntry nrmTex = AtlasAddTexture(draw.texId[2]);
				if (nrmTex.valid)
				{
					flags |= CinemaKernelM0::MATF_HAS_NRMTEX;
					ComposeUvRect(nrmTex, draw.uvRect[2], mat.uvRectNrm);
					mat.sliceNrm = nrmTex.slice;
				}
			}

			if (draw.isWater)
				flags |= CinemaKernelM0::MATF_WATER | CinemaKernelM0::MATF_ALPHA_BLEND;
			if (draw.twoSided)
				flags |= CinemaKernelM0::MATF_TWO_SIDED;
			if (draw.sss > 0.f)
				flags |= CinemaKernelM0::MATF_SUBSURFACE;
			if (draw.alphaHash && draw.blend[3] < 0.999f)
				flags |= CinemaKernelM0::MATF_ALPHA_HASH;
			// F4: paridad con el raster. El sampler del raster envuelve la UV
			// LOCAL a [0,1) antes de componerla con el uvRect del sprite
			// siempre que el sampler tenga repeat, y render_set_texture lo
			// pone SIEMPRE (render_set_texture.gml:68). Sin esto una UV > 1 lee
			// la pagina de textura POR FUERA del sprite: sector negro (el
			// relleno transparente del packer) + color del sprite vecino.
			// Afecta a las primitivas curvas (tex1[X] += 0.25 -> U hasta 1.25),
			// a shape_tex_hrepeat/vrepeat y a los path mas largos que su
			// tex_length.
			if (draw.isGround || draw.texRepeat[0])
				flags |= CinemaKernelM0::MATF_WRAP_UV;

			// Backface culling capturado del raster (draw.cullMode). El kernel
			// descarta en la traversal el hit cuyo lado (respecto al rayo,
			// via normal geometrica del winding ALMACENADO) el raster cull-earia.
			// NONE (0, objetos two-sided/backfaces) => sin flag => nunca se culla.
			if (draw.cullMode == 2)
				flags |= CinemaKernelM0::MATF_CULL_BACK;
			else if (draw.cullMode == 1)
				flags |= CinemaKernelM0::MATF_CULL_FRONT;

			// F1: precedencia MATTE > INVISIBLE. Si por lo que sea llegan los dos
			// bits puestos, gana MATTE (el comportamiento mas conservador: el
			// objeto sigue sin verse y ademas tapa).
			if (draw.camMatte)
				flags |= CinemaKernelM0::MATF_CAM_MATTE;
			else if (draw.camInvisible)
				flags |= CinemaKernelM0::MATF_CAM_INVISIBLE;

			// F1 4a: SHADOW CATCHER. Precedencia: los dos holdouts MANDAN
			// (matte y invisible se resuelven en la traversal, antes de que el
			// integrador llegue a sombrear nada), asi que un objeto con matte y
			// catcher a la vez sigue siendo un agujero. Un catcher a secas
			// escribe el bit y su opacidad.
			if (draw.shadowCatcher && !draw.camMatte && !draw.camInvisible)
				flags |= CinemaKernelM0::MATF_SHADOW_CATCHER;

			switch (draw.sectionTag)
			{
				case CINEMA_SECTION_BLOCK:
					flags |= CinemaKernelM0::MATF_IS_BLOCK;
					break;
				case CINEMA_SECTION_SCENERY:
					flags |= CinemaKernelM0::MATF_SCENERY | CinemaKernelM0::MATF_IS_BLOCK;
					break;
				case CINEMA_SECTION_GROUND:
					flags |= CinemaKernelM0::MATF_WRAP_UV | CinemaKernelM0::MATF_IS_BLOCK;
					break;
				case CINEMA_SECTION_CLOUDS:
					// thin-translucent (0.7/0.3): el kernel lo clasifica
					// PT_KIND_LEAF por el bit SUBSURFACE -> panza suave, no
					// cristal. NO se marca IS_BLOCK (no es geometria de mundo).
					// R4: MATF_NO_SHADOW -> las nubes NO ocluyen rayos de sombra
					// (TraceShadow las ignora). El plano de nubes captado como
					// geometria recortaba el sol sobre grandes zonas -> escena
					// mas oscura. Minecraft vanilla y el raster tampoco sombrean
					// por nubes. Los rayos primarios y de GI SIGUEN viendolas.
					flags |= CinemaKernelM0::MATF_SUBSURFACE | CinemaKernelM0::MATF_NO_SHADOW;
					break;
				default:
					break;
			}

			// uBlendColor linealizado; el alpha de objeto NO se linealiza
			mat.blendR = SrgbToLinear(Clamp01(draw.blend[0]));
			mat.blendG = SrgbToLinear(Clamp01(draw.blend[1]));
			mat.blendB = SrgbToLinear(Clamp01(draw.blend[2]));
			mat.blendA = Clamp01(draw.blend[3]);
			mat.emissive = Clamp01(draw.emissive);
			mat.metallic = Clamp01(draw.metallic);
			mat.roughness = Clamp01(draw.roughness);
			// TRIM (ShinyTrims). El bit y el pack SOLO se ponen si hay efecto
			// Y mascara resoluble: sin mascara la cobertura seria 0 en todo
			// texel y el kernel pagaria el dispatch para no dibujar nada. Los
			// dos contadores del build son lo que el acta rinde -- una pieza
			// con fx > 0 y sin mascara es un fallo SILENCIOSO de otro modo.
			mat.trimPack = 0;
			mat.trimOrigin[0] = mat.trimOrigin[1] = mat.trimOrigin[2] = 0.f;
			mat.trimOrigin[3] = 0.f;
			mat.uvRectTrim[0] = 0.f; mat.uvRectTrim[1] = 0.f;
			mat.uvRectTrim[2] = 1.f; mat.uvRectTrim[3] = 1.f;
			if (draw.trimFx > 0)
			{
				scene.trim.drawsWithTrim++;
				AtlasEntry trimTex = (draw.trimMaskTexId > 0)
					? AtlasAddTexture(draw.trimMaskTexId) : AtlasEntry();
				if (trimTex.valid)
				{
					ComposeUvRect(trimTex, draw.trimMaskUvRect, mat.uvRectTrim);
					mat.trimPack = ((uint32_t)draw.trimFx & 255u)
						| ((trimTex.slice & 255u) << 8u)
						| (draw.trimOutline ? 65536u : 0u)
						| ((flags & CinemaKernelM0::MATF_WRAP_UV) ? 131072u : 0u);
					mat.trimOrigin[0] = draw.trimOrigin[0];
					mat.trimOrigin[1] = draw.trimOrigin[1];
					mat.trimOrigin[2] = draw.trimOrigin[2];
					mat.trimOrigin[3] = draw.trimIntensity;
					flags |= CinemaKernelM0::MATF_HAS_TRIM;
				}
				else
					scene.trim.drawsTrimNoMask++;
			}

			mat.flags = flags;
			// F1 4a: se escribe SIEMPRE (1 en todo lo que no sea catcher) para
			// que el buffer de materiales no tenga ni un byte indeterminado.
			mat.shadowOpacity = Clamp01(draw.shadowOpacity);
			return mat;
		}
	#endif // API_D3D11

		// -----------------------------------------------------------------
		// R18: recorte por DISTANCIA (ver CinemaScene.hpp).
		// -----------------------------------------------------------------

		struct ClipCtx
		{
			float cx = 0.f, cy = 0.f, cz = 0.f; // ojo
			float r2 = 0.f;                     // radio^2
		};

		// Posicion de MUNDO de un vertice fuente (mismo almacenamiento
		// column-major que FlattenDraw: p' = m0*x + m4*y + m8*z + m12).
		inline void ClipWorldPos(const RealType* m, const Vertex& sv, float out[3])
		{
			RealType x = sv.x, y = sv.y, z = sv.z;
			out[0] = (float)(m[0] * x + m[4] * y + m[8] * z + m[12]);
			out[1] = (float)(m[1] * x + m[5] * y + m[9] * z + m[13]);
			out[2] = (float)(m[2] * x + m[6] * y + m[10] * z + m[14]);
		}

		// true si el triangulo entra: distancia^2 del ojo a su AABB <= r2. El
		// test por AABB (y no por centroide) conserva los triangulos GRANDES
		// que cruzan el radio aunque sus 3 vertices queden fuera.
		inline BoolType ClipTriKeep(const ClipCtx& c,
			const float p0[3], const float p1[3], const float p2[3])
		{
			float d2 = 0.f;
			for (int a = 0; a < 3; a++)
			{
				float mn = p0[a] < p1[a] ? p0[a] : p1[a];
				if (p2[a] < mn) mn = p2[a];
				float mx = p0[a] > p1[a] ? p0[a] : p1[a];
				if (p2[a] > mx) mx = p2[a];
				float e = (a == 0) ? c.cx : (a == 1 ? c.cy : c.cz);
				float d = (e < mn) ? (mn - e) : ((e > mx) ? (e - mx) : 0.f);
				d2 += d * d;
			}
			return d2 <= c.r2;
		}

		// Secciones EXENTAS del recorte: el suelo infinito y las nubes son un
		// punado de triangulos gigantes que envuelven la escena entera; su coste
		// es nulo y quitarlos cambiaria la imagen radicalmente.
		inline BoolType ClipExemptSection(IntType sectionTag)
		{
			return sectionTag == CINEMA_SECTION_GROUND
				|| sectionTag == CINEMA_SECTION_CLOUDS;
		}

		// Cuenta (sin escribir nada) los verts/tris de un draw que SOBREVIVEN al
		// recorte. Comparte predicado (ClipTriKeep) y recorrido con FlattenDraw:
		// los conteos casan exactamente con lo que aquel escribe.
		void CountDrawClipped(const PTDraw& draw, VertexBuffer* buffer,
			const ClipCtx& clip, IntType& outVerts, IntType& outTris)
		{
			const RealType* m = draw.world.m;
			outVerts = 0;
			outTris = 0;

			for (Mesh<>* mesh : buffer->meshes)
			{
				if (!mesh || !mesh->numVertices || !mesh->numIndices)
					continue;
				if (mesh->vertexData.Size() < mesh->numVertices ||
					mesh->indexData.Size() < mesh->numIndices)
					continue; // MISMO predicado que el plan y que FlattenDraw

				const Vertex* src = mesh->vertexData.Data();
				const uint32_t* srcIdx = mesh->indexData.Data();
				IntType nv = mesh->numVertices;
				std::vector<uint8_t> used((size_t)nv, 0);
				IntType meshTris = mesh->numIndices / 3;
				IntType kept = 0;

				for (IntType i = 0; i < meshTris; i++)
				{
					uint32_t a = srcIdx[i * 3 + 0];
					uint32_t b = srcIdx[i * 3 + 1];
					uint32_t c = srcIdx[i * 3 + 2];
					if ((IntType)a >= nv || (IntType)b >= nv || (IntType)c >= nv)
						continue; // indice corrupto: fuera (nunca OOB)
					float p0[3], p1[3], p2[3];
					ClipWorldPos(m, src[a], p0);
					ClipWorldPos(m, src[b], p1);
					ClipWorldPos(m, src[c], p2);
					if (!ClipTriKeep(clip, p0, p1, p2))
						continue;
					used[a] = 1;
					used[b] = 1;
					used[c] = 1;
					kept++;
				}

				IntType uv = 0;
				for (IntType v = 0; v < nv; v++)
					uv += used[(size_t)v];
				outVerts += uv;
				outTris += kept;
			}
		}

		// -----------------------------------------------------------------
		// Flatten de un draw: vertices a mundo + PTTri con matId. Escrituras
		// a rangos disjuntos precomputados: seguro bajo OpenMP.
		// -----------------------------------------------------------------

		// M8-A: outVertsLocal/outTrisLocal (opcionales, no-null con el toggle ON)
		// reciben la MISMA malla en espacio LOCAL (pos/normal/tangente crudos del
		// vertice fuente) y con el winding FUENTE (SIN el swap de espejo): el
		// espejo se muda a la matriz de la instancia y el kernel lo compensa por
		// det<0. Los indices son paralelos a los del array mundo (mismo orden de
		// vertice), asi que 'outRange' vale para AMBOS.
		void FlattenDraw(const PTDraw& draw, VertexBuffer* buffer,
			IntType drawIdx, IntType vertStart, IntType triStart, uint32_t matId,
			float vertexAo, PTVert* outVerts, PTTri* outTris, PTDrawRange& outRange,
			PTVert* outVertsLocal = nullptr, PTTri* outTrisLocal = nullptr,
			const ClipCtx* clip = nullptr)
		{
			const RealType* m = draw.world.m;

			// --- COFACTOR de la 3x3 de la world (adjunta transpuesta = det*M^-T).
			// Es la matriz con la que se transforma una NORMAL; la 3x3 DIRECTA solo
			// coincide para rotacion + escala UNIFORME.
			// Por que importa aqui y no en cualquier motor: las normales del motor
			// llegan CUANTIZADAS a 8 bits por eje (Vertex.cpp: byte=(n+1)/2*255), asi
			// que una normal de cara (0,0,1) esta guardada como (-0.0039,-0.0039,1).
			// Con la 3x3 directa ese residuo se MULTIPLICA por los ejes grandes
			// mientras la componente buena se multiplica por el pequeno: en la losa
			// del suelo de una escena real, escala (66.1, 99.9, 0.35), (0,0,1) acaba
			// siendo una normal inclinada ~53 grados y el suelo se sombrea como una
			// rampa. El cofactor DIVIDE por las escalas (cof(diag(a,b,c)) =
			// diag(bc,ac,ab)) y el residuo se vuelve despreciable.
			// ESPEJO (det<0): cof(M) = det(M)*M^-T, o sea la normal con el signo
			// INVERTIDO respecto a M^-T. Es inofensivo A PROPOSITO: el kernel y el
			// oraculo voltean SIEMPRE la normal de sombreado hacia el rayo, asi que
			// ns y -ns dan el mismo resultado (t42_normales_aniso.json fija ese caso
			// con SCA_X negativa). Se deja el cofactor a pelo -- sin el signo del
			// det -- para que host y shader hagan la MISMA operacion exacta.
			// Almacenamiento column-major del motor: columna j = (m[4j], m[4j+1], m[4j+2]).
			const RealType c0[3] = { m[0], m[1], m[2] };
			const RealType c1[3] = { m[4], m[5], m[6] };
			const RealType c2[3] = { m[8], m[9], m[10] };
			auto cross3 = [](const RealType a[3], const RealType b[3], RealType out[3])
			{
				out[0] = a[1] * b[2] - a[2] * b[1];
				out[1] = a[2] * b[0] - a[0] * b[2];
				out[2] = a[0] * b[1] - a[1] * b[0];
			};
			RealType cof0[3], cof1[3], cof2[3]; // columnas del cofactor
			cross3(c1, c2, cof0);
			cross3(c2, c0, cof1);
			cross3(c0, c1, cof2);

			// R18: indice "vertice descartado" del remap del recorte.
			const uint32_t CLIP_DROP = 0xFFFFFFFFu;

			// Politica de AO de vertice (seccion 7.1): en scenery/bloques el
			// RGB del vertice ES el AO vanilla horneado; el GI real lo
			// sustituye. aoMix 0 = ignorar horneado (default del diseno).
			BoolType applyAoPolicy =
				draw.sectionTag == CINEMA_SECTION_BLOCK ||
				draw.sectionTag == CINEMA_SECTION_SCENERY ||
				draw.sectionTag == CINEMA_SECTION_GROUND;
			float aoMix = Clamp01(vertexAo);

			// uDefaultEmissive se pliega en el byte emissive del vertice
			// (formula del shader high: e = max(uEmissive, byte/255 *
			// uDefaultEmissive)); el kernel ya no necesita el uniform.
			float defEmissive = draw.defaultEmissive < 0.f ? 0.f : draw.defaultEmissive;

			// R6: correccion de winding por MATRIZ ESPEJADA (det de la 3x3 de la
			// world < 0, p.ej. escala negativa). Una world espejada invierte el
			// winding de los triangulos al transformar los vertices, y con el la
			// normal geometrica (cross de aristas) que usa el backface culling ->
			// las caras cercanas se cullarian y el objeto saldria hueco/negro
			// (verificado: bloque con SCA_X<0 sale negro sin este swap, mientras
			// el raster lo muestra solido). Fix estandar: intercambiar i1<->i2 de
			// cada triangulo del draw, restaurando el winding consistente sin
			// tocar los vertices. Es AUTO-CONSISTENTE con el fetch de atributos
			// (bu,bv siguen a i1,i2). Se hace POR DRAW (no por triangulo): NO
			// toca la inversion INTENCIONAL de los modelos "outline" (shell con
			// caras invertidas HORNEADAS en la malla, sobre un objeto de det>0),
			// que el culling replicado preserva. Column-major: columnas en
			// m[0..2], m[4..6], m[8..10].
			BoolType mirrored =
				(m[0] * (m[5] * m[10] - m[9] * m[6])
			   - m[4] * (m[1] * m[10] - m[9] * m[2])
			   + m[8] * (m[1] * m[6] - m[5] * m[2])) < 0.0;

			IntType vo = vertStart;
			IntType to = triStart;

			for (Mesh<>* mesh : buffer->meshes)
			{
				if (!mesh || !mesh->numVertices || !mesh->numIndices)
					continue;
				if (mesh->vertexData.Size() < mesh->numVertices ||
					mesh->indexData.Size() < mesh->numIndices)
					continue; // validado antes (MISMO predicado que el plan:
					          // si divergieran, los offsets se desincronizan)

				const Vertex* src = mesh->vertexData.Data();
				const uint32_t* srcIdx = mesh->indexData.Data();
				IntType meshVertBase = vo;
				IntType meshNumTris = mesh->numIndices / 3;

				// R18: recorte por distancia. Primero se clasifican los
				// TRIANGULOS (AABB vs esfera de la camara); luego los vertices
				// que ningun triangulo superviviente usa se descartan y los
				// indices se remapean a la numeracion compactada. Con clip=null
				// no se toca nada: el camino de siempre, bit a bit.
				std::vector<uint32_t> clipRemap;
				std::vector<uint8_t> clipKeep;
				if (clip)
				{
					clipRemap.assign((size_t)mesh->numVertices, CLIP_DROP);
					clipKeep.assign((size_t)meshNumTris, 0);
					for (IntType i = 0; i < meshNumTris; i++)
					{
						uint32_t a = srcIdx[i * 3 + 0];
						uint32_t b = srcIdx[i * 3 + 1];
						uint32_t c = srcIdx[i * 3 + 2];
						if ((IntType)a >= mesh->numVertices ||
							(IntType)b >= mesh->numVertices ||
							(IntType)c >= mesh->numVertices)
							continue;
						float p0[3], p1[3], p2[3];
						ClipWorldPos(m, src[a], p0);
						ClipWorldPos(m, src[b], p1);
						ClipWorldPos(m, src[c], p2);
						if (!ClipTriKeep(*clip, p0, p1, p2))
							continue;
						clipKeep[(size_t)i] = 1;
						clipRemap[a] = 0u;
						clipRemap[b] = 0u;
						clipRemap[c] = 0u;
					}
					// Numeracion compactada EN ORDEN DE VERTICE (igual que el
					// conteo de CountDrawClipped).
					uint32_t next = 0;
					for (IntType v = 0; v < mesh->numVertices; v++)
						if (clipRemap[(size_t)v] != CLIP_DROP)
							clipRemap[(size_t)v] = next++;
				}

				for (IntType v = 0; v < mesh->numVertices; v++)
				{
					if (clip && clipRemap[(size_t)v] == CLIP_DROP)
						continue;
					const Vertex& sv = src[v];
					PTVert pv;

					// Posicion a mundo (almacenamiento column-major del motor:
					// p' = m0*x + m4*y + m8*z + m12, igual que Matrix*VecType)
					RealType x = sv.x, y = sv.y, z = sv.z;
					pv.px = (float)(m[0] * x + m[4] * y + m[8] * z + m[12]);
					pv.py = (float)(m[1] * x + m[5] * y + m[9] * z + m[13]);
					pv.pz = (float)(m[2] * x + m[6] * y + m[10] * z + m[14]);

					// Normal: COFACTOR de la 3x3 de la world + renormalizar (ver la
					// nota del calculo del cofactor arriba). La tangente SI va con la
					// 3x3 directa: es una direccion DENTRO de la superficie, no una
					// normal, y esa es su transformacion correcta.
					float n[3], nw[3];
					DecodePacked888(sv.normal, n);
					nw[0] = (float)(cof0[0] * n[0] + cof1[0] * n[1] + cof2[0] * n[2]);
					nw[1] = (float)(cof0[1] * n[0] + cof1[1] * n[1] + cof2[1] * n[2]);
					nw[2] = (float)(cof0[2] * n[0] + cof1[2] * n[1] + cof2[2] * n[2]);
					float len = std::sqrt(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
					if (len > 1.0e-12f)
					{
						nw[0] /= len;
						nw[1] /= len;
						nw[2] /= len;
						pv.normal888 = EncodePacked888(nw);
					}
					else
						pv.normal888 = sv.normal;

					// Tangente: misma transformacion (M2, normal maps)
					float t[3], tw[3];
					DecodePacked888(sv.tangent, t);
					tw[0] = (float)(m[0] * t[0] + m[4] * t[1] + m[8] * t[2]);
					tw[1] = (float)(m[1] * t[0] + m[5] * t[1] + m[9] * t[2]);
					tw[2] = (float)(m[2] * t[0] + m[6] * t[1] + m[10] * t[2]);
					float tlen = std::sqrt(tw[0] * tw[0] + tw[1] * tw[1] + tw[2] * tw[2]);
					if (tlen > 1.0e-12f)
					{
						tw[0] /= tlen;
						tw[1] /= tlen;
						tw[2] /= tlen;
						pv.tangent888 = EncodePacked888(tw);
					}
					else
						pv.tangent888 = sv.tangent;

					// UV crudas: el remap al atlas lo hace el kernel via
					// PTMaterial.uvRect (regla de UVs de la seccion 4.5)
					pv.u = sv.u;
					pv.v = sv.v;

					// Color: politica de AO en el RGB; alpha se conserva.
					// El RGB del vertice trae AO acromatico horneado MULTIPLICADO
					// por el TINTE DE BIOMA (hojas/hierba verdes de los mundos).
					// La formula vieja  c' = 255 + (c-255)*aoMix  POR CANAL
					// arrastraba TODO el color hacia blanco -> arboles GRISES.
					// Fix: desvanecer solo la MAGNITUD acromatica (el AO) y
					// preservar el CROMA (ratio de canales) intacto.
					pv.colorABGR = ApplyVertexAoColor(sv.color, applyAoPolicy, aoMix);

					// dataFlags: bits 0-2 (wave/subsurface) + byte emissive
					// con uDefaultEmissive YA plegado. Los bytes 2-3 (indice
					// de batch en copias) se descartan: siempre leemos los
					// vbuffers FUENTE, jamas batches.
					pv.dataFlags = FoldEmissiveDataFlags(sv.data, defEmissive);

					pv.pad0 = 0;
					pv.pad1 = 0;
					pv.pad2 = 0;
					IntType vslot = vo++;
					outVerts[vslot] = pv;

					// M8-A: vert LOCAL paralelo. Solo pos/normal/tangente cambian;
					// UV/color/emissive/pads son invariantes a la matriz (se copian
					// de pv). Normal/tangente CRUDAS del vertice fuente (mismo
					// empaquetado 888 que EncodePacked888 -> el kernel las decodifica
					// con DecodeNormal888 igual que las de mundo).
					if (outVertsLocal)
					{
						PTVert lv = pv;
						lv.px = (float)sv.x;
						lv.py = (float)sv.y;
						lv.pz = (float)sv.z;
						lv.normal888 = sv.normal;
						lv.tangent888 = sv.tangent;
						outVertsLocal[vslot] = lv;
					}
				}

				IntType meshTris = meshNumTris;
				for (IntType i = 0; i < meshTris; i++)
				{
					if (clip && !clipKeep[(size_t)i])
						continue;
					// R6: los triangulos se emiten con el winding de la fuente,
					// salvo el swap i1<->i2 cuando la world es ESPEJADA (det<0,
					// arriba). NO hay reparacion por-triangulo (ronda 3, revertida)
					// ni comparacion con normales de vertice: eso destruia la
					// inversion INTENCIONAL de los modelos "outline". La visibilidad
					// la da el backface culling replicado del raster (MATF_CULL_*
					// + descarte en la traversal), que usa la normal geometrica del
					// winding ALMACENADO (cross de aristas en el orden ya corregido
					// por espejo). Asi: outline (det>0, inversion horneada) se
					// preserva; espejo por matriz (det<0) se corrige.
					uint32_t r0 = srcIdx[i * 3 + 0];
					uint32_t r1 = srcIdx[i * 3 + 1];
					uint32_t r2 = srcIdx[i * 3 + 2];
					if (clip)
					{
						r0 = clipRemap[(size_t)r0];
						r1 = clipRemap[(size_t)r1];
						r2 = clipRemap[(size_t)r2];
					}
					uint32_t s0 = (uint32_t)(meshVertBase + r0);
					uint32_t s1 = (uint32_t)(meshVertBase + r1);
					uint32_t s2 = (uint32_t)(meshVertBase + r2);
					PTTri tri;
					tri.i0 = s0;
					if (mirrored)
					{
						tri.i1 = s2;
						tri.i2 = s1;
					}
					else
					{
						tri.i1 = s1;
						tri.i2 = s2;
					}
					tri.matId = matId;
					IntType tslot = to++;
					outTris[tslot] = tri;

					// M8-A: tri LOCAL con winding FUENTE (sin swap): el espejo lo
					// aporta la matriz de la instancia, no el winding almacenado.
					if (outTrisLocal)
					{
						PTTri lt;
						lt.i0 = s0;
						lt.i1 = s1;
						lt.i2 = s2;
						lt.matId = matId;
						outTrisLocal[tslot] = lt;
					}
				}
			}

			outRange.drawIdx = drawIdx;
			outRange.vertStart = vertStart;
			outRange.vertCount = vo - vertStart;
			outRange.triStart = triStart;
			outRange.triCount = to - triStart;
		}

		// M8-C: re-flatten LOCAL de UN solo draw (bend en vivo). Reescribe SOLO los
		// verts LOCALES del rango de la instancia (scene.vertsLocal[vertStart..]) con
		// la geometria FUENTE FRESCA (posiciones/normales deformadas por el bend). Los
		// bytes producidos son IDENTICOS a los de FlattenDraw (mismos helpers de
		// AO/emissive) -> el render vivo casa bit a bit con un build frio. NO toca los
		// tris (los indices no cambian si el count es el mismo, precondicion del
		// llamador) ni la matriz de mundo (esa viaja en la instancia TLAS). Devuelve
		// false si el buffer no resuelve o el count no casa (el llamador cae a rebuild).
		BoolType ReflattenDrawLocal(size_t instIdx)
		{
			if (instIdx >= scene.ranges.size())
				return false;
			const PTDrawRange& rg = scene.ranges[instIdx];
			const PTDraw& draw = scene.draws[(size_t)rg.drawIdx];
			VertexBuffer* buffer = FindVertexBuffer(draw.vbufferId);
			if (!buffer)
				return false;

			BoolType applyAoPolicy =
				draw.sectionTag == CINEMA_SECTION_BLOCK ||
				draw.sectionTag == CINEMA_SECTION_SCENERY ||
				draw.sectionTag == CINEMA_SECTION_GROUND;
			float aoMix = Clamp01((float)scene.lastVertexAo);
			float defEmissive = draw.defaultEmissive < 0.f ? 0.f : draw.defaultEmissive;

			IntType vo = rg.vertStart;
			const IntType vEnd = rg.vertStart + rg.vertCount;
			for (Mesh<>* mesh : buffer->meshes)
			{
				if (!mesh || !mesh->numVertices || !mesh->numIndices)
					continue;
				if (mesh->vertexData.Size() < mesh->numVertices ||
					mesh->indexData.Size() < mesh->numIndices)
					continue;
				const Vertex* src = mesh->vertexData.Data();
				for (IntType v = 0; v < mesh->numVertices; v++)
				{
					if (vo >= vEnd || (size_t)vo >= scene.vertsLocal.size())
						return false; // count no casa -> el llamador rebuild-ea
					const Vertex& sv = src[v];
					PTVert lv;
					lv.px = (float)sv.x;
					lv.py = (float)sv.y;
					lv.pz = (float)sv.z;
					lv.normal888 = sv.normal;
					lv.tangent888 = sv.tangent;
					lv.u = sv.u;
					lv.v = sv.v;
					lv.colorABGR = ApplyVertexAoColor(sv.color, applyAoPolicy, aoMix);
					lv.dataFlags = FoldEmissiveDataFlags(sv.data, defEmissive);
					lv.pad0 = 0;
					lv.pad1 = 0;
					lv.pad2 = 0;
					scene.vertsLocal[(size_t)vo] = lv;
					vo++;
				}
			}
			return vo == vEnd; // el nº de verts fuente debe casar exactamente
		}

		// -----------------------------------------------------------------
		// Limpieza
		// -----------------------------------------------------------------

	#if API_D3D11
		void ReleaseGpu()
		{
			releaseAndReset(scene.gpu.vertsSrv);
			releaseAndReset(scene.gpu.trisSrv);
			releaseAndReset(scene.gpu.nodesSrv);
			releaseAndReset(scene.gpu.materialsSrv);
			releaseAndReset(scene.gpu.lightsSrv);
			releaseAndReset(scene.gpu.emiTriIdxSrv);
			releaseAndReset(scene.gpu.emiCdfSrv);
			releaseAndReset(scene.gpu.atlasSrv);
			releaseAndReset(scene.gpu.verts);
			releaseAndReset(scene.gpu.tris);
			releaseAndReset(scene.gpu.nodes);
			releaseAndReset(scene.gpu.materials);
			releaseAndReset(scene.gpu.lights);
			releaseAndReset(scene.gpu.emiTriIdx);
			releaseAndReset(scene.gpu.emiCdf);
			releaseAndReset(scene.gpu.atlas);
			scene.gpu = CinemaSceneGpu();
		}
	#endif

		void ResetBuiltCpu()
		{
			scene.verts.clear();
			scene.verts.shrink_to_fit();
			scene.materials.clear();
			scene.ranges.clear();
			scene.bvh.Clear();
			scene.bvhDeferred = false;
			scene.emiTriIdx.clear();
			scene.emiCdf.clear();
			scene.emiPower = 0.f;
			// M8-A: representacion local + instancias
			scene.instanced = false;
			scene.vertsLocal.clear();
			scene.vertsLocal.shrink_to_fit();
			scene.trisLocal.clear();
			scene.trisLocal.shrink_to_fit();
			scene.emiTriIdxLocal.clear();
			scene.emiCdfLocal.clear();
			scene.emiO2W.clear();
			scene.meanRgbCache.clear();
			scene.whitenedRegions.clear();
			scene.slices.clear();
			scene.packSlice = -1;
			scene.shelfX = 0;
			scene.shelfY = 0;
			scene.shelfH = 0;
			scene.texEntries.clear();
			scene.resolvedImages.clear();
			scene.alphaClassCache.clear();
			scene.whiteEntry = AtlasEntry();
			cinemaAlphaProbeLines = 0; // sonda de alfa: presupuesto por build
		}

		RealType MsSince(const std::chrono::steady_clock::time_point& t)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
		}

		// CAPAS N: percentil determinista (std::nth_element) sobre una copia
		// mutable del vector de distancias de un objeto. p en [0,1]; el vector
		// queda parcialmente reordenado tras la llamada (uso interno, de un
		// solo tiro por percentil, igual que CinemaRenderHeatmap con la mediana).
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
	}

	// ---------------------------------------------------------------------
	// API de captura
	// ---------------------------------------------------------------------

	void CinemaSceneCaptureBegin()
	{
		cinemaRetainVbuffers = true;
		if (!CinemaGpuSupported())
		{
			scene.state = "error";
			scene.error = "cinema_requires_fl11";
			WARNING("Cinema capture requires D3D11 feature level 11_0");
			return;
		}

		scene.draws.clear();
		scene.lights.clear();
		scene.skippedNoShader = 0;
		scene.sectionTag = CINEMA_SECTION_DEFAULT;
		scene.camInvisible = 0; // R15
		scene.camMatte = 0;     // F1
		scene.objectLayer = 0;  // COBERTURA
		scene.objectId = 0;     // CAPAS N
		scene.emissive = 0.f;   // FIX-B
		scene.shadowCatcher = 0;    // F1 4a
		scene.shadowOpacity = 1.f;  // F1 4a
		// TRIM: el efecto NO persiste entre capturas. Si persistiera, un
		// render posterior heredaria el trim del anterior en piezas que no lo
		// llevan -- el mismo modo de fallo que pt_section resuelve resetando.
		scene.trimFx = 0;
		scene.trimOutline = 0;
		scene.trimIntensity = 1.f;
		scene.trimOrigin[0] = scene.trimOrigin[1] = scene.trimOrigin[2] = 0.f;
		scene.trimMaskTexId = -1;
		scene.trimMaskUvRect[0] = 0.f; scene.trimMaskUvRect[1] = 0.f;
		scene.trimMaskUvRect[2] = 1.f; scene.trimMaskUvRect[3] = 1.f;
		capasProbeObj.clear();  // CAPAS N v2: invalida la cache por rayo (build nuevo)
		capasProbeDist.clear();
		capasProbeW = 0; capasProbeH = 0;  // T6
		scene.error.clear();
		scene.captureTarget = GFX ? (const void*)GFX->surface : nullptr;
		scene.state = "capturing";
		cinemaSceneCaptureActive = true;
	}

	BoolType CinemaSceneCaptureWantsDraw()
	{
		if (!cinemaSceneCaptureActive)
			return false;
		// Draws hacia otro render target durante la captura = trabajo
		// offscreen (surfaces temporales), no escena: rasterizan de verdad
		return !GFX || (const void*)GFX->surface == scene.captureTarget;
	}

	void CinemaSceneCaptureEnd()
	{
		cinemaRetainVbuffers = false;
		cinemaSceneCaptureActive = false;
		if (scene.state == "capturing")
			scene.state = "captured";
	}

	void CinemaSceneCaptureSetSection(IntType tag)
	{
		if (tag < CINEMA_SECTION_DEFAULT || tag > CINEMA_SECTION_CLOUDS)
		{
			WARNING("Cinema capture: invalid section tag " + NumStr(tag));
			tag = CINEMA_SECTION_DEFAULT;
		}
		scene.sectionTag = tag;
	}

	// R15 (holdout): estado de captura por timeline, igual que la seccion.
	void CinemaSceneCaptureSetCamInvisible(BoolType on)
	{
		scene.camInvisible = on ? 1 : 0;
	}

	// F1: estado de captura por timeline, igual que el holdout de invisible.
	void CinemaSceneCaptureSetCamMatte(BoolType on)
	{
		scene.camMatte = on ? 1 : 0;
	}

	// COBERTURA: estado de captura por timeline, igual que el holdout.
	void CinemaSceneCaptureSetObjectLayer(BoolType on)
	{
		scene.objectLayer = on ? 1 : 0;
	}

	// CAPAS N: id de objeto por timeline, igual que la seccion.
	void CinemaSceneCaptureSetObjectId(IntType id)
	{
		scene.objectId = id;
	}

	// FIX-B: emisividad por draw, igual que la seccion. El raster la manda con
	// render_set_uniform("uEmissive", v) y ese uniform NO existe en el shader que
	// se liga durante la captura (shader_color_fog): la llamada es silenciosa y
	// CaptureRecord leia siempre el default 0. Aqui se captura el MISMO valor por
	// la puerta de al lado. Clamp a >= 0: EMISSIVE no tiene techo en el clamp de
	// MI3 (tl_value_clamp: 0..no_limit) y BuildMaterial ya hace Clamp01.
	// F1 4a: shadow catcher por draw, mismo patron de estado de captura que
	// pt_cam_matte. La opacidad viaja en la MISMA llamada para no gastar dos
	// natives (y porque las dos vienen del mismo objeto: SHADOW_CATCHER +
	// SHADOW_OPACITY del timeline). Clamp a [0,1]: SHADOW_OPACITY es un factor,
	// no una ganancia (por encima de 1 el alpha ya satura en el kernel, pero
	// prefiero que el material que sube a GPU diga la verdad).
	void CinemaSceneCaptureSetShadowCatcher(BoolType on, RealType opacity)
	{
		float k = (float)opacity;
		if (!(k > 0.f))
			k = 0.f;
		if (k > 1.f)
			k = 1.f;
		scene.shadowCatcher = on ? 1 : 0;
		scene.shadowOpacity = k;
	}

	void CinemaSceneCaptureSetEmissive(RealType v)
	{
		float e = (float)v;
		if (!(e > 0.f))
			e = 0.f;
		scene.emissive = e;
	}

	// TRIM: efecto de los draws siguientes. Mismo patron de estado de captura.
	void CinemaSceneCaptureSetTrim(IntType fx, RealType intensity,
		RealType originX, RealType originY, RealType originZ,
		IntType outline, IntType maskTexId)
	{
		// Slot fuera del rango del pack = sin efecto. Silencio no: un fx que
		// el kernel no sabe despachar dejaria la pieza negra sin motivo.
		if (fx != 0 && (fx < 199 || fx > 254))
		{
			WARNING("Cinema trim: slot de efecto invalido " + NumStr(fx) + " (se ignora)");
			fx = 0;
		}
		scene.trimFx = fx;
		scene.trimOutline = outline ? 1 : 0;
		scene.trimIntensity = (float)intensity;
		scene.trimOrigin[0] = (float)originX;
		scene.trimOrigin[1] = (float)originY;
		scene.trimOrigin[2] = (float)originZ;
		// La mascara llega como id CRUDO de textura (sprite_get_texture). Se
		// resuelve a pagina + uvRect con EXACTAMENTE el mismo camino que usa
		// el raster al bindear un sampler (Shader::SetSamplerTexture ->
		// TexturePageLocation::Find): si GML tuviera que calcular el rect por
		// su cuenta, ese calculo seria una segunda fuente de verdad que se
		// separa de la del motor en cuanto cambie el empaquetado de paginas.
		scene.trimMaskTexId = -1;
		scene.trimMaskUvRect[0] = 0.f; scene.trimMaskUvRect[1] = 0.f;
		scene.trimMaskUvRect[2] = 1.f; scene.trimMaskUvRect[3] = 1.f;
		if (maskTexId > 0)
		{
			if (TexturePageLocation* loc = TexturePageLocation::Find(maskTexId))
			{
				scene.trimMaskTexId = loc->page->GetTexture()->GetId();
				scene.trimMaskUvRect[0] = (float)loc->uvRect.x;
				scene.trimMaskUvRect[1] = (float)loc->uvRect.y;
				scene.trimMaskUvRect[2] = (float)loc->uvRect.w;
				scene.trimMaskUvRect[3] = (float)loc->uvRect.h;
			}
			else
				scene.trimMaskTexId = maskTexId; // textura suelta: uvRect identidad
		}
	}

	// TRIM: estado de ESCENA (una vez por frame). El TIEMPO llega ya resuelto
	// desde GML como funcion pura del timeline: asi el reloj de pared del
	// preview del raster (shader_use.gml, e_render_mode.PREVIEW) no puede
	// colarse en Cinema por construccion, y el frame N da SIEMPRE la misma
	// imagen acumulada.
	void CinemaSceneTrimScene(RealType tick0, RealType uTime, RealType loopL,
		RealType sceneLight, RealType bodyScale, RealType emissive,
		IntType noiseTexId, RealType noiseSize, IntType ldrClip)
	{
		scene.trim.tick0 = (float)tick0;
		scene.trim.uTime = (float)uTime;
		scene.trim.loopL = (float)loopL;
		scene.trim.sceneLight = (float)sceneLight;
		scene.trim.bodyScale = (float)bodyScale;
		scene.trim.emissive = (float)emissive;
		scene.trim.noiseTexId = noiseTexId;
		scene.trim.noiseSize = (noiseSize > 0.0) ? (float)noiseSize : 128.f;
		scene.trim.ldrClip = ldrClip ? 1 : 0;
	}

	const CinemaTrimState& CinemaSceneTrim()
	{
		return scene.trim;
	}

	const std::vector<uint32_t>& CinemaSceneTrimSlots()
	{
		return scene.trimSlots;
	}

	void CinemaSceneCaptureRecord(VertexBuffer* buffer)
	{
		if (!cinemaSceneCaptureActive || !buffer)
			return;
		if (buffer->meshes.size() == 0)
			return; // mismo descarte de vacios que VertexBufferRenderer::Add

		Shader* shader = GFX ? GFX->shader : nullptr;
		if (!shader || !shader->IsLoaded())
		{
			scene.skippedNoShader++;
			return;
		}

		PTDraw draw;
		draw.vbufferId = buffer->id;
		draw.world = GFX->matrixM;
		draw.sectionTag = scene.sectionTag;
		draw.camInvisible = scene.camInvisible; // R15 (holdout)
		draw.camMatte = scene.camMatte;         // F1 (holdout que ocluye)
	draw.shadowCatcher = scene.shadowCatcher; // F1 4a (shadow catcher)
	draw.shadowOpacity = scene.shadowOpacity; // F1 4a
		draw.objectLayer = scene.objectLayer;   // COBERTURA (alpha-as-coverage)
		draw.objectId = scene.objectId;         // CAPAS N (id de objeto para la sonda)
		draw.trimFx = scene.trimFx;             // TRIM (ShinyTrims)
		draw.trimOutline = scene.trimOutline;
		draw.trimIntensity = scene.trimIntensity;
		draw.trimOrigin[0] = scene.trimOrigin[0];
		draw.trimOrigin[1] = scene.trimOrigin[1];
		draw.trimOrigin[2] = scene.trimOrigin[2];
		draw.trimMaskTexId = scene.trimMaskTexId;
		for (IntType k = 0; k < 4; k++)
			draw.trimMaskUvRect[k] = scene.trimMaskUvRect[k];
		// Cull EFECTIVO del rasterizer en este instante (0 none, 1 front, 2 back).
		// render_world_tl hace render_set_culling(!backfaces): geometria solida =
		// BACK, objetos con backfaces (elytra/capas/hojas) = NONE. El shell
		// invertido de un modelo outline se dibuja con BACK y el raster descarta
		// su cara cercana; el kernel debe hacer lo mismo (BuildMaterial -> flags).
		draw.cullMode = GFX->cinemaCullMode;

		// Samplers por nombre (mismo mecanismo que render_set_texture)
		static const char* samplerNames[3] = { "uTexture", "uTextureMaterial", "uTextureNormal" };
		for (IntType s = 0; s < 3; s++)
		{
			IntType idx = shader->GetSamplerIndex(StringType(samplerNames[s]));
			if (idx < 0 || idx >= 32)
				continue;
			draw.texId[s] = shader->samplerState[idx].currentTexId;
			// F4: el repeat del sampler es parte del ESTADO del draw, igual que
			// el uvRect. Sin el, el kernel no puede reproducir el mod(uv,1) que
			// el raster hace dentro de _sampleUvRect.
			draw.texRepeat[s] = (shader->samplerRepeat[idx] != 0) ? 1 : 0;
			const QVector4D& rect = shader->samplerUvRect[idx];
			draw.uvRect[s][0] = rect.x();
			draw.uvRect[s][1] = rect.y();
			draw.uvRect[s][2] = rect.z();
			draw.uvRect[s][3] = rect.w();
		}

		// Uniforms del objeto por nombre (mismo mecanismo que SubmitObject;
		// el hook retorna ANTES de SubmitObject asi que el slot actual del
		// batch buffer contiene exactamente los valores de este draw)
		ReadUniformVec4(shader, "uBlendColor", draw.blend);
		// FIX-B: uEmissive NO se puede leer del shader ligado (shader_color_fog no
		// lo declara y ReadUniformFloat caia al default 0 en TODA escena de
		// Cinema: una losa con EMISSIVE=1 daba media 0.00000). Viene por estado de
		// captura desde pt_emissive, que GML llama en los MISMOS 16 sitios en los
		// que hace render_set_uniform("uEmissive", ...) del camino de mundo.
		draw.emissive = scene.emissive;
		draw.metallic = ReadUniformFloat(shader, "uMetallic", 0.f);
		draw.roughness = ReadUniformFloat(shader, "uRoughness", 0.8f);
		draw.defaultEmissive = ReadUniformFloat(shader, "uDefaultEmissive", 1.f);
		draw.sss = ReadUniformFloat(shader, "uSSS", 0.f);
		draw.twoSided = ReadUniformInt(shader, "uTwoSided", 0);
		draw.isWater = ReadUniformInt(shader, "uIsWater", 0);
		draw.alphaHash = ReadUniformInt(shader, "uAlphaHash", 0);
		draw.materialFormat = ReadUniformInt(shader, "uMaterialFormat", 0);
		draw.useNormalMap = ReadUniformInt(shader, "uUseNormalMap", 0);
		draw.isGround = ReadUniformInt(shader, "uIsGround", 0);

		scene.draws.push_back(draw);
	}

	// ---------------------------------------------------------------------
	// Entorno / luces / camara
	// ---------------------------------------------------------------------

	void CinemaSceneEnvSet(
		RealType sunDirX, RealType sunDirY, RealType sunDirZ,
		IntType sunColor, RealType sunStrength,
		IntType ambientColor, RealType ambientStrength,
		IntType skyZenithColor, IntType skyHorizonColor,
		RealType nightAlpha,
		BoolType fogShow, IntType fogColor, IntType fogColorSun,
		RealType fogDistance, RealType fogSize, RealType fogHeight,
		RealType sunAngleDeg, RealType envLight,
		IntType backdrop, IntType backdropColor, IntType backdropColor2,
		RealType backdropAngle,
		IntType tileX0, IntType tileY0, IntType tileFullW, IntType tileFullH)
	{
		DEBUG("Cinema env_set: sunDir=" + NumStr(sunDirX) + "/" + NumStr(sunDirY) + "/" + NumStr(sunDirZ)
			+ " sunColor=" + NumStr(sunColor) + " sunStr=" + NumStr(sunStrength)
			+ " ambColor=" + NumStr(ambientColor) + " ambStr=" + NumStr(ambientStrength)
			+ " skyZen=" + NumStr(skyZenithColor) + " skyHor=" + NumStr(skyHorizonColor)
			+ " night=" + NumStr(nightAlpha));

		CinemaEnvState env;
		env.valid = true;

		RealType len = std::sqrt(sunDirX * sunDirX + sunDirY * sunDirY + sunDirZ * sunDirZ);
		if (len < 1.0e-9)
		{
			WARNING("Cinema env: zero sun direction, using +Z");
			env.sunDir[0] = 0.f;
			env.sunDir[1] = 0.f;
			env.sunDir[2] = 1.f;
		}
		else
		{
			env.sunDir[0] = (float)(sunDirX / len);
			env.sunDir[1] = (float)(sunDirY / len);
			env.sunDir[2] = (float)(sunDirZ / len);
		}

		GmlColorToLinear(sunColor, sunStrength, env.sunRadiance);
		GmlColorToLinear(ambientColor, ambientStrength, env.ambient);
		GmlColorToLinear(skyZenithColor, 1.0, env.skyZenith);
		GmlColorToLinear(skyHorizonColor, 1.0, env.skyHorizon);
		env.nightAlpha = Clamp01((float)nightAlpha);
		env.fogShow = fogShow;
		GmlColorToLinear(fogColor, 1.0, env.fogColor);
		GmlColorToLinear(fogColorSun, 1.0, env.fogColorSun);
		env.fogDistance = (float)fogDistance;
		env.fogSize = (float)fogSize;
		env.fogHeight = (float)fogHeight;

		// FIX-A (sun_softness). Contrato: sunAngleDeg es el DIAMETRO angular del
		// disco solar en grados, la misma magnitud que el raster
		// (render_high_shadows:90 divide entre 2 para sacar el radio) y la que el
		// asistente expone como "sun_softness". Cinema quiere el RADIO.
		//   Cotas [0, 170]: 0 es un sol DELTA legitimo (ver abajo) y por encima de
		//     170 el "disco" abarca casi la esfera y el cono deja de tener sentido.
		//   omega se calcula con la formula EXACTA del kernel --
		//     PT_TWO_PI * (1 - cosSunMax) en float, CinemaKernelM0.hpp:2418 -- y NO
		//     con una version mas precisa. El kernel NO recibe omega: lo deriva del
		//     coseno que viaja en el cbuffer. Si el host usara otro numero, el
		//     "divide por omega / multiplica por omega" dejaria de cancelar y la
		//     energia del sol se iria un 1 % (a 0.526 grados el ulp de float junto
		//     a 1.0 es 1.19e-7 contra un 1-cos de 1.05e-5). Usando SU formula, la
		//     cuantizacion es la misma a los dos lados y se cancela exacta.
		//   REGIMEN DELTA: si omega <= 1e-6 el kernel (:2673-2683) y el oraculo
		//     (:1734) cambian de rama y tratan sunRadiance como IRRADIANCIA
		//     PREINTEGRADA (sunWeight = 1, sin MIS). El divisor se decide alli con
		//     este mismo umbral. Medido: sin esta rama, sun_softness = 0 daba
		//     radiancia 2e8 y cinema.parity reventaba (rmse 107001 contra 0.013).
		{
			double angDiam = (double)sunAngleDeg;
			if (!(angDiam >= 0.0))
				angDiam = 0.0;
			if (angDiam > 170.0)
				angDiam = 170.0;
			const double PI_D = 3.14159265358979323846;
			double halfRad = angDiam * 0.5 * PI_D / 180.0;   // RADIO en radianes
			env.sunAngleDeg = (float)angDiam;
			env.sunCosAng = (float)std::cos(halfRad);
			env.sunOmega = 6.28318530717959f * (1.f - env.sunCosAng); // = PT_TWO_PI
		}

		// F5 (C1): peso del grupo ENTORNO en la NEE. 0 (y cualquier valor no
		// positivo o no finito) = OFF = camino BIT-IDENTICO. El clamp superior es
		// defensivo: con k enorme el domo se comeria TODAS las muestras y las
		// luces reales se quedarian sin ninguna (varianza, no sesgo, pero es una
		// forma tonta de arruinar un render).
		{
			double k = (double)envLight;
			if (!(k > 0.0))
				k = 0.0;
			if (k > 64.0)
				k = 64.0;
			env.envLight = (float)k;
		}

		// FONDO DE PELICULA: modo + los dos colores + el angulo de la rampa.
		// Los colores se quedan en DISPLAY (0..1 sRGB tal cual salen del selector):
		// NO se linealizan, porque el resolve compone DESPUES del tonemap.
		// Linealizarlos aqui seria EL bug de este mando -- meterlo en el camino de
		// la radiancia es justo lo que este ajuste NO debe hacer.
		{
			IntType m = backdrop;
			if (m < 0) m = 0;
			if (m > 2) m = 2;
			env.backdrop = (int)m;
			GmlColorToDisplay(backdropColor, env.backdropColor);
			GmlColorToDisplay(backdropColor2, env.backdropColor2);
			double a = (double)backdropAngle;
			if (!(a >= -1.0e6 && a <= 1.0e6))
				a = 0.0;
			env.backdropAngle = (float)a;
			// TILED EXPORT: solo se acepta un rect COHERENTE (lienzo positivo y no
			// absurdo). Cualquier otra cosa = render normal, que es el camino de
			// siempre: mas vale una rampa medida contra la superficie que una
			// rampa medida contra un lienzo inventado.
			if (tileFullW > 0 && tileFullH > 0 && tileFullW <= 262144 && tileFullH <= 262144)
			{
				env.tileX0 = (int)tileX0;
				env.tileY0 = (int)tileY0;
				env.tileFullW = (int)tileFullW;
				env.tileFullH = (int)tileFullH;
			}
		}

		scene.env = env;
	}

	// R12: gamma del espacio de mezcla de la niebla. Se lee UNA vez (estatico:
	// kernel y oraculo deben ver EXACTAMENTE el mismo valor durante la corrida).
	// Contrato y semantica en CinemaScene.hpp.
	float CinemaSceneFogMixGamma()
	{
		static float cached = -1.f;
		if (cached < 0.f)
		{
			cached = 2.2f; // default: espejo del raster (mix en espacio display)
			const char* v = std::getenv("MI3_CINEMA_FOGGAMMA10");
			if (v && *v)
			{
				long long n = atoll(v);
				if (n >= 0 && n < 1000)
					cached = (float)n / 10.f; // 0 = mix en lineal puro
			}
		}
		return cached;
	}

	IntType CinemaSceneLightAdd(
		RealType x, RealType y, RealType z,
		RealType dirX, RealType dirY, RealType dirZ,
		RealType range, IntType color, RealType strength,
		RealType size, RealType fadeSize,
		RealType spotRadius, RealType spotSharpness, BoolType isSpot,
		IntType typeOverride)
	{
		PTLight light = {};
		light.posx = (float)x;
		light.posy = (float)y;
		light.posz = (float)z;
		light.range = (float)range;
		float col[3];
		GmlColorToLinear(color, strength, col);
		light.colR = col[0];
		light.colG = col[1];
		light.colB = col[2];
		light.sizeRadius = (float)size;
		light.fadeSize = (float)fadeSize;
		light.sharpness = (float)spotSharpness;
		light.pad = 0.f;

		RealType len = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
		if (len > 1.0e-9)
		{
			light.dirx = (float)(dirX / len);
			light.diry = (float)(dirY / len);
			light.dirz = (float)(dirZ / len);
		}
		else
		{
			if (isSpot)
				WARNING("Cinema light: spot with zero direction, using -Z");
			light.dirx = 0.f;
			light.diry = 0.f;
			light.dirz = -1.f;
		}

		if (typeOverride == 2)
		{
			light.type = 2;
			light.cosOuter = -2.f;
		}
		else if (isSpot)
		{
			light.type = 1;
			// spotRadius = angulo COMPLETO del cono en grados (LIGHT_SPOT_RADIUS)
			light.cosOuter = (float)std::cos((spotRadius * 0.5) * DEGTORAD);
		}
		else
		{
			light.type = 0;
			light.cosOuter = -2.f; // marca "sin cono" del diseno
		}

		scene.lights.push_back(light);
		return (IntType)scene.lights.size();
	}

	void CinemaSceneCameraSet(const Matrix& viewProj,
		RealType camX, RealType camY, RealType camZ,
		RealType zNear, RealType zFar,
		RealType focusDist, RealType apertureRadius, RealType apertureCoef, BoolType dofOn, BoolType autofocus,
		BoolType ortho,
		RealType blades, RealType lensRot, RealType lensRatio, RealType lensBias)
	{
		CinemaCameraState cam;
		Matrix inv = viewProj.GetInversed();

		// Verificacion: viewProj * inv debe dar identidad (GetInversed
		// devuelve basura no inicializada si el determinante es 0)
		Matrix prod = viewProj * inv;
		RealType maxErr = 0.0;
		for (IntType i = 0; i < 16; i++)
		{
			RealType expected = (i % 5 == 0) ? 1.0 : 0.0;
			RealType err = std::abs(prod.m[i] - expected);
			if (err > maxErr)
				maxErr = err;
		}

		if (maxErr > 1.0e-3)
		{
			WARNING("Cinema camera: view_proj matrix is not invertible (err " + NumStr(maxErr) + ")");
			cam.valid = false;
		}
		else
			cam.valid = true;

		// Mismo orden de memoria que Matrix::Copy: el cbuffer PTFrame recibe
		// invViewProj memcpy TAL CUAL (nota de convencion en CinemaKernelM0.hpp)
		for (IntType i = 0; i < 16; i++)
		{
			cam.viewProj[i] = (float)viewProj.m[i];
			cam.invViewProj[i] = (float)inv.m[i];
		}
		cam.pos[0] = (float)camX;
		cam.pos[1] = (float)camY;
		cam.pos[2] = (float)camZ;
		cam.nearPlane = (float)zNear;
		cam.farPlane = (float)zFar;

		// DoF fisico (retrato). apertureRadius<=0 o dofOn=false => rayo primario
		// pinhole IDENTICO al actual (el kernel/oraculo saltan el muestreo de
		// lente por completo). El radio ya viene calibrado desde GML.
		// DOF-FIEL A3: con autofocus (CAM_DOF_DEPTH==0) GML YA NO manda un
		// focusDist/apertureRadius de fallback (llegan en 0): el foco lo mide
		// la sonda (RunAutofocusProbe) y lo aplica CinemaSceneCameraApplyAutofocus.
		// apertureCoef llega YA CALCULADO desde GML (uBlurSize * 2 * tan(fov/2));
		// ya no se deriva de apertureRadius/focusDist (esa division seria
		// indeterminada con focusDist=0). El regimen AUTOFOCUS ya NO exige
		// focusDist>0: basta con que el toggle este activo y haya un
		// apertureCoef valido (el radio real lo calcula la sonda al medir).
		// R14 (ortho): el DoF de lente fina se IGNORA bajo proyeccion
		// ortografica en v1 -- con rayos paralelos no hay un modelo de lente
		// fisico estandar (la apertura tendria que ser un plano, no un punto),
		// y el autofocus tampoco aplica. Se apaga aqui, en el saneado, para que
		// kernel y oraculo vean exactamente lo mismo (pinhole paralelo).
		cam.ortho = ortho;
		if (ortho)
			dofOn = false;

		cam.autofocus = dofOn && autofocus && apertureCoef > 0.0;
		cam.dofOn = dofOn && ((apertureRadius > 0.0 && focusDist > 0.0) || cam.autofocus);
		cam.focusDist = (float)focusDist;                          // 0 con autofocus
		cam.apertureRadius = cam.dofOn ? (float)apertureRadius : 0.f; // 0 con autofocus
		cam.apertureCoef = (float)apertureCoef;                    // ya NO se deriva de radio/foco

		// A1/A5: apertura poligonal. Todos 0 (blades<3, ratio=0, bias=0) =>
		// disco uniforme, el camino de siempre.
		cam.blades = (float)blades;
		cam.lensRot = (float)lensRot;
		cam.lensRatio = (float)lensRatio;
		cam.lensBias = (float)lensBias;

		// A2: eje optico unitario, derivado de invViewProj EXACTAMENTE igual
		// que el kernel (ndc 0,0, z=0 vs z=1, mul(invViewProj,v), normalize de
		// la diferencia). Math manual sobre inv.m[16] (mismo patron column-major
		// que ClipWorldPos mas arriba en este archivo) para no depender de
		// VecType.hpp aqui.
		{
			auto unproj4 = [&inv](RealType nx, RealType ny, RealType nz, RealType nw, RealType out[4])
			{
				out[0] = inv.m[0] * nx + inv.m[4] * ny + inv.m[8] * nz + inv.m[12] * nw;
				out[1] = inv.m[1] * nx + inv.m[5] * ny + inv.m[9] * nz + inv.m[13] * nw;
				out[2] = inv.m[2] * nx + inv.m[6] * ny + inv.m[10] * nz + inv.m[14] * nw;
				out[3] = inv.m[3] * nx + inv.m[7] * ny + inv.m[11] * nz + inv.m[15] * nw;
			};
			RealType pNear4[4], pFar4[4];
			unproj4(0.0, 0.0, 0.0, 1.0, pNear4);
			unproj4(0.0, 0.0, 1.0, 1.0, pFar4);
			RealType nxp = pNear4[0] / pNear4[3], nyp = pNear4[1] / pNear4[3], nzp = pNear4[2] / pNear4[3];
			RealType fxp = pFar4[0] / pFar4[3], fyp = pFar4[1] / pFar4[3], fzp = pFar4[2] / pFar4[3];
			RealType dx = fxp - nxp, dy = fyp - nyp, dz = fzp - nzp;
			RealType len = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (len > 1.0e-9)
			{
				cam.camFwd[0] = (float)(dx / len);
				cam.camFwd[1] = (float)(dy / len);
				cam.camFwd[2] = (float)(dz / len);
			}
			else
			{
				cam.camFwd[0] = 0.f;
				cam.camFwd[1] = 0.f;
				cam.camFwd[2] = 1.f;
			}
		}

		scene.camera = cam;
	}

	// R9 (FIX 6): CinemaRenderBegin escribe aqui el foco medido por la sonda de
	// profundidad (autofocus). El diafragma sigue al foco medido con apertureCoef.
	void CinemaSceneCameraApplyAutofocus(RealType focusDist)
	{
		if (focusDist <= 0.0)
			return;
		scene.camera.focusDist = (float)focusDist;
		scene.camera.apertureRadius = scene.camera.apertureCoef * (float)focusDist;
	}

	// DOF-FIEL A3 nivel 3: ver contrato en CinemaScene.hpp.
	void CinemaSceneCameraDisableDof()
	{
		scene.camera.dofOn = false;
	}

	// F2: lectura del DoF EFECTIVO de la camara ya saneada (y ya sobrescrita por
	// la sonda de autofocus si corrio). Solo lectura: no altera el render.
	RealType CinemaSceneCameraFocusDist() { return (RealType)scene.camera.focusDist; }
	RealType CinemaSceneCameraAperture()  { return (RealType)scene.camera.apertureRadius; }

	// ---------------------------------------------------------------------
	// Build
	// ---------------------------------------------------------------------

	// R9 (FIX 1): definido tras CinemaSceneBuild (comparte el Paso 4b/5).
	static QString FinishBuildEmissiveAndUpload(const std::chrono::steady_clock::time_point& tTotal);

	QString CinemaSceneBuild(RealType vertexAo, BoolType deferBvh)
	{
		// Por si la orquestacion olvido pt_end
		cinemaSceneCaptureActive = false;

	#if !API_D3D11
		(void)vertexAo; (void)deferBvh;
		scene.state = "error";
		scene.error = "cinema_requires_d3d11";
		return scene.error;
	#else
		// R18: mallas cuya copia CPU restaura ESTE build por readback GPU. Se
		// liberan tras el flatten (dieta P2) Y TAMBIEN si el build aborta: un
		// build fallido no puede dejarse cientos de MB de geometria colgados.
		std::vector<Mesh<>*> readbackRestored;
		auto freeReadbackCopies = [&]() -> int64_t
		{
			int64_t freed = 0;
			for (Mesh<>* mesh : readbackRestored)
			{
				if (!mesh)
					continue;
				freed += (int64_t)mesh->vertexData.Size() * (int64_t)sizeof(Vertex)
				       + (int64_t)mesh->indexData.Size() * (int64_t)sizeof(uint32_t);
				mesh->vertexData.FreeData();
				mesh->indexData.FreeData();
			}
			readbackRestored.clear();
			return freed;
		};

		auto fail = [&](const QString& error) -> QString
		{
			scene.state = "error";
			scene.error = error;
			freeReadbackCopies();
			WARNING("Cinema scene build failed: " + error);
			return error;
		};

		if (!CinemaGpuSupported())
			return fail("cinema_requires_fl11");

		auto tTotal = std::chrono::steady_clock::now();
		scene.error.clear();
		scene.lastVertexAo = vertexAo; // M8-B: para el re-flatten defensivo tras stale

		ReleaseGpu();
		ResetBuiltCpu();

		scene.stats = CinemaSceneStats();
		scene.stats.draws = (IntType)scene.draws.size();
		scene.stats.lights = (IntType)scene.lights.size();
		scene.stats.skippedNoShader = scene.skippedNoShader;

		if (scene.draws.empty())
			return fail("no_draws");

		if (!scene.camera.valid)
			WARNING("Cinema scene build: no valid camera set (pt_camera_set missing?)");
		if (!scene.env.valid)
			WARNING("Cinema scene build: no environment set (pt_env_set missing?)");

		// Vaciar el raster pendiente antes de leer superficies (ToImage)
		if (GFX)
			GFX->SubmitBatch();

		// ---- Paso 1: validar draws y calcular offsets (prefix sums) ----
		struct DrawPlan
		{
			IntType drawIdx = 0;
			VertexBuffer* buffer = nullptr;
			IntType vertStart = 0, triStart = 0;
			BoolType clipped = false; // R18: este draw pasa por el recorte
		};
		std::vector<DrawPlan> plan;
		plan.reserve(scene.draws.size());

		IntType totalVerts = 0, totalTris = 0, missingCpu = 0;
		// R13: contabilidad del readback GPU (fallback universal, abajo).
		IntType readbackDraws = 0, readbackMeshes = 0;
		int64_t readbackBytes = 0;
		RealType msReadback = 0;
		std::unordered_set<const Mesh<>*> readbackFailed;
		for (IntType d = 0; d < (IntType)scene.draws.size(); d++)
		{
			VertexBuffer* buffer = FindVertexBuffer(scene.draws[(size_t)d].vbufferId);
			if (!buffer || buffer->meshes.size() == 0)
				continue;

			BoolType missing = false, restored = false;
			IntType drawVerts = 0, drawTris = 0;
			for (Mesh<>* mesh : buffer->meshes)
			{
				if (!mesh || !mesh->numVertices || !mesh->numIndices)
					continue;
				if (mesh->vertexData.Size() < mesh->numVertices ||
					mesh->indexData.Size() < mesh->numIndices)
				{
					// Copia CPU liberada O PARCIAL (seccion 4.4): el flatten
					// lee numVertices/numIndices elementos, asi que un Size()
					// truncado seria un OOB read.
					// R13: en vez de omitir el draw, LEER LOS BYTES DE VUELTA DE
					// LA GPU (los buffers D3D11 de la malla siguen vivos). Con
					// esto el scenery recien importado (la isla) entra al render
					// como cualquier otra malla. Solo se reintenta una vez por
					// malla por build: readbackFailed cachea lo irrecuperable.
					if (readbackFailed.count(mesh))
					{
						missing = true;
						break;
					}
					auto tRb = std::chrono::steady_clock::now();
					int64_t meshBytes = 0;
					BoolType ok = ReadbackMeshCpuData(mesh, meshBytes);
					msReadback += MsSince(tRb);
					if (!ok)
					{
						readbackFailed.insert(mesh);
						missing = true;
						break;
					}
					readbackMeshes++;
					readbackBytes += meshBytes;
					readbackRestored.push_back(mesh);
					restored = true;
				}
				drawVerts += mesh->numVertices;
				drawTris += mesh->numIndices / 3;
			}

			if (missing)
			{
				missingCpu++;
				continue;
			}
			if (restored)
				readbackDraws++;
			if (!drawTris)
				continue;

			DrawPlan entry;
			entry.drawIdx = d;
			entry.buffer = buffer;
			entry.vertStart = totalVerts;
			entry.triStart = totalTris;
			plan.push_back(entry);
			totalVerts += drawVerts;
			totalTris += drawTris;
		}

		scene.stats.missingCpu = missingCpu;
		scene.stats.validDraws = (IntType)plan.size();
		scene.stats.readbackDraws = readbackDraws;
		scene.stats.readbackMeshes = readbackMeshes;
		scene.stats.bytesReadback = readbackBytes;
		scene.stats.msReadback = msReadback;

		// R13: geometria recuperada leyendo los buffers D3D11 de vuelta (el
		// scenery importado pierde su copia CPU al escribir el .meshcache).
		if (readbackDraws)
			DEBUG("Cinema build: " + QString::number(readbackDraws)
				+ " draws restaurados por readback GPU ("
				+ QString::number(readbackMeshes) + " mallas, "
				+ QString::number((double)readbackBytes / (1024.0 * 1024.0), 'f', 1) + " MB, "
				+ QString::number(msReadback, 'f', 1) + " ms)");

		// Draws cuya geometria NO es recuperable ni por CPU ni por readback GPU
		// (malla sin buffers D3D11, p.ej. tras un soft-fail de VRAM): degradacion
		// LIMPIA — se omiten con WARNING y el resto de la escena renderiza. Solo
		// es fatal si no queda NINGUN draw valido.
		if (missingCpu)
			WARNING("Cinema build: " + QString::number(missingCpu)
				+ " draws sin geometria recuperable omitidos del render"
				+ " (sin copia CPU y sin buffers GPU legibles)");
		if (plan.empty())
			return fail(missingCpu ? "cpu_data_missing_all" : "no_draws");
		if (totalVerts > CINEMA_MAX_ELEMENTS || totalTris > CINEMA_MAX_ELEMENTS)
			return fail("scene_too_large");

		// ---- Paso 1b (R18): RECORTE POR DISTANCIA a la camara ----
		// El scenery de MI no esta chunkeado espacialmente (son unos pocos
		// vbuffers gigantes agrupados por profundidad/material), asi que no se
		// puede descartar por DRAW: se hace por TRIANGULO. Aqui solo se CUENTA
		// (para poder recalcular los prefix sums y dimensionar los arrays); el
		// flatten de abajo repite el mismo predicado al escribir.
		ClipCtx clipCtx;
		BoolType clipOn = false;
		IntType clipTrisRaw = totalTris, clipVertsRaw = totalVerts;
		if (cinemaClipRadius > 0.0 && scene.camera.valid)
		{
			clipOn = true;
			clipCtx.cx = scene.camera.pos[0];
			clipCtx.cy = scene.camera.pos[1];
			clipCtx.cz = scene.camera.pos[2];
			clipCtx.r2 = (float)(cinemaClipRadius * cinemaClipRadius);
		}
		else if (cinemaClipRadius > 0.0)
			WARNING("Cinema build: scene clip pedido pero la camara no es valida -- recorte OFF");

		scene.stats.clipRadius = clipOn ? cinemaClipRadius : 0.0;

		if (clipOn)
		{
			auto tClip = std::chrono::steady_clock::now();
			IntType numRaw = (IntType)plan.size();
			std::vector<IntType> keepVerts((size_t)numRaw, 0), keepTris((size_t)numRaw, 0);
			#pragma OPENMP_FOR
			for (IntType p = 0; p < numRaw; p++)
			{
				const DrawPlan& entry = plan[(size_t)p];
				const PTDraw& draw = scene.draws[(size_t)entry.drawIdx];
				if (ClipExemptSection(draw.sectionTag))
				{
					// El suelo/las nubes entran enteros (sin coste).
					keepVerts[(size_t)p] = -1;
					continue;
				}
				CountDrawClipped(draw, entry.buffer, clipCtx,
					keepVerts[(size_t)p], keepTris[(size_t)p]);
			}

			// Re-plan: los draws que se quedan SIN triangulos salen del build
			// (no gastan material, ni rango, ni BLAS).
			std::vector<DrawPlan> clipped;
			clipped.reserve(plan.size());
			IntType newVerts = 0, newTris = 0;
			for (IntType p = 0; p < numRaw; p++)
			{
				DrawPlan entry = plan[(size_t)p];
				IntType dv, dt;
				if (keepVerts[(size_t)p] < 0) // exento: cuenta cruda del draw
				{
					dv = (p + 1 < numRaw ? plan[(size_t)p + 1].vertStart : clipVertsRaw) - entry.vertStart;
					dt = (p + 1 < numRaw ? plan[(size_t)p + 1].triStart : clipTrisRaw) - entry.triStart;
					entry.clipped = false;
				}
				else
				{
					dv = keepVerts[(size_t)p];
					dt = keepTris[(size_t)p];
					entry.clipped = true;
				}
				if (dt <= 0)
					continue;
				entry.vertStart = newVerts;
				entry.triStart = newTris;
				clipped.push_back(entry);
				newVerts += dv;
				newTris += dt;
			}

			scene.stats.msClip = MsSince(tClip);
			scene.stats.clipTrisIn = newTris;
			scene.stats.clipTrisOut = clipTrisRaw - newTris;
			scene.stats.clipVertsIn = newVerts;
			scene.stats.clipVertsOut = clipVertsRaw - newVerts;

			DEBUG("Cinema build R18 scene clip: radio=" + QString::number(cinemaClipRadius, 'f', 0)
				+ " tris " + NumStr(newTris) + " dentro / " + NumStr(clipTrisRaw - newTris) + " fuera"
				+ " (" + QString::number(clipTrisRaw ? (100.0 * (double)(clipTrisRaw - newTris) / (double)clipTrisRaw) : 0.0, 'f', 1) + "% descartado)"
				+ ", verts " + NumStr(newVerts) + " / " + NumStr(clipVertsRaw)
				+ ", draws " + NumStr((IntType)clipped.size()) + "/" + NumStr(numRaw)
				+ ", " + QString::number(scene.stats.msClip, 'f', 1) + " ms");

			plan.swap(clipped);
			totalVerts = newVerts;
			totalTris = newTris;
			scene.stats.validDraws = (IntType)plan.size();
			if (plan.empty())
				return fail("scene_clipped_empty");
		}

		// ---- Paso 1c (R18): PRESUPUESTO DE MEMORIA ANTES de construir ----
		// Estimacion de la memoria CPU que va a pedir el build con los conteos
		// YA recortados. Si no cabe con margen, se aborta LIMPIO aqui (nada
		// alojado todavia) y el editor sigue vivo. Jamas llegar al bad_alloc.
		BoolType wantInstanced = CinemaSceneInstancedEnabled();
		{
			int64_t nV = (int64_t)totalVerts, nT = (int64_t)totalTris;
			int64_t estVerts = nV * (int64_t)sizeof(PTVert);
			int64_t estTris = nT * (int64_t)sizeof(PTTri);
			// BVH: bvh.tris (16 B) + triSource/triOrder (4 B). Con arbol
			// completo ademas nodes (~1 por tri x 32 B) y el scratch del build
			// (bounds 24 + centroides 12 + orden 4 = 40 B/tri, transitorio).
			int64_t estBvh = deferBvh
				? nT * (16 + 4)
				: nT * (16 + 4 + 32 + 40);
			int64_t estLocal = wantInstanced ? (estVerts + estTris) : 0;
			int64_t estTotal = estVerts + estTris + estLocal + estBvh;

			IntType availMB = CinemaSceneMemBudgetMB();
			IntType freeMB = CinemaSceneMemAvailableMB();
			IntType procMB = CinemaSceneMemProcessMB();
			IntType vramMB = CinemaGpuAvailableMB();
			int64_t needMB = estTotal / (1024 * 1024);
			// VRAM: verts + tris (+ nodos con BVH de CPU) suben tal cual; con
			// DXR se suman ademas las AS (BLAS+TLAS), del orden de la propia
			// geometria. Factor 2 sobre verts+tris es una cota prudente.
			int64_t needVramMB = ((estVerts + estTris) * 2 + (deferBvh ? 0 : nT * 32)) / (1024 * 1024);

			scene.stats.bytesEstimated = estTotal;
			scene.stats.memAvailMB = availMB;
			scene.stats.vramAvailMB = vramMB;

			DEBUG("Cinema build R18 presupuesto: tris=" + NumStr(totalTris)
				+ " verts=" + NumStr(totalVerts)
				+ " necesita ~" + NumStr((IntType)needMB) + " MB RAM (~"
				+ NumStr((IntType)needVramMB) + " MB VRAM)"
				+ ", presupuesto " + NumStr(availMB) + " MB (libre fisica " + NumStr(freeMB)
				+ " MB) / " + NumStr(vramMB) + " MB VRAM"
				+ ", proceso " + NumStr(procMB) + " MB");

			// Margen de 768 MB: el build tiene picos transitorios (scratch del
			// BVH, staging del upload) y el resto de la app debe seguir viva.
			const int64_t marginMB = 768;

			// P2.1 (dieta): la copia LOCAL de M8-A DUPLICA la geometria (48 B
			// por vertice otra vez). Si el build no cabe CON ella pero SI sin
			// ella, se cae al camino no-instanciado (una BLAS con verts en
			// MUNDO, instancia identidad; el camino previo a M8, correcto) en
			// vez de abortar. El coste es perder el update en vivo de M8-B.
			if (wantInstanced && availMB > 0 && needMB + marginMB > (int64_t)availMB)
			{
				int64_t altMB = (estTotal - estLocal) / (1024 * 1024);
				if (altMB + marginMB <= (int64_t)availMB)
				{
					WARNING("Cinema build R18: la copia LOCAL (instanced) no cabe ("
						+ NumStr((IntType)needMB) + " MB); se construye SIN ella ("
						+ NumStr((IntType)altMB) + " MB) -- sin update en vivo");
					wantInstanced = false;
					estTotal -= estLocal;
					estLocal = 0;
					needMB = estTotal / (1024 * 1024);
					scene.stats.bytesEstimated = estTotal;
				}
			}

			if (availMB > 0 && needMB + marginMB > (int64_t)availMB)
				return fail("scene_out_of_memory tris=" + QString::number(totalTris)
					+ " need_mb=" + QString::number(needMB)
					+ " avail_mb=" + QString::number(availMB));
			if (vramMB > 0 && needVramMB + 256 > (int64_t)vramMB)
				return fail("scene_out_of_vram tris=" + QString::number(totalTris)
					+ " need_mb=" + QString::number(needVramMB)
					+ " avail_mb=" + QString::number(vramMB));
		}

		// ---- Paso 2: atlas + materiales (serial: QImage/QHash) ----
		auto tAtlas = std::chrono::steady_clock::now();
		AtlasAddWhite();
		// TRIM: la textura de RUIDO va al atlas ANTES que los materiales para
		// que su slice/rect esten listos cuando se llene el cbuffer. Es una
		// textura de escena (128x128, un asset commiteado), no de draw: entra
		// UNA vez y el dedup de AtlasAddTexture se encarga del resto.
		scene.trim.drawsWithTrim = 0;
		scene.trim.drawsTrimNoMask = 0;
		scene.trim.noiseSlice = 0;
		scene.trim.noiseRect[0] = 0.f; scene.trim.noiseRect[1] = 0.f;
		scene.trim.noiseRect[2] = 1.f; scene.trim.noiseRect[3] = 1.f;
		if (scene.trim.noiseTexId > 0)
		{
			AtlasEntry noise = AtlasAddTexture(scene.trim.noiseTexId);
			if (noise.valid)
			{
				const float ident[4] = { 0.f, 0.f, 1.f, 1.f };
				ComposeUvRect(noise, ident, scene.trim.noiseRect);
				scene.trim.noiseSlice = noise.slice;
			}
		}
		scene.materials.resize(plan.size());
		for (size_t p = 0; p < plan.size(); p++)
			scene.materials[p] = BuildMaterial(scene.draws[(size_t)plan[p].drawIdx]);
		// TRIM: conjunto ORDENADO de slots presentes. Es lo que decide QUE
		// efectos se compilan en el kernel de este render (D10) -- y por eso
		// se toma del material FINAL (con mascara resuelta), no del draw: una
		// pieza cuyo trim no llego a cablearse no debe arrastrar su rama.
		{
			scene.trimSlots.clear();
			for (size_t p = 0; p < scene.materials.size(); p++)
			{
				uint32_t fx = scene.materials[p].trimPack & 255u;
				if (fx == 0u)
					continue;
				if (std::find(scene.trimSlots.begin(), scene.trimSlots.end(), fx)
					== scene.trimSlots.end())
					scene.trimSlots.push_back(fx);
			}
			std::sort(scene.trimSlots.begin(), scene.trimSlots.end());
			QString slotStr;
			for (size_t i = 0; i < scene.trimSlots.size(); i++)
				slotStr += (i ? "," : "") + QString::number(scene.trimSlots[i]);
			// Diagnostico del trim en el acta: cuantos draws pedian efecto,
			// cuantos se quedaron SIN mascara resoluble (su trim no se ve y sin
			// esta linea seria un fallo silencioso) y que efectos acaban
			// compilandose en el kernel de este render.
			DEBUG("CinemaTrim: draws_con_trim=" + QString::number(scene.trim.drawsWithTrim)
				+ " sin_mascara=" + QString::number(scene.trim.drawsTrimNoMask)
				+ " slots=[" + slotStr + "]"
				+ " ruido_tex=" + QString::number(scene.trim.noiseTexId)
				+ " ruido_slice=" + QString::number(scene.trim.noiseSlice)
				+ " tick0=" + QString::number(scene.trim.tick0)
				+ " uTime=" + QString::number(scene.trim.uTime));
		}
		scene.stats.msAtlas = MsSince(tAtlas);

		// ---- Paso 3: flatten (OpenMP, rangos disjuntos por draw) ----
		// M8-A: con el toggle instanced ON, el flatten puebla ADEMAS la
		// representacion LOCAL paralela (vertsLocal/trisLocal). El array MUNDO
		// (verts/inputTris) sigue igual: es la fuente del BVH/oraculo/D3D11.
		BoolType instanced = wantInstanced;
		auto tFlatten = std::chrono::steady_clock::now();
		scene.verts.resize((size_t)totalVerts);
		std::vector<PTTri> inputTris((size_t)totalTris);
		scene.ranges.resize(plan.size());
		if (instanced)
		{
			scene.vertsLocal.resize((size_t)totalVerts);
			scene.trisLocal.resize((size_t)totalTris);
		}

		IntType numPlans = (IntType)plan.size();
		#pragma OPENMP_FOR
		for (IntType p = 0; p < numPlans; p++)
		{
			const DrawPlan& entry = plan[(size_t)p];
			FlattenDraw(scene.draws[(size_t)entry.drawIdx], entry.buffer,
				entry.drawIdx, entry.vertStart, entry.triStart, (uint32_t)p,
				(float)vertexAo, scene.verts.data(), inputTris.data(),
				scene.ranges[(size_t)p],
				instanced ? scene.vertsLocal.data() : nullptr,
				instanced ? scene.trisLocal.data() : nullptr,
				entry.clipped ? &clipCtx : nullptr);
		}
		scene.stats.msFlatten = MsSince(tFlatten);

		// R18: verificacion de coherencia conteo/escritura del recorte. Si un
		// draw escribiera menos (o mas) de lo planeado, los offsets del
		// siguiente estarian desplazados: mejor abortar limpio que renderizar
		// basura. Con clip OFF los rangos son los de siempre (no cuesta nada).
		if (clipOn)
		{
			for (IntType p = 0; p < numPlans; p++)
			{
				IntType expV = ((p + 1 < numPlans) ? plan[(size_t)p + 1].vertStart : totalVerts) - plan[(size_t)p].vertStart;
				IntType expT = ((p + 1 < numPlans) ? plan[(size_t)p + 1].triStart : totalTris) - plan[(size_t)p].triStart;
				if (scene.ranges[(size_t)p].vertCount != expV || scene.ranges[(size_t)p].triCount != expT)
					return fail("clip_count_mismatch draw=" + QString::number(p));
			}
		}

		// ---- P2 (dieta de memoria): soltar las copias CPU del readback ----
		// R13 restauraba la geometria del scenery leyendo los buffers D3D11 y
		// dejaba esa copia CPU VIVA para siempre (+cientos de MB en la isla, GB
		// en un mundo grande). Ya esta aplanada: se libera aqui. El siguiente
		// build la vuelve a leer de la GPU (coste medido en ms_readback).
		// IMPORTANTE: se libera ANTES de calcular liveInstGeoFp para que el
		// fingerprint del build y el de los frames siguientes usen la MISMA
		// informacion (malla sin copia CPU) y el camino vivo no vea cambios
		// fantasma que forzarian rebuild en cada scrub.
		if (!readbackRestored.empty())
		{
			IntType nMeshes = (IntType)readbackRestored.size();
			int64_t freed = freeReadbackCopies();
			scene.stats.bytesReadbackFreed = freed;
			DEBUG("Cinema build R18 dieta: liberadas "
				+ QString::number((double)freed / (1024.0 * 1024.0), 'f', 1)
				+ " MB de copias CPU restauradas por readback ("
				+ NumStr(nMeshes) + " mallas)");
		}

		// M8-A: CDF emisiva INSTANCIADA (paralela a la de mundo de FinishBuild).
		// Recorre trisLocal (orden de ENTRADA, el que veran gTris y las BLAS del
		// backend instanciado) y pesa por potencia = area MUNDO x luminancia media
		// x e -- IDENTICA a la de mundo salvo el ORDEN de indice. El area sale de
		// scene.verts (MUNDO, paralelo a vertsLocal), asi que es el area de mundo
		// real. emiO2W guarda la object->world del draw de cada emisor (el sampling
		// de NEE la necesita: no pasa por RayQuery). Congelada en fase A (escena
		// estatica); la fase B recomputara al alta/baja de emisores.
		if (instanced)
		{
			scene.liveInstWorld.resize(plan.size());
			scene.liveInstSection.resize(plan.size());
			scene.liveInstGeoFp.resize(plan.size());
			scene.liveEmiInstance.clear();
			std::vector<float> matLum(scene.materials.size(), -1.f);
			double cum = 0.0;
			for (size_t pp = 0; pp < plan.size(); pp++)
			{
				const PTDrawRange& rg = scene.ranges[pp];
				CinemaInstXform o2w = CinemaMatrixToInstXform(scene.draws[(size_t)rg.drawIdx].world);
				scene.liveInstWorld[pp] = o2w;
				scene.liveInstSection[pp] = scene.draws[(size_t)rg.drawIdx].sectionTag;
				// M8-C: fingerprint de la geometria FUENTE de esta instancia (para
				// detectar deformaciones por bend en el camino vivo).
				scene.liveInstGeoFp[pp] = CinemaDrawGeoFingerprint(scene.draws[(size_t)rg.drawIdx], nullptr, nullptr);
				IntType tEnd = rg.triStart + rg.triCount;
				for (IntType t = rg.triStart; t < tEnd; t++)
				{
					const PTTri& tri = scene.trisLocal[(size_t)t];
					const PTVert& v0 = scene.verts[tri.i0];
					const PTVert& v1 = scene.verts[tri.i1];
					const PTVert& v2 = scene.verts[tri.i2];
					float eVert = ((float)((v0.dataFlags >> 8u) & 255u)
					             + (float)((v1.dataFlags >> 8u) & 255u)
					             + (float)((v2.dataFlags >> 8u) & 255u)) / (3.f * 255.f);
					const PTMaterial& mat = scene.materials[tri.matId];
					float e = mat.emissive > eVert ? mat.emissive : eVert;
					if (e <= 0.f)
						continue;
					float ax = v1.px - v0.px, ay = v1.py - v0.py, az = v1.pz - v0.pz;
					float bx = v2.px - v0.px, by = v2.py - v0.py, bz = v2.pz - v0.pz;
					float cx = ay * bz - az * by;
					float cy = az * bx - ax * bz;
					float cz = ax * by - ay * bx;
					float area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
					if (!(area > 0.f))
						continue;
					float& lum = matLum[tri.matId];
					if (lum < 0.f)
					{
						float rgb[3];
						MeanAlbedoLinear(scene.draws[(size_t)scene.ranges[tri.matId].drawIdx], mat, rgb);
						lum = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
					}
					double power = (double)area * (double)lum * (double)e;
					if (!(power > 0.0))
						continue;
					cum += power;
					scene.emiTriIdxLocal.push_back((uint32_t)t);
					scene.emiCdfLocal.push_back((float)cum);
					scene.emiO2W.push_back(o2w);
					scene.liveEmiInstance.push_back((uint32_t)pp);
				}
			}
			scene.instanced = true;

			// M8-B: firma de topologia (order-dependiente sobre campos ESTABLES:
			// seccion + cullMode de TODOS los draws capturados) + nº de draws. Si el
			// frame nuevo no la reproduce -> topologia distinta -> rebuild seguro.
			scene.liveDrawCount = scene.draws.size();
			scene.liveStructHash = CinemaDrawTopologyHash(scene.draws);
			scene.liveLightCount = scene.lights.size();
			scene.liveLightHash = CinemaLightsHash(scene.lights);

			// C3 (dedup de BLAS por contenido, MEDICION): el fingerprint de geometria
			// ya es una clave de contenido. Cuantas BLAS unicas habria si los draws con
			// la MISMA geometria compartieran BLAS. NOTA honesta: el dedup REAL exige
			// tambien UV/color/material identicos (el vertice los lleva), asi que este
			// numero es una COTA SUPERIOR del ahorro; en escenas de personajes (partes
			// unicas) el ahorro es marginal. Se mide para decidir si vale implementarlo.
			{
				std::vector<uint64_t> fps = scene.liveInstGeoFp;
				std::sort(fps.begin(), fps.end());
				size_t uniq = std::unique(fps.begin(), fps.end()) - fps.begin();
				DEBUG("Cinema M8-C C3 dedup potencial: " + NumStr((IntType)plan.size())
					+ " draws -> " + NumStr((IntType)uniq) + " BLAS unicas por geometria ("
					+ NumStr((IntType)(plan.size() - uniq)) + " duplicados; cota superior, sin UV/mat)");
			}
		}

		// ---- Paso 4: BVH (los tris quedan REORDENADOS en bvh.tris) ----
		// R9 (FIX 1): con deferBvh (preview DXR) SALTAMOS el arbol de CPU (el
		// grueso del "tarda en cargar", ~3.5 s en escenas grandes). El backend
		// DXR traza por la BLAS de hardware, que se arma desde bvh.tris en el
		// orden en que esten -> dejamos los tris EN ORDEN DE ENTRADA con un unico
		// nodo dummy (jamas se recorre en DXR; el fallback D3D11 y el oraculo
		// completan el reorden con CinemaSceneEnsureFullBuild). La CDF emisiva de
		// abajo recorre bvh.tris, asi que queda tambien en orden de entrada:
		// indices consistentes entre BLAS, buffer de tris y CDF.
		auto tBvh = std::chrono::steady_clock::now();
		if (deferBvh)
		{
			scene.bvh.Clear();
			scene.bvh.tris.assign(inputTris.begin(), inputTris.end());
			scene.bvh.verts = scene.verts.data();
			scene.bvh.numVerts = totalVerts;
			scene.bvh.triSource.resize((size_t)totalTris);
			for (IntType i = 0; i < totalTris; i++)
				scene.bvh.triSource[(size_t)i] = (uint32_t)i;
			// Nodo dummy: hoja VACIA (count 0) con bounds amplios. Nunca se
			// recorre en el camino DXR; si por defensa alguien lo recorriera,
			// una hoja de 0 tris devuelve "miss" sin OOB.
			PTNode dummy;
			dummy.bminx = dummy.bminy = dummy.bminz = -1.0e18f;
			dummy.bmaxx = dummy.bmaxy = dummy.bmaxz = 1.0e18f;
			dummy.leftOrFirst = PT_NODE_LEAF_BIT | 0u;
			dummy.rightOrCount = 0u;
			scene.bvh.nodes.push_back(dummy);
			scene.bvh.numLeaves = 1;
			scene.bvh.maxDepth = 0;
			scene.bvhDeferred = true;
		}
		else
		{
			if (!CinemaBvhBuild(scene.bvh, scene.verts.data(), totalVerts, inputTris.data(), totalTris))
				return fail("bvh_build_failed");
			scene.bvhDeferred = false;
		}
		scene.stats.msBvh = MsSince(tBvh);

		// El array de entrada ya no hace falta: la GPU recibe bvh.tris
		std::vector<PTTri>().swap(inputTris);

		return FinishBuildEmissiveAndUpload(tTotal);
	#endif // API_D3D11
	}

	// R9 (FIX 1): Paso 4b (CDF emisiva) + Paso 5 (upload GPU) + stats finales,
	// compartido por el build normal y por CinemaSceneEnsureFullBuild (que
	// primero reconstruye el arbol BVH sobre los tris ya reordenados). Recorre
	// scene.bvh.tris en el orden en que esten (entrada o reordenado): emiTriIdx
	// queda consistente con gTris/BLAS. Devuelve "" o un codigo de error.
	static QString FinishBuildEmissiveAndUpload(const std::chrono::steady_clock::time_point& tTotal)
	{
	#if !API_D3D11
		(void)tTotal;
		return QString("cinema_requires_d3d11");
	#else
		auto fail = [&](const QString& error) -> QString
		{
			scene.state = "error";
			scene.error = error;
			WARNING("Cinema scene build failed: " + error);
			return error;
		};
		const IntType totalVerts = (IntType)scene.verts.size();

		// Re-entrante (EnsureFullBuild reconstruye sobre el mismo scene): partir de
		// CDF vacia y liberar los buffers GPU del build previo antes de re-subir.
		scene.emiTriIdx.clear();
		scene.emiCdf.clear();
		scene.emiPower = 0.f;
		ReleaseGpu();

		// ---- Paso 4b: CDF de triangulos emisores (NEE de M1) ----
		// "Es emisor" replica la logica de emision del kernel (EvalSurface:
		// e = max(mat.emissive, byte emissive del vertice, con uDefaultEmissive
		// YA plegado por el flatten en ese byte). Potencia aprox = area mundo
		// x luminancia media lineal del albedo x e (la media de los 3 bytes
		// de vertice aproxima la integral sobre el tri). Se recorre bvh.tris
		// (orden REORDENADO por hoja, el mismo que ve la GPU): emiTriIdx
		// indexa gTris directamente y el resultado es determinista. La CDF
		// pesa la DISTRIBUCION del muestreo; la energia de la imagen la pone
		// EvalSurface por texel (con emissive_scale), por eso aqui no entra
		// project_cinema_emissive_scale.
		{
			std::vector<float> matLum(scene.materials.size(), -1.f); // luminancia lazy por material
			double cum = 0.0;
			for (size_t t = 0; t < scene.bvh.tris.size(); t++)
			{
				const PTTri& tri = scene.bvh.tris[t];
				const PTVert& v0 = scene.verts[tri.i0];
				const PTVert& v1 = scene.verts[tri.i1];
				const PTVert& v2 = scene.verts[tri.i2];
				const PTMaterial& mat = scene.materials[tri.matId];

				// Byte emissive en bits 8-15 de dataFlags (contrato PTVert)
				float eVert = ((float)((v0.dataFlags >> 8u) & 255u)
				             + (float)((v1.dataFlags >> 8u) & 255u)
				             + (float)((v2.dataFlags >> 8u) & 255u)) / (3.f * 255.f);
				float e = mat.emissive > eVert ? mat.emissive : eVert;
				// TRIM (P4): un tri con efecto ES un emisor aunque su material
				// no emita nada. Entra a la CDF con una COTA ESTATICA de
				// potencia: max(gain) del catalogo x cinema_gain x intensidad.
				//
				// Por que la cota es max y no p90 (que era lo "razonable"): las
				// dos estimaciones son INSESGADAS -- el balance MIS solo exige
				// que la pdf de NEE sea la misma que la usada al pesar el hit
				// de BSDF, y que sea > 0 donde la radiancia lo es -- asi que la
				// diferencia es de VARIANZA, y NO es simetrica. Subestimar
				// produce pesos MIS grandes justo en los picos (cola pesada:
				// luciernagas); sobreestimar solo reparte muestras de mas. Con
				// p90, molten se subestimaria 6.87x y tide 3.56x, y esos dos
				// son precisamente los efectos cuyo CARACTER es el pico.
				// La cota efectiva maxima es 1.66 (spark), menos de 2x el
				// fullbright de glowing: la CDF no se desequilibra.
				//
				// Precedente que esto sigue, no excepcion que inventa: el
				// camino vivo M8-C ya congela la CDF cuando la geometria se
				// anima ("solo la VARIANZA, NUNCA la ENERGIA"). Aqui se aplica
				// la misma politica en otro eje. El gate G4 lo DEMUESTRA: el
				// mismo par de renders con la CDF desactivada tiene que
				// converger al mismo valor medio dentro del 1% a 4096 spp.
				float eTrim = TrimPowerBound(mat);
				if (eTrim > e)
					e = eTrim;
				if (e <= 0.f)
					continue;

				// Area mundo = |cross(p1 - p0, p2 - p0)| / 2
				float ax = v1.px - v0.px, ay = v1.py - v0.py, az = v1.pz - v0.pz;
				float bx = v2.px - v0.px, by = v2.py - v0.py, bz = v2.pz - v0.pz;
				float cx = ay * bz - az * by;
				float cy = az * bx - ax * bz;
				float cz = ax * by - ay * bx;
				float area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
				if (!(area > 0.f))
					continue; // degenerado (o posicion no finita)

				float& lum = matLum[tri.matId];
				if (lum < 0.f)
				{
					float rgb[3];
					MeanAlbedoLinear(scene.draws[(size_t)scene.ranges[tri.matId].drawIdx], mat, rgb);
					lum = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
				}

				double power = (double)area * (double)lum * (double)e;
				if (!(power > 0.0))
					continue; // albedo negro: Le = albedo * e = 0 tambien en el kernel

				cum += power; // acumulacion en double: CDF monotona y estable
				scene.emiTriIdx.push_back((uint32_t)t);
				scene.emiCdf.push_back((float)cum);
			}
			scene.emiPower = (float)cum;
		}

		// ---- Paso 5: upload GPU ----
		auto tUpload = std::chrono::steady_clock::now();
		CinemaSceneGpu& gpu = scene.gpu;
		// FIX race capas: generacion monotona de build (ver CinemaSceneGpu::buildGen).
		// Estatica de funcion: sobrevive a ReleaseGpu y jamas se repite en el proceso.
		{
			static uint64_t s_buildGenCounter = 0;
			gpu.buildGen = ++s_buildGenCounter;
		}
		gpu.numVerts = (UINT)totalVerts;
		gpu.numTris = (UINT)scene.bvh.tris.size();
		gpu.numNodes = (UINT)scene.bvh.nodes.size();
		gpu.numMaterials = (UINT)scene.materials.size();
		gpu.numSlices = (UINT)scene.slices.size();

		gpu.verts = CinemaGpu::CreateStructuredBuffer(sizeof(PTVert), gpu.numVerts, scene.verts.data(), false);
		gpu.tris = CinemaGpu::CreateStructuredBuffer(sizeof(PTTri), gpu.numTris, scene.bvh.tris.data(), false);
		gpu.nodes = CinemaGpu::CreateStructuredBuffer(sizeof(PTNode), gpu.numNodes, scene.bvh.nodes.data(), false);
		gpu.materials = CinemaGpu::CreateStructuredBuffer(sizeof(PTMaterial), gpu.numMaterials, scene.materials.data(), false);
		gpu.vertsSrv = gpu.verts ? CinemaGpu::CreateBufferSRV(gpu.verts, gpu.numVerts) : nullptr;
		gpu.trisSrv = gpu.tris ? CinemaGpu::CreateBufferSRV(gpu.tris, gpu.numTris) : nullptr;
		gpu.nodesSrv = gpu.nodes ? CinemaGpu::CreateBufferSRV(gpu.nodes, gpu.numNodes) : nullptr;
		gpu.materialsSrv = gpu.materials ? CinemaGpu::CreateBufferSRV(gpu.materials, gpu.numMaterials) : nullptr;

		// Luces y CDF de emisivos (NEE M1): opcionales, nullptr con conteo 0.
		// PTLight se sube TAL CUAL (stride 64, layout del diseno seccion 3);
		// emiTriIdx/emiCdf son arrays paralelos de stride 4.
		gpu.numLights = (UINT)scene.lights.size();
		gpu.numEmiTris = (UINT)scene.emiTriIdx.size();
		gpu.emiPowerTotal = scene.emiPower;
		if (gpu.numLights)
		{
			gpu.lights = CinemaGpu::CreateStructuredBuffer(sizeof(PTLight), gpu.numLights, scene.lights.data(), false);
			gpu.lightsSrv = gpu.lights ? CinemaGpu::CreateBufferSRV(gpu.lights, gpu.numLights) : nullptr;
		}
		if (gpu.numEmiTris)
		{
			gpu.emiTriIdx = CinemaGpu::CreateStructuredBuffer(sizeof(uint32_t), gpu.numEmiTris, scene.emiTriIdx.data(), false);
			gpu.emiCdf = CinemaGpu::CreateStructuredBuffer(sizeof(float), gpu.numEmiTris, scene.emiCdf.data(), false);
			gpu.emiTriIdxSrv = gpu.emiTriIdx ? CinemaGpu::CreateBufferSRV(gpu.emiTriIdx, gpu.numEmiTris) : nullptr;
			gpu.emiCdfSrv = gpu.emiCdf ? CinemaGpu::CreateBufferSRV(gpu.emiCdf, gpu.numEmiTris) : nullptr;
		}

		// Atlas: los slices comparten formato/dimensiones; verificar el pitch
		// que asume CreateTexture2DArraySRGB (w * 4)
		std::vector<const void*> slicePtrs((size_t)scene.slices.size());
		BoolType slicesOk = true;
		for (IntType s = 0; s < scene.slices.size(); s++)
		{
			QImage& img = scene.slices[(int)s];
			if (img.width() != CINEMA_ATLAS_SIZE || img.height() != CINEMA_ATLAS_SIZE ||
				img.format() != QImage::Format_RGBA8888 ||
				img.bytesPerLine() != CINEMA_ATLAS_SIZE * 4)
			{
				WARNING("Cinema atlas: slice " + NumStr(s) + " has unexpected layout");
				slicesOk = false;
				break;
			}
			slicePtrs[(size_t)s] = img.constBits();
		}

		if (slicesOk && !slicePtrs.empty())
		{
			gpu.atlas = CinemaGpu::CreateTexture2DArraySRGB(
				(UINT)CINEMA_ATLAS_SIZE, (UINT)CINEMA_ATLAS_SIZE,
				gpu.numSlices, slicePtrs.data());
			gpu.atlasSrv = gpu.atlas ? CinemaGpu::CreateTextureSRV(gpu.atlas) : nullptr;
		}

		// Los buffers opcionales tambien son fatales si TENIAN contenido: un
		// render silenciosamente sin luces o sin NEE de emisivos divergeria
		// del oraculo y del raster sin aviso.
		if (!gpu.IsValid() ||
			(gpu.numLights && !gpu.lightsSrv) ||
			(gpu.numEmiTris && (!gpu.emiTriIdxSrv || !gpu.emiCdfSrv)))
		{
			ReleaseGpu();
			return fail("gpu_alloc_failed");
		}
		scene.stats.msUpload = MsSince(tUpload);

		// ---- Stats finales ----
		scene.stats.verts = totalVerts;
		scene.stats.tris = (IntType)scene.bvh.tris.size();
		scene.stats.nodes = (IntType)scene.bvh.nodes.size();
		scene.stats.materials = (IntType)scene.materials.size();
		scene.stats.emiTris = (IntType)scene.emiTriIdx.size();
		scene.stats.emiPower = (RealType)scene.emiPower;
		scene.stats.atlasSlices = (IntType)scene.slices.size();
		scene.stats.bytesVerts = (int64_t)totalVerts * (int64_t)sizeof(PTVert);
		scene.stats.bytesTris = (int64_t)scene.bvh.tris.size() * (int64_t)sizeof(PTTri);
		scene.stats.bytesNodes = (int64_t)scene.bvh.nodes.size() * (int64_t)sizeof(PTNode);
		scene.stats.bytesMaterials = (int64_t)scene.materials.size() * (int64_t)sizeof(PTMaterial);
		scene.stats.bytesAtlas = (int64_t)scene.slices.size() * (int64_t)CINEMA_ATLAS_SIZE * CINEMA_ATLAS_SIZE * 4;
		scene.stats.msTotal = MsSince(tTotal);
		scene.stats.memProcessMB = CinemaSceneMemProcessMB(); // R18

		scene.state = "built";
		scene.worldStale = false; // un build completo refresca los verts MUNDO
		DEBUG("Cinema scene built: " + NumStr(scene.stats.validDraws) + " draws, "
			+ NumStr(scene.stats.tris) + " tris, " + NumStr(scene.stats.atlasSlices)
			+ " atlas slices, " + NumStr(scene.stats.msTotal) + " ms"
			+ (scene.bvhDeferred ? QString(" [BVH DIFERIDO]") : QString())
			+ " (flatten=" + NumStr(scene.stats.msFlatten) + " atlas=" + NumStr(scene.stats.msAtlas)
			+ " bvh=" + NumStr(scene.stats.msBvh) + " upload=" + NumStr(scene.stats.msUpload) + " ms)"
			+ " [R18 clip=" + NumStr(scene.stats.clipRadius)
			+ " tris_out=" + NumStr(scene.stats.clipTrisOut)
			+ " proc=" + NumStr(scene.stats.memProcessMB) + " MB"
			+ " libre=" + NumStr(CinemaSceneMemAvailableMB()) + " MB]");
		{
			// R15: cuantos draws y cuantos MATERIALES llevan el bit de holdout
			// (la sonda que dice si el flag llego de GML al kernel).
			// F1: mismo conteo para el holdout que OCLUYE (camMatte).
			IntType dInv = 0, mInv = 0, dMatte = 0, mMatte = 0, dCat = 0, mCat = 0;
			for (size_t d = 0; d < scene.draws.size(); d++)
			{
				if (scene.draws[d].camInvisible)
					dInv++;
				if (scene.draws[d].camMatte)
					dMatte++;
				if (scene.draws[d].shadowCatcher)
					dCat++;
			}
			for (size_t m = 0; m < scene.materials.size(); m++)
			{
				if (scene.materials[m].flags & CinemaKernelM0::MATF_CAM_INVISIBLE)
					mInv++;
				if (scene.materials[m].flags & CinemaKernelM0::MATF_CAM_MATTE)
					mMatte++;
				if (scene.materials[m].flags & CinemaKernelM0::MATF_SHADOW_CATCHER)
					mCat++;
			}
			DEBUG("Cinema holdout: draws=" + NumStr((IntType)scene.draws.size())
				+ " cam_invisible_draws=" + NumStr(dInv)
				+ " materials=" + NumStr((IntType)scene.materials.size())
				+ " cam_invisible_materials=" + NumStr(mInv)
				+ " cam_matte_draws=" + NumStr(dMatte)
				+ " cam_matte_materials=" + NumStr(mMatte)
				+ " shadow_catcher_draws=" + NumStr(dCat)
				+ " shadow_catcher_materials=" + NumStr(mCat));
		}
		DEBUG("Cinema scene debug: texEntries=" + NumStr((IntType)scene.texEntries.size())
			+ " emiPower=" + NumStr((RealType)scene.emiPower)
			+ " env.valid=" + NumStr((IntType)(scene.env.valid ? 1 : 0))
			+ " sunRad=" + NumStr((RealType)scene.env.sunRadiance[0]) + "/"
			+ NumStr((RealType)scene.env.sunRadiance[1]) + "/"
			+ NumStr((RealType)scene.env.sunRadiance[2])
			+ " skyZen=" + NumStr((RealType)scene.env.skyZenith[0]) + "/"
			+ NumStr((RealType)scene.env.skyZenith[1]) + "/"
			+ NumStr((RealType)scene.env.skyZenith[2])
			+ " amb=" + NumStr((RealType)scene.env.ambient[0]));

		// M8-B Paso 0: digest de los draws capturados para MEDIR (headless) la
		// estabilidad del vbufferId entre frames del timeline (riesgo #1 del
		// diseno). vbHash = FNV-1a sobre la secuencia de vbufferId; sigHash =
		// FNV sobre TODO lo estructural del draw MENOS la matriz (vbufferId,
		// section, cull, texId, uvRect, uniforms de material); xHash = FNV sobre
		// las 16 celdas de la matriz de mundo. Entre dos markers de la MISMA
		// escena: n/vbHash/sigHash IGUALES + xHash DISTINTO == cambio transform-only
		// (camino vivo valido). Si vbHash o sigHash cambian, esos draws NO son
		// instanciables-en-vivo y disparan rebuild completo.
		{
			auto fnvU32 = [](uint64_t h, uint32_t v) -> uint64_t {
				for (int b = 0; b < 4; b++) { h ^= (uint64_t)((v >> (b * 8)) & 0xFFu); h *= 1099511628211ull; }
				return h;
			};
			auto fnvF = [&](uint64_t h, float f) -> uint64_t {
				uint32_t u; memcpy(&u, &f, 4); return fnvU32(h, u);
			};
			// Sig ESTRUCTURAL por-draw SIN world y SIN vbufferId (el vbufferId puede
			// rotar por captura aunque la geometria sea la misma).
			auto drawSig = [&](const PTDraw& dr) -> uint64_t {
				uint64_t h = 1469598103934665603ull;
				h = fnvU32(h, (uint32_t)dr.sectionTag); h = fnvU32(h, (uint32_t)dr.cullMode);
				for (int s = 0; s < 3; s++) { h = fnvU32(h, (uint32_t)dr.texId[s]);
					for (int k = 0; k < 4; k++) h = fnvF(h, dr.uvRect[s][k]); }
				for (int k = 0; k < 4; k++) h = fnvF(h, dr.blend[k]);
				h = fnvF(h, dr.emissive); h = fnvF(h, dr.metallic); h = fnvF(h, dr.roughness);
				h = fnvF(h, dr.defaultEmissive); h = fnvF(h, dr.sss);
				h = fnvU32(h, (uint32_t)(dr.twoSided | (dr.isWater << 4) | (dr.alphaHash << 8)
					| (dr.materialFormat << 12) | (dr.useNormalMap << 16) | (dr.isGround << 20)
					| ((dr.texRepeat[0] & 0x1) << 21)  // F4: decide WRAP_UV = material
					| (dr.camInvisible << 24)     // R15: holdout = cambio de material
					| (dr.camMatte << 25)         // F1: holdout que ocluye = cambio de material
					| (dr.shadowCatcher << 26)    // F1 4a: catcher = cambio de material
					| (dr.objectLayer << 28)));  // COBERTURA = cambio de material
				h = fnvF(h, dr.shadowOpacity);   // F1 4a
				// TRIM: fx / intensidad / mascara / outline SI entran (cambian
				// solo al EDITAR, y cuando cambian la CDF de emisores deja de
				// ser valida de verdad). El ORIGEN del espacio CUERPO NO entra
				// a proposito: es la posicion de mundo de la raiz del set y
				// cambia CADA FRAME en cuanto el personaje camina -- meterlo
				// aqui seria un rebuild completo (~430 ms) por frame, o sea
				// justo lo que P5 prohibe. Viaja por el camino vivo, que ya
				// re-sube gMats igual que re-sube gLights cuando una luz se
				// mueve.
				h = fnvU32(h, (uint32_t)dr.trimFx);
				h = fnvU32(h, (uint32_t)dr.trimOutline);
				h = fnvF(h, dr.trimIntensity);
				h = fnvU32(h, (uint32_t)dr.trimMaskTexId);
				for (int k = 0; k < 4; k++) h = fnvF(h, dr.trimMaskUvRect[k]);
				return h;
			};
			// ORDER-DEPENDIENTE: hashes en orden de captura.
			uint64_t vbHash = 1469598103934665603ull, ordSig = 1469598103934665603ull, ordX = 1469598103934665603ull;
			// ORDER-INDEPENDIENTE: agregados conmutativos (XOR/suma) -> revelan si el
			// CONJUNTO es estable aunque el orden cambie.
			uint64_t vbXor = 0, sigXor = 0; uint64_t vbSum = 0, sigSum = 0;
			uint32_t vbMin = 0xFFFFFFFFu, vbMax = 0;
			QString first;
			for (size_t d = 0; d < scene.draws.size(); d++)
			{
				const PTDraw& dr = scene.draws[d];
				uint32_t vb = (uint32_t)dr.vbufferId;
				uint64_t ds = drawSig(dr);
				vbHash = fnvU32(vbHash, vb);
				ordSig = fnvU32(fnvU32(ordSig, vb), (uint32_t)ds) ^ (ds << 1);
				for (int k = 0; k < 16; k++) ordX = fnvF(ordX, (float)dr.world.m[k]);
				vbXor ^= vb; vbSum += vb; sigXor ^= ds; sigSum += ds;
				if (vb < vbMin) vbMin = vb; if (vb > vbMax) vbMax = vb;
				if (d < 8) first += (d ? "," : "") + QString::number((int)vb);
			}
			DEBUG("Cinema draw-digest: n=" + NumStr((IntType)scene.draws.size())
				+ " valid=" + NumStr(scene.stats.validDraws)
				+ " vbHash=" + QString::number((qulonglong)vbHash, 16)
				+ " ordSig=" + QString::number((qulonglong)ordSig, 16)
				+ " ordX=" + QString::number((qulonglong)ordX, 16)
				+ " vbXor=" + QString::number((qulonglong)vbXor, 16)
				+ " vbSum=" + QString::number((qulonglong)vbSum)
				+ " sigXor=" + QString::number((qulonglong)sigXor, 16)
				+ " sigSum=" + QString::number((qulonglong)sigSum)
				+ " vbRange=" + QString::number((int)vbMin) + ".." + QString::number((int)vbMax)
				+ " first8=" + first);
		}

		if (qEnvironmentVariableIsSet("MI3_CINEMA_DEBUG_ATLAS"))
			for (IntType s = 0; s < scene.slices.size(); s++)
				scene.slices[(int)s].save("Data/dbg_atlas" + QString::number((int)s) + ".png");
		return QString();
	#endif // API_D3D11
	}

	BoolType CinemaSceneIsBvhDeferred()
	{
		return scene.bvhDeferred;
	}

	QString CinemaSceneEnsureFullBuild()
	{
	#if !API_D3D11
		return QString("cinema_requires_d3d11");
	#else
		if (!scene.bvhDeferred)
			return QString(); // ya completo

		if (scene.verts.empty() || scene.bvh.tris.empty())
		{
			WARNING("CinemaSceneEnsureFullBuild: no hay escena diferida que completar");
			return QString("scene_not_built");
		}

		auto tTotal = std::chrono::steady_clock::now();

		// bvh.tris esta en ORDEN DE ENTRADA (build diferido). Copiar antes de
		// construir el arbol: CinemaBvhBuild hace bvh.Clear() (borraria la fuente).
		std::vector<PTTri> inputTris = scene.bvh.tris;
		const IntType totalVerts = (IntType)scene.verts.size();
		const IntType totalTris = (IntType)inputTris.size();

		auto tBvh = std::chrono::steady_clock::now();
		if (!CinemaBvhBuild(scene.bvh, scene.verts.data(), totalVerts, inputTris.data(), totalTris))
		{
			scene.state = "error";
			scene.error = "bvh_build_failed";
			return scene.error;
		}
		scene.stats.msBvh = MsSince(tBvh);
		scene.bvhDeferred = false;
		std::vector<PTTri>().swap(inputTris);

		// Re-arma CDF emisiva (orden de hoja) + re-sube buffers D3D11 (con la
		// escena reordenada, tal cual el build completo).
		QString err = FinishBuildEmissiveAndUpload(tTotal);
		if (!err.isEmpty())
			return err;

		DEBUG("CinemaSceneEnsureFullBuild: BVH de CPU construido tras build diferido ("
			+ NumStr(scene.stats.msBvh) + " ms bvh)");
		return QString();
	#endif
	}

	void CinemaSceneInvalidate()
	{
		cinemaSceneCaptureActive = false;
	#if API_D3D11
		ReleaseGpu();
	#endif
		ResetBuiltCpu();
		scene.draws.clear();
		scene.lights.clear();
		scene.skippedNoShader = 0;
		scene.sectionTag = CINEMA_SECTION_DEFAULT;
		scene.camInvisible = 0; // R15
		scene.camMatte = 0;     // F1
		scene.objectLayer = 0;  // COBERTURA
		scene.objectId = 0;     // CAPAS N
		scene.emissive = 0.f;   // FIX-B
		scene.shadowCatcher = 0;    // F1 4a
		scene.shadowOpacity = 1.f;  // F1 4a
		scene.env = CinemaEnvState();
		scene.camera = CinemaCameraState();
		scene.stats = CinemaSceneStats();
		scene.error.clear();
		scene.state = "empty";
	}

	QString CinemaSceneStatus()
	{
		const CinemaSceneStats& s = scene.stats;
		QString out;
		out += "state=" + scene.state;
		out += " error=" + (scene.error.isEmpty() ? QString("none") : scene.error);
		out += " draws=" + QString::number(scene.state == "built" || scene.state == "error"
			? s.draws : (IntType)scene.draws.size());
		out += " valid_draws=" + QString::number(s.validDraws);
		out += " missing_cpu=" + QString::number(s.missingCpu);
		out += " readback_draws=" + QString::number(s.readbackDraws);
		out += " readback_meshes=" + QString::number(s.readbackMeshes);
		out += " skipped_no_shader=" + QString::number(scene.skippedNoShader);
		// R15 (holdout): cuantos draws capturados llevan la marca "invisible a
		// camara". Es la sonda que dice si el flag llego de GML al material.
		{
			IntType camInv = 0;
			for (size_t d = 0; d < scene.draws.size(); d++)
				if (scene.draws[d].camInvisible)
					camInv++;
			out += " cam_invisible_draws=" + QString::number(camInv);
		}
		out += " verts=" + QString::number(s.verts);
		out += " tris=" + QString::number(s.tris);
		out += " nodes=" + QString::number(s.nodes);
		out += " materials=" + QString::number(s.materials);
		out += " lights=" + QString::number((IntType)scene.lights.size());
		{
			IntType areaLights = 0;
			double areaColorSum = 0.0;
			double areaRange = 0.0;
			double areaSide = 0.0;
			for (const PTLight& light : scene.lights)
			{
				if (light.type != 2)
					continue;
				++areaLights;
				areaColorSum += light.colR + light.colG + light.colB;
				areaRange = light.range;
				areaSide = light.sizeRadius;
			}
			out += " area_lights=" + QString::number(areaLights);
			out += " area_color_sum=" + QString::number(areaColorSum, 'g', 6);
			out += " area_range=" + QString::number(areaRange, 'g', 6);
			out += " area_side=" + QString::number(areaSide, 'g', 6);
		}
#if API_D3D11
		out += " lights_gpu=" + QString::number((IntType)scene.gpu.numLights);
#else
		out += " lights_gpu=0";
#endif
		out += " emi_tris=" + QString::number(s.emiTris);
		out += " emi_power=" + QString::number((double)s.emiPower, 'g', 6);
		out += " atlas_slices=" + QString::number(s.atlasSlices);
		out += " textures_missing=" + QString::number(s.texturesMissing);
		out += " camera=" + QString::number(scene.camera.valid ? 1 : 0);
		out += " env=" + QString::number(scene.env.valid ? 1 : 0);
		out += " bytes_verts=" + QString::number((qlonglong)s.bytesVerts);
		out += " bytes_tris=" + QString::number((qlonglong)s.bytesTris);
		out += " bytes_nodes=" + QString::number((qlonglong)s.bytesNodes);
		out += " bytes_materials=" + QString::number((qlonglong)s.bytesMaterials);
		out += " bytes_atlas=" + QString::number((qlonglong)s.bytesAtlas);
		out += " bytes_total=" + QString::number((qlonglong)(s.bytesVerts + s.bytesTris + s.bytesNodes + s.bytesMaterials + s.bytesAtlas));
		out += " bytes_readback=" + QString::number((qlonglong)s.bytesReadback);
		out += " ms_readback=" + QString::number(s.msReadback, 'f', 1);
		out += " ms_flatten=" + QString::number(s.msFlatten, 'f', 1);
		out += " ms_atlas=" + QString::number(s.msAtlas, 'f', 1);
		out += " ms_bvh=" + QString::number(s.msBvh, 'f', 1);
		out += " ms_upload=" + QString::number(s.msUpload, 'f', 1);
		out += " ms_total=" + QString::number(s.msTotal, 'f', 1);
		// R18 (escenas gigantes): recorte por distancia, presupuesto y dieta.
		out += " clip_radius=" + QString::number(s.clipRadius, 'f', 1);
		out += " clip_tris_in=" + QString::number(s.clipTrisIn);
		out += " clip_tris_out=" + QString::number(s.clipTrisOut);
		out += " clip_verts_in=" + QString::number(s.clipVertsIn);
		out += " clip_verts_out=" + QString::number(s.clipVertsOut);
		out += " ms_clip=" + QString::number(s.msClip, 'f', 1);
		out += " bytes_estimated=" + QString::number((qlonglong)s.bytesEstimated);
		out += " bytes_readback_freed=" + QString::number((qlonglong)s.bytesReadbackFreed);
		out += " mem_avail_mb=" + QString::number(s.memAvailMB);
		out += " mem_process_mb=" + QString::number(s.memProcessMB);
		out += " vram_avail_mb=" + QString::number(s.vramAvailMB);
		out += " instanced=" + QString::number(scene.instanced ? 1 : 0);
		return out;
	}

	// ---------------------------------------------------------------------
	// R13: control y repro del readback GPU (solo harness/diagnostico)
	// ---------------------------------------------------------------------

	void CinemaSceneSetReadback(BoolType on)
	{
		cinemaReadbackVbuffers = on;
	}

	IntType CinemaSceneDebugFreeCpuMeshes()
	{
		// Reproduce EXACTAMENTE el estado que deja el motor tras escribir un
		// .meshcache (VertexBuffer::Write) o tras completar un vbuffer grande
		// (VertexBuffer::Complete): la geometria vive SOLO en la GPU. Se aplica
		// al mismo conjunto que el motor libera de verdad -- vbuffers que no
		// caben en un batch (numIndices > MAX_BATCH_INDICES) y con sus buffers
		// D3D11 ya creados -- para no romper el batcher del raster, que SI
		// necesita la copia CPU de los vbuffers pequenos. Devuelve el numero de
		// mallas liberadas. Ningun camino de producto la llama.
		IntType freed = 0;
		for (IntType i = 0; i < Asset::assets.Size(); i++)
		{
			Asset* asset = Asset::assets.Value(i);
			if (!asset || asset->assetId != ID_VertexBuffer)
				continue;

			VertexBuffer* buffer = (VertexBuffer*)asset;
			if (buffer->numIndices <= MAX_BATCH_INDICES || buffer->saveData)
				continue;

			for (Mesh<>* mesh : buffer->meshes)
			{
				if (!mesh || !mesh->HasBuffers())
					continue;
				if (!mesh->vertexData.Size() && !mesh->indexData.Size())
					continue;
				mesh->vertexData.FreeData();
				mesh->indexData.FreeData();
				freed++;
			}
		}
		return freed;
	}

	// ---------------------------------------------------------------------
	// Lectura
	// ---------------------------------------------------------------------

	const CinemaEnvState& CinemaSceneEnv()
	{
		return scene.env;
	}

	const CinemaCameraState& CinemaSceneCamera()
	{
		return scene.camera;
	}

	const std::vector<PTLight>& CinemaSceneLights()
	{
		return scene.lights;
	}

	const CinemaSceneStats& CinemaSceneGetStats()
	{
		return scene.stats;
	}

	const std::vector<PTDraw>& CinemaSceneGetDraws()
	{
		return scene.draws;
	}

	const std::vector<PTDrawRange>& CinemaSceneGetDrawRanges()
	{
		return scene.ranges;
	}

	const CinemaBvh& CinemaSceneGetBvh()
	{
		return scene.bvh;
	}

	const std::vector<PTVert>& CinemaSceneGetVerts()
	{
		return scene.verts;
	}

	const std::vector<PTMaterial>& CinemaSceneGetMaterials()
	{
		return scene.materials;
	}

	const std::vector<uint32_t>& CinemaSceneGetEmissiveTris()
	{
		return scene.emiTriIdx;
	}

	const std::vector<float>& CinemaSceneGetEmissiveCdf()
	{
		return scene.emiCdf;
	}

	RealType CinemaSceneGetEmissivePower()
	{
		return (RealType)scene.emiPower;
	}

	// M8-A: toggle instanced (env var cacheada; default ON). "0"/"false"/"off"
	// -> OFF (camino de una BLAS con verts en MUNDO). Se lee una sola vez: no
	// cambia dentro de una sesion (un cambio exigiria rebuild de escena).
	BoolType CinemaSceneInstancedEnabled()
	{
		static int cached = -1;
		if (cached < 0)
		{
			QByteArray v = qgetenv("MI3_CINEMA_INSTANCED").trimmed().toLower();
			cached = (v == "0" || v == "false" || v == "off") ? 0 : 1;
			DEBUG("Cinema M8-A: MI3_CINEMA_INSTANCED=" + QString(cached ? "1 (instanciado)" : "0 (BLAS unica)"));
		}
		return cached != 0;
	}

	BoolType CinemaSceneIsInstanced()
	{
		return scene.instanced;
	}

	const std::vector<PTVert>& CinemaSceneGetVertsLocal()
	{
		return scene.vertsLocal;
	}

	const std::vector<PTTri>& CinemaSceneGetTrisLocal()
	{
		return scene.trisLocal;
	}

	const std::vector<uint32_t>& CinemaSceneGetEmissiveTrisLocal()
	{
		return scene.emiTriIdxLocal;
	}

	const std::vector<float>& CinemaSceneGetEmissiveCdfLocal()
	{
		return scene.emiCdfLocal;
	}

	const std::vector<CinemaInstXform>& CinemaSceneGetEmissiveO2W()
	{
		return scene.emiO2W;
	}

	BoolType CinemaSceneWorldStale()
	{
		return scene.worldStale;
	}

	// M8-B (§5): si un update en vivo dejo los verts MUNDO desfasados y un consumidor
	// del camino aplanado (fallback D3D11, oraculo) los necesita, re-aplanar los
	// draws FRESCOS (scene.draws quedo con el frame nuevo tras la re-captura) con un
	// build completo -> verts/BVH MUNDO consistentes y worldStale limpio. No-op si
	// no hay stale. Solo se dispara en el cambio de backend a mitad de sesion (el
	// camino vivo es DXR-only): protege ese caso raro.
	QString CinemaSceneRebuildIfWorldStale()
	{
		if (!scene.worldStale)
			return QString();
		DEBUG("Cinema M8-B: worldStale -> rebuild completo (consumidor del camino MUNDO CPU)");
		return CinemaSceneBuild(scene.lastVertexAo, false);
	}

	// CAPAS N: sonda PINHOLE de CPU (probeW x probeH, centro de pixel, sin AA
	// ni DoF) sobre el BVH del ultimo build valido, agregada por
	// PTDraw::objectId. V3 (T6, Cambio 22): el rayo de la sonda es un RAYO DE
	// CAMARA y se traza con CinemaOracleTraceCameraRay -- LA MISMA traversal
	// que usa el oraculo/kernel para el rayo primario (espejo 1:1, gate de
	// paridad P1/P2/P3): holdout MATF_CAM_INVISIBLE con presupuesto, backface
	// culling, cutout ALPHA_TEST (texel.a < 0.5 descartado) y TEXEL FANTASMA
	// ALPHA_BLEND (aEff = texel.a * blend.a < 0.004 descartado, el FIX 3 de
	// SPEC_RONDA2_FIXES.md). YA NO es geometria pura: antes de esta ronda
	// CinemaBvhTrace no muestreaba el atlas y una superficie ALPHA_BLEND
	// transparente delante de la camara (overlay/goggles) tapaba la sonda
	// entera sin que el render la viera -- causa raiz medida de T6
	// (probe_near=18.04 vs depth_near=42.99, agree=0; ver SS6.6 del spec).
	//
	// Formato de retorno (ok), v2:
	//   "ok probe=<W>x<H> rays=<n> hits=<n> sky=<n> near=<f> far=<f> objs=<m>;<rec>;<rec>;..."
	// con cada <rec> = "id,px,dmin,d10,d50,d90,dmax,sec,src" (9 campos, coma,
	// sin espacios). dmin/dmax = percentiles 2/98 (no extremos); d10/d50/d90 =
	// percentiles 10/50/90; todos de la distancia RADIAL (magnitud de
	// direccion normalizada, igual convencion que CinemaOracle). sec = el
	// sectionTag que mas impactos aporto a ese objeto. src = 0 (registro de la
	// SONDA, px = numero de rayos) o 1 (registro de FALLBACK por AABB mundo,
	// px = 0: el objeto no tuvo NINGUN primer impacto -- 100% tapado o fuera
	// de cuadro -- y D4 le estima la profundidad geometricamente para que el
	// planificador no lo mande al telon por falta de dato). near/far del
	// header = minimo/maximo de las distancias de TODOS los impactos de la
	// SONDA (no percentiles; no incluye los objetos de fallback AABB).
	// Registros ordenados por id ascendente (sonda + AABB mezclados).
	//
	// Errores (mismo patron que pt_depth_save): "error:bad_arg" (W/H fuera de
	// [16, 1024]), "error:scene_not_built" (BVH invalido o sin verts),
	// "error:camera_not_set" (camara no fijada), "error:world_stale" (si
	// CinemaSceneRebuildIfWorldStale() no consigue dejar la escena fresca).
	//
	// CAPAS N v2 (D2): esta funcion llena la cache POR RAYO capasProbeObj /
	// capasProbeDist (declaradas arriba, junto a cinemaSceneCaptureActive) que
	// consume CinemaSceneBandDepths mas abajo en este archivo.
	QString CinemaSceneObjectDepths(IntType probeW, IntType probeH)
	{
		if (probeW < 16 || probeW > 1024 || probeH < 16 || probeH > 1024)
			return QString("error:bad_arg");

		if (scene.worldStale)
		{
			QString err = CinemaSceneRebuildIfWorldStale();
			if (!err.isEmpty())
				return QString("error:world_stale");
		}

		// T6: con BVH DIFERIDO (preview DXR) el arbol es una hoja vacia y la sonda
		// devolveria "todo cielo" en silencio. Completar el build es lo mismo que
		// hace el fallback D3D11/oraculo.
		if (CinemaSceneIsBvhDeferred())
		{
			QString errb = CinemaSceneEnsureFullBuild();
			if (!errb.isEmpty())
				return QString("error:bvh_deferred");
		}

		if (!scene.bvh.IsValid() || scene.verts.empty())
			return QString("error:scene_not_built");
		if (!scene.camera.valid)
			return QString("error:camera_not_set");

		const CinemaCameraState& cam = scene.camera;
		const CinemaBvh& bvh = scene.bvh;
		const std::vector<PTDrawRange>& ranges = scene.ranges;
		const std::vector<PTDraw>& draws = scene.draws;

		// Unproyeccion manual sobre invViewProj: MISMA convencion column-major
		// que el oraculo (CinemaOracle.cpp, MulMatVec) y que CinemaSceneCameraSet
		// mas arriba en este archivo: out[i] = m[i] + m[4+i]*y + m[8+i]*z + m[12+i]*w.
		auto unproj4 = [&cam](float nx, float ny, float nz, float nw, float out[4])
		{
			const float* m = cam.invViewProj;
			out[0] = m[0] * nx + m[4] * ny + m[8] * nz + m[12] * nw;
			out[1] = m[1] * nx + m[5] * ny + m[9] * nz + m[13] * nw;
			out[2] = m[2] * nx + m[6] * ny + m[10] * nz + m[14] * nw;
			out[3] = m[3] * nx + m[7] * ny + m[11] * nz + m[15] * nw;
		};

		struct ObjAccum
		{
			std::vector<float> dist;
			std::unordered_map<IntType, IntType> secCounts;
		};
		std::map<IntType, ObjAccum> accum;

		IntType rays = probeW * probeH;
		IntType hits = 0, sky = 0;
		BoolType haveAny = false;
		float distMin = 0.f, distMax = 0.f;
		// V3: ya NO cuenta saltos (el presupuesto de camInvisible/transparencia
		// ahora es el del oraculo/kernel -- ORACLE_MAX_TRANSP/_HOLDOUT -- que es
		// el que protege el gate de paridad). Se mantiene la variable y la clave
		// "skip=0" en la cabecera para no romper el formato que consume el
		// arnes/checker; el campo nuevo "sem=" documenta el cambio de semantica.
		IntType skipOverflow = 0;

		// CAPAS N v2: arranca la cache POR RAYO de esta corrida (16c.1).
		capasProbeObj.clear();
		capasProbeDist.clear();
		capasProbeObj.reserve((size_t)rays);
		capasProbeDist.reserve((size_t)rays);
		// T6: dimensiones de ESTA sonda, para el comparador pt_probe_agree
		// (CinemaSceneGetProbeCache).
		capasProbeW = probeW;
		capasProbeH = probeH;
		// T6 (15c): matId POR RAYO, solo para el volcado de diagnostico de abajo
		// (MI3_CAPAS_PROBE_DEBUG=1). Local a esta llamada, no se cachea.
		std::vector<IntType> dbgRayMatId;
		dbgRayMatId.reserve((size_t)rays);

		// V3: fallback honesto. Si el atlas CPU esta vacio, SampleAlbedo (dentro
		// del oraculo) devuelve blanco opaco y la traversal se comporta como la
		// geometrica pura de antes -- el "agree" bajo que salga es informacion,
		// no un misterio. Se loguea UNA vez por llamada, no por rayo.
		if (CinemaSceneGetAtlasSlices().isEmpty())
			DEBUG("T6: atlas CPU vacio -- la sonda cae a traversal geometrica");

		for (IntType y = 0; y < probeH; y++)
		{
			float ndcY = 1.f - (((float)y + 0.5f) / (float)probeH) * 2.f;
			for (IntType x = 0; x < probeW; x++)
			{
				float ndcX = (((float)x + 0.5f) / (float)probeW) * 2.f - 1.f;

				float pNear[4], pFar[4];
				unproj4(ndcX, ndcY, 0.f, 1.f, pNear);
				unproj4(ndcX, ndcY, 1.f, 1.f, pFar);
				float nearW[3] = { pNear[0] / pNear[3], pNear[1] / pNear[3], pNear[2] / pNear[3] };
				float farW[3] = { pFar[0] / pFar[3], pFar[1] / pFar[3], pFar[2] / pFar[3] };
				float dirx = farW[0] - nearW[0], diry = farW[1] - nearW[1], dirz = farW[2] - nearW[2];
				float dlen = std::sqrt(dirx * dirx + diry * diry + dirz * dirz);
				if (dlen > 1.0e-9f) { dirx /= dlen; diry /= dlen; dirz /= dlen; }

				// R14: bajo ortho el origen es el punto del plano near de ESTE
				// pixel (rayos paralelos); en perspectiva es cam.pos. Mismo
				// espejo que el oraculo/kernel.
				float orgx = cam.ortho ? nearW[0] : cam.pos[0];
				float orgy = cam.ortho ? nearW[1] : cam.pos[1];
				float orgz = cam.ortho ? nearW[2] : cam.pos[2];

				CinemaBvhRay ray;
				ray.ox = orgx; ray.oy = orgy; ray.oz = orgz;
				ray.dx = dirx; ray.dy = diry; ray.dz = dirz;
				ray.tMin = 1.0e-4f;
				ray.tMax = cam.farPlane;

				// V3 (T6): el rayo de la sonda es un RAYO DE CAMARA y se traza con
				// LA MISMA traversal que el render (via el oraculo, espejo 1:1 del
				// kernel): holdout camInvisible con presupuesto, backface culling,
				// cutout alpha-test y TEXEL FANTASMA alpha-blend (FIX 3). Antes se
				// usaba CinemaBvhTrace CRUDO: en el demo real los 57600 rayos
				// chocaban con texels ALPHA_BLEND transparentes de objId=1
				// (matId=59, alphaBlend=1) a t=18.04 -- geometria que el _depth.png
				// NO ve -- y la concordancia salia agree=0.000 med_ratio=0.1358.
				CinemaBvhHit hit;
				hit.triIdx = PT_BVH_NO_HIT;
				hit.t = 0.f; hit.u = 0.f; hit.v = 0.f;
				{
					float org3[3] = { orgx, orgy, orgz };
					float dir3[3] = { dirx, diry, dirz };
					float ht = 0.f, hbu = 0.f, hbv = 0.f;
					IntType htri = -1;
					if (CinemaOracleTraceCameraRay(org3, dir3, ray.tMin, ray.tMax,
						&ht, &htri, &hbu, &hbv) && htri >= 0)
					{
						hit.triIdx = (uint32_t)htri;
						hit.t = ht; hit.u = hbu; hit.v = hbv;
					}
				}
				if (hit.triIdx == PT_BVH_NO_HIT)
				{
					sky++;
					capasProbeObj.push_back(-1);
					capasProbeDist.push_back(0.f);
					dbgRayMatId.push_back(-1);
					continue;
				}
				hits++;

				IntType matId = (IntType)bvh.tris[hit.triIdx].matId;
				if (matId < 0 || (size_t)matId >= ranges.size())
				{
					// defensivo: no deberia pasar (matId es indice en ranges).
					// CAPAS N v2: cuenta como telon (0), NO como cielo -- y
					// sigue sin contaminar accum (D4/16c.2).
					capasProbeObj.push_back(0);
					capasProbeDist.push_back(hit.t);
					dbgRayMatId.push_back(-1);
					continue;
				}
				IntType drawIdx = ranges[(size_t)matId].drawIdx;
				if (drawIdx < 0 || (size_t)drawIdx >= draws.size())
				{
					capasProbeObj.push_back(0);
					capasProbeDist.push_back(hit.t);
					dbgRayMatId.push_back(matId);
					continue;
				}
				const PTDraw& draw = draws[(size_t)drawIdx];
				IntType objId = draw.objectId;

				if (!haveAny) { distMin = hit.t; distMax = hit.t; haveAny = true; }
				else { if (hit.t < distMin) distMin = hit.t; if (hit.t > distMax) distMax = hit.t; }

				ObjAccum& oa = accum[objId];
				oa.dist.push_back(hit.t);
				oa.secCounts[draw.sectionTag]++;

				capasProbeObj.push_back(objId);
				capasProbeDist.push_back(hit.t);
				dbgRayMatId.push_back(matId);
			}
		}

		// T6 (15c): volcado de diagnostico de los 20 hits mas cercanos --
		// SOLO con MI3_CAPAS_PROBE_DEBUG=1. Nombra al culpable cuando la
		// concordancia (pt_probe_agree) sigue baja tras el arreglo 15a: que
		// objeto/material tapa la escena en la sonda de CPU. No hay un campo
		// "display_name" en PTDraw (es un dato de GML, no de esta capa C++):
		// se vuelca objId (resoluble a nombre desde GML via mi3rootName[],
		// el mismo mapa que usa pt_object_bands) en vez del string.
		if (!qgetenv("MI3_CAPAS_PROBE_DEBUG").trimmed().isEmpty()
			&& qgetenv("MI3_CAPAS_PROBE_DEBUG").trimmed() != QByteArray("0"))
		{
			std::vector<IntType> order((size_t)rays);
			for (IntType i = 0; i < rays; i++) order[(size_t)i] = i;
			std::sort(order.begin(), order.end(), [&](IntType a, IntType b)
			{
				float da = (capasProbeObj[(size_t)a] >= 0) ? capasProbeDist[(size_t)a] : 1.0e30f;
				float db = (capasProbeObj[(size_t)b] >= 0) ? capasProbeDist[(size_t)b] : 1.0e30f;
				return da < db;
			});
			DEBUG("T6 probe debug: 20 hits mas cercanos de CinemaSceneObjectDepths (probe="
				+ QString::number(probeW) + "x" + QString::number(probeH) + ")");
			IntType shown = 0;
			for (IntType oi = 0; oi < rays && shown < 20; oi++)
			{
				IntType ri = order[(size_t)oi];
				IntType oid = capasProbeObj[(size_t)ri];
				if (oid < 0)
					break;   // el resto son cielo (o ya se listaron los 20)
				float t = capasProbeDist[(size_t)ri];
				IntType mId = dbgRayMatId[(size_t)ri];
				QString line = "T6 probe debug: #" + QString::number(shown) + " t=" + QString::number(t, 'g', 6)
					+ " objId=" + QString::number(oid) + " matId=" + QString::number(mId);
				if (mId >= 0 && (size_t)mId < ranges.size())
				{
					IntType dIdx = ranges[(size_t)mId].drawIdx;
					if (dIdx >= 0 && (size_t)dIdx < draws.size())
					{
						const PTDraw& d = draws[(size_t)dIdx];
						line += " sectionTag=" + QString::number(d.sectionTag)
							+ " camInvisible=" + QString::number(d.camInvisible)
							+ " camMatte=" + QString::number(d.camMatte);
					}
				}
				if (mId >= 0 && (size_t)mId < scene.materials.size())
				{
					uint32_t flags = scene.materials[(size_t)mId].flags;
					line += " alphaTest=" + QString::number((flags & CinemaKernelM0::MATF_ALPHA_TEST) ? 1 : 0)
						+ " alphaBlend=" + QString::number((flags & CinemaKernelM0::MATF_ALPHA_BLEND) ? 1 : 0)
						+ " water=" + QString::number((flags & CinemaKernelM0::MATF_WATER) ? 1 : 0);
				}
				DEBUG(line);
				shown++;
			}
			if (shown == 0)
				DEBUG("T6 probe debug: sin hits (todo cielo o sonda vacia)");
		}

		// CAPAS N v2 (D4): registros de la SONDA (src=0), un string por objeto,
		// en un map ordenado por id -- se funde despues con los de AABB.
		std::map<IntType, QString> records;

		for (auto& kv : accum)
		{
			IntType objId = kv.first;
			ObjAccum& oa = kv.second;
			IntType px = (IntType)oa.dist.size();
			if (px == 0)
				continue;

			// sectionTag dominante (mas impactos); desempate deterministico:
			// el tag menor gana (recorrido de unordered_map -> orden estable
			// forzado comparando <, no solo >).
			IntType bestSec = 0, bestCount = -1;
			for (auto& sc : oa.secCounts)
				if (sc.second > bestCount || (sc.second == bestCount && sc.first < bestSec))
				{
					bestSec = sc.first;
					bestCount = sc.second;
				}

			std::vector<float> tmp;
			tmp = oa.dist; float dmin = CapasPercentile(tmp, 0.02f);
			tmp = oa.dist; float d10 = CapasPercentile(tmp, 0.10f);
			tmp = oa.dist; float d50 = CapasPercentile(tmp, 0.50f);
			tmp = oa.dist; float d90 = CapasPercentile(tmp, 0.90f);
			tmp = oa.dist; float dmax = CapasPercentile(tmp, 0.98f);

			records[objId] = QString::number(objId) + "," + QString::number(px) + ","
				+ QString::number(dmin, 'g', 6) + "," + QString::number(d10, 'g', 6) + ","
				+ QString::number(d50, 'g', 6) + "," + QString::number(d90, 'g', 6) + ","
				+ QString::number(dmax, 'g', 6) + "," + QString::number(bestSec) + ",0";
		}

		// CAPAS N v2 (D4, 16c.3): fallback por AABB MUNDO para los objetos SIN
		// NINGUN primer impacto (100% tapados o fuera de cuadro): sin esto el
		// planificador los perdia (sin registro = banda 0 = telon), y en capas
		// intermedias salian como MATTE abriendo agujeros (caso prop_d del
		// rechazo v1). Occlusion-agnostico, geometria pura: recorre los DRAW
		// RANGES del build (no la sonda) y expande una AABB por objeto sobre
		// TODOS sus rangos.
		{
			const std::vector<PTVert>& verts = CinemaSceneGetVerts();
			struct AabbAccum
			{
				float mn[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
				float mx[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
				IntType bestSec = 0, bestTriCount = -1;
				BoolType any = false;
			};
			std::map<IntType, AabbAccum> aabbAccum;

			if (!verts.empty())
			{
				for (const PTDrawRange& range : ranges)
				{
					if (range.drawIdx < 0 || (size_t)range.drawIdx >= draws.size())
						continue;
					const PTDraw& draw = draws[(size_t)range.drawIdx];
					IntType objId = draw.objectId;
					if (objId == 0)
						continue;
					if (records.find(objId) != records.end())
						continue; // ya tiene registro de sonda
					if (range.vertStart < 0 || range.vertCount <= 0 ||
						(size_t)(range.vertStart + range.vertCount) > verts.size())
						continue;

					AabbAccum& aa = aabbAccum[objId];
					for (IntType vi = range.vertStart; vi < range.vertStart + range.vertCount; vi++)
					{
						const PTVert& v = verts[(size_t)vi];
						if (v.px < aa.mn[0]) aa.mn[0] = v.px;
						if (v.py < aa.mn[1]) aa.mn[1] = v.py;
						if (v.pz < aa.mn[2]) aa.mn[2] = v.pz;
						if (v.px > aa.mx[0]) aa.mx[0] = v.px;
						if (v.py > aa.mx[1]) aa.mx[1] = v.py;
						if (v.pz > aa.mx[2]) aa.mx[2] = v.pz;
						aa.any = true;
					}
					// mismo desempate que el bestSec de la sonda: tag menor gana.
					if (range.triCount > aa.bestTriCount ||
						(range.triCount == aa.bestTriCount && draw.sectionTag < aa.bestSec))
					{
						aa.bestTriCount = range.triCount;
						aa.bestSec = draw.sectionTag;
					}
				}
			}

			for (auto& kv : aabbAccum)
			{
				IntType objId = kv.first;
				AabbAccum& aa = kv.second;
				if (!aa.any)
					continue;

				float cx = (aa.mn[0] + aa.mx[0]) * 0.5f;
				float cy = (aa.mn[1] + aa.mx[1]) * 0.5f;
				float cz = (aa.mn[2] + aa.mx[2]) * 0.5f;
				auto distTo = [&cam](float x, float y, float z) -> float
				{
					float dx = x - cam.pos[0], dy = y - cam.pos[1], dz = z - cam.pos[2];
					return std::sqrt(dx * dx + dy * dy + dz * dz);
				};

				float corner[8]; IntType ci = 0;
				for (IntType ix = 0; ix < 2; ix++)
					for (IntType iy = 0; iy < 2; iy++)
						for (IntType iz = 0; iz < 2; iz++)
						{
							float x = ix ? aa.mx[0] : aa.mn[0];
							float y = iy ? aa.mx[1] : aa.mn[1];
							float z = iz ? aa.mx[2] : aa.mn[2];
							corner[ci++] = distTo(x, y, z);
						}
				float dmin = corner[0], dmax = corner[0];
				for (IntType i = 1; i < 8; i++)
				{
					if (corner[i] < dmin) dmin = corner[i];
					if (corner[i] > dmax) dmax = corner[i];
				}
				float d02 = dmin, d98 = dmax;
				float d10 = dmin + 0.10f * (dmax - dmin);
				float d90 = dmin + 0.90f * (dmax - dmin);
				float d50 = distTo(cx, cy, cz);

				records[objId] = QString::number(objId) + ",0,"
					+ QString::number(d02, 'g', 6) + "," + QString::number(d10, 'g', 6) + ","
					+ QString::number(d50, 'g', 6) + "," + QString::number(d90, 'g', 6) + ","
					+ QString::number(d98, 'g', 6) + "," + QString::number(aa.bestSec) + ",1";
			}
		}

		QString out = "ok probe=" + QString::number(probeW) + "x" + QString::number(probeH)
			+ " rays=" + QString::number(rays)
			+ " hits=" + QString::number(hits)
			+ " sky=" + QString::number(sky)
			+ " near=" + QString::number(distMin, 'g', 6)
			+ " far=" + QString::number(distMax, 'g', 6)
			+ " objs=" + QString::number((IntType)records.size())
			+ " skip=" + QString::number(skipOverflow)     // V3: siempre 0, ver comentario arriba
			+ " sem=oracle_cameraray";                     // V3: traversal real de camara (Cambio 22)

		for (auto& kv : records)
			out += ";" + kv.second;

		return out;
	}

	// T6: expone la cache POR RAYO de la ultima CinemaSceneObjectDepths (para
	// pt_probe_agree en CinemaRender.cpp). Punteros validos solo hasta la
	// proxima CinemaSceneCaptureBegin/CinemaSceneObjectDepths.
	void CinemaSceneGetProbeCache(const std::vector<IntType>** objs,
		const std::vector<float>** dists, IntType* w, IntType* h)
	{
		if (objs) *objs = &capasProbeObj;
		if (dists) *dists = &capasProbeDist;
		if (w) *w = capasProbeW;
		if (h) *h = capasProbeH;
	}

	// CAPAS N v2 (D2, 16d): percentiles de profundidad POR BANDA sobre la
	// CACHE POR RAYO de la ultima CinemaSceneObjectDepths. Coste de trazado
	// CERO (solo reagrupa datos ya calculados): se puede llamar varias veces
	// en el bucle P7/P8 del planificador (mi3_layers_plan) sin pensarlo, pero
	// SIEMPRE dentro del mismo build que la sonda -- CinemaSceneCaptureBegin
	// invalida la cache en cuanto empieza uno nuevo.
	//
	// bandMap = "id:banda,id:banda,..." (separado por comas, ids ausentes ->
	// banda 0). skyDist = distancia a asignar a los rayos SIN impacto (pasar
	// el "far" del _depth.json: es la misma convencion que usa el mapa de
	// profundidad para el cielo).
	//
	// Formato de retorno (ok): "ok bands=<nbands>;<rec>;<rec>;..." con <rec> =
	// "band,px,d02,d10,d50,d80,d90,d98" (8 campos). Banda VACIA (sin rayos):
	// "band,0,0,0,0,0,0,0". nbands = 1 + banda maxima vista en bandMap
	// (siempre >= 1). Percentiles con CapasPercentile (nth_element,
	// determinista), misma convencion que CinemaSceneObjectDepths.
	//
	// Errores: "error:no_probe" (no hay cache -- CinemaSceneObjectDepths no se
	// llamo en este build), "error:bad_arg" (skyDist <= 0).
	QString CinemaSceneBandDepths(const QString& bandMap, float skyDist)
	{
		if (capasProbeObj.empty())
			return QString("error:no_probe");
		if (skyDist <= 0.f)
			return QString("error:bad_arg");

		// Parseo manual de "id:banda,id:banda,..." (evita cualquier duda de
		// version sobre QString::split / Qt::SkipEmptyParts).
		std::unordered_map<IntType, IntType> obj2band;
		IntType maxBand = 0;
		{
			IntType pos = 0, len = bandMap.length();
			while (pos < len)
			{
				IntType comma = (IntType)bandMap.indexOf(',', pos);
				QString token = (comma < 0) ? bandMap.mid(pos) : bandMap.mid(pos, comma - pos);
				pos = (comma < 0) ? len : comma + 1;
				if (token.isEmpty())
					continue;
				IntType colon = (IntType)token.indexOf(':');
				if (colon < 0)
					continue;
				bool ok1 = false, ok2 = false;
				IntType id = token.left(colon).toInt(&ok1);
				IntType band = token.mid(colon + 1).toInt(&ok2);
				if (!ok1 || !ok2)
					continue;
				obj2band[id] = band;
				if (band > maxBand)
					maxBand = band;
			}
		}

		std::map<IntType, std::vector<float>> bands;
		IntType n = (IntType)capasProbeObj.size();
		for (IntType i = 0; i < n; i++)
		{
			IntType obj = capasProbeObj[(size_t)i];
			float d = (obj < 0) ? skyDist : capasProbeDist[(size_t)i];
			IntType band = 0;
			if (obj >= 0)
			{
				auto it = obj2band.find(obj);
				if (it != obj2band.end())
					band = it->second;
			}
			bands[band].push_back(d);
		}

		IntType nbands = maxBand + 1;
		QString out = "ok bands=" + QString::number(nbands);
		for (IntType b = 0; b < nbands; b++)
		{
			auto it = bands.find(b);
			if (it == bands.end() || it->second.empty())
			{
				out += ";" + QString::number(b) + ",0,0,0,0,0,0,0";
				continue;
			}
			std::vector<float>& v = it->second;
			IntType px = (IntType)v.size();
			std::vector<float> tmp;
			tmp = v; float d02 = CapasPercentile(tmp, 0.02f);
			tmp = v; float d10 = CapasPercentile(tmp, 0.10f);
			tmp = v; float d50 = CapasPercentile(tmp, 0.50f);
			tmp = v; float d80 = CapasPercentile(tmp, 0.80f);
			tmp = v; float d90 = CapasPercentile(tmp, 0.90f);
			tmp = v; float d98 = CapasPercentile(tmp, 0.98f);
			out += ";" + QString::number(b) + "," + QString::number(px) + ","
				+ QString::number(d02, 'g', 6) + "," + QString::number(d10, 'g', 6) + ","
				+ QString::number(d50, 'g', 6) + "," + QString::number(d80, 'g', 6) + ","
				+ QString::number(d90, 'g', 6) + "," + QString::number(d98, 'g', 6);
		}
		return out;
	}

	// CAPAS SLICING: que OBJETOS son visibles en cada capa, sobre la cache POR
	// RAYO de la ultima CinemaSceneObjectDepths (capasProbeObj/capasProbeDist).
	// Coste de trazado CERO. cutsCsv = "s1,s2,...,s{N-1}" en unidades de MUNDO,
	// ASCENDENTE (los cortes de la planificacion). Capa K (0 = la mas lejana) =
	// ventana [t0_K, t1_K): K0=[s_{M},+inf), K en medio=[s_{M-K},s_{M-K+1}),
	// K=N-1=[0,s_1) con M = N-1 = numero de cortes. Para una distancia d, K se
	// obtiene contando cuantos cortes son <= d: K = M - count(cuts <= d).
	// Salida: "ok bands=N;K,objId,px;K,objId,px;..." ordenado por (K asc, px
	// desc, objId asc). Rayos de cielo -> capa 0 con objId -1 (no se emiten).
	// Errores: "error:no_probe", "error:bad_arg".
	QString CinemaSceneObjectBands(const QString& cutsCsv)
	{
		if (capasProbeObj.empty())
			return QString("error:no_probe");

		// Parseo manual de "s1,s2,...,s{M}" (mismo estilo que CinemaSceneBandDepths).
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
		IntType nbands = M + 1;

		// (K, objId) -> px, en un map ordenado (K, objId) para la salida
		// determinista; se reordena por px DESC dentro de cada K al emitir.
		std::map<std::pair<IntType, IntType>, IntType> counts;
		IntType n = (IntType)capasProbeObj.size();
		for (IntType i = 0; i < n; i++)
		{
			IntType obj = capasProbeObj[(size_t)i];
			if (obj < 0)
				continue;   // cielo: capa 0, pero NO se emite (contrato)
			float d = capasProbeDist[(size_t)i];
			IntType cnt = 0;
			for (IntType c = 0; c < M; c++)
				if (cuts[(size_t)c] <= d)
					cnt++;
			IntType band = M - cnt;
			counts[{band, obj}]++;
		}

		// Agrupar por banda, ordenar cada grupo por (px DESC, objId ASC).
		std::map<IntType, std::vector<std::pair<IntType, IntType>>> byBand; // K -> [(objId,px),...]
		for (auto& kv : counts)
			byBand[kv.first.first].push_back({ kv.first.second, kv.second });
		for (auto& kv : byBand)
			std::sort(kv.second.begin(), kv.second.end(), [](const std::pair<IntType, IntType>& a, const std::pair<IntType, IntType>& b)
			{
				if (a.second != b.second) return a.second > b.second; // px desc
				return a.first < b.first;                              // objId asc
			});

		QString out = "ok bands=" + QString::number(nbands);
		for (IntType b = 0; b < nbands; b++)
		{
			auto it = byBand.find(b);
			if (it == byBand.end())
				continue;
			for (auto& rec : it->second)
				out += ";" + QString::number(b) + "," + QString::number(rec.first) + "," + QString::number(rec.second);
		}
		return out;
	}

	// M8-B: clasifica el frame RECIEN capturado (scene.draws frescos) contra el
	// snapshot del ultimo build instanciado. NO re-aplana: usa el matching por
	// ORDINAL de draw (el Paso 0 midio que el vbufferId rota, pero el walk es
	// determinista) y verifica solo campos ESTABLES (nº de draws, topologia
	// seccion+cull, seccion por instancia). Resultado:
	//   REBUILD  -> topologia distinta / no instanciado / sin snapshot: el caller
	//               hace pt_build completo (fallback seguro para todo lo no-transform).
	//   STATIC   -> nada cambio (ni transforms ni luces): no hay trabajo GPU.
	//   TRANSFORM-> solo cambiaron matrices y/o luces: outWorlds trae el world nuevo
	//               por instancia y outEmiO2W el o2w nuevo por emisor; el caller
	//               reescribe las instancias + TLAS + gEmiO2W + gLights (µs).
	// Deja el estado en "built" (el GPU del ultimo build sigue valido) y marca
	// worldStale si hubo cambio (los verts MUNDO del CPU quedan desfasados).
	CinemaLiveClass CinemaSceneClassifyLive(std::vector<CinemaInstXform>& outWorlds,
		std::vector<CinemaInstXform>& outEmiO2W, std::vector<int>& outChangedDraws)
	{
		outWorlds.clear();
		outEmiO2W.clear();
		outChangedDraws.clear();
		if (!scene.instanced)
			return CINEMA_LIVE_REBUILD;
		// El camino vivo solo tras una re-captura valida (draws frescos).
		if (scene.state != "captured" && scene.state != "built")
			return CINEMA_LIVE_REBUILD;

		const std::vector<PTDraw>& draws = scene.draws;      // FRESCOS (re-captura)
		const std::vector<PTDrawRange>& ranges = scene.ranges; // del ultimo build
		const size_t N = ranges.size();
		if (N == 0 || scene.liveInstWorld.size() != N || scene.liveInstSection.size() != N)
			return CINEMA_LIVE_REBUILD;

		// Topologia: mismo nº de draws + misma firma estable (seccion+cull).
		if (draws.size() != scene.liveDrawCount)
			return CINEMA_LIVE_REBUILD;
		if (CinemaDrawTopologyHash(draws) != scene.liveStructHash)
			return CINEMA_LIVE_REBUILD;

		// Por instancia: seccion estable + world nuevo por ordinal (drawIdx).
		outWorlds.resize(N);
		BoolType anyChanged = false;
		for (size_t i = 0; i < N; i++)
		{
			size_t d = (size_t)ranges[i].drawIdx;
			if (d >= draws.size())
				return CINEMA_LIVE_REBUILD;
			if ((IntType)draws[d].sectionTag != scene.liveInstSection[i])
				return CINEMA_LIVE_REBUILD;
			CinemaInstXform w = CinemaMatrixToInstXform(draws[d].world);
			outWorlds[i] = w;
			if (memcmp(w.m, scene.liveInstWorld[i].m, sizeof(w.m)) != 0)
				anyChanged = true;
			// TRIM: el ORIGEN del espacio CUERPO NO esta en drawSig (a
			// proposito: es posicion de mundo y cambia cada frame en cuanto el
			// personaje camina, asi que meterlo alli seria un rebuild por
			// frame). Pero SI tiene que llegar a la GPU, o el patron se
			// desliza sobre el cuerpo. En v1 un origen movido fuerza REBUILD.
			// DEUDA DECLARADA, con su diseno ya escrito: lo correcto es
			// refrescar trimOrigin en scene.materials y RE-SUBIR gMats por el
			// camino vivo -- 128 B x numDraws, 64 KB con 500 draws, el mismo
			// patron con el que ya se re-sube gLights cuando una luz se mueve.
			// No entra en esta ronda porque el buffer de materiales se crea
			// inmutable y cambiar eso toca los DOS backends a la vez que se
			// introduce el trim: las dos cosas quedarian sin demostrar.
			if (i < scene.materials.size() && (scene.materials[i].trimPack & 255u) != 0u)
			{
				const float* mo = scene.materials[i].trimOrigin;
				const float* dof = draws[d].trimOrigin;
				if (mo[0] != dof[0] || mo[1] != dof[1] || mo[2] != dof[2])
					return CINEMA_LIVE_REBUILD;
			}
		}

		// M8-C (C1): fingerprint de GEOMETRIA por instancia. El BEND deforma los
		// verts del vbuffer (mismo objeto, misma matriz o distinta, geometria LOCAL
		// distinta): M8-B lo veia transform-only y dejaba la malla obsoleta. Aqui se
		// compara la firma de contenido por ordinal:
		//   - count IGUAL (mismo nº verts/tris) -> re-flatten LOCAL de ese draw
		//     (posiciones deformadas) + rebuild PARCIAL de SU BLAS (sub-ms/parte).
		//   - count DISTINTO (p.ej. cruzar rest<->bent cambia el nº de segmentos) ->
		//     el layout de buffers se desplaza -> REBUILD total (fallback seguro).
		if (scene.liveInstGeoFp.size() != N)
			return CINEMA_LIVE_REBUILD; // snapshot incompleto -> rebuild seguro
		std::vector<uint64_t> freshGeoFp(N);
		for (size_t i = 0; i < N; i++)
		{
			size_t d = (size_t)ranges[i].drawIdx;
			IntType fv = 0, ft = 0;
			uint64_t fp = CinemaDrawGeoFingerprint(draws[d], &fv, &ft);
			freshGeoFp[i] = fp;
			if (fp != scene.liveInstGeoFp[i])
			{
				if (fv != ranges[i].vertCount || ft != ranges[i].triCount)
					return CINEMA_LIVE_REBUILD; // topologia del draw cambio
				outChangedDraws.push_back((int)i);
			}
		}

		// Luces: mismo conteo (add/baja cambia ds_list_size -> geomgen -> rebuild;
		// un keyframe de hide de una luz cambia el conteo emitido -> rebuild). Una
		// luz MOVIDA (mismo conteo, contenido distinto) = camino vivo (re-sube gLights).
		if (scene.lights.size() != scene.liveLightCount)
			return CINEMA_LIVE_REBUILD;
		if (CinemaLightsHash(scene.lights) != scene.liveLightHash)
			anyChanged = true;

		// C1: re-flatten LOCAL de los draws con geometria cambiada (bend). Si alguno
		// falla (buffer irresoluble / count discrepante) -> rebuild seguro.
		for (size_t k = 0; k < outChangedDraws.size(); k++)
		{
			if (!ReflattenDrawLocal((size_t)outChangedDraws[k]))
			{
				outChangedDraws.clear();
				return CINEMA_LIVE_REBUILD;
			}
		}

		// gEmiO2W nuevo (paralelo a emiO2W): o2w = world de la instancia duena. Se
		// refresca SIEMPRE (transform y geometria) para que el muestreo NEE apunte al
		// emisor en su pose actual.
		// C4 (emisivos dinamicos): la CDF de potencia (emiCdf/emiTriIdx) NO se recomputa
		// en el camino vivo: el AREA MUNDO de cada tri emisor queda CONGELADA al valor
		// del build (aproximacion documentada, diseno 4.2b). Solo afecta la VARIANZA del
		// muestreo (reparto), NUNCA la ENERGIA de la imagen (esa la pone EvalSurface por
		// texel). Un cambio del CONJUNTO de emisores (alta/baja) pasa por history_amount/
		// ds_list_size -> rebuild completo, que SI recomputa la CDF. Congelar el area de
		// un emisor escalado o doblado (bend) sube un poco el ruido de esa fuente, sin
		// sesgo (converge al mismo resultado).
		outEmiO2W.resize(scene.liveEmiInstance.size());
		for (size_t s = 0; s < scene.liveEmiInstance.size(); s++)
		{
			uint32_t inst = scene.liveEmiInstance[s];
			outEmiO2W[s] = (inst < N) ? outWorlds[inst] : outWorlds[0];
		}

		// Commit del snapshot (worlds/luces/geometria del frame nuevo) y estado.
		scene.liveInstWorld = outWorlds;
		scene.liveInstGeoFp = freshGeoFp;
		scene.liveLightHash = CinemaLightsHash(scene.lights);
		scene.state = "built"; // el GPU del ultimo build sigue valido
		if (anyChanged || !outChangedDraws.empty())
			scene.worldStale = true;
		if (!outChangedDraws.empty())
			return CINEMA_LIVE_GEOMETRY; // el caller hace update parcial de verts+BLAS+TLAS
		return anyChanged ? CINEMA_LIVE_TRANSFORM : CINEMA_LIVE_STATIC;
	}

	const QVector<QImage>& CinemaSceneGetAtlasSlices()
	{
		return scene.slices;
	}

#if API_D3D11
	const CinemaSceneGpu& CinemaSceneGetGpu()
	{
		return scene.gpu;
	}
#endif
}
