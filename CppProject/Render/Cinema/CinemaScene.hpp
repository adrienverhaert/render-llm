#pragma once
#include "Common.hpp"
#include "Render/GraphicsApiHandler.hpp"
#include "Render/Matrix.hpp"
#include "CinemaBvh.hpp"

#include <vector>

// CinemaScene: captura de draws + conversion de escena para el motor Cinema
// (CINEMA_DESIGN.md, secciones 4.5-4.7 y 5; contratos de layout GPU en
// CinemaBvh.hpp y CinemaKernelM0.hpp).
//
// Responsabilidades:
//   1. CAPTURA: snapshot de cada draw durante la pasada real de render (hook
//      en VertexBufferRenderer::Add; ver cinemaSceneCaptureActive abajo).
//      Cada PTDraw congela: vbuffer, matriz de mundo, texturas ligadas con su
//      uvRect y los uniforms de material del objeto (uBlendColor, uEmissive,
//      uTwoSided, uIsWater, uAlphaHash, uMetallic, uRoughness,
//      uDefaultEmissive, uMaterialFormat, uUseNormalMap, uIsGround, uSSS).
//   2. FLATTEN: vertices fuente transformados a mundo -> PTVert (layout de
//      CinemaBvh.hpp) + PTTri con matId; registro drawId -> rango de tris.
//   3. ATLAS: Texture2DArray RGBA8 SRGB 4096x4096. TexturePages -> slice
//      directo; imagenes sueltas (Frames de Sprite, Surfaces) -> shelf-packer
//      en slices extra. Dedup por texId. Por textura se guarda slice + rect
//      de remapeo dentro del slice.
//   4. MATERIALES: un PTMaterial por draw valido (stride 96, layout EXACTO de
//      CinemaKernelM0.hpp; static_assert en el .cpp).
//   5. LUCES / ENV / CAMARA: structs receptores de pt_env_set / pt_light_add /
//      pt_camera_set (PTLight 64 B, diseno seccion 3; inversa de view_proj
//      calculada en CPU con Matrix::GetInversed).
//   6. BUILD: CinemaSceneBuild() = flatten + atlas + CinemaBvhBuild + CDF de
//      triangulos emisores (NEE M1) + upload de verts/tris(REORDENADOS)/
//      nodos/materiales/atlas/luces/CDF emisiva via CinemaGpu.
//
// NO despacha el kernel: eso es CinemaRender (consume CinemaSceneGetGpu(),
// CinemaSceneEnv(), CinemaSceneCamera(), CinemaSceneLights()).
//
// Threading: TODO el API corre en el hilo de render/GML (los natives llaman
// aqui y el hook captura durante draws del mismo hilo). El flatten y el BVH
// usan OpenMP internamente con escrituras a rangos disjuntos.

namespace CppProject
{
	struct VertexBuffer;

	// --------------------------------------------------------------------
	// Structs GPU (complemento de los PTVert/PTTri/PTNode de CinemaBvh.hpp)
	// --------------------------------------------------------------------

	// Material del path tracer, 128 bytes. Layout EXACTO del struct HLSL
	// PTMaterial de CinemaKernelM0.hpp (stride verificado por static_assert
	// contra CinemaKernelM0::PTMATERIAL_STRIDE en el .cpp). Los bits de
	// flags son los MATF_* de CinemaKernelM0.hpp.
	struct PTMaterial
	{
		float uvRect[4];    // remap del albedo dentro del slice (offset.xy, escala.zw)
		float uvRectMat[4]; // idem material map (invalido si !HAS_MATTEX)
		float uvRectNrm[4]; // idem normal map (invalido si !HAS_NRMTEX)
		float blendR, blendG, blendB, blendA; // uBlendColor LINEALIZADO; alpha de objeto en A (lineal tal cual)
		float emissive;     // uEmissive del draw (0..1)
		float metallic;     // uMetallic del draw
		float roughness;    // uRoughness del draw
		uint32_t flags;     // CinemaKernelM0::MATF_*
		uint32_t sliceAlbedo, sliceMat, sliceNrm;
		// TRIM (ShinyTrims en Cinema): reemplaza al antiguo 'pad'.
		//   bits  0-7   fx        slot del efecto (199..254; 0 = sin trim)
		//   bits  8-15  sliceTrim slice del atlas donde vive la mascara M1
		//   bit  16     OUTLINE   esta pieza es la cascara del outline 3D
		//   bit  17     WRAPUV    la mascara usa frac() de la UV
		//   bits 18-31  reservados (0)
		uint32_t trimPack;
		float trimOrigin[4];  // xyz = origen del espacio CUERPO (mundo); w = uTrimIntensity
		float uvRectTrim[4];  // rect de la mascara M1 dentro de su slice del atlas
		// F1 (oleada 4a): multiplicador del alpha del shadow catcher
		// (SHADOW_OPACITY, 0..1). BuildMaterial lo escribe SIEMPRE (1 por
		// defecto), asi que el buffer que sube a GPU no gana un solo byte
		// indeterminado.
		// INTEGRACION shiny-trims <- mi3: en F1 este campo ocupaba el hueco de
		// `pad`, que aqui se lo queda trimPack; pasa al FINAL y los tres pads
		// cierran la fila de 16 B. Layout ESPEJO del PTMaterial HLSL de
		// CinemaKernelM0.hpp (stride 144).
		float shadowOpacity;
		float pad0, pad1, pad2;
	};

	// Luz puntual/spot/area, 64 bytes (CINEMA_DESIGN.md seccion 3). Se conserva en
	// CPU (base del oraculo M1) y el build la sube TAL CUAL como
	// StructuredBuffer de stride 64 (CinemaSceneGpu.lights) para la NEE del
	// kernel M1.
	struct PTLight
	{
		float posx, posy, posz; // posicion en mundo
		float range;            // LIGHT_RANGE
		float colR, colG, colB; // color lineal x LIGHT_STRENGTH (radiancia)
		float sizeRadius;       // point/spot: radio; area: lado del cuadrado
		float dirx, diry, dirz; // spot: eje; area: normal; sin uso en point
		float cosOuter;         // spot: cos(LIGHT_SPOT_RADIUS/2 en radianes); point: -2
		float sharpness;        // LIGHT_SPOT_SHARPNESS
		float fadeSize;         // LIGHT_FADE_SIZE
		uint32_t type;          // 0 point, 1 spot, 2 square area
		float pad;              // siempre 0
	};

	static_assert(sizeof(PTMaterial) == 144, "PTMaterial must be 144 bytes (GPU layout)");
	static_assert(sizeof(PTLight) == 64, "PTLight must be 64 bytes (GPU layout)");

	// M8-A: object->world 3x4 en orden FILA-MAYOR (12 floats), layout compartido
	// byte a byte con la instancia DXR (D3D12_RAYTRACING_INSTANCE_DESC.Transform,
	// float[3][4]) y con el struct HLSL InstXform (StructuredBuffer<InstXform>,
	// stride 48). Elemento (fila r, columna c) = m[r * 4 + c].
	struct CinemaInstXform { float m[12]; };
	static_assert(sizeof(CinemaInstXform) == 48, "CinemaInstXform must be 48 bytes (GPU/DXR layout)");

