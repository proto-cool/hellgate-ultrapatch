/*
 * The inventory's Sort button (docs/spikes/inventory-sort.md).
 *
 * A click reads the backpack and plans a sorted layout (src/invplan.c);
 * then, once a frame from Present, each item is moved the way a drag does:
 * pick up to the cursor, wait until the game shows it there, put down at
 * x, y, wait until it shows it there. All sent in one frame, the first try
 * moved nothing (2026-09-23): the game takes its answers a frame or more
 * later. A step not confirmed within MAX_WAIT frames puts the item back
 * where it was and stops the sort; so does an item on the cursor that is
 * not ours. Nothing moves if the cursor already holds an item or no sorted
 * layout can be reached (ip_plan runs a plan only if it gets every item
 * home). All of it runs on the main thread (the click and Present share
 * it), where the game's inventory UI reads the same structures without a
 * lock.
 *
 * - Player: FUN_0045a24c, the component in EAX and one stack argument (1,
 *   popped by the caller), walks up to the component's focus unit.
 * - Backpack: the unit's inventory at unit+0x144; its location records at
 *   +0xc (0x80 bytes each, count at +0x2c). The one with +0x00 == 0x18 (the
 *   big pack) has the grid size at +0x44 / +0x48 and its first item at
 *   +0x6c.
 * - Items: each item's node at item+0x140: +0x00 the owner, +0x28 the
 *   location, +0x2c / +0x30 x and y, +0x10 the next item. The chain runs on
 *   past this location, so the walk stops where the owner or location
 *   changes. The item's id is at item+0x2dc.
 * - Size: FUN_005ffb8a(item, stat, 0), cdecl, stats 0xde (width) and 0xdf
 *   (height); 0 means 1.
 * - Moves: FUN_006309fa, owner in EDI, item in ESI, then location, x, y on
 *   the stack (caller pops); it sends message 0x2d, as a drag does (probe,
 *   2026-09-23). Location 0x36 is the cursor.
 * - The mouse: the UI keeps its own cursor item id at +0x2ec of its state
 *   (*0x00f27250), set by the server on each pick up and cleared only by the
 *   UI's own drop, so after the last move the item stayed on the mouse.
 *   FUN_005ab767 (cdecl, no arguments) is the UI's clear: the id, the cursor
 *   graphic, and a put back only if the cursor location still holds an item.
 * - Categories: the item's type at item+0x340; FUN_0045a692(type, isa),
 *   cdecl, the game's is-a over the unittypes tree. Crafting materials are
 *   scrap (374: scrap, tech, holy, magic, nanoshards), the essences (490-493,
 *   689) and recipes (292); gear is equipable (480) or a mod (15); anything
 *   else (consumables, quest items, keys, dyes) goes to the top.
 */
#include <windows.h>
#include "panel.h"
#include "invplan.h"

#define VA_FOCUS_UNIT 0x0045a24cu
#define VA_STAT_GET   0x005ffb8au
#define VA_PUT_ITEM   0x006309fau
#define VA_UI_STATE   0x00f27250u
#define VA_CURSOR_CLR 0x005ab767u
#define VA_TYPE_ISA   0x0045a692u
#define LOC_BIGPACK   0x18
#define LOC_CURSOR    0x36
#define STAT_INVW     0xde
#define STAT_INVH     0xdf

typedef int (__cdecl *stat_fn)(void *unit, int stat, int param);
typedef int (__cdecl *isa_fn)(int type, int isa);
typedef void (__cdecl *clear_fn)(void);

/* 0 consumables and anything else, 1 crafting materials, 2 gear */
static int item_cat(unsigned char *item)
{
    static const int materials[] = { 374, 490, 491, 492, 493, 689, 292 };
    isa_fn isa = (isa_fn)(UINT_PTR)VA_TYPE_ISA;
    int type = *(int *)(item + 0x340), i;
    if (type < 0) return 0;
    for (i = 0; i < (int)(sizeof materials / sizeof materials[0]); i++)
        if (isa(type, materials[i])) return 1;
    if (isa(type, 480) || isa(type, 15)) return 2;
    return 0;
}

static void *focus_unit(void *comp)
{
    void *r;
    __asm__ volatile ("pushl $1\n\t"
                      "call *%[fn]\n\t"
                      "addl $4, %%esp"
                      : "=a"(r)
                      : "a"(comp), [fn] "S"(VA_FOCUS_UNIT)
                      : "ecx", "edx", "memory", "cc");
    return r;
}

/* the game's "put item at location x, y" (a pick up is a put at the cursor) */
static void put_item(void *owner, void *item, int loc, int x, int y)
{
    int args[3] = { loc, x, y };
    int *p = args;
    __asm__ volatile ("pushl 8(%%eax)\n\t"
                      "pushl 4(%%eax)\n\t"
                      "pushl (%%eax)\n\t"
                      "call *%%ebx\n\t"
                      "addl $12, %%esp"
                      : "+a"(p), "+D"(owner), "+S"(item)
                      : "b"(VA_PUT_ITEM), "m"(args)
                      : "ecx", "edx", "memory", "cc");
}

