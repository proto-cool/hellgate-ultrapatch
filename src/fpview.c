/*
 * First person: the weapon's view model placed and swayed, the way
 * src/shoulder.c makes the third-person camera feel better.
 *
 * How the engine draws it. FUN_00778f55 builds the camera's matrices for
 * whoever asks: the view, the projection, and (its third stack argument)
 * a second projection, built from the same camera with the field of view
 * replaced by a constant (0xa81ad0, 0.589 rad). Models with
 * MODEL_FLAGBIT_FIRST_PERSON_PROJ are drawn with that one (the render
 * context's +0x80, filled by sRenderContextSetViewParams 0x7eef0c and read
 * by sGetMatrices 0x7c7f91): the first-person weapon and arms. The
 * weapon's particles (its muzzle flash) ask FUN_00778f55 for their own
 * copy, so the transform goes in there, after the game built it, and every
 * consumer gets the same weapon (in the render context alone, the flash
 * stayed where the stock weapon was, 2026-09-25):
 *
 *     P' = M * P * S      M: rotate, then move, in view space (the model's
 *                            own position is left alone: it stays at the eye)
 *                         S: scales x and y of clip space (the weapon's FOV)
 *
 * M carries the offsets you set (position, angles) and the motion: sway
 * (the weapon lags behind the view as you turn), tilt (into a strafe), a
 * dip on landing and a recoil kick when you fire. No bob: the first-person
 * animations carry their own. The camera itself is never moved: no kick,
 * no shake.
 *
 * The motion reads the eye (+0xc0) and the view's axes from the main
 * render context once a frame (sRenderContextSetViewParams, index 0 or
 * -1), and so does the body's clip plane (gfxprobe.c body_clip).
 *
 * Calling conventions, both hand-shimmed: sRenderContextSetViewParams takes
 * the viewer in ecx and two stack arguments the CALLER cleans (`pop ecx;
 * pop ecx` after the call at 0x7ef447); FUN_00778f55 takes ecx, eax and six
 * stack arguments, caller-cleaned (plain `ret`).
 */
#include <windows.h>
#include <math.h>
#include <string.h>
#include "target.h"
#include "panel.h"

void hg_log(const char *fmt, ...);
LONG shoulder_fire_events(void);

#define RVA_SET_VIEW_PARAMS 0x003EEF0Cu     /* sRenderContextSetViewParams, 0x7eef0c */
#define RVA_GET_MATRICES    0x00378F55u     /* FUN_00778f55: view, projection, weapon projection */
#define FP_STOCK_FOV        0.5890486f      /* 0xa81ad0: the weapon's field of view (radians) */

/* settings (panel: First person) */
/* the defaults are the user's own tuning (2026-09-26) */
static volatile LONG g_vm_x = -20, g_vm_y = -20, g_vm_z = 30;            /* position, mm (right, up, forward) */
static volatile LONG g_vm_pitch = -40, g_vm_yaw = -15, g_vm_roll = 35;  /* angles, tenths of a degree */
/* in a safe level (a station, a town: no weapons) the weapon eases down and
 * to the left, away from people's faces; the user's pose (2026-09-25) */
static volatile LONG g_safe_on = 1;
static volatile LONG g_safe_x = 65, g_safe_y = -15, g_safe_z = 25;
static volatile LONG g_safe_pitch = 80, g_safe_yaw = -300, g_safe_roll = 30;
int hg_player_safe_level(void);
/* holding two guns (gfxprobe sees one each side, fpview_set_dual): a pose
 * of its own, for the right gun; the left takes it mirrored (2026-09-25) */
