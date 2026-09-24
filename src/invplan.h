/* The inventory sort's plan (src/invplan.c). */
#ifndef HG_INVPLAN_H
#define HG_INVPLAN_H

#define IP_MAXITEMS 128
#define IP_MAXCELLS 256

typedef struct {
    int id;             /* the game's item id; ties in the layout go by it */
    int cat;            /* category, sorted first: 0 consumables, 1 materials, 2 gear */
    int x, y, w, h;     /* where it is now, and its size in cells */
    int tx, ty;         /* where the layout puts it */
} ip_item;

typedef struct { int item, x, y; } ip_move;   /* item index, target cell */

/* Fills tx/ty; 0 if the sorted layout does not fit (then nothing should move).
 * bands: each category starts on a new row. */
int ip_layout(int gw, int gh, ip_item *it, int n, int bands);

/* Moves from x/y towards tx/ty, each onto cells no other item holds at that
 * point; updates x/y. Returns the count (all items home if nothing is
 * blocked), -1 on bad input. */
int ip_moves(int gw, int gh, ip_item *it, int n, ip_move *mv, int maxmv);

/* The sort: the banded layout if it fits and every item can get home, else
 * the packed one. Returns the move count (0: already sorted) with x/y and
 * tx/ty at the result, or -1 if neither layout can be reached (it is then
 * unchanged: nothing should move). */
int ip_plan(int gw, int gh, ip_item *it, int n, ip_move *mv, int maxmv);

#endif
