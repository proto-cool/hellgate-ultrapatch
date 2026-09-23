/*
 * Keys the game must not see: our screenshot combo, Ctrl+Alt+Shift+P
 * (src/compare.c). Shift+P is the game's effects toggle, so pressing the
 * combo made the swing trails, impacts and weather ash vanish, in both
 * shots and after them.
 *
 * The game takes the keyboard as window messages (DirectInput is only its
 * mouse; see src/altlatch.c), so the filter runs in the window procedure
 * altlatch subclasses. Two earlier versions filtered DirectInput keyboard
 * reads and never saw a key.
 *
 * A P pressed while Ctrl, Alt and Shift are all held is swallowed, and so
 * is every P message after it until P is released: letting go of Ctrl or
 * Alt first must not hand the game a Shift+P. Our own trigger polls
 * GetAsyncKeyState, which this does not touch.
 *
 * The decision is Windows-free so test/ui.c can drive it.
 */

enum { KF_DOWN, KF_UP, KF_CHAR };

typedef struct {
    int eating;             /* a swallowed P is still held */
} kf_state;

/* One P message; combo = Ctrl, Alt and Shift held. Returns 1 to swallow. */
int kf_key(kf_state *s, int kind, int combo)
{
    switch (kind) {
    case KF_DOWN:
        if (combo) s->eating = 1;
        return s->eating;
    case KF_UP:
        if (!s->eating) return 0;
        s->eating = 0;
        return 1;
    default:
        return s->eating;
    }
}

#ifdef _WIN32
#include <windows.h>
#include "panel.h"

static kf_state      g_kf;                  /* window thread only */
static volatile LONG g_dropped;

/* From the subclassed window procedure: 1 = do not pass it on. */
int inputfilter_msg(UINT msg, WPARAM wp)
{
    int kind, combo;
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp != 'P') return 0;
        kind = KF_DOWN;
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (wp != 'P') return 0;
        kind = KF_UP;
        break;
    case WM_CHAR: case WM_SYSCHAR:          /* 'p', 'P', or Ctrl+P's 0x10 */
        if (wp != 'p' && wp != 'P' && wp != 0x10) return 0;
        kind = KF_CHAR;
        break;
    default:
        return 0;
    }
    combo = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (!kf_key(&g_kf, kind, combo)) return 0;
    InterlockedIncrement(&g_dropped);
    return 1;
}

long inputfilter_dropped(void) { return g_dropped; }
#endif