	// M8-B: clasificacion de un frame nuevo (marker distinto) contra el snapshot
	// del ultimo build instanciado (CinemaSceneClassifyLive).
	enum CinemaLiveClass
	{
		CINEMA_LIVE_REBUILD = 0,   // topologia distinta / no instanciado -> pt_build completo
		CINEMA_LIVE_STATIC = 1,    // nada cambio -> sin trabajo GPU
		CINEMA_LIVE_TRANSFORM = 2, // solo transforms/luces -> update de instancias + TLAS
		CINEMA_LIVE_GEOMETRY = 3   // M8-C: bend deformo verts -> re-flatten local + BLAS parcial + TLAS
	};

	// Convierte una Matrix del motor (almacenamiento COLUMNA-MAYOR, p' = m0*x +
	// m4*y + m8*z + m12) al 3x4 fila-mayor que consumen la TLAS y gEmiO2W.
	inline CinemaInstXform CinemaMatrixToInstXform(const Matrix& w)
	{
		const RealType* s = w.m;
		CinemaInstXform o;
		o.m[0] = (float)s[0]; o.m[1] = (float)s[4]; o.m[2]  = (float)s[8];  o.m[3]  = (float)s[12];
		o.m[4] = (float)s[1]; o.m[5] = (float)s[5]; o.m[6]  = (float)s[9];  o.m[7]  = (float)s[13];
		o.m[8] = (float)s[2]; o.m[9] = (float)s[6]; o.m[10] = (float)s[10]; o.m[11] = (float)s[14];
		return o;
	}

	// --------------------------------------------------------------------
	// Captura
	// --------------------------------------------------------------------

	// Tags de seccion de la pasada de captura (los fija GML con pt_section
	// antes de cada fase; ver spec de natives). Determinan los flags
	// IS_BLOCK / SCENERY / WRAP_UV del material y la politica de AO de
	// vertice (seccion 7.1 del diseno).
	enum CinemaSection : IntType
	{
		CINEMA_SECTION_DEFAULT = 0, // personajes / items / particulas / texto
		CINEMA_SECTION_BLOCK = 1,   // timelines de bloque suelto -> IS_BLOCK
		CINEMA_SECTION_SCENERY = 2, // scenery / mundo importado -> SCENERY | IS_BLOCK
		CINEMA_SECTION_GROUND = 3,  // suelo infinito -> WRAP_UV | IS_BLOCK
		CINEMA_SECTION_CLOUDS = 4   // nubes (toggle, OFF en M0)
	};

	// Snapshot de un draw capturado. Todo queda congelado en el instante del
	// Add: el motor ya resolvio textura/uvRect/uniforms por nosotros
	// (principio de la seccion 4.1: no replicar la seleccion por objeto).
	struct PTDraw
	{
		IntType vbufferId = 0;    // id del VertexBuffer fuente (se re-resuelve en el build)
		Matrix world;             // GFX->matrixM en el instante del draw
		IntType sectionTag = CINEMA_SECTION_DEFAULT;

		// Modo de cull EFECTIVO del rasterizer en el instante del Add
		// (GFX->cinemaCullMode): 0 = none, 1 = front, 2 = back. Replica el
		// backface culling del raster (render_set_culling / SetCullFrontFace)
		// en la traversal del kernel: los modelos "outline" (shell invertido)
		// dependen de que el lado culled NO intersecte, igual que el raster.
		IntType cullMode = 2;     // default BACK (estado inicial del rasterizer)

		// R15 (holdout, "invisible a camara"): lo fija GML por timeline con
		// pt_cam_invisible antes de sus draws (mismo patron que pt_section).
		// Se traduce a MATF_CAM_INVISIBLE: los rayos de CAMARA (primarios, de
		// lente DoF y sus continuaciones por transparencia) atraviesan el draw;
		// sombras, GI, reflejos y NEE de emisores lo siguen viendo.
		IntType camInvisible = 0;

		// F1: MATTE ("holdout que ocluye"). Lo marca pt_cam_matte antes de sus
		// draws (mismo patron que pt_cam_invisible). -> MATF_CAM_MATTE.
		IntType camMatte = 0;

		// F1 (oleada 4a): SHADOW CATCHER. Lo marca pt_shadow_catcher antes de
		// sus draws (mismo patron que pt_cam_matte). La superficie deja de
		// pintar y pasa a escribir en el ALPHA la sombra que recibe:
		// alpha = 1 - L_recibida / L_sin_ocluyentes. -> MATF_SHADOW_CATCHER.
		// shadowOpacity (0..1) multiplica ese alpha; 1 = sombra fisica entera.
		IntType shadowCatcher = 0;
		float shadowOpacity = 1.f;

		// COBERTURA (alpha-as-coverage): lo fija GML por timeline con
		// pt_object_layer antes de sus draws (mismo patron que pt_cam_invisible).
		// 1 = CAPA DE OBJETO (partes de personaje/modelo, items, texto,
		// primitivas): su alpha de textura es COBERTURA y va a MATF_COVERAGE.
		// 0 = geometria de block sheet (BLOCK/SCENERY/MODEL), suelo y nubes: su
		// alpha se sigue tratando como TRANSMISION tenida (cristal de bloque,
		// hielo) o la maneja MATF_WATER. Sin marcar, el comportamiento es el
		// historico: el bit solo ANADE cobertura donde GML la pide.
		IntType objectLayer = 0;

		// CAPAS DE PROFUNDIDAD N: id del OBJETO (timeline RAIZ) dueño de este
		// draw. Lo fija GML con pt_object_id antes de sus draws (mismo patron
		// que pt_section / pt_cam_invisible). 0 = backdrop/sin asignar (suelo
		// infinito, nubes, cualquier draw que GML no marque). Solo lo consume
		// CinemaSceneObjectDepths: NO entra en PTMaterial ni en la GPU, asi
		// que el render es BIT-IDENTICO con o sin este campo.
		IntType objectId = 0;

		// Samplers: 0 = uTexture, 1 = uTextureMaterial, 2 = uTextureNormal.
		// texId es el id RESUELTO (Texture/FrameBuffer, ya no page location);
		// uvRect es el samplerUvRect vigente (local 0-1 -> region de pagina;
		// identidad para texturas sueltas como el block sheet).
		IntType texId[3] = { -1, -1, -1 };
		float uvRect[3][4] = { { 0.f, 0.f, 1.f, 1.f }, { 0.f, 0.f, 1.f, 1.f }, { 0.f, 0.f, 1.f, 1.f } };

		// F4: repeat POR SAMPLER vigente en el raster (Shader::samplerRepeat).
		// El wrapper que el cargador de shaders inyecta hace
		// "if (repeat) uv = mod(uv, 1.0)" sobre la uv LOCAL, ANTES de componerla
		// con el uvRect del sprite (ShaderLoadD3D11.cpp:159, con
		// "mod(a,b) = a - b*floor(a/b)" = frac() exacto para b=1), y
		// render_set_texture lo deja SIEMPRE en true (:68). Sin capturarlo, el
		// kernel muestrea FUERA del rect del sprite en cuanto una UV pasa de 1
		// (primitivas curvas: vbuffer_create_{cylinder,cone,sphere} hacen
		// tex1[X] += 0.25 -> U hasta 1.25) y lee el hueco del packer (negro
		// opaco) o el sprite vecino (color aleatorio segun el packer).
		IntType texRepeat[3] = { 0, 0, 0 };

