/*
 * Feet on the ground: two-bone IK on your character's legs (ik.feet, on),
 * each foot turned to the ground's slope (ik.feet_tilt).
 *
 * Where: right after hkaAnimatedSkeleton samples its animations into the
 * local pose (0x7f9610, thiscall (pose, nbones, cache, flag), ret 0x10;
 * pose is nbones hkQsTransform: translation, rotation xyzw, scale, 48
 * bytes each). The engine builds the model-space pose and the skinning
 * matrices from that buffer afterwards, so a corrected pose carries into
 * every pass, shadows and attachments included. Only your character's
 * skeleton: the engine's animation record for your third-person model
 * (FUN_0049c129, model id in ecx) holds its hkaAnimatedSkeleton at +0x2c.
 *
 * What: each foot's world position (the model's world matrix, its record's
 * +0x130, times the model-space pose) is cast down against the level (the
 * game's ray helper, hook.c hg_ray_down). The animation stands the feet on
 * a flat floor at the model's origin; a foot over lower ground (a step
 * down, a slope) wants to go down by the difference, one over higher
 * ground up. The body (the bone above both thighs and the spine) drops
 * by the larger of the two downward offsets,
 * so the lower foot can reach, and each leg is bent to its target by a
 * two-bone solve (thigh and calf; the foot keeps its angle to the calf).
 * Offsets are eased, capped at 0.45 units, and fade out when the ground
 * under a foot is not found or is far from the floor (a jump, a ledge).
 * The slope under each foot comes from two more rays a step along world x
 * and y (skipped across a step's edge); a foot within 0.15 of its ground
 * turns to it, 30 degrees at most, fading out as it lifts.
 *
 * Havok's own foot IK (hkbFootIkModifier) is in the exe as class metadata
 * only; the solve here is ours (2026-09-26 spike).
 */
#include <windows.h>
#include <math.h>
#include <string.h>
#include "panel.h"

void hg_log(const char *fmt, ...);
void settings_var(const char *key, volatile LONG *var, LONG lo, LONG hi);
void *hg_player_anim_skeleton(float world[16]);
int hg_ray_down(const float from[3], float length, float *hit_z);

#define RVA_HK_SAMPLE 0x003F9610u
#define QS 12                                   /* floats per bone */
#define MAXB 160

static volatile LONG g_on = 1;                  /* ik.feet (on: the user, 2026-09-26) */
static volatile LONG g_strength = 100;          /* ik.feet_strength, percent */
static volatile LONG g_tilt = 1;                /* ik.feet_tilt: feet turned to the ground's slope */
static int g_mirror;                            /* the world matrix flips handedness: turn the other way */
#define TILT_STEP 0.15f                         /* the slope rays' spacing, world units */
#define TILT_MAX 0.52f                          /* 30 degrees at most */

typedef void (__fastcall *sample_fn)(void *skel, void *edx, float *pose, int nbones, void *cache, int flag);
static sample_fn o_sample;
static LARGE_INTEGER g_qpf;

