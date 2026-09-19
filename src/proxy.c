/*
 * version.dll proxy: forwards every real export to the system DLL via naked
 * jmp thunks, so we never need the real signatures.
 *
 * The game imports only GetFileVersionInfoW and VerQueryValueW, but we export
 * the full surface so the DLL is safe to drop next to any host.
 */
#include <windows.h>
#include "proxy.h"

#define PROXY_EXPORTS(X)            \
    X(0,  GetFileVersionInfoA)      \
    X(1,  GetFileVersionInfoByHandle) \
    X(2,  GetFileVersionInfoExA)    \
    X(3,  GetFileVersionInfoExW)    \
    X(4,  GetFileVersionInfoSizeA)  \
    X(5,  GetFileVersionInfoSizeExA)\
    X(6,  GetFileVersionInfoSizeExW)\
    X(7,  GetFileVersionInfoSizeW)  \
    X(8,  GetFileVersionInfoW)      \
    X(9,  VerFindFileA)             \
    X(10, VerFindFileW)             \
    X(11, VerInstallFileA)          \
    X(12, VerInstallFileW)          \
    X(13, VerLanguageNameA)         \
    X(14, VerLanguageNameW)         \
    X(15, VerQueryValueA)           \
    X(16, VerQueryValueW)

#define PROXY_COUNT 17

void *g_real[PROXY_COUNT];
static HMODULE g_realdll;

/* Resolved once from DllMain, before the host can call anything. */
int proxy_init(void)
{
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 16)
        return 0;
    lstrcatW(path, L"\\version.dll");
    g_realdll = LoadLibraryW(path);
    if (!g_realdll)
        return 0;
#define X(i, name) g_real[i] = (void *)GetProcAddress(g_realdll, #name);
    PROXY_EXPORTS(X)
#undef X
    /* The two the game actually imports must be present. */
    return g_real[8] && g_real[16];
}

/*
 * Naked jmp thunks. Exported under the real names via the .def file, so the
 * name decoration of these symbols does not matter.
 */
#define X(i, name)                                              \
    __attribute__((naked)) void thunk_##name(void)              \
    {                                                           \
        __asm__ __volatile__("jmp *%0" : : "m"(g_real[i]));     \
    }
PROXY_EXPORTS(X)
#undef X
