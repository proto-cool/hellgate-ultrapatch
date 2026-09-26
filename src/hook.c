/*
 * Two-point instrumentation.
 *
 *   1. hkMoppLongRayVirtualMachine::queryRayOnTree — the thing that burns the
 *      frame. Hooked only to count calls and accumulate time; the detour must
 *      stay cheap because during a stall it runs tens of thousands of times
 *      per frame.
 *
 *   2. The game's central world-raycast helper — the thing that *asks* for the
 *      work, and the only place where origin, direction and ray length are all
 *      visible as plain arguments. This is where attribution happens.
 *
 * Correlating the two per 100ms window answers H1 (count), H2 (length),
 * H3 (cost per ray) and H4 (growth over time) from one log.
 *
 * Hot paths touch only thread-local memory: no locks, no allocation.
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "target.h"
#include "sha256.h"
#include "panel.h"

void overlay_start(unsigned int image);
int  fpview_install(unsigned int image, int (*hook)(unsigned int, void *, void **, const char *));
int  shoulder_install(unsigned int image,
                      int (*hook)(unsigned int, void *, void **, const char *));
void gfxprobe_install(unsigned int image);
void gfxprobe_poll(void);
void *hg_client_game(void);
long hg_shadow_calls(void);
long hg_shadow_player_calls(void);
#include "../ref/minhook/include/MinHook.h"

#define HG_WINDOW_MS    100
#define HG_SITES        128     /* distinct call sites tracked per thread */
#define HG_FRAMES       12      /* return addresses kept per site */

/*
 * queryRayOnTree runs ~13k/s in ordinary play and far more during a stall, so
 * capturing a stack on every call is not affordable. Sample one call in
 * HG_QSAMPLE instead and scale the counts; must be a power of two.
 */
#define HG_QSAMPLE      32u
#define HG_SKIP         1       /* skip the detour's own frame */

/* A ray this long is nonsense for a Hellgate level; flagged, not clamped. */
#define HG_HUGE_LEN     1.0e5f

/* A window busier than this is a stall, not normal play. Marked in the log so
 * spike windows can be pulled out with a single grep. */
/*
 * Calibrated against a 20-minute clean session: normal play reaches
 * p99 = 6177 queryRayOnTree calls and 5.6ms per 100ms window, peaking at
 * 14417 calls / 13.1ms. The first threshold was 5000 calls, which marked
 * ordinary play as a spike. What actually distinguishes the bug is *time*:
 * a window that spends a third of itself in the MOPP tree is pathological,
 * and the 1 FPS stall should blow past both of these.
 */
#define HG_SPIKE_QMS    33.0
#define HG_SPIKE_QRAY   50000u
/* A single Havok step longer than this means the frame already blew out. */
#define HG_SPIKE_DT     0.1f

/*
 * Address-space sampling rate, in windows. Walking every VA region is not
 * free and these counters move over minutes, not milliseconds, so this runs
 * at 1Hz while everything else runs at 10Hz. Instrumentation that perturbs
 * the thing it measures is worse than no instrumentation.
 */
#define HG_MEM_EVERY    10

/* MXCSR exception-status bits (sticky). DE = denormal operand. */
#define MXCSR_STATUS    0x3Fu
#define MXCSR_DE        0x02u
#define MXCSR_DAZ       0x0040u   /* denormals-are-zero  */
#define MXCSR_FTZ       0x8000u   /* flush-to-zero       */

/*
 * queryRayOnTree ends in `ret 0x0c`: callee-cleaned, this in ecx, three stack
 * args. GCC's __fastcall with five parameters emits exactly that.
 */
typedef void (__fastcall *query_ray_fn)(void *ecx, void *edx, void *a1, void *a2, void *a3);

/*
 * The game helper is NOT __fastcall. It takes ecx and edx in registers but
 * ends in a plain `ret`, and its caller does `add esp, 0x14` — register args
 * with a *caller*-cleaned stack. MSVC generates this for internal functions;
 * GCC has no matching attribute, so the detour is a hand-written shim that
 * preserves the convention exactly. Declaring this __fastcall would emit
 * `ret 0x14` and corrupt the caller's stack on every raycast.
 */
typedef void (*game_ray_fn)(void);

/* per-object physics step(this, float delta) — `ret 4`, callee-cleaned. */
typedef void (__fastcall *step_fn)(void *ecx, void *edx, float delta);
/* hkWorldCinfo::hkWorldCinfo(this) — __fastcall, bare ret. */
typedef void (__fastcall *cinfo_fn)(void *ecx, void *edx);
/* hkWorld::addEntity / ::removeEntity — thiscall, ret 8. */
typedef void *(__fastcall *entity_fn)(void *ecx, void *edx, void *a1, void *a2);
/* Camera: __cdecl, caller-cleaned, as the FirstPersonCamera handler shows. */
typedef void (__cdecl *set_cam_fn)(int mode, int force);
typedef void (__cdecl *restore_cam_fn)(void);
typedef int  (__cdecl *can_fp_fn)(void *item);
/* Script action handler: __cdecl, one pointer to the action context. */
typedef int (__cdecl *spawn_fn)(void *ctx);
/* The shared spawn primitive: __cdecl, 13 dword args, forwarded verbatim. */
typedef int (__cdecl *spawn13_fn)(void *, void *, void *, void *, void *, void *,
                                  void *, void *, void *, void *, void *, void *, void *);

typedef struct {
    unsigned int  frames[HG_FRAMES];
    unsigned int  count;
    unsigned int  nan;          /* origin/dir/length not finite */
    unsigned int  huge;         /* ray longer than HG_HUGE_LEN */
    unsigned int  degenerate;   /* zero-length ray */
    float         len_min, len_max;
    double        len_sum;
    float         scalar_max;   /* largest `length` argument seen */

    /* The single worst ray this site asked for in this window, kept verbatim.
     * Statistics say "something here is wrong"; this says exactly what the
     * game passed in, which is the difference between a hypothesis and a
     * root cause. A non-finite ray always wins over a merely long one. */
    int           worst_valid;
    int           worst_bad;    /* the kept ray was non-finite */
    float         worst_len;
    float         worst_origin[3];
    float         worst_dir[3];
    float         worst_length;

    unsigned long long ticks;   /* queryRayOnTree sites only */
} site;

typedef struct thread_block {
    struct thread_block *next;
    DWORD                tid;

    /* queryRayOnTree */
    volatile unsigned int      qcalls;
    volatile unsigned long long qticks;
    unsigned int  qsample;
    /* H7: floating-point state as seen on the physics thread */
    unsigned int  fp_samples;   /* sampled calls inspected */
    unsigned int  fp_denorm;    /* of those, calls that raised DE */
    unsigned int  fp_mxcsr;     /* last observed control word */
    unsigned short fp_cw;       /* last observed x87 control word */      /* free-running; every HG_QSAMPLE'th is kept */
    site          qsites[HG_SITES];
    unsigned int  nqsites;

    /* game raycast helper */
    unsigned int  gcalls;
    site          sites[HG_SITES];
    unsigned int  nsites;

    /* hkMoppBvTreeShape::castRay — actual rays, not tree-node visits */
    unsigned int  rcalls;
    unsigned int  rsample;
    site          rsites[HG_SITES];
    unsigned int  nrsites;

    /* per-object physics step */
    unsigned int  nsteps;
    float         dt_min, dt_max;
    double        dt_sum;
} thread_block;

static query_ray_fn  g_orig_query;
static step_fn       g_orig_step;
static cinfo_fn      g_orig_cinfo;
static query_ray_fn  g_orig_castray;
static query_ray_fn  g_orig_castray_coll;
static unsigned int  g_rsample = 1;   /* capture every ray by default */
static spawn_fn      g_orig_spawn_obj;
static spawn_fn      g_orig_spawn_mon;
static spawn13_fn    g_orig_spawn_prim;
static entity_fn     g_orig_add_entity;
static entity_fn     g_orig_rem_entity;
static volatile LONG g_bodies_live;
static volatile LONG g_bodies_added;
static volatile LONG g_bodies_removed;
static unsigned int  g_spawn_mult = 1;    /* 1 = passthrough, harness off */
static unsigned int  g_spawn_cap = 200;   /* max extra spawns per window */
static volatile LONG g_spawn_extra;
static volatile LONG g_spawn_budget;
static int           g_simtype_override = -1;
static int           g_simtype_seen = -1;

/* Viewmodel experiment. Both callees use conventions GCC cannot express,
 * so they are reached through the shims below rather than declared. */
static unsigned int   g_fn_model_third;
static unsigned int   g_fn_model_flag;
/* Local copy of the player getter; panel.c has its own and this file must
 * not depend on the panel being compiled in. */
typedef int (__cdecl *local_player_fn)(void);
static local_player_fn g_get_player_local;
static volatile LONG  g_model_req = -1;   /* packed bit<<8 | value | 0x10000 */

/* First person and its weapon gate. See target.h for the whole mechanism. */
static set_cam_fn     g_set_camera;
static restore_cam_fn g_restore_camera;
static can_fp_fn      g_orig_can_fp;
static const int     *g_cam_mode;
static unsigned char *g_kick_site;
static unsigned char  g_kick_orig;
static volatile LONG  g_fp_melee;      /* the unlock is on                */
static volatile LONG  g_cam_req = -1;  /* pending request from a frontend */
static game_ray_fn   g_orig_game;
static DWORD         g_tls = TLS_OUT_OF_INDEXES;
static thread_block *volatile g_blocks;
static LARGE_INTEGER g_qpf;
static HANDLE        g_log = INVALID_HANDLE_VALUE;
static unsigned int  g_image;
static unsigned int  g_stack_depth = HG_FRAMES;
static WCHAR         g_dll_dir[MAX_PATH];
static unsigned long long g_window;

/* ------------------------------------------------------------------ */

/*
 * A ring of the most recent log lines, so the overlay can show them without
 * anyone alt-tabbing to read the file.
 *
 * Per-slot seqlock rather than a critical section: logv_ is called from the
 * worker, the render thread and the game thread, and one of those must never
 * block. A reader that loses the race retries and then gives up, because a
 * missed line in a debug pane costs nothing.
 */
static struct {
    volatile LONG seq;
    char          text[HG_LOGLINE];
} g_ring[HG_LOGRING];
static volatile LONG g_ring_w;

static void ring_put(const char *s, int n)
{
    LONG slot = (InterlockedIncrement(&g_ring_w) - 1) & (HG_LOGRING - 1);
    if (n >= HG_LOGLINE) n = HG_LOGLINE - 1;
    InterlockedIncrement(&g_ring[slot].seq);
    MemoryBarrier();
    memcpy(g_ring[slot].text, s, (size_t)n);
    g_ring[slot].text[n] = 0;
    MemoryBarrier();
    InterlockedIncrement(&g_ring[slot].seq);
}

int hg_log_line(int age, char *out, int cap)
{
    LONG w = g_ring_w;
    LONG slot;
    int tries;

    if (age < 0 || age >= HG_LOGRING || age >= w || cap <= 0) return 0;
    slot = (w - 1 - age) & (HG_LOGRING - 1);

    for (tries = 0; tries < 4; tries++) {
        LONG a = g_ring[slot].seq;
        if (a & 1) continue;
        MemoryBarrier();
        lstrcpynA(out, g_ring[slot].text, cap);
        MemoryBarrier();
        if (g_ring[slot].seq == a) return out[0] != 0;
    }
    return 0;
}

