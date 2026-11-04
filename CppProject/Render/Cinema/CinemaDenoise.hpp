#pragma once
#include "Common.hpp"

// MI3 Cinema: denoise del beauty LINEAL (CINEMA_DESIGN.md seccion 9).
//
// Dos motores tras una API comun:
//   1. Intel Open Image Denoise 2.x, device CPU. Carga 100 por ciento
//      dinamica (QLibrary): el motor NUNCA depende de OIDN para compilar
//      ni arrancar. DLLs esperadas JUNTO AL EXE: OpenImageDenoise.dll
//      (+ OpenImageDenoise_core.dll, OpenImageDenoise_device_cpu.dll,
//      tbb12.dll, que resuelven solas por el orden de busqueda estandar
//      si estan al lado). Receta canonica del diseno: prefilter de albedo
//      y de normal (filtros RT dedicados in-place) y filtro principal
//      color+albedo+normal con hdr=true y cleanAux=true.
//   2. Fallback propio A-TROUS edge-aware (wavelet B3 5x5, 4 iteraciones,
//      pesos por luminancia comprimida / normal / profundidad relativa /
//      albedo, OpenMP por filas) para maquinas sin las DLLs de OIDN.
//
// Orden inviolable del pipeline (seccion 9): acumulacion lineal ->
// DENOISE (lineal) -> fog post -> exposure -> AgX -> sRGB. Nunca
// denoise post-tonemap. El caller (CinemaRenderFinish) pasa la
// radiancia lineal MEDIA (accum.rgb / peso), no el acumulador crudo.
//
// Politica de errores (misma que el resto del modulo Cinema): nunca
// crashear. DLL o simbolo ausente -> WARNING UNA sola vez por sesion y
// degradacion silenciosa al fallback; fallo total -> out = copia del
// color de entrada.

namespace CppProject
{
	// Denoise de un frame. Todas las imagenes son buffers RGBA float
	// planos de w*h*4 (pix = y * w + x, mismo layout que el readback de
	// CinemaRender); internamente se re-empaquetan a float3 para OIDN.
	//   colorRgba       rgb = radiancia lineal MEDIA del pixel; w = lo que
	//                   sea (cobertura): se copia TAL CUAL a outRgba.
	//   albedoRgba      rgb = albedo medio del primer hit (AOV u1 / spp),
	//                   rango esperado [0,1]. Puede ser nullptr: el
	//                   denoise pierde la guia (OIDN cae a filtro
	//                   color-only, a-trous ignora el peso de albedo).
	//   normalDepthRgba xyz = normal media del primer hit (con normal map,
	//                   AOV u2 / spp; unitaria o casi: se re-normaliza),
	//                   w = profundidad media t. Puede ser nullptr.
	//   outRgba         salida w*h*4: rgb denoised, w copiado de colorRgba.
	//                   Puede apuntar al MISMO buffer que colorRgba
	//                   (ambos motores trabajan sobre copias internas).
	//   preferOidn      true = intentar OIDN y degradar a a-trous si no
	//                   esta; false = a-trous directo (modo forzado).
	// Devuelve el motor usado: "oidn" | "atrous" | "" (fallo total; en ese
	// caso outRgba = colorRgba copiado y se emitio WARNING).
	QString CinemaDenoiseRun(IntType w, IntType h,
		const float* colorRgba, const float* albedoRgba,
		const float* normalDepthRgba, float* outRgba, BoolType preferOidn);

	// R9 (FIX 3): true si el device OIDN activo (o el que se auto-seleccionaria) es
	// CUDA (GPU, denoise de decenas de ms). El preview lo consulta para el
	// checkpoint denoisado a ~32 samples: en CPU (lag de segundos) no se hace.
	// Inicializa el device OIDN la primera vez (idempotente). false si CPU/sin OIDN.
	BoolType CinemaDenoiseIsCudaAvailable();
}
