/*
 * Clicks on the dev panel stay out of the game.
 *
 * The 2018 build reads its mouse buttons as Raw Input (WM_INPUT and
 * GetRawInputData; the exe imports RegisterRawInputDevices), and its menus
 * may take the plain button messages too; DirectInput 8 is still created.
 * All three are filtered: GetRawInputData is hooked (a button's down flag,
 * and its matching up, cleared), the window procedure asks diblock_msg
 * (src/altlatch.c's subclass), and DirectInput below. The DirectInput hooks
 * alone did not keep a click out (2026-09-24).
 *
 * The game reads the mouse with DirectInput 8 (the keyboard comes as window
 * messages, src/inputfilter.c), so a click on the panel also swung whatever
 * the player held. The two ways a DirectInput mouse is read are hooked:
 * IDirectInputDevice8::GetDeviceState (the whole state) and GetDeviceData
 * (a buffer of events). The functions come from a mouse device of our own,
 * created once and released; both the A and W flavours are hooked where
 * they differ.
 *
 * Only mouse devices are touched (GetCapabilities, cached per device): a
 * keyboard's buffered offsets overlap the mouse button offsets. Only
 * buttons are held back, never movement or the wheel. A button pressed
 * while the cursor is over the open panel (src/overlay.c) is withheld until
 * it is released, wherever the cursor is by then; a press elsewhere goes
 * through as always, so dragging out of the panel does not fire.
 */
#include <windows.h>
#include <dinput.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

int overlay_mouse_over(void);

/* the GUIDs, without linking dxguid */
static const GUID k_iid_di8a  = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID k_iid_di8w  = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID k_sysmouse  = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

typedef HRESULT (WINAPI *create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT (WINAPI *state_fn)(void *self, DWORD cb, void *data);
typedef HRESULT (WINAPI *data_fn)(void *self, DWORD cbo, DIDEVICEOBJECTDATA *rg, DWORD *n, DWORD flags);
typedef HRESULT (WINAPI *caps_fn)(void *self, DIDEVCAPS *caps);

/* [0] the A flavour, [1] the W one */
static state_fn o_state[2];
static data_fn  o_data[2];
static caps_fn  g_caps[2];
static void    *g_tgt_state, *g_tgt_data;   /* the A flavour's functions, as hooked */
static LONG g_blocked;

/* per mouse button: pressed over the panel, so withheld until released */
static volatile LONG g_latch[8];
static BYTE g_prev[8];

/* mouse or not, per device (a handful at most) */
static struct { void *dev; int mouse; } g_devs[8];
static int g_ndevs;
static CRITICAL_SECTION g_cs;

static int is_mouse(void *self, int w)
{
    int i, m = 0;
    DIDEVCAPS c;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_ndevs; i++)
        if (g_devs[i].dev == self) { m = g_devs[i].mouse; LeaveCriticalSection(&g_cs); return m; }
    ZeroMemory(&c, sizeof c);
    c.dwSize = sizeof c;
    m = g_caps[w] && SUCCEEDED(g_caps[w](self, &c)) && GET_DIDEVICE_TYPE(c.dwDevType) == DI8DEVTYPE_MOUSE;
    if (g_ndevs < 8) { g_devs[g_ndevs].dev = self; g_devs[g_ndevs].mouse = m; g_ndevs++; }
    LeaveCriticalSection(&g_cs);
    return m;
}

static HRESULT filter_state(int w, void *self, DWORD cb, void *data)
{
    HRESULT hr = o_state[w](self, cb, data);
    int nb, b, over;
    BYTE *btn;
    if (FAILED(hr) || !data || (cb != sizeof(DIMOUSESTATE) && cb != sizeof(DIMOUSESTATE2)) || !is_mouse(self, w))
        return hr;
    over = overlay_mouse_over();
    nb = cb == sizeof(DIMOUSESTATE2) ? 8 : 4;
    btn = (BYTE *)data + 12;
    for (b = 0; b < nb; b++) {
        int down = (btn[b] & 0x80) != 0;
        if (down && !g_prev[b]) g_latch[b] = over;      /* a new press: over the panel? */
        if (!down) g_latch[b] = 0;
        g_prev[b] = (BYTE)down;
        if (g_latch[b]) { btn[b] = 0; InterlockedIncrement(&g_blocked); }
    }
    return hr;
}

