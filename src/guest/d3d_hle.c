/*
 * Direct3D, replaced at its API (in progress).
 *
 * Like DirectSound, the title links the XDK's Direct3D library, which drives
 * the NV2A GPU directly. Its entry points are replaced here one by one.
 *
 * From Concept of Love (JSRF, XDK 4134). Addresses are Orta's (XDK 4928),
 * mapped by XbSymbolDatabase name, each checked in Orta's own code: same
 * shape and the same `ret N` as JSRF's. Where 4928 differs it says so.
 *
 * Same conventions as dsound_hle.c: __stdcall, arguments at g_esp + 4 + 4n,
 * each function pops exactly what the original's `ret N` did.
 */

#include "recomp_types.h"

#include "gfx/renderer.h"
#include "gfx/xtex.h"
#include "guest/vsh.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3D_DEVICE_PTR      0x002BD708u     /* D3D__pDevice */
/* Device fields moved by 0x124 from 4134: JSRF's callback is at +0x242C. */
#define DEV_VBLANK_CALLBACK 0x2550u         /* set by SetVerticalBlankCallback (0x002ADC90) */
#define DEV_BACKBUFFERS     0x21C0u         /* surfaces GetBackBuffer2 returns, by index */

/* NTSC: 60000/1001 Hz. */
#define VBLANK_NS 16683333ull

extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static Uint64 epoch;              /* time of vblank 0 */
static SDL_AtomicInt last_called;  /* last vblank whose callback has run */
static uint32_t vblank_data;       /* guest D3DVBLANKDATA */
static int swap_interval = 1;      /* vblanks per Swap; 0 = immediate */

static void note_backbuffers(void);

/* Draw calls since the last fps line: made, drawn, and dropped (no vertex
 * data, an unhandled FVF or a program never created). */
static SDL_AtomicInt draws_called, draws_drawn, draws_dropped;

/* Call a guest __cdecl function taking one pointer argument. */
static void call_guest_cdecl1(uint32_t fn_va, uint32_t arg)
{
    recomp_func_t fn = recomp_lookup_manual(fn_va);

    if (!fn)
        fn = recomp_lookup(fn_va);
    if (!fn) {
        SDL_Log("d3d: vblank callback 0x%08x has no recompiled function", fn_va);
        return;
    }
    g_esp -= 4;
    MEM32(g_esp) = arg;
    g_esp -= 4;
    MEM32(g_esp) = 0;   /* return address; the callee pops it */
    fn();
    g_esp += 4;         /* __cdecl: the caller pops the argument */
}

/* D3DDevice_BlockUntilVerticalBlank(): no arguments.
 *
 * On the console this waits for the GPU's vblank interrupt, whose handler
 * also runs the title's vertical-blank callback. JSRF paced everything from
 * it (a thread waited here and woke the main thread); Orta's game loop does
 * the same through Swap's presentation interval. So this is the frame clock,
 * and the place a different refresh rate will be decided later. */
/* Sleep until the next vblank on the shared timeline; returns its index.
 * Vblanks fall on one fixed timeline and every waiter wakes at the next one,
 * as on hardware. */
/* PDO_FAST=n: vblanks come n times as often, so a debugging run reaches a
 * point of the game sooner (the game is paced on vblanks; sound and other
 * clocks are not, so timing-sensitive behaviour changes). */
static Uint64 vblank_ns(void)
{
    static Uint64 ns;
    if (!ns) {
        const char *e = getenv("PDO_FAST");
        int n = e ? atoi(e) : 1;
        ns = VBLANK_NS / (Uint64)(n > 1 ? n : 1);
    }
    return ns;
}

static Uint64 wait_vblank(void)
{
    Uint64 now = SDL_GetTicksNS(), index, at;

    if (!epoch)
        epoch = now;
    index = (now - epoch) / vblank_ns() + 1;
    at = epoch + index * vblank_ns();
    if (at > now)
        SDL_DelayPrecise(at - now);
    return index;
}

static Uint64 current_vblank(void)
{
    return epoch ? (SDL_GetTicksNS() - epoch) / vblank_ns() : 0;
}

void sub_002ADCB0(void)
{
    Uint64 index = wait_vblank();
    uint32_t dev, cb;
    int prev;

    /* The interrupt runs the title's callback once per vblank, whoever is
     * waiting: the first waiter to reach a new vblank runs it. */
    prev = SDL_GetAtomicInt(&last_called);
    if ((Uint64)prev < index && SDL_CompareAndSwapAtomicInt(&last_called, prev, (int)index)) {
        dev = MEM32(D3D_DEVICE_PTR);
        cb = dev ? MEM32(dev + DEV_VBLANK_CALLBACK) : 0;
        if (cb) {
            if (!vblank_data)
                vblank_data = xbox_HeapAlloc(12, 4);
            if (vblank_data) {
                MEM32(vblank_data + 0) = (uint32_t)index;  /* VBlank */
                MEM32(vblank_data + 4) = 0;                /* Swap */
                MEM32(vblank_data + 8) = 0;                /* Flags */
                call_guest_cdecl1(cb, vblank_data);
            }
        }
        if (index <= 3 || index % 600 == 0)
            SDL_Log("d3d: vblank %llu", (unsigned long long)index);
    }
    /* Loading screens and movies wait here for seconds without a Swap: keep
     * the window answering (Windows marks it "Not Responding" otherwise). */
    if (!pdo_gfx_pump_events()) {
        SDL_Log("d3d: window closed");
        exit(0);
    }
    g_esp += 4;
}

/* Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
 *                       pPresentationParameters, ppDevice)
 *
 * The XDK's own CreateDevice still runs: it builds the device structure the
 * rest of the library and the game's inline code read. We open our window
 * at the back buffer size the game asked for. */
extern void sub_002B3510_gen(void);
void sub_002B3510(void)
{
    uint32_t pp = MEM32(g_esp + 20);
    int w = pp ? (int)MEM32(pp + 0) : 640, h = pp ? (int)MEM32(pp + 4) : 480;

    if (w <= 0 || h <= 0)
        w = 640, h = 480;
    /* FullScreen_PresentationInterval: how many vblanks each Swap waits. */
    if (pp) {
        uint32_t interval = MEM32(pp + 48);
        swap_interval = interval == 0x80000000u ? 0          /* IMMEDIATE */
                      : interval == 0 ? 1                    /* DEFAULT = ONE */
                      : (int)(interval & 0xF);
    }
    SDL_Log("d3d: CreateDevice, back buffer %dx%d, presentation interval %d", w, h, swap_interval);
    pdo_gfx_init(w, h);
    sub_002B3510_gen();
    note_backbuffers();
}

/* D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil) */
void sub_002B53F0(void)
{
    uint32_t count = MEM32(g_esp + 4), flags = MEM32(g_esp + 12);
    uint32_t color = MEM32(g_esp + 16), zbits = MEM32(g_esp + 20), stencil = MEM32(g_esp + 24);
    float z;

    memcpy(&z, &zbits, sizeof z);
    if (count) {
        static int warned;
        if (!warned++)
            SDL_Log("d3d: Clear with %u rectangles: clearing the whole target for now", count);
    }
    pdo_gfx_clear(flags, color, z, stencil);
    g_eax = 0;
    g_esp += 4 + 24;
}

/* PDO_PEEK=VA:N[,VA:N...] (hex VA, dword count) logs guest memory with the
 * frame-rate line, for bring-up. */
static void peek_guest(void)
{
    const char *e = getenv("PDO_PEEK");
    while (e && *e) {
        char *end;
        uint32_t va = (uint32_t)strtoul(e, &end, 16), n = 1;
        if (*end == ':')
            n = (uint32_t)strtoul(end + 1, &end, 0);
        for (uint32_t i = 0; i < n; i += 8) {
            char line[128];
            int len = SDL_snprintf(line, sizeof line, "peek %08X:", va + i * 4);
            for (uint32_t j = i; j < n && j < i + 8; j++)
                len += SDL_snprintf(line + len, sizeof line - len, " %08X", MEM32(va + j * 4));
            SDL_Log("%s", line);
        }
        e = *end == ',' ? end + 1 : NULL;
    }
}

/* D3DDevice_Swap(Flags): the end of a frame. Like the console, it shows the
 * frame at a vblank, waiting for as many as the presentation interval asks
 * since the previous one. */
void sub_002BAFC0(void)
{
    static Uint64 last_vblank;
    uint64_t n;

    if (swap_interval > 0) {
        while (current_vblank() < last_vblank + (Uint64)swap_interval)
            wait_vblank();
        last_vblank = current_vblank();
    }

    if (!pdo_gfx_present()) {
        SDL_Log("d3d: window closed");
        exit(0);
    }
    n = pdo_gfx_frame_count();
    {
        /* PDO_STALL=FRAME:MS stalls the game thread once, for testing how the
         * title copes with the host pausing it (window moves, disk). */
        const char *st = getenv("PDO_STALL");
        if (st && (Uint64)strtoull(st, NULL, 10) == n && strchr(st, ':')) {
            SDL_Log("d3d: stalling %s ms at frame %llu", strchr(st, ':') + 1, (unsigned long long)n);
            SDL_Delay((Uint32)atoi(strchr(st, ':') + 1));
        }
    }
    if (n <= 3)
        SDL_Log("d3d: Swap, frame %llu", (unsigned long long)n);
    {
        /* Measured frame rate, in vblanks per frame, every 5 seconds. */
        static Uint64 t0, f0, v0;
        Uint64 now = SDL_GetTicksNS();
        if (!t0)
            t0 = now, f0 = n, v0 = current_vblank();
        else if (now - t0 >= 5 * SDL_NS_PER_SECOND) {
            double fps = (double)(n - f0) * SDL_NS_PER_SECOND / (double)(now - t0);
            SDL_Log("d3d: %.2f fps (%.2f vblanks per frame), frame %llu; draws %d, drawn %d, dropped %d", fps,
                    (double)(current_vblank() - v0) / (double)(n - f0), (unsigned long long)n,
                    SDL_SetAtomicInt(&draws_called, 0), SDL_SetAtomicInt(&draws_drawn, 0),
                    SDL_SetAtomicInt(&draws_dropped, 0));
            t0 = now, f0 = n, v0 = current_vblank();
            peek_guest();
        }
    }
    g_eax = 0;
    g_esp += 4 + 4;
}

