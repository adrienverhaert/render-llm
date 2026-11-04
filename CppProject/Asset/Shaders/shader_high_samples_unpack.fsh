/// shader_high_samples_unpack
/// MI3: HDR sample resolve + display transform. The accumulator is a single
/// RGBA32F surface holding the SUM of all rendered samples; this pass
/// averages it (x 1/N) and, for the AgX tonemappers, applies the display
/// transform HERE, on the converged linear-domain average (the Blender way)
/// instead of per sample. Classic tonemappers (None/Reinhard/ACES) keep the
/// legacy per-sample path and arrive here already display-encoded, with
/// uTonemapper set to 0.

varying vec2 vTexCoord;

uniform float uSamplesStrength;
uniform int uTonemapper;
uniform float uExposure;
uniform float uGamma;

// Veiling-glare floor: every real lens scatters a fraction of the scene's
// light into the blacks (~0.1-1% of white). Added in scene-linear after
// exposure, it rescues deep-shadow/dark-albedo texture detail from the AgX
// toe without measurably moving mids or highlights. 0 = off (legacy look).
uniform float uBlackLift;

// Post-tonemap saturation boost for the Reimagined path (BSL-style; CR
// default 1.07). 1.0 = neutral.
uniform float uSatBoost;

// The engine lights in display space (sRGB albedo x sRGB light colors,
// no decode anywhere), so the accumulated average is display-referred.
// AgX is a scene-linear transform: linearize first (pow 2.2) or the
// whole image lands on the flat top of the sigmoid and washes out.
uniform int uLinearize;

// Physically-based bloom (multi-mip tent pyramid), mixed into the
// scene-linear color before exposure/tonemap. uBloomScale normalizes
// the additive mip accumulation; uBloomMix = 0 disables entirely.
uniform sampler2D uBloom;
uniform float uBloomMix;
uniform float uBloomScale;

/// AgX approximation (Benjamin Wrensch / iolite), matches Blender 4.x default
/// view transform closely. Input: scene-referred linear Rec.709, unclamped.

// 6th-order polynomial approximation of the AgX sigmoid contrast curve
vec3 agxContrast(vec3 x)
{
	vec3 x2 = x * x;
	vec3 x4 = x2 * x2;
	return + 15.5     * x4 * x2
	       - 40.14    * x4 * x
	       + 31.96    * x4
	       - 6.868    * x2 * x
	       + 0.4298   * x2
	       + 0.1191   * x
	       - 0.00232;
}

vec3 agxInset(vec3 val)
{
	float min_ev = -12.47393;
	float max_ev = 4.026069;

	// Inset matrix (expanded, no mat3 in the transpiler), log2 encoding
	// over 16.5 stops, then the sigmoid
	val = vec3(
		val.r * 0.842479062253094  + val.g * 0.0784335999999992 + val.b * 0.0792237451477643,
		val.r * 0.0423282422610123 + val.g * 0.878468636469772  + val.b * 0.0791661274605434,
		val.r * 0.0423756549057051 + val.g * 0.0784336          + val.b * 0.879142973793104
	);
	val = clamp(log2(max(val, vec3(0.0000000001))), vec3(min_ev), vec3(max_ev));
	val = (val - min_ev) / (max_ev - min_ev);
	return agxContrast(val);
}

vec3 agxOutset(vec3 val)
{
	// Inverse inset ("outset") matrix, expanded
	return vec3(
		val.r *  1.19687900512017   + val.g * -0.0980208811401368 + val.b * -0.0990297440797205,
		val.r * -0.0528968517574562 + val.g *  1.15190312990417   + val.b * -0.0989611768448433,
		val.r * -0.0529716355144438 + val.g * -0.0980434501171241 + val.b *  1.15107367264116
	);
}

// "Punchy" look, tamed: Blender's preset (power 1.35 / sat 1.4) nukes
// warm point lights in this engine's dim linear scenes -- 1.3/1.3 keeps
// the pop without the orange takeover
vec3 agxLookPunchy(vec3 val)
{
	float luma = dot(val, vec3(0.2126, 0.7152, 0.0722));
	val = pow(max(val, vec3(0.0)), vec3(1.3));
	return vec3(luma) + 1.3 * (val - vec3(luma));
}

/// "Reimagined": the Complementary Reimagined r5.8.1 display transform
/// (Lottes 2016 curve + CR's three post-hacks), ported verbatim. Unlike the
/// AgX path this expects the engine's native gamma-space light (albedo x
/// light, NO linearization) -- exactly the range this engine accumulates.

float crLuma(vec3 c)
{
	return dot(c, vec3(0.299, 0.587, 0.114));
}

// Exact sRGB encode (k = 0.055), step() instead of lessThan for the transpiler
vec3 crLinearToRGB(vec3 c)
{
	c = max(c, vec3(0.0));
	vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
	vec3 lo = 12.92 * c;
	return mix(hi, lo, step(c, vec3(0.0031308)));
}

