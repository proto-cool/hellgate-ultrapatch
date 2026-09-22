/*
 * Offline tests for the panel's UI core.
 *
 * The overlay cannot be iterated on in-game: every change costs a Proton
 * launch, a load screen and a walk to somewhere that spawns things. So the
 * parts that are easy to get quietly wrong -- a button whose hit box is not
 * where it is drawn, a click that fires two widgets, a hex dump that reports
 * the wrong byte -- are tested here, natively, in under a second.
 *
 * The method throughout is to render a frame, find where a widget was
 * actually *drawn* by looking in the command list, and then click at that
 * position. A test that computed the expected position from the same layout
 * constants the code uses would agree with a bug.
 *
 * ui.c is included rather than linked so the column arithmetic can be tested
 * directly as well as through the public surface.
 */
#include "../src/ui.c"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/panel.h"

/* ------------------------------------------------------------------ */
/* stubs for the DLL side                                              */

/*
 * panel_ui.c calls into hook.c and panel.c. Those are Windows code, so the
 * tests stand in for them and record what was asked. That recording is the
 * point: "Fire 10 queues ten spawns" is a claim about wiring, and wiring is
 * exactly what silently broke last time.
 */
static long  stub_queued, stub_queue_calls, stub_clear_calls;
static int   stub_immediate = -1, stub_simtype = -99, stub_reset_calls;
static unsigned int stub_mult, stub_peek_set, stub_watch_add;
static int   stub_peek_nudge, stub_mark, stub_watch_clear;
static unsigned int stub_poke_off, stub_poke_val;
static int   stub_poke_calls;
static const char *stub_log_text;

void hg_log(const char *fmt, ...)                  { (void)fmt; }
int  hg_flagfile(const wchar_t *n)                 { (void)n; return 0; }
int  hg_log_line(int age, char *out, int cap)
{
    if (!stub_log_text || age > 2 || cap < 8) return 0;
    snprintf(out, (size_t)cap, "%s %d", stub_log_text, age);
    return 1;
}
void hg_spawn_status(hg_spawn_state *o)            { memset(o, 0, sizeof *o); }
void hg_spawn_queue(long n)         { stub_queued += n; stub_queue_calls++; }
void hg_spawn_clear(void)                          { stub_clear_calls++; }
void hg_spawn_set_immediate(int on)                { stub_immediate = on; }
void hg_spawn_set_mult(unsigned int m)             { stub_mult = m; }
long hg_spawn_pump(void)                           { return 0; }
void hg_set_simtype(int t)                         { stub_simtype = t; }
int  hg_get_simtype_override(void)                 { return -1; }
int  hg_get_simtype_seen(void)                     { return 2; }
void hg_counters_reset(void)                       { stub_reset_calls++; }
static int stub_cam_req = -99, stub_fp_melee = -1;
void hg_set_fp_melee(int on)                       { stub_fp_melee = on; }
int  hg_get_fp_melee(void)                         { return 0; }
int  hg_fp_melee_available(void)                   { return 1; }
void hg_camera_request(int mode)                   { stub_cam_req = mode; }
int  hg_camera_mode(void)                          { return 6; }
long hg_camera_pump(void)                          { return 0; }
static int stub_bit = -1, stub_bitval = -1;
int  hg_model_third(void)                          { return 42; }
void hg_model_flag_request(int b, int v)           { stub_bit=b; stub_bitval=v; }
long hg_model_pump(void)                           { return 0; }
static int stub_sh_on = -1, stub_sh_swaps, stub_sh_doff, stub_sh_dh;
void hg_shoulder_status(hg_shoulder_state *o)      { memset(o, 0, sizeof *o); }
void hg_shoulder_set_on(int on)                    { stub_sh_on = on; }
void hg_shoulder_swap(void)                        { stub_sh_swaps++; }
static int stub_sh_dfar, stub_sh_dlift, stub_sh_dzoom, stub_sh_collide = -1;
void hg_shoulder_nudge_zoom(int l, int z)  { stub_sh_dlift += l; stub_sh_dzoom += z; }
void hg_shoulder_set_collide(int on)       { stub_sh_collide = on; }
static int stub_imp_on = -1, stub_imp_d;
void hg_impulse_set_on(int on)             { stub_imp_on = on; }
void hg_impulse_nudge(int d)               { stub_imp_d += d; }
static int stub_gfx_lights = -1;
void hg_gfx_status(hg_gfx_state *o)                 { memset(o, 0, sizeof *o); o->overrides = 2; }
void hg_gfx_set_lights(int on)                     { stub_gfx_lights = on; }
static int stub_shadow = -1, stub_sflag;
void hg_gfx_force_shadow_flag(int on)              { stub_sflag = on; }
int  hg_gfx_shadow_flag_forced(void)               { return stub_sflag; }
void hg_gfx_set_fill(int pct)                      { (void)pct; }
void hg_gfx_set_pcss(int on)                       { (void)on; }
void hg_gfx_scale_pcss(int which, int up)          { (void)which; (void)up; }
void hg_gfx_nudge_pcss_min(int d)                  { (void)d; }
void hg_gfx_nudge_look(int which, int d)           { (void)which; (void)d; }
void hg_gfx_nudge_pl(int which, int d)             { (void)which; (void)d; }
void hg_gfx_dump_shadowmaps(void)                  { }
void hg_gfx_set_shadow_debug(int on)               { (void)on; }
void hg_gfx_set_one_map(int on)                    { (void)on; }
int  hg_gfx_one_map(void)                          { return 0; }
void hg_gfx_trace_shadows(void)                    { }
int  hg_gfx_shadow_debug(void)                     { return 0; }
void hg_gfx_set_cast_all(int on)                   { (void)on; }
void hg_gfx_cast_all_status(int *on, long *s, long *v) { *on = 0; *s = 0; *v = 0; }
void hg_gfx_nudge_reach(int d)                     { (void)d; }
int  hg_gfx_reach(void)                            { return 27; }
int  hg_gfx_shadow_type(void)                      { return 2; }
void hg_shadow_set(int on)                         { stub_shadow = on; }
int  hg_shadow_get(void)                           { return stub_shadow > 0; }
static int stub_orbit = -1;
void hg_orbit_set_on(int on)               { stub_orbit = on; }
void hg_shoulder_nudge(int a, int b, int c)
{
    stub_sh_doff += a; stub_sh_dh += b; stub_sh_dfar += c;
}
static int stub_farts;
void hg_fart(void)                                 { stub_farts++; }
int  hg_fart_samples(void)                         { return 3; }
void hg_dll_dir(wchar_t *o, int c)                 { if(c>0) o[0]=0; }
void panel_start(unsigned int i)                   { (void)i; }
int  panel_wanted(void)                            { return 1; }
void panel_pump(void)                              { }
int  panel_snap_read(panel_snap *o)      { memset(o, 0, sizeof *o); return 1; }
void panel_peek_nudge(int d)                       { stub_peek_nudge += d; }
void panel_peek_set(unsigned int o)                { stub_peek_set = o; }
void panel_peek_mark(int on)                       { stub_mark = on; }
void panel_poke_async(unsigned int o, unsigned int v)
{
    stub_poke_off = o; stub_poke_val = v; stub_poke_calls++;
}
void panel_watch_add(unsigned int o)               { stub_watch_add = o; }
void panel_watch_clear(void)                       { stub_watch_clear++; }
void panel_publish(unsigned int a, unsigned int b, long c, double d,
                   float e, float f, float g)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
}