static int readable(const void *p, UINT n) { return p && !IsBadReadPtr(p, n); }

static int item_stat(void *item, int stat)
{
    int v = ((stat_fn)(UINT_PTR)VA_STAT_GET)(item, stat, 0);
    return v > 0 ? v : 1;
}

/* the location record for loc, or NULL */
static unsigned char *loc_record(unsigned char *recs, int count, int loc)
{
    int i;
    for (i = 0; i < count; i++)
        if (*(int *)(recs + i * 0x80) == loc) return recs + i * 0x80;
    return NULL;
}

#define MAX_WAIT 90             /* frames to wait for the game to confirm a step */
#define SETTLE   6              /* frames a put must hold: the client shows a move at
                                 * once and the server can still send it back */

static struct {
    int active;
    DWORD tid;
    unsigned char *unit, *cur;       /* the player, the cursor's location record */
    void *units[IP_MAXITEMS];
    ip_move mv[4 * IP_MAXITEMS];
    int m, k, phase, wait, fx, fy, n, retried, sweep;
} g_job;

enum { PH_PICK, PH_WAIT_PICK, PH_WAIT_PUT, PH_SETTLE, PH_SWEEP };

/* where an item is now; 0 if unreadable */
static int item_where(unsigned char *item, int *loc, int *x, int *y)
{
    unsigned char *node;
    if (!readable(item, 0x300)) return 0;
    node = *(unsigned char **)(item + 0x140);
    if (!readable(node, 0x34)) return 0;
    *loc = *(int *)(node + 0x28);
    *x = *(int *)(node + 0x2c);
    *y = *(int *)(node + 0x30);
    return 1;
}

/* End the job, after a last look at the cursor: one of our items still on
 * it (a put the server sent back) goes to its target, else where it was. */
static void job_end(const char *why)
{
    hg_log("invsort: %s after %d of %d moves", why, g_job.k, g_job.m);
    g_job.phase = PH_SWEEP;
    g_job.wait = 0;
}

static void sweep(void)
{
    void *held;
    int i;
    if (++g_job.wait < SETTLE * 2) return;
    held = g_job.cur ? *(void **)(g_job.cur + 0x6c) : NULL;
    g_job.active = 0;
    if (!held) {
        /* the UI still shows the last item we picked up on the mouse */
        unsigned char *ui = *(unsigned char **)(UINT_PTR)VA_UI_STATE;
        if (readable(ui, 0x300) && *(int *)(ui + 0x2ec) != -1) {
            hg_log("invsort: clearing the mouse (item %d)", *(int *)(ui + 0x2ec));
            ((clear_fn)(UINT_PTR)VA_CURSOR_CLR)();
        }
        return;
    }
    for (i = g_job.m - 1; i >= 0; i--)            /* its last planned cell */
        if (g_job.units[g_job.mv[i].item] == held) break;
    if (i < 0) return;                            /* not ours */
    hg_log("invsort: item %d was left on the cursor; putting it down", *(int *)((unsigned char *)held + 0x2dc));
    if (g_job.sweep++ < 2) {
        /* its planned target if this was the move in flight, else the free cell it came from */
        if (g_job.k < g_job.m && g_job.units[g_job.mv[g_job.k].item] == held)
            put_item(g_job.unit, held, LOC_BIGPACK, g_job.fx, g_job.fy);
        else
            put_item(g_job.unit, held, LOC_BIGPACK, g_job.mv[i].x, g_job.mv[i].y);
        g_job.active = 1;                         /* look again once it settles */
        g_job.phase = PH_SWEEP;
        g_job.wait = 0;
    }
}

/* Present, once a frame. */
void invsort_tick(void)
{
    unsigned char *item;
    int loc, x, y;
    if (!g_job.active || GetCurrentThreadId() != g_job.tid) return;
    if (g_job.phase == PH_SWEEP) { sweep(); return; }
    if (g_job.k >= g_job.m) { job_end("sorted"); return; }
    item = (unsigned char *)g_job.units[g_job.mv[g_job.k].item];
    if (!item_where(item, &loc, &x, &y)) { job_end("an item became unreadable; stopped"); return; }
    switch (g_job.phase) {
    case PH_PICK:
        if (g_job.cur && *(void **)(g_job.cur + 0x6c)) { job_end("the cursor holds an item; stopped"); return; }
        if (loc != LOC_BIGPACK) { job_end("an item left the backpack; stopped"); return; }
        g_job.fx = x; g_job.fy = y;
        put_item(g_job.unit, item, LOC_CURSOR, 0, 0);
        g_job.phase = PH_WAIT_PICK;
        g_job.wait = 0;
        break;
    case PH_WAIT_PICK:
        if (loc == LOC_CURSOR) {
            put_item(g_job.unit, item, LOC_BIGPACK, g_job.mv[g_job.k].x, g_job.mv[g_job.k].y);
            g_job.phase = PH_WAIT_PUT;
            g_job.wait = 0;
        } else if (++g_job.wait > MAX_WAIT) {
            job_end("a pick up was not confirmed; stopped");
        }
        break;
    case PH_WAIT_PUT:
        if (loc == LOC_BIGPACK && x == g_job.mv[g_job.k].x && y == g_job.mv[g_job.k].y) {
            g_job.phase = PH_SETTLE;
            g_job.wait = 0;
        } else if (loc == LOC_BIGPACK) {
            job_end("an item landed somewhere else; stopped");
        } else if (++g_job.wait > MAX_WAIT) {
            if (loc == LOC_CURSOR) put_item(g_job.unit, item, LOC_BIGPACK, g_job.fx, g_job.fy);
            job_end("a put down was refused (item put back); stopped");
        }
        break;
    case PH_SETTLE:
        if (loc == LOC_BIGPACK && x == g_job.mv[g_job.k].x && y == g_job.mv[g_job.k].y) {
            if (++g_job.wait >= SETTLE) { g_job.k++; g_job.phase = PH_PICK; g_job.retried = 0; }
        } else if (loc == LOC_CURSOR && !g_job.retried) {
            hg_log("invsort: item %d came back to the cursor; putting it down again",
                   *(int *)(item + 0x2dc));
            g_job.retried = 1;
            put_item(g_job.unit, item, LOC_BIGPACK, g_job.mv[g_job.k].x, g_job.mv[g_job.k].y);
            g_job.phase = PH_WAIT_PUT;
            g_job.wait = 0;
        } else {
            job_end("a put down did not hold; stopped");
        }
        break;
    case PH_SWEEP:
        break;
    }
}

