/*
 * Extensions to the game's own UI, through its own data paths
 * (docs/spikes/inventory-sort.md):
 *
 * - XML overrides. Every UI screen is data\uix\xml\<name>.xml, read from the
 *   archive and handed to a load-complete callback (FUN_0047381d, cdecl, the
 *   request record) that parses it on the main thread. If
 *   <game>\override\data\uix\xml\<name>.xml exists, the record points at it
 *   for that call: the buffer at req+0x4 and ctx+0x140 (ctx = req+0x28, whose
 *   +0x0 is the path), the size at req+0xc/+0x18/+0x38 and wherever else in
 *   ctx it is repeated. The game's own pointers go back afterwards, so it
 *   frees its own buffer; ours are kept for the session in case parsed
 *   components refer into them. tools/ui/mkuix.py builds the files.
 *   bin\hellgate_uix.off turns this off.
 * - Strings. The string-by-key lookup (FUN_00507a38, cdecl (key, 0, 0, 0),
 *   returns wide text) answers our own keys, "ultra ..."; any other key goes
 *   to the game.
 * - Buttons. Click handlers bind by name through a static {name, fn} table
 *   (0xb94750, 242 entries). New names cannot be added, so our buttons use
 *   the hidden security button's handler, UIinventorySecurityOnClk (entry
 *   227, 0x48d3cf), and the table entry is pointed at ours: it acts on our
 *   controls by their name (component+0) and passes everything else on.
 */
#include <windows.h>
#include <string.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

#define VA_UIDONE        0x0047381du
#define VA_STRING        0x00507a38u
#define VA_HANDLERS      0x00b94750u
#define HANDLER_SECURITY 227
#define VA_SECURITY      0x0048d3cfu

void invsort_click(void *comp);
int optpage_click(void *comp, const char *name, int msg, int wp, int lp, int *ret);
const WCHAR *optpage_string(const char *key);

typedef void (__cdecl *uidone_fn)(unsigned char *req);
typedef const WCHAR *(__cdecl *string_fn)(const char *key, int a, int b, int c);
typedef int (__cdecl *handler_fn)(void *comp, int msg, int wp, int lp);

static uidone_fn o_uidone;
static string_fn o_string;
static volatile LONG g_swaps;

/* ---- XML overrides ---- */

#define MAX_OVR 16
static struct { char path[128]; char *buf; DWORD size; } g_ovr[MAX_OVR];

/* The override for a pak path, read once and kept; NULL if there is none. */
static char *override_for(const char *pak, DWORD *size)
{
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD n, got = 0;
    char *buf;
    int i, len;
    for (i = 0; i < MAX_OVR && g_ovr[i].path[0]; i++)
        if (!lstrcmpiA(g_ovr[i].path, pak)) { *size = g_ovr[i].size; return g_ovr[i].buf; }
    if (i == MAX_OVR || lstrlenA(pak) >= (int)sizeof g_ovr[0].path) return NULL;
    hg_dll_dir(path, MAX_PATH);
    lstrcatW(path, L"\\..\\override\\");
    len = lstrlenW(path);
    MultiByteToWideChar(CP_ACP, 0, pak, -1, path + len, MAX_PATH);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(h, NULL);
    buf = (n && n != INVALID_FILE_SIZE) ? (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n + 2) : NULL;
    if (buf && !ReadFile(h, buf, n, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!buf || got != n) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        hg_log("uiext: override %ls unreadable", path);
        return NULL;
    }
    lstrcpyA(g_ovr[i].path, pak);
    g_ovr[i].buf = buf;
    g_ovr[i].size = n;
    *size = n;
    return buf;
}

static void __cdecl d_uidone(unsigned char *req)
{
    unsigned char *ctx;
    const char *pak;
    char *buf, *gbuf;
    DWORD size, gsize;
    DWORD *patched[48], saved[48];
    int np = 0, off;

    if (!req || IsBadReadPtr(req, 0x40) || hg_flagfile(L"hellgate_uix.off")) { o_uidone(req); return; }
    ctx = *(unsigned char **)(req + 0x28);
    if (!ctx || IsBadWritePtr(ctx, 0x180)) { o_uidone(req); return; }
    pak = *(const char **)ctx;
    gbuf = *(char **)(req + 0x4);
    gsize = *(DWORD *)(req + 0x38);
    if (!pak || IsBadStringPtrA(pak, 128) || !gbuf || !gsize ||
        !(buf = override_for(pak, &size))) { o_uidone(req); return; }

    /* every copy of the game's pointer and size, in the record and its context */
    for (off = 0; off < 0x40 && np < 48; off += 4) {
        DWORD *p = (DWORD *)(req + off);
        if (*p == (DWORD)(UINT_PTR)gbuf || *p == gsize) { patched[np] = p; saved[np++] = *p; }
    }
    for (off = 0; off < 0x180 && np < 48; off += 4) {
        DWORD *p = (DWORD *)(ctx + off);
        if (*p == (DWORD)(UINT_PTR)gbuf || *p == gsize) { patched[np] = p; saved[np++] = *p; }
    }
    for (off = 0; off < np; off++)
        *patched[off] = saved[off] == gsize ? size : (DWORD)(UINT_PTR)buf;
    if (InterlockedIncrement(&g_swaps) <= 16)
        hg_log("uiext: %s <- override (%lu bytes, stock %lu; %d fields)", pak, size, gsize, np);
    o_uidone(req);
    for (off = 0; off < np; off++) *patched[off] = saved[off];
}

/* ---- strings ---- */