#include "../src/panel_ui.c"
#include "../src/fart.c"
#include "../src/shoulder.c"
#include "../src/altlatch.c"

static int g_fail;
static int g_run;

static void ok(int cond, const char *what, ...)
{
    va_list ap;
    g_run++;
    if (cond) return;
    g_fail++;
    fputs("FAIL  ", stdout);
    va_start(ap, what);
    vprintf(what, ap);
    va_end(ap);
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */

/*
 * Where was this label drawn, and what rectangle sits immediately behind
 * it? For a button that rectangle is the face, which is exactly the region
 * the hit test is supposed to use.
 */
static int label_rect(const ui_ctx *u, const char *label,
                      float *x, float *y, float *w, float *h)
{
    int i, j;
    for (i = 0; i < u->ncmds; i++) {
        if (u->cmds[i].kind != UI_TEXT || !u->cmds[i].text) continue;
        if (strcmp(u->cmds[i].text, label) != 0) continue;
        for (j = i - 1; j >= 0; j--) {
            if (u->cmds[j].kind != UI_RECT) continue;
            if (u->cmds[j].w < 2.0f || u->cmds[j].h < 2.0f) continue;
            *x = u->cmds[j].x; *y = u->cmds[j].y;
            *w = u->cmds[j].w; *h = u->cmds[j].h;
            return 1;
        }
        return 0;
    }
    return 0;
}

static int text_at(const ui_ctx *u, const char *label, float *x, float *y)
{
    int i;
    for (i = 0; i < u->ncmds; i++)
        if (u->cmds[i].kind == UI_TEXT && u->cmds[i].text &&
            strcmp(u->cmds[i].text, label) == 0) {
            *x = u->cmds[i].x; *y = u->cmds[i].y;
            return 1;
        }
    return 0;
}

/* A frame far away from every widget: draws everything, clicks nothing. */
static void idle_frame(ui_ctx *u, void (*body)(ui_ctx *))
{
    ui_begin(u, -1000.0f, -1000.0f, 0);
    body(u);
    ui_end(u);
}

/* Mouse goes down at (x,y) this frame. The caller releases by running an
 * idle frame afterwards, exactly as a real click behaves. */
static void click_frame(ui_ctx *u, float x, float y, void (*body)(ui_ctx *))
{
    ui_begin(u, x, y, 1);
    body(u);
    ui_end(u);
    ui_begin(u, x, y, 0);       /* release */
    body(u);
    ui_end(u);
}

/* ------------------------------------------------------------------ */
/* scenes                                                              */

static const char *const TABS[8] = {
    "Live", "Player", "Memory", "Spawn", "Physics", "Camera", "Model", "Log"
};

static int hit_alpha, hit_beta, hit_gamma, hit_wrapped;

static void scene_buttons(ui_ctx *u)
{
    ui_panel_begin(u, "HELLGATE DEV", "sub", 784.0f, 586.0f);
    ui_tabs(u, TABS, 8);
    ui_group(u, "GROUP ONE");
    if (ui_button(u, "Alpha")) hit_alpha++;
    if (ui_button(u, "Beta"))  hit_beta++;
    ui_group_end(u);
    ui_group(u, "GROUP TWO");
    if (ui_button(u, "Gamma")) hit_gamma++;
    ui_group_end(u);
    ui_panel_end(u);
}

/* Enough buttons to force the row to wrap several times. */
static void scene_wrap(ui_ctx *u)
{
    int i;
    char name[16];
    ui_panel_begin(u, "W", NULL, 400.0f, 500.0f);
    ui_group(u, "MANY");
    for (i = 0; i < 20; i++) {
        snprintf(name, sizeof name, "btn%02d", i);
        if (ui_button(u, name) && i == 17) hit_wrapped++;
    }
    ui_group_end(u);
    ui_panel_end(u);
}

static unsigned char g_data[256], g_mark[256];
static int g_hexhit = -1;
static int g_hexsel = -1;

static void scene_hex(ui_ctx *u)
{
    int r;
    ui_panel_begin(u, "M", NULL, 784.0f, 586.0f);
    ui_tabs(u, TABS, 8);
    ui_group(u, "WINDOW");
    ui_text(u, UI_C_DIM, "unit +0x0000");
    ui_group_end(u);
    r = ui_hex(u, g_data, g_mark, 256, 0x40, g_hexsel);
    if (r >= 0) g_hexhit = r;
    ui_panel_end(u);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */

static void test_button_hit(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_buttons);

    ok(label_rect(&u, "Alpha", &x, &y, &w, &h), "Alpha was drawn");
    ok(w > 20.0f && h > 10.0f, "Alpha has a sane face (%.0fx%.0f)", w, h);

    hit_alpha = hit_beta = hit_gamma = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_buttons);
    ok(hit_alpha == 1, "clicking Alpha's centre fires it once (got %d)", hit_alpha);
    ok(hit_beta == 0 && hit_gamma == 0, "no other button fired");

    /* The press edge is what fires, so holding must not re-fire. */
    hit_alpha = 0;
    ui_begin(&u, x + w / 2.0f, y + h / 2.0f, 1);
    scene_buttons(&u);
    ui_end(&u);
    ui_begin(&u, x + w / 2.0f, y + h / 2.0f, 1);   /* still held */
    scene_buttons(&u);
    ui_end(&u);
    ok(hit_alpha == 1, "a held button fires once, not every frame (got %d)",
       hit_alpha);
}

static void test_button_edges(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_buttons);
    ok(label_rect(&u, "Beta", &x, &y, &w, &h), "Beta was drawn");

    hit_beta = 0;
    click_frame(&u, x - 3.0f, y + h / 2.0f, scene_buttons);
    ok(hit_beta == 0, "a click just left of Beta misses");

    hit_beta = 0;
    click_frame(&u, x + w + 3.0f, y + h / 2.0f, scene_buttons);
    ok(hit_beta == 0, "a click in the gap right of Beta misses");

    hit_beta = 0;
    click_frame(&u, x + 0.5f, y + 0.5f, scene_buttons);
    ok(hit_beta == 1, "the top-left pixel of Beta hits");

    hit_beta = 0;
    click_frame(&u, x + w - 0.5f, y + h - 0.5f, scene_buttons);
    ok(hit_beta == 1, "the bottom-right pixel of Beta hits");
}

static void test_one_click_one_widget(void)
{
    ui_ctx u;
    float ax, ay, aw, ah;

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_buttons);
    label_rect(&u, "Alpha", &ax, &ay, &aw, &ah);

    hit_alpha = hit_beta = hit_gamma = 0;
    click_frame(&u, ax + aw / 2.0f, ay + ah / 2.0f, scene_buttons);
    ok(hit_alpha + hit_beta + hit_gamma == 1,
       "exactly one widget consumed the click (got %d)",
       hit_alpha + hit_beta + hit_gamma);
}

static void test_tabs(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_buttons);
    ok(u.tab == 0, "the first tab starts active");

    ok(label_rect(&u, "Spawn", &x, &y, &w, &h), "the Spawn tab was drawn");
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_buttons);
    ok(u.tab == 3, "clicking Spawn selects tab 3 (got %d)", u.tab);

    idle_frame(&u, scene_buttons);
    ok(label_rect(&u, "Log", &x, &y, &w, &h), "the Log tab was drawn");
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_buttons);
    ok(u.tab == 7, "clicking Log selects the last tab (got %d)", u.tab);

    /* A tab click must not also fall through to a button underneath. */
    hit_alpha = 0;
    idle_frame(&u, scene_buttons);
    label_rect(&u, "Live", &x, &y, &w, &h);
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_buttons);
    ok(u.tab == 0 && hit_alpha == 0, "a tab click does not also press a button");
}