/* D3D_BlockOnTime(Time, Flags): every GPU wait in the library comes here
 * ("has the GPU got past push-buffer time T yet?"). Our renderer copies what
 * it needs at draw time and presents synchronously, so the GPU is never
 * behind: the answer is always yes. */
void sub_002B30B0(void)
{
    g_esp += 4 + 8;
}

/* Tracing hook for the generated entry hooks in d3d_calltrace.c.
 * PDO_D3D_TRACE=first-last logs every Direct3D call made during those frames
 * (PDO_D3D_TRACE=1 means frames 0-2): name, stack arguments, ecx and edx
 * (some XDK functions are __fastcall). */
void d3d_trace_call(uint32_t va, const char *name, int args)
{
    static int parsed;
    static uint64_t first, last;
    uint64_t frame;
    char line[512];
    int len;

    if (!parsed) {
        const char *v = getenv("PDO_D3D_TRACE");
        parsed = 1;
        if (v && *v && *v != '0') {
            parsed = 2;
            first = 0, last = 2;
            if (strchr(v, '-'))
                first = strtoull(v, NULL, 10), last = strtoull(strchr(v, '-') + 1, NULL, 10);
        }
    }
    if (parsed != 2)
        return;
    frame = pdo_gfx_frame_count();
    if (frame < first || frame > last)
        return;
    len = snprintf(line, sizeof line, "d3d %llu: %s(", (unsigned long long)frame, name);
    for (int i = 0; i < args && len < (int)sizeof line - 16; i++)
        len += snprintf(line + len, sizeof line - (size_t)len, "%s0x%x", i ? ", " : "", MEM32(g_esp + 4 + 4 * i));
    snprintf(line + len, sizeof line - (size_t)len, ") ecx=0x%x edx=0x%x [0x%08x]", g_ecx, g_edx, va);
    SDL_Log("%s", line);
}

/* ── State capture ────────────────────────────────────────────────────────
 * The XDK's own setters still run (wrapped below), so the library's
 * bookkeeping and its state tables stay exactly as on the console; we only
 * note what each draw needs. Render and texture-stage states are read from
 * the library's tables at draw time (D3D__RenderState, D3D__TextureState),
 * which also covers the game's inline writes to them. */

#define RS_BASE 0x002BD918u   /* D3D__RenderState[] */
#define TS_BASE 0x002BD718u   /* D3D__TextureState[4][32] */
#define RS(i)    MEM32(RS_BASE + 4u * (i))
#define TS(s, i) MEM32(TS_BASE + 128u * (s) + 4u * (i))

/* D3DRENDERSTATETYPE (XDK 4928), as far as used here.
 *
 * Simple states (57-91) are numbered as in 4134: Orta's encode table at
 * 0x00310BFC maps 57.. to NV2A methods in 4134's order (depth func, alpha
 * func, blend enable...), then adds DEPTHCLIPCONTROL 82, STIPPLEENABLE 83
 * and 8 unused, ending at 92. Deferred states are 4134's + 10 (FOGENABLE
 * 92: XbSymbolDatabase's D3DRS_FogEnable is RS_BASE + 4*92). Complex states
 * are 4134's + 19: each checked against the address its setter writes
 * (FogColor 138, ZEnable 143, StencilEnable 144, StencilFail 145, CullMode
 * 147, TextureFactor 148, ZBias 149). */
enum {
    RS_ZFUNC = 57, RS_ALPHAFUNC = 58, RS_ALPHABLENDENABLE = 59, RS_ALPHATESTENABLE = 60,
    RS_ALPHAREF = 61, RS_SRCBLEND = 62, RS_DESTBLEND = 63, RS_ZWRITEENABLE = 64,
    RS_COLORWRITEENABLE = 67, RS_STENCILZFAIL = 68, RS_STENCILPASS = 69, RS_STENCILFUNC = 70,
    RS_STENCILREF = 71, RS_STENCILMASK = 72, RS_STENCILWRITEMASK = 73, RS_BLENDOP = 74,
    RS_POLYGONOFFSETZSLOPESCALE = 77, RS_POLYGONOFFSETZOFFSET = 78, RS_SOLIDOFFSETENABLE = 81,
    RS_FOGENABLE = 92, RS_FOGTABLEMODE = 93, RS_FOGSTART = 94, RS_FOGEND = 95, RS_FOGDENSITY = 96,
    RS_LIGHTING = 102, RS_SPECULARENABLE = 103, RS_COLORVERTEX = 105,
    RS_SPECULARMATERIALSOURCE = 110, RS_DIFFUSEMATERIALSOURCE = 111,
    RS_AMBIENTMATERIALSOURCE = 112, RS_EMISSIVEMATERIALSOURCE = 113, RS_AMBIENT = 115,
    RS_FOGCOLOR = 138,
    RS_ZENABLE = 143, RS_STENCILENABLE = 144, RS_STENCILFAIL = 145, RS_CULLMODE = 147,
    RS_TEXTUREFACTOR = 148,
};
/* D3DDevice_SetRenderState_Simple(method in ecx, value in edx): writes one
 * NV2A method to the push buffer. The XDK's inline SetRenderState stores the
 * value in D3D__RenderState before calling it, but Orta's renderer object calls
 * it directly (0x00279C70 sets COLORWRITEENABLE this way, others blending and
 * the depth test), so the table we read at draw time never saw those states:
 * blending stayed off and text boxes came out opaque. Record the value under
 * the state whose method it is, from the XDK's own encode table (states 57-83;
 * the values of simple states are the NV2A ones). */
#define RS_ENCODE 0x00310BFCu
void sub_002AF710_enter(void)
{
    static uint32_t method[83 + 1];
    static bool ready;

    d3d_trace_call(0x002AF710u, "D3DDevice_SetRenderState_Simple", 0);
    if (!ready) {
        for (int i = 57; i <= 83; i++)
            method[i] = MEM32(RS_ENCODE + 4u * (uint32_t)(i - 57));
        ready = true;
    }
    for (int i = 57; i <= 83; i++)
        if (method[i] == g_ecx) {
            RS(i) = g_edx;
            return;
        }
}

/* D3DTEXTURESTAGESTATETYPE: as in 4134 (SetTextureState_TexCoordIndex,
 * 0x002AFF70, writes index 28). */
enum {
    TSS_ADDRESSU = 0, TSS_ADDRESSV = 1, TSS_MAGFILTER = 3, TSS_MINFILTER = 4, TSS_MIPFILTER = 5,
    TSS_COLOROP = 12, TSS_COLORARG0 = 13, TSS_COLORARG1 = 14, TSS_COLORARG2 = 15,
    TSS_ALPHAOP = 16, TSS_ALPHAARG0 = 17, TSS_ALPHAARG1 = 18, TSS_ALPHAARG2 = 19,
    TSS_RESULTARG = 20, TSS_TEXTURETRANSFORMFLAGS = 21, TSS_BUMPENVMAT00 = 22,
    TSS_TEXCOORDINDEX = 28,
};
/* D3DTRANSFORMSTATETYPE on the Xbox: VIEW 0, PROJECTION 1, TEXTURE0-3 2-5,
 * WORLD 6, WORLD1-3 7-9. */
enum { TS_VIEW = 0, TS_PROJECTION = 1, TS_TEXTURE0 = 2, TS_WORLD = 6, TS_COUNT = 10 };

static struct {
    uint32_t tex[4];
    uint32_t vb, stride;          /* stream 0 */
    uint32_t vbs[16], strides[16];
    uint32_t ib, base_vertex;
    uint32_t vshader;
    uint32_t pshader;             /* SetPixelShader handle; 0 = fixed function */
    float    mat[TS_COUNT][16];
    float    material[4][4];      /* D3DMATERIAL8: diffuse, ambient, specular, emissive */
    float    power;
    uint8_t  light_on[8];
    uint32_t light_raw[8][26];    /* D3DLIGHT8 as the game passed it */
    bool     have_viewport;
    uint32_t vp_x, vp_y, vp_w, vp_h;
    float    vp_minz, vp_maxz;
} st;

static void ident(float *m)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void state_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    for (int i = 0; i < TS_COUNT; i++)
        ident(st.mat[i]);
}

#define WRAP_BEGIN(n) uint32_t a[n]; for (int i_ = 0; i_ < (n); i_++) a[i_] = MEM32(g_esp + 4 + 4 * i_)

/* ── Render targets ───────────────────────────────────────────────────────
 * The back and front buffers are surface objects the library creates in
 * CreateDevice and sets as the target before the game asks for them (4928
 * keeps them in the device, at DEV_BACKBUFFERS; JSRF's 4134 had them in its
 * data), plus whatever GetBackBuffer2 hands out. Our Swap never rotates
 * buffers, so they stay put. Any other surface set as a target is drawn on
 * the GPU (see pdo_gfx_set_target). D3DSurface: Common, Data +4, Lock +8,
 * Format +0xC, Size +0x10, Parent +0x14. */

#define MAX_BACKBUFFERS 8
static uint32_t backbuffers[MAX_BACKBUFFERS];
static int nbackbuffers;
static pdo_surface cur_rt;                           /* data 0: back buffer */

static void note_backbuffer(uint32_t surf)
{
    if (!surf)
        return;
    for (int i = 0; i < nbackbuffers; i++)
        if (backbuffers[i] == surf)
            return;
    if (nbackbuffers < MAX_BACKBUFFERS) {
        backbuffers[nbackbuffers++] = surf;
        SDL_Log("d3d: back buffer %d: surface 0x%08x data 0x%08x", nbackbuffers - 1, surf, MEM32(surf + 4));
    }
}

/* After CreateDevice: the buffers it made. CreateDevice also set the back
 * buffer as the target, through our SetRenderTarget, before it was known
 * to be one; re-resolve the target now. */
static void note_backbuffers(void)
{
    uint32_t dev = MEM32(D3D_DEVICE_PTR);
    for (uint32_t i = 0; dev && i < 3; i++)
        note_backbuffer(MEM32(dev + DEV_BACKBUFFERS + 4 * i));
    for (int i = 0; cur_rt.data && i < nbackbuffers; i++)
        if (MEM32(backbuffers[i] + 4) == cur_rt.data) {
            memset(&cur_rt, 0, sizeof cur_rt);
            pdo_gfx_set_target(&cur_rt);
            break;
        }
}

