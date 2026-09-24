/*
 * The Ultrapatch tab in the game's Options dialog (docs/spikes/options-page.md).
 *
 * tools/ui/mkuix.py adds the tab to options.xml: a fifth tab button and a
 * panel in the Video tab's style (section bars, the Look dropdown,
 * checkboxes and -/+ steppers in two columns, a defaults button), every
 * control named "ultra ..." and bound to the handler src/uiext.c takes over,
 * which passes them here by name:
 *   "ultra settings panel"  OnPostActivate: fill every row and the dropdown
 *   "ultra <row> btn"       a checkbox, OnLButtonDown: the stock toggle
 *                           (FUN_005ac82d) first; only if it took the click
 *                           (the message reaches every control), its state
 *   "ultra <row> dn" / "up" a stepper: the value -/+ its step, clamped
 *   "ultra defaults btn"    every setting on the tab back to its default
 * The dropdown ("ultra look combo": Default, 2007, and Custom while the four
 * LOOK values match neither) has no select handler of ours; optpage_tick
 * reads its selection once a frame and applies a change.
 * Values are the saved settings (src/settings.c), set by key, so a change
 * here is live, saved, and shown by the dev panel alike. The dialog's own
 * Accept and Cancel never look at our controls: changes apply at once and
 * Cancel does not undo them (the tab's footer says so).
 *
 * Game functions: FUN_00472a4b(root, name, 0), cdecl, finds a control under
 * the UI root (*0x00f274ec); FUN_005ac75a(root, name, on) and
 * FUN_005ac713(root, name), cdecl, set and read a checkbox; FUN_005b8171
 * (label, text, 0), cdecl, sets a label's text. Combo boxes (type 0xe at
 * +0x98; the list at +0x280), from the aspect-ratio combo (FUN_005224dd):
 * FUN_005a7fe2 clears the list (list in EAX); FUN_005a8953 adds an item
 * (list in ESI, EAX 0; text, 0, 0, 0 on the stack, caller pops);
 * FUN_005a8da8 selects (combo in EAX; index, 1, 1, caller pops);
 * FUN_005a8aaf(root, name), cdecl, reads the selection (-1: none).
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
#define VA_CB_CLEAR   0x005a7fe2u
#define VA_CB_ADD     0x005a8953u
#define VA_CB_SELECT  0x005a8da8u
#define VA_CB_GETSEL  0x005a8aafu
#define UI_COMBOBOX   0xe

typedef void *(__cdecl *find_fn)(void *root, const char *name, int z);
typedef int (__cdecl *check_set_fn)(void *root, const char *name, int on);
typedef int (__cdecl *check_get_fn)(void *root, const char *name);
typedef int (__cdecl *handler_fn)(void *comp, int msg, int wp, int lp);
typedef int (__cdecl *label_fn)(void *label, const WCHAR *text, int z);
typedef int (__cdecl *getsel_fn)(void *root, const char *name);

/* kept in step with OPT_CHECKS and OPT_STEPS in tools/ui/mkuix.py */
static const struct { const char *row, *key; const WCHAR *label; } g_checks[] = {
    { "hdr",      "hdr.on",            L"HDR" },
    { "ao",       "ao.on",             L"Ambient occlusion" },
    { "fog",      "fog.on",            L"Volumetric fog" },
    { "bloom",    "bloom.on",          L"Bloom" },
    { "grade",    "grade.on",          L"Colour grade" },
    { "pcss",     "shadow.pcss",       L"Soft shadows" },
    { "lights",   "lights.per_pixel",  L"Per-pixel lights" },
    { "plshadow", "pointshadow.on",    L"Point-light shadows" },
};
static const struct {
    const char *row, *key;
    const WCHAR *label;
    int step;
    int rel;                /* shown as a percentage of the default, not the raw value */
} g_steps[] = {
    { "shafts",   "fog.sun_shafts",   L"Sun shafts",     10, 0 },
    { "density",  "fog.density",      L"Fog density",    5,  1 },
    { "aostr",    "ao.strength",      L"AO strength",    10, 0 },
    { "bounce",   "ao.colour_bounce", L"Colour bounce",  25, 0 },
    { "bloomi",   "bloom.intensity",  L"Bloom strength", 10, 0 },
    { "sharpen",  "sharpen",          L"Sharpening",     10, 0 },
    { "vignette", "grade.vignette",   L"Vignette",       5,  0 },
};
#define NCHECKS ((int)(sizeof g_checks / sizeof g_checks[0]))
#define NSTEPS ((int)(sizeof g_steps / sizeof g_steps[0]))

