/*
 * Hand-written hooks the generated code calls into: manual function
 * overrides and diagnostics for indirect calls and untranslated instructions.
 *
 * Adapted from xboxrecomp's templates/new-game/src/recomp_manual.c
 * (MIT, Copyright (c) 2026 sp00nz; see licenses/xboxrecomp-MIT.txt).
 */

#include "recomp_types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* Return a replacement for the guest function at xbox_va, or NULL to use the
 * generated one. Called on every dispatched call, so keep it cheap. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);    {   /* The first few in full: who called, with what. The call has pushed
         * its guest return address, so the stack names the caller and the
         * code addresses above it its callers. */
        static int detailed;
        if (detailed++ < 4) {
            int shown = 0;
            fprintf(stderr, "  returning to 0x%08X; eax %08X ebx %08X ecx %08X edx %08X esi %08X "
                            "edi %08X ebp %08X esp %08X\n  guest stack, code addresses:",
                    MEM32(g_esp), g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp);
            for (uint32_t k = 0; k < 256 && shown < 12; k++) {
                uint32_t v = MEM32(g_esp + 4 * k);
                if (v >= 0x00011000u && v < 0x002AC080u) {
                    fprintf(stderr, " %08X", v);
                    shown++;
                }
            }
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    /* The call pushed its guest return address before the check: that
     * names the caller, which the target alone (usually 0) cannot. */
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu, returning to 0x%08X)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count, MEM32(g_esp));
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── The idle thread ─────────────────────────────────────────────────── */

/*
 * CRI's spin thread (see cri_hle.c: LOCK resumes it, UNLOCK suspends it):
 * it counts at 0x00485B28 in a loop until 0x00485B40 is set, then ends its
 * thread. The loop makes no kernel call: on the console the scheduler
 * preempts it; under the
 * guest CPU lock (guest.c turns it on) it would hold the lock forever and
 * no other guest thread would run again. Same loop, giving the lock up each
 * time round. Nothing reads the count.
 */
#include <SDL3/SDL.h>

extern void sub_00233CB0_gen(void);
int guest_cpu_yield_begin(void);
void guest_cpu_yield_end(int held);

void sub_00233CB0(void)
{
    while (!MEM32(0x485B40)) {
        int held;
        MEM32(0x485B28) += 1;
        held = guest_cpu_yield_begin();
        SDL_Delay(1);
        guest_cpu_yield_end(held);
    }
    sub_00233CB0_gen();         /* sees the flag: ends the thread */
}
