/*
 * The inventory sort's plan (src/invsort.c): where each item goes, and moves
 * that get it there. Plain C with no Windows dependency so test/ui.c can
 * drive it.
 *
 * Layout: items by category (consumables, materials, gear: the top of the
 * bag to the bottom), then area, height and width (largest first), ties by
 * where the item is now (row by row), so a sorted bag stays as it is; each
 * placed at the first free cell, row by row. Banded, each category starts
 * on a new row, so the bag reads as three bands; packed, a category runs on
 * in the row the last one ended in. If the sorted layout does not fit (a
 * bag packed tighter than first fit manages), nothing moves.
 *
 * Moves: the game moves an item as pick up, then put down (the cursor holds
 * one item), so a move only needs its target cells free of other items.
 * Repeatedly move any item whose target is free. When every remaining item
 * is blocked, take the largest whose blockers can all be parked off its
 * target (on cells no other remaining target needs, if possible), park
 * them and move it in: every round sends at least one item home, so this
 * ends. If no item can be freed, stop: the bag is left valid, not sorted.
 *
 * ip_plan runs only a plan that gets every item home: banded, else packed,
 * else nothing. A near-full bag (7 free cells of 72, 2026-09-23) has no room
 * to park its 2-cell gear, and running the moves found up to the dead end
 * shuffled a few items and stopped, which read as a broken button.
 */
#include <string.h>
#include "invplan.h"

static int fits(const unsigned char *occ, int gw, int gh, int x, int y, int w, int h, int self, const signed char *owner)
{
    int i, j;
    if (x < 0 || y < 0 || x + w > gw || y + h > gh) return 0;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (occ[j * gw + i] && owner[j * gw + i] != self) return 0;
    return 1;
}

static void mark(unsigned char *occ, signed char *owner, int gw, int x, int y, int w, int h, int who)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) {
            occ[j * gw + i] = who >= 0;
            owner[j * gw + i] = (signed char)who;
        }
}

static int bigger(const ip_item *a, const ip_item *b)
{
    if (a->cat != b->cat) return a->cat < b->cat;
    if (a->w * a->h != b->w * b->h) return a->w * a->h > b->w * b->h;
    if (a->h != b->h) return a->h > b->h;
    if (a->w != b->w) return a->w > b->w;
    if (a->y != b->y) return a->y < b->y;
    if (a->x != b->x) return a->x < b->x;
    return a->id < b->id;
}

int ip_layout(int gw, int gh, ip_item *it, int n, int bands)
{
    unsigned char occ[IP_MAXCELLS];
    signed char owner[IP_MAXCELLS];
    int order[IP_MAXITEMS], i, j, k, band = 0;
    if (gw <= 0 || gh <= 0 || gw * gh > IP_MAXCELLS || n < 0 || n > IP_MAXITEMS) return 0;
    for (i = 0; i < n; i++) order[i] = i;
    for (i = 1; i < n; i++)                            /* insertion sort: n is small */
        for (j = i; j > 0 && bigger(&it[order[j]], &it[order[j - 1]]); j--) {
            k = order[j]; order[j] = order[j - 1]; order[j - 1] = k;
        }
    memset(occ, 0, sizeof occ);
    memset(owner, -1, sizeof owner);
    for (k = 0; k < n; k++) {
        ip_item *t = &it[order[k]];
        int placed = 0, x, y;
        if (bands && k > 0 && t->cat != it[order[k - 1]].cat)
            for (i = 0; i < k; i++) {                  /* a new category: below everything placed */
                ip_item *p = &it[order[i]];
                if (p->ty + p->h > band) band = p->ty + p->h;
            }
        for (y = bands ? band : 0; y < gh && !placed; y++)
            for (x = 0; x < gw && !placed; x++)
                if (fits(occ, gw, gh, x, y, t->w, t->h, -2, owner)) {
                    t->tx = x; t->ty = y;
                    mark(occ, owner, gw, x, y, t->w, t->h, order[k]);
                    placed = 1;
                }
        if (!placed) return 0;
    }
    return 1;
}

/* free cells for item k outside the cells in forbid, avoiding the targets of
 * items not yet home where possible; 0 if there are none */
static int park(const unsigned char *occ, const signed char *owner, int gw, int gh,
                const ip_item *it, int n, const unsigned char *home, const unsigned char *forbid,
                int k, int *px, int *py)
{
    unsigned char want[IP_MAXCELLS];
    int pass, x, y, i, a, b;
    memset(want, 0, sizeof want);
    for (i = 0; i < n; i++)
        if (!home[i] && i != k)
            for (b = it[i].ty; b < it[i].ty + it[i].h; b++)
                for (a = it[i].tx; a < it[i].tx + it[i].w; a++) want[b * gw + a] = 1;
    for (pass = 0; pass < 2; pass++)
        for (y = 0; y < gh; y++)
            for (x = 0; x < gw; x++) {
                int ok = fits(occ, gw, gh, x, y, it[k].w, it[k].h, -2, owner);
                for (b = y; b < y + it[k].h && ok; b++)
                    for (a = x; a < x + it[k].w && ok; a++)
                        if (forbid[b * gw + a] || (pass == 0 && want[b * gw + a])) ok = 0;
                if (ok) { *px = x; *py = y; return 1; }
            }
    return 0;
}

