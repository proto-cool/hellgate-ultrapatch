/*
 * Animation snap detector, for the 3rd-person pops (Blademaster's head,
 * Guardian's shield).
 *
 * The first version hooked Granny 2 and saw zero calls in a whole session:
 * each animation record carries both a Granny control (+0x14) and a Havok
 * one (+0x18), and this build always takes the Havok branch. Animation is
 * Havok Animation 4.0, statically linked. What is hooked instead:
 *
 *   0x490d9a  the game's own animation trace, called with an event name
 *             ("ANIM_EASE_OUT UPDATE_STANCE", ...) and the model, with the
 *             animation record in esi. Normally silent behind a debug flag;
 *             we take the call regardless. Record: +0x0 id, +0x18 Havok
 *             control, +0x1c definition (def+0xc file name, +0x134 fEaseIn,
 *             +0x138 fEaseOut), +0x2c priority. Model +0x18 = owner unit id.
 *   0x4910a9  hkDefaultAnimationControl::easeIn  (control in eax, duration
 *   0x49cbba  hkDefaultAnimationControl::easeOut  on the stack, ret 4)
 *
 * A duration under ~1e-7 makes Havok set the inverse duration to FLT_MAX:
 * the blend completes in one frame. The game guards ease-in (a zero
 * fEaseIn becomes 0.1s at 0x4934fd) but passes fEaseOut straight through,
 * so a zero ease-out is a one-frame snap. That is what this looks for, on
 * the local player only, named by animation file and the event that asked.
 *
 * The bone-delta math below is kept for a later pass on the final pose.
 * It is Windows-free so test/ui.c can drive it.
 */
#include <math.h>
#include <string.h>
#include "panel.h"

/* ------------------------------------------------------------------ */
/* the pure part                                                       */

/* Row-major 4x4, D3D convention: rows 0-2 are the axes, row 3 the origin. */

/* Rotation angle, in degrees, between the rotation parts of two matrices. */
float aw_rot_delta(const float *a, const float *b)
{
    float ra[9], rb[9], tr = 0.0f, c;
    int i, j;

    for (i = 0; i < 3; i++) {                 /* normalise away any scale */
        float la = sqrtf(a[i*4]*a[i*4] + a[i*4+1]*a[i*4+1] + a[i*4+2]*a[i*4+2]);
        float lb = sqrtf(b[i*4]*b[i*4] + b[i*4+1]*b[i*4+1] + b[i*4+2]*b[i*4+2]);
        if (la < 1e-6f || lb < 1e-6f) return 0.0f;
        for (j = 0; j < 3; j++) { ra[i*3+j] = a[i*4+j] / la; rb[i*3+j] = b[i*4+j] / lb; }
    }
    for (i = 0; i < 3; i++)                   /* trace(A * B^T) */
        for (j = 0; j < 3; j++) tr += ra[i*3+j] * rb[i*3+j];
    c = (tr - 1.0f) * 0.5f;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * 57.29578f;
}

/*
 * A bone's matrix in its model's space: bone * inverse(model). The model
 * matrix is a rigid transform (rotation + translation) in practice, so the
 * inverse is the transpose plus a moved origin.
 */
void aw_to_model(const float *bone, const float *model, float *out)
{
    float inv[16];
    int i, j, k;
    float s2 = model[0]*model[0] + model[1]*model[1] + model[2]*model[2];
    float s = s2 > 1e-12f ? 1.0f / s2 : 1.0f;  /* uniform scale, if any */

    memset(inv, 0, sizeof inv);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) inv[i*4+j] = model[j*4+i] * s;
    for (j = 0; j < 3; j++)
        inv[12+j] = -(model[12]*inv[j] + model[13]*inv[4+j] + model[14]*inv[8+j]);
    inv[15] = 1.0f;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float v = 0.0f;
            for (k = 0; k < 4; k++) v += bone[i*4+k] * inv[k*4+j];
            out[i*4+j] = v;
        }
}

typedef struct {
    float prev[16];
    int   have;
    float ema_rot, ema_pos;     /* typical per-frame motion, for context */
} aw_track;

