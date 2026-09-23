/*
 * Keys the game must not see: our screenshot combo, Ctrl+Alt+Shift+P
 * (src/compare.c). The game reads the keyboard through DirectInput 8, not
 * window messages, so every key the DLL uses reached it too: pressing the
 * combo made the swing trails, impacts and weather ash vanish, in both
 * shots and after them (a game binding on one of those keys).
 *
 * GetDeviceState and GetDeviceData are hooked on a keyboard device the DLL
 * creates at load through both interfaces (ANSI and wide) and releases: the
 * game's devices share those implementations. Hooking CreateDevice instead
 * never saw the game's device under Wine. While Ctrl, Alt and Shift are
 * all held, P is taken out of the keyboard state and out of the buffered
 * events; Shift+P is the game's effects toggle. Nothing else changes.
 */
#include <windows.h>
#include "panel.h"
#include "MinHook.h"

#define DIK_P 0x19

typedef struct { DWORD dwOfs, dwData, dwTimeStamp, dwSequence; UINT_PTR uAppData; } dod8;   /* DIDEVICEOBJECTDATA */

typedef HRESULT (WINAPI *di8create_fn)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef HRESULT (STDMETHODCALLTYPE *createdev_fn)(void *, REFGUID, void **, void *);
typedef HRESULT (STDMETHODCALLTYPE *getstate_fn)(void *, DWORD, void *);
typedef HRESULT (STDMETHODCALLTYPE *getdata_fn)(void *, DWORD, dod8 *, DWORD *, DWORD);

static volatile LONG g_dropped;

static int combo_held(void)
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
           (GetAsyncKeyState(VK_SHIFT) & 0x8000);
}

/* The ANSI and wide device interfaces may have separate implementations:
 * two of each detour, each with its own trampoline. */
#define DETOURS(n)                                                                                  \
static getstate_fn g_orig_getstate##n;                                                              \
static getdata_fn g_orig_getdata##n;                                                                \
static HRESULT STDMETHODCALLTYPE detour_getstate##n(void *dev, DWORD cb, void *data)                \
{                                                                                                   \
    HRESULT hr = g_orig_getstate##n(dev, cb, data);                                                 \
    if (SUCCEEDED(hr)) filter_state(cb, data);                                                      \
    return hr;                                                                                      \
}                                                                                                   \
static HRESULT STDMETHODCALLTYPE detour_getdata##n(void *dev, DWORD cb, dod8 *rg, DWORD *k, DWORD f) \
{                                                                                                   \
    HRESULT hr = g_orig_getdata##n(dev, cb, rg, k, f);                                              \
    if (SUCCEEDED(hr)) filter_data(cb, rg, k);                                                      \
    return hr;                                                                                      \
}

static void filter_state(DWORD cb, void *data)
{
    if (cb == 256 && data && combo_held() && ((BYTE *)data)[DIK_P]) {
        ((BYTE *)data)[DIK_P] = 0;
        InterlockedIncrement(&g_dropped);
    }
}

static void filter_data(DWORD cb, dod8 *rg, DWORD *n)
{
    DWORD i, k = 0;
    if (!rg || !n || !*n || cb < sizeof(DWORD) || !combo_held()) return;
    for (i = 0; i < *n; i++) {
        dod8 *e = (dod8 *)((char *)rg + i * cb);
        if (e->dwOfs == DIK_P) { InterlockedIncrement(&g_dropped); continue; }
        if (k != i) memmove((char *)rg + k * cb, e, cb);
        k++;
    }
    *n = k;
}

DETOURS(0)
DETOURS(1)

static void *g_hooked[4];

/* hooks vt[slot] unless that function is hooked already; which: 0 or 1 */
static void hook_fn(void **vt, int slot, int which, const char *name)
{
    void *t = vt[slot], *detour, **orig;
    int i;
    for (i = 0; i < 4; i++) if (g_hooked[i] == t) return;
    if (slot == 9) { detour = which ? (void *)detour_getstate1 : (void *)detour_getstate0;
                     orig = which ? (void **)&g_orig_getstate1 : (void **)&g_orig_getstate0; }
    else           { detour = which ? (void *)detour_getdata1 : (void *)detour_getdata0;
                     orig = which ? (void **)&g_orig_getdata1 : (void **)&g_orig_getdata0; }
    if (*orig) return;
    if (MH_CreateHook(t, detour, orig) == MH_OK && MH_EnableHook(t) == MH_OK) {
        for (i = 0; i < 4; i++) if (!g_hooked[i]) { g_hooked[i] = t; break; }
        hg_log("input: hooked %s at %p", name, t);
    } else {
        hg_log("input: FAILED to hook %s at %p", name, t);
    }
}

/* A keyboard device of our own through each interface, to reach the
 * implementations every keyboard device shares; released straight away. */
void inputfilter_install(void)
{
    static const GUID iid_a = { 0xbf798030, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
    static const GUID iid_w = { 0xbf798031, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
    static const GUID kbd = { 0x6f1d2b61, 0xd5a0, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    HMODULE m = GetModuleHandleA("dinput8.dll");
    di8create_fn create;
    int w;
    if (!m) m = LoadLibraryA("dinput8.dll");
    create = m ? (di8create_fn)(void *)GetProcAddress(m, "DirectInput8Create") : NULL;
    if (!create) { hg_log("input: no DirectInput8Create (the game will see the screenshot keys)"); return; }
    for (w = 0; w < 2; w++) {
        void *di = NULL, *dev = NULL;
        if (FAILED(create(GetModuleHandleA(NULL), 0x0800, w ? &iid_w : &iid_a, &di, NULL)) || !di) continue;
        if (SUCCEEDED(((createdev_fn)(*(void ***)di)[3])(di, &kbd, &dev, NULL)) && dev) {
            void **vt = *(void ***)dev;
            hook_fn(vt, 9, w, w ? "keyboard GetDeviceState (W)" : "keyboard GetDeviceState (A)");
            hook_fn(vt, 10, w, w ? "keyboard GetDeviceData (W)" : "keyboard GetDeviceData (A)");
            ((ULONG (STDMETHODCALLTYPE *)(void *))(*(void ***)dev)[2])(dev);
        }
        ((ULONG (STDMETHODCALLTYPE *)(void *))(*(void ***)di)[2])(di);
    }
    hg_log("input: the game will not see P while Ctrl+Alt+Shift are held (Shift+P is its effects toggle)");
}

long inputfilter_dropped(void) { return g_dropped; }
