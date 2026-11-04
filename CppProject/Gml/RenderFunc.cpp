#include "Generated/GmlFunc.hpp"

#include "AppHandler.hpp"
#include "AppWindow.hpp"
#include "Asset/Buffer.hpp"
#include "Asset/Shader.hpp"
#include "Asset/Surface.hpp"
#include "Asset/VertexBuffer.hpp"
#include "Render/GLWidget.hpp"
#include "Render/GraphicsApiHandler.hpp"
#include "Render/PrimitiveRenderer.hpp"
#include "Render/VertexBufferRenderer.hpp"
#include "Render/Cinema/CinemaGpu.hpp"
#include "Render/Cinema/CinemaBvh.hpp"
#include "Render/Cinema/CinemaRender.hpp"
#include "Render/Cinema/CinemaScene.hpp"
#include "Render/Cinema/CinemaDenoise.hpp"
#include "Render/Mi3WhiteDetails.hpp"

#include <algorithm>
#include <vector>

#if API_D3D11
#include "renderdoc_app.h" // DEV: captura RenderDoc del render offscreen
#endif

namespace CppProject
{
	IntType gpu_get_blendmode()
	{
		if (GFX->blendSrcFactor == bm_src_alpha && GFX->blendDstFactor == bm_one)
			return bm_add;
		if (GFX->blendSrcFactor == bm_zero && GFX->blendDstFactor == bm_inv_src_color)
			return bm_subtract;
		if (GFX->blendSrcFactor == bm_src_alpha && GFX->blendDstFactor == bm_inv_src_color)
			return bm_max;

		return bm_normal;
	}

	IntType gpu_get_cullmode()
	{
		return (GFX->culling ? cull_counterclockwise : cull_noculling);
	}

	// MI3: read-and-clear the recoverable-allocation-failure flag set by
	// CheckD3DErrorSoft. GML polls this after a render pass to detect GPU OOM.
	IntType gpu_last_alloc_failed()
	{
		IntType r = GFX->gpuAllocFailed ? 1 : 0;
		GFX->gpuAllocFailed = false;
		return r;
	}

