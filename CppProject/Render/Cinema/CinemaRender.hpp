#pragma once
#include "Common.hpp"

// MI3 Cinema: host del path tracer (estado del render, dispatch del kernel
// M0, resolve CPU y upload al surface). Contratos: CINEMA_DESIGN.md
// secciones 2, 6, 10 y 11; kernel y cbuffer PTFrame en CinemaKernelM0.hpp;
// escena/camara/entorno en CinemaScene.hpp; helpers GPU en CinemaGpu.hpp.
//
// Flujo (lo orquesta render_cinema() en GML via los natives pt_render_*):
//   pt_build (CinemaScene)
//   -> CinemaRenderBegin(w, h, seed)      limpia acumulador + AOVs, spp = 0
//   -> CinemaRenderSample(...) x N        1 spp por llamada, tiles anti-TDR
//   -> CinemaRenderFinish(surf, exposure, tonemap, denoiseMode, saturation)
//        readback + denoise (CinemaDenoise, M3) + resolve + upload
// CinemaRenderFinish NO cierra el render: se puede seguir muestreando y
// re-resolver (preview progresivo, seccion 11.1 del diseno).
//
// Politica de errores (misma que CinemaScene/CinemaGpu): nunca crashear.
// Todo fallo emite WARNING y devuelve un codigo de error estable, tambien
// consultable en CinemaRenderStatus().

namespace CppProject
{
	// DIAGNOSTICO (gate G4): con false, el proximo render NO usa la CDF de
	// triangulos emisivos (numEmiTris = 0 en el cbuffer). La emision sigue
	// entrando por hits de BSDF con peso 1, asi que es el MISMO integrando
	// estimado de otra forma: converge al mismo valor con mas varianza.
	void CinemaRenderSetEmiCdf(BoolType on);

	// (Re)inicia el render: compila los kernels CSPathTrace/CSClear la
	// primera vez (lazy, desde CinemaKernelM0::SOURCE), (re)crea el
	// acumulador y los dos AOVs si cambia la resolucion (buffers
	// estructurados float4 de w*h, ver CinemaKernelM0.hpp), limpia los tres
	// UAVs con CSClear y resetea sampleIdx. Requiere escena construida
	// (CinemaSceneBuild OK). Devuelve "" si OK o un codigo estable:
	// "cinema_requires_d3d11", "cinema_requires_fl11", "invalid_size",
	// "scene_not_built", "kernel_compile_failed", "gpu_alloc_failed".
	QString CinemaRenderBegin(IntType w, IntType h, IntType seed);

	// R9 (FIX 1/5): backend de trazado que elegira el proximo Begin (DXR si el
	// device esta disponible y no se forzo dx11 por env, si no D3D11). Prediccion
	// estable usada por (a) el defer del BVH del preview y (b) el presupuesto de
	// batching del export (native pt_backend). Devuelve true = DXR, false = D3D11.
	BoolType CinemaRenderWantsDxr();

	// Acumula sppCount samples por pixel (batching multi-spp de la ronda 7):
	// llena el cbuffer PTFrame (240 B) desde CinemaScene y despacha CSPathTrace
	// en tiles adaptativos (~1 Mpix) con Flush por presupuesto de pixeles
	// anti-TDR, repitiendo el frame sppCount veces y avanzando tile.z = sampleIdx
	// como sin batch (resultado BIT-IDENTICO a sppCount llamadas de 1 spp: mismo
	// RNG, mismo orden de acumulacion). sppCount<1 se trata como 1. bounces se
	// clampa a 1..8 (M0: 3); clampIndirect 0 = off; emissiveScale escala la
	// radiancia de emisivos; alphaFilm activa el film transparente
	// (FRAMEF_ALPHA_FILM). Overrides de perfilado por env MI3_CINEMA_TILE /
	// MI3_CINEMA_FLUSHMPX (no alteran el resultado). Codigos: "render_not_begun",
	// "scene_not_built", "camera_not_set".
	// noiseThreshold (M7 adaptive): 0 = OFF (camino bit-identico; sin early-out ni
	// marca de convergencia, el resolve divide por el spp global). > 0 activa el
	// muestreo guiado por varianza (Welford por pixel en gVarCount, piso de 32
	// samples, chequeo cada 32): los pixeles convergidos se saltan y el resolve
	// pasa a dividir por el CONTEO POR PIXEL. El oraculo/paridad SIEMPRE llama con
	// 0 (no se toca). Recomendado 0.02 con denoise, 0.01 sin denoise.
	QString CinemaRenderSample(IntType bounces, RealType clampIndirect,
		RealType emissiveScale, BoolType alphaFilm, IntType sppCount = 1, RealType noiseThreshold = 0.0);

