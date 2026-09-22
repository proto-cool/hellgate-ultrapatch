/*
 * Action camera: a WoW-ActionCam-style third-person camera.
 *
 *   - over the shoulder, middle mouse swaps sides
 *   - height blends from a close-zoom value to a far-zoom value
 *   - dynamic pitch: zoomed out, the view tilts up toward the horizon
 *     instead of staring down at the top of your head
 *   - longer zoom than the stock 5 m
 *   - its own collision, which snaps in at a wall and eases back out
 *   - a short push-in and dip when you start a melee attack, for weight
 *
 * How: CameraUpdate (0x4d9efa) computes the whole camera every frame into
 * one global block, CAMERA_INFO at 0xf6f178, and CameraGetInfo() hands that
 * same block to the renderer and to mouse/aim picking. We post-hook it and
 * rebuild the eye and look-at from what the game produced. Picking reads the
 * same block, so the reticle keeps pointing where the shots go.
 *
 * What we keep from the game: the orbit direction (yaw and pitch from the
 * mouse) and the pivot (the look-at point at head height). What we replace:
 * where along that line the eye sits, the sideways offset, the heights, and
 * the collision. The game's own collision already ran and may have pulled
 * its eye in; the direction survives that, and the uncollided length can be
 * rebuilt exactly from the zoom distance (see cam_compose), so we cast our
 * own rays from our own pivot and do not inherit a clip the game computed
 * for a camera that is no longer there.
 *
 * Collision uses the game's own camera cast (0x587ebd) against the level's
 * Havok world. When the world is not reachable the camera falls back to the
 * old hedge: shrink the offset in proportion to how far the game had to
 * pull its own camera in.
 *
 * The math is kept free of Windows so test/ui.c can check it natively.
 */
#include <math.h>
#include "panel.h"

/*
 * The game's third-person zoom: the wheel steps the target distance by 0.5
 * (0x4dc364). The floor is 1.5 -- scrolling in past it drops to first
 * person -- and the ceiling is patched to a value of ours (see
 * shoulder_install), stock 5.
 */
#define CAM_ZOOM_MIN      1.5f
#define CAM_WALL_MARGIN   0.25f     /* keep the eye this far off any surface */
#define CAM_EASE_OUT      4.0f      /* per second, after a wall lets go       */

/* Returns the free distance along `d` (unit) from `o`, capped at `len`. */
typedef float (*cam_ray_fn)(void *ctx, const float *o, const float *d, float len);

typedef struct {
    float side;         /* -1..+1, smoothed; +1 is the right shoulder     */
    float offset;       /* metres sideways at side = +-1                  */
    float height;       /* metres added to eye and pivot                  */
    float lift;         /* metres added to the look-at only (dynamic pitch) */
    float dist;         /* the game's smoothed zoom distance              */
    float push;         /* metres the boom is shortened by (melee impulse) */
    float dip;          /* metres the look-at is lowered by (melee impulse) */
    int   orbit;        /* 1: true spherical orbit using `pitch`          */
    float pitch;        /* the game's view pitch, radians, CAMERA_INFO+0x34 */
} cam_params;

typedef struct {
    float s;            /* sideways metres currently allowed, eased       */
    float b;            /* boom metres currently allowed, eased           */
    int   primed;       /* s/b hold real values                           */
} cam_state;

/* ------------------------------------------------------------------ */
/* the pure part                                                       */

/* Blend a value across the zoom range, clamped at both ends. */
float cam_zoom_blend(float near_v, float far_v, float dist, float zmax)
{
    float t = zmax > CAM_ZOOM_MIN ? (dist - CAM_ZOOM_MIN) / (zmax - CAM_ZOOM_MIN) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return near_v + (far_v - near_v) * t;
}

/* Move `cur` toward `target` at `rate` per second, never overshooting. */
float shoulder_ease(float cur, float target, float rate, float dt)
{
    float a = rate * dt;
    if (a >= 1.0f || a < 0.0f) return target;
    return cur + (target - cur) * a;
}

