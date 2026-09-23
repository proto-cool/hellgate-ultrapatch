/*
 * The inventory sort's plan (src/invsort.c): where each item goes, and moves
 * that get it there. Plain C with no Windows dependency so test/ui.c can
 * drive it.
 *
 * Layout: items by area, then height, then width (largest first), ties by
 * item id so the same bag always sorts the same way; each placed at the
 * first free cell, row by row. If the sorted layout does not fit (a bag
 * packed tighter than first fit manages), nothing moves.
 *
 * Moves: the game moves an item as pick up, then put down (the cursor holds
 * one item), so a move only needs its target cells free of other items.
 * Repeatedly move any item whose target is free; when every remaining item
 * is blocked, park one blocker on free cells no remaining target needs (or,
 * failing that, any free cells) and go on. If nothing can move, stop: the
 * bag is left valid, just not fully sorted.
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
    if (a->w * a->h != b->w * b->h) return a->w * a->h > b->w * b->h;
    if (a->h != b->h) return a->h > b->h;
    if (a->w != b->w) return a->w > b->w;
    return a->id < b->id;
}

int ip_layout(int gw, int gh, ip_item *it, int n)
{
    unsigned char occ[IP_MAXCELLS];
    signed char owner[IP_MAXCELLS];
    int order[IP_MAXITEMS], i, j, k;
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
        for (y = 0; y < gh && !placed; y++)
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

/* free cells for item k, avoiding the targets of items not yet home if possible */
static int park(const unsigned char *occ, const signed char *owner, int gw, int gh,
                const ip_item *it, int n, const unsigned char *home, int k, int *px, int *py)
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
                if (ok && pass == 0)
                    for (b = y; b < y + it[k].h && ok; b++)
                        for (a = x; a < x + it[k].w && ok; a++) if (want[b * gw + a]) ok = 0;
                if (ok && (x != it[k].x || y != it[k].y)) { *px = x; *py = y; return 1; }
            }
    return 0;
}

int ip_moves(int gw, int gh, ip_item *it, int n, ip_move *mv, int maxmv)
{
    unsigned char occ[IP_MAXCELLS], home[IP_MAXITEMS];
    signed char owner[IP_MAXCELLS];
    int m = 0, i, left, parks = 0;
    if (gw <= 0 || gh <= 0 || gw * gh > IP_MAXCELLS || n < 0 || n > IP_MAXITEMS) return -1;
    memset(occ, 0, sizeof occ);
    memset(owner, -1, sizeof owner);
    for (i = 0; i < n; i++) {
        mark(occ, owner, gw, it[i].x, it[i].y, it[i].w, it[i].h, i);
        home[i] = it[i].x == it[i].tx && it[i].y == it[i].ty;
    }
    for (;;) {
        int moved = 0;
        for (left = 0, i = 0; i < n; i++) left += !home[i];
        if (!left) return m;
        for (i = 0; i < n && m < maxmv; i++) {
            if (home[i] || !fits(occ, gw, gh, it[i].tx, it[i].ty, it[i].w, it[i].h, i, owner)) continue;
            mark(occ, owner, gw, it[i].x, it[i].y, it[i].w, it[i].h, -1);
            mark(occ, owner, gw, it[i].tx, it[i].ty, it[i].w, it[i].h, i);
            it[i].x = it[i].tx; it[i].y = it[i].ty;
            home[i] = 1;
            mv[m].item = i; mv[m].x = it[i].x; mv[m].y = it[i].y; m++;
            moved = 1;
        }
        if (m >= maxmv) return m;
        if (moved) continue;
        /* all blocked: park an item that sits on someone else's target */
        if (++parks > 2 * n) return m;
        for (i = 0; i < n; i++) {
            int px, py, b, a, blocks = 0, j;
            if (home[i]) continue;
            for (j = 0; j < n && !blocks; j++)
                if (!home[j] && j != i)
                    for (b = it[j].ty; b < it[j].ty + it[j].h && !blocks; b++)
                        for (a = it[j].tx; a < it[j].tx + it[j].w; a++)
                            if (owner[b * gw + a] == i) { blocks = 1; break; }
            if (!blocks || !park(occ, owner, gw, gh, it, n, home, i, &px, &py)) continue;
            mark(occ, owner, gw, it[i].x, it[i].y, it[i].w, it[i].h, -1);
            mark(occ, owner, gw, px, py, it[i].w, it[i].h, i);
            it[i].x = px; it[i].y = py;
            mv[m].item = i; mv[m].x = px; mv[m].y = py; m++;
            moved = 1;
            break;
        }
        if (!moved || m >= maxmv) return m;
    }
}