	// Readback del acumulador + AOVs (staging de buffer), denoise opcional
	// (M3) y resolve CPU:
	// color = acum.rgb / max(peso, 1) (radiancia lineal MEDIA)
	//   -> denoiseMode: 0 = off (byte a byte identico al camino M0-M2,
	//      goldens y paridad intactos); 1 = auto (OIDN CPU dinamico via
	//      CinemaDenoise y, si falta la DLL o falla, degradacion silenciosa
	//      al a-trous propio); 2 = a-trous forzado. El denoise opera sobre
	//      la radiancia lineal media guiado por el albedo medio (AOV u1) y
	//      la normal mapeada + depth medios (AOV u2), SIEMPRE antes del
	//      tonemap (orden inviolable, diseno seccion 9).
	//   -> R12: ya NO hay fog post. La niebla atmosferica se resuelve EN EL
	//      KERNEL sobre el hit primario real (formula exacta del raster,
	//      shader_high_fog.fsh + su apply bicolor) y llega aqui ya dentro de
	//      la radiancia acumulada; el denoise la ve como parte de la imagen.
	//   -> * exposure (multiplicador LINEAL puro pre-tonemap, sin EV ni
	//      ancla; a diferencia del raster, el camino AgX NO lleva el x1.7
	//      porque aqui la radiancia es fisica; el nivel de escena se
	//      calibra con CINEMA_EXPOSURE_BASE en el .cpp).
	// tonemap (look de color): 0 = encode sRGB exacto (camino M0/M1,
	// goldens/paridad); 1 = AgX look Base (matrices identicas al raster
	// next-gen, shader_high_samples_unpack.fsh); 2 = AgX look Punchy (Base
	// + agxLookPunchy pow 1.3 / sat 1.3 ENTRE sigmoide y outset, igual que
	// el shader con uTonemapper == 4: colores vibrantes sin quemar); 3 =
	// sRGB con hombro suave (hombro exponencial C1 por canal, rodilla en
	// 0.5 lineal: identidad exacta en los medios [0, 0.5], rolloff
	// monotono y acotado por 1 hacia blanco en las altas -- blanco
	// efectivo ~x = 4; ver ShoulderCurve en el .cpp); 4 = hombro
	// preservando color (la MISMA ShoulderCurve del 3 pero aplicada sobre
	// la luma Rec.709: el RGB se escala por t/luma, conservando el ratio
	// de color exacto al rodar -- los highlights saturados NO desaturan a
	// blanco cremoso como en el 3 --, y un soft-clip exponencial C1 por
	// canal, identidad hasta 0.98 y asintotico a 1, recorta el exceso
	// extremo de los canales > 1 antes del encode; ver SoftClipChannel en
	// el .cpp). Los caminos AgX salen display 2.2 y se cuantizan directos;
	// los sRGB pasan por el OETF. Cualquier tonemap desconocido degrada a
	// sRGB simple.
	//   -> saturation (default 1.0): saturacion clasica POST-tonemap en
	//      espacio display, c' = luma + (c - luma) * saturation con luma
	//      Rec.709 y clamp [0,1]; comun a los cinco tonemaps. 1.0 es
	//      identidad byte a byte (goldens intactos). No toca el alpha.
	// En todos los caminos el tonemap toca SOLO el RGB: alpha = cobertura
	// media straight (film transparente intacto; el denoise ni el fog
	// tampoco tocan el alpha). Subida al surface por el camino
	// UpdateSubresource verificado de CinemaFinishTest.
	// Codigos: "no_samples", "invalid_surface", "surface_size_mismatch",
	// "readback_failed", "upload_failed".
	QString CinemaRenderFinish(IntType surfId, RealType exposure, IntType tonemap = 0,
		IntType denoiseMode = 0, RealType saturation = 1.0);

