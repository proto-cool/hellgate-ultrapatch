/*
 * Internal HDR (docs/spikes/hdr.md): the 3D scene draws into a 16-bit float
 * target of ours instead of the back buffer, so light above 1 survives
 * until one tone map writes the 8-bit back buffer, just before the UI.
 *
 * The engine keeps the real back buffer's pointer and binds it again and
 * again (dx9_SetRenderTarget), so the redirect sits at the device: while
 * the frame is in its scene phase,
 *   SetRenderTarget(0, back buffer)  binds our float surface instead;
 *   GetRenderTarget(0)               answers the back buffer when ours is
 *                                    bound, so the engine's save and restore
 *                                    (and our passes') round-trip through it;
 *   StretchRect                      reads and writes ours for the back
 *                                    buffer (the engine's glow copy, AO's
 *                                    bounce copy).
 * Our code goes through the same hooks, so the passes that name the back
 * buffer draw on the float scene without knowing.
 *
 * The phase starts after every Present (and at creation) and ends at the
 * resolve: src/postfx.c's composite (bloom and the grade) reads the float
 * scene and writes the back buffer, at the first UI pass, or at Present on a
 * frame that never reached one. Anything left in the phase at Present is
 * copied over as it is (hdr_finish), so a frame without our passes (menus,
 * loading screens, postfx failed) still shows.
 *
 * Needs the SMAA device path (no MSAA, so the float target matches the
 * INTZ depth). On by default; the setting (panel, Options tab) switches it
 * at the next frame; off, the hooks pass everything through.
 */
#include <windows.h>
#include <stddef.h>
#include <math.h>
#include <d3d9.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

typedef HRESULT (WINAPI *set_rt_fn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (WINAPI *get_rt_fn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 **);
typedef HRESULT (WINAPI *stretch_fn)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *,
                                     IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);

static set_rt_fn  o_set_rt;
static get_rt_fn  o_get_rt;
static stretch_fn o_stretch;

static volatile LONG g_want = 1;        /* the setting (hdr.on) */
static LONG g_live;                     /* this device: the float scene exists */
static IDirect3DDevice9 *g_dev;
static IDirect3DSurface9 *g_bb;         /* the real back buffer (no reference kept: the swap chain holds it) */
static IDirect3DTexture9 *g_tex;        /* the float scene, A16B16G16R16F, back-buffer size */
static IDirect3DSurface9 *g_scene;      /* its level 0 */
static volatile LONG g_phase;           /* 1: the scene draws into g_scene */
static LONG g_stretch_fail, g_copies;
static volatile LONG g_tm = 1;          /* the tone map, and the materials unclamped (A/B at run time) */
static volatile LONG g_exposure = 100;  /* percent */
static volatile LONG g_knee = 80;       /* where the shoulder starts, percent of white */
static volatile LONG g_auto = 50;       /* auto exposure: share of the way to the target middle, percent (0 off) */
static volatile LONG g_auto_key = 80;   /* the target middle: log-average scene luminance x1000 */
static volatile LONG g_auto_stops = 10; /* the most it moves exposure, stops x10, either way */
static volatile LONG g_spill = 50;     /* highlight spill above white, percent (0: hue kept; 100 blew spells out to white) */
static volatile LONG g_bloom_thr = 100; /* bloom from this brightness up, percent of white (the stock
                                           path's threshold is on the display's 0..1 instead) */

void hdr_begin_frame(IDirect3DDevice9 *dev);

/* ------------------------------------------------------------------ */

static HRESULT WINAPI d_set_rt(IDirect3DDevice9 *dev, DWORD i, IDirect3DSurface9 *s)
{
    if (i == 0 && s && s == g_bb && g_phase && dev == g_dev) s = g_scene;
    return o_set_rt(dev, i, s);
}

static HRESULT WINAPI d_get_rt(IDirect3DDevice9 *dev, DWORD i, IDirect3DSurface9 **out)
{
    HRESULT hr = o_get_rt(dev, i, out);
    if (SUCCEEDED(hr) && i == 0 && out && *out && *out == g_scene && g_scene) {
        IDirect3DSurface9_Release(*out);
        *out = g_bb;
        IDirect3DSurface9_AddRef(g_bb);
    }
    return hr;
}