	// MI3: free local video memory in MB (budget - current usage), or -1 if the
	// runtime doesn't expose IDXGIAdapter3. Diagnostic for the OOM handling.
	IntType gpu_memory_available()
	{
	#if API_D3D11
		if (GFX->dxgiAdapter3)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO info;
			if (SUCCEEDED(GFX->dxgiAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				return (IntType)((info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0) / (1024 * 1024));
		}
	#endif
		return -1;
	}

	// MI3: flush queued GPU commands so freed resources are actually reclaimed
	// before an OOM retry re-allocates. Always returns 0.
	// MI3 Cinema: selftests del motor path tracer. Devuelven "" si todo OK,
	// o el mensaje de error (patron string vacio = exito, para GML).
	StringType pt_selftest()
	{
		QString err;
		if (CinemaSelftest(&err))
			return StringType();
		return StringType(err);
	}

	StringType pt_bvh_selftest()
	{
		IntType mismatches = 0;
		if (CinemaBvhSelftest(&mismatches))
			return StringType();
		return StringType(QString("bvh selftest: %1 mismatches").arg(mismatches));
	}

	void pt_begin() { CinemaSceneCaptureBegin(); }
	void pt_end() { CinemaSceneCaptureEnd(); }
	void pt_section(IntType tag) { CinemaSceneCaptureSetSection(tag); }
	// R15 (holdout): marca los draws que se capturen a continuacion como
	// "invisibles a camara" (mismo patron que pt_section: estado de captura que
	// GML fija por timeline en render_cinema_capture). El bit viaja al material
	// como MATF_CAM_INVISIBLE y SOLO lo filtran los rayos de camara.
	void pt_cam_invisible(IntType on) { CinemaSceneCaptureSetCamInvisible(on != 0); }
	// F1: marca los draws que se capturen a continuacion como MATTE (holdout que
	// OCLUYE). Mismo patron de estado de captura que pt_section/pt_cam_invisible.
	void pt_cam_matte(IntType on) { CinemaSceneCaptureSetCamMatte(on != 0); }
	// F1 (oleada 4a): SHADOW CATCHER. Marca los draws siguientes como
	// "superficie que solo recoge sombra": no aporta color al pixel, aporta
	// ALPHA = 1 - L_recibida/L_sin_ocluyentes (por canal, se emite el canal mas
	// ocluido) multiplicado por `opacity` (SHADOW_OPACITY, 0..1). Es la
	// contrapartida NATIVA de la receta de 3 pasadas (sujeto / suelo con sombra
	// / suelo sin sombra + division): un solo render, y el alfa ya trae la
	// sombra de contacto. Mismo patron de estado de captura que pt_cam_matte.
	void pt_shadow_catcher(IntType on, RealType opacity)
	{
		CinemaSceneCaptureSetShadowCatcher(on != 0, opacity);
	}
	// COBERTURA (alpha-as-coverage): marca los draws que se capturen a
	// continuacion como CAPA DE OBJETO (partes de personaje/modelo, items,
	// texto, primitivas). Su alpha de textura es COBERTURA -> MATF_COVERAGE y
	// el kernel usa PT_KIND_COVER. La geometria de block sheet (BLOCK/SCENERY/
	// MODEL), el suelo y las nubes NO lo activan: su alpha se sigue tratando
	// como TRANSMISION tenida (cristal de bloque, hielo) o via uIsWater.
	void pt_object_layer(IntType on) { CinemaSceneCaptureSetObjectLayer(on != 0); }
	// CAPAS N: id de OBJETO (timeline raiz) de los draws siguientes. 0 =
	// backdrop / sin asignar. Mismo patron de estado de captura que pt_section.
	void pt_object_id(IntType id) { CinemaSceneCaptureSetObjectId(id); }
	// Retencion de la copia CPU de vbuffers FUERA de la captura: permite
	// reconstruir las nubes (con su render offscreen) ANTES de pt_begin sin
	// que su vbuffer se libere antes del flatten. Restaurar a 0 tras usarlo.
	void pt_retain(IntType on) { cinemaRetainVbuffers = (on != 0); }
	// TRIM (ShinyTrims en Cinema). Mismo patron de estado de captura que
	// pt_section / pt_cam_invisible, y por el MISMO motivo que aquellos: el
	// shader de la pasada de captura (shader_color_fog) no declara ni uno de
	// los uniformes del trim, asi que preguntarselos a la captura devolveria
	// el default en silencio. fx = 0 apaga.
	void pt_trim(IntType fx, RealType intensity, RealType ox, RealType oy, RealType oz,
		IntType outline, IntType maskTexId)
	{
		CinemaSceneCaptureSetTrim(fx, intensity, ox, oy, oz, outline, maskTexId);
	}
	// TRIM: estado de ESCENA, una vez por frame. El TIEMPO llega ya resuelto
	// desde GML (tick0 + uTime/3, la MISMA formula del raster) para que el
	// reloj de pared del preview no pueda colarse en Cinema por construccion.
	// DIAGNOSTICO (gate G4): apaga la NEE de triangulos emisivos para el
	// PROXIMO render. Con ella apagada la emision entra SOLO por hits de BSDF
	// -- otro estimador de la misma integral. Si los dos convergen al mismo
	// valor medio, la cota estatica de la CDF reparte sin sesgar.
	void pt_emi_cdf(IntType on) { CinemaRenderSetEmiCdf(on != 0); }
	void pt_trim_scene(RealType tick0, RealType utime, RealType loopL, RealType sceneLight,
		RealType bodyScale, RealType emissive, IntType noiseTexId, RealType noiseSize,
		IntType ldrClip)
	{
		CinemaSceneTrimScene(tick0, utime, loopL, sceneLight, bodyScale, emissive,
			noiseTexId, noiseSize, ldrClip);
	}

	// FIX-B: EMISSIVE del objeto (value_inherit[e_value.EMISSIVE], ya clampeado y
	// heredado por tl_update_matrix) para los draws que se capturen a
	// continuacion. Espeja EXACTAMENTE el render_set_uniform("uEmissive", v) del
	// raster, que en la captura de Cinema es SILENCIOSO porque el shader ligado
	// (shader_color_fog) no declara ese uniform: CinemaSceneCaptureRecord leia el
	// nombre y caia siempre al default 0, asi que una superficie EMISSIVE=1
	// renderizaba NEGRA en Cinema (media 0.00000) mientras lucia en el raster.
	// Mismo patron de estado de captura que pt_cam_matte / pt_object_layer.
	void pt_emissive(RealType v) { CinemaSceneCaptureSetEmissive(v); }

	// FIX-A: sunAngleDeg = background_sunlight_angle, el DIAMETRO ANGULAR del
	// disco solar en grados (0.526 = sol real). Ultimo argumento por compatibilidad
	// de orden con las 16 que ya viajaban.
	// F5 (C1): envLight = project_cinema_env_light (0 = OFF, default). Mismo
	// criterio: entra al FINAL para no mover el orden de los que ya viajaban.
	// FONDO DE PELICULA: backdrop / backdropColor / backdropColor2 / backdropAngle
	// = project_cinema_backdrop* (0 = OFF, default). Mismo criterio otra vez:
	// al FINAL, sin mover el orden de los 18 anteriores.
	void pt_env_set(RealType sdx, RealType sdy, RealType sdz, IntType sunColor, RealType sunStrength,
		IntType ambColor, RealType ambStrength, IntType skyZenith, IntType skyHorizon, RealType nightAlpha,
		BoolType fogShow, IntType fogColor, IntType fogColorSun,
		RealType fogDist, RealType fogSize, RealType fogHeight, RealType sunAngleDeg,
		RealType envLight,
		IntType backdrop, IntType backdropColor, IntType backdropColor2, RealType backdropAngle,
		IntType tileX0, IntType tileY0, IntType tileFullW, IntType tileFullH)
	{
		CinemaSceneEnvSet(sdx, sdy, sdz, sunColor, sunStrength, ambColor, ambStrength,
			skyZenith, skyHorizon, nightAlpha, fogShow, fogColor, fogColorSun,
			fogDist, fogSize, fogHeight, sunAngleDeg, envLight,
			backdrop, backdropColor, backdropColor2, backdropAngle,
			tileX0, tileY0, tileFullW, tileFullH);
	}

	IntType pt_light_add(RealType x, RealType y, RealType z, RealType dx, RealType dy, RealType dz,
		RealType range, IntType color, RealType strength, RealType size, RealType fadeSize,
		RealType spotRadius, RealType spotSharpness, BoolType isSpot)
	{
		return CinemaSceneLightAdd(x, y, z, dx, dy, dz, range, color, strength, size, fadeSize,
			spotRadius, spotSharpness, isSpot);
	}

	// DOF-FIEL A3: apertureCoef llega ya calculado desde GML (uBlurSize * 2 *
	// tan(fov/2)); blades/lensRot/lensRatio/lensBias son los controles de
	// apertura poligonal de A1/A5 (todos 0 => disco uniforme, bit-identico).
	void pt_camera_set(RealType cx, RealType cy, RealType cz, MatrixType viewProj, RealType zNear, RealType zFar,
		RealType focusDist, RealType apertureRadius, RealType apertureCoef, BoolType dofOn, BoolType autofocus, BoolType ortho,
		RealType blades, RealType lensRot, RealType lensRatio, RealType lensBias)
	{
		CinemaSceneCameraSet(viewProj.matrix, cx, cy, cz, zNear, zFar, focusDist, apertureRadius, apertureCoef, dofOn, autofocus, ortho,
			blades, lensRot, lensRatio, lensBias);
	}

	StringType pt_render_begin(IntType w, IntType h, IntType seed)
	{
		return StringType(CinemaRenderBegin(w, h, seed));
	}

	StringType pt_render_sample(IntType bounces, RealType clampIndirect, RealType emissiveScale, BoolType alphaFilm, IntType sppCount, RealType noiseThreshold)
	{
		return StringType(CinemaRenderSample(bounces, clampIndirect, emissiveScale, alphaFilm, sppCount, noiseThreshold));
	}

	// M7 adaptive: numero de pixeles NO convergidos (-1 si no hay render adaptive).
	// render_cinema lo sondea para la terminacion temprana global.
	IntType pt_render_active_count()
	{
		return CinemaRenderActiveCount();
	}

	// M7 adaptive: vuelca el conteo por pixel como heatmap al surface (evidencia).
	StringType pt_render_heatmap(IntType surfId, RealType maxCount)
	{
		return StringType(CinemaRenderHeatmap(surfId, maxCount));
	}

	StringType pt_render_finish(IntType surfId, RealType exposure, IntType tonemap, IntType denoiseMode, RealType saturation)
	{
		return StringType(CinemaRenderFinish(surfId, exposure, tonemap, denoiseMode, saturation));
	}

	StringType pt_parity(IntType w, IntType h, IntType spp, IntType seed, IntType bounces,
		RealType clampIndirect, RealType emissiveScale, RealType rmseThreshold)
	{
		return StringType(CinemaRenderParity(w, h, spp, seed, bounces, clampIndirect, emissiveScale, rmseThreshold));
	}

	StringType pt_render_status()
	{
		return StringType(CinemaRenderStatus());
	}

	// F2: DoF EFECTIVO de la camara ya saneada (y ya sobrescrita por la sonda de
	// autofocus si corrio). Solo lectura: no altera el render.
	RealType pt_focus_dist() { return CinemaSceneCameraFocusDist(); }
	RealType pt_aperture()   { return CinemaSceneCameraAperture(); }

	// DOF-FIEL D1: cadena clave=valor del ULTIMO CinemaRenderBegin (autofocus):
	// "mode=region focus=57.31 aperture=0.418 region_hits=203 frame_hits=3871 planar=1"
	StringType pt_dof_info() { return StringType(CinemaRenderDofInfo()); }

	// F2/R17: escribe el mapa de profundidad del ultimo render resuelto. Hace su
	// PROPIO readback del AOV: NO toca el acumulador ni el resolve -> el beauty
	// queda bit-identico con el pase activado o no.
	StringType pt_depth_save(StringType filename) { return StringType(CinemaRenderSaveDepth(filename)); }

	// CAPAS N: estadistica de profundidad POR OBJETO (sonda pinhole de CPU
	// sobre el BVH del ultimo build). Ver CinemaSceneObjectDepths.
	StringType pt_object_depths(RealType w, RealType h)
	{ return StringType(CinemaSceneObjectDepths((IntType)w, (IntType)h)); }

	// CAPAS N v2: percentiles de profundidad POR BANDA sobre la cache por rayo
	// de la ultima pt_object_depths. Ver CinemaSceneBandDepths.
	StringType pt_band_depths(StringType bandmap, RealType skydist)
	{ return StringType(CinemaSceneBandDepths(bandmap, (float)skydist)); }

	// CAPAS SLICING: ventana de profundidad de la pasada (0,0 = OFF).
	void pt_band_window(RealType t0, RealType t1)
	{ CinemaSceneBandWindowSet((float)t0, (float)t1); }

	// CAPAS SLICING: histograma / estadisticas por ventana del ULTIMO mapa de
	// profundidad guardado (misma fuente y mismas unidades que el _depth.json).
	StringType pt_depth_hist(RealType bins)
	{ return StringType(CinemaRenderDepthHist((IntType)bins)); }
	StringType pt_depth_bands(StringType cuts)
	{ return StringType(CinemaRenderDepthBands(cuts)); }
	StringType pt_object_bands(StringType cuts)
	{ return StringType(CinemaSceneObjectBands(cuts)); }

	// T6: concordancia por rayo sonda-de-objetos vs mapa de profundidad.
	StringType pt_probe_agree()
	{ return StringType(CinemaRenderProbeAgree()); }

	// V1(b)/V2: estadisticas de las capas ENTREGADAS (red post-render + manifest)
	StringType pt_layers_top_stats(StringType base, RealType n)
	{ return StringType(CinemaRenderLayersTopStats(base, (IntType)n)); }

	// SPEC_WHITE_DETAILS #C2: natives del post-proceso "white details" (inner
	// outline + cunas rim-light). Post-proceso CPU puro sobre PNG straight-alpha,
	// fuera del camino de render/paridad (ver Render/Mi3WhiteDetails.hpp).
	// SPEC_WHITE_DETAILS #C2 (DEPRECADO, ver SPEC_WHITE_DETAILS_GOLD C3):
	// native viejo de 10 argumentos posicionales sobre el post-proceso "white
	// details". Mi3WhiteDetailsRun ahora es un wrapper que arma la cadena
	// "opts" equivalente y delega en el motor GOLD (Mi3WhiteDetailsRunOpt).
	// Se mantiene para no romper llamadores existentes.
	StringType mi3_whitedetails_run(StringType src, StringType out, RealType angle, RealType density,
		RealType wedgeW, RealType outlineW, RealType outlineA, RealType minRun, IntType seed, IntType bake)
	{ return StringType(Mi3WhiteDetailsRun(src, out, angle, density, wedgeW, outlineW, outlineA, minRun, seed, bake)); }
	RealType mi3_whitedetails_auto_angle() { return Mi3WhiteDetailsAutoAngle(); }

	// SPEC_WHITE_DETAILS_GOLD #C3: native del motor GOLD, pilotado por una
	// cadena "opts" (clave=valor;...) en vez de argumentos posicionales fijos
	// (ver Render/Mi3WhiteDetails.hpp / SS4 del spec para la tabla completa).
	StringType mi3_whitedetails_run_opt(StringType src, StringType out, StringType opts)
	{ return StringType(Mi3WhiteDetailsRunOpt(src, out, opts)); }

	// R9 (FIX 1): deferBvh salta el arbol BVH de CPU cuando el backend sera DXR
	// (preview interactivo agil). 0 = build completo (export/final/headless).
	StringType pt_build(RealType vertexAo, RealType deferBvh)
	{
		return StringType(CinemaSceneBuild(vertexAo, deferBvh != 0));
	}

	// R9 (FIX 1/5): backend de trazado efectivo (0 = D3D11, 1 = DXR). GML lo usa
	// para decidir el defer del preview y para escalar el presupuesto del export.
	IntType pt_backend() { return CinemaRenderWantsDxr() ? 1 : 0; }

	// R9 (FIX 3): 1 si el device OIDN es CUDA (denoise de decenas de ms). El
	// preview lo consulta para el checkpoint denoisado a ~32 samples (en CPU no).
	IntType pt_oidn_cuda() { return CinemaDenoiseIsCudaAvailable() ? 1 : 0; }

	// R10 (FIX 1/2): ms (wall) del ultimo Begin (reset) y del ultimo Sample. El
	// preview interactivo los usa para (a) verificar el reset BARATO y (b) la
	// resolucion dinamica adaptativa (ajusta el factor de escala al ms/megapixel
	// medido apuntando a ~14 ms de GPU por frame en movimiento). En DXR el Sample
	// drena la GPU, asi que el ms es tiempo de GPU real.
	RealType pt_ms_begin() { return CinemaRenderMsBegin(); }
	RealType pt_ms_sample() { return CinemaRenderMsSample(); }
	RealType pt_ms_finish() { return CinemaRenderMsFinish(); }

	// M8-B ("escena viva"): tras una re-captura del frame (render_cinema_capture),
	// intenta un update TRANSFORM-ONLY contra el snapshot del ultimo build. Devuelve
	// "transform" (instancias+TLAS actualizados en µs), "static" (nada cambio) o
	// "rebuild" (topologia distinta / backend D3D11 -> el caller hace pt_build).
	StringType pt_scene_update() { return StringType(CinemaRenderSceneUpdate()); }
	// Ms del ultimo update en vivo (gate de coste M8-B).
	RealType pt_ms_update() { return CinemaRenderMsUpdate(); }
	StringType pt_status() { return StringType(CinemaSceneStatus()); }
	void pt_invalidate() { CinemaSceneInvalidate(); }

	// R13: interruptor del readback GPU del flatten (1 = ON, el default) y
	// repro del estado "geometria solo en GPU" que deja el .meshcache. Ambas
	// son de harness/diagnostico: ningun camino de producto las llama.
	void pt_readback(IntType on) { CinemaSceneSetReadback(on != 0); }
	IntType pt_debug_free_cpu() { return CinemaSceneDebugFreeCpuMeshes(); }

	// R18 (escenas gigantes): radio de RECORTE POR DISTANCIA del flatten en
	// unidades de mundo (<= 0 = OFF, bit-identico al camino de siempre). Lo fija
	// render_cinema desde project_cinema_clip ANTES de pt_build. No es culling
	// por frustum: lo que queda dentro del radio sigue dando sombras/GI/reflejos
	// aunque este fuera de cuadro; lo que queda fuera desaparece igual que en el
	// raster mas alla de project_render_distance.
	void pt_clip(RealType radius) { CinemaSceneSetClipRadius(radius); }

	// R18 (solo harness/diagnostico): fuerza el presupuesto de memoria del build
	// a N MB para poder probar el ABORTO LIMPIO de forma determinista en
	// cualquier maquina. 0 = volver a la medida real del sistema.
	void pt_mem_limit(RealType mb) { CinemaSceneSetMemLimitMB(mb); }

	// R18: RAM fisica libre y working set del proceso, en MB (-1 desconocido).
	// GML los usa para el toast del abort y para el log de progreso.
	IntType pt_mem_free() { return CinemaSceneMemAvailableMB(); }
	IntType pt_mem_proc() { return CinemaSceneMemProcessMB(); }

	StringType pt_probe(IntType surfId, StringType tag)
	{
		return CinemaProbe(surfId, tag);
	}

	IntType pt_finish_test(IntType surfId)
	{
		return CinemaFinishTest(surfId) ? 1 : 0;
	}

	IntType gpu_flush()
	{
	#if API_D3D11
		if (D3DContext)
			D3DContext->Flush();
	#endif
		return 0;
	}

	BoolType gpu_get_tex_filter()
	{
		return GFX->texFilter;
	}

	IntType gpu_get_tex_mip_bias()
	{
		return GFX->lodBias;
	}

	IntType gpu_get_tex_mip_enable()
	{
		return GFX->mipMap ? mip_on : mip_off;
	}

	BoolType gpu_get_texfilter_ext(IntType sampler)
	{
		return GFX->shader->samplerState[sampler].filter;
	}

	void gpu_set_alphatestenable(BoolType)
	{
		// Do nothing
	}

	void gpu_set_alphatestref(IntType)
	{
		// Do nothing
	}

	void gpu_set_blendenable(BoolType enabled)
	{
		// Blending always enabled
	}

	void gpu_set_blendmode_ext_sepalpha(IntType src, IntType dest, IntType alphasrc, IntType alphadest)
	{
		GFX->SetBlendingFuncs(src, dest, alphasrc, alphadest);
	}

	void gpu_set_blendmode_ext(IntType src, IntType dest)
	{
		gpu_set_blendmode_ext_sepalpha(src, dest, src, dest);
	}

	void gpu_set_blendmode(IntType mode)
	{
		switch (mode)
		{
			case bm_normal: gpu_set_blendmode_ext(bm_src_alpha, bm_inv_src_alpha); break;
			case bm_add: gpu_set_blendmode_ext(bm_src_alpha, bm_one); break;
			case bm_subtract: gpu_set_blendmode_ext(bm_zero, bm_inv_src_color); break;
			case bm_max: gpu_set_blendmode_ext(bm_src_alpha, bm_inv_src_color); break;
		}
	}

	void gpu_set_cullmode(IntType mode)
	{
		GFX->SetCulling(mode == cull_counterclockwise);
	}

	void gpu_set_tex_filter(BoolType enabled)
	{
		for (IntType s = 0; s < GFX->shader->numSamplers; s++)
		{
			Shader::SamplerState& state = GFX->shader->samplerState[s];
			if (state.filter == enabled)
				continue;

			GFX->SubmitBatch();
			state.filter = enabled;
			state.changed = true;
		}

		GFX->texFilter = enabled;
	}

	void gpu_set_tex_filter_ext(IntType sampler, BoolType enabled)
	{
		Shader::SamplerState& state = GFX->shader->samplerState[sampler];
		if (state.filter == enabled)
			return;

		GFX->SubmitBatch();
		state.filter = enabled;
		state.changed = true;
	}

	void gpu_set_texrepeat(BoolType enabled)
	{
		IntType enabledInt = enabled ? 1 : 0;
		for (IntType s = 0; s < GFX->shader->numSamplers; s++)
		{
			if (GFX->shader->samplerRepeat[s] == enabledInt)
				continue;

			GFX->SubmitBatch();
			GFX->shader->samplerRepeat[s] = enabledInt;
		}

		GFX->texRepeat = enabled;
	}

	void gpu_set_texrepeat_ext(IntType sampler, BoolType enabled)
	{
		IntType enabledInt = enabled ? 1 : 0;
		if (GFX->shader->samplerRepeat[sampler] == enabledInt)
			return;

		GFX->SubmitBatch();
		GFX->shader->samplerRepeat[sampler] = enabledInt;
	}

	void gpu_set_tex_max_mip(IntType maxmip)
	{
		// Do nothing
	}

	void gpu_set_tex_mip_bias(IntType bias)
	{
		GFX->SetLODBias(bias);

		for (IntType s = 0; s < GFX->shader->numSamplers; s++)
			GFX->shader->samplerState[s].changed = true;
	}

	void gpu_set_tex_mip_enable(IntType mode)
	{
		BoolType enabled = (mode == mip_on);
		if (GFX->mipMap == enabled)
			return;

		GFX->SubmitBatch();
		GFX->mipMap = enabled;

		for (IntType s = 0; s < GFX->shader->numSamplers; s++)
			GFX->shader->samplerState[s].changed = true;
	}

	void gpu_set_tex_mip_filter_ext(IntType sampler, IntType filter)
	{
		// Always linear
	}

	void gpu_set_tex_mip_filter(IntType filter)
	{
		// Always linear
	}

	void gpu_set_tex_repeat(BoolType enabled)
	{
		gpu_set_texrepeat(enabled);
	}

	void gpu_set_texfilter_ext(IntType sampler, BoolType enabled)
	{
		gpu_set_tex_filter_ext(sampler, enabled);
	}

	void gpu_set_texfilter(BoolType enabled)
	{
		return gpu_set_tex_filter(enabled);
	}

	void gpu_set_ztestenable(BoolType enabled)
	{
		GFX->SetDepthTest(enabled);
	}

	void gpu_set_zwriteenable(BoolType enabled)
	{
		GFX->SetDepthWrite(enabled);
	}

	MatrixType matrix_build_lookat(RealType xfrom, RealType yfrom, RealType zfrom, RealType xto, RealType yto, RealType zto, RealType xup, RealType yup, RealType zup)
	{
		return Matrix::LookAt(
			{ xfrom, yfrom, zfrom },
			{ xto, yto, zto },
			{ xup, yup, zup }
		);
	}

	MatrixType matrix_build_projection_ortho(RealType width, RealType height, RealType znear, RealType zfar)
	{
		return Matrix::Ortho(0, width, 0, height, znear, zfar);
	}

	MatrixType matrix_build_projection_perspective_fov(RealType fov, RealType aspect, RealType znear, RealType zfar)
	{
		return Matrix::Perspective(fov, aspect, znear, zfar);
	}

	MatrixType matrix_build(RealType posX, RealType posY, RealType posZ, RealType rotX, RealType rotY, RealType rotZ, RealType scaleX, RealType scaleY, RealType scaleZ)
	{
		BoolType noPos = (posX == 0.0 && posY == 0.0 && posZ == 0.0);
		BoolType noRot = (rotX == 0.0 && rotY == 0.0 && rotZ == 0.0);
		BoolType noScale = (scaleX == 1.0 && scaleY == 1.0 && scaleZ == 1.0);

		if (noPos && noRot && noScale)
			return Matrix::Identity();

		if (!noPos && noRot && noScale)
			return Matrix::Translation(posX, posY, posZ);

		if (noPos && noRot && !noScale)
			return Matrix::Scale(scaleX, scaleY, scaleZ);

		if (noPos && !noRot && noScale)
			return Matrix::Rotation(rotX, rotY, rotZ).GetTransposed();

		return (Matrix::Scale(scaleX, scaleY, scaleZ) *
			   Matrix::Rotation(rotX, rotY, rotZ)).GetTransposed() *
			   Matrix::Translation(posX, posY, posZ);
	}

	// MI3 R14 (camara ORTOGRAFICA): GraphicsApiHandler::SetMatrix descarta la
	// actualizacion del frustum de culling cuando la proyeccion tiene m[15] != 0
	// ("ignore ortho matrices"), heuristica pensada para los pases 2D
	// fullscreen. Una camara de ESCENA ortografica tambien tiene m[15] = 1, asi
	// que sin esto el culling se quedaria con el frustum PERSPECTIVO anterior
	// (el de la ultima luz o el del render previo) y descartaria geometria
	// visible. render_set_projection lo llama explicitamente en su rama ortho.
	void render_frustum_update()
	{
		GFX->UpdateFrustum();
	}

	MatrixType matrix_get(IntType type)
	{
		switch (type)
		{
			case matrix_world: return GFX->matrixM;
			case matrix_view: return GFX->matrixV;
			case matrix_projection: return GFX->matrixP;
		}
		return MatrixType();
	}

	MatrixType matrix_multiply(MatrixType m1, MatrixType m2)
	{
		return m1.matrix * m2.matrix;
	}

	void matrix_set(IntType type, MatrixType mat)
	{
		if (type == matrix_projection)
		{
			// Flip 2nd column
			mat.matrix.m[1] *= -1;
			mat.matrix.m[5] *= -1;
			mat.matrix.m[9] *= -1;
			mat.matrix.m[13] *= -1;
		}
		GFX->SetMatrix(type, mat.matrix);
	}

	VecType matrix_transform_vertex(MatrixType m, RealType x, RealType y, RealType z)
	{
		return m.matrix * VecType(x, y, z);
	}

	IntType shader_get_sampler_index(IntType id, StringType name)
	{
		if (Shader* shader = FindShader(id))
			return shader->GetSamplerIndex(name);
		return -1;
	}

	IntType shader_get_uniform(IntType id, StringType name)
	{
		if (Shader* shader = FindShader(id))
			return shader->GetUniformIndex(name);
		return -1;
	}

	BoolType shader_is_compiled(IntType id)
	{
		if (Shader* shader = FindShader(id))
			return shader->IsLoaded();
		return false;
	}

	void shader_reset()
	{
		if (GFX->shader == PR->GetShader())
			WARNING("shader_reset on default shader");

		GFX->SubmitBatch();
		GFX->shader->EndUse();
		GFX->shader = PR->GetShader();
		GFX->shader->BeginUse();
	}

	void shader_set_uniform_f_array(IntType handle, VarType arr)
	{
		// Unused
	}

	void shader_set_uniform_f(VarType args)
	{
		// Unused
	}

	void shader_set_uniform_i_array(IntType, ArrType)
	{
		// Unused
	}

	void shader_set_uniform_i(VarType args)
	{
		// Unused
	}

	void shader_set(IntType id)
	{
		if (Shader* shader = FindShader(id))
		{
			GFX->SubmitBatch();
			GFX->shader->EndUse();
			GFX->shader = shader;
			GFX->shader->BeginUse();

			// Update shader with latest matrices
			GFX->shader->SubmitMatrix(Shader::M, GFX->matrixM);
			GFX->shader->SubmitMatrix(Shader::V, GFX->matrixV);
			GFX->shader->SubmitMatrix(Shader::P, GFX->matrixP);
		}
	}

	BoolType shaders_are_supported()
	{
		return true;
	}

	void surface_copy(IntType dest, IntType x, IntType y, IntType src)
	{
		surface_set_target(dest);
		draw_clear_alpha(0, 0);
		draw_surface(src, x, y);
		surface_reset_target();
	}

	IntType surface_create(IntType width, IntType height)
	{
		return (new Surface({ (int)width, (int)height }))->id;
	}

	IntType surface_create_ext2(IntType width, IntType height, BoolType depthBuffer, BoolType hdr)
	{
		return (new Surface({ (int)width, (int)height }, depthBuffer, hdr))->id;
	}

	BoolType surface_exists(IntType id)
	{
		return (FindSurface(id) != nullptr);
	}

	void surface_free(IntType id)
	{
		if (Surface* surf = FindSurface(id))
		{
			GFX->SubmitBatch();
			delete surf;
		}
	}

	IntType surface_get_height(IntType id)
	{
		if (Surface* surf = FindSurface(id))
			return surf->size.height();
		return -1;
	}

	IntType surface_get_texture(IntType id)
	{
		if (Surface* surf = FindSurface(id))
			return surf->frameBuffer->GetColorTexId();
		return -1;
	}

	IntType surface_get_width(IntType id)
	{
		if (Surface* surf = FindSurface(id))
			return surf->size.width();
		return -1;
	}

	IntType surface_getpixel(IntType id, IntType x, IntType y)
	{
		if (Surface* surf = FindSurface(id))
		{
			GFX->SubmitBatch();
			return GFX->QColorToInt(surf->GetColor(QPoint(x, y), false));
		}
		return -1;
	}

	void surface_clear_depth_cache(IntType id)
	{
		if (Surface* surf = FindSurface(id))
			surf->ClearDepthCache();
	}

	// DOF-FIEL C1/Cambio 21: mediana de profundidad EMPAQUETADA (r + g/255 +
	// b/65025, la misma convencion de getDepth() de shader_depth.fsh) de una
	// region de un surface de depth pass. UN SOLO readback (CopyColorData)
	// para toda la superficie: surface_getpixel haria uno por pixel. Descarta
	// el cielo (valor >= skyMax) y submuestrea a un maximo de maxSamples^2
	// puntos. Devuelve -1 si no hay hits (region entera de cielo).
	RealType surface_depth_median(IntType surfId, IntType x0, IntType y0, IntType x1, IntType y1,
		RealType skyMax, IntType maxSamples)
	{
		Surface* surf = FindSurface(surfId);
		if (!surf || !surf->frameBuffer)
			return -1.0;
		IntType w = surf->size.width(), h = surf->size.height();
		if (w <= 0 || h <= 0)
			return -1.0;

		GFX->SubmitBatch();
		QVector<uchar> colorData((int)(w * h * 4));
		surf->frameBuffer->CopyColorData(colorData.data());

		IntType cx0 = std::clamp(x0, (IntType)0, w - 1);
		IntType cy0 = std::clamp(y0, (IntType)0, h - 1);
		IntType cx1 = std::clamp(x1, (IntType)0, w - 1);
		IntType cy1 = std::clamp(y1, (IntType)0, h - 1);
		if (cx1 < cx0) std::swap(cx1, cx0);
		if (cy1 < cy0) std::swap(cy1, cy0);
		IntType rw = cx1 - cx0 + 1, rh = cy1 - cy0 + 1;
		IntType samp = maxSamples > 0 ? maxSamples : 1;
		IntType stepX = rw / samp; if (stepX < 1) stepX = 1;
		IntType stepY = rh / samp; if (stepY < 1) stepY = 1;

		std::vector<float> hits;
		hits.reserve((size_t)samp * (size_t)samp);
		for (IntType py = cy0; py <= cy1; py += stepY)
			for (IntType px = cx0; px <= cx1; px += stepX)
			{
				size_t idx = ((size_t)py * (size_t)w + (size_t)px) * 4;
				float r = colorData[(int)idx + 0] / 255.f;
				float g = colorData[(int)idx + 1] / 255.f;
				float b = colorData[(int)idx + 2] / 255.f;
				float v = r + g / 255.f + b / 65025.f;
				if (v < (float)skyMax)
					hits.push_back(v);
			}
		if (hits.empty())
			return -1.0;
		std::sort(hits.begin(), hits.end());
		return (RealType)hits[hits.size() / 2];
	}

	void mi3_rdoc_capture(IntType begin)
	{
	#if API_D3D11
		// DEV: captura RenderDoc del render OFFSCREEN sin depender de Present.
		// Solo actua si el proceso corre hookeado bajo renderdoccmd.
		if (HMODULE mod = GetModuleHandleA("renderdoc.dll"))
		{
			pRENDERDOC_GetAPI getApi = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
			RENDERDOC_API_1_1_2* api = nullptr;
			if (getApi && getApi(eRENDERDOC_API_Version_1_1_2, (void**)&api) == 1 && api)
			{
				if (begin)
					api->StartFrameCapture(nullptr, nullptr);
				else
					api->EndFrameCapture(nullptr, nullptr);
			}
		}
	#endif
	}

	RealType surface_get_depth(IntType id, IntType x, IntType y)
	{
		if (Surface* surf = FindSurface(id))
			return surf->GetDepth(QPoint(x, y));
		return 0.0;
	}

	IntType surface_get_max_size()
	{
		return GFX->GetMaxSize();
	}

	void surface_reset_target()
	{
		if (GFX->surface == AppWin->GetSurface())
		{
			WARNING("Invalid surface_reset_target");
			return;
		}

		GFX->SubmitBatch();
		GFX->ResetMRT();
		GFX->surface->EndUse();
		GFX->surface = AppWin->GetSurface();
		GFX->surface->BeginUse();
	}

	void surface_resize(IntType id, IntType width, IntType height)
	{
		if (Surface* surf = FindSurface(id))
			surf->Resize({ (int)width, (int)height });
	}

	void surface_save(IntType id, StringType filename)
	{
		if (Surface* surf = FindSurface(id))
		{
			GFX->SubmitBatch();
			surf->ToImage().save(filename);
		}
	}

	IntType surface_set_target_ext(IntType index, IntType id)
	{
		if (index == 0)
			surface_set_target(id);
		
		if (Surface* surf = FindSurface(id))
		{
			GFX->SetMRTIndex(index, surf->frameBuffer);
			return 1;
		}

		return -1;
	}

	IntType surface_set_target(IntType id)
	{
		if (Surface* surf = FindSurface(id))
		{
			GFX->SubmitBatch();
			GFX->ClipEnd();
			GFX->ResetMRT();
			GFX->surface->EndUse();
			GFX->surface = surf;
			if (GFX->surface->BeginUse())
				return 1;
		}
		WARNING("surface_set_target failed for id " + NumStr(id));
		return -1;
	}

	void texture_set_stage(IntType stage, IntType tex)
	{
		if (tex > 0 && GFX->shader->IsLoaded())
			GFX->shader->SubmitTexture(stage, tex);
	}

	void vertex_begin(IntType buffer, IntType format)
	{
		// Do nothing
	}

	void vertex_color(IntType buffer, IntType color, RealType alpha)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
			buf->GetCurrentVertex(2).SetColor(color, alpha);
	}