	// F2/R17: guarda <filename> con el mapa de profundidad del ultimo render
	// RESUELTO (pt_render_finish ya corrio). PNG 8-bit gray, 255=cerca, 0=lejos
	// (cielo=0), lineal en distancia de mundo entre near/far del frame. Hace su
	// PROPIO readback de los AOV (accum=nullptr): no toca el acumulador ni el
	// resolve -> el beauty queda bit-identico con el pase activado o no. Debe
	// llamarse entre el pt_render_finish final y el siguiente pt_render_begin
	// (los UAV siguen vivos). Devuelve el JSON del sidecar (near/far/etc) o
	// "error:<motivo>" ("no_samples", "readback_failed", "save_failed").
	QString CinemaRenderSaveDepth(const QString& filename);

	// CAPAS SLICING (S2): histograma del ULTIMO mapa de profundidad guardado, en
	// DISPARIDAD normalizada u = (1/d - 1/far)/(1/near - 1/far), bins uniformes
	// en [0,1] (bin i = u en [i/B,(i+1)/B); u=1 cae en el ultimo bin). El CIELO
	// se cuenta aparte (nunca entra en el histograma: pertenece al telon por
	// definicion y sesgaria el primer bin).
	// Salida: "ok w=.. h=.. near=.. far=.. hits=.. sky=.. bins=B;c0,c1,...,c{B-1}"
	// Errores: "error:no_depth" (no hay mapa cacheado), "error:bad_arg" (B fuera
	// de [16,1024] o near/far invalidos).
	QString CinemaRenderDepthHist(IntType bins);

	// CAPAS SLICING (S3): estadisticas por CAPA sobre el MISMO mapa cacheado.
	// cutsCsv = "s1,s2,...,s{N-1}" en unidades de MUNDO, ASCENDENTE (los cortes
	// de la planificacion). Capa K (0 = la mas lejana) = ventana [t0_K, t1_K).
	// Salida: "ok bands=N;<rec>;..." con <rec> =
	//   "K,px,sky,d02,d50,d80,d98"   (px incluye el cielo de la capa 0;
	//    los percentiles se calculan SOLO sobre pixeles con d > 0)
	// Capa vacia: "K,0,0,0,0,0,0". Errores: "error:no_depth", "error:bad_arg"
	// (cortes no ascendentes, N fuera de [1,12], o algun corte <= 0).
	QString CinemaRenderDepthBands(const QString& cutsCsv);

	// T6: concordancia POR RAYO entre la sonda de objetos (CPU) y el mapa de
	// profundidad cacheado (GPU, full-res). Salida: "ok agree=<frac>
	// med_ratio=<r> near_ratio=<r> co=<n> mapped=<n> sky_probe=<n> sky_map=<n>".
	// Errores: "error:no_depth", "error:no_probe".
	QString CinemaRenderProbeAgree();

