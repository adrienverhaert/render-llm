// 13-tap downsample (Jimenez, SIGGRAPH 2014 "Next Generation
// Post Processing in Call of Duty: Advanced Warfare").
// Runs at the destination (half) resolution; taps sample the larger
// source mip through bilinear filtering. uKaris enables luma-weighted
// group averaging on the first mip to suppress fireflies.

varying vec2 vTexCoord;

uniform vec2 uTexelSize; // 1 / source resolution
uniform int uKaris;

float karisWeight(vec3 c)
{
	return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
}

void main()
{
	float x = uTexelSize.x;
	float y = uTexelSize.y;

	vec3 a = texture2D(gm_BaseTexture, vTexCoord + vec2(-2.0 * x,  2.0 * y)).rgb;
	vec3 b = texture2D(gm_BaseTexture, vTexCoord + vec2( 0.0,      2.0 * y)).rgb;
	vec3 c = texture2D(gm_BaseTexture, vTexCoord + vec2( 2.0 * x,  2.0 * y)).rgb;
	vec3 d = texture2D(gm_BaseTexture, vTexCoord + vec2(-2.0 * x,  0.0)).rgb;
	vec3 e = texture2D(gm_BaseTexture, vTexCoord).rgb;
	vec3 f = texture2D(gm_BaseTexture, vTexCoord + vec2( 2.0 * x,  0.0)).rgb;
	vec3 g = texture2D(gm_BaseTexture, vTexCoord + vec2(-2.0 * x, -2.0 * y)).rgb;
	vec3 h = texture2D(gm_BaseTexture, vTexCoord + vec2( 0.0,     -2.0 * y)).rgb;
	vec3 i = texture2D(gm_BaseTexture, vTexCoord + vec2( 2.0 * x, -2.0 * y)).rgb;
	vec3 j = texture2D(gm_BaseTexture, vTexCoord + vec2(-x,  y)).rgb;
	vec3 k = texture2D(gm_BaseTexture, vTexCoord + vec2( x,  y)).rgb;
	vec3 l = texture2D(gm_BaseTexture, vTexCoord + vec2(-x, -y)).rgb;
	vec3 m = texture2D(gm_BaseTexture, vTexCoord + vec2( x, -y)).rgb;

	vec3 col;
	if (uKaris > 0)
	{
		// Weigh each overlapping 2x2 box by its Karis luma weight
		vec3 g0 = (j + k + l + m) * 0.25;
		vec3 g1 = (a + b + d + e) * 0.25;
		vec3 g2 = (b + c + e + f) * 0.25;
		vec3 g3 = (d + e + g + h) * 0.25;
		vec3 g4 = (e + f + h + i) * 0.25;
		float w0 = karisWeight(g0);
		float w1 = karisWeight(g1);
		float w2 = karisWeight(g2);
		float w3 = karisWeight(g3);
		float w4 = karisWeight(g4);
		col = (g0 * w0 * 0.5 + (g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) * 0.125)
		    / (w0 * 0.5 + (w1 + w2 + w3 + w4) * 0.125);
	}
	else
	{
		// 4 overlapping 2x2 boxes: center box 0.5, corner boxes 0.125 each
		col = e * 0.125;
		col += (a + c + g + i) * 0.03125;
		col += (b + d + f + h) * 0.0625;
		col += (j + k + l + m) * 0.125;
	}

	gl_FragColor = vec4(col, 1.0);
}
