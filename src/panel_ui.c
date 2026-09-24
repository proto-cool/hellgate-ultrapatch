/*
 * The dev panel's contents: what each tab shows and what each button does.
 *
 * Deliberately free of D3D and of windows.h. overlay.c owns the device, the
 * font and the cursor; this file owns the panel, and test/ui.c builds every
 * tab of it natively to check that the buttons are wired to what their
 * labels claim and that nothing spills out of the window.
 *
 * Everything here runs on the render thread, so it only ever reads the
 * snapshot panel_pump() publishes and writes through the interlocked
 * setters in panel.h. Nothing in this file touches game memory.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "panel_ui.h"

static int g_sel = -1;    /* selected byte in the hex view, or -1 */
static int g_marked;      /* a baseline has been asked for        */
static int g_flagbit = HG_MODEL_FP_PROJ;   /* model bit being poked */

void panel_ui_size(const ui_ctx *u, float *w, float *h)
{
    float chw = u->chw > 0.0f ? u->chw : 7.0f;
    float chh = u->chh > 0.0f ? u->chh : 15.0f;

    /* the sidebar, then 83 columns of content (the 74-column hex dump plus
     * its group indents needs 78; a setting row's label, bar and value 80) */
    *w = (float)(PANEL_NAV_COLS + 83) * chw + 72.0f;
    if (*w < 700.0f) *w = 700.0f;

    /*
     * The tallest page, Camera, measured (uitest --heights): 884 px at a
     * 7x15 cell and 1339 at 14x28, so 35 cells plus 359 px of fixed
     * padding; a little more, and never taller than the screen.
     */
    *h = 35.0f * chh + 380.0f;
    if (u->screen_h > 0.0f && *h > u->screen_h - 20.0f) *h = u->screen_h - 20.0f;
    if (*h < 540.0f) *h = 540.0f;
}