static volatile LONG g_dual_x = -10, g_dual_y = -30, g_dual_z = 80;       /* the user's tuning (2026-09-25) */
static volatile LONG g_dual_pitch = -20, g_dual_yaw = 25, g_dual_roll = 150;
static volatile LONG g_dual_fov = 495;
/* two guns in a town: lowered, the right one's; the left mirrored */
static volatile LONG g_dtown_x = -15, g_dtown_y = 55, g_dtown_z = 70;     /* the user's tuning (2026-09-25) */
static volatile LONG g_dtown_pitch = 240, g_dtown_yaw = 80, g_dtown_roll = 80;
static volatile LONG g_dtown_fov = 630, g_safe_fov = 440;
/* one gun in one hand (a pistol) */
static volatile LONG g_one_x = -40, g_one_y = -40, g_one_z = 65;          /* the user's tuning (2026-09-25) */
static volatile LONG g_one_pitch = -50, g_one_yaw = -10, g_one_roll = 60, g_one_fov = 495;
static volatile LONG g_otown_x = 5, g_otown_y = -15, g_otown_z = 25;         /* the user's tuning (2026-09-25) */
static volatile LONG g_otown_pitch = 115, g_otown_yaw = -150, g_otown_roll = 155, g_otown_fov = 460;
int hg_player_hands(void);
static volatile LONG g_dual_now;
static volatile LONG g_dual = 1;                        /* setting: the pose, and the left gun mirrored */
void fpview_set_dual(int on) { g_dual_now = on; }
static volatile LONG g_vm_fov = 450;                    /* the weapon's field of view, tenths of a degree (stock 33.8) */
/* no bob: the first-person animations carry their own (2026-09-25) */
static volatile LONG g_sway = 100, g_tilt = 100, g_land = 100, g_recoil = 100;   /* percent */
static volatile LONG g_vm_on = 1;
static volatile LONG g_flash = 100;                     /* the muzzle flash's light, percent */
static volatile LONG g_vm_fill = 35;                    /* the weapon's fill light, percent (the characters' is 12) */
static volatile LONG g_sprint_fov = 60;                 /* sprinting widens the view this much, tenths of a degree */
static volatile LONG g_anim_ease = 150;                 /* first-person animations blend at least this long, ms (hook.c) */

/* the main camera, for the muzzle flash's light */
static float g_eye_now[3], g_fwd_now[3], g_up_now[3], g_right_now[3];
static volatile DWORD g_eye_ms;         /* when the main camera was last seen */

/* The main camera's position, every 3D frame (character select too, where
 * the engine sets no shadow parameters): 1 if seen in the last 100 ms. */
int fpview_eye(float out[3])
{
    if (!g_eye_ms || GetTickCount() - g_eye_ms > 100) return 0;
    memcpy(out, g_eye_now, 12);
    return 1;
}
static float g_main_p11 = 1, g_main_p22 = 1, g_fp_p11 = 1, g_fp_p22 = 1;   /* the projections' scales */
static volatile LONG g_have_vp;

static void *g_orig;
static const int *g_mode;               /* the live camera mode: 0 is first person */
static LARGE_INTEGER g_qpf, g_last;
static LONG g_calls, g_frames_moved;

/* a critically damped spring: x follows target, stiffness k (1/s^2) */
typedef struct { float x, v; } spring;

static void spring_step(spring *s, float target, float k, float dt)
{
    float w = sqrtf(k);
    s->v += (k * (target - s->x) - 2.0f * w * s->v) * dt;
    s->x += s->v * dt;
}

static struct {
    int have;
    float eye[3], fwd[3], right[3], up[3];
    float vz_prev;
    float safe;                     /* 0 the aiming pose .. 1 the town pose, eased */
    float dual;                     /* 0 the aiming pose .. 1 the dual wield pose, eased */
    float one;                      /* 0 the aiming pose .. 1 the one-handed pose, eased */
    float left;                     /* 1: one gun in the left hand alone, the pose mirrored; eased */
    spring sway_y, sway_p, tilt, land, kick_z, kick_p;
    LONG fire_seen;
    float M[16];
    float ML[16];                   /* the same for a gun in the left hand: the pose mirrored */
} S;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* row-vector 4x4 multiply: o = a * b */
static void mul44(float *o, const float *a, const float *b)
{
    float t[16];
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, sizeof t);
}

static void ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }

/* D3DX-style rotations for row vectors (left-handed) */
static void rot_x(float *m, float a) { ident(m); m[5] = cosf(a); m[6] = sinf(a); m[9] = -sinf(a); m[10] = cosf(a); }
static void rot_y(float *m, float a) { ident(m); m[0] = cosf(a); m[2] = -sinf(a); m[8] = sinf(a); m[10] = cosf(a); }
static void rot_z(float *m, float a) { ident(m); m[0] = cosf(a); m[1] = sinf(a); m[4] = -sinf(a); m[5] = cosf(a); }