		// Uniforms del objeto (crudos, sRGB donde aplique; el build linealiza)
		float blend[4] = { 1.f, 1.f, 1.f, 1.f }; // uBlendColor rgb + alpha de objeto
		float emissive = 0.f;        // uEmissive
		float metallic = 0.f;        // uMetallic
		float roughness = 0.8f;      // uRoughness (0.8 = dielectrico mate, diseno seccion 7)
		float defaultEmissive = 1.f; // uDefaultEmissive (se pliega en el byte emissive del vertice al aplanar)
		float sss = 0.f;             // uSSS (flag SUBSURFACE de material)
		IntType twoSided = 0;        // uTwoSided
		IntType isWater = 0;         // uIsWater
		IntType alphaHash = 0;       // uAlphaHash
		IntType materialFormat = 0;  // uMaterialFormat (0 = sin material map)
		IntType useNormalMap = 0;    // uUseNormalMap
		IntType isGround = 0;        // uIsGround (WRAP_UV)

		// TRIM (ShinyTrims). Lo fija GML con pt_trim antes de los draws de la
		// pieza, mismo patron de estado de captura que pt_section /
		// pt_cam_invisible. NO viaja por uniformes: shader_color_fog (el
		// shader de la pasada de CAPTURA) no declara ni uno de los uniformes
		// del trim, asi que preguntarselos devolveria el default en silencio.
		IntType trimFx = 0;           // slot del efecto (0 = sin trim)
		IntType trimOutline = 0;      // esta pieza es la cascara del outline 3D
		float trimIntensity = 1.f;    // uTrimIntensity del usuario
		// Origen del espacio CUERPO: posicion de mundo de la RAIZ del set,
		// COMPARTIDA por las 7 piezas. Cambia CADA FRAME en cuanto el
		// personaje se mueve -- por eso NO entra en la firma de escena (seria
		// un rebuild de ~430 ms por frame) y viaja por el camino vivo.
		float trimOrigin[3] = { 0.f, 0.f, 0.f };
		// Mascara M1: material map de la pieza (alpha = cobertura del trim).
		// texId resuelto + su samplerUvRect, igual que texId[]/uvRect[].
		IntType trimMaskTexId = -1;
		float trimMaskUvRect[4] = { 0.f, 0.f, 1.f, 1.f };
	};

	// Rango de un draw valido dentro de los arrays aplanados. triStart/triCount
	// indexan el array de tris EN ORDEN DE ENTRADA (pre-BVH); para mapear al
	// array reordenado que se sube a GPU usar CinemaBvh::triSource.
	struct PTDrawRange
	{
		IntType drawIdx = 0; // indice en CinemaSceneGetDraws()
		IntType vertStart = 0, vertCount = 0;
		IntType triStart = 0, triCount = 0;
	};

	// --------------------------------------------------------------------
	// Entorno / camara (receptores de pt_env_set / pt_camera_set)
	// --------------------------------------------------------------------

	struct CinemaEnvState
	{
		BoolType valid = false;
		float sunDir[3] = { 0.f, 0.f, 1.f };      // unitaria, HACIA el sol
		float sunRadiance[3] = { 0.f, 0.f, 0.f }; // color lineal x strength (boost/warmth ya aplicados en GML)
		float ambient[3] = { 0.f, 0.f, 0.f };     // color lineal x strength (suelo del domo)
		float skyZenith[3] = { 0.f, 0.f, 0.f };   // lineal
		float skyHorizon[3] = { 0.f, 0.f, 0.f };  // lineal
		float nightAlpha = 0.f;

		// FIX-A (sun_softness): tamano del disco solar, que hasta ahora era una
		// CONSTANTE del host (CINEMA_SUN_ANGLE_DEG = 2 grados de radio) y dejaba
		// muerto el dragger background_sunlight_angle de la UI.
		//   sunAngleDeg = DIAMETRO angular en grados tal cual lo expone MI3
		//     (0.526 = el sol real; misma semantica que render_high_shadows:90 y
		//     que "sun_softness" del asistente). Ya SANEADO por el setter.
		//   sunCosAng  = cos(radio) = cos(radians(sunAngleDeg/2)), lo que el
		//     cbuffer llama sunDir_cosAng.w.
		//   sunOmega   = angulo solido del disco. Se calcula UNA vez aqui, en
		//     doble, con la identidad 1-cos(a) = 2*sin^2(a/2): en float, con el
		//     sol real (radio 0.263 grados) 1-cos vale 1.05e-5 y el ulp junto a
		//     1.0 es 1.19e-7 -> 1 % de error de energia; por debajo de ~0.03
		//     grados daria 0 exacto y la division por omega seria un infinito.
		// Los DOS consumidores (CinemaRender::FillFrame y el espejo CPU de
		// CinemaOracle) leen estos campos: no hay formula duplicada que pueda
		// desincronizarse, que era el riesgo del gate de paridad.
		float sunAngleDeg = 0.f;
		float sunCosAng = 1.f;
		float sunOmega = 0.f;

		BoolType fogShow = false;
		// R12: niebla atmosferica (perspectiva aerea) con la formula EXACTA del
		// raster. fogColor = background_fog_OBJECT_color_final (el que usa
		// shader_high_fog_apply_set, NO el del cielo); fogColorSun = el tono
		// calido del inscatter hacia el sol, merge_color(sunlight_final,
		// fog_object_final, 0.35), calculado en GML igual que el apply.
		float fogColor[3] = { 0.f, 0.f, 0.f };    // lineal
		float fogColorSun[3] = { 0.f, 0.f, 0.f }; // lineal (lobulo Mie)
		float fogDistance = 0.f, fogSize = 0.f, fogHeight = 0.f;

		// F5 (C1): NEE DE ENTORNO. El domo (SkyRadiance) YA existia como dato y
		// el miss lo devolvia, pero solo se alcanzaba por rebote: con
		// bounces = 1 una escena iluminada solo por domo salia NEGRA (medido,
		// 0.0000). envLight lo mete como un grupo mas de la CDF de
		// SampleOtherLight. Semantica del valor:
		//   0   = OFF (default de proyecto) -> pEnv = 0 -> BIT-IDENTICO a antes.
		//   k>0 = peso RELATIVO del grupo entorno frente a (emisivos + luces):
		//         wEnvEff = k * (wEmiEff + wLights), o sea k = 1 reparte las
		//         muestras al 50 %. Sin emisivos ni luces el domo se lleva el
		//         grupo entero. Solo mueve VARIANZA: pEnv entra igual en la pdf
		//         y en el peso MIS, asi que la imagen convergida no cambia --
		//         SALVO a bounces = 1, donde antes no habia entorno en absoluto.
		// Viaja en fogParams1.w del cbuffer (la antigua reserva): PTFrame no
		// crece y no hay bit nuevo de flagsFrame.
		float envLight = 0.f;

