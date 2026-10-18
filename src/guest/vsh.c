/*
 * NV2A vertex program interpreter. See vsh.h.
 *
 * Instruction layout (128 bits; word 0 carries nothing, as emulators
 * document it): word 1 holds the ILU and MAC opcodes, the constant and input
 * indices and operand A's sign and swizzle; word 2 operand A's register and
 * mux, operand B, and operand C up to the top bits of its register; word 3
 * the rest of C, the write masks, the destination and the final flag.
 *
 * Semantics, including the JSRF-specific points:
 * - Both units read the registers as they were before the instruction.
 * - When the MAC and the ILU both run, the ILU's temp destination is R1.
 * - R12 is the same storage as oPos.
 * - A relative constant index is clamped to 0..191, not wrapped.
 * - x * 0 is 0 even for infinities, as on the hardware (an unwritten or
 *   zero normal must not turn a position into NaN). This file is compiled
 *   without fused multiply-add for the same reason: results are rounded
 *   per operation, as the hardware does.
 * - Unwritten outputs: oD0 = 1, oD1 = 0, oT = 0, oPts = 1, oFog = 1.
 */

#include "guest/vsh.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <string.h>

static uint32_t bits(const uint32_t *w, int word, int pos, int size)
{
    return (w[word] >> pos) & ((1u << size) - 1u);
}

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_NONE, MUX_R, MUX_V, MUX_C };

/* Fields, from the instruction layout above. */
static void decode(const uint32_t *w, pdo_vsh_op *o)
{
    static const uint8_t mac_use[16] = {
        0, 1, 3, 5, 7, 3, 3, 3, 3, 3, 3, 3, 3, 1, 0, 0,   /* bits: A B C */
    };
    o->ilu = (uint8_t)bits(w, 1, 25, 3);
    o->mac = (uint8_t)bits(w, 1, 21, 4);
    o->cidx = (uint8_t)bits(w, 1, 13, 8);
    o->vidx = (uint8_t)bits(w, 1, 9, 4);
    o->src[0].neg = (uint8_t)bits(w, 1, 8, 1);
    o->src[0].swz[0] = (uint8_t)bits(w, 1, 6, 2); o->src[0].swz[1] = (uint8_t)bits(w, 1, 4, 2);
    o->src[0].swz[2] = (uint8_t)bits(w, 1, 2, 2); o->src[0].swz[3] = (uint8_t)bits(w, 1, 0, 2);
    o->src[0].reg = (uint8_t)bits(w, 2, 28, 4); o->src[0].mux = (uint8_t)bits(w, 2, 26, 2);
    o->src[1].neg = (uint8_t)bits(w, 2, 25, 1);
    o->src[1].swz[0] = (uint8_t)bits(w, 2, 23, 2); o->src[1].swz[1] = (uint8_t)bits(w, 2, 21, 2);
    o->src[1].swz[2] = (uint8_t)bits(w, 2, 19, 2); o->src[1].swz[3] = (uint8_t)bits(w, 2, 17, 2);
    o->src[1].reg = (uint8_t)bits(w, 2, 13, 4); o->src[1].mux = (uint8_t)bits(w, 2, 11, 2);
    o->src[2].neg = (uint8_t)bits(w, 2, 10, 1);
    o->src[2].swz[0] = (uint8_t)bits(w, 2, 8, 2); o->src[2].swz[1] = (uint8_t)bits(w, 2, 6, 2);
    o->src[2].swz[2] = (uint8_t)bits(w, 2, 4, 2); o->src[2].swz[3] = (uint8_t)bits(w, 2, 2, 2);
    o->src[2].reg = (uint8_t)((bits(w, 2, 0, 2) << 2) | bits(w, 3, 30, 2));
    o->src[2].mux = (uint8_t)bits(w, 3, 28, 2);
    o->mac_mask = (uint8_t)bits(w, 3, 24, 4);
    o->out_r = (uint8_t)bits(w, 3, 20, 4);
    o->ilu_mask = (uint8_t)bits(w, 3, 16, 4);
    o->o_mask = (uint8_t)bits(w, 3, 12, 4);
    o->orb = (uint8_t)bits(w, 3, 11, 1);
    o->addr = (uint8_t)bits(w, 3, 3, 8);
    o->out_mux = (uint8_t)bits(w, 3, 2, 1);
    o->rel = (uint8_t)bits(w, 3, 1, 1);
    /* Paired with the MAC, the ILU's temp destination is R1. */
    o->ilu_r = o->mac != MAC_NOP ? 1 : o->out_r;
    o->use = (uint8_t)(mac_use[o->mac] | (o->ilu != ILU_NOP ? 4 : 0));
}

