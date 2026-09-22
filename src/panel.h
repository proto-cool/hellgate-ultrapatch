#ifndef HG_PANEL_H
#define HG_PANEL_H

/* For wchar_t: panel_ui.c and its tests include this without windows.h. */
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* shared services, implemented in hook.c                              */

/* Shared log sink. */
void hg_log(const char *fmt, ...);

/* True if the named file sits next to the DLL. */
int  hg_flagfile(const wchar_t *name);

/* The directory the DLL sits in. */
void hg_dll_dir(wchar_t *out, int cap);

/*
 * The last few log lines, kept in a ring so the overlay can show them.
 * `age` 0 is the newest. Returns 0 when there is no line that old.
 *
 * This exists because the panel's own actions used to be invisible: you
 * pressed a key, nothing happened on screen, and the only way to find out
 * why was to alt-tab and read hellgate_rays.log. Now the reason is in the
 * panel.
 */
#define HG_LOGRING   32      /* power of two: the ring index is masked */
#define HG_LOGLINE   160
int  hg_log_line(int age, char *out, int cap);

/* ------------------------------------------------------------------ */
/* spawn control, implemented in hook.c                                */

/*
 * What the panel can drive, and the honest limits of it.
 *
 * The game's spawn primitive takes thirteen dwords of context that nobody
 * has mapped, so the DLL cannot synthesise a spawn from nothing. What it
 * can do is *record* a spawn the game performs -- every argument, verbatim
 * -- and replay that exact call later on demand. One real spawn anywhere in
 * the zone arms the panel; after that the buttons fire immediately.
 *
 * Until that first spawn is observed there is no template and the buttons
 * cannot do anything. The earlier design hid that: it queued the request
 * and waited for a spawn that, in a quiet room, never came, so the panel
 * sat there claiming "10 queued" forever. The state below is reported in
 * full precisely so that case names itself instead of looking broken.
 */
enum {
    HG_TMPL_NONE = 0,
    HG_TMPL_PRIM,        /* the shared 13-argument spawn primitive        */
    HG_TMPL_SCRIPT_OBJ,  /* SpawnObject script action                     */
    HG_TMPL_SCRIPT_MON   /* SpawnMonsterNearby script action              */
};

typedef struct {
    long          seen_prim;    /* calls observed at the spawn primitive   */
    long          seen_script;  /* calls observed at the script actions    */
    long          fired;        /* replays issued since load               */
    long          queued;       /* replays still owed                      */
    long          amplified;    /* extras issued by the HG_SPAWN_MULT rig  */
    int           hooked;       /* the spawn hooks were installed at all   */
    int           tmpl_kind;    /* HG_TMPL_*                               */
    int           immediate;    /* fire from the pump, not on a live spawn */
    unsigned int  mult;         /* HG_SPAWN_MULT                           */
    unsigned long tmpl_tid;     /* thread the template was captured on     */
    unsigned long pump_tid;     /* thread the pump runs on                 */
} hg_spawn_state;

void hg_spawn_status(hg_spawn_state *out);

/* Adds to the queue -- it does not replace it, so two clicks queue twice. */
void hg_spawn_queue(long n);
void hg_spawn_clear(void);

/*
 * Immediate mode (the default) fires from the pump, inside the Havok step.
 * That is what makes a button do something in a quiet room. Turning it off
 * falls back to the old piggyback behaviour: the queue drains only when the
 * game itself spawns, which is a slower but strictly safer context.
 */
void hg_spawn_set_immediate(int on);
void hg_spawn_set_mult(unsigned int m);

/* Drained on the game thread by panel_pump(). Returns how many fired. */
long hg_spawn_pump(void);

/* ------------------------------------------------------------------ */
/* experiment knobs, implemented in hook.c                             */

/*
 * hkWorldCinfo::m_simulationType. The override applies when the game next
 * constructs a world, not retroactively -- the panel says so rather than
 * pretending the button took effect immediately.
 */
void hg_set_simtype(int t);          /* -1 = observe only */
int  hg_get_simtype_override(void);
int  hg_get_simtype_seen(void);
void hg_counters_reset(void);

/* ------------------------------------------------------------------ */
/* first person, implemented in hook.c                                 */

/*
 * Melee weapons carry two data flags that forbid first person, and the
 * engine enforces them inside SetCameraMode itself, so every request is
 * rewritten to third person before it lands. See target.h for the full
 * mechanism and the two places it bites.
 *
 * The unlock detours the one predicate that answers the question, and flips
 * one byte to skip the separate kick on skill start. Off by default; set
 * HG_FP_MELEE=1 or use the panel's Camera tab.
 */
