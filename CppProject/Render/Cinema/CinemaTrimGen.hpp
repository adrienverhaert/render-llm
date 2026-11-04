#pragma once

// ---------------------------------------------------------------------------
// CinemaTrimGen.hpp -- FICHERO GENERADO. NO EDITAR A MANO.
//   Fuente:    tools/shinytrims/trim_effects.json
//   Generador: py tools/shinytrims/gen_trim_effects.py
//
// TERCER TARGET del catalogo de ShinyTrims (P1 del PO: el catalogo es LA
// fuente; el raster tiene dos targets, Cinema el tercero). Contiene el runtime
// st* traducido a HLSL y el CUERPO de cada efecto por separado, para que
// CinemaTrimSource componga en runtime SOLO los efectos presentes en la escena
// (D10). Con cero efectos el marcador // @@TRIM@@ del kernel se sustituye por
// la cadena VACIA y el texto HLSL es BYTE A BYTE el de siempre -- que es la
// garantia estructural del gate de regresion G5.
//
// Contrato de nombres que el kernel debe cumplir ANTES del marcador:
//   float4 trimNoise      rect de la textura de ruido dentro de su slice del
//                         atlas (xy = origen, zw = escala)
//   float4 trimNoiseSize  x = uTrimNoiseSize (ESCALAR, 128 -- el ruido es
//                         cuadrado, igual que en el raster), y = slice del
//                         atlas, z = trim_emissive del usuario, w = uTrimBodyScale
//   #define uTrimTick0 / uTime / uTrimLoopL sobre el cbuffer PTFrame
//   float3 LinearToSrgb3(float3)  (ya existe en el kernel)
// ---------------------------------------------------------------------------

namespace CppProject
{
	namespace CinemaTrimGen
	{
		// Un efecto del catalogo. `body` son las sentencias HLSL que calculan
		// `add` a partir de (base, p, tx, uv, uTextureSize, uTrimSceneLight).
		struct Effect
		{
			unsigned int slot;
			const char*  name;
			const char*  body;
			// Correccion de calibracion por efecto (campo emissive_gain del
			// catalogo, o cinema_gain si el catalogo lo declara). En Cinema el
			// A/B es contra EL DEFECTO DE ENVIO del raster, asi que se aplica:
			// sin ella spark (p90 2.471) y charge (1.849) quemarian justo como
			// la regla del p90 se invento para evitar.
			float        cinemaGain;
			// Cotas de la ganancia sobre la rejilla determinista del generador
			// (240 muestras). gainMax x cinemaGain es la COTA ESTATICA de la
			// CDF de emisores (D7): sobreestimar solo reparte de mas, mientras
			// que subestimar produce luciernagas en los picos.
			float        gainMin;
			float        gainP90;
			float        gainMax;
		};


		// Runtime compartido (una sola copia; se emite solo si la escena
		// lleva al menos un efecto).
		constexpr const char* RUNTIME = R"HLSL(
// Runtime st* del trim, TRADUCIDO del bloque SHARED de
// GmProject/shaders/shader_high_glint/shader_high_glint.fsh.
// Tres funciones se sobrescriben (y solo tres): stHash (el ruido vive
// en el atlas SRGB, no en una surface UNORM) y stBody/stScreen (que se
// ELIMINAN: el espacio lo calcula el kernel y llega como parametro p).
// uTrimTick0 / uTime / uTrimLoopL los define el kernel sobre su cbuffer,
// asi que la FORMULA del tiempo es literalmente la misma que en el raster.

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
// En modo FIEL replica el pack: frac(GameTime * n) con GameTime = frac(t/24000).
// En LOOP LOCK se reduce el tiempo a fmod(t, L) ANTES de multiplicar: asi
// t = 0 y t = L dan bit a bit el mismo valor (con la forma frac(frac(t/24000)*n)
// el redondeo de fp32 deja un residuo ~1e-7 y el loop no cierra byte a byte).
float stPhase(float n)
{
	float nn = stQn(n);
	float t = stTicks();

	if (uTrimLoopL <= 0.0)
		return frac(frac(t / 24000.0) * nn);

	float tl = fmod(t, uTrimLoopL);
	return frac((tl * nn) / 24000.0);
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
	return sin(frac(stPhase(n) + ph) * 6.2831853);
}

float stLum(float3 c)
{
	return dot(c, float3(0.299, 0.587, 0.114));
}

float3 stHsv(float h, float s, float v)
{
	float3 p = abs(frac(((float3)(h)) + float3(0.0, 0.6666667, 0.3333333)) * 6.0 - 3.0);
	return v * lerp(((float3)(1.0)), clamp(p - 1.0, 0.0, 1.0), s);
}

float stHash(float2 c)
{
	float2 t = frac((floor(c) + 0.5) / trimNoiseSize.x);
	float2 uv = trimNoise.xy + t * trimNoise.zw;
	float3 lin = gAtlas.SampleLevel(gSampPoint, float3(uv, trimNoiseSize.y), 0.0).rgb;
	return LinearToSrgb3(lin).r;
}
)HLSL";

		constexpr unsigned int COUNT = 20;