static void test_wrap(void)
{
    ui_ctx u;
    float x, y, w, h, panel_r;
    int i;
    char name[16];

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_wrap);

    panel_r = u.px + 400.0f;
    for (i = 0; i < 20; i++) {
        snprintf(name, sizeof name, "btn%02d", i);
        if (!label_rect(&u, name, &x, &y, &w, &h)) { ok(0, "%s drawn", name); return; }
        if (x + w > panel_r) { ok(0, "%s overflows the panel edge", name); return; }
    }
    ok(1, "20 wrapped buttons all stay inside the panel");

    /* And a wrapped button still hits where it is drawn. */
    label_rect(&u, "btn17", &x, &y, &w, &h);
    hit_wrapped = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_wrap);
    ok(hit_wrapped == 1, "a button on a wrapped row hits (got %d)", hit_wrapped);
}

static void test_hex_columns(void)
{
    int i, bad = 0;
    for (i = 0; i < 16; i++) {
        if (col_byte(hex_col(i)) != i) bad++;
        if (col_byte(hex_col(i) + 1) != i) bad++;    /* second nibble */
        if (col_byte(asc_col(i)) != i) bad++;
    }
    ok(!bad, "every byte column maps back to its own index (%d bad)", bad);
    ok(col_byte(0) == -1 && col_byte(5) == -1, "the offset column selects nothing");
    ok(col_byte(80) == -1, "past the ascii gutter selects nothing");
}

static void test_hex_click(void)
{
    ui_ctx u;
    float x = 0.0f, y = 0.0f;
    int i, bad = 0;
    float line_h;

    for (i = 0; i < 256; i++) { g_data[i] = (unsigned char)i; g_mark[i] = (unsigned char)i; }

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_hex);
    line_h = u.chh + 1.0f;

    /* Row 0's text carries the base offset, which is where the dump starts. */
    ok(text_at(&u, "0040  00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f "
                   " |................|", &x, &y),
       "the first hex row renders with its offset, gap and ascii gutter");

    for (i = 0; i < 256; i++) {
        float cx = x + ((float)hex_col(i % 16) + 0.5f) * u.chw;
        float cy = y + (float)(i / 16) * line_h + 2.0f;
        g_hexhit = -1;
        click_frame(&u, cx, cy, scene_hex);
        if (g_hexhit != i) {
            if (!bad) printf("      first mismatch: clicked byte %d, got %d\n",
                             i, g_hexhit);
            bad++;
        }
    }
    ok(!bad, "all 256 hex cells report their own byte index (%d wrong)", bad);

    /* The ascii gutter selects the same byte as the hex column. */
    bad = 0;
    for (i = 0; i < 16; i++) {
        float cx = x + ((float)asc_col(i) + 0.5f) * u.chw;
        g_hexhit = -1;
        click_frame(&u, cx, y + 2.0f, scene_hex);
        if (g_hexhit != i) bad++;
    }
    ok(!bad, "the ascii gutter selects the same byte (%d wrong)", bad);

    /* A click left of the dump is not a selection. */
    g_hexhit = -1;
    click_frame(&u, x + 1.0f, y + 2.0f, scene_hex);
    ok(g_hexhit == -1, "clicking the offset column selects nothing");
}

static void test_hex_budget(void)
{
    ui_ctx u;
    int i;

    /* Worst case: every byte differs from the mark and a dword is selected,
     * which is the most rectangles this view can ever emit. */
    for (i = 0; i < 256; i++) { g_data[i] = (unsigned char)i; g_mark[i] = (unsigned char)~i; }
    g_hexsel = 132;

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_hex);

    ok(!u.overflow, "a fully-highlighted memory tab fits the draw buffer");
    ok(u.ncmds < UI_MAX_CMDS, "commands used: %d of %d", u.ncmds, UI_MAX_CMDS);
    ok(u.arena_used < UI_ARENA, "string arena used: %d of %d",
       u.arena_used, UI_ARENA);
    g_hexsel = -1;
    for (i = 0; i < 256; i++) g_mark[i] = (unsigned char)i;
}

static void test_drag(void)
{
    ui_ctx u;
    float x0, y0;

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_buttons);
    x0 = u.px; y0 = u.py;
    u.screen_w = 1920.0f; u.screen_h = 1080.0f;

    /* Press in the title bar, move, and the panel should follow. */
    ui_begin(&u, x0 + 100.0f, y0 + 6.0f, 1);
    scene_buttons(&u);
    ui_end(&u);
    ui_begin(&u, x0 + 160.0f, y0 + 46.0f, 1);
    scene_buttons(&u);
    ui_end(&u);

    ok(u.px == x0 + 60.0f && u.py == y0 + 40.0f,
       "dragging the title bar moves the panel (%.0f,%.0f -> %.0f,%.0f)",
       x0, y0, u.px, u.py);

    hit_alpha = 0;
    ui_begin(&u, u.px + 100.0f, u.py + 6.0f, 0);   /* release */
    scene_buttons(&u);
    ui_end(&u);
    ok(!u.dragging, "releasing ends the drag");
    ok(hit_alpha == 0, "a drag never also presses a button");

    /* Dragged far off the right edge, a grabbable strip must remain. */
    ui_begin(&u, u.px + 20.0f, u.py + 6.0f, 1);
    scene_buttons(&u);
    ui_end(&u);
    ui_begin(&u, 9000.0f, 9000.0f, 1);
    scene_buttons(&u);
    ui_end(&u);
    ok(u.px <= 1920.0f - 60.0f && u.py <= 1080.0f,
       "the panel cannot be dragged out of reach (%.0f,%.0f)", u.px, u.py);
}

static void test_close(void)
{
    ui_ctx u;
    float x = 0.0f, y = 0.0f;

    memset(&u, 0, sizeof u);
    idle_frame(&u, scene_buttons);
    ok(text_at(&u, "x", &x, &y), "the close box was drawn");

    ui_begin(&u, x + 1.0f, y + 3.0f, 1);
    scene_buttons(&u);
    ui_end(&u);
    ok(u.want_close, "clicking the close box asks to close");
    ok(!u.dragging, "closing does not start a drag instead");

    idle_frame(&u, scene_buttons);
    ok(!u.want_close, "want_close is per-frame, not sticky");
}

static void test_overflow_is_safe(void)
{
    ui_ctx u;
    int i;
    char name[32];

    memset(&u, 0, sizeof u);
    ui_begin(&u, -1000.0f, -1000.0f, 0);
    ui_panel_begin(&u, "X", NULL, 784.0f, 586.0f);
    ui_group(&u, "FLOOD");
    for (i = 0; i < 4000; i++) {
        snprintf(name, sizeof name, "flood-%04d", i);
        ui_button(&u, name);
    }
    ui_group_end(&u);
    ui_panel_end(&u);
    ui_end(&u);

    ok(u.overflow, "flooding the buffer is reported, not ignored");
    ok(u.ncmds <= UI_MAX_CMDS, "command count stays within bounds (%d)", u.ncmds);
    ok(u.arena_used <= UI_ARENA, "arena stays within bounds (%d)", u.arena_used);
    for (i = 0; i < u.ncmds; i++)
        if (u.cmds[i].kind == UI_TEXT && !u.cmds[i].text) {
            ok(0, "a text command was left with a null string");
            return;
        }
    ok(1, "every emitted text command still has a valid string");
}

