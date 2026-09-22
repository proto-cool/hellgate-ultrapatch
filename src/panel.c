/*
 * Developer / cheat panel.
 *
 * The retail SP build has no usable dev console: ..\consolecmd.cpp survives
 * as exactly one assert (the /stuck path) and there is not a single
 * slash-prefixed command string left in the image. So this is not a matter of
 * re-enabling something — the panel has to be built from outside.
 *
 * Two frontends, one back end:
 *
 *   - overlay.c draws an in-game D3D9 window with tabs and buttons. That is
 *     the one you actually use.
 *
 *   - A loopback HTTP server on 127.0.0.1, opt-in via HG_PANEL_HTTP, serving
 *     one embedded page plus a JSON API. It is better for poking at things
 *     from a second machine, and it is the only frontend the offline selftest
 *     can exercise, which is why it is kept in step with the overlay rather
 *     than left to rot.
 *
 * Nothing either frontend does touches game state directly. The HTTP server
 * parks a request in a single command slot and waits; the overlay, which runs
 * on the render thread and must never block, posts into lock-free mailboxes
 * instead. Both are drained on the game thread by panel_pump(), called from
 * the per-object physics step detour — the one hook known to run ~45x/frame
 * on the thread that owns the world.
 *
 * What the panel can and cannot do, honestly:
 *
 *   The unit struct is almost entirely unmapped. Two offsets are known
 *   (+0x110 flags, +0x120 name) and they came from the default-name sites.
 *   There is no point inventing a "set health" button on top of a layout
 *   nobody has recovered. So the panel ships as a live memory explorer over
 *   the player unit — peek, mark, diff, watch, poke — which is the tool you
 *   use to FIND the health and money offsets. Once an offset is confirmed it
 *   costs three lines to promote it to a named button.
 *
 * Off unless HG_PANEL is set. Loopback-bound only. Single player only: this
 * pokes memory in a live process and will happily corrupt a save.
 */
#include <winsock2.h>    /* must precede windows.h */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "target.h"
#include "panel.h"

void overlay_start(unsigned int image);

#define PANEL_DEFAULT_PORT 7777
#define PANEL_RESULT       16384
#define PANEL_PEEK_MAX     4096
#define PANEL_WAIT_MS      1000
#define PANEL_POKEQ        8     /* pending render-thread pokes; power of two */

/* ------------------------------------------------------------------ */
/* state                                                               */

typedef int (__cdecl *get_player_fn)(void);

enum {
    OP_NONE = 0,
    OP_PLAYER,      /* resolve the player unit, read name + flags        */
    OP_PEEK,        /* a = offset/address, b = length, c = 1 if absolute */
    OP_POKE32,      /* a = offset/address, b = value, c = 1 if absolute  */
    OP_POKEF        /* a = offset/address, b = float bits, c as above    */
};

typedef struct {
    volatile LONG  state;       /* 0 idle, 1 pending, 2 done */
    int            op;
    unsigned int   a, b, c;
    char           result[PANEL_RESULT];
} cmd_slot;

static cmd_slot        g_cmd;
static CRITICAL_SECTION g_submit;   /* serialises submitters */
static unsigned int    g_image;
static int             g_wanted;
static int             g_port = PANEL_DEFAULT_PORT;
static get_player_fn   g_get_player;
static volatile LONG   g_pumps;         /* proves the game thread is draining */

static panel_snap      g_snap;
static volatile LONG   g_snap_seq;      /* seqlock: odd while being written */
static volatile LONG   g_peek_off;      /* overlay's current peek window    */
static volatile LONG   g_snap_tick;

/* Baseline for the hex diff. Requested from any thread, taken by the pump. */
static volatile LONG   g_mark_req;      /* 1 = take, -1 = drop */
static unsigned char   g_mark[PANEL_PEEKW];
static volatile LONG   g_mark_ok;
static volatile LONG   g_mark_off;

/* Watch list. Written by the frontends, read by the pump. */
static volatile LONG   g_watch[PANEL_WATCH];
static volatile LONG   g_nwatch;

/*
 * Pokes posted from the render thread. A ring rather than a slot because a
 * click that lands while the previous poke is still pending should not be
 * silently dropped — and because the render thread cannot wait to find out.
 */
static struct { volatile LONG live; unsigned int off, val; } g_pokeq[PANEL_POKEQ];
static volatile LONG   g_pokeq_w;

/* Telemetry, published once per window by report_window(). */
static volatile LONG   g_t_qray, g_t_rays, g_t_bodies;
static double          g_t_qms;
static float           g_t_dtmin, g_t_dtavg, g_t_dtmax;
static volatile LONG   g_t_window;

/*
 * History ring, written by the reporting thread at 10Hz and unrolled into
 * the snapshot by the pump. Torn by a frame at worst, which in a bar chart
 * is invisible; a lock on the reporting path would not be.
 */
static float           g_h_qms[PANEL_HIST], g_h_dt[PANEL_HIST];
static volatile LONG   g_h_w;

void panel_publish(unsigned int qray, unsigned int rays, long bodies,
                   double qms, float dtmin, float dtavg, float dtmax)
{
    LONG slot;

    g_t_qray = (LONG)qray; g_t_rays = (LONG)rays; g_t_bodies = bodies;
    g_t_qms = qms; g_t_dtmin = dtmin; g_t_dtavg = dtavg; g_t_dtmax = dtmax;

    slot = (InterlockedIncrement(&g_h_w) - 1) & (PANEL_HIST - 1);
    g_h_qms[slot] = (float)qms;
    g_h_dt[slot]  = dtavg * 1000.0f;

    InterlockedIncrement(&g_t_window);
}