/* Once a frame, from the main context: the motion, and M. */
static void update(const float *view, const float *eye)
{
    LARGE_INTEGER now;
    float dt, f[3], r[3], u[3], vel[3], lat, turn_r, turn_u, k;
    float Rx[16], Ry[16], Rz[16], T[16];
    LONG fire;

    f[0] = view[2]; f[1] = view[6]; f[2] = view[10];     /* the view's columns: right, up, forward */
    r[0] = view[0]; r[1] = view[4]; r[2] = view[8];
    u[0] = view[1]; u[1] = view[5]; u[2] = view[9];
    QueryPerformanceCounter(&now);
    dt = S.have ? (float)(now.QuadPart - g_last.QuadPart) / (float)g_qpf.QuadPart : 0.0f;
    g_last = now;
    if (dt <= 0.0f || dt > 0.25f) {         /* the first frame, or a pause: start still */
        memcpy(S.eye, eye, 12); memcpy(S.fwd, f, 12); memcpy(S.right, r, 12); memcpy(S.up, u, 12);
        S.have = 1;
        S.vz_prev = 0;
        dt = 0.0f;
    }
    if (dt > 0.05f) dt = 0.05f;

    if (dt > 0.0f) {
        vel[0] = (eye[0] - S.eye[0]) / dt; vel[1] = (eye[1] - S.eye[1]) / dt; vel[2] = (eye[2] - S.eye[2]) / dt;
        /* turning: the new forward along last frame's right and up */
        turn_r = dot3(f, S.right) / dt;
        turn_u = dot3(f, S.up) / dt;
        lat = (vel[0] * r[0] + vel[1] * r[1]) / (sqrtf(r[0] * r[0] + r[1] * r[1]) + 1e-4f);

        /* sway: the weapon lags the turn, a little, and settles */
        k = g_sway / 100.0f;
        spring_step(&S.sway_y, clampf(-turn_r * 0.035f, -0.08f, 0.08f) * k, 90.0f, dt);
        spring_step(&S.sway_p, clampf(turn_u * 0.035f, -0.06f, 0.06f) * k, 90.0f, dt);

        /* tilt: rolled away from the strafe */
        spring_step(&S.tilt, clampf(-lat / 6.0f, -1.0f, 1.0f) * 0.07f * (g_tilt / 100.0f), 60.0f, dt);

        /* landing: falling, then stopped */
        if (S.vz_prev < -3.0f && vel[2] > -0.5f)
            S.land.v -= clampf(-S.vz_prev / 10.0f, 0.2f, 1.2f) * 0.9f * (g_land / 100.0f);
        S.vz_prev = vel[2];
        spring_step(&S.land, 0.0f, 120.0f, dt);

        /* recoil: a kick back and up on each shot, back quickly */
        fire = shoulder_fire_events();
        if (fire != S.fire_seen) {
            float kr = g_recoil / 100.0f;
            S.kick_z.v -= 1.2f * kr;
            S.kick_p.v -= 2.2f * kr;
            S.fire_seen = fire;
        }
        spring_step(&S.kick_z, 0.0f, 400.0f, dt);
        spring_step(&S.kick_p, 0.0f, 400.0f, dt);
    }
    memcpy(S.eye, eye, 12); memcpy(S.fwd, f, 12); memcpy(S.right, r, 12); memcpy(S.up, u, 12);
    S.safe += ((g_safe_on && hg_player_safe_level() ? 1.0f : 0.0f) - S.safe) * clampf(dt * 5.0f, 0.0f, 1.0f);
    S.dual += ((g_dual && g_dual_now ? 1.0f : 0.0f) - S.dual) * clampf(dt * 5.0f, 0.0f, 1.0f);
    {
        int hands = hg_player_hands();
        S.one += ((hands == 1 || hands == 3 ? 1.0f : 0.0f) - S.one) * clampf(dt * 5.0f, 0.0f, 1.0f);
        S.left += ((hands == 3 ? 1.0f : 0.0f) - S.left) * clampf(dt * 5.0f, 0.0f, 1.0f);
    }

    {
        const float d2r = 3.14159265f / 1800.0f;       /* tenths of a degree */
        const float aim[6] = { g_vm_x, g_vm_y, g_vm_z, g_vm_pitch, g_vm_yaw, g_vm_roll };
        const float one[6] = { g_one_x, g_one_y, g_one_z, g_one_pitch, g_one_yaw, g_one_roll };
        const float two[6] = { g_dual_x, g_dual_y, g_dual_z, g_dual_pitch, g_dual_yaw, g_dual_roll };
        const float twn[6] = { g_safe_x, g_safe_y, g_safe_z, g_safe_pitch, g_safe_yaw, g_safe_roll };
        const float otw[6] = { g_otown_x, g_otown_y, g_otown_z, g_otown_pitch, g_otown_yaw, g_otown_roll };
        const float dtw[6] = { g_dtown_x, g_dtown_y, g_dtown_z, g_dtown_pitch, g_dtown_yaw, g_dtown_roll };
        float p[6];
        int i;
        for (i = 0; i < 6; i++) {
            /* two-handed, one-handed or dual; aiming or in a town */
            float a = aim[i] + (one[i] - aim[i]) * S.one, t = twn[i] + (otw[i] - twn[i]) * S.one;
            a += (two[i] - a) * S.dual;
            t += (dtw[i] - t) * S.dual;
            p[i] = a + (t - a) * S.safe;
        }
        /* one gun in the left hand alone: the pose mirrored across your
         * centre line (it took the right hand's, 2026-09-26) */
        p[0] *= 1.0f - 2.0f * S.left;
        p[4] *= 1.0f - 2.0f * S.left;
        p[5] *= 1.0f - 2.0f * S.left;
        float ox = p[0] / 1000.0f + S.sway_y.x * 0.15f;
        float oy = p[1] / 1000.0f + S.land.x * 0.05f;
        float oz = p[2] / 1000.0f + S.kick_z.x * 0.03f;
        float pitch = p[3] * d2r + S.sway_p.x + S.kick_p.x * 0.03f + S.land.x * 0.02f;
        float yaw = p[4] * d2r + S.sway_y.x;
        float roll = p[5] * d2r + S.tilt.x;
        /* the left hand's (dual wield): the pose mirrored across your
         * centre line, the motion as it is (both guns sway the same way) */
        float lx = -p[0] / 1000.0f + S.sway_y.x * 0.15f;
        float lyaw = -p[4] * d2r + S.sway_y.x;
        float lroll = -p[5] * d2r + S.tilt.x;
        rot_z(Rz, roll); rot_x(Rx, pitch); rot_y(Ry, yaw);
        ident(T); T[12] = ox; T[13] = oy; T[14] = oz;
        mul44(S.M, Rz, Rx);
        mul44(S.M, S.M, Ry);
        mul44(S.M, S.M, T);
        rot_z(Rz, lroll); rot_y(Ry, lyaw);
        T[12] = lx;
        mul44(S.ML, Rz, Rx);
        mul44(S.ML, S.ML, Ry);
        mul44(S.ML, S.ML, T);
    }
    g_frames_moved++;
}

