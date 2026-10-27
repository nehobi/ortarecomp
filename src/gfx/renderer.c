/* SDL3 GPU renderer for the Direct3D replacement. See renderer.h. */

#include "gfx/renderer.h"

#include "audio/mixer.h"
#include "config.h"
#include "gfx/overlay.h"

#include "gfx/shaders_spv.h"
#include "gfx/xtex.h"

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_TITLE   "Orta Recomp"
#define MAX_PIPELINES  1024
#define MAX_SAMPLERS   64
#define MAX_TEXTURES   4096

extern ptrdiff_t g_xbox_mem_offset;
extern void pdo_dump_threads(void);   /* src/guest/crash_report.c: F11 */
extern void pdo_dump_guest_waits(void);  /* src/guest/wait_trace.c */

static SDL_GPUDevice *dev;
static SDL_Window *window;
static SDL_ThreadID window_thread;
static bool have_swapchain;
static SDL_GPUTexture *frame_color, *frame_depth, *white;
static SDL_GPUTextureFormat depth_format;
static int frame_w, frame_h;
static uint64_t frames;
static SDL_GPUShader *vs, *fs, *vsh_vs, *fvf_vs;

/* Overlay settings in force: every render target is rscale times the game's
 * size (viewports and copies scaled to match), drawn with nsamples samples
 * into a multisampled buffer (ms) that resolves into the texture the game
 * samples. A target whose resolved texture was written directly (a copy, a
 * new target) is stale: its multisampled buffer is seeded from it before the
 * next draw. */
static int rscale = 1, nsamples = 1;
static SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
static SDL_GPUTexture *frame_ms;
static bool frame_stale;
static volatile bool paused;        /* the overlay menu holds the game */

/* Window pass: the frame (aspect, softening) and the overlay over it; and the
 * seed pass that copies a resolved target back into its multisampled one. */
static SDL_GPUGraphicsPipeline *screen_pipeline, *overlay_pipeline, *seed_pipeline;
static SDL_GPUSampler *linear;
static SDL_GPUTexture *overlay_tex;

/* Present pass: frame -> gamma ramp -> output, then output -> window. */
static SDL_GPUTexture *output, *gamma_lut;
static SDL_GPUGraphicsPipeline *present_pipeline;
static SDL_GPUSampler *nearest;
static uint8_t gamma_rgba[256 * 4];
static bool gamma_dirty = true;

static const char *shot_dir;
static int shot_every = 1;

/* F11 dumps every thread's stack (hangs). F12 (or PDO_CAPTURE=frame): log every command of the next frame
 * ("capture:" lines) and save it as capture_<frame>.bmp in the working
 * directory. */
static uint64_t capture_frame = UINT64_MAX;

/* ── Frame recording ──────────────────────────────────────────────────── */

enum { CMD_DRAW, CMD_CLEAR, CMD_TARGET, CMD_COPY };

typedef struct command {
    uint8_t  kind;
    /* target (surf), copy (surf to surf2, rect: x, y, w, h, dx, dy) */
    pdo_surface surf, surf2;
    int      rect[6];
    /* clear */
    uint32_t clear_flags, clear_color, clear_stencil;
    float    clear_z;
    /* draw */
    pdo_draw draw;
    uint32_t first_index, index_count;
    /* a vertex-program draw on the GPU (pdo_gfx_draw_vsh); has_cpu: the CPU
     * interpreter's vertices are recorded too, at first_index */
    bool     vsh, has_cpu;
    int32_t  fvf;               /* or a fixed-function one: its uniforms in fvf_us, else -1 */
    uint32_t vsh_first_index, vsh_index_count;
    struct vsh_uniforms {
        int32_t info[4];                /* program base, count, constant base, - */
        int32_t inputs[16][4];          /* byte base in the stream blob, stride, offset, type */
        float   scale[4], offset[4];
    } vu;
} command;

static pdo_vertex *verts;
static uint32_t nverts, cap_verts;
static uint32_t *indices;
static uint32_t nindices, cap_indices;
static command *cmds;
static uint32_t ncmds, cap_cmds;

/* Vertex-program draws: this frame's stream bytes and constant sets, and
 * every program compiled so far (kept, uploaded again when one is added). */
static uint8_t *blob;
static uint32_t nblob, cap_blob;
static float (*vconsts)[4];
static uint32_t nvconsts, cap_vconsts;
static int32_t last_consts = -1;
static uint32_t *progs;
static uint32_t nprogs, cap_progs;
static bool progs_dirty;
static bool cpu_vsh;        /* captures only: GPU-vertex draws drawn from the CPU's vertices */
static pdo_fvf_uniforms *fvf_us;
static uint32_t nfvf_us, cap_fvf_us;

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return p;
    while (*cap < need)
        *cap = *cap ? *cap * 2 : 1024;
    return SDL_realloc(p, (size_t)*cap * elem);
}

/* Forget the frame's commands and what they recorded. */
static void drop_frame(void)
{
    nverts = nindices = ncmds = 0;
    nblob = nvconsts = nfvf_us = 0;
    last_consts = -1;
}

static command *new_command(void)
{
    cmds = grow(cmds, &cap_cmds, ncmds + 1, sizeof *cmds);
    memset(&cmds[ncmds], 0, sizeof *cmds);
    cmds[ncmds].fvf = -1;
    return &cmds[ncmds++];
}

/* Bytes the vertex shaders read, 4-aligned with 8 bytes of slack past the
 * end for their word reads. Returns where they start. */
static uint32_t add_blob(const uint8_t *data, uint32_t n)
{
    uint32_t at;

    nblob = (nblob + 3) & ~3u;
    blob = grow(blob, &cap_blob, nblob + n + 8, 1);
    memcpy(blob + nblob, data, n);
    at = nblob;
    nblob += n;
    memset(blob + nblob, 0, 8);
    return at;
}

void pdo_gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    command *c = new_command();
    c->kind = CMD_CLEAR;
    c->clear_flags = flags;
    c->clear_color = color;
    c->clear_z = z;
    c->clear_stencil = stencil;
}

void pdo_gfx_draw(const pdo_draw *d, const pdo_vertex *v, uint32_t nv,
                  const uint32_t *idx, uint32_t ni)
{
    command *c;

    if (!nv || !ni)
        return;
    verts = grow(verts, &cap_verts, nverts + nv, sizeof *verts);
    indices = grow(indices, &cap_indices, nindices + ni, sizeof *indices);
    memcpy(verts + nverts, v, nv * sizeof *v);
    for (uint32_t i = 0; i < ni; i++)
        indices[nindices + i] = nverts + idx[i];

    {
        /* PDO_GFX_DEBUG=n: describe the draws of frame n. */
        static long debug_frame = -2;
        static long debug_every;
        if (debug_frame == -2) {
            /* PDO_GFX_DEBUG=n, or n/every to repeat every 'every' frames */
            const char *v = getenv("PDO_GFX_DEBUG");
            debug_frame = v ? atol(v) : -1;
            debug_every = v && strchr(v, '/') ? atol(strchr(v, '/') + 1) : 0;
        }
        if ((long)frames == debug_frame ||
            (debug_every > 0 && debug_frame >= 0 && (long)frames > debug_frame &&
             ((long)frames - debug_frame) % debug_every == 0)) {
            pdo_xtex_info ti;
            pdo_xtex_describe(d->tex[0].format_word, d->tex[0].size_word, &ti);
            SDL_Log("gfx draw @%llu: %u verts %u idx topo %u blend %u %x/%x z %u tex0 %08x %s %ux%u "
                    "cop0 %d/%x/%x aop0 %d/%x/%x vp %.0f,%.0f %.0fx%.0f",
                    (unsigned long long)frames, nv, ni, d->ps.topology, d->ps.blend_enable, d->ps.src_blend, d->ps.dst_blend,
                    d->ps.z_enable, d->tex[0].data, pdo_xtex_format_name(ti.format), ti.width, ti.height,
                    d->ff.color_op[0][0], d->ff.color_op[0][1], d->ff.color_op[0][2],
                    d->ff.alpha_op[0][0], d->ff.alpha_op[0][1], d->ff.alpha_op[0][2],
                    d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3]);
            for (uint32_t i = 0; i < nv && i < 4; i++)
                SDL_Log("gfx   v%u pos %.3f %.3f %.3f %.3f diffuse %08x tc0 %.3f %.3f", i,
                        v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].pos[3], v[i].diffuse,
                        v[i].tc[0][0], v[i].tc[0][1]);
        }
    }
    c = new_command();
    c->draw = *d;
    c->first_index = nindices;
    c->index_count = ni;
    nverts += nv;
    nindices += ni;
}

int32_t pdo_gfx_vsh_program(const uint32_t *code, uint32_t count)
{
    uint32_t base = nprogs / 4;     /* in uvec4 */

    if (!count || !vsh_vs)
        return -1;
    progs = grow(progs, &cap_progs, nprogs + count * 8, sizeof *progs);
    memcpy(progs + nprogs, code, (size_t)count * 32);
    nprogs += count * 8;
    progs_dirty = true;
    return (int32_t)base;
}

void pdo_gfx_draw_vsh(const pdo_draw *d, const pdo_gpu_vsh *g, uint32_t nv,
                      const uint32_t *idx, uint32_t ni, const pdo_vertex *cpu)
{
    uint32_t sbase[16] = { 0 };
    int32_t consts;
    command *c;

    if (!nv || !ni || g->program < 0)
        return;
    if (cpu) {
        pdo_gfx_draw(d, cpu, nv, idx, ni);
        c = &cmds[ncmds - 1];
        c->has_cpu = true;
    } else {
        c = new_command();
        c->draw = *d;
    }
    c->vsh = true;

    /* The streams, 4-aligned; 8 bytes of slack past the end for the
     * shader's word reads. */
    for (int k = 0; k < 16; k++)
        if (g->stream_bytes[k] && g->stream_data[k])
            sbase[k] = add_blob(g->stream_data[k], g->stream_bytes[k]);

    /* The constants, shared with the draw before when they are the same. */
    if (last_consts >= 0 && !memcmp(vconsts[last_consts], g->constants, 192 * 16)) {
        consts = last_consts;
    } else {
        vconsts = grow(vconsts, &cap_vconsts, nvconsts + 192, sizeof *vconsts);
        memcpy(vconsts[nvconsts], g->constants, 192 * 16);
        consts = (int32_t)nvconsts;
        last_consts = consts;
        nvconsts += 192;
    }

    c->vu.info[0] = g->program;
    c->vu.info[1] = (int32_t)g->count;
    c->vu.info[2] = consts;
    for (int r = 0; r < 16; r++) {
        int32_t st = g->input[r][0];
        if (st < 0 || st > 15 || !g->input[r][3] || !g->stream_bytes[st]) {
            memset(c->vu.inputs[r], 0, sizeof c->vu.inputs[r]);
            continue;
        }
        c->vu.inputs[r][0] = (int32_t)sbase[st];
        c->vu.inputs[r][1] = g->input[r][1];
        c->vu.inputs[r][2] = g->input[r][2];
        c->vu.inputs[r][3] = g->input[r][3];
    }
    memcpy(c->vu.scale, g->vp_scale, sizeof c->vu.scale);
    memcpy(c->vu.offset, g->vp_offset, sizeof c->vu.offset);

    indices = grow(indices, &cap_indices, nindices + ni, sizeof *indices);
    memcpy(indices + nindices, idx, ni * sizeof *idx);
    c->vsh_first_index = nindices;
    c->vsh_index_count = ni;
    nindices += ni;
}