		// FONDO DE PELICULA (cinema_backdrop). Un color que la CAMARA ve pero
		// que NO ilumina: lo que hoy se resuelve poniendo un domo blanco, que
		// APLANA al sujeto porque el blanco rebota luz (medido en el hero:
		// -46 % de rango y -32 % de contraste interno).
		//   backdrop 0 = OFF (default) -> BIT-IDENTICO al motor de siempre.
		//            1 = color plano.
		//            2 = rampa lineal en PANTALLA entre color y color2.
		// Los dos colores viven en espacio DISPLAY [0,1] (sRGB, tal cual los
		// eligio el usuario en el selector), NO en lineal: el resolve compone
		// DESPUES del tonemap, asi que un blanco 247 sale en 247 exacto. El
		// angulo es en grados sobre la pantalla, 0 = izquierda->derecha,
		// 90 = arriba->abajo (eje Y hacia abajo, como los pixeles).
		// Solo tiene efecto con film OPACO: con "Remove background" el alpha
		// manda y el fondo no debe ensuciarlo.
		int   backdrop = 0;
		float backdropColor[3] = { 1.f, 1.f, 1.f };
		float backdropColor2[3] = { 1.f, 1.f, 1.f };
		float backdropAngle = 0.f;
		// TILED EXPORT: cada tile es un render ENTERO en su propia superficie, con
		// su sub-frustum off-axis metido en invViewProj (export_update.gml). La
		// rampa se mide en PANTALLA: si se midiera contra el tamano de la
		// SUPERFICIE volveria a empezar en cada tile y el degradado saldria a
		// franjas. Estos cuatro numeros son el rect del tile en pixeles del
		// LIENZO COMPLETO (render_tile_x0/y0 + render_tile_fullw/fullh de GML;
		// x0/y0 pueden ser NEGATIVOS por la banda de guarda).
		// tileFullW/H <= 0 = render normal -> el resolve usa el tamano de la
		// superficie, que es el camino de siempre.
		int   tileX0 = 0, tileY0 = 0, tileFullW = 0, tileFullH = 0;
	};

	// R12: gamma del espacio en el que se MEZCLA la niebla (contrato compartido
	// kernel/oraculo; viaja en fogParams2.w del cbuffer).
	//   > 0 : el mix ocurre en el dominio pow(1/gamma) = espacio DISPLAY, que es
	//         donde lo hace el raster (su surface de escena es gamma-nativa y el
	//         AgX del resolve la lineariza con pow 2.2). Default 2.2 = port 1:1.
	//   = 0 : mix en LINEAL puro (mas denso en sombras que el raster; A/B r14).
	// Override para calibrar: MI3_CINEMA_FOGGAMMA10 (decimas; "0" = lineal).
	float CinemaSceneFogMixGamma();

	struct CinemaCameraState
	{
		BoolType valid = false;
		float viewProj[16] = {};    // view_proj del motor tal cual (debug/calibracion)
		float invViewProj[16] = {}; // inversa, en el MISMO orden de memoria que Matrix::Copy
		                            // (el cbuffer PTFrame la recibe memcpy TAL CUAL, sin transponer;
		                            // ver nota de convencion en CinemaKernelM0.hpp)
		float pos[3] = { 0.f, 0.f, 0.f };
		float nearPlane = 0.f, farPlane = 0.f;

		// DoF fisico de lente fina (efecto retrato). focusDist = distancia de
		// foco en unidades de mundo (CAM_DOF_DEPTH); apertureRadius = radio de
		// la lente en unidades de mundo (calibrado en GML desde CAM_DOF_BLUR_SIZE
		// x focusDist). dofOn=false o apertureRadius=0 => rayo primario IDENTICO
		// byte a byte al pinhole actual (sin coste).
		BoolType dofOn = false;
		float focusDist = 0.f;
		float apertureRadius = 0.f;
		// R9 (FIX 6): autofocus. autofocus=true (CAM_DOF_DEPTH==0) => el foco NO
		// viene de GML: CinemaRenderBegin lanza una sonda de profundidad central
		// y fija focusDist = mediana de los primeros hits, apertureRadius =
		// apertureCoef * focusDist (el diafragma sigue al foco medido para mantener
		// el mismo desenfoque de fondo). autofocus=false => manual: focusDist y
		// apertureRadius vienen ya calculados de GML (CAM_DOF_DEPTH>0). apertureCoef
		// = uBlurSize * 2 * tan(fov/2), constante para reescalar el diafragma tras
		// medir el foco.
		BoolType autofocus = false;
		float apertureCoef = 0.f;

		// DOF-FIEL A1: apertura poligonal (palas de diafragma). blades<3 => disco
		// (camino de siempre, bit-identico); lensRot en RADIANES; lensRatio = 0
		// circular, >0 anamorfico; lensBias = 0 pupila uniforme (bit-identico),
		// >0 apodizacion "edge highlight" (A5).
		float blades = 0.f;
		float lensRot = 0.f;
		float lensRatio = 0.f;
		float lensBias = 0.f;
		// DOF-FIEL A2: eje optico unitario (hacia donde mira la camara), usado
		// para refocalizar por PLANO en vez de por esfera. Derivado de
		// invViewProj (ndc 0,0): fwd = normalize(unproject(0,0,1) - unproject(0,0,0)).
		float camFwd[3] = { 0.f, 0.f, 1.f };

		// R14: proyeccion ORTOGRAFICA (look isometrico). La invViewProj ya trae
		// la matriz ortho del motor, pero el rayo primario NO puede salir de
		// camPos: bajo ortho el origen es el punto del PLANO NEAR del pixel y
		// todas las direcciones son paralelas. Viaja al kernel/oraculo como
		// FRAMEF_ORTHO. DoF se IGNORA con ortho en v1 (la lente fina sobre
		// rayos paralelos no es fisica estandar; ver CinemaSceneCameraSet).
		BoolType ortho = false;
	};

	// Contadores y metricas del ultimo build (pt_status).
	struct CinemaSceneStats
	{
		IntType draws = 0;         // draws capturados
		IntType validDraws = 0;    // draws que entraron al flatten
		IntType missingCpu = 0;    // draws sin geometria recuperable (seccion 4.4)
		IntType readbackDraws = 0;   // R13: draws salvados leyendo la GPU
		IntType readbackMeshes = 0;  // R13: mallas reconstruidas por readback
		IntType skippedNoShader = 0; // draws capturados sin shader activo
		IntType verts = 0, tris = 0, nodes = 0, materials = 0, lights = 0;
		IntType atlasSlices = 0;
		IntType texturesMissing = 0; // texIds no resolubles a QImage
		IntType emiTris = 0;         // triangulos emisores en la CDF de NEE (M1)
		RealType emiPower = 0;       // potencia emisora total (area mundo x luminancia media x e), SIN emissive_scale
		int64_t bytesVerts = 0, bytesTris = 0, bytesNodes = 0, bytesMaterials = 0, bytesAtlas = 0;
		int64_t bytesReadback = 0;   // R13: bytes reconstruidos desde la GPU
		int64_t bytesReadbackFreed = 0; // R18: copias CPU del readback liberadas tras el flatten
		RealType msFlatten = 0, msAtlas = 0, msBvh = 0, msUpload = 0, msTotal = 0;
		RealType msReadback = 0;     // R13: coste del readback (una vez por malla)

