#pragma once

// ---------------------------------------------------------------------------
// CinemaKernelM0.hpp -- kernel HLSL (cs_5_0) del motor Cinema (path tracer,
// project_render_engine = 2), evolucionado EN SITIO al alcance M2 (el nombre
// de archivo/namespace se conserva por estabilidad de includes). Contrato:
// HEADLESS/CINEMA_DESIGN.md, secciones 3 (estructuras), 5 (sombras), 6
// (integrador NEE+MIS), 7 (materiales) y 13 (alcances M1/M2).
//
// Alcance M2 sobre el M1 (seccion 7 del diseno):
//   - GGX single-scatter (VNDF Heitz 2018, Smith height-correlated) mezclado
//     con Lambert por peso de Fresnel; metallic/roughness del draw o de
//     uTextureMaterial (SEUS: rough = 1-R, metal = G; LabPBR: rough =
//     (1-R)^2, metal = G > 229/255; MISMOS decodes que getMaterial del
//     shader_high_material.fsh). F0 dielectrico 0.04, metal F0 = albedo.
//   - Normal maps (uTextureNormal sobre tangent888 mundo + bitangente
//     cross(T, N), decode Y-flip IDENTICO a getMappedNormal del raster).
//   - Agua (MATF_WATER): dielectrico IOR 1.33 con Fresnel exacto + TIR,
//     reflexion/refraccion delta sobre micro-normal VNDF (roughness 0.07),
//     absorcion Beer-Lambert por distancia DENTRO del agua (sigma derivada
//     del blendColor, 1 bloque = 16 unidades de referencia).
//   - Cristal DEPTH2 (MATF_ALPHA_BLEND sin WATER): thin-surface delta,
//     reflexion Fresnel (F0 0.04) + transmision recta tintada por
//     texel.rgb * blend.rgb * (1 - texel.a * blend.a) -- EXACTAMENTE el
//     factor por capa del TraceShadow M1 (sombra coloreada consistente).
//     Texels con alpha efectiva ~ 1 (marcos opacos del panel) shadean como
//     el kind estandar (el raster los pinta opacos y la sombra los ocluye).
//   - Hojas (MATF_SUBSURFACE o bit 2 del vertice): thin-translucent, lobulo
//     difuso de reflexion (peso 0.7) + lobulo difuso de transmision (peso
//     0.3): el glow de copas a contraluz. Reciben NEE por ambos lados.
//   - TWO_SIDED: cubierto por la politica global M0/M1 de voltear normales
//     hacia el rayo (el flag MATF_TWO_SIDED queda implicito).
//   - MIS/NEE: los lobulos DELTA (agua/cristal) no reciben NEE ni pesan MIS
//     (tras un rebote delta, emision y disco solar entran con peso 1). El
//     resto (Lambert+GGX, hojas) recibe NEE de sol y de "otra fuente" con
//     MIS balance contra la pdf de MEZCLA del BSDF. GGX nunca es delta
//     (floor de roughness PT_ROUGH_MIN): su MIS siempre esta definido.
//
// Este header SOLO contiene el fuente HLSL embebido como raw string ASCII y
// las constantes de contrato (strides, slots, entry points) que el host
// (CinemaGpu/CinemaRender) debe respetar. No hay codigo runtime aqui.
//
// Contrato de compilacion:
//   - Target "cs_5_0" via D3DCompile (d3dcompiler ya linkeado en el repo).
//   - Recomendado D3DCOMPILE_IEEE_STRICTNESS: el kernel usa isfinite() para
//     garantizar cero NaN/inf en el acumulador (criterio (e) de M0); sin
//     strictness el optimizador puede eliminar el chequeo.
//   - Dos entry points sobre el MISMO fuente: CSPathTrace y CSClear.
//
// Contrato de bindings (constantes SLOT_* abajo):
//   b0: cbuffer PTFrame (240 bytes exactos)
//   t0: StructuredBuffer<PTVert>     stride 48 (layout de CinemaBvh.hpp)
//   t1: StructuredBuffer<PTTri>      stride 16
//   t2: StructuredBuffer<PTNode>     stride 32 (leaf bit 31, CinemaBvh.hpp)
//   t3: StructuredBuffer<PTMaterial> stride 96
//   t4: Texture2DArray RGBA8 SRGB (atlas; SampleLevel devuelve lineal gratis)
//   t5: StructuredBuffer<PTLight>    stride 64 (CinemaScene.hpp; counts.x)
//   t6: StructuredBuffer<uint>       emiTriIdx: indices de tri EMISOR en el
//       array REORDENADO por hoja (el que ve la GPU), ORDEN ASCENDENTE
//       obligatorio (el kernel hace busqueda binaria de pertenencia para el
//       peso MIS de los hits de BSDF); counts.y elementos
//   t7: StructuredBuffer<float>      emiCdf: CDF por potencia SIN normalizar,
//       paralela a t6 (emiCdf[i] = suma acumulada de potencia de los tris
//       de la lista hasta i; el ultimo elemento es el total W_emi).
//       Contrato de potencia por tri (el que IMPLEMENTA CinemaScene paso 4b
//       y el oraculo debe replicar):
//         powerTri = areaMundo * lumRec709(albedoMedioLineal) * e_combinado
//       con lumRec709 = dot(c, (0.2126, 0.7152, 0.0722)) y e_combinado =
//       max(mat.emissive, mediaByteEmissiveVertices). SIN emissive_scale ni
//       pi: la CDF solo pesa la DISTRIBUCION dentro del grupo (cualquier
//       factor global se cancela); para el reparto de muestras CONTRA las
//       luces analiticas el kernel usa la potencia efectiva
//       W_emi * emissiveScale * pi (solo varianza, nunca sesgo).
//   s0: sampler point (MIN_MAG_MIP_POINT, address CLAMP)
//   u0: RWStructuredBuffer<float4> acumulador (rgb = suma color, w = suma alpha)
//   u1: RWStructuredBuffer<float4> AOV albedo (rgb = suma, w = num samples)
//   u2: RWStructuredBuffer<float4> AOV normal+depth (xyz = suma normal, w = suma t)
//
// IMPORTANTE (por que los UAVs son buffers y no Texture2D RGBA32F): en D3D11
// FL 11_0 los typed UAV loads solo estan permitidos para formatos de UN canal
// de 32 bits (error de compilacion X3676 con RWTexture2D<float4>). El
// acumulador necesita leer-modificar-escribir (+=), asi que los tres UAVs son
// RWStructuredBuffer<float4> de width*height elementos, indexados
// pix = y * width + x. El readback es copia staging de BUFFER (no de textura).
//
// Convencion de matriz (VERIFICADA contra el raster): el cbuffer declara
// invViewProj SIN row_major y CinemaGpu::CreateCS no pasa PACK_MATRIX_*,
// asi que rige el packing column_major por defecto de HLSL. El Matrix del
// motor guarda m[c*4+r] (almacenamiento column-major, convencion
// vector-COLUMNA v' = M*v; ver Matrix.cpp operator*(VecType): result.x =
// m[0]x + m[4]y + m[8]z + m[12]w). Con memcpy del m[16] TAL CUAL, HLSL lee
// M[r][c] = mem[c*4+r] = M_motor[r][c] y mul(invViewProj, v) computa
// EXACTAMENTE el M*v del motor: la misma tripleta (PACK_MATRIX_COLUMN_MAJOR
// + upload de Matrix::Copy sin transponer + mul(M, v)) que usa el raster
// D3D11 (ShaderLoadD3D11.cpp:365, Shader.cpp SubmitMatrix/SubmitMat4).
// Es decir: subir inverse(view_proj_matrix) TAL CUAL, sin transponer.
// La escena de calibracion de esquinas (M0 paso 6) confirma la convencion;
// si saliera especular, transponer en el upload (no tocar el kernel).
// ---------------------------------------------------------------------------

namespace CppProject
{
	namespace CinemaKernelM0
	{
		// Entry points y target de compilacion
		constexpr const char* ENTRY_PATHTRACE = "CSPathTrace";
		constexpr const char* ENTRY_CLEAR = "CSClear";
		constexpr const char* TARGET = "cs_5_0";

		// Threadgroup 8x8. El dispatch de CSPathTrace cubre UN tile:
		//   Dispatch(ceil(tileW / 8), ceil(tileH / 8), 1)
		// con tile.xy del cbuffer como offset en pixeles. CSClear ignora el
		// tile y cubre el frame entero: Dispatch(ceil(w / 8), ceil(h / 8), 1).
		constexpr unsigned int THREADGROUP_SIZE = 8;

		// Strides de los structured buffers. El host debe hacer static_assert
		// de sizeof() de sus structs C++ contra estas constantes.
		// Nota PTVert: el layout AUTORITATIVO es el struct C++ de CinemaBvh.hpp
		// (static_assert sizeof == 48): 9 campos de 4 B + pad0/pad1/pad2 hasta
		// 48 B (3 x 16, fetchable como tripletes float4/uint4). El diseno
		// (seccion 3) documenta 40 B: esta desactualizado respecto al codigo.
		constexpr unsigned int PTVERT_STRIDE = 48;
		constexpr unsigned int PTTRI_STRIDE = 16;
		constexpr unsigned int PTNODE_STRIDE = 32;
		// TRIM (ShinyTrims en Cinema): 96 -> 128 B EXACTOS. El 'pad' que ya
		// existia se convierte en trimPack y se anaden dos float4 (trimOrigin,
		// uvRectTrim). Multiplo de 16, cero padding implicito. Se eligio
		// extender PTMaterial en vez de anadir un SRV nuevo porque asi NO se
		// tocan root signatures (CinemaGpu12 params[3..5] = t8/t9/t10) ni el
		// CSSetShaderResources del D3D11, y porque PTMaterial YA lo re-sube el
		// camino vivo -- que es justo la tuberia que necesita el ORIGEN animado
		// del trim (ver CinemaScene, camino vivo).
		// INTEGRACION shiny-trims <- mi3: 128 (trim) + shadowOpacity de F1 y su
		// relleno de fila = 144. Ver el struct PTMaterial (C++ y HLSL).
		constexpr unsigned int PTMATERIAL_STRIDE = 144;
		// M1: luces analiticas (PTLight de CinemaScene.hpp, static_assert 64)
		// y arrays paralelos de NEE de emisivos (uint / float escalares)
		constexpr unsigned int PTLIGHT_STRIDE = 64;
		constexpr unsigned int PTEMITRI_STRIDE = 4;
		constexpr unsigned int PTEMICDF_STRIDE = 4;

		// cbuffer PTFrame: 320 bytes exactos (20 registros de 16 B)
		// R6: +float4 dofParams al final; R12: +float4 fogParams2 (niebla EN
		// EL KERNEL: color del lobulo Mie + gamma del espacio de mezcla);
		// DOF-FIEL (A0): +float4 lensParams (apertura poligonal) +float4 camFwd
		// (eje optico, para A2 plano de foco);
		//   bandParams         304  (CAPAS SLICING: ventana de profundidad)
		// TRIM: +48 B al final (tres float4). El TIEMPO del efecto viaja como
		// CONSTANTE de frame, no como contenido de textura: regenerar texturas
		// por frame dispararia el rebuild de BVH/atlas (~430 ms/frame) porque
		// la firma de escena hashea texId. Es P5 del PO escrito en el layout.
		//   trimTime           320  (x = uTrimTick0, y = uTime, z = uTrimLoopL,
		//                            w = uTrimSceneLight)
		//   trimNoise          336  (rect de la textura de ruido en su slice)
		//   trimNoiseSize      352  (x = uTrimNoiseSize escalar, y = slice del
		//                            atlas, z = trim_emissive, w = uTrimBodyScale)
		constexpr unsigned int PTFRAME_SIZE = 368;

		// Slots de bindings
		constexpr unsigned int SLOT_CB_FRAME = 0;
		constexpr unsigned int SLOT_SRV_VERTS = 0;
		constexpr unsigned int SLOT_SRV_TRIS = 1;
		constexpr unsigned int SLOT_SRV_NODES = 2;
		constexpr unsigned int SLOT_SRV_MATERIALS = 3;
		constexpr unsigned int SLOT_SRV_ATLAS = 4;
		constexpr unsigned int SLOT_SRV_LIGHTS = 5;   // M1: StructuredBuffer<PTLight>
		constexpr unsigned int SLOT_SRV_EMITRI = 6;   // M1: StructuredBuffer<uint> (orden ascendente)
		constexpr unsigned int SLOT_SRV_EMICDF = 7;   // M1: StructuredBuffer<float> (CDF sin normalizar)
		constexpr unsigned int SLOT_SAMPLER_POINT = 0;
		constexpr unsigned int SLOT_UAV_ACCUM = 0;
		constexpr unsigned int SLOT_UAV_AOV_ALBEDO = 1;
		constexpr unsigned int SLOT_UAV_AOV_NORMAL_DEPTH = 2;
		constexpr unsigned int SLOT_UAV_VARCOUNT = 3; // M7 adaptive: Welford + conteo por pixel

		// Bits de PTMaterial.flags (deben casar con CinemaScene al aplanar)
		constexpr unsigned int MATF_ALPHA_TEST = 1;
		constexpr unsigned int MATF_ALPHA_BLEND = 2;
		constexpr unsigned int MATF_WATER = 4;
		constexpr unsigned int MATF_TWO_SIDED = 8;
		constexpr unsigned int MATF_SUBSURFACE = 16;
		constexpr unsigned int MATF_ALPHA_HASH = 32;
		constexpr unsigned int MATF_SCENERY = 64;
		constexpr unsigned int MATF_WRAP_UV = 128;
		constexpr unsigned int MATF_HAS_MATTEX = 256;
		constexpr unsigned int MATF_HAS_NRMTEX = 512;
		constexpr unsigned int MATF_IS_BLOCK = 1024;
		// M2: formato del material map cuando HAS_MATTEX esta puesto.
		// Bit LIMPIO = SEUS (uMaterialFormat 1: rough = 1-R, metal = G);
		// bit PUESTO = LabPBR (uMaterialFormat 2: rough = (1-R)^2, metal =
		// G > 229/255). Lo fija CinemaScene al aplanar leyendo el
		// draw.materialFormat capturado (wiring M2).
		constexpr unsigned int MATF_MAT_LABPBR = 2048;
			// R4: la seccion CLOUDS lleva este flag para que sus draws NO
			// ocluyan rayos de sombra (TraceShadow los ignora). Los rayos
			// PRIMARIOS y de GI siguen viendo las nubes; solo la sombra las
			// omite. Minecraft vanilla y el raster tampoco proyectan sombra
			// de nubes: el plano de nubes captado como geometria recortaba el
			// sol sobre grandes zonas -> escena ~26% mas oscura.
			constexpr unsigned int MATF_NO_SHADOW = 4096;
		// R6: backface culling capturado del raster (draw.cullMode). Con
		// CULL_BACK se descarta en la traversal el hit cuya normal geometrica
		// del winding ALMACENADO apunta EN el sentido del rayo (cara trasera);
		// con CULL_FRONT el caso opuesto. Ninguno => two-sided (no se culla).
		// Reproduce los modelos "outline" (shell invertido) igual que el raster.
		constexpr unsigned int MATF_CULL_BACK = 8192;
		constexpr unsigned int MATF_CULL_FRONT = 16384;
		// R15: HOLDOUT ("invisible a camara", equivalente a Ray Visibility ->
		// Camera off de Blender). Los rayos de CAMARA (primario, los de lente
		// del DoF y sus continuaciones por transparencia, todo lo anterior al
		// primer scatter) ATRAVIESAN el triangulo; TraceShadow, los rebotes de
		// GI, los reflejos y la NEE de emisores NO lo filtran: el objeto sigue
		// proyectando sombra, tinendo por rebote, apareciendo en espejos y, si
		// es emisivo, iluminando. Sin objetos marcados el bit vale 0 en todos
		// los materiales y el camino es BIT-IDENTICO al de siempre.
		constexpr unsigned int MATF_CAM_INVISIBLE = 32768;
		// COBERTURA (alpha-as-coverage). El alpha de la textura de una CAPA DE
		// OBJETO (segunda capa de skin, partes de modelo, items, texto,
		// primitivas) NO es un indice de refraccion: es COBERTURA. alpha 114/255
		// significa "este texel tapa el 44.7 por ciento de la superficie" y el
		// resto deja ver lo de detras. Con este bit el kernel usa PT_KIND_COVER
		// (mezcla estocastica difuso/pasa-de-largo), que converge EXACTAMENTE a
		// la composicion del raster (c_capa*a + c_detras*(1-a)) y da sombra
		// (1-a) sin tinte. SIN el bit se conserva PT_KIND_GLASS (transmision
		// TENIDA + Fresnel): cristal de bloque, hielo y agua SI son transmision
		// de verdad y NO deben pasar a cobertura. Lo pone CinemaScene solo en
		// los draws marcados por pt_object_layer desde render_world_tl.
		constexpr unsigned int MATF_COVERAGE = 65536;
		// F1: MATTE ("holdout que OCLUYE"). Geometria normal para TODO menos
		// para el rayo de CAMARA: un rayo que sigue siendo de camara
		// (stillPrimary) y lo impacta MUERE ahi (radiancia 0, cobertura 0 =
		// un AGUJERO). Sombras, GI, reflejos y NEE de emisores lo ven NORMAL
		// (llegan con stillPrimary=false): sigue proyectando sombra, rebotando
		// luz y saliendo en espejos. Diferencia con MATF_CAM_INVISIBLE: el
		// invisible se ATRAVIESA (se ve lo de detras); el matte TAPA (no se ve
		// nada). Precedencia: hide > MATTE > INVISIBLE. Con film OPACO la
		// region matte queda NEGRA (alpha=1 forzado del invariante opaco
		// rellena con color 0) -- comportamiento definido, el caso de uso es
		// el film TRANSPARENTE. Sin materiales matte el bit vale 0 en toda la
		// escena y el camino es BIT-IDENTICO.
		constexpr unsigned int MATF_CAM_MATTE = 131072;
		// F1 (oleada 4a): SHADOW CATCHER. La superficie NO aporta radiancia
		// propia al pixel: aporta ALPHA. En el vertice primario el integrador
		// monta un estimador GEMELO -- numerador = lo que de verdad vuelve
		// (NEE ocluida + continuacion trazada), denominador = lo MISMO con
		// trans = 1 y con la continuacion sustituida por su referencia sin
		// ocluyentes -- y escribe alpha = 1 - Lnum/Lden (por canal, se emite el
		// canal mas ocluido) x SHADOW_OPACITY. Cero rayos extra: el denominador
		// reutiliza las MISMAS direcciones muestreadas, y por eso numerador y
		// denominador estan fuertemente correlados (el cociente tiene mucha
		// menos varianza que cualquiera de los dos por separado). Sin
		// materiales catcher el bit vale 0 en toda la escena y el camino es
		// BIT-IDENTICO. Bit 18: el mas alto usado pasa de 17 a 18.
		constexpr unsigned int MATF_SHADOW_CATCHER = 262144;
		// TRIM: esta pieza lleva un efecto de ShinyTrims. Sin el bit, el
		// material recorre EXACTAMENTE el camino de siempre (el bloque del
		// trim ni existe en el texto HLSL cuando la escena no tiene efectos).
		// INTEGRACION shiny-trims <- mi3: en su rama este bit era el 18
		// (262144), el MISMO que MATF_SHADOW_CATCHER de F1. Aliasarlos haria
		// que todo shadow-catcher se leyese como pieza con trim y viceversa,
		// asi que el trim sube al bit 19. Todos los consumidores lo usan POR
		// NOMBRE (C++ y HLSL), asi que renumerar aqui basta. Bits 20-31 libres.
		constexpr unsigned int MATF_HAS_TRIM = 524288;

		// Bits de PTFrame.counts.w (flagsFrame)
		constexpr unsigned int FRAMEF_ALPHA_FILM = 1;
		constexpr unsigned int FRAMEF_SKY_PHYSICAL = 2; // M4; ignorado en M0
		constexpr unsigned int FRAMEF_DEPTH_PROBE = 4;  // R9 FIX 6: sonda de autofocus
		constexpr unsigned int FRAMEF_ORTHO = 8;        // R14: camara ORTOGRAFICA (rayos paralelos)
		// FONDO DE PELICULA (cinema_backdrop): el film OPACO se comporta como
		// HOLDOUT para lo que hay DETRAS del sujeto -- el cielo no pinta ni
		// cubre, y el catcher no pinta su fondo. El COLOR lo pone el resolve
		// (CinemaRenderFinish) componiendo en espacio DISPLAY. El host solo
		// enciende este bit con film opaco: con alpha film ya es el camino de
		// siempre y el flag no llega nunca -> BIT-IDENTICO.
		constexpr unsigned int FRAMEF_BACKDROP = 16;
		// TRIM (D5): recortar el termino aditivo a 1.0 ANTES de linealizarlo,
		// imitando el buffer LDR R8G8B8A8_UNORM del raster. APAGADO por
		// defecto: Cinema es HDR de punta a punta y el recorte es una perdida
		// de informacion, no una fidelidad. Se implementa porque es la
		// diferencia mas visible entre motores en spark/molten/tide/charge y
		// esa la decide el critico mirando, no el spec por decreto.
		// INTEGRACION shiny-trims <- mi3: en su rama este flag valia 16, el
		// MISMO que FRAMEF_BACKDROP del FONDO DE PELICULA. Sube a 32.
		constexpr unsigned int FRAMEF_TRIM_LDR_CLIP = 32;

