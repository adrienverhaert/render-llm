#pragma once
#include "Common.hpp"
#include "CinemaScene.hpp" // CinemaInstXform (update en vivo M8-B)
#include <vector>

// ---------------------------------------------------------------------------
// CinemaGpu12: backend D3D12 STANDALONE del path tracer Cinema (M6, fase A).
//
// MI3 es y sigue siendo D3D11. DXR exige D3D12, asi que el trazador D3D12 vive
// AISLADO (CERO interop D3D11<->12): re-sube la escena desde los datos CPU que
// CinemaScene ya retiene (verts/tris/nodos/materiales/atlas/luces/CDF), compila
// el MISMO kernel HLSL de CinemaKernelM0.hpp a DXIL con dxc (SM 6.5), despacha
// el compute en tiles (bit-identico al particionado D3D11) y hace readback del
// acumulador + AOVs. El resolve CPU y el oraculo NO se tocan: leen los mismos
// float4 premultiplicados que produce el backend D3D11.
//
// Fase A: traversal BVH POR SOFTWARE (el kernel tal cual, sin RayQuery). Fase B
// sustituira TraceClosest/TraceShadow por RayQuery inline + TLAS.
//
// Politica de errores (misma que CinemaGpu/CinemaScene/CinemaRender): NUNCA
// crashear. Todo fallo emite WARNING, libera lo suyo y devuelve false; el caller
// (CinemaRender) cae limpio al backend D3D11. dxc/dxil ausentes -> no disponible.
//
// dxc/dxil se cargan dinamicamente (patron OIDN de CinemaDenoise): sin ellas el
// backend queda no disponible. dxil.dll DEBE viajar junto a dxcompiler.dll o el
// DXIL sale sin firmar y CreateComputePipelineState lo rechaza.
// ---------------------------------------------------------------------------

namespace CppProject
{
	// Disponibilidad del backend D3D12/DXR (init perezoso y cacheado por sesion:
	// crea device con tier DXR >= 1.1 via adaptador HIGH_PERFORMANCE, carga dxc,
	// compila los dos PSOs y arma root signature + heaps). whyOut (opcional)
	// recibe un motivo corto cuando NO esta disponible, para el log de backend.
	BoolType CinemaGpu12Available(QString* whyOut = nullptr);

	// Linea legible del adaptador/backend elegido (para el log "que backend corre").
	QString CinemaGpu12BackendInfo();

	// Selftest patron (como CinemaSelftest): compila un kernel cs_6_5 trivial con
	// dxc, lo despacha sobre un RWStructuredBuffer<float4>, lee de vuelta y verifica
	// contra una referencia CPU. Prueba el camino dxc->DXIL firmado->PSO->dispatch->
	// readback de punta a punta. Nunca crashea.
	BoolType CinemaGpu12Selftest(QString* errorOut = nullptr);

	// Sube la escena actual de CinemaScene al device D3D12, reconstruyendo SOLO si
	// la escena cambio desde el ultimo upload (firma por punteros/conteos). Debe
	// haber un build valido (CinemaSceneGetGpu().IsValid()).
	BoolType CinemaGpu12EnsureScene();

	// M8-B ("escena viva"): update TRANSFORM-ONLY de la escena ya subida. worlds =
	// object->world nuevo por instancia (paralelo a CinemaSceneGetDrawRanges);
	// emiO2W = o2w nuevo por emisor de la CDF emisiva local. Reescribe las
	// INSTANCE_DESC.Transform + reconstruye la TLAS + re-sube gEmiO2W/gLights, SIN
	// tocar BLAS/verts. Requiere un build instanciado previo (g_instDescCpu). false
	// si no aplica (backend no DXR, escena no instanciada, conteo distinto).
	BoolType CinemaGpu12UpdateInstances(const std::vector<CinemaInstXform>& worlds,
		const std::vector<CinemaInstXform>& emiO2W);

	// M8-C ("escena viva", C1): update PARCIAL de geometria (bend). changedDraws =
	// indices de instancia cuya geometria LOCAL se re-aplano (CinemaScene.vertsLocal,
	// mismo count). Reescribe SOLO esos rangos de g_verts, reconstruye SOLO sus BLAS
	// en su mismo offset y luego la TLAS con las transforms nuevas (+ gEmiO2W/gLights).
	// Bit-identico a un build frio del mismo frame; false -> el caller hace rebuild.
	BoolType CinemaGpu12UpdateGeometry(const std::vector<CinemaInstXform>& worlds,
		const std::vector<CinemaInstXform>& emiO2W, const std::vector<int>& changedDraws);

	// Ms (wall, con drain de GPU) del ultimo CinemaGpu12UpdateInstances/UpdateGeometry.
	double CinemaGpu12LastUpdateMs();

	// (Re)crea el acumulador + 2 AOVs (RWStructuredBuffer<float4> de w*h) y los
	// limpia con CSClear. Requiere EnsureScene previo.
	BoolType CinemaGpu12Begin(IntType w, IntType h);

	// Ejecuta sppCount pasadas COMPLETAS del frame en tiles. frame256 = el PTFrame
	// de 256 B ya rellenado por CinemaRender::FillFrame; tile.xy (offset absoluto)
	// y tile.z (sampleIdxBase + s) se re-escriben por tile/sample EXACTAMENTE como
	// el bucle D3D11 -> particion bit-identica. Barreras UAV entre samples (RMW
	// sobre los mismos pixeles); tiles intra-sample disjuntos, sin barrera.
	// flushBudgetPix acota el trabajo encolado por submission (anti-TDR).
	BoolType CinemaGpu12Sample(const void* frame256, IntType sampleIdxBase,
		IntType sppCount, IntType tileSize, IntType flushBudgetPix);

	// Readback del acumulador + AOVs + varCount (M7). Cada puntero es opcional
	// (nullptr para omitir ese plano); arrays float del caller de w*h*4 cada uno.
	BoolType CinemaGpu12Readback(float* accum, float* aovAlbedo, float* aovNormalDepth, float* varCount = nullptr);

	// Libera acumulador/AOVs/readback (targets por resolucion). El device, dxc y
	// PSOs se conservan para el siguiente render.
	void CinemaGpu12ReleaseTargets();
}