#define AW_SNAP_DEG      14.0f  /* a frame's rotation must beat this ...   */
#define AW_SNAP_RATIO    4.0f   /* ... and this many times the usual       */
#define AW_SNAP_CM       7.0f
#define AW_EMA           0.1f

/*
 * Feed one frame of a bone, in model space. Returns 1 if this frame is a
 * snap, with the jump in *deg / *cm.
 */
int aw_feed(aw_track *t, const float *m, float *deg, float *cm)
{
    float r, p;
    int snap = 0;

    if (!t->have) {
        memcpy(t->prev, m, sizeof t->prev);
        t->have = 1;
        *deg = *cm = 0.0f;
        return 0;
    }
    r = aw_rot_delta(t->prev, m);
    p = 100.0f * sqrtf((m[12]-t->prev[12])*(m[12]-t->prev[12]) +
                       (m[13]-t->prev[13])*(m[13]-t->prev[13]) +
                       (m[14]-t->prev[14])*(m[14]-t->prev[14]));
    if (r > AW_SNAP_DEG && r > AW_SNAP_RATIO * t->ema_rot + 3.0f) snap = 1;
    if (p > AW_SNAP_CM  && p > AW_SNAP_RATIO * t->ema_pos + 2.0f) snap = 1;

    /* A snap does not teach the average: the next one must still stand out. */
    if (!snap) {
        t->ema_rot += (r - t->ema_rot) * AW_EMA;
        t->ema_pos += (p - t->ema_pos) * AW_EMA;
    }
    memcpy(t->prev, m, sizeof t->prev);
    *deg = r; *cm = p;
    return snap;
}

#ifdef _WIN32
/* ------------------------------------------------------------------ */
/* the game-facing part                                                */

#include <windows.h>
#include <stdio.h>
#include "target.h"

static unsigned int g_img;

static LARGE_INTEGER g_qpf;
static double now_s(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_qpf.QuadPart;
}

/* The local player's unit id, via the global game (0xf267a4). */
static int player_id(void)
{
    unsigned char *game = *(unsigned char **)(g_img + RVA_GAME_GLOBAL), *unit;
    if (!game || *(int *)(game + GAME_IS_SERVER)) return -1;
    unit = *(unsigned char **)(game + GAME_PLAYER_UNIT);
    return unit ? *(int *)(unit + UNIT_ID) : -1;
}

/* Recent trace events on the player: which control belongs to which file. */
#define AW_RECENT 32
typedef struct {
    double      t;
    const char *evt;          /* static string in the exe */
    void       *control;
    char        file[64];
    float       ease_in, ease_out;
} aw_rec;
static aw_rec g_recent[AW_RECENT];
static unsigned int g_recent_w;

static void copy_name(char *dst, const unsigned char *def)
{
    const char *src = (const char *)(def + ANIMDEF_FILE);
    int i;
    dst[0] = 0;
    if (IsBadReadPtr(src, 1)) return;
    for (i = 0; i < 63 && !IsBadReadPtr(src + i, 1) && src[i]; i++) {
        char c = src[i];
        dst[i] = (c >= 32 && c < 127) ? c : '?';
    }
    dst[i] = 0;
}

/*
 * Local playback time of the player's controls, watched every frame from
 * the camera hook. Havok 4.0 keeps hkAnimationControl::m_localTime at +0x8;
 * that is an assumption about this build, so the first control seen also
 * gets its first 0x70 bytes dumped as floats to check it against.
 *
 * A wrap (time drops by most of a cycle) is a normal loop and is logged with
 * the cycle length: if snaps line up with wraps, the loop seam in the asset
 * is bad. A jump (time moves backwards by less, or forwards by far more than
 * a frame) means code moved the playhead: a restart or a retime.
 */
#define AW_TRACK 16
static struct { void *ctl; char file[64]; float last, maxseen; double tw; int wraps; } g_trk[AW_TRACK];