bool pdo_gfx_draw_fvf(const pdo_draw *d, const pdo_gpu_fvf *g, uint32_t nv,
                      const uint32_t *idx, uint32_t ni, const pdo_vertex *cpu)
{
    command *c;

    if (!fvf_vs || !nv || !ni || !g->data)
        return false;
    if (cpu) {
        pdo_gfx_draw(d, cpu, nv, idx, ni);
        c = &cmds[ncmds - 1];
        c->has_cpu = true;
    } else {
        c = new_command();
        c->draw = *d;
    }
    fvf_us = grow(fvf_us, &cap_fvf_us, nfvf_us + 1, sizeof *fvf_us);
    fvf_us[nfvf_us] = g->u;
    fvf_us[nfvf_us].layout0[0] = (int32_t)add_blob(g->data, g->bytes);
    c->fvf = (int32_t)nfvf_us++;
    indices = grow(indices, &cap_indices, nindices + ni, sizeof *indices);
    memcpy(indices + nindices, idx, ni * sizeof *idx);
    c->vsh_first_index = nindices;
    c->vsh_index_count = ni;
    nindices += ni;
    return true;
}

bool pdo_gfx_capturing(void)
{
    return frames + 1 == capture_frame;
}

/* ── Pipelines and samplers ───────────────────────────────────────────── */

static SDL_GPUBlendFactor blend_factor(uint16_t gl)
{
    switch (gl) {
    case 0x000: return SDL_GPU_BLENDFACTOR_ZERO;
    case 0x001: return SDL_GPU_BLENDFACTOR_ONE;
    case 0x300: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 0x301: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 0x303: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case 0x305: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 0x307: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case 0x8002: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default:    return SDL_GPU_BLENDFACTOR_ONE;
    }
}

static SDL_GPUBlendOp blend_op(uint16_t gl)
{
    switch (gl) {
    case 0x800A: return SDL_GPU_BLENDOP_SUBTRACT;
    case 0x800B: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 0x8007: return SDL_GPU_BLENDOP_MIN;
    case 0x8008: return SDL_GPU_BLENDOP_MAX;
    default:     return SDL_GPU_BLENDOP_ADD;
    }
}

static SDL_GPUStencilOp stencil_op(uint16_t gl)
{
    switch (gl) {
    case 0x0000: return SDL_GPU_STENCILOP_ZERO;
    case 0x1E01: return SDL_GPU_STENCILOP_REPLACE;
    case 0x1E02: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case 0x1E03: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case 0x150A: return SDL_GPU_STENCILOP_INVERT;
    case 0x8507: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case 0x8508: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    default:     return SDL_GPU_STENCILOP_KEEP;     /* 0x1E00 */
    }
}

