// 9-tap tent upsample, kernel [1 2 1; 2 4 2; 1 2 1] / 16.
// Drawn additively (one, one) into the next-larger bloom mip.

varying vec2 vTexCoord;

uniform vec2 uTexelSize; // 1 / source resolution

void main()
{
	float x = uTexelSize.x;
	float y = uTexelSize.y;

	vec3 a = texture2D(gm_BaseTexture, vTexCoord + vec2(-x,  y)).rgb;
	vec3 b = texture2D(gm_BaseTexture, vTexCoord + vec2(0.0,  y)).rgb;
	vec3 c = texture2D(gm_BaseTexture, vTexCoord + vec2( x,  y)).rgb;
	vec3 d = texture2D(gm_BaseTexture, vTexCoord + vec2(-x, 0.0)).rgb;
	vec3 e = texture2D(gm_BaseTexture, vTexCoord).rgb;
	vec3 f = texture2D(gm_BaseTexture, vTexCoord + vec2( x, 0.0)).rgb;
	vec3 g = texture2D(gm_BaseTexture, vTexCoord + vec2(-x, -y)).rgb;
	vec3 h = texture2D(gm_BaseTexture, vTexCoord + vec2(0.0, -y)).rgb;
	vec3 i = texture2D(gm_BaseTexture, vTexCoord + vec2( x, -y)).rgb;

	vec3 col = e * 4.0;
	col += (b + d + f + h) * 2.0;
	col += (a + c + g + i);

	gl_FragColor = vec4(col * (1.0 / 16.0), 1.0);
}
