#pragma once
#include "Common.hpp"

#include <cstdint>

// ---------------------------------------------------------------------------
// CinemaOracle -- oraculo CPU del integrador M2 del motor Cinema
// (HEADLESS/CINEMA_DESIGN.md, secciones 5, 6, 7 y milestones M1/M2 de la 13).
//
// Es el MISMO integrador del kernel M2 en C++ escalar, corriendo sobre la
// escena CPU retenida por CinemaScene (PTVert/PTTri reordenados + PTNode de
// CinemaBvh, PTMaterial, PTLight, env, camara y los slices QImage del atlas).
// Sirve para validar el kernel GPU por paridad de imagen (RMSE) a
// resoluciones chicas (320x180 / 16 spp): claridad sobre velocidad. La unica
// paralelizacion es OpenMP por filas; cada pixel es independiente porque el
// RNG es counter-based (funcion pura de (pixel, sample, seed, dim)).
//
// CONTRATO DE BIT-COMPATIBILIDAD CON EL KERNEL (espejo del kernel M2 de
// CinemaKernelM0.hpp):
//
//   RNG: pcg4d identico al de CinemaKernelM0.hpp con el mapeo a float
//        (x >> 8) * (1 / 2^24)  ([0,1) garantizado y exacto en float).
//        Semilla del vector: (pixelAbs.x, pixelAbs.y,
//        sampleIdx + frameSeed * 0x9E3779B9, dim) con aritmetica uint32.
//
//   Dimensiones por sample (CinemaOracleDims abajo, espejo del contrato
//   documentado en el kernel M2 de CinemaKernelM0.hpp):
//        dim 0                  .xy = jitter subpixel de camara (igual M0)
//        dim 1 + bounce*4 + 0   .xy = disco solar (NEE del sol)
//                               .zw = hemisferio coseno (lobulo DIFUSO del
//                                     BSDF: Lambert de kind 0 u hoja)
//        dim 1 + bounce*4 + 1   .x  = seleccion de fuente (grupo emisivos vs
//                                     luces y, REMAPEADA, la fuente dentro
//                                     del grupo)
//                               .yz = punto por area en el tri emisor O cono
//                                     hacia la esfera de la luz (segun grupo)
//                               .w  = reserva (no usada)
//        dim 1 + bounce*4 + 2   .x  = ruleta rusa (solo bounce >= 3)
//                               .yzw = reserva
//        dim 1 + bounce*4 + 3   .x  = seleccion de lobo del BSDF M2 (difuso
//                                     vs GGX por peso de Fresnel; reflexion
//                                     vs transmision de hoja; Fresnel de
//                                     agua/cristal)
//                               .yz = muestra VNDF del semivector (GGX /
//                                     micro-normal del agua)
//                               .w  = reserva (no usada)
//        El M0 usaba stride 1; M1 stride 3; M2 usa stride 4 (la dim k=3 es
//        nueva). El mapeo es FIJO por bounce: Rand4 es funcion pura de
//        (pixel, sample, dim), asi que saltarse una tecnica (o un kind que
//        no usa .yz) no desplaza las dimensiones de las demas.
//
//   Seleccion de "otra fuente" (splitting: el sol SIEMPRE se muestrea
//        aparte). Porte EXACTO del SampleOtherLight del kernel M1, con las
//        MISMAS fuentes de datos que CinemaScene sube a GPU:
//          - CinemaSceneLights(): array de PTLight (t5)
//          - CinemaSceneGetEmissiveTris() / GetEmissiveCdf(): CDF acumulada
//            SIN normalizar por potencia (area x luminancia media del albedo
//            x e, sin emissive_scale), sobre el array REORDENADO bvh.tris --
//            los mismos bytes que los buffers t6/t7 del kernel.
//        Reparto y pdfs (formulas del kernel, replicadas literalmente):
//          wEmi    = emiCdf.back()  (CRUDO: manda en el dominio de la CDF)
//          wEmiEff = wEmi * emissiveScale * pi  (potencia efectiva de grupo;
//                    la CDF del host no trae la escala ni el pi Lambert)
//          wLights = suma EN FLOAT y en orden de LightPowerWeight(l) =
//                    lum(col) * max(range^2, 1e-4)
//          pEmi    = wEmiEff / (wEmiEff + wLights)
//          con r = rl.x (un solo numero para todo el pick):
//            r <  pEmi -> emisor: rIn = min(r / max(pEmi, 1e-20),
//                         0.99999994); slot = primer j con emiCdf[j] >=
//                         rIn * wEmi; pdf de slot = pEmi * (delta_j / wEmi).
//                         Un emisor no recibe NEE sobre si mismo (selfTri).
//            r >= pEmi -> luz por potencia: recorrido lineal acumulando
//                         LightPowerWeight hasta superar rInL * wLights;
//                         pSel = (1 - pEmi) * peso(luz) / wLights
//        lum() = dot(c, (0.2126, 0.7152, 0.0722)). Si la seleccion
//        divergiera entre kernel y oraculo las imagenes solo coinciden en
//        esperanza y el RMSE a 16 spp sube: mismas formulas, mismo orden.
//
//   Logica del integrador M1 (base; sin cambios en M2 salvo lo marcado):
//        - Sol: NEE del cono del disco SIEMPRE en kinds no-delta (splitting)
//          con MIS balance contra la pdf de MEZCLA del BSDF (M2; en M1 era
//          el coseno). En el miss desde bounce >= 1, un rayo BSDF que cae
//          dentro del disco suma sunRadiance con el peso del lado BSDF
//          prevPdf / (prevPdf + 1/omega); tras un rebote DELTA (agua o
//          cristal, M2) el disco entra con peso 1 (prevDelta). El disco NO
//          se pinta en rayos primarios (el fondo del raster tampoco lo
//          trae) y el cielo no recibe NEE (peso 1 siempre). prevPdf usa el
//          sentinela -1 del kernel para marcar el rayo de camara.
//        - Otra fuente: 1 muestra por vertice de camino (ver reparto abajo).
//          Point/spot: falloff y cono ARTISTICOS del raster, portes exactos
//          de LightFalloff / SpotMask del kernel (sin 1/d^2: la curva
//          LIGHT_RANGE / LIGHT_FADE_SIZE / LIGHT_SPOT_SHARPNESS del forward
//          ES el look MI3), medidos al CENTRO de la luz; sizeRadius > 0 SOLO
//          suaviza la sombra (cono hacia la esfera con energia intacta);
//          sombra con tMax = max(dist - sizeRadius, eps); sin MIS (las luces
//          no estan en el BVH: los rayos de BSDF no pueden alcanzarlas).
//          Emisores: muestra uniforme de area (mapeo sqrt sobre rl.yz), pdf
//          en angulo solido = pEmi * EmiSlotProb * d^2 / max(area * |cosL|,
//          1e-12), MIS balance contra la pdf de mezcla, sombra con tMax =
//          d * 0.9999; radiancia como EmissiveRadianceAt del kernel (texel *
//          blend.rgb * rgbVert * max(e_mat, e_vert) * emissiveScale, con
//          texel de cutout alpha < 0.5 = agujero). M2: la respuesta de la
//          superficie es fcos del BSDF completo (EvalBsdfCosPdf), no solo
//          Lambert, y el origen del shadow ray salta de lado en hojas
//          retroiluminadas (NeeShadowOrg).
//        - Emision vista por BSDF: peso MIS prevPdf / (prevPdf + pdfNee) si
//          el triangulo esta en la lista NEE (bounce 0, rebote delta previo
//          o tri fuera de lista: peso 1).
//        - Sombras coloreadas (mismas ramas que el TraceShadow del kernel):
//          si el material es ALPHA_TEST (y hay presupuesto), texel con alpha
//          < 0.5 pasa libre y texel solido OCLUYE (aunque tambien sea
//          ALPHA_BLEND); si no es ALPHA_TEST pero si ALPHA_BLEND / WATER, la
//          transmitancia acumula texel.rgb * blend.rgb * (1 - texel.a *
//          blendA) y el early-out es trans.r + trans.g + trans.b < 1e-4.
//          Presupuesto compartido de 32 hits transparentes, igual M0. En
//          TraceClosest (rayos de camara/BSDF) ALPHA_BLEND sigue opaco: el
//          cristal/agua como BSDF (M2) se resuelve en el CLOSEST hit.
//        - Ruleta rusa desde bounce >= 3 (0-based), tras actualizar
//          throughput: p = clamp(lum(throughput), 0.05, 0.95); sobrevive si
//          rr.x < p y divide throughput por p.
//        - Clamp de indirecta y saneo NaN/inf por sample: igual M0.
//        - ALPHA_HASH y blue noise NO participan en el oraculo (el kernel
//          debe dejarlos apagados en los runs de paridad).
//
//   Logica M2 (materiales, seccion 7 del diseno; porte 1:1 del kernel):
//        - kind 0 (estandar): Lambert * (1-metallic) + GGX single-scatter
//          (VNDF Heitz 2018, Smith height-correlated, alpha = roughness^2
//          con floor 0.03); F0 = lerp(0.04, albedo, metallic); seleccion de
//          lobo pSpec = lumF / (lumF + lumDiff) con lumF = lum(Schlick(F0,
//          cosO)); peso del rebote = fcos/pdf de la MEZCLA (funcion
//          compartida con la NEE: MIS exacto). metallic/roughness del draw
//          o de uTextureMaterial (SEUS: rough = 1-R, metal = G; LabPBR con
//          flag MATF_MAT_LABPBR: rough = (1-R)^2, metal = G > 0.898); los
//          texels de DATOS se leen en bytes crudos (el kernel deshace la
//          linealizacion sRGB del atlas; el oraculo lee el QImage tal cual).
//        - kind 1 (hoja, MATF_SUBSURFACE o bit 2 de dataFlags en cualquiera
//          de los 3 vertices): thin-translucent, lobulo coseno de reflexion
//          (0.7) + lobulo coseno de transmision (0.3); NEE por ambos lados.
//        - kind 2 (agua): delta dielectrico IOR 1.33, Fresnel exacto + TIR,
//          micro-normal VNDF roughness 0.07, Beer-Lambert con sigma =
//          -log(clamp(blend.rgb, 1e-3, 1)) / 16 aplicada a la distancia de
//          cada segmento recorrido dentro del agua (estado inWater togglea
//          en cada refraccion segun frontFace).
//        - kind 3 (cristal thin, ALPHA_BLEND sin WATER, con alpha efectiva
//          texel.a * blend.a < 0.999): delta, reflexion Schlick F0 0.04 +
//          transmision RECTA tintada por texel.rgb * blend.rgb * (1 -
//          texel.a * blend.a) (el MISMO factor por capa de TraceShadow:
//          sombra y camino coherentes). alpha efectiva ~ 1 (marcos opacos
//          del panel) cae al kind 0 estandar.
//        - Normal maps (MATF_HAS_NRMTEX): tangente 888 mundo interpolada,
//          Gram-Schmidt contra ns volteada, bitangente cross(T, N), decode
//          del raster (xy*2-1, z reconstruida, Y invertida, alpha < 0.01 =
//          plano); si la normal perturbada cae bajo el hemisferio de ng se
//          conserva la interpolada.
//        - El canal emisivo del material map NO se lee (la CDF de NEE y
//          TraceShadow del host no lo conocen: mantenerlo apagado conserva
//          la coherencia; documentado en el kernel).
//
//   Constantes de host duplicadas de CinemaRender.cpp (angulo solar 2 grados
//   y CINEMA_SUN_IRRADIANCE_SCALE = 5): si CinemaRender las cambia o las
//   vuelve settings de proyecto, actualizar el oraculo en el mismo commit.
//
// El oraculo no despacha GPU ni depende de D3D: funciona en cualquier build,
// pero solo hay escena CPU valida tras un CinemaSceneBuild() correcto.
// ---------------------------------------------------------------------------

