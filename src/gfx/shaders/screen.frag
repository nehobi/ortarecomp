#version 450
// One image onto a target: the finished frame onto the window (optionally
// softened like a TV encoder's flicker filter, which averages each line with
// its neighbours, 1/4 1/2 1/4), the options overlay over it, and a resolved
// render target back into its multisampled buffer.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D img;
layout(set = 3, binding = 0) uniform Params {
    vec4 params;        // x: soften (0 or 1), y: one game line in uv
};

void main()
{
    vec4 c = texture(img, v_uv);
    if (params.x != 0.0) {
        vec2 d = vec2(0.0, params.y);
        c = 0.25 * texture(img, v_uv - d) + 0.5 * c + 0.25 * texture(img, v_uv + d);
    }
    out_color = c;
}