/* A committed, readable page check for verify_bytes. */
static int readable_code(const void *p, unsigned int len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (unsigned int)((const char *)mbi.BaseAddress + mbi.RegionSize
                          - (const char *)p) >= len;
}

static void logv_(const char *fmt, va_list ap)
{
    char buf[2048];
    int n;
    DWORD written;

    n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    ring_put(buf, n);
    buf[n++] = '\n';
    if (g_log != INVALID_HANDLE_VALUE)
        WriteFile(g_log, buf, n, &written, NULL);
    OutputDebugStringA(buf);
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logv_(fmt, ap);
    va_end(ap);
}

/*
 * Same sink, reachable from panel.c and overlay.c. Those two had no way to
 * report anything, which meant a silent overlay was indistinguishable from a
 * missing one -- exactly the case that wasted a session.
 */
void hg_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logv_(fmt, ap);
    va_end(ap);
}

static thread_block *get_block(void)
{
    thread_block *tb = (thread_block *)TlsGetValue(g_tls);
    thread_block *head;

    if (tb) return tb;
    tb = (thread_block *)VirtualAlloc(NULL, sizeof *tb, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE);
    if (!tb) return NULL;
    tb->tid = GetCurrentThreadId();
    TlsSetValue(g_tls, tb);
    do {
        head = g_blocks;
        tb->next = head;
    } while (InterlockedCompareExchangePointer((PVOID volatile *)&g_blocks, tb, head) != head);
    return tb;
}

static site *find_site(site *tab, unsigned int *n, const unsigned int *frames);

/* ------------------------------------------------------------------ */
/* floating-point state (H7)                                            */

static unsigned short x87_cw(void)
{
    unsigned short w;
    __asm__ __volatile__("fnstcw %0" : "=m"(w));
    return w;
}
static unsigned int mxcsr_get(void)
{
    unsigned int v;
    __asm__ __volatile__("stmxcsr %0" : "=m"(v));
    return v;
}
static void mxcsr_set(unsigned int v)
{
    __asm__ __volatile__("ldmxcsr %0" : : "m"(v));
}

/* ------------------------------------------------------------------ */
/* 1. queryRayOnTree — count, time, and sampled attribution            */

static void __fastcall detour_query(void *ecx, void *edx, void *a1, void *a2, void *a3)
{
    thread_block *tb = get_block();
    LARGE_INTEGER t0, t1;
    unsigned int m0 = 0;
    int sample;

    if (!tb) { g_orig_query(ecx, edx, a1, a2, a3); return; }

    /*
     * H7: does this call generate denormals? MXCSR's exception-status bits
     * are sticky, so a plain read only says "at some point, yes". To get a
     * rate we clear them, run the call, read them back, then put the
     * original sticky bits back exactly as they were — only the *status*
     * bits are touched, never a control bit, so the game's FP behaviour is
     * unchanged. Sampled, because stmxcsr/ldmxcsr on every call would not
     * be free.
     */
    sample = ((++tb->qsample & (HG_QSAMPLE - 1)) == 0);
    if (sample) {
        m0 = mxcsr_get();
        mxcsr_set(m0 & ~MXCSR_STATUS);
    }

    QueryPerformanceCounter(&t0);
    g_orig_query(ecx, edx, a1, a2, a3);
    QueryPerformanceCounter(&t1);
    tb->qcalls++;
    tb->qticks += (unsigned long long)(t1.QuadPart - t0.QuadPart);

    /*
     * A clean 20-minute session showed 393 queryRayOnTree calls for every one
     * world raycast the game asked for, and thousands of calls in windows
     * where the game asked for none at all. Whatever drives this is not the
     * path the other hook watches, so sample the stack here and find out.
     */
    if (sample) {
        unsigned int frames[HG_FRAMES];
        unsigned int m1 = mxcsr_get();
        site *qs;

        mxcsr_set((m1 & ~MXCSR_STATUS) | (m0 & MXCSR_STATUS));
        tb->fp_samples++;
        if (m1 & MXCSR_DE) tb->fp_denorm++;
        tb->fp_mxcsr = m1;
        tb->fp_cw = x87_cw();

        memset(frames, 0, sizeof frames);
        CaptureStackBackTrace(HG_SKIP, g_stack_depth, (PVOID *)frames, NULL);
        qs = find_site(tb->qsites, &tb->nqsites, frames);
        qs->count++;
        qs->ticks += (unsigned long long)(t1.QuadPart - t0.QuadPart);
    }
}

/* ------------------------------------------------------------------ */
/* 2. the game's raycast helper — attribution and ray geometry          */

static int finite_f(float f)
{
    unsigned int u;
    memcpy(&u, &f, 4);
    return (u & 0x7f800000u) != 0x7f800000u;   /* false for inf and NaN */
}

static int readable(const void *p, SIZE_T len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (SIZE_T)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)p) >= len;
}

static site *find_site(site *tab, unsigned int *n, const unsigned int *frames)
{
    unsigned int i, h = 2166136261u;
    for (i = 0; i < g_stack_depth; i++) { h ^= frames[i]; h *= 16777619u; }
    for (i = 0; i < *n; i++) {
        if (memcmp(tab[i].frames, frames, g_stack_depth * sizeof(unsigned int)) == 0)
            return &tab[i];
    }
    if (*n >= HG_SITES) return &tab[h % HG_SITES];   /* fold; rare */
    {
        site *s = &tab[(*n)++];
        memcpy(s->frames, frames, g_stack_depth * sizeof(unsigned int));
        s->len_min = 1e30f;
        s->len_max = -1e30f;
        return s;
    }
}

/*
 * Observer half of the shim: plain cdecl, so GCC can generate it normally.
 * `length_bits` carries the float as raw bits to keep the ABI unambiguous.
 */
void game_ray_observe(void *ecx, void *edx, const float *origin,
                      const float *dir, unsigned length_bits,
                      unsigned a6, unsigned a7)
{
    thread_block *tb = get_block();
    float length;
    unsigned int frames[HG_FRAMES];
    site *s;
    float raylen = 0.0f;
    int bad = 0;

    (void)a6; (void)a7;
    if (!tb) return;
    memcpy(&length, &length_bits, 4);

    memset(frames, 0, sizeof frames);
    CaptureStackBackTrace(HG_SKIP, g_stack_depth, (PVOID *)frames, NULL);

    tb->gcalls++;
    s = find_site(tb->sites, &tb->nsites, frames);
    s->count++;

    /* The helper itself null-checks ecx/edx and does nothing if either is
     * null, so a null call is not a ray at all — record it as a call and
     * leave the geometry alone. */
    if (!ecx || !edx) return;

    if (!finite_f(length)) bad = 1;
    if (readable(dir, 12) && readable(origin, 12)) {
        float dx = dir[0] * length, dy = dir[1] * length, dz = dir[2] * length;
        if (!finite_f(origin[0]) || !finite_f(origin[1]) || !finite_f(origin[2]) ||
            !finite_f(dir[0])    || !finite_f(dir[1])    || !finite_f(dir[2]))
            bad = 1;
        raylen = (float)sqrt((double)dx*dx + (double)dy*dy + (double)dz*dz);
        if (!finite_f(raylen)) bad = 1;
    } else {
        bad = 1;
    }

    /* Keep the worst offender: any non-finite ray beats a finite one, and
     * among finite rays the longest wins. */
    if ((bad && !s->worst_bad) || (bad == s->worst_bad && raylen > s->worst_len)) {
        s->worst_valid = 1;
        s->worst_bad = bad;
        s->worst_len = raylen;
        s->worst_length = length;
        if (readable(origin, 12)) memcpy(s->worst_origin, origin, 12);
        if (readable(dir, 12))    memcpy(s->worst_dir, dir, 12);
    }

    if (bad) {
        s->nan++;
    } else {
        if (raylen > s->len_max) s->len_max = raylen;
        if (raylen < s->len_min) s->len_min = raylen;
        s->len_sum += raylen;
        if (raylen > HG_HUGE_LEN) s->huge++;
        if (raylen == 0.0f) s->degenerate++;
        if (length > s->scalar_max) s->scalar_max = length;
    }

}

/*
 * Shim entry. Stack on entry:
 *     [ebp+0x04] return address
 *     [ebp+0x08] origin      [ebp+0x0c] dir     [ebp+0x10] length
 *     [ebp+0x14] a6          [ebp+0x18] a7
 * ecx/edx hold the two register arguments; esi/edi are callee-saved across
 * the C call, so they are safe places to park them. We clean our own pushes
 * and return with a bare `ret`, leaving the arguments for the caller to pop,
 * exactly as the original does.
 */
__asm__(
    ".text\n"
    ".globl _detour_game_shim\n"
    "_detour_game_shim:\n"
    "    push  %ebp\n"
    "    mov   %esp, %ebp\n"
    "    push  %esi\n"
    "    push  %edi\n"
    "    mov   %ecx, %esi\n"
    "    mov   %edx, %edi\n"
    /* game_ray_observe(ecx, edx, origin, dir, length, a6, a7) — cdecl */
    "    push  0x18(%ebp)\n"
    "    push  0x14(%ebp)\n"
    "    push  0x10(%ebp)\n"
    "    push  0x0c(%ebp)\n"
    "    push  0x08(%ebp)\n"
    "    push  %edx\n"
    "    push  %ecx\n"
    "    call  _game_ray_observe\n"
    "    add   $28, %esp\n"
    /* now the original, same convention, caller-cleaned */
    "    push  0x18(%ebp)\n"
    "    push  0x14(%ebp)\n"
    "    push  0x10(%ebp)\n"
    "    push  0x0c(%ebp)\n"
    "    push  0x08(%ebp)\n"
    "    mov   %esi, %ecx\n"
    "    mov   %edi, %edx\n"
    "    call  *_g_orig_game\n"
    "    add   $20, %esp\n"
    "    pop   %edi\n"
    "    pop   %esi\n"
    "    mov   %ebp, %esp\n"
    "    pop   %ebp\n"
    "    ret\n");

extern void detour_game_shim(void);

/* ------------------------------------------------------------------ */
/* 3. per-object physics step — records the frame delta                 */

static void __fastcall detour_step(void *ecx, void *edx, float delta)
{
    thread_block *tb = get_block();
    if (tb) {
        if (!tb->nsteps) { tb->dt_min = delta; tb->dt_max = delta; }
        else {
            if (delta < tb->dt_min) tb->dt_min = delta;
            if (delta > tb->dt_max) tb->dt_max = delta;
        }
        tb->nsteps++;
        tb->dt_sum += (double)delta;
    }
    /* The panel's command queue is drained here: this is the one hook that
     * reliably runs on the thread owning the world. Cheap no-op when off. */
    panel_pump();
    g_orig_step(ecx, edx, delta);
}

/* ------------------------------------------------------------------ */
/* 3b. hkMoppBvTreeShape::castRay — one call == one real ray             */

static void castray_common(thread_block *tb, unsigned long long dt)
{
    unsigned int frames[HG_FRAMES];
    site *rs;

    tb->rcalls++;
    if (g_rsample > 1 && (++tb->rsample % g_rsample)) return;

    memset(frames, 0, sizeof frames);
    CaptureStackBackTrace(HG_SKIP, g_stack_depth, (PVOID *)frames, NULL);
    rs = find_site(tb->rsites, &tb->nrsites, frames);
    rs->count++;
    rs->ticks += dt;
}

