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

    /* 78 columns: the 74-column hex dump plus the group indents around it. */
    *w = 78.0f * chw + 46.0f;
    if (*w < 600.0f) *w = 600.0f;

    /*
     * 56 rows: the Camera tab, now the tallest, plus its chrome. It was
     * 34 (the memory tab) until the action camera controls ran off the
     * bottom of the frame in game.
     */
    *h = 56.0f * chh + 78.0f;
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
    if (ui_button(u, "Peek at +0x000")) { panel_peek_set(0); u->tab = 2; }
    if (ui_button(u, "Peek at flags"))  { panel_peek_set(0x100); u->tab = 2; }
    if (ui_button(u, "Peek at name"))   { panel_peek_set(0x120); u->tab = 2; }
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
static void tab_viewmodel(ui_ctx *u, const panel_snap *s)
{
    const hg_animfix_state *af = &s->animfix;
    const hg_gfx_state *gx = &s->gfx;
    ui_group(u, "GRAPHICS");
    if (!gx->overrides) {
        ui_text(u, UI_C_DIM, "No replacement effects loaded (override\\ missing or .off set).");
    } else {
        if (ui_toggle(u, "Per-pixel lights (5 per model)", gx->lights_on)) hg_gfx_set_lights(!gx->lights_on);
        ui_newline(u);
        ui_text(u, UI_C_TEXT, "strength %d%%", gx->strength);
        if (ui_button(u, "-")) hg_gfx_nudge_strength(-10);
        if (ui_button(u, "+")) hg_gfx_nudge_strength(10);
        ui_newline(u);
        ui_text(u, UI_C_DIM, "%d effects replaced  lit draws %ld  clamped %ld", gx->overrides, gx->n_lit, gx->n_clamped);
        if (ui_toggle(u, "Shadow fill (shadow takes only the sun)", gx->fill_pct > 0)) hg_gfx_set_fill(gx->fill_pct > 0 ? 0 : 100);
        ui_newline(u);
        ui_text(u, UI_C_TEXT, "fill %d%%", gx->fill_pct);
        if (ui_button(u, "-")) hg_gfx_set_fill(gx->fill_pct - 25);
        if (ui_button(u, "+")) hg_gfx_set_fill(gx->fill_pct + 25);
        ui_newline(u);
        if (ui_toggle(u, "PCSS soft shadows", gx->pcss_on)) hg_gfx_set_pcss(!gx->pcss_on);
        ui_newline(u);
        ui_text(u, UI_C_TEXT, "sun size outdoor %d", gx->pcss_scale);
        if (ui_button(u, "-")) hg_gfx_scale_pcss(0, 0);
        if (ui_button(u, "+")) hg_gfx_scale_pcss(0, 1);
        ui_newline(u);
        ui_text(u, UI_C_TEXT, "sun size indoor %d", gx->pcss_scale_in);
        if (ui_button(u, "-")) hg_gfx_scale_pcss(1, 0);
        if (ui_button(u, "+")) hg_gfx_scale_pcss(1, 1);
        ui_newline(u);
        ui_text(u, UI_C_TEXT, "bias %d", gx->pcss_bias);
        if (ui_button(u, "-")) hg_gfx_scale_pcss(2, 0);
        if (ui_button(u, "+")) hg_gfx_scale_pcss(2, 1);
        ui_newline(u);
        ui_text(u, UI_C_DIM, "bias: lower until feet touch their shadow; speckle = too low");
        ui_text(u, UI_C_TEXT, "min softness %d", gx->pcss_min);
        if (ui_button(u, "-")) hg_gfx_nudge_pcss_min(-1);
        if (ui_button(u, "+")) hg_gfx_nudge_pcss_min(1);
        ui_newline(u);
        ui_text(u, UI_C_DIM, "shadow map type %d  knob writes %ld", gx->shadow_type, gx->ultra_writes);
        if (gx->shadow_type != 2)
            ui_text(u, UI_C_BAD, "PCSS needs the colour shadow map (type %d now; remove hellgate_shadowtype2.off, restart)", gx->shadow_type);
    }
    if (gx->overrides) {
        ui_text(u, UI_C_TEXT, "LOOK  fill %+d%%", gx->look_fill);
        if (ui_button(u, "-")) hg_gfx_nudge_look(0, -10);
        if (ui_button(u, "+")) hg_gfx_nudge_look(0, 10);
        ui_text(u, UI_C_TEXT, "fog start %d%%", gx->look_fog);
        if (ui_button(u, "-")) hg_gfx_nudge_look(1, -5);
        if (ui_button(u, "+")) hg_gfx_nudge_look(1, 5);
        ui_text(u, UI_C_TEXT, "sun %+d%%", gx->look_sun);
        if (ui_button(u, "-")) hg_gfx_nudge_look(2, -10);
        if (ui_button(u, "+")) hg_gfx_nudge_look(2, 10);
        ui_newline(u);
        if (ui_button(u, "2007 look")) hg_gfx_nudge_look(-1, 1);
        if (ui_button(u, "stock look")) hg_gfx_nudge_look(-1, 0);
        ui_newline(u);
    }
    ui_text(u, UI_C_DIM, "Off = the stock shaders exactly. Takes effect on the next frame.");
    if (ui_toggle(u, "Player casts shadow", gx->shadow_on)) hg_shadow_set(!gx->shadow_on);
    ui_text(u, UI_C_DIM, "Clears NOSHADOW (bit 4) on your model; re-applied after respawn/zone.");
    if (ui_toggle(u, "Force engine shadow flag", hg_gfx_shadow_flag_forced())) hg_gfx_force_shadow_flag(!hg_gfx_shadow_flag_forced());
    ui_text(u, UI_C_DIM, "Experiment: the render flag dx9_RenderModelShadow requires. Off restores it.");
    ui_group_end(u);
    ui_group(u, "ANIMATION FIXES");
    if (!af->installed) {
        ui_text(u, UI_C_BAD, "Unavailable - see Log.");
    } else {
        if (ui_toggle(u, "Stance ease", af->stance_on)) hg_animfix_set(0, !af->stance_on);
        if (ui_toggle(u, "Ease-out floor", af->minout_on)) hg_animfix_set(3, !af->minout_on);
        if (ui_toggle(u, "Seam blend (pre-wrap)", af->seam_on)) hg_animfix_set(1, !af->seam_on);
        if (ui_toggle(u, "Phase match", af->phase_on)) hg_animfix_set(2, !af->phase_on);
        if (ui_toggle(u, "Spike log", af->spike_on)) hg_animfix_set(4, !af->spike_on);
        ui_newline(u);
        ui_text(u, UI_C_DIM, "stance %ld  floor %ld  seams %ld  phase %ld  spikes %ld",
                af->n_stance, af->n_minout, af->n_seam, af->n_phase, af->n_spike);
    }
    ui_text(u, UI_C_DIM, "Toggle one off and watch the same run to see what it does.");
    ui_group_end(u);

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
    static const char *const TABS[8] = {
        "Live", "Player", "Memory", "Spawn", "Physics", "Camera",
        "Model", "Log"
    };

    float pw, ph;

    panel_ui_size(u, &pw, &ph);
    ui_panel_begin(u, "HELLGATE DEV", "shift+` close   ctrl+1..8 tabs", pw, ph);
    ui_tabs(u, TABS, 8);

    switch (u->tab) {
    case 0: tab_live(u, s, have);  break;
    case 1: tab_player(u, s);      break;
    case 2: tab_memory(u, s);      break;
    case 3: tab_spawn(u, s);       break;
    case 4: tab_physics(u, s);     break;
    case 5: tab_camera(u, s);      break;
    case 6: tab_viewmodel(u, s);   break;
    default: tab_log(u);           break;
    }

    /*
     * Said on every tab because it is the one surprise in the design: the
     * game reads DINPUT8 directly and the overlay cannot swallow a button,
     * so a click on a panel button also swings whatever is in your hands.
     */
    ui_gap(u, 2.0f);
    ui_text(u, UI_C_DIM,
            "clicks also reach the game - every control has a ctrl+key too");

    ui_panel_end(u);
}