static void target_size(int *w, int *h)
{
    pdo_gfx_surface_size(&cur_rt, w, h);
}

static pdo_surface surface_of(uint32_t surf)
{
    pdo_surface s = { 0, 0, 0 };

    if (!surf)
        return s;
    for (int i = 0; i < nbackbuffers; i++)
        if (backbuffers[i] == surf || MEM32(backbuffers[i] + 4) == MEM32(surf + 4))
            return s;
    s.data = MEM32(surf + 4);
    s.format_word = MEM32(surf + 0xC);
    s.size_word = MEM32(surf + 0x10);
    return s;
}

/* D3DDevice_GetBackBuffer2(BackBuffer): 4928 returns the surface (4134's
 * GetBackBuffer stored it through a pointer argument). */
extern void sub_002ADF50_gen(void);
void sub_002ADF50(void)
{
    sub_002ADF50_gen();
    note_backbuffer(g_eax);
}

/* D3DDevice_SetRenderTarget(pRenderTarget, pNewZStencil). NULL keeps the
 * current colour target. */
extern void sub_002AD9C0_gen(void);
void sub_002AD9C0(void)
{
    WRAP_BEGIN(2);
    if (a[0]) {
        cur_rt = surface_of(a[0]);
        pdo_gfx_set_target(&cur_rt);
        if (getenv("PDO_D3D_TRACE"))
            SDL_Log("d3d: SetRenderTarget 0x%08x data 0x%08x format 0x%08x size 0x%08x",
                    a[0], cur_rt.data, cur_rt.format_word, cur_rt.size_word);
    }
    sub_002AD9C0_gen();     /* also resets the viewport to the target */
}

/* D3DDevice_CopyRects(pSourceSurface, pSourceRectsArray, cRects,
 *                     pDestinationSurface, pDestPointsArray): RECTs are
 * {left, top, right, bottom}, POINTs {x, y}; no stretching. */
extern void sub_002AE0C0_gen(void);
void sub_002AE0C0(void)
{
    pdo_surface src, dst;
    uint32_t n;

    WRAP_BEGIN(5);
    src = surface_of(a[0]);
    dst = surface_of(a[3]);
    n = a[1] ? a[2] : 1;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        int x = 0, y = 0, w, h, dx, dy;
        if (a[1]) {
            uint32_t r = a[1] + 16 * i;
            x = (int)MEM32(r);
            y = (int)MEM32(r + 4);
            w = (int)MEM32(r + 8) - x;
            h = (int)MEM32(r + 12) - y;
        } else {
            pdo_gfx_surface_size(&src, &w, &h);
        }
        dx = a[4] ? (int)MEM32(a[4] + 8 * i) : x;
        dy = a[4] ? (int)MEM32(a[4] + 8 * i + 4) : y;
        if (getenv("PDO_D3D_TRACE"))
            SDL_Log("d3d: CopyRects 0x%08x -> 0x%08x (%d,%d %dx%d) at %d,%d",
                    src.data, dst.data, x, y, w, h, dx, dy);
        pdo_gfx_copy_rect(&src, &dst, x, y, w, h, dx, dy);
    }
    sub_002AE0C0_gen();
}

/* ── Fixed-function lighting state ─────────────────────────────────────── */

static void unpack_d3dcolor(uint32_t c, float out[4]);
static uint32_t pack_color(const float *c);

/* D3DDevice_SetMaterial(pMaterial) */
extern void sub_002ADBE0_gen(void);
void sub_002ADBE0(void)
{
    WRAP_BEGIN(1);
    sub_002ADBE0_gen();
    if (a[0]) {
        for (int i = 0; i < 16; i++) {
            uint32_t v = MEM32(a[0] + 4 * i);
            memcpy(&st.material[i / 4][i % 4], &v, 4);
        }
        {
            uint32_t v = MEM32(a[0] + 64);
            memcpy(&st.power, &v, 4);
        }
    }
}

/* D3DDevice_SetLight(Index, pLight) */
extern void sub_002AE510_gen(void);
void sub_002AE510(void)
{
    WRAP_BEGIN(2);
    sub_002AE510_gen();
    if (a[0] < 8 && a[1])
        for (int i = 0; i < 26; i++)
            st.light_raw[a[0]][i] = MEM32(a[1] + 4 * i);
}

/* D3DDevice_LightEnable(Index, bEnable) */
extern void sub_002AE6F0_gen(void);
void sub_002AE6F0(void)
{
    WRAP_BEGIN(2);
    sub_002AE6F0_gen();
    if (a[0] < 8)
        st.light_on[a[0]] = a[1] != 0;
}

