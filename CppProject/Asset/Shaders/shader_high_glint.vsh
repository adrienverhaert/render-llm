/// shader_high_glint

attribute vec3 in_Position;
attribute vec3 in_Normal;
attribute vec4 in_Colour;
attribute vec2 in_TextureCoord;
attribute vec4 in_Wave;
attribute vec3 in_Tangent;

varying vec3 vPosition;
varying vec4 vColor;
varying vec2 vTexCoord;
varying float vWindDirection;
// ShinyTrims (oleada 3): posicion de CLIP interpolada, para el espacio
// PANTALLA del efecto de trim. Es el PRIMER cambio del .vsh de este shader.
// No se usa gl_FragCoord en el .fsh: el traductor GLSL->HLSL NO lo sustituye
// (solo mapea gl_FragColor y gl_FragData[N], ShaderLoadD3D11.cpp:131-140) y
// ademas el origen Y de SV_POSITION (arriba-izquierda) es el contrario del de
// GLSL (abajo-izquierda), asi que copiarlo literal invertiria la direccion del
// barrido en uno de los dos backends. Con el clip interpolado la formula es la
// MISMA en los dos, la Y la controlamos nosotros, y normalizar contra el
// lienzo COMPLETO (render_tile_fullw/fullh) sale gratis -- que es lo que la
// decision (b) del PO exige para que tiles/SSAA/overscan no partan el patron.
// Coste: un interpolador mas en el struct Vars, solo en el pase GLINT. El gate
// GK.1 verifica que no mueve ni un pixel con uTrimFx = 0.
varying vec4 vClip;

uniform vec4 uBlendColor;

// Wind
uniform float uTime; // static
uniform float uWindEnable;
uniform float uWindTerrain;
uniform float uWindSpeed; // static
uniform float uWindStrength;
uniform vec2 uWindDirection; // static
uniform float uWindDirectionalSpeed; // static
uniform float uWindDirectionalStrength;

// TAA
uniform mat4 uTAAMatrix; // static

// GPU Gems 3: Chapter 6
#define PI 3.14159265
float getNoise(float v)
{
	return cos(v * PI) * cos(v * 3.0 * PI) * cos(v * 5.0 * PI) * cos(v * 7.0 * PI) + sin(v * 5.0 * PI) * 0.1;
}

vec3 getWind()
{
	return vec3(
		sin((uTime + in_Position.x * 10.0 + in_Position.y + in_Position.z) * (uWindSpeed / 5.0)) * max(in_Wave.x * uWindTerrain, uWindEnable) * uWindStrength,
		sin((uTime + in_Position.x + in_Position.y * 10.0 + in_Position.z) * (uWindSpeed / 7.5)) * max(in_Wave.x * uWindTerrain, uWindEnable) * uWindStrength,
		sin((uTime + in_Position.x + in_Position.y + in_Position.z * 10.0) * (uWindSpeed / 10.0)) * max(in_Wave.y * uWindTerrain, uWindEnable) * uWindStrength
	);
}

vec3 getWindAngle(vec3 pos)
{
	float strength = dot(pos.xy/16.0, uWindDirection) / dot(uWindDirection, uWindDirection);
	float diroff = getNoise(((uWindDirectionalSpeed - (strength / 3.0) - (pos.z/64.0)) * .075));
	return vec3(uWindDirection * diroff, 0.0) * (1.0 - step(max(in_Wave.x * uWindTerrain, uWindEnable), 0.0)) * uWindDirectionalStrength;
}

void main()
{
	// Wind
	if (max((in_Wave.x + in_Wave.y) * uWindTerrain, uWindEnable) * uWindStrength > 0.0)
	{
		vPosition = (gm_Matrices[MATRIX_WORLD] * vec4(in_Position + getWind(), 1.0)).xyz;
		vPosition += getWindAngle(in_Position);
	}
	else
		vPosition = (gm_Matrices[MATRIX_WORLD] * vec4(in_Position, 1.0)).xyz;
	
	vColor = in_Colour * uBlendColor;
	vTexCoord = in_TextureCoord;
	
	gl_Position = uTAAMatrix * gm_Matrices[MATRIX_PROJECTION] * (gm_Matrices[MATRIX_VIEW] * vec4(vPosition, 1.0));
	vClip = gl_Position;
}
