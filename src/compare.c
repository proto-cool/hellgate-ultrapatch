/*
 * Ctrl+Alt+Shift+P: a stock / new screenshot pair of the same moment.
 *
 * The engine cannot draw one frame twice, so the pair is two frames a few
 * milliseconds apart: this frame as it is (our graphics), then every
 * setting switched to stock (hg_gfx_stock_view: knobs, techniques, shadow
 * map binding, engine patches, AO and SMAA), a few frames for the
 * technique caches and the near shadow map to catch up, that frame, and
 * everything back. Both are taken at Present, with the dev panel hidden.
 *
 * The one thing stock view cannot bring back is MSAA: with SMAA on, the
 * device has none until a restart, so the stock shot has no anti-aliasing.
 *
 * The game reads the keyboard itself, and the combo triggered one of its
 * bindings (effects vanished): src/inputfilter.c keeps P from it while
 * Ctrl+Alt+Shift are held.
 *
 * Files: <game>\screenshots\hg_<date>_<time>_new.png and ..._stock.png.
 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9tex.h>
#include "panel.h"

void hg_gfx_stock_view(int on);
long inputfilter_dropped(void);

#define SETTLE_FRAMES 4         /* technique caches: 1; near shadow map: every other frame */

static int  g_state;            /* 0 idle, >0 frames left before the stock shot */
static WCHAR g_stem[MAX_PATH];

static int save_png(IDirect3DDevice9 *dev, const WCHAR *path)
{
    typedef HRESULT (WINAPI *save_fn)(LPCWSTR, D3DXIMAGE_FILEFORMAT, IDirect3DSurface9 *,
                                      const PALETTEENTRY *, const RECT *);
    static save_fn save;
    IDirect3DSurface9 *bb = NULL, *mem = NULL;
    D3DSURFACE_DESC d;
    HRESULT hr = E_FAIL;
    if (!save) {
        HMODULE m = GetModuleHandleA("d3dx9_34.dll");
        if (m) save = (save_fn)(void *)GetProcAddress(m, "D3DXSaveSurfaceToFileW");
        if (!save) { hg_log("compare: no d3dx9_34!D3DXSaveSurfaceToFileW"); return 0; }
    }
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return 0;
    if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d)) &&
        SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format,
                                                               D3DPOOL_SYSTEMMEM, &mem, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, mem))) {
        /* the game keeps glow in the back buffer's alpha: a PNG with it
         * showed 89% of the frame see-through in image viewers */
        D3DLOCKED_RECT lr;
        if ((d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) &&
            SUCCEEDED(IDirect3DSurface9_LockRect(mem, &lr, NULL, 0))) {
            UINT x, y;
            for (y = 0; y < d.Height; y++) {
                DWORD *row = (DWORD *)((char *)lr.pBits + y * lr.Pitch);
                for (x = 0; x < d.Width; x++) row[x] |= 0xff000000u;
            }
            IDirect3DSurface9_UnlockRect(mem);
        }
        hr = save(path, D3DXIFF_PNG, mem, NULL, NULL);
    }
    if (mem) IDirect3DSurface9_Release(mem);
    IDirect3DSurface9_Release(bb);
    hg_log("compare: %ls %s (P presses kept from the game so far: %ld)", path, SUCCEEDED(hr) ? "saved" : "NOT saved",
           inputfilter_dropped());
    return SUCCEEDED(hr);
}

static void shot(IDirect3DDevice9 *dev, const WCHAR *which)
{
    WCHAR p[MAX_PATH];
    lstrcpyW(p, g_stem);
    lstrcatW(p, which);
    save_png(dev, p);
}

static int hotkey(void)
{
    static int was;
    int now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
              (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('P') & 0x8000);
    int edge = now && !was;
    was = now;
    return edge;
}

/*
 * From src/device.c at Present, once a frame (the engine ends a scene several
 * times a frame; the first version shot at EndScene and saved the cleared
 * back buffer: two black images). g_state: 0 idle; -1 armed, the panel is
 * hidden this frame and the next Present takes the "new" shot; > 0 frames
 * left before the stock one.
 */
void compare_present(IDirect3DDevice9 *dev)
{
    if (g_state == -1) {
        shot(dev, L"_new.png");
        hg_gfx_stock_view(1);
        g_state = SETTLE_FRAMES;
        return;
    }
    if (g_state > 0) {
        if (--g_state == 0) {
            shot(dev, L"_stock.png");
            hg_gfx_stock_view(0);
        }
        return;
    }
    if (!hotkey()) return;
    {
        SYSTEMTIME t;
        WCHAR dir[MAX_PATH];
        hg_dll_dir(dir, MAX_PATH);
        lstrcatW(dir, L"\\..\\screenshots");
        CreateDirectoryW(dir, NULL);
        GetLocalTime(&t);
        wsprintfW(g_stem, L"%s\\hg_%04u%02u%02u_%02u%02u%02u", dir, t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond);
    }
    g_state = -1;               /* next frame: no panel, then the shot */
}

int compare_hides_overlay(void) { return g_state != 0; }