static float lf(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* Direct3D fixed-function lighting for one vertex, world-space position p
 * and normal n (unit, or zero without normals): emissive + global ambient
 * x ambient + per light (ambient + diffuse N.L) x attenuation, with
 * material colours taken from the vertex where the material sources say
 * so. Specular highlights are not computed. Returns a D3DCOLOR. */
static uint32_t light_vertex(const float p[3], const float n[3], uint32_t vdiffuse, uint32_t vspecular,
                             bool has_diffuse, bool has_specular)
{
    float mat[4][4], amb[4], out[4];
    bool cv = RS(RS_COLORVERTEX) != 0;
    static const int src_rs[4] = { RS_DIFFUSEMATERIALSOURCE, RS_AMBIENTMATERIALSOURCE,
                                   RS_SPECULARMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE };

    memcpy(mat, st.material, sizeof mat);
    for (int k = 0; k < 4; k++) {
        uint32_t src = RS(src_rs[k]);   /* D3DMCS_MATERIAL 0, COLOR1 1, COLOR2 2 */
        if (cv && src == 1 && has_diffuse)
            unpack_d3dcolor(vdiffuse, mat[k]);
        else if (cv && src == 2 && has_specular)
            unpack_d3dcolor(vspecular, mat[k]);
    }
    unpack_d3dcolor(RS(RS_AMBIENT), amb);
    for (int c = 0; c < 3; c++)
        out[c] = mat[3][c] + amb[c] * mat[1][c];
    for (int l = 0; l < 8; l++) {
        const uint32_t *L = st.light_raw[l];
        uint32_t type = L[0];
        float ldir[3], att = 1.0f, ndotl;

        if (!st.light_on[l])
            continue;
        /* D3DLIGHT8 in words: Type 0, Diffuse 1-4, Specular 5-8, Ambient 9-12,
         * Position 13-15, Direction 16-18, Range 19, Falloff 20,
         * Attenuation0-2 21-23, Theta 24, Phi 25. */
        if (type == 3) {                          /* directional */
            for (int c = 0; c < 3; c++)
                ldir[c] = -lf(L[16 + c]);
        } else {                                  /* point, spot */
            float d2 = 0, d, range = lf(L[19]);
            for (int c = 0; c < 3; c++) {
                ldir[c] = lf(L[13 + c]) - p[c];
                d2 += ldir[c] * ldir[c];
            }
            d = sqrtf(d2);
            if (range > 0 && d > range)
                continue;
            if (d > 0)
                for (int c = 0; c < 3; c++)
                    ldir[c] /= d;
            att = lf(L[21]) + lf(L[22]) * d + lf(L[23]) * d2;
            att = att > 0 ? 1.0f / att : 1.0f;
            if (type == 2) {                      /* spot: inner and outer cone */
                float dl = sqrtf(lf(L[16]) * lf(L[16]) + lf(L[17]) * lf(L[17]) + lf(L[18]) * lf(L[18]));
                float rho = 0, ct = cosf(lf(L[24]) * 0.5f), cp = cosf(lf(L[25]) * 0.5f);
                for (int c = 0; c < 3 && dl > 0; c++)
                    rho -= ldir[c] * lf(L[16 + c]) / dl;
                if (rho <= cp)
                    att = 0;
                else if (rho < ct && ct > cp)
                    att *= powf((rho - cp) / (ct - cp), lf(L[20]));
            }
        }
        ndotl = n[0] * ldir[0] + n[1] * ldir[1] + n[2] * ldir[2];
        if (ndotl < 0)
            ndotl = 0;
        for (int c = 0; c < 3; c++)
            out[c] += att * (lf(L[9 + c]) * mat[1][c] + ndotl * lf(L[1 + c]) * mat[0][c]);
    }
    out[3] = mat[0][3];
    return pack_color(out);
}

/* D3DDevice_SetTexture(Stage, pTexture) */
extern void sub_002AE810_gen(void);
void sub_002AE810(void)
{
    WRAP_BEGIN(2);
    sub_002AE810_gen();
    if (a[0] < 4)
        st.tex[a[0]] = a[1];
}

/* D3DDevice_SetStreamSource(StreamNumber, pStreamData, Stride) */
extern void sub_002B19A0_gen(void);
void sub_002B19A0(void)
{
    WRAP_BEGIN(3);
    sub_002B19A0_gen();
    if (a[0] < 16) {
        st.vbs[a[0]] = a[1];
        st.strides[a[0]] = a[2];
    }
    if (a[0] == 0) {
        st.vb = a[1];
        st.stride = a[2];
    }
}

/* ── Vertex shader programs ───────────────────────────────────────────────
 * Programs are captured when created, keyed by handle, and run on the CPU
 * (src/guest/vsh.c). The constant file is the library's own shadow of what
 * it sent the GPU: 4928's SetVertexShaderConstant1/4/NotInline (the only
 * forms Orta's code calls; it writes no constant loads into the push buffer
 * itself) copy each constant to VSH_SHADOW + 16 * slot, slot being the NV2A
 * one (D3D register + 96). It is read at each draw. */

#define VSH_SHADOW 0x002BEB08u

static float vconst[PDO_VSH_CONSTANTS][4];

/* Orta creates a pure device (D3DCREATE_PUREDEVICE, flag 0x10 kept at
 * 0x002BDEB8): SetVertexShaderConstantNotInline then sends the constants to
 * the GPU only and skips the shadow, and that is how the game loads its
 * bone palettes (76 constants at slot 112 before each skinned draw). The
 * programs ran with stale matrices: vertices thrown thousands of units out,
 * the lines out of the dragon and the boss in pieces. So every constant
 * setter also writes the shadow here; a pure device never reads it back. */
static void shadow_constants(uint32_t slot, uint32_t src, uint32_t dwords)
{
    if (slot >= PDO_VSH_CONSTANTS || !src)
        return;
    if (dwords > (PDO_VSH_CONSTANTS - slot) * 4)
        dwords = (PDO_VSH_CONSTANTS - slot) * 4;
    for (uint32_t i = 0; i < dwords; i++)
        MEM32(VSH_SHADOW + 16 * slot + 4 * i) = MEM32(src + 4 * i);
}

/* D3DDevice_SetVertexShaderConstant1(ecx = slot, edx = pConstantData) */
extern void sub_002B1770_gen(void);
void sub_002B1770(void)
{
    shadow_constants(g_ecx, g_edx, 4);
    sub_002B1770_gen();
}

/* D3DDevice_SetVertexShaderConstant4(ecx = slot, edx = pConstantData) */
extern void sub_002B17D0_gen(void);
void sub_002B17D0(void)
{
    shadow_constants(g_ecx, g_edx, 16);
    sub_002B17D0_gen();
}

/* D3DDevice_SetVertexShaderConstantNotInline(ecx = slot, edx = pConstantData,
 * DwordCount) */
extern void sub_002B1960_gen(void);
void sub_002B1960(void)
{
    shadow_constants(g_ecx, g_edx, MEM32(g_esp + 4));
    sub_002B1960_gen();
}

/* D3DDevice_SetShaderConstantMode(Mode): the 96-constant mode loads its
 * fixed constants (slots 60-62, from a table in the library) straight into
 * the push buffer. */
extern void sub_002B1B10_gen(void);
void sub_002B1B10(void)
{
    if ((MEM32(g_esp + 4) & ~0x10u) == 0)
        shadow_constants(60, 0x002BC618u, 12);
    sub_002B1B10_gen();
}

#define MAX_PROGRAMS 256
static struct { uint32_t handle; pdo_vsh_program *prog; } programs[MAX_PROGRAMS];

static const pdo_vsh_program *find_program(uint32_t handle)
{
    for (int i = 0; i < MAX_PROGRAMS; i++)
        if (programs[i].handle == handle && programs[i].prog)
            return programs[i].prog;
    return NULL;
}

/* D3DDevice_CreateVertexShader(pDeclaration, pFunction, pHandle, Usage) */
extern void sub_002B1630_gen(void);
void sub_002B1630(void)
{
    WRAP_BEGIN(4);
    uint32_t decl[96], func[1 + 4 * PDO_VSH_MAX_INSNS], nd = 0, nf = 0, handle;
    pdo_vsh_program *p;
    int slot = -1;

    sub_002B1630_gen();
    handle = a[2] ? MEM32(a[2]) : 0;
    if (!handle || !a[1])
        return;
    for (; a[0] && nd < 96; nd++)
        if ((decl[nd] = MEM32(a[0] + 4 * nd)) == 0xFFFFFFFFu) {
            nd++;
            break;
        }
    func[0] = MEM32(a[1]);
    nf = 1 + 4 * (func[0] >> 16);
    if (nf > SDL_arraysize(func))
        nf = SDL_arraysize(func);
    for (uint32_t i = 1; i < nf; i++)
        func[i] = MEM32(a[1] + 4 * i);

    p = SDL_malloc(sizeof *p);
    if (!p)
        return;
    if (!pdo_vsh_parse(p, func, nf, a[0] ? decl : NULL, nd)) {
        SDL_Log("d3d: vertex shader 0x%08x: function header 0x%08x not handled", handle, func[0]);
        SDL_free(p);
        return;
    }
    for (int i = 0; i < MAX_PROGRAMS && slot < 0; i++)
        if (programs[i].handle == handle)
            slot = i;
    for (int i = 0; i < MAX_PROGRAMS && slot < 0; i++)
        if (!programs[i].prog)
            slot = i;
    if (slot < 0) {
        SDL_Log("d3d: more than %d vertex shaders", MAX_PROGRAMS);
        SDL_free(p);
        return;
    }
    SDL_free(programs[slot].prog);
    programs[slot].handle = handle;
    programs[slot].prog = p;
}

/* D3DDevice_SetPixelShaderConstant(Register, pConstantData, ConstantCount):
 * float4 constants 0-15, routed to a stage's C0/C1 by the definition's
 * PSC0Mapping/PSC1Mapping (a nibble per stage, 0xF = the definition's own
 * PSConstant value). */
static float psconst[16][4];
static bool psconst_set[16];

/* D3DDevice_SetPixelShader(Handle): 0 = fixed-function stages, otherwise
 * the definition at handle + 8 drives the register combiners in ff.frag.
 * Each new one is logged with its definition. */
extern void sub_002BB5D0_gen(void);
void sub_002BB5D0(void)
{
    WRAP_BEGIN(1);
    sub_002BB5D0_gen();
    if (a[0] != st.pshader && a[0]) {
        static uint32_t seen[64];
        static int nseen;
        int i;
        for (i = 0; i < nseen && seen[i] != a[0]; i++)
            ;
        if (i == nseen && nseen < 64) {
            uint32_t def = MEM32(a[0] + 8);
            seen[nseen++] = a[0];
            SDL_Log("d3d: pixel shader 0x%08x set (%d distinct so far), definition:", a[0], nseen);
            for (int r = 0; def && r < 60; r += 10)
                SDL_Log("  ps[%2d] %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", r,
                        MEM32(def + 4 * r), MEM32(def + 4 * r + 4), MEM32(def + 4 * r + 8),
                        MEM32(def + 4 * r + 12), MEM32(def + 4 * r + 16), MEM32(def + 4 * r + 20),
                        MEM32(def + 4 * r + 24), MEM32(def + 4 * r + 28), MEM32(def + 4 * r + 32),
                        MEM32(def + 4 * r + 36));
        }
    }
    st.pshader = a[0];
    /* The XDK's SetPixelShader loads every constant from the definition
     * into the combiners; values set earlier with SetPixelShaderConstant
     * do not survive it. Kept, a constant meant for another shader replaced
     * the graffiti shader's channel selectors and hid layers of the tag. */
    memset(psconst_set, 0, sizeof psconst_set);
}


extern void sub_002BB7D0_gen(void);
void sub_002BB7D0(void)
{
    WRAP_BEGIN(3);
    for (uint32_t i = 0; a[1] && i < a[2]; i++)
        if (a[0] + i < 16) {
            for (int k = 0; k < 4; k++) {
                uint32_t b = MEM32(a[1] + 16 * i + 4 * (uint32_t)k);
                memcpy(&psconst[a[0] + i][k], &b, 4);
            }
            psconst_set[a[0] + i] = true;
        }
    sub_002BB7D0_gen();
}


/* D3DDevice_SetIndices(pIndexData, BaseVertexIndex) */
extern void sub_002AE9C0_gen(void);
void sub_002AE9C0(void)
{
    WRAP_BEGIN(2);
    sub_002AE9C0_gen();
    st.ib = a[0];
    st.base_vertex = a[1];
}

/* D3DDevice_SetVertexShader(Handle): an FVF code, or a program (bit 0 set). */
extern void sub_002B1D70_gen(void);
void sub_002B1D70(void)
{
    WRAP_BEGIN(1);
    sub_002B1D70_gen();
    st.vshader = a[0];
}

/* D3DDevice_SetTransform(State, pMatrix) */
extern void sub_002ADAA0_gen(void);
void sub_002ADAA0(void)
{
    WRAP_BEGIN(2);
    state_init();
    if (a[0] < TS_COUNT && a[1])
        for (int i = 0; i < 16; i++) {
            uint32_t bits = MEM32(a[1] + 4 * (uint32_t)i);
            memcpy(&st.mat[a[0]][i], &bits, 4);
        }
    sub_002ADAA0_gen();
}

/* D3DDevice_SetViewport(pViewport): D3DVIEWPORT8 {X, Y, Width, Height,
 * float MinZ, float MaxZ}. */
extern void sub_002AE3B0_gen(void);
void sub_002AE3B0(void)
{
    WRAP_BEGIN(1);
    if (a[0]) {
        uint32_t z0 = MEM32(a[0] + 16), z1 = MEM32(a[0] + 20);
        st.vp_x = MEM32(a[0]);
        st.vp_y = MEM32(a[0] + 4);
        st.vp_w = MEM32(a[0] + 8);
        st.vp_h = MEM32(a[0] + 12);
        memcpy(&st.vp_minz, &z0, 4);
        memcpy(&st.vp_maxz, &z1, 4);
        st.have_viewport = true;
    }
    sub_002AE3B0_gen();
}

/* ── Draws ────────────────────────────────────────────────────────────── */

static void mat_mul(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++)
                v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}

