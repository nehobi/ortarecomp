#version 450
// Direct3D 8 fixed-function texture stages, interpreted from uniforms so one
// shader covers every combination the game sets up. Values are the XDK's:
// D3DTOP_* operations and D3DTA_* arguments.

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
    ivec4 stage_misc[4]; // result arg, has texture, texcoord set, -
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

vec4 tc_for(int set)
{
    if (set == 1) return v_tc1;
    if (set == 2) return v_tc2;
    if (set == 3) return v_tc3;
    return v_tc0;
}

// Fog factor: 1 = no fog. The coordinate is an eye distance, or with table
// mode NONE the factor itself (vertex fog).
float fog_factor()
{
    float d = abs(v_fog);
    if (fog_mode.x == 0) return 1.0;
    if (fog_mode.y == 1) return clamp(exp(-fog_params.z * d), 0.0, 1.0);
    if (fog_mode.y == 2) return clamp(exp(-(fog_params.z * d) * (fog_params.z * d)), 0.0, 1.0);
    if (fog_mode.y == 3) {
        float range = fog_params.y - fog_params.x;
        return range != 0.0 ? clamp((fog_params.y - d) / range, 0.0, 1.0) : 1.0;
    }
    return clamp(v_fog, 0.0, 1.0);
}

vec3 apply_fog(vec3 c)
{
    return fog_mode.x != 0 ? mix(fog_color.rgb, c, fog_factor()) : c;
}

vec4 sample_tex(int s, vec2 uv)
{
    vec4 c;
    if (s == 0) c = texture(tex0, uv);
    else if (s == 1) c = texture(tex1, uv);
    else if (s == 2) c = texture(tex2, uv);
    else c = texture(tex3, uv);
    if (tc_scale[s].z != 0.0) c.a = 1.0;
    if (tc_scale[s].w != 0.0) c.rg = clamp((c.rg * 255.0 - 128.0) / 127.0, -1.0, 1.0);
    return c;
}

vec4 sample_stage(int s)
{
    vec4 tc = tc_for(stage_misc[s].z);
    if (stage_misc[s].y == 0) return vec4(1.0);
    vec2 uv = tc.xy * tc_scale[s].xy;
    return sample_tex(s, uv);
}

vec4 arg(int a, vec4 current, vec4 tex, vec4 temp)
{
    vec4 v;
    int sel = a & 0xF;
    if (sel == 0) v = v_diffuse;
    else if (sel == 1) v = current;
    else if (sel == 2) v = tex;
    else if (sel == 3) v = tfactor;
    else if (sel == 4) v = v_specular;
    else v = temp;
    if ((a & 0x10) != 0) v = vec4(1.0) - v;     // D3DTA_COMPLEMENT
    if ((a & 0x20) != 0) v = vec4(v.a);         // D3DTA_ALPHAREPLICATE
    return v;
}

vec4 op(int o, vec4 a1, vec4 a2, vec4 a0, vec4 current, vec4 tex)
{
    if (o == 2)  return a1;                                  // SELECTARG1
    if (o == 3)  return a2;                                  // SELECTARG2
    if (o == 4)  return a1 * a2;                             // MODULATE
    if (o == 5)  return a1 * a2 * 2.0;                       // MODULATE2X
    if (o == 6)  return a1 * a2 * 4.0;                       // MODULATE4X
    if (o == 7)  return a1 + a2;                             // ADD
    if (o == 8)  return a1 + a2 - 0.5;                       // ADDSIGNED
    if (o == 9)  return (a1 + a2 - 0.5) * 2.0;               // ADDSIGNED2X
    if (o == 10) return a1 - a2;                             // SUBTRACT
    if (o == 11) return a1 + a2 - a1 * a2;                   // ADDSMOOTH
    if (o == 12) return mix(a2, a1, v_diffuse.a);            // BLENDDIFFUSEALPHA
    // The Xbox numbers these four differently from PC Direct3D.
    if (o == 13) return mix(a2, a1, current.a);              // BLENDCURRENTALPHA
    if (o == 14) return mix(a2, a1, tex.a);                  // BLENDTEXTUREALPHA
    if (o == 15) return mix(a2, a1, tfactor.a);              // BLENDFACTORALPHA
    if (o == 16) return a1 + a2 * (1.0 - tex.a);             // BLENDTEXTUREALPHAPM
    if (o == 18) return vec4(a1.rgb + a1.a * a2.rgb, a1.a);  // MODULATEALPHA_ADDCOLOR
    if (o == 19) return vec4(a1.rgb * a2.rgb + a1.a, a1.a);  // MODULATECOLOR_ADDALPHA
    if (o == 20) return vec4(a1.rgb + (1.0 - a1.a) * a2.rgb, a1.a);
    if (o == 21) return vec4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);
    if (o == 22) return vec4(clamp(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5), 0.0, 1.0)); // DOTPRODUCT3
    if (o == 23) return a0 + a1 * a2;                        // MULTIPLYADD
    if (o == 24) return mix(a2, a1, a0);                     // LERP
    return a1;
}