/* 4x4 inverse (row-major), 0 if singular */
static int inv44(float *o, const float *m)
{
    float inv[16], det;
    int i;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return 0;
    det = 1.0f / det;
    for (i = 0; i < 16; i++) o[i] = inv[i] * det;
    return 1;
}

LONG fpview_anim_ease_ms(void) { return g_anim_ease; }
/* the fill light for the first-person weapon's draws, 0..1; negative: none
 * (not first person) */
float fpview_weapon_fill(void) { return g_mode && *g_mode == 0 ? g_vm_fill / 100.0f : -1.0f; }

/*
 * The muzzle flash's light, for the light spill pass (src/postfx.c spill):
 * a warm light just ahead of the weapon for about 50 ms after each shot
 * (a fire event: the player's skill start with a gun, src/shoulder.c),
 * in first person ahead of and below the eye, in third person ahead of the
 * head (the camera's look-at). Returns 0 when there is none.
 */
static LARGE_INTEGER g_fire_t;
static LONG g_fire_seen2;
static const float *g_lookat;

int fpview_flash(float pos[3], float rgb[3], float *radius)
{
    LARGE_INTEGER now;
    float t, e, k = g_flash / 100.0f;
    LONG f = shoulder_fire_events();
    QueryPerformanceCounter(&now);
    if (f != g_fire_seen2) { g_fire_seen2 = f; g_fire_t = now; }
    if (k <= 0 || !g_fire_t.QuadPart || !g_have_vp) return 0;
    t = (float)(now.QuadPart - g_fire_t.QuadPart) / (float)g_qpf.QuadPart;
    if (t > 0.25f) return 0;
    e = expf(-t / 0.05f) * k;
    if (g_mode && *g_mode == 0) {
        /* the barrel's end in the weapon's view space, moved by M, then
         * where it shows on screen taken back to the main camera at the
         * same depth (the weapon is drawn with its own projection) */
        float m[4] = { 0.08f, -0.10f, 0.75f, 1.0f }, p[3], q[3];
        int c;
        for (c = 0; c < 3; c++)
            p[c] = g_vm_on ? m[0] * S.M[c] + m[1] * S.M[4 + c] + m[2] * S.M[8 + c] + S.M[12 + c] : m[c];
        q[0] = p[0] * (g_vm_on ? g_fp_p11 : g_main_p11) / g_main_p11;
        q[1] = p[1] * (g_vm_on ? g_fp_p22 : g_main_p22) / g_main_p22;
        q[2] = p[2];
        for (c = 0; c < 3; c++)
            pos[c] = g_eye_now[c] + g_right_now[c] * q[0] + g_up_now[c] * q[1] + g_fwd_now[c] * q[2];
    } else if (g_lookat) {
        pos[0] = g_lookat[0] + g_fwd_now[0] * 0.8f;
        pos[1] = g_lookat[1] + g_fwd_now[1] * 0.8f;
        pos[2] = g_lookat[2] + g_fwd_now[2] * 0.8f;
    } else return 0;
    rgb[0] = 1.0f * 2.5f * e; rgb[1] = 0.72f * 2.5f * e; rgb[2] = 0.40f * 2.5f * e;
    *radius = 6.0f;
    return 1;
}