static float f32(uint32_t va)
{
    uint32_t bits = MEM32(va);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void unpack_d3dcolor(uint32_t c, float out[4])
{
    out[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(c & 0xFF) / 255.0f;
    out[3] = (float)(c >> 24) / 255.0f;
}

static void fill_draw_state(pdo_draw *d, bool pretransformed)
{
    int rtw, rth;

    memset(d, 0, sizeof *d);
    target_size(&rtw, &rth);
    d->ps.blend_enable = RS(RS_ALPHABLENDENABLE) != 0;
    d->ps.src_blend = (uint16_t)RS(RS_SRCBLEND);
    d->ps.dst_blend = (uint16_t)RS(RS_DESTBLEND);
    d->ps.blend_op = (uint16_t)RS(RS_BLENDOP);
    d->ps.z_enable = RS(RS_ZENABLE) != 0;
    d->ps.z_write = RS(RS_ZWRITEENABLE) != 0;
    d->ps.z_func = (uint16_t)RS(RS_ZFUNC);
    d->ps.color_write = RS(RS_COLORWRITEENABLE);
    /* D3DCULL_NONE 0, D3DCULL_CW 0x900, D3DCULL_CCW 0x901 (GL-valued). */
    d->ps.cull = RS(RS_CULLMODE) == 0x900 ? 1 : RS(RS_CULLMODE) == 0x901 ? 2 : 0;
    /* Stencil (STENCILENABLE 144 and STENCILFAIL 145: their setters write
     * there; the rest sit with the simple states). */
    if (RS(RS_STENCILENABLE)) {
        d->ps.stencil_enable = 1;
        d->ps.stencil_func = (uint16_t)RS(RS_STENCILFUNC);
        d->ps.stencil_fail = (uint16_t)RS(RS_STENCILFAIL);
        d->ps.stencil_zfail = (uint16_t)RS(RS_STENCILZFAIL);
        d->ps.stencil_pass = (uint16_t)RS(RS_STENCILPASS);
        d->ps.stencil_mask = (uint8_t)RS(RS_STENCILMASK);
        d->ps.stencil_write_mask = (uint8_t)RS(RS_STENCILWRITEMASK);
        d->stencil_ref = (uint8_t)RS(RS_STENCILREF);
    }
    /* Polygon offset, as D3DRS_ZBIAS leaves it: offset -ZBias depth units,
     * slope scale -ZBias times a constant. Same units as our 24-bit depth. */
    if (RS(RS_SOLIDOFFSETENABLE)) {
        uint32_t o = RS(RS_POLYGONOFFSETZOFFSET), sl = RS(RS_POLYGONOFFSETZSLOPESCALE);
        memcpy(&d->ps.depth_bias, &o, 4);
        memcpy(&d->ps.depth_slope, &sl, 4);
    }

    for (int s = 0; s < 4; s++) {
        uint32_t t = st.tex[s];
        d->ff.color_op[s][0] = (int32_t)TS(s, TSS_COLOROP);
        d->ff.color_op[s][1] = (int32_t)TS(s, TSS_COLORARG1);
        d->ff.color_op[s][2] = (int32_t)TS(s, TSS_COLORARG2);
        d->ff.color_op[s][3] = (int32_t)TS(s, TSS_COLORARG0);
        d->ff.alpha_op[s][0] = (int32_t)TS(s, TSS_ALPHAOP);
        d->ff.alpha_op[s][1] = (int32_t)TS(s, TSS_ALPHAARG1);
        d->ff.alpha_op[s][2] = (int32_t)TS(s, TSS_ALPHAARG2);
        d->ff.alpha_op[s][3] = (int32_t)TS(s, TSS_ALPHAARG0);
        d->ff.stage_misc[s][0] = (int32_t)TS(s, TSS_RESULTARG);
        d->ff.stage_misc[s][1] = t != 0;
        d->ff.stage_misc[s][2] = s;
        d->ff.tc_scale[s][0] = d->ff.tc_scale[s][1] = 1.0f;
        /* BUMPENVMAT00, 01, 11, 10, LSCALE, LOFFSET: indices 22-27, which
         * 0x002B0260 copies as they are into the NV2A's per-stage bump
         * environment methods (0x1B68). */
        for (int i = 0; i < 4; i++) {
            uint32_t m = TS(s, TSS_BUMPENVMAT00 + i);
            memcpy(&d->ff.bump_mat[s][i], &m, 4);
        }
        for (int i = 0; i < 2; i++) {
            uint32_t m = TS(s, TSS_BUMPENVMAT00 + 4 + i);
            memcpy(&d->ff.bump_lum[s][i], &m, 4);
        }
        if (t) {
            pdo_xtex_info ti;
            pdo_xtex_describe(MEM32(t + 12), MEM32(t + 16), &ti);
            if (ti.pitch && ti.width && ti.height) {    /* linear: texel coordinates */
                d->ff.tc_scale[s][0] = 1.0f / (float)ti.width;
                d->ff.tc_scale[s][1] = 1.0f / (float)ti.height;
            }
            /* No alpha in the format: alpha reads as 1. Decoded textures
             * have it already; render targets (RGBA on our side) need it. */
            switch (ti.format) {
            case 0x03: case 0x05: case 0x07: case 0x11: case 0x1C: case 0x1E:
                d->ff.tc_scale[s][2] = 1.0f;
                break;
            case 0x28:      /* V8U8 */
                d->ff.tc_scale[s][3] = 1.0f;
                break;
            }
            d->tex[s].data = MEM32(t + 4);
            d->tex[s].format_word = MEM32(t + 12);
            d->tex[s].size_word = MEM32(t + 16);
            d->tex[s].address_u = (uint8_t)TS(s, TSS_ADDRESSU);
            d->tex[s].address_v = (uint8_t)TS(s, TSS_ADDRESSV);
            d->tex[s].mag_filter = (uint8_t)TS(s, TSS_MAGFILTER);
            d->tex[s].min_filter = (uint8_t)TS(s, TSS_MINFILTER);
            d->tex[s].mip_filter = (uint8_t)TS(s, TSS_MIPFILTER);
        }
    }
    {
        uint32_t tf = RS(RS_TEXTUREFACTOR);
        d->ff.tfactor[0] = (float)((tf >> 16) & 0xFF) / 255.0f;
        d->ff.tfactor[1] = (float)((tf >> 8) & 0xFF) / 255.0f;
        d->ff.tfactor[2] = (float)(tf & 0xFF) / 255.0f;
        d->ff.tfactor[3] = (float)(tf >> 24) / 255.0f;
    }
    if (st.pshader) {
        /* D3DPIXELSHADERDEF, at handle + 8 (4134 and 4928 alike), in words:
         * 0 PSAlphaInputs[8], 8 FinalCombinerInputsABCD, 9 ...EFG,
         * 10 PSConstant0[8], 18 PSConstant1[8], 26 PSAlphaOutputs[8],
         * 34 PSRGBInputs[8], 42 CompareMode, 43/44 FinalCombinerConstant0/1,
         * 45 PSRGBOutputs[8], 53 PSCombinerCount, 54 PSTextureModes, ... */
        uint32_t def = MEM32(st.pshader + 8);
        if (def) {
            for (int i = 0; i < 8; i++) {
                d->ff.ps_a_in[i] = (int32_t)MEM32(def + 4 * (0 + i));
                d->ff.ps_a_out[i] = (int32_t)MEM32(def + 4 * (26 + i));
                d->ff.ps_rgb_in[i] = (int32_t)MEM32(def + 4 * (34 + i));
                d->ff.ps_rgb_out[i] = (int32_t)MEM32(def + 4 * (45 + i));
                unpack_d3dcolor(MEM32(def + 4 * (10 + i)), d->ff.ps_c0[i]);
                unpack_d3dcolor(MEM32(def + 4 * (18 + i)), d->ff.ps_c1[i]);
            }
            {   /* Constants routed from SetPixelShaderConstant. */
                uint32_t m0 = MEM32(def + 4 * 57), m1 = MEM32(def + 4 * 58);
                for (int i = 0; i < 8; i++) {
                    uint32_t k0 = (m0 >> (4 * i)) & 0xF, k1 = (m1 >> (4 * i)) & 0xF;
                    if (k0 < 0xF && psconst_set[k0])
                        memcpy(d->ff.ps_c0[i], psconst[k0], sizeof psconst[k0]);
                    if (k1 < 0xF && psconst_set[k1])
                        memcpy(d->ff.ps_c1[i], psconst[k1], sizeof psconst[k1]);
                }
            }
            d->ff.ps_final[0] = (int32_t)MEM32(def + 4 * 8);
            d->ff.ps_final[1] = (int32_t)MEM32(def + 4 * 9);
            d->ff.ps_final[2] = (int32_t)MEM32(def + 4 * 53);
            d->ff.ps_final[3] = (int32_t)MEM32(def + 4 * 54);
            unpack_d3dcolor(MEM32(def + 4 * 43), d->ff.ps_fc0);
            unpack_d3dcolor(MEM32(def + 4 * 44), d->ff.ps_fc1);
        }
    }
    /* Fog (FOGCOLOR is state 138 in 4928: its setter writes there). */
    if (RS(RS_FOGENABLE)) {
        uint32_t v;
        d->ff.fog_mode[0] = 1;
        d->ff.fog_mode[1] = pretransformed ? 0 : (int32_t)RS(RS_FOGTABLEMODE);
        unpack_d3dcolor(RS(RS_FOGCOLOR), d->ff.fog_color);
        v = RS(RS_FOGSTART);   memcpy(&d->ff.fog_params[0], &v, 4);
        v = RS(RS_FOGEND);     memcpy(&d->ff.fog_params[1], &v, 4);
        v = RS(RS_FOGDENSITY); memcpy(&d->ff.fog_params[2], &v, 4);
    }
    d->ff.alpha_test[0] = RS(RS_ALPHATESTENABLE) != 0;
    d->ff.alpha_test[1] = (int32_t)RS(RS_ALPHAFUNC);
    d->ff.alpha_test[2] = (int32_t)(RS(RS_ALPHAREF) & 0xFF);
    d->ff.alpha_test[3] = RS(RS_SPECULARENABLE) != 0;

    if (pretransformed || !st.have_viewport) {
        d->viewport[2] = (float)rtw;
        d->viewport[3] = (float)rth;
        d->viewport[5] = 1.0f;
    } else {
        d->viewport[0] = (float)st.vp_x;
        d->viewport[1] = (float)st.vp_y;
        d->viewport[2] = (float)st.vp_w;
        d->viewport[3] = (float)st.vp_h;
        d->viewport[4] = st.vp_minz;
        d->viewport[5] = st.vp_maxz;
    }
}

/* D3DTSS_TEXTURETRANSFORMFLAGS: stage s's coordinates times its texture
 * matrix (D3DTS_TEXTURE0 + s), as a row vector. Low byte: how many output
 * components the rasterizer uses (0 disables); 256 (PROJECTED): divide by
 * the last of them. The input is padded the way D3D pads it: the first
 * missing component is 1, so a 2D coordinate (u, v, 1, 0) picks up the
 * matrix's third row as a translation -- how textures are scrolled. */
static void texture_transform(int s, uint32_t n, float *tc)
{
    uint32_t flags = TS(s, TSS_TEXTURETRANSFORMFLAGS), count = flags & 0xFF;
    const float *m = st.mat[TS_TEXTURE0 + s];
    float in[4] = { 0, 0, 0, 0 }, out[4];

    if (!count)
        return;
    for (uint32_t c = 0; c < n && c < 4; c++)
        in[c] = tc[c];
    if (n < 4)
        in[n] = 1.0f;
    for (int c = 0; c < 4; c++)
        out[c] = in[0] * m[c] + in[1] * m[4 + c] + in[2] * m[8 + c] + in[3] * m[12 + c];
    if ((flags & 256) && count >= 2 && count <= 4 && out[count - 1] != 0.0f)
        for (uint32_t c = 0; c + 1 < count; c++)
            out[c] /= out[count - 1];
    for (int c = 0; c < 4; c++)
        tc[c] = out[c];
}

/* Fetch vertices [first, first + count) of an FVF stream into our layout. */
static bool fetch_fvf(uint32_t fvf, uint32_t base, uint32_t stride, uint32_t first, uint32_t count,
                      pdo_vertex *out, bool *pretransformed)
{
    uint32_t pos = fvf & 0x00E, off = 0, ntc = (fvf >> 8) & 0xF, tc_off[4] = {0}, tc_n[4] = {0};
    uint32_t diffuse_off = 0, specular_off = 0, normal_off = 0, set_of[4];
    bool has_diffuse = (fvf & 0x40) != 0, has_specular = (fvf & 0x80) != 0, has_normal = (fvf & 0x10) != 0;
    bool lit;
    float wvp[16], wv[16];
    int rtw, rth;

    *pretransformed = pos == 0x004;
    if (pos == 0x002) off = 12;
    else if (pos == 0x004) off = 16;
    else if (pos >= 0x006 && pos <= 0x00C) off = 12 + 4 * ((pos - 0x004) / 2);
    else return false;
    if (fvf & 0x010) { normal_off = off; off += 12; }   /* normal */
    if (fvf & 0x020) off += 4;           /* point size */
    if (has_diffuse) { diffuse_off = off; off += 4; }
    if (has_specular) { specular_off = off; off += 4; }
    for (uint32_t i = 0; i < ntc && i < 4; i++) {
        static const uint32_t sizes[4] = { 2, 3, 4, 1 };
        tc_n[i] = sizes[(fvf >> (16 + 2 * i)) & 3];
        tc_off[i] = off;
        off += 4 * tc_n[i];
    }
    if (!stride)
        stride = off;
    for (int s = 0; s < 4; s++)
        set_of[s] = TS(s, TSS_TEXCOORDINDEX) & 0xFFFF;

    target_size(&rtw, &rth);
    lit = !*pretransformed && RS(RS_LIGHTING);
    mat_mul(st.mat[TS_WORLD], st.mat[TS_VIEW], wv);
    mat_mul(wv, st.mat[TS_PROJECTION], wvp);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = base + (first + i) * stride;
        pdo_vertex *o = &out[i];
        float x = f32(v), y = f32(v + 4), z = f32(v + 8);

        memset(o, 0, sizeof *o);
        if (*pretransformed) {
            float rhw = f32(v + 12), w = rhw != 0.0f ? 1.0f / rhw : 1.0f;
            o->pos[0] = (x / (float)rtw * 2.0f - 1.0f) * w;
            o->pos[1] = (1.0f - y / (float)rth * 2.0f) * w;
            o->pos[2] = z * w;
            o->pos[3] = w;
        } else {
            for (int c = 0; c < 4; c++)
                o->pos[c] = x * wvp[c] + y * wvp[4 + c] + z * wvp[8 + c] + wvp[12 + c];
        }
        o->diffuse = has_diffuse ? MEM32(v + diffuse_off) : 0xFFFFFFFFu;
        o->specular = has_specular ? MEM32(v + specular_off) : 0;
        if (lit) {
            const float *w = st.mat[TS_WORLD];
            float wp[3], wn[3] = { 0, 0, 0 };
            for (int c = 0; c < 3; c++)
                wp[c] = x * w[c] + y * w[4 + c] + z * w[8 + c] + w[12 + c];
            if (has_normal) {
                float nx = f32(v + normal_off), ny = f32(v + normal_off + 4), nz = f32(v + normal_off + 8), len;
                for (int c = 0; c < 3; c++)
                    wn[c] = nx * w[c] + ny * w[4 + c] + nz * w[8 + c];
                len = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                if (len > 0)
                    for (int c = 0; c < 3; c++)
                        wn[c] /= len;
            }
            o->diffuse = light_vertex(wp, wn, o->diffuse, o->specular, has_diffuse, has_specular);
            o->specular = 0;
        }
        /* Fog coordinate: eye depth for table fog, else the specular
         * alpha as the factor (no specular: no fog). */
        if (!*pretransformed && RS(RS_FOGTABLEMODE))
            o->fog = x * wv[2] + y * wv[6] + z * wv[10] + wv[14];
        else
            o->fog = has_specular ? (float)(o->specular >> 24) / 255.0f : 1.0f;
        for (int s = 0; s < 4; s++) {
            uint32_t set = set_of[s];
            if (set < ntc && set < 4)
                for (uint32_t c = 0; c < tc_n[set]; c++)
                    o->tc[s][c] = f32(v + tc_off[set] + 4 * c);
            if (!*pretransformed && set < ntc && set < 4)
                texture_transform(s, tc_n[set], o->tc[s]);
        }
    }
    return true;
}

static uint32_t pack_color(const float *c)
{
    uint32_t r[4];
    for (int i = 0; i < 4; i++) {
        float v = c[i] != c[i] ? 0.0f : c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
        r[i] = (uint32_t)(v * 255.0f + 0.5f);
    }
    return (r[3] << 24) | (r[0] << 16) | (r[1] << 8) | r[2];
}

/* Run the current vertex program over vertices [first, first + count).
 * base/stride describe stream 0 (a user-pointer draw passes its own); the
 * other streams come from SetStreamSource.
 *
 * The XDK ends every program with its viewport transform: oPos.xyz =
 * clip.xyz / w * c[58] + c[59] (D3D's c[-38]/c[-37]), which the hardware
 * mirrors from the viewport. We supply those two constants and undo the
 * transform to get the clip-space position the renderer takes. */
typedef struct vsh_job {
    const pdo_vsh_program *p;
    uint32_t handle;
    uint32_t sbase[16], sstride[16], first;
    float scale[3], offset[3], zs;
    pdo_vertex *out;
} vsh_job;

/* Vertices [lo, hi) of a job: fetch the inputs, run the program, unpack. */
static void vsh_range(void *ctx, uint32_t lo, uint32_t hi)
{
    const vsh_job *j = ctx;
    const pdo_vsh_program *p = j->p;
    float in[PDO_VSH_INPUTS][4], o[VSH_OUTPUTS][4];

    for (int r = 0; r < PDO_VSH_INPUTS; r++) {
        in[r][0] = in[r][1] = in[r][2] = 0.0f;
        in[r][3] = 1.0f;
    }
    for (uint32_t i = lo; i < hi; i++) {
        pdo_vertex *v = &j->out[i];
        float cw;

        for (int r = 0; r < PDO_VSH_INPUTS; r++) {
            const pdo_vsh_input *in_r = &p->input[r];
            if (!(p->inputs_read & (1u << r)))
                continue;
            if (in_r->present && j->sbase[in_r->stream]) {
                uint32_t va = j->sbase[in_r->stream] + (j->first + i) * j->sstride[in_r->stream] + in_r->offset;
                pdo_vsh_fetch(in_r->type, (const uint8_t *)XBOX_PTR(va), in[r]);
            } else {
                in[r][0] = in[r][1] = in[r][2] = 0.0f;
                in[r][3] = 1.0f;
            }
        }
        pdo_vsh_run(p, (const float (*)[4])in, vconst, o);

        cw = o[VSH_OPOS][3];
        {   /* PDO_VSH_BAD=n: for the first n vertices that land far outside
             * the screen, log the inputs and re-run with an instruction trace. */
            static int budget = -1;
            static SDL_AtomicInt left;
            float x = (o[VSH_OPOS][0] - j->offset[0]) / j->scale[0];
            if (budget < 0) {
                budget = getenv("PDO_VSH_BAD") ? atoi(getenv("PDO_VSH_BAD")) : 0;
                SDL_SetAtomicInt(&left, budget);
            }
            if (budget > 0 && (fabsf(x) > 50.0f || !(cw == cw)) && SDL_AddAtomicInt(&left, -1) > 0) {
                static SDL_Mutex *mx;
                float o2[VSH_OUTPUTS][4];
                if (!mx) mx = SDL_CreateMutex();
                SDL_LockMutex(mx);
                SDL_Log("vsh bad: program 0x%08x vertex %u (%u insns) oPos %g %g %g %g frame %llu", j->handle,
                        j->first + i, p->count, o[VSH_OPOS][0], o[VSH_OPOS][1], o[VSH_OPOS][2], o[VSH_OPOS][3],
                        (unsigned long long)pdo_gfx_frame_count());
                for (int r = 0; r < PDO_VSH_INPUTS; r++)
                    if (p->inputs_read & (1u << r))
                        SDL_Log("vsh bad:   v%d type 0x%02x stream %u +%u = %g %g %g %g", r, p->input[r].type,
                                p->input[r].stream, p->input[r].offset, in[r][0], in[r][1], in[r][2], in[r][3]);
                pdo_vsh_trace = true;
                pdo_vsh_run(p, (const float (*)[4])in, vconst, o2);
                pdo_vsh_trace = false;
                SDL_UnlockMutex(mx);
            }
        }
        v->pos[0] = (o[VSH_OPOS][0] - j->offset[0]) / j->scale[0] * cw;
        v->pos[1] = (o[VSH_OPOS][1] - j->offset[1]) / j->scale[1] * cw;
        v->pos[2] = (o[VSH_OPOS][2] - j->offset[2]) / j->zs * cw;
        v->pos[3] = cw;
        v->diffuse = pack_color(o[VSH_OD0]);
        v->specular = pack_color(o[VSH_OD1]);
        v->fog = o[VSH_OFOG][0];
        for (int t = 0; t < 4; t++)
            memcpy(v->tc[t], o[VSH_OT0 + t], sizeof v->tc[t]);
    }
}

/* Run the current vertex program over vertices [first, first + count).
 * base/stride describe stream 0 (a user-pointer draw passes its own); the
 * other streams come from SetStreamSource. Vertices are independent, so a
 * large draw is spread over worker threads, unless the program writes the
 * constant file (that persists from vertex to vertex, so order matters).
 *
 * The XDK ends every program with its viewport transform: oPos.xyz =
 * clip.xyz / w * c[58] + c[59] (D3D's c[-38]/c[-37]), which the hardware
 * mirrors from the viewport. We supply those two constants and undo the
 * transform to get the clip-space position the renderer takes. */
static bool fetch_vsh(const pdo_vsh_program *p, uint32_t base, uint32_t stride,
                      uint32_t first, uint32_t count, pdo_vertex *out)
{
    vsh_job j;
    uint64_t t0;
    float x = 0, y = 0, w = 640, h = 480, zs = 16777215.0f, zo = 0;
    int rtw, rth;

    memcpy(vconst, XBOX_PTR(VSH_SHADOW), sizeof vconst);
    target_size(&rtw, &rth);
    if (st.have_viewport) {
        x = (float)st.vp_x; y = (float)st.vp_y; w = (float)st.vp_w; h = (float)st.vp_h;
        zs = 16777215.0f * (st.vp_maxz - st.vp_minz);
        zo = 16777215.0f * st.vp_minz;
    } else {
        w = (float)rtw; h = (float)rth;
    }
    j.p = p;
    j.handle = st.vshader;
    j.first = first;
    j.out = out;
    j.scale[0] = w * 0.5f; j.scale[1] = -h * 0.5f; j.scale[2] = zs;
    j.offset[0] = x + w * 0.5f + 0.53125f; j.offset[1] = y + h * 0.5f + 0.53125f; j.offset[2] = zo;
    for (int k = 0; k < 3; k++) {
        vconst[58][k] = j.scale[k];
        vconst[59][k] = j.offset[k];
    }
    j.zs = zs != 0.0f ? zs : 1.0f;
    for (int s = 0; s < 16; s++) {
        j.sbase[s] = st.vbs[s] ? MEM32(st.vbs[s] + 4) | 0x80000000u : 0;
        j.sstride[s] = st.strides[s];
    }
    j.sbase[0] = base;
    j.sstride[0] = stride;

    t0 = SDL_GetPerformanceCounter();
    if (p->writes_constants)
        vsh_range(&j, 0, count);
    else
        pdo_vsh_parallel(count, vsh_range, &j);

    /* Once a second: how many vertices the programs ran on, and the time. */
    {
        static uint64_t ticks, verts, since;
        uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
        ticks += now - t0;
        verts += count;
        if (!since)
            since = now;
        if (now - since >= f) {
            SDL_Log("vsh: %llu vertices/s, %.1f ms/s in vertex programs",
                    (unsigned long long)(verts * f / (now - since)),
                    1000.0 * (double)ticks / (double)(now - since));
            ticks = verts = 0;
            since = now;
        }
    }
    return true;
}

/* Turn a D3DPRIMITIVETYPE run of n vertices into list indices. Returns the
 * topology (0 triangles, 1 lines, 2 points) and the index count. */
static uint32_t build_indices(uint32_t type, const uint32_t *in, uint32_t n, uint32_t *out, uint8_t *topology)
{
    uint32_t k = 0;
    *topology = 0;
    switch (type) {
    case 1: *topology = 2; for (uint32_t i = 0; i < n; i++) out[k++] = in[i]; break;
    case 2: *topology = 1; for (uint32_t i = 0; i + 1 < n; i += 2) { out[k++] = in[i]; out[k++] = in[i + 1]; } break;
    case 3: case 4:
        *topology = 1;
        for (uint32_t i = 0; i + 1 < n; i++) { out[k++] = in[i]; out[k++] = in[i + 1]; }
        if (type == 3 && n > 2) { out[k++] = in[n - 1]; out[k++] = in[0]; }
        break;
    case 5: for (uint32_t i = 0; i + 2 < n; i += 3) { out[k++] = in[i]; out[k++] = in[i + 1]; out[k++] = in[i + 2]; } break;
    case 6:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) { out[k++] = in[i + 1]; out[k++] = in[i]; }
            else       { out[k++] = in[i];     out[k++] = in[i + 1]; }
            out[k++] = in[i + 2];
        }
        break;
    case 7: case 10:
        for (uint32_t i = 1; i + 1 < n; i++) { out[k++] = in[0]; out[k++] = in[i]; out[k++] = in[i + 1]; }
        break;
    case 8:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            out[k++] = in[i]; out[k++] = in[i + 1]; out[k++] = in[i + 2];
            out[k++] = in[i]; out[k++] = in[i + 2]; out[k++] = in[i + 3];
        }
        break;
    case 9:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            out[k++] = in[i];     out[k++] = in[i + 1]; out[k++] = in[i + 2];
            out[k++] = in[i + 1]; out[k++] = in[i + 3]; out[k++] = in[i + 2];
        }
        break;
    default:
        break;
    }
    return k;
}