static void __fastcall detour_castray(void *ecx, void *edx, void *a1, void *a2, void *a3)
{
    thread_block *tb = get_block();
    LARGE_INTEGER t0, t1;
    if (!tb) { g_orig_castray(ecx, edx, a1, a2, a3); return; }
    QueryPerformanceCounter(&t0);
    g_orig_castray(ecx, edx, a1, a2, a3);
    QueryPerformanceCounter(&t1);
    castray_common(tb, (unsigned long long)(t1.QuadPart - t0.QuadPart));
}

static void __fastcall detour_castray_coll(void *ecx, void *edx, void *a1, void *a2, void *a3)
{
    thread_block *tb = get_block();
    LARGE_INTEGER t0, t1;
    if (!tb) { g_orig_castray_coll(ecx, edx, a1, a2, a3); return; }
    QueryPerformanceCounter(&t0);
    g_orig_castray_coll(ecx, edx, a1, a2, a3);
    QueryPerformanceCounter(&t1);
    castray_common(tb, (unsigned long long)(t1.QuadPart - t0.QuadPart));
}

/* ------------------------------------------------------------------ */
/* 6. live physics-body count — H8's independent variable                */

static void *__fastcall detour_add_entity(void *ecx, void *edx, void *a1, void *a2)
{
    void *r = g_orig_add_entity(ecx, edx, a1, a2);
    InterlockedIncrement(&g_bodies_live);
    InterlockedIncrement(&g_bodies_added);
    return r;
}

static void *__fastcall detour_rem_entity(void *ecx, void *edx, void *a1, void *a2)
{
    void *r = g_orig_rem_entity(ecx, edx, a1, a2);
    InterlockedDecrement(&g_bodies_live);
    InterlockedIncrement(&g_bodies_removed);
    return r;
}

/* ------------------------------------------------------------------ */
/* 5. spawn: capture a real spawn, then replay it on demand              */

/*
 * What was here before queued a request and waited for the game to spawn
 * something of its own accord, then rode along. In a quiet room the game
 * never spawns, so the button did nothing, said nothing, and left "10
 * queued" on screen indefinitely. That is the bug: it was never a spawn
 * command, it was a multiplier with no input.
 *
 * The replacement records the spawn instead of riding it. Every argument of
 * the last real spawn is kept verbatim -- all thirteen dwords of the shared
 * primitive, or the single context pointer of a script action -- and the
 * panel replays that exact call as many times as asked, from the pump, on
 * the game thread, immediately.
 *
 * The honest limit: the arguments still have to come from somewhere. Nobody
 * has mapped that context struct, so it cannot be synthesised, and until the
 * game spawns something once there is no template and the buttons cannot
 * fire. The difference is that the panel now *says* so -- template state,
 * observed call counts per hook and the owning thread are all reported, so
 * "nothing happened" always resolves into a specific reason.
 *
 * Where the replay runs is a real trade-off:
 *
 *   immediate (default) fires from panel_pump(), which is called inside the
 *   Havok step. It is the only place that runs often enough on the world's
 *   own thread to make a button feel like a button, and it means creating an
 *   entity part-way through a simulation step.
 *
 *   piggyback fires from inside the game's own spawn call, which is a
 *   context the game has just proved safe, but only when the game spawns.
 *
 * Immediate is the default because a dev panel that does nothing is worse
 * than one that can destabilise a session you started in order to break it.
 * The toggle is one click away on the Spawn tab.
 */

#define SPAWN_ARGS      13
#define SPAWN_PER_PUMP  4   /* replays per pump call; the pump runs ~45x/frame */
#define SPAWN_QUEUE_MAX 2000

static void         *g_tmpl_args[SPAWN_ARGS];
static volatile LONG g_tmpl_kind;
static volatile LONG g_tmpl_tid;
static volatile LONG g_pump_tid;
static volatile LONG g_fire_queue;
static volatile LONG g_fired;
static volatile LONG g_seen_prim;
static volatile LONG g_seen_script;
static volatile LONG g_replaying;
static volatile LONG g_fire_immediate = 1;
static volatile LONG g_spawn_extra_total;
static int           g_spawn_hooked;

/*
 * The panel wants totals since load; the log line wants per-window deltas.
 * Keeping both beats making the log reset a counter the panel is reading.
 */
static volatile LONG g_seen_prim_win, g_seen_script_win, g_fired_win;

static const char *tmpl_name(LONG k)
{
    switch (k) {
    case HG_TMPL_PRIM:       return "spawn primitive";
    case HG_TMPL_SCRIPT_OBJ: return "SpawnObject";
    case HG_TMPL_SCRIPT_MON: return "SpawnMonsterNearby";
    default:                 return "none";
    }
}

/*
 * Never called while a replay is in flight: a spawn arriving on another
 * thread mid-replay would splice its arguments into the array being read
 * and hand the game a mixed context.
 */
static void tmpl_capture(int kind, void *const *argv, int n)
{
    int i;
    LONG was;

    if (g_replaying) return;
    for (i = 0; i < n && i < SPAWN_ARGS; i++) g_tmpl_args[i] = argv[i];
    for (; i < SPAWN_ARGS; i++) g_tmpl_args[i] = NULL;
    InterlockedExchange(&g_tmpl_tid, (LONG)GetCurrentThreadId());
    MemoryBarrier();
    was = InterlockedExchange(&g_tmpl_kind, kind);
    if (was != kind)
        logf_("spawn: template captured from %s on tid %lu — panel spawn "
              "buttons are now live", tmpl_name(kind),
              (unsigned long)GetCurrentThreadId());
}

static long spawn_replay(long want)
{
    long fired = 0;

    if (want <= 0 || !g_tmpl_kind) return 0;
    if (InterlockedExchange(&g_replaying, 1)) return 0;   /* already inside */

    while (fired < want) {
        void **a = g_tmpl_args;
        switch (g_tmpl_kind) {
        case HG_TMPL_PRIM:
            if (!g_orig_spawn_prim) goto done;
            g_orig_spawn_prim(a[0], a[1], a[2], a[3], a[4], a[5], a[6],
                              a[7], a[8], a[9], a[10], a[11], a[12]);
            break;
        case HG_TMPL_SCRIPT_OBJ:
            if (!g_orig_spawn_obj) goto done;
            g_orig_spawn_obj(a[0]);
            break;
        case HG_TMPL_SCRIPT_MON:
            if (!g_orig_spawn_mon) goto done;
            g_orig_spawn_mon(a[0]);
            break;
        default:
            goto done;
        }
        fired++;
    }
done:
    InterlockedExchange(&g_replaying, 0);
    if (fired) {
        InterlockedExchangeAdd(&g_fired, (LONG)fired);
        InterlockedExchangeAdd(&g_fired_win, (LONG)fired);
    }
    return fired;
}

/* `cap` of 0 means drain the whole queue. */
static long spawn_drain(long cap)
{
    LONG want = g_fire_queue;
    long fired;

    if (want <= 0) return 0;
    if (cap > 0 && want > cap) want = (LONG)cap;
    fired = spawn_replay(want);
    if (fired) InterlockedExchangeAdd(&g_fire_queue, -(LONG)fired);
    return fired;
}

void hg_spawn_queue(long n)
{
    if (n <= 0) return;
    if (n > SPAWN_QUEUE_MAX) n = SPAWN_QUEUE_MAX;
    /* Clamp the total too, not just one request: the buttons add rather than
     * replace, and a leaning finger should not bank ten thousand spawns. */
    if (InterlockedExchangeAdd(&g_fire_queue, (LONG)n) + n > SPAWN_QUEUE_MAX)
        InterlockedExchange(&g_fire_queue, SPAWN_QUEUE_MAX);
    if (!g_spawn_hooked)
        logf_("panel: queued %ld spawn(s) — but the spawn hooks are NOT "
              "installed, so nothing can fire", n);
    else if (!g_tmpl_kind)
        logf_("panel: queued %ld spawn(s) — no template yet; nothing fires "
              "until the game spawns something once (seen: prim=%ld script=%ld)",
              n, g_seen_prim, g_seen_script);
    else
        logf_("panel: queued %ld spawn(s), %ld owed, template=%s",
              n, (long)g_fire_queue, tmpl_name(g_tmpl_kind));
}

void hg_spawn_clear(void)
{
    LONG had = InterlockedExchange(&g_fire_queue, 0);
    if (had) logf_("panel: spawn queue cleared (%ld dropped)", (long)had);
}

void hg_spawn_set_immediate(int on)
{
    InterlockedExchange(&g_fire_immediate, on ? 1 : 0);
    logf_("panel: spawn fire mode = %s", on
          ? "immediate (from the physics pump)"
          : "piggyback (only when the game itself spawns)");
}

void hg_spawn_set_mult(unsigned int m)
{
    if (m == 0) m = 1;
    if (m > 500) m = 500;
    g_spawn_mult = m;
    logf_("panel: spawn multiplier = %u (cap %u/window)", m, g_spawn_cap);
}

void hg_spawn_status(hg_spawn_state *out)
{
    if (!out) return;
    out->seen_prim   = g_seen_prim;
    out->seen_script = g_seen_script;
    out->fired       = g_fired;
    out->queued      = g_fire_queue;
    out->amplified   = g_spawn_extra_total;
    out->hooked      = g_spawn_hooked;
    out->tmpl_kind   = (int)g_tmpl_kind;
    out->immediate   = (int)g_fire_immediate;
    out->mult        = g_spawn_mult;
    out->tmpl_tid    = (unsigned long)g_tmpl_tid;
    out->pump_tid    = (unsigned long)g_pump_tid;
}

/*
 * The game thread's half of it. Recording the pump's thread id here is not
 * decoration: if it ever differs from the thread the template was captured
 * on, replaying would call a game spawn from the wrong thread, so the replay
 * is skipped and the panel shows both ids side by side.
 */
long hg_spawn_pump(void)
{
    LONG me = (LONG)GetCurrentThreadId();

    InterlockedExchange(&g_pump_tid, me);
    if (!g_fire_immediate || !g_tmpl_kind) return 0;
    if (me != g_tmpl_tid) return 0;
    return spawn_drain(SPAWN_PER_PUMP);
}

/*
 * HG_SPAWN_MULT: every spawn the game performs becomes N. Unchanged in
 * purpose -- it is the deterministic repro harness for H8 -- but it now
 * shares the capture and drain path with the panel.
 */
static void spawn_multiply(spawn_fn orig, void *ctx)
{
    unsigned int i;
    for (i = 1; i < g_spawn_mult; i++) {
        if (InterlockedIncrement(&g_spawn_budget) > (LONG)g_spawn_cap) break;
        InterlockedIncrement(&g_spawn_extra);
        InterlockedIncrement(&g_spawn_extra_total);
        orig(ctx);
    }
}

static int amplify(spawn_fn orig, int kind, void *ctx)
{
    int r = orig(ctx);

    InterlockedIncrement(&g_seen_script);
    InterlockedIncrement(&g_seen_script_win);
    tmpl_capture(kind, (void *const *)&ctx, 1);

    /* Piggyback drain. Runs in both modes: the game has just proved this
     * context valid, which makes it the safest moment there is. */
    spawn_drain(0);

    if (g_spawn_mult > 1) spawn_multiply(orig, ctx);
    return r;
}