/* ------------------------------------------------------------------ */
/* the real panel                                                      */

static panel_snap g_snap;
static int        g_have = 1;

static void scene_panel(ui_ctx *u) { panel_ui_build(u, &g_snap, g_have); }

/* Builds every tab in turn and checks it stays inside its own window. */
static void test_tabs_fit(void)
{
    ui_ctx u;
    int t;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.unit = 0x12345678;
    g_snap.peek_ok = 1;
    g_snap.mark_ok = 1;
    g_snap.nwatch = PANEL_WATCH;
    g_snap.spawn.hooked = 1;
    g_snap.spawn.tmpl_kind = HG_TMPL_PRIM;
    g_snap.spawn.mult = 10;
    strcpy(g_snap.name, "Marcus Fidelius");
    stub_log_text = "a log line long enough to be realistic in the pane";
    /*
     * Everything the Camera tab can show, on: it is the tallest tab, and
     * with these zero it drew a stub and this test passed while the real
     * one ran off the bottom of the frame in game.
     */
    g_snap.fp_avail = 1;
    g_snap.shoulder.installed = 1;
    g_snap.shoulder.on = 1;
    g_snap.shoulder.impulse_avail = 1;
    g_snap.shoulder.impulse_on = 1;
    g_snap.shoulder.zoom_patched = 1;
    g_snap.shoulder.collide = 1;
    g_snap.shoulder.have_world = 1;

    memset(&u, 0, sizeof u);
    for (t = 0; t < 8; t++) {
        float bottom, pw, ph;
        u.tab = t;
        idle_frame(&u, scene_panel);
        panel_ui_size(&u, &pw, &ph);
        bottom = u.cy - u.py;
        ok(!u.overflow, "tab %d does not overflow the draw buffer", t);
        ok(bottom <= ph, "tab %d content fits the window "
           "(%.0f of %.0f px)", t, bottom, ph);
        ok(u.ncmds < UI_MAX_CMDS, "tab %d commands %d/%d", t, u.ncmds,
           UI_MAX_CMDS);
    }

    /* And again with nothing resolved, which is how it looks at the menu. */
    memset(&g_snap, 0, sizeof g_snap);
    g_have = 0;
    for (t = 0; t < 8; t++) {
        float pw, ph;
        u.tab = t;
        idle_frame(&u, scene_panel);
        panel_ui_size(&u, &pw, &ph);
        ok(!u.overflow, "tab %d with no snapshot does not overflow", t);
        ok((u.cy - u.py) <= ph, "tab %d with no snapshot fits", t);
    }
    g_have = 1;
}

/*
 * The wiring test. Each of these buttons was, in the previous design,
 * connected to something that could not fire; asserting the call is what
 * stops that recurring silently.
 */
static void test_spawn_buttons(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.spawn.hooked = 1;
    g_snap.spawn.tmpl_kind = HG_TMPL_PRIM;
    g_snap.spawn.immediate = 1;
    g_snap.spawn.mult = 1;

    memset(&u, 0, sizeof u);
    u.tab = 3;
    idle_frame(&u, scene_panel);

    ok(label_rect(&u, "Fire 10", &x, &y, &w, &h), "Fire 10 is drawn");
    stub_queued = stub_queue_calls = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_queue_calls == 1 && stub_queued == 10,
       "Fire 10 queues exactly 10 spawns (%ld calls, %ld queued)",
       stub_queue_calls, stub_queued);

    u.tab = 3;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Fire 1", &x, &y, &w, &h);
    stub_queued = stub_queue_calls = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_queued == 1, "Fire 1 queues one (got %ld)", stub_queued);

    u.tab = 3;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Fire 100", &x, &y, &w, &h);
    stub_queued = stub_queue_calls = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_queued == 100, "Fire 100 queues a hundred (got %ld)", stub_queued);

    u.tab = 3;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Clear queue", &x, &y, &w, &h);
    stub_clear_calls = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_clear_calls == 1, "Clear queue clears once");

    u.tab = 3;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Piggyback", &x, &y, &w, &h);
    stub_immediate = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_immediate == 0, "Piggyback turns immediate mode off (got %d)",
       stub_immediate);

    u.tab = 3;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Immediate", &x, &y, &w, &h);
    stub_immediate = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_immediate == 1, "Immediate turns it back on (got %d)",
       stub_immediate);

    u.tab = 3;
    idle_frame(&u, scene_panel);
    label_rect(&u, "x10", &x, &y, &w, &h);
    stub_mult = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_mult == 10, "x10 sets the amplifier to 10 (got %u)", stub_mult);
}

/* The Spawn tab's job is to explain itself. Each state must say something. */
static void test_spawn_states(void)
{
    ui_ctx u;
    float x, y;

    memset(&u, 0, sizeof u);
    u.tab = 3;

    memset(&g_snap, 0, sizeof g_snap);
    idle_frame(&u, scene_panel);
    ok(text_at(&u, "Spawn hooks are NOT installed. Nothing here can work.",
               &x, &y), "un-hooked says the hooks are missing");

    g_snap.spawn.hooked = 1;
    idle_frame(&u, scene_panel);
    ok(text_at(&u, "No template yet. Nothing fires until the game spawns once.",
               &x, &y), "hooked but unarmed says it is waiting for a spawn");

    g_snap.spawn.tmpl_kind = HG_TMPL_PRIM;
    idle_frame(&u, scene_panel);
    ok(text_at(&u, "Armed from spawn primitive. Buttons fire immediately.",
               &x, &y), "armed says which hook armed it");

    /* A template on one thread and a pump on another must be called out. */
    g_snap.spawn.tmpl_tid = 111;
    g_snap.spawn.pump_tid = 222;
    idle_frame(&u, scene_panel);
    ok(text_at(&u, "Threads differ: replay is skipped rather than called from"
                   " the wrong one.", &x, &y),
       "a thread mismatch is reported, not hidden");
}

static void test_memory_buttons(void)
{
    ui_ctx u;
    float x, y, w, h;
    int i;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.peek_ok = 1;
    g_snap.peek_off = 0x40;
    g_snap.peek_addr = 0x0a000040;
    for (i = 0; i < PANEL_PEEKW; i++) g_snap.peek[i] = (unsigned char)i;
    /* A known dword at +0x10 of the window: 10 11 12 13 -> 0x13121110. */

    memset(&u, 0, sizeof u);
    u.tab = 2;
    idle_frame(&u, scene_panel);

    label_rect(&u, "+0x10", &x, &y, &w, &h);
    stub_peek_nudge = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_peek_nudge == 0x10, "+0x10 nudges the window by 0x10 (got %d)",
       stub_peek_nudge);

    u.tab = 2;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Top", &x, &y, &w, &h);
    stub_peek_set = 0xffff;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_peek_set == 0, "Top returns the window to offset 0");

    /* Select the dword at window offset 0x10 by clicking byte 16, then
     * check the poke buttons act on the offset that selection implies. */
    u.tab = 2;
    idle_frame(&u, scene_panel);
    if (!text_at(&u, "0050  10 11 12 13 14 15 16 17  18 19 1a 1b 1c 1d 1e 1f "
                     " |................|", &x, &y)) {
        ok(0, "the hex row holding byte 16 was drawn");
        return;
    }
    click_frame(&u, x + ((float)hex_col(0) + 0.5f) * u.chw, y + 2.0f,
                scene_panel);

    u.tab = 2;
    idle_frame(&u, scene_panel);
    ok(text_at(&u, "unit+0x50   @ 0x0a000050", &x, &y),
       "the selected dword reports the right offset and address");
    ok(text_at(&u, "319951120   0x13121110   319951120   0.0000", &x, &y),
       "and decodes the dword four ways");

    label_rect(&u, "Watch it", &x, &y, &w, &h);
    stub_watch_add = 0xffff;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_watch_add == 0x50, "Watch it watches unit+0x50 (got 0x%x)",
       stub_watch_add);

    u.tab = 2;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Set 0", &x, &y, &w, &h);
    stub_poke_calls = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_poke_calls == 1 && stub_poke_off == 0x50 && stub_poke_val == 0,
       "Set 0 pokes unit+0x50 with 0 (off 0x%x val %u)",
       stub_poke_off, stub_poke_val);

    u.tab = 2;
    idle_frame(&u, scene_panel);
    label_rect(&u, "+1", &x, &y, &w, &h);
    stub_poke_calls = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_poke_calls == 1 && stub_poke_val == 0x13121111u,
       "+1 pokes the selected value plus one (got 0x%x)", stub_poke_val);
}

