#ifndef HG_UI_H
#define HG_UI_H

/*
 * A very small immediate-mode UI, deliberately free of every dependency.
 *
 * No windows.h, no d3d9.h, no allocation. Widgets go in, a flat list of
 * coloured rectangles and strings comes out, and the caller draws them
 * however it likes. overlay.c is the only renderer today; test/ui.c is the
 * second "renderer", and it is the whole reason for the split.
 *
 * The overlay cannot be iterated on in-game: every change costs a Proton
 * launch, a load screen and a walk to somewhere interesting. So the parts
 * that are actually easy to get wrong -- hit testing, tab switching, row
 * wrapping, which byte of a hex dump a click landed on -- live here, on the
 * near side of the toolchain, where `make test` answers in a second.
 *
 * Immediate mode with no widget IDs: widgets never overlap, so "was the
 * cursor inside this rectangle when the button went down" is the entire
 * interaction model.
 */

#include <stddef.h>

#define UI_ARGB(a, r, g, b) (((unsigned int)(a) << 24) | ((unsigned int)(r) << 16) \
                           | ((unsigned int)(g) << 8)  |  (unsigned int)(b))

/* Palette. Tuned against the game's own dark HUD rather than in a vacuum. */
#define UI_C_PANEL    UI_ARGB(232,  13,  17,  23)
#define UI_C_TITLE    UI_ARGB(255,  22,  27,  34)
#define UI_C_LINE     UI_ARGB(255,  48,  54,  61)
#define UI_C_ACCENT   UI_ARGB(255,  78, 161, 255)
#define UI_C_TEXT     UI_ARGB(255, 230, 237, 243)
#define UI_C_DIM      UI_ARGB(255, 139, 148, 158)
#define UI_C_BTN      UI_ARGB(255,  33,  38,  45)
#define UI_C_BTN_HOT  UI_ARGB(255,  55,  62,  71)
#define UI_C_BTN_ON   UI_ARGB(255,  31,  84, 145)
#define UI_C_OK       UI_ARGB(255,  63, 185,  80)
#define UI_C_BAD      UI_ARGB(255, 248,  81,  73)
#define UI_C_WARN     UI_ARGB(255, 210, 153,  34)
#define UI_C_SEL      UI_ARGB(255,  56,  88, 130)
#define UI_C_GROUP    UI_ARGB(255,  18,  23,  30)

enum { UI_RECT = 0, UI_TEXT };

typedef struct {
    int          kind;
    float        x, y, w, h;
    unsigned int color;
    const char  *text;      /* UI_TEXT only; lives until the next ui_begin */
} ui_cmd;

#define UI_MAX_CMDS  1024
#define UI_ARENA     16384
#define UI_MAX_TABS  24

typedef struct {
    /* ---- persistent state, owned by the ui ---- */
    float px, py;             /* panel origin, moved by dragging the title  */
    int   tab;                /* active tab (or sidebar page) index         */
    unsigned int folds;       /* open collapsible sections, one bit each    */
    float grab;               /* the value bar being dragged (its row y + 1), 0 none */
    int   dragging;
    float drag_dx, drag_dy;   /* cursor offset within the title bar         */
    int   placed;             /* px/py have been initialised                */

    /* ---- per-frame input, set by ui_begin ---- */
    float mx, my;
    int   mdown;              /* button held this frame                     */
    int   mclick;             /* button went down this frame (edge)         */
    int   prev_mdown;

    /* ---- font metrics; ui_begin defaults them if left at zero ---- */
    float chw, chh;           /* one character cell, in pixels              */

    /*
     * Back buffer size, when the caller knows it. Used only to keep a
     * dragged panel from being pushed off-screen where it can never be
     * grabbed again. Zero means "do not clamp".
     */
    float screen_w, screen_h;

    /* ---- per-frame output ---- */
    ui_cmd cmds[UI_MAX_CMDS];
    int    ncmds;
    char   arena[UI_ARENA];
    int    arena_used;
    int    overflow;          /* a command or string was dropped            */

    /* ---- layout ---- */
    float  pw, ph;
    float  cx, cy;            /* flow cursor                                */
    float  row_h;
    float  content_l, content_r;
    float  content_top;
    int    group_cmd;         /* command index of the open group's frame    */
    float  group_y;
    int    in_group;
    int    want_close;        /* the title bar's [x] was clicked            */
} ui_ctx;

/* ---- frame ---- */