/*
 * Melee impulse envelope, 0..1: a fast attack (linear over CAM_IMP_ATTACK)
 * then an exponential settle. Starts at 0 so a new swing never jumps.
 */
#define CAM_IMP_ATTACK 0.07f
#define CAM_IMP_SETTLE 0.20f
#define CAM_IMP_END    1.0f       /* past this it is treated as over */

float cam_impulse(float t)
{
    if (t < 0.0f || t > CAM_IMP_END) return 0.0f;
    if (t < CAM_IMP_ATTACK) return t / CAM_IMP_ATTACK;
    return expf(-(t - CAM_IMP_ATTACK) / CAM_IMP_SETTLE);
}

/* Snap in when a wall demands it, ease back out when it lets go. */
static float wall_ease(float cur, float allowed, float dt)
{
    return allowed < cur ? allowed : shoulder_ease(cur, allowed, CAM_EASE_OUT, dt);
}

/*
 * Build the final eye and look-at from the game's.
 *
 * eye0/at0 are what the game produced; eye/at are written. `ray` may be
 * NULL, which selects the fallback. Returns the fraction of the wanted
 * sideways offset in use, for the panel (1 = nothing is in the way).
 */
float cam_compose(const float *eye0, const float *at0, const cam_params *p,
                  cam_state *st, float dt, cam_ray_fn ray, void *ctx,
                  float *eye, float *at)
{
    float u[3] = { eye0[0] - at0[0], eye0[1] - at0[1], eye0[2] - at0[2] };
    float len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    float hu, want_b, want_s, r[3], piv[3], base[3], s, b, frac;
    int i;

    if (len < 1e-4f) {                      /* degenerate: leave it alone */
        for (i = 0; i < 3; i++) { eye[i] = eye0[i]; at[i] = at0[i]; }
        return 1.0f;
    }
    for (i = 0; i < 3; i++) u[i] /= len;

    /*
     * Uncollided boom length. The stock geometry puts the look-at 0.2 ahead
     * of the player and the eye `dist` behind, both horizontally, and the
     * pitch only moves the eye's z -- so the boom's horizontal extent is
     * exactly dist + 0.2 whatever the game's collision did to its length.
     */
    hu = sqrtf(u[0] * u[0] + u[1] * u[1]);
    if (hu < 0.05f) hu = 0.05f;             /* looking straight down */
    want_b = (p->dist + 0.2f) / hu;

    /*
     * True orbit. The stock camera keeps the eye `dist` behind horizontally
     * at every pitch and only raises it by dist*sin(pitch), so the view
     * never gets steeper than about 45 degrees however far the mouse goes
     * (it allows 85). Rebuild the direction as a real sphere: horizontal
     * shrinks by cos(pitch), and the view pitch is the mouse pitch.
     * eye.z - at.z = -dist*sin(pitch) in the stock formula (0x4da649), so
     * the vertical component is -sin(pitch).
     */
    if (p->orbit) {
        float c = cosf(p->pitch), sn = sinf(p->pitch);
        float bx = u[0] / hu, by = u[1] / hu;
        if (c < 0.0f) c = -c;               /* pitch wraps through 2*pi */
        u[0] = bx * c; u[1] = by * c; u[2] = -sn;
        hu = c > 0.05f ? c : 0.05f;
        want_b = p->dist + 0.2f;
    }

    /* Right is (-fwd.y, fwd.x) in the game's axes; fwd = -u. Checked in game. */
    {
        float h2 = sqrtf(u[0] * u[0] + u[1] * u[1]);
        if (h2 < 1e-4f) h2 = 1e-4f;
        r[0] = u[1] / h2;
        r[1] = -u[0] / h2;
    }
    r[2] = 0.0f;

    for (i = 0; i < 3; i++) piv[i] = at0[i];
    piv[2] += p->height;

    want_s = p->side * p->offset;           /* signed */

    if (ray) {
        /* Sideways first: the pivot must not end up inside a wall. */
        float dir = want_s < 0.0f ? -1.0f : 1.0f, rs[3];
        float free_s = fabsf(want_s) > 1e-4f
                     ? ray(ctx, piv, (rs[0] = r[0] * dir, rs[1] = r[1] * dir,
                                      rs[2] = 0.0f, rs), fabsf(want_s) + CAM_WALL_MARGIN)
                       - CAM_WALL_MARGIN
                     : 0.0f;
        if (free_s < 0.0f) free_s = 0.0f;
        if (free_s > fabsf(want_s)) free_s = fabsf(want_s);
        if (!st->primed) { st->s = free_s; }
        st->s = wall_ease(st->s, free_s, dt);
        s = st->s * dir;
        for (i = 0; i < 3; i++) base[i] = piv[i] + r[i] * s;

        /* Then the boom, from the shifted pivot along the orbit direction. */
        b = ray(ctx, base, u, want_b + CAM_WALL_MARGIN) - CAM_WALL_MARGIN;
        if (b < 0.1f) b = 0.1f;
        if (b > want_b) b = want_b;
        if (!st->primed) st->b = b;
        st->b = wall_ease(st->b, b, dt);
        st->primed = 1;
        b = st->b;
        frac = fabsf(want_s) > 1e-4f ? st->s / fabsf(want_s) : 1.0f;
    } else {
        /* Fallback: trust the game's collided length, scale the rest by it. */
        float k = len < want_b ? len / want_b : 1.0f;
        s = want_s * k;
        b = len;
        for (i = 0; i < 3; i++) base[i] = piv[i] + r[i] * s;
        st->primed = 0;
        frac = k;
    }

    /* The impulse rides on top of collision, and can only shorten the boom. */
    if (p->push > 0.0f) {
        b -= p->push;
        if (b < 0.3f) b = 0.3f;
    }

    for (i = 0; i < 3; i++) {
        eye[i] = base[i] + u[i] * b;
        at[i]  = base[i];
    }
    at[2] += p->lift;                       /* dynamic pitch: look-at only */
    at[2] -= p->dip;
    return frac;
}