void hg_set_fp_melee(int on);
int  hg_get_fp_melee(void);
int  hg_fp_melee_available(void);

/*
 * Over-the-shoulder offset for the third-person camera, src/shoulder.c.
 * On by default; middle mouse swaps shoulders in game.
 */
typedef struct {
    int  installed, on, right;
    int  offset_mm;
    int  height_near_mm, height_far_mm;   /* at closest / farthest zoom      */
    int  height_now_mm, zoom_mm;          /* what was applied, at what zoom  */
    int  lift_mm;                         /* look-at lift at farthest zoom   */
    int  zoom_max_mm, zoom_patched;       /* ceiling, and whether it took    */
    int  collide, have_world;             /* own collision wanted / possible */
    int  orbit;                           /* true spherical orbit            */
    int  impulse_avail, impulse_on, impulse_pct;
    long skill_events, melee_events;      /* skill starts seen / yours, melee */
    int  hedge_pct;      /* share of the shoulder offset in use; <100 = a wall  */
    long frames;         /* frames the offset has been applied                   */
} hg_shoulder_state;

void hg_shoulder_status(hg_shoulder_state *out);
void hg_shoulder_set_on(int on);
void hg_shoulder_swap(void);
void hg_shoulder_nudge(int d_offset_mm, int d_near_mm, int d_far_mm);
void hg_shoulder_nudge_zoom(int d_lift_mm, int d_zoom_max_m);
void hg_shoulder_set_collide(int on);
void hg_impulse_set_on(int on);
void hg_orbit_set_on(int on);
void hg_impulse_nudge(int d_pct);


/* Graphics overrides, src/gfxprobe.c. lights_on gates the per-pixel light
 * techniques at request time; off renders exactly the stock passes. */
typedef struct {
    int  overrides;            /* replacement effects loaded this session      */
    int  lights_on;
    int  strength;             /* percent, the added lights' intensity         */
    int  shadow_on;
    long n_lit, n_clamped;     /* draws with the additive states / clamped     */
    int  fill_pct;             /* shadow fill 0..100, 0 = stock                 */
    int  pcss_on, pcss_scale;  /* soft shadows; penumbra scale                  */
    int  pcss_min;             /* softest contact edge, texels                  */
    int  pcss_scale_in;        /* sun size for indoor materials                 */
    int  pcss_bias;            /* depth bias, 1e-6 per texel of radius          */
    int  look_fill, look_fog, look_sun;   /* scene look, percent; 0 = stock    */
    int  shadow_type;          /* engine nShadowType: PCSS needs 2              */
    long ultra_writes;         /* times our effects received the knobs          */
    int  pl_smooth, pl_spec;   /* base-pass point lights: falloff, highlights   */
    int  pl_pct;               /* their strength, percent                       */
} hg_gfx_state;
void hg_gfx_status(hg_gfx_state *out);
void hg_gfx_set_lights(int on);
void hg_gfx_nudge_strength(int d_pct);
void hg_gfx_force_shadow_flag(int on);   /* experiment: engine render flag "shadows" := 1 */
int  hg_gfx_shadow_flag_forced(void);
/* Material knobs (shaders/ultra.hlsl); all default to the stock look. */
void hg_gfx_set_fill(int pct);
void hg_gfx_set_pcss(int on);
void hg_gfx_scale_pcss(int which, int up);   /* 0 sun outdoor, 1 sun indoor, 2 bias */
void hg_gfx_nudge_pcss_min(int d);
void hg_gfx_nudge_pl(int which, int d);     /* 0 falloff, 1 specular (toggle), 2 strength +d% */
void hg_gfx_nudge_look(int which, int d);    /* 0 fill, 1 fog start, 2 sun; -1 preset (d 1 = 2007, 0 = stock) */
int  hg_gfx_shadow_type(void);
/* Player shadow (src/hook.c): clears MODEL_FLAGBIT_NOSHADOW on the player's model. */
void hg_shadow_set(int on);
int  hg_shadow_get(void);

#define HG_CAM_FIRST    0
#define HG_CAM_THIRD    6
#define HG_CAM_RESTORE (-2)

/* Ask for a camera mode. Safe from any thread; applied by the pump. */
void hg_camera_request(int mode);
int  hg_camera_mode(void);
long hg_camera_pump(void);

/*
 * Viewmodel experiment. Unlocking the camera leaves nothing to look at,
 * because melee weapons have no first-person appearance; the third-person
 * model is the only complete one. These poke MODEL_FLAGBIT bits on it so
 * the right bit can be found by looking rather than by disassembly.
 */
/* MODEL_FLAGBIT_FIRST_PERSON_PROJ, mirrored from target.h for the UI. */
#define HG_MODEL_FP_PROJ 7