static void track_control(void *c, const char *file)
{
    int i, fr = -1;
    static int dumped;
    if (!c || IsBadReadPtr(c, 0x70)) return;
    for (i = 0; i < AW_TRACK; i++) {
        if (g_trk[i].ctl == c) return;
        if (!g_trk[i].ctl && fr < 0) fr = i;
    }
    if (fr < 0) fr = (int)(g_recent_w % AW_TRACK);    /* evict something */
    memset(&g_trk[fr], 0, sizeof g_trk[fr]);
    g_trk[fr].ctl = c;
    g_trk[fr].last = -1.0f;
    lstrcpynA(g_trk[fr].file, file[0] ? file : "?", 64);
    if (!dumped) {
        const float *f = (const float *)c;
        const unsigned int *u = (const unsigned int *)c;
        char buf[900];
        int n = snprintf(buf, sizeof buf, "anim: control layout %p %s:", c, file);
        for (i = 0; i < 28 && n < (int)sizeof buf - 30; i++)
            n += snprintf(buf + n, sizeof buf - n, " +%02x=%g/%08x", i * 4, f[i], u[i]);
        hg_log("%s", buf);
        dumped = 1;
    }
}

static void watch_controls(double t)
{
    int i;
    for (i = 0; i < AW_TRACK; i++) {
        float lt, d;
        if (!g_trk[i].ctl) continue;
        if (IsBadReadPtr(g_trk[i].ctl, 0x10)) { g_trk[i].ctl = NULL; continue; }
        lt = *(const float *)((const unsigned char *)g_trk[i].ctl + HKCTL_LOCAL_TIME);
        if (!(lt == lt)) continue;
        if (g_trk[i].last >= 0.0f) {
            d = lt - g_trk[i].last;
            if (lt > g_trk[i].maxseen) g_trk[i].maxseen = lt;
            if (d < -0.5f * g_trk[i].maxseen && g_trk[i].maxseen > 0.05f) {
                g_trk[i].wraps++;
                if (g_trk[i].wraps <= 3 || t - g_trk[i].tw > 5.0)
                    hg_log("anim: wrap %s %.3f -> %.3f (cycle ~%.3fs, wrap #%d)",
                           g_trk[i].file, g_trk[i].last, lt, g_trk[i].maxseen, g_trk[i].wraps);
                g_trk[i].tw = t;
            } else if (d < -0.02f || d > 0.25f) {
                hg_log("anim: JUMP %s %.3f -> %.3f (%+.3fs) -- playhead moved by code",
                       g_trk[i].file, g_trk[i].last, lt, d);
            }
        }
        g_trk[i].last = lt;
    }
}

volatile LONG g_aw_player_model = -1;
int aw_player_model_id(void) { return (int)g_aw_player_model; }

void __cdecl aw_on_trace(const char *evt, unsigned char *model, unsigned char *rec)
{
    aw_rec *r;
    unsigned char *def;
    int pid;

    if (!model || !rec || IsBadReadPtr(model, 0x1c) || IsBadReadPtr(rec, 0x30)) return;
    pid = player_id();
    if (pid < 0 || *(int *)(model + MODEL_OWNER_ID) != pid) return;
    /* The model record's id (+8, what the model hash table keys on). The
     * unit -> pGfx -> model chain fails for the local player (the unit the
     * getter returns has no graphics block), so this is how the DLL learns
     * the player's third-person model. */
    g_aw_player_model = *(int *)(model + 8);

    def = *(unsigned char **)(rec + ANIMREC_DEF);
    r = &g_recent[g_recent_w++ & (AW_RECENT - 1)];
    r->t = now_s();
    r->evt = evt;
    r->control = *(void **)(rec + ANIMREC_CONTROL);
    r->file[0] = 0;
    r->ease_in = r->ease_out = -1.0f;
    if (def && !IsBadReadPtr(def, ANIMDEF_EASE_OUT + 4)) {
        copy_name(r->file, def);
        r->ease_in  = *(float *)(def + ANIMDEF_EASE_IN);
        r->ease_out = *(float *)(def + ANIMDEF_EASE_OUT);
    }

    /* Every event on the player, so a re-triggered run cycle shows itself. */
    {
        static double win;
        static int n;
        if (r->t - win > 1.0) { win = r->t; n = 0; }
        if (++n <= 20)
            hg_log("anim: evt %-28s %s ctl=%p", evt ? evt : "?", r->file[0] ? r->file : "?",
                   r->control);
    }
    track_control(r->control, r->file);
}