		// R18 (escenas gigantes): recorte por DISTANCIA a la camara + presupuesto.
		RealType clipRadius = 0;     // radio efectivo del recorte (0 = OFF)
		IntType clipTrisIn = 0;      // triangulos que entraron al flatten
		IntType clipTrisOut = 0;     // triangulos descartados por distancia
		IntType clipVertsIn = 0;     // vertices que entraron
		IntType clipVertsOut = 0;    // vertices descartados con ellos
		RealType msClip = 0;         // coste del conteo del recorte
		int64_t bytesEstimated = 0;  // memoria CPU estimada del build ANTES de construir
		IntType memAvailMB = 0;      // RAM fisica disponible al estimar (-1 = desconocido)
		IntType memProcessMB = 0;    // working set del proceso al terminar el build
		IntType vramAvailMB = 0;     // VRAM disponible al estimar (-1 = desconocido)
	};

	// --------------------------------------------------------------------
	// API de captura (hook + natives pt_begin/pt_end/pt_section)
	// --------------------------------------------------------------------

	// Flag global leido por el hook de VertexBufferRenderer::Add (coste cero
	// cuando esta apagado). Gestionado SOLO por CinemaSceneCaptureBegin/End.
	extern BoolType cinemaSceneCaptureActive;

	// Retencion de copias CPU: mientras esta activo (lo gestiona la captura),
	// VertexBuffer::Complete NO libera vertexData/indexData de buffers
	// grandes. Permite reconstruir vbuffers procedurales (p.ej. las nubes)
	// DENTRO de la pasada de captura y que el flatten los pueda leer.
	extern BoolType cinemaRetainVbuffers;

	// TRUE si la captura esta activa Y el render target actual sigue siendo
	// el que habia al empezar la captura. El hook de VertexBufferRenderer::Add
	// debe usar ESTO (no cinemaSceneCaptureActive a secas): los draws hacia
	// surfaces temporales durante la captura (p.ej. background_sky_update_clouds
	// rasteriza su textura a una surface para leer los pixeles) NO son
	// geometria de escena y deben rasterizar de verdad.
	BoolType CinemaSceneCaptureWantsDraw();

	// Activa la captura: limpia draws y luces pendientes y resetea la seccion
	// a DEFAULT. Falla (WARNING + estado de error consultable en pt_status)
	// si el hardware no soporta Cinema (feature level < 11_0 u OpenGL).
	void CinemaSceneCaptureBegin();

	// Desactiva la captura (los draws quedan pendientes para el build).
	void CinemaSceneCaptureEnd();

	// Fija el tag de seccion de los draws que se capturen a continuacion.
	void CinemaSceneCaptureSetSection(IntType tag);
	// Marca los draws siguientes como CAPA DE OBJETO (ver PTDraw::objectLayer).
	void CinemaSceneCaptureSetObjectLayer(BoolType on);

	// R15: marca los draws que se capturen a continuacion como "invisibles a
	// camara" (holdout). Estado de captura, igual que la seccion; se resetea a
	// false en CinemaSceneCaptureBegin.
	void CinemaSceneCaptureSetCamInvisible(BoolType on);

	// F1: marca los draws que se capturen a continuacion como MATTE (holdout
	// que OCLUYE). Mismo patron de estado de captura, se resetea a false en
	// CinemaSceneCaptureBegin. -> MATF_CAM_MATTE.
	void CinemaSceneCaptureSetCamMatte(BoolType on);

	// F1 (oleada 4a): marca los draws que se capturen a continuacion como
	// SHADOW CATCHER, con su opacidad de sombra (0..1). Mismo patron de estado
	// de captura; se resetea a (off, 1.0) en CinemaSceneCaptureBegin.
	// -> MATF_SHADOW_CATCHER + PTMaterial::shadowOpacity.
	void CinemaSceneCaptureSetShadowCatcher(BoolType on, RealType opacity);

	// CAPAS N: id de objeto de los draws que se capturen a continuacion.
	void CinemaSceneCaptureSetObjectId(IntType id);

	// FIX-B: emisividad (uEmissive del raster) de los draws que se capturen a
	// continuacion. Estado de captura, igual que la seccion; se resetea a 0 en
	// CinemaSceneCaptureBegin. -> PTDraw::emissive -> PTMaterial::emissive.
	// Existe porque el shader de la captura (shader_color_fog) NO declara
	// uEmissive y render_set_uniform es SILENCIOSA con los uniforms que no
	// existen: leerlo por nombre en CaptureRecord devolvia siempre 0.
	void CinemaSceneCaptureSetEmissive(RealType v);

	// TRIM (ShinyTrims): efecto de los draws que se capturen a continuacion.
	// fx = 0 lo APAGA (y es el estado inicial de cada captura). maskTexId es
	// el material map de la pieza, cuyo ALPHA es la cobertura del trim; sin
	// mascara el trim no se evalua (cobertura 0) y el build lo CUENTA.
	void CinemaSceneCaptureSetTrim(IntType fx, RealType intensity,
		RealType originX, RealType originY, RealType originZ,
		IntType outline, IntType maskTexId);

	// TRIM: estado de ESCENA (una vez por frame, desde render_cinema_env_camera).
	// El TIEMPO viaja aqui, como constante, resuelto en GML a partir del
	// timeline: es P2 (determinismo) y P5 (cero rebuilds) a la vez.
	void CinemaSceneTrimScene(RealType tick0, RealType uTime, RealType loopL,
		RealType sceneLight, RealType bodyScale, RealType emissive,
		IntType noiseTexId, RealType noiseSize, IntType ldrClip);

	// TRIM: estado de escena para el relleno del cbuffer y el diagnostico.
	struct CinemaTrimState
	{
		float tick0 = 0.f;
		float uTime = 0.f;
		float loopL = 0.f;
		float sceneLight = 0.f;
		float bodyScale = 1.f;
		float emissive = 1.f;
		IntType noiseTexId = -1;
		float noiseSize = 128.f;
		IntType ldrClip = 0;
		// Rellenado por el build: rect/slice del ruido dentro del atlas.
		float noiseRect[4] = { 0.f, 0.f, 1.f, 1.f };
		uint32_t noiseSlice = 0;
		// Diagnostico del build (lo rinde el acta): piezas con fx > 0 pero sin
		// mascara resoluble -- su trim NO se ve y hay que saberlo.
		IntType drawsWithTrim = 0;
		IntType drawsTrimNoMask = 0;
	};
	const CinemaTrimState& CinemaSceneTrim();

	// TRIM: conjunto ORDENADO de slots de efecto presentes en la escena
	// construida. Lo consume el compositor del kernel (D10).
	const std::vector<uint32_t>& CinemaSceneTrimSlots();

	// Registra un draw. La llama SOLO el hook de VertexBufferRenderer::Add
	// mientras cinemaSceneCaptureActive es true. No rasteriza nada.
	void CinemaSceneCaptureRecord(VertexBuffer* buffer);

	// --------------------------------------------------------------------
	// API de escena (natives pt_env_set / pt_light_add / pt_camera_set)
	// --------------------------------------------------------------------