/* the LOOK values the dropdown drives, and its presets (src/gfxprobe.c) */
static const char *const g_look_keys[4] = { "look.fill", "look.fill_indoors", "look.fog_start", "look.sun" };
static const long g_look_2007[4] = { -60, -60, 20, 20 };
static int g_look_items;        /* items in the dropdown now (3 with Custom) */
static int g_look_shown = -1;   /* the selection we set */

static void *root(void)
{
    void **p = (void **)(UINT_PTR)VA_OPT_ROOT;
    return IsBadReadPtr(p, 4) ? NULL : *p;
}

static void *find(void *r, const char *name)
{
    return r ? ((find_fn)(UINT_PTR)VA_FIND)(r, name, 0) : NULL;
}

static void show_value(void *r, int i)
{
    static WCHAR text[NSTEPS][24];
    char name[48];
    void *label;
    hg_setting st;
    if (!hg_setting_get(g_steps[i].key, &st)) return;
    wsprintfA(name, "ultra %s val", g_steps[i].row);
    if (!(label = find(r, name))) return;
    wsprintfW(text[i], L"%ld%%", g_steps[i].rel && st.def ? st.val * 100 / st.def : st.val);
    ((label_fn)(UINT_PTR)VA_LABEL_SET)(label, text[i], 0);
}

/* 0 stock, 1 the 2007 look, 2 anything else */
static int look_now(void)
{
    int k, stock = 1, y2007 = 1;
    for (k = 0; k < 4; k++) {
        hg_setting st;
        if (!hg_setting_get(g_look_keys[k], &st)) return 2;
        if (st.val != 0) stock = 0;
        if (st.val != g_look_2007[k]) y2007 = 0;
    }
    return stock ? 0 : y2007 ? 1 : 2;
}

static void combo_add(void *list, const WCHAR *text)
{
    __asm__ volatile ("pushl $0\n\tpushl $0\n\tpushl $0\n\tpushl %[t]\n\t"
                      "xorl %%eax, %%eax\n\t"
                      "call *%[fn]\n\t"
                      "addl $16, %%esp"
                      : : [t] "r"(text), "S"(list), [fn] "r"(VA_CB_ADD)
                      : "eax", "ecx", "edx", "memory", "cc");
}

static void combo_select(void *combo, int index)
{
    __asm__ volatile ("pushl $1\n\tpushl $1\n\tpushl %[i]\n\t"
                      "call *%[fn]\n\t"
                      "addl $12, %%esp"
                      : "+a"(combo) : [i] "r"(index), [fn] "r"(VA_CB_SELECT)
                      : "ecx", "edx", "memory", "cc");
}

/* the dropdown: Default, 2007, and Custom while the values match neither */
static void fill_look(void *r)
{
    unsigned char *combo = (unsigned char *)find(r, "ultra look combo");
    void *list;
    int now = look_now();
    if (!combo || IsBadReadPtr(combo, 0x284) || *(int *)(combo + 0x98) != UI_COMBOBOX) return;
    if (!(list = *(void **)(combo + 0x280))) return;
    __asm__ volatile ("call *%[fn]" : "+a"(list) : [fn] "r"(VA_CB_CLEAR) : "ecx", "edx", "memory", "cc");
    list = *(void **)(combo + 0x280);
    combo_add(list, L"Default");
    combo_add(list, L"2007");
    g_look_items = 2;
    if (now == 2) { combo_add(list, L"Custom"); g_look_items = 3; }
    combo_select(combo, now);
    g_look_shown = now;
}

static void refresh(void)
{
    void *r = root();
    int i;
    if (!r) return;
    for (i = 0; i < NCHECKS; i++) {
        hg_setting st;
        char name[48];
        if (!hg_setting_get(g_checks[i].key, &st)) continue;
        wsprintfA(name, "ultra %s btn", g_checks[i].row);
        ((check_set_fn)(UINT_PTR)VA_CHECK_SET)(r, name, st.val ? 1 : 0);
    }
    for (i = 0; i < NSTEPS; i++) show_value(r, i);
    fill_look(r);
}

