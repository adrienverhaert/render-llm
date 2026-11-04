uniform sampler2D uTexture; // static
uniform vec2 uTextureSize;

uniform sampler2D uGlintTexture; // static
uniform vec2 uGlintOffset;
uniform vec2 uGlintSize;
uniform int uGlintEnabled;
uniform float uGlintStrength;

uniform float uGamma;

uniform float uSampleIndex;
uniform int uAlphaHash;

// --- ShinyTrims -----------------------------------------------------------
// ORDEN OBLIGATORIO: escalares primero, el ARRAY al final. Ver el quirk de
// packing del cbuffer en Shader.cpp AddUniform (un escalar declarado DESPUES
// de un array se coloca 4 bytes antes de donde lo pone HLSL, y ademas la
// escritura del array lo pisa). Mismo motivo documentado en
// shader_high_lighting_apply.fsh. Si anades un uniform, va ANTES de
// uTrimPalette[8], y tambien ANTES en el bloque de shader_startup.
//
//   POR OBJETO (los escriben render_world_tl / render_world_model_part; el
//   buffer por objeto HEREDA del objeto anterior, asi que se fijan SIEMPRE):
//     uTrimFx         0 = ninguno; si no, el SLOT del pack (254 = glowing ...)
//     uTrimIntensity  ganancia del termino aditivo
//     uTrimOrigin     origen del espacio CUERPO en mundo (raiz del set)
//     uFogShow        lo escribe render_world_tl:180-184 por objeto
//
//   DE ESCENA (los escribe shader_use una sola vez por pase; los arrays y los
//   uniformes marcados static son compartidos por todo el batch):
//     uTime           ya lo registra new_shader y lo setea shader_use
//     uTrimTick0      offset inicial en ticks de Minecraft
//     uTrimLoopL      longitud del loop en ticks (0 = modo FIEL)
//     uTrimSpace      -1 = AUTO (el default por efecto del catalogo),
//                      0 = CUERPO, 1 = PANTALLA
//     uTrimBodyScale  px de referencia por unidad de MI3 (espacio CUERPO)
//     uTrimRefH       altura de referencia en px (1080, decision (b) del PO)
//     uTrimFullSize   lienzo COMPLETO en px (render_tile_fullw/fullh)
//     uTrimTileOrg    esquina del tile dentro del lienzo completo
//     uTrimTileSize   tamano del tile en px del lienzo completo
//     uTrimSceneLight luz media de la escena en 0..1 (aproximacion, nocturne)
//     uTrimNoiseSize  lado de la textura de ruido (128)
//     uTrimFogThrough 0 (POR DEFECTO) = el efecto RECIBE la niebla de MI3;
//                      1 = la atraviesa, "fiel a Minecraft" (oleada 4)
//     uTrimNoise      ruido azul POINT-sampled: el stHash determinista
//     uTrimPalette    8 colores de la paleta del trim (mascara M2) -- EL ULTIMO
//
//   NIEBLA (oleada 4): uFogShow / uFogDistance / uFogSize / uFogHeight y
//   uCameraPosition NO necesitan fontaneria nueva -- new_shader.gml:29-35 los
//   registra de oficio en TODOS los shaders y shader_use.gml:38-55 los rellena
//   si el shader los expone. Basta con DECLARARLOS aqui. uFogColor no se
//   declara a proposito: el termino es aditivo y "aplicar niebla" es atenuar
//   hacia 0, no mezclar hacia el color de la niebla (que shader_color_fog ya
//   horneo en el albedo; mezclarlo aqui la sumaria dos veces).
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
uniform int uTrimFogThrough; // static
uniform int uFogShow;
uniform float uFogDistance; // static
uniform float uFogSize; // static
uniform float uFogHeight; // static
uniform vec3 uCameraPosition; // static
uniform sampler2D uTrimNoise; // static
uniform vec3 uTrimPalette[8]; // static