static HRESULT WINAPI d_stretch(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *sr,
                                IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
    HRESULT hr;
    int sub = 0;
    if (g_phase && dev == g_dev && g_scene) {
        if (src == g_bb) { src = g_scene; sub = 1; }
        if (dst == g_bb) { dst = g_scene; sub = 1; }
    }
    hr = o_stretch(dev, src, sr, dst, dr, f);
    if (sub && FAILED(hr) && InterlockedIncrement(&g_stretch_fail) <= 8) {
        D3DSURFACE_DESC a, b;
        a.Format = b.Format = D3DFMT_UNKNOWN; a.Width = a.Height = b.Width = b.Height = 0;
        IDirect3DSurface9_GetDesc(src, &a);
        IDirect3DSurface9_GetDesc(dst, &b);
        hg_log("hdr: StretchRect %ux%u fmt %d -> %ux%u fmt %d FAILED (hr=0x%08lx)", a.Width, a.Height,
               (int)a.Format, b.Width, b.Height, (int)b.Format, (unsigned long)hr);
    }
    return hr;
}

/* Rebind render target 0 from one surface to the other if it is bound,
 * keeping the viewport (SetRenderTarget resets it). */
static void swap_bound(IDirect3DDevice9 *dev, IDirect3DSurface9 *from, IDirect3DSurface9 *to)
{
    IDirect3DSurface9 *rt = NULL;
    D3DVIEWPORT9 vp;
    if (FAILED(o_get_rt(dev, 0, &rt)) || !rt) return;
    if (rt == from) {
        IDirect3DDevice9_GetViewport(dev, &vp);
        o_set_rt(dev, 0, to);
        IDirect3DDevice9_SetViewport(dev, &vp);
    }
    IDirect3DSurface9_Release(rt);
}

static int hook_vt(void **vt, size_t off, void *detour, void **orig, const char *name)
{
    void *target = vt[off / sizeof(void *)];
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        hg_log("hdr: FAILED to hook %s at %p", name, target);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */

static LONG g_attached;                 /* hooks in, this device's back buffer known */
static LONG g_failed;                   /* the float target could not be made: no retry until switched off */

void postfx_reset(void);

/* The float scene, made or dropped between frames (hdr_begin_frame). */
static int tex_create(IDirect3DDevice9 *dev)
{
    D3DSURFACE_DESC d;
    HRESULT hr;
    if (FAILED(IDirect3DSurface9_GetDesc(g_bb, &d))) return 0;
    hr = IDirect3DDevice9_CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                        D3DPOOL_DEFAULT, &g_tex, NULL);
    if (FAILED(hr) || !g_tex) {
        hg_log("hdr: float scene %ux%u NOT created (hr=0x%08lx); HDR off", d.Width, d.Height, (unsigned long)hr);
        g_tex = NULL;
        return 0;
    }
    IDirect3DTexture9_GetSurfaceLevel(g_tex, 0, &g_scene);
    g_live = 1;
    hg_log("hdr: the scene draws into a %ux%u A16B16G16R16F target", d.Width, d.Height);
    return 1;
}

static void tex_release(IDirect3DDevice9 *dev)
{
    if (!g_live) return;
    if (dev) swap_bound(dev, g_scene, g_bb);
    g_phase = 0;
    g_live = 0;
    if (g_scene) IDirect3DSurface9_Release(g_scene);
    if (g_tex) IDirect3DTexture9_Release(g_tex);
    g_scene = NULL;
    g_tex = NULL;
}

/* From src/device.c after the device is created (and after a Reset), on the
 * SMAA path only: the hooks (pass-through while HDR is off), and the float
 * scene if HDR is on. */
void hdr_create(IDirect3DDevice9 *dev)
{
    static LONG hooked;
    IDirect3DSurface9 *bb = NULL;
    g_live = 0;
    g_phase = 0;
    g_failed = 0;
    if (InterlockedCompareExchange(&hooked, 1, 0) == 0) {
        void **vt = *(void ***)dev;
        if (!hook_vt(vt, offsetof(IDirect3DDevice9Vtbl, SetRenderTarget), (void *)d_set_rt, (void **)&o_set_rt, "SetRenderTarget") ||
            !hook_vt(vt, offsetof(IDirect3DDevice9Vtbl, GetRenderTarget), (void *)d_get_rt, (void **)&o_get_rt, "GetRenderTarget") ||
            !hook_vt(vt, offsetof(IDirect3DDevice9Vtbl, StretchRect), (void *)d_stretch, (void **)&o_stretch, "StretchRect")) {
            hooked = 2;
        }
    }
    if (hooked != 1) { hg_log("hdr: device hooks missing; HDR off"); return; }
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9_Release(bb);          /* the pointer stays valid: the swap chain holds it */
    g_dev = dev;
    g_bb = bb;
    g_attached = 1;
    if (g_want && !tex_create(dev)) g_failed = 1;
    hdr_begin_frame(dev);
}

