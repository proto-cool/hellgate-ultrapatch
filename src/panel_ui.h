#ifndef HG_PANEL_UI_H
#define HG_PANEL_UI_H

#include "ui.h"
#include "panel.h"

/*
 * The dev panel's actual contents: tab strip, groups, buttons, hex view.
 *
 * Split out of overlay.c so it holds no D3D and no windows.h, which means
 * test/ui.c can build every tab natively and assert that the buttons call
 * what they say they call and that the content fits inside the window.
 * Getting that wrong is otherwise a Proton launch away from being noticed.
 *
 * `s` is the latest snapshot; `have` is 0 when the game thread has not
 * published one, in which case `s` must still be a valid zeroed struct.
 */
void panel_ui_build(ui_ctx *u, const panel_snap *s, int have);

/* The sidebar's pages, in order (u->tab). */
enum {
    PG_LIGHTING, PG_SHADOWS, PG_IMAGE, PG_HDR, PG_ATMOSPHERE,
    PG_CAMERA, PG_FIRSTPERSON,
    PG_GFX_DEBUG, PG_PERF, PG_PLAYER, PG_MEMORY, PG_SPAWN, PG_PHYSICS, PG_VIEWMODEL, PG_LOG,
    PG_COUNT
};
#define PANEL_NAV_COLS 15   /* the sidebar's width, characters */

/*
 * The window's size, in back buffer pixels, derived from the measured
 * character cell rather than fixed.
 *
 * The overlay asks d3dx9 for a 14pt fixed-pitch font and gets whatever the
 * system substitutes, which under Proton is not always the cell this was
 * laid out against. Every width here is really a character count -- the hex
 * dump is exactly 74 columns -- so sizing from the cell keeps the dump
 * inside the frame whatever font turns up.
 */
void panel_ui_size(const ui_ctx *u, float *w, float *h);

/* Ctrl+M's half of the hex baseline toggle, so the key and the button agree. */
void panel_ui_toggle_mark(void);

#endif