		inline const Effect* Table()
		{
			static const Effect table[COUNT] = {
				{ 254, "glowing",
				  R"HLSL(
					add = base;
				  )HLSL",
				  1.0f, 1.0f, 1.0f, 1.0f },
				{ 253, "periodic",
				  R"HLSL(
					float pulse = 0.5 + 0.5 * stWave(1273.0);
					add = base * (pulse * pulse);
				  )HLSL",
				  1.0f, 0.0f, 0.950996f, 0.999975f },
				{ 252, "shimmer",
				  R"HLSL(
					float s = 0.5 * stWavep(318.0, p.y * 0.01) + 0.5 * stWavep(796.0, p.x * 0.007);
					add = base * (0.3 * smoothstep(-0.2, 0.8, s));
				  )HLSL",
				  1.0f, 0.0f, 0.284645f, 0.3f },
				{ 251, "aurora",
				  R"HLSL(
					float curtain = 0.5 + 0.5 * sin(p.x * 0.045 + 3.0 * stWave(90.0) + 2.0 * stWavep(140.0, p.y * 0.0012));
					float band = curtain * curtain * curtain;
					float huedrift = 0.5 + 0.5 * stWavep(70.0, p.x * 0.001);
					float3 veil = stHsv(lerp(0.34, 0.76, huedrift), 0.6, 1.0) * stLum(base) * 1.6;
					add = veil * (band * 0.85);
				  )HLSL",
				  0.15f, 0.0f, 0.81577f, 1.100134f },
				{ 250, "bodysweep",
				  R"HLSL(
					float band = frac(stCycle(220.0) + p.y * 0.004);
					float bar = smoothstep(0.0, 0.12, band) * (1.0 - smoothstep(0.12, 0.35, band));
					add = base * 0.35 + base * bar * 1.6;
				  )HLSL",
				  0.553f, 0.35f, 1.809833f, 1.949922f },
				{ 249, "breath",
				  R"HLSL(
					float b = 0.5 + 0.5 * stWave(300.0);
					float inten = 0.25 + 0.85 * b * b;
					add = base * inten * 0.6;
				  )HLSL",
				  1.0f, 0.15f, 0.632989f, 0.65997f },
				{ 248, "charge",
				  R"HLSL(
					float t = stCycle(140.0);
					float fill = clamp((t - 0.05) / 0.50, 0.0, 1.0) - clamp((t - 0.64) / 0.31, 0.0, 1.0);
					float lit = step(stHash(tx), fill);
					float flash = smoothstep(0.55, 0.60, t) * (1.0 - smoothstep(0.60, 0.72, t));
					add = base * (0.85 + 0.5 * fill) * lit + base * flash * 1.6;
				  )HLSL",
				  0.541f, 0.0f, 1.848702f, 2.946561f },
				{ 247, "chrome",
				  R"HLSL(
					float band1 = 0.5 + 0.5 * sin(p.x * 0.05 + p.y * 0.02 + 4.0 * stWave(120.0));
					float band2 = 0.5 + 0.5 * sin(p.x * 0.013 - p.y * 0.04 + 3.0 * stWave(90.0));
					float spec = band1 * band1 * band2;
					add = base * (0.95 + 0.9 * spec) + ((float3)(1.0)) * stLum(base) * spec * spec * 0.8;
				  )HLSL",
				  0.106f, 0.95f, 1.886391f, 2.573255f },
				{ 246, "kaleido",
				  R"HLSL(
					float2 cell = floor(p / 7.0);
					float idc = stHash(cell);
					float3 jewel = stHsv(frac(idc + stCycle(60.0)), 0.65, 1.0);
					float tw = 0.8 + 0.2 * stWavep(240.0, idc);
					add = lerp(base, jewel * stLum(base) * 1.7, 0.5) * tw;
				  )HLSL",
				  0.357f, 0.539518f, 1.121368f, 1.233481f },
				{ 245, "lava_lamp",
				  R"HLSL(
					float2 q = p * 0.012;
					float blob = 0.5 + 0.5 * sin(q.y * 2.2 - 6.2831 * stCycle(70.0) + 1.7 * sin(q.x * 2.9 + 2.0 * stWave(50.0)));
					float body = smoothstep(0.35, 0.9, blob);
					float3 warm = base * float3(1.25, 0.95, 0.75);
					add = lerp(base * 0.95, warm * 1.5, body);
				  )HLSL",
				  0.052f, 0.95f, 1.52535f, 1.52535f },
				{ 244, "molten",
				  R"HLSL(
					float col = floor(uv.x * uTextureSize.x);
					float drop = frac(uv.y * 0.04 - stCycle(140.0) - stHash(float2(col, 1.0)));
					float tail = smoothstep(0.35, 0.95, drop);
					float head = smoothstep(0.88, 0.97, drop) * (1.0 - smoothstep(0.97, 1.0, drop));
					float3 hot = base * float3(1.35, 1.0, 0.7);
					add = hot * (0.25 * tail + 1.4 * head);
				  )HLSL",
				  1.0f, 0.0f, 0.253227f, 1.739123f },
				{ 243, "nebula",
				  R"HLSL(
					float2 q = p * 0.015;
					float f1 = 0.5 + 0.5 * stWavep(300.0, q.x * 0.3 + q.y * 0.17);
					float f2 = 0.5 + 0.5 * stWavep(430.0, q.y * 0.23 - q.x * 0.11);
					float cloud = f1 * 0.6 + f2 * 0.4;
					float3 hue = stHsv(frac(0.72 + 0.1 * f1 + stCycle(50.0)), 0.55, 1.0);
					float3 neb = lerp(base, hue * stLum(base) * 1.7, 0.45 + 0.3 * cloud);
					add = neb * (0.85 + 0.45 * cloud);
				  )HLSL",
				  0.165f, 0.822324f, 1.519057f, 1.810717f },
				{ 242, "nocturne",
				  R"HLSL(
					float dark = 1.0 - smoothstep(0.25, 0.85, uTrimSceneLight);
					add = base * 1.35 * (0.9 + 0.1 * stWave(600.0)) * dark;
				  )HLSL",
				  0.158f, 0.0f, 1.265563f, 1.349933f },
				{ 241, "oilslick",
				  R"HLSL(
					float film = sin(p.x * 0.03 + 2.0 * stWave(80.0)) + sin(p.y * 0.037 - 2.0 * stWave(110.0)) + sin((p.x + p.y) * 0.021 + 3.0 * stWave(60.0));
					float3 slick = stHsv(frac(film * 0.16 + stCycle(40.0)), 0.7, 1.0);
					float lum = stLum(base);
					add = lerp(base * 1.1, slick * lum * 1.8, 0.55);
				  )HLSL",
				  0.297f, 0.877998f, 1.345993f, 1.40528f },
				{ 240, "opal",
				  R"HLSL(
					float band = p.x * 0.008 + p.y * 0.011;
					float3 sheen = stHsv(frac(band * 0.25 + stCycle(60.0)), 0.35, 1.0);
					float lum = stLum(base);
					float glide = 0.45 + 0.25 * stWavep(90.0, band);
					add = sheen * lum * 1.5 * (glide * 0.6);
				  )HLSL",
				  1.0f, 0.131506f, 0.514975f, 0.601201f },
				{ 239, "plasma",
				  R"HLSL(
					float2 q = p * 0.02;
					float w = stWavep(700.0, q.x * 0.35) + stWavep(1100.0, q.y * 0.27 + q.x * 0.11);
					float m2 = 0.5 + 0.25 * w;
					float3 shift = stHsv(frac(0.55 + 0.12 * w + stCycle(80.0)), 0.45, 1.0);
					add = base * shift * (0.7 + 0.8 * m2) * 0.7;
				  )HLSL",
				  1.0f, 0.315092f, 0.76296f, 0.925348f },
				{ 238, "rainbow_flow",
				  R"HLSL(
					float h = frac(stCycle(160.0) + (p.x + p.y) * 0.0022);
					float3 tint = stHsv(h, 0.75, 1.0) * (0.35 + 0.9 * stLum(base));
					add = tint * 0.8;
				  )HLSL",
				  0.35f, 0.336318f, 0.819028f, 0.911391f },
				{ 237, "silk",
				  R"HLSL(
					float fold = 0.5 + 0.5 * sin(p.y * 0.025 + 3.0 * sin(p.x * 0.008 + 2.0 * stWave(70.0)) + 6.2831 * stCycle(100.0));
					float3 pearl = stHsv(frac(stCycle(45.0) + 0.5), 0.4, 1.0) * stLum(base) * 1.6;
					add = lerp(base * 1.05, pearl, fold * 0.6) * (0.9 + 0.35 * fold);
				  )HLSL",
				  0.133f, 0.945089f, 1.501587f, 1.664681f },
				{ 236, "spark",
				  R"HLSL(
					float ph = stHash(tx);
					float tw = smoothstep(0.55, 1.0, 0.5 + 0.5 * stWavep(600.0, ph));
					float gl = smoothstep(0.97, 1.0, stHash(tx + float2(floor(stCycle(48.0) * 8.0), 3.0)));
					add = base * (0.9 + 1.6 * tw) * (0.55 + 0.45 * tw) + base * gl * 1.8;
				  )HLSL",
				  0.405f, 0.495f, 2.471152f, 4.110358f },
				{ 235, "tide",
				  R"HLSL(
					float ph2 = frac(stCycle(180.0) + p.y * 0.003 + 0.15 * sin(p.x * 0.02));
					float swell = smoothstep(0.0, 0.45, ph2) * (1.0 - smoothstep(0.45, 0.8, ph2));
					float crest = smoothstep(0.42, 0.48, ph2) * (1.0 - smoothstep(0.48, 0.54, ph2));
					float foam = crest * step(0.5, stHash(floor(p * 0.5) + floor(stCycle(1200.0) * 4.0)));
					float3 sea = base * float3(0.8, 1.05, 1.25);
					add = sea * swell * 0.5 + ((float3)(1.0)) * stLum(base) * foam * 1.2;
				  )HLSL",
				  1.0f, 0.0f, 0.473914f, 1.688348f },
			};
			return table;
		}
	}
}