/*
 * Sprinting widens the view a little (fp.sprint_fov), eased in and out.
 * Sprint is a skill, Swiftness_Boost: one press puts a buff on you, the
 * state swiftness_boost (states.txt row 136; speed_burst, row 86, was a
 * guess that never showed, 2026-09-26, and is still asked), asked of the
 * game each frame (hg_player_has_state);
 * while you move across the ground (the eye in first person, the camera's
 * look-at, your head, in third, where the eye swings with the camera).
 * Once a frame from the main context.
 */
#define STATE_SWIFTNESS_BOOST 136
#define STATE_SPEED_BURST 86
int hg_player_has_state(int state);
static struct { float x, y, speed, k; LARGE_INTEGER t; int have; } g_spr;

static void sprint_update(const float *eye)
{
    LARGE_INTEGER now;
    const float *p = g_mode && *g_mode != 0 && g_lookat ? g_lookat : eye;
    float dt, v, target;
    QueryPerformanceCounter(&now);
    dt = g_spr.have ? (float)(now.QuadPart - g_spr.t.QuadPart) / (float)g_qpf.QuadPart : 0.0f;
    g_spr.t = now;
    if (!g_spr.have || dt <= 0.0f || dt > 0.25f) {
        g_spr.x = p[0]; g_spr.y = p[1]; g_spr.have = 1;
        return;
    }
    v = sqrtf((p[0] - g_spr.x) * (p[0] - g_spr.x) + (p[1] - g_spr.y) * (p[1] - g_spr.y)) / dt;
    g_spr.x = p[0]; g_spr.y = p[1];
    if (v > 30.0f) v = g_spr.speed;                 /* a teleport, a zone change */
    g_spr.speed += (v - g_spr.speed) * clampf(dt * 8.0f, 0.0f, 1.0f);
    {
        static int was = -1;
        int on = hg_player_has_state(STATE_SWIFTNESS_BOOST) ? 1 : hg_player_has_state(STATE_SPEED_BURST) ? 2 : 0;
        if (on != was) hg_log("fpview: sprint %s", on == 1 ? "on (swiftness_boost)" : on == 2 ? "on (speed_burst)" : "off");
        was = on;
        target = on && g_spr.speed > 1.5f ? 1.0f : 0.0f;
    }
    /* in quickly, out a little slower */
    g_spr.k += (target - g_spr.k) * clampf(dt * (target > g_spr.k ? 6.0f : 4.0f), 0.0f, 1.0f);
}

/* Called for every context the game fills; rewrites its weapon projection. */
int __cdecl fp_after_view_params(void *viewer, unsigned char *ctx, int index)
{
    int r;
    __asm__ __volatile__("pushl %[i]\n\t"
                         "pushl %[c]\n\t"
                         "call *%[fn]\n\t"
                         "addl $8, %%esp"
                         : "=a"(r)
                         : [fn] "r"(g_orig), "c"(viewer), [c] "r"(ctx), [i] "r"(index)
                         : "edx", "cc", "memory");
    InterlockedIncrement(&g_calls);
    if (r < 0 || !ctx) return r;
    if (index == 0 || index == -1) {            /* the main camera, for the muzzle flash's light */
        const float *V = (const float *)ctx;
        memcpy(g_eye_now, ctx + 0xc0, 12);
        g_eye_ms = GetTickCount();
        sprint_update(g_eye_now);
        g_fwd_now[0] = V[2]; g_fwd_now[1] = V[6]; g_fwd_now[2] = V[10];
        g_up_now[0] = V[1]; g_up_now[1] = V[5]; g_up_now[2] = V[9];
        g_right_now[0] = V[0]; g_right_now[1] = V[4]; g_right_now[2] = V[8];
        InterlockedExchange(&g_have_vp, 1);
    }
    if (index == 0 || index == -1) {
        g_main_p11 = ((const float *)(ctx + 0x40))[0];
        g_main_p22 = ((const float *)(ctx + 0x40))[5];
    }
    if (!g_vm_on || !g_mode || *g_mode != 0) return r;
    if (index == 0 || index == -1) update((const float *)ctx, (const float *)(ctx + 0xc0));
    return r;
}

