uniform sampler2D uTexture; // static
uniform sampler2D uTextureMaterial; // static
uniform sampler2D uTextureNormal; // static

uniform float uSampleIndex;
uniform int uAlphaHash;

uniform vec3 uCameraPosition; // static
uniform float uMetallic;
uniform float uRoughness;
uniform float uEmissive;
uniform int uIsSky;

// Materials
uniform int uMaterialFormat;
uniform float uDefaultEmissive;
uniform float uDefaultSubsurface;

// --- ShinyTrims (oleada 4, corregida en la 4b) -----------------------------
// El emisivo del trim NO cuelga del slider global "Block emissive"
// (uDefaultEmissive, shader_use.gml:57-59, UI en tab_properties_render.gml
// :428). Tiene escalar PROPIO, porque son dos cosas distintas: uno gradua la
// luz de bloque de un mundo importado y el otro gradua un efecto de armadura.
//
// LA CORRECCION DE LA OLEADA 4b (ADENDA 2 del PO). En la 4 el emisivo era
// cobertura x escalar: un SUELO fullbright CONSTANTE EN EL TIEMPO. Sobre ese
// suelo el termino aditivo del pase GLINT (que satura a 1.0 en un buffer LDR)
// dejaba de leerse y los 20 efectos se aplanaban: la amplitud temporal mediana
// caia de 5.90 a 2.55, charge no empezaba vacia, shimmer dejaba de ser tenue y
// nocturne ardia DE DIA (rompiendo el unico contrato explicito del pack).
// Ahora el emisivo por texel es
//
//     cobertura x GANANCIA DEL PROPIO EFECTO EN ESTE FRAME x intensidad x escala
//
// y la ganancia sale del MISMO catalogo que el termino aditivo del glint
// (tools/shinytrims/trim_effects.json, bloques generados de abajo). El color no
// se pierde al pasar a un escalar: el emisivo del G-buffer es una GANANCIA
// sobre diffuse (shader_high_lighting_apply.fsh:105-107 hace
// diffuse += unpackEmissive(...)), asi que el color lo pone el albedo del texel.
//
//   uTrimMask          POR SHAPE: 1 = el material map de ESTE draw lleva la
//                      MASCARA DEL TRIM en el alpha (0 = nada, 1 = trim).
//                      Se escribe SIEMPRE (render_world_tl por objeto y
//                      render_world_model_part por shape): el buffer POR
//                      OBJETO hereda del draw anterior.
//   uTrimEmissiveScale DE ESCENA: ganancia del emisivo del trim.
//   uTrimFx/uTrimIntensity/uTrimOrigin  POR OBJETO, ya los escriben
//                      render_world_tl:277-302 y render_world_model_part:272
//                      para todos los pases (render_set_uniform es un no-op
//                      silencioso donde el uniform no existe).
//   el resto          DE ESCENA, los sube shader_use una vez por pase.
//
// COSTE DECLARADO: uTrimFx, uTrimIntensity y uTrimOrigin son uniformes POR
// OBJETO en el pase PRINCIPAL, y cada uno reduce batchBufferMaxObjects
// (ShaderLoadD3D11.cpp:305-311) => algun draw call mas en TODA la escena, no
// solo en la armadura. Se acepta porque sin ellos la ganancia no puede
// evaluarse por texel y el emisivo vuelve a ser un suelo plano.
//
// Este shader no declara ningun ARRAY (uTrimPalette[8] es de la mascara M2 y
// vive solo en el glint), asi que el quirk de packing del cbuffer -- un escalar
// declarado DESPUES de un array acaba 4 bytes antes de donde lo pone HLSL,
// Shader.cpp AddUniform -- no aplica aqui. Aun asi el orden de estas
// declaraciones es el MISMO que el del bloque de shader_startup.gml.
uniform vec2 uTextureSize;
uniform float uTime; // static
uniform int uTrimFx;
uniform float uTrimIntensity;
uniform vec3 uTrimOrigin;
uniform float uTrimTick0; // static
uniform float uTrimLoopL; // static
uniform int uTrimSpace; // static
uniform float uTrimBodyScale; // static
uniform float uTrimRefH; // static
uniform vec2 uTrimFullSize; // static
uniform vec2 uTrimTileOrg; // static
uniform vec2 uTrimTileSize; // static
uniform float uTrimSceneLight; // static
uniform float uTrimNoiseSize; // static
uniform sampler2D uTrimNoise; // static
uniform int uTrimMask;
uniform float uTrimEmissiveScale; // static
uniform int uTrimOutline;

varying vec3 vPosition;
varying vec3 vNormal;
varying vec3 vTangent;
varying vec4 vColor;
varying vec2 vTexCoord;
varying vec4 vCustom;
varying mat3 vTBN;
// ShinyTrims (oleada 4b): posicion de CLIP interpolada, para el espacio
// PANTALLA de la ganancia. Mismo motivo, palabra por palabra, que en
// shader_high_glint.vsh: gl_FragCoord no lo traduce el backend D3D11 y su
// origen Y esta invertido respecto a GLSL. Sin esto la ganancia emisiva se
// quedaria SIEMPRE en espacio CUERPO y con uTrimSpace = 1 (el modo "fiel a
// Minecraft", que no es el defecto) el brillo y el color del efecto dejarian de
// caer en los mismos pixeles. Coste: un interpolador mas en el pase principal.
varying vec4 vClip;

