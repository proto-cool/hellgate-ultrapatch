/*
 * The inventory's Sort button (docs/spikes/inventory-sort.md).
 *
 * A click reads the backpack, plans a sorted layout (src/invplan.c) and
 * moves each item there the way a drag does: pick up to the cursor, put
 * down at x, y, through the game's own helper, so the server checks every
 * move and a refused one only leaves the item where it was. Nothing moves
 * if the cursor already holds an item or the sorted layout does not fit.
 * Everything runs in the button's click handler, on the main thread, where
 * the game's own inventory UI reads the same structures without a lock.
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
 */
#include <windows.h>
#include "panel.h"
#include "invplan.h"

#define VA_FOCUS_UNIT 0x0045a24cu
#define VA_STAT_GET   0x005ffb8au
#define VA_PUT_ITEM   0x006309fau
#define LOC_BIGPACK   0x18
#define LOC_CURSOR    0x36
#define STAT_INVW     0xde
#define STAT_INVH     0xdf

typedef int (__cdecl *stat_fn)(void *unit, int stat, int param);

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

void invsort_click(void *comp)
{
    static ip_item it[IP_MAXITEMS];
    static void *units[IP_MAXITEMS];
    static ip_move mv[4 * IP_MAXITEMS];
    unsigned char *unit, *inv, *recs, *rec, *cur, *item;
    int count, gw, gh, n = 0, m, k;

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
        units[n] = item;
        it[n].id = *(int *)(item + 0x2dc);
        it[n].x = *(int *)(node + 0x2c);
        it[n].y = *(int *)(node + 0x30);
        it[n].w = item_stat(item, STAT_INVW);
        it[n].h = item_stat(item, STAT_INVH);
        n++;
        item = *(unsigned char **)(node + 0x10);
    }
    if (!ip_layout(gw, gh, it, n)) { hg_log("invsort: %d items in %d x %d: the sorted layout does not fit; nothing moved", n, gw, gh); return; }
    m = ip_moves(gw, gh, it, n, mv, 4 * IP_MAXITEMS);
    if (m < 0) return;
    for (k = 0; k < m; k++) {
        put_item(unit, units[mv[k].item], LOC_CURSOR, 0, 0);
        put_item(unit, units[mv[k].item], LOC_BIGPACK, mv[k].x, mv[k].y);
    }
    for (k = 0; k < n; k++)
        if (it[k].x != it[k].tx || it[k].y != it[k].ty) break;
    hg_log("invsort: %d items, %d moves sent%s", n, m, k < n ? " (bag too full to finish; left valid)" : "");
}
