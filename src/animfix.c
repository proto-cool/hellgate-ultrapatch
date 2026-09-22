/*
 * Animation fixes, from what the snap detector (animwatch.c) found.
 *
 * 1. Stance poses cut to zero weight in one frame. In the per-frame
 *    UPDATE_WEIGHTS pass (0x493528) a pose leaving the blend either eases
 *    out, or -- when its record is not flagged for easing -- has its master
 *    weight stored straight to 0 at 0x49359a ("ANIM_SET_WEIGHT TO ZERO",
 *    716 times in one session, the Guardian's shield poses most of all).
 *    That store is hooked: a control that is fully in is eased out with the
 *    game's own easeOut instead, and the original store runs once the fade
 *    has finished, which is the state the game wanted.
 *
 * 2. Loop seams. The run cycles pop once per stride (0.666s, steady): the
 *    last frame of the asset does not meet the first. Fixed by
 *    inertialization on the sampled pose, after hkAnimatedSkeleton's
 *    sampler (0x7f9610) runs: on any frame where a control wraps, jumps,
 *    appears, disappears or changes weight abruptly, the difference
 *    between the new pose and where the old one was heading is captured
 *    per bone and faded out over ~0.2s. Nothing changes between events.
 *
 * 3. Direction switches. Moving between run / runLf / runRt restarts the
 *    incoming cycle at an arbitrary point (0.319 -> 0.000, 0.000 -> 0.536).
 *    A locomotion control that appears or jumps while another is playing
 *    is put at the same phase of the stride instead.
 *
 * 4. Bone spike finder (logging only). The Guardian's shield has one frame
 *    in the walk cycle where it angles backwards -- a bad key in the asset,
 *    not a blend problem, so none of the above can touch it. After each
 *    sample, every bone's local rotation is compared with its previous
 *    frame; a jump far beyond the bone's usual per-frame motion is logged
 *    with the bone's name and every active control's file, local time and
 *    duration. That names the bone and the time window a per-bone
 *    re-sample fix has to cover.
 *
 * The math is Windows-free so test/ui.c can check it.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "panel.h"

/* ------------------------------------------------------------------ */
/* quaternions, (x, y, z, w) as Havok stores them                      */

static void q_mul(const float *a, const float *b, float *o)
{
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}

static void q_conj(const float *a, float *o) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }

static void q_norm(float *q)
{
    float l = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (l < 1e-8f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    q[0] /= l; q[1] /= l; q[2] /= l; q[3] /= l;
}

/* Shortest-arc form: w >= 0, so decaying toward identity takes the short way. */
static void q_canon(float *q)
{
    if (q[3] < 0.0f) { q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; q[3] = -q[3]; }
}

/* Scale a rotation's angle by k (0 = identity, 1 = unchanged). */
void af_q_scale(float *q, float k)
{
    float a, s, ns;
    q_canon(q);
    if (q[3] > 1.0f) q[3] = 1.0f;
    a = acosf(q[3]);                        /* half angle */
    s = sinf(a);
    if (s < 1e-6f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    ns = sinf(a * k) / s;
    q[0] *= ns; q[1] *= ns; q[2] *= ns;
    q[3] = cosf(a * k);
}

/* ------------------------------------------------------------------ */
/* inertialization, per bone                                           */

/*
 * A bone as it is kept: translation xyz then rotation xyzw. The engine's
 * pose is hkQsTransform, 12 floats: translation (4), rotation (4), scale (4).
 */
#define AF_QS_FLOATS 12

/*
 * Capture: where the output was heading (constant velocity from the last
 * two outputs) against the fresh sample. The offset is what, added to the
 * sample, keeps the output on course this frame.
 */
void af_capture(const float *out1, const float *out2, const float *raw, float *off)
{
    float vel[4], pred[4], inv[4];
    int i;

    /* rotation: pred = (out1 * conj(out2)) * out1 */
    q_conj(out2 + 3, inv);
    q_mul(out1 + 3, inv, vel);
    q_canon(vel);
    q_mul(vel, out1 + 3, pred);
    q_norm(pred);
    q_conj(raw + 3, inv);
    q_mul(pred, inv, off + 3);
    q_norm(off + 3);
    q_canon(off + 3);

    /* translation: pred = 2*out1 - out2 */
    for (i = 0; i < 3; i++) off[i] = (2.0f * out1[i] - out2[i]) - raw[i];
}

/* Apply an offset to a sample, producing the output. */
void af_apply(const float *raw, const float *off, float *out)
{
    int i;
    for (i = 0; i < 3; i++) out[i] = raw[i] + off[i];
    q_mul(off + 3, raw + 3, out + 3);
    q_norm(out + 3);
}

/* Fade an offset by k (0..1). */
void af_decay(float *off, float k)
{
    off[0] *= k; off[1] *= k; off[2] *= k;
    af_q_scale(off + 3, k);
}

/* ------------------------------------------------------------------ */
/* phase                                                               */

/* The time in `to` that sits at the same fraction of its cycle as `from`. */
float af_phase_match(float t_from, float dur_from, float dur_to)
{
    float ph;
    if (dur_from <= 1e-4f || dur_to <= 1e-4f) return 0.0f;
    ph = fmodf(t_from, dur_from) / dur_from;
    if (ph < 0.0f) ph += 1.0f;
    return ph * dur_to;
}

/* Locomotion by file name: the cycles a direction change swaps between. */
int af_is_locomotion(const char *file)
{
    static const char *const KEYS[] = { "run", "walk", "sprint", "jog", "strafe", NULL };
    char low[64];
    int i;
    if (!file) return 0;
    for (i = 0; i < 63 && file[i]; i++)
        low[i] = (char)((file[i] >= 'A' && file[i] <= 'Z') ? file[i] + 32 : file[i]);
    low[i] = 0;
    if (strstr(low, "pose") || strstr(low, "jump")) return 0;
    for (i = 0; KEYS[i]; i++) if (strstr(low, KEYS[i])) return 1;
    return 0;
}

#ifdef _WIN32
/* ------------------------------------------------------------------ */
/* the game-facing part                                                */

#include <windows.h>
#include <stdlib.h>
#include "target.h"

/* From animwatch.c: the player's controls, by file, as traced. */
const char *aw_control_file(void *control);

typedef void (__fastcall *sample_fn)(void *skel, void *edx, float *pose,
                                     int nbones, void *cache, int flag);
static sample_fn o_sample;

/*
 * Stance ease and phase match default OFF: the first in-game pass
 * (2026-09-21) reported floaty motion, legs going wrong on sprint and
 * animations stopping. Seam smoothing is ON but now fires on loop wraps only
 * (see pre_sample), which is the Guardian shield's actual defect. Each fix
 * stays hooked (a passthrough when off) so the panel toggles are instant.
 */
static volatile LONG g_on_stance = 0, g_on_seam = 1, g_on_phase = 0;   /* seam = pre-wrap blend (see seam_preblend) */
static volatile LONG g_on_spike = 1;         /* logging only, so on by default */
static volatile LONG g_n_spike;
static volatile LONG g_n_stance, g_n_seam, g_n_phase;
static volatile LONG g_phase_ok = -1;       /* -1 unverified, 0 off, 1 verified */
static LARGE_INTEGER g_qpf;

#define AF_TAU      0.07f                   /* seconds: ~0.2s to fade to 5% */
#define AF_MAXB     160
#define AF_MAXC     8
#define AF_SKELS    64

typedef struct {
    void   *skel;
    int     n;
    double  t;
    int     hist;                            /* output history frames held */
    float  *out1, *out2, *off;               /* 7 floats per bone each      */
    int     active;                          /* offsets are non-zero        */
    int     nctl;
    void   *ctl[AF_MAXC];
    float   ctl_t[AF_MAXC], ctl_w[AF_MAXC];
    float   ctl_tmax[AF_MAXC];               /* highest local time seen: ~ the cycle length */
    int     ctl_wraps[AF_MAXC];              /* wraps seen: tmax is the cycle length once > 0 */
    float  *frame0;                          /* pose at the start of the current cycle, 7/bone */
    void   *frame0_ctl;                      /* the control frame0 belongs to */
    int     frame0_valid;
    float  *sp_q, *sp_ema;                   /* spike finder: last rotation, usual delta */
    int     sp_have;
    double  sp_t;
    const char *wrap_file;                   /* which control wrapped this frame */
} af_skel;

static af_skel g_sk[AF_SKELS];

static double now_s(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_qpf.QuadPart;
}

static af_skel *skel_state(void *skel, int n)
{
    unsigned int h = ((unsigned int)skel >> 4) % AF_SKELS, i;
    af_skel *e = NULL;
    for (i = 0; i < AF_SKELS; i++) {
        af_skel *c = &g_sk[(h + i) % AF_SKELS];
        if (c->skel == skel) { e = c; break; }
        if (!c->skel) { e = c; break; }
    }
    if (!e) e = &g_sk[h];                    /* full: evict the home slot */
    if (e->skel != skel || e->n != n) {
        free(e->out1); free(e->out2); free(e->off); free(e->sp_q); free(e->sp_ema); free(e->frame0);
        memset(e, 0, sizeof *e);
        e->skel = skel;
        e->n = n;
        e->out1 = (float *)calloc((size_t)n * 7, sizeof(float));
        e->out2 = (float *)calloc((size_t)n * 7, sizeof(float));
        e->off  = (float *)calloc((size_t)n * 7, sizeof(float));
        e->sp_q   = (float *)calloc((size_t)n * 4, sizeof(float));
        e->sp_ema = (float *)calloc((size_t)n, sizeof(float));
        e->frame0 = (float *)calloc((size_t)n * 7, sizeof(float));
        if (!e->out1 || !e->out2 || !e->off || !e->sp_q || !e->sp_ema || !e->frame0) { e->skel = NULL; return NULL; }
    }
    return e;
}

static float ctl_time(void *c)   { return *(float *)((char *)c + HKCTL_LOCAL_TIME); }
static float ctl_weight(void *c) { return *(float *)((char *)c + HKCTL_MASTER_WEIGHT); }

/* hkAnimationBinding -> animation -> duration. Checked before it is trusted. */
static float ctl_duration(void *c)
{
    char *bind = *(char **)((char *)c + HKCTL_BINDING), *anim;
    float d;
    if (!bind || IsBadReadPtr(bind, HKBIND_ANIM + 4)) return -1.0f;
    anim = *(char **)(bind + HKBIND_ANIM);
    if (!anim || IsBadReadPtr(anim, HKANIM_DURATION + 4)) return -1.0f;
    d = *(float *)(anim + HKANIM_DURATION);
    return (d == d && d > 0.01f && d < 120.0f) ? d : -1.0f;
}

/*
 * Before sampling: read the controls, decide whether this frame is an
 * event, and phase-match locomotion. Returns 1 on an event.
 */
static int pre_sample(af_skel *st, void *skel, float dt)
{
    char *s = (char *)skel;
    void **list = *(void ***)(s + HKSKEL_CONTROLS);
    int n = *(int *)(s + HKSKEL_NCONTROLS), i, j, event = 0;
    void *ctl[AF_MAXC];
    float t[AF_MAXC], w[AF_MAXC];

    if (n < 0 || n > AF_MAXC || (n && (!list || IsBadReadPtr(list, n * 4)))) return 1;
    for (i = 0; i < n; i++) {
        ctl[i] = list[i];
        if (!ctl[i] || IsBadReadPtr(ctl[i], 0x70)) return 1;
        t[i] = ctl_time(ctl[i]);
        w[i] = ctl_weight(ctl[i]);
    }

    /*
     * The duration offsets are Havok 4.0's layout, assumed. A playback time
     * past its duration means they are wrong here: phase matching turns
     * itself off rather than write nonsense times.
     */
    if (g_phase_ok != 0)
        for (i = 0; i < n; i++) {
            float d = ctl_duration(ctl[i]);
            static int checks;
            if (d <= 0.0f) continue;
            if (t[i] > d + 0.05f) {
                g_phase_ok = 0;
                hg_log("animfix: control %p time %.3f exceeds its duration %.3f -- "
                       "duration offset wrong; phase match OFF", ctl[i], t[i], d);
                break;
            }
            if (g_phase_ok < 0 && ++checks >= 200) {
                g_phase_ok = 1;
                hg_log("animfix: animation durations check out over 200 samples; "
                       "phase match live");
            }
        }

    for (i = 0; i < n; i++) {
        int seen = -1;
        for (j = 0; j < st->nctl; j++) if (st->ctl[j] == ctl[i]) { seen = j; break; }
        if (seen < 0) { event = 1; }
        else {
            float d = t[i] - st->ctl_t[seen];
            int jumped = (d < -0.02f) || (d > dt * 4.0f + 0.05f);
            /*
             * Seam events are loop WRAPS only: the playhead went backwards on
             * a control that was well into its cycle and carries real weight.
             * The first version also fired on retimes, weight changes and
             * controls appearing or leaving -- all of them transitions the
             * game already eases -- and re-capturing offsets there is what
             * made animations "floaty". The shield's backwards frame is the
             * run cycle's seam (HandLf jumps 17 degrees at the wrap, the
             * attachment shows the old frame for one frame), so wraps are
             * exactly the case that needs it.
             */
            /*
             * A true loop wrap: the playhead was at the END of the cycle (within
             * 10% of the highest time this control has reached) and is now at
             * the start. The first version accepted any backwards jump past
             * 0.3 s, which also caught a run cycle *restarting* mid-stride
             * (46-59 degree "seams" on the legs in the log) and applied that
             * whole difference as an offset -- the folded body.
             */
            if (d < -0.02f && w[i] >= 0.5f && st->ctl_tmax[seen] > 0.3f
                && st->ctl_t[seen] >= 0.9f * st->ctl_tmax[seen] && t[i] < 0.1f * st->ctl_tmax[seen]) {
                event = 1;
                st->wrap_file = aw_control_file(ctl[i]);
                st->ctl_wraps[seen]++;
            }

            /* Phase: a locomotion control that jumped mid-cycle (not a wrap). */
            if (jumped && g_on_phase && g_phase_ok == 1) {
                const char *f = aw_control_file(ctl[i]);
                float di = ctl_duration(ctl[i]);
                if (f && af_is_locomotion(f) && di > 0.0f &&
                    !(st->ctl_t[seen] > di * 0.8f && t[i] < di * 0.2f)) {   /* not a wrap */
                    int k;
                    for (k = 0; k < n; k++) {
                        const char *g;
                        float dk;
                        if (k == i || w[k] < 0.05f) continue;
                        g = aw_control_file(ctl[k]);
                        dk = ctl_duration(ctl[k]);
                        if (!g || !af_is_locomotion(g) || dk <= 0.0f) continue;
                        if (dk < di * 0.7f || dk > di * 1.43f) continue;
                        {
                            float nt = af_phase_match(t[k], dk, di);
                            *(float *)((char *)ctl[i] + HKCTL_LOCAL_TIME) = nt;
                            InterlockedIncrement(&g_n_phase);
                            if (g_n_phase <= 5)
                                hg_log("animfix: phase %s %.3f -> %.3f to match %s at %.3f/%.3f",
                                       f, t[i], nt, g, t[k], dk);
                            t[i] = nt;
                        }
                        break;
                    }
                }
            }
        }
    }
    {
        float tmax[AF_MAXC];
        int wraps[AF_MAXC];
        for (i = 0; i < n; i++) {
            int seen = -1;
            for (j = 0; j < st->nctl; j++) if (st->ctl[j] == ctl[i]) { seen = j; break; }
            tmax[i] = seen >= 0 ? st->ctl_tmax[seen] : 0.0f;
            wraps[i] = seen >= 0 ? st->ctl_wraps[seen] : 0;
            if (t[i] > tmax[i]) tmax[i] = t[i];
        }
        st->nctl = n;
        for (i = 0; i < n; i++) {
            st->ctl[i] = ctl[i]; st->ctl_t[i] = t[i]; st->ctl_w[i] = w[i];
            st->ctl_tmax[i] = tmax[i]; st->ctl_wraps[i] = wraps[i];
        }
    }
    return event;
}

static void to7(const float *qs, float *b7)
{
    b7[0] = qs[0]; b7[1] = qs[1]; b7[2] = qs[2];
    b7[3] = qs[4]; b7[4] = qs[5]; b7[5] = qs[6]; b7[6] = qs[7];
}

static void from7(const float *b7, float *qs)
{
    qs[0] = b7[0]; qs[1] = b7[1]; qs[2] = b7[2];
    qs[4] = b7[3]; qs[5] = b7[4]; qs[6] = b7[5]; qs[7] = b7[6];
}


/* ---- 4. bone spike finder --------------------------------------------- */

#define AF_SPIKE_DEG    20.0f
#define AF_SPIKE_RATIO  4.0f
#define AF_SPIKE_BUDGET 40       /* log lines per 5 s, so a bad asset cannot flood */

/*
 * hkSkeleton in Havok 4.0 is a plain struct: m_name, m_parentIndices,
 * m_numParentIndices, m_bones (hkBone **), m_numBones (+0x10, which is what
 * target.h recorded), ... hkBone is { char *m_name; hkBool m_lockTranslation }.
 * Every pointer is checked and the bone count must match the sampled pose;
 * anything else falls back to the bone index.
 */
static const char *bone_name(void *skel, int i, int nbones)
{
    char *sk, **bones, *b, *nm;
    int k;
    if (IsBadReadPtr((char *)skel + 0x18, 4)) return NULL;
    sk = *(char **)((char *)skel + 0x18);
    if (!sk || IsBadReadPtr(sk, 0x14) || *(int *)(sk + 0x10) != nbones) return NULL;
    bones = *(char ***)(sk + 0x0C);
    if (!bones || IsBadReadPtr(bones, (size_t)nbones * sizeof(void *))) return NULL;
    b = bones[i];
    if (!b || IsBadReadPtr(b, 8)) return NULL;
    nm = *(char **)b;
    if (!nm || IsBadReadPtr(nm, 1)) return NULL;
    for (k = 0; k < 48; k++) {
        if (IsBadReadPtr(nm + k, 1)) return NULL;
        if (!nm[k]) break;
        if (nm[k] < 32 || nm[k] > 126) return NULL;
    }
    return (k > 0 && k < 48) ? nm : NULL;
}

static int spike_budget(void)
{
    static double win;
    static int used;
    double t = now_s();
    if (t - win > 5.0) { win = t; used = 0; }
    return used++ < AF_SPIKE_BUDGET;
}

/*
 * Targeted trace for the shield: the male 3p appearance attaches the left
 * weapon to "Bip01 prop2" (fallback "ForearmLfC"), so while the run cycle
 * plays on the player's skeleton, log those bones' local rotations plus
 * HandLf every frame against the run's local time. 400 samples (~6 s) at
 * start-up, re-armed by bin\hellgate_animtrace.on (deleted when consumed).
 * Plotting angle against local time finds the bad frame without a threshold.
 */
#define AF_TRACE_SAMPLES 400
static volatile LONG g_trace_left = AF_TRACE_SAMPLES;
static const char *const g_trace_names[3] = { "Bip01 prop2", "ForearmLfC", "HandLf" };

void animfix_rearm_trace(void) { InterlockedExchange(&g_trace_left, AF_TRACE_SAMPLES); }

static void trace_bones(af_skel *st, void *skel, const float *pose, int nbones)
{
    void **list = *(void ***)((char *)skel + HKSKEL_CONTROLS);
    int n = *(int *)((char *)skel + HKSKEL_NCONTROLS), j, k, idx[3];
    const char *run = NULL;
    float t = 0.0f, w = 0.0f;
    char buf[400];
    int len;
    (void)st;
    if (!list || n <= 0 || n > 32 || IsBadReadPtr(list, (size_t)n * sizeof(void *))) return;
    for (j = 0; j < n; j++) {
        const char *f;
        if (!list[j] || IsBadReadPtr(list[j], 0x70)) continue;
        f = aw_control_file(list[j]);
        if (f && (strstr(f, "_run") || strstr(f, "_walk")) && !strstr(f, "Torso")
            && ctl_weight(list[j]) > 0.5f) {
            run = f; t = ctl_time(list[j]); w = ctl_weight(list[j]);
            break;
        }
    }
    if (!run) return;
    for (k = 0; k < 3; k++) {
        idx[k] = -1;
        for (j = 0; j < nbones; j++) {
            const char *nm = bone_name(skel, j, nbones);
            if (nm && !strcmp(nm, g_trace_names[k])) { idx[k] = j; break; }
        }
    }
    if (InterlockedDecrement(&g_trace_left) < 0) { g_trace_left = -1; return; }
    len = snprintf(buf, sizeof buf, "animfix: trace %s@%.3f w%.2f", run, t, w);
    for (k = 0; k < 3; k++) {
        const float *q;
        if (idx[k] < 0) { len += snprintf(buf + len, sizeof buf - len, " %s=?", g_trace_names[k]); continue; }
        q = pose + idx[k] * AF_QS_FLOATS;
        len += snprintf(buf + len, sizeof buf - len, " %s=[%d](%.3f %.3f %.3f | %.4f %.4f %.4f %.4f)",
                        g_trace_names[k], idx[k], q[0], q[1], q[2], q[4], q[5], q[6], q[7]);
    }
    hg_log("%s", buf);
}

static void spike_scan(af_skel *st, void *skel, const float *pose, int nbones)
{
    double t = now_s();
    int i, player = -1;
    if (st->sp_t > 0.0 && t - st->sp_t < 0.0015) return;   /* same frame, sampled again */
    st->sp_t = t;
    if (!st->sp_have) {
        const char *root = bone_name(skel, 0, nbones);
        for (i = 0; i < nbones; i++) memcpy(st->sp_q + i * 4, pose + i * AF_QS_FLOATS + 4, 16);
        st->sp_have = 1;
        if (spike_budget())
            hg_log("animfix: skeleton %p nbones=%d root=%s", skel, nbones, root ? root : "?");
        return;
    }
    for (i = 0; i < nbones; i++) {
        const float *q = pose + i * AF_QS_FLOATS + 4;
        float *pq = st->sp_q + i * 4, d, ang;
        d = fabsf(q[0]*pq[0] + q[1]*pq[1] + q[2]*pq[2] + q[3]*pq[3]);
        if (d > 1.0f) d = 1.0f;
        ang = 2.0f * acosf(d) * 57.29578f;
        if (ang > AF_SPIKE_DEG && ang > AF_SPIKE_RATIO * st->sp_ema[i] + 5.0f) {
            if (player < 0) {
                /* Only the player's skeleton: a control animwatch has a file for. */
                void **list = *(void ***)((char *)skel + HKSKEL_CONTROLS);
                int n = *(int *)((char *)skel + HKSKEL_NCONTROLS), j;
                player = 0;
                if (list && n > 0 && n <= 32 && !IsBadReadPtr(list, (size_t)n * sizeof(void *)))
                    for (j = 0; j < n; j++)
                        if (list[j] && !IsBadReadPtr(list[j], 0x70) && aw_control_file(list[j])) { player = 1; break; }
            }
            /*
             * Non-player skeletons are logged too, above 40 degrees: the shield
             * is a separate model and may well have its own hkAnimatedSkeleton,
             * whose controls animwatch never traced. Those lines carry the
             * skeleton pointer and bone count so they can be told apart.
             */
            if ((player || ang > 40.0f) && spike_budget()) {
                char buf[640];
                const char *nm = bone_name(skel, i, nbones);
                void **list = *(void ***)((char *)skel + HKSKEL_CONTROLS);
                int n = *(int *)((char *)skel + HKSKEL_NCONTROLS), j, len;
                InterlockedIncrement(&g_n_spike);
                len = snprintf(buf, sizeof buf, "animfix: spike%s bone %d %s %.0f deg (usual %.1f) |",
                               player ? "" : " (other)", i, nm ? nm : "?", ang, st->sp_ema[i]);
                if (!player)
                    len += snprintf(buf + len, sizeof buf - len, " skel %p n=%d root=%s |",
                                    skel, nbones, bone_name(skel, 0, nbones) ? bone_name(skel, 0, nbones) : "?");
                if (!list || n <= 0 || n > 32 || IsBadReadPtr(list, (size_t)n * sizeof(void *))) n = 0;
                for (j = 0; j < n && j < 32 && len < (int)sizeof buf - 80; j++) {
                    const char *f;
                    float w;
                    if (!list[j] || IsBadReadPtr(list[j], 0x70)) continue;
                    w = ctl_weight(list[j]);
                    if (w <= 0.001f) continue;
                    f = aw_control_file(list[j]);
                    len += snprintf(buf + len, sizeof buf - len, " %s@%.3f/%.3f w%.2f",
                                    f ? f : "?", ctl_time(list[j]), ctl_duration(list[j]), w);
                }
                hg_log("%s", buf);
            }
        } else {
            st->sp_ema[i] += (ang - st->sp_ema[i]) * 0.1f;
        }
        memcpy(pq, q, 16);
    }
}

/*
 * Seam pre-blend. Inertialization (below, kept as g_seam_inertial = 0)
 * spreads the *correction* over the frames after the wrap, which made the
 * shield drift for 0.2 s -- more visible than the one-frame snap it hid
 * (user, 2026-09-21). This removes the discontinuity instead: the pose at
 * the start of each cycle of the dominant looping control is remembered,
 * and over the last 20% of the cycle the sampled pose is blended toward it,
 * so the wrap lands exactly on the frame it started from. The blend happens
 * while the limbs are moving anyway; nothing is left to correct afterwards.
 */
static volatile LONG g_seam_inertial = 0;
#define AF_PRE_START 0.80f

static void seam_preblend(af_skel *st, float *pose, int nbones)
{
    int j, best = -1, i;
    float bw = 0.5f, t, tmax, phase, w;
    for (j = 0; j < st->nctl; j++)
        if (st->ctl_w[j] >= bw && st->ctl_wraps[j] > 0 && st->ctl_tmax[j] > 0.3f) { bw = st->ctl_w[j]; best = j; }
    if (best < 0) { st->frame0_valid = 0; return; }
    t = st->ctl_t[best]; tmax = st->ctl_tmax[best];
    phase = t / tmax;
    if (st->frame0_ctl != st->ctl[best]) { st->frame0_ctl = st->ctl[best]; st->frame0_valid = 0; }
    if (phase < 0.05f) {
        for (i = 0; i < nbones; i++) to7(pose + i * AF_QS_FLOATS, st->frame0 + i * 7);
        st->frame0_valid = 1;
        return;
    }
    if (!st->frame0_valid || phase < AF_PRE_START) return;
    w = (phase - AF_PRE_START) / (1.0f - AF_PRE_START);
    if (w > 1.0f) w = 1.0f;
    w = w * w * (3.0f - 2.0f * w);                       /* smoothstep */
    for (i = 0; i < nbones; i++) {
        float *qs = pose + i * AF_QS_FLOATS, *f0 = st->frame0 + i * 7, q[4], d, l;
        int k;
        for (k = 0; k < 3; k++) qs[k] += (f0[k] - qs[k]) * w;
        d = qs[4]*f0[3] + qs[5]*f0[4] + qs[6]*f0[5] + qs[7]*f0[6];
        for (k = 0; k < 4; k++) q[k] = d < 0.0f ? -f0[3 + k] : f0[3 + k];   /* same hemisphere */
        for (k = 0; k < 4; k++) qs[4 + k] += (q[k] - qs[4 + k]) * w;
        l = sqrtf(qs[4]*qs[4] + qs[5]*qs[5] + qs[6]*qs[6] + qs[7]*qs[7]);
        if (l > 1e-6f) for (k = 0; k < 4; k++) qs[4 + k] /= l;
    }
    InterlockedIncrement(&g_n_seam);
}

static void __fastcall d_sample(void *skel, void *edx, float *pose, int nbones,
                                void *cache, int flag)
{
    af_skel *st = NULL;
    double t = now_s();
    float dt = 0.0f;
    int event = 0, i;

    if (g_on_seam || g_on_phase || g_on_spike) {
        if (skel && pose && nbones > 0 && nbones <= AF_MAXB) {
            st = skel_state(skel, nbones);
            if (st) {
                dt = st->t > 0.0 ? (float)(t - st->t) : 0.0f;
                if (dt > 0.0015f && (g_on_seam || g_on_phase)) event = pre_sample(st, skel, dt);
            }
        }
    }

    o_sample(skel, edx, pose, nbones, cache, flag);

    if (st && g_on_seam && dt > 0.0015f) seam_preblend(st, pose, nbones);
    if (st && g_on_spike) {
        spike_scan(st, skel, pose, nbones);
        if (g_trace_left > 0) trace_bones(st, skel, pose, nbones);
    }
    if (!st || !g_on_seam || !g_seam_inertial) {
        /* The per-skeleton clock used to be advanced only by the
         * inertialization block below; with that off, dt stayed 0 and
         * neither pre_sample (wraps, cycle lengths) nor the pre-wrap blend
         * ever ran ("seams 0" on the panel). Advance it here. */
        if (st && (dt > 0.0015f || st->t <= 0.0)) st->t = t;
        return;
    }
    if (dt <= 0.0015f && st->hist) {
        /* Same frame, sampled again: apply what is held, change nothing. */
        if (st->active)
            for (i = 0; i < nbones; i++) {
                float raw[7], out[7];
                to7(pose + i * AF_QS_FLOATS, raw);
                af_apply(raw, st->off + i * 7, out);
                from7(out, pose + i * AF_QS_FLOATS);
            }
        return;
    }
    if (dt > 0.25f) { st->hist = 0; st->active = 0; }   /* hitch or reappearance: start over */

    {
        float k = st->active ? expf(-dt / AF_TAU) : 0.0f;
        int capture = event && st->hist >= 2;
        if (capture) InterlockedIncrement(&g_n_seam);
        for (i = 0; i < nbones; i++) {
            float raw[7], out[7], *off = st->off + i * 7;
            to7(pose + i * AF_QS_FLOATS, raw);
            if (st->active) af_decay(off, k);
            else { memset(off, 0, 7 * sizeof(float)); off[6] = 1.0f; }
            if (capture) {
                /* Where the output was heading, including what was already held. */
                float fresh[7];
                af_capture(st->out1 + i * 7, st->out2 + i * 7, raw, fresh);
                memcpy(off, fresh, sizeof fresh);
            }
            af_apply(raw, off, out);
            from7(out, pose + i * AF_QS_FLOATS);
            memcpy(st->out2 + i * 7, st->out1 + i * 7, 7 * sizeof(float));
            memcpy(st->out1 + i * 7, out, 7 * sizeof(float));
        }
        if (capture) {
            /* Report the seam per bone for the first few wraps: this is the
             * measurement of the asset's loop discontinuity. */
            static LONG reports;
            st->active = 1;
            /* Player's skeleton only (NPC idles wrap too and ate the first
             * budget); always name the largest seam even when it is small. */
            if (st->wrap_file && InterlockedIncrement(&reports) <= 12) {
                char buf[500];
                int len = snprintf(buf, sizeof buf, "animfix: wrap seam %s:", st->wrap_file);
                float best = 0.0f; int besti = -1;
                for (i = 0; i < nbones && len < (int)sizeof buf - 40; i++) {
                    float *q = st->off + i * 7 + 3, d = fabsf(q[3]);
                    float deg = 2.0f * acosf(d > 1.0f ? 1.0f : d) * 57.29578f;
                    if (deg > best) { best = deg; besti = i; }
                    if (deg > 3.0f) {
                        const char *nm = bone_name(skel, i, nbones);
                        len += snprintf(buf + len, sizeof buf - len, " %s=%.0f", nm ? nm : "?", deg);
                    }
                }
                if (besti >= 0) {
                    const char *nm = bone_name(skel, besti, nbones);
                    len += snprintf(buf + len, sizeof buf - len, " | max %s=%.1f deg, smoothing over %.2fs",
                                    nm ? nm : "?", best, 3.0f * AF_TAU);
                }
                hg_log("%s", buf);
            }
        } else if (st->active && k < 0.02f) st->active = 0;
        if (st->hist < 2) st->hist++;
        st->t = t;
    }
}

/* ---- 1. the stance weight cut ------------------------------------------ */

/*
 * At 0x49359a, eax is the control and the instruction is
 *     movss [eax+0x2c], xmm1          ; xmm1 = 0, master weight := 0
 * The stub asks C what to do: 0 = run the original store, otherwise the
 * duration to ease out over, in which case the store is skipped. xmm1 must
 * be zero again on the way out; the code after relies on it, and the game's
 * easeOut clobbers xmm0-2.
 */
void *g_af_tramp_stance, *g_af_after_stance;
void af_stub_stance(void);

unsigned int __cdecl af_on_stance(unsigned char *ctl)
{
    float dur = 0.15f;
    unsigned int bits;
    if (!g_on_stance || !ctl || IsBadReadPtr(ctl, 0x70)) return 0;
    if (ctl[HKCTL_EASE_STATUS]) {           /* in, or easing in: fade it out */
        InterlockedIncrement(&g_n_stance);
        memcpy(&bits, &dur, 4);
        return bits;
    }
    if (*(float *)(ctl + HKCTL_EASE_T) > 0.001f) return 1;   /* already fading: leave it */
    return 0;                                /* faded out: the game's zero is right */
}

__asm__(
    ".text\n\t"
    ".globl _af_stub_stance\n"
    "_af_stub_stance:\n\t"
    "pushal\n\t"
    "pushfl\n\t"
    "pushl %eax\n\t"
    "call _af_on_stance\n\t"
    "addl $4, %esp\n\t"
    "testl %eax, %eax\n\t"
    "jz 1f\n\t"
    "cmpl $1, %eax\n\t"
    "je 2f\n\t"
    "pushl %eax\n\t"                        /* duration, as easeOut's stack argument */
    "movl 36(%esp), %eax\n\t"               /* the saved eax: the control */
    "movl $0x49cbba, %ecx\n\t"
    "call *%ecx\n\t"                        /* easeOut, ret 4 pops the duration */
    "2:\n\t"
    "popfl\n\t"
    "popal\n\t"
    "xorps %xmm1, %xmm1\n\t"
    "jmp *_g_af_after_stance\n\t"
    "1:\n\t"
    "popfl\n\t"
    "popal\n\t"
    "jmp *_g_af_tramp_stance\n\t");

/* ---- install ---------------------------------------------------------- */

int animfix_install(unsigned int image,
                    int (*hook)(unsigned int, void *, void **, const char *))
{
    static const unsigned char stance_sig[5] = { 0xf3, 0x0f, 0x11, 0x48, 0x2c };
    static const unsigned char sample_sig[8] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x6a, 0xff };
    int n = 0;

    QueryPerformanceFrequency(&g_qpf);
    g_af_after_stance = (void *)(image + RVA_STANCE_ZERO_STORE + 5);
    if (!memcmp((void *)(image + RVA_STANCE_ZERO_STORE), stance_sig, 5))
        n += hook(RVA_STANCE_ZERO_STORE, (void *)af_stub_stance, &g_af_tramp_stance,
                  "stance weight cut (ease it instead)");
    else
        hg_log("animfix: stance store bytes differ; that fix is off");
    if (!memcmp((void *)(image + RVA_HK_SAMPLE), sample_sig, 8))
        n += hook(RVA_HK_SAMPLE, (void *)d_sample, (void **)&o_sample,
                  "hkAnimatedSkeleton sample (seams, phase)");
    else
        hg_log("animfix: sampler bytes differ; seam and phase fixes are off");
    hg_log("animfix: %d of 2 hooks in; stance ease %s, seam smoothing %s, phase match %s, "
           "spike log %s", n, g_on_stance ? "on" : "off", g_on_seam ? "on" : "off",
           g_on_phase ? "on" : "off", g_on_spike ? "on" : "off");
    return n;
}

/* ---- panel ------------------------------------------------------------- */

extern volatile LONG g_aw_minout_n, g_aw_minout_on;

void hg_animfix_status(hg_animfix_state *o)
{
    o->installed  = o_sample != NULL || g_af_tramp_stance != NULL;
    o->stance_on  = (int)g_on_stance;
    o->seam_on    = (int)g_on_seam;
    o->phase_on   = (int)g_on_phase;
    o->minout_on  = (int)g_aw_minout_on;
    o->spike_on   = (int)g_on_spike;
    o->n_spike    = g_n_spike;
    o->n_stance   = g_n_stance;
    o->n_seam     = g_n_seam;
    o->n_phase    = g_n_phase;
    o->n_minout   = g_aw_minout_n;
}

void hg_animfix_set(int which, int on)
{
    volatile LONG *p = which == 0 ? &g_on_stance : which == 1 ? &g_on_seam
                     : which == 2 ? &g_on_phase : which == 3 ? &g_aw_minout_on : &g_on_spike;
    InterlockedExchange(p, on ? 1 : 0);
    hg_log("animfix: %s %s", which == 0 ? "stance ease" : which == 1 ? "seam smoothing"
                            : which == 2 ? "phase match" : which == 3 ? "ease-out minimum"
                            : "spike log",
           on ? "on" : "off");
}
#endif
