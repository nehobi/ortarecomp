#version 450
// One image onto a target: the finished frame onto the window (optionally
// edge-smoothed, FXAA, and softened like a TV encoder's flicker filter, which
// averages each line with its neighbours, 1/4 1/2 1/4), the options overlay
// over it, and a resolved render target back into its multisampled buffer.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D img;
layout(set = 3, binding = 0) uniform Params {
    vec4 params;        // x: soften (0 or 1), y: one game line in uv, z: FXAA (0 or 1)
};

float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// FXAA, the compact form: find an edge from the brightness of the four
// diagonal neighbours, and where the contrast is high average along the
// edge (two taps near, two further); keep the wider average unless it
// strays outside the neighbourhood's brightness range (it crossed the edge).
vec3 fxaa(vec2 uv)
{
    vec2 px = 1.0 / vec2(textureSize(img, 0));
    vec3 m = texture(img, uv).rgb;
    float lm = luma(m);
    float nw = luma(texture(img, uv + vec2(-px.x, -px.y)).rgb);
    float ne = luma(texture(img, uv + vec2(px.x, -px.y)).rgb);
    float sw = luma(texture(img, uv + vec2(-px.x, px.y)).rgb);
    float se = luma(texture(img, uv + vec2(px.x, px.y)).rgb);
    float lo = min(lm, min(min(nw, ne), min(sw, se)));
    float hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(1.0 / 32.0, hi / 8.0))
        return m;       // flat enough: no edge here

    vec2 dir = vec2((sw + se) - (nw + ne), (nw + sw) - (ne + se));
    float bias = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + bias), -8.0, 8.0) * px;

    vec3 near = 0.5 * (texture(img, uv - dir / 6.0).rgb + texture(img, uv + dir / 6.0).rgb);
    vec3 wide = 0.5 * near + 0.25 * (texture(img, uv - dir / 2.0).rgb + texture(img, uv + dir / 2.0).rgb);
    float lw = luma(wide);
    return lw < lo || lw > hi ? near : wide;
}

void main()
{
    vec4 c = texture(img, v_uv);
    if (params.z != 0.0)
        c.rgb = fxaa(v_uv);
    if (params.x != 0.0) {
        vec2 d = vec2(0.0, params.y);
        c = 0.25 * texture(img, v_uv - d) + 0.5 * c + 0.25 * texture(img, v_uv + d);
    }
    out_color = c;
}
