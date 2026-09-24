/*
 * In-game D3D9 overlay: the dev panel you actually use.
 *
 * Toggled with Shift+` — CMD_CONSOLE_TOGGLE's own binding, recovered from the
 * keybind table (see KEY_CONSOLE_TOGGLE in target.h). The console it opened is
 * compiled out of this build, so that binding is dead and free to take over.
 *
 * The Shift matters. A bare ` opens the chatbox, which is live code; only the
 * Shift+` console is gone. Binding the overlay to a bare ` would shadow the
 * chatbox every time the panel was opened.
 *
 * Shape: a draggable window with a tab strip and groups of clickable buttons.
 * All of the layout and hit testing lives in ui.c, which has no D3D and no
 * windows.h and is unit-tested offline — the overlay cannot be iterated on
 * in-game without a Proton launch and a walk to somewhere interesting, so
 * anything that can be tested on this side of the toolchain is.
 *
 * Mouse:
 *
 *   The cursor position is read with GetCursorPos and mapped through the
 *   device window's client rect into back buffer space, because the client
 *   area and the back buffer are only the same size by coincidence.
 *
 *   The click also reaches the game. The game reads DINPUT8 directly and
 *   there is no way to swallow a button from here short of hooking
 *   IDirectInputDevice8::GetDeviceState, which is a separate project. So
 *   clicking a button in the panel also swings whatever you are holding.
 *   The footer says so. Every control is also on a Ctrl-modified key for
 *   when that matters, or when exclusive fullscreen pins the cursor.
 *
 * How the device is reached: src/device.c hooks the game's device when it is
 *   created and calls overlay_endscene / overlay_reset from its EndScene and
 *   Reset detours. We never proxy d3d9.dll (DXVK owns it under Proton).
 *
 * Threading:
 *
 *   EndScene runs on the render thread. It must never block, so the overlay
 *   never issues a panel command and waits for it. It reads the snapshot that
 *   panel_pump() maintains on the game thread, and writes only through the
 *   interlocked setters in panel.h. The worst case is showing data one frame
 *   stale.
 *
 * Device loss:
 *
 *   overlay_reset runs before every Reset. The font and state block are
 *   released there and rebuilt lazily afterwards, which is what keeps alt-tab and
 *   resolution changes from taking the process down.
 */
#include <d3d9.h>
#include <d3dx9core.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "target.h"
#include "panel.h"

#include "ui.h"
#include "panel_ui.h"
typedef HRESULT (WINAPI *createfont_fn)(IDirect3DDevice9 *, INT, UINT, UINT, UINT,
                                        BOOL, DWORD, DWORD, DWORD, DWORD,
                                        LPCSTR, LPD3DXFONT *);

static createfont_fn  g_createfont;
static LPD3DXFONT     g_font;
static IDirect3DStateBlock9 *g_sb;
static IDirect3DDevice9 *g_dev;     /* the device the font belongs to */
static HWND           g_hwnd;
static int            g_visible;
/*
 * Two different failures, deliberately not merged. A missing d3dx9 is
 * permanent and there is no point retrying it every frame. A failed font
 * creation can be transient (it happens around device loss), so that one is
 * cleared on Reset and retried rather than disabling the overlay for the
 * rest of the session.
 */
static int            g_no_d3dx;
static int            g_font_failed;

static ui_ctx         g_ui;

typedef struct { float x, y, z, rhw; D3DCOLOR c; } ovtx;

/* Builds one frame of the panel. Everything it draws comes back in g_ui. */
static void build_ui(float mx, float my, int mdown, float sw, float sh)
{
    panel_snap snap;
    int have = panel_snap_read(&snap);

    if (!have) memset(&snap, 0, sizeof snap);
    g_ui.screen_w = sw;
    g_ui.screen_h = sh;

    ui_begin(&g_ui, mx, my, mdown);
    panel_ui_build(&g_ui, &snap, have);
    ui_end(&g_ui);

    if (g_ui.want_close) g_visible = 0;
}

/* ------------------------------------------------------------------ */
/* input                                                               */

/*
 * Edge-detected GetAsyncKeyState. DINPUT8 is what the game reads, so these
 * presses still reach it; every binding below is either the dead console key
 * or Ctrl-modified, which keeps the clash surface near zero.
 */
static int pressed(int vk)
{
    static unsigned char was[256];
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !was[vk & 0xff];
    was[vk & 0xff] = (unsigned char)down;
    return edge;
}

/*
 * Screen cursor -> back buffer coordinates.
 *
 * The client area and the back buffer are different spaces whenever the
 * game is rendering at anything other than the window size, which under
 * Proton with a scaling compositor is most of the time. Mapping through the
 * client rect is what keeps the hit boxes under the drawn buttons.
 */