/* The shared part of every draw: vertices [first, first+count) of stream
 * data at 'base', primitive order given by 'order' (n entries, relative to
 * first). */
static void submit(uint32_t type, uint32_t base, uint32_t stride, uint32_t first, uint32_t count,
                   const uint32_t *order, uint32_t n)
{
    static pdo_vertex *vbuf;
    static uint32_t *ibuf, vcap, icap;
    pdo_draw d;
    bool pre;
    uint32_t ni;

    state_init();
    if (count > vcap) {
        vcap = count * 2;
        vbuf = SDL_realloc(vbuf, vcap * sizeof *vbuf);
    }
    if (n * 6 > icap) {
        icap = n * 6;
        ibuf = SDL_realloc(ibuf, icap * sizeof *ibuf);
    }
    if (st.vshader & 1) {
        const pdo_vsh_program *p = find_program(st.vshader);
        if (!p) {
            static int warned;
            if (!warned++)
                SDL_Log("d3d: vertex shader 0x%08x was never created through CreateVertexShader", st.vshader);
            SDL_AddAtomicInt(&draws_dropped, 1);
            return;
        }
        fetch_vsh(p, base, stride, first, count, vbuf);
        pre = false;
        {   /* How many program draws also have a pixel shader (not drawn yet). */
            static uint32_t draws, with_ps;
            static uint64_t last;
            draws++;
            with_ps += st.pshader != 0;
            if (pdo_gfx_frame_count() >= last + 300) {
                SDL_Log("d3d: %u vertex-program draws, %u with a pixel shader", draws, with_ps);
                draws = with_ps = 0;
                last = pdo_gfx_frame_count();
            }
        }
    } else if (!fetch_fvf(st.vshader, base, stride, first, count, vbuf, &pre)) {
        static int warned;
        if (!warned++)
            SDL_Log("d3d: FVF 0x%x not handled", st.vshader);
        SDL_AddAtomicInt(&draws_dropped, 1);
        return;
    }
    fill_draw_state(&d, pre);
    {   /* PDO_CAPTURE=frame: the render-state table around fog, once. */
        static long long cap = -2;
        if (cap == -2)
            cap = getenv("PDO_CAPTURE") ? atoll(getenv("PDO_CAPTURE")) : -1;
        if (cap >= 0 && pdo_gfx_frame_count() == (uint64_t)cap) {
            cap = -1;
            for (int r = 76; r < 124; r += 8)
                SDL_Log("capture: RS[%d..] %08x %08x %08x %08x %08x %08x %08x %08x", r, RS(r), RS(r + 1),
                        RS(r + 2), RS(r + 3), RS(r + 4), RS(r + 5), RS(r + 6), RS(r + 7));
        }
    }
    {   /* PDO_PS_DEBUG=frame: describe the pixel-shader draws of that frame. */
        static long dbg = -2;
        if (dbg == -2) {
            const char *e = getenv("PDO_PS_DEBUG");
            dbg = e ? atol(e) : -1;
        }
        if (dbg >= 0 && (long)pdo_gfx_frame_count() == dbg && st.pshader) {
            SDL_Log("ps draw: ps 0x%08x vs 0x%08x %u verts; v0 diffuse %08x spec %08x",
                    st.pshader, st.vshader, count, vbuf[0].diffuse, vbuf[0].specular);
            for (int s = 0; s < 4; s++) {
                pdo_xtex_info ti;
                pdo_xtex_describe(d.tex[s].format_word, d.tex[s].size_word, &ti);
                if (d.tex[s].data && ti.known && ti.width <= 1024 && ti.height <= 1024) {
                    /* Save the decoded texture as ps_tex_<address>.bmp. */
                    uint8_t *rgba = SDL_malloc((size_t)ti.width * ti.height * 4);
                    if (rgba) {
                        char name[64];
                        SDL_Surface *sf;
                        pdo_xtex_decode(&ti, (const uint8_t *)XBOX_PTR(d.tex[s].data | 0x80000000u), rgba);
                        sf = SDL_CreateSurfaceFrom((int)ti.width, (int)ti.height, SDL_PIXELFORMAT_RGBA32,
                                                   rgba, (int)ti.width * 4);
                        snprintf(name, sizeof name, "ps_tex_%08x.bmp", d.tex[s].data);
                        if (sf) { SDL_SaveBMP(sf, name); SDL_DestroySurface(sf); }
                        SDL_free(rgba);
                    }
                }
                SDL_Log("  T%d: data %08x %s %ux%u pitch %u scale %g,%g tc %g,%g,%g,%g "
                        "c0 %.2f,%.2f,%.2f,%.2f c1 %.2f,%.2f,%.2f,%.2f",
                        s, d.tex[s].data, d.tex[s].data ? pdo_xtex_format_name(ti.format) : "-",
                        ti.width, ti.height, ti.pitch, d.ff.tc_scale[s][0], d.ff.tc_scale[s][1],
                        vbuf[0].tc[s][0], vbuf[0].tc[s][1], vbuf[0].tc[s][2], vbuf[0].tc[s][3],
                        d.ff.ps_c0[s][0], d.ff.ps_c0[s][1], d.ff.ps_c0[s][2], d.ff.ps_c0[s][3],
                        d.ff.ps_c1[s][0], d.ff.ps_c1[s][1], d.ff.ps_c1[s][2], d.ff.ps_c1[s][3]);
            }
        }
    }
    ni = build_indices(type, order, n, ibuf, &d.ps.topology);
    pdo_gfx_draw(&d, vbuf, count, ibuf, ni);
    SDL_AddAtomicInt(&draws_drawn, 1);
}