static int __cdecl detour_spawn_prim(void *a0, void *a1, void *a2, void *a3, void *a4,
                                     void *a5, void *a6, void *a7, void *a8, void *a9,
                                     void *a10, void *a11, void *a12)
{
    void *argv[SPAWN_ARGS];
    int r = g_orig_spawn_prim(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12);
    unsigned int i;

    argv[0]=a0;  argv[1]=a1;  argv[2]=a2;  argv[3]=a3;  argv[4]=a4;
    argv[5]=a5;  argv[6]=a6;  argv[7]=a7;  argv[8]=a8;  argv[9]=a9;
    argv[10]=a10; argv[11]=a11; argv[12]=a12;

    InterlockedIncrement(&g_seen_prim);
    InterlockedIncrement(&g_seen_prim_win);
    tmpl_capture(HG_TMPL_PRIM, argv, SPAWN_ARGS);
    spawn_drain(0);

    if (g_spawn_mult <= 1) return r;
    for (i = 1; i < g_spawn_mult; i++) {
        if (InterlockedIncrement(&g_spawn_budget) > (LONG)g_spawn_cap) break;
        InterlockedIncrement(&g_spawn_extra);
        InterlockedIncrement(&g_spawn_extra_total);
        g_orig_spawn_prim(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12);
    }
    return r;
}

static int __cdecl detour_spawn_obj(void *ctx)
{
    return amplify(g_orig_spawn_obj, HG_TMPL_SCRIPT_OBJ, ctx);
}

static int __cdecl detour_spawn_mon(void *ctx)
{
    return amplify(g_orig_spawn_mon, HG_TMPL_SCRIPT_MON, ctx);
}

/* ------------------------------------------------------------------ */
/* 4. hkWorldCinfo — the simulation type (H8)                            */

/*
 * Observe, and optionally override, hkWorldCinfo::m_simulationType.
 *
 * The stock value is 2 (CONTINUOUS): Havok sweeps every moving body against
 * the world every step, and each sweep against level geometry is a MOPP ray
 * query. That is the suspected source of the raycast volume.
 *
 * HG_SIM_TYPE=1 forces DISCRETE, reproducing what the "2026 fix" does by
 * patching the binary. This exists to *prove causation* with a controlled
 * A/B on one machine — run the same route twice and compare `qray`. It is
 * not a shipping fix: global DISCRETE removes tunnelling protection from
 * everything, including projectiles and the player.
 */
static void __fastcall detour_cinfo(void *ecx, void *edx)
{
    unsigned char *p = (unsigned char *)ecx;

    g_orig_cinfo(ecx, edx);

    if (!p) return;
    g_simtype_seen = p[HKWORLDCINFO_SIMTYPE];
    logf_("C hkWorldCinfo at %p simulationType=%u (%s)", p,
          p[HKWORLDCINFO_SIMTYPE],
          p[HKWORLDCINFO_SIMTYPE] == 1 ? "DISCRETE" :
          p[HKWORLDCINFO_SIMTYPE] == 2 ? "CONTINUOUS" :
          p[HKWORLDCINFO_SIMTYPE] == 3 ? "MULTITHREADED" : "?");

    if (g_simtype_override >= 0) {
        p[HKWORLDCINFO_SIMTYPE] = (unsigned char)g_simtype_override;
        logf_("C   overridden to %d by HG_SIM_TYPE", g_simtype_override);
    }
}

/* ------------------------------------------------------------------ */
/* panel-facing experiment knobs                                        */

/*
 * The override is latched here and applied the next time the game builds a
 * world -- hkWorldCinfo is a construction-time descriptor, so nothing about
 * the live world changes when this is clicked. The panel reports the
 * observed value and the pending override separately for that reason; a
 * button that silently does nothing until a zone change is exactly the kind
 * of thing that wasted a session on the spawn queue.
 */
void hg_set_simtype(int t)
{
    if (t < -1 || t > 3) return;
    g_simtype_override = t;
    if (t < 0)
        logf_("panel: hkWorldCinfo override cleared; the game's own value stands");
    else
        logf_("panel: hkWorldCinfo::m_simulationType override = %d (%s) — "
              "applies at the next world construction, i.e. the next zone",
              t, t == 1 ? "DISCRETE" : t == 2 ? "CONTINUOUS" :
                 t == 3 ? "MULTITHREADED" : "INVALID");
}

int hg_get_simtype_override(void) { return g_simtype_override; }
int hg_get_simtype_seen(void)     { return g_simtype_seen; }

/* ------------------------------------------------------------------ */
/* first person                                                         */

/*
 * The detour is a passthrough until the unlock is on, so installing it
 * costs nothing: CanUseFirstPerson is asked twice per camera-mode change,
 * not per frame.
 */
static int __cdecl detour_can_fp(void *item)
{
    if (g_fp_melee) return 1;
    return g_orig_can_fp ? g_orig_can_fp(item) : 1;
}

/*
 * The skill-start kick is a conditional jump rather than a call, so there is
 * nothing to detour -- one byte flips it to unconditional. Written only
 * after the original has been checked against what the disassembly says is
 * there, because a wild write into .text is the one mistake here that takes
 * the process with it.
 */