static void test_memory_unreadable(void)
{
    ui_ctx u;
    float x, y;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.peek_ok = 0;
    g_snap.peek_off = 0x2000;

    memset(&u, 0, sizeof u);
    u.tab = 2;
    idle_frame(&u, scene_panel);
    ok(text_at(&u, "nothing to show: that window is not committed, or there is",
               &x, &y), "an unreadable window says so instead of showing stale bytes");
    ok(!text_at(&u, "0000  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00 "
                    " |................|", &x, &y),
       "and draws no hex dump at all");
}

static void test_other_tabs_wiring(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&g_snap, 0, sizeof g_snap);
    memset(&u, 0, sizeof u);

    u.tab = 0;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Reset counters", &x, &y, &w, &h);
    stub_reset_calls = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_reset_calls == 1, "Reset counters resets once");

    u.tab = 4;
    idle_frame(&u, scene_panel);
    label_rect(&u, "DISCRETE (1)", &x, &y, &w, &h);
    stub_simtype = -99;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_simtype == 1, "DISCRETE sets the simulation type to 1 (got %d)",
       stub_simtype);

    u.tab = 4;
    idle_frame(&u, scene_panel);
    label_rect(&u, "No override", &x, &y, &w, &h);
    stub_simtype = -99;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_simtype == -1, "No override clears it (got %d)", stub_simtype);

    u.tab = 1;
    idle_frame(&u, scene_panel);
    label_rect(&u, "Peek at name", &x, &y, &w, &h);
    stub_peek_set = 0xffff;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_peek_set == 0x120, "Peek at name jumps to +0x120 (got 0x%x)",
       stub_peek_set);
    ok(u.tab == 2, "and switches to the Memory tab (got %d)", u.tab);
}

/* ------------------------------------------------------------------ */
/* ascii preview                                                       */

/*
 * `uitest --dump` renders each tab as text, by mapping the draw commands
 * back onto the character grid they were laid out on. It is not a substitute
 * for seeing it in game, but it turns "is the Spawn tab a mess" into a
 * question answerable in a second rather than a Proton launch, and it is how
 * the layout below was actually tuned.
 */
#define DUMP_COLS 118
#define DUMP_ROWS 44

static void dump_tab(ui_ctx *u, int tab, const char *name)
{
    static char canvas[DUMP_ROWS][DUMP_COLS + 1];
    int r, c, i;
    float dump_w, dump_h;

    u->tab = tab;
    idle_frame(u, scene_panel);
    panel_ui_size(u, &dump_w, &dump_h);

    for (r = 0; r < DUMP_ROWS; r++) {
        for (c = 0; c < DUMP_COLS; c++) canvas[r][c] = ' ';
        canvas[r][DUMP_COLS] = 0;
    }

    /* Boxes first: anything button- or tile-sized gets an outline. */
    for (i = 0; i < u->ncmds; i++) {
        const ui_cmd *k = &u->cmds[i];
        int c0, c1, r0, r1;
        if (k->kind != UI_RECT) continue;
        if (k->w < 3.0f * u->chw || k->h < 0.8f * u->chh) continue;
        if (k->w > 0.98f * dump_w && k->h > 0.5f * dump_h) continue;
        c0 = (int)((k->x - u->px) / u->chw);
        c1 = (int)((k->x + k->w - u->px) / u->chw);
        r0 = (int)((k->y - u->py) / u->chh);
        r1 = (int)((k->y + k->h - u->py) / u->chh);
        if (c0 < 0 || r0 < 0 || c1 >= DUMP_COLS || r1 >= DUMP_ROWS) continue;
        for (c = c0; c <= c1; c++) {
            canvas[r0][c] = '-';
            canvas[r1][c] = '-';
        }
        for (r = r0; r <= r1; r++) {
            canvas[r][c0] = '|';
            canvas[r][c1] = '|';
        }
        canvas[r0][c0] = canvas[r0][c1] = '+';
        canvas[r1][c0] = canvas[r1][c1] = '+';
    }

    /* Then the text, which wins wherever they collide. */
    for (i = 0; i < u->ncmds; i++) {
        const ui_cmd *k = &u->cmds[i];
        int len, j;
        if (k->kind != UI_TEXT || !k->text) continue;
        c = (int)((k->x - u->px) / u->chw);
        r = (int)((k->y - u->py + u->chh * 0.4f) / u->chh);
        if (r < 0 || r >= DUMP_ROWS) continue;
        len = (int)strlen(k->text);
        for (j = 0; j < len && c + j < DUMP_COLS; j++)
            if (c + j >= 0) canvas[r][c + j] = k->text[j];
    }

    printf("\n=== tab %d: %s ===\n", tab, name);
    for (r = 0; r < DUMP_ROWS; r++) {
        int last = DUMP_COLS - 1;
        while (last >= 0 && canvas[r][last] == ' ') last--;
        canvas[r][last + 1] = 0;
        printf("%s\n", canvas[r]);
    }
}

static void dump_all(void)
{
    static const char *const NAMES[8] = {
        "Live", "Player", "Memory", "Spawn", "Physics", "Camera",
        "Model", "Log"
    };
    ui_ctx u;
    int t, i;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.cam_mode = 6; g_snap.fp_avail = 1;
    strcpy(g_snap.name, "Marcus Fidelius");
    g_snap.unit = 0x0a3f1c40;
    g_snap.flags = 0x00000142;
    g_snap.peek_ok = 1;
    g_snap.peek_off = 0x40;
    g_snap.peek_addr = 0x0a3f1c80;
    for (i = 0; i < PANEL_PEEKW; i++) g_snap.peek[i] = (unsigned char)(i * 7 + 3);
    memcpy(g_snap.peek + 0x20, "Marcus", 6);
    g_snap.mark_ok = 1;
    memcpy(g_snap.mark, g_snap.peek, PANEL_PEEKW);
    g_snap.mark[9] ^= 0xff;
    g_snap.nwatch = 2;
    g_snap.watch_off[0] = 0x110; g_snap.watch_val[0] = 0x142; g_snap.watch_ok[0] = 1;
    g_snap.watch_off[1] = 0x2c0; g_snap.watch_ok[1] = 0;
    g_snap.qray = 6177; g_snap.rays = 412; g_snap.bodies = 137;
    g_snap.qms = 5.6; g_snap.dtavg = 0.0141f;
    g_snap.dtmin = 0.0092f; g_snap.dtmax = 0.0510f;
    g_snap.pumps = 918342;
    g_snap.simtype_seen = 2; g_snap.simtype_override = -1;
    g_snap.hist_n = PANEL_HIST;
    for (i = 0; i < PANEL_HIST; i++) {
        /* A plausible shape: quiet, then a stall arriving. */
        float t = (float)i / (float)PANEL_HIST;
        g_snap.hist_qms[i] = 3.0f + 2.0f * (float)((i * 7) % 5)
                           + (i > 48 ? (float)(i - 48) * 3.4f : 0.0f);
        g_snap.hist_dt[i]  = 12.0f + 4.0f * t
                           + (i > 52 ? (float)(i - 52) * 9.0f : 0.0f);
    }
    g_snap.spawn.hooked = 1;
    g_snap.spawn.tmpl_kind = HG_TMPL_PRIM;
    g_snap.spawn.seen_prim = 63; g_snap.spawn.seen_script = 0;
    g_snap.spawn.fired = 110; g_snap.spawn.queued = 0;
    g_snap.spawn.immediate = 1; g_snap.spawn.mult = 1;
    g_snap.spawn.tmpl_tid = 412; g_snap.spawn.pump_tid = 412;
    stub_log_text = "spawn: template captured from spawn primitive on tid 412";

    memset(&u, 0, sizeof u);
    for (t = 0; t < 8; t++) dump_tab(&u, t, NAMES[t]);
}

