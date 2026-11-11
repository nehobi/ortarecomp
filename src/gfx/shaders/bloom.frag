#version 450
// Bloom (renderer.c, bloom_pass), on the scene before the HUD is drawn:
//   mode 0: the bright parts, at a quarter of the target's size (four
//           bilinear taps, so 16 texels each), past a soft threshold;
//   mode 1: a 9-tap Gaussian blur along step (one direction a pass);
//   mode 2: the blurred light, scaled, added onto the scene (the pipeline
//           blends ONE + ONE).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D img;
layout(set = 3, binding = 0) uniform Params {
    vec4 params;        // x: mode, y: threshold (0) or strength (2), zw: step in uv
};

void main()
{
    int mode = int(params.x + 0.5);

    if (mode == 0) {
        vec2 px = 1.0 / vec2(textureSize(img, 0));
        vec3 c = 0.25 * (texture(img, v_uv + vec2(-px.x, -px.y)).rgb + texture(img, v_uv + vec2(px.x, -px.y)).rgb +
                         texture(img, v_uv + vec2(-px.x, px.y)).rgb + texture(img, v_uv + vec2(px.x, px.y)).rgb);
        float hi = max(c.r, max(c.g, c.b));
        // Soft knee: nothing below the threshold, rising to full over 0.1.
        float k = clamp((hi - params.y) / 0.1, 0.0, 1.0);
        out_color = vec4(c * k * k, 1.0);
    } else if (mode == 1) {
        const float w[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
        vec3 c = texture(img, v_uv).rgb * w[0];
        for (int i = 1; i < 5; i++)
            c += (texture(img, v_uv + params.zw * float(i)).rgb + texture(img, v_uv - params.zw * float(i)).rgb) * w[i];
        out_color = vec4(c, 1.0);
    } else {
        out_color = vec4(texture(img, v_uv).rgb * params.y, 0.0);
    }
}