static int kick_patch(int on)
{
    DWORD old;
    unsigned char want = on ? SKILL_FP_KICK_JMP : g_kick_orig;

    if (!g_kick_site) return 0;
    if (g_kick_orig != SKILL_FP_KICK_JNE) return 0;
    if (*g_kick_site == want) return 1;
    if (!VirtualProtect(g_kick_site, 1, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("fp: VirtualProtect failed at %p (%lu)", (void *)g_kick_site,
              GetLastError());
        return 0;
    }
    *g_kick_site = want;
    VirtualProtect(g_kick_site, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_kick_site, 1);
    return 1;
}

int hg_fp_melee_available(void) { return g_orig_can_fp != NULL; }
int hg_get_fp_melee(void)       { return (int)g_fp_melee; }

void hg_set_fp_melee(int on)
{
    if (!g_orig_can_fp) {
        logf_("fp: unlock unavailable — CanUseFirstPerson was not hooked");
        return;
    }
    InterlockedExchange(&g_fp_melee, on ? 1 : 0);
    if (!kick_patch(on))
        logf_("fp: skill-start kick NOT patched (byte at %p was 0x%02x, "
              "expected 0x%02x) — swinging may still drop you to third",
              (void *)g_kick_site, g_kick_orig, SKILL_FP_KICK_JNE);
    logf_("fp: melee first-person unlock %s%s", on ? "ON" : "off",
          on ? " — the weapon gate and the skill-start kick are both bypassed"
             : "");
}

int hg_camera_mode(void)
{
    return g_cam_mode ? *g_cam_mode : -1;
}

/* ------------------------------------------------------------------ */
/* viewmodel                                                            */

/*
 * c_UnitGetModelIdThirdPerson takes its argument in EAX and returns in EAX
 * with no stack arguments -- a whole-program-optimisation convention, not
 * anything GCC has an attribute for. Same trap as 0x4213c2 in target.h:
 * declaring it as a normal function would pass the unit on the stack and
 * read a register that happens to hold something else.
 */
static int model_third_id(void *unit)
{
    int ret;
    if (!g_fn_model_third || !unit) return -1;
    __asm__ __volatile__("call *%[fn]"
                         : "=a"(ret)
                         : "a"(unit), [fn] "r"(g_fn_model_third)
                         : "ecx", "edx", "cc", "memory");
    return ret;
}

/*
 * e_ModelSetFlagbit(ecx = model, bit, value), both stack arguments cleaned
 * by the caller -- the `pop ecx; pop ecx` after every call site in the
 * game. Pushed value-first so that [esp] is the bit, matching 0x004d31f7.
 */
static int model_set_flagbit(int model, int bit, int value)
{
    int ret;
    if (!g_fn_model_flag || model < 0) return -1;
    __asm__ __volatile__("push %[v]\n\t"
                         "push %[b]\n\t"
                         "call *%[fn]\n\t"
                         "add $8, %%esp"
                         : "=a"(ret)
                         : [fn] "r"(g_fn_model_flag), "c"(model),
                           [b] "r"(bit), [v] "r"(value)
                         : "edx", "cc", "memory");
    return ret;
}

/*
 * Reports the whole chain, because "-1" on its own is three different bugs
 * wearing the same hat: no unit, no pGfx, or no model on the pGfx.
 *   unit        -> the local player
 *   unit+0x160  -> pGfx        (offset cross-checked at 0x004b92ca)
 *   pGfx+0x10   -> model id    (pGfx+0x08 is the paperdoll model)
 */
void hg_model_chain(unsigned int *unit, unsigned int *gfx, int *id)
{
    unsigned char *u = NULL;

    *unit = 0; *gfx = 0; *id = -1;
    if (!g_get_player_local) return;
    u = (unsigned char *)(unsigned int)g_get_player_local();
    *unit = (unsigned int)(unsigned long)u;
    if (!u) return;
    if (readable_code(u + 0x160, 4))
        *gfx = *(const unsigned int *)(u + 0x160);
    *id = model_third_id(u);
    if (*id < 0) {
        /* The getter's unit has no pGfx: try the client game's control
         * unit, which the camera code already tracks. */
        unsigned char *g = (unsigned char *)hg_client_game();
        if (g && readable_code(g + GAME_CONTROL_UNIT, 4)) {
            unsigned char *cu = *(unsigned char **)(g + GAME_CONTROL_UNIT);
            if (cu && readable_code(cu + 0x160, 4)) {
                *unit = (unsigned int)(unsigned long)cu;
                *gfx = *(const unsigned int *)(cu + 0x160);
                *id = model_third_id(cu);
            }
        }
    }
}

/*
 * First-person animations blend (src/fpview.c, fp.anim_ease). The engine
 * starts an animation through FUN_006122a8 (cdecl: unit, model id, the
 * ANIMATION_DEFINITION, ...), and a definition's fEaseIn and fEaseOut
 * (+0x134, +0x138, seconds; read by the debug dump at 0x611a76) set how
 * long it blends in and out. The first-person ones are short or zero, so
 * the arms snapped from one pose to the next (2026-09-25). When the model
 * is your first-person one (pGfx[0] in first person) and a definition eases
 * for less than the minimum, the minimum is written into it: the
 * first-person appearance has its own definitions, so nothing else
 * changes, and each is raised once.
 */
typedef int (__cdecl *anim_play_fn)(void *, int, unsigned char *, void *, unsigned int, void *, void *);
static anim_play_fn g_orig_anim_play;
static LONG g_anim_eased;
LONG fpview_anim_ease_ms(void);

static int __cdecl detour_anim_play(void *unit, int model, unsigned char *def, void *a4, unsigned int flags,
                                    void *a6, void *a7)
{
    LONG ms = fpview_anim_ease_ms();
    if (ms > 0 && def && model >= 0 && *(const int *)(g_image + RVA_CAMERA_MODE_CUR) == 0) {
        unsigned int u, gfx;
        int third;
        hg_model_chain(&u, &gfx, &third);
        if (gfx && readable_code((void *)(unsigned long)gfx, 4) && *(const int *)(unsigned long)gfx == model &&
            readable_code(def + 0x134, 8)) {
            float lo = ms / 1000.0f, *ein = (float *)(def + 0x134), *eout = (float *)(def + 0x138);
            if (*ein < lo || *eout < lo) {
                if (InterlockedIncrement(&g_anim_eased) <= 20)
                    logf_("fpview: first-person animation %.40s eased: in %.3f -> %.3f s, out %.3f -> %.3f s",
                          (const char *)(def + 0xc), *ein, *ein < lo ? lo : *ein, *eout, *eout < lo ? lo : *eout);
                if (*ein < lo) *ein = lo;
                if (*eout < lo) *eout = lo;
            }
        }
    }
    return g_orig_anim_play(unit, model, def, a4, flags, a6, a7);
}

/*
 * What the player holds (src/fpview.c picks the view model's pose by it):
 * the items in the weapon slots, 7 and 8, as SetCameraMode asks for them
 * (0x4dc0ba: FUN_0062b74d, unit in eax, the slot pushed, caller-clean,
 * the item back in eax), and whether one is a two-handed gun: a
 * two-handed weapon fills slot 7 alone, like a pistol (2026-09-25).
 * 0x45a6bd is UnitIsA (item in eax, the unit type pushed; the script's
 * getWieldingIsACount asks it, and so does CanUseFirstPerson, for melee
 * 44 and shield 41); gun2h is unittypes row 49 (hunter_gun2h, the
 * snipers, beams, missiles and fields all are one).
 * 2: two items (dual wield, as isDualWielding counts them); 1: one
 * one-handed item in the right hand (slot 7); 3: one in the left hand
 * alone (slot 8); 0: a two-handed gun, or nothing.
 */
#define UNITTYPE_GUN2H 49

static void *slot_item(unsigned char *unit, int slot)
{
    void *it;
    __asm__ __volatile__("pushl %[s]\n\t"
                         "call *%[fn]\n\t"
                         "addl $4, %%esp"
                         : "=a"(it)
                         : [fn] "r"(g_image + 0x0022B74Du), "a"(unit), [s] "r"(slot)
                         : "ecx", "edx", "cc", "memory");
    return it;
}

static int unit_is_a(void *item, int type)
{
    int r;
    __asm__ __volatile__("pushl %[t]\n\t"
                         "call *%[fn]\n\t"
                         "addl $4, %%esp"
                         : "=a"(r)
                         : [fn] "r"(g_image + 0x0005A6BDu), "a"(item), [t] "r"(type)
                         : "ecx", "edx", "cc", "memory");
    return r;
}

int hg_player_hands(void)
{
    static void *last_a = (void *)1, *last_b = (void *)1;
    unsigned int unit, gfx;
    int id, n, two;
    void *a, *b;
    hg_model_chain(&unit, &gfx, &id);
    if (!unit || !readable_code((unsigned char *)(unsigned long)unit + 0x144, 4)) return 0;
    a = slot_item((unsigned char *)(unsigned long)unit, 7);
    b = slot_item((unsigned char *)(unsigned long)unit, 8);
    two = (a && unit_is_a(a, UNITTYPE_GUN2H)) || (b && unit_is_a(b, UNITTYPE_GUN2H));
    n = two ? 0 : a && b && a != b ? 2 : a ? 1 : b ? 3 : 0;
    if (a != last_a || b != last_b)
        logf_("fpview: weapon slots 7 %p, 8 %p: %s", a, b,
              two ? "a two-handed gun" : n == 2 ? "two items (dual wield)" : n == 1 ? "one item, right hand" :
              n == 3 ? "one item, left hand (the pose mirrored)" : "empty");
    last_a = a; last_b = b;
    return n;
}

/*
 * Does the player have a state (states.txt row)? UnitHasState, 0x5252fc
 * (cdecl: game, unit, state), as the script's hasState asks it; the game
 * is the unit's first field. Sprint is a skill that puts a state on you:
 * swiftness_boost, row 136 (src/fpview.c, the sprint's wider view).
 */
int hg_player_has_state(int state)
{
    typedef int (__cdecl *has_state_fn)(void *, void *, int);
    unsigned int unit, gfx;
    int id;
    void *game;
    hg_model_chain(&unit, &gfx, &id);
    if (!unit || !readable_code((unsigned char *)(unsigned long)unit, 4)) return 0;
    game = *(void **)(unsigned long)unit;
    if (!game) return 0;
    return ((has_state_fn)(g_image + 0x001252FCu))(game, (void *)(unsigned long)unit, state) != 0;
}

/*
 * Is the player in a safe level (a station, a town: no weapons)? The
 * automap asks the same (0x5bc551, e_AutomapSetShowAll's bIsSafe): the
 * level's definition (FUN_004e6093, level in eax, the LEVEL row in eax) has
 * +0xa8 or +0xc4 set. The level is unit -> room +0x2c -> level +0x130.
 */
int hg_player_safe_level(void)
{
    static void *last_level;
    static int last_safe;
    unsigned int unit, gfx;
    int id;
    unsigned char *u, *room, *level, *def;
    int safe;
    hg_model_chain(&unit, &gfx, &id);
    u = (unsigned char *)(unsigned long)unit;
    if (!u || !readable_code(u + UNIT_ROOM, 4)) return 0;
    room = *(unsigned char **)(u + UNIT_ROOM);
    if (!room || !readable_code(room + ROOM_LEVEL, 4)) return 0;
    level = *(unsigned char **)(room + ROOM_LEVEL);
    if (!level || !readable_code(level + 0x16f8, 4)) return 0;
    /* asked every time, not cached by the level's address: a new level
     * allocated where the station was kept its answer, and the weapon stayed
     * lowered after loading (2026-09-25) */
    __asm__ __volatile__("call *%[fn]"
                         : "=a"(def)
                         : [fn] "r"(g_image + 0x000E6093u), "a"(level)
                         : "ecx", "edx", "cc", "memory");
    safe = def && readable_code(def + 0xa8, 0x20) && (*(int *)(def + 0xa8) || *(int *)(def + 0xc4));
    if (level != last_level || safe != last_safe)
        logf_("fpview: level %p is %s", (void *)level, safe ? "safe (a station or town: the lowered weapon)" : "not safe");
    last_level = level;
    last_safe = safe;
    return safe;
}

/*
 * Player shadow. MODEL_FLAGBIT_NOSHADOW (4) keeps a model out of the shadow
 * map; the panel toggle clears it on the player's model, and again whenever
 * the model id changes (respawn, zone). Runs from the game-thread pump.
 */
static volatile LONG g_shadow_on;
static int g_shadow_applied = -1;
void hg_shadow_set(int on)
{
    InterlockedExchange(&g_shadow_on, on ? 1 : 0);
    g_shadow_applied = -1;
    logf_("model: player shadow %s (shadow pass calls so far: total %ld, player %ld)", on ? "ON (clearing NOSHADOW on the player's model)" : "off",
          hg_shadow_calls(), hg_shadow_player_calls());
}
int hg_shadow_get(void) { return (int)g_shadow_on; }

int hg_model_third(void)
{
    unsigned int unit, gfx;
    int id;
    hg_model_chain(&unit, &gfx, &id);
    return id;
}

void hg_model_flag_request(int bit, int value)
{
    if (bit < 0 || bit > 63) return;
    InterlockedExchange(&g_model_req, (LONG)(0x10000 | (bit << 8) | (value & 1)));
}

long hg_model_pump(void)
{
    LONG r = InterlockedExchange(&g_model_req, -1);
    int bit, val, id, rc;

    if (g_shadow_on && g_fn_model_flag) {
        unsigned int unit, gfx;
        int cur;
        hg_model_chain(&unit, &gfx, &cur);
        if (cur >= 0 && cur != g_shadow_applied) {
            rc = model_set_flagbit(cur, MODEL_FLAGBIT_NOSHADOW, 0);
            logf_("model: player model %d NOSHADOW := 0 (rc %d)", cur, rc);
            g_shadow_applied = cur;
        }
    }
    if (r < 0 || !(r & 0x10000)) return 0;
    bit = (int)((r >> 8) & 0xff);
    val = (int)(r & 1);
    {
        unsigned int unit, gfx;
        hg_model_chain(&unit, &gfx, &id);
        if (id < 0) {
            logf_("model: no model id — unit=0x%08x pGfx(unit+0x160)=0x%08x "
                  "(%s)", unit, gfx,
                  !unit ? "no local player unit" :
                  !gfx  ? "unit has no pGfx — graphics not attached?"
                        : "pGfx+0x10 held -1");
            return 0;
        }
    }
    rc = model_set_flagbit(id, bit, val);
    logf_("model: third-person model %d, flagbit %d := %d (rc %d)%s",
          id, bit, val, rc,
          bit == MODEL_FLAGBIT_FP_PROJ
              ? "  [FIRST_PERSON_PROJ]" : "");
    return 1;
}

void hg_camera_request(int mode)
{
    if (!g_set_camera) {
        logf_("camera: setter not resolved");
        return;
    }
    InterlockedExchange(&g_cam_req, (LONG)mode);
}

/*
 * Drained on the game thread, like everything else that touches the world.
 * The overlay runs on the render thread and asks from there.
 */
long hg_camera_pump(void)
{
    LONG m = InterlockedExchange(&g_cam_req, -1);

    if (m == -1) return 0;
    if (m == HG_CAM_RESTORE) {
        if (!g_restore_camera) return 0;
        g_restore_camera();
        logf_("camera: restored (now mode %d)", hg_camera_mode());
        return 1;
    }
    if (m < 0 || !g_set_camera) return 0;
    g_set_camera((int)m, 1);
    logf_("camera: requested mode %ld, now %d%s", (long)m, hg_camera_mode(),
          (m == CAM_FIRST_PERSON && hg_camera_mode() == CAM_THIRD_PERSON)
              ? " — overridden to third; your weapon forbids first person"
              : "");
    return 1;
}

void hg_counters_reset(void)
{
    thread_block *tb;

    for (tb = g_blocks; tb; tb = tb->next) {
        tb->qcalls = 0; tb->qticks = 0; tb->gcalls = 0; tb->rcalls = 0;
        tb->nsteps = 0; tb->dt_sum = 0.0; tb->dt_min = 0.0f; tb->dt_max = 0.0f;
        tb->nqsites = 0; tb->nsites = 0; tb->nrsites = 0;
    }
    InterlockedExchange(&g_bodies_added, 0);
    InterlockedExchange(&g_bodies_removed, 0);
    InterlockedExchange(&g_fired, 0);
    InterlockedExchange(&g_spawn_extra_total, 0);
    InterlockedExchange(&g_seen_prim, 0);
    InterlockedExchange(&g_seen_script, 0);
    logf_("panel: counters reset (body count left alone — it tracks the "
          "live world, not a window)");
}

/* ------------------------------------------------------------------ */
/* address space                                                        */

/*
 * H6: this is a 32-bit process, so it has a hard address-space ceiling.
 * Old D3D9 keeps system-memory shadow copies of managed-pool resources and
 * is heavy on that budget; DXVK is much lighter, which is the standard
 * reason a 32-bit game stops running out of memory when you drop DXVK in.
 * If pressure and fragmentation are what make MOPP walks expensive, then
 * free space should fall and `qavg` should rise together.
 *
 * `largest` matters more than `free`: an allocator with 400MB free in 4MB
 * shards is in far worse shape than one with 400MB in a single block, and
 * that difference is exactly what scatters Havok's structures.
 */
/*
 * Who owns the address space: the VA walk grouped by allocation, totalled
 * by type (image / mapped / private) and state, with the largest
 * allocations and, for images, their module. Logged automatically when
 * free space first falls below 400 MB and again below 250 MB, or when
 * bin\hellgate_mem.dump exists. Aimed at the ~2.4 GB "reserve" seen in
 * sessions near the 4 GB ceiling (DXVK? the Vulkan driver? game heaps?).
 */
#define MB_TOP 24
static void mem_breakdown(const char *why)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL, *cur_base = NULL;
    unsigned long long tot[3][2] = {{0}};       /* [image, mapped, private][commit, reserve] */
    struct { void *base; unsigned long long size, commit; DWORD type; } top[MB_TOP], a = {0};
    int ntop = 0, i, j;
    unsigned int regions = 0;

    memset(top, 0, sizeof top);
    for (;;) {
        int ok = VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi;
        unsigned char *next = ok ? (unsigned char *)mbi.BaseAddress + mbi.RegionSize : NULL;
        /* close the current allocation when a new one starts (or at the end) */
        if (!ok || mbi.State == MEM_FREE || mbi.AllocationBase != cur_base) {
            if (a.base && a.size) {
                for (i = 0; i < ntop && top[i].size >= a.size; i++) ;
                if (i < MB_TOP) {
                    for (j = (ntop < MB_TOP ? ntop : MB_TOP - 1); j > i; j--) top[j] = top[j - 1];
                    top[i].base = a.base; top[i].size = a.size; top[i].commit = a.commit; top[i].type = a.type;
                    if (ntop < MB_TOP) ntop++;
                }
            }
            a.base = NULL; a.size = a.commit = 0;
            cur_base = NULL;
        }
        if (!ok) break;
        regions++;
        if (mbi.State != MEM_FREE) {
            int t = mbi.Type == MEM_IMAGE ? 0 : mbi.Type == MEM_MAPPED ? 1 : 2;
            tot[t][mbi.State == MEM_COMMIT ? 0 : 1] += mbi.RegionSize;
            if (!a.base) { a.base = mbi.AllocationBase; a.type = mbi.Type; cur_base = mbi.AllocationBase; }
            a.size += mbi.RegionSize;
            if (mbi.State == MEM_COMMIT) a.commit += mbi.RegionSize;
        }
        if (next <= addr || regions > 200000) break;
        addr = next;
    }
    logf_("MEM breakdown (%s): image commit %lluMB reserve %lluMB | mapped commit %lluMB reserve %lluMB | private commit %lluMB reserve %lluMB",
          why, tot[0][0] >> 20, tot[0][1] >> 20, tot[1][0] >> 20, tot[1][1] >> 20, tot[2][0] >> 20, tot[2][1] >> 20);
    for (i = 0; i < ntop; i++) {
        char mod[MAX_PATH] = "";
        if (top[i].type == MEM_IMAGE) {
            char *slash;
            GetModuleFileNameA((HMODULE)top[i].base, mod, sizeof mod);
            slash = strrchr(mod, '\\');
            if (slash) memmove(mod, slash + 1, strlen(slash));
        }
        logf_("MEM  %p %6lluMB (commit %6lluMB) %s %s", top[i].base, top[i].size >> 20, top[i].commit >> 20,
              top[i].type == MEM_IMAGE ? "image" : top[i].type == MEM_MAPPED ? "mapped" : "private", mod);
    }
}