static HRESULT filter_data(int w, void *self, DWORD cbo, DIDEVICEOBJECTDATA *rg, DWORD *n, DWORD flags)
{
    HRESULT hr = o_data[w](self, cbo, rg, n, flags);
    DWORD i, out = 0;
    int over;
    if (FAILED(hr) || !rg || !n || !*n || (flags & DIGDD_PEEK) || cbo < 8 || !is_mouse(self, w)) return hr;
    over = overlay_mouse_over();
    for (i = 0; i < *n; i++) {
        DIDEVICEOBJECTDATA *e = (DIDEVICEOBJECTDATA *)((char *)rg + i * cbo);
        int keep = 1;
        if (e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7) {
            int b = (int)(e->dwOfs - DIMOFS_BUTTON0);
            if (e->dwData & 0x80) {
                g_latch[b] = over;
                keep = !over;
            } else {
                keep = !g_latch[b];                     /* the game never saw the press */
                g_latch[b] = 0;
            }
            if (!keep) InterlockedIncrement(&g_blocked);
        }
        if (keep) {
            if (out != i) memmove((char *)rg + out * cbo, e, cbo);
            out++;
        }
    }
    *n = out;
    return hr;
}

static HRESULT WINAPI d_state_a(void *s, DWORD cb, void *d) { return filter_state(0, s, cb, d); }
static HRESULT WINAPI d_state_w(void *s, DWORD cb, void *d) { return filter_state(1, s, cb, d); }
static HRESULT WINAPI d_data_a(void *s, DWORD c, DIDEVICEOBJECTDATA *r, DWORD *n, DWORD f) { return filter_data(0, s, c, r, n, f); }
static HRESULT WINAPI d_data_w(void *s, DWORD c, DIDEVICEOBJECTDATA *r, DWORD *n, DWORD f) { return filter_data(1, s, c, r, n, f); }

/* vtable slots of IDirectInputDevice8: GetCapabilities 3, GetDeviceState 9, GetDeviceData 10 */
static int hook_flavour(create_fn create, int w)
{
    IUnknown *di = NULL;
    void **dev = NULL, **vt;
    int ok = 0;
    void *st, *dt;
    if (FAILED(create(GetModuleHandleA(NULL), 0x0800, w ? &k_iid_di8w : &k_iid_di8a, (void **)&di, NULL)) || !di)
        return 0;
    /* IDirectInput8::CreateDevice is slot 3 in both flavours */
    {
        typedef HRESULT (WINAPI *cd_fn)(void *, REFGUID, void **, LPUNKNOWN);
        cd_fn cd = (cd_fn)(*(void ***)di)[3];
        if (FAILED(cd(di, &k_sysmouse, (void **)&dev, NULL)) || !dev) { di->lpVtbl->Release(di); return 0; }
    }
    vt = *(void ***)dev;
    g_caps[w] = (caps_fn)vt[3];
    st = vt[9]; dt = vt[10];
    /* where A and W share a function, the A hook already serves both */
    if (w && st == g_tgt_state) o_state[1] = o_state[0];
    else ok += MH_CreateHook(st, w ? (void *)d_state_w : (void *)d_state_a, (void **)&o_state[w]) == MH_OK &&
               MH_EnableHook(st) == MH_OK;
    if (w && dt == g_tgt_data) o_data[1] = o_data[0];
    else ok += MH_CreateHook(dt, w ? (void *)d_data_w : (void *)d_data_a, (void **)&o_data[w]) == MH_OK &&
               MH_EnableHook(dt) == MH_OK;
    if (!w) { g_tgt_state = st; g_tgt_data = dt; }
    ((IUnknown *)dev)->lpVtbl->Release((IUnknown *)dev);
    di->lpVtbl->Release(di);
    return ok;
}

typedef UINT (WINAPI *grid_fn)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static grid_fn o_grid;
static UINT WINAPI d_grid(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr);