static void cursor_pos(IDirect3DDevice9 *dev, float *ox, float *oy,
                       float *sw, float *sh)
{
    D3DVIEWPORT9 vp;
    POINT p;
    RECT rc;

    *ox = *oy = -1.0f;
    *sw = *sh = 0.0f;

    if (FAILED(IDirect3DDevice9_GetViewport(dev, &vp))) return;
    *sw = (float)vp.Width;
    *sh = (float)vp.Height;

    if (!GetCursorPos(&p)) return;
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc) ||
        rc.right <= rc.left || rc.bottom <= rc.top) {
        *ox = (float)p.x;           /* no window: assume 1:1 */
        *oy = (float)p.y;
        return;
    }
    ScreenToClient(g_hwnd, &p);
    *ox = (float)p.x * (float)vp.Width  / (float)(rc.right - rc.left);
    *oy = (float)p.y * (float)vp.Height / (float)(rc.bottom - rc.top);
}

/* The keyboard path. Everything here has a button too; this is the escape
 * hatch for when the cursor is pinned by exclusive fullscreen. */
static void poll_keys(void)
{
    int ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    int shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;

    /*
     * Edge-detect the key unconditionally, then require Shift. Testing the
     * modifier first would leave a stale edge: press `, release, then press
     * Shift+` and the second press would not register as an edge.
     */
    if (pressed(KEY_CONSOLE_TOGGLE) && shift) g_visible = !g_visible;
    if (!g_visible || !ctrl) return;

    if (pressed('1')) g_ui.tab = 0;
    if (pressed('2')) g_ui.tab = 1;
    if (pressed('3')) g_ui.tab = 2;
    if (pressed('4')) g_ui.tab = 3;
    if (pressed('5')) g_ui.tab = 4;
    if (pressed('6')) g_ui.tab = 5;
    if (pressed('7')) g_ui.tab = 6;
    if (pressed('8')) g_ui.tab = 7;
    if (pressed('9')) g_ui.tab = 8;
    if (pressed('0')) g_ui.tab = 9;

    if (pressed(VK_DOWN))  panel_peek_nudge(0x10);
    if (pressed(VK_UP))    panel_peek_nudge(-0x10);
    if (pressed(VK_NEXT))  panel_peek_nudge(0x100);
    if (pressed(VK_PRIOR)) panel_peek_nudge(-0x100);
    if (pressed(VK_HOME))  panel_peek_set(0);
    if (pressed('M'))      panel_ui_toggle_mark();
    if (pressed('B'))      hg_spawn_queue(10);
    if (pressed('F'))      hg_fart();
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

#define MAXQUAD 320

static ovtx  g_vtx[MAXQUAD * 6];
static int   g_nvtx;

static void flush_quads(IDirect3DDevice9 *dev)
{
    if (!g_nvtx) return;
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST,
                                     (UINT)(g_nvtx / 3), g_vtx, sizeof g_vtx[0]);
    g_nvtx = 0;
}

/*
 * Rectangles are batched into one triangle list and flushed whenever a text
 * command interrupts them, which keeps a panel of ~200 rectangles down to a
 * handful of draw calls instead of two hundred.
 */
static void push_quad(IDirect3DDevice9 *dev, float x, float y, float w, float h,
                      D3DCOLOR c)
{
    ovtx *v;
    if (w <= 0.0f || h <= 0.0f) return;
    if (g_nvtx + 6 > MAXQUAD * 6) flush_quads(dev);
    v = &g_vtx[g_nvtx];
    g_nvtx += 6;

    v[0].x = x;     v[0].y = y;     v[1].x = x + w; v[1].y = y;
    v[2].x = x;     v[2].y = y + h; v[3].x = x + w; v[3].y = y;
    v[4].x = x + w; v[4].y = y + h; v[5].x = x;     v[5].y = y + h;
    {
        int i;
        for (i = 0; i < 6; i++) { v[i].z = 0.0f; v[i].rhw = 1.0f; v[i].c = c; }
    }
}

static void draw_cursor(IDirect3DDevice9 *dev, float x, float y)
{
    D3DCOLOR w = D3DCOLOR_ARGB(255, 255, 255, 255);
    D3DCOLOR k = D3DCOLOR_ARGB(190, 0, 0, 0);

    /* A crosshair rather than an arrow: the game usually hides the system
     * cursor, and a crosshair says exactly which pixel the hit test uses. */
    push_quad(dev, x - 9.0f, y - 2.0f, 18.0f, 3.0f, k);
    push_quad(dev, x - 2.0f, y - 9.0f, 3.0f, 18.0f, k);
    push_quad(dev, x - 8.0f, y - 1.0f, 16.0f, 1.0f, w);
    push_quad(dev, x - 1.0f, y - 8.0f, 1.0f, 16.0f, w);
}