int  panel_wanted(void) { return g_wanted; }

/* ------------------------------------------------------------------ */
/* safe memory access                                                  */

static int readable(const void *p, SIZE_T len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (SIZE_T)((const char *)mbi.BaseAddress + mbi.RegionSize
                    - (const char *)p) >= len;
}

static int writable(void *p, SIZE_T len)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD w = PAGE_READWRITE | PAGE_WRITECOPY
            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!p) return 0;
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (!(mbi.Protect & w)) return 0;
    return (SIZE_T)((char *)mbi.BaseAddress + mbi.RegionSize
                    - (char *)p) >= len;
}

/* ------------------------------------------------------------------ */
/* frontend input — all callable from the render thread                */

void panel_peek_nudge(int delta)
{
    LONG v = g_peek_off + delta;
    if (v < 0) v = 0;
    if (v > 0x10000) v = 0x10000;
    InterlockedExchange(&g_peek_off, v);
}

void panel_peek_set(unsigned int off)
{
    if (off > 0x10000u) off = 0x10000u;
    InterlockedExchange(&g_peek_off, (LONG)off);
}

void panel_peek_mark(int on)
{
    InterlockedExchange(&g_mark_req, on ? 1 : -1);
}

void panel_poke_async(unsigned int off, unsigned int value)
{
    LONG slot = (InterlockedIncrement(&g_pokeq_w) - 1) & (PANEL_POKEQ - 1);
    g_pokeq[slot].off = off;
    g_pokeq[slot].val = value;
    MemoryBarrier();
    InterlockedExchange(&g_pokeq[slot].live, 1);
}

void panel_watch_add(unsigned int off)
{
    LONG n = g_nwatch, i;
    for (i = 0; i < n; i++)
        if ((unsigned int)g_watch[i] == off) return;    /* already watched */
    if (n >= PANEL_WATCH) {
        hg_log("panel: watch list full (%d slots)", PANEL_WATCH);
        return;
    }
    g_watch[n] = (LONG)off;
    MemoryBarrier();
    InterlockedExchange(&g_nwatch, n + 1);
    hg_log("panel: watching unit+0x%x", off);
}

void panel_watch_clear(void)
{
    InterlockedExchange(&g_nwatch, 0);
    hg_log("panel: watch list cleared");
}