int  hg_model_third(void);                   /* game thread only */
/* The full lookup chain, so a failure names which link broke. */
void hg_model_chain(unsigned int *unit, unsigned int *gfx, int *id);
void hg_model_flag_request(int bit, int value);
long hg_model_pump(void);

/* ------------------------------------------------------------------ */
/* /fart, implemented in fart.c                                        */

/*
 * Synthesised locally and played through winmm. Depends on nothing
 * recovered from the binary, which makes it the one thing here that cannot
 * fail for the usual reason. Safe from any thread.
 */
void hg_fart(void);

/* How many real samples were found in bin/farts/, and where it looked. */
int  hg_fart_samples(void);

/* ------------------------------------------------------------------ */
/* the panel itself                                                    */

/*
 * Developer panel: an in-game D3D9 overlay, optionally also a loopback HTTP
 * server, driving a command queue that is drained on the game thread.
 *
 * Off unless HG_PANEL is set. See panel.c for why it is built this way.
 */

/* Started from the worker thread once the host binary has been verified. */
void panel_start(unsigned int image);

/* True if HG_PANEL asked for the panel. Read before installing spawn hooks. */
int  panel_wanted(void);

/*
 * Drained on the game thread. Called from the per-object physics step detour,
 * which is the one hook guaranteed to run on the thread that owns the world.
 */
void panel_pump(void);

#define PANEL_WATCH  6
#define PANEL_PEEKW  256
/*
 * Reporting windows of history carried in the snapshot. At one window per
 * 100ms that is 6.4 seconds, which is long enough to see a stall arrive and
 * short enough that the graph still has resolution. The whole investigation
 * is about a frame-time excursion, so being able to watch one happen is
 * worth 512 bytes of snapshot.
 */
#define PANEL_HIST   64

/*
 * Snapshot published by the game thread, consumed by the D3D9 overlay.
 *
 * The overlay runs on the render thread and must never block it, so it does
 * not use the request/wait path the HTTP server uses. Instead the pump
 * refreshes this struct in place and the overlay reads whatever the latest
 * stable copy is. A torn read would only ever cost one frame of scrambled
 * hex, but the seqlock removes even that.
 */
typedef struct {
    unsigned int   unit, flags;
    unsigned int   peek_addr, peek_off;
    int            peek_ok;
    char           name[48];
    unsigned char  peek[PANEL_PEEKW];

    /*
     * A baseline copy of the window, taken when the user asks for one. The
     * overlay draws every byte that differs from it in a different colour,
     * which is the whole technique for finding an unmapped offset: mark,
     * take a hit, see what moved.
     *
     * Deliberately a marked baseline rather than "the previous refresh".
     * The pump refreshes several times a frame, so a frame-to-frame diff
     * lights up everything that merely animates and tells you nothing.
     */
    unsigned char  mark[PANEL_PEEKW];
    int            mark_ok;
    unsigned int   mark_off;

    unsigned int   watch_off[PANEL_WATCH];
    unsigned int   watch_val[PANEL_WATCH];
    unsigned char  watch_ok[PANEL_WATCH];
    int            nwatch;

    long           qray, rays, bodies, pumps;
    double         qms;
    float          dtmin, dtavg, dtmax;

    /* Oldest first, ready to draw. `hist_n` is how many are populated. */
    float          hist_qms[PANEL_HIST];
    float          hist_dt[PANEL_HIST];
    int            hist_n;

    hg_spawn_state spawn;
    int            simtype_seen, simtype_override;
    int            cam_mode, fp_melee, fp_avail;
    hg_shoulder_state shoulder;
    hg_gfx_state      gfx;
    int            model_third;
    unsigned int   model_unit, model_gfx;
} panel_snap;

/* Returns 1 and fills `out` with a stable copy; 0 if the writer kept winning. */
int  panel_snap_read(panel_snap *out);

/* ---- overlay input. All safe to call from the render thread. ---- */

void panel_peek_nudge(int delta);
void panel_peek_set(unsigned int off);

/* Take (or drop) the baseline the hex view diffs against. */
void panel_peek_mark(int on);

/*
 * A poke posted from the render thread and applied by the pump. The overlay
 * must never block the render thread waiting on the game thread, so it
 * cannot use the HTTP server's request/wait slot. The outcome is written to
 * the log ring, which the panel's own Log tab shows.
 */
void panel_poke_async(unsigned int off, unsigned int value);

void panel_watch_add(unsigned int off);
void panel_watch_clear(void);

/* Published once per reporting window by report_window(). */
void panel_publish(unsigned int qray, unsigned int rays, long bodies,
                   double qms, float dtmin, float dtavg, float dtmax);

#endif
