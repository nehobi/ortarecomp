/*
 * Crash report (Windows): say where a hard crash happened.
 *
 * An access violation in game code otherwise ends the process with no word
 * in the log. This prints, to stderr (boot.log), the exception and the host
 * address with its function and source line from the build's own debug
 * information, the guest registers, and the game-code return addresses
 * still on the guest stack: those name the guest call chain, and each one
 * can be looked up in gen/ by address.
 *
 * Only on Windows. Linux crashes are read with gdb; there the kernel layer
 * also owns the SIGSEGV handler for its own memory traps, which this must
 * not replace.
 */

#ifdef _WIN32
/* Before the generated-code header, whose short macros would clash. */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

#include "recomp_types.h"

#include "xbox_memory_layout.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

static void print_host_address(const char *what, DWORD64 addr)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    IMAGEHLP_LINE64 line = { sizeof line };
    DWORD64 sym_off = 0;
    DWORD line_off = 0;

    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    fprintf(stderr, "  %s 0x%016llx", what, (unsigned long long)addr);
    if (SymFromAddr(GetCurrentProcess(), addr, &sym_off, sym))
        fprintf(stderr, "  %s+0x%llx", sym->Name, (unsigned long long)sym_off);
    if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &line_off, &line))
        fprintf(stderr, "  (%s:%lu)", line.FileName, line.LineNumber);
    fputc('\n', stderr);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    uint32_t sp = g_esp;   /* not "esp": recomp_types.h defines that */
    int shown = 0;

    fprintf(stderr, "\n=== CRASH: exception 0x%08lX", er->ExceptionCode);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, " (%s host 0x%llx, guest 0x%08llx)",
                er->ExceptionInformation[0] == 1 ? "write to" :
                er->ExceptionInformation[0] == 8 ? "execute at" : "read of",
                (unsigned long long)er->ExceptionInformation[1],
                (unsigned long long)(er->ExceptionInformation[1] - (ULONG_PTR)g_xbox_mem_offset));
    fprintf(stderr, " on thread %lu\n", GetCurrentThreadId());

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
#ifdef _M_X64
    print_host_address("at", c->Rip);
    {   /* A few host frames: the C caller chain inside generated code. */
        STACKFRAME64 f = {0};
        CONTEXT copy = *c;
        f.AddrPC.Offset = c->Rip;     f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = c->Rbp;  f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = c->Rsp;  f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 12 && StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                GetCurrentProcess(), GetCurrentThread(), &f, &copy, NULL,
                SymFunctionTableAccess64, SymGetModuleBase64, NULL); i++) {
            if (i == 0)
                continue;
            print_host_address("from", f.AddrPC.Offset);
        }
    }
#endif
    fprintf(stderr, "  guest eax %08X ebx %08X ecx %08X edx %08X\n"
                    "        esi %08X edi %08X esp %08X\n",
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, sp);

    /* Return addresses into game code still on the guest stack. Not every
     * code-range value there is one, but the call chain is among them. */
    fprintf(stderr, "  guest stack, code addresses:");
    if (sp >= 0x1000 && sp < XBOX_HEAP_TOP - 0x400) {
        for (uint32_t a = sp; a < sp + 0x400 && shown < 16; a += 4) {
            uint32_t v = MEM32(a);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, " %08X", v);
                shown++;
            }
        }
    }
    fprintf(stderr, shown ? "\n" : " (none)\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void pdo_crash_report_install(void)
{
    SetUnhandledExceptionFilter(crash_filter);
}

/* F11: every thread's host call stack, for hangs. Generated functions are
 * named after their guest address, so this shows where each game thread
 * waits. Each thread is suspended only while its stack is unwound with
 * RtlVirtualUnwind (no allocation, so a thread holding the heap lock cannot
 * deadlock us); symbols are looked up after it resumes. */
void pdo_dump_threads(void)
{
#ifdef _M_X64
    enum { MAX_THREADS = 64, MAX_FRAMES = 24 };
    static DWORD64 pcs[MAX_THREADS][MAX_FRAMES];
    static DWORD ids[MAX_THREADS];
    static int counts[MAX_THREADS];
    int nthreads = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    for (BOOL ok = Thread32First(snap, &te); ok && nthreads < MAX_THREADS; ok = Thread32Next(snap, &te)) {
        HANDLE th;
        CONTEXT ctx;
        int n = 0;

        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                        te.th32ThreadID);
        if (!th)
            continue;
        if (SuspendThread(th) != (DWORD)-1) {
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                while (n < MAX_FRAMES && ctx.Rip) {
                    DWORD64 base = 0, frame = 0;
                    PRUNTIME_FUNCTION rf;
                    PVOID handler = NULL;

                    pcs[nthreads][n++] = ctx.Rip;
                    rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if (rf) {
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler, &frame, NULL);
                    } else {                      /* leaf: return address on top */
                        ctx.Rip = *(DWORD64 *)ctx.Rsp;
                        ctx.Rsp += 8;
                    }
                }
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        ids[nthreads] = te.th32ThreadID;
        counts[nthreads++] = n;
    }
    CloseHandle(snap);

    fprintf(stderr, "\n=== THREADS (F11): %d\n", nthreads);
    for (int t = 0; t < nthreads; t++) {
        fprintf(stderr, "thread %lu\n", ids[t]);
        for (int i = 0; i < counts[t]; i++)
            print_host_address(i ? "from" : "at", pcs[t][i]);
    }
    fprintf(stderr, "=== end of threads\n");
    fflush(stderr);
#endif
}

#else

void pdo_crash_report_install(void)
{
}

void pdo_dump_threads(void)
{
    fprintf(stderr, "thread dump: Windows only; on Linux use gdb (thread apply all bt)\n");
}

#endif