int panel_snap_read(panel_snap *out)
{
    int tries;
    for (tries = 0; tries < 8; tries++) {
        LONG a = g_snap_seq;
        if (a & 1) continue;                /* writer mid-update */
        MemoryBarrier();
        memcpy(out, &g_snap, sizeof *out);
        MemoryBarrier();
        if (g_snap_seq == a) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* snapshot — written on the game thread, read by the overlay          */

/*
 * Refreshed from the pump, which runs ~45x per frame. The overlay can only
 * show one value per frame, so most of that work would be thrown away;
 * refresh every 8th pump instead.
 */
static void fill_snap(void)
{
    unsigned char *u = NULL;
    unsigned int off = (unsigned int)g_peek_off;
    LONG markreq = InterlockedExchange(&g_mark_req, 0);
    LONG nw = g_nwatch, i;

    if (markreq < 0) InterlockedExchange(&g_mark_ok, 0);

    InterlockedIncrement(&g_snap_seq);      /* -> odd, writing */
    MemoryBarrier();

    g_snap.name[0] = 0;
    g_snap.unit = 0;
    g_snap.flags = 0;
    g_snap.peek_ok = 0;
    g_snap.peek_off = off;
    g_snap.peek_addr = 0;
    g_snap.nwatch = (int)nw;

    if (g_get_player) u = (unsigned char *)(unsigned int)g_get_player();
    if (u) {
        g_snap.unit = (unsigned int)(unsigned long)u;
        if (readable(u + UNIT_OFF_NAME, UNIT_NAME_CHARS * 2)) {
            const unsigned short *w = (const unsigned short *)(u + UNIT_OFF_NAME);
            for (i = 0; i < (LONG)UNIT_NAME_CHARS
                        && i < (LONG)sizeof g_snap.name - 1; i++) {
                unsigned short ch = w[i];
                if (!ch) break;
                g_snap.name[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
            }
            g_snap.name[i] = 0;
        }
        if (readable(u + UNIT_OFF_FLAGS, 4))
            g_snap.flags = *(const unsigned int *)(u + UNIT_OFF_FLAGS);
        if (readable(u + off, PANEL_PEEKW)) {
            memcpy(g_snap.peek, u + off, PANEL_PEEKW);
            g_snap.peek_addr = (unsigned int)(unsigned long)(u + off);
            g_snap.peek_ok = 1;
            if (markreq > 0) {
                memcpy(g_mark, g_snap.peek, PANEL_PEEKW);
                InterlockedExchange(&g_mark_off, (LONG)off);
                InterlockedExchange(&g_mark_ok, 1);
            }
        }
        for (i = 0; i < nw && i < PANEL_WATCH; i++) {
            unsigned int wo = (unsigned int)g_watch[i];
            g_snap.watch_off[i] = wo;
            g_snap.watch_ok[i] = (unsigned char)readable(u + wo, 4);
            g_snap.watch_val[i] = g_snap.watch_ok[i]
                                ? *(const unsigned int *)(u + wo) : 0u;
        }
    }

    /* The baseline only means anything for the window it was taken in. */
    g_snap.mark_ok = g_mark_ok && (unsigned int)g_mark_off == off;
    g_snap.mark_off = (unsigned int)g_mark_off;
    if (g_snap.mark_ok) memcpy(g_snap.mark, g_mark, PANEL_PEEKW);

    {   /* Unroll the ring oldest-first so the overlay can just draw it. */
        LONG w = g_h_w, k;
        g_snap.hist_n = (w < PANEL_HIST) ? (int)w : PANEL_HIST;
        for (k = 0; k < g_snap.hist_n; k++) {
            LONG src = (w - g_snap.hist_n + k) & (PANEL_HIST - 1);
            g_snap.hist_qms[k] = g_h_qms[src];
            g_snap.hist_dt[k]  = g_h_dt[src];
        }
    }

    g_snap.qray   = g_t_qray;   g_snap.rays  = g_t_rays;
    g_snap.bodies = g_t_bodies; g_snap.qms   = g_t_qms;
    g_snap.dtmin  = g_t_dtmin;  g_snap.dtavg = g_t_dtavg; g_snap.dtmax = g_t_dtmax;
    g_snap.pumps  = g_pumps;

    hg_spawn_status(&g_snap.spawn);
    g_snap.simtype_seen = hg_get_simtype_seen();
    g_snap.simtype_override = hg_get_simtype_override();
    g_snap.cam_mode = hg_camera_mode();
    g_snap.fp_melee = hg_get_fp_melee();
    g_snap.fp_avail = hg_fp_melee_available();
    hg_shoulder_status(&g_snap.shoulder);
    hg_gfx_status(&g_snap.gfx);
    g_snap.gfx.shadow_on = hg_shadow_get();
    hg_model_chain(&g_snap.model_unit, &g_snap.model_gfx, &g_snap.model_third);

    MemoryBarrier();
    InterlockedIncrement(&g_snap_seq);      /* -> even, stable */
}

/* ------------------------------------------------------------------ */
/* command execution — GAME THREAD ONLY                                */

static unsigned char *resolve(unsigned int a, unsigned int absolute,
                              const char **err)
{
    unsigned char *base;
    if (absolute) return (unsigned char *)a;
    if (!g_get_player) { *err = "player getter not resolved"; return NULL; }
    base = (unsigned char *)(unsigned int)g_get_player();
    if (!base) { *err = "no local player unit (not in a game?)"; return NULL; }
    return base + a;
}

static void do_player(cmd_slot *c)
{
    unsigned char *u;
    char name[UNIT_NAME_CHARS + 1];
    unsigned int i, flags = 0;

    if (!g_get_player) {
        snprintf(c->result, PANEL_RESULT, "{\"ok\":false,\"err\":\"no getter\"}");
        return;
    }
    u = (unsigned char *)(unsigned int)g_get_player();
    if (!u) {
        snprintf(c->result, PANEL_RESULT,
                 "{\"ok\":true,\"unit\":0,\"name\":\"\",\"flags\":0}");
        return;
    }

    /* +0x120 is 0x20 wide chars, memcpy'd verbatim at 0x00517f09. */
    name[0] = 0;
    if (readable(u + UNIT_OFF_NAME, UNIT_NAME_CHARS * 2)) {
        const unsigned short *w = (const unsigned short *)(u + UNIT_OFF_NAME);
        for (i = 0; i < UNIT_NAME_CHARS; i++) {
            unsigned short ch = w[i];
            if (!ch) break;
            name[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        name[i] = 0;
    }
    if (readable(u + UNIT_OFF_FLAGS, 4))
        flags = *(const unsigned int *)(u + UNIT_OFF_FLAGS);

    snprintf(c->result, PANEL_RESULT,
             "{\"ok\":true,\"unit\":%u,\"name\":\"%s\",\"flags\":%u}",
             (unsigned int)(unsigned long)u, name, flags);
}

static void do_peek(cmd_slot *c)
{
    const char *err = NULL;
    unsigned char *p = resolve(c->a, c->c, &err);
    unsigned int len = c->b, i, n = 0;
    char *o = c->result;

    if (!p) {
        snprintf(c->result, PANEL_RESULT, "{\"ok\":false,\"err\":\"%s\"}",
                 err ? err : "bad address");
        return;
    }
    if (len > PANEL_PEEK_MAX) len = PANEL_PEEK_MAX;
    if (!readable(p, len)) {
        snprintf(c->result, PANEL_RESULT,
                 "{\"ok\":false,\"err\":\"unreadable at %p+%u\"}", p, len);
        return;
    }

    n += snprintf(o + n, PANEL_RESULT - n,
                  "{\"ok\":true,\"addr\":%u,\"len\":%u,\"hex\":\"",
                  (unsigned int)(unsigned long)p, len);
    for (i = 0; i < len && n < PANEL_RESULT - 8; i++)
        n += snprintf(o + n, PANEL_RESULT - n, "%02x", p[i]);
    snprintf(o + n, PANEL_RESULT - n, "\"}");
}

/*
 * One dword, aligned and committed only. Returns 0 and fills `err` rather
 * than writing anything it is not certain about: a stray write into a live
 * process is the one failure mode here that can cost someone a save.
 */
static int poke32(unsigned char *p, unsigned int val, unsigned int *old,
                  const char **err)
{
    if (!p)                                   { *err = "bad address";           return 0; }
    if (((unsigned int)(unsigned long)p) & 3u){ *err = "not dword-aligned";     return 0; }
    if (!writable(p, 4))                      { *err = "not writable";          return 0; }
    *old = *(unsigned int *)p;
    *(unsigned int *)p = val;
    return 1;
}

static void do_poke(cmd_slot *c, int is_float)
{
    const char *err = NULL;
    unsigned char *p = resolve(c->a, c->c, &err);
    unsigned int old = 0;

    if (p && poke32(p, c->b, &old, &err)) {
        snprintf(c->result, PANEL_RESULT,
                 "{\"ok\":true,\"addr\":%u,\"old\":%u,\"new\":%u,\"float\":%d}",
                 (unsigned int)(unsigned long)p, old, c->b, is_float);
        return;
    }
    snprintf(c->result, PANEL_RESULT, "{\"ok\":false,\"err\":\"%s at %p\"}",
             err ? err : "bad address", (void *)p);
}

/* Drain the render thread's pokes. Their results go to the log ring, which
 * the overlay's Log tab shows — there is nowhere else to put them. */
static void drain_pokes(void)
{
    int i;
    for (i = 0; i < PANEL_POKEQ; i++) {
        unsigned int off, val, old = 0;
        const char *err = NULL;
        unsigned char *p;

        if (!g_pokeq[i].live) continue;
        off = g_pokeq[i].off;
        val = g_pokeq[i].val;
        MemoryBarrier();
        InterlockedExchange(&g_pokeq[i].live, 0);

        p = resolve(off, 0, &err);
        if (p && poke32(p, val, &old, &err))
            hg_log("poke: unit+0x%x (%p) %u -> %u", off, (void *)p, old, val);
        else
            hg_log("poke: unit+0x%x REFUSED — %s", off,
                   err ? err : "bad address");
    }
}

void panel_pump(void)
{
    cmd_slot *c = &g_cmd;

    if (!g_wanted) return;
    InterlockedIncrement(&g_pumps);

    /* The spawn queue first: it is the one thing here that is latency
     * sensitive, because a button that answers next frame feels broken. */
    hg_spawn_pump();
    hg_camera_pump();
    hg_model_pump();

    if ((InterlockedIncrement(&g_snap_tick) & 7) == 0) {
        drain_pokes();
        fill_snap();
    }

    if (c->state != 1) return;

    switch (c->op) {
    case OP_PLAYER: do_player(c);   break;
    case OP_PEEK:   do_peek(c);     break;
    case OP_POKE32: do_poke(c, 0);  break;
    case OP_POKEF:  do_poke(c, 1);  break;
    default:
        snprintf(c->result, PANEL_RESULT, "{\"ok\":false,\"err\":\"bad op\"}");
        break;
    }
    /* Publish the result before the state flip, or the waiter can read a
     * half-written buffer. */
    MemoryBarrier();
    c->state = 2;
}

/*
 * Submit from the server thread and wait for the game thread to run it.
 * A timeout here is a real answer, not a failure: it means physics is not
 * stepping, which is exactly what happens at the main menu or on a load
 * screen, and the panel should say so rather than hang.
 */
static void submit(int op, unsigned int a, unsigned int b, unsigned int c,
                   char *out, int outcap)
{
    int waited = 0;

    EnterCriticalSection(&g_submit);
    g_cmd.op = op; g_cmd.a = a; g_cmd.b = b; g_cmd.c = c;
    g_cmd.result[0] = 0;
    MemoryBarrier();
    g_cmd.state = 1;

    while (g_cmd.state != 2 && waited < PANEL_WAIT_MS) {
        Sleep(2);
        waited += 2;
    }
    if (g_cmd.state == 2) {
        lstrcpynA(out, g_cmd.result, outcap);
    } else {
        snprintf(out, outcap,
                 "{\"ok\":false,\"err\":\"game thread did not pump within %dms "
                 "(paused, menu, or loading)\"}", PANEL_WAIT_MS);
    }
    g_cmd.state = 0;
    LeaveCriticalSection(&g_submit);
}

/* ------------------------------------------------------------------ */
/* the page                                                            */

/*
 * Tabs here mirror the overlay's tabs. Two frontends that disagree about
 * what the tool can do is worse than one, so when a control is added to the
 * overlay it gets added here too.
 */
static const char PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Hellgate Dev Panel</title><style>"
":root{--bg:#0e1116;--fg:#e6edf3;--dim:#8b949e;--line:#232b36;--acc:#4ea1ff;"
"--ok:#3fb950;--bad:#f85149;--warn:#d29922;--card:#161b22}"
"*{box-sizing:border-box}"
"body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.5 ui-monospace,"
"SFMono-Regular,Menlo,Consolas,monospace;padding:16px}"
"h1{font-size:15px;margin:0 0 4px;letter-spacing:.04em}"
"h2{font-size:12px;margin:0 0 8px;color:var(--dim);text-transform:uppercase;"
"letter-spacing:.08em;font-weight:600}"
".sub{color:var(--dim);margin:0 0 14px}"
"nav{display:flex;gap:3px;border-bottom:1px solid var(--line);margin-bottom:14px}"
"nav b{padding:7px 13px;cursor:pointer;color:var(--dim);font-weight:600;"
"border-bottom:2px solid transparent;user-select:none}"
"nav b.on{color:var(--fg);border-bottom-color:var(--acc);background:var(--card)}"
"section{display:none}section.on{display:block}"
".grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}"
".card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px}"
".tiles{display:grid;gap:8px;grid-template-columns:repeat(auto-fit,minmax(92px,1fr))}"
".tile{background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:8px}"
".tile b{display:block;font-size:17px;font-weight:600}"
".tile span{color:var(--dim);font-size:11px}"
"label{display:block;color:var(--dim);font-size:11px;margin:8px 0 2px}"
"input,select{width:100%;background:var(--bg);color:var(--fg);border:1px solid "
"var(--line);border-radius:5px;padding:6px 8px;font:inherit}"
"button{background:var(--acc);color:#04121f;border:0;border-radius:5px;"
"padding:7px 12px;font:inherit;font-weight:600;cursor:pointer;margin:8px 4px 0 0}"
"button.alt{background:var(--line);color:var(--fg)}"
"button:active{transform:translateY(1px)}"
"pre{background:var(--bg);border:1px solid var(--line);border-radius:6px;"
"padding:8px;overflow:auto;max-height:340px;margin:10px 0 0;font-size:12px}"
".row{display:flex;gap:8px}.row>*{flex:1}"
".ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}"
".dim{color:var(--dim)}"
"@media(prefers-color-scheme:light){:root{--bg:#f6f8fa;--fg:#1f2328;--dim:#59636e;"
"--line:#d1d9e0;--card:#fff;--acc:#0969da}button{color:#fff}}"
"</style></head><body>"
"<h1>Hellgate Dev Panel</h1>"
"<p class='sub'>Hellgate_sp_x86.exe &middot; in-process &middot; commands run on the game thread"
" &middot; in-game: <b>Shift+`</b></p>"
"<nav id='nav'></nav>"

"<section id='s_live'><div class='grid'>"
"<div class='card'><h2>Live</h2><div class='tiles'>"
"<div class='tile'><b id='t_fps'>-</b><span>frame ms (avg)</span></div>"
"<div class='tile'><b id='t_bodies'>-</b><span>bodies</span></div>"
"<div class='tile'><b id='t_rays'>-</b><span>rays/window</span></div>"
"<div class='tile'><b id='t_qray'>-</b><span>mopp nodes</span></div>"
"<div class='tile'><b id='t_qms'>-</b><span>mopp ms</span></div>"
"<div class='tile'><b id='t_pump'>-</b><span>pumps</span></div>"
"</div><button class='alt' onclick=\"go('/api/reset')\">Reset counters</button>"
"</div></div></section>"

"<section id='s_player'><div class='grid'>"
"<div class='card'><h2>Player unit</h2>"
"<div id='p_out' class='dim'>not resolved</div>"
"<button onclick='player()'>Resolve</button>"
"<button class='alt' onclick='watch()'>Auto-refresh</button></div>"
"<div class='card'><h2>Watches</h2>"
"<p class='dim' style='margin:0'>Dwords re-read every pump, unit-relative.</p>"
"<label>Offset (hex)</label><input id='wa_off' value='110'>"
"<button onclick=\"go('/api/watch?add='+hx($('wa_off').value))\">Watch</button>"
"<button class='alt' onclick=\"go('/api/watch?clear=1')\">Clear</button>"
"<pre id='wa_out'>&nbsp;</pre></div>"
"</div></section>"

"<section id='s_mem'><div class='grid'>"
"<div class='card'><h2>Peek</h2>"
"<label>Base</label><select id='k_base'>"
"<option value='0'>player unit + offset</option>"
"<option value='1'>absolute address</option></select>"
"<div class='row'><div><label>Offset / address (hex)</label>"
"<input id='k_off' value='0'></div>"
"<div><label>Length</label><input id='k_len' value='256'></div></div>"
"<button onclick='peek()'>Read</button>"
"<pre id='k_out'>&nbsp;</pre></div>"

"<div class='card'><h2>Poke</h2>"
"<p class='dim' style='margin:0'>Writes one dword. Aligned + committed only.</p>"
"<label>Base</label><select id='w_base'>"
"<option value='0'>player unit + offset</option>"
"<option value='1'>absolute address</option></select>"
"<div class='row'><div><label>Offset / address (hex)</label>"
"<input id='w_off' value='110'></div>"
"<div><label>Value</label><input id='w_val' value='0'></div></div>"
"<label>Interpret value as</label><select id='w_type'>"
"<option value='u'>unsigned (decimal)</option>"
"<option value='x'>hex</option>"
"<option value='f'>float</option></select>"
"<button onclick='poke()'>Write</button>"
"<pre id='w_out'>&nbsp;</pre></div>"
"</div></section>"

"<section id='s_spawn'><div class='grid'>"
"<div class='card'><h2>Spawn</h2>"
"<p class='dim' style='margin:0 0 6px'>Replays the last spawn the game "
"performed, argument for argument. Until one has been seen there is no "
"template and nothing can fire.</p>"
"<div id='sp_state' class='dim'>&nbsp;</div>"
"<button onclick=\"go('/api/spawn?n=1')\">Fire 1</button>"
"<button onclick=\"go('/api/spawn?n=10')\">Fire 10</button>"
"<button onclick=\"go('/api/spawn?n=100')\">Fire 100</button>"
"<button class='alt' onclick=\"go('/api/spawn?clear=1')\">Clear queue</button>"
"<label>Fire mode</label>"
"<button class='alt' onclick=\"go('/api/spawn?immediate=1')\">Immediate</button>"
"<button class='alt' onclick=\"go('/api/spawn?immediate=0')\">Piggyback</button>"
"<label>Amplifier: every game spawn becomes N</label>"
"<input id='sp_mult' value='1'>"
"<button class='alt' onclick=\"go('/api/spawn?mult='+(parseInt($('sp_mult').value)||1))\">Set</button>"
"<pre id='sp_out'>&nbsp;</pre></div></div></section>"

"<section id='s_phys'><div class='grid'>"
"<div class='card'><h2>hkWorldCinfo::m_simulationType</h2>"
"<div id='ph_state' class='dim'>&nbsp;</div>"
"<p class='dim'>Applies at the next world construction &mdash; a zone change, "
"not this instant. DISCRETE removes tunnelling protection from everything.</p>"
"<button class='alt' onclick=\"go('/api/sim?t=1')\">DISCRETE (1)</button>"
"<button class='alt' onclick=\"go('/api/sim?t=2')\">CONTINUOUS (2)</button>"
"<button class='alt' onclick=\"go('/api/sim?t=-1')\">No override</button>"
"<pre id='ph_out'>&nbsp;</pre></div></div></section>"

"<section id='s_log'><div class='card'><h2>Log</h2>"
"<pre id='lg_out' style='max-height:60vh'>&nbsp;</pre></div></section>"

"<script>"
"const $=i=>document.getElementById(i);"
"const hx=v=>parseInt(String(v).replace(/^0x/i,''),16)||0;"
"const TABS=[['Live','s_live'],['Player','s_player'],['Memory','s_mem'],"
"['Spawn','s_spawn'],['Physics','s_phys'],['Log','s_log']];"
"let T=0;"
"function draw(){$('nav').innerHTML='';TABS.forEach((t,i)=>{"
"const b=document.createElement('b');b.textContent=t[0];"
"if(i==T)b.className='on';b.onclick=()=>{T=i;draw();tick()};"
"$('nav').appendChild(b);"
"$(t[1]).className=(i==T)?'on':''});}"
"draw();"
"async function j(u){try{const r=await fetch(u);return await r.json()}"
"catch(e){return{ok:false,err:String(e)}}}"
"async function go(u){const r=await j(u);"
"const p=$(['s_live','s_player','s_mem','s_spawn','s_phys','s_log'][T]"
"==='s_spawn'?'sp_out':'ph_out');tick();}"
"function spawnLine(s){"
"if(!s.hooked)return['<span class=bad>spawn hooks not installed</span>',''];"
"const t=s.tmpl||'none';"
"const head=(t=='none')"
"?'<span class=warn>no template yet &mdash; waiting for the game to spawn "
"something once</span>'"
":('<span class=ok>armed</span> from '+t);"
"return[head,'seen: primitive '+s.seen_prim+', script '+s.seen_script+"
"'  |  fired '+s.fired+'  queued '+s.queued+'  amplified '+s.amplified+"
"'\\nmode: '+(s.immediate?'immediate':'piggyback')+'   mult '+s.mult+"
"'   template tid '+s.tmpl_tid+'   pump tid '+s.pump_tid];}"
"async function tick(){const s=await j('/api/state');if(!s.ok)return;"
"$('t_fps').textContent=s.dtavg.toFixed(1);"
"$('t_bodies').textContent=s.bodies;"
"$('t_rays').textContent=s.rays;"
"$('t_qray').textContent=s.qray;"
"$('t_qms').textContent=s.qms.toFixed(1);"
"$('t_pump').textContent=s.pumps;"
"const sl=spawnLine(s.spawn);"
"$('sp_state').innerHTML=sl[0];$('sp_out').textContent=sl[1];"
"$('ph_state').innerHTML='observed '+s.sim_seen+'   override '+"
"(s.sim_over<0?'none':s.sim_over);"
"$('wa_out').textContent=(s.watch&&s.watch.length)?s.watch.map(w=>"
"'unit+0x'+w.off.toString(16)+'  '+(w.ok?(w.val+'  0x'+w.val.toString(16)):'unreadable')"
").join('\\n'):'(none)';"
"if(T==5){const l=await j('/api/log');"
"$('lg_out').textContent=(l.lines||[]).join('\\n')||'(empty)';}}"
"setInterval(tick,500);tick();"
"async function player(){const r=await j('/api/exec?op=player');"
"$('p_out').innerHTML=r.ok?(r.unit?('<span class=ok>'+(r.name||'(unnamed)')+"
"'</span><br>unit 0x'+r.unit.toString(16)+'<br>flags 0x'+r.flags.toString(16))"
":'<span class=dim>no unit &mdash; not in a game</span>')"
":('<span class=bad>'+r.err+'</span>');}"
"let W=0;function watch(){if(W){clearInterval(W);W=0;return}W=setInterval(player,1000);player();}"
"function fmt(hex,base){let o='';for(let i=0;i<hex.length;i+=32){"
"const row=hex.slice(i,i+32);let h='',a='';"
"for(let k=0;k<row.length;k+=2){const b=parseInt(row.substr(k,2),16);"
"h+=row.substr(k,2)+((k%8==6)?'  ':' ');"
"a+=(b>=32&&b<127)?String.fromCharCode(b):'.';}"
"o+=(base+i/2).toString(16).padStart(4,'0')+'  '+h.padEnd(52)+' |'+a+'|\\n';}"
"return o||'(empty)';}"
"async function peek(){const off=hx($('k_off').value),ln=parseInt($('k_len').value)||0;"
"const r=await j('/api/exec?op=peek&a='+off+'&b='+ln+'&c='+$('k_base').value);"
"$('k_out').textContent=r.ok?fmt(r.hex,$('k_base').value=='1'?r.addr:off)"
":('error: '+r.err);}"
"async function poke(){const off=hx($('w_off').value),t=$('w_type').value;"
"let v=$('w_val').value,n;"
"if(t=='f'){const f=new Float32Array(1);f[0]=parseFloat(v);"
"n=new Uint32Array(f.buffer)[0];}else if(t=='x'){n=hx(v)>>>0;}"
"else{n=(parseInt(v)||0)>>>0;}"
"const r=await j('/api/exec?op=poke'+(t=='f'?'f':'')+'&a='+off+'&b='+n+"
"'&c='+$('w_base').value);"
"$('w_out').textContent=r.ok?('ok  0x'+r.addr.toString(16)+'  '+r.old+' -> '+r.new)"
":('error: '+r.err);}"
"</script></body></html>";

/* ------------------------------------------------------------------ */
/* HTTP                                                                */

static void send_all(SOCKET s, const char *p, int n)
{
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w <= 0) return;
        p += w; n -= w;
    }
}

static void respond(SOCKET s, const char *ctype, const char *body, int len)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                     "Content-Length: %d\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", ctype, len);
    send_all(s, hdr, n);
    send_all(s, body, len);
}

/* Returns the integer value of ?name=... in `q`, or `dflt`. */
static int qparam(const char *q, const char *name, int dflt)
{
    char key[32];
    const char *p;
    int n = snprintf(key, sizeof key, "%s=", name);
    if (!q) return dflt;
    for (p = q; (p = strstr(p, key)) != NULL; p++) {
        if (p == q || p[-1] == '?' || p[-1] == '&')
            return (int)strtol(p + n, NULL, 10);
    }
    return dflt;
}

static int qhas(const char *q, const char *name, const char *val)
{
    char want[48];
    if (!q) return 0;
    snprintf(want, sizeof want, "%s=%s", name, val);
    {
        const char *p = strstr(q, want);
        int n = (int)strlen(want);
        if (!p) return 0;
        return p[n] == 0 || p[n] == '&';
    }
}

static const char *tmpl_json(int kind)
{
    switch (kind) {
    case HG_TMPL_PRIM:       return "primitive";
    case HG_TMPL_SCRIPT_OBJ: return "SpawnObject";
    case HG_TMPL_SCRIPT_MON: return "SpawnMonsterNearby";
    default:                 return "none";
    }
}

static int state_json(char *out, int cap)
{
    hg_spawn_state sp;
    LONG nw = g_nwatch, i;
    int n;

    hg_spawn_status(&sp);
    n = snprintf(out, cap,
        "{\"ok\":true,\"window\":%ld,\"qray\":%ld,\"rays\":%ld,\"bodies\":%ld,"
        "\"qms\":%.3f,\"dtmin\":%.2f,\"dtavg\":%.2f,\"dtmax\":%.2f,"
        "\"pumps\":%ld,\"sim_seen\":%d,\"sim_over\":%d,"
        "\"cam\":%d,\"fp_melee\":%d,\"fp_avail\":%d,"
        "\"spawn\":{\"hooked\":%d,\"tmpl\":\"%s\",\"seen_prim\":%ld,"
        "\"seen_script\":%ld,\"fired\":%ld,\"queued\":%ld,\"amplified\":%ld,"
        "\"immediate\":%d,\"mult\":%u,\"tmpl_tid\":%lu,\"pump_tid\":%lu},"
        "\"watch\":[",
        g_t_window, g_t_qray, g_t_rays, g_t_bodies, g_t_qms,
        g_t_dtmin * 1000.0f, g_t_dtavg * 1000.0f, g_t_dtmax * 1000.0f,
        g_pumps, hg_get_simtype_seen(), hg_get_simtype_override(),
        hg_camera_mode(), hg_get_fp_melee(), hg_fp_melee_available(),
        sp.hooked, tmpl_json(sp.tmpl_kind), sp.seen_prim, sp.seen_script,
        sp.fired, sp.queued, sp.amplified, sp.immediate, sp.mult,
        sp.tmpl_tid, sp.pump_tid);

    /*
     * The offset is frontend state and is always known. The value is game
     * memory, so it can only come from the snapshot the pump publishes --
     * this is the server thread and it must not read unit memory itself.
     * Taking both from the snapshot reported offset 0 for every watch until
     * the first pump, which reads as "it did not work".
     */
    {
        panel_snap s;
        int have = panel_snap_read(&s) && s.nwatch > 0;
        for (i = 0; i < nw && i < PANEL_WATCH && n < cap - 64; i++) {
            unsigned int off = (unsigned int)g_watch[i];
            int fresh = have && i < s.nwatch && s.watch_off[i] == off;
            n += snprintf(out + n, cap - n,
                          "%s{\"off\":%u,\"ok\":%d,\"val\":%u}",
                          i ? "," : "", off,
                          fresh ? s.watch_ok[i] : 0,
                          fresh ? s.watch_val[i] : 0u);
        }
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

static void handle(SOCKET s, char *req)
{
    char out[PANEL_RESULT];
    char *sp, *path, *q;

    if (strncmp(req, "GET ", 4) != 0) {
        respond(s, "text/plain", "method", 6);
        return;
    }
    path = req + 4;
    sp = strchr(path, ' ');
    if (!sp) return;
    *sp = 0;
    q = strchr(path, '?');
    if (q) *q++ = 0;

    if (strcmp(path, "/") == 0) {
        respond(s, "text/html; charset=utf-8", PAGE, (int)sizeof PAGE - 1);
        return;
    }

    if (strcmp(path, "/api/state") == 0) {
        int n = state_json(out, sizeof out);
        respond(s, "application/json", out, n);
        return;
    }

    if (strcmp(path, "/api/log") == 0) {
        char line[HG_LOGLINE];
        int n = snprintf(out, sizeof out, "{\"ok\":true,\"lines\":["), i;
        for (i = 0; i < HG_LOGRING && n < (int)sizeof out - 256; i++) {
            const char *c;
            if (!hg_log_line(i, line, sizeof line)) break;
            n += snprintf(out + n, sizeof out - n, "%s\"", i ? "," : "");
            for (c = line; *c && n < (int)sizeof out - 8; c++) {
                if (*c == '"' || *c == '\\')
                    n += snprintf(out + n, sizeof out - n, "\\%c", *c);
                else if ((unsigned char)*c >= 32)
                    n += snprintf(out + n, sizeof out - n, "%c", *c);
            }
            n += snprintf(out + n, sizeof out - n, "\"");
        }
        n += snprintf(out + n, sizeof out - n, "]}");
        respond(s, "application/json", out, n);
        return;
    }

    /* /api/burst is the old name; kept so existing notes keep working. */
    if (strcmp(path, "/api/spawn") == 0 || strcmp(path, "/api/burst") == 0) {
        int n;
        if (qparam(q, "clear", 0)) hg_spawn_clear();
        if (strstr(q ? q : "", "immediate="))
            hg_spawn_set_immediate(qparam(q, "immediate", 1));
        if (strstr(q ? q : "", "mult="))
            hg_spawn_set_mult((unsigned int)qparam(q, "mult", 1));
        if (qparam(q, "n", 0) > 0) hg_spawn_queue(qparam(q, "n", 0));
        n = state_json(out, sizeof out);
        respond(s, "application/json", out, n);
        return;
    }

    /* Both spellings: "/fart" so it can literally be typed as a command. */
    if (strcmp(path, "/fart") == 0 || strcmp(path, "/api/fart") == 0) {
        int n;
        hg_fart();
        n = snprintf(out, sizeof out, "{\"ok\":true,\"parp\":true}");
        respond(s, "application/json", out, n);
        return;
    }

    if (strcmp(path, "/api/camera") == 0) {
        int n;
        if (strstr(q ? q : "", "fp_melee="))
            hg_set_fp_melee(qparam(q, "fp_melee", 0));
        if (strstr(q ? q : "", "mode="))
            hg_camera_request(qparam(q, "mode", HG_CAM_THIRD));
        if (strstr(q ? q : "", "bit="))
            hg_model_flag_request(qparam(q, "bit", 7), qparam(q, "val", 1));
        n = state_json(out, sizeof out);
        respond(s, "application/json", out, n);
        return;
    }

    if (strcmp(path, "/api/sim") == 0) {
        int n;
        hg_set_simtype(qparam(q, "t", -1));
        n = state_json(out, sizeof out);
        respond(s, "application/json", out, n);
        return;
    }

    if (strcmp(path, "/api/reset") == 0) {
        int n;
        hg_counters_reset();
        n = state_json(out, sizeof out);
        respond(s, "application/json", out, n);
        return;
    }

    if (strcmp(path, "/api/watch") == 0) {
        int n;
        if (qparam(q, "clear", 0)) panel_watch_clear();
        if (strstr(q ? q : "", "add="))
            panel_watch_add((unsigned int)qparam(q, "add", 0));
        n = state_json(out, sizeof out);
        respond(s, "application/json", out, n);
        return;
    }

    if (strcmp(path, "/api/exec") == 0) {
        unsigned int a = (unsigned int)qparam(q, "a", 0);
        unsigned int b = (unsigned int)qparam(q, "b", 0);
        unsigned int c = (unsigned int)qparam(q, "c", 0);
        int op = OP_NONE;

        if (qhas(q, "op", "player"))     op = OP_PLAYER;
        else if (qhas(q, "op", "peek"))  op = OP_PEEK;
        else if (qhas(q, "op", "pokef")) op = OP_POKEF;
        else if (qhas(q, "op", "poke"))  op = OP_POKE32;

        if (op == OP_NONE) {
            int n = snprintf(out, sizeof out,
                             "{\"ok\":false,\"err\":\"unknown op\"}");
            respond(s, "application/json", out, n);
            return;
        }
        submit(op, a, b, c, out, sizeof out);
        respond(s, "application/json", out, (int)strlen(out));
        return;
    }

    respond(s, "text/plain", "no", 2);
}

static DWORD WINAPI server(LPVOID unused)
{
    WSADATA wsa;
    SOCKET ls;
    struct sockaddr_in sa;
    int on = 1;

    (void)unused;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return 0;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback only */

    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(ls, 8) != 0) {
        closesocket(ls);
        hg_log("panel: could not bind 127.0.0.1:%d", g_port);
        return 0;
    }
    hg_log("panel: HTTP on http://127.0.0.1:%d/", g_port);

    for (;;) {
        SOCKET cs = accept(ls, NULL, NULL);
        char buf[2048];
        int n;
        if (cs == INVALID_SOCKET) { Sleep(50); continue; }
        n = recv(cs, buf, sizeof buf - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            handle(cs, buf);
        }
        shutdown(cs, SD_SEND);
        closesocket(cs);
    }
}

/* ------------------------------------------------------------------ */

void panel_start(unsigned int image)
{
    WCHAR env[32];

    /*
     * Either switch turns it on: HG_PANEL=1 in the environment, or a file
     * named hellgate_panel.on sitting next to the DLL. The file is there
     * because editing Steam launch options to flip a debug flag is friction
     * you pay every single run.
     */
    if (!((GetEnvironmentVariableW(L"HG_PANEL", env, 32) > 0 && env[0] != L'0')
          || hg_flagfile(L"hellgate_panel.on"))) {
        hg_log("hellgate-rays: panel off (set HG_PANEL=1 or create "
               "hellgate_panel.on next to version.dll)");
        return;
    }
    g_wanted = 1;
    g_image = image;

    if (GetEnvironmentVariableW(L"HG_PANEL_PORT", env, 32) > 0) {
        int p = _wtoi(env);
        if (p > 0 && p < 65536) g_port = p;
    }

    g_get_player = (get_player_fn)(unsigned int)(g_image + RVA_GET_LOCAL_PLAYER);
    InitializeCriticalSection(&g_submit);

    /*
     * The HTTP frontend is opt-in now that the overlay exists. It is still
     * the better tool for poking at things from a second machine, and it is
     * the only frontend the offline selftest can exercise.
     */
    if (GetEnvironmentVariableW(L"HG_PANEL_HTTP", env, 32) > 0 && env[0] != L'0')
        CreateThread(NULL, 0, server, NULL, 0, NULL);
}