namespace CppProject
{
	// Asignacion de dimensiones del RNG por sample (contrato oraculo/kernel,
	// espejo del bloque de comentario del RNG en CinemaKernelM0.hpp M2).
	namespace CinemaOracleDims
	{
		// dim 0: .xy jitter subpixel del rayo de camara
		constexpr uint32_t DIM_CAMERA = 0;
		// dims por bounce: dim = DIM_BOUNCE_BASE + DIM_BOUNCE_STRIDE * bounce + k
		constexpr uint32_t DIM_BOUNCE_BASE = 1;
		constexpr uint32_t DIM_BOUNCE_STRIDE = 4; // M2 (M1 usaba 3)
		// k = 0: .xy disco solar, .zw hemisferio coseno (lobulo difuso)
		constexpr uint32_t DIM_SUN_BSDF = 0;
		// k = 1: .x pick de fuente (grupo + fuente, remapeado), .yz superficie
		constexpr uint32_t DIM_LIGHT = 1;
		// k = 2: .x ruleta rusa (solo bounce >= 3)
		constexpr uint32_t DIM_RR = 2;
		// k = 3 (M2): .x seleccion de lobo / Fresnel, .yz muestra VNDF
		constexpr uint32_t DIM_LOBE = 3;
	}

	// Resultado de CinemaOracleCompare.
	struct CinemaOracleCompareResult
	{
		RealType rmse = -1.0; // RMSE lineal sobre los 4 canales (RGB + ALPHA); -1 = no comparable
		BoolType ok = false;  // rmse valido y <= umbral
		// Peor pixel. El RMSE es una MEDIA y es CIEGO a una discrepancia que
		// solo toca unas decenas de pixeles (un detalle chico de una textura
		// puede diverger por completo y mover el rmse en la 4a decimal).
		// maxDiff es el gate que si la ve, y maxX/maxY dicen DONDE mirar.
		RealType maxDiff = 0.0;       // |gpu - cpu| maximo por canal
		IntType maxX = -1, maxY = -1; // pixel donde ocurre
		IntType maxC = -1;            // canal donde ocurre (0=r 1=g 2=b 3=alpha)
	};

