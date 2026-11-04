// GTAO: ground-truth ambient occlusion (Jimenez et al. 2016).
// Horizon-based and cosine-weighted, replacing the old hemisphere-kernel
// SSAO. Slice rotation and march offset come from the per-sample blue
// noise, so the estimate converges across the render's sample accumulation.
#define PI 3.14159265
#define HALF_PI 1.5707963
#define SLICES 3
#define STEPS 6

varying vec2 vTexCoord;

uniform sampler2D uDepthBuffer;
uniform sampler2D uNormalBuffer;
uniform sampler2D uEmissiveBuffer;
uniform sampler2D uNoiseBuffer;
uniform sampler2D uMaskBuffer;

uniform float uNormalBufferScale;

uniform float uNear;
uniform float uFar;
uniform float uOrthoScale;

uniform mat4 uProjMatrix;
uniform mat4 uProjMatrixInv;

uniform vec2 uScreenSize;
uniform float uNoiseSize;

uniform float uRadius;
uniform float uPower;
uniform vec4 uColor;

// Get depth
float unpackValue(vec4 c)
{
	return c.r + c.g / 255.0 + c.b / (255.0 * 255.0);
}

// Get normal Value
vec3 unpackNormal(vec4 c)
{
	return (c.rgb / uNormalBufferScale) * 2.0 - 1.0;
}

// Transform linear depth to exponential depth.
// MI3 R14: with an ORTHOGRAPHIC camera the NDC z is already linear, so the
// mapping is a plain scale: uOrthoScale = (uFar - uNear) / (camFar - camNear),
// which rescales the buffer's normalized linear depth into the NDC range of the
// real projection matrix fed to uProjMatrixInv. 0 = perspective (old path).
float transformDepth(float depth)
{
	if (uOrthoScale > 0.0)
		return depth * uOrthoScale;

	return (uFar - (uNear * uFar) / (depth * (uFar - uNear) + uNear)) / (uFar - uNear);
}

// Reconstruct a position from a screen space coordinate and (linear) depth
vec3 posFromBuffer(vec2 coord, float depth)
{
	vec4 pos = uProjMatrixInv * vec4(coord.x * 2.0 - 1.0, 1.0 - coord.y * 2.0, transformDepth(depth), 1.0);
	return pos.xyz / pos.w;
}

float getSSAOstrength(vec2 uv)
{
	float emissive = unpackValue(texture2D(uEmissiveBuffer, uv)) * 255.0;
	float mask = texture2D(uMaskBuffer, uv).r;
	return (1.0 - clamp(emissive, 0.0, 1.0)) * mask;
}

// acos approximation, max error ~0.0068 rad
float fastAcos(float x)
{
	float res = -0.156583 * abs(x) + HALF_PI;
	res *= sqrt(1.0 - abs(x));
	return (x >= 0.0) ? res : PI - res;
}

// Cosine-weighted visible-arc integral (paper eq. 2, half)
float arcIntegral(float h, float n)
{
	return 0.25 * (-cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n));
}

void main()
{
	// Perform alpha test to ignore background
	if (texture2D(uDepthBuffer, vTexCoord).a < 1.0)
		discard;

	// View space origin, surface normal, view direction
	float originDepth = unpackValue(texture2D(uDepthBuffer, vTexCoord));
	vec3 vpos = posFromBuffer(vTexCoord, originDepth);
	vec3 vnorm = unpackNormal(texture2D(uNormalBuffer, vTexCoord));
	vec3 vdir = normalize(-vpos);

	// Screen-space extent of uRadius view units at this depth, in pixels
	float projScale = uProjMatrix[1][1] * uScreenSize.y * 0.5;
	float ssRadius = min(uRadius * projScale / max(abs(vpos.z), 0.0001), 256.0);
	float stepPix = max(ssRadius / float(STEPS), 1.0);

	// Blue noise decorrelates slices per pixel and per accumulation sample
	vec2 noiseScale = uScreenSize / uNoiseSize;
	vec4 noise = texture2D(uNoiseBuffer, vTexCoord * noiseScale);
	float rotBase = noise.r;
	float offBase = noise.g;

	vec2 texel = 1.0 / uScreenSize;
	float radius2 = uRadius * uRadius;

	float visibility = 0.0;
	int s;
	for (s = 0; s < SLICES; s++)
	{
		// Slice direction in view space; screen march flips Y (uv Y is down)
		float phi = PI * fract(rotBase + float(s) / float(SLICES));
		vec3 sliceDir = vec3(cos(phi), sin(phi), 0.0);
		vec2 screenDir = vec2(sliceDir.x, -sliceDir.y) * texel;

		// March both sides of the slice for the two horizon cosines.
		// Taps with depth alpha < 1 hit empty space (transparent-background
		// renders leave the sky unwritten -- raw reads decode as depth 0,
		// i.e. glued to the camera, occluding every silhouette to black):
		// treat them as open sky.
		float h0 = -1.0;
		float h1 = -1.0;
		int i;
		for (i = 0; i < STEPS; i++)
		{
			float t = (float(i) + offBase) * stepPix + 1.0;

			vec2 suv = vTexCoord + screenDir * t;
			if (suv.x >= 0.0 && suv.y >= 0.0 && suv.x <= 1.0 && suv.y <= 1.0)
			{
				vec4 dtex = texture2D(uDepthBuffer, suv);
				if (dtex.a >= 1.0)
				{
					vec3 ws = posFromBuffer(suv, unpackValue(dtex)) - vpos;
					float dist2 = max(dot(ws, ws), 0.0001);
					float falloff = clamp(1.0 - dist2 / radius2, 0.0, 1.0);
					h0 = max(h0, mix(-1.0, dot(ws, vdir) / sqrt(dist2), falloff));
				}
			}

			suv = vTexCoord - screenDir * t;
			if (suv.x >= 0.0 && suv.y >= 0.0 && suv.x <= 1.0 && suv.y <= 1.0)
			{
				vec4 dtex = texture2D(uDepthBuffer, suv);
				if (dtex.a >= 1.0)
				{
					vec3 ws = posFromBuffer(suv, unpackValue(dtex)) - vpos;
					float dist2 = max(dot(ws, ws), 0.0001);
					float falloff = clamp(1.0 - dist2 / radius2, 0.0, 1.0);
					h1 = max(h1, mix(-1.0, dot(ws, vdir) / sqrt(dist2), falloff));
				}
			}
		}

		// Project the normal into the slice plane, integrate the visible arc
		vec3 planeN = normalize(cross(sliceDir, vdir));
		vec3 projN = vnorm - planeN * dot(vnorm, planeN);
		float projNL = max(length(projN), 0.0001);
		float cosN = clamp(dot(projN, vdir) / projNL, -1.0, 1.0);
		vec3 tangent = cross(vdir, planeN);
		float n = ((dot(projN, tangent) >= 0.0) ? 1.0 : -1.0) * fastAcos(cosN);

		// Clamp both horizons to the hemisphere around the normal
		float a1 = n + max(-fastAcos(h1) - n, -HALF_PI);
		float a2 = n + min(fastAcos(h0) - n, HALF_PI);
		visibility += projNL * (arcIntegral(a1, n) + arcIntegral(a2, n));
	}
	visibility = clamp(visibility / float(SLICES), 0.0, 1.0);

	// uPower steepens the response like the old SSAO power curve
	float occlusion = 1.0 - pow(visibility, uPower);
	occlusion *= getSSAOstrength(vTexCoord);
	occlusion = clamp(occlusion, 0.0, 1.0);

	// Mix
	gl_FragColor = mix(vec4(1.0), uColor, occlusion);
}