#ifdef _WIN32
/* ------------------------------------------------------------------ */
/* the game-facing part                                                */

#include <windows.h>
#include <string.h>
#include "target.h"

typedef void (__cdecl *cam_update_fn)(void *game);
void animwatch_tick(void);

static cam_update_fn g_orig_update;
static float        *g_eye;         /* CAMERA_INFO.vPosition             */
static float        *g_at;          /* CAMERA_INFO.vLookAt               */
static const float  *g_dist;        /* smoothed zoom distance            */
static const int    *g_mode;        /* current camera mode               */
static void         *g_fn_ray;      /* the game's camera collision cast  */
static DWORD         g_pid;

/*
 * The zoom ceiling. The wheel handler reads it through the operand we
 * repoint at install; it has to live at a fixed address for that, which a
 * static does.
 */
static volatile float g_zoom_max = 10.0f;
static int            g_zoom_patched;

/* Settings: written from the render thread, read on the game thread. */
static volatile LONG g_on = 1;
static volatile LONG g_right = 1;          /* target shoulder             */
static volatile LONG g_off_mm = 600;       /* sideways, millimetres       */
static volatile LONG g_hn_mm = -500;       /* up at closest zoom, mm      */
static volatile LONG g_hf_mm = -400;       /* up at farthest zoom, mm     */
/*
 * Off by default. On an orbit camera, lifting the look-at makes the player
 * pitch the mouse further down to see ahead, and pitching down swings the
 * eye *up* -- the lift ends up causing the high camera it was meant to
 * hide. Tested in game 2026-09-21: "too high, have to look down so far".
 * A little is fine: these defaults (600 sideways, -500 close, -400 far,
 * +200 lift) are the values tuned in game the same day.
 */
static volatile LONG g_lift_mm = 200;        /* look-at lift at farthest zoom */
static volatile LONG g_collide = 1;
static volatile LONG g_orbit = 1;          /* true spherical orbit        */
static const float  *g_pitch;              /* CAMERA_INFO view pitch      */

