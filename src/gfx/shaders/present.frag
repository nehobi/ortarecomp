#version 450
// Present pass: the Xbox's display gamma ramp (D3DDevice_SetGammaRamp),
// which the game uses for its fades, then the player's picture settings (F1,
// Picture): they touch only the finished frame. lut holds the 256-entry red,
// green and blue ramps in its r, g and b channels.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D frame;
layout(set = 2, binding = 1) uniform sampler2D lut;
// x: CRT look (1) or not, y: brightness (added), z: contrast (factor),
// w: gamma (exponent 1 / gamma); pic2.x: saturation (factor).
layout(set = 3, binding = 0) uniform Picture { vec4 pic; vec4 pic2; };

float ramp(float v, int ch)
{
    float x = (floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) + 0.5) / 256.0;
    return texture(lut, vec2(x, 0.5))[ch];
}

void main()
{
    vec3 c = texture(frame, v_uv).rgb;
    c = vec3(ramp(c.r, 0), ramp(c.g, 1), ramp(c.b, 2));
    // The game was mastered for CRT TVs, darker and punchier than a PC display
    // (gamma about 2.5 against sRGB's 2.2).
    if (pic.x != 0.0) c = pow(c, vec3(2.5 / 2.2));
    c = pow(c, vec3(pic.w));
    c = (c - 0.5) * pic.z + 0.5 + pic.y;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, pic2.x);
    out_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