	// Renderiza la escena CPU del ultimo CinemaSceneBuild() con el integrador
	// M1 escalar y deja la imagen en un buffer interno (consultable con
	// CinemaOracleLastImage y comparable con CinemaOracleCompare).
	//
	//   width/height  resolucion (pensada para 320x180 en tests)
	//   spp           samples por pixel (>= 1); acumulacion secuencial en float
	//   seed          semilla del frame (tile.w del kernel)
	//   bounces       max bounces, clamp a 1..8 (counts.z del kernel)
	//   clampIndirect clamp de contribuciones indirectas (<= 0 = off)
	//   emissiveScale escala de radiancia de emisivos (res_clamp.w del kernel)
	//   outRgba       opcional: si no es nullptr recibe width*height*4 floats
	//                 con la MISMA normalizacion que el resolve de produccion
	//                 (CinemaRenderFinish): rgb = suma_color / max(suma_alpha, 1)
	//                 (radiancia lineal DES-PREMULTIPLICADA, straight-alpha) y
	//                 a = suma_alpha / spp (cobertura media). Es tambien la
	//                 normalizacion que aplica el lado GPU del arnes.
	//
	// Fondo siempre opaco (el flag alpha-film no participa en la paridad M1:
	// el arnes GPU debe renderizar con alphaFilm = 0). En render OPACO la
	// cobertura es 1 en TODO el encuadre por INVARIANTE del kernel: el fondo
	// forma parte de la imagen, asi que no existe pixel translucido (ver la
	// linea "if (!alphaFilm) alpha = 1.0" del kernel y su espejo en el
	// oraculo). Hubo una epoca en que el kernel solo subia el alpha a 1 en el
	// MISS DEL REBOTE 0, y un primario que atravesaba una capa de cobertura o
	// un vidrio fino y se iba al cielo dejaba el pixel con cobertura parcial:
	// eso desincronizaba las dos normalizaciones (por suma de alphas y por
	// spp) y ademas INFLABA el brillo de esos pixeles por 1/E[alpha]. Ya no.
	// Aun asi la normalizacion documentada arriba sigue siendo la unica
	// valida: es la del resolve de produccion, que SI ve alphas parciales
	// cuando el film transparente esta activo.
	// Devuelve "" si OK o un codigo estable: "invalid_arg", "scene_not_built",
	// "camera_not_set".
	QString CinemaOracleRender(IntType width, IntType height, IntType spp,
		IntType seed, IntType bounces, RealType clampIndirect,
		RealType emissiveScale, float* outRgba);