bool alpha_pass(float a)
{
    float ref = float(alpha_test.z) / 255.0;
    int f = alpha_test.y;               // GL-valued: 0x200 NEVER .. 0x207 ALWAYS
    if (f == 0x200) return false;
    if (f == 0x201) return a < ref;
    if (f == 0x202) return a == ref;
    if (f == 0x203) return a <= ref;
    if (f == 0x204) return a > ref;
    if (f == 0x205) return a != ref;
    if (f == 0x206) return a >= ref;
    return true;
}

// ── Pixel shaders: NV2A register combiners ────────────────────────────
// Registers: 0 zero, 1 C0, 2 C1, 3 fog, 4 V0 (diffuse), 5 V1 (specular),
// 8-11 T0-T3, 12 R0, 13 R1; in the final combiner also 14 V1+R0, 15 E*F.

vec4 ps_reg[16];

int ps_word(ivec4 v[2], int i) { return v[i >> 2][i & 3]; }

// One input byte: register (bits 0-3), channel (bit 4: in the RGB portion
// alpha replicated instead of RGB; in the alpha portion alpha instead of
// blue), mapping (bits 5-7).
float ps_map(float x, int m)
{
    if (m == 0) return max(x, 0.0);                       // UNSIGNED_IDENTITY
    if (m == 1) return 1.0 - clamp(x, 0.0, 1.0);          // UNSIGNED_INVERT
    if (m == 2) return 2.0 * max(x, 0.0) - 1.0;           // EXPAND_NORMAL
    if (m == 3) return -2.0 * max(x, 0.0) + 1.0;          // EXPAND_NEGATE
    if (m == 4) return max(x, 0.0) - 0.5;                 // HALFBIAS_NORMAL
    if (m == 5) return -max(x, 0.0) + 0.5;                // HALFBIAS_NEGATE
    if (m == 6) return x;                                 // SIGNED_IDENTITY
    return -x;                                            // SIGNED_NEGATE
}

vec3 ps_in_rgb(int b)
{
    vec4 r = ps_reg[b & 15];
    vec3 v = (b & 16) != 0 ? vec3(r.a) : r.rgb;
    int m = (b >> 5) & 7;
    return vec3(ps_map(v.r, m), ps_map(v.g, m), ps_map(v.b, m));
}

float ps_in_alpha(int b)
{
    vec4 r = ps_reg[b & 15];
    return ps_map((b & 16) != 0 ? r.a : r.b, (b >> 5) & 7);   // channel: blue or alpha
}

vec4 ps_scale(vec4 v, int mapping)
{
    if (mapping == 1) v = v - 0.5;                        // BIAS
    else if (mapping == 2) v = v * 2.0;                   // SHIFTLEFT_1
    else if (mapping == 3) v = (v - 0.5) * 2.0;           // SHIFTLEFT_1_BIAS
    else if (mapping == 4) v = v * 4.0;                   // SHIFTLEFT_2
    else if (mapping == 6) v = v * 0.5;                   // SHIFTRIGHT_1
    return clamp(v, -1.0, 1.0);
}