/* Before a Reset or a new device: everything in the default pool goes. */
void hdr_release(IDirect3DDevice9 *dev)
{
    tex_release(dev);
    g_attached = 0;
    g_bb = NULL;
    g_dev = NULL;
}

/* After each Present: the next frame's scene goes to the float target. The
 * setting switches here, between frames: the target is made or dropped,
 * and postfx rebuilds its own (AO's bounce copy and the auto exposure
 * follow the scene's format). */
void hdr_begin_frame(IDirect3DDevice9 *dev)
{
    if (!g_attached || dev != g_dev) return;
    if (!g_want) g_failed = 0;
    if ((g_want ? 1 : 0) != g_live && !g_failed) {
        if (g_want) { if (!tex_create(dev)) g_failed = 1; }
        else { tex_release(dev); hg_log("hdr: off"); }
        postfx_reset();
        hg_gfx_knobs_changed();         /* the materials' clamp follows */
    }
    if (!g_live) return;
    g_phase = 1;
    swap_bound(dev, g_bb, g_scene);
}

/* Panel button: the finished float scene read back once and logged, what
 * an 8-bit frame cannot hold: NaN, infinity, below 0, above 1, and the peak. */
static volatile LONG g_scan_req;

static float half_to_float(unsigned short h)
{
    unsigned e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? m / 16777216.0f : (float)(1024 + m) * (float)(1u << e) / 33554432.0f;
    return h & 0x8000 ? -v : v;
}

static void scan(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *mem = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    long nan = 0, inf = 0, neg = 0, over = 0, n = 0;
    float peak = 0;
    UINT x, y, c;
    if (FAILED(IDirect3DSurface9_GetDesc(g_scene, &d)) ||
        FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &mem, NULL)) ||
        FAILED(IDirect3DDevice9_GetRenderTargetData(dev, g_scene, mem)) ||
        FAILED(IDirect3DSurface9_LockRect(mem, &lr, NULL, D3DLOCK_READONLY))) {
        hg_log("hdr: scan FAILED");
        if (mem) IDirect3DSurface9_Release(mem);
        return;
    }
    for (y = 0; y < d.Height; y++) {
        const unsigned short *row = (const unsigned short *)((const char *)lr.pBits + y * lr.Pitch);
        for (x = 0; x < d.Width; x++, n++) {
            int bad = 0, hi = 0, lo = 0;
            for (c = 0; c < 3; c++) {
                unsigned short h = row[x * 4 + c];
                if (((h >> 10) & 0x1f) == 0x1f) { if (h & 0x3ff) bad |= 1; else bad |= 2; continue; }
                {
                    float v = half_to_float(h);
                    if (v < 0) lo = 1;
                    if (v > 1) hi = 1;
                    if (v > peak) peak = v;
                }
            }
            if (bad & 1) nan++; else if (bad & 2) inf++;
            neg += lo; over += hi;
        }
    }
    IDirect3DSurface9_UnlockRect(mem);
    IDirect3DSurface9_Release(mem);
    hg_log("hdr: scan of %ld pixels: %ld NaN, %ld infinite, %ld below 0, %ld above 1, peak %.2f",
           n, nan, inf, neg, over, peak);
}

/* The resolve: from here on the back buffer is the back buffer. The caller
 * draws the float scene into it (hdr_texture). */
void hdr_end_scene(IDirect3DDevice9 *dev)
{
    if (!g_phase || dev != g_dev) return;
    if (g_scan_req) { g_scan_req = 0; scan(dev); }
    g_phase = 0;
    swap_bound(dev, g_scene, g_bb);
}

/* At Present, after our passes: a frame still in its scene phase is copied
 * over as it is. */
void hdr_finish(IDirect3DDevice9 *dev)
{
    if (!g_phase || dev != g_dev) return;
    hdr_end_scene(dev);
    if (FAILED(o_stretch(dev, g_scene, NULL, g_bb, NULL, D3DTEXF_NONE)) && InterlockedIncrement(&g_stretch_fail) <= 8)
        hg_log("hdr: the plain copy to the back buffer FAILED");
    g_copies++;
}

