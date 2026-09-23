/*
 * The immediate-mode UI core. See ui.h for why it has no dependencies.
 *
 * Layout is a single left-to-right flow with wrapping, which is all a panel
 * of tabs and button groups needs. Widgets are placed at a cursor, the
 * cursor wraps when a widget will not fit, and groups indent the margins
 * and patch their own frame height on the way out.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

#define UI_PAD      12.0f   /* panel edge to content                    */
#define UI_GAP       6.0f   /* between widgets on a row                 */
#define UI_ROWGAP    5.0f   /* between rows                             */
#define UI_BTN_PADX  9.0f   /* button label inset, each side            */
#define UI_BTN_PADY  5.0f
#define UI_GRP_PAD   7.0f   /* group frame inset                        */

/* ------------------------------------------------------------------ */
/* primitives                                                          */

static const char *push_str(ui_ctx *u, const char *s)
{
    int n = (int)strlen(s);
    char *d;
    if (u->arena_used + n + 1 > UI_ARENA) { u->overflow = 1; return ""; }
    d = u->arena + u->arena_used;
    memcpy(d, s, (size_t)n + 1);
    u->arena_used += n + 1;
    return d;
}

static ui_cmd *push_cmd(ui_ctx *u)
{
    if (u->ncmds >= UI_MAX_CMDS) { u->overflow = 1; return NULL; }
    return &u->cmds[u->ncmds++];
}

static int push_rect(ui_ctx *u, float x, float y, float w, float h,
                     unsigned int c)
{
    ui_cmd *k = push_cmd(u);
    if (!k) return -1;
    k->kind = UI_RECT; k->x = x; k->y = y; k->w = w; k->h = h;
    k->color = c; k->text = NULL;
    return u->ncmds - 1;
}

/* A one-pixel outline, as four thin fills. Cheaper than a line primitive. */
static void push_frame(ui_ctx *u, float x, float y, float w, float h,
                       unsigned int c)
{
    push_rect(u, x, y, w, 1.0f, c);
    push_rect(u, x, y + h - 1.0f, w, 1.0f, c);
    push_rect(u, x, y, 1.0f, h, c);
    push_rect(u, x + w - 1.0f, y, 1.0f, h, c);
}

static void push_text(ui_ctx *u, float x, float y, const char *s,
                      unsigned int c)
{
    ui_cmd *k;
    if (!s || !*s) return;
    k = push_cmd(u);
    if (!k) return;
    k->kind = UI_TEXT; k->x = x; k->y = y; k->w = 0.0f; k->h = u->chh;
    k->color = c; k->text = push_str(u, s);
}

int ui_hit(const ui_ctx *u, float x, float y, float w, float h)
{
    return u->mx >= x && u->mx < x + w && u->my >= y && u->my < y + h;
}

float ui_label_w(const ui_ctx *u, const char *label)
{
    return (float)strlen(label) * u->chw + 2.0f * UI_BTN_PADX;
}

/* ------------------------------------------------------------------ */
/* flow                                                                */

static void row_break(ui_ctx *u)
{
    u->cy += (u->row_h > 0.0f ? u->row_h : u->chh) + UI_ROWGAP;
    u->cx = u->content_l;
    u->row_h = 0.0f;
}

static void flush_row(ui_ctx *u)
{
    if (u->cx > u->content_l || u->row_h > 0.0f) row_break(u);
}

static void place(ui_ctx *u, float w, float h, float *ox, float *oy)
{
    if (u->cx > u->content_l && u->cx + w > u->content_r) row_break(u);
    *ox = u->cx;
    *oy = u->cy;
    u->cx += w + UI_GAP;
    if (h > u->row_h) u->row_h = h;
}

void ui_newline(ui_ctx *u) { flush_row(u); }
void ui_gap(ui_ctx *u, float h) { flush_row(u); u->cy += h; }

void ui_rule(ui_ctx *u)
{
    flush_row(u);
    push_rect(u, u->content_l, u->cy, u->content_r - u->content_l, 1.0f,
              UI_C_LINE);
    u->cy += 1.0f + UI_ROWGAP;
}

/* ------------------------------------------------------------------ */
/* frame                                                               */

void ui_begin(ui_ctx *u, float mx, float my, int mdown)
{
    if (u->chw <= 0.0f) u->chw = 7.0f;
    if (u->chh <= 0.0f) u->chh = 15.0f;

    u->mx = mx; u->my = my;
    u->mdown = mdown;
    u->mclick = (mdown && !u->prev_mdown);
    u->prev_mdown = mdown;

    u->ncmds = 0;
    u->arena_used = 0;
    u->overflow = 0;
    u->want_close = 0;
    u->in_group = 0;
}

void ui_end(ui_ctx *u) { (void)u; }

