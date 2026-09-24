/*
 * Saved settings: bin\ultrapatch.ini, one [settings] section of key=value.
 *
 * A module registers each setting a player can change (not debug views or
 * traces) when it installs, before it applies anything that depends on it:
 *   settings_var(key, &var, lo, hi)  loads the saved value into var (clamped;
 *                                    none saved: var keeps its default) and
 *                                    watches it;
 *   settings_get(key, def, lo, hi)   just the saved value, for a setting whose
 *                                    setter does more than store it (engine
 *                                    patches); pair it with settings_watch.
 * settings_present() runs once a frame and writes a watched value that has
 * changed since it was loaded or last written, from the panel or the
 * Options page alike. A missing or unreadable file is the defaults.
 *
 * Each setting keeps its default (the variable's value before the file was
 * read, or settings_get's def) and range, so the panel can show where a
 * value sits and what is changed, and set or reset any setting by key
 * (hg_setting_*). A setting whose setter does more than store the value
 * (an engine patch) registers it with settings_apply; after any set, the
 * shader constants and technique caches are told (hg_settings_touched).
 */
#include <windows.h>
#include <stdlib.h>
#include <limits.h>
#include "panel.h"

#define MAX_SETTINGS 128

static struct {
    const char *key;
    volatile LONG *var;
    LONG last, lo, hi, def;
    void (*apply)(LONG);
} g_set[MAX_SETTINGS];
static int g_nset;

/* settings_get's default, for the settings_watch that follows it */
static const char *g_pend_key;
static LONG g_pend_def, g_pend_lo, g_pend_hi;

void hg_settings_touched(void);     /* src/gfxprobe.c */
static WCHAR g_ini[MAX_PATH];

static const WCHAR *ini_path(void)
{
    if (!g_ini[0]) {
        hg_dll_dir(g_ini, MAX_PATH - 20);
        lstrcatW(g_ini, L"\\ultrapatch.ini");
    }
    return g_ini;
}

LONG settings_get(const char *key, LONG def, LONG lo, LONG hi)
{
    WCHAR wkey[64], buf[32];
    LONG v;
    g_pend_key = key; g_pend_def = def; g_pend_lo = lo; g_pend_hi = hi;
    MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    if (!GetPrivateProfileStringW(L"settings", wkey, L"", buf, 32, ini_path()) || !buf[0]) return def;
    v = (LONG)wcstol(buf, NULL, 10);
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_watch(const char *key, volatile LONG *var)
{
    if (g_nset == MAX_SETTINGS) { hg_log("settings: more than %d settings; %s is not saved", MAX_SETTINGS, key); return; }
    g_set[g_nset].key = key;
    g_set[g_nset].var = var;
    g_set[g_nset].last = *var;
    g_set[g_nset].def = *var;
    g_set[g_nset].lo = LONG_MIN;
    g_set[g_nset].hi = LONG_MAX;
    g_set[g_nset].apply = NULL;
    if (g_pend_key && !lstrcmpA(g_pend_key, key)) {
        g_set[g_nset].def = g_pend_def;
        g_set[g_nset].lo = g_pend_lo;
        g_set[g_nset].hi = g_pend_hi;
    }
    g_pend_key = NULL;
    g_nset++;
}

/* The setter for a setting that is more than its variable (an engine patch). */
void settings_apply(const char *key, void (*apply)(LONG))
{
    int i;
    for (i = 0; i < g_nset; i++)
        if (!lstrcmpA(g_set[i].key, key)) g_set[i].apply = apply;
}

void settings_var(const char *key, volatile LONG *var, LONG lo, LONG hi)
{
    LONG def = *var;
    InterlockedExchange(var, settings_get(key, def, lo, hi));
    settings_watch(key, var);
    if (g_nset && g_set[g_nset - 1].var == var) {
        g_set[g_nset - 1].lo = lo;
        g_set[g_nset - 1].hi = hi;
        g_set[g_nset - 1].def = def;
    }
}

static int find(const char *key)
{
    int i;
    for (i = 0; i < g_nset; i++)
        if (!lstrcmpA(g_set[i].key, key)) return i;
    return -1;
}

/* Panel: a setting's value, default and range; 0 if there is none. */
int hg_setting_get(const char *key, hg_setting *out)
{
    int i = find(key);
    if (i < 0) return 0;
    out->val = *g_set[i].var;
    out->def = g_set[i].def;
    out->lo = g_set[i].lo;
    out->hi = g_set[i].hi;
    return 1;
}

static void set_one(int i, LONG v)
{
    if (v < g_set[i].lo) v = g_set[i].lo;
    if (v > g_set[i].hi) v = g_set[i].hi;
    if (g_set[i].apply) g_set[i].apply(v);
    else InterlockedExchange(g_set[i].var, v);
}

/* Panel: set a setting by key (clamped; saved at the next frame). */
void hg_setting_set(const char *key, long v)
{
    int i = find(key);
    if (i < 0) return;
    set_one(i, v);
    hg_settings_touched();
    hg_log("settings: %s = %ld", key, *g_set[i].var);
}

/* Panel: every listed key back to its default; the count reset. */
int hg_settings_reset(const char *const *keys, int n)
{
    int k, i, c = 0;
    for (k = 0; k < n; k++)
        if ((i = find(keys[k])) >= 0 && *g_set[i].var != g_set[i].def) {
            set_one(i, g_set[i].def);
            c++;
        }
    if (c) hg_settings_touched();
    hg_log("settings: %d setting%s back to default", c, c == 1 ? "" : "s");
    return c;
}

/* A registered setting by key, with its range; NULL if there is none. */
volatile LONG *settings_find(const char *key, LONG *lo, LONG *hi)
{
    int i;
    for (i = 0; i < g_nset; i++)
        if (!lstrcmpA(g_set[i].key, key)) {
            if (lo) *lo = g_set[i].lo;
            if (hi) *hi = g_set[i].hi;
            return g_set[i].var;
        }
    return NULL;
}

/* Once a frame: save what changed. Cheap when nothing did. */
void settings_present(void)
{
    int i, wrote = 0;
    for (i = 0; i < g_nset; i++) {
        LONG v = *g_set[i].var;
        WCHAR wkey[64], buf[16];
        if (v == g_set[i].last) continue;
        g_set[i].last = v;
        MultiByteToWideChar(CP_ACP, 0, g_set[i].key, -1, wkey, 64);
        wsprintfW(buf, L"%ld", v);
        WritePrivateProfileStringW(L"settings", wkey, buf, ini_path());
        wrote++;
    }
    if (wrote) hg_log("settings: saved %d change%s to ultrapatch.ini", wrote, wrote == 1 ? "" : "s");
}