static void mem_report(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    unsigned long long freetot = 0, commit = 0, reserve = 0;
    unsigned long long largest = 0;
    unsigned int regions = 0, freeregions = 0;
    PROCESS_MEMORY_COUNTERS_EX pmc;

    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        regions++;
        if (mbi.State == MEM_FREE) {
            freetot += mbi.RegionSize;
            freeregions++;
            if (mbi.RegionSize > largest) largest = mbi.RegionSize;
        } else if (mbi.State == MEM_COMMIT) {
            commit += mbi.RegionSize;
        } else {
            reserve += mbi.RegionSize;
        }
        if (next <= addr) break;          /* wrapped at the top of the range */
        addr = next;
        if (regions > 200000) break;      /* paranoia */
    }

    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(),
                         (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);

    /*
     * Wine does not populate PrivateUsage — it comes back 0 — so the VA walk
     * above is the number to trust, not the psapi counters. Logged anyway in
     * case this is ever run on Windows, but do not draw conclusions from a
     * private= column of zeroes.
     */
    {
        static int stage;
        static WCHAR flag[MAX_PATH];
        if (!flag[0]) { hg_dll_dir(flag, MAX_PATH); lstrcatW(flag, L"\\hellgate_mem.dump"); }
        if (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(flag); mem_breakdown("requested"); }
        else if (stage == 0 && (freetot >> 20) < 400) { stage = 1; mem_breakdown("free < 400MB"); }
        else if (stage == 1 && (freetot >> 20) < 250) { stage = 2; mem_breakdown("free < 250MB"); }
    }
    /* reserve= includes Wine's pool of not-yet-handed-out address space
     * (the whole top 2 GB of this large-address-aware process reads as one
     * reservation, yet allocations land there: probed 2026-09-22), so
     * free= understates what the game can still get under Wine */
    logf_("M private=%lluMB ws=%lluMB commit=%lluMB reserve=%lluMB "
          "free=%lluMB largestfree=%lluMB regions=%u freeregions=%u",
          (unsigned long long)pmc.PrivateUsage >> 20,
          (unsigned long long)pmc.WorkingSetSize >> 20,
          commit >> 20, reserve >> 20, freetot >> 20, largest >> 20,
          regions, freeregions);
}

/* The memory line on demand (src/crashlog.c logs it as the process ends). */
void hg_mem_report(void) { mem_report(); }

/* ------------------------------------------------------------------ */
/* sampler                                                             */

static void fmt_frames(char *out, int cap, const unsigned int *frames)
{
    int n = 0;
    unsigned int i;
    for (i = 0; i < g_stack_depth && n < cap - 16; i++) {
        unsigned int a = frames[i];
        if (!a) break;
        if (a >= g_image && a < g_image + 0x00e82000u)
            n += snprintf(out + n, cap - n, "%s%08x", i ? "<" : "", a - g_image);
        else
            n += snprintf(out + n, cap - n, "%s!%08x", i ? "<" : "", a);
    }
    out[n] = 0;
}

static void report_window(void)
{
    thread_block *tb;
    unsigned long long qticks = 0;
    unsigned int qcalls = 0, gcalls = 0, nsteps = 0, rcalls = 0;
    float dt_max = 0.0f, dt_min = 1e30f;
    double dt_sum = 0.0, qms;

    g_window++;
    if ((g_window % HG_MEM_EVERY) == 0) mem_report();

    for (tb = g_blocks; tb; tb = tb->next) {
        qcalls += tb->qcalls;
        qticks += tb->qticks;
        gcalls += tb->gcalls;
        rcalls += tb->rcalls;
    }
    qms = (double)qticks * 1000.0 / (double)g_qpf.QuadPart;

    for (tb = g_blocks; tb; tb = tb->next) {
        if (!tb->nsteps) continue;
        nsteps += tb->nsteps;
        dt_sum += tb->dt_sum;
        if (dt_max < tb->dt_max) dt_max = tb->dt_max;
        if (dt_min > tb->dt_min) dt_min = tb->dt_min;
    }

    {
        LONG prim  = InterlockedExchange(&g_seen_prim_win, 0);
        LONG scr   = InterlockedExchange(&g_seen_script_win, 0);
        LONG extra = InterlockedExchange(&g_spawn_extra, 0);
        LONG fired = InterlockedExchange(&g_fired_win, 0);
        InterlockedExchange(&g_spawn_budget, 0);
        if (prim || scr || extra || fired)
            logf_("X spawns prim=%ld script=%ld amplified=%ld panel=%ld "
                  "owed=%ld mult=%u", prim, scr, extra, fired,
                  (long)g_fire_queue, g_spawn_mult);
    }

    if (qcalls || gcalls || nsteps || rcalls) {
        logf_("W %llu qray=%u qms=%.3f qavg=%.2fus rays=%u nodes/ray=%.1f "
              "bodies=%ld +%ld/-%ld grays=%u steps=%u dt=[%.1f/%.1f/%.1f]ms%s",
              g_window, qcalls, qms,
              qcalls ? qms * 1000.0 / qcalls : 0.0,
              rcalls, rcalls ? (double)qcalls / rcalls : 0.0,
              g_bodies_live,
              InterlockedExchange(&g_bodies_added, 0),
              InterlockedExchange(&g_bodies_removed, 0),
              gcalls, nsteps,
              nsteps ? dt_min * 1000.0f : 0.0f,
              nsteps ? (float)(dt_sum / nsteps) * 1000.0f : 0.0f,
              nsteps ? dt_max * 1000.0f : 0.0f,
              (qms > HG_SPIKE_QMS || qcalls > HG_SPIKE_QRAY ||
               dt_max > HG_SPIKE_DT) ? " SPIKE" : "");
    }

    panel_publish(qcalls, rcalls, g_bodies_live, qms,
                  nsteps ? dt_min : 0.0f,
                  nsteps ? (float)(dt_sum / nsteps) : 0.0f,
                  nsteps ? dt_max : 0.0f);

    /* H7: floating-point state on the physics thread. */
    for (tb = g_blocks; tb; tb = tb->next) {
        if (!tb->fp_samples) continue;
        logf_("  F tid=%lu sampled=%u denorm=%u (%.1f%%) mxcsr=%04x "
              "FTZ=%d DAZ=%d x87cw=%04x pc=%s",
              tb->tid, tb->fp_samples, tb->fp_denorm,
              100.0 * tb->fp_denorm / tb->fp_samples,
              tb->fp_mxcsr,
              (tb->fp_mxcsr & MXCSR_FTZ) ? 1 : 0,
              (tb->fp_mxcsr & MXCSR_DAZ) ? 1 : 0,
              tb->fp_cw,
              ((tb->fp_cw >> 8) & 3) == 0 ? "single(24)" :
              ((tb->fp_cw >> 8) & 3) == 2 ? "double(53)" :
              ((tb->fp_cw >> 8) & 3) == 3 ? "extended(64)" : "reserved");
    }

    /* Ray-level attribution: who actually asks for a MOPP raycast. */
    for (tb = g_blocks; tb; tb = tb->next) {
        unsigned int i;
        for (i = 0; i < tb->nrsites; i++) {
            site *s = &tb->rsites[i];
            char fr[1024];
            if (!s->count) continue;
            fmt_frames(fr, sizeof fr, s->frames);
            logf_("  R tid=%lu n=%u ms=%.3f site=%s", tb->tid, s->count,
                  (double)s->ticks * 1000.0 / (double)g_qpf.QuadPart, fr);
        }
    }

    /* Sampled attribution for queryRayOnTree itself. */
    for (tb = g_blocks; tb; tb = tb->next) {
        unsigned int i;
        for (i = 0; i < tb->nqsites; i++) {
            site *s = &tb->qsites[i];
            char fr[1024];
            if (!s->count) continue;
            fmt_frames(fr, sizeof fr, s->frames);
            logf_("  Q tid=%lu sampled=%u est=%u ms=%.3f site=%s",
                  tb->tid, s->count, s->count * HG_QSAMPLE,
                  (double)s->ticks * 1000.0 / (double)g_qpf.QuadPart, fr);
        }
    }

    for (tb = g_blocks; tb; tb = tb->next) {
        unsigned int i;
        for (i = 0; i < tb->nsites; i++) {
            site *s = &tb->sites[i];
            char fr[512];
            if (!s->count) continue;
            fmt_frames(fr, sizeof fr, s->frames);
            logf_("  S tid=%lu n=%u nan=%u huge=%u zero=%u len=[%.1f/%.1f/%.1f] "
                  "scalarmax=%.4g site=%s",
                  tb->tid, s->count, s->nan, s->huge, s->degenerate,
                  s->len_min > 1e29f ? 0.0f : s->len_min,
                  s->count > s->nan ? (float)(s->len_sum / (s->count - s->nan)) : 0.0f,
                  s->len_max < -1e29f ? 0.0f : s->len_max,
                  s->scalar_max, fr);

            /* Print the offending ray verbatim for anything suspicious. */
            if (s->worst_valid && (s->nan || s->huge)) {
                logf_("    ! worst %s origin=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f) "
                      "length=%.6g raylen=%.6g",
                      s->worst_bad ? "NON-FINITE" : "long",
                      s->worst_origin[0], s->worst_origin[1], s->worst_origin[2],
                      s->worst_dir[0], s->worst_dir[1], s->worst_dir[2],
                      s->worst_length, s->worst_len);
            }
        }
    }

    for (tb = g_blocks; tb; tb = tb->next) {
        tb->qcalls = 0;
        tb->qticks = 0;
        tb->gcalls = 0;
        tb->nsteps = 0;
        tb->dt_sum = 0.0;
        tb->dt_min = 0.0f;
        tb->dt_max = 0.0f;
        tb->nsites = 0;
        tb->nqsites = 0;
        tb->rcalls = 0;
        tb->nrsites = 0;
        memset(tb->rsites, 0, sizeof tb->rsites);
        tb->fp_samples = 0;
        tb->fp_denorm = 0;
        memset(tb->sites, 0, sizeof tb->sites);
        memset(tb->qsites, 0, sizeof tb->qsites);
    }
}