	// Colores GML empaquetados 0xBBGGRR (byte 0 = rojo); se linealizan aqui.
	void CinemaSceneEnvSet(
		RealType sunDirX, RealType sunDirY, RealType sunDirZ,
		IntType sunColor, RealType sunStrength,
		IntType ambientColor, RealType ambientStrength,
		IntType skyZenithColor, IntType skyHorizonColor,
		RealType nightAlpha,
		BoolType fogShow, IntType fogColor, IntType fogColorSun,
		RealType fogDistance, RealType fogSize, RealType fogHeight,
		// FIX-A: DIAMETRO angular del sol en grados (background_sunlight_angle).
		RealType sunAngleDeg,
		// F5 (C1): project_cinema_env_light. 0 = OFF (default, camino
		// BIT-IDENTICO); > 0 = el domo entra como grupo muestreable en la NEE
		// de "otra fuente" y el valor es su peso RELATIVO frente al resto.
		RealType envLight,
		// FONDO DE PELICULA: project_cinema_backdrop (0 = OFF, 1 = plano,
		// 2 = rampa), los dos colores GML 0xBBGGRR (se quedan en DISPLAY: el
		// resolve compone despues del tonemap) y el angulo de la rampa en
		// grados. Al final por compatibilidad de orden, como sunAngleDeg y
		// envLight.
		IntType backdrop, IntType backdropColor, IntType backdropColor2,
		RealType backdropAngle,
		// TILED EXPORT: rect del tile en pixeles del LIENZO COMPLETO. Todo a 0 =
		// render normal (el resolve usa el tamano de la superficie).
		IntType tileX0, IntType tileY0, IntType tileFullW, IntType tileFullH);

	// Anade una luz point/spot/area; devuelve el numero de luces tras anadir.
	// spotRadius es el angulo COMPLETO del cono en grados (LIGHT_SPOT_RADIUS,
	// el mismo que render_light_fov); cosOuter = cos(radianes(spotRadius)/2).
	IntType CinemaSceneLightAdd(
		RealType x, RealType y, RealType z,
		RealType dirX, RealType dirY, RealType dirZ,
		RealType range, IntType color, RealType strength,
		RealType size, RealType fadeSize,
		RealType spotRadius, RealType spotSharpness, BoolType isSpot,
		IntType typeOverride = -1);

	// Fija la camara: view_proj del motor (con el sub-frustum off-axis del
	// tiled export dentro) + posicion + near/far. La inversa se calcula aqui
	// y se verifica multiplicando de vuelta (si no da identidad: WARNING y
	// camara invalida).
	// DOF-FIEL A3/A4: apertureCoef llega YA CALCULADO desde GML (uBlurSize * 2 *
	// tan(fov/2); ya NO se deriva de apertureRadius/focusDist -- con autofocus
	// focusDist llega en 0 y esa division seria indeterminada). blades/lensRot/
	// lensRatio/lensBias son los controles de apertura poligonal de A1/A5 (todos
	// 0 => disco uniforme, camino bit-identico al de antes de esta ronda).
	void CinemaSceneCameraSet(const Matrix& viewProj,
		RealType camX, RealType camY, RealType camZ,
		RealType zNear, RealType zFar,
		RealType focusDist, RealType apertureRadius, RealType apertureCoef, BoolType dofOn, BoolType autofocus = false,
		BoolType ortho = false,
		RealType blades = 0.0, RealType lensRot = 0.0, RealType lensRatio = 0.0, RealType lensBias = 0.0);

	// R9 (FIX 6): fija el foco medido por la sonda de autofocus (CinemaRenderBegin)
	// y reescala el diafragma con apertureCoef. No-op con focusDist<=0.
	void CinemaSceneCameraApplyAutofocus(RealType focusDist);

	// DOF-FIEL A3 nivel 3 ("sin geometria visible"): el autofocus se rindio (ni
	// sonda ni AABB de la escena dentro de near/far) -> apaga el DoF para ESTE
	// render (focusDist/apertureRadius ya estaban en 0). No toca nada mas.
	void CinemaSceneCameraDisableDof();

	// F2: lectura del DoF EFECTIVO de la camara ya saneada (y ya sobrescrita
	// por la sonda de autofocus si corrio). Solo lectura: no altera el render.
	RealType CinemaSceneCameraFocusDist();
	RealType CinemaSceneCameraAperture();

	// --------------------------------------------------------------------
	// Build / estado (natives pt_build / pt_status / pt_invalidate)
	// --------------------------------------------------------------------

	// Orquesta flatten + atlas + BVH + upload GPU. vertexAo es
	// project_cinema_vertex_ao (0 = ignorar el AO vanilla horneado en el RGB
	// de vertice de scenery/bloques, seccion 7.1). Devuelve "" si OK o un
	// codigo de error estable: "cinema_requires_fl11", "no_draws",
	// "cpu_data_missing:<n>", "bvh_build_failed", "gpu_alloc_failed".
	//
	// R9 (FIX 1): deferBvh = 1 SALTA la construccion del arbol BVH de CPU (~3.5 s
	// en escenas grandes), el grueso del "tarda en cargar" del preview. Solo
	// tiene efecto si el backend efectivo es DXR (que traza por la BLAS de
	// hardware y NO usa el BVH de CPU). En ese caso los tris quedan en ORDEN DE
	// ENTRADA (sin reordenar), la CDF emisiva se construye en ese mismo orden y
	// la BLAS se arma desde ese array -> render CORRECTO (indices consistentes),
	// pero el patron de ruido de la NEE difiere del build completo (converge al
	// mismo resultado). Se usa SOLO en el preview interactivo; export/final/
	// headless/parity siempre pasan deferBvh=0 (build completo, reordenado, con
	// resultado BIT-IDENTICO al de siempre). Si mas tarde se cae a D3D11 (BVH de
	// CPU obligatorio) se completa con CinemaSceneEnsureFullBuild().
	QString CinemaSceneBuild(RealType vertexAo, BoolType deferBvh = false);

	// R9 (FIX 1): si el ultimo build fue diferido (sin arbol BVH de CPU),
	// construye AHORA el BVH completo (reordena tris, re-arma la CDF emisiva en
	// orden de hoja y re-sube los buffers D3D11). Necesario para el oraculo/
	// paridad y para el fallback D3D11 (que trazan por el BVH de CPU). No-op si
	// el build ya era completo. Devuelve "" si OK o un codigo de error.
	QString CinemaSceneEnsureFullBuild();

	// R9 (FIX 1): true si el ultimo build valido fue diferido (BVH de CPU aun
	// sin construir). CinemaRender lo consulta para completar el build antes de
	// usar el camino D3D11.
	BoolType CinemaSceneIsBvhDeferred();

	// M8-A: toggle MI3_CINEMA_INSTANCED (env var, cacheado; default ON). ON =
	// captura en espacio LOCAL + instancias TLAS (fase A: equivalencia estatica).
	// OFF = camino de siempre (una BLAS con verts en MUNDO, instancia identidad).
	// Solo afecta al backend DXR: D3D11 y el oraculo trazan siempre la escena
	// MUNDO. Con OFF la salida es BIT-IDENTICA al camino previo a M8.
	BoolType CinemaSceneInstancedEnabled();