/* ------------------------------------------------------------------ */
/* panel                                                               */

void ui_panel_begin(ui_ctx *u, const char *title, const char *subtitle,
                    float w, float h)
{
    float title_h = u->chh + 12.0f;
    float bx, bw = u->chw * 3.0f + 8.0f;

    if (!u->placed) { u->px = 24.0f; u->py = 24.0f; u->placed = 1; }

    u->pw = w; u->ph = h;
    bx = u->px + w - bw - 6.0f;

    /*
     * Close first, then drag. A click on [x] sits inside the title bar, so
     * testing the drag first would swallow it and the panel would only ever
     * jump a pixel instead of closing.
     */
    if (u->mclick && ui_hit(u, bx, u->py + 5.0f, bw, title_h - 10.0f)) {
        u->want_close = 1;
        u->mclick = 0;
    } else if (u->mclick && ui_hit(u, u->px, u->py, w, title_h)) {
        u->dragging = 1;
        u->drag_dx = u->mx - u->px;
        u->drag_dy = u->my - u->py;
        u->mclick = 0;
    }
    if (u->dragging) {
        if (!u->mdown) {
            u->dragging = 0;
        } else {
            u->px = u->mx - u->drag_dx;
            u->py = u->my - u->drag_dy;
            /* Keep a grabbable strip on screen; a panel dragged fully off
             * one edge can never be dragged back. */
            if (u->screen_w > 0.0f) {
                float minx = 40.0f - w, maxx = u->screen_w - 60.0f;
                if (u->px < minx) u->px = minx;
                if (u->px > maxx) u->px = maxx;
            }
            if (u->screen_h > 0.0f) {
                float maxy = u->screen_h - title_h;
                if (u->py < 0.0f) u->py = 0.0f;
                if (u->py > maxy) u->py = maxy;
            }
        }
        u->mclick = 0;      /* a drag never also clicks a widget */
    }

    push_rect(u, u->px, u->py, w, h, UI_C_PANEL);
    push_rect(u, u->px, u->py, w, title_h, UI_C_TITLE);
    push_rect(u, u->px, u->py, w, 2.0f, UI_C_ACCENT);
    push_frame(u, u->px, u->py, w, h, UI_C_LINE);

    push_text(u, u->px + UI_PAD, u->py + 6.0f, title, UI_C_TEXT);
    if (subtitle)
        push_text(u, u->px + UI_PAD + (float)strlen(title) * u->chw + u->chw * 2.0f,
                  u->py + 6.0f, subtitle, UI_C_DIM);

    push_rect(u, bx, u->py + 5.0f, bw, title_h - 10.0f,
              ui_hit(u, bx, u->py + 5.0f, bw, title_h - 10.0f)
                  ? UI_C_BAD : UI_C_BTN);
    push_text(u, bx + u->chw, u->py + 6.0f, "x", UI_C_TEXT);

    u->content_l = u->px + UI_PAD;
    u->content_r = u->px + w - UI_PAD;
    u->cx = u->content_l;
    u->cy = u->py + title_h + 8.0f;
    u->content_top = u->cy;
    u->row_h = 0.0f;
}

void ui_panel_end(ui_ctx *u)
{
    flush_row(u);
    if (u->overflow)
        push_text(u, u->px + UI_PAD, u->py + u->ph - u->chh - 6.0f,
                  "ui: draw buffer full", UI_C_BAD);
}

int ui_tabs(ui_ctx *u, const char *const *names, int n)
{
    float h = u->chh + 9.0f;
    float x = u->content_l;
    int i;

    if (n > UI_MAX_TABS) n = UI_MAX_TABS;
    if (u->tab >= n) u->tab = 0;
    if (u->tab < 0) u->tab = 0;

    for (i = 0; i < n; i++) {
        float w = (float)strlen(names[i]) * u->chw + 2.0f * UI_BTN_PADX;
        int on = (i == u->tab);
        int hot = ui_hit(u, x, u->cy, w, h);

        if (hot && u->mclick) { u->tab = i; u->mclick = 0; on = 1; }

        push_rect(u, x, u->cy, w, h,
                  on ? UI_C_BTN_ON : (hot ? UI_C_BTN_HOT : UI_C_BTN));
        if (on) push_rect(u, x, u->cy + h - 2.0f, w, 2.0f, UI_C_ACCENT);
        push_text(u, x + UI_BTN_PADX, u->cy + 4.0f, names[i],
                  on ? UI_C_TEXT : UI_C_DIM);
        x += w + 3.0f;
    }

    u->cy += h + 1.0f;
    push_rect(u, u->content_l, u->cy, u->content_r - u->content_l, 1.0f,
              UI_C_LINE);
    u->cy += 1.0f + 9.0f;
    u->cx = u->content_l;
    u->row_h = 0.0f;
    return u->tab;
}