/* Log lines are longer than the window; DT_NOCLIP would run them over the
 * game, so the tab must cut them. */
static void test_log_truncation(void)
{
    ui_ctx u;
    float pw, ph;
    int i, longest = 0;

    memset(&g_snap, 0, sizeof g_snap);
    stub_log_text = "spawn: an extremely long log line of the sort logf_ "
                    "actually emits, well past the width of the panel window "
                    "and then some more besides to be sure of it";

    memset(&u, 0, sizeof u);
    u.tab = 5;
    idle_frame(&u, scene_panel);
    panel_ui_size(&u, &pw, &ph);

    for (i = 0; i < u.ncmds; i++) {
        const ui_cmd *k = &u.cmds[i];
        int end;
        if (k->kind != UI_TEXT || !k->text) continue;
        end = (int)(k->x + (float)strlen(k->text) * u.chw - u.px);
        if (end > longest) longest = end;
    }
    ok((float)longest <= pw, "no log line runs past the window edge "
       "(%d of %.0f px)", longest, pw);
}

/*
 * The camera tab's whole reason to exist is that the engine overrides the
 * request; so at minimum the buttons must ask for the mode they name.
 */
static void test_camera_tab(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.cam_mode = 6;
    g_snap.fp_avail = 1;
    g_snap.fp_melee = 0;

    memset(&u, 0, sizeof u);
    u.tab = 5;
    idle_frame(&u, scene_panel);

    ok(label_rect(&u, "First person", &x, &y, &w, &h), "First person is drawn");
    stub_cam_req = -99;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_cam_req == HG_CAM_FIRST, "First person requests mode 0 (got %d)",
       stub_cam_req);

    u.tab = 5; idle_frame(&u, scene_panel);
    label_rect(&u, "Third person", &x, &y, &w, &h);
    stub_cam_req = -99;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_cam_req == HG_CAM_THIRD, "Third person requests mode 6 (got %d)",
       stub_cam_req);

    u.tab = 5; idle_frame(&u, scene_panel);
    label_rect(&u, "Restore", &x, &y, &w, &h);
    stub_cam_req = -99;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_cam_req == HG_CAM_RESTORE, "Restore requests the restore path (got %d)",
       stub_cam_req);

    /* The unlock toggle reports the state it is in, and flips it. */
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "Locked (stock)", &x, &y, &w, &h),
       "the unlock reads Locked while stock");
    stub_fp_melee = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_fp_melee == 1, "clicking it turns the unlock on (got %d)",
       stub_fp_melee);

    g_snap.fp_melee = 1;
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "Unlocked", &x, &y, &w, &h),
       "and reads Unlocked once on");
    stub_fp_melee = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_fp_melee == 0, "clicking again turns it off (got %d)", stub_fp_melee);

    /* With the hook missing the tab must say so, not offer a dead button. */
    g_snap.fp_avail = 0;
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(text_at(&u, "Unavailable - CanUseFirstPerson was not hooked this session.",
               &x, &y), "an un-hooked session says so instead of offering a toggle");
}

/*
 * A fake world for cam_compose: a wall at x = wall_x (facing -x) and a wall
 * at y = side_y (facing -y). Rays report the free distance to whichever
 * they reach first, like the game's cast.
 */
static float g_wall_x = 1e9f, g_side_y = 1e9f;
static int   g_rays;

static float fake_ray(void *ctx, const float *o, const float *d, float len)
{
    float t = len;
    (void)ctx;
    g_rays++;
    if (d[0] > 1e-6f) { float h = (g_wall_x - o[0]) / d[0]; if (h >= 0 && h < t) t = h; }
    if (d[1] > 1e-6f) { float h = (g_side_y - o[1]) / d[1]; if (h >= 0 && h < t) t = h; }
    return t;
}

static int near3(const float *v, float x, float y, float z)
{
    return fabsf(v[0] - x) < 1e-3f && fabsf(v[1] - y) < 1e-3f && fabsf(v[2] - z) < 1e-3f;
}

