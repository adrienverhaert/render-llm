uniform sampler2D uFogBuffer;
uniform vec4 uFogColor;
uniform vec4 uFogColorSun;

varying vec2 vTexCoord;

// MI3 realismo: inscatter bicolor -- el color de la niebla vira del tono de
// horizonte (uFogColor) al tono de sol calido (uFogColorSun) segun el lobulo
// forward-scatter calculado por-pixel en el pase de niebla (canal G).
void main()
{
	vec2 fogData = texture2D(uFogBuffer, vTexCoord).rg;
	float fog = fogData.r;
	float mie = fogData.g;
	vec3 col = mix(uFogColor.rgb, uFogColorSun.rgb, clamp(mie * 0.7, 0.0, 1.0));
	gl_FragColor = vec4(col, fog);
}