	// M8-A: true si el ULTIMO build genero la representacion local + instancias
	// (toggle ON y build valido). CinemaGpu12 lo consulta para elegir el camino.
	BoolType CinemaSceneIsInstanced();

	// Libera escena CPU + buffers GPU y vuelve al estado vacio (recaptura
	// forzada en el siguiente render).
	void CinemaSceneInvalidate();

	// Estado en pares "clave=valor" separados por espacio (parseable simple
	// desde GML). Claves: state (empty|capturing|captured|built|error), error, draws,
	// valid_draws, missing_cpu, skipped_no_shader, verts, tris, nodes,
	// materials, lights, lights_gpu, emi_tris, emi_power, atlas_slices,
	// textures_missing, camera, env, bytes_verts, bytes_tris, bytes_nodes,
	// bytes_materials, bytes_atlas, bytes_total, readback_draws,
	// readback_meshes, bytes_readback, ms_readback, ms_flatten, ms_atlas,
	// ms_bvh, ms_upload, ms_total.
	QString CinemaSceneStatus();

	// R13: interruptor del readback GPU del flatten (ON por defecto). Apagarlo
	// devuelve el comportamiento historico (omitir los draws sin copia CPU con
	// WARNING) — es el A/B de verificacion y la valvula de emergencia.
	void CinemaSceneSetReadback(BoolType on);

	// R13 (solo harness/diagnostico): libera la copia CPU de los vbuffers
	// grandes, reproduciendo el estado que deja el motor tras escribir un
	// .meshcache. Devuelve el numero de mallas liberadas.
	IntType CinemaSceneDebugFreeCpuMeshes();

	// R18 (escenas GIGANTES) — recorte por DISTANCIA a la camara.
	// radius <= 0 = OFF: camino historico, BIT-IDENTICO (ni siquiera se evalua
	// el predicado). radius > 0: en el FLATTEN se descarta todo triangulo cuyo
	// AABB de mundo quede a mas de 'radius' del ojo, y con el los vertices que
	// solo el usaba. Es el MISMO criterio con el que el raster decide hasta
	// donde dibuja (project_render_distance), no un culling por frustum: la
	// geometria fuera de cuadro pero cerca SIGUE proyectando sombras, aportando
	// GI y apareciendo en reflejos (recortar por frustum romperia justo eso).
	// Las secciones GROUND y CLOUDS quedan EXENTAS (son un punado de triangulos
	// gigantes que cubren el mundo entero: su AABB siempre intersecaria, y
	// ademas su coste es nulo). Lo fija GML (pt_clip) antes de pt_build.
	void CinemaSceneSetClipRadius(RealType radius);

	// R18: radio de recorte actualmente configurado (0 = OFF).
	RealType CinemaSceneGetClipRadius();

	// R18: RAM fisica disponible en MB (-1 si no se puede consultar) y working
	// set del proceso en MB (-1 idem). Los usa el guard de presupuesto y el log.
	IntType CinemaSceneMemAvailableMB();
	IntType CinemaSceneMemProcessMB();

	// R18 (solo harness/diagnostico): fija a mano el presupuesto de memoria del
	// build en MB (0 = medirlo del sistema, el default). Permite probar el
	// aborto limpio de forma determinista sin depender de la RAM de la maquina.
	void CinemaSceneSetMemLimitMB(RealType mb);

	// --------------------------------------------------------------------
	// Lectura para CinemaRender / tests
	// --------------------------------------------------------------------

	const CinemaEnvState& CinemaSceneEnv();
	const CinemaCameraState& CinemaSceneCamera();
	const std::vector<PTLight>& CinemaSceneLights();
	const CinemaSceneStats& CinemaSceneGetStats();
	const std::vector<PTDraw>& CinemaSceneGetDraws();
	const std::vector<PTDrawRange>& CinemaSceneGetDrawRanges();

	// CAPAS N: estadistica de PROFUNDIDAD POR OBJETO del ultimo build valido.
	// Lanza una sonda PINHOLE de CPU (probeW x probeH, centro de pixel, sin AA
	// ni DoF) contra el BVH de CPU y agrega por PTDraw::objectId la distancia
	// RADIAL del primer impacto. Formato de retorno: ver CinemaScene.cpp.
	QString CinemaSceneObjectDepths(IntType probeW, IntType probeH);

	// CAPAS N v2 (D2): percentiles de profundidad POR BANDA calculados sobre la
	// CACHE POR RAYO de la ultima CinemaSceneObjectDepths (mismos rayos, mismo
	// orden: el resultado es comparable pixel a pixel con el mapa de
	// profundidad). bandMap = "id:banda,id:banda,..." (ids ausentes -> banda 0);
	// skyDist = distancia asignada a los rayos SIN impacto (usar el "far" del
	// _depth.json). Formato de retorno: ver CinemaScene.cpp.
	QString CinemaSceneBandDepths(const QString& bandMap, float skyDist);

	// CAPAS SLICING (S1): ventana de profundidad del segmento de CAMARA para la
	// pasada de capa en curso. La fija GML por pasada (pt_band_window) y la leen
	// FillFrame (GPU) y CinemaOracleRender (CPU). OFF por defecto => todo el
	// camino de render queda BIT-IDENTICO. t0 <= 0 -> sin corte cercano; t1 <= 0
	// -> sin corte lejano (+inf); t1 <= t0 con ambos > 0 es una ventana VACIA:
	// se rechaza y se apaga (mejor una capa de mas que un export negro).
	void CinemaSceneBandWindowSet(float t0, float t1);
	BoolType CinemaSceneBandWindowActive();
	float    CinemaSceneBandWindowT0();
	float    CinemaSceneBandWindowT1();

	// CAPAS SLICING: que OBJETOS son visibles en cada capa de corte, sobre la
	// cache POR RAYO de la ultima CinemaSceneObjectDepths (capasProbeObj/
	// capasProbeDist). Coste de trazado CERO. cutsCsv = "s1,s2,...,s{N-1}" en
	// unidades de MUNDO, ASCENDENTE (los cortes de la planificacion). Formato
	// de retorno: ver CinemaScene.cpp.
	QString CinemaSceneObjectBands(const QString& cutsCsv);

	// T6: cache POR RAYO de la ultima CinemaSceneObjectDepths (paralelas, tamano
	// probeW*probeH, orden fila-mayor y MISMOS centros de pixel en NDC que el
	// mapa de profundidad). dist <= 0 = cielo. La usa CinemaRenderProbeAgree
	// (CinemaRender.cpp) para comparar rayo a rayo contra el mapa de profundidad.
	void CinemaSceneGetProbeCache(const std::vector<IntType>** objs,
		const std::vector<float>** dists, IntType* w, IntType* h);

	// Escena CPU (base del oraculo de M1 y de los golden tests). Los verts
	// estan en orden de entrada; el BVH guarda los tris reordenados por hoja
	// (los que se suben a GPU) y triSource para mapear al orden original.
	const CinemaBvh& CinemaSceneGetBvh();
	const std::vector<PTVert>& CinemaSceneGetVerts();
	const std::vector<PTMaterial>& CinemaSceneGetMaterials();

