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
 */
#include <windows.h>
#include <stdlib.h>
#include <limits.h>
#include "panel.h"

#define MAX_SETTINGS 128

static struct { const char *key; volatile LONG *var; LONG last, lo, hi; } g_set[MAX_SETTINGS];
static int g_nset;
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
    g_set[g_nset].lo = LONG_MIN;
    g_set[g_nset].hi = LONG_MAX;
    g_nset++;
}

void settings_var(const char *key, volatile LONG *var, LONG lo, LONG hi)
{
    InterlockedExchange(var, settings_get(key, *var, lo, hi));
    settings_watch(key, var);
    if (g_nset && g_set[g_nset - 1].var == var) { g_set[g_nset - 1].lo = lo; g_set[g_nset - 1].hi = hi; }
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