static const struct { const char *key; const WCHAR *text; } g_strings[] = {
    { "ultra sort", L"Sort" },
    { "ultra sort tooltip", L"Sort the backpack: largest items first, then by type" },
};

static const WCHAR *__cdecl d_string(const char *key, int a, int b, int c)
{
    /* hot: every string the UI shows comes through here; the game's keys are
     * valid strings, so a prefix test is enough to leave them alone */
    if (key && key[0] == 'u' && key[1] == 'l' && !strncmp(key, "ultra ", 6)) {
        const WCHAR *t = optpage_string(key);
        int i;
        if (t) return t;
        for (i = 0; i < (int)(sizeof g_strings / sizeof g_strings[0]); i++)
            if (!lstrcmpA(key, g_strings[i].key)) return g_strings[i].text;
    }
    return o_string(key, a, b, c);
}

/* ---- buttons ---- */

static handler_fn o_security;

static int __cdecl d_security(void *comp, int msg, int wp, int lp)
{
    const char *name = (const char *)comp;
    if (name && !IsBadStringPtrA(name, 0x80) && !lstrcmpA(name, "ultra sort btn")) {
        hg_log("uiext: Sort clicked");
        invsort_click(comp);
        return 1;
    }
    if (name && !IsBadStringPtrA(name, 0x80) && !strncmp(name, "ultra ", 6)) {
        int ret = 1;
        if (optpage_click(comp, name, msg, wp, lp, &ret)) return ret;
    }
    return o_security(comp, msg, wp, lp);
}

static void patch_handler(void)
{
    DWORD *entry = (DWORD *)(UINT_PTR)(VA_HANDLERS + HANDLER_SECURITY * 8);
    const char *name;
    DWORD old;
    if (IsBadReadPtr(entry, 8)) { hg_log("uiext: handler table unreadable"); return; }
    name = (const char *)(UINT_PTR)entry[0];
    if (IsBadStringPtrA(name, 64) || lstrcmpA(name, "UIinventorySecurityOnClk") || entry[1] != VA_SECURITY) {
        hg_log("uiext: handler table entry %d is not UIinventorySecurityOnClk; Sort button off", HANDLER_SECURITY);
        return;
    }
    o_security = (handler_fn)(UINT_PTR)entry[1];
    if (!VirtualProtect(&entry[1], 4, PAGE_READWRITE, &old)) { hg_log("uiext: handler table not writable"); return; }
    entry[1] = (DWORD)(UINT_PTR)d_security;
    VirtualProtect(&entry[1], 4, old, &old);
    hg_log("uiext: our buttons ride on UIinventorySecurityOnClk (entry %d)", HANDLER_SECURITY);
}

/* ---- the character select open or not: its screen's activate and
 * inactivate handlers, wrapped (for effects that do not belong there: the
 * ground mist covered its backdrop; its preview is a player unit, so the
 * player test could not tell, 2026-09-24) ---- */

#define VA_CS_ON  0x004f615du         /* UICharacterSelectOnPostActivate */
#define VA_CS_OFF 0x004f64dau         /* UICharacterSelectOnPostInactivate */
static const unsigned char k_cs_on[6]  = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8 };   /* push ebp; mov ebp,esp; and esp,-8 */
static const unsigned char k_cs_off[5] = { 0xa1, 0x9c, 0x75, 0xf2, 0x00 };         /* mov eax,[0xf2759c] */
static handler_fn o_cs_on, o_cs_off;
static volatile LONG g_charsel;
int hg_charselect(void) { return (int)g_charsel; }

static int __cdecl d_cs_on(void *comp, int msg, int wp, int lp)
{
    if (!g_charsel) hg_log("uiext: character select open");
    InterlockedExchange(&g_charsel, 1);
    return o_cs_on(comp, msg, wp, lp);
}

static int __cdecl d_cs_off(void *comp, int msg, int wp, int lp)
{
    if (g_charsel) hg_log("uiext: character select closed");
    InterlockedExchange(&g_charsel, 0);
    return o_cs_off(comp, msg, wp, lp);
}

static void hook(unsigned int va, void *detour, void **orig, const char *name)
{
    if (MH_CreateHook((void *)(UINT_PTR)va, detour, orig) == MH_OK && MH_EnableHook((void *)(UINT_PTR)va) == MH_OK)
        hg_log("uiext: hooked %s at %08x", name, va);
    else
        hg_log("uiext: FAILED to hook %s at %08x", name, va);
}

/* Worker thread, after MH_Initialize, before the UI loads. */
void uiext_install(void)
{
    hook(VA_UIDONE, (void *)d_uidone, (void **)&o_uidone, "UI load callback");
    hook(VA_STRING, (void *)d_string, (void **)&o_string, "string lookup");
    patch_handler();
    /* The character select's two handlers are not in the handler table
     * (wrap_handler found neither: "wrapped: 0, 0", so the fog took the
     * character select for a game, 2026-09-24): its screen registers them
     * from a table it builds on the stack (0x4fb5cc). Hooked in place,
     * after a check of their first bytes. */
    if (!memcmp((void *)(UINT_PTR)VA_CS_ON, k_cs_on, sizeof k_cs_on) &&
        !memcmp((void *)(UINT_PTR)VA_CS_OFF, k_cs_off, sizeof k_cs_off)) {
        hook(VA_CS_ON, (void *)d_cs_on, (void **)&o_cs_on, "UICharacterSelectOnPostActivate");
        hook(VA_CS_OFF, (void *)d_cs_off, (void **)&o_cs_off, "UICharacterSelectOnPostInactivate");
    } else {
        hg_log("uiext: character select handlers not recognised; the character select passes for a game");
    }
}
