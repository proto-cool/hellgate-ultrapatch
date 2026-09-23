/*
 * Ctrl+Alt+Shift+P: a stock / new screenshot pair of the same moment.
 *
 * The engine cannot draw one frame twice, so the pair is two frames a few
 * milliseconds apart: this frame as it is (our graphics), then every
 * setting switched to stock (hg_gfx_stock_view: knobs, techniques, shadow
 * map binding, engine patches, AO and SMAA), a few frames for the
 * technique caches and the near shadow map to catch up, that frame, and
 * everything back. Both are taken at EndScene before the dev panel draws.
 *
 * The one thing stock view cannot bring back is MSAA: with SMAA on, the
 * device has none until a restart, so the stock shot has no anti-aliasing.
 *
 * Files: <game>\screenshots\hg_<date>_<time>_new.png and ..._stock.png.
 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9tex.h>
#include "panel.h"

void hg_gfx_stock_view(int on);

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
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, mem)))
        hr = save(path, D3DXIFF_PNG, mem, NULL, NULL);
    if (mem) IDirect3DSurface9_Release(mem);
    IDirect3DSurface9_Release(bb);
    hg_log("compare: %ls %s", path, SUCCEEDED(hr) ? "saved" : "NOT saved");
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

/* From src/device.c's EndScene, after our passes and before the panel. */
void compare_endscene(IDirect3DDevice9 *dev)
{
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
    shot(dev, L"_new.png");
    hg_gfx_stock_view(1);
    g_state = SETTLE_FRAMES;
}
