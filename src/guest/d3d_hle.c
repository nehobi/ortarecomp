/*
 * Direct3D, replaced at its API (in progress).
 *
 * The title links the XDK's Direct3D library (4928), which drives the NV2A
 * GPU directly. Its entry points are replaced here one by one, from Concept
 * of Love's JSRF (4134) replacements. Each address below was checked against
 * Orta's own code, not taken from the match alone: same shape, same `ret N`.
 *
 * Conventions: __stdcall, arguments at g_esp + 4 + 4n, each function pops
 * exactly what the original's `ret N` did.
 */

#include "recomp_types.h"

#include "gfx/renderer.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define D3D_DEVICE_PTR      0x002BD708u     /* D3D__pDevice */
/* Device fields moved by 0x124 from 4134: JSRF's callback is at +0x242C. */
#define DEV_VBLANK_CALLBACK 0x2550u         /* set by SetVerticalBlankCallback (0x002ADC90) */

/* NTSC: 60000/1001 Hz. */
#define VBLANK_NS 16683333ull

extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static Uint64 epoch;              /* time of vblank 0 */
static SDL_AtomicInt last_called;  /* last vblank whose callback has run */
static uint32_t vblank_data;       /* guest D3DVBLANKDATA */
static int swap_interval = 1;      /* vblanks per Swap; 0 = immediate */

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

/* Sleep until the next vblank on the shared timeline; returns its index.
 * Vblanks fall on one fixed timeline and every waiter wakes at the next one,
 * as on hardware. */
static Uint64 wait_vblank(void)
{
    Uint64 now = SDL_GetTicksNS(), index, at;

    if (!epoch)
        epoch = now;
    index = (now - epoch) / VBLANK_NS + 1;
    at = epoch + index * VBLANK_NS;
    if (at > now)
        SDL_DelayPrecise(at - now);
    return index;
}

static Uint64 current_vblank(void)
{
    return epoch ? (SDL_GetTicksNS() - epoch) / VBLANK_NS : 0;
}

/* D3DDevice_BlockUntilVerticalBlank(): no arguments.
 *
 * On the console this waits for the GPU's vblank interrupt, whose handler
 * also runs the title's vertical-blank callback. So this is the frame clock.
 * The interrupt runs the callback once per vblank, whoever is waiting: the
 * first waiter to reach a new vblank runs it. */
void sub_002ADCB0(void)
{
    Uint64 index = wait_vblank();
    uint32_t dev, cb;
    int prev;

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
}

/* D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil) */
void sub_002B53F0(void)
{
    uint32_t count = MEM32(g_esp + 4), flags = MEM32(g_esp + 12);
    uint32_t color = MEM32(g_esp + 16), zbits = MEM32(g_esp + 20), stencil = MEM32(g_esp + 24);
    float z;

    SDL_memcpy(&z, &zbits, sizeof z);
    if (count) {
        static int warned;
        if (!warned++)
            SDL_Log("d3d: Clear with %u rectangles: clearing the whole target for now", count);
    }
    pdo_gfx_clear(flags, color, z, stencil);
    g_eax = 0;
    g_esp += 4 + 24;
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
            SDL_Log("d3d: %.2f fps (%.2f vblanks per frame), frame %llu", fps,
                    (double)(current_vblank() - v0) / (double)(n - f0), (unsigned long long)n);
            t0 = now, f0 = n, v0 = current_vblank();
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