/*
 * Dual wield: the engine draws both guns, and both arms, with the weapon
 * projection, so M put the left gun where the right one's tuning says
 * (2026-09-25). For a draw in the left hand gfxprobe's draw hook turns M
 * into ML: D = ML x inverse(M) in the weapon's view space, and
 * C = inverse(P) x D x P the same in clip space. The engine builds more
 * than one weapon projection a frame (a context each), and the arms are
 * drawn with another than the guns: the hand stayed where it was while
 * its gun moved (2026-09-25). Each one seen (told apart by its stock
 * values) is kept with its inverse and C, for the draw hook to find the
 * one a draw was made with.
 */
#define WP_MAX 4
static struct {
    float key[4];                   /* the stock projection's _11 _22 _33 _43 */
    float inv[16], c[16];
    LONG seen;                      /* g_frames_moved when last built */
} g_wp[WP_MAX];
static float g_mir_d[16];
static volatile LONG g_mir_ok;

int fpview_inv44(float *o, const float *m) { return inv44(o, m); }

/* the weapon projections of the last frames: inverse and C (C null when the
 * left hand's pose is not wanted); the count */
int fpview_weapon_projs(const float *inv[WP_MAX], const float *c[WP_MAX])
{
    int i, n = 0, mir = g_dual && g_mir_ok && g_vm_on;
    if (!g_mode || *g_mode != 0) return 0;
    for (i = 0; i < WP_MAX; i++)
        if (g_wp[i].seen && g_frames_moved - g_wp[i].seen <= 3) {
            inv[n] = g_wp[i].inv;
            c[n] = mir ? g_wp[i].c : NULL;
            n++;
        }
    return n;
}

/* M, the view model's pose (the weapon's view space to where it is drawn),
 * for its own shadow map (gfxprobe.c vm_cast); identity when off */
const float *fpview_vm_M(void)
{
    static float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    return S.have && g_vm_on && g_mode && *g_mode == 0 ? S.M : I;
}

/* D when the left hand's pose is wanted */
const float *fpview_mirror_d(void)
{
    return g_dual && g_mir_ok && g_vm_on && g_mode && *g_mode == 0 ? g_mir_d : NULL;
}

/* After FUN_00778f55: its weapon projection (null when not asked for),
 * with M and the pose's field of view put in. */