// Fresnel Schlick approximation
float fresnelSchlickRoughness(float cosTheta, float F0, float roughness)
{
	return F0 + (max((1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

uniform int uUseNormalMap; // static
vec3 getMappedNormal(vec2 uv)
{
	if (uUseNormalMap < 1)
		return vec3(vTBN[2][0], vTBN[2][1], vTBN[2][2]);
	
	vec4 n = texture2D(uTextureNormal, uv).rgba;
	n.rgba = (n.a < 0.01 ? vec4(.5, .5, 0.0, 1.0) : n.rgba); // No normal?
	n.xy = n.xy * 2.0 - 1.0; // Decode
	n.z = sqrt(max(0.0, 1.0 - dot(n.xy, n.xy))); // Get Z
	n.y *= -1.0; // Convert Y- to Y+
	return normalize(vTBN * n.xyz);
}

float hash(vec2 c)
{
	return fract(10000.0 * sin(17.0 * c.x + 0.1 * c.y) *
	(0.1 + abs(sin(13.0 * c.y + c.x))));
}

vec3 packEmissive(float f)
{
	return vec3(floor(f * 255.0) / 255.0, fract(f * 255.0), fract(f * 255.0 * 255.0));
}

// >>> BEGIN GENERATED trim runtime
// NO EDITAR A MANO. Copia LITERAL del bloque SHARED de
// GmProject/shaders/shader_high_glint/shader_high_glint.fsh (MI3 no tiene
// #include: la unica alternativa era copiar-pegar a mano y esperar que los
// dos shaders no se separaran). El generador lo copia y --check falla si
// difieren en un solo byte.
// Regenerar: py tools/shinytrims/gen_trim_effects.py
// --- Contrato de tiempo (oleada 2) --------------------------------------
// ticks = tick0 + (timeline_marker / project_tempo) * 20, y uTime ya vale
// (timeline_marker / project_tempo) * 60, asi que ticks = tick0 + uTime/3.
// Funcion pura del frame: ni un reloj de pared.
// NOTA: los nombres NO llevan guion bajo. El traductor GLSL->HLSL busca
// funciones con ^([a-zA-Z0-9]+?) ([a-zA-Z0-9]+?)\( y un "_" rompe el match
// (0 de 131 shaders del repo declara una funcion con guion bajo).
float stTicks()
{
	return uTrimTick0 + (uTime / 3.0);
}

// LOOP LOCK: la escena declara L (uTrimLoopL) y cada n se cuantiza a un
// multiplo de k = 24000/L, de modo que el efecto cierra exactamente en L ticks.
// floor(x + 0.5) en vez de round(): round() no existe en GLSL 1.20.
float stQn(float n)
{
	if (uTrimLoopL <= 0.0)
		return n;

	float k = 24000.0 / uTrimLoopL;
	return max(k, floor((n / k) + 0.5) * k);
}

// Fase en [0,1) del ritmo n (n ciclos por dia de Minecraft = 24000 ticks).
// En modo FIEL replica el pack: fract(GameTime * n) con GameTime = fract(t/24000).
// En LOOP LOCK se reduce el tiempo a mod(t, L) ANTES de multiplicar: asi
// t = 0 y t = L dan bit a bit el mismo valor (con la forma fract(fract(t/24000)*n)
// el redondeo de fp32 deja un residuo ~1e-7 y el loop no cierra byte a byte).
float stPhase(float n)
{
	float nn = stQn(n);
	float t = stTicks();

	if (uTrimLoopL <= 0.0)
		return fract(fract(t / 24000.0) * nn);

	float tl = mod(t, uTrimLoopL);
	return fract((tl * nn) / 24000.0);
}

float stCycle(float n)
{
	return stPhase(n);
}

float stWave(float n)
{
	return sin(stPhase(n) * 6.2831853);
}

float stWavep(float n, float ph)
{
	return sin(fract(stPhase(n) + ph) * 6.2831853);
}

float stLum(vec3 c)
{
	return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 stHsv(float h, float s, float v)
{
	vec3 p = abs(fract(vec3(h) + vec3(0.0, 0.6666667, 0.3333333)) * 6.0 - 3.0);
	return v * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), s);
}

// --- stHash: ruido determinista (oleada 3) --------------------------------
// El st_hash del pack es fract(sin(dot(p, vec2(12.9898,78.233))) * 43758.5453):
// con p = indice de texel, el sin() recibe cientos de miles de radianes, muy
// por debajo de la precision util de fp32, y el resultado cambia entre drivers.
// Aqui se lee una textura de ruido POINT-SAMPLED de 128x128 (spr_blue_noise
// copiado a una surface propia, medido: 256 valores distintos por canal, media
// exacta 127.5, R/G/B decorrelados). Es un asset commiteado -> identico en toda
// maquina. El patron concreto NO es el de Minecraft; la estadistica si.
// Se usa una SURFACE y no el sprite: las texturas de sprite se crean con la
// cadena de mips completa (Texture.cpp:23 MipLevels = 0 + GENERATE_MIPS) y
// _sampleUvRect muestrea con SampleGrad, asi que las derivadas salvajes de un
// lookup de ruido elegirian un mip alto y lavarian el patron; los FrameBuffer
// de surface son MipLevels = 1 (FrameBuffer.cpp:58) -> siempre LOD 0.
float stHash(vec2 c)
{
	return texture2D(uTrimNoise, (floor(c) + 0.5) / uTrimNoiseSize).r;
}

// --- Espacio del patron (oleada 3) ---------------------------------------
// Los 13 efectos de pantalla del pack estan escritos en PIXELES. Este port
// mantiene sus constantes INTACTAS y cambia solo de donde sale "p".
//
//   CUERPO (POR DEFECTO -- decision (f) del PO):
//     p = (mundo - origen del set) * uTrimBodyScale, proyectado sobre el plano
//     XZ (Z es la vertical en MI3, ver shader_color_fog.fsh:53). El patron
//     queda clavado al cuerpo: no depende de la camara, ni de la resolucion,
//     ni de los tiles, y el loop cierra aunque la camara se mueva.
//     LIMITACION DECLARADA: es una proyeccion PLANA a lo largo del eje Y del
//     mundo. Una camara que mire a lo largo de X ve colapsar la variacion
//     horizontal, y un personaje que GIRE ve deslizarse el patron. Es la
//     version debil del "shower door" de Minecraft.
//     OJO: in_Position (espacio local) NO sirve para esto -- es local a CADA
//     CUBO (render_world_model_part.gml:299-302 da a cada shape su propia
//     matriz), asi que un barrido en local saltaria del casco al brazo. Lo
//     unico continuo entre las 4 piezas es vPosition (mundo) menos un origen
//     COMPARTIDO por las 7 partes del set.
//
//   PANTALLA ("fiel a Minecraft"):
//     p = pixeles del LIENZO COMPLETO renormalizados a la altura de referencia
//     uTrimRefH (1080). Con eso el look NO cambia entre 1080p, 4K, SSAA,
//     overscan ni export tileado (decision (b) del PO).
//     No se usa gl_FragCoord: el traductor GLSL->HLSL NO lo sustituye (solo
//     mapea gl_FragColor y gl_FragData[N], ShaderLoadD3D11.cpp:131-140) y
//     ademas el origen Y de SV_POSITION es el contrario del de GLSL, asi que
//     copiarlo literal invertiria el barrido en uno de los dos backends. Con
//     el clip interpolado (vClip) la formula es la MISMA en los dos.
//
// ST_Y_UP: la Y del pack CRECE HACIA ARRIBA (convencion de gl_FragCoord en
// GLSL). MEDIDO con la sonda P-Y de la oleada 3 (efecto de depuracion slot 199,
// add = vec3(p.y / uTrimRefH), render con uTrimSpace = 1): el resultado esta en
// evidencia_o34\gates.log. La NDC de MI3 tiene +y ARRIBA (render_set_projection
// construye la proyeccion con la Y negada y matrix_set la vuelve a invertir),
// asi que t01.y ya vale 1 arriba y 0 abajo -- que es exactamente la convencion
// del pack. Por eso NO se voltea aqui.
vec2 stScreen()
{
	vec2 ndc = vClip.xy / vClip.w;
	vec2 t01 = ndc * 0.5 + 0.5;
	vec2 px = uTrimTileOrg + t01 * uTrimTileSize;
	return px * (uTrimRefH / uTrimFullSize.y);
}

vec2 stBody()
{
	vec2 b = vec2(vPosition.x - uTrimOrigin.x, vPosition.z - uTrimOrigin.z);
	return b * uTrimBodyScale;
}
// <<< END GENERATED trim runtime

// >>> BEGIN GENERATED trim space defaults
// NO EDITAR A MANO. Fuente: tools/shinytrims/trim_effects.json
// Regenerar: py tools/shinytrims/gen_trim_effects.py
// Espacio POR DEFECTO de cada efecto, cuando la escena no fuerza uno
// (uTrimSpace < 0 = AUTO). Devuelve 1.0 = PANTALLA, 0.0 = CUERPO.
// La decision (f) del PO fija CUERPO como default y autoriza
// EXCEPCIONES POR EFECTO con evidencia; el motivo de cada excepcion
// esta en el campo space_note del catalogo.
float stFxScreen()
{
	return 0.0;
}
// <<< END GENERATED trim space defaults

// >>> BEGIN GENERATED trim emissive gain
// NO EDITAR A MANO. Fuente: tools/shinytrims/trim_effects.json
// Regenerar: py tools/shinytrims/gen_trim_effects.py
// Correccion de calibracion POR EFECTO sobre la ganancia emisiva
// (campo emissive_gain del catalogo). 1.0 = el efecto se calibra
// solo con el escalar global uTrimEmissiveScale y no aparece aqui.
// NO entra en el contrato gain == stLum(add|base=1): es una
// correccion de look. REGLA (oleada 4b): emissive_gain = 1/p90(gain)
// para los efectos con p90 > 1.0 (oleada 8, decision 2 del PO: la banda
// muerta era 1.25 sin medida detras), o sea 'ninguno mete por el canal
// emisivo, DE FORMA HABITUAL, mas ganancia que glowing'. Los p90 de
// abajo los recalcula el generador desde el catalogo en cada corrida,
// asi que --check falla si un valor deja de casar con su regla.
float stFxEmissiveGain()
{
	// aurora: ganancia min 0.000 / p90 0.816 / max 1.100, regla=chroma  -- ADENDA 3 #2 (O5c), croma = media de max(rgb)-min(rgb) sobre el conjunto del trim en el marker 0, misma escena y mismo conjunto que measure_o4b.py caracter. Su color vive en el VELO stHsv del canal aditivo; el emisivo es ganancia sobre el albedo y lo lavaba: croma 25.58 frente a 33.36 del TECHO aditivo, un 77%. p90 no pedia correccion (p90 <= 1.25). Con 0.15 el croma es 30.06 = 90.1% del techo
	if (uTrimFx == 251)
		return 0.15;
	// bodysweep: ganancia min 0.350 / p90 1.810 / max 1.950, regla=p90  -- barra de 250 px de referencia sobre un piso de 0.35; el pico de la barra (p90 1.810) doblaba fullbright
	if (uTrimFx == 250)
		return 0.553;
	// charge: ganancia min 0.000 / p90 1.849 / max 2.947, regla=p90  -- el fogonazo del ciclo (max 2.947) es intencionado, pero su p90 1.849 quemaba el ciclo entero
	if (uTrimFx == 248)
		return 0.541;
	// chrome: ganancia min 0.950 / p90 1.886 / max 2.573, regla=burn  -- O5c, medido con measure_o4b.py caracter (markers no armonicos 0/6/13/29/47/71, quemados = % del conjunto del trim con max(rgb)>250 en el marker 0). PISO ALTO por diseno (nunca baja de 0.95): p90 pedia 0.530 y con el quemaba el 48.7%. Con 0.106 quema 33.6%, +9.4 pp sobre el 24.2% de su TECHO aditivo; ademas recupera parte de su (pequena) variacion espacial, que la parte plana de la curva de tono le comia (amplitud 3.94 -> 4.38, techo 4.64)
	if (uTrimFx == 247)
		return 0.106;
	// kaleido: ganancia min 0.540 / p90 1.121 / max 1.233, regla=burn  -- Oleada 8, decisiones 2 y 3 del PO, en ese orden y las dos publicadas. (1) DECISION 2: kaleido NO es fullbright por contrato del pack (glowing si lo es, y por eso glowing esta exento); su p90 es 1.121, o sea de forma HABITUAL mete un 12% mas ganancia emisiva que fullbright, y con la banda muerta de la regla en 1.25 se colaba con emissive_gain = 1.0. Aplicada la regla tal cual, 1/1.121 = 0.892, y MEDIDO con ella: 31.8% de quemados contra 5.6% de su techo aditivo = +26.3 pp. (2) DECISION 3: el criterio (e) del gate de caracter pone el limite en 15 pp, asi que 1/p90 NO basta y kaleido pasa a la regla burn, la misma que O5c dio a chrome/lava_lamp/nebula/nocturne/oilslick/silk por el mismo motivo. Barrido por escena (trim_emissive escala el mismo producto que emissive_gain: emissive = matColor.a * stTrimGain() * uTrimIntensity * uTrimEmissiveScale), 6 puntos, equivalencias en emissive_gain: 0.624 -> 23.8% (+18.3 pp), 0.491 -> 19.7% (+14.1 pp), 0.357 -> 15.8% (+10.2 pp), 0.268 -> 13.4% (+7.9 pp), 0.178 -> 11.5% (+6.0 pp), 0.089 -> 9.2% (+3.7 pp). Elegido 0.357: deja los quemados en la familia de las otras seis excepciones burn (+0.5 a +9.4 pp) en vez de rozar el limite, y el color de kaleido vive en el canal ADITIVO, que esto no toca. Todas las cifras: measure_o4b.py caracter, markers no armonicos 0/6/13/29/47/71, quemados = % del conjunto del trim (15114 px) con max(rgb) > 250 en el marker 0.
	if (uTrimFx == 246)
		return 0.357;
	// lava_lamp: ganancia min 0.950 / p90 1.525 / max 1.525, regla=burn  -- O5c, medido con measure_o4b.py caracter (markers no armonicos 0/6/13/29/47/71, quemados = % del conjunto del trim con max(rgb)>250 en el marker 0). p90 pedia 0.656 (piso 0.95 y blobs calidos a 1.5x) y con el quemaba el 62.9%. Su canal aditivo ya quema el 35.6% el solo, asi que el margen es estrecho: con 0.052 quema 40.6% (+5.0 pp sobre su TECHO); en el barrido de calibracion el escalon siguiente (0.079) ya daba 46.3%
	if (uTrimFx == 245)
		return 0.052;
	// nebula: ganancia min 0.822 / p90 1.519 / max 1.811, regla=burn  -- O5c, medido con measure_o4b.py caracter (markers no armonicos 0/6/13/29/47/71, quemados = % del conjunto del trim con max(rgb)>250 en el marker 0). p90 pedia 0.658 (nubes de interferencia sobre un piso de 0.822) y con el quemaba el 47.3%. Con 0.165 quema 28.1%, +9.0 pp sobre el 19.1% de su TECHO aditivo
	if (uTrimFx == 243)
		return 0.165;
	// nocturne: ganancia min 0.000 / p90 1.266 / max 1.350, regla=burn  -- O5c, medido con measure_o4b.py caracter (markers no armonicos 0/6/13/29/47/71, quemados = % del conjunto del trim con max(rgb)>250 en el marker 0). p90 pedia 0.790 y con el nocturne quemaba el 69.4%, EL MISMO % que glowing (fullbright por contrato). Con 0.158 quema 35.0%, o sea +0.5 pp sobre el 34.5% que ya mete su propio canal aditivo (TECHO). Cierra ademas el unico fallo del gate de caracter: perdia 4.40 luma de amplitud con limite 2.00, ahora 1.01. De dia sigue siendo 0 EXACTO (el factor dark multiplica ANTES)
	if (uTrimFx == 242)
		return 0.158;
	// oilslick: ganancia min 0.878 / p90 1.346 / max 1.405, regla=burn  -- O5c, medido con measure_o4b.py caracter (markers no armonicos 0/6/13/29/47/71, quemados = % del conjunto del trim con max(rgb)>250 en el marker 0). p90 pedia 0.743 (pelicula fina siempre viva, piso 0.878) y con el quemaba el 55.1%. Con 0.297 quema 38.5%, +5.3 pp sobre el 33.2% de su TECHO aditivo
	if (uTrimFx == 241)
		return 0.297;
	// rainbow_flow: ganancia min 0.336 / p90 0.819 / max 0.911, regla=chroma  -- ADENDA 3 #2 (O5c), croma = media de max(rgb)-min(rgb) sobre el conjunto del trim en el marker 0, misma escena y mismo conjunto que measure_o4b.py caracter. Su color vive en el ARCOIRIS del canal aditivo; el emisivo es ganancia sobre el albedo (oro) y lo lavaba: croma 76.23 frente a 90.39 del TECHO aditivo, un 84%. p90 no pedia correccion (p90 <= 1.25). Con 0.35 el croma es 84.07 = 93.0% del techo, y de paso los quemados bajan de 25.6% a 10.6%
	if (uTrimFx == 238)
		return 0.35;
	// silk: ganancia min 0.945 / p90 1.502 / max 1.665, regla=burn  -- O5c, medido con measure_o4b.py caracter (markers no armonicos 0/6/13/29/47/71, quemados = % del conjunto del trim con max(rgb)>250 en el marker 0). p90 pedia 0.666 (pliegues perlados sobre un piso de 0.945) y con el quemaba el 50.9%. Con 0.133 quema 27.7%, +6.8 pp sobre el 20.9% de su TECHO aditivo
	if (uTrimFx == 237)
		return 0.133;
	// spark: ganancia min 0.495 / p90 2.471 / max 4.110, regla=p90  -- el mas TENUE de los tres de calibracion en media, pero sus glints llegan a 4.11x fullbright: el p90 2.471 es el que manda
	if (uTrimFx == 236)
		return 0.405;

	return 1.0;
}
// <<< END GENERATED trim emissive gain

// --- ShinyTrims (oleada 4b): la ganancia emisiva del efecto -----------------
// Devuelve la LUMINANCIA DEL TERMINO ADITIVO POR UNIDAD DE COLOR del texel, o
// sea la envolvente del efecto en este frame y en este texel, sin color. El
// contrato exacto, que tools/shinytrims/gen_trim_effects.py comprueba
// NUMERICAMENTE sobre 244 muestras por efecto (validacion 12), es
//
//     gain == stLum(add evaluado con base = vec3(1.0))
//
// Consecuencias que son gates: nocturne da 0 EXACTO de dia en los DOS canales
// (su ganancia lleva el factor dark), y charge empieza el ciclo con fill = 0,
// o sea sin un solo texel encendido.
float stTrimGain()
{
	if (uTrimFx < 1)
		return 0.0;

	// Mismo espacio que el pase GLINT, byte a byte: si el patron de color y el
	// patron de brillo no cayeran en los mismos texeles, el efecto se veria
	// doble. uTrimSpace: -1 AUTO (el default del catalogo por efecto), 0 fuerza
	// CUERPO, 1 fuerza PANTALLA.
	float scr = stFxScreen();
	if (uTrimSpace == 0)
		scr = 0.0;
	else if (uTrimSpace > 0)
		scr = 1.0;

	vec2 p = mix(stBody(), stScreen(), scr);
	vec2 tx = floor(vTexCoord * uTextureSize);
	vec2 uv = vTexCoord;
	float gain = 0.0;

	// >>> BEGIN GENERATED trim gain
		// NO EDITAR A MANO. Fuente: tools/shinytrims/trim_effects.json
		// Regenerar: py tools/shinytrims/gen_trim_effects.py
	// --- glowing (slot 254, reagent glow_ink_sac, espacio none)
	// add: add = base;
	if (uTrimFx == 254)
	{
		gain = 1.0;
	}
	// --- periodic (slot 253, reagent echo_shard, espacio time)
	// add: float pulse = 0.5 + 0.5 * stWave(1273.0);
	// add: add = base * (pulse * pulse);
	else if (uTrimFx == 253)
	{
		float pulse = 0.5 + 0.5 * stWave(1273.0);
		gain = pulse * pulse;
	}
	// --- shimmer (slot 252, reagent glowstone_dust, espacio plane)
	// add: float s = 0.5 * stWavep(318.0, p.y * 0.01) + 0.5 * stWavep(796.0, p.x * 0.007);
	// add: add = base * (0.3 * smoothstep(-0.2, 0.8, s));
	else if (uTrimFx == 252)
	{
		float s = 0.5 * stWavep(318.0, p.y * 0.01) + 0.5 * stWavep(796.0, p.x * 0.007);
		gain = 0.3 * smoothstep(-0.2, 0.8, s);
	}
	// --- aurora (slot 251, reagent heart_of_the_sea, espacio plane)
	// add: float curtain = 0.5 + 0.5 * sin(p.x * 0.045 + 3.0 * stWave(90.0) + 2.0 * stWavep(140.0, p.y * 0.0012));
	// add: float band = curtain * curtain * curtain;
	// add: float huedrift = 0.5 + 0.5 * stWavep(70.0, p.x * 0.001);
	// add: vec3 veil = stHsv(mix(0.34, 0.76, huedrift), 0.6, 1.0) * stLum(base) * 1.6;
	// add: add = veil * (band * 0.85);
	else if (uTrimFx == 251)
	{
		float curtain = 0.5 + 0.5 * sin(p.x * 0.045 + 3.0 * stWave(90.0) + 2.0 * stWavep(140.0, p.y * 0.0012));
		float band = curtain * curtain * curtain;
		float huedrift = 0.5 + 0.5 * stWavep(70.0, p.x * 0.001);
		gain = stLum(stHsv(mix(0.34, 0.76, huedrift), 0.6, 1.0)) * 1.6 * (band * 0.85);
	}
	// --- bodysweep (slot 250, reagent breeze_rod, espacio plane)
	// add: float band = fract(stCycle(220.0) + p.y * 0.004);
	// add: float bar = smoothstep(0.0, 0.12, band) * (1.0 - smoothstep(0.12, 0.35, band));
	// add: add = base * 0.35 + base * bar * 1.6;
	else if (uTrimFx == 250)
	{
		float band = fract(stCycle(220.0) + p.y * 0.004);
		float bar = smoothstep(0.0, 0.12, band) * (1.0 - smoothstep(0.12, 0.35, band));
		gain = 0.35 + bar * 1.6;
	}
	// --- breath (slot 249, reagent ghast_tear, espacio time)
	// add: float b = 0.5 + 0.5 * stWave(300.0);
	// add: float inten = 0.25 + 0.85 * b * b;
	// add: add = base * inten * 0.6;
	else if (uTrimFx == 249)
	{
		float b = 0.5 + 0.5 * stWave(300.0);
		float inten = 0.25 + 0.85 * b * b;
		gain = inten * 0.6;
	}
	// --- charge (slot 248, reagent lightning_rod, espacio texel)
	// add: float t = stCycle(140.0);
	// add: float fill = clamp((t - 0.05) / 0.50, 0.0, 1.0) - clamp((t - 0.64) / 0.31, 0.0, 1.0);
	// add: float lit = step(stHash(tx), fill);
	// add: float flash = smoothstep(0.55, 0.60, t) * (1.0 - smoothstep(0.60, 0.72, t));
	// add: add = base * (0.85 + 0.5 * fill) * lit + base * flash * 1.6;
	else if (uTrimFx == 248)
	{
		float t = stCycle(140.0);
		float fill = clamp((t - 0.05) / 0.50, 0.0, 1.0) - clamp((t - 0.64) / 0.31, 0.0, 1.0);
		float lit = step(stHash(tx), fill);
		float flash = smoothstep(0.55, 0.60, t) * (1.0 - smoothstep(0.60, 0.72, t));
		gain = (0.85 + 0.5 * fill) * lit + flash * 1.6;
	}
	// --- chrome (slot 247, reagent heavy_core, espacio plane)
	// add: float band1 = 0.5 + 0.5 * sin(p.x * 0.05 + p.y * 0.02 + 4.0 * stWave(120.0));
	// add: float band2 = 0.5 + 0.5 * sin(p.x * 0.013 - p.y * 0.04 + 3.0 * stWave(90.0));
	// add: float spec = band1 * band1 * band2;
	// add: add = base * (0.95 + 0.9 * spec) + vec3(1.0) * stLum(base) * spec * spec * 0.8;
	else if (uTrimFx == 247)
	{
		float band1 = 0.5 + 0.5 * sin(p.x * 0.05 + p.y * 0.02 + 4.0 * stWave(120.0));
		float band2 = 0.5 + 0.5 * sin(p.x * 0.013 - p.y * 0.04 + 3.0 * stWave(90.0));
		float spec = band1 * band1 * band2;
		gain = (0.95 + 0.9 * spec) + spec * spec * 0.8;
	}
	// --- kaleido (slot 246, reagent firework_star, espacio plane)
	// add: vec2 cell = floor(p / 7.0);
	// add: float idc = stHash(cell);
	// add: vec3 jewel = stHsv(fract(idc + stCycle(60.0)), 0.65, 1.0);
	// add: float tw = 0.8 + 0.2 * stWavep(240.0, idc);
	// add: add = mix(base, jewel * stLum(base) * 1.7, 0.5) * tw;
	else if (uTrimFx == 246)
	{
		vec2 cell = floor(p / 7.0);
		float idc = stHash(cell);
		vec3 jewel = stHsv(fract(idc + stCycle(60.0)), 0.65, 1.0);
		float tw = 0.8 + 0.2 * stWavep(240.0, idc);
		gain = mix(1.0, stLum(jewel) * 1.7, 0.5) * tw;
	}
	// --- lava_lamp (slot 245, reagent fire_charge, espacio plane)
	// add: vec2 q = p * 0.012;
	// add: float blob = 0.5 + 0.5 * sin(q.y * 2.2 - 6.2831 * stCycle(70.0) + 1.7 * sin(q.x * 2.9 + 2.0 * stWave(50.0)));
	// add: float body = smoothstep(0.35, 0.9, blob);
	// add: vec3 warm = base * vec3(1.25, 0.95, 0.75);
	// add: add = mix(base * 0.95, warm * 1.5, body);
	else if (uTrimFx == 245)
	{
		vec2 q = p * 0.012;
		float blob = 0.5 + 0.5 * sin(q.y * 2.2 - 6.2831 * stCycle(70.0) + 1.7 * sin(q.x * 2.9 + 2.0 * stWave(50.0)));
		float body = smoothstep(0.35, 0.9, blob);
		gain = mix(0.95, stLum(vec3(1.25, 0.95, 0.75)) * 1.5, body);
	}
	// --- molten (slot 244, reagent blaze_powder, espacio texel)
	// add: float col = floor(uv.x * uTextureSize.x);
	// add: float drop = fract(uv.y * 0.04 - stCycle(140.0) - stHash(vec2(col, 1.0)));
	// add: float tail = smoothstep(0.35, 0.95, drop);
	// add: float head = smoothstep(0.88, 0.97, drop) * (1.0 - smoothstep(0.97, 1.0, drop));
	// add: vec3 hot = base * vec3(1.35, 1.0, 0.7);
	// add: add = hot * (0.25 * tail + 1.4 * head);
	else if (uTrimFx == 244)
	{
		float col = floor(uv.x * uTextureSize.x);
		float drop = fract(uv.y * 0.04 - stCycle(140.0) - stHash(vec2(col, 1.0)));
		float tail = smoothstep(0.35, 0.95, drop);
		float head = smoothstep(0.88, 0.97, drop) * (1.0 - smoothstep(0.97, 1.0, drop));
		gain = stLum(vec3(1.35, 1.0, 0.7)) * (0.25 * tail + 1.4 * head);
	}
	// --- nebula (slot 243, reagent ender_eye, espacio plane)
	// add: vec2 q = p * 0.015;
	// add: float f1 = 0.5 + 0.5 * stWavep(300.0, q.x * 0.3 + q.y * 0.17);
	// add: float f2 = 0.5 + 0.5 * stWavep(430.0, q.y * 0.23 - q.x * 0.11);
	// add: float cloud = f1 * 0.6 + f2 * 0.4;
	// add: vec3 hue = stHsv(fract(0.72 + 0.1 * f1 + stCycle(50.0)), 0.55, 1.0);
	// add: vec3 neb = mix(base, hue * stLum(base) * 1.7, 0.45 + 0.3 * cloud);
	// add: add = neb * (0.85 + 0.45 * cloud);
	else if (uTrimFx == 243)
	{
		vec2 q = p * 0.015;
		float f1 = 0.5 + 0.5 * stWavep(300.0, q.x * 0.3 + q.y * 0.17);
		float f2 = 0.5 + 0.5 * stWavep(430.0, q.y * 0.23 - q.x * 0.11);
		float cloud = f1 * 0.6 + f2 * 0.4;
		vec3 hue = stHsv(fract(0.72 + 0.1 * f1 + stCycle(50.0)), 0.55, 1.0);
		gain = mix(1.0, stLum(hue) * 1.7, 0.45 + 0.3 * cloud) * (0.85 + 0.45 * cloud);
	}
	// --- nocturne (slot 242, reagent phantom_membrane, espacio time)
	// add: float dark = 1.0 - smoothstep(0.25, 0.85, uTrimSceneLight);
	// add: add = base * 1.35 * (0.9 + 0.1 * stWave(600.0)) * dark;
	else if (uTrimFx == 242)
	{
		float dark = 1.0 - smoothstep(0.25, 0.85, uTrimSceneLight);
		gain = 1.35 * (0.9 + 0.1 * stWave(600.0)) * dark;
	}
	// --- oilslick (slot 241, reagent prismarine_shard, espacio plane)
	// add: float film = sin(p.x * 0.03 + 2.0 * stWave(80.0)) + sin(p.y * 0.037 - 2.0 * stWave(110.0)) + sin((p.x + p.y) * 0.021 + 3.0 * stWave(60.0));
	// add: vec3 slick = stHsv(fract(film * 0.16 + stCycle(40.0)), 0.7, 1.0);
	// add: float lum = stLum(base);
	// add: add = mix(base * 1.1, slick * lum * 1.8, 0.55);
	else if (uTrimFx == 241)
	{
		float film = sin(p.x * 0.03 + 2.0 * stWave(80.0)) + sin(p.y * 0.037 - 2.0 * stWave(110.0)) + sin((p.x + p.y) * 0.021 + 3.0 * stWave(60.0));
		vec3 slick = stHsv(fract(film * 0.16 + stCycle(40.0)), 0.7, 1.0);
		gain = mix(1.1, stLum(slick) * 1.8, 0.55);
	}
	// --- opal (slot 240, reagent crying_obsidian, espacio plane)
	// add: float band = p.x * 0.008 + p.y * 0.011;
	// add: vec3 sheen = stHsv(fract(band * 0.25 + stCycle(60.0)), 0.35, 1.0);
	// add: float lum = stLum(base);
	// add: float glide = 0.45 + 0.25 * stWavep(90.0, band);
	// add: add = sheen * lum * 1.5 * (glide * 0.6);
	else if (uTrimFx == 240)
	{
		float band = p.x * 0.008 + p.y * 0.011;
		vec3 sheen = stHsv(fract(band * 0.25 + stCycle(60.0)), 0.35, 1.0);
		float glide = 0.45 + 0.25 * stWavep(90.0, band);
		gain = stLum(sheen) * 1.5 * (glide * 0.6);
	}
	// --- plasma (slot 239, reagent magma_cream, espacio plane)
	// add: vec2 q = p * 0.02;
	// add: float w = stWavep(700.0, q.x * 0.35) + stWavep(1100.0, q.y * 0.27 + q.x * 0.11);
	// add: float m2 = 0.5 + 0.25 * w;
	// add: vec3 shift = stHsv(fract(0.55 + 0.12 * w + stCycle(80.0)), 0.45, 1.0);
	// add: add = base * shift * (0.7 + 0.8 * m2) * 0.7;
	else if (uTrimFx == 239)
	{
		vec2 q = p * 0.02;
		float w = stWavep(700.0, q.x * 0.35) + stWavep(1100.0, q.y * 0.27 + q.x * 0.11);
		float m2 = 0.5 + 0.25 * w;
		vec3 shift = stHsv(fract(0.55 + 0.12 * w + stCycle(80.0)), 0.45, 1.0);
		gain = stLum(shift) * (0.7 + 0.8 * m2) * 0.7;
	}
	// --- rainbow_flow (slot 238, reagent prismarine_crystals, espacio plane)
	// add: float h = fract(stCycle(160.0) + (p.x + p.y) * 0.0022);
	// add: vec3 tint = stHsv(h, 0.75, 1.0) * (0.35 + 0.9 * stLum(base));
	// add: add = tint * 0.8;
	else if (uTrimFx == 238)
	{
		float h = fract(stCycle(160.0) + (p.x + p.y) * 0.0022);
		gain = stLum(stHsv(h, 0.75, 1.0)) * 1.25 * 0.8;
	}
	// --- silk (slot 237, reagent string, espacio plane)
	// add: float fold = 0.5 + 0.5 * sin(p.y * 0.025 + 3.0 * sin(p.x * 0.008 + 2.0 * stWave(70.0)) + 6.2831 * stCycle(100.0));
	// add: vec3 pearl = stHsv(fract(stCycle(45.0) + 0.5), 0.4, 1.0) * stLum(base) * 1.6;
	// add: add = mix(base * 1.05, pearl, fold * 0.6) * (0.9 + 0.35 * fold);
	else if (uTrimFx == 237)
	{
		float fold = 0.5 + 0.5 * sin(p.y * 0.025 + 3.0 * sin(p.x * 0.008 + 2.0 * stWave(70.0)) + 6.2831 * stCycle(100.0));
		float pearl = stLum(stHsv(fract(stCycle(45.0) + 0.5), 0.4, 1.0)) * 1.6;
		gain = mix(1.05, pearl, fold * 0.6) * (0.9 + 0.35 * fold);
	}
	// --- spark (slot 236, reagent amethyst_shard, espacio texel)
	// add: float ph = stHash(tx);
	// add: float tw = smoothstep(0.55, 1.0, 0.5 + 0.5 * stWavep(600.0, ph));
	// add: float gl = smoothstep(0.97, 1.0, stHash(tx + vec2(floor(stCycle(48.0) * 8.0), 3.0)));
	// add: add = base * (0.9 + 1.6 * tw) * (0.55 + 0.45 * tw) + base * gl * 1.8;
	else if (uTrimFx == 236)
	{
		float ph = stHash(tx);
		float tw = smoothstep(0.55, 1.0, 0.5 + 0.5 * stWavep(600.0, ph));
		float gl = smoothstep(0.97, 1.0, stHash(tx + vec2(floor(stCycle(48.0) * 8.0), 3.0)));
		gain = (0.9 + 1.6 * tw) * (0.55 + 0.45 * tw) + gl * 1.8;
	}
	// --- tide (slot 235, reagent nautilus_shell, espacio plane)
	// add: float ph2 = fract(stCycle(180.0) + p.y * 0.003 + 0.15 * sin(p.x * 0.02));
	// add: float swell = smoothstep(0.0, 0.45, ph2) * (1.0 - smoothstep(0.45, 0.8, ph2));
	// add: float crest = smoothstep(0.42, 0.48, ph2) * (1.0 - smoothstep(0.48, 0.54, ph2));
	// add: float foam = crest * step(0.5, stHash(floor(p * 0.5) + floor(stCycle(1200.0) * 4.0)));
	// add: vec3 sea = base * vec3(0.8, 1.05, 1.25);
	// add: add = sea * swell * 0.5 + vec3(1.0) * stLum(base) * foam * 1.2;
	else if (uTrimFx == 235)
	{
		float ph2 = fract(stCycle(180.0) + p.y * 0.003 + 0.15 * sin(p.x * 0.02));
		float swell = smoothstep(0.0, 0.45, ph2) * (1.0 - smoothstep(0.45, 0.8, ph2));
		float crest = smoothstep(0.42, 0.48, ph2) * (1.0 - smoothstep(0.48, 0.54, ph2));
		float foam = crest * step(0.5, stHash(floor(p * 0.5) + floor(stCycle(1200.0) * 4.0)));
		gain = stLum(vec3(0.8, 1.05, 1.25)) * swell * 0.5 + foam * 1.2;
	}
	// <<< END GENERATED trim gain

	// El max() es una guarda de empaquetado, no de look: packEmissive() hace
	// floor()/fract() sobre el valor y un negativo saldria por el canal B como
	// un valor enorme. Ningun efecto del catalogo produce ganancia negativa.
	return max(gain, 0.0) * stFxEmissiveGain();
}

void getMaterial(out float roughness, out float metallic, out float emissive, out float F0, out float sss)
{
	vec4 matColor = texture2D(uTextureMaterial, vTexCoord);
	
	if (uMaterialFormat == 2) // LabPBR
	{
		if (matColor.g > 0.898) // Metallic
		{
			metallic = 1.0; F0 = 1.0; sss = 0.0;
		}
		else // Non-metallic
		{
			metallic = 0.0; F0 = matColor.g;
			sss = (matColor.b > 0.255 ? (((matColor.b - 0.255) / 0.745) * uDefaultSubsurface) : 0.0);
		}
		
		roughness = pow(1.0 - matColor.r, 2.0);
		// ShinyTrims (oleada 4): con uTrimMask la alpha de este mapa es la
		// COBERTURA DEL TRIM con la rampa completa 0..255, NO la codificacion
		// LabPBR (que usa alpha 255 = sin emision y 254 = emision maxima: un
		// filo de UN LSB que sobrevive al muestreo POINT y muere con cualquier
		// interpolacion o mipmap). El decode LabPBR de todo lo demas queda
		// intacto -- con uTrimMask == 0 esta linea es exactamente la de
		// siempre, byte a byte (gate GP.1).
		//
		// OLEADA 4b: la cobertura ya NO va sola. Multiplica a la GANANCIA DEL
		// EFECTO en este frame (stTrimGain), que es lo que devuelve el caracter
		// a los 20: sin ese factor el emisivo era un suelo constante y el
		// termino animado del glint, que satura, dejaba de leerse encima.
		//
		// OLEADA 9 -- LA BANDA DE OUTLINE. En el pack, los slots 80..154 son
		// la cascara 3D y llevan un PROLOGO propio antes de caer en la rama del
		// efecto (st_effects.glsl:21-37, slot += 100):
		//     st_ol_pos  = st_lum(base.rgb)          la luminancia ES la altura
		//     st_ol_wave = st_wavep(1600.0, pos*1.3) 1.3 ondas a lo largo del item
		//     fragColor  = base.rgb * (1 + 0.18*wave)   ... y FULLBRIGHT
		// Aqui la traduccion es directa y sin shader nuevo: la cobertura del
		// texel (que en la cascara es 1 en todos lados, el mapa M1 del outline
		// es alpha 255 entero) se multiplica por ese mismo (1 + 0.18*onda) y
		// sigue por la MISMA rama de ganancia del efecto que el trim plano.
		// El "fullbright" sale solo: emisivo 1.0 hace diffuse += 1.0 en
		// lighting_apply, o sea baseColor * (luz + 1); a oscuras eso es
		// EXACTAMENTE el 100 % del color del material, que es el "=" del pack.
		// A plena luz suma por encima de la luz, igual que ya le pasa a
		// glowing en este puerto -- diferencia declarada, no fullbright literal.
		//
		// La luminancia se lee de la DIFUSA de este texel, no de un varying: la
		// UV de la cascara es v = 16 - y sobre el item entero, asi que el
		// degradado de la textura ES la coordenada de altura. Misma propiedad
		// que usa el pack, mismo coste (una lectura que el fragmento ya hace en
		// main(), pero getMaterial no la recibe).
		if (uTrimMask > 0)
		{
			float trimGain = stTrimGain();
			float trimCov = matColor.a;
			if (uTrimOutline > 0)
			{
				float olPos = stLum(texture2D(uTexture, vTexCoord).rgb);
				trimCov *= (1.0 + 0.18 * stWavep(1600.0, olPos * 1.3));
				// EL SUELO DE FULLBRIGHT, y por que es un max y no un producto:
				// en el pack el prologo de la banda de outline hace fullbright
				// ANTES de caer en la rama del efecto, o sea el efecto solo
				// puede sumar sobre un borde que YA esta al 100 %. Aqui
				// stTrimGain() devuelve 0 cuando no hay efecto
				// (uTrimFx < 1), asi que multiplicar por el dejaria la cascara
				// sin emisivo -- y una cascara sin emisivo no es un borde, es
				// una tira de textura que la noche apaga. Con el max, sin
				// efecto el borde vale exactamente 1.0 (100 % del color del
				// material a oscuras) y con efecto sube donde el efecto sube.
				// El trim PLANO no lleva este suelo, y tampoco debe: un trim
				// sin efecto no brilla en el pack ni aqui.
				trimGain = max(trimGain, 1.0);
			}
			emissive = trimCov * trimGain * uTrimIntensity * uTrimEmissiveScale;
		}
		else
			emissive = (matColor.a < 1.0 ? matColor.a /= 0.9961 : 0.0) * uDefaultEmissive;

		return;
	}
	
	if (uMaterialFormat == 1) // SEUS
	{
		roughness = (1.0 - matColor.r);
		metallic = matColor.g;
		emissive = (matColor.b * uDefaultEmissive);
	}
	else // No map
	{
		roughness = uRoughness;
		metallic = uMetallic;
		emissive = max(uEmissive, vCustom.z * uDefaultEmissive);
	}
	
	F0 = mix(0.0, 1.0, metallic);
	sss = vCustom.w * uDefaultSubsurface;
}

void main()
{
	vec2 tex = vTexCoord;
	vec4 baseColor = vColor * texture2D(uTexture, tex);
	
	if (baseColor.a < 0.0001)
		discard;
	
	if (uAlphaHash > 0)
	{
		if (baseColor.a < hash(vec2(hash(vPosition.xy + (uSampleIndex / 255.0)), vPosition.z + (uSampleIndex / 255.0))))
			discard;
		else
			baseColor.a = 1.0;
	}
	
	// Get material data
	float roughness, metallic, emissive, F0, sss;
	getMaterial(roughness, metallic, emissive, F0, sss);
	
	// Fresnel
	vec3 N = getMappedNormal(vTexCoord);
	vec3 V  = normalize(uCameraPosition - vPosition);
	vec3 H  = normalize(V + -reflect(V, N));
	float F = fresnelSchlickRoughness(max(dot(H, V), 0.0), F0, roughness);
	
	if (uIsSky > 0)
		F = 0.0;
	
	gl_FragData[0] = vec4(roughness, metallic, F, 1.0);
	
	// Emissive
	gl_FragData[1] = vec4(packEmissive((emissive / 255.0) * baseColor.a), baseColor.a);
}