static void test_action_camera(void)
{
    /*
     * The game's camera for a player at the origin looking down -x... i.e.
     * the eye sits at +x behind them: look-at 0.2 ahead at head height,
     * eye 1.75 behind, level. So the orbit direction u = +x.
     */
    const float at0[3]  = { -0.2f, 0.0f, 1.8f };
    const float eye0[3] = { 1.75f, 0.0f, 1.8f };
    cam_params p = { 1.0f, 0.6f, 0.0f, 0.0f, 1.75f, 0.0f, 0.0f, 0, 0.0f };
    cam_state st = { 0, 0, 0 };
    float eye[3], at[3], f;

    g_wall_x = g_side_y = 1e9f;
    f = cam_compose(eye0, at0, &p, &st, 0.016f, fake_ray, NULL, eye, at);
    /* fwd = -u = -x; right = (-fwd.y, fwd.x) = (0, -1). */
    ok(near3(at, -0.2f, -0.6f, 1.8f) && near3(eye, 1.75f, -0.6f, 1.8f),
       "open ground: both points shift 0.6 to the right, eye at full boom "
       "(at %.2f,%.2f,%.2f eye %.2f,%.2f,%.2f)", at[0], at[1], at[2], eye[0], eye[1], eye[2]);
    ok(f > 0.999f, "and the whole offset is in use (%.3f)", f);

    {   /* The game had already pulled its eye in; we rebuild full length. */
        const float eye1[3] = { 0.5f, 0.0f, 1.8f };
        cam_state s2 = { 0, 0, 0 };
        cam_compose(eye1, at0, &p, &s2, 0.016f, fake_ray, NULL, eye, at);
        ok(fabsf(eye[0] - 1.75f) < 1e-3f,
           "a boom the game shortened is rebuilt from the zoom distance (%.3f)", eye[0]);
    }

    /* A wall behind: snap in at once, with the margin. */
    g_wall_x = 1.0f;
    cam_compose(eye0, at0, &p, &st, 0.016f, fake_ray, NULL, eye, at);
    ok(fabsf(eye[0] - (1.0f - CAM_WALL_MARGIN)) < 1e-3f,
       "a wall behind pulls the eye in immediately (%.3f)", eye[0]);

    /* Wall gone: ease out, not snap. */
    g_wall_x = 1e9f;
    cam_compose(eye0, at0, &p, &st, 0.016f, fake_ray, NULL, eye, at);
    ok(eye[0] > 0.75f && eye[0] < 1.75f, "and eases back out when it lets go (%.3f)", eye[0]);
    cam_compose(eye0, at0, &p, &st, 0.25f, fake_ray, NULL, eye, at);
    ok(fabsf(eye[0] - 1.75f) < 1e-3f, "reaching full length given time (%.3f)", eye[0]);

    /* A wall on the shoulder side (at y = -0.45; ray goes toward -y). */
    {
        cam_state s3 = { 0, 0, 0 };
        cam_params pl = p;
        pl.side = -1.0f;                    /* left shoulder = +y */
        g_side_y = 0.45f;
        f = cam_compose(eye0, at0, &pl, &s3, 0.016f, fake_ray, NULL, eye, at);
        ok(fabsf(at[1] - (0.45f - CAM_WALL_MARGIN)) < 1e-3f,
           "a wall on the shoulder side limits the offset (%.3f)", at[1]);
        ok(f < 0.5f, "and the panel is told how much was lost (%.3f)", f);
        g_side_y = 1e9f;
    }

    /* Dynamic pitch lifts the look-at only; height moves both. */
    {
        cam_state s4 = { 0, 0, 0 };
        cam_params ph = p;
        ph.height = -0.5f; ph.lift = 1.0f;
        cam_compose(eye0, at0, &ph, &s4, 0.016f, fake_ray, NULL, eye, at);
        ok(fabsf(eye[2] - 1.3f) < 1e-3f && fabsf(at[2] - 2.3f) < 1e-3f,
           "height lowers the eye and pivot, lift raises only the look-at "
           "(eye z %.2f, at z %.2f)", eye[2], at[2]);
    }

    /* No world: the fallback scales by the game's own collided length. */
    {
        const float eye1[3] = { 0.775f, 0.0f, 1.8f };   /* half the boom */
        cam_state s5 = { 0, 0, 0 };
        g_rays = 0;
        f = cam_compose(eye1, at0, &p, &s5, 0.016f, NULL, NULL, eye, at);
        ok(g_rays == 0 && fabsf(f - 0.5f) < 1e-3f && fabsf(at[1] - -0.3f) < 1e-3f,
           "without a world it casts nothing and halves the offset (%.3f, %.3f)",
           f, at[1]);
    }

    /* Melee impulse: pushes the eye in along the boom and dips the view. */
    {
        cam_state s6 = { 0, 0, 0 };
        cam_params pi = p;
        pi.push = 0.25f; pi.dip = 0.06f;
        cam_compose(eye0, at0, &pi, &s6, 0.016f, fake_ray, NULL, eye, at);
        ok(fabsf(eye[0] - 1.5f) < 1e-3f && fabsf(at[2] - 1.74f) < 1e-3f,
           "a push shortens the boom and a dip lowers the look-at (%.3f, %.3f)",
           eye[0], at[2]);
        pi.push = 5.0f;
        cam_compose(eye0, at0, &pi, &s6, 0.016f, fake_ray, NULL, eye, at);
        ok(fabsf(eye[0] - (-0.2f + 0.3f)) < 1e-3f,
           "and can never push the eye through the player (%.3f)", eye[0]);
    }
    ok(cam_impulse(0.0f) == 0.0f, "the impulse starts at rest, so a new swing never jumps");
    ok(fabsf(cam_impulse(CAM_IMP_ATTACK) - 1.0f) < 1e-4f, "it peaks at the end of the attack");
    ok(cam_impulse(CAM_IMP_ATTACK + 0.5f) < 0.1f, "and has mostly settled half a second on");
    ok(cam_impulse(CAM_IMP_END + 0.1f) == 0.0f && cam_impulse(-1.0f) == 0.0f,
       "outside its window it is exactly zero");

    /*
     * True orbit. Mouse pitch 300 deg (-60, looking down): the stock camera
     * would sit 1.95 back and dist*sin60 up; the orbit puts it on a sphere.
     */
    {
        const float eyeS[3] = { 1.75f, 0.0f, 1.8f + 1.75f * 0.8660254f };
        cam_state s7 = { 0, 0, 0 };
        cam_params po = p;
        float dx, dz;
        po.side = 0.0f; po.orbit = 1; po.pitch = 300.0f / 57.29578f;
        cam_compose(eyeS, at0, &po, &s7, 0.016f, fake_ray, NULL, eye, at);
        dx = eye[0] - at[0]; dz = eye[2] - at[2];
        ok(fabsf(atan2f(dz, dx) * 57.29578f - 60.0f) < 0.1f &&
           fabsf(sqrtf(dx * dx + dz * dz) - 1.95f) < 1e-3f,
           "true orbit: view pitch equals mouse pitch, on a 1.95 sphere (%.1f deg, %.3f)",
           atan2f(dz, dx) * 57.29578f, sqrtf(dx * dx + dz * dz));
        po.pitch = 85.0f / 57.29578f;       /* looking up: eye below the head */
        cam_compose(eyeS, at0, &po, &s7, 0.25f, fake_ray, NULL, eye, at);
        ok(eye[2] < at[2] - 1.9f, "and looking up puts the eye below the pivot (%.2f)",
           eye[2] - at[2]);
    }

    ok(cam_zoom_blend(0.2f, -0.5f, 1.5f, 10.0f) == 0.2f, "closest zoom takes the close value");
    ok(cam_zoom_blend(0.2f, -0.5f, 10.0f, 10.0f) == -0.5f, "farthest zoom takes the far value");
    ok(fabsf(cam_zoom_blend(0.0f, -0.7f, 3.25f, 5.0f) - -0.35f) < 1e-5f,
       "halfway through the range is halfway between");
    ok(cam_zoom_blend(0.2f, -0.5f, 0.5f, 10.0f) == 0.2f &&
       cam_zoom_blend(0.2f, -0.5f, 30.0f, 10.0f) == -0.5f, "and it clamps");

    ok(shoulder_ease(1.0f, -1.0f, 8.0f, 0.0f) == 1.0f, "no time, no movement");
    ok(fabsf(shoulder_ease(1.0f, -1.0f, 8.0f, 0.0625f) - 0.0f) < 1e-5f,
       "halfway after half the time constant");
    ok(shoulder_ease(1.0f, -1.0f, 8.0f, 0.25f) == -1.0f,
       "a long frame lands on the target instead of overshooting");
}