/* Present, once a frame: the dropdown's pick, if it changed. */
void optpage_tick(void)
{
    void *r;
    unsigned char *combo;
    int sel;
    if (g_look_shown < 0 || !(r = root())) return;
    combo = (unsigned char *)find(r, "ultra look combo");
    if (!combo || IsBadReadPtr(combo, 0x284) || *(int *)(combo + 0x98) != UI_COMBOBOX) return;
    sel = ((getsel_fn)(UINT_PTR)VA_CB_GETSEL)(r, "ultra look combo");
    if (sel < 0 || sel == g_look_shown || sel >= g_look_items) return;
    g_look_shown = sel;
    if (sel == 2) return;               /* Custom: what is set now */
    hg_gfx_nudge_look(-1, sel);         /* 0 stock, 1 the 2007 look */
    hg_log("optpage: look %s", sel ? "2007" : "default");
    if (g_look_items == 3) fill_look(r);    /* no longer custom */
}

/* A control of ours (its name starts "ultra "); returns 1 if it was handled. */
int optpage_click(void *comp, const char *name, int msg, int wp, int lp, int *ret)
{
    int i, n;
    if (!lstrcmpA(name, "ultra settings panel")) { refresh(); *ret = 1; return 1; }
    if (!lstrcmpA(name, "ultra defaults btn")) {
        const char *keys[NCHECKS + NSTEPS + 4];
        int k = 0;
        for (i = 0; i < NCHECKS; i++) keys[k++] = g_checks[i].key;
        for (i = 0; i < NSTEPS; i++) keys[k++] = g_steps[i].key;
        for (i = 0; i < 4; i++) keys[k++] = g_look_keys[i];
        hg_settings_reset(keys, k);
        refresh();
        *ret = 1;
        return 1;
    }
    for (i = 0; i < NCHECKS; i++) {
        char want[48];
        void *r;
        hg_setting st;
        wsprintfA(want, "ultra %s btn", g_checks[i].row);
        if (lstrcmpA(name, want)) continue;
        /* OnLButtonDown reaches every control that has one, wherever the
         * click was: the stock toggle says whether it was this one (0: not
         * ours). Acting on every call turned all eight settings off at any
         * click, the dev panel's included (2026-09-23). */
        *ret = ((handler_fn)(UINT_PTR)VA_CHECK_DEF)(comp, msg, wp, lp);   /* the stock toggle */
        if (*ret && (r = root()) && hg_setting_get(g_checks[i].key, &st)) {
            int on = ((check_get_fn)(UINT_PTR)VA_CHECK_GET)(r, name) ? 1 : 0;
            if (on != (st.val ? 1 : 0)) hg_setting_set(g_checks[i].key, on);
        }
        return 1;
    }
    for (i = 0; i < NSTEPS; i++) {
        char want[48];
        hg_setting st;
        n = wsprintfA(want, "ultra %s ", g_steps[i].row);
        if (strncmp(name, want, n) || (lstrcmpA(name + n, "dn") && lstrcmpA(name + n, "up"))) continue;
        if (hg_setting_get(g_steps[i].key, &st))
            hg_setting_set(g_steps[i].key, st.val + (name[n] == 'u' ? g_steps[i].step : -g_steps[i].step));
        show_value(root(), i);
        *ret = 1;
        return 1;
    }
    return 0;
}

/* Our label strings: "ultra opt <row>" is the row's name. */
const WCHAR *optpage_string(const char *key)
{
    static const struct { const char *k; const WCHAR *s; } fixed[] = {
        { "tab", L"Ultrapatch" },
        { "val", L"-" },
        { "look", L"Look" },
        { "look tip", L"Default: the 2018 lighting as shipped. 2007: the retail game's flatter ambient, later fog and stronger sun." },
        { "sec look", L"LOOK" },
        { "sec effects", L"EFFECTS" },
        { "sec tuning", L"TUNING" },
        { "defaults", L"Restore defaults" },
        { "footer", L"Changes apply at once and are saved. Cancel does not undo them." },
    };
    int i;
    if (strncmp(key, "ultra opt ", 10)) return NULL;
    key += 10;
    for (i = 0; i < NCHECKS; i++) if (!lstrcmpA(key, g_checks[i].row)) return g_checks[i].label;
    for (i = 0; i < NSTEPS; i++) if (!lstrcmpA(key, g_steps[i].row)) return g_steps[i].label;
    for (i = 0; i < (int)(sizeof fixed / sizeof fixed[0]); i++) if (!lstrcmpA(key, fixed[i].k)) return fixed[i].s;
    return NULL;
}