	IntType vertex_create_buffer()
	{
		return (new VertexBuffer)->id;
	}

	IntType vertex_create_buffer_from_buffer(IntType, IntType)
	{
		// Unused
		return 0;
	}

	void vertex_delete_buffer(IntType id)
	{
		if (VertexBuffer* buffer = FindVertexBuffer(id))
			delete buffer;
	}

	void vertex_end(IntType buffer)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
			buf->Complete();
	}

	void vertex_float3(IntType buffer, RealType x, RealType y, RealType z)
	{
		// Do nothing
	}

	void vertex_float4(IntType buffer, RealType x, RealType y, RealType z, RealType w)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
		{
			Vertex& vertex = buf->GetCurrentVertex(4);
			if (x > 0.0)
				vertex.EnableFlag(Vertex::WAVE_XY);
			if (y > 0.0)
				vertex.EnableFlag(Vertex::WAVE_Z);
			vertex.SetEmissive(z);
			if (w > 0.0)
				vertex.EnableFlag(Vertex::SUBSURFACE);
		}
	}

	void vertex_format_add_colour()
	{
		// Do nothing
	}

	void vertex_format_add_custom(IntType type, IntType usage)
	{
		// Do nothing
	}

	void vertex_format_add_normal()
	{
		// Do nothing
	}

	void vertex_format_add_position_3d()
	{
		// Do nothing
	}

	void vertex_format_add_texcoord()
	{
		// Do nothing
	}

	void vertex_format_begin()
	{
		// Do nothing
	}

	IntType vertex_format_end()
	{
		// Do nothing
		return 0;
	}

	void vertex_freeze(IntType buffer)
	{
		// Do nothing
	}

	IntType vertex_get_number(IntType buffer)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
			return buf->numIndices;
		return 0;
	}

	void vertex_normal(IntType buffer, RealType nx, RealType ny, RealType nz)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
			buf->GetCurrentVertex(1).SetNormal(nx, ny, nz);
	}

	void vertex_position_3d(IntType buffer, RealType x, RealType y, RealType z)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
		{
			Vertex& vertex = buf->GetCurrentVertex(0);
			vertex.x = x;
			vertex.y = y;
			vertex.z = z;
		}
	}

	void vertex_submit(IntType buffer, IntType primitive, IntType texture)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
			VB->Add(buf);
	}

	void vertex_texcoord(IntType buffer, RealType u, RealType v)
	{
		if (VertexBuffer* buf = FindVertexBuffer(buffer))
		{
			Vertex& vertex = buf->GetCurrentVertex(3);
			vertex.u = u;
			vertex.v = v;
		}
	}

	IntType get_vertex_buffer_triangles()
	{
		IntType num = VB->trianglesSubmitted;
		VB->trianglesSubmitted = 0;
		return num;
	}

	IntType get_vertex_buffer_render_calls()
	{
		IntType num = VB->renderCalls;
		VB->renderCalls = 0;
		return num;
	}

	IntType get_primitive_lines()
	{
		IntType num = PR->linesSubmitted;
		PR->linesSubmitted = 0;
		return num;
	}

	IntType get_primitive_triangles()
	{
		IntType num = PR->trianglesSubmitted;
		PR->trianglesSubmitted = 0;
		return num;
	}

	IntType get_primitive_render_calls()
	{
		IntType num = PR->renderCalls;
		PR->renderCalls = 0;
		return num;
	}

	void vertex_buffer_set_save_data(IntType id, BoolType save)
	{
		if (VertexBuffer* buf = FindVertexBuffer(id))
			buf->saveData = save;
	}

	void submit_batch()
	{
		GFX->SubmitBatch();
	}

	void shader_submit_int(IntType index, IntType value)
	{
		GFX->shader->SubmitInt(index, value);
	}

	void shader_submit_float(IntType index, RealType value)
	{
		GFX->shader->SubmitFloat(index, value);
	}

	void shader_submit_vec2(IntType index, RealType x, RealType y)
	{
		GFX->shader->SubmitVec2(index, x, y);
	}

	void shader_submit_vec3(IntType index, RealType x, RealType y, RealType z)
	{
		GFX->shader->SubmitVec3(index, x, y, z);
	}

	void shader_submit_vec4(IntType index, RealType x, RealType y, RealType z, RealType w)
	{
		GFX->shader->SubmitVec4(index, x, y, z, w);
	}

	void shader_submit_float_array(IntType index, VarType array)
	{
		GFX->shader->SubmitFloatArray(index, array);
	}

	void shader_submit_mat4_array(IntType index, ArrType array)
	{
		GFX->shader->SubmitMat4Array(index, array);
	}

	void update_frustum()
	{
		GFX->UpdateFrustum();
	}
}