static uint32_t *sequential(uint32_t n)
{
    static uint32_t *seq, cap;
    if (n > cap) {
        cap = n * 2;
        seq = SDL_realloc(seq, cap * sizeof *seq);
        for (uint32_t i = 0; i < cap; i++)
            seq[i] = i;
    }
    return seq;
}

static uint32_t vb_data(void)
{
    /* CPU view of a resource: Data | 0x80000000, as D3D itself computes it. */
    return st.vb ? MEM32(st.vb + 4) | 0x80000000u : 0;
}

/* D3DDevice_DrawVertices(PrimitiveType, StartVertex, VertexCount) */
void sub_002AF2B0(void)
{
    SDL_AddAtomicInt(&draws_called, 1);
    uint32_t type = MEM32(g_esp + 4), start = MEM32(g_esp + 8), n = MEM32(g_esp + 12);
    if (vb_data() && n)
        submit(type, vb_data(), st.stride, start, n, sequential(n), n);
    g_esp += 4 + 12;
}

/* D3DDevice_DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData):
 * 16-bit indices, offset by SetIndices' BaseVertexIndex. */
void sub_002AF350(void)
{
    SDL_AddAtomicInt(&draws_called, 1);
    uint32_t type = MEM32(g_esp + 4), n = MEM32(g_esp + 8), pidx = MEM32(g_esp + 12);
    static uint32_t *order, cap;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;

    if (vb_data() && n && pidx) {
        if (n > cap) {
            cap = n * 2;
            order = SDL_realloc(order, cap * sizeof *order);
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = MEM16(pidx + 2 * i) + st.base_vertex;
            order[i] = v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        for (uint32_t i = 0; i < n; i++)
            order[i] -= lo;
        submit(type, vb_data(), st.stride, lo, hi - lo + 1, order, n);
    }
    g_esp += 4 + 12;
}

/* D3DDevice_DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData,
 * VertexStreamZeroStride) */
void sub_002AF010(void)
{
    SDL_AddAtomicInt(&draws_called, 1);
    uint32_t type = MEM32(g_esp + 4), n = MEM32(g_esp + 8), data = MEM32(g_esp + 12), stride = MEM32(g_esp + 16);
    if (data && n)
        submit(type, data, stride, 0, n, sequential(n), n);
    g_esp += 4 + 16;
}

/* The game's renderer object draws most of its geometry without D3D's draw
 * calls: Begin (0x00279D30) reserves push-buffer space with BeginPush and
 * writes NV2A SET_BEGIN_END(type), each GetSpace (0x00279E10) puts an
 * INLINE_ARRAY header (method 0x1818) before room for N vertices that the
 * caller fills, and End (0x00279E50) writes SET_BEGIN_END(0) and calls
 * EndPush. The cursor is at 0x004EB1EC; the vertex stride is the object's
 * +0x6C. Nothing executes our push buffer, so End draws what was written:
 * the vertex runs, packed together over their headers, through the same path
 * as DrawVerticesUP with the vertex format the game set. */
#define INLINE_CURSOR 0x004EB1ECu

static uint32_t inline_type, inline_start, inline_stride;

extern void sub_00279D30_gen(void);
void sub_00279D30(void)
{
    uint32_t self = MEM32(g_esp + 4), type = MEM32(g_esp + 8);

    sub_00279D30_gen();
    inline_start = g_eax == 0 ? MEM32(INLINE_CURSOR) : 0;
    inline_type = type;
    inline_stride = MEM32(self + 0x6C);
}

extern void sub_00279E50_gen(void);
void sub_00279E50(void)
{
    uint32_t p = inline_start, end = MEM32(INLINE_CURSOR), out = inline_start;

    SDL_AddAtomicInt(&draws_called, 1);
    while (p && p < end) {
        uint32_t head = MEM32(p), n = (head >> 18) & 0x7FF;
        if ((head & 0xE003FFFFu) != 0x40001818u || p + 4 + n * 4 > end) {
            SDL_Log("d3d: inline vertices: unexpected word 0x%08X", head);
            out = inline_start;
            break;
        }
        SDL_memmove((void *)XBOX_PTR(out), (void *)XBOX_PTR(p + 4), n * 4);
        out += n * 4;
        p += 4 + n * 4;
    }
    if (inline_stride && out > inline_start) {
        uint32_t n = (out - inline_start) / inline_stride;
        submit(inline_type, inline_start, inline_stride, 0, n, sequential(n), n);
    }
    inline_start = 0;
    sub_00279E50_gen();
}

/* D3DDevice_DrawIndexedVerticesUP(PrimitiveType, VertexCount, pIndexData,
 * pVertexStreamZeroData, VertexStreamZeroStride) */
void sub_002AF170(void)
{
    SDL_AddAtomicInt(&draws_called, 1);
    uint32_t type = MEM32(g_esp + 4), n = MEM32(g_esp + 8), pidx = MEM32(g_esp + 12);
    uint32_t data = MEM32(g_esp + 16), stride = MEM32(g_esp + 20);
    static uint32_t *order, cap;
    uint32_t hi = 0;

    if (data && n && pidx) {
        if (n > cap) {
            cap = n * 2;
            order = SDL_realloc(order, cap * sizeof *order);
        }
        for (uint32_t i = 0; i < n; i++) {
            order[i] = MEM16(pidx + 2 * i);
            if (order[i] > hi) hi = order[i];
        }
        submit(type, data, stride, 0, hi + 1, order, n);
    }
    g_esp += 4 + 20;
}

/* D3DDevice_SetGammaRamp(Flags, pRamp): BYTE red[256], green[256],
 * blue[256]. JSRF faded its logos with it. */
extern void sub_002AD9E0_gen(void);
void sub_002AD9E0(void)
{
    uint8_t ramp[768];
    WRAP_BEGIN(2);
    if (a[1]) {
        for (uint32_t i = 0; i < 768; i++)
            ramp[i] = MEM8(a[1] + i);
        pdo_gfx_set_gamma(ramp);
    }
    sub_002AD9E0_gen();
}

/* ── Immediate mode: Begin / SetVertexData* / End ────────────────────────
 * JSRF drew its fades and message boxes this way. Attributes are latched
 * per register (D3DVSDE_*); writing the position emits a vertex with the
 * latched attributes, as on the hardware. Positions are interpreted by the
 * current FVF: screen space (XYZRHW) or transformed by the matrices. */

enum { VDE_POSITION = 0, VDE_DIFFUSE = 3, VDE_SPECULAR = 4, VDE_TEXCOORD0 = 9 };

static struct {
    bool       active;
    uint32_t   type;
    float      diffuse[4], specular[4], tc[4][4];
    pdo_vertex *v;
    uint32_t   n, cap;
} imm;

static void imm_emit(float x, float y, float z, float w)
{
    pdo_vertex *o;
    int rtw, rth;

    if (imm.n == imm.cap) {
        imm.cap = imm.cap ? imm.cap * 2 : 64;
        imm.v = SDL_realloc(imm.v, imm.cap * sizeof *imm.v);
    }
    o = &imm.v[imm.n++];
    memset(o, 0, sizeof *o);
    target_size(&rtw, &rth);
    if ((st.vshader & 0x00E) == 0x004) {
        float iw = w != 0.0f ? 1.0f / w : 1.0f;     /* w carries rhw */
        o->pos[0] = (x / (float)rtw * 2.0f - 1.0f) * iw;
        o->pos[1] = (1.0f - y / (float)rth * 2.0f) * iw;
        o->pos[2] = z * iw;
        o->pos[3] = iw;
    } else {
        float wv[16], wvp[16];
        mat_mul(st.mat[TS_WORLD], st.mat[TS_VIEW], wv);
        mat_mul(wv, st.mat[TS_PROJECTION], wvp);
        for (int c = 0; c < 4; c++)
            o->pos[c] = x * wvp[c] + y * wvp[4 + c] + z * wvp[8 + c] + w * wvp[12 + c];
    }
    o->diffuse = pack_color(imm.diffuse);
    o->specular = pack_color(imm.specular);
    o->fog = 1.0f;
    for (int s = 0; s < 4; s++) {
        uint32_t set = TS(s, TSS_TEXCOORDINDEX) & 0xFFFF;
        if (set < 4)
            memcpy(o->tc[s], imm.tc[set], sizeof o->tc[s]);
    }
}

static void imm_set(uint32_t reg, float a, float b, float c, float d)
{
    float v[4] = { a, b, c, d };

    if (reg == VDE_POSITION || reg == 0xFFFFFFFFu)
        imm_emit(a, b, c, d);
    else if (reg == VDE_DIFFUSE)
        memcpy(imm.diffuse, v, sizeof v);
    else if (reg == VDE_SPECULAR)
        memcpy(imm.specular, v, sizeof v);
    else if (reg >= VDE_TEXCOORD0 && reg < VDE_TEXCOORD0 + 4)
        memcpy(imm.tc[reg - VDE_TEXCOORD0], v, sizeof v);
}

/* D3DDevice_Begin(PrimitiveType) */
void sub_002AF660(void)
{
    state_init();
    imm.active = true;
    imm.type = MEM32(g_esp + 4);
    imm.n = 0;
    imm.diffuse[0] = imm.diffuse[1] = imm.diffuse[2] = imm.diffuse[3] = 1.0f;
    memset(imm.specular, 0, sizeof imm.specular);
    memset(imm.tc, 0, sizeof imm.tc);
    g_esp += 4 + 4;
}

/* D3DDevice_SetVertexData2f(Register, a, b) */
void sub_002AF5C0(void)
{
    imm_set(MEM32(g_esp + 4), f32(g_esp + 8), f32(g_esp + 12), 0.0f, 1.0f);
    g_esp += 4 + 12;
}

/* D3DDevice_SetVertexData4f(Register, a, b, c, d) */
void sub_002AF600(void)
{
    imm_set(MEM32(g_esp + 4), f32(g_esp + 8), f32(g_esp + 12), f32(g_esp + 16), f32(g_esp + 20));
    g_esp += 4 + 20;
}

/* D3DDevice_End() */
void sub_002AF6A0(void)
{
    SDL_AddAtomicInt(&draws_called, 1);
    if (imm.active && imm.n) {
        static uint32_t *ibuf, icap;
        pdo_draw d;
        if (imm.n * 6 > icap) {
            icap = imm.n * 6;
            ibuf = SDL_realloc(ibuf, icap * sizeof *ibuf);
        }
        fill_draw_state(&d, (st.vshader & 0x00E) == 0x004);
        pdo_gfx_draw(&d, imm.v, imm.n,
                     ibuf, build_indices(imm.type, sequential(imm.n), imm.n, ibuf, &d.ps.topology));
    }
    imm.active = false;
    g_esp += 4;
}