		// -------------------------------------------------------------------
		// Fuente HLSL completo (ASCII estricto). Se compila dos veces, una
		// por entry point. NO pasa por el preambulo autogenerado de cbuffers
		// del motor: el cbuffer va escrito a mano con packing verificado.
		// -------------------------------------------------------------------
		constexpr const char* SOURCE = R"HLSL(
// =============================================================================
// Cinema M2 -- megakernel de path tracing (cs_5_0)
// Alcance M1 (CINEMA_DESIGN.md secciones 6 y 13): NEE completo por vertice de
// camino con SPLITTING del sol (siempre 1 rayo al disco solar + 1 rayo a OTRA
// fuente elegida por potencia entre triangulos emisores, via CDF t7, y luces
// point/spot, via t5), MIS balance heuristic entre NEE y BSDF, ruleta rusa
// desde el bounce 3, sombras COLOREADAS (TraceShadow devuelve transmitancia
// float3 a traves de ALPHA_BLEND/WATER), cutout con continuacion in-stack,
// clamp de indirecta, acumulacion determinista (misma semilla -> misma
// imagen; el oraculo CPU replica bit a bit el mapeo de dims RNG documentado
// abajo).
// Alcance M2 (seccion 7): BSDF por "kind" de superficie --
//   kind 0 estandar: Lambert * (1-metallic) + GGX single-scatter (VNDF,
//          Smith height-correlated, alpha = roughness^2 como el raster);
//          seleccion de lobo por peso de Fresnel (dim RNG nueva);
//          metallic/roughness del draw o de uTextureMaterial (SEUS/LabPBR).
//   kind 1 hoja: thin-translucent (refl 0.7 + transmision difusa 0.3).
//   kind 2 agua: dielectrico delta IOR 1.33 (Fresnel exacto + TIR) sobre
//          micro-normal VNDF roughness 0.07; Beer-Lambert por distancia
//          dentro del agua (sigma del blendColor).
//   kind 3 cristal thin: reflexion delta Fresnel + transmision recta tintada
//          con el MISMO factor que TraceShadow.
//   Normal maps sobre tangent888; kinds 0/1 reciben NEE (MIS contra la pdf
//   de mezcla), kinds 2/3 son delta (sin NEE; el siguiente hit/miss entra
//   con peso 1 via prevDelta).
// =============================================================================

// ---------------------------------------------------------------------------
// Constantes
// ---------------------------------------------------------------------------
#define PT_PI            3.14159265358979
#define PT_TWO_PI        6.28318530717959
#define PT_T_MAX         1.0e30
#define PT_T_EPS         1.0e-4       // epsilon minimo en t (unidades del motor)
#define PT_STACK_SIZE    48           // profundidad del stack de traversal (PT_BVH_TRACE_STACK)
#define PT_MAX_TRANSP    32           // presupuesto de hits transparentes por rayo
// R15: presupuesto AMPLIADO para el rayo de CAMARA que YA ha atravesado un
// objeto "invisible a camara" (holdout). El limite de 32 esta dimensionado para
// el caso normal, en el que el primer solido corta el rayo enseguida; tras
// cruzar un holdout el rayo puede recorrer MEDIA ESCENA (vegetacion cutout,
// hojas) antes de comitear, y agotarlo hace que el cutout pase a comportarse
// OPACO -> se comitea un texel TRANSPARENTE, cuyo RGB en las texturas de
// Minecraft es NEGRO. Es una salvaguarda, no un fix de un bug observado (en la
// escena del usuario no se llego a agotar). Se sube UNA sola vez por rayo y
// SOLO tras cruzar un holdout: una escena sin holdouts queda BIT-IDENTICA
// (ningun material lleva el bit, la rama no entra nunca).
#define PT_MAX_TRANSP_HOLDOUT 256u
#define PT_INVALID       0xffffffffu
#define PT_NODE_LEAF_BIT 0x80000000u  // bit 31 de PTNode.leftOrFirst = hoja (CinemaBvh.hpp)

// --- constantes de material M2 (espejadas en CinemaOracle.cpp) ---
#define PT_ROUGH_MIN     0.03         // floor de roughness GGX: el lobulo nunca es delta (MIS definido)
#define PT_F0_DIELECTRIC 0.04         // F0 dielectrico (diseno seccion 7); metal -> F0 = albedo
#define PT_LEAF_TRANSMIT 0.3          // peso del lobulo de transmision difusa de hojas
#define PT_WATER_IOR     1.33         // agua
#define PT_WATER_ROUGH   0.07         // micro-normal VNDF del agua (valor MI3, seccion 7)
#define PT_GLASS_F0      0.04         // cristal thin (Schlick escalar)
#define PT_WATER_ABS_DIST 16.0        // Beer: a esta distancia (1 bloque) transmite exactamente blend.rgb
#define PT_SIGMA_FLOOR   1.0e-3      // blend minimo antes del log de la sigma (evita inf)
#define PT_SEG_MAX       1.0e6       // distancia maxima de un segmento para Beer (miss = agua infinita)

// kinds de superficie del BSDF M2 (EvalSurface decide; ver banner)
#define PT_KIND_STD      0u
#define PT_KIND_LEAF     1u
#define PT_KIND_WATER    2u
#define PT_KIND_GLASS    3u
// Cobertura (MATF_COVERAGE): mezcla estocastica de un lobulo DIFUSO estandar
// con peso aEff y un lobulo PASA-DE-LARGO (delta, sin tinte) con peso 1-aEff.
// Es el alpha de un rasterizador visto como BSDF: converge a c_capa*a +
// c_detras*(1-a). No es cristal: el cristal tine lo que lo cruza pero no
// aporta color propio; una capa de skin al 44.7 por ciento SI aporta el suyo.
#define PT_KIND_COVER    4u

// Bits de PTMaterial.flags (espejo de CinemaKernelM0::MATF_* del host)
#define MATF_ALPHA_TEST  1u
#define MATF_ALPHA_BLEND 2u
#define MATF_WATER       4u
#define MATF_TWO_SIDED   8u
#define MATF_SUBSURFACE  16u
#define MATF_ALPHA_HASH  32u
#define MATF_SCENERY     64u
#define MATF_WRAP_UV     128u
#define MATF_HAS_MATTEX  256u
#define MATF_HAS_NRMTEX  512u
#define MATF_IS_BLOCK    1024u
#define MATF_MAT_LABPBR  2048u  // M2: material map en formato LabPBR (limpio = SEUS)
#define MATF_NO_SHADOW   4096u  // R4: seccion CLOUDS no ocluye rayos de sombra
#define MATF_CULL_BACK   8192u  // R6: cull de caras traseras (replica el raster)
#define MATF_CULL_FRONT  16384u // R6: cull de caras frontales (objetos front-cull)
#define MATF_CAM_INVISIBLE 32768u // R15: holdout -- solo los rayos de CAMARA lo atraviesan
#define MATF_COVERAGE    65536u // alpha-as-coverage (capas de objeto; ver PT_KIND_COVER)
#define MATF_CAM_MATTE 131072u // F1: holdout que OCLUYE (agujero: ni pinta ni deja ver)
#define MATF_SHADOW_CATCHER 262144u // F1 4a: catcher (no pinta; escribe alpha = 1 - Lnum/Lden)
#define MATF_HAS_TRIM  524288u // TRIM: esta pieza lleva un efecto de ShinyTrims (bit 19: el 18 es el catcher)

// Bits de counts.w (flagsFrame)
#define FRAMEF_ALPHA_FILM    1u
#define FRAMEF_SKY_PHYSICAL  2u   // M4; ignorado aqui
#define FRAMEF_DEPTH_PROBE   4u   // R9 FIX 6: sonda de profundidad (autofocus)
#define FRAMEF_ORTHO         8u   // R14: camara ORTOGRAFICA (rayos paralelos)
#define FRAMEF_BACKDROP     16u   // FONDO DE PELICULA: holdout del fondo con film opaco
#define FRAMEF_TRIM_LDR_CLIP 32u  // TRIM (D5): recortar el aditivo a 1.0 (imita el buffer LDR del raster)

// M7 adaptive sampling: piso de samples antes de evaluar convergencia, intervalo
// de chequeo y piso del denominador del error relativo (evita div por ~0 en
// negros). El umbral por render llega en dofParams.w (0 = OFF = bit-identico).
#define PT_ADAPT_MIN       32.0
#define PT_ADAPT_INTERVAL  32.0
#define PT_ADAPT_FLOOR     0.001

// ---------------------------------------------------------------------------
// Estructuras. Layout identico byte a byte a los structs C++ del host: en
// structured buffers HLSL empaqueta los escalares de 4 B de forma compacta
// (NO aplican las reglas de packing de cbuffer), asi que el layout es el del
// struct C plano y los float4 internos son solo 16 bytes consecutivos.
// ---------------------------------------------------------------------------

// stride 48 B -- layout EXACTO del struct C++ PTVert de CinemaBvh.hpp
// (static_assert sizeof == 48; 3 x 16 B). Un stride distinto desalinearia
// TODOS los vertices a partir del elemento 1.
struct PTVert
{
	float px, py, pz;   // posicion en MUNDO (transformada al aplanar)
	uint  normal888;    // 8:8:8 (x | y<<8 | z<<16), decodifica byte*2/255 - 1
	float u, v;         // UV crudas (locales 0-1 en texture pages, absolutas en block sheet)
	uint  colorABGR;    // r | g<<8 | b<<16 | a<<24; el RGB ya trae la politica de AO (seccion 7.1)
	uint  dataFlags;    // bit2 SUBSURFACE; byte 1 (bits 8-15) = emissive*255
	uint  tangent888;   // tangente 8:8:8 (normal maps, M2; sin uso en M0)
	uint  pad0, pad1, pad2; // relleno hasta 48 B, siempre 0 (CinemaBvh.hpp)
};

// stride 16 B
struct PTTri { uint i0, i1, i2, matId; };

// stride 32 B -- layout EXACTO del struct C++ PTNode de CinemaBvh.hpp:
//   interno (bit 31 de leftOrFirst LIMPIO):
//     leftOrFirst  = indice del hijo izquierdo (NO contiguos: el derecho es explicito)
//     rightOrCount = indice del hijo derecho
//   hoja (bit 31 de leftOrFirst PUESTO):
//     leftOrFirst  = PT_NODE_LEAF_BIT | primer tri (en el array REORDENADO por hoja)
//     rightOrCount = numero de tris (1..4)
// El nodo 0 es siempre la raiz (puede ser hoja en escenas diminutas).
struct PTNode
{
	float bminx, bminy, bminz;
	uint  leftOrFirst;
	float bmaxx, bmaxy, bmaxz;
	uint  rightOrCount;
};

// stride 144 B
struct PTMaterial
{
	float4 uvRect;      // remap del albedo dentro del slice (offset.xy, escala.zw)
	float4 uvRectMat;   // idem material map (M2; invalido si !HAS_MATTEX)
	float4 uvRectNrm;   // idem normal map (M2)
	float4 blend;       // uBlendColor LINEALIZADO x alpha de objeto (rgb, a)
	float  emissive;    // uEmissive del draw (0..1)
	float  metallic;    // M2
	float  roughness;   // M2
	uint   flags;       // MATF_*
	uint   sliceAlbedo; // slice del atlas para el albedo
	uint   sliceMat;    // M2
	uint   sliceNrm;    // M2
	// TRIM: bits 0-7 fx (slot 199..254; 0 = sin trim), bits 8-15 slice del
	// atlas donde vive la MASCARA M1, bit 16 OUTLINE (esta pieza es la cascara
	// del outline 3D), bit 17 WRAPUV (la mascara usa frac de la UV), 18-31 = 0.
	uint   trimPack;
	float4 trimOrigin;  // xyz = origen del espacio CUERPO (mundo); w = uTrimIntensity
	float4 uvRectTrim;  // rect de la mascara M1 dentro de su slice del atlas
	// F1 4a: multiplicador del alpha del SHADOW CATCHER (e_value SHADOW_OPACITY,
	// 0..1). Solo lo lee la rama del catcher; en cualquier otro material vale 1
	// y no se toca (BuildMaterial lo escribe siempre, asi que el buffer sigue
	// sin bytes indeterminados).
	// INTEGRACION shiny-trims <- mi3: el hueco de `pad` que este campo ocupaba
	// en F1 se lo quedo trimPack, asi que shadowOpacity pasa al final. Los tres
	// pads cierran la fila de 16 B (stride 144); `PTMaterial mat = {}` los deja
	// a 0, asi que no entra un solo byte indeterminado.
	float  shadowOpacity;
	float  pad0, pad1, pad2;
};

// stride 64 B -- layout EXACTO del struct C++ PTLight de CinemaScene.hpp
// (static_assert sizeof == 64; diseno seccion 3). El color ya es lineal x
// LIGHT_STRENGTH; el host puede plegar ahi cualquier puente display ->
// radiometrico (como el CINEMA_SUN_IRRADIANCE_SCALE del sol).
struct PTLight
{
	float3 pos;         // posicion en mundo
	float  range;       // LIGHT_RANGE (la curva de falloff del forward)
	float3 col;         // color lineal x LIGHT_STRENGTH
	float  sizeRadius;  // point/spot: radio; area: lado del cuadrado
	float3 dir;         // spot: eje; area: normal; sin uso en point
	float  cosOuter;    // spot: cos(LIGHT_SPOT_RADIUS/2); point: -2 (sin cono)
	float  sharpness;   // LIGHT_SPOT_SHARPNESS (0..1)
	float  fadeSize;    // LIGHT_FADE_SIZE (0..1)
	uint   type;        // 0 point, 1 spot, 2 square area
	float  pad;
};

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// cbuffer PTFrame -- packing verificado a mano (reglas cbuffer D3D11).
// TODOS los miembros son float4x4/float4/uint4, multiplos exactos de un
// registro de 16 B: no existe ningun escalar suelto ni escalar-tras-array
// (la trampa conocida de packing del motor) y ningun miembro cruza frontera
// de registro. Offsets en bytes:
//   invViewProj          0  (64 B)
//   camPos              64
//   sunDir_cosAng       80
//   sunRadiance         96
//   skyZenith          112
//   skyHorizon         128
//   ambient_nightAlpha 144
//   fogParams0         160
//   fogParams1         176  (w = F5: envLight, la antigua reserva)
//   counts             192
//   tile               208
//   res_clamp          224
//   dofParams          240  (R6)
//   fogParams2         256  (R12)
//   lensParams         272  (DOF-FIEL A0/A1)
//   camFwd             288  (DOF-FIEL A0/A2)
//   bandParams         304  (CAPAS SLICING)
// Total: 320 B (multiplo de 16). El host crea el cbuffer de 320 B exactos y
// hace static_assert(sizeof(PTFrameCpu) == 320). OJO D3D12: la CBV se alinea
// a 256 B -> el anillo de CinemaGpu12 usa stride 512 >= 320 -> sin cambios.
// ---------------------------------------------------------------------------
cbuffer PTFrame : register(b0)
{
	float4x4 invViewProj;        // inversa de view_proj_matrix (memcpy del m[16] del motor TAL CUAL, sin transponer; ver nota de convencion en el header C++)
	float4   camPos;             // xyz = posicion de camara; w = near
	float4   sunDir_cosAng;      // xyz = direccion HACIA el sol, normalizada; w = cos(radio angular)
	float4   sunRadiance;        // rgb = radiancia lineal del disco solar; w = far
	float4   skyZenith;          // rgb = color lineal del cenit
	float4   skyHorizon;         // rgb = color lineal del horizonte
	float4   ambient_nightAlpha; // rgb = ambient lineal (suelo del domo); w = night_alpha
	float4   fogParams0;         // R12 niebla: rgb = fogColor LINEAL (horizonte), w = fogDistance
	float4   fogParams1;         // R12 niebla: x = fogSize, y = fogHeight, z = show (0/1), w = F5 envLight (project_cinema_env_light; 0 = OFF)
	uint4    counts;             // x = numLights (t5), y = numEmiTris (t6/t7), z = maxBounces, w = flagsFrame FRAMEF_*
	uint4    tile;               // x,y = offset del tile en pixeles; z = sampleIdx; w = semilla del frame
	float4   res_clamp;          // x = width, y = height, z = clamp de indirecta (0 = off), w = escala de emisivos
	float4   dofParams;          // 240: R6 DoF fisico -> x = focusDist (mundo), y = radio de lente (mundo), z = dofOn (0/1), w = umbral adaptive
	float4   fogParams2;         // 256: R12 niebla -> rgb = fogColorSun LINEAL (lobulo Mie), w = gamma del espacio de mezcla (0 = lineal)
	float4   lensParams;         // 272: A1 apertura -> x = palas (0/<3 = disco), y = rotacion en RADIANES, z = ratio anamorfico (0 = circular), w = bias de apodizacion (0 = uniforme)
	float4   camFwd;             // 288: A2 plano de foco -> xyz = eje optico unitario (hacia donde mira la camara), w = reserva
	float4   bandParams;         // 304: CAPAS SLICING -> x = t0 (mundo, 0 = sin corte cercano), y = t1 (mundo; <= 0 = +inf), z = on (0/1), w = reserva
	float4   trimTime;           // 320: TRIM -> x = uTrimTick0, y = uTime, z = uTrimLoopL, w = uTrimSceneLight
	float4   trimNoise;          // 336: TRIM -> rect de la textura de ruido en su slice (xy origen, zw escala)
	float4   trimNoiseSize;      // 352: TRIM -> x = uTrimNoiseSize (ESCALAR, 128), y = slice del atlas, z = trim_emissive del usuario, w = uTrimBodyScale
};

// TRIM: la FORMULA del tiempo es literalmente la misma que la del raster
// (stTicks = uTrimTick0 + uTime/3). Estos tres nombres existen para que el
// bloque SHARED del glint se pueda TRADUCIR sin tocarlo: el generador no
// reescribe la formula, solo cambia de donde salen sus entradas.
#define uTrimTick0 (trimTime.x)
#define uTime      (trimTime.y)
#define uTrimLoopL (trimTime.z)

// ---------------------------------------------------------------------------
// Recursos
// ---------------------------------------------------------------------------
StructuredBuffer<PTVert>     gVerts : register(t0);
StructuredBuffer<PTTri>      gTris  : register(t1);
StructuredBuffer<PTNode>     gNodes : register(t2);
StructuredBuffer<PTMaterial> gMats  : register(t3);
Texture2DArray               gAtlas : register(t4);     // RGBA8 SRGB: SampleLevel devuelve lineal
// M1: luces analiticas y NEE de emisivos. gEmiTriIdx trae indices de tri del
// array REORDENADO (el de gTris) en ORDEN ASCENDENTE (contrato del header
// C++: la busqueda binaria de pertenencia del MIS lo exige); gEmiCdf es la
// CDF por potencia SIN normalizar paralela (ultimo elemento = W_emi total).
StructuredBuffer<PTLight>    gLights    : register(t5);
StructuredBuffer<uint>       gEmiTriIdx : register(t6);
StructuredBuffer<float>      gEmiCdf    : register(t7);
#ifdef CINEMA_RAYQUERY
// FASE B (M6) + M8-A: TLAS de la escena para el trazado por HARDWARE (RayQuery
// inline, SM 6.5, Tier 1.1). M8-A generaliza la geometria a ESPACIO LOCAL +
// instancias TLAS (una BLAS por draw, una instancia con la matriz de mundo del
// draw); el kernel refetch en LOCAL y transforma a mundo con ObjectToWorld3x4()
// por hit. Con MI3_CINEMA_INSTANCED=0 el host sube el camino de siempre (verts
// en MUNDO, UNA BLAS, instancia IDENTIDAD) -> o2w=identidad -> transform no-op
// BIT-IDENTICO. Geometria NON_OPAQUE: cada candidato replica los descartes
// custom del software (backface cull, cutout, ghost-texel, nubes, sombra
// coloreada). Solo la compila dxc (D3D12); fxc/D3D11 nunca ve este bloque.
RaytracingAccelerationStructure gScene : register(t8);

// M8-A: base de tri GLOBAL por instancia -> triIdx = gInstBase[InstanceIndex] +
// PrimitiveIndex (mapea la primitiva LOCAL de la BLAS al array gTris global).
// OFF: 1 elemento [0] (BLAS unica, PrimitiveIndex ya es global).
StructuredBuffer<uint> gInstBase : register(t9);

// M8-A: object->world (3x4 fila-mayor) POR EMISOR de la CDF de NEE. El sampling
// de emisores no pasa por RayQuery (no hay ObjectToWorld disponible), asi que su
// transform a mundo viaja aparte, paralela a gEmiTriIdx/gEmiCdf. OFF/identidad
// cuando la geometria emisora ya esta en mundo.
struct InstXform { float4 r0, r1, r2; };
StructuredBuffer<InstXform> gEmiO2W : register(t10);
float3x4 InstXformMat(InstXform x) { return float3x4(x.r0, x.r1, x.r2); }

// M8-A: transform de punto y de direccion por una o2w 3x4; signo del det de la
// 3x3 (para compensar el winding de instancias espejadas, ver EvalSurface/cull).
float3 XformPos(float3x4 o2w, float3 p) { return mul(o2w, float4(p, 1.0)); }
float3 XformDir(float3x4 o2w, float3 v) { return mul((float3x3)o2w, v); }
// Normal LOCAL -> mundo por la matriz de COFACTORES (adjunta transpuesta =
// det * M^-T), espejo 1:1 del cofactor de FlattenDraw. La 3x3 DIRECTA solo
// vale para rotacion + escala UNIFORME: con escala anisotropa inclina la
// normal, y como las normales llegan cuantizadas a 8 bits el residuo de las
// componentes pequenas se multiplica por los ejes grandes. El cofactor divide
// por las escalas y el residuo se vuelve despreciable.
// ESPEJO (det<0): cof(M) = det(M)*M^-T, asi que devuelve la normal con el signo
// INVERTIDO respecto a la transformacion "de libro" M^-T. Es INOFENSIVO aqui a
// proposito: EvalSurface (y el oraculo) voltean SIEMPRE ns hacia el rayo justo
// despues, asi que ns y -ns dan el MISMO resultado. Se deja el cofactor a pelo
// -- sin multiplicar por DetSign3x4 -- para que host y shader hagan la misma
// operacion EXACTA; si algun dia se usa ns sin voltear, hay que meter el signo.
float3 XformNormal(float3x4 o2w, float3 n)
{
	float3 c0 = float3(o2w[0][0], o2w[1][0], o2w[2][0]);
	float3 c1 = float3(o2w[0][1], o2w[1][1], o2w[2][1]);
	float3 c2 = float3(o2w[0][2], o2w[1][2], o2w[2][2]);
	return cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
}
float  DetSign3x4(float3x4 o2w) { return (determinant((float3x3)o2w) < 0.0) ? -1.0 : 1.0; }
#endif
SamplerState                 gSampPoint : register(s0); // point, address clamp

// UAVs: buffers estructurados (NO texturas tipadas: typed UAV load de float4
// es ilegal en D3D11 FL11, error X3676). Indice: pix = y * width + x.
RWStructuredBuffer<float4> gAccum          : register(u0); // rgb = suma color, w = suma alpha
RWStructuredBuffer<float4> gAovAlbedo      : register(u1); // rgb = suma albedo 1er hit, w = num samples
RWStructuredBuffer<float4> gAovNormalDepth : register(u2); // xyz = suma normal 1er hit, w = suma depth (t)
RWStructuredBuffer<float4> gVarCount        : register(u3); // M7 adaptive: x=media lum, y=M2 Welford, z=conteo pixel, w=convergido

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// RNG determinista: pcg4d sobre coordenadas ABSOLUTAS del frame.
// Contrato M2 FINAL (el oraculo CPU debe replicarlo bit a bit; espejo en
// CinemaOracleDims de CinemaOracle.hpp). El stride por bounce sube de 3 (M1)
// a 4: la dim nueva k=3 es la seleccion/muestreo de lobo del BSDF M2.
//   semilla = (pixelAbs.x, pixelAbs.y, sampleIdx + frameSeed * 0x9E3779B9, dim)
//   dim 0                  -> .xy = jitter subpixel de camara
//   dim 1 + bounce * 4 + 0 -> .xy = disco solar (NEE sol),
//                             .zw = hemisferio coseno (lobulo DIFUSO del
//                                   BSDF: Lambert de kind 0 o hemisferio de
//                                   reflexion/transmision de hoja)
//   dim 1 + bounce * 4 + 1 -> .x  = seleccion de fuente (grupo emisivos vs
//                                   luces vs ENTORNO y, REMAPEADA, la fuente
//                                   dentro del grupo; ver SampleOtherLight),
//                             .yz = punto por area en el tri emisor, cono
//                                   hacia la esfera de la luz, O hemisferio
//                                   coseno del domo (segun grupo),
//                             .w  = reserva (no usada)
//   F5 (C1): la NEE de ENTORNO entra por este MISMO slot -- es un grupo mas de
//   la CDF de SampleOtherLight, no una tecnica aparte. El contrato de dims
//   sigue siendo 4 por bounce: NINGUNA imagen del motor cambia de semilla.
//   dim 1 + bounce * 4 + 2 -> .x  = ruleta rusa (solo se evalua en
//                                   bounce >= 3), .yzw = reserva
//   dim 1 + bounce * 4 + 3 -> .x  = seleccion de lobo del BSDF M2:
//                                   kind 0: difuso vs GGX (peso de Fresnel);
//                                   kind 1: reflexion vs transmision de hoja
//                                           (PT_LEAF_TRANSMIT);
//                                   kind 2/3: Fresnel reflexion vs
//                                           refraccion/transmision (delta),
//                             .yz = muestra VNDF del semivector GGX (kind 0)
//                                   o de la micro-normal del agua (kind 2),
//                             .w  = reserva (no usada)
// El mapeo es FIJO por bounce: Rand4 es funcion pura de (pixel, sample, dim),
// asi que saltarse una tecnica (p.ej. sin luces en escena, o un kind que no
// usa .yz) NO desplaza las dimensiones de las demas. pixelAbs incluye el
// offset del tile: el tiled export es bit-igual al render entero y no hay
// costuras (seccion 6).
// ---------------------------------------------------------------------------
uint4 Pcg4d(uint4 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
	v ^= v >> 16u;
	v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
	return v;
}

float4 Rand4(uint2 pixelAbs, uint dim)
{
	uint4 v = uint4(pixelAbs.x, pixelAbs.y, tile.z + tile.w * 0x9E3779B9u, dim);
	// >> 8 y * 2^-24: [0, 1) GARANTIZADO y exacto en float. Con * 2^-32 el
	// float(uint) redondea (p.ej. 0xFFFFFFFF -> 2^32) y puede devolver 1.0,
	// que sangra el jitter al pixel vecino y rompe el contrato del oraculo.
	return float4(Pcg4d(v) >> 8u) * (1.0 / 16777216.0);
}

// ---------------------------------------------------------------------------
// Decodificacion de atributos de vertice (misma codificacion del motor,
// ver Render/Vertex.hpp: UNPACK_VERTEX_NORMAL / UNPACK_VERTEX_COLOR)
// ---------------------------------------------------------------------------
float3 DecodeNormal888(uint n)
{
	// INVERSA EXACTA del empaquetado del motor (Vertex::SetNormal:
	// byte = (n+1)/2 * 255) y del EncodePacked888 del host. Antes esto dividia
	// por 127 -- otra escala, con el cero medio cuanto corrido y el +1 en
	// 1.0079 -- asi que host y shader decodificaban BYTES IGUALES a normales
	// DISTINTAS. Con la copia instanciada (normales LOCALES) esa diferencia
	// sub-cuanto se amplificaba por la razon de escalas del objeto.
	return float3(float(n & 255u), float((n >> 8u) & 255u), float((n >> 16u) & 255u)) * (2.0 / 255.0) - 1.0;
}

float4 DecodeColorAbgr(uint c)
{
	return float4(float(c & 255u), float((c >> 8u) & 255u), float((c >> 16u) & 255u), float((c >> 24u) & 255u)) / 255.0;
}

float DecodeVertEmissive(uint dataFlags)
{
	return float((dataFlags >> 8u) & 255u) / 255.0;
}

float3 PtVertPos(PTVert vtx) { return float3(vtx.px, vtx.py, vtx.pz); }
float2 PtVertUv(PTVert vtx)  { return float2(vtx.u, vtx.v); }

// ---------------------------------------------------------------------------
// Base ortonormal (Duff et al. 2017)
// ---------------------------------------------------------------------------
void BuildOnb(float3 n, out float3 t, out float3 b)
{
	float s = (n.z >= 0.0) ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float c = n.x * n.y * a;
	t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
	b = float3(c, s + n.y * n.y * a, -n.y);
}

// Muestreo coseno del hemisferio alrededor de n; pdf = cos(theta)/pi.
// Con Lambert (f = albedo/pi) el peso f*cos/pdf se reduce a albedo.
float3 CosineSampleHemisphere(float2 r, float3 n)
{
	float phi = PT_TWO_PI * r.x;
	float sr = sqrt(r.y);
	float3 t, b;
	BuildOnb(n, t, b);
	float3 d = t * (cos(phi) * sr) + b * (sin(phi) * sr) + n * sqrt(max(0.0, 1.0 - r.y));
	return normalize(d);
}

// Muestreo uniforme del cono del disco solar (eje sunDir, coseno limite
// cosMax); pdf de angulo solido = 1 / (2*pi*(1 - cosMax)).
float3 SampleSunCone(float2 r, float3 sunDir, float cosMax)
{
	float cosTheta = 1.0 - r.x * (1.0 - cosMax);
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	float phi = PT_TWO_PI * r.y;
	float3 t, b;
	BuildOnb(sunDir, t, b);
	return normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + sunDir * cosTheta);
}

// ---------------------------------------------------------------------------
// Cielo M0 (miss): gradiente analitico con las globals FINALES del motor
// (seccion 8 del diseno). Mundo Z-up: up = dir.z. Hemisferio superior:
// horizonte -> cenit; inferior: horizonte -> ambient (el ambient artistico
// es el suelo del domo). night_alpha atenua el domo entero hacia un suelo
// del 5 por ciento (la paridad exacta de fondo llega en M3 con el bake del
// cielo raster a cubemap).
// NOTA: SkyRadiance NO incluye el disco solar. En los miss de bounce > 0 el
// integrador anade el disco con peso MIS (lado BSDF del splitting del sol);
// en primarios el disco no se pinta (el fondo del raster tampoco lo trae;
// paridad exacta de fondo en M3 via bake a cubemap).
// ---------------------------------------------------------------------------
float3 SkyRadiance(float3 dir)
{
	float up = dir.z;
	float3 grad;
	if (up >= 0.0)
		grad = lerp(skyHorizon.rgb, skyZenith.rgb, saturate(up));
	else
		grad = lerp(skyHorizon.rgb, ambient_nightAlpha.rgb, saturate(-up));
	return lerp(grad, grad * 0.05, saturate(ambient_nightAlpha.w));
}

// Clamp duro de la contribucion indirecta (res_clamp.z; 0 = off)
float3 ClampIndirect(float3 c)
{
	return (res_clamp.z > 0.0) ? min(c, res_clamp.zzz) : c;
}

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// R12 -- NIEBLA ATMOSFERICA (perspectiva aerea). Port EXACTO de la formula del
// raster (shader_high_fog.fsh + shader_high_fog_apply.fsh), aplicada EN EL
// KERNEL sobre el HIT PRIMARIO REAL (la traversal ya se comio ghost-texels y
// cutouts pasantes: por construccion no hay profundidades infladas, que era el
// fallo del fog post de M3):
//   d     = distancia camara -> punto  (= hit.t, rayDir unitario)
//   sigma = 1.6 / max(fogDistance, 1)
//   clear = fogDistance * 0.04                       (zona despejada)
//   dens  = 1 - exp(-max(d - clear, 0) * sigma)
//   hfall = exp(-max(z - fogHeight, 0) / max(fogSize, 1))   (z = ALTURA, Z-up)
//   fog   = saturate(dens * lerp(0.55, 1.0, hfall))
//   mie   = pow(saturate(dot(viewDir, sunDir)), 8)   (forward scatter)
//   col   = lerp(fogColor, fogColorSun, saturate(mie * 0.7))
// Rayos secundarios (GI/reflejos) SIN niebla en v1: el raster es screen-space
// y el objetivo es calibrar contra el. El MISS (cielo) tampoco se toca: el
// gradiente del env ya trae su propio blend de horizonte.
// ---------------------------------------------------------------------------
float FogFactor(float dist, float worldZ)
{
	float fogDist = fogParams0.w;
	float sigma = 1.6 / max(fogDist, 1.0);
	float clearZone = fogDist * 0.04;
	float dens = 1.0 - exp(-max(dist - clearZone, 0.0) * sigma);
	float hfall = exp(-max(worldZ - fogParams1.y, 0.0) / max(fogParams1.x, 1.0));
	return saturate(dens * lerp(0.55, 1.0, hfall));
}

float3 FogColorFor(float3 viewDir)
{
	float m = pow(saturate(dot(viewDir, sunDir_cosAng.xyz)), 8.0);
	return lerp(fogParams0.rgb, fogParams2.rgb, saturate(m * 0.7));
}

// Mezcla de la niebla. fogParams2.w = gamma del espacio de mezcla:
//   > 0 -> se mezcla en el dominio pow(1/gamma), que es el ESPACIO DISPLAY en
//          el que mezcla el raster (su surface de escena es gamma-nativa y el
//          AgX del resolve la lineariza con pow 2.2). Default 2.2 = port 1:1.
//   = 0 -> mezcla en LINEAL puro (fundido mas denso en sombras que el raster).
float3 FogMix(float3 radiance, float3 fogCol, float fog)
{
	float g = fogParams2.w;
	float3 zero3 = float3(0.0, 0.0, 0.0);
	float3 c = max(radiance, zero3);
	float3 f = max(fogCol, zero3);
	if (g <= 0.0)
		return lerp(c, f, fog);
	float3 invG3 = float3(1.0 / g, 1.0 / g, 1.0 / g);
	float3 d = lerp(pow(c, invG3), pow(f, invG3), fog);
	return pow(max(d, zero3), float3(g, g, g));
}

// ---------------------------------------------------------------------------
)HLSL" R"HLSL(// Atlas: remap de UV por material y fetch nearest. SampleLevel mip 0 SIEMPRE
// (compute: no hay gradientes; ademas nearest es el look Minecraft y evita
// bleeding entre tiles del atlas).
// ---------------------------------------------------------------------------
float2 RemapUv(PTMaterial mat, float2 rawUv)
{
	// WRAP_UV (texturas con tiling, p.ej. ground): repetir dentro del rect.
	// Sin flag: los sprites de texture page traen UV local 0-1 (uvRect del
	// draw) y el block sheet UV absoluta con uvRect identidad: misma formula.
	float2 local = ((mat.flags & MATF_WRAP_UV) != 0u) ? frac(rawUv) : rawUv;
	return mat.uvRect.xy + local * mat.uvRect.zw;
}

float4 SampleAlbedo(PTMaterial mat, float2 rawUv)
{
	float2 uv = RemapUv(mat, rawUv);
	return gAtlas.SampleLevel(gSampPoint, float3(uv, float(mat.sliceAlbedo)), 0.0);
}

// UV cruda interpolada de un triangulo
float2 TriRawUv(PTTri tri, float bu, float bv)
{
	float2 uv0 = PtVertUv(gVerts[tri.i0]);
	float2 uv1 = PtVertUv(gVerts[tri.i1]);
	float2 uv2 = PtVertUv(gVerts[tri.i2]);
	return uv0 * (1.0 - bu - bv) + uv1 * bu + uv2 * bv;
}

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// M2: fetch de DATOS (material map / normal map). El atlas es SRGB y el
// sample devuelve LINEAL: para datos hay que DESHACER la linealizacion con
// la OETF sRGB exacta y recuperar el byte original (el oraculo CPU lee los
// bytes crudos del QImage: mismo valor modulo redondeo float). El alpha del
// formato SRGB es lineal y se usa tal cual.
// ---------------------------------------------------------------------------
float3 LinearToSrgb3(float3 c)
{
	c = saturate(c);
	float3 lo = c * 12.92;
	float3 hi = 1.055 * pow(max(c, 1.0e-8), 1.0 / 2.4) - 0.055;
	return float3(c.x <= 0.0031308 ? lo.x : hi.x,
	              c.y <= 0.0031308 ? lo.y : hi.y,
	              c.z <= 0.0031308 ? lo.z : hi.z);
}

// Fetch RAW (bytes originales re-codificados) de un rect generico del atlas
// (uvRectMat / uvRectNrm); mismo WRAP_UV y nearest mip 0 que el albedo.
float4 SampleDataTexel(uint matFlags, float4 rect, uint slice, float2 rawUv)
{
	float2 local = ((matFlags & MATF_WRAP_UV) != 0u) ? frac(rawUv) : rawUv;
	float2 uv = rect.xy + local * rect.zw;
	float4 t = gAtlas.SampleLevel(gSampPoint, float3(uv, float(slice)), 0.0);
	return float4(LinearToSrgb3(t.rgb), t.a);
}

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// TRIM (ShinyTrims en Cinema). El BLOQUE ENTERO -- runtime st*, cuerpo de los
// efectos y la formula de emision -- lo INYECTA CinemaTrimSource en el
// marcador de abajo, y solo con los efectos que la escena usa de verdad.
//
// Consecuencias de que sea asi y no un if(fx) con las 20 ramas siempre
// compiladas:
//   (1) P6 por construccion. Una escena real lleva 1 o 2 efectos, no 20; la
//       especializacion no es la mitigacion de emergencia, es el diseno.
//   (2) La REGRESION se vuelve estructural. Sin trims el marcador se sustituye
//       por la cadena VACIA, CINEMA_TRIM no queda definida y todos los
//       #ifdef de abajo desaparecen: el texto HLSL de una escena sin trim es
//       BYTE A BYTE el mismo siempre, sin depender de que -Gis impida al
//       compilador reordenar el resto del kernel al crecer.
//   (3) Determinismo intacto: el trim no consume NINGUNA dimension de Rand4.
//
// Lo que el bloque inyectado puede dar por hecho (contrato con el generador):
// gAtlas/gSampPoint, LinearToSrgb3, el cbuffer PTFrame con trimTime/trimNoise/
// trimNoiseSize y los #define uTrimTick0/uTime/uTrimLoopL.
// ---------------------------------------------------------------------------

// Inversa exacta de LinearToSrgb3 (EOTF sRGB). El snippet del efecto se evalua
// en sRGB -- que es lo que ve el raster, `rawColor.rgb` es el byte/255 de una
// textura NO-sRGB -- y su resultado se devuelve a lineal para sumarlo a la
// radiancia. Evaluarlo directamente sobre el texel lineal del atlas NO es una
// aproximacion aceptable: MEDIDO sobre 4 colores base x 60 muestras, 19 de los
// 20 efectos difieren, hasta 0.5774 en RGB lineal (mas de medio rango). Solo
// glowing sale identico, y solo porque add = base y la ida-y-vuelta se cancela.
float3 SrgbToLinear3(float3 c)
{
	c = saturate(c);
	float3 lo = c / 12.92;
	float3 hi = pow(max((c + 0.055) / 1.055, 1.0e-8), 2.4);
	return float3(c.x <= 0.04045 ? lo.x : hi.x,
	              c.y <= 0.04045 ? lo.y : hi.y,
	              c.z <= 0.04045 ? lo.z : hi.z);
}

// @@TRIM@@

#ifdef CINEMA_TRIM
// --- La FORMULA de emision del trim. Vive AQUI y no en el generador a
// proposito: lo que sale del catalogo es el CUERPO de cada efecto (el
// dispatch y las cotas); como esa `add` se convierte en radiancia es una
// decision del motor y tiene que poder auditarse en el kernel.
//
// Cinco cosas que no son obvias y por que:
//  (a) se multiplica por blend.rgb * vertRgb porque en el raster el emisivo
//      del trim CABALGA sobre el albedo (el emisivo del G-buffer es una
//      ganancia sobre diffuse). Sin eso el trim seria la unica superficie de
//      Cinema que ignora el tinte de objeto y el AO de vertice.
//  (b) SUMA y no max: un max borraria el emisivo propio de un material que ya
//      emitia debajo del trim.
//  (c) se aplica cinemaGain(fx) porque el A/B de aceptacion es contra EL
//      DEFECTO DE ENVIO del raster. Sin el, spark (p90 2.471) y charge
//      (1.849) quemarian exactamente como la regla del p90 se invento para
//      evitar.
//  (d) NO se aplica uTrimEmissiveScale: ese calibra la tuberia del RASTER
//      (canal aditivo LDR + ganancia emisiva). Cinema tiene su propio
//      res_clamp.w y aplicar los dos haria del trim la unica superficie con
//      exposicion ajena. Lo que SI viaja es la intencion RELATIVA del usuario
//      (trimNoiseSize.z, default 1).
//  (e) la niebla se hereda gratis por la atmosfera del path tracer. El toggle
//      "los efectos atraviesan la atmosfera" NO se soporta: un emisor que
//      ignore la extincion del medio no es representable sin mentir.
uint TrimFx(PTMaterial mat)     { return mat.trimPack & 255u; }
uint TrimSlice(PTMaterial mat)  { return (mat.trimPack >> 8u) & 255u; }

// Tamano de la textura en TEXELES, DERIVADO -- no viaja por draw.
// ComposeUvRect compone (WxH de la fuente / 4096) con la fraccion de pagina
// del draw, asi que uvRect.zw * 4096 es un numero ENTERO de texeles salvo
// error de coma flotante (~1e-4, cuatro ordenes por debajo del 0.5 que
// round() necesita). Ahorra 8 B por material y, sobre todo, elimina una via
// de desincronizacion entre GML y el atlas. El gate G7 lo COMPRUEBA contra
// sprite_get_width/height en vez de darlo por bueno.
float2 TrimTexSize(PTMaterial mat)
{
	return round(mat.uvRect.zw * 4096.0);
}

// Mascara M1: material map por pieza, alpha = COBERTURA del trim. Bytes
// crudos (el atlas es SRGB y SampleDataTexel deshace la linealizacion).
float TrimCoverage(PTMaterial mat, float2 rawUv)
{
	float2 local = ((mat.trimPack & 131072u) != 0u) ? frac(rawUv) : rawUv;
	float2 uv = mat.uvRectTrim.xy + local * mat.uvRectTrim.zw;
	return gAtlas.SampleLevel(gSampPoint, float3(uv, float(TrimSlice(mat))), 0.0).a;
}

// Cuantizacion a byte. charge hace step(stHash, fill) y spark
// smoothstep(0.97, 1.0, stHash): un residuo sub-1/255 en `base` voltea un
// texel y el critico lo lee como "grano distinto". En el raster `base` es
// literalmente un byte/255. Gate G6.
float3 TrimQuantize(float3 c)
{
	return floor(saturate(c) * 255.0 + 0.5) / 255.0;
}

)HLSL" R"HLSL(// Radiancia (lineal) que ANADE el trim en un punto de la superficie.
// texelLin = texel del albedo YA lineal; vertRgb = color de vertice.
float3 TrimRadiance(PTMaterial mat, float2 rawUv, float3 texelLin,
                    float3 vertRgb, float3 posWorld)
{
	uint fx = TrimFx(mat);
	if (fx == 0u)
		return float3(0.0, 0.0, 0.0);

	// Mascara ANTES de tocar el ruido: la salida temprana por cobertura 0 es
	// la mitigacion de coste de P6 que el diseno ya lleva puesta.
	float cov = TrimCoverage(mat, rawUv);
	if (cov <= 0.0)
		return float3(0.0, 0.0, 0.0);

	// ESPACIO: solo CUERPO en v1 (D11). s.pos es la posicion de MUNDO del hit
	// en las MISMAS unidades que vPosition, asi que la paridad con stBody() es
	// EXACTA -- incluida la limitacion "shower door" del raster, que es
	// precisamente lo que P3 quiere heredar. PANTALLA no se implementa: los 20
	// efectos tienen default_space body, y en un path tracer un patron en
	// espacio de pantalla es semanticamente incorrecto (un reflejo veria el
	// patron de la camara pegado a la superficie reflejada).
	float2 p = float2(posWorld.x - mat.trimOrigin.x,
	                  posWorld.z - mat.trimOrigin.z) * trimNoiseSize.w;

	float2 texSize = TrimTexSize(mat);
	float2 local = ((mat.trimPack & 131072u) != 0u) ? frac(rawUv) : rawUv;
	float2 tx = floor(local * texSize);

	float3 base = TrimQuantize(LinearToSrgb3(texelLin));
	float3 add = TrimAddDispatch(fx, base, p, tx, local, texSize, trimTime.w);

	// OUTLINE: la cascara invertida del outline 3D lleva un suelo de
	// fullbright en el borde (mismo termino que el raster).
	if ((mat.trimPack & 65536u) != 0u)
	{
		cov *= 1.0 + 0.18 * stWavep(1600.0, stLum(base) * 1.3);
		add = max(add, base);
	}

	// D5: el add del raster se recorta a 1.0 en render_surface[1]
	// (R8G8B8A8_UNORM), asi que spark (max 4.110) y molten (max/p90 = 6.87)
	// pueden salir MAS BRILLANTES en Cinema. El flag esta APAGADO por defecto
	// (Cinema es HDR); existe porque es la diferencia mas visible entre
	// motores y la decide el critico mirando la galeria.
	//
	// CUIDADO -- ESTO SE MIDIO Y ESTABA MAL. SrgbToLinear3 empieza por
	// saturate(), asi que la conversion de vuelta YA recortaba a 1.0 pasara lo
	// que pasara con el flag: la columna "con recorte" salia BYTE A BYTE igual
	// que la columna "sin recorte" (sha256 identico en charge/spark/molten/
	// tide), o sea que el flag era codigo muerto y la promesa de HDR era falsa.
	// Con el flag APAGADO hay que EXTENDER la EOTF por encima de 1: se
	// normaliza el color a su canal maximo, se decodifica el color normalizado
	// (que ya cae en el dominio donde sRGB esta definido) y se devuelve la
	// magnitud multiplicando en LINEAL. Asi el TONO se conserva exactamente y
	// la energia por encima de fullbright sobrevive, que es lo unico que hace
	// distinto a un motor HDR.
	float3 addLin;
	if ((counts.w & FRAMEF_TRIM_LDR_CLIP) != 0u)
	{
		addLin = SrgbToLinear3(saturate(add));
	}
	else
	{
		float m = max(max(add.r, add.g), max(add.b, 1.0));
		addLin = SrgbToLinear3(add / m) * m;
	}

	return addLin * mat.blend.rgb * vertRgb
	     * (cov * mat.trimOrigin.w * TrimCinemaGain(fx) * trimNoiseSize.z);
}
#endif

// ---------------------------------------------------------------------------
// M2: Fresnel y GGX single-scatter (VNDF Heitz 2018 + Smith height-
// correlated). Todo con el MISMO orden de operaciones que el oraculo CPU.
// ---------------------------------------------------------------------------
// Schlick con pow5 por multiplicaciones explicitas
float3 SchlickF3(float3 f0, float cosX)
{
	float p = saturate(1.0 - cosX);
	float p2 = p * p;
	float p5 = p2 * p2 * p;
	return f0 + (float3(1.0, 1.0, 1.0) - f0) * p5;
}

// Fresnel dielectrico exacto (etaRel = n_incidente / n_transmitido); cosI y
// cosT positivos. El caller detecta TIR antes (sin2T >= 1 -> F = 1).
float FresnelDielectric(float cosI, float cosT, float etaRel)
{
	float rs = (etaRel * cosI - cosT) / max(etaRel * cosI + cosT, 1.0e-8);
	float rp = (cosI - etaRel * cosT) / max(cosI + etaRel * cosT, 1.0e-8);
	return saturate(0.5 * (rs * rs + rp * rp));
}

// D de GGX. alphaG = roughness^2: la MISMA convencion del raster
// (distributionGGX usa a2 = roughness^4, shader_high_light_sun.fsh:66)
float GgxD(float cosNH, float alphaG)
{
	float a2 = alphaG * alphaG;
	float d = cosNH * cosNH * (a2 - 1.0) + 1.0;
	return a2 / max(PT_PI * d * d, 1.0e-12);
}

// Lambda de Smith para GGX; G1 = 1/(1+L), G2 height-correlated = 1/(1+Lo+Li)
float GgxLambda(float cosX, float alphaG)
{
	float c2 = cosX * cosX;
	float t2 = max(0.0, 1.0 - c2) / max(c2, 1.0e-8);
	return 0.5 * (sqrt(1.0 + alphaG * alphaG * t2) - 1.0);
}

// Muestreo VNDF (Heitz 2018) en marco local (z = normal), woL.z > 0.
// pdf del wi reflejado = G1(wo) * D(h) / (4 * cosO): dot(wo,h) se cancela.
float3 SampleGgxVndf(float3 woL, float alphaG, float2 r)
{
	float3 vh = normalize(float3(alphaG * woL.x, alphaG * woL.y, woL.z));
	float lensq = vh.x * vh.x + vh.y * vh.y;
	float3 t1v = (lensq > 1.0e-12) ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
	float3 t2v = cross(vh, t1v);
	float rr = sqrt(r.x);
	float phi = PT_TWO_PI * r.y;
	float t1 = rr * cos(phi);
	float t2 = rr * sin(phi);
	float s = 0.5 * (1.0 + vh.z);
	t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
	float3 nh = t1 * t1v + t2 * t2v + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * vh;
	return normalize(float3(alphaG * nh.x, alphaG * nh.y, max(1.0e-6, nh.z)));
}

// ---------------------------------------------------------------------------
// Interseccion rayo-AABB (slabs). Devuelve la distancia de entrada o
// PT_T_MAX si no hay solape en (0, tMax).
// ---------------------------------------------------------------------------
float3 SafeInvDir(float3 d)
{
	// Evita 0 * inf = NaN en el test de slabs preservando el signo
	float3 s = float3(d.x >= 0.0 ? 1.0 : -1.0,
	                  d.y >= 0.0 ? 1.0 : -1.0,
	                  d.z >= 0.0 ? 1.0 : -1.0);
	return s / max(abs(d), 1.0e-12);
}

float IntersectAabb(float3 bmin, float3 bmax, float3 org, float3 invDir, float tMax)
{
	float3 t0 = (bmin - org) * invDir;
	float3 t1 = (bmax - org) * invDir;
	float3 tn = min(t0, t1);
	float3 tf = max(t0, t1);
	float tNear = max(max(tn.x, tn.y), max(tn.z, 0.0));
	float tFar = min(min(tf.x, tf.y), min(tf.z, tMax));
	return (tNear <= tFar) ? tNear : PT_T_MAX;
}

// ---------------------------------------------------------------------------
// Moller-Trumbore SIN backface cull (la politica two-sided de M0 voltea la
// normal en el shading, no aqui). El anti-autointerseccion combina el offset
// del origen por la normal geometrica (en el integrador) con el tMin del
// caller.
// ---------------------------------------------------------------------------
bool IntersectTri(float3 org, float3 dir, float3 p0, float3 p1, float3 p2,
                  float tMin, float tMax, out float t, out float bu, out float bv)
{
	t = 0.0; bu = 0.0; bv = 0.0;
	float3 e1 = p1 - p0;
	float3 e2 = p2 - p0;
	float3 pv = cross(dir, e2);
	float det = dot(e1, pv);
	if (abs(det) < 1.0e-12)
		return false;
	float invDet = 1.0 / det;
	float3 tv = org - p0;
	bu = dot(tv, pv) * invDet;
	if (bu < 0.0 || bu > 1.0)
		return false;
	float3 qv = cross(tv, e1);
	bv = dot(dir, qv) * invDet;
	if (bv < 0.0 || bu + bv > 1.0)
		return false;
	t = dot(e2, qv) * invDet;
	return (t > tMin && t < tMax);
}

// ---------------------------------------------------------------------------
// R6: backface culling replicado del raster. La normal geometrica ng se saca
// del winding ALMACENADO (cross de aristas i0->i1, i0->i2, SIN ningun flip; el
// flatten ya no repara winding). El rasterizer del motor usa
// FrontCounterClockwise = FALSE (D3D11_RASTERIZER_DESC {}); bajo esa convencion
// la cara que SOBREVIVE a CULL_BACK es la que mira al observador, i.e. su
// normal geometrica apunta CONTRA el rayo: dot(ng, dir) < 0. Por tanto CULL_BACK
// descarta dot(ng,dir) > 0 y CULL_FRONT descarta dot(ng,dir) < 0. El caso
// borde exacto (dot == 0, cara de canto, area de pantalla nula) no se culla.
// El rayo CONTINUA tras el descarte (como el ghost-skip de texels), sin gastar
// el presupuesto PT_MAX_TRANSP (es geometria, no transparencia).
bool FaceCulled(uint flags, float3 p0, float3 p1, float3 p2, float3 dir)
{
	uint cf = flags & (MATF_CULL_BACK | MATF_CULL_FRONT);
	if (cf == 0u)
		return false;
	float sd = dot(cross(p1 - p0, p2 - p0), dir);
	if ((cf & MATF_CULL_BACK) != 0u && sd > 0.0)
		return true;
	if ((cf & MATF_CULL_FRONT) != 0u && sd < 0.0)
		return true;
	return false;
}

// A1: muestreo de la PUPILA. blades < 3 -> disco unitario uniforme (r = sqrt(u.x),
// EXACTAMENTE la formula de R6: con blades 0 esta funcion es bit-identica a la
// anterior). blades >= 3 -> poligono regular INSCRITO en el disco unitario,
// muestreado uniforme EN AREA (mapa sector->triangulo; reusa u.x tras extraer el
// sector, sin consumir dimensiones RNG extra) y RE-ESCALADO para conservar el
// AREA del disco: el usuario cambia la FORMA del bokeh, no su tamano ni su
// energia. Despues: squash anamorfico y rotacion (mismo orden que el raster,
// render_generate_dof_samples: mascara -> ratio -> rotacion).
// El oraculo (CinemaOracle.cpp, OLensApertureSample) es espejo 1:1 de esto.
float2 LensApertureSample(float2 u, float blades, float rotRad, float ratio)
{
	float2 p;
	if (blades < 3.0)
	{
		float r = sqrt(u.x);
		float ang = PT_TWO_PI * u.y;
		p = float2(r * cos(ang), r * sin(ang));
	}
	else
	{
		float n = floor(blades);
		float sector = floor(u.x * n);
		float su = u.x * n - sector;          // resto reutilizado (no gasta dims)
		// punto uniforme en el triangulo (0,0),(cos a, -sin a),(cos a, +sin a)
		float a = PT_PI / n;                  // medio angulo del sector
		float s = sqrt(su);
		float t = u.y * s;
		float w0 = 1.0 - s;
		p = float2((w0 + t) * cos(a), (w0 - t) * sin(a));
		// rotacion al sector correspondiente
		float sang = sector * 2.0 * a;
		float cs = cos(sang), sn = sin(sang);
		p = float2(cs * p.x - sn * p.y, sn * p.x + cs * p.y);
		// area del poligono inscrito = (n/2) sin(2pi/n) < pi -> re-escalar a area pi
		float areaPoly = 0.5 * n * sin(PT_TWO_PI / n);
		p *= sqrt(PT_PI / areaPoly);
	}
	// anamorfico (mismo signo/orden que el raster) y rotacion de usuario
	p.y *= (1.0 - ratio);
	float cr = cos(rotRad), sr = sin(rotRad);
	return float2(cr * p.x - sr * p.y, sr * p.x + cr * p.y);
}

// ---------------------------------------------------------------------------
// TraceClosest: traversal del BVH (layout PTNode de CinemaBvh.hpp: hoja por
// bit 31 de leftOrFirst, hijo derecho explicito en rightOrCount), stack de
// 48, visita near-first por distancia de entrada a los AABB de los hijos.
// Alpha-test cutout DENTRO del traversal: si el texel del hit tiene
// alpha < 0.5 el hit se IGNORA y la travesia CONTINUA con el mismo stack
// (JAMAS relanzar el rayo desde t+eps: re-pagaria la travesia entera por
// cada hoja de vegetacion, seccion 5 del diseno). Presupuesto PT_MAX_TRANSP
// de texels fantasma por rayo; agotado, el cutout pasa a comportarse opaco.
// TraceClosest/TraceShadow son la costura para un backend RayQuery (fase 2).
// ---------------------------------------------------------------------------
struct HitInfo
{
	float t;      // distancia del hit (PT_T_MAX si miss)
	uint  tri;    // indice del triangulo (PT_INVALID si miss)
	float bu, bv; // baricentricas de i1 e i2
#ifdef CINEMA_RAYQUERY
	// M8-A: object->world de la instancia impactada (identidad con BLAS unica).
	// EvalSurface/TriArea transforman los verts LOCALES a mundo con esta matriz.
	float3x4 o2w;
#endif
};

#ifdef CINEMA_RAYQUERY
)HLSL" R"HLSL(// ===========================================================================
// FASE B (M6): TraceClosest / TraceShadow por RayQuery inline (Tier 1.1). La
// traversal del BVH, el near-first y el tMax = best.t los hace el HARDWARE
// (gScene = TLAS). Geometria NON_OPAQUE -> Proceed() para en cada triangulo
// candidato y el bucle replica EXACTAMENTE los descartes del kernel software:
//   - normal geometrica RECOMPUTADA de los verts (NO CandidateTriangleFrontFace,
//     que usa otra convencion) -> mismo FaceCulled sign(dot(ng, dir)).
//   - baricentricas de DXR: b.x pesa i1, b.y pesa i2 == (bu, bv) de Moller-
//     Trumbore del kernel software -> TriRawUv/SampleAlbedo identicos.
//   - PrimitiveIndex == triIdx (BLAS desde el array reordenado bvh.tris).
// El kernel software (traversal por stack) queda INTACTO en el #else de abajo
// (fallback D3D11 + espejo del oraculo). El shading/NEE/MIS/RR/RNG no cambian.
// ===========================================================================
// M8-A: cull de cara con geometria LOCAL transformada a mundo por instancia. El
// signo del det de la instancia compensa el winding de las instancias espejadas
// (det<0): sin swap en la BLAS, el cross de las aristas MUNDO sale con el signo
// opuesto al del flatten-mundo-con-swap de hoy; detSign lo restaura (verificado
// contra r8_mirror). Con la instancia identidad (OFF) detSign=+1 -> no-op.
bool FaceCulledInst(uint flags, float3 p0w, float3 p1w, float3 p2w, float3 dir, float detSign)
{
	uint cf = flags & (MATF_CULL_BACK | MATF_CULL_FRONT);
	if (cf == 0u)
		return false;
	float sd = detSign * dot(cross(p1w - p0w, p2w - p0w), dir);
	if ((cf & MATF_CULL_BACK) != 0u && sd > 0.0)
		return true;
	if ((cf & MATF_CULL_FRONT) != 0u && sd < 0.0)
		return true;
	return false;
}

// R15 (holdout): isCameraRay marca el segmento de CAMARA (primario, rayo de
// lente del DoF y sus continuaciones rectas por transparencia; false desde el
// primer scatter). Con el, los triangulos MATF_CAM_INVISIBLE se ignoran y el
// rayo CONTINUA -- igual que una cara culled, y sin gastar el presupuesto
// PT_MAX_TRANSP (es geometria, no transparencia). TraceShadow NUNCA filtra.
HitInfo TraceClosest(float3 org, float3 dir, float tMin, bool isCameraRay)
{
	HitInfo best;
	best.t = PT_T_MAX;
	best.tri = PT_INVALID;
	best.bu = 0.0;
	best.bv = 0.0;
	best.o2w = float3x4(1,0,0,0, 0,1,0,0, 0,0,1,0);

	RayDesc ray;
	ray.Origin = org;
	ray.Direction = dir;
	ray.TMin = tMin;
	ray.TMax = PT_T_MAX;

	RayQuery<RAY_FLAG_NONE> q;
	q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xffu, ray);

	uint transpLeft = PT_MAX_TRANSP;
	bool holdoutCrossed = false; // R15: ver PT_MAX_TRANSP_HOLDOUT
	while (q.Proceed())
	{
		if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
		{
			// M8-A: primitiva LOCAL de la BLAS -> tri GLOBAL; verts LOCALES a mundo.
			float3x4 o2w = q.CandidateObjectToWorld3x4();
			uint triIdx = gInstBase[q.CandidateInstanceIndex()] + q.CandidatePrimitiveIndex();
			PTTri tri = gTris[triIdx];
			float3 p0 = XformPos(o2w, PtVertPos(gVerts[tri.i0]));
			float3 p1 = XformPos(o2w, PtVertPos(gVerts[tri.i1]));
			float3 p2 = XformPos(o2w, PtVertPos(gVerts[tri.i2]));
			PTMaterial hmat = gMats[tri.matId];
			// (0) R15 holdout: invisible SOLO al rayo de camara -> no comitear
			// (el rayo sigue). Sombras/GI/reflejos entran aqui con isCameraRay
			// false y lo ven normal.
			if (isCameraRay && (hmat.flags & MATF_CAM_INVISIBLE) != 0u)
			{
				// El rayo sigue MUCHO mas lejos de lo normal -> subir el
				// presupuesto de texels fantasma una sola vez (salvaguarda,
				// ver la nota de PT_MAX_TRANSP_HOLDOUT).
				if (!holdoutCrossed)
				{
					holdoutCrossed = true;
					transpLeft = PT_MAX_TRANSP_HOLDOUT;
				}
				continue;
			}
			// (1) backface cull del raster: candidato NO comiteado (rayo sigue),
			// sin gastar el presupuesto de transparencia (es geometria).
			if (FaceCulledInst(hmat.flags, p0, p1, p2, dir, DetSign3x4(o2w)))
				continue;
			float2 cb = q.CandidateTriangleBarycentrics();
			bool ghost = false;
			if ((hmat.flags & MATF_ALPHA_TEST) != 0u && transpLeft > 0u)
			{
				// (2) cutout: alpha < 0.5 -> texel fantasma
				float a = SampleAlbedo(hmat, TriRawUv(tri, cb.x, cb.y)).a;
				if (a < 0.5)
				{
					ghost = true;
					transpLeft--;
				}
			}
			else if ((hmat.flags & (MATF_ALPHA_BLEND | MATF_WATER)) == MATF_ALPHA_BLEND && transpLeft > 0u)
			{
				// (3) ghost-texel del overlay ALPHA_BLEND puro (aEff < 0.004):
				// velo lechoso de goggles/skin-overlay. WATER excluido (== puro).
				float aEff = SampleAlbedo(hmat, TriRawUv(tri, cb.x, cb.y)).a * hmat.blend.a;
				if (aEff < 0.004)
				{
					ghost = true;
					transpLeft--;
				}
			}
			// Cualquier otro candidato (incl. cutout solido o presupuesto
			// agotado) es un hit real -> comitear (el HW poda el resto por tMax).
			if (!ghost)
				q.CommitNonOpaqueTriangleHit();
		}
	}

	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		best.t = q.CommittedRayT();
		best.tri = gInstBase[q.CommittedInstanceIndex()] + q.CommittedPrimitiveIndex();
		best.o2w = q.CommittedObjectToWorld3x4();
		float2 cb = q.CommittedTriangleBarycentrics();
		best.bu = cb.x;
		best.bv = cb.y;
	}
	return best;
}

