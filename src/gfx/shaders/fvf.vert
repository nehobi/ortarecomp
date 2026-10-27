#version 450
// Direct3D fixed-function vertices on the GPU: what fetch_fvf and
// light_vertex in src/guest/d3d_hle.c do per vertex (they stay the
// reference), from the game's raw FVF stream bytes. Per draw, the CPU
// gathers the state into the uniforms: layout, matrices, texture stages,
// material and lights.

layout(set = 0, binding = 0, std430) readonly buffer Streams { uint blob[]; };

layout(set = 1, binding = 0, std140) uniform Draw {
    ivec4 layout0;       // byte base, stride, flags, coordinate sets in the FVF
    ivec4 offs;          // normal, diffuse, specular offsets
    ivec4 tc_off;        // per coordinate set: offset
    ivec4 tc_n;          //   and components
    ivec4 stage_set;     // per stage: D3DTSS_TEXCOORDINDEX set
    ivec4 stage_gen;     //   generation: 0 none, 1 camera-space normal, 2 position
    ivec4 stage_tt;      //   D3DTSS_TEXTURETRANSFORMFLAGS
    ivec4 light_info;    // lights, COLORVERTEX
    ivec4 mat_src;       // material sources: diffuse, ambient, specular, emissive
    vec4  rt;            // target width, height (pretransformed vertices)
    vec4  wvp[4];        // rows of world * view * projection (row vectors)
    vec4  wv[4];
    vec4  world[4];
    vec4  tex[16];       // the four stages' texture matrices, rows
    vec4  material[4];   // diffuse, ambient, specular, emissive
    vec4  ambient;       // D3DRS_AMBIENT
    vec4  light[56];     // 8 lights x 7: (type, range, falloff, -), diffuse,
                         // ambient, position, direction (unit for directional:
                         // to the light), attenuation (a0, a1, a2, |direction|),
                         // cone (cos theta/2, cos phi/2)
};

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) out vec4 v_tc0;
layout(location = 3) out vec4 v_tc1;
layout(location = 4) out vec4 v_tc2;
layout(location = 5) out vec4 v_tc3;
layout(location = 6) out float v_fog;

const int F_PRE = 1, F_DIFFUSE = 2, F_SPECULAR = 4, F_NORMAL = 8, F_LIT = 16, F_FOGTABLE = 32;

uint rd8(uint a)  { return (blob[a >> 2] >> ((a & 3u) * 8u)) & 0xFFu; }
uint rd32(uint a)
{
    if ((a & 3u) == 0u) return blob[a >> 2];
    return rd8(a) | (rd8(a + 1u) << 8) | (rd8(a + 2u) << 16) | (rd8(a + 3u) << 24);
}
float f32(uint a) { return uintBitsToFloat(rd32(a)); }

// A D3DCOLOR (B, G, R, A in memory) as RGBA.
vec4 color(uint c) { return vec4(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu), float(c & 0xFFu), float(c >> 24)) / 255.0; }

// pack_color: NaN as 0, clamped, to 8 bits.
vec4 quantize(vec4 c)
{
    c = mix(clamp(c, 0.0, 1.0), vec4(0.0), isnan(c));
    return floor(c * 255.0 + 0.5) / 255.0;
}

// p (x, y, z, 1) times a matrix given as rows, added in the CPU's order.
vec4 xform(vec3 p, vec4 r0, vec4 r1, vec4 r2, vec4 r3)
{
    return ((p.x * r0 + p.y * r1) + p.z * r2) + r3;
}

vec3 xform3(vec3 n, vec4 r0, vec4 r1, vec4 r2)
{
    return ((n.x * r0.xyz + n.y * r1.xyz) + n.z * r2.xyz);
}

// texture_transform: through the stage's matrix, the first missing input
// component 1; PROJECTED divides by the last output; q kept only for an
// undivided fourth.
vec4 tex_transform(int s, int n, vec4 tc)
{
    int flags = stage_tt[s], count = flags & 0xFF;
    vec4 in4 = vec4(0.0), o;

    if (count == 0) return tc;
    for (int c = 0; c < n && c < 4; c++) in4[c] = tc[c];
    if (n < 4) in4[n] = 1.0;
    o = ((in4.x * tex[s * 4] + in4.y * tex[s * 4 + 1]) + in4.z * tex[s * 4 + 2]) + in4.w * tex[s * 4 + 3];
    if ((flags & 256) != 0 && count >= 2 && count <= 4 && o[count - 1] != 0.0)
        for (int c = 0; c + 1 < count; c++) o[c] /= o[count - 1];
    if (count != 4 || (flags & 256) != 0) o.w = 0.0;
    return o;
}