varying vec3 vPosition;
varying vec4 vColor;
varying vec2 vTexCoord;
varying vec4 vClip;

float hash(vec2 c)
{
	return fract(10000.0 * sin(17.0 * c.x + 0.1 * c.y) *
	(0.1 + abs(sin(13.0 * c.y + c.x))));
}

// EL BLOQUE DE ABAJO ES LA FUENTE DEL RUNTIME COMPARTIDO.
// tools/shinytrims/gen_trim_effects.py lo COPIA tal cual dentro de
// shader_high_material.fsh (bloque generado "trim runtime"), porque la ganancia
// emisiva del trim tiene que evaluar la MISMA envolvente temporal que este
// termino aditivo y MI3 no tiene #include. --check falla si los dos difieren en
// un byte. Ahi dentro no puede entrar nada que dependa de la mascara M2 (el
// array de paleta) ni del pase GLINT: el otro shader no los tiene.
// >>> BEGIN SHARED trim runtime
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
// <<< END SHARED trim runtime

// --- Mascara M2: deteccion por paleta ------------------------------------
// El compositor funde base + trim en la misma surface y pierde la distincion
// (minecraft_update_armor_generate.gml:82-99). Pero shader_palette remapea por
// igualdad EXACTA de RGB (shader_palette.fsh:16-17), asi que los texeles del
// trim llevan literalmente uno de los 8 colores de la paleta. La textura se
// crea R8G8B8A8_UNORM (sin sRGB) y se muestrea POINT por defecto, asi que el
// valor que llega aqui es el byte crudo / 255. Tolerancia 1/255 para absorber
// la conversion.
float stTrimMask(vec4 raw)
{
	if (raw.a < 0.5)
		return 0.0;

	for (int i = 0; i < 8; i++)
	{
		vec3 d = abs(raw.rgb - uTrimPalette[i]);
		if (max(d.r, max(d.g, d.b)) < 0.004)
			return 1.0;
	}

	return 0.0;
}

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