	// Compara la imagen GPU (width*height*4 floats, YA normalizada como el
	// resolve de produccion: rgb des-premultiplicado en lineal, a = cobertura
	// media) contra el ultimo CinemaOracleRender a la misma resolucion.
	// RMSE sobre los CUATRO canales (RGB + ALPHA) de todos los pixeles: el
	// alpha entro en el gate porque un canal que no se compara es un canal
	// donde puede vivir cualquier bug.
	// ok = false si no hay render previo, la resolucion no casa o aparece
	// algun valor no finito.
	CinemaOracleCompareResult CinemaOracleCompare(const float* gpuRgba,
		IntType width, IntType height, RealType rmseThreshold);

	// Ultima imagen del oraculo (width*height*4 floats) o nullptr si no hay.
	// Util para volcados de debug del arnes.
	const float* CinemaOracleLastImage(IntType* outWidth = nullptr, IntType* outHeight = nullptr);

	// V3 (T6): closest-hit del RAYO DE CAMARA con la MISMA semantica que el
	// render. Envuelve TraceClosest(..., isCameraRay = true) -- la misma funcion
	// que usa el primario del oraculo y que es espejo 1:1 del kernel HLSL (gate
	// de paridad P1/P2/P3): holdout MATF_CAM_INVISIBLE con presupuesto,
	// backface culling, cutout ALPHA_TEST (texel.a < 0.5) y TEXEL FANTASMA
	// ALPHA_BLEND (aEff = texel.a * blend.a < 0.004: el FIX 3 de la ronda 2,
	// CinemaOracle.cpp lineas 674-687, espejo de CinemaKernelM0.hpp 1091-1096 y
	// 1255-1263). NO se copia ninguna de esas reglas a ningun otro sitio: si el
	// kernel cambia, esto cambia con el.
	//
	// Escena CPU: la del ultimo CinemaSceneBuild valido (verts MUNDO, bvh.tris en
	// orden GPU, materiales y slices del atlas). El caller DEBE haber resuelto
	// antes worldStale y el BVH diferido (la sonda ya lo hace, Cambio 15a.1).
	//
	// outTriIdx es indice en CinemaSceneGetBvh().tris -- EL MISMO que devuelve
	// CinemaBvhTrace, asi que el mapeo matId -> ranges -> draws del caller no
	// cambia. Devuelve false si no hay impacto en (tMin, tMax] o si no hay
	// escena CPU valida.
	BoolType CinemaOracleTraceCameraRay(const float org[3], const float dir[3],
		float tMin, float tMax, float* outT, IntType* outTriIdx,
		float* outBu, float* outBv);
}