void panel_ui_toggle_mark(void)
{
    g_marked = !g_marked;
    panel_peek_mark(g_marked);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

/*
 * Rotating scratch buffers, so a value can be formatted inline in a call
 * that takes a string. Eight is more than any one widget row uses.
 */
static const char *fmtv(const char *fmt, ...)
{
    static char buf[8][64];
    static int  slot;
    char *o = buf[slot & 7];
    va_list ap;

    slot++;
    va_start(ap, fmt);
    vsnprintf(o, sizeof buf[0], fmt, ap);
    va_end(ap);
    return o;
}

/*
 * Reinterpret a dword as a float. Via memcpy, not a cast through a float*:
 * at -O2 the cast is a strict-aliasing violation and GCC says so.
 */
static float as_float(unsigned int v)
{
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static const char *camname(int m)
{
    switch (m) {
    case 0:  return "FIRST PERSON";
    case 5:  return "mode 5";
    case 6:  return "third person";
    case 7:  return "mode 7";
    case 8:  return "mode 8";
    case 9:  return "mode 9";
    case 17: return "mode 17";
    case 20: return "mode 20";
    default: return "unknown";
    }
}

static const char *simname(int t)
{
    switch (t) {
    case 0:  return "INVALID";
    case 1:  return "DISCRETE";
    case 2:  return "CONTINUOUS";
    case 3:  return "MULTITHREADED";
    default: return "unknown";
    }
}

static const char *tmplname(int k)
{
    switch (k) {
    case HG_TMPL_PRIM:       return "spawn primitive";
    case HG_TMPL_SCRIPT_OBJ: return "SpawnObject";
    case HG_TMPL_SCRIPT_MON: return "SpawnMonsterNearby";
    default:                 return "none";
    }
}

/* ------------------------------------------------------------------ */
/* tab contents                                                        */

static void tab_live(ui_ctx *u, const panel_snap *s, int have)
{
    ui_group(u, "FRAME");
    ui_tile(u, "ms avg", fmtv("%.1f", s->dtavg * 1000.0f),
            s->dtavg > 0.033f ? UI_C_BAD : UI_C_OK);
    ui_tile(u, "ms min", fmtv("%.1f", s->dtmin * 1000.0f), UI_C_DIM);
    ui_tile(u, "ms max", fmtv("%.1f", s->dtmax * 1000.0f),
            s->dtmax > 0.1f ? UI_C_BAD : UI_C_DIM);
    ui_tile(u, "pumps", fmtv("%ld", s->pumps), UI_C_DIM);
    ui_group_end(u);

    ui_group(u, "HAVOK / MOPP  (per 100ms window)");
    ui_tile(u, "bodies", fmtv("%ld", s->bodies), UI_C_ACCENT);
    ui_tile(u, "rays", fmtv("%ld", s->rays), UI_C_ACCENT);
    ui_tile(u, "mopp nodes", fmtv("%ld", s->qray),
            s->qray > 50000 ? UI_C_BAD : UI_C_ACCENT);
    ui_tile(u, "mopp ms", fmtv("%.1f", s->qms),
            s->qms > 33.0 ? UI_C_BAD : UI_C_ACCENT);
    ui_tile(u, "nodes/ray", fmtv("%.1f", s->rays ? (double)s->qray / s->rays : 0.0),
            UI_C_DIM);
    ui_newline(u);
    if (s->qms > 33.0 || s->qray > 50000)
        ui_text(u, UI_C_BAD, "SPIKE: this window is pathological, not busy.");
    ui_group_end(u);

    /*
     * The whole investigation is about an excursion in these two numbers, so
     * the tab that gets looked at most shows them over time rather than only
     * right now. At one sample per 100ms window this is the last 6 seconds.
     */
    ui_group(u, "HISTORY");
    if (s->hist_n > 1) {
        ui_graph(u, "mopp ms/window", s->hist_qms, s->hist_n, 10.0f, 4,
                 UI_C_ACCENT);
        ui_graph(u, "frame ms", s->hist_dt, s->hist_n, 20.0f, 4, UI_C_OK);
    } else {
        ui_text(u, UI_C_DIM,
                "no history yet - one sample arrives per 100ms window");
    }
    ui_group_end(u);

    ui_group(u, "COUNTERS");
    if (ui_button(u, "Reset counters")) hg_counters_reset();
    ui_newline(u);
    ui_text(u, UI_C_DIM, "Body count is live world state and is left alone.");
    ui_group_end(u);

    if (!have)
        ui_text(u, UI_C_WARN,
                "snapshot stale - the game thread is not pumping (menu or load?)");
}

static void tab_player(ui_ctx *u, const panel_snap *s)
{
    int i;

    ui_group(u, "LOCAL PLAYER");
    if (s->unit) {
        ui_kv(u, "name", UI_C_OK, "%s", s->name[0] ? s->name : "(unnamed)");
        ui_kv(u, "unit", UI_C_TEXT, "0x%08x", s->unit);
        ui_kv(u, "flags +0x110", UI_C_TEXT, "0x%08x", s->flags);
        ui_kv(u, "  bit 0x100", s->flags & 0x100 ? UI_C_OK : UI_C_DIM, "%s",
              s->flags & 0x100 ? "set" : "clear");
    } else {
        ui_text(u, UI_C_WARN, "no unit - not in a game");
    }
    ui_newline(u);
    if (ui_button(u, "Peek at +0x000")) { panel_peek_set(0); u->tab = PG_MEMORY; }
    if (ui_button(u, "Peek at flags"))  { panel_peek_set(0x100); u->tab = PG_MEMORY; }
    if (ui_button(u, "Peek at name"))   { panel_peek_set(0x120); u->tab = PG_MEMORY; }
    ui_group_end(u);

    ui_group(u, "WATCHES");
    ui_text(u, UI_C_DIM,
            "Re-read every pump. Add from Memory: select a dword, then Watch.");
    for (i = 0; i < s->nwatch && i < PANEL_WATCH; i++)
        ui_kv(u, fmtv("unit+0x%x", s->watch_off[i]),
              s->watch_ok[i] ? UI_C_TEXT : UI_C_BAD,
              s->watch_ok[i] ? "%10u   0x%08x   %.4f" : "unreadable",
              s->watch_val[i], s->watch_val[i], as_float(s->watch_val[i]));
    if (!s->nwatch) ui_text(u, UI_C_DIM, "(none)");
    ui_newline(u);
    if (ui_button(u, "Clear watches")) panel_watch_clear();
    ui_group_end(u);

    ui_group(u, "FART");
    if (ui_button(u, "Fart")) hg_fart();
    ui_newline(u);
    {
        int n = hg_fart_samples();
        ui_kv(u, "samples", n ? UI_C_OK : UI_C_WARN,
              n ? "%d real, picked at random" : "none - using synthesis", n);
    }
    ui_text(u, UI_C_DIM,
            "Drop .wav files in <game>/bin/farts/. ctrl+F also works.");
    ui_group_end(u);
}

static void tab_memory(ui_ctx *u, const panel_snap *s)
{
    int clicked, base = (g_sel >= 0) ? (g_sel & ~3) : -1;
    unsigned int val = 0, off = 0;

    ui_group(u, "WINDOW");
    ui_text(u, s->peek_ok ? UI_C_TEXT : UI_C_BAD,
            s->peek_ok ? "unit +0x%04x   @ 0x%08x   (%d bytes)"
                       : "unit +0x%04x   unreadable",
            s->peek_off, s->peek_addr, PANEL_PEEKW);
    if (ui_button(u, "-0x100")) panel_peek_nudge(-0x100);
    if (ui_button(u, "-0x10"))  panel_peek_nudge(-0x10);
    if (ui_button(u, "+0x10"))  panel_peek_nudge(0x10);
    if (ui_button(u, "+0x100")) panel_peek_nudge(0x100);
    if (ui_button(u, "Top"))    panel_peek_set(0);
    if (ui_toggle(u, s->mark_ok ? "Marked" : "Mark", s->mark_ok)) {
        g_marked = !s->mark_ok;
        panel_peek_mark(g_marked);
    }
    ui_newline(u);
    ui_text(u, UI_C_DIM,
            s->mark_ok
                ? "Bytes differing from the mark are highlighted. Take a hit."
                : "Mark takes a baseline; changed bytes then light up.");
    ui_group_end(u);

    if (s->peek_ok) {
        clicked = ui_hex(u, s->peek, s->mark_ok ? s->mark : NULL,
                         PANEL_PEEKW, s->peek_off, g_sel);
        if (clicked >= 0) g_sel = clicked;
    } else {
        /* Drawing the last good window here would present stale bytes as
         * current, which is the one thing a memory explorer must not do. */
        ui_text(u, UI_C_BAD,
                "nothing to show: that window is not committed, or there is");
        ui_text(u, UI_C_BAD, "no player unit yet.");
        ui_gap(u, 6.0f);
    }

    ui_group(u, "SELECTED DWORD");
    if (base >= 0 && base + 3 < PANEL_PEEKW) {
        memcpy(&val, s->peek + base, 4);
        off = s->peek_off + (unsigned int)base;
        ui_kv(u, "offset", UI_C_TEXT, "unit+0x%x   @ 0x%08x", off,
              s->peek_addr + (unsigned int)base);
        ui_kv(u, "value", UI_C_TEXT, "%u   0x%08x   %d   %.4f",
              val, val, (int)val, as_float(val));
        if (ui_button(u, "Watch it")) panel_watch_add(off);
        if (ui_button(u, "Set 0"))    panel_poke_async(off, 0);
        if (ui_button(u, "Set 1"))    panel_poke_async(off, 1);
        if (ui_button(u, "+1"))       panel_poke_async(off, val + 1);
        if (ui_button(u, "-1"))       panel_poke_async(off, val - 1);
        if (ui_button(u, "Set 9999"))  panel_poke_async(off, 9999);
        if (ui_button(u, "Set 999999"))panel_poke_async(off, 999999);
        ui_newline(u);
        ui_text(u, UI_C_DIM,
                "Writes are refused unless aligned and committed. Log tab shows"
                " the result.");
    } else {
        ui_text(u, UI_C_DIM, "Click a byte in the dump to select its dword.");
    }
    ui_group_end(u);
}

static void tab_spawn(ui_ctx *u, const panel_snap *s)
{
    const hg_spawn_state *sp = &s->spawn;

    ui_group(u, "STATE");
    if (!sp->hooked) {
        ui_text(u, UI_C_BAD,
                "Spawn hooks are NOT installed. Nothing here can work.");
    } else if (sp->tmpl_kind == HG_TMPL_NONE) {
        ui_text(u, UI_C_WARN,
                "No template yet. Nothing fires until the game spawns once.");
        ui_text(u, UI_C_DIM,
                "Walk somewhere things spawn - loot drops, a shell casing, a");
        ui_text(u, UI_C_DIM,
                "monster - and the buttons below arm themselves.");
    } else {
        ui_text(u, UI_C_OK, "Armed from %s. Buttons fire immediately.",
                tmplname(sp->tmpl_kind));
    }
    ui_kv(u, "observed", UI_C_TEXT, "primitive %ld    script %ld",
          sp->seen_prim, sp->seen_script);
    ui_kv(u, "fired", UI_C_TEXT, "%ld    queued %ld    amplified %ld",
          sp->fired, sp->queued, sp->amplified);
    ui_kv(u, "threads", sp->tmpl_kind && sp->pump_tid && sp->tmpl_tid != sp->pump_tid
                        ? UI_C_BAD : UI_C_DIM,
          "template tid %lu    pump tid %lu", sp->tmpl_tid, sp->pump_tid);
    if (sp->tmpl_kind && sp->pump_tid && sp->tmpl_tid != sp->pump_tid)
        ui_text(u, UI_C_BAD,
                "Threads differ: replay is skipped rather than called from the"
                " wrong one.");
    ui_group_end(u);

    ui_group(u, "FIRE");
    if (ui_button(u, "Fire 1"))   hg_spawn_queue(1);
    if (ui_button(u, "Fire 10"))  hg_spawn_queue(10);
    if (ui_button(u, "Fire 100")) hg_spawn_queue(100);
    if (ui_button_c(u, "Clear queue", UI_C_BTN)) hg_spawn_clear();
    ui_newline(u);
    ui_text(u, UI_C_DIM,
            "Replays the recorded spawn verbatim. The context is the game's"
            " own,");
    ui_text(u, UI_C_DIM, "so no struct layout has to be understood.");
    ui_group_end(u);

    ui_group(u, "FIRE MODE");
    if (ui_toggle(u, "Immediate", sp->immediate)) hg_spawn_set_immediate(1);
    if (ui_toggle(u, "Piggyback", !sp->immediate)) hg_spawn_set_immediate(0);
    ui_newline(u);
    ui_text(u, UI_C_DIM, sp->immediate
            ? "Immediate: fires from the physics pump. Creates an entity"
              " mid-step."
            : "Piggyback: fires only when the game itself spawns. Safer,"
              " slower.");
    ui_group_end(u);

    ui_group(u, "AMPLIFIER  (repro harness for H8)");
    ui_kv(u, "multiplier", sp->mult > 1 ? UI_C_WARN : UI_C_DIM, "%u", sp->mult);
    if (ui_button(u, "x1"))  hg_spawn_set_mult(1);
    if (ui_button(u, "x2"))  hg_spawn_set_mult(2);
    if (ui_button(u, "x5"))  hg_spawn_set_mult(5);
    if (ui_button(u, "x10")) hg_spawn_set_mult(10);
    ui_newline(u);
    ui_text(u, UI_C_DIM,
            "Every spawn the game performs becomes N. Above x1 this"
            " deliberately");
    ui_text(u, UI_C_DIM, "destabilises the session.");
    ui_group_end(u);
}

static void tab_physics(ui_ctx *u, const panel_snap *s)
{
    ui_group(u, "hkWorldCinfo::m_simulationType");
    ui_kv(u, "observed", UI_C_TEXT, "%d (%s)", s->simtype_seen,
          simname(s->simtype_seen));
    ui_kv(u, "override", s->simtype_override >= 0 ? UI_C_WARN : UI_C_DIM,
          s->simtype_override >= 0 ? "%d (%s)" : "none",
          s->simtype_override, simname(s->simtype_override));
    if (ui_toggle(u, "DISCRETE (1)", s->simtype_override == 1)) hg_set_simtype(1);
    if (ui_toggle(u, "CONTINUOUS (2)", s->simtype_override == 2)) hg_set_simtype(2);
    if (ui_toggle(u, "No override", s->simtype_override < 0)) hg_set_simtype(-1);
    ui_newline(u);
    ui_text(u, UI_C_WARN,
            "Applies at the next world construction - a zone change, not now.");
    ui_text(u, UI_C_DIM,
            "DISCRETE is what the 2026 fix patches in. It stops the stall and");
    ui_text(u, UI_C_DIM,
            "removes tunnelling protection from everything, player included.");
    ui_group_end(u);

    ui_group(u, "WORLD");
    ui_tile(u, "bodies", fmtv("%ld", s->bodies), UI_C_ACCENT);
    ui_tile(u, "rays", fmtv("%ld", s->rays), UI_C_ACCENT);
    ui_tile(u, "mopp ms", fmtv("%.1f", s->qms), UI_C_ACCENT);
    ui_newline(u);
    ui_text(u, UI_C_DIM,
            "Run the same route twice, once per setting, and compare mopp ms.");
    ui_group_end(u);
}

static void tab_camera(ui_ctx *u, const panel_snap *s)
{
    ui_group(u, "CAMERA");
    ui_kv(u, "mode", s->cam_mode == 0 ? UI_C_OK : UI_C_TEXT, "%d (%s)",
          s->cam_mode, camname(s->cam_mode));
    if (ui_button(u, "First person")) hg_camera_request(HG_CAM_FIRST);
    if (ui_button(u, "Third person")) hg_camera_request(HG_CAM_THIRD);
    if (ui_button(u, "Restore"))      hg_camera_request(HG_CAM_RESTORE);
    ui_newline(u);
    ui_text(u, UI_C_DIM,
            "Calls the engine's own SetCameraMode on the game thread.");
    ui_group_end(u);

    ui_group(u, "ACTION CAMERA");
    if (!s->shoulder.installed) {
        ui_text(u, UI_C_BAD, "Unavailable - CameraUpdate was not hooked.");
    } else {
        if (ui_toggle(u, s->shoulder.on ? "On" : "Off (stock)", s->shoulder.on))
            hg_shoulder_set_on(!s->shoulder.on);
        if (ui_button(u, s->shoulder.right ? "Right -> left" : "Left -> right"))
            hg_shoulder_swap();
        ui_newline(u);
        ui_kv(u, "sideways", UI_C_TEXT, "%d mm", s->shoulder.offset_mm);
        if (ui_button(u, "-100")) hg_shoulder_nudge(-100, 0, 0);
        if (ui_button(u, "+100")) hg_shoulder_nudge(100, 0, 0);
        ui_newline(u);
        ui_kv(u, "height close", UI_C_TEXT, "%+d mm", s->shoulder.height_near_mm);
        if (ui_button(u, "close down")) hg_shoulder_nudge(0, -100, 0);
        if (ui_button(u, "close up"))   hg_shoulder_nudge(0, 100, 0);
        ui_newline(u);
        ui_kv(u, "height far", UI_C_TEXT, "%+d mm", s->shoulder.height_far_mm);
        if (ui_button(u, "far down")) hg_shoulder_nudge(0, 0, -100);
        if (ui_button(u, "far up"))   hg_shoulder_nudge(0, 0, 100);
        ui_newline(u);
        ui_kv(u, "pitch lift far", UI_C_TEXT, "%+d mm", s->shoulder.lift_mm);
        if (ui_button(u, "lift -")) hg_shoulder_nudge_zoom(-200, 0);
        if (ui_button(u, "lift +")) hg_shoulder_nudge_zoom(200, 0);
        ui_newline(u);
        ui_kv(u, "max zoom", s->shoulder.zoom_patched ? UI_C_TEXT : UI_C_BAD,
              s->shoulder.zoom_patched ? "%.0f m" : "%.0f m (patch failed)",
              s->shoulder.zoom_max_mm / 1000.0f);
        if (ui_button(u, "zoom -1")) hg_shoulder_nudge_zoom(0, -1);
        if (ui_button(u, "zoom +1")) hg_shoulder_nudge_zoom(0, 1);
        ui_newline(u);
        if (ui_toggle(u, s->shoulder.orbit ? "True orbit" : "Stock orbit",
                      s->shoulder.orbit))
            hg_orbit_set_on(!s->shoulder.orbit);
        if (ui_toggle(u, s->shoulder.collide ? "Own collision" : "Game collision",
                      s->shoulder.collide))
            hg_shoulder_set_collide(!s->shoulder.collide);
        ui_text(u, s->shoulder.collide && !s->shoulder.have_world ? UI_C_WARN : UI_C_DIM,
                "%s", !s->shoulder.collide ? "stock ray, offset scaled by it"
                    : s->shoulder.have_world ? "casting against the level"
                    : "no level world yet - using the fallback");
        ui_newline(u);
        if (s->shoulder.impulse_avail) {
            if (ui_toggle(u, s->shoulder.impulse_on ? "Melee impulse" : "No impulse",
                          s->shoulder.impulse_on))
                hg_impulse_set_on(!s->shoulder.impulse_on);
            if (ui_button(u, "impulse -")) hg_impulse_nudge(-25);
            if (ui_button(u, "impulse +")) hg_impulse_nudge(25);
            ui_text(u, UI_C_DIM, "%d%%  swings %ld / skills %ld",
                    s->shoulder.impulse_pct, s->shoulder.melee_events,
                    s->shoulder.skill_events);
        } else {
            ui_text(u, UI_C_BAD, "melee impulse unavailable - see Log");
        }
        ui_newline(u);
        ui_kv(u, "now", UI_C_DIM, "zoom %.2f m, height %+d mm",
              s->shoulder.zoom_mm / 1000.0f, s->shoulder.height_now_mm);
        ui_kv(u, "shoulder room", s->shoulder.hedge_pct < 100 ? UI_C_WARN : UI_C_DIM,
              "%d%% of the offset in use", s->shoulder.hedge_pct);
    }
    ui_text(u, UI_C_DIM,
            "Middle mouse swaps shoulders. Height and lift blend from close");
    ui_text(u, UI_C_DIM, "to far across the zoom range. Third person only.");
    ui_group_end(u);

    ui_group(u, "MELEE FIRST-PERSON UNLOCK");
    if (!s->fp_avail) {
        ui_text(u, UI_C_BAD,
                "Unavailable - CanUseFirstPerson was not hooked this session.");
    } else {
        if (ui_toggle(u, s->fp_melee ? "Unlocked" : "Locked (stock)",
                      s->fp_melee))
            hg_set_fp_melee(!s->fp_melee);
        ui_newline(u);
    }
    ui_text(u, UI_C_DIM,
            "Melee weapons carry two data flags that forbid first person, and");
    ui_text(u, UI_C_DIM,
            "SetCameraMode enforces them itself - it rewrites any request to");
    ui_text(u, UI_C_DIM,
            "third person before it lands, including the engine's own event.");
    ui_text(u, UI_C_DIM,
            "A second kick fires on skill start. The unlock bypasses both.");
    ui_gap(u, 4.0f);
    ui_text(u, UI_C_WARN,
            "Expect cosmetic trouble: first-person models are a separate");
    ui_text(u, UI_C_WARN,
            "appearance group and melee weapons may have no entries in it.");
    ui_group_end(u);
}

/*
 * The viewmodel half of the first-person problem. Unlocking the camera
 * leaves an empty view because melee weapons have no first-person
 * appearance; the third-person model is the only complete one, and the
 * engine explicitly tells it not to use first-person projection.
 *
 * Only one of the two bits involved is known by number, so rather than
 * guess the other from disassembly this pokes whichever bit you name. One
 * look in game settles what a bit does.
 */
/*
 * Graphics pages. Each setting is a row read from the settings registry by
 * key (src/settings.c): its value, default and range, so the row shows where
 * the value sits, marks it when it differs from the default, and the page's
 * Reset puts exactly its own rows back. The key list of a page is gathered
 * while it draws and used by the next frame's Reset button.
 */
typedef struct {
    const char *key, *label;
    int lo, hi;          /* the bar's span in stored units (the setting's own range if equal) */
    int step;            /* - / + and the bar snap to this */
    int div, dec;        /* shown as value / div with dec decimals */
    const char *unit;
    int sign;            /* show a + on positive values */
} srow;

static struct { int page, n, changed; const char *keys[128]; } g_pk, g_pk_prev;

static void page_key(const char *key, int changed)
{
    if (g_pk.n < (int)(sizeof g_pk.keys / sizeof g_pk.keys[0])) g_pk.keys[g_pk.n++] = key;
    g_pk.changed += changed != 0;
}

/* The page title and its Reset, from the previous frame's rows. */
static void page_begin(ui_ctx *u, const char *title)
{
    int prev = g_pk_prev.page == u->tab;
    g_pk.page = u->tab;
    g_pk.n = g_pk.changed = 0;
    if (ui_page(u, title, prev ? g_pk_prev.changed : 0) && prev)
        hg_settings_reset(g_pk_prev.keys, g_pk_prev.n);
}

static void fmt_val(char *o, int cap, long v, int div, int dec, const char *unit, int sign)
{
    long a = v < 0 ? -v : v;
    const char *sg = v < 0 ? "-" : sign && v > 0 ? "+" : "";
    if (div <= 1) snprintf(o, cap, "%s%ld%s", sg, a, unit ? unit : "");
    else if (dec == 1) snprintf(o, cap, "%s%ld.%01ld%s", sg, a / div, a % div / (div / 10), unit ? unit : "");
    else if (dec == 2) snprintf(o, cap, "%s%ld.%02ld%s", sg, a / div, a % div / (div / 100), unit ? unit : "");
    else snprintf(o, cap, "%s%ld.%03ld%s", sg, a / div, a % div / (div / 1000 ? div / 1000 : 1), unit ? unit : "");
}

static void row_value(ui_ctx *u, const srow *r)
{
    hg_setting st;
    char txt[40];
    long lo, hi, v;
    float f = 0.0f;
    int res;
    if (!hg_setting_get(r->key, &st)) { ui_hint(u, "%s: not available", r->label); return; }
    lo = r->lo != r->hi ? r->lo : st.lo;
    hi = r->lo != r->hi ? r->hi : st.hi;
    if (hi <= lo) hi = lo + 1;
    fmt_val(txt, sizeof txt, st.val, r->div, r->dec, r->unit, r->sign);
    page_key(r->key, st.val != st.def);
    res = ui_value(u, r->label, txt, (float)(st.val - lo) / (float)(hi - lo),
                   (float)(st.def - lo) / (float)(hi - lo), st.val != st.def, &f);
    if (res == -1 || res == 1) {
        v = st.val + res * r->step;
        if (v < lo && st.val >= lo) v = lo;
        if (v > hi && st.val <= hi) v = hi;
        hg_setting_set(r->key, v);
    } else if (res == 2) {
        long span = hi - lo, k = (long)(f * (float)span / (float)r->step + 0.5f);
        v = lo + k * r->step;
        if (v > hi) v = hi;
        if (v != st.val) hg_setting_set(r->key, v);
    }
}

static void row_switch(ui_ctx *u, const char *key, const char *label)
{
    hg_setting st;
    if (!hg_setting_get(key, &st)) { ui_hint(u, "%s: not available", label); return; }
    page_key(key, st.val != st.def);
    if (ui_switch(u, label, st.val != 0, st.val != st.def)) hg_setting_set(key, st.val ? 0 : 1);
}

/* values[i] is what choice i stores (NULL: the index itself) */
static void row_choice(ui_ctx *u, const char *key, const char *label, const char *const *names,
                       const int *values, int n)
{
    hg_setting st;
    int i, cur = -1, c;
    if (!hg_setting_get(key, &st)) { ui_hint(u, "%s: not available", label); return; }
    for (i = 0; i < n; i++) if ((values ? values[i] : i) == st.val) cur = i;
    page_key(key, st.val != st.def);
    c = ui_choice(u, label, names, n, cur, st.val != st.def);
    if (c >= 0) hg_setting_set(key, values ? values[c] : c);
}

static void rows(ui_ctx *u, const srow *r, int n)
{
    int i;
    for (i = 0; i < n; i++) row_value(u, &r[i]);
}

#define N(a) ((int)(sizeof a / sizeof a[0]))

static void no_overrides(ui_ctx *u)
{
    ui_hint(u, "No replacement effects loaded (override\\ missing or .off set).");
}

static void page_lighting(ui_ctx *u, const panel_snap *s)
{
    static const srow pl[] = { { "lights.strength", "Strength", 0, 400, 25, 1, 0, "%", 0 } };
    static const srow look[] = {
        { "look.fill", "Ambient fill, outdoors", -90, 200, 10, 1, 0, "%", 1 },
        { "look.fill_indoors", "Ambient fill, indoors", -90, 200, 10, 1, 0, "%", 1 },
        { "look.fog_start", "Fog starts later", 0, 90, 5, 1, 0, "%", 0 },
        { "look.sun", "Sun strength", -90, 200, 10, 1, 0, "%", 1 },
    };
    static const srow surf[] = {
        { "surface.gloss", "Gloss", 10, 200, 10, 1, 0, "%", 0 },
        { "surface.highlight", "Highlights", 0, 200, 10, 1, 0, "%", 0 },
        { "surface.reflection", "Reflections", 0, 200, 10, 1, 0, "%", 0 },
        { "surface.reflection_blur", "Reflection blur", 0, 600, 25, 100, 2, "", 0 },
    };
    static const srow tex[] = {
        { "texture.mip_bias", "Texture sharpness (mip bias)", -150, 100, 5, 100, 2, "", 0 },
        { "detail.sun", "Normal-map detail, sun", 0, 100, 10, 1, 0, "%", 0 },
        { "detail.rest", "Normal-map detail, other", 0, 100, 10, 1, 0, "%", 0 },
    };
    static const char *const aniso_n[] = { "off", "2x", "4x", "8x", "16x" };
    static const int aniso_v[] = { 1, 2, 4, 8, 16 };

    page_begin(u, "Lighting");
    if (!s->gfx.overrides) { no_overrides(u); return; }
    ui_section(u, "POINT LIGHTS");
    row_switch(u, "lights.per_pixel", "Per-pixel lights");
    row_switch(u, "lights.smooth", "Smooth falloff");
    row_switch(u, "lights.highlights", "Highlights from lights");
    rows(u, pl, N(pl));

    ui_section(u, "LOOK");
    rows(u, look, N(look));
    if (ui_button(u, "2007 look")) hg_gfx_nudge_look(-1, 1);
    if (ui_button(u, "Stock look")) hg_gfx_nudge_look(-1, 0);
    ui_newline(u);

    ui_section(u, "SURFACES");
    rows(u, surf, N(surf));
    row_switch(u, "surface.indoors", "Also indoors");

    ui_section(u, "TEXTURES");
    row_choice(u, "texture.anisotropy", "Anisotropic filtering", aniso_n, aniso_v, 5);
    rows(u, tex, N(tex));
    row_switch(u, "lightmap.bicubic", "Smooth light maps (bicubic)");
}

static void page_shadows(ui_ctx *u, const panel_snap *s)
{
    const hg_gfx_state *gx = &s->gfx;
    static const srow fill[] = {
        { "shadow.fill", "Shadow fill", 0, 100, 25, 1, 0, "%", 0 },
        { "shadow.fill_floor_indoor", "Indoor shadow keeps ambient", 0, 100, 10, 1, 0, "%", 0 },
    };
    static const srow pcss[] = {
        { "shadow.sun_size_out", "Sun size, outdoors", 1, 100, 1, 1, 0, "", 0 },
        { "shadow.sun_size_in", "Sun size, indoors", 1, 60, 1, 1, 0, "", 0 },
        { "shadow.bias", "Bias", 0, 800, 20, 1, 0, "", 0 },
        { "shadow.min_softness", "Minimum softness", 1, 12, 1, 1, 0, " texels", 0 },
    };
    static const srow maps[] = {
        { "shadow.wide_every_ms", "Wide maps redrawn every", 200, 10000, 200, 1000, 1, " s", 0 },
        { "shadow.fine_follow", "Fine map follows every", 0, 40, 2, 1, 0, " units", 0 },
    };
    static const srow chars[] = {
        { "shadow.character_offset", "Character offset", 0, 300, 10, 1000, 3, " units", 0 },
        { "shadow.surface_offset", "Level offset", 0, 300, 10, 1000, 3, " units", 0 },
    };
    static const srow cfill[] = { { "character.fill", "Character fill light", 0, 50, 2, 1, 0, "%", 0 } };
    static const srow pls[] = {
        { "pointshadow.bias", "Bias", 0, 100, 1, 100, 2, " units", 0 },
        { "pointshadow.softness", "Softness", 0, 200, 5, 1, 0, "", 0 },
    };
    static const char *const caster_n[] = { "off", "props", "all" };

    page_begin(u, "Shadows");
    if (!gx->overrides) { no_overrides(u); return; }
    ui_section(u, "SUN SHADOWS");
    rows(u, fill, N(fill));
    ui_hint(u, "a shadow takes only the sun's light; indoors, part of the ambient too");
    row_switch(u, "shadow.pcss", "Soft shadows (PCSS)");
    rows(u, pcss, N(pcss));
    if (gx->shadow_type != 2)
        ui_text(u, UI_C_WARN, "PCSS needs the colour shadow map (type %d now; remove hellgate_shadowtype2.off, restart)", gx->shadow_type);

    ui_section(u, "SHADOW MAPS");
    row_switch(u, "shadow.fine_map", "Fine map per pixel");
    row_choice(u, "shadow.static_casters", "Static objects cast", caster_n, NULL, 3);
    row_switch(u, "shadow.stable_casters", "Stable casters");
    rows(u, maps, N(maps));
    if (hg_gfx_reach()) {
        float f = 0.0f;
        int r = ui_value(u, "Near map reach (not saved)", fmtv("%d units", hg_gfx_reach()),
                         (float)(hg_gfx_reach() - 10) / 190.0f, (27.0f - 10.0f) / 190.0f,
                         hg_gfx_reach() != 27, &f);
        if (r == -1 || r == 1) hg_gfx_nudge_reach(10 * r);
    }

    ui_section(u, "CHARACTERS");
    row_switch(u, "shadow.characters", "Self-shadowing");
    if (ui_switch(u, "Player casts a shadow", gx->shadow_on, 0)) hg_shadow_set(!gx->shadow_on);
    rows(u, chars, N(chars));
    ui_hint(u, "offsets: up if striped or speckled, down if feet float");
    rows(u, cfill, N(cfill));
    ui_hint(u, "fill: characters in the dark keep their shape; 0 is stock");

    ui_section(u, "POINT-LIGHT SHADOWS");
    row_switch(u, "pointshadow.on", "Fires and lamps cast shadows");
    rows(u, pls, N(pls));
}

static void page_image(ui_ctx *u, const panel_snap *s)
{
    static const srow cas[] = { { "sharpen", "Sharpen (CAS)", 0, 100, 10, 1, 0, "%", 0 } };
    static const srow ao[] = {
        { "ao.radius", "Radius", 10, 800, 20, 100, 2, " units", 0 },
        { "ao.strength", "Strength", 10, 300, 20, 1, 0, "%", 0 },
        { "ao.less_in_sun", "Less in direct sun", 0, 100, 10, 1, 0, "%", 0 },
        { "ao.colour_bounce", "Colour bounce", 0, 300, 25, 1, 0, "%", 0 },
    };
    static const srow part[] = {
        { "particles.soft", "Soft particles", 0, 500, 10, 100, 2, " units", 0 },
        { "particles.light", "Lit by nearby lights", 0, 300, 10, 1, 0, "%", 0 },
        { "particles.shadow", "Darker in sun shadow", 0, 100, 10, 1, 0, "%", 0 },
    };
    (void)s;
    page_begin(u, "Image");
    ui_section(u, "ANTI-ALIASING");
    if (ui_switch(u, "SMAA instead of MSAA", hg_gfx_smaa(), !hg_gfx_smaa())) hg_gfx_set_smaa(!hg_gfx_smaa());
    if (hg_gfx_smaa() != hg_gfx_smaa_live()) ui_text(u, UI_C_WARN, "restart the game to switch anti-aliasing");
    if (!hg_gfx_smaa_live()) {
        ui_hint(u, "Sharpening, ambient occlusion, fog and HDR need SMAA (the scene depth comes with it).");
        return;
    }
    rows(u, cas, N(cas));

    ui_section(u, "AMBIENT OCCLUSION");
    row_switch(u, "ao.on", "Ambient occlusion");
    rows(u, ao, N(ao));

    ui_section(u, "PARTICLES");
    rows(u, part, N(part));
    ui_hint(u, "soft: sprites fade where they meet geometry; lit: smoke takes the colour of fires");
}

static void page_hdr(ui_ctx *u, const panel_snap *s)
{
    static const srow tm[] = {
        { "hdr.exposure", "Exposure", 25, 400, 5, 1, 0, "%", 0 },
        { "hdr.knee", "Shoulder starts at", 30, 95, 5, 1, 0, "% of white", 0 },
        { "hdr.spill", "Highlight spill", 0, 400, 25, 1, 0, "%", 0 },
        { "hdr.bloom_threshold", "Bloom from", 25, 400, 10, 1, 0, "% of white", 0 },
    };
    static const srow ae[] = {
        { "hdr.auto", "Auto exposure", 0, 100, 10, 1, 0, "%", 0 },
        { "hdr.auto_middle", "Target brightness", 10, 500, 10, 1000, 3, "", 0 },
        { "hdr.auto_stops", "Range", 0, 30, 5, 10, 1, " stops", 0 },
    };
    (void)s;
    page_begin(u, "HDR");
    if (!hg_gfx_smaa_live()) { ui_hint(u, "Needs SMAA instead of MSAA (Image page)."); return; }
    row_switch(u, "hdr.on", "HDR scene (float target)");
    row_switch(u, "hdr.tonemap", "Tone map (off: stock clamp)");
    if (!hg_gfx_hdr_live()) { ui_hint(u, "HDR is off this frame."); return; }
    ui_section(u, "TONE MAP");
    rows(u, tm, N(tm));
    ui_hint(u, "spill: overbright colour burns towards white like stock; 0 keeps the hue");
    ui_section(u, "AUTO EXPOSURE");
    rows(u, ae, N(ae));
    ui_hint(u, "scene brightness now %.3f", hg_gfx_hdr_eye());
}

static void page_atmosphere(ui_ctx *u, const panel_snap *s)
{
    static const srow fog[] = {
        { "fog.density", "Density, outdoors", 0, 500, 5, 1000, 3, " /unit", 0 },
        { "fog.density_indoors", "Density, indoors", 0, 500, 2, 1000, 3, " /unit", 0 },
        { "fog.haze", "Distance haze", 0, 200, 2, 1000, 3, " /unit", 0 },
        { "fog.sun_shafts", "Sun shafts", 0, 300, 10, 1, 0, "%", 0 },
        { "fog.shaft_reach", "Shafts reach", 10, 200, 10, 1, 0, " units", 0 },
        { "fog.sky", "On the sky", 0, 100, 10, 1, 0, "%", 0 },
        { "fog.light_glow", "Light halos", 0, 300, 10, 1, 0, "%", 0 },
        { "fog.lamp_shafts", "Lamp shafts, indoors", 0, 100, 10, 1, 0, "%", 0 },
        { "fog.mist", "Ground mist", 0, 200, 5, 1000, 3, " /unit", 0 },
        { "fog.mist_height", "Mist height", 5, 400, 10, 100, 2, " units", 0 },
    };
    static const srow grade[] = {
        { "bloom.intensity", "Bloom strength", 0, 300, 10, 1, 0, "%", 0 },
        { "bloom.threshold", "Bloom threshold (no HDR)", 0, 100, 5, 1, 0, "%", 0 },
        { "grade.saturation", "Saturation", 0, 300, 5, 1, 0, "%", 0 },
        { "grade.contrast", "Contrast", 0, 100, 5, 1, 0, "%", 0 },
        { "grade.shadow_tint", "Shadow tint", 0, 100, 5, 1, 0, "%", 0 },
        { "grade.vignette", "Vignette", 0, 100, 5, 1, 0, "%", 0 },
    };
    (void)s;
    page_begin(u, "Atmosphere");
    if (!hg_gfx_smaa_live()) { ui_hint(u, "Needs SMAA instead of MSAA (Image page)."); return; }
    ui_section(u, "VOLUMETRIC FOG");
    row_switch(u, "fog.on", "Volumetric fog");
    rows(u, fog, N(fog));
    ui_section(u, "BLOOM AND COLOUR GRADE");
    row_switch(u, "bloom.on", "Bloom");
    row_switch(u, "grade.on", "Colour grade");
    rows(u, grade, N(grade));
    ui_hint(u, "shadow tint: shadows lean towards the level's fog colour");
}

/* Views, traces and counters: A/B tools, nothing saved. */
static void page_gfx_debug(ui_ctx *u, const panel_snap *s)
{
    const hg_gfx_state *gx = &s->gfx;
    page_begin(u, "Graphics debug");
    ui_section(u, "SHADOW MAPS");
    if (ui_toggle(u, "Map view", hg_gfx_shadow_debug())) hg_gfx_set_shadow_debug(!hg_gfx_shadow_debug());
    if (ui_button(u, "Dump maps")) hg_gfx_dump_shadowmaps();
    if (ui_button(u, "Trace maps")) hg_gfx_trace_shadows();
    if (ui_toggle(u, "Force engine shadow flag", hg_gfx_shadow_flag_forced())) hg_gfx_force_shadow_flag(!hg_gfx_shadow_flag_forced());
    ui_newline(u);
    ui_hint(u, "view: red near map, green wide, blue fine-map weight; dark = shadow");
    ui_hint(u, "map type %d, knob writes %ld", gx->shadow_type, gx->ultra_writes);
    {
        float lp[3];
        long casts, replays;
        int on = hg_gfx_plshadow_status(lp, &casts, &replays);
        ui_hint(u, on ? "point-light shadow: light at %.0f %.0f %.0f, %ld draws" : "point-light shadow: no light near", lp[0], lp[1], lp[2], replays);
    }

    ui_section(u, "CULLING");
    {
        long tests, hidden;
        int hooked;
        hg_cull_counts(&tests, &hidden, &hooked);
        if (ui_toggle(u, "Occlusion: all visible", hg_cull_all_visible())) hg_cull_set_all_visible(!hg_cull_all_visible());
        ui_newline(u);
        ui_hint(u, hooked ? "Umbra occlusion tests %ld, answered hidden %ld (all visible: nothing culled by occlusion)"
                          : "Umbra not hooked (see Log)", tests, hidden);
    }

    ui_section(u, "PASSES");
    if (hg_gfx_smaa_live()) {
        if (ui_toggle(u, "SMAA pass (A/B)", hg_gfx_smaa_pass())) hg_gfx_set_smaa_pass(!hg_gfx_smaa_pass());
        if (ui_toggle(u, "AO alone", hg_gfx_ao_show() == 1)) hg_gfx_set_ao_show(hg_gfx_ao_show() == 1 ? 0 : 1);
        if (ui_toggle(u, "AO bounce x4", hg_gfx_ao_show() == 2)) hg_gfx_set_ao_show(hg_gfx_ao_show() == 2 ? 0 : 2);
        if (ui_toggle(u, "Fog alone", hg_gfx_fog_show())) hg_gfx_set_fog_show(!hg_gfx_fog_show());
        ui_newline(u);
        ui_hint(u, "runs: SMAA %ld, AO %ld, fog %ld", hg_gfx_postfx_runs(1), hg_gfx_postfx_runs(0), hg_gfx_postfx_runs(2));
    }
    if (hg_gfx_hdr_live()) {
        if (ui_button(u, "Scan the float scene (log)")) hg_gfx_hdr_scan();
        ui_newline(u);
        ui_hint(u, "HDR plain copies %ld", hg_gfx_hdr_copies());
    }
    ui_hint(u, "%d effects replaced, lit requests %ld, clamped %ld", gx->overrides, gx->n_lit, gx->n_clamped);
}

static void tab_viewmodel(ui_ctx *u, const panel_snap *s)
{
    ui_group(u, "VIEWMODEL  (experimental)");
    /*
     * The chain, not just the verdict: -1 is three different failures
     * wearing the same hat, and guessing which cost a session already.
     */
    ui_kv(u, "unit", s->model_unit ? UI_C_TEXT : UI_C_BAD, "0x%08x",
          s->model_unit);
    ui_kv(u, "pGfx  +0x160", s->model_gfx ? UI_C_TEXT : UI_C_BAD, "0x%08x",
          s->model_gfx);
    ui_kv(u, "3rd model id", s->model_third >= 0 ? UI_C_OK : UI_C_BAD,
          s->model_third >= 0 ? "%d" : "%d  (nothing to poke)",
          s->model_third);
    if (s->model_third < 0)
        ui_text(u, UI_C_BAD, "%s",
                !s->model_unit ? "no local player unit"
              : !s->model_gfx  ? "unit has no pGfx - graphics not attached"
                               : "pGfx+0x10 held -1");
    ui_text(u, UI_C_DIM,
            "Melee has no first-person appearance, so first person draws");
    ui_text(u, UI_C_DIM,
            "nothing. The third-person model is the one with a body in it.");
    ui_newline(u);

    ui_kv(u, "flagbit", UI_C_TEXT, "%d%s", g_flagbit,
          g_flagbit == HG_MODEL_FP_PROJ ? "   FIRST_PERSON_PROJ"
          : g_flagbit == 4 ? "   NOSHADOW (Set 0 = let the model cast a shadow)" : "");
    if (ui_button(u, "-")) { if (g_flagbit > 0) g_flagbit--; }
    if (ui_button(u, "+")) { if (g_flagbit < 31) g_flagbit++; }
    if (ui_button(u, "Set 1"))  hg_model_flag_request(g_flagbit, 1);
    if (ui_button(u, "Set 0"))  hg_model_flag_request(g_flagbit, 0);
    ui_newline(u);
    if (ui_button(u, "3rd model -> FP projection"))
        hg_model_flag_request(HG_MODEL_FP_PROJ, 1);
    if (ui_button(u, "undo"))
        hg_model_flag_request(HG_MODEL_FP_PROJ, 0);
    ui_newline(u);
    ui_text(u, UI_C_DIM,
            "Bit 7 is FIRST_PERSON_PROJ, recovered from the setup code at");
    ui_text(u, UI_C_DIM,
            "0x004d31b0. Bit 4 is NOSHADOW (the paperdoll setter at 0x4b92ee).");
    ui_text(u, UI_C_DIM,
            "walk the bits and watch. Results go to the Log tab.");
    ui_group_end(u);
}

static void tab_log(ui_ctx *u)
{
    char line[HG_LOGLINE];
    int i, max;

    ui_group(u, "LOG  (newest first)");

    /*
     * Text is drawn with DT_NOCLIP, so a long line would run out past the
     * panel edge and over the game. Log lines routinely run to 160
     * characters, so they are cut to the window here.
     */
    max = (int)((u->content_r - u->content_l) / u->chw);
    if (max < 8) max = 8;
    if (max > (int)sizeof line - 1) max = (int)sizeof line - 1;

    for (i = 0; i < 20; i++) {
        if (!hg_log_line(i, line, sizeof line)) break;
        if ((int)strlen(line) > max) {
            line[max] = 0;
            line[max - 1] = line[max - 2] = line[max - 3] = '.';
        }
        ui_text(u, i == 0 ? UI_C_TEXT : UI_C_DIM, "%s", line);
    }
    if (!i) ui_text(u, UI_C_DIM, "(empty)");
    ui_group_end(u);
}

/* ------------------------------------------------------------------ */
/* the panel                                                           */

void panel_ui_build(ui_ctx *u, const panel_snap *s, int have)
{
    static const char *const NAV[] = {
        "#GRAPHICS", "Lighting", "Shadows", "Image", "HDR", "Atmosphere",
        "#GAMEPLAY", "Camera",
        "#DEBUG", "Graphics debug", "Performance", "Player", "Memory", "Spawn",
        "Physics", "View model", "Log",
    };
    float pw, ph;

    panel_ui_size(u, &pw, &ph);
    ui_panel_begin(u, "MARCUS FIDELIUS ULTRAPATCH", "shift+` close   ctrl+1..0 pages", pw, ph);
    ui_nav(u, NAV, N(NAV), PANEL_NAV_COLS);

    switch (u->tab) {
    case PG_LIGHTING:    page_lighting(u, s);   break;
    case PG_SHADOWS:     page_shadows(u, s);    break;
    case PG_IMAGE:       page_image(u, s);      break;
    case PG_HDR:         page_hdr(u, s);        break;
    case PG_ATMOSPHERE:  page_atmosphere(u, s); break;
    case PG_CAMERA:      page_begin(u, "Camera"); tab_camera(u, s); break;
    case PG_GFX_DEBUG:   page_gfx_debug(u, s);  break;
    case PG_PERF:        page_begin(u, "Performance"); tab_live(u, s, have); break;
    case PG_PLAYER:      page_begin(u, "Player"); tab_player(u, s); break;
    case PG_MEMORY:      page_begin(u, "Memory"); tab_memory(u, s); break;
    case PG_SPAWN:       page_begin(u, "Spawn"); tab_spawn(u, s); break;
    case PG_PHYSICS:     page_begin(u, "Physics"); tab_physics(u, s); break;
    case PG_VIEWMODEL:   page_begin(u, "View model"); tab_viewmodel(u, s); break;
    default:             page_begin(u, "Log"); tab_log(u); break;
    }
    g_pk_prev = g_pk;

    /*
     * The one surprise in the design: the game reads DINPUT8 directly and
     * the overlay cannot swallow a button, so a click on the panel also
     * swings whatever is in your hands.
     */
    ui_gap(u, 2.0f);
    ui_hint(u, "clicks also reach the game - every control has a ctrl+key too");

    ui_panel_end(u);
}
