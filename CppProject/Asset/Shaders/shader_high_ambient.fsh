// Hemisphere ambient: sky-tinted light from above, dimmed bounce from
// below, blended by the G-buffer normal. Replaces the flat ambient clear
// of the diffuse light buffer, so shadowed sides pick up sky color the
// way a Blender world/Blockbench preview does.

varying vec2 vTexCoord;

uniform sampler2D uNormalBuffer;
uniform float uNormalBufferScale;
// HDR multiplier (light buffer is RGBA32F): "Ambient boost" render
// setting -- studio/product presets lift fill light above 1.0 so flat
// promo shots need no manual lights
uniform float uAmbientStrength;
uniform vec4 uAmbientUp;
uniform vec4 uAmbientDown;

vec3 unpackNormal(vec4 c)
{
	return (c.rgb / uNormalBufferScale) * 2.0 - 1.0;
}

void main()
{
	vec3 normal = unpackNormal(texture2D(uNormalBuffer, vTexCoord));
	float up = clamp(normal.z * 0.5 + 0.5, 0.0, 1.0);
	gl_FragColor = vec4(mix(uAmbientDown.rgb, uAmbientUp.rgb, up) * uAmbientStrength, 1.0);
}