/* ------------------------------------------------------------------ */
/* groups                                                              */

void ui_group(ui_ctx *u, const char *title)
{
    flush_row(u);
    u->group_cmd = push_rect(u, u->content_l, u->cy,
                             u->content_r - u->content_l, 0.0f, UI_C_GROUP);
    u->group_y = u->cy;
    u->in_group = 1;

    push_text(u, u->content_l + UI_GRP_PAD, u->cy + 5.0f, title, UI_C_DIM);
    push_rect(u, u->content_l, u->cy, 2.0f, u->chh + 10.0f, UI_C_ACCENT);

    u->cy += u->chh + 12.0f;
    u->content_l += UI_GRP_PAD;
    u->content_r -= UI_GRP_PAD;
    u->cx = u->content_l;
    u->row_h = 0.0f;
}

void ui_group_end(ui_ctx *u)
{
    flush_row(u);
    u->content_l -= UI_GRP_PAD;
    u->content_r += UI_GRP_PAD;
    u->cx = u->content_l;
    u->in_group = 0;

    if (u->group_cmd >= 0 && u->group_cmd < u->ncmds)
        u->cmds[u->group_cmd].h = (u->cy - u->group_y) + 2.0f;
    u->cy += UI_ROWGAP + 6.0f;
}

/* ------------------------------------------------------------------ */
/* widgets                                                             */

int ui_button_c(ui_ctx *u, const char *label, unsigned int face)
{
    float w = ui_label_w(u, label);
    float h = u->chh + 2.0f * UI_BTN_PADY;
    float x, y;
    int hot, clicked;

    place(u, w, h, &x, &y);
    hot = ui_hit(u, x, y, w, h);
    clicked = hot && u->mclick;
    if (clicked) u->mclick = 0;     /* one click, one widget */

    push_rect(u, x, y, w, h, hot ? UI_C_BTN_HOT : face);
    if (hot) push_frame(u, x, y, w, h, UI_C_ACCENT);
    push_text(u, x + UI_BTN_PADX, y + UI_BTN_PADY, label, UI_C_TEXT);
    return clicked;
}

int ui_button(ui_ctx *u, const char *label)
{
    return ui_button_c(u, label, UI_C_BTN);
}

int ui_toggle(ui_ctx *u, const char *label, int on)
{
    return ui_button_c(u, label, on ? UI_C_BTN_ON : UI_C_BTN);
}

void ui_tile(ui_ctx *u, const char *label, const char *value, unsigned int c)
{
    float lw = (float)strlen(label) * u->chw;
    float vw = (float)strlen(value) * u->chw;
    float w = (lw > vw ? lw : vw) + 16.0f;
    float h = u->chh * 2.0f + 12.0f;
    float x, y;

    if (w < 78.0f) w = 78.0f;
    place(u, w, h, &x, &y);
    push_rect(u, x, y, w, h, UI_C_GROUP);
    push_rect(u, x, y, 2.0f, h, c);
    push_text(u, x + 8.0f, y + 4.0f, value, c);
    push_text(u, x + 8.0f, y + 4.0f + u->chh, label, UI_C_DIM);
}

void ui_text(ui_ctx *u, unsigned int color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    flush_row(u);
    push_text(u, u->content_l, u->cy, buf, color);
    u->row_h = u->chh;
    row_break(u);
}

void ui_label(ui_ctx *u, unsigned int color, int cols, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    float w, h = u->chh + 2.0f * UI_BTN_PADY, x, y;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    w = (float)(cols > 0 ? cols : (int)strlen(buf)) * u->chw;
    place(u, w, h, &x, &y);
    push_text(u, x, y + UI_BTN_PADY, buf, color);
}

void ui_kv(ui_ctx *u, const char *key, unsigned int color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    flush_row(u);
    push_text(u, u->content_l, u->cy, key, UI_C_DIM);
    push_text(u, u->content_l + 15.0f * u->chw, u->cy, buf, color);
    u->row_h = u->chh;
    row_break(u);
}

/* ------------------------------------------------------------------ */
/* graph                                                               */