static aw_rec *find_control(void *c)
{
    unsigned int i;
    for (i = 0; i < AW_RECENT; i++) {
        aw_rec *r = &g_recent[(g_recent_w - 1 - i) & (AW_RECENT - 1)];
        if (r->evt && r->control == c) return r;
    }
    return NULL;
}

/* Summary: ease durations on the player, and which files were instant. */
static long g_hist[5];                      /* <0.05, <0.10, <0.20, <0.40, >=0.40 */
#define AW_WORST 12
static struct { char file[64]; int n_in, n_out; } g_worst[AW_WORST];
static double g_flush;
static volatile LONG g_eases, g_instants;

static void note_worst(const char *file, int in)
{
    int i;
    for (i = 0; i < AW_WORST; i++)
        if (g_worst[i].file[0] && !strcmp(g_worst[i].file, file)) break;
    if (i == AW_WORST)
        for (i = 0; i < AW_WORST && g_worst[i].file[0]; i++) ;
    if (i == AW_WORST) return;
    if (!g_worst[i].file[0]) lstrcpynA(g_worst[i].file, file, 64);
    if (in) g_worst[i].n_in++; else g_worst[i].n_out++;
}

static void flush_summary(void)
{
    char buf[1100];
    int n, i;
    double t = now_s();
    if (t - g_flush < 10.0) return;
    g_flush = t;
    if (!(g_hist[0] + g_hist[1] + g_hist[2] + g_hist[3] + g_hist[4])) return;
    n = snprintf(buf, sizeof buf, "anim: 10s eases <50ms=%ld <100ms=%ld <200ms=%ld "
                 "<400ms=%ld longer=%ld | instant:",
                 g_hist[0], g_hist[1], g_hist[2], g_hist[3], g_hist[4]);
    for (i = 0; i < AW_WORST && n < (int)sizeof buf - 90; i++) {
        if (!g_worst[i].file[0]) continue;
        n += snprintf(buf + n, sizeof buf - n, " %s in%d/out%d;",
                      g_worst[i].file, g_worst[i].n_in, g_worst[i].n_out);
    }
    hg_log("%s", buf);
    memset(g_hist, 0, sizeof g_hist);
    memset(g_worst, 0, sizeof g_worst);
}

/*
 * The game gives ease-in a floor (0 becomes 0.1s at 0x4934fd) but passes
 * ease-out straight through, and Havok turns a ~0 duration into a one-frame
 * cut. Same floor, other direction, for every unit. Returned as the float's
 * bits; the stub writes it back over the stack argument.
 */
#define AW_MINOUT      0.12f
/* Default OFF since the 2026-09-21 in-game pass reported regressions; the
 * panel's Anim tab ("Ease-out floor") turns it on for an A/B. */
volatile LONG g_aw_minout_on = 0, g_aw_minout_n;

unsigned int __cdecl aw_on_ease(int in, void *control, float dur)
{
    aw_rec *r = find_control(control);
    unsigned int bits;
    int b;

    {
        float use = dur;
        if (!in && g_aw_minout_on && dur < 0.05f) {
            InterlockedIncrement(&g_aw_minout_n);
            use = AW_MINOUT;
        }
        memcpy(&bits, &use, 4);
    }

    flush_summary();
    if (!r) return bits;                    /* not the player's, or untraced */
    InterlockedIncrement(&g_eases);
    b = dur < 0.05f ? 0 : dur < 0.10f ? 1 : dur < 0.20f ? 2 : dur < 0.40f ? 3 : 4;
    g_hist[b]++;
    if (b == 0) {
        InterlockedIncrement(&g_instants);
        note_worst(r->file[0] ? r->file : "?", in);
        hg_log("anim: INSTANT ease-%s %.3fs  %s  (%s, %.2fs ago; def in %.2f out %.2f)",
               in ? "in" : "out", dur, r->file[0] ? r->file : "?", r->evt,
               now_s() - r->t, r->ease_in, r->ease_out);
    }
    return bits;
}

