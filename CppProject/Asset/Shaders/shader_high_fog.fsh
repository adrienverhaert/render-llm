uniform sampler2D uTexture; // static

uniform float uSampleIndex;
uniform int uAlphaHash;

uniform int uFogShow;
uniform vec4 uFogColor; // static
uniform float uFogDistance; // static
uniform float uFogSize; // static
uniform float uFogHeight; // static
uniform vec3 uSunDirection;

uniform vec3 uCameraPosition; // static

varying vec3 vPosition;
varying vec4 vColor;
varying vec2 vTexCoord;
varying float vDepth;

// MI3 realismo: perspectiva AEREA en vez de fog lineal (REALISM_DESIGN).
// Extincion exponencial exp(-d*sigma) con zona despejada cerca de camara,
// mas caida exponencial de densidad con la altura sobre uFogHeight.
// uFogDistance conserva su significado de "distancia de visibilidad".
float getFog()
{
	float fog;
	if (uFogShow > 0)
	{
		float fogDepth = distance(vPosition, uCameraPosition);
		float sigma = 1.6 / max(uFogDistance, 1.0);
		float clearZone = uFogDistance * 0.04;
		float dens = 1.0 - exp(-max(fogDepth - clearZone, 0.0) * sigma);
		float hfall = exp(-max(vPosition.z - uFogHeight, 0.0) / max(uFogSize, 1.0));
		fog = clamp(dens * mix(0.55, 1.0, hfall), 0.0, 1.0);
	}
	else
		fog = 0.0;

	return fog;
}

// Lobulo forward-scatter (Mie aprox): la niebla se tine del color del sol
// cuando la vista apunta hacia el; el apply mezcla con este factor (canal G)
float getMie()
{
	vec3 viewDir = normalize(vPosition - uCameraPosition);
	float m = clamp(dot(viewDir, normalize(uSunDirection)), 0.0, 1.0);
	return pow(m, 8.0);
}

float hash(vec2 c)
{
	return fract(10000.0 * sin(17.0 * c.x + 0.1 * c.y) *
	(0.1 + abs(sin(13.0 * c.y + c.x))));
}

void main()
{
	vec2 tex = vTexCoord;
	vec4 baseColor = vColor * texture2D(uTexture, tex);

	if (baseColor.a > 0.0)
		gl_FragColor = vec4(getFog(), getMie(), 0.0, 1.0);
	else
		discard;

	if (uAlphaHash > 0 && (baseColor.a < hash(vec2(hash(vPosition.xy + (uSampleIndex / 255.0)), vPosition.z + (uSampleIndex / 255.0)))))
		discard;
}
