/*
 * The Ultrapatch tab in the game's Options dialog (docs/spikes/options-page.md).
 *
 * tools/ui/mkuix.py adds the tab to options.xml: a fifth tab button and a
 * panel of rows, every control named "ultra <row> ..." and bound to the
 * handler src/uiext.c takes over, which passes them here by name:
 *   "ultra settings panel"  OnPostActivate: fill every row from the values
 *   "ultra <row> btn"       a checkbox, OnLButtonDown: the stock toggle
 *                           (FUN_005ac82d) first; only if it took the click
 *                           (the message reaches every control), its state
 *   "ultra <row> dn" / "up" a stepper: the value -/+ its step, clamped
 * Values are the saved settings (src/settings.c), found by key, so a change
 * here is live, saved, and shown by the dev panel alike. The dialog's own
 * Accept and Cancel never look at our controls: changes apply at once and
 * Cancel does not undo them (the tab's footer says so).
 *
 * Game functions, all cdecl: FUN_00472a4b(root, name, 0) finds a control
 * under the dialog root (*0x00f274ec); FUN_005ac75a(root, name, on) and
 * FUN_005ac713(root, name) set and read a checkbox; FUN_005b8171(label,
 * text, 0) sets a label's text.
 */
#include <windows.h>
#include <string.h>
#include "panel.h"

#define VA_OPT_ROOT   0x00f274ecu
#define VA_FIND       0x00472a4bu
#define VA_CHECK_SET  0x005ac75au
#define VA_CHECK_GET  0x005ac713u
#define VA_CHECK_DEF  0x005ac82du
#define VA_LABEL_SET  0x005b8171u

typedef void *(__cdecl *find_fn)(void *root, const char *name, int z);
typedef int (__cdecl *check_set_fn)(void *root, const char *name, int on);
typedef int (__cdecl *check_get_fn)(void *root, const char *name);
typedef int (__cdecl *handler_fn)(void *comp, int msg, int wp, int lp);
typedef int (__cdecl *label_fn)(void *label, const WCHAR *text, int z);

/* kept in step with the rows in tools/ui/mkuix.py */
static const struct {
    const char *row;        /* control names: "ultra <row> btn" / "dn" / "up" / "val" */
    const char *key;        /* the setting (src/settings.c) */
    const WCHAR *label;
    int step;               /* 0: a checkbox */
    int percent;            /* show the value with a % sign (signed if the setting can go below 0) */
} g_rows[] = {
    { "ao",       "ao.on",               L"Ambient occlusion",   0,  0 },
    { "fog",      "fog.on",              L"Volumetric fog",      0,  0 },
    { "bloom",    "bloom.on",            L"Bloom",               0,  0 },
    { "grade",    "grade.on",            L"Colour grade",        0,  0 },
    { "pcss",     "shadow.pcss",         L"Soft shadows",        0,  0 },
    { "lights",   "lights.per_pixel",    L"Per-pixel lights",    0,  0 },
    { "smaa",     "smaa.pass",           L"SMAA anti-aliasing",  0,  0 },
    { "plshadow", "pointshadow.on",      L"Point-light shadows", 0,  0 },
    { "hdr",      "hdr.on",              L"HDR (from next start)", 0, 0 },
    { "shafts",   "fog.sun_shafts",      L"Sun shafts",          10, 1 },
    { "density",  "fog.density",         L"Fog density",         10, 0 },
    { "bounce",   "ao.colour_bounce",    L"Colour bounce",       25, 1 },
    { "aostr",    "ao.strength",         L"AO strength",         10, 1 },
    { "bloomi",   "bloom.intensity",     L"Bloom strength",      10, 1 },
    { "sharpen",  "sharpen",             L"Sharpening",          10, 1 },
    { "indoor",   "look.fill_indoors",   L"Indoor light",        10, 1 },
    { "vignette", "grade.vignette",      L"Vignette",            5,  1 },
};
#define NROWS ((int)(sizeof g_rows / sizeof g_rows[0]))

static void *root(void)
{
    void **p = (void **)(UINT_PTR)VA_OPT_ROOT;
    return IsBadReadPtr(p, 4) ? NULL : *p;
}

