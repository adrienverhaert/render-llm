/// shader_high_material

attribute vec3 in_Position;
attribute vec3 in_Normal;
attribute vec4 in_Colour;
attribute vec2 in_TextureCoord;
attribute vec4 in_Wave;
attribute vec3 in_Tangent;

varying vec3 vPosition;
varying vec3 vNormal;
varying vec3 vTangent;
varying mat3 vTBN;
varying vec4 vColor;
varying vec4 vCustom;
varying vec2 vTexCoord;
varying float vTime;
varying float vWindDirection;
// ShinyTrims (oleada 4b): posicion de CLIP interpolada, para el espacio
// PANTALLA de la ganancia emisiva del trim. Copia literal de lo que la oleada 3
// hizo en shader_high_glint.vsh y por el mismo motivo: el .fsh no puede usar
// gl_FragCoord (el traductor GLSL->HLSL solo mapea gl_FragColor y gl_FragData[N],
// ShaderLoadD3D11.cpp:131-140, y el origen Y de SV_POSITION esta invertido
// respecto al de GLSL). Con el clip interpolado la formula es la misma en los
// dos backends y normalizar contra el lienzo COMPLETO sale gratis.
// Coste: un interpolador mas, esta vez en el pase PRINCIPAL. El gate GF mide
// que con el trim apagado no mueve ni un pixel.
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
	
	vNormal = (gm_Matrices[MATRIX_WORLD] * vec4(in_Normal, 0.0)).xyz;
	vTangent = (gm_Matrices[MATRIX_WORLD] * vec4(in_Tangent, 0.0)).xyz;
	vTangent = normalize(vTangent - dot(vTangent, vNormal) * vNormal);
	vTBN = mat3(vTangent, cross(vTangent, vNormal), vNormal);
	
	vColor = in_Colour * uBlendColor;
	vTexCoord = in_TextureCoord;
	vTime = uTime;
	vCustom = in_Wave;
	
	gl_Position = uTAAMatrix * gm_Matrices[MATRIX_PROJECTION] * (gm_Matrices[MATRIX_VIEW] * vec4(vPosition, 1.0));
	vClip = gl_Position;
}