static SDL_GPUCompareOp compare_op(uint16_t gl)
{
    switch (gl) {
    case 0x200: return SDL_GPU_COMPAREOP_NEVER;
    case 0x201: return SDL_GPU_COMPAREOP_LESS;
    case 0x202: return SDL_GPU_COMPAREOP_EQUAL;
    case 0x203: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case 0x204: return SDL_GPU_COMPAREOP_GREATER;
    case 0x205: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case 0x206: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default:    return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

static struct { pdo_pipeline_state key; SDL_GPUGraphicsPipeline *p; } pipelines[MAX_PIPELINES];
static int npipelines;

/* Making a pipeline mid-frame can take the driver tens of milliseconds, a
 * visible hitch the first time an effect appears. Every state combination
 * met is appended to pipelines.bin in the data folder, and the next start
 * (or a change of anti-aliasing) makes them all up front. The file is
 * only a list of our own keys: a different build or a bad file is ignored. */
#define PIPELINE_FILE_MAGIC 0x4C505044u /* "DPPL" */
static bool warming;                     /* making the saved ones: do not save again */
static Uint64 pipeline_ns;               /* time making them mid-game, for the timing log */
static uint32_t pipelines_made;

static void pipeline_file(char *path, size_t n)
{
    snprintf(path, n, "%spipelines.bin", pdo_data_dir());
}

static void save_pipeline_key(const pdo_pipeline_state *ps)
{
    char path[PDO_PATH_MAX];
    SDL_PathInfo info;
    SDL_IOStream *io;

    pipeline_file(path, sizeof path);
    if (!SDL_GetPathInfo(path, &info) || info.size == 0) {
        uint32_t head[2] = { PIPELINE_FILE_MAGIC, (uint32_t)sizeof *ps };
        io = SDL_IOFromFile(path, "wb");
        if (!io)
            return;
        SDL_WriteIO(io, head, sizeof head);
    } else {
        io = SDL_IOFromFile(path, "ab");
        if (!io)
            return;
    }
    SDL_WriteIO(io, ps, sizeof *ps);
    SDL_CloseIO(io);
}

static SDL_GPUGraphicsPipeline *get_pipeline(const pdo_pipeline_state *ps);

static void warm_pipelines(void)
{
    char path[PDO_PATH_MAX];
    size_t size = 0;
    uint8_t *data;
    Uint64 t0 = SDL_GetTicksNS();
    int before = npipelines;

    pipeline_file(path, sizeof path);
    data = SDL_LoadFile(path, &size);
    if (!data)
        return;
    if (size >= 8 && ((uint32_t *)data)[0] == PIPELINE_FILE_MAGIC
            && ((uint32_t *)data)[1] == sizeof(pdo_pipeline_state)) {
        warming = true;
        for (size_t o = 8; o + sizeof(pdo_pipeline_state) <= size && npipelines < MAX_PIPELINES;
             o += sizeof(pdo_pipeline_state)) {
            pdo_pipeline_state ps;
            memcpy(&ps, data + o, sizeof ps);
            get_pipeline(&ps);
        }
        warming = false;
        SDL_Log("gfx: made %d saved pipelines in %.0f ms", npipelines - before,
                (double)(SDL_GetTicksNS() - t0) / 1e6);
    } else {
        SDL_Log("gfx: %s is from another build; starting it again", path);
        SDL_RemovePath(path);
    }
    SDL_free(data);
}

static Uint64 swap_wait_ns;              /* waiting for a swapchain image, for the timing log */

uint64_t pdo_gfx_take_swap_wait_ns(void)
{
    Uint64 t = swap_wait_ns;
    swap_wait_ns = 0;
    return t;
}

uint64_t pdo_gfx_take_pipeline_ns(uint32_t *made)
{
    Uint64 t = pipeline_ns;
    *made = pipelines_made;
    pipeline_ns = 0;
    pipelines_made = 0;
    return t;
}

static SDL_GPUGraphicsPipeline *get_pipeline(const pdo_pipeline_state *ps)
{
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUVertexBufferDescription vb;
    SDL_GPUVertexAttribute attrs[8];
    SDL_GPUColorTargetDescription ct;
    SDL_GPUGraphicsPipeline *p;
    uint32_t cw = ps->color_write;
    Uint64 t0;

    for (int i = 0; i < npipelines; i++)
        if (!memcmp(&pipelines[i].key, ps, sizeof *ps))
            return pipelines[i].p;
    if (npipelines == MAX_PIPELINES)
        return pipelines[0].p;
    t0 = SDL_GetTicksNS();

    SDL_zero(vb);
    vb.slot = 0;
    vb.pitch = sizeof(pdo_vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    for (int i = 0; i < 8; i++) {
        attrs[i].location = (Uint32)i;
        attrs[i].buffer_slot = 0;
    }
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[0].offset = offsetof(pdo_vertex, pos);
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attrs[1].offset = offsetof(pdo_vertex, diffuse);
    attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attrs[2].offset = offsetof(pdo_vertex, specular);
    for (int i = 0; i < 4; i++) {
        attrs[3 + i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[3 + i].offset = (Uint32)(offsetof(pdo_vertex, tc) + (size_t)i * 16);
    }
    attrs[7].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    attrs[7].offset = offsetof(pdo_vertex, fog);

    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ct.blend_state.enable_blend = ps->blend_enable != 0;
    ct.blend_state.src_color_blendfactor = blend_factor(ps->src_blend);
    ct.blend_state.dst_color_blendfactor = blend_factor(ps->dst_blend);
    ct.blend_state.color_blend_op = blend_op(ps->blend_op);
    ct.blend_state.src_alpha_blendfactor = blend_factor(ps->src_blend);
    ct.blend_state.dst_alpha_blendfactor = blend_factor(ps->dst_blend);
    ct.blend_state.alpha_blend_op = blend_op(ps->blend_op);
    ct.blend_state.enable_color_write_mask = true;
    /* Xbox D3DCOLORWRITEENABLE: red bit 16, green 8, blue 0, alpha 24. */
    ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)(
        ((cw & 0x00FF0000u) ? SDL_GPU_COLORCOMPONENT_R : 0) |
        ((cw & 0x0000FF00u) ? SDL_GPU_COLORCOMPONENT_G : 0) |
        ((cw & 0x000000FFu) ? SDL_GPU_COLORCOMPONENT_B : 0) |
        ((cw & 0xFF000000u) ? SDL_GPU_COLORCOMPONENT_A : 0));

    SDL_zero(pi);
    pi.vertex_shader = ps->gpu_vsh == 1 ? vsh_vs : ps->gpu_vsh == 2 ? fvf_vs : vs;
    pi.fragment_shader = fs;
    if (!ps->gpu_vsh) {         /* vsh.vert reads its streams from storage buffers */
        pi.vertex_input_state.vertex_buffer_descriptions = &vb;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = 8;
    }
    pi.primitive_type = ps->topology == 1 ? SDL_GPU_PRIMITIVETYPE_LINELIST
                      : ps->topology == 2 ? SDL_GPU_PRIMITIVETYPE_POINTLIST
                      : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    /* D3D culls by screen-space winding, which ours matches: clip space is
     * D3D's (y up) and SDL presents it the same way up. JSRF's outlines are
     * the mesh drawn again, inflated, in one colour, with the other side
     * culled; without culling that shell covers the whole character. */
    pi.rasterizer_state.cull_mode = ps->cull ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = ps->cull == 1 ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE
                                                   : SDL_GPU_FRONTFACE_CLOCKWISE;
    if (ps->depth_bias != 0.0f || ps->depth_slope != 0.0f) {
        pi.rasterizer_state.enable_depth_bias = true;
        pi.rasterizer_state.depth_bias_constant_factor = ps->depth_bias;
        pi.rasterizer_state.depth_bias_slope_factor = ps->depth_slope;
    }
    pi.depth_stencil_state.enable_depth_test = ps->z_enable != 0;
    pi.depth_stencil_state.enable_depth_write = ps->z_enable && ps->z_write;
    pi.depth_stencil_state.compare_op = compare_op(ps->z_func);
    if (ps->stencil_enable) {
        SDL_GPUStencilOpState so;
        so.fail_op = stencil_op(ps->stencil_fail);
        so.depth_fail_op = stencil_op(ps->stencil_zfail);
        so.pass_op = stencil_op(ps->stencil_pass);
        so.compare_op = compare_op(ps->stencil_func);
        pi.depth_stencil_state.enable_stencil_test = true;
        pi.depth_stencil_state.front_stencil_state = so;
        pi.depth_stencil_state.back_stencil_state = so;
        pi.depth_stencil_state.compare_mask = ps->stencil_mask;
        pi.depth_stencil_state.write_mask = ps->stencil_write_mask;
    }
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = depth_format;
    pi.target_info.has_depth_stencil_target = true;
    pi.multisample_state.sample_count = ps->single_sample ? SDL_GPU_SAMPLECOUNT_1 : sample_count;

    p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (!p) {
        SDL_Log("gfx: pipeline creation failed (%s)", SDL_GetError());
        return NULL;
    }
    pipelines[npipelines].key = *ps;
    pipelines[npipelines].p = p;
    npipelines++;
    if (!warming) {
        pipeline_ns += SDL_GetTicksNS() - t0;
        pipelines_made++;
        save_pipeline_key(ps);
    }
    return p;
}

static struct { uint32_t key; SDL_GPUSampler *s; } samplers[MAX_SAMPLERS];
static int nsamplers;

static SDL_GPUSamplerAddressMode address_mode(uint8_t a)
{
    switch (a) {
    case 2:  return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case 3: case 4: case 5: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    }
}

static SDL_GPUSampler *get_sampler(const pdo_tex_ref *t)
{
    uint32_t key = (uint32_t)t->min_filter | (uint32_t)t->mag_filter << 8 |
                   (uint32_t)t->address_u << 16 | (uint32_t)t->address_v << 24 |
                   (uint32_t)(t->mip_filter & 3) << 30;
    SDL_GPUSamplerCreateInfo si;

    for (int i = 0; i < nsamplers; i++)
        if (samplers[i].key == key)
            return samplers[i].s;
    if (nsamplers == MAX_SAMPLERS)
        return samplers[0].s;
    SDL_zero(si);
    si.min_filter = t->min_filter >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mag_filter = t->mag_filter >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    /* D3DTSS_MIPFILTER: 0 none (level 0 only), 1 point, 2+ linear. */
    si.mipmap_mode = t->mip_filter >= 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.min_lod = 0.0f;
    si.max_lod = t->mip_filter ? 1000.0f : 0.0f;
    si.address_mode_u = address_mode(t->address_u);
    si.address_mode_v = address_mode(t->address_v);
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplers[nsamplers].key = key;
    samplers[nsamplers].s = SDL_CreateGPUSampler(dev, &si);
    return samplers[nsamplers++].s;
}

/* ── Texture cache ────────────────────────────────────────────────────────
 * Keyed by the guest data address and descriptor, validated by a hash of
 * the texel data each frame it is used, so a game that rewrites a texture
 * in place is seen. */

typedef struct gtex {
    uint32_t data, format_word, size_word;
    uint64_t hash;
    uint64_t last_used;
    uint32_t w, h;
    SDL_GPUTexture *tex;
} gtex;

static gtex textures[MAX_TEXTURES];
static int ntextures;

/* A texture's bytes, to notice when the game rewrites them: it runs over
 * every texture in use every frame, so it has to be fast more than strong.
 * Four independent 64-bit lanes take 32 bytes a step (the byte-at-a-time
 * FNV-1a this replaced cost the player's PC about 300 ms a second). */
static uint64_t rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t a = k, b = k ^ 0x5bd1e995u, c = ~k, d = rotl64(k, 17), h;
    size_t i = 0;

    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        a = rotl64((a ^ w[0]) * k, 31);
        b = rotl64((b ^ w[1]) * k, 31);
        c = rotl64((c ^ w[2]) * k, 31);
        d = rotl64((d ^ w[3]) * k, 31);
    }
    h = rotl64(a, 1) ^ rotl64(b, 7) ^ rotl64(c, 12) ^ rotl64(d, 18) ^ (uint64_t)n;
    for (; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

/* ── Render targets ───────────────────────────────────────────────────────
 * A surface the game draws into (SetRenderTarget) or copies into
 * (CopyRects) lives on the GPU, keyed by its guest data address. A texture
 * over the same memory samples it from there: JSRF draws its save-menu and
 * map backgrounds and its boost blur this way. Guest memory is not written
 * back; nothing we know of reads it on the CPU. */

#define MAX_TARGETS 64

typedef struct rtarget {
    uint32_t data;            /* guest data address, 0x80000000 masked off */
    uint32_t w, h;            /* the game's size */
    uint32_t pw, ph;          /* ours: times rscale */
    SDL_GPUTexture *color, *depth, *ms;
    bool stale;
    bool depth_only;          /* a shadow map: keyed by its depth surface,
                                 single-sampled, depth readable by shaders */
} rtarget;

static rtarget targets[MAX_TARGETS];
static int ntargets;
static bool no_shadows;     /* captures only: shadow maps read as lit */
static Uint64 texture_ns;   /* time resolving textures, for the timing log */

uint64_t pdo_gfx_take_texture_ns(void)
{
    Uint64 t = texture_ns;
    texture_ns = 0;
    return t;
}

static uint32_t phys(uint32_t data)
{
    return data & 0x7FFFFFFFu;
}

void pdo_gfx_surface_size(const pdo_surface *s, int *w, int *h)
{
    pdo_xtex_info info;

    if (!s->data) {
        *w = frame_w;
        *h = frame_h;
        return;
    }
    pdo_xtex_describe(s->format_word, s->size_word, &info);
    *w = (int)info.width;
    *h = (int)info.height;
}

static rtarget *find_target(uint32_t data, uint32_t w, uint32_t h)
{
    for (int i = 0; i < ntargets; i++)
        if (targets[i].data == phys(data) && targets[i].w == w && targets[i].h == h)
            return &targets[i];
    return NULL;
}

/* A target's textures at our size: the sampled colour, depth, and with
 * multisampling the multisampled colour (depth is multisampled then too). */
static bool make_target_as(Uint32 w, Uint32 h, SDL_GPUTexture **color, SDL_GPUTexture **depth,
                           SDL_GPUTexture **ms, bool depth_only)
{
    SDL_GPUTextureCreateInfo ti;
    int n = depth_only ? 1 : nsamples;

    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    *color = SDL_CreateGPUTexture(dev, &ti);
    *ms = NULL;
    if (n > 1) {
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.sample_count = sample_count;
        *ms = SDL_CreateGPUTexture(dev, &ti);
    }
    ti.format = depth_format;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (depth_only)
        ti.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.sample_count = n > 1 ? sample_count : SDL_GPU_SAMPLECOUNT_1;
    *depth = SDL_CreateGPUTexture(dev, &ti);
    if (*color && *depth && (n == 1 || *ms))
        return true;
    SDL_ReleaseGPUTexture(dev, *color);
    SDL_ReleaseGPUTexture(dev, *depth);
    SDL_ReleaseGPUTexture(dev, *ms);
    *color = *depth = *ms = NULL;
    return false;
}

static bool make_target(Uint32 w, Uint32 h, SDL_GPUTexture **color, SDL_GPUTexture **depth,
                        SDL_GPUTexture **ms)
{
    return make_target_as(w, h, color, depth, ms, false);
}

/* Clear a new target to black, its multisampled buffer too. */
static void clear_texture(SDL_GPUTexture *t, SDL_GPUTexture *ms)
{
    SDL_GPUCommandBuffer *c = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUColorTargetInfo ct;

    if (!c)
        return;
    SDL_zero(ct);
    ct.texture = ms ? ms : t;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = ms ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
    ct.resolve_texture = ms ? t : NULL;
    ct.clear_color.a = 1.0f;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(c, &ct, 1, NULL));
    SDL_SubmitGPUCommandBuffer(c);
}

/* The GPU target for a guest surface, created on first use. */
/* A colour surface without memory (data 0, with a format and size): the
 * game draws depth only into it, e.g. a shadow map whose depth buffer is a
 * texture. Its colour goes to a scratch target of that size, not the frame
 * (data 0 with no format is the back buffer). */
#define NO_COLOUR_DATA 0x7FFFF000u

static rtarget *target_for_surface_as(const pdo_surface *s, bool depth_only);

static rtarget *target_for_surface(const pdo_surface *s)
{
    return target_for_surface_as(s, false);
}

/* With a depth surface (z data not 0), a depth-only pass gets a target of
 * its own, keyed by that surface's memory: two shadow maps of one size
 * stay apart, and a texture over the depth surface finds it. */
static rtarget *target_of(const pdo_surface *s, const pdo_surface *z)
{
    pdo_surface scratch;

    if (s->data)
        return target_for_surface(s);
    if (!s->format_word)
        return NULL;
    scratch = *s;
    if (z && z->data) {
        scratch.data = z->data;
        return target_for_surface_as(&scratch, true);
    }
    scratch.data = NO_COLOUR_DATA;
    return target_for_surface(&scratch);
}

static rtarget *target_for_surface_as(const pdo_surface *s, bool depth_only)
{
    rtarget *r;
    int w, h;

    pdo_gfx_surface_size(s, &w, &h);
    if (!dev || !s->data || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return NULL;
    r = find_target(s->data, (uint32_t)w, (uint32_t)h);
    if (r)
        return r;
    /* Same memory, other size: the old surface is gone. */
    for (int i = 0; i < ntargets; i++)
        if (targets[i].data == phys(s->data)) {
            SDL_ReleaseGPUTexture(dev, targets[i].color);
            SDL_ReleaseGPUTexture(dev, targets[i].depth);
            SDL_ReleaseGPUTexture(dev, targets[i].ms);
            targets[i] = targets[--ntargets];
            break;
        }
    if (ntargets == MAX_TARGETS) {
        static bool warned;
        if (!warned) {
            warned = true;
            SDL_Log("gfx: more than %d render targets", MAX_TARGETS);
        }
        return NULL;
    }
    r = &targets[ntargets];
    memset(r, 0, sizeof *r);
    r->data = phys(s->data);
    r->w = (uint32_t)w;
    r->h = (uint32_t)h;
    r->pw = r->w * (uint32_t)rscale;
    r->ph = r->h * (uint32_t)rscale;
    r->depth_only = depth_only;
    if (!make_target_as(r->pw, r->ph, &r->color, &r->depth, &r->ms, depth_only)) {
        SDL_Log("gfx: cannot create a %ux%u render target (%s)", r->pw, r->ph, SDL_GetError());
        return NULL;
    }
    clear_texture(r->color, r->ms);
    SDL_Log("gfx: render target %ux%u at 0x%08x (%s)%s", r->w, r->h, r->data,
            pdo_xtex_format_name((s->format_word >> 8) & 0xFF),
            depth_only ? ", depth only: a shadow map" : "");
    ntargets++;
    return r;
}

static pdo_surface cur_depth_surface;

void pdo_gfx_set_depth(const pdo_surface *depth)
{
    cur_depth_surface = *depth;
}

void pdo_gfx_set_target(const pdo_surface *color)
{
    command *c = new_command();
    c->kind = CMD_TARGET;
    c->surf = *color;
    c->surf2 = cur_depth_surface;
    target_of(color, &cur_depth_surface);
}

void pdo_gfx_copy_rect(const pdo_surface *src, const pdo_surface *dst,
                       int x, int y, int w, int h, int dx, int dy)
{
    command *c = new_command();
    c->kind = CMD_COPY;
    c->surf = *src;
    c->surf2 = *dst;
    c->rect[0] = x; c->rect[1] = y; c->rect[2] = w; c->rect[3] = h;
    c->rect[4] = dx; c->rect[5] = dy;
    if (dst->data)
        target_for_surface(dst);
}

typedef struct pending_upload {
    gtex    *t;
    uint32_t offset;   /* in the frame's texture transfer buffer */
    uint32_t level, w, h;
} pending_upload;

#define MAX_UPLOADS 2048

/* Find or create the GPU texture for t; if its texels need (re)uploading,
 * decode them into *staging and note the upload. */
static SDL_GPUTexture *texture_for(const pdo_tex_ref *t, uint8_t **staging, uint32_t *staging_len,
                                   uint32_t *staging_cap, pending_upload *ups, int *nups)
{
    pdo_xtex_info info;
    const uint8_t *src;
    gtex *g = NULL;
    uint64_t h;
    uint32_t levels;

    if (!t->data)
        return white;
    pdo_xtex_describe(t->format_word, t->size_word, &info);
    if (!info.width || !info.height || info.width > 4096 || info.height > 4096)
        return white;
    {
        rtarget *r = find_target(t->data, info.width, info.height);
        if (r)
            return r->depth_only ? (no_shadows ? white : r->depth) : r->color;
    }
    if (info.format >= 0x2A && info.format <= 0x31) {
        /* Depth textures (D24S8, F24S8, D16, F16, swizzled and linear) are
         * shadow maps, compared in the shader (ff.frag, shadow_stage). One
         * that no depth-only pass drew reads as fully lit. */
        static bool noted;
        if (!noted) {
            noted = true;
            SDL_Log("gfx: depth texture (format 0x%02x) at 0x%08x sampled before any pass drew it: lit",
                    info.format, t->data);
        }
        return white;
    }
    if (!info.known) {
        static uint32_t warned[256];
        if (!warned[info.format]++)
            SDL_Log("gfx: texture format 0x%02x not decoded yet", info.format);
    }

    for (int i = 0; i < ntextures; i++) {
        if (textures[i].data == t->data && textures[i].format_word == t->format_word &&
            textures[i].size_word == t->size_word) {
            g = &textures[i];
            break;
        }
    }
    if (g && g->last_used == frames)
        return g->tex;      /* already checked this frame */
    /* The CPU view of a resource is Data | 0x80000000, as D3D computes it. */
    src = (const uint8_t *)((uintptr_t)(t->data | 0x80000000u) + g_xbox_mem_offset);
    levels = info.known ? pdo_xtex_level_count(&info) : 1;
    {   /* the whole mip chain, so a rewrite of any level is seen */
        pdo_xtex_info last;
        uint32_t off;
        pdo_xtex_level(&info, levels - 1, &last, &off);
        h = info.bytes ? hash_bytes(src, off + last.bytes) : 0;
    }
    if (!g) {
        SDL_GPUTextureCreateInfo ti;
        if (ntextures == MAX_TEXTURES)
            return white;
        g = &textures[ntextures++];
        memset(g, 0, sizeof *g);
        g->data = t->data;
        g->format_word = t->format_word;
        g->size_word = t->size_word;
        g->w = info.width;
        g->h = info.height;
        g->hash = h + 1;    /* force the first upload */
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = info.width;
        ti.height = info.height;
        ti.layer_count_or_depth = 1;
        ti.num_levels = levels;
        g->tex = SDL_CreateGPUTexture(dev, &ti);
    }
    g->last_used = frames;
    if (g->hash != h && *nups + (int)levels <= MAX_UPLOADS) {
        /* Every mip level: sampled with only level 0, distant textures
         * shimmered and looked grainy. */
        for (uint32_t l = 0; l < levels; l++) {
            pdo_xtex_info li;
            uint32_t off, need;
            pdo_xtex_level(&info, l, &li, &off);
            need = li.width * li.height * 4;
            if (*staging_len + need > *staging_cap) {
                *staging_cap = (*staging_len + need) * 2;
                *staging = SDL_realloc(*staging, *staging_cap);
            }
            pdo_xtex_decode(&li, src + off, *staging + *staging_len);
            ups[*nups].t = g;
            ups[*nups].offset = *staging_len;
            ups[*nups].level = l;
            ups[*nups].w = li.width;
            ups[*nups].h = li.height;
            (*nups)++;
            *staging_len += need;
        }
        g->hash = h;
    }
    return g->tex;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

static SDL_GPUShader *make_shader_sb(const uint8_t *code, size_t len, SDL_GPUShaderStage stage,
                                     Uint32 samplers_n, Uint32 uniforms_n, Uint32 storage_n)
{
    SDL_GPUShaderCreateInfo si;
    SDL_zero(si);
    si.code = code;
    si.code_size = len;
    si.entrypoint = "main";
    si.format = SDL_GPU_SHADERFORMAT_SPIRV;
    si.stage = stage;
    si.num_samplers = samplers_n;
    si.num_uniform_buffers = uniforms_n;
    si.num_storage_buffers = storage_n;
    return SDL_CreateGPUShader(dev, &si);
}

static SDL_GPUShader *make_shader(const uint8_t *code, size_t len, SDL_GPUShaderStage stage,
                                  Uint32 samplers_n, Uint32 uniforms_n)
{
    return make_shader_sb(code, len, stage, samplers_n, uniforms_n, 0);
}

static SDL_GPUShader *screen_vs, *screen_fs;

/* Render scale and samples from the settings, the samples as many as both
 * the colour and the depth format take. */
static void choose_quality(void)
{
    static const SDL_GPUSampleCount counts[] = { SDL_GPU_SAMPLECOUNT_1, SDL_GPU_SAMPLECOUNT_2,
                                                 SDL_GPU_SAMPLECOUNT_4, SDL_GPU_SAMPLECOUNT_8 };
    const pdo_settings *st = pdo_settings_get();
    int want = st->msaa >= 8 ? 3 : st->msaa >= 4 ? 2 : st->msaa >= 2 ? 1 : 0;

    while (want > 0 && !(SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, counts[want])
                         && SDL_GPUTextureSupportsSampleCount(dev, depth_format, counts[want])))
        want--;
    rscale = SDL_clamp(st->render_scale, 1, 4);
    nsamples = 1 << want;
    sample_count = counts[want];
}

/* The frame after the gamma ramp, at our size. */
static SDL_GPUTexture *make_output(void)
{
    SDL_GPUTextureCreateInfo ti;

    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)(frame_w * rscale);
    ti.height = (Uint32)(frame_h * rscale);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ti);
}

/* Draws a resolved target into its multisampled buffer: inside a target's
 * pass, so with its depth buffer and sample count, depth left alone. */
static void make_seed_pipeline(void)
{
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUColorTargetDescription ct;

    if (seed_pipeline)
        SDL_ReleaseGPUGraphicsPipeline(dev, seed_pipeline);
    seed_pipeline = NULL;
    if (nsamples == 1 || !screen_vs || !screen_fs)
        return;
    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_zero(pi);
    pi.vertex_shader = screen_vs;
    pi.fragment_shader = screen_fs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = depth_format;
    pi.target_info.has_depth_stencil_target = true;
    pi.multisample_state.sample_count = sample_count;
    seed_pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (!seed_pipeline)
        SDL_Log("gfx: cannot create the multisample seed pass (%s)", SDL_GetError());
}

/* The overlay changed the render scale or anti-aliasing: every target is
 * made again at the new size (their contents go; the game redraws them),
 * and the pipelines for the new sample count. */
static void apply_quality(void)
{
    int old_scale = rscale, old_samples = nsamples;

    choose_quality();
    if (rscale == old_scale && nsamples == old_samples)
        return;
    SDL_WaitForGPUIdle(dev);
    for (int i = 0; i < npipelines; i++)
        SDL_ReleaseGPUGraphicsPipeline(dev, pipelines[i].p);
    npipelines = 0;
    SDL_ReleaseGPUTexture(dev, frame_color);
    SDL_ReleaseGPUTexture(dev, frame_depth);
    SDL_ReleaseGPUTexture(dev, frame_ms);
    SDL_ReleaseGPUTexture(dev, output);
    if (!make_target((Uint32)(frame_w * rscale), (Uint32)(frame_h * rscale), &frame_color, &frame_depth,
                     &frame_ms)) {
        SDL_Log("gfx: cannot render at %dx with %d samples (%s); back to 1x, no anti-aliasing",
                rscale, nsamples, SDL_GetError());
        rscale = nsamples = 1;
        sample_count = SDL_GPU_SAMPLECOUNT_1;
        make_target((Uint32)frame_w, (Uint32)frame_h, &frame_color, &frame_depth, &frame_ms);
    }
    clear_texture(frame_color, frame_ms);
    frame_stale = false;
    /* Each target again at the new size, kept in the list so this frame's
     * draws still find them as targets, not as textures in guest memory. */
    for (int i = 0; i < ntargets; i++) {
        rtarget *r = &targets[i];
        SDL_ReleaseGPUTexture(dev, r->color);
        SDL_ReleaseGPUTexture(dev, r->depth);
        SDL_ReleaseGPUTexture(dev, r->ms);
        r->pw = r->w * (uint32_t)rscale;
        r->ph = r->h * (uint32_t)rscale;
        r->stale = false;
        if (!make_target_as(r->pw, r->ph, &r->color, &r->depth, &r->ms, r->depth_only)) {
            targets[i--] = targets[--ntargets];
            continue;
        }
        clear_texture(r->color, r->ms);
    }
    output = make_output();
    make_seed_pipeline();
    warm_pipelines();
    SDL_Log("gfx: rendering at %dx (%dx%d), %d sample%s", rscale, frame_w * rscale, frame_h * rscale,
            nsamples, nsamples > 1 ? "s" : "");
}

static bool init_present(void)
{
    SDL_GPUTextureCreateInfo ti;
    SDL_GPUSamplerCreateInfo si;
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUColorTargetDescription ct;
    SDL_GPUShader *pvs, *pfs;

    output = make_output();
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = PDO_OVERLAY_W;
    ti.height = PDO_OVERLAY_H;
    overlay_tex = SDL_CreateGPUTexture(dev, &ti);
    ti.width = 256;
    ti.height = 1;
    gamma_lut = SDL_CreateGPUTexture(dev, &ti);

    SDL_zero(si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest = SDL_CreateGPUSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    linear = SDL_CreateGPUSampler(dev, &si);

    pvs = make_shader(present_vert_spv, sizeof present_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    pfs = make_shader(present_frag_spv, sizeof present_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0);
    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_zero(pi);
    pi.vertex_shader = pvs;
    pi.fragment_shader = pfs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    present_pipeline = pvs && pfs ? SDL_CreateGPUGraphicsPipeline(dev, &pi) : NULL;
    SDL_ReleaseGPUShader(dev, pfs);
    screen_fs = make_shader(screen_frag_spv, sizeof screen_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    screen_vs = pvs;
    if (have_swapchain && screen_fs) {
        ct.format = SDL_GetGPUSwapchainTextureFormat(dev, window);
        pi.fragment_shader = screen_fs;
        screen_pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        overlay_pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!screen_pipeline || !overlay_pipeline)
            SDL_Log("gfx: cannot create the window pass (%s)", SDL_GetError());
    }
    make_seed_pipeline();

    for (int i = 0; i < 256; i++) {     /* identity until the game sets one */
        gamma_rgba[i * 4 + 0] = gamma_rgba[i * 4 + 1] = gamma_rgba[i * 4 + 2] = (uint8_t)i;
        gamma_rgba[i * 4 + 3] = 255;
    }
    if (!output || !gamma_lut || !nearest || !present_pipeline) {
        SDL_Log("gfx: cannot create the present pass (%s)", SDL_GetError());
        return false;
    }
    return true;
}

void pdo_gfx_set_gamma(const uint8_t ramp[768])
{
    if (getenv("PDO_GAMMA_TRACE"))
        SDL_Log("gfx: frame %llu gamma r[64]=%u r[128]=%u r[255]=%u g[128]=%u b[128]=%u",
                (unsigned long long)frames, ramp[64], ramp[128], ramp[255], ramp[256 + 128], ramp[512 + 128]);
    for (int i = 0; i < 256; i++) {
        gamma_rgba[i * 4 + 0] = ramp[i];
        gamma_rgba[i * 4 + 1] = ramp[256 + i];
        gamma_rgba[i * 4 + 2] = ramp[512 + i];
    }
    gamma_dirty = true;
}

/* frame -> (gamma) -> output. */
static void present_pass(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding tsb[2];
    SDL_GPURenderPass *pass;

    if (gamma_dirty) {
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, sizeof gamma_rgba, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        SDL_GPUCopyPass *copy;

        memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), gamma_rgba, sizeof gamma_rgba);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        copy = SDL_BeginGPUCopyPass(cmd);
        SDL_zero(src);
        src.transfer_buffer = tb;
        SDL_zero(dst);
        dst.texture = gamma_lut;
        dst.w = 256;
        dst.h = dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        gamma_dirty = false;
    }

    SDL_zero(ct);
    ct.texture = output;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!pass)
        return;
    SDL_BindGPUGraphicsPipeline(pass, present_pipeline);
    tsb[0].texture = frame_color;
    tsb[0].sampler = nearest;
    tsb[1].texture = gamma_lut;
    tsb[1].sampler = nearest;
    SDL_BindGPUFragmentSamplers(pass, 0, tsb, 2);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

/* A hang where the game thread never comes back to pump window events
 * ignores F11, so a watchdog takes the thread dump itself once no frame has
 * been presented for 8 s (once per stall). */
static int SDLCALL watchdog(void *unused)
{
    uint64_t seen = 0;
    int still = 0;
    bool dumped = false;

    (void)unused;
    for (;;) {
        uint64_t now = *(volatile uint64_t *)&frames;
        SDL_Delay(1000);
        if (now != seen || paused) {
            seen = now;
            still = 0;
            dumped = false;
        } else if (++still >= 8 && !dumped && seen > 0) {
            SDL_Log("watchdog: no frame for %d s (frame %llu), dumping threads", still,
                    (unsigned long long)seen);
            pdo_dump_threads();
            pdo_dump_guest_waits();
            dumped = true;
        }
    }
    return 0;
}

bool pdo_gfx_init(int width, int height)
{
    SDL_GPUTextureCreateInfo ti;
    const char *every;

    if (dev)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_Log("gfx: no video (%s)", SDL_GetError());
        return false;
    }
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!dev) {
        SDL_Log("gfx: no GPU device (%s)", SDL_GetError());
        return false;
    }
    SDL_Log("gfx: GPU driver %s", SDL_GetGPUDeviceDriver(dev));

    window_thread = SDL_GetCurrentThreadID();
    window = SDL_CreateWindow(WINDOW_TITLE, width * 2, height * 2, SDL_WINDOW_RESIZABLE);
    if (window && SDL_ClaimWindowForGPUDevice(dev, window))
        have_swapchain = true;
    else
        SDL_Log("gfx: rendering without a window (%s)", SDL_GetError());

    vs = make_shader(ff_vert_spv, sizeof ff_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    fs = make_shader(ff_frag_spv, sizeof ff_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    vsh_vs = make_shader_sb(vsh_vert_spv, sizeof vsh_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 3);
    if (!vsh_vs)
        SDL_Log("gfx: no GPU vertex programs (%s); they run on the CPU", SDL_GetError());
    fvf_vs = make_shader_sb(fvf_vert_spv, sizeof fvf_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
    if (!fvf_vs)
        SDL_Log("gfx: no GPU fixed-function vertices (%s); they run on the CPU", SDL_GetError());
    if (!vs || !fs) {
        SDL_Log("gfx: shader creation failed (%s)", SDL_GetError());
        return false;
    }

    frame_w = width;
    frame_h = height;
    depth_format = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                                                SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                   ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
                   : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    choose_quality();
    if (!make_target((Uint32)(width * rscale), (Uint32)(height * rscale), &frame_color, &frame_depth, &frame_ms))
        frame_color = NULL;

    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    white = SDL_CreateGPUTexture(dev, &ti);
    if (!frame_color || !frame_depth || !white) {
        SDL_Log("gfx: cannot create the frame targets (%s)", SDL_GetError());
        return false;
    }
    {
        /* Fill the 1x1 stand-in for an unbound stage. */
        static const uint8_t px[4] = { 255, 255, 255, 255 };
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 4, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer *c = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        SDL_GPUCopyPass *copy;

        memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), px, 4);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        copy = SDL_BeginGPUCopyPass(c);
        SDL_zero(src);
        src.transfer_buffer = tb;
        SDL_zero(dst);
        dst.texture = white;
        dst.w = dst.h = dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(c);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }

    if (!init_present())
        return false;
    SDL_Log("gfx: rendering at %dx (%dx%d), %d sample%s", rscale, frame_w * rscale, frame_h * rscale,
            nsamples, nsamples == 1 ? "" : "s");
    warm_pipelines();

    if (getenv("PDO_CAPTURE"))      /* same as F12 at that frame */
        capture_frame = (uint64_t)atoll(getenv("PDO_CAPTURE"));
    SDL_DetachThread(SDL_CreateThread(watchdog, "watchdog", NULL));
    shot_dir = getenv("PDO_SCREENSHOT");
    every = getenv("PDO_SCREENSHOT_EVERY");
    if (every && atoi(every) > 0)
        shot_every = atoi(every);
    return true;
}