/*
 * `mdown` is the raw held state; the press edge is derived here so callers
 * cannot get it subtly wrong in two different places.
 */
void ui_begin(ui_ctx *u, float mx, float my, int mdown);
void ui_end(ui_ctx *u);

/* ---- containers ---- */

void ui_panel_begin(ui_ctx *u, const char *title, const char *subtitle,
                    float w, float h);
void ui_panel_end(ui_ctx *u);

/* Draws the tab strip and returns the active tab index. */
int  ui_tabs(ui_ctx *u, const char *const *names, int n);

void ui_group(ui_ctx *u, const char *title);
void ui_group_end(ui_ctx *u);

/*
 * A sidebar of pages down the panel's left side, below the title. Entries
 * starting with '#' are section headings (not selectable); the page index
 * (u->tab) counts only the others. The content area moves right of it.
 */
int  ui_nav(ui_ctx *u, const char *const *items, int n, int cols);
/* A page's title; with `changed` > 0, a "Reset page" button at its right.
 * Returns 1 when that is clicked. */
int  ui_page(ui_ctx *u, const char *title, int changed);
/* A heading inside a page (a rule and a dim title, no frame). */
void ui_section(ui_ctx *u, const char *title);
/* A collapsible heading; `bit` (0..31) keeps its state in u->folds.
 * Returns 1 when open. */
int  ui_fold(ui_ctx *u, const char *title, int bit);

/*
 * Setting rows: one per line, the label in a fixed column. `changed` (from
 * the default) marks the row with an accent bar and colours the value.
 *
 * ui_value: a bar showing `frac` (0..1) of the setting's range with a tick
 * at the default, the value text, and - / +. Returns -1 or +1 for the
 * buttons, 2 when the bar was clicked or dragged (*set_frac, 0..1), else 0.
 * ui_switch: a checkbox; 1 when clicked. ui_choice: segmented buttons; the
 * clicked index or -1. ui_hint: a dim line under the row.
 */
int  ui_value(ui_ctx *u, const char *label, const char *value, float frac,
              float def_frac, int changed, float *set_frac);
int  ui_switch(ui_ctx *u, const char *label, int on, int changed);
int  ui_choice(ui_ctx *u, const char *label, const char *const *names, int n,
               int cur, int changed);
void ui_hint(ui_ctx *u, const char *fmt, ...);

/* ---- widgets ---- */

int  ui_button(ui_ctx *u, const char *label);
int  ui_button_c(ui_ctx *u, const char *label, unsigned int face);
/* A button drawn lit when `on`. Returns 1 on click, like any other button. */
int  ui_toggle(ui_ctx *u, const char *label, int on);
void ui_tile(ui_ctx *u, const char *label, const char *value, unsigned int c);
void ui_text(ui_ctx *u, unsigned int color, const char *fmt, ...);
/* Text that flows in the row like a button, `cols` characters wide (0: as
 * wide as the text), so a value and its buttons share one row. */
void ui_label(ui_ctx *u, unsigned int color, int cols, const char *fmt, ...);
void ui_kv(ui_ctx *u, const char *key, unsigned int color, const char *fmt, ...);
void ui_gap(ui_ctx *u, float h);
void ui_newline(ui_ctx *u);
void ui_rule(ui_ctx *u);

/*
 * A bar chart of `n` samples, oldest first, drawn `rows` character cells
 * tall. `hi` is a floor for the vertical scale, not a ceiling: the graph
 * always autoscales to the tallest sample, because the interesting sample
 * is by definition the one that went off the top.
 */
void ui_graph(ui_ctx *u, const char *label, const float *v, int n,
              float hi, int rows, unsigned int color);

/*
 * A 16-bytes-per-row hex dump with an ASCII gutter.
 *
 *   `prev`, when non-NULL, is the previous snapshot of the same window;
 *   bytes that differ are drawn in warn colour. Watching which bytes move
 *   when you take a hit is the entire technique for finding an offset, so
 *   it is built in rather than left to the eye.
 *
 *   `sel` is the selected byte index, or -1.
 *
 * Returns the byte index under a click, or -1 if the click was elsewhere.
 */
int  ui_hex(ui_ctx *u, const unsigned char *data, const unsigned char *prev,
            int len, unsigned int base_off, int sel);

/* ---- helpers exposed for the tests ---- */

int  ui_hit(const ui_ctx *u, float x, float y, float w, float h);
/* Pixel width of a label as drawn, including the button's own padding. */
float ui_label_w(const ui_ctx *u, const char *label);

#endif