/* Game thread only. */
static float     g_side;                   /* smoothed -1..+1             */
static cam_state g_st;
static LONGLONG  g_last;
static LARGE_INTEGER g_qpf;
static int       g_mmb_was;

/*
 * Melee impulse. The skill-start stub bumps g_melee_events; the camera sees
 * the change and restarts the envelope. g_game is the last CameraUpdate
 * argument, used to tell the local player's swings from everyone else's.
 */
static void * volatile g_game;
/* The client game the camera runs on: its control unit is the player's
 * client-side unit, the one that owns the graphics (the "local player"
 * getter hands back a unit with no pGfx, presumably the server copy). */
void *hg_client_game(void) { return g_game; }
static volatile LONG   g_melee_events, g_skill_events;
static LONG            g_melee_seen;
static float           g_imp_t = CAM_IMP_END + 1.0f;
static volatile LONG   g_imp_on = 1;
static volatile LONG   g_imp_pct = 100;         /* strength, percent */
static void           *g_fn_testflag;
void                  *g_impulse_tramp;         /* read by the stub below */

/*
 * The game's own look-at, before we move it: head height at the player.
 * The animation watcher uses it to find the player's model.
 */
volatile float g_cam_pivot[3];
volatile LONG  g_cam_pivot_ok;

/* For the panel. */
static volatile LONG g_frac_pct = 100, g_frames, g_zoom_mm, g_hcur_mm, g_have_world;

static int game_has_focus(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == g_pid;
}

/*
 * game -> controlled unit -> room -> level -> hkWorld. The same chain
 * CameraUpdate walks itself (0x505e09, then 0x45a709, then +0xb8), read
 * here as plain loads so no register-convention helpers are called.
 */
static void *level_world(void *game)
{
    unsigned char *g = (unsigned char *)game, *unit, *room, *level;
    if (!g || *(int *)(g + GAME_IS_SERVER)) return NULL;
    unit = *(unsigned char **)(g + GAME_CONTROL_UNIT);
    if (!unit) return NULL;
    room = *(unsigned char **)(unit + UNIT_ROOM);
    if (!room) return NULL;
    level = *(unsigned char **)(room + ROOM_LEVEL);
    if (!level) return NULL;
    return *(void **)(level + LEVEL_HKWORLD);
}

/*
 * 0x587ebd: origin in ecx, unit direction in eax, then (world, length,
 * flags) on the stack, caller cleans, free distance back in xmm0. Flags 0
 * is what the camera itself passes. ebx/esi/edi/ebp are preserved by it.
 */
static float game_ray(void *world, const float *o, const float *d, float len)
{
    float out;
    unsigned int lenbits;
    const float *oo = o, *dd = d;

    memcpy(&lenbits, &len, 4);
    __asm__ __volatile__(
        "pushl $0\n\t"
        "pushl %[len]\n\t"
        "pushl %[world]\n\t"
        "call *%[fn]\n\t"
        "addl $12, %%esp\n\t"
        "movss %%xmm0, %[out]\n\t"
        : [out] "=m" (out), "+c" (oo), "+a" (dd)
        : [len] "r" (lenbits), [world] "r" (world), [fn] "r" (g_fn_ray)
        /*
         * The callee trashes xmm0-7 too, but this DLL builds without -msse,
         * so GCC never keeps a value in an xmm register and there is nothing
         * to lose -- which is also why it refuses to accept them as clobbers.
         * Turning SSE on for the DLL would make these clobbers mandatory.
         */
        : "edx", "memory", "cc");
    if (!(out == out) || out < 0.0f) return 0.0f;     /* NaN or nonsense: blocked */
    return out > len ? len : out;
}

static float ray_cb(void *ctx, const float *o, const float *d, float len)
{
    return game_ray(ctx, o, d, len);
}

