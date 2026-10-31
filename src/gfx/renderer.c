/* SDL3 GPU renderer for the Direct3D replacement. See renderer.h. */

#include "gfx/renderer.h"

#include "audio/mixer.h"
#include "config.h"
#include "gfx/overlay.h"

#include "gfx/shaders_spv.h"
#include "gfx/xtex.h"
#include "guest/vsh.h"      /* pdo_parallel */

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define PDO_TLS __declspec(thread)
#else
#define PDO_TLS _Thread_local
#endif

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
static SDL_GPUShader *vs, *fs, *fs_ps, *vsh_vs, *fvf_vs;

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

#define PDO_FEAT 40     /* first and last vertex (8), matrices (32) or folded constants (16) */

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
        /* frame interpolation (match_frames): see shaders/vsh.vert */
        int32_t interp[4];
        int32_t prev_in[16];
        float   blend_t[4];
    } vu;
    /* What the draw drew from, to find it again in the next frame: the
     * host address of its (first) vertex data and its vertex count. */
    uintptr_t id_src;
    uint32_t id_nv;
    /* CPU-made vertices: where the draw's start in verts, and the paired
     * draw's in the previous frame (-1: none); unpaired: for the tint. */
    uint32_t vbase;
    int32_t prev_vbase;
    bool unpaired;
    /* Pairing (match_frames): where the draw is, as numbers to compare
     * (feat_ok: worked out), and how they moved since its partner in the
     * frame before (has_vel), to guess where it is in the next. */
    bool feat_ok, has_vel;
    float feat[PDO_FEAT], vel[PDO_FEAT];
} command;

/* ── Threads ──────────────────────────────────────────────────────────────
 * The game runs on a thread of its own; this file's GPU work all happens on
 * the main thread, which owns the window. The game thread records a frame
 * into a frame_data and, at Swap, resolves its textures (while guest memory
 * still holds that frame's) and hands it over; the main thread uploads,
 * replays and presents it. Five frame_datas go round: the one being
 * recorded, the latest handed over, the two the main thread holds (the
 * newest it took, and the one before for frame interpolation), and a spare.
 * The game never waits for drawing: a frame handed over while the one
 * before was not taken yet replaces it. F is the one this thread works on. */

typedef struct gtex gtex;
typedef struct pending_upload pending_upload;

/* A draw's texture stage as resolved on the game thread: a texture, or a
 * render target (target_w not 0) that the main thread looks up by its
 * memory and size, where it lives. */
typedef struct tex_slot {
    SDL_GPUTexture *tex;
    uint32_t target_data, target_w, target_h;
} tex_slot;

typedef struct frame_data {
    uint64_t frame;
    command *cmds;
    uint32_t ncmds, cap_cmds;
    pdo_vertex *verts;
    uint32_t nverts, cap_verts;
    uint32_t *indices;
    uint32_t nindices, cap_indices;
    /* Vertex-program and fixed-function draws on the GPU: their stream
     * bytes, constant sets and uniforms. */
    uint8_t *blob;
    uint32_t nblob, cap_blob;
    float (*vconsts)[4];
    uint32_t nvconsts, cap_vconsts;
    int32_t last_consts;
    pdo_fvf_uniforms *fvf_us;
    uint32_t nfvf_us, cap_fvf_us;
    /* Resolved at Swap: four slots a command, texels to upload. */
    tex_slot *tex;
    uint32_t cap_tex;
    uint8_t *staging;
    uint32_t staging_len, staging_cap;
    pending_upload *ups;
    int nups;
    /* Every vertex program so far, when one was added since the last
     * frame handed over; the gamma ramp, when it changed. */
    uint32_t *progs;
    uint32_t nprogs, cap_progs;
    bool progs_new, gamma_new;
    uint8_t gamma[256 * 4];
    Uint64 stamp;           /* when it was handed over */
    /* Main thread: the frame's own GPU copies, uploaded once (on_gpu) and
     * used for every frame shown from it. */
    SDL_GPUBuffer *gvb, *gib, *gblob, *gconst;
    Uint32 gvb_size, gib_size, gblob_size, gconst_size;
    bool on_gpu;
} frame_data;

static frame_data pool[5];
static PDO_TLS frame_data *F;

static SDL_Mutex *q_lock;
static SDL_Condition *q_cond;
static frame_data *ready;           /* handed over, not drawn yet */
static uint32_t frames_dropped;     /* replaced before they were drawn */
static uint32_t frames_drawn;       /* by the main thread */

void pdo_gfx_take_frame_counts(uint32_t *drawn, uint32_t *dropped)
{
    SDL_LockMutex(q_lock);
    *drawn = frames_drawn;
    *dropped = frames_dropped;
    frames_drawn = frames_dropped = 0;
    SDL_UnlockMutex(q_lock);
}
static frame_data *held_cur;        /* the main thread's newest */
static frame_data *held_prev;       /* and the one before it */
static bool game_done;              /* the game thread has ended */
static int init_w, init_h, init_state;  /* pdo_gfx_init asked of the main thread:
                                         * 1 asked, 2 done, 3 failed */
static SDL_ThreadID main_thread;

static bool on_main_thread(void)
{
    return !main_thread || SDL_GetCurrentThreadID() == main_thread;
}

static bool init_here(int width, int height);

/* Every program compiled so far (game thread). */
static uint32_t *progs;
static uint32_t nprogs, cap_progs;
static bool progs_dirty;
static bool cpu_vsh;        /* captures only: GPU-vertex draws drawn from the CPU's vertices */

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return p;
    while (*cap < need)
        *cap = *cap ? *cap * 2 : 1024;
    return SDL_realloc(p, (size_t)*cap * elem);
}

/* Forget a frame's commands and what they recorded. */
static void reset_frame(frame_data *f)
{
    f->nverts = f->nindices = f->ncmds = 0;
    f->nblob = f->nvconsts = f->nfvf_us = 0;
    f->last_consts = -1;
    f->staging_len = 0;
    f->nups = 0;
    f->progs_new = f->gamma_new = false;
    f->on_gpu = false;
}

/* The game thread records into pool[0] first. */
#define RECORDING() do { if (!F) { F = &pool[0]; reset_frame(F); } } while (0)

static command *new_command(void)
{
    F->cmds = grow(F->cmds, &F->cap_cmds, F->ncmds + 1, sizeof *F->cmds);
    memset(&F->cmds[F->ncmds], 0, sizeof *F->cmds);
    F->cmds[F->ncmds].fvf = -1;
    return &F->cmds[F->ncmds++];
}

/* Bytes the vertex shaders read, 4-aligned with 8 bytes of slack past the
 * end for their word reads. Returns where they start. */
static uint32_t add_blob(const uint8_t *data, uint32_t n)
{
    uint32_t at;

    F->nblob = (F->nblob + 3) & ~3u;
    F->blob = grow(F->blob, &F->cap_blob, F->nblob + n + 8, 1);
    memcpy(F->blob + F->nblob, data, n);
    at = F->nblob;
    F->nblob += n;
    memset(F->blob + F->nblob, 0, 8);
    return at;
}

void pdo_gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    RECORDING();
    command *c = new_command();
    c->kind = CMD_CLEAR;
    c->clear_flags = flags;
    c->clear_color = color;
    c->clear_z = z;
    c->clear_stencil = stencil;
}

static uintptr_t draw_src;     /* pdo_gfx_set_draw_source, for the next draw */