// F1 4a: el shadow-catcher necesita distinguir "me tapa el SUJETO" (que ES una
// sombra) de "me tapa el CUERPO DE LA PROPIA LAMPARA QUE ESTOY MUESTREANDO"
// (que NO lo es: la NEE de MI3 emite por las DOS caras -- cosL con abs -- asi
// que la mitad de los samples de una placa emisiva solida caen en su cara de
// atras y el rayo de sombra choca contra la propia placa. Medido: con un
// softbox CUBE eso metia alfa espurio p99 132/255 sobre un suelo SIN NADA que
// le hiciera sombra; con un emisor SURFACE de una sola cara salia 0 exacto).
//
// RONDA 5 -- FIX DEL ACANTILADO. La primera version sacaba un BOOLEANO ("el que
// corta es emisivo"). Eso era un acantilado: CUALQUIER ocluyente emisivo -- una
// antorcha, un trim brillante de ShinyTrims, un cubo con EMISSIVE 0,001 --
// igualaba numerador y denominador y borraba la sombra ENTERA del cuadro.
// MEDIDO (suelo catcher + cubo solido encima, 384^2, 256 spp): con EMISSIVE 0
// el catcher tiene 34.706 px de sombra; con EMISSIVE 0,001 quedan 405, y esos
// 405 son SOLO la silueta antialiaseada del propio cubo -- de sombra, cero.
// Ahora lo que sale es la IDENTIDAD del bloqueador --
// oBlkMat = matId del triangulo que corto el rayo, PT_INVALID si no lo corto
// nadie -- y quien decide es el CALL SITE, el unico que sabe QUE fuente esta
// muestreando:
//   - grupo emisivo (NEE por CDF): es auto-sombra SOLO si el bloqueador es el
//     MISMO objeto emisivo que el emisor muestreado. matId es una entrada POR
//     DRAW (CinemaScene.cpp: scene.materials[p] = BuildMaterial(draws[plan[p]]))
//     asi que "mismo matId" == "mismo objeto", no "mismo triangulo": en el caso
//     2.1 la cara DELANTERA del cubo del softbox tapa un punto muestreado en su
//     cara de ATRAS, y son dos triangulos distintos del mismo draw. Comparar
//     indices de triangulo (o de slot de la CDF) NO lo cubriria.
//   - sol y luces ANALITICAS: NUNCA hay auto-sombra. No tienen geometria en el
//     BVH, asi que ningun triangulo puede ser "su propio cuerpo"; lo que corte
//     el rayo es siempre un ocluyente de verdad. Esos dos call sites llaman ya
//     a la sobrecarga historica de 4 parametros.
// La sobrecarga sin el parametro deja TODOS los call sites de siempre intactos.
float3 TraceShadow(float3 org, float3 dir, float tMin, float tMax, out uint oBlkMat)
{
	oBlkMat = PT_INVALID;
	float3 trans = float3(1.0, 1.0, 1.0);

	RayDesc ray;
	ray.Origin = org;
	ray.Direction = dir;
	ray.TMin = tMin;
	ray.TMax = tMax;

	RayQuery<RAY_FLAG_NONE> q;
	q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xffu, ray);

	uint transpLeft = PT_MAX_TRANSP;
	while (q.Proceed())
	{
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		// M8-A: primitiva LOCAL -> tri GLOBAL; verts LOCALES a mundo por instancia.
		float3x4 o2w = q.CandidateObjectToWorld3x4();
		uint triIdx = gInstBase[q.CandidateInstanceIndex()] + q.CandidatePrimitiveIndex();
		PTTri tri = gTris[triIdx];
		float3 p0 = XformPos(o2w, PtVertPos(gVerts[tri.i0]));
		float3 p1 = XformPos(o2w, PtVertPos(gVerts[tri.i1]));
		float3 p2 = XformPos(o2w, PtVertPos(gVerts[tri.i2]));
		PTMaterial smat = gMats[tri.matId];
		// R8: culling del pase de SOMBRA = facing DESDE LA LUZ (-dir). El shadow
		// map del raster rasteriza las caras vistas desde la luz.
		if (FaceCulledInst(smat.flags, p0, p1, p2, -dir, DetSign3x4(o2w)))
			continue;
		if ((smat.flags & MATF_NO_SHADOW) != 0u)
			continue; // R4: nubes no proyectan sombra (rayo sigue sin atenuar)
		float2 cb = q.CandidateTriangleBarycentrics();
		if ((smat.flags & MATF_ALPHA_TEST) != 0u && transpLeft > 0u)
		{
			float a = SampleAlbedo(smat, TriRawUv(tri, cb.x, cb.y)).a;
			if (a < 0.5)
			{
				transpLeft--;
				continue; // texel fantasma del cutout
			}
			// a >= 0.5: opaco -> ocluido (cae al return de abajo)
		}
		else if ((smat.flags & (MATF_ALPHA_BLEND | MATF_WATER)) != 0u && transpLeft > 0u)
		{
			// Sombra coloreada: cristal/agua ACUMULA transmitancia SIN comitear
			// (nunca es un hit; el producto es conmutativo -> el orden arbitrario
			// de candidatos del HW no cambia el resultado).
			float4 texel = SampleAlbedo(smat, TriRawUv(tri, cb.x, cb.y));
			float aEff = saturate(texel.a * smat.blend.a);
			// COBERTURA: transmitancia (1-aEff) SIN TINTE. La fraccion cubierta
			// es opaca (bloquea) y la descubierta deja pasar la luz intacta:
			// no hay medio absorbente que tenir. Sale sola de la semantica, sin
			// caso especial (cf. cristal tintado, que si multiplica por el RGB).
			if ((smat.flags & MATF_COVERAGE) != 0u)
				trans *= (1.0 - aEff);
			else
				trans *= texel.rgb * smat.blend.rgb * (1.0 - aEff);
			transpLeft--;
			if (trans.r + trans.g + trans.b < 1.0e-4)
			{
				oBlkMat = tri.matId;
				return float3(0.0, 0.0, 0.0); // filtro ya opaco -> early-out
			}
			continue;
		}
		// Oclusor opaco (o presupuesto agotado): primer hit solido = sombra total
		oBlkMat = tri.matId;
		return float3(0.0, 0.0, 0.0);
	}

	return trans; // (1,1,1) = visible sin filtros
}

