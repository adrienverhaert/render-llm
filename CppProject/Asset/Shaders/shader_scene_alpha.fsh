// Writes the scene-coverage mask (SCENE_TEST, in .r) into the alpha
// channel. Drawn with separate-alpha blending (zero,one | one,zero) so
// RGB stays untouched -- used to cut the background out of renders that
// were internally drawn WITH a background (the reliable path).

varying vec2 vTexCoord;

void main()
{
	gl_FragColor = vec4(0.0, 0.0, 0.0, texture2D(gm_BaseTexture, vTexCoord).r);
}