/* ------------------------------------------------------------------ */
/* startup                                                             */

static int verify_exe(char *got, unsigned int *size_out)
{
    WCHAR path[MAX_PATH];
    HANDLE f;
    sha256_ctx c;
    unsigned char digest[32], buf[65536];
    DWORD rd;
    int i;
    LARGE_INTEGER sz;

    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    GetFileSizeEx(f, &sz);
    *size_out = (unsigned int)sz.QuadPart;
    sha256_init(&c);
    while (ReadFile(f, buf, sizeof buf, &rd, NULL) && rd)
        sha256_update(&c, buf, rd);
    CloseHandle(f);
    sha256_final(&c, digest);
    for (i = 0; i < 32; i++) sprintf(got + i*2, "%02x", digest[i]);
    got[64] = 0;
    return 1;
}

/*
 * True if `name` exists next to the DLL. Mirrors the kill-switch below, and
 * exists because Steam launch options are an awkward place to put a flag you
 * want to flip between runs -- and an easy one to forget, which is how the
 * panel shipped disabled.
 */
/* The directory the DLL sits in, for anything that loads files next to it. */
void hg_dll_dir(WCHAR *out, int cap)
{
    lstrcpynW(out, g_dll_dir, cap);
}

int hg_flagfile(const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    lstrcpyW(p, g_dll_dir);
    lstrcatW(p, L"\\");
    lstrcatW(p, name);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* Create (on) or delete (off) an empty flag file next to the DLL: settings
 * that only apply at the next start. */
void hg_set_flagfile(const WCHAR *name, int on)
{
    WCHAR p[MAX_PATH];
    lstrcpyW(p, g_dll_dir);
    lstrcatW(p, L"\\");
    lstrcatW(p, name);
    if (on) {
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    } else {
        DeleteFileW(p);
    }
}

static int disabled(void)
{
    WCHAR p[MAX_PATH];
    if (GetEnvironmentVariableW(L"HG_RAYS_DISABLE", p, MAX_PATH) > 0 && p[0] != L'0')
        return 1;
    lstrcpyW(p, g_dll_dir);
    lstrcatW(p, L"\\hellgate_rays.off");
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/*
 * True if the file contains "SPIKE" anywhere. Chunked, with the overlap a
 * match split across two reads needs.
 */
static int log_has_spike(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    static char buf[1 << 16];
    DWORD got, keep = 0, i;
    int found = 0;

    if (h == INVALID_HANDLE_VALUE) return 0;
    while (!found && ReadFile(h, buf + keep, sizeof buf - keep, &got, NULL) && got) {
        DWORD n = keep + got;
        for (i = 0; i + 5 <= n; i++)
            if (buf[i] == 'S' && !memcmp(buf + i, "SPIKE", 5)) { found = 1; break; }
        keep = n < 4 ? n : 4;
        memmove(buf, buf + n - keep, keep);
    }
    CloseHandle(h);
    return found;
}

/*
 * Did the previous session end through src/crashlog.c's exit hooks? A log
 * from a build with them ("each address once" in its crash line) that has
 * no "exit:" line ended in a native crash, a kill or a hang. The tail is
 * kept in g_prev_tail for the new log's first lines.
 */
static char g_prev_tail[1536];
static int log_ended_badly(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    static char head[4096], tail[4096];
    DWORD got = 0, size;
    int armed, exited;
    char *t;

    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, head, sizeof head - 1, &got, NULL);
    head[got] = 0;
    armed = strstr(head, "each address once") != NULL;
    size = GetFileSize(h, NULL);
    SetFilePointer(h, size > sizeof tail - 1 ? (LONG)(size - (sizeof tail - 1)) : 0, NULL, FILE_BEGIN);
    got = 0;
    ReadFile(h, tail, sizeof tail - 1, &got, NULL);
    tail[got] = 0;
    CloseHandle(h);
    exited = strstr(tail, "\nexit: ") != NULL;
    if (!armed || exited) return 0;
    /* the last lines, as many as fit */
    t = tail + got;
    while (t > tail && (tail + got) - t < (int)sizeof g_prev_tail - 1) t--;
    while (*t && *t != '\n') t++;
    lstrcpynA(g_prev_tail, *t ? t + 1 : t, sizeof g_prev_tail);
    return 1;
}

/*
 * The previous session's log is never simply overwritten. It becomes
 * hellgate_rays.log.1, unless it caught a stall or ended without an exit
 * (a crash), in which case it is kept for good under a timestamped name.
 * The first ever capture of the real 1 FPS bug was lost to CREATE_ALWAYS on
 * the next launch; this is why.
 */
static WCHAR g_prev_crash[MAX_PATH];
static void keep_previous_log(const WCHAR *p)
{
    WCHAR dst[MAX_PATH];
    SYSTEMTIME st;
    int crashed;

    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return;
    crashed = log_ended_badly(p);
    if (crashed || log_has_spike(p)) {
        GetLocalTime(&st);
        lstrcpyW(dst, g_dll_dir);
        wsprintfW(dst + lstrlenW(dst), L"\\hellgate_rays.%s-%04u%02u%02u-%02u%02u%02u.log",
                  crashed ? L"crash" : L"spike",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        if (crashed) lstrcpyW(g_prev_crash, dst);
    } else {
        lstrcpyW(dst, p);
        lstrcatW(dst, L".1");
    }
    MoveFileExW(p, dst, MOVEFILE_REPLACE_EXISTING);
}

static void open_log(void)
{
    WCHAR p[MAX_PATH];
    if (GetEnvironmentVariableW(L"HG_RAYS_LOG", p, MAX_PATH) == 0) {
        lstrcpyW(p, g_dll_dir);
        lstrcatW(p, L"\\hellgate_rays.log");
    }
    keep_previous_log(p);
    g_log = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_prev_crash[0]) {
        logf_("previous session: ENDED WITHOUT AN EXIT (native crash, kill or hang); kept as %ls; its last lines:",
              g_prev_crash);
        logf_("%s", g_prev_tail);
        logf_("previous session: end of its last lines");
    }
}

/*
 * Check that the bytes at an address are the ones the disassembly showed
 * before anything relies on them.
 *
 * Every address in target.h was recovered statically and, until now, used
 * on faith: if one was wrong the result was a silent no-op or a wild call,
 * indistinguishable from "the feature did nothing". That is exactly how
 * three separate changes shipped looking broken. A signature check costs
 * nothing at load and turns a wrong address into a named, logged failure.
 */
static int verify_bytes(unsigned int rva, const unsigned char *want, int n,
                        const char *name)
{
    const unsigned char *p = (const unsigned char *)(g_image + rva);
    char got[64], exp[64];
    int i, ok;

    if (!readable_code(p, (unsigned int)n)) {
        logf_("verify: %-28s UNREADABLE at %p (rva 0x%08x)", name,
              (const void *)p, rva);
        return 0;
    }
    ok = (memcmp(p, want, (size_t)n) == 0);
    if (ok) return 1;

    got[0] = exp[0] = 0;
    for (i = 0; i < n && i < 12; i++) {
        snprintf(got + i * 3, 4, "%02x ", p[i]);
        snprintf(exp + i * 3, 4, "%02x ", want[i]);
    }
    logf_("verify: %-28s MISMATCH at %p (rva 0x%08x)", name,
          (const void *)p, rva);
    logf_("verify:   expected %s", exp);
    logf_("verify:   found    %s", got);
    return 0;
}

static int hook_one(unsigned int rva, void *detour, void **orig, const char *name)
{
    void *target = (void *)(g_image + rva);
    if (MH_CreateHook(target, detour, orig) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        logf_("hellgate-rays: FAILED to hook %s at %p (rva 0x%08x)", name, target, rva);
        return 0;
    }
    logf_("hellgate-rays: hooked %s at %p (rva 0x%08x)", name, target, rva);
    return 1;
}


static DWORD WINAPI worker(LPVOID unused)
{
    char got[72];
    unsigned int size = 0;
    WCHAR envbuf[32];

    (void)unused;
    open_log();
    QueryPerformanceFrequency(&g_qpf);
    g_image = (unsigned int)GetModuleHandleW(NULL);

    logf_("hellgate-rays: image base 0x%08x qpf=%lld", g_image, (long long)g_qpf.QuadPart);

    /*
     * Self-test: exercise the address-space walk on any host, without
     * hooking anything. The hash guard means the normal offline test never
     * reaches the sampler, so without this the memory code would ship
     * having never once been run.
     */
    if (GetEnvironmentVariableW(L"HG_RAYS_SELFTEST", envbuf, 32) > 0) {
        logf_("hellgate-rays: selftest — address space walk:");
        mem_report();
    }

    /*
     * Same reasoning as the walk above: the hash guard means the panel's
     * socket and page-serving code would otherwise never run on any host we
     * can actually test on. With the hooks absent nothing pumps the command
     * queue, so game-thread ops correctly time out instead of pretending.
     */
    if (GetEnvironmentVariableW(L"HG_PANEL_SELFTEST", envbuf, 32) > 0) {
        logf_("hellgate-rays: panel selftest — serving without hooks");
        panel_start(g_image);
    }

    if (disabled()) {
        logf_("hellgate-rays: kill switch active, not hooking");
        return 0;
    }
    if (!verify_exe(got, &size)) {
        logf_("hellgate-rays: FATAL cannot hash host exe; refusing to hook");
        return 0;
    }
    logf_("hellgate-rays: host sha256=%s size=%u", got, size);
    if (size != HG_EXE_SIZE || lstrcmpA(got, HG_EXE_SHA256) != 0) {
        logf_("hellgate-rays: FATAL host binary is not the build these addresses "
              "were derived from.");
        logf_("hellgate-rays:   expected %s (%u bytes)", HG_EXE_SHA256, HG_EXE_SIZE);
        logf_("hellgate-rays:   refusing to hook.");
        MessageBoxA(NULL,
                    "hellgate-rays: host executable does not match the expected build.\n"
                    "Hooking is disabled. See hellgate_rays.log.",
                    "hellgate-rays", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 0;
    }
    if (g_image != HG_IMAGE_BASE) {
        logf_("hellgate-rays: FATAL image at 0x%08x, expected 0x%08x", g_image, HG_IMAGE_BASE);
        return 0;
    }

    /*
     * Everything this DLL calls or patches, checked against the bytes the
     * disassembly recorded. Failures are logged and counted; they do not
     * abort, because a wrong viewmodel address should not cost you the
     * raycast instrumentation.
     */
    {
        static const unsigned char b_player[]  = {0x51,0xa1,0xec,0x6d,0xb9,0x00};
        static const unsigned char b_spawn[]   = {0x55,0x8b,0xec,0x83,0xe4,0xf0};
        static const unsigned char b_canfp[]   = {0x8b,0x44,0x24,0x04,0x6a,0x2c};
        static const unsigned char b_setcam[]  = {0x55,0x8b,0xec,0x83,0xe4,0xf8};
        static const unsigned char b_rescam[]  = {0x83,0x3d,0x5c,0xf1,0xed,0x00};
        static const unsigned char b_m3rd[]    = {0x85,0xc0,0x75,0x04,0x83,0xc8,0xff,0xc3};
        static const unsigned char b_mflag[]   = {0x83,0xf9,0xff,0x74,0x09};
        static const unsigned char b_kick[]    = {0x75,0x40};
        int bad = 0;

        bad += !verify_bytes(RVA_GET_LOCAL_PLAYER, b_player, 6, "local player getter");
        bad += !verify_bytes(RVA_SPAWN_PRIMITIVE, b_spawn, 6, "spawn primitive");
        bad += !verify_bytes(RVA_CAN_FIRST_PERSON, b_canfp, 6, "CanUseFirstPerson");
        bad += !verify_bytes(RVA_SET_CAMERA_MODE, b_setcam, 6, "SetCameraMode");
        bad += !verify_bytes(RVA_RESTORE_CAMERA, b_rescam, 6, "RestoreCamera");
        bad += !verify_bytes(RVA_UNIT_MODEL_THIRD, b_m3rd, 8, "UnitGetModelIdThirdPerson");
        bad += !verify_bytes(RVA_MODEL_SET_FLAGBIT, b_mflag, 5, "e_ModelSetFlagbit");
        bad += !verify_bytes(RVA_SKILL_FP_KICK, b_kick, 2, "skill-start fp kick");
        logf_("verify: %d of 8 addresses matched their recorded signature",
              8 - bad);
    }

    if (GetEnvironmentVariableW(L"HG_SPAWN_MULT", envbuf, 32) > 0) {
        int m = _wtoi(envbuf);
        if (m >= 1 && m <= 500) g_spawn_mult = (unsigned int)m;
    }
    if (GetEnvironmentVariableW(L"HG_SPAWN_CAP", envbuf, 32) > 0) {
        int c = _wtoi(envbuf);
        if (c >= 1) g_spawn_cap = (unsigned int)c;
    }
    if (GetEnvironmentVariableW(L"HG_RAYS_RSAMPLE", envbuf, 32) > 0) {
        int r = _wtoi(envbuf);
        if (r >= 1) g_rsample = (unsigned int)r;
    }
    if (GetEnvironmentVariableW(L"HG_SIM_TYPE", envbuf, 32) > 0) {
        int t = _wtoi(envbuf);
        if (t >= 0 && t <= 3) {
            g_simtype_override = t;
            logf_("hellgate-rays: HG_SIM_TYPE=%d — will override "
                  "hkWorldCinfo::m_simulationType (experiment only)", t);
        }
    }
    if (GetEnvironmentVariableW(L"HG_RAYS_STACKDEPTH", envbuf, 32) > 0) {
        int d = _wtoi(envbuf);
        if (d >= 1 && d <= HG_FRAMES) g_stack_depth = (unsigned int)d;
    }

    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) { logf_("hellgate-rays: TlsAlloc failed"); return 0; }
    {
        MH_STATUS st = MH_Initialize();        /* hook_start may have done it already */
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { logf_("hellgate-rays: MH_Initialize failed"); return 0; }
    }
    /* Graphics probe (docs/graphics-plan.md step 0): effect names and
     * tiers, loose-file opens, depth formats, frame structure. Goes in
     * first so the effect-creation hook is in place before the renderer
     * loads its shaders. */
    if (hg_flagfile(L"hellgate_gfxprobe.off"))
        logf_("gfxprobe: DISABLED by bin\\hellgate_gfxprobe.off (no D3DX/D3D9/shadow/technique hooks, no effect overrides)");
    else
        gfxprobe_install(g_image);

    hook_one(RVA_QUERY_RAY_ON_TREE, (void *)detour_query, (void **)&g_orig_query,
             "hkMoppLongRayVirtualMachine::queryRayOnTree");
    hook_one(RVA_GAME_RAYCAST, (void *)detour_game_shim, (void **)&g_orig_game,
             "game world-raycast helper");
    hook_one(RVA_PHYS_OBJ_STEP, (void *)detour_step, (void **)&g_orig_step,
             "per-object physics step");
    if (!panel_wanted()) panel_start(g_image);
    if (panel_wanted()) {
        overlay_start(g_image);
        logf_("hellgate-rays: dev panel active — press Shift+` in game "
              "(CMD_CONSOLE_TOGGLE's own binding; a bare ` is the chatbox)");
        if (GetEnvironmentVariableW(L"HG_PANEL_HTTP", envbuf, 32) > 0)
            logf_("hellgate-rays: HTTP frontend also on 127.0.0.1 "
                  "(HG_PANEL_PORT to move it off 7777)");
    }

    if (g_spawn_mult > 1 || panel_wanted()) {
        int n = 0;

        if (g_spawn_mult > 1)
            logf_("hellgate-rays: SPAWN HARNESS ACTIVE mult=%u cap=%u/window "
                  "— this deliberately destabilises the game",
                  g_spawn_mult, g_spawn_cap);
        /* The primitive, not the script action: the script action never
         * fired once across an entire session. Both are hooked anyway —
         * whichever fires first arms the panel, and the panel reports the
         * per-hook counts so which one that was is never a guess. */
        n += hook_one(RVA_SPAWN_PRIMITIVE, (void *)detour_spawn_prim,
                      (void **)&g_orig_spawn_prim, "spawn primitive (0x61c8f1)");
        n += hook_one(RVA_SPAWN_OBJECT, (void *)detour_spawn_obj,
                      (void **)&g_orig_spawn_obj, "SpawnObject script action");
        if (GetEnvironmentVariableW(L"HG_SPAWN_MONSTERS", envbuf, 32) > 0)
            n += hook_one(RVA_SPAWN_MONSTER_NEAR, (void *)detour_spawn_mon,
                          (void **)&g_orig_spawn_mon, "SpawnMonsterNearby");
        g_spawn_hooked = (n > 0);
        if (!g_spawn_hooked)
            logf_("hellgate-rays: no spawn hook took — the panel's spawn "
                  "buttons cannot work this session");
    }

    /*
     * First person. The hook is a passthrough unless the unlock is turned
     * on, but it has to be in place before the first camera change, so it
     * goes in whenever anything could enable it.
     */
    {
        int want_fp = (GetEnvironmentVariableW(L"HG_FP_MELEE", envbuf, 32) > 0
                       && envbuf[0] != L'0');
        if (want_fp || panel_wanted()) {
            g_set_camera     = (set_cam_fn)(g_image + RVA_SET_CAMERA_MODE);
            g_restore_camera = (restore_cam_fn)(g_image + RVA_RESTORE_CAMERA);
            g_cam_mode       = (const int *)(g_image + RVA_CAMERA_MODE_CUR);
            g_kick_site      = (unsigned char *)(g_image + RVA_SKILL_FP_KICK);
            g_fn_model_third = g_image + RVA_UNIT_MODEL_THIRD;
            g_fn_model_flag  = g_image + RVA_MODEL_SET_FLAGBIT;
            g_get_player_local =
                (local_player_fn)(unsigned int)(g_image + RVA_GET_LOCAL_PLAYER);
            g_kick_orig      = *g_kick_site;
            if (g_kick_orig != SKILL_FP_KICK_JNE)
                logf_("fp: WARNING byte at the skill-start kick (%p) is "
                      "0x%02x, expected 0x%02x — not patching that half",
                      (void *)g_kick_site, g_kick_orig, SKILL_FP_KICK_JNE);
            hook_one(RVA_CAN_FIRST_PERSON, (void *)detour_can_fp,
                     (void **)&g_orig_can_fp, "CanUseFirstPerson (the melee gate)");
            if (want_fp) hg_set_fp_melee(1);
        }
    }

    shoulder_install(g_image, hook_one);
    fpview_install(g_image, hook_one);
    {
        static const unsigned char want[7] = { 0x55, 0x8b, 0xec, 0x51, 0x51, 0x83, 0x7d };
        if (memcmp((void *)(g_image + 0x002122A8u), want, sizeof want) == 0)
            hook_one(0x002122A8u, (void *)detour_anim_play, (void **)&g_orig_anim_play,
                     "animation start (first-person blending)");
        else
            logf_("fpview: animation start (0x6122a8) bytes differ; first-person animations unchanged");
    }

    hook_one(RVA_HK_ADD_ENTITY, (void *)detour_add_entity,
             (void **)&g_orig_add_entity, "hkWorld::addEntity");
    hook_one(RVA_HK_REMOVE_ENTITY, (void *)detour_rem_entity,
             (void **)&g_orig_rem_entity, "hkWorld::removeEntity");

    hook_one(RVA_MOPP_CASTRAY, (void *)detour_castray,
             (void **)&g_orig_castray, "hkMoppBvTreeShape::castRay");
    hook_one(RVA_MOPP_CASTRAY_COLL, (void *)detour_castray_coll,
             (void **)&g_orig_castray_coll, "hkMoppBvTreeShape::castRay(collector)");
    hook_one(RVA_HK_WORLDCINFO_CTOR, (void *)detour_cinfo, (void **)&g_orig_cinfo,
             "hkWorldCinfo::hkWorldCinfo");

    logf_("hellgate-rays: window=%dms stackdepth=%u qsample=1/%u",
          HG_WINDOW_MS, g_stack_depth, HG_QSAMPLE);
    logf_("hellgate-rays: W = per-window totals, S = per-callsite stats "
          "(len=[min/mean/max], site frames are RVAs, innermost first)");

    {
        unsigned int tick = 0;
        for (;;) {
            Sleep(HG_WINDOW_MS);
            report_window();
            if ((++tick % 10) == 0) {
                gfxprobe_poll();
            }
            FlushFileBuffers(g_log);
        }
    }
}

void hook_start(void)
{
    HMODULE self = NULL;
    WCHAR *slash;

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&hook_start, &self);
    GetModuleFileNameW(self, g_dll_dir, MAX_PATH);
    slash = wcsrchr(g_dll_dir, L'\\');
    if (slash) *slash = 0;

    CreateThread(NULL, 0, worker, NULL, 0, NULL);
}