static void __cdecl detour_update(void *game)
{
    LARGE_INTEGER now;
    float dt, target, zmax, eye0[3], at0[3];
    cam_params p;
    void *world;
    int mmb;

    g_orig_update(game);
    g_game = game;
    animwatch_tick();

    QueryPerformanceCounter(&now);
    dt = g_last ? (float)(now.QuadPart - g_last) / (float)g_qpf.QuadPart : 0.0f;
    g_last = now.QuadPart;
    if (dt > 0.25f) dt = 0.25f;             /* a hitch is not a reason to snap */

    /*
     * Middle mouse swaps. Polled here rather than in the overlay so it works
     * with the panel closed; the focus check stops a click in another window
     * from swapping shoulders behind your back.
     */
    mmb = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    if (mmb && !g_mmb_was && game_has_focus() && g_on) {
        LONG r = !g_right;
        InterlockedExchange(&g_right, r);
        hg_log("camera: shoulder swapped to %s (middle mouse)", r ? "right" : "left");
    }
    g_mmb_was = mmb;

    target = !g_on ? 0.0f : g_right ? 1.0f : -1.0f;
    g_side = shoulder_ease(g_side, target, 8.0f, dt);

    if (g_melee_events != g_melee_seen) {
        g_melee_seen = g_melee_events;
        g_imp_t = 0.0f;                     /* a new swing restarts it */
    } else if (g_imp_t <= CAM_IMP_END) {
        g_imp_t += dt;
    }

    if (*g_mode != CAM_THIRD_PERSON) { g_st.primed = 0; return; }
    if (!g_on && g_side > -1e-3f && g_side < 1e-3f) { g_st.primed = 0; return; }

    zmax = g_zoom_patched ? g_zoom_max : 5.0f;
    p.side   = g_side;
    p.offset = (float)g_off_mm / 1000.0f;
    p.height = g_on ? cam_zoom_blend((float)g_hn_mm / 1000.0f,
                                     (float)g_hf_mm / 1000.0f, *g_dist, zmax) : 0.0f;
    p.lift   = g_on ? cam_zoom_blend(0.0f, (float)g_lift_mm / 1000.0f, *g_dist, zmax)
                    : 0.0f;
    p.dist   = *g_dist;
    p.orbit  = g_on && g_orbit;
    p.pitch  = *g_pitch;
    {
        float e = g_imp_on ? cam_impulse(g_imp_t) * (float)g_imp_pct / 100.0f : 0.0f;
        p.push = 0.25f * e;
        p.dip  = 0.06f * e;
    }

    world = (g_collide && g_fn_ray) ? level_world(game) : NULL;
    InterlockedExchange(&g_have_world, world != NULL);

    memcpy(eye0, g_eye, sizeof eye0);
    memcpy(at0, g_at, sizeof at0);
    g_cam_pivot[0] = at0[0]; g_cam_pivot[1] = at0[1]; g_cam_pivot[2] = at0[2];
    g_cam_pivot_ok = 1;
    {
        float f = cam_compose(eye0, at0, &p, &g_st, dt,
                              world ? ray_cb : NULL, world, g_eye, g_at);
        InterlockedExchange(&g_frac_pct, (LONG)(f * 100.0f + 0.5f));
    }
    InterlockedExchange(&g_zoom_mm, (LONG)(*g_dist * 1000.0f));
    InterlockedExchange(&g_hcur_mm, (LONG)(p.height * 1000.0f));
    InterlockedIncrement(&g_frames);
}

/*
 * UnitTestFlag (0x45a6bd): item in eax, flag on the stack, caller cleans,
 * result in eax. ecx and edx go through its callee.
 */
static int unit_test_flag(void *item, int flag)
{
    int r;
    void *it = item;
    __asm__ __volatile__(
        "pushl %[flag]\n\t"
        "call *%[fn]\n\t"
        "addl $4, %%esp\n\t"
        : "+a" (it)
        : [flag] "r" (flag), [fn] "r" (g_fn_testflag)
        : "ecx", "edx", "memory", "cc");
    r = (int)(unsigned int)it;
    return r;
}