void invsort_click(void *comp)
{
    static ip_item it[IP_MAXITEMS];
    unsigned char *unit, *inv, *recs, *rec, *cur, *item;
    int count, gw, gh, n = 0, m;

    if (g_job.active) { hg_log("invsort: already sorting"); return; }
    unit = (unsigned char *)focus_unit(comp);
    if (!readable(unit, 0x300)) { hg_log("invsort: no unit for the button"); return; }
    inv = *(unsigned char **)(unit + 0x144);
    if (!readable(inv, 0x30)) { hg_log("invsort: unit %p has no inventory", unit); return; }
    recs = *(unsigned char **)(inv + 0xc);
    count = *(int *)(inv + 0x2c);
    if (count <= 0 || count > 256 || !readable(recs, (UINT)count * 0x80)) {
        hg_log("invsort: inventory %p: %d location records at %p?", inv, count, recs);
        return;
    }
    if (!(rec = loc_record(recs, count, LOC_BIGPACK))) { hg_log("invsort: no big pack among %d locations", count); return; }
    cur = loc_record(recs, count, LOC_CURSOR);
    if (cur && *(void **)(cur + 0x6c)) { hg_log("invsort: the cursor holds an item; not sorting"); return; }
    gw = *(int *)(rec + 0x44);
    gh = *(int *)(rec + 0x48);
    if (gw <= 0 || gh <= 0 || gw * gh > IP_MAXCELLS) { hg_log("invsort: big pack %d x %d?", gw, gh); return; }

    for (item = *(unsigned char **)(rec + 0x6c); item; ) {
        unsigned char *node;
        if (n == IP_MAXITEMS) { hg_log("invsort: more than %d items; not sorting", IP_MAXITEMS); return; }
        if (!readable(item, 0x300)) { hg_log("invsort: item %p unreadable; not sorting", item); return; }
        node = *(unsigned char **)(item + 0x140);
        if (!readable(node, 0x34)) { hg_log("invsort: item %p: node unreadable; not sorting", item); return; }
        if (*(unsigned char **)node != unit || *(int *)(node + 0x28) != LOC_BIGPACK) break;
        g_job.units[n] = item;
        it[n].id = *(int *)(item + 0x2dc);
        it[n].cat = item_cat(item);
        it[n].type = *(int *)(item + 0x340);
        it[n].x = *(int *)(node + 0x2c);
        it[n].y = *(int *)(node + 0x30);
        it[n].w = item_stat(item, STAT_INVW);
        it[n].h = item_stat(item, STAT_INVH);
        hg_log("invsort:   item %d type %d -> %s, %d x %d at (%d, %d)", it[n].id, *(int *)(item + 0x340),
               it[n].cat == 0 ? "consumable" : it[n].cat == 1 ? "material" : "gear",
               it[n].w, it[n].h, it[n].x, it[n].y);
        n++;
        item = *(unsigned char **)(node + 0x10);
    }
    m = ip_plan(gw, gh, it, n, g_job.mv, 4 * IP_MAXITEMS);
    if (m < 0) {
        int used = 0, i;
        for (i = 0; i < n; i++) used += it[i].w * it[i].h;
        hg_log("invsort: %d items in %d x %d (%d cells free): no sorted layout can be reached; nothing moved",
               n, gw, gh, gw * gh - used);
        return;
    }
    if (m == 0) { hg_log("invsort: %d items, already sorted", n); return; }
    g_job.unit = unit;
    g_job.cur = cur;
    g_job.n = n;
    g_job.m = m;
    g_job.k = 0;
    g_job.phase = PH_PICK;
    g_job.retried = g_job.sweep = 0;
    g_job.tid = GetCurrentThreadId();
    g_job.active = 1;
    hg_log("invsort: %d items, %d moves planned", n, m);
}