vec4 lighting(vec3 p, vec3 n, vec4 vd, vec4 vs, bool hd, bool hs)
{
    vec4 mat[4] = vec4[4](material[0], material[1], material[2], material[3]);
    bool cv = light_info.y != 0;
    vec3 o;

    for (int k = 0; k < 4; k++) {
        int src = mat_src[k];
        if (cv && src == 1 && hd) mat[k] = vd;
        else if (cv && src == 2 && hs) mat[k] = vs;
    }
    o = mat[3].rgb + ambient.rgb * mat[1].rgb;
    for (int i = 0; i < light_info.x && i < 8; i++) {
        vec4 info = light[i * 7], att4 = light[i * 7 + 5], cone = light[i * 7 + 6];
        vec3 ldir;
        float att = 1.0;
        if (info.x == 3.0) {
            ldir = light[i * 7 + 4].xyz;
        } else {
            vec3 dv = light[i * 7 + 3].xyz - p;
            float d2 = (dv.x * dv.x + dv.y * dv.y) + dv.z * dv.z, d = sqrt(d2);
            if (info.y > 0.0 && d > info.y) continue;
            ldir = d > 0.0 ? dv / d : dv;
            att = (att4.x + att4.y * d) + att4.z * d2;
            att = att > 0.0 ? 1.0 / att : 1.0;
            if (info.x == 2.0) {
                vec3 dir = light[i * 7 + 4].xyz;
                float rho = 0.0;
                if (att4.w > 0.0) {
                    rho -= ldir.x * dir.x / att4.w;
                    rho -= ldir.y * dir.y / att4.w;
                    rho -= ldir.z * dir.z / att4.w;
                }
                if (rho <= cone.y) att = 0.0;
                else if (rho < cone.x && cone.x > cone.y) att *= pow((rho - cone.y) / (cone.x - cone.y), info.z);
            }
        }
        float ndotl = max((n.x * ldir.x + n.y * ldir.y) + n.z * ldir.z, 0.0);
        o += att * (light[i * 7 + 2].rgb * mat[1].rgb + ndotl * light[i * 7 + 1].rgb * mat[0].rgb);
    }
    return vec4(o, mat[0].a);
}

void main()
{
    int flags = layout0.z;
    bool pre = (flags & F_PRE) != 0, hd = (flags & F_DIFFUSE) != 0, hs = (flags & F_SPECULAR) != 0;
    bool hn = (flags & F_NORMAL) != 0;
    uint v = uint(layout0.x) + uint(gl_VertexIndex) * uint(layout0.y);
    vec3 p = vec3(f32(v), f32(v + 4u), f32(v + 8u));
    vec3 nrm = hn ? vec3(f32(v + uint(offs.x)), f32(v + uint(offs.x) + 4u), f32(v + uint(offs.x) + 8u)) : vec3(0.0);
    uint dw = hd ? rd32(v + uint(offs.y)) : 0xFFFFFFFFu, sw = hs ? rd32(v + uint(offs.z)) : 0u;
    vec4 diffuse = color(dw), specular = color(sw);
    vec3 e = xform(p, wv[0], wv[1], wv[2], wv[3]).xyz;

    if (pre) {
        float rhw = f32(v + 12u), w = rhw != 0.0 ? 1.0 / rhw : 1.0;
        gl_Position = vec4((p.x / rt.x * 2.0 - 1.0) * w, (1.0 - p.y / rt.y * 2.0) * w, p.z * w, w);
    } else {
        gl_Position = xform(p, wvp[0], wvp[1], wvp[2], wvp[3]);
    }
    if ((flags & F_LIT) != 0) {
        vec3 wp = xform(p, world[0], world[1], world[2], world[3]).xyz, wn = vec3(0.0);
        if (hn) {
            wn = xform3(nrm, world[0], world[1], world[2]);
            float len = sqrt((wn.x * wn.x + wn.y * wn.y) + wn.z * wn.z);
            if (len > 0.0) wn /= len;
        }
        diffuse = quantize(lighting(wp, wn, diffuse, specular, hd, hs));
        specular = vec4(0.0);
    }
    v_diffuse = diffuse;
    v_specular = specular;
    // Fog: eye depth for table fog, else the specular alpha (lit: 0).
    v_fog = (!pre && (flags & F_FOGTABLE) != 0) ? e.z : (hs ? specular.a : 1.0);

    vec4 tc[4];
    for (int s = 0; s < 4; s++) {
        int set = stage_set[s], gen = stage_gen[s];
        vec4 t = vec4(0.0);
        if (gen != 0) {
            vec3 g = vec3(0.0);
            if (gen == 2) {
                g = e;
            } else if (hn) {
                g = xform3(nrm, wv[0], wv[1], wv[2]);
                float len = sqrt((g.x * g.x + g.y * g.y) + g.z * g.z);
                if (len > 0.0) g /= len;
            }
            tc[s] = tex_transform(s, 3, vec4(g, 0.0));
            continue;
        }
        if (set < layout0.w && set < 4) {
            uint a = v + uint(tc_off[set]);
            for (int c = 0; c < tc_n[set]; c++) t[c] = f32(a + 4u * uint(c));
            if (!pre) t = tex_transform(s, tc_n[set], t);
        }
        tc[s] = t;
    }
    v_tc0 = tc[0];
    v_tc1 = tc[1];
    v_tc2 = tc[2];
    v_tc3 = tc[3];
}