/*
 * Called by the stub at skill start with the skill context (ebx there):
 * unit at +0x4, weapon at +0x8. Melee is the same two flags SetCameraMode
 * uses to keep melee weapons out of first person, asked the same way.
 */
void __cdecl impulse_on_skill(unsigned char *ctx)
{
    unsigned char *game = (unsigned char *)g_game, *unit, *item;

    InterlockedIncrement(&g_skill_events);
    if (!ctx || !game || *(int *)(game + GAME_IS_SERVER)) return;
    unit = *(unsigned char **)(ctx + SKILLCTX_UNIT);
    if (!unit || unit != *(unsigned char **)(game + GAME_PLAYER_UNIT)) return;
    item = *(unsigned char **)(ctx + SKILLCTX_WEAPON);
    if (!item) return;
    if (unit_test_flag(item, ITEMFLAG_NO_FP_A) || unit_test_flag(item, ITEMFLAG_NO_FP_B))
        InterlockedIncrement(&g_melee_events);
}

/*
 * Mid-function hook at 0x62b31b. Every register and the flags survive, then
 * the trampoline runs the displaced `cmp` and jumps back to the `jne` at
 * 0x62b322 -- which is the byte the first-person unlock patches, so the two
 * do not overlap.
 */
void impulse_stub(void);
__asm__(
    ".text\n\t"
    ".globl _impulse_stub\n"
    "_impulse_stub:\n\t"
    "pushal\n\t"
    "pushfl\n\t"
    "pushl %ebx\n\t"
    "call _impulse_on_skill\n\t"
    "addl $4, %esp\n\t"
    "popfl\n\t"
    "popal\n\t"
    "jmp *_g_impulse_tramp\n\t");

/*
 * Repoint the zoom ceiling's operand. The instruction is
 *     0x4dc3a8  movss xmm2, dword [0x00a0086c]      f3 0f 10 15 <addr32>
 * and 0xa0086c (5.0) is read by 102 other instructions, so the constant
 * itself must not be touched: only this one read is redirected.
 */