	// V1(b)+V2: estadisticas por capa sobre los PNG ENTREGADOS y el mapa de
	// profundidad cacheado -- la UNICA medida de "lo entregado de verdad" (la
	// consumen la red de seguridad post-render SS3.4 y el manifest SS4.2, asi
	// que no pueden discrepar). Salida: "ok n=<N> w=<W> h=<H> geom=<n> sky=<n>"
	// y por capa ";<K>,<alpha_px>,<top_px>,<top_px_geom>,<top_sky_px>,<d02>,
	// <d50>,<d80>,<d98>". Errores: "error:no_depth", "error:bad_arg",
	// "error:no_file:<K>", "error:dims:<K>".
	QString CinemaRenderLayersTopStats(const QString& base, IntType n);

	// DOF-FIEL D1: cadena "clave=valor" del ULTIMO RunAutofocusProbe (el que
	// corrio en el CinemaRenderBegin mas reciente con DoF+autofocus activos).
	// Claves: mode (region|frame|aabb|off), focus, aperture, region_hits,
	// frame_hits, planar (siempre 1: A2 mide en distancia PLANAR). mode vacio
	// si nunca corrio un autofocus (DoF off o foco manual).
	QString CinemaRenderDofInfo();

	// Estado en pares "clave=valor" separados por espacio (mismo formato que
	// CinemaSceneStatus). Claves: state (idle|ready|sampling|finished|error),
	// error, spp, width, height, seed, ms_sample (wall del ultimo sample),
	// ms_finish (wall del ultimo resolve+upload), denoiser (oidn|atrous|off,
	// el motor REALMENTE usado en el ultimo Finish), ms_denoise (wall del
	// denoise, 0 si off), tonemap y saturation (los del ultimo Finish).
	QString CinemaRenderStatus();

	// R10 (FIX 1/2): ms (wall) del ultimo Begin (reset de camara) y del ultimo
	// Sample. El preview interactivo los lee (pt_ms_begin/pt_ms_sample) para
	// verificar el reset BARATO y para la resolucion dinamica adaptativa (escala
	// el pixel budget del frame en movimiento al ms/megapixel medido). En DXR el
	// Sample drena la GPU (ExecuteAndWait) -> el ms es tiempo de GPU real.
	RealType CinemaRenderMsBegin();
	RealType CinemaRenderMsSample();
	RealType CinemaRenderMsFinish();

	// M8-B ("escena viva"): update TRANSFORM-ONLY del frame recien capturado.
	// "rebuild" | "static" | "transform" (ver definicion). CinemaRenderMsUpdate =
	// ms del ultimo update GPU (gate de coste).
	QString CinemaRenderSceneUpdate();
	RealType CinemaRenderMsUpdate();

	// Rellena el surface con un gradiente determinista (rojo = x, verde = y,
	// azul = damero 32px, alpha = 1) por el MISMO camino de upload que
	// CinemaRenderFinish y lo AUTOVERIFICA con readback. Devuelve false (con
	// WARNING) si el surface no existe o el upload no es posible.
	BoolType CinemaFinishTest(IntType surfId);

	// Sonda de diagnostico: pixel (10,10) como "r,g,b" + log con etiqueta.
	QString CinemaProbe(IntType surfId, const QString& tag);

	// Paridad GPU vs oraculo CPU: "rmse=X ok=0|1" o codigo de error.
	QString CinemaRenderParity(IntType w, IntType h, IntType spp, IntType seed,
		IntType bounces, RealType clampIndirect, RealType emissiveScale, RealType rmseThreshold);

	// M7 adaptive: numero de pixeles NO convergidos del render en curso (readback
	// del buffer varCount). -1 si no hay render adaptive. Lo sondea render_cinema
	// para la terminacion temprana global (0 => todos convergidos).
	IntType CinemaRenderActiveCount();

	// M7 adaptive: vuelca el conteo por pixel de samples como heatmap jet al
	// surface (evidencia). maxCount <= 0 usa el spp maximo. Devuelve "mean_spp=X"
	// o un codigo de error. Llamar tras el render, antes del siguiente Begin.
	QString CinemaRenderHeatmap(IntType surfId, RealType maxCount);
}
