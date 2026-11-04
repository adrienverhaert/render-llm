#define PI 3.14159265359

uniform int uShadowsEnabled;
uniform int uFaceShade;
uniform int uFloor;
uniform float uNormalBufferScale;
uniform int uSpecularEnabled;
uniform sampler2D uShadows;
uniform sampler2D uEmissive;
uniform sampler2D uSpecular;

uniform sampler2D uMask;
uniform sampler2D uMaterialBuffer;
uniform int uReflectionsEnabled;
uniform vec4 uFallbackColor;

uniform float uGamma;

// MI3: tabla de sombreado de caras vanilla (Level.getShade) - arriba 1.0,
// abajo 0.5, caras N-S 0.8, caras E-O 0.6. Cuantiza la normal de MUNDO del
// G-buffer (Z-up, igual que el hemisferio del ambient) al eje dominante.
// Da la profundidad "chunky" legible de Minecraft. Activable por render setting.
// (uFaceShade y uNormalBufferScale declarados arriba, junto a los escalares
// existentes, por el quirk de packing del cbuffer.)
uniform sampler2D uNormalBuffer;

varying vec2 vTexCoord;

float unpackEmissive(vec4 c)
{
	return c.r + c.g / 255.0 + c.b / (255.0 * 255.0);
}

vec3 unpackNormal(vec4 c)
{
	return (c.rgb / uNormalBufferScale) * 2.0 - 1.0;
}

float getFaceShade()
{
	vec3 fsNormal = unpackNormal(texture2D(uNormalBuffer, vTexCoord));
	vec3 fsAbs = abs(fsNormal);
	float fsShade = 0.6; // este-oeste
	if (fsAbs.y >= fsAbs.x)
		fsShade = 0.8; // norte-sur
	if (fsAbs.z >= fsAbs.x && fsAbs.z >= fsAbs.y)
		fsShade = (fsNormal.z > 0.0 ? 1.0 : 0.5); // arriba / abajo
	return fsShade;
}

void main()
{
	vec4 baseColor = texture2D(gm_BaseTexture, vTexCoord);
	float mask = texture2D(uMask, vTexCoord).r;
	
	// Apply gamma to base
	baseColor.rgb = pow(baseColor.rgb, mix(vec3(1.0), vec3(uGamma), mask));
	
	vec4 matColor = texture2D(uMaterialBuffer, vTexCoord); // G = metallic, B = fresnel
	vec3 spec = mix(vec3(1.0), baseColor.rgb, matColor.g) * pow(uFallbackColor.rgb, vec3(uGamma)) * matColor.b;
	
	// Sum up lighting
	vec3 diffuse = vec3(1.0);
	
	if (uShadowsEnabled > 0)
		diffuse = texture2D(uShadows, vTexCoord).rgb;
	
	// Reduce diffuse based on Metallic
	diffuse *= (1.0 - matColor.g);
	
	// "Energy conservation", remove diffuse based on fresnel
	diffuse *= (1.0 - matColor.b);

	// MI3: sombreado de caras vanilla (Level.getShade) + PISO de luz.
	// (1) atenua la LUZ por orientacion de cara (el look "chunky" legible).
	// (2) PISO: ninguna cara baja de faceShade * cielo. En vanilla ninguna cara
	//     es negro puro en el dia; el modelo diferido SI ennegrece caras que
	//     miran lejos de toda luz (p.ej. el enves que drapea de una elytra: su
	//     albedo es correcto pero su buffer de luz queda en 0 -> negro). El piso
	//     lo rescata con luz de cielo tenue tintada por la tabla de caras.
	//     RenderDoc (22 jul) probo que el albedo del ala es perfecto; esto es
	//     puramente el termino de luz. Gateado por el toggle face-shade.
	if (uFloor > 0 || uFaceShade > 0)
	{
		float fsShade = getFaceShade();

		// Multiply "chunky" (toggle del usuario): atenua la luz por cara.
		if (uFaceShade > 0)
			diffuse *= fsShade;

		// PISO (siempre que haya normales): ninguna cara baja de faceShade*cielo.
		// El modelo de Minecraft -- ninguna cara es negro puro de dia. Rescata el
		// enves que drapea de la elytra (albedo correcto, buffer de luz en 0) y
		// cualquier cara que el deferred puro ennegrece. Cielo desaturado ~60%
		// hacia gris para mostrar los colores REALES de la textura, no un lavado
		// azul, con un toque de tinte de cielo (rebote natural del enves).
		if (uFloor > 0)
		{
			vec3 fsSky = pow(uFallbackColor.rgb, vec3(uGamma));
			vec3 fsFloorCol = mix(vec3(dot(fsSky, vec3(0.299, 0.587, 0.114))), fsSky, 0.4);
			diffuse = max(diffuse, fsShade * fsFloorCol * 0.55);
		}
	}

	// Add emissive
	if (uShadowsEnabled > 0)
		diffuse += unpackEmissive(texture2D(uEmissive, vTexCoord) * 255.0);
	
	baseColor.rgb *= mix(vec3(1.0), diffuse, mask);
	
	// If reflections are disabled, add fallback color for fresnel
	if (uReflectionsEnabled == 0)
		baseColor.rgb += spec;
	
	if (uSpecularEnabled > 0)
		baseColor.rgb += texture2D(uSpecular, vTexCoord).rgb;
	
	gl_FragColor = baseColor;
}