/* ------------------------------------------------------------------ */
/* quaternions (x y z w) and vectors                                   */

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
    if (l < 1e-8f) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    q[0] /= l; q[1] /= l; q[2] /= l; q[3] /= l;
}
static void q_rot(const float *q, const float *v, float *o)      /* o = q v q* */
{
    float t[3], c[3];
    t[0] = 2 * (q[1]*v[2] - q[2]*v[1]);
    t[1] = 2 * (q[2]*v[0] - q[0]*v[2]);
    t[2] = 2 * (q[0]*v[1] - q[1]*v[0]);
    c[0] = q[1]*t[2] - q[2]*t[1];
    c[1] = q[2]*t[0] - q[0]*t[2];
    c[2] = q[0]*t[1] - q[1]*t[0];
    o[0] = v[0] + q[3]*t[0] + c[0];
    o[1] = v[1] + q[3]*t[1] + c[1];
    o[2] = v[2] + q[3]*t[2] + c[2];
}
static void q_axis(const float *axis, float ang, float *q)
{
    float s = sinf(ang * 0.5f);
    q[0] = axis[0]*s; q[1] = axis[1]*s; q[2] = axis[2]*s; q[3] = cosf(ang * 0.5f);
}
static float dot3(const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void cross3(const float *a, const float *b, float *o)
{
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
static float len3(const float *a) { return sqrtf(dot3(a, a)); }
static void sub3(const float *a, const float *b, float *o) { o[0] = a[0]-b[0]; o[1] = a[1]-b[1]; o[2] = a[2]-b[2]; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the rotation taking direction a to direction b */
static void q_between(const float *a, const float *b, float *q)
{
    float c[3], d = dot3(a, b) / (len3(a) * len3(b) + 1e-12f);
    cross3(a, b, c);
    q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
    q[3] = len3(a) * len3(b) + dot3(a, b);
    if (d < -0.9999f) { q[0] = 1; q[1] = q[2] = q[3] = 0; return; }      /* opposite: any axis */
    q_norm(q);
}

/* ------------------------------------------------------------------ */
/* the skeleton                                                        */

/*
 * hkaSkeleton (Havok 4.0), at the animated skeleton's +0x18: m_name,
 * m_parentIndices (hkInt16 *), m_numParentIndices, m_bones (hkaBone **),
 * m_numBones (+0x10). hkaBone is { char *m_name; hkBool m_lockTranslation }.
 */
static struct {
    void *skel;
    int n, ok;
    short parent[MAXB];
    int thigh[2], calf[2], foot[2], pelvis;     /* [0] left, [1] right */
    float off[2], drop;                         /* eased offsets, world units */
    float nrm[2][3];                            /* eased ground normal under each foot, world */
    LARGE_INTEGER t;
} g_sk;

static const char *bone_name(char *sk, int i)
{
    char **bones = *(char ***)(sk + 0x0c), *b, *nm;
    if (!bones || IsBadReadPtr(bones + i, 4)) return NULL;
    b = bones[i];
    if (!b || IsBadReadPtr(b, 4)) return NULL;
    nm = *(char **)b;
    if (!nm || IsBadReadPtr(nm, 8)) return NULL;
    return nm;
}

static int find_bone(char *sk, int n, const char *a, const char *side)
{
    int i;
    for (i = 0; i < n; i++) {
        const char *nm = bone_name(sk, i);
        if (nm && strstr(nm, a) && strstr(nm, side)) return i;
    }
    return -1;
}

static int is_ancestor(int a, int b)            /* a above b */
{
    while (b >= 0) { if (b == a) return 1; b = g_sk.parent[b]; }
    return 0;
}

/* learn the skeleton once: parents, the leg chains, the pelvis */
static int learn(void *skel, int nbones)
{
    char *sk;
    short *par;
    int i, s;
    static const char *const SIDE[2] = { "Lf", "Rt" };
    memset(&g_sk, 0, sizeof g_sk);
    g_sk.skel = skel;
    g_sk.n = nbones;
    if (IsBadReadPtr((char *)skel + 0x18, 4)) return 0;
    sk = *(char **)((char *)skel + 0x18);
    if (!sk || IsBadReadPtr(sk, 0x14) || *(int *)(sk + 0x10) != nbones || nbones > MAXB) return 0;
    par = *(short **)(sk + 0x04);
    if (!par || IsBadReadPtr(par, nbones * 2)) return 0;
    for (i = 0; i < nbones; i++) g_sk.parent[i] = par[i];
    for (s = 0; s < 2; s++) {
        g_sk.thigh[s] = find_bone(sk, nbones, "Thigh", SIDE[s]);
        g_sk.calf[s] = find_bone(sk, nbones, "Calf", SIDE[s]);
        if (g_sk.calf[s] < 0) g_sk.calf[s] = find_bone(sk, nbones, "Shin", SIDE[s]);
        g_sk.foot[s] = find_bone(sk, nbones, "Foot", SIDE[s]);
    }
    {   /* every bone's name, once, for the record */
        char line[600];
        int len = 0;
        for (i = 0; i < nbones; i++) {
            const char *nm = bone_name(sk, i);
            len += wsprintfA(line + len, " %d:%s<%d", i, nm ? nm : "?", g_sk.parent[i]);
            if (len > 480 || i == nbones - 1) { hg_log("footik: bones%s", line); len = 0; }
        }
    }
    for (s = 0; s < 2; s++)
        if (g_sk.thigh[s] < 0 || g_sk.calf[s] < 0 || g_sk.foot[s] < 0 ||
            !is_ancestor(g_sk.thigh[s], g_sk.calf[s]) || !is_ancestor(g_sk.calf[s], g_sk.foot[s])) {
            hg_log("footik: no leg chain on side %s (thigh %d calf %d foot %d); off", SIDE[s], g_sk.thigh[s],
                   g_sk.calf[s], g_sk.foot[s]);
            return 0;
        }
    /* what drops: the lowest bone above both thighs and the head, so the
     * whole body goes down together. The lowest above the thighs alone is
     * Pelvis here, under Lumbar, while the spine hangs from the root
     * (CATRigTemplarHub): dropping Pelvis stretched the torso (2026-09-26) */
    {
        int head = find_bone(sk, nbones, "Head", "");
        if (head < 0) head = find_bone(sk, nbones, "Neck", "");
        if (head < 0) head = find_bone(sk, nbones, "spine", "");
        g_sk.pelvis = g_sk.parent[g_sk.thigh[0]];
        while (g_sk.pelvis >= 0 && !(is_ancestor(g_sk.pelvis, g_sk.thigh[1]) && (head < 0 || is_ancestor(g_sk.pelvis, head))))
            g_sk.pelvis = g_sk.parent[g_sk.pelvis];
    }
    hg_log("footik: legs: thigh %d/%d calf %d/%d foot %d/%d, pelvis %d (%d bones)", g_sk.thigh[0], g_sk.thigh[1],
           g_sk.calf[0], g_sk.calf[1], g_sk.foot[0], g_sk.foot[1], g_sk.pelvis, nbones);
    return g_sk.pelvis >= 0;
}

/* model-space transforms of bone i (translation t, rotation q), from the pose */
static void model_of(const float *pose, int i, float *t, float *q)
{
    int chain[MAXB], n = 0, k;
    while (i >= 0 && n < MAXB) { chain[n++] = i; i = g_sk.parent[i]; }
    t[0] = t[1] = t[2] = 0; q[0] = q[1] = q[2] = 0; q[3] = 1;
    for (k = n - 1; k >= 0; k--) {
        const float *b = pose + chain[k] * QS;
        float r[3], nq[4];
        q_rot(q, b, r);
        t[0] += r[0]; t[1] += r[1]; t[2] += r[2];
        q_mul(q, b + 4, nq);
        q_norm(nq);
        memcpy(q, nq, 16);
    }
}

/* model space to world (row vectors: p W) */
static void to_world(const float *W, const float *p, float *o)
{
    int c;
    for (c = 0; c < 3; c++) o[c] = p[0]*W[c] + p[1]*W[4 + c] + p[2]*W[8 + c] + W[12 + c];
}

/* a world direction in model space (W's rotation and uniform scale undone) */
static void dir_to_model(const float *W, const float *d, float *o)
{
    float s2 = W[0]*W[0] + W[1]*W[1] + W[2]*W[2];
    int r;
    for (r = 0; r < 3; r++) o[r] = (W[r*4]*d[0] + W[r*4 + 1]*d[1] + W[r*4 + 2]*d[2]) / (s2 > 1e-12f ? s2 : 1);
}

/* bend one leg so its foot lands at target (model space) */
static void solve_leg(float *pose, int s, const float *target)
{
    int A = g_sk.thigh[s], B = g_sk.calf[s], C = g_sk.foot[s];
    float ta[3], qa[4], tb[3], qb[4], tc[3], qc[4], ba[3], bc[3], at[3], ac[3], n[3], rk[4], rh[4];
    float a, b, d, th0, th1, cosv, pa_t[3], pa_q[4], inv[4], nq[4], t1[4];
    model_of(pose, A, ta, qa);
    model_of(pose, B, tb, qb);
    model_of(pose, C, tc, qc);
    sub3(ta, tb, ba); sub3(tc, tb, bc); sub3(target, ta, at);
    a = len3(ba); b = len3(bc);
    if (a < 1e-4f || b < 1e-4f) return;
    d = clampf(len3(at), fabsf(a - b) + 1e-3f, (a + b) * 0.999f);
    /* the knee: the angle at B from the law of cosines */
    cosv = clampf(dot3(ba, bc) / (a * b), -1, 1); th0 = acosf(cosv);
    th1 = acosf(clampf((a*a + b*b - d*d) / (2*a*b), -1, 1));
    cross3(bc, ba, n);
    if (len3(n) < 1e-6f) return;                /* a straight leg: no bend plane */
    n[0] /= len3(n); n[1] /= len3(n); n[2] /= len3(n);
    q_axis(n, th0 - th1, rk);                   /* turns BC toward BA by th0 - th1 */
    {   /* the foot after the knee turn, then the hip turn onto the target */
        float c2[3], r[3];
        q_rot(rk, bc, r);
        c2[0] = tb[0] + r[0]; c2[1] = tb[1] + r[1]; c2[2] = tb[2] + r[2];
        sub3(c2, ta, ac);
        q_between(ac, at, rh);
    }
    /* back to local rotations: thigh = parent* (rh qa), calf = qa* rk qb */
    model_of(pose, g_sk.parent[A], pa_t, pa_q);
    q_mul(rh, qa, nq); q_norm(nq);
    q_conj(pa_q, inv);
    q_mul(inv, nq, t1); q_norm(t1);
    memcpy(pose + A*QS + 4, t1, 16);
    /* the calf's parent may not be the thigh (twist bones): its local rotation
     * is conj(parent model) * new model; the new model is rh rk qb */
    {
        float nb[4], pb_t[3], pb_q[4], tmp[4];
        q_mul(rk, qb, tmp); q_mul(rh, tmp, nb); q_norm(nb);
        model_of(pose, g_sk.parent[B], pb_t, pb_q);        /* the pose now has the new thigh */
        q_conj(pb_q, inv);
        q_mul(inv, nb, tmp); q_norm(tmp);
        memcpy(pose + B*QS + 4, tmp, 16);
    }
}

static void apply(float *pose, int nbones)
{
    float W[16], tf[2][3], qf[4], wf[2][3], up_m[3], hit = 0, dt, want[2], ground[2], k = g_strength / 100.0f;
    static const float UP[3] = { 0, 0, 1 };
    LARGE_INTEGER now;
    int s, ok[2];
    static LONG logged;
    if (!hg_player_anim_skeleton(W)) return;
    g_mirror = W[0]*(W[5]*W[10] - W[6]*W[9]) - W[1]*(W[4]*W[10] - W[6]*W[8]) + W[2]*(W[4]*W[9] - W[5]*W[8]) < 0;
    QueryPerformanceCounter(&now);
    dt = g_sk.t.QuadPart ? (float)(now.QuadPart - g_sk.t.QuadPart) / (float)g_qpf.QuadPart : 0;
    g_sk.t = now;
    if (dt <= 0 || dt > 0.25f) dt = 0.016f;
    for (s = 0; s < 2; s++) {
        float from[3], n[3] = { 0, 0, 1 }, hx, hy, l;
        model_of(pose, g_sk.foot[s], tf[s], qf);
        to_world(W, tf[s], wf[s]);
        from[0] = wf[s][0]; from[1] = wf[s][1]; from[2] = W[14] + 0.6f;
        ok[s] = hg_ray_down(from, 1.6f, &hit);
        ground[s] = hit;
        /* the slope: two more rays a short step along world x and y */
        if (ok[s] && g_tilt) {
            float fx[3] = { from[0] + TILT_STEP, from[1], from[2] }, fy[3] = { from[0], from[1] + TILT_STEP, from[2] };
            if (hg_ray_down(fx, 1.6f, &hx) && hg_ray_down(fy, 1.6f, &hy) &&
                fabsf(hx - hit) < 0.12f && fabsf(hy - hit) < 0.12f) {           /* not across a step's edge */
                n[0] = -(hx - hit) / TILT_STEP; n[1] = -(hy - hit) / TILT_STEP; n[2] = 1;
            }
        }
        l = len3(n); n[0] /= l; n[1] /= l; n[2] /= l;
        if (!g_sk.nrm[s][2]) { g_sk.nrm[s][2] = 1; }
        g_sk.nrm[s][0] += (n[0] - g_sk.nrm[s][0]) * clampf(dt * 10.0f, 0, 1);
        g_sk.nrm[s][1] += (n[1] - g_sk.nrm[s][1]) * clampf(dt * 10.0f, 0, 1);
        g_sk.nrm[s][2] += (n[2] - g_sk.nrm[s][2]) * clampf(dt * 10.0f, 0, 1);
        /* the ground under the foot against the floor the animation stands on */
        want[s] = ok[s] ? hit - W[14] : 0.0f;
        if (fabsf(want[s]) > 0.45f) want[s] = 0.0f;               /* a ledge, a jump: leave it */
        want[s] *= k;
        g_sk.off[s] += (want[s] - g_sk.off[s]) * clampf(dt * 12.0f, 0, 1);
    }
    if (InterlockedIncrement(&logged) <= 20)
        hg_log("footik: floor %.2f; feet at %.2f/%.2f, ground %s%.2f/%s%.2f -> offsets %.2f/%.2f", W[14], wf[0][2], wf[1][2],
               ok[0] ? "" : "(none) ", want[0], ok[1] ? "" : "(none) ", want[1], g_sk.off[0], g_sk.off[1]);
    /* the pelvis drops by the larger downward offset */
    {
        float drop = g_sk.off[0] < g_sk.off[1] ? g_sk.off[0] : g_sk.off[1];
        if (drop > 0) drop = 0;
        g_sk.drop = drop;
    }
    dir_to_model(W, UP, up_m);
    {
        float s_len = sqrtf(W[0]*W[0] + W[1]*W[1] + W[2]*W[2]), l = len3(up_m);
        float pt[3], pq[4], inv[4], d_m[3], d_l[3];
        if (l < 1e-6f || s_len < 1e-6f) return;
        up_m[0] /= l; up_m[1] /= l; up_m[2] /= l;
        /* the body's drop, in its parent's frame (the root has none: model space) */
        d_m[0] = up_m[0] * g_sk.drop / s_len; d_m[1] = up_m[1] * g_sk.drop / s_len; d_m[2] = up_m[2] * g_sk.drop / s_len;
        if (g_sk.parent[g_sk.pelvis] >= 0) {
            model_of(pose, g_sk.parent[g_sk.pelvis], pt, pq);
            q_conj(pq, inv);
            q_rot(inv, d_m, d_l);
        } else
            memcpy(d_l, d_m, 12);
        pose[g_sk.pelvis*QS + 0] += d_l[0];
        pose[g_sk.pelvis*QS + 1] += d_l[1];
        pose[g_sk.pelvis*QS + 2] += d_l[2];
        /* each foot to its ground: where it was, plus its offset (the pelvis
         * drop is already in the pose, so the foot target is absolute) */
        for (s = 0; s < 2; s++) {
            float target[3], m = g_sk.off[s] / s_len;
            if (fabsf(g_sk.off[s] - g_sk.drop) < 1e-4f && fabsf(g_sk.drop) < 1e-4f) continue;
            target[0] = tf[s][0] + up_m[0] * m;
            target[1] = tf[s][1] + up_m[1] * m;
            target[2] = tf[s][2] + up_m[2] * m;
            solve_leg(pose, s, target);
        }
        /* each foot turned to its ground's slope, while it is on the ground
         * (a foot in the air keeps the animation's angle) */
        if (g_tilt)
            for (s = 0; s < 2; s++) {
                float n[3], axis[3], axm[3], ang, hf, w, l, qm[4], ft[3], fq[4], pt2[3], pq2[4], inv2[4], nq[4], lq[4];
                int P = g_sk.parent[g_sk.foot[s]];
                if (!ok[s]) continue;
                memcpy(n, g_sk.nrm[s], 12);
                l = len3(n); if (l < 1e-6f) continue;
                n[0] /= l; n[1] /= l; n[2] /= l;
                hf = wf[s][2] + g_sk.off[s] - ground[s];              /* the ankle over its ground */
                w = clampf((0.25f - hf) / 0.10f, 0, 1) * k;
                ang = acosf(clampf(n[2], -1, 1));
                if (ang > TILT_MAX) ang = TILT_MAX;
                ang *= w;
                if (ang < 1e-3f) continue;
                cross3(UP, n, axis);                                   /* up turned onto the normal */
                dir_to_model(W, axis, axm);
                l = len3(axm); if (l < 1e-6f) continue;
                axm[0] /= l; axm[1] /= l; axm[2] /= l;
                q_axis(axm, g_mirror ? -ang : ang, qm);
                model_of(pose, g_sk.foot[s], ft, fq);
                q_mul(qm, fq, nq); q_norm(nq);
                model_of(pose, P, pt2, pq2);
                q_conj(pq2, inv2);
                q_mul(inv2, nq, lq); q_norm(lq);
                memcpy(pose + g_sk.foot[s]*QS + 4, lq, 16);
            }
    }
    (void)nbones;
}

static void __fastcall d_sample(void *skel, void *edx, float *pose, int nbones, void *cache, int flag)
{
    o_sample(skel, edx, pose, nbones, cache, flag);
    if (!g_on || !pose || nbones <= 0 || nbones > MAXB) return;
    {
        float W[16];
        void *mine = hg_player_anim_skeleton(W);
        if (!mine || mine != skel) return;
    }
    if (g_sk.skel != skel || g_sk.n != nbones) g_sk.ok = learn(skel, nbones);
    if (g_sk.ok) apply(pose, nbones);
}

int footik_install(unsigned int image, int (*hook)(unsigned int, void *, void **, const char *))
{
    static const unsigned char sig[8] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x6a, 0xff };
    QueryPerformanceFrequency(&g_qpf);
    settings_var("ik.feet", &g_on, 0, 1);
    settings_var("ik.feet_strength", &g_strength, 0, 150);
    settings_var("ik.feet_tilt", &g_tilt, 0, 1);
    if (memcmp((void *)(image + RVA_HK_SAMPLE), sig, 8)) {
        hg_log("footik: sampler bytes differ; feet IK is off");
        return 0;
    }
    return hook(RVA_HK_SAMPLE, (void *)d_sample, (void **)&o_sample, "hkaAnimatedSkeleton sample (feet IK)");
}