/* ── Declaration and function parsing ─────────────────────────────────── */

uint32_t pdo_vsh_type_size(uint8_t type)
{
    switch (type) {
    case 0x12: case 0x22: case 0x32: case 0x42: return 4u * (type >> 4);  /* FLOATn */
    case 0x40: return 4;                                                  /* D3DCOLOR */
    case 0x15: case 0x25: case 0x35: case 0x45:                           /* SHORTn */
    case 0x11: case 0x21: case 0x31: case 0x41: return 2u * (type >> 4);  /* NORMSHORTn */
    case 0x16: return 4;                                                  /* NORMPACKED3 */
    case 0x14: case 0x24: case 0x34: case 0x44: return type >> 4;         /* PBYTEn */
    default: return 0;
    }
}

bool pdo_vsh_parse(pdo_vsh_program *p, const uint32_t *function, uint32_t function_words,
                   const uint32_t *decl, uint32_t decl_words)
{
    uint32_t head, slots, stream = 0, offset[16] = {0};

    memset(p, 0, sizeof *p);
    if (!function || function_words < 1)
        return false;
    head = function[0];
    slots = head >> 16;
    /* XDK program header: low half 0x2078 (a vertex program) or 0x7978 (a
     * state program); only the former is drawn with. */
    if ((head & 0xFFFF) != 0x2078 || slots == 0 || slots > PDO_VSH_MAX_INSNS
            || 1 + 4 * slots > function_words)
        return false;
    p->count = slots;
    memcpy(p->insn, function + 1, slots * 16);
    for (uint32_t i = 0; i < slots; i++) {
        decode(p->insn[i], &p->op[i]);
        for (int k = 0; k < 3; k++)
            if ((p->op[i].use & (1u << k)) && p->op[i].src[k].mux == 2)
                p->inputs_read |= (uint16_t)(1u << p->op[i].vidx);
        if (p->op[i].o_mask && !p->op[i].orb)
            p->writes_constants = true;
        if (bits(p->insn[i], 3, 0, 1)) {          /* final */
            p->count = i + 1;
            break;
        }
    }

    for (uint32_t i = 0; decl && i < decl_words; i++) {
        uint32_t t = decl[i], kind = t >> 29;
        if (t == 0xFFFFFFFFu)                    /* D3DVSD_END */
            break;
        if (kind == 1) {                         /* D3DVSD_STREAM(n) */
            stream = t & 0xF;
        } else if (kind == 2) {                  /* STREAMDATA */
            if (t & 0x10000000u) {               /* SKIP: dwords, or bytes */
                uint32_t n = (t >> 16) & 0xFF;
                offset[stream] += (t & 0x08000000u) ? n : 4u * n;
            } else {                             /* REG(reg, type) */
                uint32_t reg = t & 0x1F, type = (t >> 16) & 0xFF;
                if (reg < PDO_VSH_INPUTS) {
                    p->input[reg].stream = (uint8_t)stream;
                    p->input[reg].type = (uint8_t)type;
                    p->input[reg].offset = (uint16_t)offset[stream];
                    p->input[reg].present = pdo_vsh_type_size((uint8_t)type) != 0;
                }
                offset[stream] += pdo_vsh_type_size((uint8_t)type);
            }
        } else if (kind == 4) {                  /* CONSTMEM: count * 4 words */
            i += 4 * ((t >> 25) & 0xF);
        } else if (kind == 5) {                  /* EXT: count words */
            i += (t >> 24) & 0x1F;
        }
    }
    return true;
}

static float s16(const uint8_t *s) { int16_t v; memcpy(&v, s, 2); return (float)v; }
static float f32p(const uint8_t *s) { float v; memcpy(&v, s, 4); return v; }