void __cdecl fp_after_matrices(float *P, float *Pmain)
{
    float fov, s, a, t, key[4], Minv[16], tmp[16];
    int c, i, slot = -1, old = 0;
    /* sprinting: the main projection's field of view, wider (the
     * weapon's widens by as much, below) */
    if (Pmain && g_spr.k > 0.001f && g_sprint_fov > 0 && Pmain[5] > 0.0f) {
        float half = atanf(1.0f / Pmain[5]), add = g_sprint_fov / 10.0f * 3.14159265f / 360.0f * g_spr.k;
        float k = tanf(half) / tanf(clampf(half + add, 0.05f, 1.5f));
        Pmain[0] *= k;
        Pmain[5] *= k;
    }
    if (!P || !g_mode || *g_mode != 0) return;
    key[0] = P[0]; key[1] = P[5]; key[2] = P[10]; key[3] = P[14];
    if (S.have && g_vm_on) {
        a = g_vm_fov + (g_one_fov - g_vm_fov) * S.one;
        t = g_safe_fov + (g_otown_fov - g_safe_fov) * S.one;
        a += (g_dual_fov - a) * S.dual;
        t += (g_dtown_fov - t) * S.dual;
        a += (t - a) * S.safe;
        a += g_sprint_fov * g_spr.k;                /* sprinting: the view model widens with the view */
        fov = clampf(a / 10.0f, 10.0f, 120.0f) * 3.14159265f / 180.0f;
        s = tanf(FP_STOCK_FOV * 0.5f) / tanf(fov * 0.5f);
        mul44(P, S.M, P);
        for (c = 0; c < 4; c++) { P[c * 4] *= s; P[c * 4 + 1] *= s; }    /* x and y of clip space */
        g_fp_p11 = P[0];
        g_fp_p22 = P[5];
        if (inv44(Minv, S.M)) { mul44(g_mir_d, S.ML, Minv); g_mir_ok = 1; }
        else g_mir_ok = 0;
    } else
        g_mir_ok = 0;
    for (i = 0; i < WP_MAX; i++) {
        if (g_wp[i].seen && !memcmp(g_wp[i].key, key, sizeof key)) { slot = i; break; }
        if (slot < 0 || g_wp[i].seen < g_wp[old].seen) old = i;
    }
    if (slot < 0) {
        for (i = 0; i < WP_MAX && g_wp[i].seen; i++) ;
        slot = i < WP_MAX ? i : old;
        memcpy(g_wp[slot].key, key, sizeof key);
    }
    if (!inv44(g_wp[slot].inv, P)) { g_wp[slot].seen = 0; return; }
    if (g_mir_ok) {
        mul44(tmp, g_wp[slot].inv, g_mir_d);
        mul44(g_wp[slot].c, tmp, P);
    }
    g_wp[slot].seen = g_frames_moved ? g_frames_moved : 1;
}

static void *g_orig_mats;
void fp_matrices_stub(void);
__asm__(
    ".text\n\t"
    ".globl _fp_matrices_stub\n"
    "_fp_matrices_stub:\n\t"
    "pushl 24(%esp)\n\t"       /* the six arguments again, last first (each push */
    "pushl 24(%esp)\n\t"       /* moves the next one up to the same offset); */
    "pushl 24(%esp)\n\t"       /* eax and ecx still hold what the caller put */
    "pushl 24(%esp)\n\t"
    "pushl 24(%esp)\n\t"
    "pushl 24(%esp)\n\t"
    "call *_g_orig_mats\n\t"
    "addl $24, %esp\n\t"
    "pushl %eax\n\t"           /* what it returns: some of its 14 callers read it */
    "pushl %edx\n\t"
    "pushl %ecx\n\t"
    "pushl 20(%esp)\n\t"       /* the second argument (past the three saved): the projection */
    "pushl 28(%esp)\n\t"       /* the third (past those and the push): the weapon projection */
    "call _fp_after_matrices\n\t"
    "addl $8, %esp\n\t"
    "popl %ecx\n\t"
    "popl %edx\n\t"
    "popl %eax\n\t"
    "ret\n\t");

void fp_view_params_stub(void);
__asm__(
    ".text\n\t"
    ".globl _fp_view_params_stub\n"
    "_fp_view_params_stub:\n\t"
    "pushl 8(%esp)\n\t"         /* index */
    "pushl 8(%esp)\n\t"         /* ctx (the push moved it up by 4) */
    "pushl %ecx\n\t"            /* viewer */
    "call _fp_after_view_params\n\t"
    "addl $12, %esp\n\t"
    "ret\n\t");

