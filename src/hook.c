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
#include <math.h>
#include "target.h"
#include "sha256.h"
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
static volatile LONG g_spawn_seen;
static volatile LONG g_spawn_extra;
static volatile LONG g_spawn_budget;
static int           g_simtype_override = -1;
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

static void logf_(const char *fmt, ...)
{
    char buf[2048];
    int n;
    va_list ap;
    DWORD written;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log != INVALID_HANDLE_VALUE)
        WriteFile(g_log, buf, n, &written, NULL);
    OutputDebugStringA(buf);
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
/* 5. spawn amplifier — the deterministic repro harness (G1)             */

/*
 * H8 says the raycast volume comes from continuous collision detection
 * sweeping every *moving body* against the world. The prediction is
 * therefore simple: push the moving-body count up and `rays=` should climb
 * with it, superlinearly once the frame starts losing.
 *
 * Three sessions failed to provoke the stall by hand, and this environment
 * appears to suppress it, so we provoke it deliberately instead. Whenever
 * the game spawns something, spawn HG_SPAWN_MULT-1 more with the identical
 * context it just used — guaranteed valid, no struct layout required.
 *
 * Off by default (mult 1). Budgeted per window so a runaway cannot wedge
 * the process, and monsters are behind a separate switch because spawning
 * crowds of them is far more disruptive than spawning objects.
 */
static int amplify(spawn_fn orig, void *ctx)
{
    int r = orig(ctx);
    unsigned int i;

    InterlockedIncrement(&g_spawn_seen);
    if (g_spawn_mult <= 1) return r;

    for (i = 1; i < g_spawn_mult; i++) {
        if (InterlockedIncrement(&g_spawn_budget) > (LONG)g_spawn_cap) break;
        InterlockedIncrement(&g_spawn_extra);
        orig(ctx);
    }
    return r;
}

static int __cdecl detour_spawn_prim(void *a0, void *a1, void *a2, void *a3, void *a4,
                                     void *a5, void *a6, void *a7, void *a8, void *a9,
                                     void *a10, void *a11, void *a12)
{
    int r = g_orig_spawn_prim(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12);
    unsigned int i;

    InterlockedIncrement(&g_spawn_seen);
    if (g_spawn_mult <= 1) return r;

    for (i = 1; i < g_spawn_mult; i++) {
        if (InterlockedIncrement(&g_spawn_budget) > (LONG)g_spawn_cap) break;
        InterlockedIncrement(&g_spawn_extra);
        g_orig_spawn_prim(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12);
    }
    return r;
}

static int __cdecl detour_spawn_obj(void *ctx) { return amplify(g_orig_spawn_obj, ctx); }
static int __cdecl detour_spawn_mon(void *ctx) { return amplify(g_orig_spawn_mon, ctx); }

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
    logf_("M private=%lluMB ws=%lluMB commit=%lluMB reserve=%lluMB "
          "free=%lluMB largestfree=%lluMB regions=%u freeregions=%u",
          (unsigned long long)pmc.PrivateUsage >> 20,
          (unsigned long long)pmc.WorkingSetSize >> 20,
          commit >> 20, reserve >> 20, freetot >> 20, largest >> 20,
          regions, freeregions);
}

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
        LONG seen = InterlockedExchange(&g_spawn_seen, 0);
        LONG extra = InterlockedExchange(&g_spawn_extra, 0);
        InterlockedExchange(&g_spawn_budget, 0);
        if (seen || extra)
            logf_("X spawns=%ld amplified=%ld mult=%u", seen, extra, g_spawn_mult);
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

static int disabled(void)
{
    WCHAR p[MAX_PATH];
    if (GetEnvironmentVariableW(L"HG_RAYS_DISABLE", p, MAX_PATH) > 0 && p[0] != L'0')
        return 1;
    lstrcpyW(p, g_dll_dir);
    lstrcatW(p, L"\\hellgate_rays.off");
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void open_log(void)
{
    WCHAR p[MAX_PATH];
    if (GetEnvironmentVariableW(L"HG_RAYS_LOG", p, MAX_PATH) == 0) {
        lstrcpyW(p, g_dll_dir);
        lstrcatW(p, L"\\hellgate_rays.log");
    }
    g_log = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
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
    if (MH_Initialize() != MH_OK) { logf_("hellgate-rays: MH_Initialize failed"); return 0; }

    hook_one(RVA_QUERY_RAY_ON_TREE, (void *)detour_query, (void **)&g_orig_query,
             "hkMoppLongRayVirtualMachine::queryRayOnTree");
    hook_one(RVA_GAME_RAYCAST, (void *)detour_game_shim, (void **)&g_orig_game,
             "game world-raycast helper");
    hook_one(RVA_PHYS_OBJ_STEP, (void *)detour_step, (void **)&g_orig_step,
             "per-object physics step");
    if (g_spawn_mult > 1) {
        logf_("hellgate-rays: SPAWN HARNESS ACTIVE mult=%u cap=%u/window "
              "— this deliberately destabilises the game",
              g_spawn_mult, g_spawn_cap);
        /* The primitive, not the script action: the script action never
         * fired once across an entire session. */
        hook_one(RVA_SPAWN_PRIMITIVE, (void *)detour_spawn_prim,
                 (void **)&g_orig_spawn_prim, "spawn primitive (0x61c8f1)");
        hook_one(RVA_SPAWN_OBJECT, (void *)detour_spawn_obj,
                 (void **)&g_orig_spawn_obj, "SpawnObject script action");
        if (GetEnvironmentVariableW(L"HG_SPAWN_MONSTERS", envbuf, 32) > 0)
            hook_one(RVA_SPAWN_MONSTER_NEAR, (void *)detour_spawn_mon,
                     (void **)&g_orig_spawn_mon, "SpawnMonsterNearby");
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

    for (;;) {
        Sleep(HG_WINDOW_MS);
        report_window();
        FlushFileBuffers(g_log);
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
