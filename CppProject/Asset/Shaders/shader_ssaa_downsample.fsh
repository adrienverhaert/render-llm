/// shader_ssaa_downsample
/// MI3: supersampling resolve. The frame is rendered at an integer multiple
/// of the target resolution and averaged down with an exact SxS box filter
/// (correct and ring-free for integer scales). Fixed 4x4 loop with a guard so
/// SM4.0 can unroll it; taps land on texel centers (point sampling).

varying vec2 vTexCoord;

uniform float uScale;
uniform vec2 uTexelSize;

void main()
{
	vec4 sum = vec4(0.0);
	float s = uScale;

	for (float y = 0.0; y < 4.0; y += 1.0)
	{
		for (float x = 0.0; x < 4.0; x += 1.0)
		{
			if (x < s && y < s)
				sum += texture2D(gm_BaseTexture, vTexCoord + (vec2(x, y) - (s - 1.0) * 0.5) * uTexelSize);
		}
	}

	gl_FragColor = sum / (s * s);
}