void pdo_vsh_fetch(uint8_t type, const uint8_t *src, float out[4])
{
    uint32_t n = type >> 4;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type & 0x0F) {
    case 0x2:                                    /* FLOATn */
        for (uint32_t i = 0; i < n && i < 4; i++) out[i] = f32p(src + 4 * i);
        break;
    case 0x0:                                    /* D3DCOLOR: B, G, R, A */
        if (type == 0x40) {
            out[0] = src[2] / 255.0f; out[1] = src[1] / 255.0f;
            out[2] = src[0] / 255.0f; out[3] = src[3] / 255.0f;
        }
        break;
    case 0x5:                                    /* SHORTn, not normalised */
        for (uint32_t i = 0; i < n && i < 4; i++) out[i] = s16(src + 2 * i);
        break;
    case 0x1:                                    /* NORMSHORTn */
        for (uint32_t i = 0; i < n && i < 4; i++) {
            float v = s16(src + 2 * i) / 32767.0f;
            out[i] = v < -1.0f ? -1.0f : v;
        }
        break;
    case 0x6: {                                  /* NORMPACKED3: 11:11:10 signed */
        uint32_t v; memcpy(&v, src, 4);
        int32_t x = (int32_t)(v << 21) >> 21, y = (int32_t)(v << 10) >> 21, z = (int32_t)v >> 22;
        out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
        break;
    }
    case 0x4:                                    /* PBYTEn */
        for (uint32_t i = 0; i < n && i < 4; i++) out[i] = src[i] / 255.0f;
        break;
    default:
        break;
    }
}

/* ── Execution ────────────────────────────────────────────────────────── */

static float mulz(float a, float b)
{
    return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
}

typedef struct vm {
    float r[13][4];          /* r12 is oPos: read and written through out[0] */
    float (*out)[4];
    float (*c)[4];
    const float (*v)[4];
    int   a0;
} vm;

static const float *reg_ptr(vm *m, uint32_t mux, uint32_t r, uint32_t vidx, int cidx)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    switch (mux) {
    case MUX_R: return r == 12 ? m->out[VSH_OPOS] : (r < 12 ? m->r[r] : zero);
    case MUX_V: return m->v[vidx & 15];
    case MUX_C: return m->c[cidx];
    default:    return zero;
    }
}

/* Operand: swizzle and sign applied. */
static void operand(vm *m, const pdo_vsh_src *sr, uint32_t vidx, int cidx, float o[4])
{
    const float *src = reg_ptr(m, sr->mux, sr->reg, vidx, cidx);
    if (sr->neg) {
        o[0] = -src[sr->swz[0]]; o[1] = -src[sr->swz[1]];
        o[2] = -src[sr->swz[2]]; o[3] = -src[sr->swz[3]];
    } else {
        o[0] = src[sr->swz[0]]; o[1] = src[sr->swz[1]];
        o[2] = src[sr->swz[2]]; o[3] = src[sr->swz[3]];
    }
}

static void write_masked(float *dst, const float v[4], uint32_t mask)
{
    /* Mask bits: 8 = x, 4 = y, 2 = z, 1 = w. */
    for (int i = 0; i < 4; i++)
        if (mask & (8u >> i))
            dst[i] = v[i];
}

static float rcp(float x)
{
    return x == 1.0f ? 1.0f : 1.0f / x;
}

static float rcc(float x)
{
    float r = 1.0f / x, a = fabsf(r);
    if (a < 5.42101086e-20f) a = 5.42101086e-20f;
    else if (a > 1.884467e+19f) a = 1.884467e+19f;
    return signbit(r) ? -a : a;
}

/* Set by a caller re-running one vertex to log every instruction
 * (PDO_VSH_BAD in d3d_hle.c); single-threaded use only. */
_Thread_local bool pdo_vsh_trace;