void main()
{
	vec2 tex = vTexCoord;
	vec4 rawColor = texture2D(uTexture, tex);
	vec4 baseColor = vColor * rawColor;

	if (baseColor.a < 0.0001)
		discard;

	if (uAlphaHash > 0)
	{
		if (baseColor.a < hash(vec2(hash(vPosition.xy + (uSampleIndex / 255.0)), vPosition.z + (uSampleIndex / 255.0))))
			discard;
		else
			baseColor.a = 1.0;
	}

	if (uGlintEnabled > 0 && baseColor.a > 0.0)
		baseColor.rgb = pow(texture2D(uGlintTexture, (tex * ((uTextureSize / uGlintSize))) + uGlintOffset).rgb * baseColor.a * uGlintStrength, vec3(uGamma));
	else
		baseColor.rgb = vec3(0.0);

	// --- ShinyTrims: termino aditivo del efecto de trim ------------------
	// ATENCION (corregido en la oleada 3; el comentario anterior AFIRMABA que
	// este pase era HDR y era FALSO). Este pase se dibuja a render_surface[1],
	// que es R8G8B8A8_UNORM -- render_high_scene.gml:8 llama a surface_require
	// SIN el argumento hdr y surface_require.gml:8 lo tiene a false por
	// defecto. Solo DESPUES (render_high_scene.gml:31-37, bm_add) se acumula
	// sobre render_surface_specular, que ese si es HDR
	// (render_high_passes.gml:9). Es decir: el termino aditivo se RECORTA A 1.0
	// ANTES de llegar al HDR, y por tanto uTrimIntensity SATURA, no escala.
	// Es FIEL a Minecraft (el pack asume backbuffer LDR con clamp) y es la
	// decision v1 del PO (ADENDA). El BRILLO y el bloom viajan por el canal
	// EMISIVO (shader_high_material.fsh + uTrimEmissiveScale, oleada 4).
	// Con uTrimFx == 0 no se ejecuta ni una instruccion nueva: es la condicion
	// de los gates de regresion GF (oleada 2) y GK.1 (oleada 3).
	if (uTrimFx > 0)
	{
		float m = stTrimMask(rawColor);

		if (m > 0.0)
		{
			vec3 base = rawColor.rgb;

			// Espacio del patron. uTrimSpace: -1 AUTO (el default del catalogo
			// por efecto), 0 fuerza CUERPO, 1 fuerza PANTALLA. Se calculan los
			// dos y se elige con un mix para que la rama sea coherente por draw
			// (uTrimFx y uTrimSpace son uniformes) y el coste, unas pocas ALU.
			float scr = stFxScreen();
			if (uTrimSpace == 0)
				scr = 0.0;
			else if (uTrimSpace > 0)
				scr = 1.0;

			vec2 p = mix(stBody(), stScreen(), scr);
			vec2 tx = floor(vTexCoord * uTextureSize);
			vec2 uv = vTexCoord;
			vec3 add = vec3(0.0);

			// >>> BEGIN GENERATED trim effects
			// NO EDITAR A MANO. Fuente: tools/shinytrims/trim_effects.json
			// Regenerar: py tools/shinytrims/gen_trim_effects.py
			// --- glowing (slot 254, reagent glow_ink_sac, espacio none, defecto CUERPO)
			// pack: fragColor.rgb = st_base.rgb;
			if (uTrimFx == 254)
			{
				add = base;
			}
			// --- periodic (slot 253, reagent echo_shard, espacio time, defecto CUERPO)
			// pack: float pulse = 0.5 + 0.5 * st_wave(1273.0);
			// pack: fragColor.rgb = mix(fragColor.rgb, st_base.rgb, pulse * pulse);
			else if (uTrimFx == 253)
			{
				float pulse = 0.5 + 0.5 * stWave(1273.0);
				add = base * (pulse * pulse);
			}
			// --- shimmer (slot 252, reagent glowstone_dust, espacio plane, defecto CUERPO)
			// pack: float s = 0.5 * st_wavep(318.0, gl_FragCoord.y * 0.01) + 0.5 * st_wavep(796.0, gl_FragCoord.x * 0.007);
			// pack: fragColor.rgb += st_base.rgb * (0.3 * smoothstep(-0.2, 0.8, s));
			else if (uTrimFx == 252)
			{
				float s = 0.5 * stWavep(318.0, p.y * 0.01) + 0.5 * stWavep(796.0, p.x * 0.007);
				add = base * (0.3 * smoothstep(-0.2, 0.8, s));
			}
			// --- aurora (slot 251, reagent heart_of_the_sea, espacio plane, defecto CUERPO)
			// pack: vec2 p = gl_FragCoord.xy;
			// pack: float curtain = 0.5 + 0.5 * sin(p.x * 0.045 + 3.0 * st_wave(90.0) + 2.0 * st_wavep(140.0, p.y * 0.0012));
			// pack: float band = curtain * curtain * curtain;
			// pack: float huedrift = 0.5 + 0.5 * st_wavep(70.0, p.x * 0.001);
			// pack: vec3 veil = st_hsv(mix(0.34, 0.76, huedrift), 0.6, 1.0) * st_lum(st_base.rgb) * 1.6;
			// pack: fragColor.rgb = max(fragColor.rgb, mix(fragColor.rgb, veil, band * 0.85));
			else if (uTrimFx == 251)
			{
				float curtain = 0.5 + 0.5 * sin(p.x * 0.045 + 3.0 * stWave(90.0) + 2.0 * stWavep(140.0, p.y * 0.0012));
				float band = curtain * curtain * curtain;
				float huedrift = 0.5 + 0.5 * stWavep(70.0, p.x * 0.001);
				vec3 veil = stHsv(mix(0.34, 0.76, huedrift), 0.6, 1.0) * stLum(base) * 1.6;
				add = veil * (band * 0.85);
			}
			// --- bodysweep (slot 250, reagent breeze_rod, espacio plane, defecto CUERPO)
			// pack: float band = fract(st_cycle(220.0) + gl_FragCoord.y * 0.004);
			// pack: float bar = smoothstep(0.0, 0.12, band) * (1.0 - smoothstep(0.12, 0.35, band));
			// pack: fragColor.rgb = mix(fragColor.rgb, st_base.rgb, 0.35);
			// pack: fragColor.rgb += st_base.rgb * bar * 1.6;
			else if (uTrimFx == 250)
			{
				float band = fract(stCycle(220.0) + p.y * 0.004);
				float bar = smoothstep(0.0, 0.12, band) * (1.0 - smoothstep(0.12, 0.35, band));
				add = base * 0.35 + base * bar * 1.6;
			}
			// --- breath (slot 249, reagent ghast_tear, espacio time, defecto CUERPO)
			// pack: float b = 0.5 + 0.5 * st_wave(300.0);
			// pack: float inten = 0.25 + 0.85 * b * b;
			// pack: fragColor.rgb = mix(fragColor.rgb, st_base.rgb * inten, 0.6);
			else if (uTrimFx == 249)
			{
				float b = 0.5 + 0.5 * stWave(300.0);
				float inten = 0.25 + 0.85 * b * b;
				add = base * inten * 0.6;
			}
			// --- charge (slot 248, reagent lightning_rod, espacio texel, defecto CUERPO)
			// pack: vec2 tx = floor(ST_UV * vec2(textureSize(Sampler0, 0)));
			// pack: float t = st_cycle(140.0);
			// pack: float fill = clamp((t - 0.05) / 0.50, 0.0, 1.0) - clamp((t - 0.64) / 0.31, 0.0, 1.0);
			// pack: float lit = step(st_hash(tx), fill);
			// pack: float flash = smoothstep(0.55, 0.60, t) * (1.0 - smoothstep(0.60, 0.72, t));
			// pack: fragColor.rgb = max(fragColor.rgb, mix(fragColor.rgb, st_base.rgb * (0.85 + 0.5 * fill), lit) + st_base.rgb * flash * 1.6);
			else if (uTrimFx == 248)
			{
				float t = stCycle(140.0);
				float fill = clamp((t - 0.05) / 0.50, 0.0, 1.0) - clamp((t - 0.64) / 0.31, 0.0, 1.0);
				float lit = step(stHash(tx), fill);
				float flash = smoothstep(0.55, 0.60, t) * (1.0 - smoothstep(0.60, 0.72, t));
				add = base * (0.85 + 0.5 * fill) * lit + base * flash * 1.6;
			}
			// --- chrome (slot 247, reagent heavy_core, espacio plane, defecto CUERPO)
			// pack: vec2 p = gl_FragCoord.xy;
			// pack: float band1 = 0.5 + 0.5 * sin(p.x * 0.05 + p.y * 0.02 + 4.0 * st_wave(120.0));
			// pack: float band2 = 0.5 + 0.5 * sin(p.x * 0.013 - p.y * 0.04 + 3.0 * st_wave(90.0));
			// pack: float spec = band1 * band1 * band2;
			// pack: fragColor.rgb = max(fragColor.rgb, st_base.rgb * (0.95 + 0.9 * spec) + vec3(1.0) * st_lum(st_base.rgb) * spec * spec * 0.8);
			else if (uTrimFx == 247)
			{
				float band1 = 0.5 + 0.5 * sin(p.x * 0.05 + p.y * 0.02 + 4.0 * stWave(120.0));
				float band2 = 0.5 + 0.5 * sin(p.x * 0.013 - p.y * 0.04 + 3.0 * stWave(90.0));
				float spec = band1 * band1 * band2;
				add = base * (0.95 + 0.9 * spec) + vec3(1.0) * stLum(base) * spec * spec * 0.8;
			}
			// --- kaleido (slot 246, reagent firework_star, espacio plane, defecto CUERPO)
			// pack: vec2 cell = floor(gl_FragCoord.xy / 7.0);
			// pack: float id = st_hash(cell);
			// pack: vec3 jewel = st_hsv(fract(id + st_cycle(60.0)), 0.65, 1.0);
			// pack: float tw = 0.8 + 0.2 * st_wavep(240.0, id);
			// pack: fragColor.rgb = max(fragColor.rgb, mix(st_base.rgb, jewel * st_lum(st_base.rgb) * 1.7, 0.5) * tw);
			else if (uTrimFx == 246)
			{
				vec2 cell = floor(p / 7.0);
				float idc = stHash(cell);
				vec3 jewel = stHsv(fract(idc + stCycle(60.0)), 0.65, 1.0);
				float tw = 0.8 + 0.2 * stWavep(240.0, idc);
				add = mix(base, jewel * stLum(base) * 1.7, 0.5) * tw;
			}
			// --- lava_lamp (slot 245, reagent fire_charge, espacio plane, defecto CUERPO)
			// pack: vec2 p = gl_FragCoord.xy * 0.012;
			// pack: float blob = 0.5 + 0.5 * sin(p.y * 2.2 - 6.2831 * st_cycle(70.0) + 1.7 * sin(p.x * 2.9 + 2.0 * st_wave(50.0)));
			// pack: float body = smoothstep(0.35, 0.9, blob);
			// pack: vec3 warm = st_base.rgb * vec3(1.25, 0.95, 0.75);
			// pack: fragColor.rgb = max(fragColor.rgb, mix(st_base.rgb * 0.95, warm * 1.5, body));
			else if (uTrimFx == 245)
			{
				vec2 q = p * 0.012;
				float blob = 0.5 + 0.5 * sin(q.y * 2.2 - 6.2831 * stCycle(70.0) + 1.7 * sin(q.x * 2.9 + 2.0 * stWave(50.0)));
				float body = smoothstep(0.35, 0.9, blob);
				vec3 warm = base * vec3(1.25, 0.95, 0.75);
				add = mix(base * 0.95, warm * 1.5, body);
			}
			// --- molten (slot 244, reagent blaze_powder, espacio texel, defecto CUERPO)
			// pack: float col = floor(ST_UV.x * float(textureSize(Sampler0, 0).x));
			// pack: float drop = fract(ST_UV.y * 5.0 - st_cycle(140.0) - st_hash(vec2(col, 1.0)));
			// pack: float tail = smoothstep(0.35, 0.95, drop);
			// pack: float head = smoothstep(0.88, 0.97, drop) * (1.0 - smoothstep(0.97, 1.0, drop));
			// pack: vec3 hot = st_base.rgb * vec3(1.35, 1.0, 0.7);
			// pack: fragColor.rgb += hot * (0.25 * tail + 1.4 * head);
			// params: span = 0.04
			else if (uTrimFx == 244)
			{
				float col = floor(uv.x * uTextureSize.x);
				float drop = fract(uv.y * 0.04 - stCycle(140.0) - stHash(vec2(col, 1.0)));
				float tail = smoothstep(0.35, 0.95, drop);
				float head = smoothstep(0.88, 0.97, drop) * (1.0 - smoothstep(0.97, 1.0, drop));
				vec3 hot = base * vec3(1.35, 1.0, 0.7);
				add = hot * (0.25 * tail + 1.4 * head);
			}
			// --- nebula (slot 243, reagent ender_eye, espacio plane, defecto CUERPO)
			// pack: vec2 p = gl_FragCoord.xy * 0.015;
			// pack: float f1 = 0.5 + 0.5 * st_wavep(300.0, p.x * 0.3 + p.y * 0.17);
			// pack: float f2 = 0.5 + 0.5 * st_wavep(430.0, p.y * 0.23 - p.x * 0.11);
			// pack: float cloud = f1 * 0.6 + f2 * 0.4;
			// pack: vec3 hue = st_hsv(fract(0.72 + 0.1 * f1 + st_cycle(50.0)), 0.55, 1.0);
			// pack: vec3 neb = mix(st_base.rgb, hue * st_lum(st_base.rgb) * 1.7, 0.45 + 0.3 * cloud);
			// pack: fragColor.rgb = max(fragColor.rgb, neb * (0.85 + 0.45 * cloud));
			else if (uTrimFx == 243)
			{
				vec2 q = p * 0.015;
				float f1 = 0.5 + 0.5 * stWavep(300.0, q.x * 0.3 + q.y * 0.17);
				float f2 = 0.5 + 0.5 * stWavep(430.0, q.y * 0.23 - q.x * 0.11);
				float cloud = f1 * 0.6 + f2 * 0.4;
				vec3 hue = stHsv(fract(0.72 + 0.1 * f1 + stCycle(50.0)), 0.55, 1.0);
				vec3 neb = mix(base, hue * stLum(base) * 1.7, 0.45 + 0.3 * cloud);
				add = neb * (0.85 + 0.45 * cloud);
			}
			// --- nocturne (slot 242, reagent phantom_membrane, espacio time, defecto CUERPO)
			// pack: float dark = 1.0 - smoothstep(0.25, 0.85, st_lum(ST_LIGHT.rgb));
			// pack: vec3 lit = max(fragColor.rgb, st_base.rgb * 1.35 * (0.9 + 0.1 * st_wave(600.0)));
			// pack: fragColor.rgb = mix(fragColor.rgb, lit, dark);
			else if (uTrimFx == 242)
			{
				float dark = 1.0 - smoothstep(0.25, 0.85, uTrimSceneLight);
				add = base * 1.35 * (0.9 + 0.1 * stWave(600.0)) * dark;
			}
			// --- oilslick (slot 241, reagent prismarine_shard, espacio plane, defecto CUERPO)
			// pack: vec2 p = gl_FragCoord.xy;
			// pack: float film = sin(p.x * 0.03 + 2.0 * st_wave(80.0)) + sin(p.y * 0.037 - 2.0 * st_wave(110.0)) + sin((p.x + p.y) * 0.021 + 3.0 * st_wave(60.0));
			// pack: vec3 slick = st_hsv(fract(film * 0.16 + st_cycle(40.0)), 0.7, 1.0);
			// pack: float lum = st_lum(st_base.rgb);
			// pack: fragColor.rgb = max(fragColor.rgb, mix(st_base.rgb * 1.1, slick * lum * 1.8, 0.55));
			else if (uTrimFx == 241)
			{
				float film = sin(p.x * 0.03 + 2.0 * stWave(80.0)) + sin(p.y * 0.037 - 2.0 * stWave(110.0)) + sin((p.x + p.y) * 0.021 + 3.0 * stWave(60.0));
				vec3 slick = stHsv(fract(film * 0.16 + stCycle(40.0)), 0.7, 1.0);
				float lum = stLum(base);
				add = mix(base * 1.1, slick * lum * 1.8, 0.55);
			}
			// --- opal (slot 240, reagent crying_obsidian, espacio plane, defecto CUERPO)
			// pack: float band = gl_FragCoord.x * 0.008 + gl_FragCoord.y * 0.011;
			// pack: vec3 sheen = st_hsv(fract(band * 0.25 + st_cycle(60.0)), 0.35, 1.0);
			// pack: float lum = st_lum(st_base.rgb);
			// pack: float glide = 0.45 + 0.25 * st_wavep(90.0, band);
			// pack: fragColor.rgb = max(fragColor.rgb, mix(fragColor.rgb, sheen * lum * 1.5, glide * 0.6));
			else if (uTrimFx == 240)
			{
				float band = p.x * 0.008 + p.y * 0.011;
				vec3 sheen = stHsv(fract(band * 0.25 + stCycle(60.0)), 0.35, 1.0);
				float lum = stLum(base);
				float glide = 0.45 + 0.25 * stWavep(90.0, band);
				add = sheen * lum * 1.5 * (glide * 0.6);
			}
			// --- plasma (slot 239, reagent magma_cream, espacio plane, defecto CUERPO)
			// pack: vec2 p = gl_FragCoord.xy * 0.02;
			// pack: float w = st_wavep(700.0, p.x * 0.35) + st_wavep(1100.0, p.y * 0.27 + p.x * 0.11);
			// pack: float m = 0.5 + 0.25 * w;
			// pack: vec3 shift = st_hsv(fract(0.55 + 0.12 * w + st_cycle(80.0)), 0.45, 1.0);
			// pack: fragColor.rgb = mix(fragColor.rgb, st_base.rgb * shift * (0.7 + 0.8 * m), 0.7);
			else if (uTrimFx == 239)
			{
				vec2 q = p * 0.02;
				float w = stWavep(700.0, q.x * 0.35) + stWavep(1100.0, q.y * 0.27 + q.x * 0.11);
				float m2 = 0.5 + 0.25 * w;
				vec3 shift = stHsv(fract(0.55 + 0.12 * w + stCycle(80.0)), 0.45, 1.0);
				add = base * shift * (0.7 + 0.8 * m2) * 0.7;
			}
			// --- rainbow_flow (slot 238, reagent prismarine_crystals, espacio plane, defecto CUERPO)
			// pack: float h = fract(st_cycle(160.0) + (gl_FragCoord.x + gl_FragCoord.y) * 0.0022);
			// pack: vec3 rgb = st_hsv(h, 0.75, 1.0) * (0.35 + 0.9 * st_lum(st_base.rgb));
			// pack: fragColor.rgb = mix(fragColor.rgb, rgb, 0.8);
			else if (uTrimFx == 238)
			{
				float h = fract(stCycle(160.0) + (p.x + p.y) * 0.0022);
				vec3 tint = stHsv(h, 0.75, 1.0) * (0.35 + 0.9 * stLum(base));
				add = tint * 0.8;
			}
			// --- silk (slot 237, reagent string, espacio plane, defecto CUERPO)
			// pack: vec2 p = gl_FragCoord.xy;
			// pack: float fold = 0.5 + 0.5 * sin(p.y * 0.025 + 3.0 * sin(p.x * 0.008 + 2.0 * st_wave(70.0)) + 6.2831 * st_cycle(100.0));
			// pack: vec3 pearl = st_hsv(fract(st_cycle(45.0) + 0.5), 0.4, 1.0) * st_lum(st_base.rgb) * 1.6;
			// pack: fragColor.rgb = max(fragColor.rgb, mix(st_base.rgb * 1.05, pearl, fold * 0.6) * (0.9 + 0.35 * fold));
			else if (uTrimFx == 237)
			{
				float fold = 0.5 + 0.5 * sin(p.y * 0.025 + 3.0 * sin(p.x * 0.008 + 2.0 * stWave(70.0)) + 6.2831 * stCycle(100.0));
				vec3 pearl = stHsv(fract(stCycle(45.0) + 0.5), 0.4, 1.0) * stLum(base) * 1.6;
				add = mix(base * 1.05, pearl, fold * 0.6) * (0.9 + 0.35 * fold);
			}
			// --- spark (slot 236, reagent amethyst_shard, espacio texel, defecto CUERPO)
			// pack: vec2 tx = floor(ST_UV * vec2(textureSize(Sampler0, 0)));
			// pack: float ph = st_hash(tx);
			// pack: float tw = smoothstep(0.55, 1.0, 0.5 + 0.5 * st_wavep(600.0, ph));
			// pack: float glint = smoothstep(0.97, 1.0, st_hash(tx + vec2(floor(st_cycle(48.0) * 8.0), 3.0)));
			// pack: fragColor.rgb = mix(fragColor.rgb, st_base.rgb * (0.9 + 1.6 * tw), 0.55 + 0.45 * tw);
			// pack: fragColor.rgb += st_base.rgb * glint * 1.8;
			else if (uTrimFx == 236)
			{
				float ph = stHash(tx);
				float tw = smoothstep(0.55, 1.0, 0.5 + 0.5 * stWavep(600.0, ph));
				float gl = smoothstep(0.97, 1.0, stHash(tx + vec2(floor(stCycle(48.0) * 8.0), 3.0)));
				add = base * (0.9 + 1.6 * tw) * (0.55 + 0.45 * tw) + base * gl * 1.8;
			}
			// --- tide (slot 235, reagent nautilus_shell, espacio plane, defecto CUERPO)
			// pack: float ph = fract(st_cycle(180.0) + gl_FragCoord.y * 0.003 + 0.15 * sin(gl_FragCoord.x * 0.02));
			// pack: float swell = smoothstep(0.0, 0.45, ph) * (1.0 - smoothstep(0.45, 0.8, ph));
			// pack: float crest = smoothstep(0.42, 0.48, ph) * (1.0 - smoothstep(0.48, 0.54, ph));
			// pack: float foam = crest * step(0.5, st_hash(floor(gl_FragCoord.xy * 0.5) + floor(st_cycle(1200.0) * 4.0)));
			// pack: vec3 sea = st_base.rgb * vec3(0.8, 1.05, 1.25);
			// pack: fragColor.rgb += sea * swell * 0.5 + vec3(1.0) * st_lum(st_base.rgb) * foam * 1.2;
			else if (uTrimFx == 235)
			{
				float ph2 = fract(stCycle(180.0) + p.y * 0.003 + 0.15 * sin(p.x * 0.02));
				float swell = smoothstep(0.0, 0.45, ph2) * (1.0 - smoothstep(0.45, 0.8, ph2));
				float crest = smoothstep(0.42, 0.48, ph2) * (1.0 - smoothstep(0.48, 0.54, ph2));
				float foam = crest * step(0.5, stHash(floor(p * 0.5) + floor(stCycle(1200.0) * 4.0)));
				vec3 sea = base * vec3(0.8, 1.05, 1.25);
				add = sea * swell * 0.5 + vec3(1.0) * stLum(base) * foam * 1.2;
			}
			// <<< END GENERATED trim effects

			add *= uTrimIntensity * m;

			// --- Oleada 4: la atmosfera de MI3 sobre el termino aditivo ---
			// uTrimFogThrough == 0 (POR DEFECTO, decision (e) del PO): el
			// efecto se atenua con la niebla como cualquier otra superficie.
			// == 1: la atraviesa, fiel a Minecraft (en el pack el efecto pisa
			// el color que YA habia pasado por apply_fog, y un trim glowing a
			// 200 bloques se ve con su color pleno y destruye la profundidad;
			// SHINY_GROUND_TRUTH seccion 8). MI3 tiene perspectiva aerea
			// propia y no la tira por defecto.
			// La formula es copia literal de shader_color_fog.fsh:45-59.
			// El termino es ADITIVO, asi que "aplicar niebla" es ATENUAR HACIA
			// CERO (add *= 1 - fog), no mezclar hacia uFogColor: el color de la
			// niebla ya esta en el albedo que sale de shader_color_fog:82, y
			// mezclarlo aqui otra vez la aplicaria dos veces.
			// El canal EMISIVO (shader_high_material, oleada 4) no necesita
			// este toggle: multiplica un albedo que ya lleva la niebla
			// horneada, asi que respeta la atmosfera gratis.
			if (uTrimFogThrough < 1 && uFogShow > 0)
			{
				float fogDepth = distance(vPosition, uCameraPosition);
				float fog = clamp(1.0 - (uFogDistance - fogDepth) / uFogSize, 0.0, 1.0);
				fog *= clamp(1.0 - (vPosition.z - uFogHeight) / uFogSize, 0.0, 1.0);
				add *= (1.0 - fog);
			}

			baseColor.rgb += add;
		}
	}

	gl_FragColor = baseColor;
}
