#version 450
// A test stand-in for ff.frag (PDO_TEST_SIMPLE_FS=1): the same inputs, the
// cheapest output, to tell how much of the GPU time is ff.frag itself.
// Pictures are wrong with it on.

layout(location = 0) in vec4 v_diffuse;
layout(location = 1) in vec4 v_specular;
layout(location = 2) in vec4 v_tc0;
layout(location = 3) in vec4 v_tc1;
layout(location = 4) in vec4 v_tc2;
layout(location = 5) in vec4 v_tc3;
layout(location = 6) in float v_fog;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D tex3;

layout(set = 3, binding = 0, std140) uniform FF {
    ivec4 color_op[4];   // op, arg1, arg2, arg0
    ivec4 alpha_op[4];   // op, arg1, arg2, arg0
    ivec4 stage_misc[4]; // result arg, has texture, texcoord set, shadow map
                         // (8 | D3DCMP_* - 0x200, + 16 for 24-bit depth)
    vec4  tfactor;
    ivec4 alpha_test;    // enable, func, ref (0-255), specular enable
    ivec4 ps_rgb_in[2];  // pixel shader (register combiners), 8 stages each
    ivec4 ps_rgb_out[2];
    ivec4 ps_a_in[2];
    ivec4 ps_a_out[2];
    vec4  ps_c0[8];
    vec4  ps_c1[8];
    ivec4 ps_final;      // final ABCD, final EFG, PSCombinerCount, texture modes
    vec4  ps_fc0;
    vec4  ps_fc1;
    vec4  tc_scale[4];   // xy: texel -> normalised for linear textures; z: no alpha;
                         // w: red and green signed (V8U8), stored offset by 128
    ivec4 fog_mode;      // enable, D3DFOGMODE (0 NONE, 1 EXP, 2 EXP2, 3 LINEAR)
    vec4  fog_color;
    vec4  fog_params;    // start, end, density
    vec4  bump_mat[4];   // BUMPENVMAT00, 01, 11, 10
    vec4  bump_lum[4];   // BUMPENVLSCALE, BUMPENVLOFFSET
};

void main()
{
    vec4 c = v_diffuse * texture(tex0, v_tc0.xy * (tc_scale[0].x != 0.0 ? tc_scale[0].xy : vec2(1.0)));
    if (alpha_test.x != 0 && c.a * 255.0 < float(alpha_test.z))
        discard;
    out_color = c;
}