int fpview_install(unsigned int image, int (*hook)(unsigned int, void *, void **, const char *))
{
    static const unsigned char want[9] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x64 };
    QueryPerformanceFrequency(&g_qpf);
    g_mode = (const int *)(image + RVA_CAMERA_MODE_CUR);
    g_lookat = (const float *)(image + RVA_CAMERA_INFO + CAMINFO_LOOKAT);
    settings_var("fp.vm_on", &g_vm_on, 0, 1);
    settings_var("fp.vm_x", &g_vm_x, -300, 300);
    settings_var("fp.dual_mirror", &g_dual, 0, 1);
    settings_var("fp.sprint_fov", &g_sprint_fov, 0, 200);
    settings_var("fp.vm_y", &g_vm_y, -300, 300);
    settings_var("fp.vm_z", &g_vm_z, -300, 300);
    settings_var("fp.vm_pitch", &g_vm_pitch, -300, 300);
    settings_var("fp.vm_yaw", &g_vm_yaw, -300, 300);
    settings_var("fp.vm_roll", &g_vm_roll, -300, 300);
    settings_var("fp.vm_fov", &g_vm_fov, 150, 900);
    settings_var("fp.safe_on", &g_safe_on, 0, 1);
    settings_var("fp.dual_x", &g_dual_x, -300, 300);
    settings_var("fp.dual_y", &g_dual_y, -300, 300);
    settings_var("fp.dual_z", &g_dual_z, -300, 300);
    settings_var("fp.dual_pitch", &g_dual_pitch, -600, 600);
    settings_var("fp.dual_yaw", &g_dual_yaw, -600, 600);
    settings_var("fp.dual_roll", &g_dual_roll, -600, 600);
    settings_var("fp.dual_fov", &g_dual_fov, 150, 900);
    settings_var("fp.dtown_x", &g_dtown_x, -300, 300);
    settings_var("fp.dtown_y", &g_dtown_y, -300, 300);
    settings_var("fp.dtown_z", &g_dtown_z, -300, 300);
    settings_var("fp.dtown_pitch", &g_dtown_pitch, -600, 600);
    settings_var("fp.dtown_yaw", &g_dtown_yaw, -600, 600);
    settings_var("fp.dtown_roll", &g_dtown_roll, -600, 600);
    settings_var("fp.dtown_fov", &g_dtown_fov, 150, 900);
    settings_var("fp.safe_fov", &g_safe_fov, 150, 900);
    settings_var("fp.one_x", &g_one_x, -300, 300);
    settings_var("fp.one_y", &g_one_y, -300, 300);
    settings_var("fp.one_z", &g_one_z, -300, 300);
    settings_var("fp.one_pitch", &g_one_pitch, -600, 600);
    settings_var("fp.one_yaw", &g_one_yaw, -600, 600);
    settings_var("fp.one_roll", &g_one_roll, -600, 600);
    settings_var("fp.one_fov", &g_one_fov, 150, 900);
    settings_var("fp.otown_x", &g_otown_x, -300, 300);
    settings_var("fp.otown_y", &g_otown_y, -300, 300);
    settings_var("fp.otown_z", &g_otown_z, -300, 300);
    settings_var("fp.otown_pitch", &g_otown_pitch, -600, 600);
    settings_var("fp.otown_yaw", &g_otown_yaw, -600, 600);
    settings_var("fp.otown_roll", &g_otown_roll, -600, 600);
    settings_var("fp.otown_fov", &g_otown_fov, 150, 900);
    settings_var("fp.safe_x", &g_safe_x, -300, 300);
    settings_var("fp.safe_y", &g_safe_y, -300, 300);
    settings_var("fp.safe_z", &g_safe_z, -300, 300);
    settings_var("fp.safe_pitch", &g_safe_pitch, -600, 600);
    settings_var("fp.safe_yaw", &g_safe_yaw, -600, 600);
    settings_var("fp.safe_roll", &g_safe_roll, -600, 600);
    settings_var("fp.sway", &g_sway, 0, 300);
    settings_var("fp.tilt", &g_tilt, 0, 300);
    settings_var("fp.land", &g_land, 0, 300);
    settings_var("fp.recoil", &g_recoil, 0, 300);
    settings_var("fp.flash", &g_flash, 0, 300);
    settings_var("fp.anim_ease", &g_anim_ease, 0, 500);
    settings_var("fp.vm_fill", &g_vm_fill, 0, 100);
    if (memcmp((void *)(image + RVA_SET_VIEW_PARAMS), want, sizeof want) != 0) {
        hg_log("fpview: sRenderContextSetViewParams bytes differ; the view model is stock");
        return 0;
    }
    if (!hook(RVA_SET_VIEW_PARAMS, (void *)fp_view_params_stub, &g_orig,
              "sRenderContextSetViewParams (first-person view model)"))
        return 0;
    {
        static const unsigned char want2[9] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x6c };
        if (memcmp((void *)(image + RVA_GET_MATRICES), want2, sizeof want2) != 0 ||
            !hook(RVA_GET_MATRICES, (void *)fp_matrices_stub, &g_orig_mats,
                  "FUN_00778f55 (the weapon projection, for every consumer)")) {
            hg_log("fpview: FUN_00778f55 not hooked; the view model stays stock");
            return 0;
        }
    }
    hg_log("fpview: view model placement on (weapon FOV %.1f deg, sway %ld%%, tilt %ld%%, recoil %ld%%)",
           g_vm_fov / 10.0, g_sway, g_tilt, g_recoil);
    return 1;
}

void fpview_status(LONG *calls, LONG *frames) { *calls = g_calls; *frames = g_frames_moved; }
