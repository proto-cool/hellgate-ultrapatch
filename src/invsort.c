/*
 * The inventory's Sort button (docs/spikes/inventory-sort.md).
 *
 * Step 3, reading the backpack: for now a click only logs what is in it,
 * to be checked against the game before any move is sent. Everything runs
 * in the button's click handler, on the main thread, where the game's own
 * inventory UI reads the same structures without a lock.
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
 */
#include <windows.h>
#include "panel.h"

#define VA_FOCUS_UNIT 0x0045a24cu
#define VA_STAT_GET   0x005ffb8au
#define LOC_BIGPACK   0x18
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

static int readable(const void *p, UINT n) { return p && !IsBadReadPtr(p, n); }

static int item_stat(void *item, int stat)
{
    int v = ((stat_fn)(UINT_PTR)VA_STAT_GET)(item, stat, 0);
    return v > 0 ? v : 1;
}

void invsort_click(void *comp)
{
    unsigned char *unit, *inv, *recs, *rec = NULL, *item;
    int count, i, gw, gh, n = 0;

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
    for (i = 0; i < count; i++)
        if (*(int *)(recs + i * 0x80) == LOC_BIGPACK) { rec = recs + i * 0x80; break; }
    if (!rec) { hg_log("invsort: no big pack among %d locations", count); return; }
    gw = *(int *)(rec + 0x44);
    gh = *(int *)(rec + 0x48);
    hg_log("invsort: unit %p id %d, big pack %d x %d (record %d of %d)",
           unit, *(int *)(unit + 0x2dc), gw, gh, i, count);

    for (item = *(unsigned char **)(rec + 0x6c); item && n < 200; n++) {
        unsigned char *node;
        if (!readable(item, 0x300)) { hg_log("invsort:   item %p unreadable", item); break; }
        node = *(unsigned char **)(item + 0x140);
        if (!readable(node, 0x34)) { hg_log("invsort:   item %p: node %p unreadable", item, node); break; }
        if (*(unsigned char **)node != unit || *(int *)(node + 0x28) != LOC_BIGPACK) break;
        hg_log("invsort:   item id %d at (%d, %d) size %d x %d",
               *(int *)(item + 0x2dc), *(int *)(node + 0x2c), *(int *)(node + 0x30),
               item_stat(item, STAT_INVW), item_stat(item, STAT_INVH));
        item = *(unsigned char **)(node + 0x10);
    }
    hg_log("invsort: %d items in the big pack", n);
}