void pdo_vsh_run(const pdo_vsh_program *p, const float in[PDO_VSH_INPUTS][4],
                 float c[PDO_VSH_CONSTANTS][4], float out[VSH_OUTPUTS][4])
{
    vm m;

    memset(&m, 0, sizeof m);
    memset(out, 0, VSH_OUTPUTS * sizeof out[0]);
    out[VSH_OPOS][3] = 1.0f;
    for (int i = 0; i < 4; i++) {
        out[VSH_OD0][i] = 1.0f;
        out[VSH_OB0][i] = 1.0f;
    }
    out[VSH_OFOG][0] = out[VSH_OFOG][1] = out[VSH_OFOG][2] = out[VSH_OFOG][3] = 1.0f;
    out[VSH_OPTS][0] = 1.0f;
    m.out = out;
    m.c = c;
    m.v = in;

    for (uint32_t pc = 0; pc < p->count; pc++) {
        const pdo_vsh_op *op = &p->op[pc];
        uint32_t ilu = op->ilu, mac = op->mac;
        int cidx = op->cidx;
        float a[4], b[4], cc[4], mr[4] = {0}, ir[4] = {0};

        if (op->rel) {                           /* relative constant: c[a0.x + n] */
            cidx += m.a0;
            cidx = cidx < 0 ? 0 : cidx > 191 ? 191 : cidx;
        }
        /* Operands read the state before this instruction writes anything. */
        if (op->use & 1) operand(&m, &op->src[0], op->vidx, cidx, a);
        if (op->use & 2) operand(&m, &op->src[1], op->vidx, cidx, b);
        if (op->use & 4) operand(&m, &op->src[2], op->vidx, cidx, cc);

        switch (mac) {
        case MAC_MOV: memcpy(mr, a, sizeof mr); break;
        case MAC_MUL: for (int i = 0; i < 4; i++) mr[i] = mulz(a[i], b[i]); break;
        case MAC_ADD: for (int i = 0; i < 4; i++) mr[i] = a[i] + cc[i]; break;
        case MAC_MAD: for (int i = 0; i < 4; i++) mr[i] = mulz(a[i], b[i]) + cc[i]; break;
        case MAC_DP3: {
            float d = mulz(a[0], b[0]) + mulz(a[1], b[1]) + mulz(a[2], b[2]);
            mr[0] = mr[1] = mr[2] = mr[3] = d;
            break;
        }
        case MAC_DPH: {
            float d = mulz(a[0], b[0]) + mulz(a[1], b[1]) + mulz(a[2], b[2]) + b[3];
            mr[0] = mr[1] = mr[2] = mr[3] = d;
            break;
        }
        case MAC_DP4: {
            float d = mulz(a[0], b[0]) + mulz(a[1], b[1]) + mulz(a[2], b[2]) + mulz(a[3], b[3]);
            mr[0] = mr[1] = mr[2] = mr[3] = d;
            break;
        }
        case MAC_DST:
            mr[0] = 1.0f; mr[1] = mulz(a[1], b[1]); mr[2] = a[2]; mr[3] = b[3];
            break;
        case MAC_MIN: for (int i = 0; i < 4; i++) mr[i] = a[i] < b[i] ? a[i] : b[i]; break;
        case MAC_MAX: for (int i = 0; i < 4; i++) mr[i] = a[i] > b[i] ? a[i] : b[i]; break;
        case MAC_SLT: for (int i = 0; i < 4; i++) mr[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
        case MAC_SGE: for (int i = 0; i < 4; i++) mr[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
        default: break;
        }

        switch (ilu) {
        case ILU_MOV: memcpy(ir, cc, sizeof ir); break;
        case ILU_RCP: ir[0] = ir[1] = ir[2] = ir[3] = rcp(cc[0]); break;
        case ILU_RCC: ir[0] = ir[1] = ir[2] = ir[3] = rcc(cc[0]); break;
        case ILU_RSQ: ir[0] = ir[1] = ir[2] = ir[3] = 1.0f / sqrtf(fabsf(cc[0])); break;
        case ILU_EXP: {
            float f = floorf(cc[0]);
            ir[0] = exp2f(f); ir[1] = cc[0] - f; ir[2] = exp2f(cc[0]); ir[3] = 1.0f;
            break;
        }
        case ILU_LOG: {
            float x = fabsf(cc[0]);
            if (x == 0.0f) {
                ir[0] = ir[1] = ir[2] = -INFINITY;
            } else {
                int e;
                float mant = frexpf(x, &e);       /* x = mant * 2^e, mant in [0.5, 1) */
                ir[0] = (float)(e - 1); ir[1] = mant * 2.0f; ir[2] = log2f(x);
            }
            ir[3] = 1.0f;
            break;
        }
        case ILU_LIT: {
            float d = cc[0] > 0 ? cc[0] : 0, s = cc[1] > 0 ? cc[1] : 0;
            float e = cc[3] < -127.9961f ? -127.9961f : cc[3] > 127.9961f ? 127.9961f : cc[3];
            ir[0] = 1.0f; ir[1] = d; ir[2] = cc[0] > 0 ? powf(s, e) : 0.0f; ir[3] = 1.0f;
            break;
        }
        default: break;
        }

        if (pdo_vsh_trace)
            SDL_Log("vsh %3u: mac %u ilu %u c%d%s a0 %d | A %g %g %g %g | B %g %g %g %g | C %g %g %g %g"
                    " | mac %g %g %g %g -> r%u/%x | ilu %g %g %g %g -> r%u/%x | o %s%u/%x",
                    pc, mac, ilu, cidx, op->rel ? "(rel)" : "", m.a0, a[0], a[1], a[2], a[3], b[0], b[1], b[2],
                    b[3], cc[0], cc[1], cc[2], cc[3], mr[0], mr[1], mr[2], mr[3], op->out_r, op->mac_mask,
                    ir[0], ir[1], ir[2], ir[3], op->ilu_r, op->ilu_mask, op->orb ? "o" : "c", op->addr,
                    op->o_mask);
        /* Writes, after both units have computed. */
        if (mac == MAC_ARL) {
            m.a0 = (int)floorf(a[0]);
        } else if (mac != MAC_NOP && op->mac_mask) {
            float *dst = op->out_r == 12 ? out[VSH_OPOS] : (op->out_r < 12 ? m.r[op->out_r] : NULL);
            if (dst) write_masked(dst, mr, op->mac_mask);
        }
        if (ilu != ILU_NOP && op->ilu_mask) {
            float *dst = op->ilu_r == 12 ? out[VSH_OPOS] : (op->ilu_r < 12 ? m.r[op->ilu_r] : NULL);
            if (dst) write_masked(dst, ir, op->ilu_mask);
        }
        if (op->o_mask) {
            const float *v = op->out_mux ? ir : mr;
            if (op->orb) {
                if (op->addr < VSH_OUTPUTS) write_masked(out[op->addr], v, op->o_mask);
            } else if (op->addr < PDO_VSH_CONSTANTS) {
                write_masked(c[op->addr], v, op->o_mask);
            }
        }
    }
}

/* ── Worker pool ──────────────────────────────────────────────────────── */

#define MAX_WORKERS 15

static struct {
    SDL_Semaphore *go, *done;
    SDL_AtomicInt  next;              /* next chunk to take */
    uint32_t       count, chunk;
    void         (*fn)(void *, uint32_t, uint32_t);
    void          *ctx;
    int            workers;
} pool;

static void run_chunks(void)
{
    for (;;) {
        uint32_t c = (uint32_t)SDL_AddAtomicInt(&pool.next, 1);
        uint32_t first = c * pool.chunk;
        if (first >= pool.count)
            return;
        pool.fn(pool.ctx, first, first + pool.chunk < pool.count ? first + pool.chunk : pool.count);
    }
}

static int SDLCALL worker(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_WaitSemaphore(pool.go);
        run_chunks();
        SDL_SignalSemaphore(pool.done);
    }
    return 0;
}

void pdo_vsh_parallel(uint32_t count, void (*fn)(void *, uint32_t, uint32_t), void *ctx)
{
    static bool started;

    /* Small draws are not worth waking anyone for. */
    if (count < 256) {
        fn(ctx, 0, count);
        return;
    }
    if (!started) {
        int cores = SDL_GetNumLogicalCPUCores();
        started = true;
        pool.go = SDL_CreateSemaphore(0);
        pool.done = SDL_CreateSemaphore(0);
        /* Leave a core for the game's own threads. */
        pool.workers = cores - 2;
        if (pool.workers > MAX_WORKERS) pool.workers = MAX_WORKERS;
        for (int i = 0; i < pool.workers; i++)
            if (!SDL_CreateThread(worker, "pdo_vsh", NULL)) {
                pool.workers = i;
                break;
            }
        SDL_Log("vsh: %d worker threads", pool.workers);
    }
    if (pool.workers <= 0) {
        fn(ctx, 0, count);
        return;
    }
    pool.fn = fn;
    pool.ctx = ctx;
    pool.count = count;
    pool.chunk = 64;
    SDL_SetAtomicInt(&pool.next, 0);
    for (int i = 0; i < pool.workers; i++)
        SDL_SignalSemaphore(pool.go);
    run_chunks();
    for (int i = 0; i < pool.workers; i++)
        SDL_WaitSemaphore(pool.done);
}
