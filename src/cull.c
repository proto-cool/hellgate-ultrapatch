/*
 * The engine's occlusion culling, observed and switchable (Graphics debug).
 *
 * Visibility goes through umbra.dll: the engine derives an Umbra::Commander,
 * calls Camera::resolveVisibility, and answers Umbra's occlusion tests with
 * real GPU queries: Umbra hands it a test shape (OcclusionQuery::getVertices /
 * getTriangles), the engine draws it against the scene depth
 * (OcclusionMap.fxo) and reports back through OcclusionQuery::setResult
 * (bool visible, int). A floor piece of a random level's room vanished at a
 * seam, showing the water plane under it (2026-09-23); with "all visible"
 * every query answers visible, so nothing is culled by occlusion (the view
 * frustum and the portals still cull). The counts say how often the queries
 * answered hidden.
 *
 * Camera::setParameters(width, height, properties, ...) is logged once, so
 * the properties Umbra was given are on record.
 */
#include <windows.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

typedef void (__attribute__((thiscall)) *setresult_fn)(void *self, int visible, int n);
typedef void (__attribute__((thiscall)) *setparams_fn)(void *self, int w, int h, unsigned int props, float a, float b);

static setresult_fn o_setresult;
static setparams_fn o_setparams;
static volatile LONG g_all_visible;     /* the toggle (debug, not saved) */
static volatile LONG g_tests, g_hidden, g_forced;
static int g_hooked;

static void __attribute__((thiscall)) d_setresult(void *self, int visible, int n)
{
    InterlockedIncrement(&g_tests);
    if (!(visible & 0xff)) {
        InterlockedIncrement(&g_hidden);
        if (g_all_visible) { InterlockedIncrement(&g_forced); visible = 1; }
    }
    o_setresult(self, visible, n);
}

static void __attribute__((thiscall)) d_setparams(void *self, int w, int h, unsigned int props, float a, float b)
{
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 4)
        hg_log("cull: Umbra camera %dx%d, properties 0x%08x, %.3f %.3f", w, h, props, a, b);
    o_setparams(self, w, h, props, a, b);
}

static void hook(HMODULE m, const char *sym, void *detour, void **orig)
{
    void *t = m ? (void *)GetProcAddress(m, sym) : NULL;
    if (t && MH_CreateHook(t, detour, orig) == MH_OK && MH_EnableHook(t) == MH_OK) g_hooked++;
    else hg_log("cull: could not hook %s", sym);
}

void cull_install(void)
{
    HMODULE m = GetModuleHandleA("umbra.dll");
    if (!m) { hg_log("cull: umbra.dll not loaded"); return; }
    hook(m, "?setResult@OcclusionQuery@Commander@Umbra@@QAEX_NH@Z", (void *)d_setresult, (void **)&o_setresult);
    hook(m, "?setParameters@Camera@Umbra@@QAEXHHIMM@Z", (void *)d_setparams, (void **)&o_setparams);
    hg_log("cull: %d of 2 Umbra hooks in", g_hooked);
}

/* Panel. */
void hg_cull_set_all_visible(int on)
{
    InterlockedExchange(&g_all_visible, on ? 1 : 0);
    hg_log("cull: occlusion tests %s (so far %ld tests, %ld hidden)", on ? "ALL VISIBLE" : "as the GPU answers",
           g_tests, g_hidden);
}
int hg_cull_all_visible(void) { return (int)g_all_visible; }
void hg_cull_counts(long *tests, long *hidden, int *hooked)
{
    *tests = g_tests;
    *hidden = g_hidden;
    *hooked = g_hooked;
}