static void test_altlatch(void)
{
    al_state s;
    int ch;

    memset(&s, 0, sizeof s);
    /* A plain hold: everything passes, nothing latches. */
    ok(al_down(&s, 10.0) == AL_PASS && al_up(&s, 11.0, &ch) == AL_PASS && !s.latched,
       "holding Alt behaves exactly as stock");

    /* One tap: passes, arms. */
    ok(al_down(&s, 20.0) == AL_PASS && al_up(&s, 20.1, &ch) == AL_PASS && !s.latched,
       "a single tap passes through and does not latch");
    /* Second tap soon after: the down passes, the up is swallowed. */
    ok(al_down(&s, 20.3) == AL_PASS, "the second press reaches the game");
    ok(al_up(&s, 20.4, &ch) == AL_SWALLOW && s.latched && ch,
       "the second release is swallowed and latches");
    ok(al_repeat(&s) == AL_SWALLOW, "auto-repeat is hidden while latched");

    /* Release tap: down swallowed (game already holds it), up passes. */
    ok(al_down(&s, 30.0) == AL_SWALLOW, "the release press is swallowed");
    ok(al_up(&s, 30.1, &ch) == AL_PASS && !s.latched && ch,
       "and its release reaches the game, unlatching");

    /* Two taps too far apart do not latch. */
    al_down(&s, 40.0); al_up(&s, 40.1, &ch);
    al_down(&s, 41.5);
    ok(al_up(&s, 41.6, &ch) == AL_PASS && !s.latched, "slow taps do not latch");

    /* A hold between taps breaks the sequence. */
    memset(&s, 0, sizeof s);
    al_down(&s, 50.0); al_up(&s, 50.1, &ch);
    al_down(&s, 50.2); al_up(&s, 51.0, &ch);          /* held 0.8s */
    ok(!s.latched, "tap then hold is not a double tap");

    /* Focus loss while latched hands back a release. */
    memset(&s, 0, sizeof s);
    al_down(&s, 60.0); al_up(&s, 60.1, &ch);
    al_down(&s, 60.2); al_up(&s, 60.3, &ch);
    ok(s.latched && al_focus_lost(&s) && !s.latched,
       "losing focus while latched releases and says so");
}

static void test_shoulder_panel(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.cam_mode = 6;
    g_snap.fp_avail = 1;
    g_snap.shoulder.installed = 1;
    g_snap.shoulder.on = 1;
    g_snap.shoulder.right = 1;
    g_snap.shoulder.offset_mm = 600;
    g_snap.shoulder.hedge_pct = 100;

    memset(&u, 0, sizeof u);
    u.tab = 5;
    idle_frame(&u, scene_panel);
    ok(label_rect(&u, "On", &x, &y, &w, &h), "the shoulder toggle reads On");
    stub_sh_on = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_sh_on == 0, "clicking it turns the offset off (got %d)", stub_sh_on);

    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "Right -> left", &x, &y, &w, &h), "swap names the direction");
    stub_sh_swaps = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_sh_swaps == 1, "and swaps once (got %d)", stub_sh_swaps);

    stub_sh_doff = stub_sh_dh = stub_sh_dfar = 0;
    u.tab = 5; idle_frame(&u, scene_panel);
    label_rect(&u, "+100", &x, &y, &w, &h);
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "close down", &x, &y, &w, &h), "close height has its own buttons");
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "far up", &x, &y, &w, &h), "and so does far height");
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_sh_doff == 100 && stub_sh_dh == -100 && stub_sh_dfar == 100,
       "+100 widens, close down lowers close only, far up raises far only (%d, %d, %d)",
       stub_sh_doff, stub_sh_dh, stub_sh_dfar);

    stub_sh_dlift = stub_sh_dzoom = 0;
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "lift +", &x, &y, &w, &h), "pitch lift has buttons");
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "zoom +1", &x, &y, &w, &h), "and so does max zoom");
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_sh_dlift == 200 && stub_sh_dzoom == 1,
       "lift + raises the lift, zoom +1 raises the ceiling (%d, %d)",
       stub_sh_dlift, stub_sh_dzoom);

    g_snap.shoulder.impulse_avail = 1;
    g_snap.shoulder.impulse_on = 1;
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "Melee impulse", &x, &y, &w, &h), "the impulse toggle is drawn");
    stub_imp_on = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_imp_on == 0, "and turns it off (got %d)", stub_imp_on);
    u.tab = 5; idle_frame(&u, scene_panel);
    label_rect(&u, "impulse +", &x, &y, &w, &h);
    stub_imp_d = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_imp_d == 25, "impulse + strengthens it (got %d)", stub_imp_d);

    g_snap.shoulder.orbit = 1;
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "True orbit", &x, &y, &w, &h), "the orbit toggle is drawn");
    stub_orbit = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_orbit == 0, "and switches back to stock (got %d)", stub_orbit);

    g_snap.shoulder.collide = 1;
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(label_rect(&u, "Own collision", &x, &y, &w, &h), "collision toggle reads its state");
    stub_sh_collide = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_sh_collide == 0, "and flips it (got %d)", stub_sh_collide);

    g_snap.shoulder.installed = 0;
    u.tab = 5; idle_frame(&u, scene_panel);
    ok(text_at(&u, "Unavailable - CameraUpdate was not hooked.", &x, &y),
       "an un-hooked session says so");
}

/* The bit poker is the whole point of the Model tab: it must send the bit
 * it displays, or walking the bits in game means nothing. */
static void test_viewmodel_tab(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.model_third = 42;
    memset(&u, 0, sizeof u);
    u.tab = 6;
    idle_frame(&u, scene_panel);

    ok(label_rect(&u, "3rd model -> FP projection", &x, &y, &w, &h),
       "the FP-projection shortcut is drawn");
    stub_bit = stub_bitval = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_bit == HG_MODEL_FP_PROJ && stub_bitval == 1,
       "it sets bit %d to 1 (got bit %d = %d)", HG_MODEL_FP_PROJ,
       stub_bit, stub_bitval);

    /* Step the bit up twice, then Set 1 must send the stepped value. */
    u.tab = 6; idle_frame(&u, scene_panel);
    label_rect(&u, "+", &x, &y, &w, &h);
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    u.tab = 6; idle_frame(&u, scene_panel);
    label_rect(&u, "+", &x, &y, &w, &h);
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    u.tab = 6; idle_frame(&u, scene_panel);
    label_rect(&u, "Set 1", &x, &y, &w, &h);
    stub_bit = -1;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_bit == HG_MODEL_FP_PROJ + 2,
       "Set 1 sends the displayed bit after stepping (got %d)", stub_bit);
    ok(stub_bitval == 1, "and the value it names");
}

static void test_fart_button(void)
{
    ui_ctx u;
    float x, y, w, h;

    memset(&g_snap, 0, sizeof g_snap);
    memset(&u, 0, sizeof u);
    u.tab = 1;
    idle_frame(&u, scene_panel);
    ok(label_rect(&u, "Fart", &x, &y, &w, &h), "the fart button is drawn");
    stub_farts = 0;
    click_frame(&u, x + w / 2.0f, y + h / 2.0f, scene_panel);
    ok(stub_farts == 1, "clicking it farts exactly once (got %d)", stub_farts);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) { dump_all(); return 0; }
    if (argc > 2 && strcmp(argv[1], "--fart") == 0) {
        const unsigned char *w = NULL;
        unsigned int n = hg_fart_render(&w, (unsigned int)time(NULL));
        FILE *f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 1; }
        fwrite(w, 1, n, f);
        fclose(f);
        printf("wrote %s  (%u bytes, %u ms)\n", argv[2], n,
               (n - 44) / 2 * 1000 / FART_SR);
        return 0;
    }

    test_button_hit();
    test_button_edges();
    test_one_click_one_widget();
    test_tabs();
    test_wrap();
    test_hex_columns();
    test_hex_click();
    test_hex_budget();
    test_drag();
    test_close();
    test_overflow_is_safe();
    test_tabs_fit();
    test_spawn_buttons();
    test_spawn_states();
    test_memory_buttons();
    test_memory_unreadable();
    test_other_tabs_wiring();
    test_log_truncation();
    test_camera_tab();
    test_action_camera();
    test_altlatch();
    test_shoulder_panel();
    test_viewmodel_tab();
    test_fart_button();

    printf("\n%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS",
           g_run, g_fail);
    return g_fail ? 1 : 0;
}
