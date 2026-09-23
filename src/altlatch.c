/*
 * Double-tap Alt to keep the cursor.
 *
 * Holding Alt shows the cursor (CMD_SHOW_ITEMS, bound to Alt). Holding it
 * down while clicking around the UI is tiring, so: hold still works as
 * before, and a double tap latches it -- the second release is swallowed,
 * the game keeps believing Alt is down, and the cursor stays. One more tap
 * releases it.
 *
 * Alt reaches the game as window messages (WM_SYSKEYDOWN / WM_SYSKEYUP);
 * DirectInput is only used for the mouse, and the key-poll loop at
 * 0x4b55dd covers mouse buttons only. So the game's window procedure is
 * subclassed and the Alt messages edited there. As a backstop against any
 * path that asks the key state directly, GetKeyState / GetAsyncKeyState
 * also report Alt held while latched.
 *
 * The tap logic is Windows-free so test/ui.c can drive it.
 */
#include "panel.h"

/* A tap is a press shorter than this; two taps closer than this latch. */
#define AL_TAP_MAX_S      0.30
#define AL_DOUBLE_GAP_S   0.40

enum { AL_PASS = 0, AL_SWALLOW = 1 };

typedef struct {
    int    latched;
    int    releasing;       /* latched, and the next up must pass to unlatch */
    double t_down;          /* last real press                               */
    double t_tap;           /* end of the last tap, 0 if none pending        */
} al_state;

/* An Alt press (not auto-repeat). Returns AL_PASS or AL_SWALLOW. */
int al_down(al_state *s, double t)
{
    if (s->latched) {
        /* The game already thinks Alt is down; this press is the release tap. */
        s->releasing = 1;
        return AL_SWALLOW;
    }
    s->t_down = t;
    return AL_PASS;
}

/* Auto-repeat while held: harmless when not latched, invisible when latched. */
int al_repeat(const al_state *s)
{
    return s->latched ? AL_SWALLOW : AL_PASS;
}

/* An Alt release. Returns AL_PASS or AL_SWALLOW; *changed set on a latch flip. */
int al_up(al_state *s, double t, int *changed)
{
    *changed = 0;
    if (s->latched) {
        if (s->releasing) {                 /* the release tap: let the game see it */
            s->latched = s->releasing = 0;
            s->t_tap = 0.0;
            *changed = 1;
            return AL_PASS;
        }
        return AL_SWALLOW;                  /* a stray up with no down: ignore */
    }
    if (t - s->t_down <= AL_TAP_MAX_S) {
        if (s->t_tap > 0.0 && t - s->t_tap <= AL_DOUBLE_GAP_S + AL_TAP_MAX_S) {
            s->latched = 1;
            s->t_tap = 0.0;
            *changed = 1;
            return AL_SWALLOW;              /* the game never sees this release */
        }
        s->t_tap = t;
    } else {
        s->t_tap = 0.0;                     /* a hold, not a tap */
    }
    return AL_PASS;
}

/* Focus lost while latched: the caller must hand the game a release. */
int al_focus_lost(al_state *s)
{
    int was = s->latched;
    s->latched = s->releasing = 0;
    s->t_tap = 0.0;
    return was;
}

#ifdef _WIN32
#include <windows.h>
#include "../ref/minhook/include/MinHook.h"

static al_state        g_al;                /* window thread only            */
static volatile LONG   g_latched;           /* mirror for the key-state hooks */
static WNDPROC         g_orig_wndproc;
static HWND            g_hwnd;
static LARGE_INTEGER   g_qpf;
static int             g_eat_numlock_up;    /* window thread only            */
static volatile LONG   g_numlock_eaten;

typedef SHORT (WINAPI *keystate_fn)(int);
static keystate_fn o_getkeystate, o_getasynckeystate;

static double now_s(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_qpf.QuadPart;
}

static int is_alt(int vk) { return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU; }

static SHORT WINAPI d_getkeystate(int vk)
{
    SHORT r = o_getkeystate(vk);
    return (g_latched && is_alt(vk)) ? (SHORT)(r | (SHORT)0x8000) : r;
}