// Sobrecarga historica: identica, sin el dato del F1. Los call sites de siempre
// la siguen llamando y su codigo no cambia ni una linea.
float3 TraceShadow(float3 org, float3 dir, float tMin, float tMax)
{
	uint blkMatIgnored;
	return TraceShadow(org, dir, tMin, tMax, blkMatIgnored);
}
)HLSL" R"HLSL(
#else
// R15 (holdout): ver la nota de isCameraRay en la variante RayQuery.
HitInfo TraceClosest(float3 org, float3 dir, float tMin, bool isCameraRay)
{
	HitInfo best;
	best.t = PT_T_MAX;
	best.tri = PT_INVALID;
	best.bu = 0.0;
	best.bv = 0.0;

	float3 invDir = SafeInvDir(dir);
	uint transpLeft = PT_MAX_TRANSP;
	bool holdoutCrossed = false; // R15: ver PT_MAX_TRANSP_HOLDOUT
	uint stack[PT_STACK_SIZE];
	uint sp = 0u;
	uint node = 0u; // raiz

	[loop]
	while (true)
	{
		PTNode n = gNodes[node];
		if ((n.leftOrFirst & PT_NODE_LEAF_BIT) != 0u)
		{
			// Hoja: leftOrFirst trae PT_NODE_LEAF_BIT | primer tri (array
			// reordenado por hoja); rightOrCount el numero de tris
			uint first = n.leftOrFirst & ~PT_NODE_LEAF_BIT;
			for (uint i = 0u; i < n.rightOrCount; i++)
			{
				uint triIdx = first + i;
				PTTri tri = gTris[triIdx];
				float3 p0 = PtVertPos(gVerts[tri.i0]);
				float3 p1 = PtVertPos(gVerts[tri.i1]);
				float3 p2 = PtVertPos(gVerts[tri.i2]);
				float t, bu, bv;
				if (IntersectTri(org, dir, p0, p1, p2, tMin, best.t, t, bu, bv))
				{
					PTMaterial hmat = gMats[tri.matId];
					// R15 holdout: invisible SOLO al rayo de camara; el rayo
					// CONTINUA sin gastar presupuesto de transparencia.
					if (isCameraRay && (hmat.flags & MATF_CAM_INVISIBLE) != 0u)
					{
						// Presupuesto ampliado una sola vez (PT_MAX_TRANSP_HOLDOUT)
						if (!holdoutCrossed)
						{
							holdoutCrossed = true;
							transpLeft = PT_MAX_TRANSP_HOLDOUT;
						}
						continue;
					}
					// R6: backface culling del raster. Cara culled -> el rayo
					// CONTINUA (no gasta el presupuesto de transparencia).
					if (FaceCulled(hmat.flags, p0, p1, p2, dir))
						continue;
					bool accept = true;
					if ((hmat.flags & MATF_ALPHA_TEST) != 0u && transpLeft > 0u)
					{
						// Cutout: nearest mip 0; alpha < 0.5 -> hit fantasma
						float a = SampleAlbedo(hmat, TriRawUv(tri, bu, bv)).a;
						if (a < 0.5)
						{
							accept = false;
							transpLeft--;
						}
					}
					else if ((hmat.flags & (MATF_ALPHA_BLEND | MATF_WATER)) == MATF_ALPHA_BLEND && transpLeft > 0u)
					{
						// Texel fantasma del overlay: la capa translucida (goggles)
						// deja casi todo el cuerpo con aEff ~ 0. Como PT_KIND_GLASS
						// esos texels transmiten pero anadian una reflexion Fresnel
						// blanca por TODO el cuerpo -> velo lechoso. El raster los
						// DESCARTA. Consistencia con TraceShadow (transmision 1).
						float aEff = SampleAlbedo(hmat, TriRawUv(tri, bu, bv)).a * hmat.blend.a;
						if (aEff < 0.004)
						{
							accept = false;
							transpLeft--;
						}
					}
					if (accept)
					{
						best.t = t;
						best.tri = triIdx;
						best.bu = bu;
						best.bv = bv;
					}
				}
			}
			if (sp == 0u)
				break;
			node = stack[--sp];
		}
		else
		{
			// Interno: hijo izq en leftOrFirst, hijo der en rightOrCount
			// (NO contiguos); near-first por distancia de entrada
			uint c0 = n.leftOrFirst;
			uint c1 = n.rightOrCount;
			PTNode n0 = gNodes[c0];
			PTNode n1 = gNodes[c1];
			float d0 = IntersectAabb(float3(n0.bminx, n0.bminy, n0.bminz),
			                         float3(n0.bmaxx, n0.bmaxy, n0.bmaxz), org, invDir, best.t);
			float d1 = IntersectAabb(float3(n1.bminx, n1.bminy, n1.bminz),
			                         float3(n1.bmaxx, n1.bmaxy, n1.bmaxz), org, invDir, best.t);
			if (d0 > d1)
			{
				float dtmp = d0; d0 = d1; d1 = dtmp;
				uint ctmp = c0; c0 = c1; c1 = ctmp;
			}
			if (d0 == PT_T_MAX)
			{
				// Ningun hijo intersecta: pop
				if (sp == 0u)
					break;
				node = stack[--sp];
			}
			else
			{
				node = c0;
				if (d1 < PT_T_MAX)
				{
					// Stack lleno no deberia ocurrir con profundidad 48; si
					// ocurriera se omite el hijo lejano (artefacto local,
					// preferible a corromper memoria)
					if (sp < PT_STACK_SIZE)
						stack[sp++] = c1;
				}
			}
		}
	}

	return best;
}

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// TraceShadow M1: TRANSMITANCIA float3 hacia una fuente (antes: visibilidad
// binaria float). Early-out al primer hit opaco (devuelve negro). Los cutout
// ALPHA_TEST dejan pasar sus texels con alpha < 0.5; ALPHA_BLEND / WATER
// multiplican la transmitancia por texel.rgb * blend.rgb * (1 - alphaEff)
// con alphaEff = texel.a * blend.a (alpha del texel x alpha de objeto):
// sombras COLOREADAS a traves de cristal tintado y agua (seccion 5 del
// diseno; el sustituto barato de las causticas). ALPHA_HASH sin ALPHA_BLEND
// sigue ocluyendo como opaco (su transmision estocastica es de rayos de
// camino). Presupuesto PT_MAX_TRANSP compartido entre texels fantasma y
// capas de cristal/agua; agotado, todo pasa a opaco. Sin orden near-first:
// la acumulacion multiplicativa es conmutativa y ahorra los dos swaps.
// ---------------------------------------------------------------------------
// F1 4a + RONDA 5 (fix del acantilado): oBlkMat = matId del triangulo que corto
// el rayo, PT_INVALID si no lo corto nadie. Espejo EXACTO de la variante
// RayQuery -- la explicacion larga (por que la identidad y no un booleano
// "es emisivo", y por que matId y no el slot de la CDF) esta alli.
float3 TraceShadow(float3 org, float3 dir, float tMin, float tMax, out uint oBlkMat)
{
	oBlkMat = PT_INVALID;
	float3 trans = float3(1.0, 1.0, 1.0);
	float3 invDir = SafeInvDir(dir);
	uint transpLeft = PT_MAX_TRANSP;
	uint stack[PT_STACK_SIZE];
	uint sp = 0u;
	uint node = 0u;

	[loop]
	while (true)
	{
		PTNode n = gNodes[node];
		if ((n.leftOrFirst & PT_NODE_LEAF_BIT) != 0u)
		{
			uint first = n.leftOrFirst & ~PT_NODE_LEAF_BIT;
			for (uint i = 0u; i < n.rightOrCount; i++)
			{
				uint triIdx = first + i;
				PTTri tri = gTris[triIdx];
				float3 p0 = PtVertPos(gVerts[tri.i0]);
				float3 p1 = PtVertPos(gVerts[tri.i1]);
				float3 p2 = PtVertPos(gVerts[tri.i2]);
				float t, bu, bv;
				if (IntersectTri(org, dir, p0, p1, p2, tMin, tMax, t, bu, bv))
				{
					PTMaterial smat = gMats[tri.matId];
					// R8: culling del pase de SOMBRA -> evaluar el facing DESDE LA
					// LUZ. El shadow map del raster rasteriza las caras vistas
					// desde la luz (mismo cull mode que el ojo, render_set_culling
					// !backfaces, pero viewpoint = luz): la cara que NO proyecta
					// sombra es back-face RESPECTO A LA LUZ, o sea el test con la
					// direccion luz->punto = -dir. (R6 usaba dir y ocluia el
					// interior de shells invertidos abiertos -> mochila negra.)
					if (FaceCulled(smat.flags, p0, p1, p2, -dir))
						continue;
					if ((smat.flags & MATF_NO_SHADOW) != 0u)
						continue; // R4: nubes no proyectan sombra (rayo sigue sin atenuar)
					if ((smat.flags & MATF_ALPHA_TEST) != 0u && transpLeft > 0u)
					{
						float a = SampleAlbedo(smat, TriRawUv(tri, bu, bv)).a;
						if (a < 0.5)
						{
							transpLeft--;
							continue; // texel fantasma del cutout
						}
					}
					else if ((smat.flags & (MATF_ALPHA_BLEND | MATF_WATER)) != 0u && transpLeft > 0u)
					{
						// Sombra coloreada: el cristal/agua filtra, no ocluye
						float4 texel = SampleAlbedo(smat, TriRawUv(tri, bu, bv));
						float aEff = saturate(texel.a * smat.blend.a);
						// COBERTURA: (1-aEff) sin tinte (ver variante RayQuery)
						if ((smat.flags & MATF_COVERAGE) != 0u)
							trans *= (1.0 - aEff);
						else
							trans *= texel.rgb * smat.blend.rgb * (1.0 - aEff);
						transpLeft--;
						if (trans.r + trans.g + trans.b < 1.0e-4)
						{
							oBlkMat = tri.matId;
							return float3(0.0, 0.0, 0.0); // filtro ya opaco
						}
						continue;
					}
					oBlkMat = tri.matId;
					return float3(0.0, 0.0, 0.0); // ocluido opaco
				}
			}
			if (sp == 0u)
				break;
			node = stack[--sp];
		}
		else
		{
			// Interno: hijo izq en leftOrFirst, hijo der en rightOrCount
			uint c0 = n.leftOrFirst;
			uint c1 = n.rightOrCount;
			PTNode n0 = gNodes[c0];
			PTNode n1 = gNodes[c1];
			float d0 = IntersectAabb(float3(n0.bminx, n0.bminy, n0.bminz),
			                         float3(n0.bmaxx, n0.bmaxy, n0.bmaxz), org, invDir, tMax);
			float d1 = IntersectAabb(float3(n1.bminx, n1.bminy, n1.bminz),
			                         float3(n1.bmaxx, n1.bmaxy, n1.bmaxz), org, invDir, tMax);
			bool h0 = (d0 < PT_T_MAX);
			bool h1 = (d1 < PT_T_MAX);
			if (h0 && h1)
			{
				node = c0;
				if (sp < PT_STACK_SIZE)
					stack[sp++] = c1;
			}
			else if (h0)
				node = c0;
			else if (h1)
				node = c1;
			else
			{
				if (sp == 0u)
					break;
				node = stack[--sp];
			}
		}
	}

	return trans; // transmitancia (1,1,1) = visible sin filtros
}

// Sobrecarga historica: identica, sin el dato del F1 (ver la variante RayQuery).
float3 TraceShadow(float3 org, float3 dir, float tMin, float tMax)
{
	uint blkMatIgnored;
	return TraceShadow(org, dir, tMin, tMax, blkMatIgnored);
}
#endif // CINEMA_RAYQUERY

)HLSL" R"HLSL(// ===========================================================================
// M1: helpers de NEE / MIS
// ===========================================================================

// Luminancia Rec.709 (misma constante en host, CDF emisiva y oraculo)
float LumRec709(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Falloff de distancia point/spot REPLICANDO el forward
// (shader_high_light_point.fsh:264):
//   att = 1 - clamp((d - range*(1 - fade)) / (range * fade), 0, 1)
// SIN 1/d^2: la curva LIGHT_RANGE/LIGHT_FADE_SIZE ES el falloff del look
// MI3. fade acotado: el forward sobrevive a fade 0 por el clamp de inf del
// GLSL, pero d == range daria 0/0 = NaN en HLSL.
float LightFalloff(float dist, float range, float fadeSize)
{
	float fade = clamp(fadeSize, 1.0e-4, 1.0);
	float r = max(range, 1.0e-4);
	return 1.0 - saturate((dist - r * (1.0 - fade)) / (r * fade));
}

// Mascara del cono spot REPLICANDO el difMask del forward
// (shader_high_light_spot.fsh:211). El forward proyecta el punto al frustum
// del spot (fov completo = LIGHT_SPOT_RADIUS) y mide el radio normalizado
// desde el centro de su textura, donde 0.5 = borde del cono:
//   radial = 0.5 * tan(anguloAlEje) / tan(fovMedio)
//   mask   = 1 - clamp((radial - 0.5*sharp) / (0.5*max(0.01, 1-sharp)), 0, 1)
// cosA = coseno entre el eje del spot y la direccion luz->punto. La marca
// cosOuter == -2 del contrato (point light) devuelve mascara 1.
float SpotMask(float cosA, float cosOuter, float sharpness)
{
	if (cosOuter <= -1.5)
		return 1.0; // point light
	if (cosA <= 1.0e-4)
		return 0.0; // detras del plano del spot
	float sinA = sqrt(max(0.0, 1.0 - cosA * cosA));
	float sinO = sqrt(max(0.0, 1.0 - cosOuter * cosOuter));
	float tanHalf = max(sinO / max(cosOuter, 1.0e-4), 1.0e-6);
	float radial = 0.5 * (sinA / cosA) / tanHalf;
	return 1.0 - saturate((radial - 0.5 * sharpness) / (0.5 * max(0.01, 1.0 - sharpness)));
}

// Peso de potencia de una luz analitica para la seleccion proporcional.
// lum * range^2 aproxima el flujo de la curva de falloff del forward.
// La formula ENTRA en la pdf de seleccion (se divide por ella): el oraculo
// CPU debe replicarla exactamente; cambiarla cambia solo la varianza.
float LightPowerWeight(PTLight l)
{
	return LumRec709(l.col) * max(l.range * l.range, 1.0e-4);
}

// Busqueda binaria en la CDF emisiva SIN normalizar: primer slot con
// gEmiCdf[slot] >= target, target en [0, W_emi). Requiere counts.y > 0.
uint SampleEmiCdf(float target)
{
	uint lo = 0u;
	uint hi = counts.y - 1u;
	[loop]
	while (lo < hi)
	{
		uint mid = (lo + hi) >> 1u;
		if (gEmiCdf[mid] >= target)
			hi = mid;
		else
			lo = mid + 1u;
	}
	return lo;
}

// Pertenencia de un tri (indice del array REORDENADO de gTris) a la lista
// emisiva: busqueda binaria sobre gEmiTriIdx (ORDEN ASCENDENTE, contrato del
// host). Devuelve el slot o PT_INVALID: los tris fuera de la lista no
// reciben descuento MIS (su emision entra entera por el hit de BSDF).
uint FindEmiSlot(uint triIdx)
{
	if (counts.y == 0u)
		return PT_INVALID;
	uint lo = 0u;
	uint hi = counts.y - 1u;
	[loop]
	while (lo < hi)
	{
		uint mid = (lo + hi) >> 1u;
		if (gEmiTriIdx[mid] >= triIdx)
			hi = mid;
		else
			lo = mid + 1u;
	}
	return (gEmiTriIdx[lo] == triIdx) ? lo : PT_INVALID;
}

// Probabilidad de seleccion de un slot DENTRO del grupo emisivo (delta de la
// CDF sin normalizar / W_emi)
float EmiSlotProb(uint slot, float wEmi)
{
	float lo = (slot == 0u) ? 0.0 : gEmiCdf[slot - 1u];
	return max(gEmiCdf[slot] - lo, 0.0) / max(wEmi, 1.0e-20);
}

// Area en mundo de un triangulo (para la conversion area -> angulo solido
// del peso MIS en los hits de BSDF sobre emisores)
float TriArea(PTTri tri)
{
	float3 p0 = PtVertPos(gVerts[tri.i0]);
	float3 e1 = PtVertPos(gVerts[tri.i1]) - p0;
	float3 e2 = PtVertPos(gVerts[tri.i2]) - p0;
	return 0.5 * length(cross(e1, e2));
}

#ifdef CINEMA_RAYQUERY
// M8-A: area en MUNDO de un tri con verts LOCALES transformados por o2w (para el
// peso MIS del hit de BSDF sobre un emisor; o2w = hit.o2w committed).
float TriAreaInst(PTTri tri, float3x4 o2w)
{
	float3 p0 = XformPos(o2w, PtVertPos(gVerts[tri.i0]));
	float3 e1 = XformPos(o2w, PtVertPos(gVerts[tri.i1])) - p0;
	float3 e2 = XformPos(o2w, PtVertPos(gVerts[tri.i2])) - p0;
	return 0.5 * length(cross(e1, e2));
}
#endif

// Emision, normal geometrica, area y posicion en un punto (baricentricas
// bu/bv) de un tri emisor. MISMA formula de radiancia que la emision por hit
// del integrador: texel * blend.rgb * colorVert * max(e_mat, e_vert) *
// emissiveScale. La normal sale SIN orientar: el caller usa abs(cos) porque
// los emisores Minecraft radian por sus dos caras (igual que los ve un hit
// de BSDF con la normal volteada hacia el rayo). Un texel de cutout con
// alpha < 0.5 es un agujero: radiancia 0 (muestra vacia, p.ej. bordes del
// portal).
float3 EmissiveRadianceAt(PTTri tri, float bu, float bv,
                          out float3 posOut, out float3 ngOut, out float areaOut)
{
	PTVert v0 = gVerts[tri.i0];
	PTVert v1 = gVerts[tri.i1];
	PTVert v2 = gVerts[tri.i2];
	float bw = 1.0 - bu - bv;
	float3 p0 = PtVertPos(v0);
	float3 p1 = PtVertPos(v1);
	float3 p2 = PtVertPos(v2);
	posOut = p0 * bw + p1 * bu + p2 * bv;

	float3 ng = cross(p1 - p0, p2 - p0);
	float ngLen = length(ng);
	areaOut = 0.5 * ngLen;
	ngOut = (ngLen > 1.0e-20) ? ng / ngLen : float3(0.0, 0.0, 1.0);

	PTMaterial mat = gMats[tri.matId];
	float2 rawUv = PtVertUv(v0) * bw + PtVertUv(v1) * bu + PtVertUv(v2) * bv;
	float4 texel = SampleAlbedo(mat, rawUv);
	if ((mat.flags & MATF_ALPHA_TEST) != 0u && texel.a < 0.5)
		return float3(0.0, 0.0, 0.0);

	float3 vertRgb = DecodeColorAbgr(v0.colorABGR).rgb * bw
	               + DecodeColorAbgr(v1.colorABGR).rgb * bu
	               + DecodeColorAbgr(v2.colorABGR).rgb * bv;
	float eVert = DecodeVertEmissive(v0.dataFlags) * bw
	            + DecodeVertEmissive(v1.dataFlags) * bu
	            + DecodeVertEmissive(v2.dataFlags) * bv;
	float e = max(mat.emissive, eVert);
	float3 le = texel.rgb * mat.blend.rgb * vertRgb * (e * res_clamp.w);
#ifdef CINEMA_TRIM
	// TRIM (P4): la NEE muestrea la radiancia REAL del punto, POR TEXEL. La
	// CDF solo reparte muestras (cota estatica); la ENERGIA sale de aqui, que
	// es lo que hace que el trim ILUMINE la escena de verdad y que la luz
	// ANIME con el efecto.
	if ((mat.flags & MATF_HAS_TRIM) != 0u)
		le += TrimRadiance(mat, rawUv, texel.rgb, vertRgb, posOut) * res_clamp.w;
#endif
	return le;
}

#ifdef CINEMA_RAYQUERY
// M8-A: version instanciada de EmissiveRadianceAt. El sampling de emisores no
// pasa por RayQuery, asi que su o2w (gEmiO2W[slot]) llega por parametro; pos,
// normal y area salen en MUNDO. Todo lo demas (UV/color/emissive/material) es
// invariante a la instancia -> identico a la version mundo.
float3 EmissiveRadianceAtInst(PTTri tri, float bu, float bv, float3x4 o2w,
                              out float3 posOut, out float3 ngOut, out float areaOut)
{
	PTVert v0 = gVerts[tri.i0];
	PTVert v1 = gVerts[tri.i1];
	PTVert v2 = gVerts[tri.i2];
	float bw = 1.0 - bu - bv;
	float3 p0 = XformPos(o2w, PtVertPos(v0));
	float3 p1 = XformPos(o2w, PtVertPos(v1));
	float3 p2 = XformPos(o2w, PtVertPos(v2));
	posOut = p0 * bw + p1 * bu + p2 * bv;

	float3 ng = cross(p1 - p0, p2 - p0);
	float ngLen = length(ng);
	areaOut = 0.5 * ngLen;
	ngOut = (ngLen > 1.0e-20) ? ng / ngLen : float3(0.0, 0.0, 1.0);

	PTMaterial mat = gMats[tri.matId];
	float2 rawUv = PtVertUv(v0) * bw + PtVertUv(v1) * bu + PtVertUv(v2) * bv;
	float4 texel = SampleAlbedo(mat, rawUv);
	if ((mat.flags & MATF_ALPHA_TEST) != 0u && texel.a < 0.5)
		return float3(0.0, 0.0, 0.0);

	float3 vertRgb = DecodeColorAbgr(v0.colorABGR).rgb * bw
	               + DecodeColorAbgr(v1.colorABGR).rgb * bu
	               + DecodeColorAbgr(v2.colorABGR).rgb * bv;
	float eVert = DecodeVertEmissive(v0.dataFlags) * bw
	            + DecodeVertEmissive(v1.dataFlags) * bu
	            + DecodeVertEmissive(v2.dataFlags) * bv;
	float e = max(mat.emissive, eVert);
	float3 le = texel.rgb * mat.blend.rgb * vertRgb * (e * res_clamp.w);
#ifdef CINEMA_TRIM
	// TRIM (P4): la NEE muestrea la radiancia REAL del punto, POR TEXEL. La
	// CDF solo reparte muestras (cota estatica); la ENERGIA sale de aqui, que
	// es lo que hace que el trim ILUMINE la escena de verdad y que la luz
	// ANIME con el efecto.
	if ((mat.flags & MATF_HAS_TRIM) != 0u)
		le += TrimRadiance(mat, rawUv, texel.rgb, vertRgb, posOut) * res_clamp.w;
#endif
	return le;
}
#endif

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// M2: superficie evaluada. albedo conserva la semantica M1 (AOV + emision);
// el shading usa diffColor/f0/alphaG (kind 0), el tinte de cristal o la
// sigma de agua segun kind. frontFace se decide ANTES del volteo de
// normales (entrar/salir del agua).
// ---------------------------------------------------------------------------
struct SurfaceM2
{
	float3 pos;        // punto de interseccion en mundo
	float3 ns;         // normal de shading (normal map aplicado; hacia el rayo)
	float3 ng;         // normal geometrica (hacia el rayo; para offsets)
	float3 albedo;     // texel * blend.rgb * colorVert (AOV + emision, igual M1)
	float3 diffColor;  // kind 0: albedo * (1 - metallic); resto: albedo
	float3 f0;         // kind 0: lerp(0.04, albedo, metallic)
	float3 glassTint;  // kind 3: texel.rgb * blend.rgb * (1 - aEff) (= TraceShadow)
	float  glassAlpha; // kind 3: aEff = texel.a * blend.a (cobertura del film)
	float3 waterSigma; // kind 2: sigma de Beer derivada del blendColor
	float  alphaG;     // kind 0: GGX alpha = roughness^2 (floor PT_ROUGH_MIN)
	float  emissive;   // e combinado material/vertice (0..1)
	uint   kind;       // PT_KIND_*
	float  frontFace;  // 1 = el rayo llego por la cara frontal geometrica
#ifdef CINEMA_TRIM
	// TRIM: radiancia LINEAL que anade el efecto, YA multiplicada por
	// cobertura/intensidad/ganancia. Termino APARTE del escalar `emissive` a
	// proposito: sustituir `emissive` por un float3 cambiaria la forma de la
	// expresion `albedo * (emissive * res_clamp.w)` en TODA escena, y la
	// multiplicacion flotante no es asociativa -- la regresion se decidiria
	// por ULPs. Como termino sumado, sin trims el camino es BIT-IDENTICO
	// porque el campo ni existe.
	float3 trimRgb;
#endif
};

// ---------------------------------------------------------------------------
// Evaluacion de superficie M2 (sustituye al EvalSurface Lambert de M0/M1):
//   albedo = texel_atlas (lineal via formato SRGB) * blend.rgb * rgb del
//            color de vertice (la politica de AO ya viene aplicada por el
//            flatten del host).
//   emision = max(uEmissive del material, byte emissive del vertice), con
//            uDefaultEmissive ya plegado por el host. SIN cambios en M2: el
//            canal emisivo del material map NO se lee (mantendria la CDF de
//            NEE y TraceShadow inconsistentes; queda documentado).
//   normal map (MATF_HAS_NRMTEX): tangente 888 en MUNDO interpolada,
//            Gram-Schmidt contra la ns volteada, bitangente cross(T, N) y
//            decode IDENTICO a getMappedNormal del raster (xy*2-1, z
//            reconstruida, Y invertida, texel con alpha < 0.01 = plano).
//            Guard: si la normal perturbada cae bajo el hemisferio
//            geometrico se conserva la interpolada.
//   metallic/roughness (kind 0): uniforms del draw o uTextureMaterial con
//            los decodes EXACTOS de getMaterial del raster (SEUS: rough =
//            1-R, metal = G; LabPBR: rough = (1-R)^2, metal = G > 0.898).
// Politica two-sided global M0/M1 intacta: normales SIEMPRE hacia el rayo
// (cubre MATF_TWO_SIDED sin caso especial).
// ---------------------------------------------------------------------------
SurfaceM2 EvalSurface(HitInfo hit, float3 rayDir)
{
	PTTri tri = gTris[hit.tri];
	PTVert v0 = gVerts[tri.i0];
	PTVert v1 = gVerts[tri.i1];
	PTVert v2 = gVerts[tri.i2];
	PTMaterial mat = gMats[tri.matId];

	float bw = 1.0 - hit.bu - hit.bv;
#ifdef CINEMA_RAYQUERY
	// M8-A: verts LOCALES -> mundo por la o2w de la instancia impactada (identidad
	// con BLAS unica -> transform no-op, bit-identico al camino aplanado).
	float3 p0 = XformPos(hit.o2w, PtVertPos(v0));
	float3 p1 = XformPos(hit.o2w, PtVertPos(v1));
	float3 p2 = XformPos(hit.o2w, PtVertPos(v2));
	float ngSign = DetSign3x4(hit.o2w);
#else
	float3 p0 = PtVertPos(v0);
	float3 p1 = PtVertPos(v1);
	float3 p2 = PtVertPos(v2);
#endif

	SurfaceM2 s;
	s.pos = p0 * bw + p1 * hit.bu + p2 * hit.bv;

	s.ng = cross(p1 - p0, p2 - p0);
#ifdef CINEMA_RAYQUERY
	// M8-A: compensa el winding de instancias espejadas (det<0). El flatten-mundo
	// de hoy corrige el espejo con el swap i1<->i2; aqui la BLAS local NO lleva
	// swap, asi que el signo del det restaura la misma normal geometrica.
	s.ng *= ngSign;
#endif
	float ngLen = length(s.ng);
	s.ng = (ngLen > 1.0e-20) ? s.ng / ngLen : float3(0.0, 0.0, 1.0);

	// Cara frontal ANTES del volteo (decide entrar/salir del agua)
	s.frontFace = (dot(s.ng, rayDir) <= 0.0) ? 1.0 : 0.0;

	float3 nsRaw = DecodeNormal888(v0.normal888) * bw
	             + DecodeNormal888(v1.normal888) * hit.bu
	             + DecodeNormal888(v2.normal888) * hit.bv;
#ifdef CINEMA_RAYQUERY
	// M8-A: normal de shading a mundo por el COFACTOR de la o2w, la MISMA
	// operacion que hace ahora FlattenDraw para la copia MUNDO (que es lo que
	// leen el oraculo y el camino no instanciado). Antes ambos lados usaban la
	// 3x3 directa pero decodificaban el 888 con escalas DISTINTAS (host 2/255,
	// shader 1/127): con escala anisotropa esa diferencia sub-cuanto se
	// amplificaba por la razon de escalas y el mismo suelo salia un 9% mas
	// oscuro en GPU que en el oraculo. El re-encode 888 del host se sigue
	// omitiendo aqui -> diferencia sub-cuanto.
	nsRaw = XformNormal(hit.o2w, nsRaw);
#endif
	float nsLen = length(nsRaw);
	s.ns = (nsLen > 1.0e-6) ? nsRaw / nsLen : s.ng;

	// Voltear ambas normales hacia el rayo incidente
	if (dot(s.ng, rayDir) > 0.0)
		s.ng = -s.ng;
	if (dot(s.ns, rayDir) > 0.0)
		s.ns = -s.ns;

	float2 rawUv = PtVertUv(v0) * bw + PtVertUv(v1) * hit.bu + PtVertUv(v2) * hit.bv;
	float4 texel = SampleAlbedo(mat, rawUv);

	// Normal map (M2)
	if ((mat.flags & MATF_HAS_NRMTEX) != 0u)
	{
		float4 nrmTexel = SampleDataTexel(mat.flags, mat.uvRectNrm, mat.sliceNrm, rawUv);
		if (nrmTexel.a >= 0.01)
		{
			float3 tRaw = DecodeNormal888(v0.tangent888) * bw
			            + DecodeNormal888(v1.tangent888) * hit.bu
			            + DecodeNormal888(v2.tangent888) * hit.bv;
#ifdef CINEMA_RAYQUERY
			tRaw = XformDir(hit.o2w, tRaw); // M8-A: tangente LOCAL -> mundo (3x3 directa)
#endif
			float3 tOrt = tRaw - dot(tRaw, s.ns) * s.ns;
			float tLen = length(tOrt);
			if (tLen > 1.0e-6)
			{
				tOrt /= tLen;
				float3 bOrt = cross(tOrt, s.ns);
				float2 nxy = nrmTexel.rg * 2.0 - 1.0;
				float nz = sqrt(max(0.0, 1.0 - dot(nxy, nxy)));
				float3 nPer = normalize(tOrt * nxy.x + bOrt * (-nxy.y) + s.ns * nz);
				if (dot(nPer, s.ng) > 0.0)
					s.ns = nPer;
			}
		}
	}

	float3 vertRgb = DecodeColorAbgr(v0.colorABGR).rgb * bw
	               + DecodeColorAbgr(v1.colorABGR).rgb * hit.bu
	               + DecodeColorAbgr(v2.colorABGR).rgb * hit.bv;

	s.albedo = texel.rgb * mat.blend.rgb * vertRgb;

	float eVert = DecodeVertEmissive(v0.dataFlags) * bw
	            + DecodeVertEmissive(v1.dataFlags) * hit.bu
	            + DecodeVertEmissive(v2.dataFlags) * hit.bv;
	s.emissive = max(mat.emissive, eVert);
#ifdef CINEMA_TRIM
	// TRIM: emisividad POR TEXEL. Es lo que ve la camara, los reflejos y la GI
	// -- no un uEmissive plano por pieza.
	s.trimRgb = ((mat.flags & MATF_HAS_TRIM) != 0u)
		? TrimRadiance(mat, rawUv, texel.rgb, vertRgb, s.pos)
		: float3(0.0, 0.0, 0.0);
#endif

	// --- clasificacion de kind (WATER > ALPHA_BLEND > hoja > estandar) ---
	uint leafBits = (v0.dataFlags | v1.dataFlags | v2.dataFlags) & 4u;
	s.glassTint = float3(0.0, 0.0, 0.0);
	s.glassAlpha = 0.0;
	s.waterSigma = float3(0.0, 0.0, 0.0);
	s.diffColor = s.albedo;
	s.f0 = float3(PT_F0_DIELECTRIC, PT_F0_DIELECTRIC, PT_F0_DIELECTRIC);
	s.alphaG = 0.0;

	if ((mat.flags & MATF_WATER) != 0u)
	{
		s.kind = PT_KIND_WATER;
		// Beer: a PT_WATER_ABS_DIST unidades (1 bloque) transmite blend.rgb
		float3 bl = clamp(mat.blend.rgb, PT_SIGMA_FLOOR, 1.0);
		s.waterSigma = -log(bl) / PT_WATER_ABS_DIST;
	}
	else if ((mat.flags & (MATF_ALPHA_BLEND | MATF_COVERAGE)) == (MATF_ALPHA_BLEND | MATF_COVERAGE)
		&& saturate(texel.a * mat.blend.a) < 0.999)
	{
		// COBERTURA: el lobulo difuso es el ESTANDAR completo (normal map,
		// material map, metallic/roughness); glassAlpha guarda la fraccion
		// cubierta aEff, que SampleBsdf/EvalBsdfCosPdf usan como peso de
		// mezcla contra el lobulo pasa-de-largo. Los texels con aEff >= 0.999
		// caen al kind estandar por el guard de arriba (ya son opacos) y los
		// de aEff ~ 0 los descarta la traversal (texel fantasma).
		s.kind = PT_KIND_COVER;
		s.glassAlpha = saturate(texel.a * mat.blend.a);
		float metallicC = mat.metallic;
		float roughnessC = mat.roughness;
		if ((mat.flags & MATF_HAS_MATTEX) != 0u)
		{
			float4 mtC = SampleDataTexel(mat.flags, mat.uvRectMat, mat.sliceMat, rawUv);
			if ((mat.flags & MATF_MAT_LABPBR) != 0u)
			{
				metallicC = (mtC.g > 0.898) ? 1.0 : 0.0;
				float smC = 1.0 - mtC.r;
				roughnessC = smC * smC;
			}
			else
			{
				roughnessC = 1.0 - mtC.r;
				metallicC = mtC.g;
			}
		}
		metallicC = saturate(metallicC);
		roughnessC = clamp(roughnessC, PT_ROUGH_MIN, 1.0);
		s.diffColor = s.albedo * (1.0 - metallicC);
		s.f0 = lerp(float3(PT_F0_DIELECTRIC, PT_F0_DIELECTRIC, PT_F0_DIELECTRIC), s.albedo, metallicC);
		s.alphaG = roughnessC * roughnessC;
	}
	else if ((mat.flags & MATF_ALPHA_BLEND) != 0u
		&& saturate(texel.a * mat.blend.a) < 0.999)
	{
		s.kind = PT_KIND_GLASS;
		// MISMO factor por capa que TraceShadow: sombra coloreada consistente.
		// Texels de un material DEPTH2 con aEff ~ 1 (marcos opacos del panel)
		// NO son cristal: caen al kind estandar difuso (abajo), igual que el
		// raster los pinta opacos y que TraceShadow los ocluye.
		float aEff = saturate(texel.a * mat.blend.a);
		s.glassTint = texel.rgb * mat.blend.rgb * (1.0 - aEff);
		s.glassAlpha = aEff;
	}
	else if ((mat.flags & MATF_SUBSURFACE) != 0u || leafBits != 0u)
	{
		s.kind = PT_KIND_LEAF;
	}
	else
	{
		s.kind = PT_KIND_STD;
		float metallic = mat.metallic;
		float roughness = mat.roughness;
		if ((mat.flags & MATF_HAS_MATTEX) != 0u)
		{
			float4 mt = SampleDataTexel(mat.flags, mat.uvRectMat, mat.sliceMat, rawUv);
			if ((mat.flags & MATF_MAT_LABPBR) != 0u)
			{
				metallic = (mt.g > 0.898) ? 1.0 : 0.0;
				float sm = 1.0 - mt.r;
				roughness = sm * sm;
			}
			else
			{
				roughness = 1.0 - mt.r;
				metallic = mt.g;
			}
		}
		metallic = saturate(metallic);
		roughness = clamp(roughness, PT_ROUGH_MIN, 1.0);
		s.diffColor = s.albedo * (1.0 - metallic);
		s.f0 = lerp(float3(PT_F0_DIELECTRIC, PT_F0_DIELECTRIC, PT_F0_DIELECTRIC), s.albedo, metallic);
		s.alphaG = roughness * roughness;
	}

	return s;
}

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// M2: evaluacion del BSDF NO-delta (kinds 0/1) para una direccion wi dada.
// Devuelve fcos = f * |cos efectivo| y la pdf de MEZCLA con la que
// SampleBsdf habria muestreado wi. NEE y muestreo de BSDF comparten esta
// funcion: el peso MIS balance es exacto por construccion. kinds 2/3 son
// delta y JAMAS llegan aqui (su NEE esta apagada).
// ---------------------------------------------------------------------------
void EvalBsdfCosPdf(SurfaceM2 s, float3 wo, float3 wi, out float3 fcos, out float pdf)
{
	fcos = float3(0.0, 0.0, 0.0);
	pdf = 0.0;

	if (s.kind == PT_KIND_LEAF)
	{
		// Thin-translucent: dos lobulos coseno excluyentes por hemisferio.
		// La transmision (cos negativo contra ns) es el glow a contraluz.
		float cosR = dot(s.ns, wi);
		if (cosR > 0.0)
		{
			fcos = s.albedo * ((1.0 - PT_LEAF_TRANSMIT) * cosR / PT_PI);
			pdf = (1.0 - PT_LEAF_TRANSMIT) * cosR / PT_PI;
		}
		else if (cosR < 0.0)
		{
			fcos = s.albedo * (PT_LEAF_TRANSMIT * (-cosR) / PT_PI);
			pdf = PT_LEAF_TRANSMIT * (-cosR) / PT_PI;
		}
		return;
	}

	// kind 0: Lambert difuso + GGX, ambos sobre el hemisferio de ns
	float cosO = dot(s.ns, wo);
	float cosI = dot(s.ns, wi);
	if (cosO <= 1.0e-6 || cosI <= 1.0e-6)
		return;

	// Pesos de lobo: MISMOS que SampleBsdf (mezcla por peso de Fresnel)
	float wSpec = LumRec709(SchlickF3(s.f0, cosO));
	float wDiff = LumRec709(s.diffColor);
	float wSum = wSpec + wDiff;
	if (wSum <= 0.0)
		return;
	float pSpec = wSpec / wSum;

	// Lobulo difuso
	fcos = s.diffColor * (cosI / PT_PI);
	pdf = (1.0 - pSpec) * (cosI / PT_PI);

	// Lobulo GGX single-scatter (f*cos = F * D * G2 / (4 * cosO))
	float3 h = wo + wi;
	float hLen = length(h);
	if (hLen > 1.0e-8)
	{
		h /= hLen;
		float cosNH = dot(s.ns, h);
		float cosOH = dot(wo, h);
		if (cosNH > 0.0 && cosOH > 0.0)
		{
			float d = GgxD(cosNH, s.alphaG);
			float lo = GgxLambda(cosO, s.alphaG);
			float li = GgxLambda(cosI, s.alphaG);
			float g2 = 1.0 / (1.0 + lo + li);
			float g1 = 1.0 / (1.0 + lo);
			float3 fr = SchlickF3(s.f0, cosOH);
			fcos += fr * (d * g2 / max(4.0 * cosO, 1.0e-8));
			pdf += pSpec * (g1 * d / max(4.0 * cosO, 1.0e-8));
		}
	}

	// COBERTURA: el BSDF es aEff * difuso_estandar + (1-aEff) * pasa-de-largo.
	// El segundo termino es DELTA (no evaluable aqui), asi que la parte
	// no-delta se escala por aEff -- en fcos (solo la fraccion cubierta
	// refleja) y en pdf (SampleBsdf elige este lobulo con probabilidad aEff).
	// Escalar AMBOS deja el peso fcos/pdf intacto y hace exacto el MIS.
	if (s.kind == PT_KIND_COVER)
	{
		fcos *= s.glassAlpha;
		pdf *= s.glassAlpha;
	}
}

// Origen del shadow ray de NEE: para hojas la luz puede estar en el
// hemisferio opuesto (transmision) y el origen salta al lado de la luz.
// El resto conserva el offset +ng de M1 (autosombra natural).
float3 NeeShadowOrg(SurfaceM2 s, float offs, float3 wi)
{
	float sgn = (s.kind == PT_KIND_LEAF && dot(wi, s.ng) < 0.0) ? -1.0 : 1.0;
	return s.pos + s.ng * (offs * sgn);
}

// ---------------------------------------------------------------------------
// M2: muestreo del BSDF. rHemi = .zw de la dim k=0 (hemisferio del lobulo
// difuso); rLobe = dim k=3 (.x seleccion de lobo / Fresnel, .yz VNDF).
// Devuelve la direccion nueva, el peso multiplicativo del camino
// (f*cos/pdf de mezcla en kinds 0/1; 1 o el tinte en los delta), la pdf de
// mezcla (kinds 0/1; 0 en delta), isDelta y refracted (cruzo la superficie:
// gestiona el estado de agua y el lado del origen). false = camino muerto.
// ---------------------------------------------------------------------------
bool SampleBsdf(SurfaceM2 s, float3 wo, float2 rHemi, float4 rLobe,
                out float3 wi, out float3 weight, out float pdfOut,
                out bool isDelta, out bool refracted)
{
	wi = float3(0.0, 0.0, 1.0);
	weight = float3(0.0, 0.0, 0.0);
	pdfOut = 0.0;
	isDelta = false;
	refracted = false;

	if (s.kind == PT_KIND_WATER)
	{
		isDelta = true;
		// Micro-normal VNDF (roughness PT_WATER_ROUGH): reflejos/refracciones
		// levemente rugosos sin dejar de tratar el lobo como delta (aprox de
		// normal perturbada, robusta en cs_5_0; documentada como sesgo leve)
		float3 t, b;
		BuildOnb(s.ns, t, b);
		float aG = PT_WATER_ROUGH * PT_WATER_ROUGH;
		float3 woL = float3(dot(wo, t), dot(wo, b), dot(wo, s.ns));
		float3 m = s.ns;
		if (woL.z > 1.0e-6)
		{
			float3 hL = SampleGgxVndf(woL, aG, rLobe.yz);
			m = t * hL.x + b * hL.y + s.ns * hL.z;
		}
		float cosI = dot(wo, m);
		if (cosI <= 1.0e-6)
		{
			m = s.ns;
			cosI = max(dot(wo, m), 1.0e-6);
		}
		// entrar (frontFace) aire->agua; salir agua->aire. etaRel = ni/nt.
		float etaRel = (s.frontFace > 0.5) ? (1.0 / PT_WATER_IOR) : PT_WATER_IOR;
		float sin2T = etaRel * etaRel * max(0.0, 1.0 - cosI * cosI);
		float fres = 1.0; // TIR
		float cosT = 0.0;
		if (sin2T < 1.0)
		{
			cosT = sqrt(1.0 - sin2T);
			fres = FresnelDielectric(cosI, cosT, etaRel);
		}
		if (rLobe.x < fres)
		{
			// Reflexion delta sobre la micro-normal
			wi = normalize(2.0 * cosI * m - wo);
			if (dot(wi, s.ng) <= 0.0)
				wi = normalize(2.0 * dot(wo, s.ns) * s.ns - wo); // fallback liso
			if (dot(wi, s.ng) <= 0.0)
				return false;
		}
		else
		{
			// Refraccion delta sobre la micro-normal
			wi = normalize((etaRel * cosI - cosT) * m - etaRel * wo);
			if (dot(wi, s.ng) >= 0.0)
			{
				// Micro-normal invalida para refractar: repetir con la lisa
				float cosI2 = max(dot(wo, s.ns), 1.0e-6);
				float sin2T2 = etaRel * etaRel * max(0.0, 1.0 - cosI2 * cosI2);
				if (sin2T2 >= 1.0)
					return false;
				float cosT2 = sqrt(1.0 - sin2T2);
				wi = normalize((etaRel * cosI2 - cosT2) * s.ns - etaRel * wo);
				if (dot(wi, s.ng) >= 0.0)
					return false;
			}
			refracted = true;
		}
		weight = float3(1.0, 1.0, 1.0);
		return true;
	}

	if (s.kind == PT_KIND_GLASS)
	{
		isDelta = true;
		// Thin-surface: Schlick escalar F0 0.04 con la normal de shading
		float cosI = max(dot(s.ns, wo), 0.0);
		float p = saturate(1.0 - cosI);
		float p2 = p * p;
		float fres = PT_GLASS_F0 + (1.0 - PT_GLASS_F0) * (p2 * p2 * p);
		if (rLobe.x < fres)
		{
			wi = normalize(2.0 * dot(wo, s.ns) * s.ns - wo);
			if (dot(wi, s.ng) <= 0.0)
				return false;
			weight = float3(1.0, 1.0, 1.0);
		}
		else
		{
			// Transmision RECTA (sin refraccion, stained glass del diseno)
			wi = -wo;
			weight = s.glassTint;
			if (weight.r + weight.g + weight.b <= 0.0)
				return false;
			refracted = true;
		}
		return true;
	}

	// COBERTURA: eleccion estocastica de lobulo con la MISMA dimension de RNG
	// que usan los demas kinds (rLobe.x), sin consumir una nueva.
	//   rLobe.x >= aEff -> PASA DE LARGO: sigue recto, peso 1, sin tinte. Es
	//     literalmente "este texel no cubre aqui": ni Fresnel ni absorcion.
	//   rLobe.x <  aEff -> lobulo DIFUSO estandar; rLobe.x se REESCALA a [0,1)
	//     para que la seleccion difuso/GGX de abajo siga siendo uniforme y
	//     decorrelada (misma secuencia RNG en GPU y en el oraculo).
	if (s.kind == PT_KIND_COVER)
	{
		if (rLobe.x >= s.glassAlpha)
		{
			isDelta = true;
			wi = -wo;
			weight = float3(1.0, 1.0, 1.0);
			refracted = true; // cruza la superficie: el origen salta al otro lado
			return true;
		}
		rLobe.x = rLobe.x / max(s.glassAlpha, 1.0e-6);
	}

	// kinds 0/1: mezcla muestreable, peso = fcos/pdf de EvalBsdfCosPdf
	if (s.kind == PT_KIND_LEAF)
	{
		if (rLobe.x < PT_LEAF_TRANSMIT)
			wi = CosineSampleHemisphere(rHemi, -s.ns);
		else
			wi = CosineSampleHemisphere(rHemi, s.ns);
	}
	else
	{
		float cosO = dot(s.ns, wo);
		if (cosO <= 1.0e-6)
			return false;
		float wSpec = LumRec709(SchlickF3(s.f0, cosO));
		float wDiff = LumRec709(s.diffColor);
		float wSum = wSpec + wDiff;
		if (wSum <= 0.0)
			return false;
		float pSpec = wSpec / wSum;
		if (rLobe.x < pSpec)
		{
			float3 t, b;
			BuildOnb(s.ns, t, b);
			float3 woL = float3(dot(wo, t), dot(wo, b), cosO);
			float3 hL = SampleGgxVndf(woL, s.alphaG, rLobe.yz);
			float3 h = t * hL.x + b * hL.y + s.ns * hL.z;
			wi = normalize(2.0 * dot(wo, h) * h - wo);
		}
		else
			wi = CosineSampleHemisphere(rHemi, s.ns);
	}

	float3 fcos;
	float pdf;
	EvalBsdfCosPdf(s, wo, wi, fcos, pdf);
	if (pdf <= 1.0e-12 || fcos.r + fcos.g + fcos.b <= 0.0)
		return false;
	weight = fcos / pdf;
	pdfOut = pdf;
	return true;
}

)HLSL" R"HLSL(// ---------------------------------------------------------------------------
// NEE de "otra fuente" (seccion 6 del diseno): 1 fuente elegida por potencia
// entre el grupo de tris emisores (CDF t6/t7) y el de luces analiticas (t5).
// M2: devuelve la contribucion del BSDF COMPLETO de la superficie (radiancia
// * fcos * sombra / pdfs de seleccion, con peso MIS donde aplica) SIN
// throughput ni clamp: los aplica el caller. Solo kinds 0/1 (los delta no
// reciben NEE).
//   r.x = seleccion de grupo y, remapeada, de fuente; r.yz = punto por area
//   en el tri emisor o cono hacia la esfera de la luz (dims RNG del contrato)
// Emisores: pdf area->angulo solido y MIS balance contra la pdf de MEZCLA
// del BSDF (EvalBsdfCosPdf: la misma que usa SampleBsdf).
// Autoexclusion selfTri: un emisor no recibe NEE sobre si mismo (seccion 7).
// Luces analiticas: radiancia col * falloff(dist al CENTRO) * spotMask
// replicando el forward; size 0 = delta (NEE puro); size > 0 SOLO suaviza la
// sombra (cono hacia la esfera con energia intacta: LIGHT_SIZE es el knob de
// penumbra del raster, no un area fisica). Sin peso MIS: las luces
// analiticas no estan en el BVH y los rayos de BSDF no pueden alcanzarlas
// (descontar contra una estrategia imposible perderia energia).
// ---------------------------------------------------------------------------
// Los pesos de grupo (wEmi = ultimo elemento de la CDF CRUDA, wLights =
// suma de LightPowerWeight, pEmi = wEmiEff/(wEmiEff+wLights) con wEmiEff =
// wEmi * emissiveScale * pi) los calcula el integrador UNA vez por camino y
// los comparte con el peso MIS de la emision por hit (misma particion
// exacta en ambos lados). wEmi crudo manda en el dominio de la CDF.
// F1 4a: oUnshadowed devuelve el estimador GEMELO -- exactamente el mismo
// termino con trans = 1 (misma fuente elegida, mismo punto, misma pdf, mismo
// peso MIS). No cuesta un rayo: es el valor que ya se calcula justo antes de
// multiplicar por la transmitancia. Fuera de la rama del catcher nadie lo lee y
// el compilador lo elimina, asi que el camino de siempre no cambia.
// F5 (C1): el DOMO entra como un TERCER grupo de esta misma NEE. Ocupa la COLA
// del intervalo de r.x, [1-pEnv, 1), a proposito: con pEnv = 0 (flag apagado)
// el reparto de r.x, el remapeo del grupo de luces y su pSel quedan
// EXACTAMENTE como estaban (1.0 - pEmi - 0.0 es 1.0 - pEmi bit a bit), asi que
// el camino de siempre no mueve un bit. NO consume ninguna dim de RNG nueva:
// reutiliza r.yz, que en este grupo son el muestreo coseno del hemisferio.
float3 SampleOtherLight(SurfaceM2 s, float3 wo, float offs,
                        uint selfTri, float4 r, float pEmi, float wEmi, float wLights,
                        float pEnv, bool envMisOn, out float3 oUnshadowed)
{
	oUnshadowed = float3(0.0, 0.0, 0.0);

	// ----- grupo ENTORNO (F5): el domo analitico como fuente muestreable -----
	// Va ANTES del guard historico: en una escena SOLO-DOMO no hay emisivos ni
	// luces (pEmi = 0, wLights = 0) y ese guard mataria justo el caso que F5
	// existe para arreglar (imagen negra a bounces = 1).
	if (pEnv > 0.0 && r.x >= 1.0 - pEnv)
	{
		// Muestreo COSENO del hemisferio de la normal de shading: pdf de angulo
		// solido = cos/pi, la misma base ortonormal (Duff) que ya usa el BSDF.
		// El hemisferio INFERIOR (transmision de hoja) no lo cubre esta
		// estrategia: alli pdfNee = 0 y el MIS le da peso 1 al lado BSDF, que es
		// lo correcto (la particion sigue sumando 1 en toda direccion).
		float3 wiE = CosineSampleHemisphere(float2(r.y, r.z), s.ns);
		float cosN = dot(wiE, s.ns);
		if (cosN <= 1.0e-6)
			return float3(0.0, 0.0, 0.0);
		float3 fcosE;
		float pdfBsdfE;
		EvalBsdfCosPdf(s, wo, wiE, fcosE, pdfBsdfE);
		if (fcosE.r + fcosE.g + fcosE.b <= 0.0)
			return float3(0.0, 0.0, 0.0);
		float pdfNeeE = pEnv * cosN / PT_PI;
		if (pdfNeeE <= 1.0e-20)
			return float3(0.0, 0.0, 0.0);
		// MIS balance contra el BSDF... PERO SOLO SI EL RAYO DE BSDF SE VA A
		// TRAZAR. En el ULTIMO rebote el bucle termina justo despues de muestrear
		// el lobo: ese rayo no llega a existir, asi que la mitad BSDF de la
		// particion nunca se cobra. Con envMisOn = false la NEE es la UNICA
		// estrategia que puede entregar el domo y le toca el peso 1 entero.
		// Sin esto, un render de bounces = 1 bajo domo puro sale a la MITAD de
		// su radiancia (pEnv = 1 y pdfBsdf ~ pdfNee -> misW = 0.5) -- deja de
		// estar negro, si, pero por el numero equivocado.
		float misWE = envMisOn ? (pdfNeeE / (pdfNeeE + pdfBsdfE)) : 1.0;
		float3 leE = SkyRadiance(wiE);
		if (leE.r + leE.g + leE.b <= 0.0)
			return float3(0.0, 0.0, 0.0);
		float3 baseV = leE * fcosE * (misWE / pdfNeeE);
		float3 transE = TraceShadow(NeeShadowOrg(s, offs, wiE), wiE, 0.0, PT_T_MAX);
		// El domo NO tiene geometria en el BVH: como el sol y las luces
		// analiticas, NUNCA puede taparse a si mismo. Lo que corte el rayo es un
		// ocluyente de verdad -> el gemelo del catcher es el termino COMPLETO.
		// Esto es, literalmente, la sombra de contacto bajo un domo.
		oUnshadowed = baseV;
		if (transE.r + transE.g + transE.b <= 0.0)
			return float3(0.0, 0.0, 0.0);
		return transE * leE * fcosE * (misWE / pdfNeeE);
	}

	if (pEmi <= 0.0 && wLights <= 0.0)
		return float3(0.0, 0.0, 0.0);

	if (r.x < pEmi)
	{
		// ----- grupo emisivos: busqueda binaria en la CDF por potencia -----
		float rIn = min(r.x / max(pEmi, 1.0e-20), 0.99999994);
		uint slot = SampleEmiCdf(rIn * wEmi);
		uint triIdx = gEmiTriIdx[slot];
		if (triIdx == selfTri)
			return float3(0.0, 0.0, 0.0);
		PTTri tri = gTris[triIdx];

		// Punto uniforme por area (mapeo sqrt estandar sobre r.yz)
		float su = sqrt(max(r.y, 0.0));
		float bu = 1.0 - su;
		float bv = r.z * su;
		float3 lp, lng;
		float area;
#ifdef CINEMA_RAYQUERY
		// M8-A: el emisor puede estar en LOCAL -> su o2w viaja por gEmiO2W[slot]
		// (identidad si el host ya lo dejo en mundo).
		float3 le = EmissiveRadianceAtInst(tri, bu, bv, InstXformMat(gEmiO2W[slot]), lp, lng, area);
#else
		float3 le = EmissiveRadianceAt(tri, bu, bv, lp, lng, area);
#endif
		if (le.r + le.g + le.b <= 0.0 || area <= 1.0e-12)
			return float3(0.0, 0.0, 0.0);

		float3 toL = lp - s.pos;
		float dist2 = dot(toL, toL);
		float dist = sqrt(dist2);
		if (dist <= 1.0e-4)
			return float3(0.0, 0.0, 0.0);
		float3 wi = toL / dist;
		float cosL = abs(dot(lng, wi)); // emision de dos caras
		float3 fcos;
		float pdfBsdf;
		EvalBsdfCosPdf(s, wo, wi, fcos, pdfBsdf);
		if (fcos.r + fcos.g + fcos.b <= 0.0 || cosL <= 1.0e-6)
			return float3(0.0, 0.0, 0.0);

		// pdf TOTAL de la estrategia NEE en angulo solido:
		//   pSel(grupo) * pSel(tri|grupo) * dist^2 / (area * cosL)
		float pdfNee = pEmi * EmiSlotProb(slot, wEmi) * dist2 / max(area * cosL, 1.0e-12);
		if (pdfNee <= 1.0e-20)
			return float3(0.0, 0.0, 0.0);
		float misW = pdfNee / (pdfNee + pdfBsdf);

		float3 baseE = le * fcos * (misW / pdfNee);
		uint blkMatE;
		float3 trans = TraceShadow(NeeShadowOrg(s, offs, wi), wi, 0.0, dist * 0.9999, blkMatE);
		// F1 4a + RONDA 5 (fix del acantilado): es auto-sombra -- y por tanto NO
		// sombra -- SOLO si el que corta el rayo es el MISMO objeto emisivo que
		// estoy muestreando (mismo matId = mismo draw). Ese es el caso 2.1: la
		// NEE emite por las dos caras, la mitad de los samples de una placa
		// solida caen en su cara de atras y el rayo choca contra la cara de
		// delante de esa MISMA placa. Un emisivo AJENO (una antorcha, un trim
		// brillante, el propio sujeto si emite) SI proyecta sombra: antes
		// bastaba con EMISSIVE 0,001 en cualquier ocluyente para igualar
		// numerador y denominador y borrar la sombra entera del cuadro.
		oUnshadowed = (blkMatE == tri.matId) ? (trans * baseE) : baseE;
		if (trans.r + trans.g + trans.b <= 0.0)
			return float3(0.0, 0.0, 0.0);
		// BIT-IDENTIDAD: lo que se DEVUELVE conserva la expresion (y por tanto la
		// asociatividad) EXACTA de siempre. baseE es solo del gemelo.
		return trans * le * fcos * (misW / pdfNee);
	}

	// ----- grupo luces analiticas: seleccion lineal por potencia -----
	if (counts.x == 0u || wLights <= 0.0)
		return float3(0.0, 0.0, 0.0);
	// F5: pLit es la probabilidad del grupo de luces. Con pEnv = 0 vale
	// 1.0 - pEmi - 0.0, que es 1.0 - pEmi BIT A BIT (restar +0.0 es exacto):
	// el remapeo y el pSel de abajo son los de siempre, carater por caracter.
	float pLit = 1.0 - pEmi - pEnv;
	float rInL = min((r.x - pEmi) / max(pLit, 1.0e-20), 0.99999994);
	float target = rInL * wLights;
	float run = 0.0;
	uint pick = counts.x - 1u;
	[loop]
	for (uint lj = 0u; lj < counts.x; lj++)
	{
		run += LightPowerWeight(gLights[lj]);
		if (target < run)
		{
			pick = lj;
			break;
		}
	}
	PTLight light = gLights[pick];
	float pSel = pLit * LightPowerWeight(light) / wLights;
	if (light.type == 2u)
	{
		float side = max(light.sizeRadius, 0.0);
		if (side <= 1.0e-6)
			return float3(0.0, 0.0, 0.0);
		float3 ln = normalize(light.dir);
		float3 helper = abs(ln.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
		float3 tangent = normalize(cross(helper, ln));
		float3 bitangent = cross(ln, tangent);
		float halfSide = 0.5 * side;
		float3 lp = light.pos
			+ tangent * ((2.0 * r.y - 1.0) * halfSide)
			+ bitangent * ((2.0 * r.z - 1.0) * halfSide);
		float3 toL = lp - s.pos;
		float dist2 = dot(toL, toL);
		if (dist2 <= 1.0e-8)
			return float3(0.0, 0.0, 0.0);
		float dist = sqrt(dist2);
		float3 wi = toL / dist;
		float cosL = max(dot(ln, -wi), 0.0);
		float att = LightFalloff(dist, light.range, light.fadeSize);
		if (cosL <= 0.0 || att <= 0.0)
			return float3(0.0, 0.0, 0.0);
		float3 fcos;
		float pdfBsdfL;
		EvalBsdfCosPdf(s, wo, wi, fcos, pdfBsdfL);
		if (fcos.r + fcos.g + fcos.b <= 0.0)
			return float3(0.0, 0.0, 0.0);
		float geometry = cosL * (side * side) / max(dist2, 1.0e-8);
		float3 baseL = light.col * fcos * (att * geometry) / max(pSel, 1.0e-20);
		float3 trans = TraceShadow(NeeShadowOrg(s, offs, wi), wi, 0.0, max(dist - PT_T_EPS, PT_T_EPS));
		oUnshadowed = baseL;
		if (trans.r + trans.g + trans.b <= 0.0)
			return float3(0.0, 0.0, 0.0);
		return trans * light.col * fcos * (att * geometry) / max(pSel, 1.0e-20);
	}

	float3 toC = light.pos - s.pos;
	float dist = length(toC);
	if (dist <= 1.0e-4)
		return float3(0.0, 0.0, 0.0);
	float3 wiC = toC / dist; // hacia el CENTRO de la luz

	// Falloff y cono del forward, medidos al centro (como el raster)
	float att = LightFalloff(dist, light.range, light.fadeSize);
	float mask = SpotMask(dot(light.dir, -wiC), light.cosOuter, light.sharpness);
	if (att <= 0.0 || mask <= 0.0)
		return float3(0.0, 0.0, 0.0);

	// Direccion de sombra: delta = al centro; size > 0 = cono hacia la
	// esfera (solo penumbra; la energia no cambia con el tamano)
	float3 wi = wiC;
	if (light.sizeRadius > 0.0 && dist > light.sizeRadius)
	{
		float sinMax = saturate(light.sizeRadius / dist);
		float cosMax = sqrt(max(0.0, 1.0 - sinMax * sinMax));
		wi = SampleSunCone(float2(r.y, r.z), wiC, cosMax);
	}
	float3 fcos;
	float pdfBsdfL;
	EvalBsdfCosPdf(s, wo, wi, fcos, pdfBsdfL); // pdf sin uso: NEE pura
	if (fcos.r + fcos.g + fcos.b <= 0.0)
		return float3(0.0, 0.0, 0.0);

	float tMaxSh = max(dist - light.sizeRadius, PT_T_EPS);
	float3 baseL = light.col * fcos * (att * mask) / max(pSel, 1.0e-20);
	float3 trans = TraceShadow(NeeShadowOrg(s, offs, wi), wi, 0.0, tMaxSh);
	// F1 4a + RONDA 5 (fix del acantilado): una luz ANALITICA no tiene geometria
	// en el BVH, asi que ningun triangulo puede ser "su propio cuerpo" y NUNCA
	// hay auto-sombra aqui: lo que corte el rayo es un ocluyente de verdad,
	// emisivo o no. Antes, un ocluyente emisivo cualquiera anulaba la sombra que
	// una point/spot proyecta sobre el catcher.
	oUnshadowed = baseL;
	if (trans.r + trans.g + trans.b <= 0.0)
		return float3(0.0, 0.0, 0.0);

	// Forward: light = col * att * mask * dot(n, l), sin 1/pi ni 1/d^2.
	// Aqui col * att * mask se interpreta como irradiancia incidente E y la
	// respuesta del BSDF es fcos * E (M1 Lambert: albedo/pi * cos * E). El
	// puente de brillo display->radiometrico (analogo al x5 del sol en el
	// host) se pliega en col ANTES de subir la luz, si hace falta.
	// BIT-IDENTIDAD: expresion EXACTA de siempre (ver la rama de emisivos).
	return trans * light.col * fcos * (att * mask) / max(pSel, 1.0e-20);
}

// ---------------------------------------------------------------------------
// CSPathTrace: 1 thread = 1 pixel del tile, 1 dispatch = 1 tile x 1 sample.
// Integrador M2 (secciones 6 y 7 del diseno): BSDF por kind (Lambert+GGX /
// hoja / agua / cristal) + NEE con splitting del sol (siempre 1 rayo al
// disco + 1 rayo a otra fuente por potencia, SOLO en kinds no-delta) + MIS
// balance NEE<->BSDF contra la pdf de mezcla + ruleta rusa desde el bounce 3
// + sombras coloreadas + Beer-Lambert dentro del agua + clamp de indirecta.
// Tras un rebote DELTA (agua/cristal) prevDelta = true: emision y disco
// solar del siguiente vertice/miss entran con peso 1 (ninguna NEE compitio).
// Mapeo de dims RNG: contrato junto a Rand4 (el oraculo lo replica bit a
// bit).
)HLSL" R"HLSL(// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSPathTrace(uint3 dtid : SV_DispatchThreadID)
{
	uint width = (uint)res_clamp.x;
	uint height = (uint)res_clamp.y;
	uint2 pixelAbs = uint2(tile.x + dtid.x, tile.y + dtid.y);
	if (pixelAbs.x >= width || pixelAbs.y >= height)
		return;
	uint pix = pixelAbs.y * width + pixelAbs.x;

	// --- M7 adaptive: umbral de ruido en dofParams.w (0 = OFF = camino
	// bit-identico: no hay early-out y el resolve usa el spp global). Si el
	// pixel ya convergio (marca en gVarCount.w), saltar el sample ENTERO: ni
	// traza ni acumula ni toca gVarCount (queda congelado). El resolve del host
	// divide por el CONTEO POR PIXEL (gVarCount.z), asi que menos samples no lo
	// oscurece. Divergencia intra-warp aceptable (los tiles GPU lo absorben). ---
	float adNoise = dofParams.w;
	if (adNoise > 0.0 && gVarCount[pix].w > 0.5)
		return;

	// --- rayo primario: invertir view_proj con jitter subpixel PCG ---
	// Convencion D3D: NDC x en [-1,1] izq->der, y en [-1,1] abajo->arriba
	// (flip respecto a filas de pixeles), z en [0,1]. El jitter en [0,1)
	// promedia al centro del pixel (+0.5): misma convencion de medio pixel
	// que el raster. La inversa lleva dentro el sub-frustum off-axis del
	// tiled export y el overscan: gratis por construccion (seccion 10).
	// Verificacion obligatoria M0: escena de calibracion de esquinas.
	float4 rj = Rand4(pixelAbs, 0u);
	float2 ndc;
	ndc.x = ((float(pixelAbs.x) + rj.x) / res_clamp.x) * 2.0 - 1.0;
	ndc.y = 1.0 - ((float(pixelAbs.y) + rj.y) / res_clamp.y) * 2.0;

	float4 pNear = mul(invViewProj, float4(ndc, 0.0, 1.0));
	float4 pFar = mul(invViewProj, float4(ndc, 1.0, 1.0));
	float3 rayDir = normalize(pFar.xyz / pFar.w - pNear.xyz / pNear.w);
	// R14 camara ORTOGRAFICA: la direccion ya sale bien de la inversa (la
	// resta de dos puntos NDC da la MISMA direccion en todos los pixeles =
	// rayos paralelos), pero el ORIGEN no puede ser camPos: bajo ortho cada
	// pixel arranca en SU punto del plano near. Un solo cambio de origen; el
	// resto del integrador consume el rayo ya generado y no se entera.
	// Con FRAMEF_ORTHO apagado el rayo queda BIT-IDENTICO al de siempre.
	float3 rayOrg = ((counts.w & FRAMEF_ORTHO) != 0u) ? (pNear.xyz / pNear.w) : camPos.xyz;

	// R9 (FIX 6): sonda de profundidad para el autofocus. Con FRAMEF_DEPTH_PROBE
	// el pase NO acumula: traza el PRIMER hit del rayo PINHOLE (sin DoF) y escribe
	// su distancia en gAovNormalDepth[pix].w (o -1 si es cielo). El host lee la
	// ventana central, saca la mediana y fija focusDist ANTES del render real
	// (y luego re-clarea). Gateado por un flag que la paridad/oraculo NUNCA
	// activan: no altera el camino de render ni requiere espejo en el oraculo.
	if ((counts.w & FRAMEF_DEPTH_PROBE) != 0u)
	{
		// R15: la sonda es un rayo de CAMARA -> ignora los holdouts; el foco cae
		// sobre lo que de verdad se ve (si no, el autofocus se clavaria en un
		// sujeto invisible y el fondo saldria desenfocado sin motivo).
		HitInfo ph = TraceClosest(rayOrg, rayDir, 0.0, true);
		float pd = (ph.tri != PT_INVALID) ? ph.t : -1.0;
		gAovNormalDepth[pix] = float4(0.0, 0.0, 0.0, pd);
		return;
	}

	// R6: DoF fisico de lente fina (efecto retrato). dofParams.z (dofOn) y
	// dofParams.y (radio de lente en mundo) gatean el efecto; con cualquiera de
	// los dos en 0 el rayo primario queda IDENTICO byte a byte al pinhole (rj.zw
	// ya esta calculado -> dim 0 tiene .zw libre, no se consumen dims extra ni
	// se desplaza la tabla del oraculo). Base ortonormal perpendicular al rayo
	// del pixel (la apertura es un disco/poligono simetrico respecto al eje del
	// rayo: su orientacion/roll de base no importa, lensParams.y solo rota la
	// FORMA de la pupila). A1: LensApertureSample sustituye LensDiskSample
	// (blades<3 es bit-identico a la formula anterior). A2: el punto focal se
	// calcula sobre el PLANO de foco perpendicular a camFwd (proyeccion sobre
	// el eje optico), no sobre una esfera centrada en la camara (bug de
	// fidelidad corregido; ver ft = dofParams.x / cos(angulo con el eje)).
	// lensBiasT (A5, apodizacion) se calcula aqui (necesita ld) pero se aplica
	// a throughput mas abajo, tras su declaracion.
	float lensBiasT = 1.0;
	if (dofParams.z > 0.5 && dofParams.y > 0.0)
	{
		float2 ld = LensApertureSample(rj.zw, lensParams.x, lensParams.y, lensParams.z) * dofParams.y;
		float3 up0 = (abs(rayDir.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
		float3 lensR = normalize(cross(up0, rayDir));
		float3 lensU = cross(rayDir, lensR);
		float cosAx = dot(rayDir, camFwd.xyz);
		float ft = dofParams.x / max(cosAx, 1.0e-4);   // distancia AL PLANO de foco
		float3 focal = rayOrg + rayDir * ft;
		rayOrg = rayOrg + lensR * ld.x + lensU * ld.y;
		rayDir = normalize(focal - rayOrg);
		// A5 (P3): apodizacion "edge highlight". lensParams.w = bias (0 = pupila
		// uniforme = camino de hoy). Ponderamos el throughput del rayo primario
		// por la transmision de la pupila, normalizada para no cambiar la
		// exposicion (E[T] = 1).
		if (lensParams.w > 0.0)
		{
			float rr = saturate(length(ld) / max(dofParams.y, 1.0e-6));  // 0 centro, 1 borde
			lensBiasT = 1.0 + lensParams.w * (rr - 0.5);                 // media exacta = 1
		}
	}

	float3 color = float3(0.0, 0.0, 0.0);
	float3 throughput = float3(1.0, 1.0, 1.0);
	if (lensParams.w > 0.0)
		throughput *= lensBiasT;   // E[T] = 1 -> exposicion intacta; con bias=0 no se ejecuta
	float alpha = 0.0;
	bool stillPrimary = true;

	// --- F1 4a: SHADOW CATCHER (estimador gemelo; ver el banner de
	// MATF_SHADOW_CATCHER). Con la escena sin catchers estos tres escalares se
	// quedan en su valor inicial, ninguna rama entra y el camino es
	// BIT-IDENTICO al de siempre.
	//   catcherPath : este camino nacio en un catcher. A partir de ahi TODO lo
	//                 que el camino acumule en `color` es el NUMERADOR (color
	//                 vale exactamente 0 cuando se marca: en el bounce 0 nada
	//                 ha sumado todavia), y al final `color` se tira.
	//   catcherSeg  : estamos en el PRIMER segmento de continuacion salido del
	//                 catcher. Es donde se decide el termino de continuacion
	//                 del denominador (ver abajo).
	//   catchDen    : el denominador -- "lo que llegaria si el sujeto no
	//                 estuviera". Se monta con las MISMAS direcciones que el
	//                 numerador, asi que los dos estan correlados y el cociente
	//                 es mucho mas limpio que sus dos mitades.
	bool   catcherPath = false;
	bool   catcherSeg = false;
	float3 catchDen = float3(0.0, 0.0, 0.0);
	float  catchOpacity = 1.0;
	float3 catchSkyBehind = float3(0.0, 0.0, 0.0); // solo para el preview OPACO
	float  catchSignal = 0.0;                      // senal del M7 adaptive (ver el final)

	uint maxBounces = clamp(counts.z, 1u, 8u);
	bool alphaFilm = (counts.w & FRAMEF_ALPHA_FILM) != 0u;
	// FONDO DE PELICULA. El fondo de la imagen deja de ser el CIELO y pasa a ser
	// un color del usuario que la CAMARA ve pero que NO ilumina. Aqui, en el
	// kernel, eso se reduce a una sola idea: lo que la camara ve DETRAS del
	// sujeto no pinta ni cubre -- exactamente el holdout que el film transparente
	// ya sabe hacer. El COLOR lo pone el resolve, componiendo el film
	// straight-alpha sobre el fondo en espacio DISPLAY (CinemaRenderFinish).
	//   POR QUE NO SE DEVUELVE EL COLOR AQUI: la radiancia de este kernel pasa
	//   despues por exposure -> tonemap -> saturacion -> encode sRGB. Un color
	//   "blanco 247" metido como radiancia NO sale en 247 (con el hombro del
	//   tonemap 3 y exposure 0,0834 hay que inyectar 17,79 para que salga), y
	//   peor: la sombra del catcher, que es un COCIENTE de luz, se multiplicaria
	//   contra ese valor ANTES del hombro. MEDIDO contra el hero publicado: la
	//   version "color en el miss" se desvia >2/255 en el 1,27 % de los pixeles
	//   (p99 = 5, max 64) solo por el espacio de composicion, y ~6/255 en toda
	//   la banda media de la rampa. Componiendo en display el fondo sale EXACTO
	//   y la sombra sale como la pone cualquier compositor.
	// NADA de esto toca la ILUMINACION: SkyRadiance sigue intacto para rebotes,
	// NEE, sombras y para el DENOMINADOR del shadow catcher.
	// El host solo enciende el bit con film OPACO (con alpha film ya es el camino
	// de siempre), asi que con el flag apagado bgHoldout == alphaFilm y todo lo
	// de abajo es BIT-IDENTICO.
	bool bgHoldout = alphaFilm || ((counts.w & FRAMEF_BACKDROP) != 0u);

	// R12 niebla: gate = fogShow del proyecto Y render OPACO. En el film
	// transparente NO hay niebla (mismo gate render_background_user del raster:
	// la niebla sobre un fondo recortado contaminaria el alpha; leccion de la
	// saga del alpha). Se resuelve en el hit PRIMARIO y se aplica al final,
	// sobre la radiancia COMPLETA del camino (out = mix(radiancia, col, fog)).
	bool fogOn = (fogParams1.z > 0.5) && !alphaFilm;
	float fogF = 0.0;
	float3 fogCol = float3(0.0, 0.0, 0.0);

	float cosSunMax = sunDir_cosAng.w;
	float sunOmega = PT_TWO_PI * (1.0 - cosSunMax); // angulo solido del disco
	float pdfSunNee = (sunOmega > 1.0e-6) ? 1.0 / sunOmega : 0.0;

	// Pesos de grupo de la NEE de "otra fuente" (constantes del frame; el
	// mismo calculo alimenta el peso MIS de la emision por hit). El bucle de
	// luces es determinista y el oraculo lo replica en el mismo orden.
	// wEmi queda en el dominio CRUDO de la CDF (para SampleEmiCdf y
	// EmiSlotProb); el split de grupo usa la potencia EFECTIVA de los
	// emisores wEmi * emissiveScale * pi (la CDF del host no incluye la
	// escala ni el pi del flujo Lambert L*pi*A). Solo cambia el reparto de
	// muestras entre grupos (varianza): pEmi entra IGUAL en la pdf de la NEE
	// y en el peso MIS del hit, asi que jamas sesga. emissiveScale 0 apaga
	// el grupo (sus muestras irian a radiancia 0).
	float wEmi = (counts.y > 0u) ? max(gEmiCdf[counts.y - 1u], 0.0) : 0.0;
	float wEmiEff = wEmi * res_clamp.w * PT_PI;
	float wLights = 0.0;
	[loop]
	for (uint li = 0u; li < counts.x; li++)
		wLights += LightPowerWeight(gLights[li]);
	// F5 (C1): peso del grupo ENTORNO. fogParams1.w = project_cinema_env_light
	// (0 = OFF = camino BIT-IDENTICO; el flag ES el valor). No es una potencia
	// fisica y no pretende serlo: el domo esta en el infinito y no tiene "area"
	// con la que competir contra un flujo lum*area, asi que envK es un REPARTO
	// DE PRESUPUESTO DE MUESTRAS -- envK = 1 da al domo la mitad de las muestras
	// de esta NEE, 0.1 le da ~1/11. Es legitimo porque pEnv entra IGUAL en la
	// pdf y en el peso MIS: cambia la VARIANZA, jamas la imagen convergida.
	// Un domo apagado (luminancia 0) pesa 0 aunque envK > 0: no se malgasta ni
	// una muestra. Sin emisivos ni luces el domo se lleva el grupo entero.
	float envK = fogParams1.w;
	float3 envAvg = (skyZenith.rgb + skyHorizon.rgb + ambient_nightAlpha.rgb) / 3.0;
	envAvg = lerp(envAvg, envAvg * 0.05, saturate(ambient_nightAlpha.w));
	float wEnvEff = 0.0;
	if (envK > 0.0 && LumRec709(envAvg) > 0.0)
		wEnvEff = (wEmiEff + wLights > 0.0) ? (envK * (wEmiEff + wLights)) : 1.0;
	// wOther suma wEnvEff AL FINAL: con wEnvEff = 0.0 exacto, (a+b)+0.0 == a+b
	// bit a bit, asi que pEmi -- que tambien alimenta el peso MIS de la emision
	// por hit -- no se mueve ni un ulp con el flag apagado.
	float wOther = wEmiEff + wLights + wEnvEff;
	float pEmi = (wOther > 0.0) ? wEmiEff / wOther : 0.0;
	float pEnv = (wOther > 0.0) ? wEnvEff / wOther : 0.0;
	// F5: pdf con la que la NEE de entorno habria muestreado la direccion que
	// genero el vertice ACTUAL (lado BSDF del MIS del domo, igual que
	// prevPdfBsdf lo es del sol). 0 = ninguna NEE de entorno compitio ->
	// el cielo entra con peso 1, que es el camino de siempre.
	float prevPdfEnvNee = 0.0;

	// pdf de MEZCLA del rayo de BSDF que genero el vertice actual; < 0 marca
	// el rayo de camara. prevDelta marca que el vertice anterior fue un lobo
	// DELTA (agua/cristal): ninguna NEE compitio alli, asi que emision y
	// disco solar del vertice/miss actual entran con peso 1 (el rayo de
	// camara comparte el peso 1 de la emision, pero NO pinta el disco:
	// paridad con el fondo del raster).
	float prevPdfBsdf = -1.0;
	bool prevDelta = false;

	// Estado de medio del agua (Beer-Lambert): activo tras una refraccion
	// que ENTRA (frontFace); se apaga al refractar de salida. La sigma es la
	// del material de la superficie por la que se entro.
	bool inWater = false;
	float3 waterSigma = float3(0.0, 0.0, 0.0);

	// CAPAS SLICING (S1): ventana de profundidad del rayo de CAMARA. t se mide
	// ACUMULADO desde el origen del rayo primario (con DoF, a lo largo del rayo
	// de lente). bandOn=0 -> camino BIT-IDENTICO al de siempre.
	bool  bandOn = (bandParams.z > 0.5);
	float bandT0 = bandParams.x;
	float bandT1 = (bandParams.y > 0.0) ? bandParams.y : PT_T_MAX;
	float tCam   = 0.0;   // distancia acumulada del segmento de camara

)HLSL" R"HLSL(	[loop]
	for (uint bounce = 0u; bounce < maxBounces; bounce++)
	{
		// R15 (holdout): stillPrimary ES el estado "sigo siendo rayo de camara"
		// -- arranca true, sobrevive a las continuaciones RECTAS por vidrio fino
		// (transmision) y se apaga en el primer scatter (superficie opaca o
		// reflexion). Justo la semantica que pide el holdout.
		// CAPAS SLICING: el "atravesar" de la ventana NO es un camino nuevo --
		// es el tMin que TraceClosest ya recibe (un candidato con t < t0 jamas
		// se acepta, exactamente como el holdout, y ademas sin gastar el
		// presupuesto de texels fantasma).
		float tMinSeg = (bounce == 0u) ? 0.0 : PT_T_EPS;
		if (bandOn && stillPrimary && bandT0 > 0.0)
			tMinSeg = max(tMinSeg, bandT0 - tCam);
		HitInfo hit = TraceClosest(rayOrg, rayDir, tMinSeg, stillPrimary);

		// CAPAS SLICING: el primer hit no-atravesado que cae MAS ALLA de la
		// ventana mata el sample (radiancia 0, cobertura 0) -- misma semantica
		// que el MATTE de F1, pero por DISTANCIA. Va ANTES del miss: un miss
		// (t = +inf) solo pertenece a la banda con t1 = +inf (el telon), y en
		// las demas capas el film transparente ya lo deja sin aportar nada.
		if (bandOn && stillPrimary && hit.tri != PT_INVALID && (tCam + hit.t) >= bandT1)
		{
			if (bounce == 0u)
			{
				// AOVs del primer contacto (guia del denoiser): sin esto un
				// pixel 100% fuera de ventana deja el AOV a conteo 0 y el
				// denoise divide por 0. Espejo del bloque MATTE de mas abajo.
				gAovAlbedo[pix] += float4(0.0, 0.0, 0.0, 1.0);
				gAovNormalDepth[pix] += float4(0.0, 0.0, 0.0, hit.t);
			}
			break;
		}

		// --- Beer-Lambert del segmento recorrido DENTRO del agua ---
		if (inWater)
		{
			float segT = min((hit.tri == PT_INVALID) ? PT_SEG_MAX : hit.t, PT_SEG_MAX);
			throughput *= exp(-waterSigma * segT);
		}

		if (hit.tri == PT_INVALID)
		{
			// --- miss: cielo (sin NEE: peso 1) y, desde bounce 1, la mitad
			// BSDF del MIS del disco solar ---
			float3 esc = SkyRadiance(rayDir);
			// F5 (C1): la OTRA mitad del MIS del domo. Si en el vertice anterior
			// la NEE de entorno pudo muestrear esta direccion, el cielo que llega
			// por el rayo de BSDF entra con su peso balance -- si no, sumaria dos
			// veces. Con el flag apagado prevPdfEnvNee es 0 en todo el camino y
			// esta rama no se ejecuta NUNCA: bit-identico.
			// Va ANTES del disco solar: el sol tiene su propio MIS y no se toca.
			if (prevPdfEnvNee > 0.0 && prevPdfBsdf > 0.0)
				esc *= prevPdfBsdf / (prevPdfBsdf + prevPdfEnvNee);
			if (bounce == 0u)
			{
				// Film transparente: el fondo no aporta color ni alpha al
				// beauty, pero cielo y sol SIGUEN iluminando en los rebotes
				// de los caminos que si tocan geometria (seccion 10).
				// El disco solar NO se pinta en primarios: el fondo del
				// raster tampoco lo trae (la paridad exacta de fondo llega
				// en M3 con el bake del cielo a cubemap).
				gAovAlbedo[pix] += float4(bgHoldout ? float3(0.0, 0.0, 0.0) : esc, 1.0);
				gAovNormalDepth[pix] += float4(0.0, 0.0, 0.0, sunRadiance.w); // depth = far
			}
			else
			{
				// MIS del sol, lado BSDF: un rayo que cae dentro del disco
				// suma su radiancia ponderada contra la pdf de la NEE del
				// sol (que SIEMPRE se lanza en kinds no-delta: splitting).
				// Tras un rebote DELTA no hubo NEE: el disco entra con peso
				// 1 (espejos/agua ven el sol). Sol delta (omega ~ 0):
				// inalcanzable por BSDF, solo NEE.
				if (pdfSunNee > 0.0 && dot(rayDir, sunDir_cosAng.xyz) >= cosSunMax)
				{
					if (prevDelta)
						esc += sunRadiance.rgb;
					else if (prevPdfBsdf > 0.0)
						esc += sunRadiance.rgb * (prevPdfBsdf / (prevPdfBsdf + pdfSunNee));
				}
			}

			// ALPHA: un rayo que SIGUE SIENDO DE CAMARA y se escapa esta
			// viendo el FONDO -- y da igual en que rebote pase (el rebote 0
			// solo es el caso particular de "no habia nada delante"). Un
			// primario que cruza recto una capa de cobertura o un vidrio fino
			// y acaba en el cielo llegaba aqui con la cobertura PARCIAL de lo
			// atravesado: en un render OPACO eso dejaba el pixel a alpha < 1
			// (fantasma al componer) y ademas le inflaba el brillo, porque el
			// resolve des-premultiplica dividiendo por max(suma_alpha, 1).
			//   - opaco: el fondo ES imagen -> aporta color (tenido por lo
			//     que el rayo haya atravesado) y el pixel queda LLENO.
			//   - film transparente (holdout): el fondo ni pinta ni cubre; el
			//     alpha se queda con la cobertura de las capas atravesadas
			//     (0 si no habia ninguna). Antes el fondo SI se colaba en el
			//     beauty desde el rebote 1, que es la misma fuga por el otro
			//     lado.
			if (stillPrimary)
			{
				// FONDO DE PELICULA: con el flag encendido esta rama se comporta
				// como el film transparente (ni color ni cobertura). El fondo lo
				// pone el resolve; y `esc` (el cielo) sigue INTACTO para el
				// catcher y para los rayos que ya no son de camara.
				if (!bgHoldout)
				{
					color += (bounce == 0u) ? esc : ClampIndirect(throughput * esc);
					alpha = 1.0;
				}
			}
			else
				color += ClampIndirect(throughput * esc);
			// F1 4a: la continuacion del catcher se escapo. En el mundo SIN
			// sujeto habria hecho exactamente lo mismo (misma direccion, mismo
			// cielo, mismo disco solar con el mismo peso MIS): el denominador
			// recibe el termino IDENTICO y este sample no aporta sombra. Es la
			// definicion de "no ocluido", y sale exacta -- no aproximada.
			if (catcherSeg)
			{
				catchDen += ClampIndirect(throughput * esc);
				catcherSeg = false;
			}
			break;
		}

)HLSL" R"HLSL(
		// R12 niebla: se resuelve en el PRIMER hit real (la traversal ya
		// descarto ghost-texels y cutouts). El punto es rayOrg + rayDir * t,
		// exactamente el vPosition del raster; viewDir = rayDir (con DoF, el
		// rayo de lente: la distancia del hit sigue siendo la correcta).
		if (fogOn && bounce == 0u)
		{
			float3 hitPos = rayOrg + rayDir * hit.t;
			fogF = FogFactor(hit.t, hitPos.z);
			fogCol = FogColorFor(rayDir);
		}

		// F1 MATTE: el rayo de CAMARA muere aqui -- radiancia 0, cobertura 0.
		// Va DESPUES de la niebla (que se resuelve en el primer hit) y ANTES de
		// EvalSurface: no hay sombreado que hacer. Los rayos de sombra/GI/reflejo
		// llegan con stillPrimary=false y NUNCA entran aqui: el objeto sigue
		// sombreando, rebotando y saliendo en espejos. Sin materiales matte el
		// bit vale 0 en toda la escena y el camino es BIT-IDENTICO.
		if (stillPrimary && (gMats[gTris[hit.tri].matId].flags & MATF_CAM_MATTE) != 0u)
		{
			if (bounce == 0u)
			{
				// AOVs del primer contacto (guia del denoiser): albedo 0 con su
				// conteo -- si no se escribe, un pixel 100% matte deja el AOV a
				// conteo 0 y el denoise divide por 0.
				gAovAlbedo[pix] += float4(0.0, 0.0, 0.0, 1.0);
				gAovNormalDepth[pix] += float4(0.0, 0.0, 0.0, hit.t);
			}
			break;
		}

		SurfaceM2 s = EvalSurface(hit, rayDir);

		// Cobertura del film (alpha): un primario que ATRAVIESA vidrio fino
		// sigue siendo primario (la transmision es recta) y la cobertura se
		// resuelve CAPA A CAPA con el operador OVER, no como un maximo sobre
		// toda la cadena. Los AOVs siguen siendo del PRIMER contacto (guia
		// del denoiser).
		if (bounce == 0u)
		{
			gAovAlbedo[pix] += float4(s.albedo, 1.0);
			gAovNormalDepth[pix] += float4(s.ns, hit.t); // ns con normal map: guia mejor al denoiser
		}
		// F1 4a: SHADOW CATCHER. Se reconoce SOLO en el vertice primario del
		// bounce 0 -- ahi `color` vale exactamente 0 (nada ha sumado todavia en
		// esta iteracion) y por eso todo lo que el camino acumule a partir de
		// aqui ES el numerador, sin necesidad de un acumulador aparte ni de
		// desviar los ~10 `color +=` del integrador. Un catcher alcanzado tras
		// un scatter (o al otro lado de un cristal) se comporta como geometria
		// normal: documentado, y ademas es lo unico seguro (alli `color` ya no
		// es 0). Kinds delta (agua/cristal) no reciben NEE: sin NEE no hay
		// cociente, asi que tampoco son catchers.
		bool isCatcher = (bounce == 0u) && stillPrimary
			&& ((gMats[gTris[hit.tri].matId].flags & MATF_SHADOW_CATCHER) != 0u)
			&& (s.kind == PT_KIND_STD || s.kind == PT_KIND_LEAF || s.kind == PT_KIND_COVER);
		if (isCatcher)
		{
			catcherPath = true;
			catchOpacity = gMats[gTris[hit.tri].matId].shadowOpacity;
			catchSkyBehind = SkyRadiance(rayDir); // preview opaco: fondo detras
			// El catcher NO cubre: su alpha lo fija el cociente al final del
			// sample, no el "he tocado algo" del film. Pero SI deja de ser rayo
			// de camara: lo que haya detras no debe repintarse ni re-cubrir.
			stillPrimary = false;
		}
		else if (stillPrimary)
		{
			// COBERTURA y VIDRIO deciden su aporte TRAS muestrear el lobo
			// (abajo): su cobertura no es una propiedad del hit sino de si el
			// sample interactuo con la capa o paso de largo. Sumar aqui aEff
			// ADEMAS de subir el alpha a 1 en la rama que interactua contaba
			// la capa DOS VECES (E[alpha] = 2a - a^2 en vez de a) y rompia el
			// premultiplicado que el resolve deshace al dividir por
			// max(suma_alpha, 1): el color straight salia mal.
			if (s.kind != PT_KIND_COVER && s.kind != PT_KIND_GLASS)
			{
				alpha = 1.0;
				stillPrimary = false;
			}
		}

		// --- emision por hit de BSDF (seccion 4.7), con peso MIS balance si
		// el tri esta en la lista NEE: la NEE de emisivos del vertice
		// anterior ya aporta su mitad y sumar el hit entero duplicaria la
		// energia. Primarios, rebotes delta y tris fuera de la lista:
		// peso 1. ---
#ifdef CINEMA_TRIM
		if (s.emissive > 0.0 || any(s.trimRgb > 0.0))
#else
		if (s.emissive > 0.0)
#endif
		{
			float misW = 1.0;
			if (!prevDelta && prevPdfBsdf > 0.0 && pEmi > 0.0)
			{
				uint slot = FindEmiSlot(hit.tri);
				if (slot != PT_INVALID)
				{
					// pdf con la que la NEE del vertice anterior habria
					// muestreado ESTE punto (seleccion * area->angulo solido;
					// s.ng ya esta volteada hacia el rayo: abs da |cosL|)
#ifdef CINEMA_RAYQUERY
					float area = TriAreaInst(gTris[hit.tri], hit.o2w); // M8-A: area MUNDO
#else
					float area = TriArea(gTris[hit.tri]);
#endif
					float cosL = abs(dot(s.ng, rayDir));
					float pdfNee = pEmi * EmiSlotProb(slot, wEmi)
						* (hit.t * hit.t) / max(area * cosL, 1.0e-12);
					misW = prevPdfBsdf / (prevPdfBsdf + pdfNee);
				}
			}
			float3 le = throughput * s.albedo * (s.emissive * res_clamp.w) * misW;
#ifdef CINEMA_TRIM
			// TRIM: sumado, NO plegado en la expresion de arriba. Ver el
			// comentario de SurfaceM2.trimRgb: asi la escena sin trim conserva
			// la forma exacta de la multiplicacion que tenia.
			le += throughput * s.trimRgb * (res_clamp.w * misW);
#endif
			color += (bounce == 0u) ? le : ClampIndirect(le);
			// F1 4a: la continuacion del catcher ha dado en una FUENTE. Una luz
			// no es un ocluyente: en el mundo sin sujeto el mismo rayo daria en
			// la misma fuente. Denominador = el MISMO termino -> aporte de
			// sombra 0. Sin esto, un softbox emisivo encima del catcher se
			// proyectaria su propia sombra sobre el suelo (el denominador solo
			// tendria el domo, mucho mas oscuro que el emisor).
			//
			// RONDA 5 -- EL SEGUNDO ACANTILADO. `s.emissive > 0` a secas era el
			// MISMO acantilado que el de TraceShadow, en el rayo de
			// CONTINUACION: un ocluyente con EMISSIVE 0,001 devuelve le ~ 0, y
			// meter ESE 0 en el denominador en vez de la referencia del domo
			// borraba la parte de la sombra que viene de la GI/cielo. MEDIDO en
			// el gate de esta ronda (sol + suelo catcher + cubo solido):
			// EMISSIVE 0 -> 34.706 px de sombra; EMISSIVE 0,001 -> 6.217 px,
			// y con 0,01 lo mismo (es un ESCALON en e > 0, no una dosis).
			//
			// El criterio de "esto es una LAMPARA, no un ocluyente" ya no es
			// "emite algo" sino "emite AL MENOS lo que emitiria el domo abierto
			// en esa direccion". Es continuo, no tiene acantilado, y cuando
			// decide "lampara" hace EXACTAMENTE lo de antes:
			//   * softbox real (EMISSIVE 1 x emissive_scale) -> arrasa a
			//     cualquier domo -> rama de siempre, bit-identica;
			//   * cielo NEGRO (la receta softbox+catcher) -> lum(domo) = 0 ->
			//     cualquier emisivo es fuente -> rama de siempre, bit-identica;
			//   * trim brillante / antorcha del PRODUCTO bajo un domo claro ->
			//     ocluyente, y su sombra de contacto sobrevive.
			// Se compara la emision CRUDA del material (sin misW y sin
			// throughput) contra la radiancia CRUDA del domo: los dos factores
			// que sobran estan en los dos lados y solo enturbiarian el umbral.
			float3 emiCruda = s.albedo * (s.emissive * res_clamp.w);
			bool esFuente = LumRec709(emiCruda) >= LumRec709(SkyRadiance(rayDir));
			if (catcherSeg && esFuente)
			{
				catchDen += ClampIndirect(le);
				catcherSeg = false;
			}
		}
		// F1 4a: y si NO era una fuente, es un OCLUYENTE. La referencia sin
		// sujeto para esa direccion es el domo analitico, que ya existe y no
		// cuesta un rayo (SkyRadiance). El numerador se quedara con lo poco que
		// el rebote devuelva: la diferencia ES la sombra, y el rebote que si
		// vuelve es justo lo que aclara la sombra por GI.
		if (catcherSeg)
		{
			// F5 (C1): el gemelo tiene que ver el domo EXACTAMENTE igual que el
			// numerador. Con NEE de entorno encendida el cielo que llega por el
			// lado BSDF ya viene ponderado por MIS (arriba, en el miss), asi que
			// la referencia "sin sujeto" de esta direccion lleva el MISMO peso.
			// Sin ese ajuste el denominador contaria el domo entero contra un
			// numerador que solo trae su mitad BSDF y el alfa se rompe.
			// Con el flag apagado prevPdfEnvNee es 0 y la expresion es la de
			// siempre, caracter por caracter.
			float3 refEnv = throughput * SkyRadiance(rayDir);
			if (prevPdfEnvNee > 0.0 && prevPdfBsdf > 0.0)
				refEnv *= prevPdfBsdf / (prevPdfBsdf + prevPdfEnvNee);
			catchDen += ClampIndirect(refEnv);
			catcherSeg = false;
		}

		// --- dims RNG del bounce (contrato junto a Rand4) ---
		float4 rs = Rand4(pixelAbs, 1u + bounce * 4u);
		float4 rLobe = Rand4(pixelAbs, 4u + bounce * 4u);
		float offs = max(PT_T_EPS, hit.t * 1.0e-4);
		float3 wo = -rayDir;

		// --- NEE (solo kinds no-delta: agua/cristal no la reciben). COBERTURA
		// SI la recibe: su lobulo difuso es no-delta y EvalBsdfCosPdf ya lo
		// escala por aEff, asi que la luz directa sobre la fraccion cubierta
		// entra por NEE (sin ella la lente saldria ruidosisima). ---
		if (s.kind == PT_KIND_STD || s.kind == PT_KIND_LEAF || s.kind == PT_KIND_COVER)
		{
			// --- NEE del sol: SIEMPRE 1 rayo al disco (splitting) ---
			float3 sunL;
			float sunWeight;
			float misSun = 1.0;
			if (sunOmega > 1.0e-6)
			{
				// Disco de area: pdf = 1/omega -> peso = omega
				sunL = SampleSunCone(rs.xy, sunDir_cosAng.xyz, cosSunMax);
				sunWeight = sunOmega;
			}
			else
			{
				// Sol delta (radio angular ~0): sunRadiance se interpreta
				// como irradiancia preintegrada; sin MIS (el BSDF no puede
				// alcanzar un delta)
				sunL = sunDir_cosAng.xyz;
				sunWeight = 1.0;
			}

			float3 fcosSun;
			float pdfSunB;
			EvalBsdfCosPdf(s, wo, sunL, fcosSun, pdfSunB);
			if (fcosSun.r + fcosSun.g + fcosSun.b > 0.0)
			{
				if (sunOmega > 1.0e-6)
					misSun = pdfSunNee / (pdfSunNee + pdfSunB);
				float3 cBase = throughput * sunRadiance.rgb
					* fcosSun * sunWeight * misSun;
				float3 sunTrans = TraceShadow(NeeShadowOrg(s, offs, sunL), sunL, 0.0, PT_T_MAX);
				// F1 4a: el gemelo del sol = el MISMO termino con trans = 1.
				// Va FUERA del `if (sunTrans > 0)`: el caso interesante del
				// catcher es justo la umbra, donde la transmitancia es 0 y el
				// numerador no suma nada. Mismo misSun a los dos lados -> el
				// peso MIS se cancela en el cociente y la penumbra del disco
				// solar (FIX-A) viaja intacta al alpha.
				// RONDA 5 (fix del acantilado): el sol NO tiene geometria en el
				// BVH, asi que NUNCA puede taparse a si mismo. Lo que corte el
				// rayo solar es un ocluyente real, emisivo o no. Antes bastaba
				// un ocluyente con EMISSIVE 0,001 para igualar denominador y
				// numerador y borrar la sombra ENTERA del cuadro (medido).
				if (isCatcher)
					catchDen += cBase;
				if (sunTrans.r + sunTrans.g + sunTrans.b > 0.0)
				{
					// BIT-IDENTIDAD: expresion EXACTA de siempre; cBase es del gemelo.
					float3 c = throughput * sunTrans * sunRadiance.rgb
						* fcosSun * sunWeight * misSun;
					color += (bounce == 0u) ? c : ClampIndirect(c);
				}
			}

			// --- NEE de OTRA fuente: 1 de {emisivos CDF, point/spot} por
			// potencia (splitting del sol aparte, seccion 6) ---
			if (pEmi > 0.0 || wLights > 0.0 || pEnv > 0.0)
			{
				float4 rl = Rand4(pixelAbs, 2u + bounce * 4u);
				float3 unsh;
				// F5: en el ultimo rebote el rayo de continuacion no se traza ->
				// la NEE de entorno no compite con nadie (ver SampleOtherLight).
				bool envMisOn = (bounce + 1u) < maxBounces;
				float3 cl = throughput * SampleOtherLight(s, wo, offs,
					hit.tri, rl, pEmi, wEmi, wLights, pEnv, envMisOn, unsh);
				color += (bounce == 0u) ? cl : ClampIndirect(cl);
				// F1 4a: el gemelo de la OTRA fuente (softbox emisivo o
				// point/spot). Misma fuente elegida, mismo punto, misma pdf y
				// mismo peso MIS que el numerador: solo se le quita el
				// TraceShadow. Este es el termino que produce la sombra de
				// contacto de la receta "softbox arriba + catcher en el suelo".
				if (isCatcher)
					catchDen += throughput * unsh;
			}
		}

		// --- continuar el camino: muestreo del BSDF M2 ---
		float3 newDir;
		float3 bsdfWeight;
		float pdfNew;
		bool isDelta;
		bool refracted;
		if (!SampleBsdf(s, wo, rs.zw, rLobe, newDir, bsdfWeight, pdfNew, isDelta, refracted))
			break;

		// Cobertura del film, resuelta con el lobo ya muestreado:
		//   - INTERACTUO con la capa (reflexion del vidrio / lobulo difuso de
		//     la cobertura): lo que se ve en este sample ES la capa y tapa lo
		//     de detras -> pixel LLENO y deja de ser primario.
		//   - PASO DE LARGO: sigue siendo primario.
		//       * COBERTURA: no aporta nada. El lobulo ya es el estimador de
		//         la cobertura (interactua con probabilidad aEff), asi que
		//         E[alpha] = aEff sobre el cielo y 1 sobre algo opaco --
		//         exacto, y en fase con el estimador de COLOR (que tambien
		//         aporta con probabilidad aEff): el resolve puede
		//         des-premultiplicar sin sesgar.
		//       * VIDRIO: su transmision NO es estocastica en aEff (siempre
		//         transmite, tenida por glassTint), asi que la cobertura de
		//         la capa entra aqui ANALITICA, con OVER. El Fresnel ya lo
		//         cubre la rama de reflexion de arriba.
		if (stillPrimary && (s.kind == PT_KIND_GLASS || s.kind == PT_KIND_COVER))
		{
			if (!refracted)
			{
				alpha = 1.0;
				stillPrimary = false;
			}
			else if (s.kind == PT_KIND_GLASS)
				alpha = alpha + (1.0 - alpha) * saturate(s.glassAlpha);
		}

		// Estado del medio: la refraccion del agua entra/sale segun la cara
		if (s.kind == PT_KIND_WATER && refracted)
		{
			inWater = (s.frontFace > 0.5);
			waterSigma = s.waterSigma;
		}

		throughput *= bsdfWeight;

		// F1 4a: sale la continuacion del catcher. El proximo vertice (o miss)
		// decide su termino del denominador -- y solo ESE, no los siguientes:
		// la GI que llegue mas adentro entra unicamente en el numerador, que es
		// lo correcto (la luz de rebote ACLARA la sombra, no la define).
		if (isCatcher)
			catcherSeg = true;

		// --- ruleta rusa desde el bounce 3 (dim RNG propia, ver contrato):
		// p = clamp(lum(throughput), 0.05, 0.95); sobrevivir divide ---
		if (bounce >= 3u)
		{
			float pCont = clamp(LumRec709(throughput), 0.05, 0.95);
			if (Rand4(pixelAbs, 3u + bounce * 4u).x >= pCont)
				break;
			throughput /= pCont;
		}

		rayDir = newDir;
		prevPdfBsdf = isDelta ? -1.0 : pdfNew;
		prevDelta = isDelta;
		// F5 (C1): pdf de la NEE de entorno para la direccion que acaba de
		// salir. Solo hubo NEE de entorno si el kind la recibe y el lobo no es
		// delta; y una direccion por DEBAJO de s.ns (transmision de hoja) esta
		// fuera del hemisferio que esta estrategia sabe generar -> pdf 0 -> el
		// cielo de esa direccion entra con peso 1. Con pEnv = 0 esto es
		// constante 0 y ninguna de las tres ramas que lo leen se ejecuta.
		prevPdfEnvNee = 0.0;
		if (pEnv > 0.0 && !isDelta
			&& (s.kind == PT_KIND_STD || s.kind == PT_KIND_LEAF || s.kind == PT_KIND_COVER))
			prevPdfEnvNee = pEnv * max(dot(newDir, s.ns), 0.0) / PT_PI;
		// CAPAS SLICING: acumular la distancia recorrida por el segmento de
		// camara (continuaciones RECTAS por vidrio fino / cobertura). Tras el
		// primer scatter stillPrimary ya es false y esto deja de importar.
		if (stillPrimary)
			tCam += hit.t;
		// Origen del rebote: al lado geometrico hacia el que sale el rayo
		// (la refraccion/transmision cruza la superficie)
		float sgnOrg = (dot(newDir, s.ng) >= 0.0) ? 1.0 : -1.0;
		rayOrg = s.pos + s.ng * (offs * sgnOrg);
	}
)HLSL" R"HLSL(
	// --- F1 4a: RESOLVE DEL SHADOW CATCHER. Aqui `color` ES el numerador
	// (todo lo que el camino trajo de vuelta desde el catcher) y catchDen el
	// denominador (lo mismo sin ocluyentes). El catcher NO aporta radiancia
	// propia al pixel: aporta ALPHA.
	//   alpha = 1 - Lnum/Lden, POR CANAL. Se conserva el cociente de los tres
	//   canales (la sombra bajo un domo frio es azulada y bajo un softbox
	//   calido es calida) pero el film tiene UN solo alpha, asi que se emite el
	//   canal MAS ocluido: es el que define la silueta de la sombra y el que no
	//   la deja evaporarse cuando una fuente de color satura un canal.
	//   El cociente va ANTES de la niebla y ANTES del invariante opaco.
	// Cero varianza cuando no hay nada que ocluir: numerador y denominador
	// comparten las mismas direcciones muestreadas, asi que en un punto sin
	// ocluyentes son el MISMO numero sample a sample y el alpha sale 0 exacto,
	// no "0 mas ruido".
	if (catcherPath)
	{
		float3 num = max(color, float3(0.0, 0.0, 0.0));
		float3 den = max(catchDen, float3(0.0, 0.0, 0.0));
		float3 a3 = float3(0.0, 0.0, 0.0);
		// Piso del denominador: un canal a cero (fuente sin ese color, o el
		// catcher mirando a un domo negro) no puede inventarse sombra.
		if (den.r > 1.0e-6) a3.r = saturate(1.0 - num.r / den.r);
		if (den.g > 1.0e-6) a3.g = saturate(1.0 - num.g / den.g);
		if (den.b > 1.0e-6) a3.b = saturate(1.0 - num.b / den.b);
		alpha = saturate(max(a3.r, max(a3.g, a3.b)) * catchOpacity);
		catchSignal = alpha;   // lo que el M7 adaptive tiene que vigilar aqui
		// Film TRANSPARENTE (el caso de uso): sombra recta, color 0. El
		// compositor la pone sobre el fondo que quiera y el alpha manda.
		// Film OPACO: se pinta el fondo atenuado por la sombra, para que el
		// preview del editor ensene EXACTAMENTE lo que va a exportar en vez de
		// un agujero negro (el invariante opaco de abajo forzara alpha = 1).
		// FONDO DE PELICULA: el catcher se queda en holdout puro (color 0, alpha =
		// sombra) tambien con film opaco. El resolve pone el fondo debajo con el
		// operador OVER en display -- que es EXACTAMENTE lo que hace un
		// compositor con esta pasada. Multiplicar aqui el fondo por (1 - alpha)
		// en radiancia y pasarlo luego por el hombro daria una sombra distinta
		// (medido: hasta 93/255 de diferencia a alpha 0,5).
		color = bgHoldout ? float3(0.0, 0.0, 0.0) : (catchSkyBehind * (1.0 - alpha));
	}

	// --- R12 niebla: out = mix(radiancia del camino, color de niebla, fog).
	// Va AQUI, sobre la radiancia completa del sample (directa + GI + emision
	// vistas a traves del segmento primario), igual que el raster fogea el
	// pixel ya iluminado. Con fog 0 (gate off, cielo, o distancia dentro de la
	// zona despejada) es EXACTAMENTE la identidad: bit-identico al camino R11.
	if (fogOn && fogF > 0.0)
		color = FogMix(color, fogCol, fogF);

	// --- INVARIANTE del render OPACO: alpha 1, sin excepciones. El fondo es
	// parte de la imagen, asi que NO existe pixel translucido; y el resolve
	// des-premultiplica dividiendo por max(suma_alpha, 1), asi que cualquier
	// alpha < 1 aqui ademas INFLA el brillo por 1/E[alpha]. Los caminos de
	// arriba ya lo dejan en 1, pero un sample puede morir todavia primario
	// por otras vias (SampleBsdf sin lobo valido, ruleta rusa, presupuesto de
	// rebotes agotado dentro de una pila de capas): esta linea las cubre
	// TODAS. En film transparente no toca nada. ---
	// FONDO DE PELICULA: tampoco toca nada. El invariante existe porque "el fondo
	// ES parte de la imagen"; con el fondo de pelicula el fondo lo pone el
	// RESOLVE, asi que el film tiene que llegar alli con la COBERTURA REAL para
	// que el OVER sea correcto (y el des-premultiplicado del resolve, que divide
	// por max(suma_alpha, 1), es justo el que hace falta). El alpha de SALIDA lo
	// fuerza a 255 el resolve: el PNG sigue siendo opaco. ---
	if (!bgHoldout)
		alpha = 1.0;

	// --- acumulacion determinista; cero NaN/inf en el acumulador (criterio
	// (e) de M0; compilar con IEEE strictness para que no se optimice) ---
	if (!all(isfinite(color)))
		color = float3(0.0, 0.0, 0.0);
	if (!isfinite(alpha))
		alpha = 0.0;
	gAccum[pix] += float4(color, alpha);

	// --- M7 adaptive: Welford online sobre la LUMINANCIA del sample (media +
	// M2: numericamente estable en float, mejor que sumas de cuadrados). Se
	// llena SIEMPRE (con adNoise 0 tambien): coste extra pero JAMAS toca
	// gAccum/AOV/RNG -> bit-identico. La convergencia solo se marca cada
	// PT_ADAPT_INTERVAL samples y con piso PT_ADAPT_MIN (evita declararlo
	// convergido por un tramo afortunado de baja varianza temprana). Va DESPUES
	// del gAccum: el compilador no puede reordenarlo para alterar color. ---
	// F1 4a: en un camino de CATCHER la radiancia del pixel es CERO por
	// definicion (el catcher no pinta: aporta alpha). Vigilando `color` la
	// senal del adaptive es constante 0, relErr sale 0 y el pixel se declara
	// CONVERGIDO a los 32 samples -- la sombra entera se quedaba en 32 spp de
	// un render de 512. MEDIDO en el hero (2048^2, 512 spp, noise_threshold
	// 0.008): ruido local 7x7 del alfa en la sombra 8.05/255 con el adaptive
	// asi, contra 2.11/255 con el adaptive APAGADO -- 4x. Lo que hay que
	// vigilar en un catcher es su ALPHA, que es lo que el pixel aporta.
	// Sin catchers en escena catcherPath es false en todos los caminos y esta
	// linea es la de siempre: BIT-IDENTICO.
	float lum = catcherPath ? catchSignal : LumRec709(color);
	float4 vc = gVarCount[pix];
	float n = vc.z + 1.0;
	float delta = lum - vc.x;
	float mean = vc.x + delta / n;
	float M2 = vc.y + delta * (lum - mean);
	float done = vc.w;
	if (adNoise > 0.0 && n >= PT_ADAPT_MIN && fmod(n, PT_ADAPT_INTERVAL) < 0.5)
	{
		float varc = (n > 1.0) ? M2 / (n - 1.0) : 0.0;
		float relErr = sqrt(max(varc, 0.0) / n) / max(mean, PT_ADAPT_FLOOR);
		if (relErr < adNoise)
			done = 1.0;
	}
	gVarCount[pix] = float4(mean, M2, n, done);
}

// ---------------------------------------------------------------------------
// CSClear: limpia acumulador y AOVs del frame ENTERO. Ignora el offset de
// tile: el host despacha ceil(width/8) x ceil(height/8) grupos una vez por
// render (antes del primer sample).
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSClear(uint3 dtid : SV_DispatchThreadID)
{
	uint width = (uint)res_clamp.x;
	uint height = (uint)res_clamp.y;
	if (dtid.x >= width || dtid.y >= height)
		return;
	uint pix = dtid.y * width + dtid.x;
	float4 zero = float4(0.0, 0.0, 0.0, 0.0);
	gAccum[pix] = zero;
	gAovAlbedo[pix] = zero;
	gAovNormalDepth[pix] = zero;
	gVarCount[pix] = zero; // M7 adaptive: media/M2/conteo/convergido a 0
}
)HLSL";
	}
}