static void show_value(void *r, int i)
{
    static WCHAR text[NROWS][24];
    char name[48];
    void *label;
    LONG lo;
    volatile LONG *v = settings_find(g_rows[i].key, &lo, NULL);
    if (!v) return;
    wsprintfA(name, "ultra %s val", g_rows[i].row);
    label = ((find_fn)(UINT_PTR)VA_FIND)(r, name, 0);
    if (!label) return;
    wsprintfW(text[i], g_rows[i].percent ? (lo < 0 && *v > 0 ? L"+%ld%%" : L"%ld%%") : L"%ld", *v);
    ((label_fn)(UINT_PTR)VA_LABEL_SET)(label, text[i], 0);
}

static void refresh(void)
{
    void *r = root();
    int i;
    if (!r) return;
    for (i = 0; i < NROWS; i++) {
        volatile LONG *v = settings_find(g_rows[i].key, NULL, NULL);
        char name[48];
        if (!v) continue;
        if (g_rows[i].step) {
            show_value(r, i);
        } else {
            wsprintfA(name, "ultra %s btn", g_rows[i].row);
            ((check_set_fn)(UINT_PTR)VA_CHECK_SET)(r, name, *v ? 1 : 0);
        }
    }
}

static void set_value(int i, LONG v)
{
    LONG lo, hi;
    volatile LONG *var = settings_find(g_rows[i].key, &lo, &hi);
    if (!var) return;
    InterlockedExchange(var, v < lo ? lo : v > hi ? hi : v);
    hg_gfx_knobs_changed();
    hg_log("optpage: %s = %ld", g_rows[i].key, *var);
}

/* A control of ours (its name starts "ultra "); returns 1 if it was handled. */
int optpage_click(void *comp, const char *name, int msg, int wp, int lp, int *ret)
{
    int i, n;
    if (!lstrcmpA(name, "ultra settings panel")) { refresh(); *ret = 1; return 1; }
    for (i = 0; i < NROWS; i++) {
        char want[48];
        n = wsprintfA(want, "ultra %s ", g_rows[i].row);
        if (strncmp(name, want, n)) continue;
        if (!lstrcmpA(name + n, "btn") && !g_rows[i].step) {
            /* OnLButtonDown reaches every control that has one, wherever the
             * click was: the stock toggle says whether it was this one (0:
             * not ours). Acting on every call turned all eight settings off
             * at any click, the dev panel's included (2026-09-23). */
            void *r = root();
            volatile LONG *v = settings_find(g_rows[i].key, NULL, NULL);
            *ret = ((handler_fn)(UINT_PTR)VA_CHECK_DEF)(comp, msg, wp, lp);   /* the stock toggle */
            if (*ret && r && v) {
                int on = ((check_get_fn)(UINT_PTR)VA_CHECK_GET)(r, name) ? 1 : 0;
                if (on != (*v ? 1 : 0)) set_value(i, on);
            }
            return 1;
        }
        if ((!lstrcmpA(name + n, "dn") || !lstrcmpA(name + n, "up")) && g_rows[i].step) {
            volatile LONG *v = settings_find(g_rows[i].key, NULL, NULL);
            void *r = root();
            if (v) set_value(i, *v + (name[n] == 'u' ? g_rows[i].step : -g_rows[i].step));
            if (r) show_value(r, i);
            *ret = 1;
            return 1;
        }
    }
    return 0;
}

/* Our label strings: "ultra opt <row>" is the row's name. */
const WCHAR *optpage_string(const char *key)
{
    int i;
    if (strncmp(key, "ultra opt ", 10)) return NULL;
    for (i = 0; i < NROWS; i++)
        if (!lstrcmpA(key + 10, g_rows[i].row)) return g_rows[i].label;
    if (!lstrcmpA(key + 10, "tab")) return L"Ultrapatch";
    if (!lstrcmpA(key + 10, "val")) return L"-";
    if (!lstrcmpA(key + 10, "footer"))
        return L"Changes apply at once and are saved. Cancel does not undo them.";
    return NULL;
}