void pdo_gfx_shutdown(void)
{
    if (!dev)
        return;
    SDL_WaitForGPUIdle(dev);
    for (int i = 0; i < ntextures; i++)
        SDL_ReleaseGPUTexture(dev, textures[i].tex);
    for (int i = 0; i < npipelines; i++)
        SDL_ReleaseGPUGraphicsPipeline(dev, pipelines[i].p);
    for (int i = 0; i < nsamplers; i++)
        SDL_ReleaseGPUSampler(dev, samplers[i].s);
    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    SDL_ReleaseGPUTexture(dev, frame_color);
    SDL_ReleaseGPUTexture(dev, frame_depth);
    SDL_ReleaseGPUTexture(dev, frame_ms);
    SDL_ReleaseGPUTexture(dev, overlay_tex);
    SDL_ReleaseGPUSampler(dev, linear);
    SDL_ReleaseGPUGraphicsPipeline(dev, screen_pipeline);
    SDL_ReleaseGPUGraphicsPipeline(dev, overlay_pipeline);
    SDL_ReleaseGPUGraphicsPipeline(dev, seed_pipeline);
    SDL_ReleaseGPUShader(dev, screen_vs);
    SDL_ReleaseGPUShader(dev, screen_fs);
    SDL_ReleaseGPUTexture(dev, white);
    SDL_ReleaseGPUTexture(dev, output);
    SDL_ReleaseGPUTexture(dev, gamma_lut);
    SDL_ReleaseGPUSampler(dev, nearest);
    SDL_ReleaseGPUGraphicsPipeline(dev, present_pipeline);
    if (have_swapchain)
        SDL_ReleaseWindowFromGPUDevice(dev, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(dev);
    dev = NULL;
}

void pdo_gfx_target_size(int *w, int *h)
{
    *w = frame_w;
    *h = frame_h;
}

uint64_t pdo_gfx_frame_count(void)
{
    return frames;
}

/* ── Replay ───────────────────────────────────────────────────────────── */

static SDL_GPUBuffer *vbuf, *ibuf, *blob_buf, *const_buf, *prog_buf;
static Uint32 vbuf_size, ibuf_size, blob_buf_size, const_buf_size, prog_buf_size;

/* A draw's vertices: the vertex buffer, or for a vertex program on the GPU
 * its streams, programs and constants. */
static void bind_geometry(SDL_GPURenderPass *pass, int gpu)
{
    SDL_GPUBufferBinding ib = { ibuf, 0 };

    if (gpu == 1) {
        SDL_GPUBuffer *sb[3] = { blob_buf, prog_buf, const_buf };
        SDL_BindGPUVertexStorageBuffers(pass, 0, sb, 3);
    } else if (gpu == 2) {
        SDL_BindGPUVertexStorageBuffers(pass, 0, &blob_buf, 1);
    } else {
        SDL_GPUBufferBinding vb = { vbuf, 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    }
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

static SDL_GPUBuffer *ensure_buffer(SDL_GPUBuffer *b, Uint32 *size, Uint32 need, SDL_GPUBufferUsageFlags usage)
{
    SDL_GPUBufferCreateInfo bi;
    if (b && *size >= need)
        return b;
    if (b)
        SDL_ReleaseGPUBuffer(dev, b);
    *size = need * 2;
    SDL_zero(bi);
    bi.usage = usage;
    bi.size = *size;
    return SDL_CreateGPUBuffer(dev, &bi);
}

/* The target being replayed into: NULL is the frame. */
static rtarget *cur_target;

static SDL_GPUTexture *cur_color(void)
{
    return cur_target ? cur_target->color : frame_color;
}

static void cur_size(Uint32 *w, Uint32 *h)
{
    *w = cur_target ? cur_target->pw : (Uint32)(frame_w * rscale);
    *h = cur_target ? cur_target->ph : (Uint32)(frame_h * rscale);
}

static SDL_GPUTexture *snapshot(SDL_GPUCommandBuffer *cmd);

static SDL_GPURenderPass *begin_pass(SDL_GPUCommandBuffer *cmd, const command *clear)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_GPURenderPass *pass;
    SDL_GPUTexture *ms = cur_target ? cur_target->ms : frame_ms, *seed = NULL;
    bool *stale = cur_target ? &cur_target->stale : &frame_stale;

    /* The resolved texture holds what the multisampled buffer does not:
     * draw it in first, unless this pass clears the colour anyway. */
    if (ms && *stale && !(clear && (clear->clear_flags & 0xF0)))
        seed = snapshot(cmd);
    *stale = false;

    SDL_zero(ct);
    ct.texture = ms ? ms : cur_color();
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = ms ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
    ct.resolve_texture = ms ? cur_color() : NULL;
    SDL_zero(dt);
    dt.texture = cur_target ? cur_target->depth : frame_depth;
    dt.load_op = SDL_GPU_LOADOP_LOAD;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_LOAD;
    dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
    if (clear) {
        uint32_t c = clear->clear_color;
        if (clear->clear_flags & 0xF0) {
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.clear_color.r = (float)((c >> 16) & 0xFF) / 255.0f;
            ct.clear_color.g = (float)((c >> 8) & 0xFF) / 255.0f;
            ct.clear_color.b = (float)(c & 0xFF) / 255.0f;
            ct.clear_color.a = (float)(c >> 24) / 255.0f;
        }
        if (clear->clear_flags & 1) {
            dt.load_op = SDL_GPU_LOADOP_CLEAR;
            dt.clear_depth = clear->clear_z;
        }
        if (clear->clear_flags & 2) {
            dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
            dt.clear_stencil = (Uint8)clear->clear_stencil;
        }
    }
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    if (pass && seed && seed_pipeline) {
        SDL_GPUTextureSamplerBinding tsb = { seed, nearest };
        float params[4] = { 0, 0, 0, 0 };
        SDL_BindGPUGraphicsPipeline(pass, seed_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof params);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    return pass;
}

static void log_command(uint32_t i, const command *c, SDL_GPUTexture *const *tex)
{
    const pdo_draw *d = &c->draw;

    if (c->kind == CMD_CLEAR) {
        SDL_Log("capture: %u clear flags %x color %08x", i, c->clear_flags, c->clear_color);
        return;
    }
    if (c->kind == CMD_TARGET) {
        SDL_Log("capture: %u target data %08x format %08x size %08x", i, c->surf.data,
                c->surf.format_word, c->surf.size_word);
        return;
    }
    if (c->kind == CMD_COPY) {
        SDL_Log("capture: %u copy %08x -> %08x (%d,%d %dx%d) at %d,%d", i, c->surf.data, c->surf2.data,
                c->rect[0], c->rect[1], c->rect[2], c->rect[3], c->rect[4], c->rect[5]);
        return;
    }
    SDL_Log("capture: %u draw %u idx topo %u blend %u %x/%x op %x z %u/%u/%x cull %u bias %g "
            "vp %.0f,%.0f %.0fx%.0f ps %d final %08x %08x atest %d/%x/%d spec %d fog %d/%d %g-%g/%g %.2f,%.2f,%.2f",
            i, c->index_count, d->ps.topology, d->ps.blend_enable, d->ps.src_blend, d->ps.dst_blend,
            d->ps.blend_op, d->ps.z_enable, d->ps.z_write, d->ps.z_func, d->ps.cull, d->ps.depth_bias,
            d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3],
            d->ff.ps_final[2] & 0xFF, d->ff.ps_final[0], d->ff.ps_final[1],
            d->ff.alpha_test[0], d->ff.alpha_test[1], d->ff.alpha_test[2], d->ff.alpha_test[3],
            d->ff.fog_mode[0], d->ff.fog_mode[1], d->ff.fog_params[0], d->ff.fog_params[1], d->ff.fog_params[2],
            d->ff.fog_color[0], d->ff.fog_color[1], d->ff.fog_color[2]);
    SDL_Log("capture:     vs %08x lighting %u colorvertex %u sources %04x ambient %08x "
            "material %08x/%08x/%08x lights %u", d->vertex_info[0], d->vertex_info[1], d->vertex_info[2] & 0xFF,
            d->vertex_info[3], d->vertex_info[4], d->vertex_info[5], d->vertex_info[6], d->vertex_info[7],
            d->vertex_info[2] >> 8);
    if (d->ff.ps_final[2] & 0xFF) {
        /* The pixel shader in full: texture modes, then each combiner stage's
         * RGB and alpha inputs and outputs with its two constants. */
        SDL_Log("capture:     ps modes %08x flags %08x fc0 %.2f,%.2f,%.2f,%.2f fc1 %.2f,%.2f,%.2f,%.2f",
                d->ff.ps_final[3], d->ff.ps_final[2], d->ff.ps_fc0[0], d->ff.ps_fc0[1], d->ff.ps_fc0[2],
                d->ff.ps_fc0[3], d->ff.ps_fc1[0], d->ff.ps_fc1[1], d->ff.ps_fc1[2], d->ff.ps_fc1[3]);
        for (int k = 0; k < (d->ff.ps_final[2] & 0xFF) && k < 8; k++)
            SDL_Log("capture:     ps %d rgb %08x>%08x a %08x>%08x c0 %.2f,%.2f,%.2f,%.2f c1 %.2f,%.2f,%.2f,%.2f",
                    k, d->ff.ps_rgb_in[k], d->ff.ps_rgb_out[k], d->ff.ps_a_in[k], d->ff.ps_a_out[k],
                    d->ff.ps_c0[k][0], d->ff.ps_c0[k][1], d->ff.ps_c0[k][2], d->ff.ps_c0[k][3],
                    d->ff.ps_c1[k][0], d->ff.ps_c1[k][1], d->ff.ps_c1[k][2], d->ff.ps_c1[k][3]);
    }
    if (d->ps.stencil_enable)
        SDL_Log("capture:     stencil func %x ref %u mask %x/%x ops %x %x %x", d->ps.stencil_func, d->stencil_ref,
                d->ps.stencil_mask, d->ps.stencil_write_mask, d->ps.stencil_fail, d->ps.stencil_zfail,
                d->ps.stencil_pass);
    for (int s = 0; s < 4; s++) {
        const pdo_tex_ref *t = &d->tex[s];
        pdo_xtex_info ti;
        const char *what = "";

        if (!t->data && d->ff.color_op[s][0] <= 1)
            continue;
        pdo_xtex_describe(t->format_word, t->size_word, &ti);
        if (tex[s] == white)
            what = t->data ? " (WHITE: no usable texture)" : " (none)";
        else if (find_target(t->data, ti.width, ti.height))
            what = " (render target)";
        SDL_Log("capture:     stage %d cop %d %x %x %x aop %d %x %x %x tex %08x fmt %08x size %08x %s %ux%u%s",
                s, d->ff.color_op[s][0], d->ff.color_op[s][1], d->ff.color_op[s][2], d->ff.color_op[s][3],
                d->ff.alpha_op[s][0], d->ff.alpha_op[s][1], d->ff.alpha_op[s][2], d->ff.alpha_op[s][3],
                t->data, t->format_word, t->size_word, pdo_xtex_format_name(ti.format), ti.width, ti.height,
                what);
    }
    if (c->vsh)
        SDL_Log("capture:     vertex program on the GPU: %d instructions at %d, constants at %d%s",
                c->vu.info[1], c->vu.info[0], c->vu.info[2], c->has_cpu ? "; the CPU's vertices:" : "");
    if (c->fvf >= 0)
        SDL_Log("capture:     fixed-function vertices on the GPU: flags %x, %d lights%s",
                fvf_us[c->fvf].layout0[2], fvf_us[c->fvf].light_info[0], c->has_cpu ? "; the CPU's vertices:" : "");
    for (uint32_t k = 0; k < 3 && k < c->index_count && ((!c->vsh && c->fvf < 0) || c->has_cpu); k++) {
        const pdo_vertex *v = &verts[indices[c->first_index + k]];
        SDL_Log("capture:     v pos %.3f %.3f %.3f %.3f d %08x s %08x t0 %.3f %.3f %.3f %.3f "
                "t1 %.3f %.3f %.3f %.3f fog %.2f",
                v->pos[0], v->pos[1], v->pos[2], v->pos[3], v->diffuse, v->specular, v->tc[0][0], v->tc[0][1],
                v->tc[0][2], v->tc[0][3], v->tc[1][0], v->tc[1][1], v->tc[1][2], v->tc[1][3], v->fog);
    }
    /* Each distinct guest texture once, as decoded. */
    for (int s = 0; s < 4; s++) {
        static uint32_t dumped[512];
        static int ndumped;
        const pdo_tex_ref *t = &d->tex[s];
        pdo_xtex_info ti;
        uint8_t *rgba;
        int k;

        if (!t->data || tex[s] == white)
            continue;
        for (k = 0; k < ndumped && dumped[k] != t->data; k++)
            ;
        if (k < ndumped || ndumped == 512)
            continue;
        dumped[ndumped++] = t->data;
        pdo_xtex_describe(t->format_word, t->size_word, &ti);
        if (find_target(t->data, ti.width, ti.height) || !ti.known)
            continue;
        rgba = SDL_malloc((size_t)ti.width * ti.height * 4);
        if (rgba) {
            char path[64];
            SDL_Surface *sf;
            pdo_xtex_decode(&ti, (const uint8_t *)((uintptr_t)(t->data | 0x80000000u) + g_xbox_mem_offset), rgba);
            sf = SDL_CreateSurfaceFrom((int)ti.width, (int)ti.height, SDL_PIXELFORMAT_ABGR8888, rgba,
                                       (int)ti.width * 4);
            snprintf(path, sizeof path, "capture_tex_%08x.bmp", t->data);
            if (sf) {
                SDL_SaveBMP(sf, path);
                SDL_DestroySurface(sf);
            }
            SDL_free(rgba);
        }
    }
}

/* A copy of the current target, for draws that sample it. */
static SDL_GPUTexture *snapshot(SDL_GPUCommandBuffer *cmd)
{
    static SDL_GPUTexture *tex;
    static Uint32 tw, th;
    SDL_GPUTextureLocation src, dst;
    SDL_GPUCopyPass *copy;
    Uint32 w, h;

    cur_size(&w, &h);
    if (!tex || tw != w || th != h) {
        SDL_GPUTextureCreateInfo ti;
        if (tex)
            SDL_ReleaseGPUTexture(dev, tex);
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        tex = SDL_CreateGPUTexture(dev, &ti);
        tw = w;
        th = h;
    }
    if (!tex)
        return white;
    SDL_zero(src);
    src.texture = cur_color();
    SDL_zero(dst);
    dst.texture = tex;
    copy = SDL_BeginGPUCopyPass(cmd);
    SDL_CopyGPUTextureToTexture(copy, &src, &dst, w, h, 1, false);
    SDL_EndGPUCopyPass(copy);
    return tex;
}

/* CopyRects: a rectangle from one surface to another, clipped to both. */
static void copy_rect(SDL_GPUCommandBuffer *cmd, const command *c)
{
    rtarget *rs = c->surf.data ? target_for_surface(&c->surf) : NULL;
    rtarget *rd = c->surf2.data ? target_for_surface(&c->surf2) : NULL;
    SDL_GPUTexture *ts = rs ? rs->color : frame_color, *td = rd ? rd->color : frame_color;
    int sw = rs ? (int)rs->pw : frame_w * rscale, sh = rs ? (int)rs->ph : frame_h * rscale;
    int dw = rd ? (int)rd->pw : frame_w * rscale, dh = rd ? (int)rd->ph : frame_h * rscale;
    int x = c->rect[0] * rscale, y = c->rect[1] * rscale, w = c->rect[2] * rscale, h = c->rect[3] * rscale;
    int dx = c->rect[4] * rscale, dy = c->rect[5] * rscale;
    SDL_GPUTextureLocation src, dst;
    SDL_GPUCopyPass *copy;

    if ((c->surf.data && !rs) || (c->surf2.data && !rd) || ts == td)
        return;
    if (x < 0) { dx -= x; w += x; x = 0; }
    if (y < 0) { dy -= y; h += y; y = 0; }
    if (dx < 0) { x -= dx; w += dx; dx = 0; }
    if (dy < 0) { y -= dy; h += dy; dy = 0; }
    w = SDL_min(w, SDL_min(sw - x, dw - dx));
    h = SDL_min(h, SDL_min(sh - y, dh - dy));
    if (w <= 0 || h <= 0)
        return;
    SDL_zero(src);
    src.texture = ts;
    src.x = (Uint32)x;
    src.y = (Uint32)y;
    SDL_zero(dst);
    dst.texture = td;
    dst.x = (Uint32)dx;
    dst.y = (Uint32)dy;
    copy = SDL_BeginGPUCopyPass(cmd);
    SDL_CopyGPUTextureToTexture(copy, &src, &dst, (Uint32)w, (Uint32)h, 1, false);
    SDL_EndGPUCopyPass(copy);
    if (rd)
        rd->stale = rd->ms != NULL;
    else
        frame_stale = frame_ms != NULL;
}

/* PDO_CAPTURE_STEP=n: at the capture frame, also save the target being
 * drawn to after every n commands (capture_<frame>_<count>.bmp), to find the
 * draw that goes wrong. The frame is replayed once per image. */
static uint32_t replay_limit = UINT32_MAX;
static bool replay_keep;

static void replay(SDL_GPUCommandBuffer *cmd)
{
    static uint8_t *staging;
    static uint32_t staging_cap;
    static SDL_GPUTexture **cmd_tex;
    static uint32_t cmd_tex_cap;
    static pending_upload ups[MAX_UPLOADS];
    int nups = 0;
    uint32_t staging_len = 0;
    SDL_GPURenderPass *pass = NULL;
    Uint32 vbytes = nverts * (Uint32)sizeof *verts, ibytes = nindices * 4;
    Uint32 bbytes = nblob ? (nblob + 8 + 3) & ~3u : 0, cbytes = nvconsts * 16, pbytes;

    /* Resolve every texture first: decoding fills the staging area. */
    Uint64 tex_t0 = SDL_GetTicksNS();
    cmd_tex = grow(cmd_tex, &cmd_tex_cap, ncmds * 4 + 4, sizeof *cmd_tex);
    for (uint32_t i = 0; i < ncmds; i++)
        if (cmds[i].kind == CMD_DRAW)
            for (int s = 0; s < 4; s++)
                cmd_tex[i * 4 + s] = texture_for(&cmds[i].draw.tex[s], &staging, &staging_len,
                                                 &staging_cap, ups, &nups);
    texture_ns += SDL_GetTicksNS() - tex_t0;

    /* A new program buffer (more programs than fit) needs them all again. */
    if (nprogs && (!prog_buf || prog_buf_size < nprogs * 4))
        progs_dirty = true;
    pbytes = progs_dirty ? nprogs * 4 : 0;
    if (vbytes || ibytes || staging_len || bbytes || cbytes || pbytes) {
        SDL_GPUTransferBufferCreateInfo tbi;
        SDL_GPUTransferBuffer *tb;
        SDL_GPUCopyPass *copy;
        uint8_t *map;
        Uint32 boff = vbytes + ibytes, coff = boff + bbytes, poff = coff + cbytes;
        Uint32 tex_off = poff + pbytes;

        if (vbytes)
            vbuf = ensure_buffer(vbuf, &vbuf_size, vbytes, SDL_GPU_BUFFERUSAGE_VERTEX);
        if (ibytes)
            ibuf = ensure_buffer(ibuf, &ibuf_size, ibytes, SDL_GPU_BUFFERUSAGE_INDEX);
        if (bbytes)
            blob_buf = ensure_buffer(blob_buf, &blob_buf_size, bbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (cbytes)
            const_buf = ensure_buffer(const_buf, &const_buf_size, cbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (pbytes)
            prog_buf = ensure_buffer(prog_buf, &prog_buf_size, pbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        SDL_zero(tbi);
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = tex_off + staging_len;
        tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        map = SDL_MapGPUTransferBuffer(dev, tb, false);
        memcpy(map, verts, vbytes);
        memcpy(map + vbytes, indices, ibytes);
        memcpy(map + boff, blob, nblob + 8 < bbytes ? nblob + 8 : bbytes);
        memcpy(map + coff, vconsts, cbytes);
        memcpy(map + poff, progs, pbytes);
        memcpy(map + tex_off, staging, staging_len);
        SDL_UnmapGPUTransferBuffer(dev, tb);

        copy = SDL_BeginGPUCopyPass(cmd);
        {
            struct { SDL_GPUBuffer *b; Uint32 off, n; } up[5] = {
                { vbuf, 0, vbytes }, { ibuf, vbytes, ibytes }, { blob_buf, boff, bbytes },
                { const_buf, coff, cbytes }, { prog_buf, poff, pbytes },
            };
            for (int k = 0; k < 5; k++)
                if (up[k].n && up[k].b) {
                    SDL_GPUTransferBufferLocation src = { tb, up[k].off };
                    SDL_GPUBufferRegion dst = { up[k].b, 0, up[k].n };
                    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
                }
            progs_dirty = false;
        }
        for (int i = 0; i < nups; i++) {
            SDL_GPUTextureTransferInfo src;
            SDL_GPUTextureRegion dst;
            SDL_zero(src);
            src.transfer_buffer = tb;
            src.offset = tex_off + ups[i].offset;
            SDL_zero(dst);
            dst.texture = ups[i].t->tex;
            dst.mip_level = ups[i].level;
            dst.w = ups[i].w;
            dst.h = ups[i].h;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }

    cur_target = NULL;
    for (uint32_t i = 0; i < ncmds; i++) {
        command *c = &cmds[i];
        SDL_GPUGraphicsPipeline *p;
        SDL_GPUTextureSamplerBinding tsb[4];
        SDL_GPUViewport vp;
        int gpu;

        if (i >= replay_limit)
            break;
        if (frames == capture_frame && replay_limit == UINT32_MAX)
            log_command(i, c, cmd_tex + i * 4);

        if (c->kind == CMD_CLEAR) {
            if (pass)
                SDL_EndGPURenderPass(pass);
            pass = begin_pass(cmd, c);
            continue;
        }
        if (c->kind == CMD_TARGET) {
            if (pass)
                SDL_EndGPURenderPass(pass);
            pass = NULL;
            cur_target = target_of(&c->surf, &c->surf2);
            continue;
        }
        if (c->kind == CMD_COPY) {
            if (pass)
                SDL_EndGPURenderPass(pass);
            pass = NULL;
            copy_rect(cmd, c);
            continue;
        }
        gpu = 0;
        if (c->vsh && !(cpu_vsh && c->has_cpu) && vsh_vs && prog_buf && blob_buf && const_buf)
            gpu = 1;
        else if (c->fvf >= 0 && !(cpu_vsh && c->has_cpu) && fvf_vs && blob_buf)
            gpu = 2;
        if ((c->vsh || c->fvf >= 0) && !gpu && !c->has_cpu)
            continue;           /* no vertices to draw it with */
        if (!pass)
            pass = begin_pass(cmd, NULL);
        {
            pdo_pipeline_state ps = c->draw.ps;
            ps.single_sample = cur_target && cur_target->depth_only;
            ps.gpu_vsh = gpu;
            p = get_pipeline(&ps);
        }
        if (!p)
            continue;
        SDL_BindGPUGraphicsPipeline(pass, p);
        vp.x = c->draw.viewport[0] * (float)rscale;
        vp.y = c->draw.viewport[1] * (float)rscale;
        vp.w = c->draw.viewport[2] * (float)rscale;
        vp.h = c->draw.viewport[3] * (float)rscale;
        vp.min_depth = c->draw.viewport[4];
        vp.max_depth = c->draw.viewport[5];
        SDL_SetGPUViewport(pass, &vp);
        bind_geometry(pass, gpu);
        for (int s = 0; s < 4; s++) {
            tsb[s].texture = cmd_tex[i * 4 + s];
            tsb[s].sampler = get_sampler(&c->draw.tex[s]);
            if (tsb[s].texture == cur_color()) {
                /* Sampling the target being drawn to (feedback effects):
                 * the console reads what is there before the draw, so draw
                 * from a copy taken now. */
                SDL_EndGPURenderPass(pass);
                tsb[s].texture = snapshot(cmd);
                pass = begin_pass(cmd, NULL);
                SDL_BindGPUGraphicsPipeline(pass, p);
                SDL_SetGPUViewport(pass, &vp);
                bind_geometry(pass, gpu);
            }
        }
        SDL_BindGPUFragmentSamplers(pass, 0, tsb, 4);
        SDL_PushGPUFragmentUniformData(cmd, 0, &c->draw.ff, sizeof c->draw.ff);
        if (c->draw.ps.stencil_enable)
            SDL_SetGPUStencilReference(pass, c->draw.stencil_ref);
        if (gpu) {
            if (gpu == 1)
                SDL_PushGPUVertexUniformData(cmd, 0, &c->vu, sizeof c->vu);
            else
                SDL_PushGPUVertexUniformData(cmd, 0, &fvf_us[c->fvf], sizeof fvf_us[0]);
            SDL_DrawGPUIndexedPrimitives(pass, c->vsh_index_count, 1, c->vsh_first_index, 0, 0);
        } else {
            SDL_DrawGPUIndexedPrimitives(pass, c->index_count, 1, c->first_index, 0, 0);
        }
    }
    if (pass)
        SDL_EndGPURenderPass(pass);

    if (!replay_keep)
        drop_frame();
}

/* Read the frame target back and write it as a BMP. Slow; debugging only. */
static void save_texture(SDL_GPUCommandBuffer **cmdp, SDL_GPUTexture *tex, int w, int h, const char *path)
{
    SDL_GPUTransferBufferCreateInfo tbi;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUTextureRegion src;
    SDL_GPUTextureTransferInfo dst;
    SDL_GPUCopyPass *copy;
    SDL_GPUFence *fence;
    void *pixels;

    SDL_zero(tbi);
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = (Uint32)(w * h * 4);
    tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb)
        return;
    copy = SDL_BeginGPUCopyPass(*cmdp);
    SDL_zero(src);
    src.texture = tex;
    src.w = (Uint32)w;
    src.h = (Uint32)h;
    src.d = 1;
    SDL_zero(dst);
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(*cmdp);
    *cmdp = NULL;
    if (fence) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    pixels = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (pixels) {
        SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, pixels, w * 4);
        if (s) {
            SDL_SaveBMP(s, path);
            SDL_DestroySurface(s);
        }
        SDL_UnmapGPUTransferBuffer(dev, tb);
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
}

static void save_screenshot(SDL_GPUCommandBuffer **cmdp, const char *dir, const char *name)
{
    char path[1024];

    snprintf(path, sizeof path, "%s/%s_%05llu.bmp", dir, name, (unsigned long long)frames);
    save_texture(cmdp, output, frame_w * rscale, frame_h * rscale, path);
}

/* PDO_CAPTURE_NOSHADOW=1: at the capture frame, save the frame's last
 * target twice, with its shadow maps and with each read as lit
 * (capture_<frame>_shadow.bmp, capture_<frame>_noshadow.bmp). */
static void capture_shadows(void)
{
    const char *e = getenv("PDO_CAPTURE_NOSHADOW");

    if (!e || !atoi(e))
        return;
    replay_keep = true;
    for (int k = 0; k < 2; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        no_shadows = k == 1;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%s.bmp", (unsigned long long)frames,
                 k ? "noshadow" : "shadow");
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    no_shadows = false;
    replay_keep = false;
}

/* PDO_CAPTURE_CPUVSH=1: at the capture frame, save the frame's last target
 * with its vertex programs run on the GPU and on the CPU
 * (capture_<frame>_gpuvsh.bmp, capture_<frame>_cpuvsh.bmp); d3d_hle records
 * both for that frame. */
static void capture_vsh(void)
{
    const char *e = getenv("PDO_CAPTURE_CPUVSH");

    if (!e || !atoi(e))
        return;
    replay_keep = true;
    for (int k = 0; k < 2; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        cpu_vsh = k == 1;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%s.bmp", (unsigned long long)frames,
                 k ? "cpuvsh" : "gpuvsh");
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    cpu_vsh = false;
    replay_keep = false;
}

static void capture_steps(void)
{
    const char *e = getenv("PDO_CAPTURE_STEP");
    uint32_t step = e ? (uint32_t)atol(e) : 0, total = ncmds;

    if (!step)
        return;
    replay_keep = true;
    for (uint32_t lim = step; lim < total; lim += step) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        replay_limit = lim;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%04u.bmp", (unsigned long long)frames, lim);
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    replay_limit = UINT32_MAX;
    replay_keep = false;
}

/* Handles window and key events. Only the thread that made the window may
 * (Windows sends its messages there); elsewhere this does nothing. */
bool pdo_gfx_pump_events(void)
{
    SDL_Event ev;
    bool keep_going = true;

    if (window_thread && SDL_GetCurrentThreadID() != window_thread)
        return true;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT)
            keep_going = false;
        if (pdo_overlay_event(&ev))
            continue;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F11 && !ev.key.repeat) {
            pdo_dump_threads();
            pdo_dump_guest_waits();
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F12 && !ev.key.repeat) {
            capture_frame = frames + 1;
            SDL_Log("capture: frame %llu", (unsigned long long)capture_frame);
        }
    }
    return keep_going;
}

/* The finished frame onto the window, 4:3 or stretched, softened if asked,
 * with the overlay over it. */
static void present_to_window(SDL_GPUCommandBuffer *cmd)
{
    const pdo_settings *st = pdo_settings_get();
    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    SDL_Surface *img;
    bool changed;
    SDL_GPUColorTargetInfo ct;
    SDL_GPURenderPass *pass;
    SDL_GPUViewport vp;

    if (!have_swapchain || !screen_pipeline)
        return;
    img = pdo_overlay_image(&changed);
    if (img && changed && overlay_tex) {
        Uint32 bytes = PDO_OVERLAY_W * PDO_OVERLAY_H * 4;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        if (tb) {
            uint8_t *map = SDL_MapGPUTransferBuffer(dev, tb, false);
            SDL_GPUTextureTransferInfo src;
            SDL_GPUTextureRegion dst;
            SDL_GPUCopyPass *copy;

            for (int y = 0; y < PDO_OVERLAY_H; y++)
                memcpy(map + (size_t)y * PDO_OVERLAY_W * 4, (uint8_t *)img->pixels + (size_t)y * img->pitch,
                       PDO_OVERLAY_W * 4);
            SDL_UnmapGPUTransferBuffer(dev, tb);
            copy = SDL_BeginGPUCopyPass(cmd);
            SDL_zero(src);
            src.transfer_buffer = tb;
            SDL_zero(dst);
            dst.texture = overlay_tex;
            dst.w = PDO_OVERLAY_W;
            dst.h = PDO_OVERLAY_H;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
            SDL_EndGPUCopyPass(copy);
            SDL_ReleaseGPUTransferBuffer(dev, tb);
        }
    }
    {   /* Blocks while the GPU still has the frames in flight: GPU-bound time. */
        Uint64 t0 = SDL_GetTicksNS();
        bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh);
        swap_wait_ns += SDL_GetTicksNS() - t0;
        if (!ok || !swap)
            return;
    }
    SDL_zero(vp);
    if (st->aspect == PDO_ASPECT_STRETCH) {
        vp.w = (float)sw;
        vp.h = (float)sh;
    } else {    /* the game's 4:3, scaled to fit, bars elsewhere */
        float k = SDL_min((float)sw / frame_w, (float)sh / frame_h);
        vp.w = SDL_floorf(frame_w * k);
        vp.h = SDL_floorf(frame_h * k);
        vp.x = SDL_floorf((sw - vp.w) / 2);
        vp.y = SDL_floorf((sh - vp.h) / 2);
    }
    vp.max_depth = 1.0f;
    SDL_zero(ct);
    ct.texture = swap;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color.a = 1.0f;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!pass)
        return;
    {
        SDL_GPUTextureSamplerBinding tsb = { output, linear };
        float params[4] = { st->soft_filter ? 1.0f : 0.0f, 1.0f / (float)frame_h, 0, 0 };
        SDL_SetGPUViewport(pass, &vp);
        SDL_BindGPUGraphicsPipeline(pass, screen_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof params);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    if (img && overlay_tex && overlay_pipeline) {
        SDL_GPUTextureSamplerBinding tsb = { overlay_tex, linear };
        float params[4] = { 0, 0, 0, 0 };
        SDL_BindGPUGraphicsPipeline(pass, overlay_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof params);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

/* The options menu is open: keep the game in this Swap, the sound held,
 * redrawing the last frame with the menu over it until it closes. */
static bool hold_for_overlay(void)
{
    bool keep_going = true;

    paused = true;
    pdo_mixer_set_paused(true);
    while (pdo_overlay_open() && keep_going) {
        SDL_GPUCommandBuffer *cmd;

        keep_going = pdo_gfx_pump_events();
        cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (cmd) {
            present_to_window(cmd);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        if (!have_swapchain)
            SDL_Delay(16);
    }
    pdo_mixer_set_paused(false);
    paused = false;
    return keep_going;
}

bool pdo_gfx_present(void)
{
    SDL_GPUCommandBuffer *cmd;
    bool keep_going = pdo_gfx_pump_events();

    frames++;
    if (!dev)
        return keep_going;

    if (frames == capture_frame) {
        capture_steps();
        capture_shadows();
        capture_vsh();
    }
    {
        /* PDO_NO_DRAW=1: drop the frame's commands unless it is captured or
         * screenshotted, so a debugging run reaches a stage at the game's
         * own pace on a slow (software) GPU. */
        static int no_draw = -1;
        bool shot = shot_dir && frames <= 300 * (uint64_t)shot_every && frames % (uint64_t)shot_every == 0;
        if (no_draw < 0)
            no_draw = getenv("PDO_NO_DRAW") && atoi(getenv("PDO_NO_DRAW")) != 0;
        if (no_draw && !shot && frames != capture_frame) {
            drop_frame();
            return keep_going;
        }
    }
    apply_quality();
    cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd)
        return keep_going;
    replay(cmd);
    present_pass(cmd);

    present_to_window(cmd);

    if (shot_dir && frames <= 300 * (uint64_t)shot_every && frames % (uint64_t)shot_every == 0)
        save_screenshot(&cmd, shot_dir, "frame");
    else if (frames == capture_frame) {
        save_screenshot(&cmd, ".", "capture");
        SDL_Log("capture: saved capture_%05llu.bmp", (unsigned long long)frames);
    }
    if (cmd)
        SDL_SubmitGPUCommandBuffer(cmd);
    pdo_overlay_frame();
    if (pdo_overlay_open())
        keep_going = hold_for_overlay() && keep_going;
    return keep_going;
}
