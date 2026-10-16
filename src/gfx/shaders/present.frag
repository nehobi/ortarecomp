#version 450
// Present pass: the Xbox's display gamma ramp (D3DDevice_SetGammaRamp),
// which JSRF uses for its fades. lut holds the 256-entry red, green and blue
// ramps in its r, g and b channels.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D frame;
layout(set = 2, binding = 1) uniform sampler2D lut;

float ramp(float v, int ch)
{
    float x = (floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) + 0.5) / 256.0;
    return texture(lut, vec2(x, 0.5))[ch];
}

void main()
{
    vec3 c = texture(frame, v_uv).rgb;
    out_color = vec4(ramp(c.r, 0), ramp(c.g, 1), ramp(c.b, 2), 1.0);
}