vec4 ps_texture(int s, int mode)
{
    if (mode == 0) return vec4(0.0, 0.0, 0.0, 1.0);       // no texture
    if (mode == 4) return clamp(tc_for(s), 0.0, 1.0);     // PASSTHRU
    if (stage_misc[s].y == 0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec4 tc = tc_for(s);
    // PROJECT2D divides by q: how a vertex program hands over screen
    // coordinates (x, y, -, w) into a copy of the frame. q = 0 is an
    // output the program never wrote.
    if (mode == 1 && tc.w != 0.0) tc.xy /= tc.w;
    vec2 uv = tc.xy * tc_scale[s].xy;
    if ((mode == 6 || mode == 7) && s > 0) {
        // BUMPENVMAP(_LUM): the previous stage's texel (du, dv), through
        // this stage's bump matrix, offsets these coordinates:
        // u += M00 du + M10 dv, v += M01 du + M11 dv.
        vec4 prev = ps_reg[8 + s - 1];
        vec4 m = bump_mat[s];
        uv += vec2(m.x * prev.r + m.w * prev.g, m.y * prev.r + m.z * prev.g);
        vec4 c = sample_tex(s, uv);
        if (mode == 7)          // luminance from the previous texel's blue
            c.rgb *= clamp(prev.b * bump_lum[s].x + bump_lum[s].y, 0.0, 1.0);
        return c;
    }
    return sample_tex(s, uv);
}

vec4 pixel_shader()
{
    int count = ps_final.z & 0xFF;
    bool mux_msb = (ps_final.z & 0x100) != 0;
    bool unique_c0 = (ps_final.z & 0x1000) != 0;
    bool unique_c1 = (ps_final.z & 0x10000) != 0;

    for (int i = 0; i < 16; i++) ps_reg[i] = vec4(0.0);
    ps_reg[3] = vec4(fog_color.rgb, fog_factor());
    ps_reg[4] = v_diffuse;
    ps_reg[5] = v_specular;
    for (int s = 0; s < 4; s++)
        ps_reg[8 + s] = ps_texture(s, (ps_final.w >> (5 * s)) & 31);
    ps_reg[12] = vec4(0.0, 0.0, 0.0, ps_reg[8].a);        // R0 starts with T0's alpha

    for (int i = 0; i < 8; i++) {
        if (i >= count) break;
        ps_reg[1] = ps_c0[unique_c0 ? i : 0];
        ps_reg[2] = ps_c1[unique_c1 ? i : 0];

        // RGB portion
        int ri = ps_word(ps_rgb_in, i), ro = ps_word(ps_rgb_out, i);
        vec3 A = ps_in_rgb(ri >> 24), B = ps_in_rgb(ri >> 16), C = ps_in_rgb(ri >> 8), D = ps_in_rgb(ri);
        int rf = ro >> 12;
        vec3 ab = (rf & 2) != 0 ? vec3(dot(A, B)) : A * B;
        vec3 cd = (rf & 1) != 0 ? vec3(dot(C, D)) : C * D;
        bool mux_r0 = mux_msb ? ps_reg[12].a >= 0.5 : (int(ps_reg[12].a * 255.0 + 0.5) & 1) != 0;
        vec3 sum = (rf & 4) != 0 ? (mux_r0 ? cd : ab) : ab + cd;
        int rmap = (rf >> 3) & 7;
        ab = ps_scale(vec4(ab, 0.0), rmap).rgb;
        cd = ps_scale(vec4(cd, 0.0), rmap).rgb;
        sum = ps_scale(vec4(sum, 0.0), rmap).rgb;

        // Alpha portion
        int ai = ps_word(ps_a_in, i), ao = ps_word(ps_a_out, i);
        float aA = ps_in_alpha(ai >> 24), aB = ps_in_alpha(ai >> 16);
        float aC = ps_in_alpha(ai >> 8), aD = ps_in_alpha(ai);
        int af = ao >> 12;
        float aab = aA * aB, acd = aC * aD;
        float asum = (af & 4) != 0 ? (mux_r0 ? acd : aab) : aab + acd;
        int amap = (af >> 3) & 7;
        aab = ps_scale(vec4(aab), amap).x;
        acd = ps_scale(vec4(acd), amap).x;
        asum = ps_scale(vec4(asum), amap).x;

        // Writes, after everything in the stage has been read.
        int d;
        d = ro & 15;         if (d != 0) ps_reg[d].rgb = cd;
        d = (ro >> 4) & 15;  if (d != 0) ps_reg[d].rgb = ab;
        d = (ro >> 8) & 15;  if (d != 0) ps_reg[d].rgb = sum;
        if ((rf & 0x40) != 0 && (ro & 15) != 0) ps_reg[ro & 15].a = cd.b;        // CD blue to alpha
        if ((rf & 0x80) != 0 && ((ro >> 4) & 15) != 0) ps_reg[(ro >> 4) & 15].a = ab.b;
        d = ao & 15;         if (d != 0) ps_reg[d].a = acd;
        d = (ao >> 4) & 15;  if (d != 0) ps_reg[d].a = aab;
        d = (ao >> 8) & 15;  if (d != 0) ps_reg[d].a = asum;
    }

    // Final combiner: A*B + (1-A)*C + D; alpha from G.
    if (ps_final.x == 0 && ps_final.y == 0) {
        vec4 r = clamp(ps_reg[12], 0.0, 1.0);
        return vec4(apply_fog(r.rgb), r.a);
    }
    ps_reg[1] = ps_fc0;
    ps_reg[2] = ps_fc1;
    int fa = ps_final.x, fe = ps_final.y, fflags = fe & 0xFF;
    vec4 v1 = ps_reg[5], r0 = ps_reg[12];
    if ((fflags & 0x40) != 0) v1 = 1.0 - clamp(v1, 0.0, 1.0);   // COMPLEMENT_V1
    if ((fflags & 0x20) != 0) r0 = 1.0 - clamp(r0, 0.0, 1.0);   // COMPLEMENT_R0
    vec4 s = v1 + r0;
    if ((fflags & 0x80) != 0) s = clamp(s, 0.0, 1.0);           // CLAMP_SUM
    ps_reg[14] = vec4(s.rgb, 0.0);
    vec3 E = ps_in_rgb(fe >> 24), F = ps_in_rgb(fe >> 16);
    ps_reg[15] = vec4(E * F, 0.0);
    vec3 A = clamp(ps_in_rgb(fa >> 24), 0.0, 1.0), B = clamp(ps_in_rgb(fa >> 16), 0.0, 1.0);
    vec3 C = clamp(ps_in_rgb(fa >> 8), 0.0, 1.0), D = clamp(ps_in_rgb(fa), 0.0, 1.0);
    float G = clamp(ps_in_alpha(fe >> 8), 0.0, 1.0);
    return clamp(vec4(A * B + (1.0 - A) * C + D, G), 0.0, 1.0);
}

void main()
{
    if ((ps_final.z & 0xFF) != 0) {
        vec4 c = pixel_shader();
        if (alpha_test.x != 0 && !alpha_pass(c.a))
            discard;
        out_color = c;
        return;
    }

    vec4 current = v_diffuse;
    vec4 temp = vec4(0.0);

    for (int s = 0; s < 4; s++) {
        int cop = color_op[s].x;
        if (cop <= 1) break;                         // D3DTOP_DISABLE
        vec4 tex = sample_stage(s);
        vec4 c = op(cop, arg(color_op[s].y, current, tex, temp), arg(color_op[s].z, current, tex, temp),
                    arg(color_op[s].w, current, tex, temp), current, tex);
        float a = current.a;
        int aop = alpha_op[s].x;
        if (aop > 1)
            a = op(aop, arg(alpha_op[s].y, current, tex, temp), arg(alpha_op[s].z, current, tex, temp),
                   arg(alpha_op[s].w, current, tex, temp), current, tex).a;
        vec4 result = clamp(vec4(c.rgb, a), 0.0, 1.0);
        if (stage_misc[s].x == 5) temp = result;     // D3DTA_TEMP
        else current = result;
    }
    if (alpha_test.w != 0)
        current.rgb = clamp(current.rgb + v_specular.rgb, 0.0, 1.0);
    current.rgb = apply_fog(current.rgb);
    if (alpha_test.x != 0 && !alpha_pass(current.a))
        discard;
    out_color = current;
}
