/*
 * "Marcus Fidelius Ultrapatch v0.N" in the bottom right corner of the main
 * menu.
 *
 * The main menu is drawn in the engine's UI-only mode: e_Render tests one
 * flag (DAT_00edfd34) and calls e_RenderUIOnly instead of the 3D frame. The
 * text is drawn at Present (src/device.c opens a scene for it) whenever that
 * flag is set, so loading screens get it too. The version is the commit
 * count, stamped by the Makefile (HG_VERSION, HG_COMMIT).
 *
 * The same font writes "STOCK" at the top of the screen while stock view is
 * held (Ctrl+Alt+Shift+S, src/compare.c), so an A/B by eye knows its side.
 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9core.h>
#include "panel.h"

#ifndef HG_VERSION
#define HG_VERSION "0.0"
#endif
#ifndef HG_COMMIT
#define HG_COMMIT "unknown"
#endif

#define RVA_UI_ONLY 0x00ADFD34u     /* DAT_00edfd34: e_Render draws the UI alone */

typedef HRESULT (WINAPI *createfont_fn)(IDirect3DDevice9 *, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD,
                                        DWORD, DWORD, LPCSTR, LPD3DXFONT *);

static unsigned int g_image;
static ID3DXFont *g_font;
static UINT g_font_h;
static int g_font_failed;

static const char k_text[] = "Marcus Fidelius Ultrapatch  v" HG_VERSION;

void brand_install(unsigned int image)
{
    g_image = image;
    hg_log("brand: Marcus Fidelius Ultrapatch v%s (%s)", HG_VERSION, HG_COMMIT);
}

/* 1 when the main menu (or another UI-only screen) is up; logs changes. */
int brand_wanted(void)
{
    static int last = -1;
    int *flag = (int *)(g_image + RVA_UI_ONLY);
    int on;
    if (!g_image || IsBadReadPtr(flag, 4)) return 0;
    on = *flag != 0;
    if (on != last) {
        last = on;
        hg_log("brand: UI-only screen %s", on ? "up (menu or loading): name shown" : "gone");
    }
    return on && !g_font_failed;
}

void brand_reset(void)
{
    if (g_font) { g_font->lpVtbl->Release(g_font); g_font = NULL; }
    g_font_failed = 0;
}

/* Inside a scene, at Present: the name (menus), the stock view tag. */
void brand_draw(IDirect3DDevice9 *dev, int name, int stock)
{
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC d;
    RECT rc;
    UINT h;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    IDirect3DSurface9_Release(bb);
    h = d.Height / 60;
    if (h < 12) h = 12;
    if (g_font && g_font_h != h) brand_reset();
    if (!g_font) {
        static createfont_fn create;
        if (!create) {
            HMODULE m = GetModuleHandleA("d3dx9_34.dll");
            if (m) create = (createfont_fn)(void *)GetProcAddress(m, "D3DXCreateFontA");
        }
        if (!create || FAILED(create(dev, (INT)h, 0, FW_SEMIBOLD, 1, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                     "Georgia", &g_font)) || !g_font) {
            hg_log("brand: font NOT created");
            g_font = NULL;
            g_font_failed = 1;
            return;
        }
        g_font_h = h;
    }
    if (stock) {
        static const char tag[] = "STOCK   (Ctrl+Alt+Shift+S)";
        rc.left = 0; rc.right = (LONG)d.Width;
        rc.top = (LONG)h; rc.bottom = (LONG)(h * 3);
        OffsetRect(&rc, 1, 1);
        g_font->lpVtbl->DrawTextA(g_font, NULL, tag, -1, &rc, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP,
                                  D3DCOLOR_ARGB(200, 0, 0, 0));
        OffsetRect(&rc, -1, -1);
        g_font->lpVtbl->DrawTextA(g_font, NULL, tag, -1, &rc, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP,
                                  D3DCOLOR_ARGB(230, 247, 142, 30));
    }
    if (!name) return;
    rc.left = 0; rc.top = 0;
    rc.right = (LONG)d.Width - (LONG)h;
    /* one line above the game's own "Single play 2.1.0.4" in the same corner */
    rc.bottom = (LONG)d.Height - (LONG)(h * 2.3f);
    /* a one-pixel shadow keeps it legible on any background */
    OffsetRect(&rc, 1, 1);
    g_font->lpVtbl->DrawTextA(g_font, NULL, k_text, -1, &rc, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOCLIP,
                              D3DCOLOR_ARGB(200, 0, 0, 0));
    OffsetRect(&rc, -1, -1);
    g_font->lpVtbl->DrawTextA(g_font, NULL, k_text, -1, &rc, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOCLIP,
                              D3DCOLOR_ARGB(220, 214, 196, 160));
}