static void measure_font(void)
{
    RECT r;
    if (!g_font) return;

    /* DT_CALCRECT on a known string: the font is requested fixed-pitch but
     * the substitute that actually loads may not be, and every hit box here
     * is derived from the cell size. */
    r.left = r.top = 0; r.right = 4096; r.bottom = 256;
    ID3DXFont_DrawTextA(g_font, NULL, "0123456789", -1, &r,
                        DT_CALCRECT | DT_LEFT | DT_TOP, 0);
    if (r.right > r.left)   g_ui.chw = (float)(r.right - r.left) / 10.0f;
    if (r.bottom > r.top)   g_ui.chh = (float)(r.bottom - r.top);
    if (g_ui.chw < 4.0f)    g_ui.chw = 7.0f;
    if (g_ui.chh < 8.0f)    g_ui.chh = 15.0f;
}

static void render(IDirect3DDevice9 *dev, float mx, float my)
{
    int i;

    if (FAILED(IDirect3DStateBlock9_Capture(g_sb))) return;

    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    g_nvtx = 0;
    for (i = 0; i < g_ui.ncmds; i++) {
        const ui_cmd *c = &g_ui.cmds[i];
        if (c->kind == UI_RECT) {
            push_quad(dev, c->x, c->y, c->w, c->h, (D3DCOLOR)c->color);
        } else {
            RECT r;
            flush_quads(dev);
            r.left = (LONG)c->x; r.top = (LONG)c->y;
            r.right = (LONG)(c->x + 4000.0f);
            r.bottom = (LONG)(c->y + c->h + 4.0f);
            ID3DXFont_DrawTextA(g_font, NULL, c->text, -1, &r,
                                DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE,
                                (D3DCOLOR)c->color);
        }
    }
    flush_quads(dev);

    if (mx >= 0.0f) {
        draw_cursor(dev, mx, my);
        flush_quads(dev);
    }

    IDirect3DStateBlock9_Apply(g_sb);
}

/* ------------------------------------------------------------------ */
/* resources                                                           */

static void release_res(void)
{
    if (g_font) { ID3DXFont_Release(g_font); g_font = NULL; }
    if (g_sb)   { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
    g_dev = NULL;
    g_font_failed = 0;      /* retry after a reset */
}

static void ensure_res(IDirect3DDevice9 *dev)
{
    if (g_no_d3dx || g_font_failed) return;
    if (g_font && g_sb && g_dev == dev) return;
    if (g_dev != dev) release_res();

    if (!g_font) {
        /*
         * d3dx9_42.dll ships in the game's own bin/ directory, so it is
         * already next to us and already loaded by the time we draw.
         */
        HMODULE dx = GetModuleHandleA("d3dx9_42.dll");
        if (!dx) dx = LoadLibraryA("d3dx9_42.dll");
        if (!dx) dx = LoadLibraryA("d3dx9_43.dll");
        if (!dx) {
            hg_log("overlay: no d3dx9_42/43.dll — cannot draw text");
            g_no_d3dx = 1;
            return;
        }
        if (!g_createfont)
            g_createfont = (createfont_fn)(void *)
                GetProcAddress(dx, "D3DXCreateFontA");
        if (!g_createfont) {
            hg_log("overlay: d3dx9 has no D3DXCreateFontA");
            g_no_d3dx = 1;
            return;
        }

        /* 21 px: 14 was too small to read at 2560x1600, 28 too big */
        if (FAILED(g_createfont(dev, 21, 0, FW_NORMAL, 1, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN,
                                "Consolas", &g_font))) {
            hg_log("overlay: D3DXCreateFontA failed");
            g_font = NULL;
            g_font_failed = 1;
            return;
        }
        measure_font();
    }
    if (!g_sb) {
        HRESULT shr = IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &g_sb);
        if (FAILED(shr)) {
            hg_log("overlay: CreateStateBlock failed hr=0x%08lx",
                   (unsigned long)shr);
            g_sb = NULL;
            return;
        }
    }
    {
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)))
            g_hwnd = cp.hFocusWindow;
    }
    g_dev = dev;
    hg_log("overlay: resources ready on device %p (cell %.1fx%.1f, hwnd %p)",
           (void *)dev, g_ui.chw, g_ui.chh, (void *)g_hwnd);
}

/* ------------------------------------------------------------------ */
/* entry points (src/device.c owns the device hooks)                   */

static volatile LONG g_on;      /* panel wanted and HG_OVERLAY_OFF unset */

void overlay_endscene(IDirect3DDevice9 *dev)
{
    if (!g_on) return;
    poll_keys();
    if (g_visible) {
        ensure_res(dev);
        if (g_font && g_sb) {
            float mx, my, sw, sh;
            cursor_pos(dev, &mx, &my, &sw, &sh);
            build_ui(mx, my,
                     (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, sw, sh);
            render(dev, mx, my);
        }
    }
}

void overlay_reset(void)
{
    release_res();
}

void overlay_start(unsigned int image)
{
    WCHAR env[32];

    (void)image;
    if (GetEnvironmentVariableW(L"HG_OVERLAY_OFF", env, 32) > 0 && env[0] != L'0') {
        hg_log("overlay: disabled by HG_OVERLAY_OFF");
        return;
    }
    InterlockedExchange(&g_on, 1);
    hg_log("overlay: ready — press Shift+` in game to open the panel");
}
