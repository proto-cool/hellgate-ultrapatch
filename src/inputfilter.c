/*
 * Keys the game must not see: our screenshot combo, Ctrl+Alt+Shift+P
 * (src/compare.c). The game reads the keyboard through DirectInput 8, not
 * window messages, so every key the DLL uses reached it too: pressing the
 * combo made the swing trails, impacts and weather ash vanish, in both
 * shots and after them (a game binding on one of those keys).
 *
 * dinput8!DirectInput8Create is hooked; on the interface it returns,
 * CreateDevice; on the devices, GetDeviceState and GetDeviceData (the
 * implementations are shared, so each is hooked once). While Ctrl, Alt and
 * Shift are all held, P is taken out of the keyboard state and out of the
 * buffered events. Nothing else changes.
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

static di8create_fn g_orig_create;
static createdev_fn g_orig_createdev;
static getstate_fn g_orig_getstate;
static getdata_fn g_orig_getdata;
static volatile LONG g_dropped;

static int combo_held(void)
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
           (GetAsyncKeyState(VK_SHIFT) & 0x8000);
}

static HRESULT STDMETHODCALLTYPE detour_getstate(void *dev, DWORD cb, void *data)
{
    HRESULT hr = g_orig_getstate(dev, cb, data);
    if (SUCCEEDED(hr) && cb == 256 && data && combo_held() && ((BYTE *)data)[DIK_P]) {
        ((BYTE *)data)[DIK_P] = 0;
        InterlockedIncrement(&g_dropped);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE detour_getdata(void *dev, DWORD cb, dod8 *rg, DWORD *n, DWORD flags)
{
    HRESULT hr = g_orig_getdata(dev, cb, rg, n, flags);
    if (SUCCEEDED(hr) && rg && n && *n && cb >= sizeof(DWORD) && combo_held()) {
        DWORD i, k = 0;
        for (i = 0; i < *n; i++) {
            dod8 *e = (dod8 *)((char *)rg + i * cb);
            if (e->dwOfs == DIK_P) { InterlockedIncrement(&g_dropped); continue; }
            if (k != i) memmove((char *)rg + k * cb, e, cb);
            k++;
        }
        *n = k;
    }
    return hr;
}

static void hook_slot(void **vt, int slot, void *detour, void **orig, const char *name)
{
    if (*orig) return;
    if (MH_CreateHook(vt[slot], detour, orig) == MH_OK && MH_EnableHook(vt[slot]) == MH_OK)
        hg_log("input: hooked %s at %p", name, vt[slot]);
    else
        hg_log("input: FAILED to hook %s", name);
}

static HRESULT STDMETHODCALLTYPE detour_createdev(void *di, REFGUID g, void **out, void *outer)
{
    HRESULT hr = g_orig_createdev(di, g, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out;
        hook_slot(vt, 9, (void *)detour_getstate, (void **)&g_orig_getstate, "IDirectInputDevice8::GetDeviceState");
        hook_slot(vt, 10, (void *)detour_getdata, (void **)&g_orig_getdata, "IDirectInputDevice8::GetDeviceData");
    }
    return hr;
}

static HRESULT WINAPI detour_create(HINSTANCE inst, DWORD ver, REFIID iid, void **out, void *outer)
{
    HRESULT hr = g_orig_create(inst, ver, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out;
        hook_slot(vt, 3, (void *)detour_createdev, (void **)&g_orig_createdev, "IDirectInput8::CreateDevice");
    }
    return hr;
}

void inputfilter_install(void)
{
    HMODULE m = GetModuleHandleA("dinput8.dll");
    void *t;
    if (!m) m = LoadLibraryA("dinput8.dll");
    t = m ? (void *)GetProcAddress(m, "DirectInput8Create") : NULL;
    if (!t || MH_CreateHook(t, (void *)detour_create, (void **)&g_orig_create) != MH_OK || MH_EnableHook(t) != MH_OK) {
        hg_log("input: DirectInput8Create NOT hooked (the game will see the screenshot keys)");
        return;
    }
    hg_log("input: hooked DirectInput8Create: the game will not see P while Ctrl+Alt+Shift are held");
}

long inputfilter_dropped(void) { return g_dropped; }