vec3 tonemapReimagined(vec3 val)
{
	float lumaIn = crLuma(val);

	// Lottes 2016, CR r5.8.1 constants: contrast 1.05, roll-off 1.0,
	// hdrMax 8.0, mid 0.25 -> 0.25
	vec3 ta = vec3(1.05);
	vec3 tad = vec3(1.05);
	vec3 hdrMax = vec3(8.0);
	vec3 midIn = vec3(0.25);
	vec3 midOut = vec3(0.25);
	vec3 tb = (-pow(midIn, ta) + pow(hdrMax, ta) * midOut) / ((pow(hdrMax, tad) - pow(midIn, tad)) * midOut);
	vec3 tc = (pow(hdrMax, tad) * pow(midIn, ta) - pow(hdrMax, ta) * midOut * pow(midIn, tad)) / ((pow(hdrMax, tad) - pow(midIn, tad)) * midOut);
	vec3 outv = pow(max(val, vec3(0.0)), ta) / (pow(max(val, vec3(0.0)), tad) * tb + tc);
	outv = crLinearToRGB(outv);

	// CR hack 1 -- dark lift: below luma 0.1 blend 75% back to plain
	// gamma 2.2 (shadow readability WITHOUT any additive veil)
	float darkLift = smoothstep(0.1, 0.0, lumaIn);
	vec3 smoothColor = pow(max(val, vec3(0.0)), vec3(1.0 / 2.2));
	outv = mix(outv, smoothColor, darkLift * 0.75);

	// CR hack 2 -- path to white: strong highlights roll to pure white
	float whitePath = smoothstep(0.0, 16.0, lumaIn);
	outv = mix(outv, vec3(1.0), whitePath);

	// CR hack 3 -- desaturate darks 25%: kills parasitic chroma in shadows
	float desatPath = smoothstep(0.1, 0.0, lumaIn);
	outv = mix(outv, vec3(crLuma(outv)), desatPath * 0.25);

	return clamp(outv, vec3(0.0), vec3(1.0));
}

void main()
{
	vec4 color = texture2D(gm_BaseTexture, vTexCoord) * uSamplesStrength;

	// The scene surface is alpha-premultiplied (rendered over cleared
	// transparent black). Tone transforms must run on STRAIGHT colors or
	// every silhouette against a transparent background gets dark,
	// hue-shifted fringes -- un-premultiply here, re-premultiply after
	// the display transform. Opaque pixels are untouched.
	// uLinearize: 0 = nothing, 1 = un-premultiply + pow 2.2 (AgX wants
	// scene-linear), 2 = un-premultiply only (Reimagined runs in the
	// engine's native gamma space, like CR/BSL do)
	if (uLinearize > 0)
	{
		float pa = max(color.a, 0.0000001);
		color.rgb = max(color.rgb / pa, vec3(0.0));
		if (uLinearize == 1)
			color.rgb = pow(color.rgb, vec3(2.2));
	}

	if (uTonemapper >= 3)
	{
		vec3 val = color.rgb;

		// Bloom pyramid is built from the resolve in the same space as the
		// tonemap input, so this mix is space-consistent for both paths
		if (uBloomMix > 0.0)
			val = mix(val, texture2D(uBloom, vTexCoord).rgb * uBloomScale, uBloomMix);

		if (uTonemapper == 5)
		{
			// Reimagined: CR exposure is fixed 1.0 upstream; uExposure stays
			// as the user's knob. No mid-gray anchor (gamma-space input).
			val *= uExposure;
			val += vec3(uBlackLift);
			val = tonemapReimagined(val);

			// CR/BSL post-tonemap saturation boost (preset-driven; CR 1.07)
			float crAvg = (val.r + val.g + val.b) / 3.0;
			val = val * uSatBoost - vec3(crAvg * (uSatBoost - 1.0));
		}
		else
		{
			// Middle-gray anchor: the engine's light units are display-tuned
			// (sun strength 1 = full brightness), which lands ~2/3 EV low once
			// linearized -- lift so AgX mids keep their perceived level
			val *= uExposure * 1.7;
			val += vec3(uBlackLift);

			val = agxInset(val);
			if (uTonemapper == 4)
				val = agxLookPunchy(val);
			val = agxOutset(val);
		}

		// The transform output is already 2.2-display-encoded; only apply the
		// user deviation from 2.2 so gamma = 2.2 is a no-op
		color.rgb = pow(max(val, vec3(0.0)), vec3(2.2/uGamma));

		// Triangular dither (+-1 LSB) breaks 8-bit banding in smooth
		// gradients; deterministic per pixel so accumulation stays stable
		float dith1 = fract(sin(dot(vTexCoord, vec2(12.9898, 78.233))) * 43758.5453);
		float dith2 = fract(sin(dot(vTexCoord, vec2(39.3468, 11.1351))) * 24634.6345);
		color.rgb += vec3((dith1 + dith2 - 1.0) / 255.0);

		// Back to premultiplied for compositing/saving
		color.rgb *= color.a;
	}

	gl_FragColor = color;
}