static SHORT WINAPI d_getasynckeystate(int vk)
{
    SHORT r = o_getasynckeystate(vk);
    return (g_latched && is_alt(vk)) ? (SHORT)(r | (SHORT)0x8000) : r;
}

int inputfilter_msg(UINT msg, WPARAM wp);

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    int changed = 0;

    if (inputfilter_msg(msg, wp)) return 0;     /* the screenshot combo's P */

    /* Wine's X11 driver keeps its lock-key state in step with the
     * desktop's by injecting a NumLock press and release ahead of a real
     * key. NumLock is the default autorun key, so every such key started
     * autorun (with or without this DLL). A real press is still down when
     * its WM_KEYDOWN is handled; an injected one has its release already
     * queued. Drop those pairs. */
    if (msg == WM_KEYDOWN && wp == VK_NUMLOCK && !(lp & (1L << 30))) {
        MSG m;
        if (PeekMessageA(&m, h, WM_KEYUP, WM_KEYUP, PM_NOREMOVE) && m.wParam == VK_NUMLOCK) {
            g_eat_numlock_up = 1;
            if (InterlockedIncrement(&g_numlock_eaten) <= 20)
                hg_log("key: dropped an injected NumLock tap (%ld so far)", g_numlock_eaten);
            return 0;
        }
    }
    if (msg == WM_KEYUP && wp == VK_NUMLOCK && g_eat_numlock_up) {
        g_eat_numlock_up = 0;
        return 0;
    }

    switch (msg) {
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (!is_alt((int)wp)) break;
        if (lp & (1L << 30)) {              /* auto-repeat */
            if (al_repeat(&g_al) == AL_SWALLOW) return 0;
            break;
        }
        if (al_down(&g_al, now_s()) == AL_SWALLOW) return 0;
        break;

    case WM_SYSKEYUP:
    case WM_KEYUP:
        if (!is_alt((int)wp)) break;
        {
            int r = al_up(&g_al, now_s(), &changed);
            if (changed) {
                InterlockedExchange(&g_latched, g_al.latched);
                hg_log("alt: cursor %s", g_al.latched ? "latched on (tap Alt to release)"
                                                     : "released");
            }
            if (r == AL_SWALLOW) return 0;
        }
        break;

    case WM_KILLFOCUS:
    case WM_ACTIVATEAPP:
        if (msg == WM_ACTIVATEAPP && wp) break;
        if (al_focus_lost(&g_al)) {
            InterlockedExchange(&g_latched, 0);
            /* Give the game the release it was owed, or it comes back stuck. */
            CallWindowProcA(g_orig_wndproc, h, WM_SYSKEYUP, VK_MENU,
                            (LPARAM)0xC0380001);
            hg_log("alt: cursor latch released (focus lost)");
        }
        break;
    }
    return CallWindowProcA(g_orig_wndproc, h, msg, wp, lp);
}

/* Once, from the overlay, when the device's window is known. */
void altlatch_attach(void *hwnd)
{
    HMODULE u;

    if (g_orig_wndproc || !hwnd) return;
    QueryPerformanceFrequency(&g_qpf);
    g_hwnd = (HWND)hwnd;
    g_orig_wndproc = (WNDPROC)SetWindowLongA(g_hwnd, GWL_WNDPROC, (LONG)wndproc);
    if (!g_orig_wndproc) {
        hg_log("alt: could not subclass window %p (%lu); double-tap Alt is off",
               hwnd, GetLastError());
        return;
    }

    u = GetModuleHandleA("user32.dll");
    if (u) {
        void *gks  = (void *)GetProcAddress(u, "GetKeyState");
        void *gaks = (void *)GetProcAddress(u, "GetAsyncKeyState");
        if (gks && MH_CreateHook(gks, (void *)d_getkeystate, (void **)&o_getkeystate) == MH_OK)
            MH_EnableHook(gks);
        if (gaks && MH_CreateHook(gaks, (void *)d_getasynckeystate,
                                  (void **)&o_getasynckeystate) == MH_OK)
            MH_EnableHook(gaks);
    }
    hg_log("alt: double-tap Alt keeps the cursor (window %p)", hwnd);
}

int hg_alt_latched(void) { return (int)g_latched; }
#endif