/* The file a player control was last traced with, or NULL. */
const char *aw_control_file(void *control)
{
    int i;
    aw_rec *r;
    for (i = 0; i < AW_TRACK; i++)
        if (g_trk[i].ctl == control && g_trk[i].file[0] != '?') return g_trk[i].file;
    r = find_control(control);
    return (r && r->file[0]) ? r->file : NULL;
}

/*
 * The three hooks take register arguments, so each gets a stub that saves
 * everything, hands the interesting registers to C, and resumes the
 * original through its trampoline.
 *
 * trace:  cdecl (evt, model), record in esi
 * ease:   control in eax, duration at [esp+4]
 */
void *g_aw_tramp_trace, *g_aw_tramp_in, *g_aw_tramp_out;
void aw_stub_trace(void);
void aw_stub_in(void);
void aw_stub_out(void);
__asm__(
    ".text\n\t"
    ".globl _aw_stub_trace\n"
    "_aw_stub_trace:\n\t"
    "pushal\n\t"
    "pushfl\n\t"
    "pushl %esi\n\t"                     /* record */
    "pushl 48(%esp)\n\t"                 /* model: 4+36 saved, +8 arg -> 44, +4 pushed */
    "pushl 48(%esp)\n\t"                 /* evt:   4+36 saved, +4 arg -> 40, +8 pushed */
    "call _aw_on_trace\n\t"
    "addl $12, %esp\n\t"
    "popfl\n\t"
    "popal\n\t"
    "jmp *_g_aw_tramp_trace\n\t"

    ".globl _aw_stub_in\n"
    "_aw_stub_in:\n\t"
    "pushal\n\t"
    "pushfl\n\t"
    "pushl 40(%esp)\n\t"                 /* duration: 36 saved + 4 ret */
    "pushl %eax\n\t"                     /* control */
    "pushl $1\n\t"
    "call _aw_on_ease\n\t"
    "addl $12, %esp\n\t"
    "movl %eax, 40(%esp)\n\t"            /* the (possibly floored) duration */
    "popfl\n\t"
    "popal\n\t"
    "jmp *_g_aw_tramp_in\n\t"

    ".globl _aw_stub_out\n"
    "_aw_stub_out:\n\t"
    "pushal\n\t"
    "pushfl\n\t"
    "pushl 40(%esp)\n\t"
    "pushl %eax\n\t"
    "pushl $0\n\t"
    "call _aw_on_ease\n\t"
    "addl $12, %esp\n\t"
    "movl %eax, 40(%esp)\n\t"
    "popfl\n\t"
    "popal\n\t"
    "jmp *_g_aw_tramp_out\n\t");

int animwatch_install(unsigned int image,
                      int (*hook)(unsigned int, void *, void **, const char *))
{
    static const unsigned char trace_sig[8] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec };
    static const unsigned char in_sig[4]    = { 0x80, 0x78, 0x68, 0x00 };
    int n = 0;

    g_img = image;
    QueryPerformanceFrequency(&g_qpf);
    if (memcmp((void *)(image + RVA_ANIM_TRACE), trace_sig, 8) ||
        memcmp((void *)(image + RVA_HK_EASE_IN), in_sig, 4) ||
        memcmp((void *)(image + RVA_HK_EASE_OUT), in_sig, 4)) {
        hg_log("anim: snap detector off -- animation code bytes do not match");
        return 0;
    }
    n += hook(RVA_ANIM_TRACE, (void *)aw_stub_trace, &g_aw_tramp_trace, "animation trace");
    n += hook(RVA_HK_EASE_IN, (void *)aw_stub_in, &g_aw_tramp_in, "hkDefaultAnimationControl::easeIn");
    n += hook(RVA_HK_EASE_OUT, (void *)aw_stub_out, &g_aw_tramp_out, "hkDefaultAnimationControl::easeOut");
    hg_log("anim: snap detector (Havok) -- %d of 3 hooks in", n);
    return n;
}

/* Every frame, from the camera hook. */
void animwatch_tick(void)
{
    if (g_qpf.QuadPart) watch_controls(now_s());
}

void hg_anim_status(long *eases, long *instants, int *unused)
{
    *eases = g_eases;
    *instants = g_instants;
    *unused = 0;
}
#endif
