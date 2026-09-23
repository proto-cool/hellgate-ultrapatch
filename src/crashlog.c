/*
 * Where a crash happened, in the log. The game dies without a word under
 * Proton, and a crash with no address costs a session of guessing.
 *
 * A vectored handler sees every exception first. Only faults are logged
 * (access violation, illegal instruction, divide by zero, stack overflow),
 * and not those inside kernel32/kernelbase/ntdll (IsBadReadPtr probes by
 * faulting there on purpose); the engine may catch some of the rest, so
 * they are "first-chance", at most 8 of them. The fatal one is logged again
 * by the unhandled-exception filter, when the game has not replaced it.
 * Each line: code, faulting address as module+offset, the data address for
 * an access violation, and the return addresses up the EBP chain.
 */
#include <windows.h>
#include <string.h>
#include "panel.h"

static volatile LONG g_logged, g_busy;

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

static void report(const char *kind, EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char at[64], line[640];
    int len, i;
    DWORD *fp = (DWORD *)ep->ContextRecord->Ebp;
    where(er->ExceptionAddress, at, sizeof at);
    len = wsprintfA(line, "crash: %s exception %08lx at %s (thread %lu)", kind, er->ExceptionCode, at,
                    GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        len += wsprintfA(line + len, ", %s %08lx", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                         (unsigned long)er->ExceptionInformation[1]);
    len += wsprintfA(line + len, "; eip %08lx esp %08lx; stack:", ep->ContextRecord->Eip, ep->ContextRecord->Esp);
    for (i = 0; i < 12 && fp && !IsBadReadPtr(fp, 8); i++) {
        char r[64];
        where((void *)fp[1], r, sizeof r);
        if (len > (int)sizeof line - 72) break;
        len += wsprintfA(line + len, " %s", r);
        if ((DWORD *)fp[0] <= fp) break;           /* the chain goes up the stack */
        fp = (DWORD *)fp[0];
    }
    hg_log("%s", line);
}

static int fault(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
           code == EXCEPTION_PRIV_INSTRUCTION;
}

static LONG CALLBACK vectored(EXCEPTION_POINTERS *ep)
{
    if (!fault(ep->ExceptionRecord->ExceptionCode) || g_logged >= 8) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_busy, 1)) return EXCEPTION_CONTINUE_SEARCH;   /* a fault while reporting */
    if (!system_module(ep->ExceptionRecord->ExceptionAddress)) {
        InterlockedIncrement(&g_logged);
        report("first-chance", ep);
    }
    InterlockedExchange(&g_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev;
static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep)
{
    if (!InterlockedExchange(&g_busy, 1)) report("FATAL", ep);
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void crashlog_install(void)
{
    AddVectoredExceptionHandler(1, vectored);
    g_prev = SetUnhandledExceptionFilter(unhandled);
    hg_log("crash: logging faults (first 8 first-chance, and the fatal one)");
}