	// CDF de triangulos emisores para la NEE de M1 (arrays paralelos).
	// emiTriIdx indexa el array REORDENADO de tris (CinemaBvh::tris, el mismo
	// que ve la GPU como gTris); emiCdf es la CDF ACUMULADA por potencia
	// (area mundo x luminancia media aprox del albedo x emissive), SIN
	// normalizar: emiCdf.back() == potencia total. El muestreo hace busqueda
	// binaria de r * potenciaTotal. La decision "es emisor" replica la logica
	// de emision del kernel (EvalSurface: e = max(mat.emissive, byte emissive
	// del vertice, con uDefaultEmissive ya plegado por el flatten)). La
	// escala global project_cinema_emissive_scale NO esta incluida (no altera
	// la distribucion relativa; la aplica el kernel via res_clamp.w).
	const std::vector<uint32_t>& CinemaSceneGetEmissiveTris();
	const std::vector<float>& CinemaSceneGetEmissiveCdf();
	RealType CinemaSceneGetEmissivePower();

	// M8-A: representacion LOCAL + instancias (solo valida con IsInstanced()).
	//   VertsLocal: verts en espacio LOCAL (para las BLAS por-draw).
	//   TrisLocal : tris winding-fuente, orden de ENTRADA por-draw (usar ranges
	//               de CinemaSceneGetDrawRanges: triStart/triCount son el rango
	//               en ESTE array; una BLAS por rango). gTris del kernel = este.
	//   EmissiveTrisLocal/CdfLocal: CDF emisiva paralela que indexa TrisLocal.
	//   EmissiveO2W: object->world por emisor (paralela a EmissiveTrisLocal).
	const std::vector<PTVert>& CinemaSceneGetVertsLocal();
	const std::vector<PTTri>& CinemaSceneGetTrisLocal();
	const std::vector<uint32_t>& CinemaSceneGetEmissiveTrisLocal();
	const std::vector<float>& CinemaSceneGetEmissiveCdfLocal();
	const std::vector<CinemaInstXform>& CinemaSceneGetEmissiveO2W();

	// M8-B/C ("escena viva"): clasifica el frame RECIEN capturado contra el snapshot
	// del ultimo build. Produce el world nuevo por instancia (outWorlds, paralelo a
	// las draw-ranges) y el o2w nuevo por emisor (outEmiO2W, paralelo a la CDF emisiva
	// local). M8-C: outChangedDraws recibe los indices de instancia cuya GEOMETRIA
	// cambio (bend) y que ya fueron re-aplanados en scene.vertsLocal -> el caller hace
	// update PARCIAL de esos verts+BLAS (clase CINEMA_LIVE_GEOMETRY). Ver CinemaScene.cpp.
	CinemaLiveClass CinemaSceneClassifyLive(std::vector<CinemaInstXform>& outWorlds,
		std::vector<CinemaInstXform>& outEmiO2W, std::vector<int>& outChangedDraws);

	// M8-B: true si un update en vivo dejo los verts MUNDO (CPU) desfasados. El
	// oraculo/fallback D3D11/export deben forzar rebuild completo antes de usarlos.
	BoolType CinemaSceneWorldStale();

	// M8-B: si worldStale, re-aplana los draws frescos (build completo) para dejar
	// los verts/BVH MUNDO consistentes; no-op si no hay stale. Lo llama el fallback
	// D3D11 antes de trazar por el BVH de CPU. Devuelve "" o un codigo de error.
	QString CinemaSceneRebuildIfWorldStale();

	// Atlas CPU del ultimo build: slices QImage RGBA8888 4096x4096 con los
	// MISMOS bytes que la Texture2DArray SRGB subida a GPU (el slice de una
	// TexturePage comparte buffer con la pagina via implicit sharing de Qt).
	// La retencion es estructural: los slices solo se liberan al empezar el
	// siguiente build o en CinemaSceneInvalidate(), asi que el oraculo CPU de
	// M1 (CinemaOracle) muestrea texels nearest sin flag adicional. Vacio si
	// no hay build valido.
	const QVector<QImage>& CinemaSceneGetAtlasSlices();

#if API_D3D11
	// Recursos GPU del ultimo build correcto. CinemaRender los liga en los
	// slots SLOT_SRV_* de CinemaKernelM0.hpp (t0 verts, t1 tris, t2 nodos,
	// t3 materiales, t4 atlas; el kernel M1 anade los slots para emiTriIdx,
	// emiCdf y lights). Propiedad de CinemaScene: nadie mas libera.
	//
	// lights/emiTriIdx/emiCdf son OPCIONALES: quedan nullptr con conteo 0
	// cuando la escena no tiene luces o emisores (CinemaRender pasa los
	// conteos en PTFrame.counts y el kernel no lee los buffers si son 0).
	struct CinemaSceneGpu
	{
		ID3D11Buffer* verts = nullptr;
		ID3D11Buffer* tris = nullptr;
		ID3D11Buffer* nodes = nullptr;
		ID3D11Buffer* materials = nullptr;
		ID3D11Buffer* lights = nullptr;     // StructuredBuffer<PTLight>, stride 64
		ID3D11Buffer* emiTriIdx = nullptr;  // StructuredBuffer<uint>, stride 4 (indices en gTris)
		ID3D11Buffer* emiCdf = nullptr;     // StructuredBuffer<float>, stride 4 (CDF acumulada, paralela)
		ID3D11Texture2D* atlas = nullptr;
		ID3D11ShaderResourceView* vertsSrv = nullptr;
		ID3D11ShaderResourceView* trisSrv = nullptr;
		ID3D11ShaderResourceView* nodesSrv = nullptr;
		ID3D11ShaderResourceView* materialsSrv = nullptr;
		ID3D11ShaderResourceView* lightsSrv = nullptr;
		ID3D11ShaderResourceView* emiTriIdxSrv = nullptr;
		ID3D11ShaderResourceView* emiCdfSrv = nullptr;
		ID3D11ShaderResourceView* atlasSrv = nullptr;
		UINT numVerts = 0, numTris = 0, numNodes = 0, numMaterials = 0, numSlices = 0;
		UINT numLights = 0;   // PTFrame.counts.x (M1)
		UINT numEmiTris = 0;  // PTFrame.counts.y (M1)
		// FIX race capas (bg==full): GENERACION monotona del build. Se bumpea en
		// cada upload GPU completo. CinemaGpu12EnsureScene la usa como firma: la
		// identidad de puntero del SRV NO es una generacion (el allocator puede
		// devolver la MISMA direccion tras liberar el SRV viejo) y los conteos son
		// identicos entre pasadas del export por capas (solo cambian flags de
		// material) -> ~1/14 el DXR reusaba la escena de la pasada ANTERIOR y la
		// capa salia byte-identica a la previa (seed fijo).
		uint64_t buildGen = 0;
		float emiPowerTotal = 0.f; // == emiCdf[numEmiTris-1]; sin emissive_scale

		BoolType IsValid() const
		{
			// Las luces y la CDF emisiva no condicionan la validez: una escena
			// sin luces/emisores es valida (buffers nullptr, conteo 0).
			return vertsSrv && trisSrv && nodesSrv && materialsSrv && atlasSrv;
		}
	};

	const CinemaSceneGpu& CinemaSceneGetGpu();
#endif
}