void pdo_gfx_draw(const pdo_draw *d, const pdo_vertex *v, uint32_t nv,
                  const uint32_t *idx, uint32_t ni)
{
    command *c;

    RECORDING();

    if (!nv || !ni)
        return;
    F->verts = grow(F->verts, &F->cap_verts, F->nverts + nv, sizeof *F->verts);
    F->indices = grow(F->indices, &F->cap_indices, F->nindices + ni, sizeof *F->indices);
    memcpy(F->verts + F->nverts, v, nv * sizeof *v);
    for (uint32_t i = 0; i < ni; i++)
        F->indices[F->nindices + i] = F->nverts + idx[i];
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
            SDL_Log("gfx draw @%llu: %u F->verts %u idx topo %u blend %u %x/%x z %u tex0 %08x %s %ux%u "
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
    c->first_index = F->nindices;
    c->index_count = ni;
    c->vbase = F->nverts;
    c->id_src = draw_src;
    c->id_nv = nv;
    draw_src = 0;
    F->nverts += nv;
    F->nindices += ni;
}

void pdo_gfx_set_draw_source(const void *src)
{
    draw_src = (uintptr_t)src;
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

    RECORDING();

    if (!nv || !ni || g->program < 0)
        return;
    if (cpu) {
        pdo_gfx_draw(d, cpu, nv, idx, ni);
        c = &F->cmds[F->ncmds - 1];
        c->has_cpu = true;
    } else {
        c = new_command();
        c->draw = *d;
    }
    c->vsh = true;
    c->id_nv = nv;

    /* The streams, 4-aligned; 8 bytes of slack past the end for the
     * shader's word reads. */
    for (int k = 0; k < 16; k++)
        if (g->stream_bytes[k] && g->stream_data[k]) {
            sbase[k] = add_blob(g->stream_data[k], g->stream_bytes[k]);
            if (!c->id_src)
                c->id_src = (uintptr_t)g->stream_data[k];
        }

    /* The constants, shared with the draw before when they are the same. */
    if (F->last_consts >= 0 && !memcmp(F->vconsts[F->last_consts], g->constants, 192 * 16)) {
        consts = F->last_consts;
    } else {
        F->vconsts = grow(F->vconsts, &F->cap_vconsts, F->nvconsts + 192, sizeof *F->vconsts);
        memcpy(F->vconsts[F->nvconsts], g->constants, 192 * 16);
        consts = (int32_t)F->nvconsts;
        F->last_consts = consts;
        F->nvconsts += 192;
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

    F->indices = grow(F->indices, &F->cap_indices, F->nindices + ni, sizeof *F->indices);
    memcpy(F->indices + F->nindices, idx, ni * sizeof *idx);
    c->vsh_first_index = F->nindices;
    c->vsh_index_count = ni;
    F->nindices += ni;
}

bool pdo_gfx_draw_fvf(const pdo_draw *d, const pdo_gpu_fvf *g, uint32_t nv,
                      const uint32_t *idx, uint32_t ni, const pdo_vertex *cpu)
{
    command *c;

    RECORDING();

    if (!fvf_vs || !nv || !ni || !g->data)
        return false;
    if (cpu) {
        pdo_gfx_draw(d, cpu, nv, idx, ni);
        c = &F->cmds[F->ncmds - 1];
        c->has_cpu = true;
    } else {
        c = new_command();
        c->draw = *d;
    }
    F->fvf_us = grow(F->fvf_us, &F->cap_fvf_us, F->nfvf_us + 1, sizeof *F->fvf_us);
    /* An unlit draw's lights and material are never read: leave them. */
    memcpy(&F->fvf_us[F->nfvf_us], &g->u, (g->u.layout0[2] & PDO_FVF_LIT) ? sizeof g->u
                                    : offsetof(pdo_fvf_uniforms, material));
    F->fvf_us[F->nfvf_us].layout0[0] = (int32_t)add_blob(g->data, g->bytes);
    c->fvf = (int32_t)F->nfvf_us++;
    c->id_src = (uintptr_t)g->data;
    c->id_nv = nv;
    F->indices = grow(F->indices, &F->cap_indices, F->nindices + ni, sizeof *F->indices);
    memcpy(F->indices + F->nindices, idx, ni * sizeof *idx);
    c->vsh_first_index = F->nindices;
    c->vsh_index_count = ni;
    F->nindices += ni;
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

/* Per-frame GPU work counts for the timing log: render passes begun,
 * multisample seeds (the resolved image drawn back into the samples),
 * snapshots (copies of the target for draws that sample it), copies. */
static uint32_t n_passes, n_seeds, n_snapshots, n_copies;

void pdo_gfx_take_pass_counts(uint32_t out[4])
{
    out[0] = n_passes; out[1] = n_seeds; out[2] = n_snapshots; out[3] = n_copies;
    n_passes = n_seeds = n_snapshots = n_copies = 0;
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
    SDL_GPUVertexBufferDescription vb[2];
    SDL_GPUVertexAttribute attrs[9];
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

    /* Slot 1: the previous game frame's vertices, for interpolation. */
    SDL_zero(vb);
    for (int i = 0; i < 2; i++) {
        vb[i].slot = (Uint32)i;
        vb[i].pitch = sizeof(pdo_vertex);
        vb[i].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    }
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
    attrs[8].location = 8;
    attrs[8].buffer_slot = 1;
    attrs[8].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[8].offset = offsetof(pdo_vertex, pos);

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
    pi.fragment_shader = ps->combiners && fs_ps ? fs_ps : fs;
    if (!ps->gpu_vsh) {         /* vsh.vert reads its streams from storage buffers */
        pi.vertex_input_state.vertex_buffer_descriptions = vb;
        pi.vertex_input_state.num_vertex_buffers = 2;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = 9;
    }
    pi.primitive_type = ps->topology == 1 ? SDL_GPU_PRIMITIVETYPE_LINELIST
                      : ps->topology == 2 ? SDL_GPU_PRIMITIVETYPE_POINTLIST
                      : ps->topology == 3 ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP
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
    bool     uploaded;      /* the GPU copy holds data with that hash */
    uint64_t staged_in;     /* hand_over: number of the frame with an upload of it
                               (a number: frame_datas are reused, addresses repeat) */
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

/* The game thread's list of the render targets above, by key only, kept
 * the same way as they are made (target_for_surface_as, target_of): which
 * textures are targets has to be known at Swap, on the game thread, while
 * the targets themselves are made on the main thread as it replays. */
static struct { uint32_t data, w, h; } tkeys[MAX_TARGETS];
static int ntkeys;

static bool target_known(uint32_t data, uint32_t w, uint32_t h)
{
    for (int i = 0; i < ntkeys; i++)
        if (tkeys[i].data == phys(data) && tkeys[i].w == w && tkeys[i].h == h)
            return true;
    return false;
}

static void note_target(const pdo_surface *s)
{
    int w, h, i;

    pdo_gfx_surface_size(s, &w, &h);
    if (!s->data || w <= 0 || h <= 0 || w > 4096 || h > 4096 || target_known(s->data, (uint32_t)w, (uint32_t)h))
        return;
    for (i = 0; i < ntkeys; i++)        /* same memory, other size: the old one is gone */
        if (tkeys[i].data == phys(s->data)) {
            tkeys[i] = tkeys[--ntkeys];
            break;
        }
    if (ntkeys == MAX_TARGETS)
        return;
    tkeys[ntkeys].data = phys(s->data);
    tkeys[ntkeys].w = (uint32_t)w;
    tkeys[ntkeys].h = (uint32_t)h;
    ntkeys++;
}

static void note_target_of(const pdo_surface *s, const pdo_surface *z)
{
    pdo_surface scratch;

    if (s->data) {
        note_target(s);
        return;
    }
    if (!s->format_word)
        return;
    scratch = *s;
    scratch.data = z && z->data ? z->data : NO_COLOUR_DATA;
    note_target(&scratch);
}

static pdo_surface cur_depth_surface;

void pdo_gfx_set_depth(const pdo_surface *depth)
{
    cur_depth_surface = *depth;
}

void pdo_gfx_set_target(const pdo_surface *color)
{
    command *c;

    RECORDING();
    c = new_command();
    c->kind = CMD_TARGET;
    c->surf = *color;
    c->surf2 = cur_depth_surface;
    note_target_of(color, &cur_depth_surface);
}

void pdo_gfx_copy_rect(const pdo_surface *src, const pdo_surface *dst,
                       int x, int y, int w, int h, int dx, int dy)
{
    command *c;

    RECORDING();
    c = new_command();
    c->kind = CMD_COPY;
    c->surf = *src;
    c->surf2 = *dst;
    c->rect[0] = x; c->rect[1] = y; c->rect[2] = w; c->rect[3] = h;
    c->rect[4] = dx; c->rect[5] = dy;
    if (dst->data)
        note_target(dst);
}

typedef struct pending_upload {
    gtex    *t;
    uint32_t offset;   /* in the frame's texture transfer buffer */
    uint32_t level, w, h;
} pending_upload;

#define MAX_UPLOADS 2048

/* A texture used this frame, to hash and maybe upload (check_textures). */
static struct tex_check {
    gtex          *g;
    const uint8_t *src;
    pdo_xtex_info  info;
    uint32_t       levels, bytes;
    uint64_t       hash;
} *checks;
static uint32_t nchecks, cap_checks;

/* Find or create the GPU texture for t, and queue it for check_textures
 * the first time it is used in a frame. */
static tex_slot texture_for(const pdo_tex_ref *t)
{
    pdo_xtex_info info;
    const uint8_t *src;
    gtex *g = NULL;
    uint32_t levels;
    tex_slot slot = { white, 0, 0, 0 };

    if (!t->data)
        return slot;
    pdo_xtex_describe(t->format_word, t->size_word, &info);
    if (!info.width || !info.height || info.width > 4096 || info.height > 4096)
        return slot;
    if (target_known(t->data, info.width, info.height)) {
        slot.tex = NULL;
        slot.target_data = t->data;
        slot.target_w = info.width;
        slot.target_h = info.height;
        return slot;
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
        return slot;
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
        { slot.tex = g->tex; return slot; }      /* already checked this frame */
    /* The CPU view of a resource is Data | 0x80000000, as D3D computes it. */
    src = (const uint8_t *)((uintptr_t)(t->data | 0x80000000u) + g_xbox_mem_offset);
    levels = info.known ? pdo_xtex_level_count(&info) : 1;
    if (!g) {
        SDL_GPUTextureCreateInfo ti;
        if (ntextures == MAX_TEXTURES)
            return slot;
        g = &textures[ntextures++];
        memset(g, 0, sizeof *g);
        g->data = t->data;
        g->format_word = t->format_word;
        g->size_word = t->size_word;
        g->w = info.width;
        g->h = info.height;
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
    /* Hashed with the rest of the frame's textures, on the worker threads
     * (check_textures); decoded after, if it changed. */
    checks = grow(checks, &cap_checks, nchecks + 1, sizeof *checks);
    checks[nchecks].g = g;
    checks[nchecks].src = src;
    checks[nchecks].info = info;
    checks[nchecks].levels = levels;
    {   /* the whole mip chain, so a rewrite of any level is seen */
        pdo_xtex_info last;
        uint32_t off;
        pdo_xtex_level(&info, levels - 1, &last, &off);
        checks[nchecks].bytes = info.bytes ? off + last.bytes : 0;
    }
    nchecks++;
    slot.tex = g->tex;
    return slot;
}

/* Hashed in blocks, so one large texture spreads over the workers too. */
#define HASH_BLOCK (256u * 1024u)
static struct { uint32_t check, offset, bytes; uint64_t hash; } *blocks;
static uint32_t nblocks, cap_blocks;

static void hash_blocks(void *ctx, uint32_t first, uint32_t end)
{
    (void)ctx;
    for (uint32_t i = first; i < end; i++)
        blocks[i].hash = hash_bytes(checks[blocks[i].check].src + blocks[i].offset, blocks[i].bytes);
}

/* Hash this frame's textures (in parallel: on the game thread it was a
 * large share of the frame), then decode the ones that changed into
 * *staging and note their uploads. */
static void check_textures(uint8_t **staging, uint32_t *staging_len, uint32_t *staging_cap,
                           pending_upload *ups, int *nups)
{
    nblocks = 0;
    for (uint32_t i = 0; i < nchecks; i++)
        for (uint32_t o = 0; o < checks[i].bytes; o += HASH_BLOCK) {
            blocks = grow(blocks, &cap_blocks, nblocks + 1, sizeof *blocks);
            blocks[nblocks].check = i;
            blocks[nblocks].offset = o;
            blocks[nblocks].bytes = checks[i].bytes - o < HASH_BLOCK ? checks[i].bytes - o : HASH_BLOCK;
            nblocks++;
        }
    pdo_parallel(nblocks, 1, 4, hash_blocks, NULL);
    for (uint32_t i = 0; i < nchecks; i++)
        checks[i].hash = 0;
    for (uint32_t b = 0; b < nblocks; b++) {     /* in order, per texture */
        uint64_t *h = &checks[blocks[b].check].hash;
        *h = rotl64(*h, 23) * 0x9E3779B97F4A7C15ull ^ blocks[b].hash;
    }
    for (uint32_t i = 0; i < nchecks; i++) {
        gtex *g = checks[i].g;
        const uint8_t *src = checks[i].src;
        pdo_xtex_info info = checks[i].info;
        uint32_t levels = checks[i].levels;
        uint64_t h = checks[i].hash;

        if ((g->uploaded && g->hash == h) || *nups + (int)levels > MAX_UPLOADS)
            continue;
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
        g->uploaded = true;
    }
    nchecks = 0;
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

/* The ramp as the game last set it (game thread); it goes over with the
 * next frame. */
static uint8_t game_gamma[256 * 4];
static bool game_gamma_dirty;

void pdo_gfx_set_gamma(const uint8_t ramp[768])
{
    if (getenv("PDO_GAMMA_TRACE"))
        SDL_Log("gfx: frame %llu gamma r[64]=%u r[128]=%u r[255]=%u g[128]=%u b[128]=%u",
                (unsigned long long)frames, ramp[64], ramp[128], ramp[255], ramp[256 + 128], ramp[512 + 128]);
    for (int i = 0; i < 256; i++) {
        game_gamma[i * 4 + 0] = ramp[i];
        game_gamma[i * 4 + 1] = ramp[256 + i];
        game_gamma[i * 4 + 2] = ramp[512 + i];
        game_gamma[i * 4 + 3] = 255;
    }
    game_gamma_dirty = true;
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

/* The game thread asks (Direct3D_CreateDevice); the main thread, which
 * owns the window, does it in pdo_gfx_main_loop. */
bool pdo_gfx_init(int width, int height)
{
    bool ok;

    if (on_main_thread())
        return init_here(width, height);
    if (dev)
        return true;
    SDL_LockMutex(q_lock);
    init_w = width;
    init_h = height;
    init_state = 1;
    SDL_BroadcastCondition(q_cond);
    while (init_state == 1)
        SDL_WaitCondition(q_cond, q_lock);
    ok = init_state == 2;
    SDL_UnlockMutex(q_lock);
    return ok;
}

static bool init_here(int width, int height)
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
    if (have_swapchain && getenv("PDO_BENCHMARK") && atoi(getenv("PDO_BENCHMARK"))) {
        /* Not tied to the monitor's refresh, so the frame rate is the PC's. */
        SDL_GPUPresentMode m = SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE)
                               ? SDL_GPU_PRESENTMODE_IMMEDIATE : SDL_GPU_PRESENTMODE_MAILBOX;
        if (SDL_WindowSupportsGPUPresentMode(dev, window, m)
                && SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, m))
            SDL_Log("gfx: benchmark: presenting %s", m == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediately" : "by mailbox");
        else
            SDL_Log("gfx: benchmark: presentation stays synchronised to the display");
    }

    vs = make_shader(ff_vert_spv, sizeof ff_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    if (getenv("PDO_TEST_SIMPLE_FS")) {     /* a test: how much of the GPU time is ff.frag */
        SDL_Log("gfx: PDO_TEST_SIMPLE_FS: the cheapest fragment shader instead of ff.frag; pictures are wrong");
        fs = make_shader(ff_test_frag_spv, sizeof ff_test_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    } else {
        fs = make_shader(ff_frag_spv, sizeof ff_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
        fs_ps = make_shader(ff_ps_frag_spv, sizeof ff_ps_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    }
    vsh_vs = make_shader_sb(vsh_vert_spv, sizeof vsh_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 5);
    if (!vsh_vs)
        SDL_Log("gfx: no GPU vertex programs (%s); they run on the CPU", SDL_GetError());
    fvf_vs = make_shader_sb(fvf_vert_spv, sizeof fvf_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
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

static SDL_GPUBuffer *prog_buf;
static Uint32 prog_buf_size;

/* A draw's vertices: the vertex buffer, or for a vertex program on the GPU
 * its streams, programs and constants. */
/* Frame interpolation (main thread): the previous game frame, when the
 * frame shown blends towards F from it, and how far (1: F as it is). */
static frame_data *P;
static float blend;
static bool interp_tint;    /* PDO_INTERP_TINT=1: unpaired draws magenta when blending */

static void bind_geometry(SDL_GPURenderPass *pass, int gpu, const command *c)
{
    SDL_GPUBufferBinding ib = { F->gib, 0 };
    SDL_GPUBuffer *pblob = P && P->gblob ? P->gblob : F->gblob;
    SDL_GPUBuffer *pconst = P && P->gconst ? P->gconst : F->gconst;

    if (gpu == 1) {
        SDL_GPUBuffer *sb[5] = { F->gblob, prog_buf, F->gconst, pblob, pconst };
        SDL_BindGPUVertexStorageBuffers(pass, 0, sb, 5);
    } else if (gpu == 2) {
        SDL_GPUBuffer *sb[2] = { F->gblob, pblob };
        SDL_BindGPUVertexStorageBuffers(pass, 0, sb, 2);
    } else {
        /* From the draw's first vertex (drawn with a vertex offset taking
         * it back off the indices); slot 1 the paired draw's, or its own. */
        bool paired = P && P->gvb && c->prev_vbase >= 0;
        SDL_GPUBufferBinding vb[2] = {
            { F->gvb, c->vbase * (Uint32)sizeof(pdo_vertex) },
            { paired ? P->gvb : F->gvb, (paired ? (Uint32)c->prev_vbase : c->vbase) * (Uint32)sizeof(pdo_vertex) },
        };
        SDL_BindGPUVertexBuffers(pass, 0, vb, 2);
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
    n_passes++;
    n_seeds += seed != NULL;
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
    if (c->fvf >= 0) {
        const pdo_fvf_uniforms *u = &F->fvf_us[c->fvf];
        SDL_Log("capture:     fixed-function vertices on the GPU: flags %x, %d lights%s",
                u->layout0[2], u->light_info[0], c->has_cpu ? "; the CPU's vertices:" : "");
        /* The first vertex as the game wrote it: position, colours. */
        if ((uint32_t)u->layout0[0] + (uint32_t)u->layout0[1] <= F->nblob && u->layout0[1] >= 16) {
            const uint8_t *v = F->blob + u->layout0[0];
            float p[4];
            uint32_t dif = 0, spe = 0;
            memcpy(p, v, 16);
            if (u->layout0[2] & PDO_FVF_DIFFUSE)
                memcpy(&dif, v + u->offs[1], 4);
            if (u->layout0[2] & PDO_FVF_SPECULAR)
                memcpy(&spe, v + u->offs[2], 4);
            SDL_Log("capture:     v0 raw %.3f %.3f %.3f %.3f diffuse %08x specular %08x, %u vertices",
                    p[0], p[1], p[2], p[3], dif, spe, c->id_nv);
        }
    }
    for (uint32_t k = 0; k < 3 && k < c->index_count && ((!c->vsh && c->fvf < 0) || c->has_cpu); k++) {
        const pdo_vertex *v = &F->verts[F->indices[c->first_index + k]];
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
    n_snapshots++;
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
    n_copies++;
    if (rd)
        rd->stale = rd->ms != NULL;
    else
        frame_stale = frame_ms != NULL;
}

/* PDO_CAPTURE_STEP=n: at the capture frame, also save the target being
 * drawn to after every n commands (capture_<frame>_<count>.bmp), to find the
 * draw that goes wrong. The frame is replayed once per image. */
static uint32_t replay_limit = UINT32_MAX;

/* A stage's texture, as the game thread resolved it: render targets are
 * looked up here, where they are made. */
static SDL_GPUTexture *slot_texture(const tex_slot *t)
{
    rtarget *r;

    if (t->tex || !t->target_w)
        return t->tex ? t->tex : white;
    r = find_target(t->target_data, t->target_w, t->target_h);
    if (!r)
        return white;
    return r->depth_only ? (no_shadows ? white : r->depth) : r->color;
}

/* The frame's vertices, indices, streams, constants (and any new programs
 * and changed texels) to the GPU, once. */
static void upload_frame(SDL_GPUCommandBuffer *cmd)
{
    uint8_t *staging = F->staging;
    pending_upload *ups = F->ups;
    int nups = F->nups;
    uint32_t staging_len = F->staging_len;
    Uint32 vbytes = F->nverts * (Uint32)sizeof *F->verts, ibytes = F->nindices * 4;
    Uint32 bbytes = F->nblob ? (F->nblob + 8 + 3) & ~3u : 0, cbytes = F->nvconsts * 16, pbytes;

    if (F->on_gpu)
        return;
    F->on_gpu = true;
    /* All the programs again when one was added. */
    pbytes = F->progs_new ? F->nprogs * 4 : 0;
    if (vbytes || ibytes || staging_len || bbytes || cbytes || pbytes) {
        SDL_GPUTransferBufferCreateInfo tbi;
        SDL_GPUTransferBuffer *tb;
        SDL_GPUCopyPass *copy;
        uint8_t *map;
        Uint32 boff = vbytes + ibytes, coff = boff + bbytes, poff = coff + cbytes;
        Uint32 tex_off = poff + pbytes;

        if (vbytes)
            F->gvb = ensure_buffer(F->gvb, &F->gvb_size, vbytes, SDL_GPU_BUFFERUSAGE_VERTEX);
        if (ibytes)
            F->gib = ensure_buffer(F->gib, &F->gib_size, ibytes, SDL_GPU_BUFFERUSAGE_INDEX);
        if (bbytes)
            F->gblob = ensure_buffer(F->gblob, &F->gblob_size, bbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (cbytes)
            F->gconst = ensure_buffer(F->gconst, &F->gconst_size, cbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (pbytes)
            prog_buf = ensure_buffer(prog_buf, &prog_buf_size, pbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        SDL_zero(tbi);
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = tex_off + staging_len;
        tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        map = SDL_MapGPUTransferBuffer(dev, tb, false);
        memcpy(map, F->verts, vbytes);
        memcpy(map + vbytes, F->indices, ibytes);
        memcpy(map + boff, F->blob, F->nblob + 8 < bbytes ? F->nblob + 8 : bbytes);
        memcpy(map + coff, F->vconsts, cbytes);
        memcpy(map + poff, F->progs, pbytes);
        memcpy(map + tex_off, staging, staging_len);
        SDL_UnmapGPUTransferBuffer(dev, tb);

        copy = SDL_BeginGPUCopyPass(cmd);
        {
            struct { SDL_GPUBuffer *b; Uint32 off, n; } up[5] = {
                { F->gvb, 0, vbytes }, { F->gib, vbytes, ibytes }, { F->gblob, boff, bbytes },
                { F->gconst, coff, cbytes }, { prog_buf, poff, pbytes },
            };
            for (int k = 0; k < 5; k++)
                if (up[k].n && up[k].b) {
                    SDL_GPUTransferBufferLocation src = { tb, up[k].off };
                    SDL_GPUBufferRegion dst = { up[k].b, 0, up[k].n };
                    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
                }
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
}

static void replay(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPURenderPass *pass = NULL;

    upload_frame(cmd);
    cur_target = NULL;
    for (uint32_t i = 0; i < F->ncmds; i++) {
        command *c = &F->cmds[i];
        SDL_GPUGraphicsPipeline *p;
        SDL_GPUTextureSamplerBinding tsb[4];
        SDL_GPUViewport vp;
        int gpu;

        if (i >= replay_limit)
            break;
        SDL_GPUTexture *tex[4];

        if (c->kind == CMD_DRAW)
            for (int s = 0; s < 4; s++)
                tex[s] = slot_texture(&F->tex[i * 4 + s]);
        if (F->frame == capture_frame && replay_limit == UINT32_MAX)
            log_command(i, c, tex);

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
        if (c->vsh && !(cpu_vsh && c->has_cpu) && vsh_vs && prog_buf && F->gblob && F->gconst)
            gpu = 1;
        else if (c->fvf >= 0 && !(cpu_vsh && c->has_cpu) && fvf_vs && F->gblob)
            gpu = 2;
        if ((c->vsh || c->fvf >= 0) && !gpu && !c->has_cpu)
            continue;           /* no vertices to draw it with */
        if (!pass)
            pass = begin_pass(cmd, NULL);
        {
            pdo_pipeline_state ps = c->draw.ps;
            ps.single_sample = cur_target && cur_target->depth_only;
            ps.gpu_vsh = gpu;
            ps.combiners = (c->draw.ff.ps_final[2] & 0xFF) != 0;
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
        bind_geometry(pass, gpu, c);
        for (int s = 0; s < 4; s++) {
            tsb[s].texture = tex[s];
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
                bind_geometry(pass, gpu, c);
            }
        }
        SDL_BindGPUFragmentSamplers(pass, 0, tsb, 4);
        SDL_PushGPUFragmentUniformData(cmd, 0, &c->draw.ff, sizeof c->draw.ff);
        if (c->draw.ps.stencil_enable)
            SDL_SetGPUStencilReference(pass, c->draw.stencil_ref);
        if (gpu) {
            float t = P ? blend : 1.0f;
            int tint = P && interp_tint && c->unpaired;
            if (gpu == 1) {
                c->vu.blend_t[0] = t;
                c->vu.interp[2] = tint;
                SDL_PushGPUVertexUniformData(cmd, 0, &c->vu, sizeof c->vu);
            } else {
                F->fvf_us[c->fvf].blend_t[0] = t;
                F->fvf_us[c->fvf].interp[2] = tint;
                SDL_PushGPUVertexUniformData(cmd, 0, &F->fvf_us[c->fvf], sizeof F->fvf_us[0]);
            }
            SDL_DrawGPUIndexedPrimitives(pass, c->vsh_index_count, 1, c->vsh_first_index, 0, 0);
        } else {
            float b[4] = { P ? blend : 1.0f, P && c->prev_vbase >= 0 ? 1.0f : 0.0f,
                           P && interp_tint && c->unpaired && c->id_src ? 1.0f : 0.0f, 0.0f };
            SDL_PushGPUVertexUniformData(cmd, 0, b, sizeof b);
            SDL_DrawGPUIndexedPrimitives(pass, c->index_count, 1, c->first_index, -(Sint32)c->vbase, 0);
        }
    }
    if (pass)
        SDL_EndGPURenderPass(pass);
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

    snprintf(path, sizeof path, "%s/%s_%05llu.bmp", dir, name, (unsigned long long)F->frame);
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
    for (int k = 0; k < 2; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        no_shadows = k == 1;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%s.bmp", (unsigned long long)F->frame,
                 k ? "noshadow" : "shadow");
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    no_shadows = false;
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
    for (int k = 0; k < 2; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        cpu_vsh = k == 1;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%s.bmp", (unsigned long long)F->frame,
                 k ? "cpuvsh" : "gpuvsh");
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    cpu_vsh = false;
}

/* PDO_CAPTURE_INTERP=1: at the capture frame, save the frame between the
 * one before and it at t 0, 0.5 and 1 (capture_<frame>_t000.bmp, _t050,
 * _t100): matched draws should sit halfway in the middle one. */
static void match_frames(frame_data *prev, frame_data *cur);

static void capture_interp(frame_data *prev, frame_data *cur)
{
    const char *e = getenv("PDO_CAPTURE_INTERP");
    static const float ts[3] = { 0.0f, 0.5f, 1.0f };

    if (!e || !atoi(e))
        return;
    match_frames(prev, cur);
    for (int k = 0; k < 3; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        F = prev;           /* prev on the GPU first, as when interpolating */
        upload_frame(cmd);
        F = cur;
        P = prev;
        blend = ts[k];
        replay(cmd);
        cur_target = NULL;
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_t%03d.bmp", (unsigned long long)cur->frame,
                 (int)(ts[k] * 100));
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    P = NULL;
    blend = 1.0f;
}

static void capture_steps(void)
{
    const char *e = getenv("PDO_CAPTURE_STEP");
    uint32_t step = e ? (uint32_t)atol(e) : 0, total = F->ncmds;

    if (!step)
        return;
    for (uint32_t lim = step; lim < total; lim += step) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        replay_limit = lim;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%04u.bmp", (unsigned long long)F->frame, lim);
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    replay_limit = UINT32_MAX;
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

/* ── Frame interpolation ─────────────────────────────────────────────────
 * The game runs at 60; frames between two of its frames are drawn by
 * blending each draw of the newer frame with the same draw in the frame
 * before. "The same draw" is a guess from what the draw is: the kind of
 * draw, its program (or fixed-function layout), vertex and index counts and
 * first texture -- not the address of its vertices, which for everything
 * the game writes each frame (menus, HUD, much of the scene) moves every
 * frame, so nothing paired. Draws alike in
 * all of that (a flock of one enemy) pair up nearest first: by how little
 * their matrices, or their program constants, differ between the frames.
 * The game's draw order is no guide -- it sorts see-through things by
 * depth, and pairing in order swapped them as they flew. A group whose
 * size changed (a pack losing and gaining members) still pairs as far as
 * it goes: refusing the whole group made it jump between the blended past
 * and the unblended newer frame as members came and went. A draw whose
 * partner is far beyond its own nearest is not blended either. A
 * fixed-function draw whose origin is on screen in both frames and jumps
 * (a teleport, a new object recycling a buffer) is not blended either, and
 * when over half of those on screen do, the camera cut and nothing is.
 * Draws are keyed without their vertex data address: dynamic vertices sit
 * somewhere new every frame. Unblended draws show the newer frame. */

typedef struct draw_key {
    uint64_t h;
    uint32_t cmd;
} draw_key;

/* loose 1: the texture aside; 2: the vertex and index counts too. */
static uint64_t key_of(const frame_data *f, const command *c, int loose)
{
    uint64_t h = 1469598103934665603ull;
    uint64_t v[7];

    v[0] = c->vsh ? 1 : c->fvf >= 0 ? 2 : 3;
    v[1] = c->vsh ? (uint64_t)(uint32_t)c->vu.info[0]
         : c->fvf >= 0 ? ((uint64_t)(uint32_t)f->fvf_us[c->fvf].layout0[2] << 32) | (uint32_t)f->fvf_us[c->fvf].layout0[1]
         : c->draw.ps.topology;
    v[2] = 0;       /* not the address: dynamic vertices move every frame */
    v[3] = loose >= 2 ? 0 : c->id_nv;
    v[4] = loose >= 2 ? 0 : c->vsh || c->fvf >= 0 ? c->vsh_index_count : c->index_count;
    v[5] = loose ? 0 : c->draw.tex[0].data;
    v[6] = c->vsh ? (uint64_t)(uint32_t)c->vu.info[1] : 0;
    for (int i = 0; i < 7; i++)
        h = (h ^ v[i]) * 1099511628211ull;
    return h;
}

static int cmp_key(const void *a, const void *b)
{
    const draw_key *x = a, *y = b;
    if (x->h != y->h)
        return x->h < y->h ? -1 : 1;
    return x->cmd < y->cmd ? -1 : x->cmd > y->cmd;
}

/* Draws on the GPU from the game's bytes, or from CPU-made vertices with
 * their source known (pdo_gfx_set_draw_source). */
static bool blendable(const command *c)
{
    return c->kind == CMD_DRAW && c->id_src;
}

/* Whether a draw has matrices or constants to blend on their own, when its
 * vertices cannot be (their count changed): not CPU-made or pretransformed
 * ones, placed by their vertices alone. */
static bool has_placement(const frame_data *f, const command *c)
{
    return c->vsh || (c->fvf >= 0 && !(f->fvf_us[c->fvf].layout0[2] & PDO_FVF_PRE));
}

/* The keys of f's blendable draws, sorted; loose (key_of): only the draws
 * skip[] does not mark. */
static uint32_t keys_of(const frame_data *f, draw_key **keys, uint32_t *cap, int loose, const uint8_t *skip)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < f->ncmds; i++)
        if (blendable(&f->cmds[i]) && !(loose && skip[i]) && !(loose >= 2 && !has_placement(f, &f->cmds[i]))) {
            *keys = grow(*keys, cap, n + 1, sizeof **keys);
            (*keys)[n].h = key_of(f, &f->cmds[i], loose);
            (*keys)[n].cmd = i;
            n++;
        }
    qsort(*keys, n, sizeof **keys, cmp_key);
    return n;
}

/* Where a fixed-function draw's origin lands on screen (NDC), and whether
 * it is in front of the eye. */
static bool origin_ndc(const float *wvp, float *x, float *y)
{
    float w = wvp[15];
    if (w <= 1e-4f)
        return false;
    *x = wvp[12] / w;
    *y = wvp[13] / w;
    return true;
}

/* Where a draw's first and last vertices are, as the raw floats of its
 * position (fixed-function), or of its first float input (programs):
 * screen pixels for pretransformed sprites, so that HUD pieces drawn alike
 * (one shared vertex area, four vertices) tell apart. False: none read. */
static bool draw_corners(const frame_data *f, const command *c, float out[8])
{
    uint32_t base, stride, n = c->id_nv ? c->id_nv : 1;

    if (!c->vsh && c->fvf < 0) {           /* CPU-made: clip-space positions */
        if (c->vbase + n > f->nverts)
            return false;
        memcpy(out, f->verts[c->vbase].pos, 16);
        memcpy(out + 4, f->verts[c->vbase + n - 1].pos, 16);
        return true;
    }
    if (c->vsh) {
        int r;
        for (r = 0; r < 16; r++)
            if ((c->vu.inputs[r][3] & 15) == 2 && (c->vu.inputs[r][3] >> 4) >= 3)
                break;
        if (r == 16)
            return false;
        base = (uint32_t)c->vu.inputs[r][0] + (uint32_t)c->vu.inputs[r][2];
        stride = (uint32_t)c->vu.inputs[r][1];
    } else {
        base = (uint32_t)f->fvf_us[c->fvf].layout0[0];
        stride = (uint32_t)f->fvf_us[c->fvf].layout0[1];
    }
    for (int k = 0; k < 2; k++) {
        uint32_t at = base + (k ? (n - 1) * stride : 0);
        if (at + 16 > f->nblob)
            return false;
        memcpy(out + k * 4, f->blob + at, 16);
    }
    return true;
}

/* A program draw's constants folded to 16 numbers, each a fixed +-1
 * weighted sum: the fold is linear, so two draws' folds differ by the fold
 * of their difference -- what all objects share cancels, and what places
 * one object (its matrix) shows -- at a 48th of the cost of comparing all
 * 768 floats. */
static void fold_consts(const float *k, float out[16])
{
    static float w[192 * 4][16];
    static bool w_ok;

    if (!w_ok) {
        for (int i = 0; i < 192 * 4; i++)
            for (int o = 0; o < 16; o++)
                w[i][o] = (((uint32_t)i * 2654435761u) >> (o + 8)) & 1 ? 1.0f : -1.0f;
        w_ok = true;
    }
    for (int o = 0; o < 16; o++)
        out[o] = 0.0f;
    for (int i = 0; i < 192 * 4; i++) {
        float v = k[i];
        if (!(SDL_fabsf(v) <= 1e30f))
            continue;               /* NaN or huge: unused registers */
        for (int o = 0; o < 16; o++)
            out[o] += v * w[i][o];
    }
}

/* What pairing compares of a draw (c->feat): its first and last vertices,
 * and its matrices (fixed-function) or folded constants (programs).
 * Copies of one model have the same vertices, so the matrices or constants
 * are what tell them apart: the vertices alone paired a row of buildings
 * in draw order, and when that shifted each blended towards its
 * neighbour. */
static void draw_features(const frame_data *f, command *c)
{
    float *x = c->feat;

    if (c->feat_ok)
        return;
    SDL_memset(x, 0, sizeof c->feat);
    if (!draw_corners(f, c, x))
        SDL_memset(x, 0, 8 * sizeof *x);
    if (c->vsh) {
        fold_consts(f->vconsts[c->vu.info[2]], x + 8);
    } else if (c->fvf >= 0) {
        memcpy(x + 8, f->fvf_us[c->fvf].wvp, 16 * sizeof *x);
        memcpy(x + 24, f->fvf_us[c->fvf].world, 16 * sizeof *x);
    }
    for (int i = 0; i < PDO_FEAT; i++)
        if (!(SDL_fabsf(x[i]) <= 1e30f))
            x[i] = 0.0f;            /* NaN or huge */
    c->feat_ok = true;
}

/* How far a draw of this frame is from an alike draw of the previous one:
 * from where that one was, or from where it was going (its place moved on
 * by how it moved since the frame before), whichever is nearer. Without
 * the guess, a shot or a flock member that moves further in a frame than
 * the gap to the next one paired with that next one (or with none); with
 * the guess alone, something that stopped (a menu piece done sliding in)
 * was looked for one step on, where an alike neighbour could be nearer. */
static float draw_distance(const command *pc, const command *c)
{
    float tie = pc->id_src == c->id_src ? 0.0f : 1e-3f;   /* same data: a tie-breaker */
    float d = 0.0f, dv = 0.0f;

    for (int i = 0; i < PDO_FEAT; i++)
        d += SDL_fabsf(pc->feat[i] - c->feat[i]);
    if (pc->has_vel) {
        for (int i = 0; i < PDO_FEAT; i++)
            dv += SDL_fabsf(pc->feat[i] + pc->vel[i] - c->feat[i]);
        if (dv < d)
            d = dv;
    }
    return d + tie;
}

/* A pair made: how the draw moved, for the guess in the next frame. */
static void note_motion(const frame_data *pf, command *pc, const frame_data *cf, command *c)
{
    draw_features(pf, pc);
    draw_features(cf, c);
    for (int i = 0; i < PDO_FEAT; i++)
        c->vel[i] = c->feat[i] - pc->feat[i];
    c->has_vel = true;
}

typedef struct pair_cand {
    float d;
    uint16_t a, b;      /* index in the group: previous frame, this frame */
} pair_cand;

static int cmp_cand(const void *x, const void *y)
{
    const pair_cand *a = x, *b = y;
    return a->d < b->d ? -1 : a->d > b->d;
}

static uint32_t st_frames, st_draws, st_blendable, st_paired, st_jumps, st_cuts, st_far, st_sprite, st_ambig;

/* Pair cur's draws with prev's (prev may be NULL: nothing pairs). */
static void match_frames(frame_data *prev, frame_data *cur)
{
    static draw_key *pk, *ck;
    static uint8_t *taken, *grouped;    /* prev draws paired; cur draws that had alike ones */
    static uint32_t cap_pk, cap_ck, cap_taken, cap_grouped;
    uint32_t np = 0, nc, i, j, fvf_pairs = 0, fvf_jumps = 0;

    for (uint32_t k = 0; k < cur->ncmds; k++) {
        command *c = &cur->cmds[k];
        if (c->kind != CMD_DRAW)
            continue;
        c->prev_vbase = -1;
        c->unpaired = true;
        c->feat_ok = c->has_vel = false;
        if (c->vsh) {
            c->vu.interp[0] = -1;
            c->vu.interp[1] = 0;
        }
        if (c->fvf >= 0) {
            cur->fvf_us[c->fvf].interp[0] = -1;
            cur->fvf_us[c->fvf].interp[1] = 0;
        }
    }
    if (!prev)
        return;
    taken = grow(taken, &cap_taken, prev->ncmds + 1, 1);
    grouped = grow(grouped, &cap_grouped, cur->ncmds + 1, 1);
    memset(taken, 0, prev->ncmds);
    memset(grouped, 0, cur->ncmds);
    /* Three passes: alike draws (same texture too); then what found none
     * with what is left, the texture aside -- effects and shots whose
     * texture animates (a new picture each frame) otherwise never paired
     * and moved at the game's rate among blended things; then the vertex
     * count aside too -- a trail that grows or shrinks each frame -- for
     * its matrices or constants alone (the camera's move then is smooth on
     * it too). */
    for (int pass = 0; pass < 3; pass++) {
    np = keys_of(prev, &pk, &cap_pk, pass, taken);
    nc = keys_of(cur, &ck, &cap_ck, pass, grouped);
    i = j = 0;
    while (i < np && j < nc) {
        uint32_t i2 = i, j2 = j;
        if (pk[i].h < ck[j].h) { i++; continue; }
        if (pk[i].h > ck[j].h) { j++; continue; }
        while (i2 < np && pk[i2].h == pk[i].h) i2++;
        while (j2 < nc && ck[j2].h == ck[j].h) j2++;
        for (uint32_t k = j; k < j2; k++)
            grouped[ck[k].cmd] = 1;
        if (i2 - i <= 256 && j2 - j <= 256) {
            uint32_t na = i2 - i, n = j2 - j, order[256];
            bool forced[256] = { false };
            static pair_cand *cand;
            static uint32_t cap_cand;

            /* Which previous draw each one pairs with: alone in both, the
             * one; otherwise nearest first, as many as pair (a pack whose
             * size changed: the rest of it stays blended, the new ones
             * show the newer frame). */
            for (uint32_t b = 0; b < n; b++)
                forced[b] = true;           /* until paired */
            if (n == 1 && na == 1) {
                order[0] = 0;
                forced[0] = false;
            } else {
                bool used_a[256] = { false }, used_b[256] = { false };
                uint32_t nc2 = 0;
                cand = grow(cand, &cap_cand, na * n, sizeof *cand);
                for (uint32_t a = 0; a < na; a++)
                    draw_features(prev, &prev->cmds[pk[i + a].cmd]);
                for (uint32_t b = 0; b < n; b++)
                    draw_features(cur, &cur->cmds[ck[j + b].cmd]);
                for (uint32_t a = 0; a < na; a++)
                    for (uint32_t b = 0; b < n; b++) {
                        cand[nc2].d = draw_distance(&prev->cmds[pk[i + a].cmd], &cur->cmds[ck[j + b].cmd]);
                        cand[nc2].a = (uint16_t)a;
                        cand[nc2].b = (uint16_t)b;
                        nc2++;
                    }
                float best[256], second[256];
                for (uint32_t b = 0; b < n; b++)
                    best[b] = second[b] = 3.4e38f;
                for (uint32_t k = 0; k < nc2; k++)
                    if (cand[k].d < best[cand[k].b]) {
                        second[cand[k].b] = best[cand[k].b];
                        best[cand[k].b] = cand[k].d;
                    } else if (cand[k].d < second[cand[k].b]) {
                        second[cand[k].b] = cand[k].d;
                    }
                qsort(cand, nc2, sizeof *cand, cmp_cand);
                for (uint32_t k = 0; k < nc2; k++)
                    if (!used_a[cand[k].a] && !used_b[cand[k].b]) {
                        used_a[cand[k].a] = used_b[cand[k].b] = true;
                        order[cand[k].b] = cand[k].a;
                        /* Its nearest went to another: what is left can be
                         * far (the end of a chain of alike segments that
                         * each moved one along). Not blended. */
                        forced[cand[k].b] = cand[k].d > best[cand[k].b] * 2.0f + 1e-3f;
                        /* No clear nearest: two alike draws (the pieces of a
                         * turning ring, a pack of one enemy) fit about as
                         * well. Blending with the wrong one sends it across
                         * the screen and back; not blended, it only moves at
                         * the game's rate. Something that did not move
                         * (distance near 0) is never in doubt. */
                        if (!forced[cand[k].b] && cand[k].d > 1e-2f && second[cand[k].b] < cand[k].d * 1.25f) {
                            forced[cand[k].b] = true;
                            st_ambig++;
                        }
                    }
            }
            for (uint32_t k = 0; k < n; k++) {
                command *pc;
                command *c = &cur->cmds[ck[j + k].cmd];
                bool same_nv;
                if (forced[k]) {
                    st_far++;
                    continue;           /* no partner (order[k] unset) */
                }
                pc = &prev->cmds[pk[i + order[k]].cmd];
                same_nv = c->id_nv == pc->id_nv && c->vsh_index_count == pc->vsh_index_count;
                if (!c->vsh && c->fvf < 0 && !pc->vsh && pc->fvf < 0) {
                    c->prev_vbase = (int32_t)pc->vbase;
                    c->unpaired = false;
                    taken[pk[i + order[k]].cmd] = 1;
                    note_motion(prev, pc, cur, c);
                } else if (c->vsh && pc->vsh) {
                    bool same_in = true;
                    for (int r = 0; r < 16; r++) {
                        if (c->vu.inputs[r][1] != pc->vu.inputs[r][1] || c->vu.inputs[r][2] != pc->vu.inputs[r][2] ||
                            c->vu.inputs[r][3] != pc->vu.inputs[r][3])
                            same_in = false;
                        c->vu.prev_in[r] = pc->vu.inputs[r][0];
                    }
                    c->vu.interp[0] = pc->vu.info[2];
                    c->vu.interp[1] = same_in && same_nv;
                    c->unpaired = false;
                    taken[pk[i + order[k]].cmd] = 1;
                    note_motion(prev, pc, cur, c);
                } else if (c->fvf >= 0 && pc->fvf >= 0) {
                    pdo_fvf_uniforms *u = &cur->fvf_us[c->fvf];
                    const pdo_fvf_uniforms *pu = &prev->fvf_us[pc->fvf];
                    float x0, y0, x1, y1, a4[8], b4[8];
                    if ((u->layout0[2] & PDO_FVF_PRE) && draw_corners(prev, pc, a4) && draw_corners(cur, c, b4) &&
                        (SDL_fabsf(a4[0] - b4[0]) > u->rt[0] * 0.25f || SDL_fabsf(a4[1] - b4[1]) > u->rt[1] * 0.25f))
                    {
                        st_sprite++;
                        continue;       /* a sprite that jumped: not blended */
                    }
                    /* Jumped: only judged with the origin on screen in both
                     * frames -- near the eye or off screen it swings far
                     * while the object hardly moves, and judged there it
                     * made every turn of the camera look like a cut. */
                    if (!(u->layout0[2] & PDO_FVF_PRE) && origin_ndc(pu->wvp, &x0, &y0) &&
                        origin_ndc(u->wvp, &x1, &y1) && SDL_fabsf(x0) < 1.0f && SDL_fabsf(y0) < 1.0f &&
                        SDL_fabsf(x1) < 1.0f && SDL_fabsf(y1) < 1.0f) {
                        fvf_pairs++;
                        if (SDL_fabsf(x1 - x0) > 0.5f || SDL_fabsf(y1 - y0) > 0.5f) {
                            fvf_jumps++;
                            continue;   /* jumped: not blended */
                        }
                    }
                    u->interp[0] = same_nv ? pu->layout0[0] : -1;     /* else the matrices alone */
                    u->interp[1] = 1;
                    c->unpaired = false;
                    taken[pk[i + order[k]].cmd] = 1;
                    note_motion(prev, pc, cur, c);
                    memcpy(u->prev_wvp, pu->wvp, sizeof u->prev_wvp);
                    memcpy(u->prev_wv, pu->wv, sizeof u->prev_wv);
                    memcpy(u->prev_world, pu->world, sizeof u->prev_world);
                }
            }
        }
        i = i2;
        j = j2;
    }
    }
    /* PDO_DBG_UNPAIRED=1: every 600th frame, the draws left unpaired. */
    if (getenv("PDO_DBG_UNPAIRED") && cur->frame % 600 == 0)
    for (uint32_t i = 0; i < cur->ncmds; i++) {
        const command *c = &cur->cmds[i];
        uint32_t same = 0, loose = 0;
        if (c->kind != CMD_DRAW || !c->unpaired)
            continue;
        for (uint32_t k = 0; k < prev->ncmds; k++)
            if (blendable(&prev->cmds[k])) {
                same += key_of(prev, &prev->cmds[k], 0) == key_of(cur, c, 0);
                loose += key_of(prev, &prev->cmds[k], 2) == key_of(cur, c, 2);
            }
        SDL_Log("unpaired %u: kind %d src %d nv %u idx %u tex %08x; prev alike %u, counts aside %u", i,
                c->vsh ? 1 : c->fvf >= 0 ? 2 : 3, c->id_src != 0, c->id_nv,
                c->vsh || c->fvf >= 0 ? c->vsh_index_count : c->index_count, (unsigned)c->draw.tex[0].data, same, loose);
    }
    /* A camera cut: most things jumped. Show the new frame as it is. */
    if (fvf_pairs >= 8 && fvf_jumps * 2 > fvf_pairs) {
        match_frames(NULL, cur);
        st_cuts++;
    }
    {   /* Every 300 frames: how much pairs. */
        uint32_t b = 0, p = 0;
        for (uint32_t k = 0; k < cur->ncmds; k++)
            if (blendable(&cur->cmds[k])) {
                b++;
                p += !cur->cmds[k].unpaired;
            }
        st_draws += cur->ncmds;
        st_blendable += b;
        st_paired += p;
        st_jumps += fvf_jumps;
        if (++st_frames == 300) {
            SDL_Log("interp: per frame %.0f commands, %.0f could pair, %.0f paired; %u jumps, %u camera cuts in 300",
                    st_draws / 300.0, st_blendable / 300.0, st_paired / 300.0, st_jumps, st_cuts);
            SDL_Log("interp: unpaired per frame %.1f without an alike draw, %.1f far, %.1f in doubt, %.1f jumping sprites",
                    (st_blendable - st_paired - st_far - st_sprite - st_jumps) / 300.0, (st_far - st_ambig) / 300.0,
                    st_ambig / 300.0, st_sprite / 300.0);
            st_frames = st_draws = st_blendable = st_paired = st_jumps = st_cuts = 0;
            st_far = st_sprite = st_ambig = 0;
        }
    }
}

/* ── The two threads ──────────────────────────────────────────────────── */

/* A frame data no thread holds (with q_lock held): not recorded into, not
 * handed over, not the one last drawn. */
static frame_data *free_frame(void)
{
    for (int i = 0; i < 5; i++)
        if (&pool[i] != F && &pool[i] != ready && &pool[i] != held_cur && &pool[i] != held_prev)
            return &pool[i];
    return NULL;
}

/* Game thread, at Swap: resolve the frame's textures while guest memory
 * still holds them, then hand it over and go on recording into another. */
static void hand_over(void)
{
    Uint64 tex_t0 = SDL_GetTicksNS();
    frame_data *next = NULL;

    F->frame = frames;
    F->stamp = tex_t0;
    F->tex = grow(F->tex, &F->cap_tex, F->ncmds * 4 + 4, sizeof *F->tex);
    if (!F->ups)
        F->ups = SDL_malloc(MAX_UPLOADS * sizeof *F->ups);
    for (uint32_t i = 0; i < F->ncmds; i++)
        if (F->cmds[i].kind == CMD_DRAW)
            for (int s = 0; s < 4; s++)
                F->tex[i * 4 + s] = texture_for(&F->cmds[i].draw.tex[s]);
    F->staging_len = 0;
    F->nups = 0;
    check_textures(&F->staging, &F->staging_len, &F->staging_cap, F->ups, &F->nups);
    texture_ns += SDL_GetTicksNS() - tex_t0;
    if (progs_dirty) {
        F->progs = grow(F->progs, &F->cap_progs, nprogs, sizeof *F->progs);
        memcpy(F->progs, progs, nprogs * sizeof *progs);
        F->nprogs = nprogs;
        F->progs_new = true;
        progs_dirty = false;
    }
    if (game_gamma_dirty) {
        memcpy(F->gamma, game_gamma, sizeof F->gamma);
        F->gamma_new = true;
        game_gamma_dirty = false;
    }

    SDL_LockMutex(q_lock);
    /* The options menu holds the game here. Otherwise no waiting: a frame
     * the main thread has not taken yet (it is behind, the display or the
     * GPU slower than the game) is dropped for this newer one. */
    while (paused)
        SDL_WaitCondition(q_cond, q_lock);
    while (ready && ready->frame == capture_frame)
        SDL_WaitConditionTimeout(q_cond, q_lock, 2);   /* a capture is never dropped */
    next = ready ? ready : free_frame();
    if (ready) {
        /* What the dropped frame alone carried goes with this one: its
         * texture uploads (this frame skipped a texture the dropped one
         * uploaded, and would draw it never filled), programs and gamma. */
        frames_dropped++;
        for (int i = 0; i < F->nups; i++)
            F->ups[i].t->staged_in = F->frame;
        for (int i = 0; i < ready->nups; i++) {
            pending_upload *u = &ready->ups[i];
            uint32_t need = u->w * u->h * 4;
            if (u->t->staged_in == F->frame)
                continue;           /* this frame has a newer copy */
            if (F->nups == MAX_UPLOADS) {
                u->t->uploaded = false;     /* next time it is used */
                continue;
            }
            if (F->staging_len + need > F->staging_cap) {
                F->staging_cap = (F->staging_len + need) * 2;
                F->staging = SDL_realloc(F->staging, F->staging_cap);
            }
            memcpy(F->staging + F->staging_len, ready->staging + u->offset, need);
            F->ups[F->nups] = *u;
            F->ups[F->nups].offset = F->staging_len;
            F->nups++;
            F->staging_len += need;
        }
        if (ready->progs_new && !F->progs_new) {
            F->progs = grow(F->progs, &F->cap_progs, nprogs, sizeof *F->progs);
            memcpy(F->progs, progs, nprogs * sizeof *progs);
            F->nprogs = nprogs;
            F->progs_new = true;
        }
        if (ready->gamma_new && !F->gamma_new) {
            memcpy(F->gamma, ready->gamma, sizeof F->gamma);
            F->gamma_new = true;
        }
    }
    ready = F;
    F = next;
    SDL_BroadcastCondition(q_cond);
    SDL_UnlockMutex(q_lock);
    reset_frame(F);
}

bool pdo_gfx_present(void)
{
    RECORDING();
    frames++;
    if (!dev)
        return true;
    {
        /* PDO_NO_DRAW=1: drop the frame's commands unless it is captured or
         * screenshotted, so a debugging run reaches a stage at the game's
         * own pace on a slow (software) GPU. */
        static int no_draw = -1;
        bool shot = shot_dir && frames <= 300 * (uint64_t)shot_every && frames % (uint64_t)shot_every == 0;
        if (no_draw < 0)
            no_draw = getenv("PDO_NO_DRAW") && atoi(getenv("PDO_NO_DRAW")) != 0;
        if (no_draw && !shot && frames != capture_frame && frames + 1 != capture_frame) {
            reset_frame(F);
            return true;
        }
    }
    hand_over();
    return true;
}

static bool shot_frame(const frame_data *f)
{
    return shot_dir && f->frame <= 300 * (uint64_t)shot_every && f->frame % (uint64_t)shot_every == 0;
}

/* Main thread: draw a handed-over frame and present it; with prev, the
 * frame between prev and f at t (0 prev, 1 f). Captures and screenshots
 * are of frames as they are. */
static void draw_frame(frame_data *f, frame_data *prev, float t)
{
    SDL_GPUCommandBuffer *cmd;
    bool shot = !prev && shot_frame(f);

    F = f;
    P = prev;
    blend = t;
    if (f->gamma_new) {
        memcpy(gamma_rgba, f->gamma, sizeof gamma_rgba);
        gamma_dirty = true;
    }
    if (!prev && f->frame == capture_frame) {
        capture_steps();
        capture_shadows();
        capture_vsh();
    }
    apply_quality();
    cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd)
        return;
    replay(cmd);
    present_pass(cmd);
    present_to_window(cmd);
    if (shot)
        save_screenshot(&cmd, shot_dir, "frame");
    else if (!prev && f->frame == capture_frame) {
        save_screenshot(&cmd, ".", "capture");
        SDL_Log("capture: saved capture_%05llu.bmp", (unsigned long long)f->frame);
    }
    if (cmd)
        SDL_SubmitGPUCommandBuffer(cmd);
    pdo_overlay_frame();
}

/* The options menu is open: the game waits at its next Swap and the sound
 * holds, while the last frame is shown with the menu over it. */
static void hold_for_overlay(void)
{
    SDL_LockMutex(q_lock);
    paused = true;
    SDL_UnlockMutex(q_lock);
    pdo_mixer_set_paused(true);
    while (pdo_overlay_open()) {
        SDL_GPUCommandBuffer *cmd;

        if (!pdo_gfx_pump_events()) {
            SDL_Log("d3d: window closed");
            exit(0);
        }
        cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (cmd) {
            present_to_window(cmd);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        if (!have_swapchain)
            SDL_Delay(16);
    }
    pdo_mixer_set_paused(false);
    SDL_LockMutex(q_lock);
    paused = false;
    SDL_BroadcastCondition(q_cond);
    SDL_UnlockMutex(q_lock);
}

void pdo_gfx_prepare_threads(void)
{
    main_thread = SDL_GetCurrentThreadID();
    q_lock = SDL_CreateMutex();
    q_cond = SDL_CreateCondition();
}

void pdo_gfx_game_finished(void)
{
    SDL_LockMutex(q_lock);
    game_done = true;
    SDL_BroadcastCondition(q_cond);
    SDL_UnlockMutex(q_lock);
}

void pdo_gfx_main_loop(void)
{
    frame_data *cur = NULL, *prev = NULL;
    double interval = 1e9 / 59.94;     /* between game frames, smoothed */

    interp_tint = getenv("PDO_INTERP_TINT") && atoi(getenv("PDO_INTERP_TINT"));

    for (;;) {
        frame_data *f;
        bool interp = pdo_settings_get()->frame_interp && !getenv("PDO_BENCHMARK");

        if (dev && !pdo_gfx_pump_events()) {
            SDL_Log("d3d: window closed");
            exit(0);
        }
        if (dev && pdo_overlay_open())     /* opened while no frame comes (loading) */
            hold_for_overlay();
        SDL_LockMutex(q_lock);
        /* Interpolating, frames are drawn whether a new one came or not. */
        if (!ready && !game_done && init_state != 1 && !(interp && prev))
            SDL_WaitConditionTimeout(q_cond, q_lock, 4);
        if (init_state == 1) {
            SDL_UnlockMutex(q_lock);
            bool ok = init_here(init_w, init_h);
            SDL_LockMutex(q_lock);
            init_state = ok ? 2 : 3;
            SDL_BroadcastCondition(q_cond);
        }
        if (game_done && !ready) {
            SDL_UnlockMutex(q_lock);
            return;
        }
        f = ready;
        ready = NULL;
        if (f) {
            held_prev = cur;
            held_cur = f;
        }
        SDL_UnlockMutex(q_lock);

        if (f) {
            if (cur) {
                double d = (double)(f->stamp - cur->stamp);
                if (d > 8e6 && d < 50e6)
                    interval += (d - interval) * 0.1;
            }
            prev = cur;
            cur = f;
            match_frames(interp ? prev : NULL, cur);
            if (prev && f->frame == capture_frame)
                capture_interp(prev, cur);
            /* Captures and screenshots are of the frame as it is. */
            if (!interp || !prev || shot_frame(f) || f->frame == capture_frame) {
                draw_frame(f, NULL, 1.0f);
                SDL_LockMutex(q_lock);
                frames_drawn++;
                SDL_UnlockMutex(q_lock);
                if (!interp)
                    continue;
            }
        }
        if (!interp || !prev || !cur)
            continue;
        {
            /* One game frame behind: as the newest arrives, show the one
             * before; as the next is due, the newest. */
            double t = (double)(SDL_GetTicksNS() - cur->stamp) / interval;
            draw_frame(cur, prev, (float)(t < 0 ? 0 : t > 1 ? 1 : t));
            SDL_LockMutex(q_lock);
            frames_drawn++;
            SDL_UnlockMutex(q_lock);
            if (!have_swapchain)
                SDL_Delay(1);
        }
    }
}