void ui_graph(ui_ctx *u, const char *label, const float *v, int n,
              float hi, int rows, unsigned int color)
{
    float h = (float)rows * u->chh;
    float w, bw, x0, y0, peak = hi;
    char cap[96];
    int i;

    if (n <= 0) return;
    flush_row(u);

    for (i = 0; i < n; i++) if (v[i] > peak) peak = v[i];
    if (peak <= 0.0f) peak = 1.0f;

    x0 = u->content_l;
    y0 = u->cy;
    w  = u->content_r - u->content_l;
    bw = w / (float)n;
    if (bw < 1.0f) bw = 1.0f;

    push_rect(u, x0, y0, w, h, UI_C_GROUP);
    for (i = 0; i < n; i++) {
        float bh = v[i] / peak * (h - 2.0f);
        if (bh < 0.0f) bh = 0.0f;
        if (bh > 0.0f && bh < 1.0f) bh = 1.0f;
        push_rect(u, x0 + (float)i * bw, y0 + h - bh,
                  bw > 2.0f ? bw - 1.0f : bw, bh, color);
    }
    push_rect(u, x0, y0 + h - 1.0f, w, 1.0f, UI_C_LINE);

    u->cy = y0 + h + 2.0f;
    u->row_h = 0.0f;
    u->cx = u->content_l;

    snprintf(cap, sizeof cap, "%s   peak %.1f   now %.1f   (%d samples, "
             "oldest left)", label, peak, v[n - 1], n);
    push_text(u, x0, u->cy, cap, UI_C_DIM);
    u->cy += u->chh + UI_ROWGAP;
}

/* ------------------------------------------------------------------ */
/* hex dump                                                            */

/*
 * Character column of byte `i` in a rendered row. The gap after byte 7 is
 * the only irregularity, and both the renderer and the hit test go through
 * here so they cannot drift apart.
 */
static int hex_col(int i) { return 6 + i * 3 + (i >= 8 ? 1 : 0); }
static int asc_col(int i) { return 57 + i; }

/* Inverse of hex_col/asc_col: character column -> byte index, or -1. */
static int col_byte(int col)
{
    if (col >= 6 && col < 55) {
        int c = col - 6;
        if (c == 24) return 7;              /* the extra gap reads as byte 7 */
        if (c > 24) return 8 + (c - 25) / 3;
        return c / 3;
    }
    if (col >= asc_col(0) && col < asc_col(16)) return col - asc_col(0);
    return -1;
}

int ui_hex(ui_ctx *u, const unsigned char *data, const unsigned char *prev,
           int len, unsigned int base_off, int sel)
{
    char line[96];
    float line_h = u->chh + 1.0f;
    float x0, y0;
    int rows = len / 16, r, i, hit = -1;

    flush_row(u);
    x0 = u->content_l;
    y0 = u->cy;

    for (r = 0; r < rows; r++) {
        const unsigned char *p = data + r * 16;
        float ly = y0 + (float)r * line_h;
        int n = snprintf(line, sizeof line, "%04x  ",
                         (unsigned int)(base_off + (unsigned int)r * 16));

        for (i = 0; i < 16; i++)
            n += snprintf(line + n, (int)sizeof line - n, "%02x%s",
                          p[i], i == 7 ? "  " : " ");
        n += snprintf(line + n, (int)sizeof line - n, " |");
        for (i = 0; i < 16; i++) {
            unsigned char ch = p[i];
            n += snprintf(line + n, (int)sizeof line - n, "%c",
                          (ch >= 32 && ch < 127) ? (char)ch : '.');
        }
        snprintf(line + n, (int)sizeof line - n, "|");

        /* Selection and change highlights go behind the text. */
        for (i = 0; i < 16; i++) {
            int idx = r * 16 + i;
            int changed = prev && prev[idx] != data[idx];
            int in_sel = sel >= 0 && idx >= (sel & ~3) && idx < (sel & ~3) + 4;

            if (in_sel)
                push_rect(u, x0 + (float)hex_col(i) * u->chw - 1.0f, ly - 1.0f,
                          u->chw * 2.0f + 2.0f, line_h, UI_C_SEL);
            else if (changed)
                push_rect(u, x0 + (float)hex_col(i) * u->chw - 1.0f, ly - 1.0f,
                          u->chw * 2.0f + 2.0f, line_h,
                          UI_ARGB(150, 120, 86, 10));
            if (in_sel)
                push_rect(u, x0 + (float)asc_col(i) * u->chw - 1.0f, ly - 1.0f,
                          u->chw + 2.0f, line_h, UI_C_SEL);
        }
        push_text(u, x0, ly, line, UI_C_TEXT);
    }

    {
        float area_h = (float)rows * line_h;
        float area_w = 74.0f * u->chw;
        if (u->mclick && ui_hit(u, x0, y0, area_w, area_h)) {
            int row = (int)((u->my - y0) / line_h);
            int col = (int)((u->mx - x0) / u->chw);
            int b = col_byte(col);
            if (b >= 0 && row >= 0 && row < rows) {
                hit = row * 16 + b;
                u->mclick = 0;
            }
        }
        u->cy = y0 + area_h;
        u->row_h = 0.0f;
        u->cx = u->content_l;
        u->cy += UI_ROWGAP;
    }
    return hit;
}