static void ip_place(unsigned char *occ, signed char *owner, int gw, ip_item *t, int who, int x, int y,
                  ip_move *mv, int *m)
{
    mark(occ, owner, gw, t->x, t->y, t->w, t->h, -1);
    mark(occ, owner, gw, x, y, t->w, t->h, who);
    t->x = x; t->y = y;
    mv[*m].item = who; mv[*m].x = x; mv[*m].y = y; (*m)++;
}

int ip_moves(int gw, int gh, ip_item *it, int n, ip_move *mv, int maxmv)
{
    unsigned char occ[IP_MAXCELLS], home[IP_MAXITEMS];
    signed char owner[IP_MAXCELLS];
    int order[IP_MAXITEMS], m = 0, i, j, o, round;
    if (gw <= 0 || gh <= 0 || gw * gh > IP_MAXCELLS || n < 0 || n > IP_MAXITEMS) return -1;
    for (i = 0; i < n; i++) order[i] = i;              /* largest first, as the layout */
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && bigger(&it[order[j]], &it[order[j - 1]]); j--) {
            o = order[j]; order[j] = order[j - 1]; order[j - 1] = o;
        }
    memset(occ, 0, sizeof occ);
    memset(owner, -1, sizeof owner);
    for (i = 0; i < n; i++) {
        mark(occ, owner, gw, it[i].x, it[i].y, it[i].w, it[i].h, i);
        home[i] = it[i].x == it[i].tx && it[i].y == it[i].ty;
    }
    /* each round sends at least one item home, or stops */
    for (round = 0; round <= n; round++) {
        int moved = 0, left = 0;
        for (i = 0; i < n; i++) left += !home[i];
        if (!left) return m;
        for (i = 0; i < n; i++) {
            if (home[i] || m >= maxmv || !fits(occ, gw, gh, it[i].tx, it[i].ty, it[i].w, it[i].h, i, owner)) continue;
            ip_place(occ, owner, gw, &it[i], i, it[i].tx, it[i].ty, mv, &m);
            home[i] = 1;
            moved = 1;
        }
        if (moved) continue;
        /* all blocked: the largest item not home whose blockers can all be
         * parked off its target; park them, then move it in */
        for (j = -1, o = 0; o < n; o++) {
            unsigned char forbid[IP_MAXCELLS], socc[IP_MAXCELLS];
            signed char sown[IP_MAXCELLS];
            int blk[IP_MAXITEMS], pxy[IP_MAXITEMS][2], nb = 0, a, b, k, ok = 1;
            i = order[o];
            if (home[i]) continue;
            memset(forbid, 0, sizeof forbid);
            for (b = it[i].ty; b < it[i].ty + it[i].h; b++)
                for (a = it[i].tx; a < it[i].tx + it[i].w; a++) {
                    int o = owner[b * gw + a];
                    forbid[b * gw + a] = 1;
                    if (o >= 0 && o != i) {
                        for (k = 0; k < nb && blk[k] != o; k++) ;
                        if (k == nb) blk[nb++] = o;
                    }
                }
            memcpy(socc, occ, sizeof socc);
            memcpy(sown, owner, sizeof sown);
            /* the item's own cells stay taken while it waits */
            for (k = 0; k < nb && ok; k++) {
                ip_item *t = &it[blk[k]];
                ok = park(socc, sown, gw, gh, it, n, home, forbid, blk[k], &pxy[k][0], &pxy[k][1]);
                if (ok) {
                    mark(socc, sown, gw, t->x, t->y, t->w, t->h, -1);
                    mark(socc, sown, gw, pxy[k][0], pxy[k][1], t->w, t->h, blk[k]);
                }
            }
            if (!ok || m + nb + 1 > maxmv) continue;
            for (k = 0; k < nb; k++) {
                ip_place(occ, owner, gw, &it[blk[k]], blk[k], pxy[k][0], pxy[k][1], mv, &m);
                home[blk[k]] = it[blk[k]].x == it[blk[k]].tx && it[blk[k]].y == it[blk[k]].ty;
            }
            j = i;
            break;
        }
        if (j < 0) return m;                 /* nothing can be freed: stop, the bag is valid */
        ip_place(occ, owner, gw, &it[j], j, it[j].tx, it[j].ty, mv, &m);
        home[j] = 1;
    }
    return m;
}

int ip_plan(int gw, int gh, ip_item *it, int n, ip_move *mv, int maxmv)
{
    ip_item save[IP_MAXITEMS];
    int bands, m, i, home;
    if (n < 0 || n > IP_MAXITEMS) return -1;
    memcpy(save, it, n * sizeof *it);
    for (bands = 1; bands >= 0; bands--) {
        memcpy(it, save, n * sizeof *it);
        if (!ip_layout(gw, gh, it, n, bands)) continue;
        m = ip_moves(gw, gh, it, n, mv, maxmv);
        for (home = 1, i = 0; i < n; i++)
            if (it[i].x != it[i].tx || it[i].y != it[i].ty) home = 0;
        if (m >= 0 && home) return m;
    }
    memcpy(it, save, n * sizeof *it);
    return -1;
}
