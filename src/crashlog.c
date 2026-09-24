/*
 * How the process ended, in the log. The game dies without a word under
 * Proton, and a crash with no address costs a session of guessing.
 *
 * Every way out leaves a line:
 *
 * - Faults. A vectored handler sees every exception first. Only faults are
 *   logged (access violation, illegal instruction, divide by zero, stack
 *   overflow), not those inside kernel32/kernelbase/ntdll (IsBadReadPtr
 *   probes by faulting there on purpose). The engine catches some, so they
 *   are "first-chance": each distinct address once, up to 32 addresses
 *   (the first 8 of the session used to be all, and harmless early ones
 *   could use them up). The fatal one is logged again by our unhandled-
 *   exception filter, which stays first even when the game installs its
 *   own (SetUnhandledExceptionFilter is hooked; theirs runs after ours).
 * - Exits. ExitProcess and ntdll's NtTerminateProcess are hooked: whoever
 *   ends the process (the game quitting, the C runtime's abort or out of
 *   memory, Wine) is logged once with the caller's stack and a memory line.
 *   "exit:" is the marker src/hook.c looks for at the next start: a log
 *   without it ended in a native crash, a kill or a hang, and is kept.
 * Each fault line: code, faulting address as module+offset, the data
 * address for an access violation, and the return addresses up the EBP
 * chain.
 */
#include <windows.h>
#include <string.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

void hg_mem_report(void);

#define MAX_SITES 32
static void *g_sites[MAX_SITES];
static volatile LONG g_nsites, g_busy, g_exiting;

static void where(void *addr, char *out, int n)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    const char *base;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) || !m || !GetModuleFileNameA(m, path, sizeof path)) {
        wsprintfA(out, "%p", addr);
        return;
    }
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if ((int)strlen(base) > n - 16) base += strlen(base) - (n - 16);
    wsprintfA(out, "%s+%lx", base, (unsigned long)((char *)addr - (char *)m));
}

static int system_module(void *addr)
{
    char s[64];
    where(addr, s, sizeof s);
    CharLowerA(s);
    return !strncmp(s, "kernel32.dll+", 13) || !strncmp(s, "kernelbase.dll+", 15) || !strncmp(s, "ntdll.dll+", 10);
}

/* " a b c" up the EBP chain from fp, at most 12 frames */
static int chain(DWORD *fp, char *out, int cap)
{
    int len = 0, i;
    for (i = 0; i < 12 && fp && !IsBadReadPtr(fp, 8); i++) {
        char r[64];
        where((void *)fp[1], r, sizeof r);
        if (len > cap - 72) break;
        len += wsprintfA(out + len, " %s", r);
        if ((DWORD *)fp[0] <= fp) break;           /* the chain goes up the stack */
        fp = (DWORD *)fp[0];
    }
    out[len] = 0;
    return len;
}

static void report(const char *kind, EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char at[64], line[640];
    int len;
    where(er->ExceptionAddress, at, sizeof at);
    len = wsprintfA(line, "crash: %s exception %08lx at %s (thread %lu)", kind, er->ExceptionCode, at,
                    GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        len += wsprintfA(line + len, ", %s %08lx", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                         (unsigned long)er->ExceptionInformation[1]);
    len += wsprintfA(line + len, "; eip %08lx esp %08lx; stack:", ep->ContextRecord->Eip, ep->ContextRecord->Esp);
    chain((DWORD *)ep->ContextRecord->Ebp, line + len, (int)sizeof line - len);
    hg_log("%s", line);
}

static int fault(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
           code == EXCEPTION_PRIV_INSTRUCTION;
}

/* 1 the first time this faulting address is seen, while there is room */
static int new_site(void *a)
{
    LONG i, n = g_nsites;
    for (i = 0; i < n && i < MAX_SITES; i++)
        if (g_sites[i] == a) return 0;
    if (n >= MAX_SITES) return 0;
    g_sites[n] = a;
    InterlockedIncrement(&g_nsites);
    return 1;
}

static LONG CALLBACK vectored(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (!fault(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_busy, 1)) return EXCEPTION_CONTINUE_SEARCH;   /* a fault while reporting */
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        hg_log("crash: first-chance stack overflow at %p (thread %lu)", er->ExceptionAddress, GetCurrentThreadId());
    else if (!system_module(er->ExceptionAddress) && new_site(er->ExceptionAddress))
        report("first-chance", ep);
    InterlockedExchange(&g_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- the unhandled-exception filter, kept first ---- */

typedef LPTOP_LEVEL_EXCEPTION_FILTER (WINAPI *setuef_fn)(LPTOP_LEVEL_EXCEPTION_FILTER);
static setuef_fn o_setuef;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev;     /* the game's (or the runtime's) filter, run after ours */

static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep)
{
    if (!InterlockedExchange(&g_busy, 1)) {
        report("FATAL", ep);
        hg_mem_report();
    }
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI d_setuef(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old = g_prev;
    if (f == unhandled) return old;
    g_prev = f;                                 /* ours stays installed and calls theirs */
    hg_log("crash: the game set its own unhandled-exception filter (%p); ours runs first", (void *)f);
    return old;
}

/* ---- exits ---- */

typedef VOID (WINAPI *exitproc_fn)(UINT);
typedef LONG (NTAPI *ntterm_fn)(HANDLE, LONG);
static exitproc_fn o_exitproc;
static ntterm_fn o_ntterm;

static void exit_line(const char *how, unsigned long code, DWORD *fp)
{
    char st[512];
    if (InterlockedExchange(&g_exiting, 1)) return;
    chain(fp, st, sizeof st);
    hg_log("exit: %s(%lu) from thread %lu; stack:%s", how, code, GetCurrentThreadId(), st);
    hg_mem_report();
}

static VOID WINAPI d_exitproc(UINT code)
{
    exit_line("ExitProcess", code, (DWORD *)__builtin_frame_address(0));
    o_exitproc(code);
}

static LONG NTAPI d_ntterm(HANDLE h, LONG code)
{
    /* NULL ends the other threads first (ExitProcess does it); -1 or our
     * own handle ends the process. Another process's handle is not ours. */
    if (!h || h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId())
        exit_line("NtTerminateProcess", (unsigned long)code, (DWORD *)__builtin_frame_address(0));
    return o_ntterm(h, code);
}

static void hook(const WCHAR *mod, const char *fn, void *detour, void **orig)
{
    void *target = GetProcAddress(GetModuleHandleW(mod), fn);
    if (!target || MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
        hg_log("crash: could not hook %s", fn);
}

void crashlog_install(void)
{
    AddVectoredExceptionHandler(1, vectored);
    g_prev = SetUnhandledExceptionFilter(unhandled);
    hook(L"kernel32.dll", "SetUnhandledExceptionFilter", (void *)d_setuef, (void **)&o_setuef);
    hook(L"kernel32.dll", "ExitProcess", (void *)d_exitproc, (void **)&o_exitproc);
    hook(L"ntdll.dll", "NtTerminateProcess", (void *)d_ntterm, (void **)&o_ntterm);
    hg_log("crash: logging faults (each address once, %d at most), the fatal one, and the exit", MAX_SITES);
}