int hdr_in_scene(void) { return (int)g_phase; }

/* Auto exposure (bloom.fx gvHdrAuto): x strength, y log of the target
 * middle, z the most it moves exposure (natural log); x 0 without the tone
 * map. w is the tone map's highlight spill (1 = per channel from 2x white). */
void hdr_auto(float v[4])
{
    v[0] = g_live && g_tm ? g_auto / 100.0f : 0.0f;
    v[1] = logf(g_auto_key / 1000.0f);
    v[2] = g_auto_stops / 10.0f * 0.693147f;
    v[3] = g_spill / 100.0f;
}

/* The materials' knob (gvUltraHDR.x): no soft clamp while the tone map is on. */
int hdr_unclamped(void) { return g_live && g_tm; }

/* The resolve's tone map (bloom.fx gvHdr): x on, y exposure, z knee; and w
 * the bloom threshold in scene brightness (1 = white). */
void hdr_tonemap(float v[4])
{
    v[0] = g_live && g_tm ? 1.0f : 0.0f;
    v[1] = g_exposure / 100.0f;
    v[2] = g_knee / 100.0f;
    v[3] = g_bloom_thr / 100.0f;
}
IDirect3DTexture9 *hdr_texture(void) { return g_live ? g_tex : NULL; }

/* From device_install, before the device exists. */
void hdr_install(void)
{
    settings_var("hdr.on", &g_want, 0, 1);
    settings_var("hdr.tonemap", &g_tm, 0, 1);
    settings_var("hdr.exposure", &g_exposure, 25, 400);
    settings_var("hdr.knee", &g_knee, 30, 95);
    settings_var("hdr.bloom_threshold", &g_bloom_thr, 25, 400);
    settings_var("hdr.auto", &g_auto, 0, 100);
    settings_var("hdr.auto_middle", &g_auto_key, 10, 500);
    settings_var("hdr.auto_stops", &g_auto_stops, 0, 30);
    settings_var("hdr.spill", &g_spill, 0, 400);
}

/* Panel. */
void hg_gfx_set_hdr(int on)
{
    InterlockedExchange(&g_want, on ? 1 : 0);
    hg_log("hdr: %s from the next frame", on ? "ON" : "off");
}
int hg_gfx_hdr(void) { return (int)g_want; }
int hg_gfx_hdr_live(void) { return (int)g_live; }
long hg_gfx_hdr_copies(void) { return g_copies; }
void hg_gfx_set_hdr_tonemap(int on)
{
    InterlockedExchange(&g_tm, on ? 1 : 0);
    hg_gfx_knobs_changed();             /* the materials' clamp follows it */
    hg_log("hdr: tone map and unclamped materials %s", on ? "ON" : "off");
}
int hg_gfx_hdr_tonemap(void) { return (int)g_tm; }
void hg_gfx_hdr_scan(void) { InterlockedExchange(&g_scan_req, 1); }
/* which: 0 exposure, 1 knee, 2 bloom threshold (percent), 3 auto exposure (percent),
 * 4 its target middle (x1000), 5 its range (stops x10), 6 highlight spill (percent) */
static volatile LONG *const g_knobs[7] = { &g_exposure, &g_knee, &g_bloom_thr, &g_auto, &g_auto_key, &g_auto_stops, &g_spill };
void hg_gfx_nudge_hdr(int which, int d)
{
    static const LONG lo[7] = { 25, 30, 25, 0, 10, 0, 0 }, hi[7] = { 400, 95, 400, 100, 500, 30, 400 };
    LONG v;
    if (which < 0 || which > 6) return;
    v = *g_knobs[which] + d;
    InterlockedExchange(g_knobs[which], v < lo[which] ? lo[which] : v > hi[which] ? hi[which] : v);
    hg_log("hdr: exposure %ld%%, knee %ld%%, spill %ld%%, bloom from %ld%% of white; auto exposure %ld%% towards %.3f, at most %.1f stops",
           g_exposure, g_knee, g_spill, g_bloom_thr, g_auto, g_auto_key / 1000.0f, g_auto_stops / 10.0f);
}
int hg_gfx_hdr_val(int which) { return which >= 0 && which <= 6 ? (int)*g_knobs[which] : 0; }