static int patch_zoom_max(unsigned int image)
{
    unsigned char *ins = (unsigned char *)(image + RVA_ZOOM_MAX_INSN);
    static const unsigned char want[8] = { 0xf3, 0x0f, 0x10, 0x15, 0x6c, 0x08, 0xa0, 0x00 };
    unsigned int addr = (unsigned int)&g_zoom_max;
    DWORD old;

    if (memcmp(ins, want, sizeof want) != 0) {
        hg_log("camera: zoom ceiling NOT patched -- bytes at %p are not the expected "
               "movss xmm2,[0xa0086c]; zoom stays at 5 m", (void *)ins);
        return 0;
    }
    if (!VirtualProtect(ins + 4, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(ins + 4, &addr, 4);
    VirtualProtect(ins + 4, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), ins, 8);
    return 1;
}

int shoulder_install(unsigned int image,
                     int (*hook)(unsigned int, void *, void **, const char *))
{
    g_eye    = (float *)(image + RVA_CAMERA_INFO + CAMINFO_EYE);
    g_at     = (float *)(image + RVA_CAMERA_INFO + CAMINFO_LOOKAT);
    g_dist   = (const float *)(image + RVA_CAMERA_DIST);
    g_pitch  = (const float *)(image + RVA_CAMERA_INFO + CAMINFO_PITCH);
    g_mode   = (const int *)(image + RVA_CAMERA_MODE_CUR);
    g_fn_ray = (void *)(image + RVA_CAMERA_RAY);
    g_pid    = GetCurrentProcessId();
    QueryPerformanceFrequency(&g_qpf);
    g_side = 1.0f;

    if (!hook(RVA_CAMERA_UPDATE, (void *)detour_update, (void **)&g_orig_update,
              "CameraUpdate (action camera)"))
        return 0;
    g_zoom_patched = patch_zoom_max(image);

    g_fn_testflag = (void *)(image + RVA_UNIT_TEST_FLAG);
    {
        static const unsigned char want[7] = { 0x83, 0x3d, 0x9c, 0x8e, 0xc4, 0x00, 0x02 };
        if (memcmp((void *)(image + RVA_SKILL_START_SITE), want, sizeof want) != 0 ||
            !hook(RVA_SKILL_START_SITE, (void *)impulse_stub, &g_impulse_tramp,
                  "skill start (melee camera impulse)"))
            hg_log("camera: melee impulse unavailable -- skill-start site did not match");
    }
    hg_log("camera: action camera on -- right shoulder %ld mm, height %+ld/%+ld mm "
           "close/far, pitch lift %ld mm, zoom to %.0f m%s; middle mouse swaps",
           (long)g_off_mm, (long)g_hn_mm, (long)g_hf_mm, (long)g_lift_mm,
           g_zoom_patched ? g_zoom_max : 5.0f,
           g_zoom_patched ? "" : " (ceiling patch failed)");
    return 1;
}

/* ------------------------------------------------------------------ */
/* panel-facing                                                        */

void hg_shoulder_status(hg_shoulder_state *o)
{
    o->installed      = g_orig_update != NULL;
    o->on             = (int)g_on;
    o->right          = (int)g_right;
    o->offset_mm      = (int)g_off_mm;
    o->height_near_mm = (int)g_hn_mm;
    o->height_far_mm  = (int)g_hf_mm;
    o->height_now_mm  = (int)g_hcur_mm;
    o->lift_mm        = (int)g_lift_mm;
    o->zoom_mm        = (int)g_zoom_mm;
    o->zoom_max_mm    = g_zoom_patched ? (int)(g_zoom_max * 1000.0f) : 5000;
    o->zoom_patched   = g_zoom_patched;
    o->collide        = (int)g_collide;
    o->orbit          = (int)g_orbit;
    o->have_world     = (int)g_have_world;
    o->hedge_pct      = (int)g_frac_pct;
    o->frames         = g_frames;
    o->impulse_avail  = g_impulse_tramp != NULL;
    o->impulse_on     = (int)g_imp_on;
    o->impulse_pct    = (int)g_imp_pct;
    o->skill_events   = g_skill_events;
    o->melee_events   = g_melee_events;
}

void hg_shoulder_set_on(int on)      { InterlockedExchange(&g_on, on ? 1 : 0); }
void hg_shoulder_swap(void)          { InterlockedExchange(&g_right, !g_right); }
void hg_shoulder_set_collide(int on) { InterlockedExchange(&g_collide, on ? 1 : 0); }
void hg_impulse_set_on(int on)       { InterlockedExchange(&g_imp_on, on ? 1 : 0); }
void hg_orbit_set_on(int on)         { InterlockedExchange(&g_orbit, on ? 1 : 0); }

static LONG clampl(LONG v, LONG lo, LONG hi) { return v < lo ? lo : v > hi ? hi : v; }

void hg_shoulder_nudge(int d_off_mm, int d_near_mm, int d_far_mm)
{
    InterlockedExchange(&g_off_mm, clampl(g_off_mm + d_off_mm, 0, 1500));
    InterlockedExchange(&g_hn_mm, clampl(g_hn_mm + d_near_mm, -1500, 1500));
    InterlockedExchange(&g_hf_mm, clampl(g_hf_mm + d_far_mm, -1500, 1500));
}

void hg_shoulder_nudge_zoom(int d_lift_mm, int d_zoom_max_m)
{
    float z = g_zoom_max + (float)d_zoom_max_m;
    InterlockedExchange(&g_lift_mm, clampl(g_lift_mm + d_lift_mm, -1000, 3000));
    if (z < 5.0f) z = 5.0f;
    if (z > 25.0f) z = 25.0f;
    g_zoom_max = z;         /* one aligned float store; the reader is the wheel */
}
void hg_impulse_nudge(int d_pct)
{
    InterlockedExchange(&g_imp_pct, clampl(g_imp_pct + d_pct, 0, 300));
}
#endif