void diblock_install(void)
{
    HMODULE m = LoadLibraryA("dinput8.dll");
    create_fn create = m ? (create_fn)GetProcAddress(m, "DirectInput8Create") : NULL;
    int a, w;
    InitializeCriticalSection(&g_cs);
    a = w = 0;
    if (create) a = hook_flavour(create, 0);
    if (create) w = hook_flavour(create, 1);
    {
        void *t = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetRawInputData");
        int r = t && MH_CreateHook(t, (void *)d_grid, (void **)&o_grid) == MH_OK && MH_EnableHook(t) == MH_OK;
        hg_log("diblock: mouse reads hooked (raw input %d, DirectInput A %d, W %d): clicks on the panel stay out of the game",
               r, a, w);
    }
}

/* ---- Raw Input ---- */

static volatile LONG g_rlatch[5];           /* left, right, middle, 4, 5 */

static UINT WINAPI d_grid(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    UINT r = o_grid(h, cmd, data, size, hdr);
    RAWINPUT *ri = (RAWINPUT *)data;
    int b, over;
    USHORT f;
    if (cmd != RID_INPUT || !data || r == (UINT)-1 || r < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) ||
        ri->header.dwType != RIM_TYPEMOUSE)
        return r;
    f = ri->data.mouse.usButtonFlags;
    if (!(f & 0x3ff)) return r;                 /* movement or the wheel only */
    over = overlay_mouse_over();
    for (b = 0; b < 5; b++) {
        USHORT down = (USHORT)(1u << (2 * b)), up = (USHORT)(2u << (2 * b));
        if (f & down) {
            g_rlatch[b] = over;
            if (over) { f &= (USHORT)~down; InterlockedIncrement(&g_blocked); }
        }
        if (f & up) {
            if (g_rlatch[b]) { f &= (USHORT)~up; InterlockedIncrement(&g_blocked); }
            g_rlatch[b] = 0;
        }
    }
    ri->data.mouse.usButtonFlags = f;
    return r;
}

/* ---- window messages (from src/altlatch.c's window procedure) ---- */

static int g_mlatch[5];                      /* window thread only */

/* 1 = do not pass it on */
int diblock_msg(UINT msg, WPARAM wp, LPARAM lp)
{
    int b, down;
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: b = 0; down = 1; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: b = 1; down = 1; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: b = 2; down = 1; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: b = HIWORD(wp) == XBUTTON2 ? 4 : 3; down = 1; break;
    case WM_LBUTTONUP: b = 0; down = 0; break;
    case WM_RBUTTONUP: b = 1; down = 0; break;
    case WM_MBUTTONUP: b = 2; down = 0; break;
    case WM_XBUTTONUP: b = HIWORD(wp) == XBUTTON2 ? 4 : 3; down = 0; break;
    default: return 0;
    }
    (void)lp;
    if (down) {
        g_mlatch[b] = overlay_mouse_over();
        return g_mlatch[b];
    }
    if (g_mlatch[b]) { g_mlatch[b] = 0; return 1; }
    return 0;
}

/* ---- polled state (GetKeyState / GetAsyncKeyState, hooked in
 * src/altlatch.c): the game can poll the buttons too ---- */

static volatile LONG g_klatch[5], g_kprev[5];

SHORT diblock_key(int vk, SHORT r)
{
    int b, down;
    switch (vk) {
    case VK_LBUTTON: b = 0; break;
    case VK_RBUTTON: b = 1; break;
    case VK_MBUTTON: b = 2; break;
    case VK_XBUTTON1: b = 3; break;
    case VK_XBUTTON2: b = 4; break;
    default: return r;
    }
    down = (r & 0x8000) != 0;
    if (down && !g_kprev[b]) g_klatch[b] = overlay_mouse_over();
    if (!down) g_klatch[b] = 0;
    g_kprev[b] = down;
    if (!g_klatch[b]) return r;
    InterlockedIncrement(&g_blocked);
    return (SHORT)(r & ~0x8001);                /* not down, not pressed since */
}

long hg_diblock_count(void) { return g_blocked; }
