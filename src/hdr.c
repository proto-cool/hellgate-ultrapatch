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
 * INTZ depth). The setting applies at the next device creation, i.e. a
 * restart; off, nothing here acts.
 */
#include <windows.h>
#include <stddef.h>
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

static volatile LONG g_want;            /* the setting (hdr.on) */
static LONG g_live;                     /* this device: the float scene exists */
static IDirect3DDevice9 *g_dev;
static IDirect3DSurface9 *g_bb;         /* the real back buffer (no reference kept: the swap chain holds it) */
static IDirect3DTexture9 *g_tex;        /* the float scene, A16B16G16R16F, back-buffer size */
static IDirect3DSurface9 *g_scene;      /* its level 0 */
static volatile LONG g_phase;           /* 1: the scene draws into g_scene */
static LONG g_stretch_fail, g_copies;

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

/* From src/device.c after the device is created (and after a Reset), on the
 * SMAA path only. */
void hdr_create(IDirect3DDevice9 *dev)
{
    static LONG hooked;
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC d;
    HRESULT hr;
    g_live = 0;
    g_phase = 0;
    if (!g_want) return;
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
    hr = IDirect3DSurface9_GetDesc(bb, &d);
    IDirect3DSurface9_Release(bb);          /* the pointer stays valid: the swap chain holds it */
    if (FAILED(hr)) return;
    hr = IDirect3DDevice9_CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                        D3DPOOL_DEFAULT, &g_tex, NULL);
    if (FAILED(hr) || !g_tex) {
        hg_log("hdr: float scene %ux%u NOT created (hr=0x%08lx); HDR off", d.Width, d.Height, (unsigned long)hr);
        g_tex = NULL;
        return;
    }
    IDirect3DTexture9_GetSurfaceLevel(g_tex, 0, &g_scene);
    g_dev = dev;
    g_bb = bb;
    g_live = 1;
    hdr_begin_frame(dev);
    hg_log("hdr: the scene draws into a %ux%u A16B16G16R16F target", d.Width, d.Height);
}

/* Before a Reset or a new device: everything in the default pool goes. */
void hdr_release(IDirect3DDevice9 *dev)
{
    if (!g_live) return;
    if (dev) swap_bound(dev, g_scene, g_bb);
    g_phase = 0;
    g_live = 0;
    if (g_scene) IDirect3DSurface9_Release(g_scene);
    if (g_tex) IDirect3DTexture9_Release(g_tex);
    g_scene = NULL;
    g_tex = NULL;
    g_bb = NULL;
    g_dev = NULL;
}

/* After each Present: the next frame's scene goes to the float target. */
void hdr_begin_frame(IDirect3DDevice9 *dev)
{
    if (!g_live || dev != g_dev) return;
    g_phase = 1;
    swap_bound(dev, g_bb, g_scene);
}

/* The resolve: from here on the back buffer is the back buffer. The caller
 * draws the float scene into it (hdr_texture). */
void hdr_end_scene(IDirect3DDevice9 *dev)
{
    if (!g_phase || dev != g_dev) return;
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
IDirect3DTexture9 *hdr_texture(void) { return g_live ? g_tex : NULL; }

/* From device_install, before the device exists. */
void hdr_install(void)
{
    settings_var("hdr.on", &g_want, 0, 1);
}

/* Panel. */
void hg_gfx_set_hdr(int on)
{
    InterlockedExchange(&g_want, on ? 1 : 0);
    hg_log("hdr: %s from the next start (this run: %s)", on ? "ON" : "off", g_live ? "on" : "off");
}
int hg_gfx_hdr(void) { return (int)g_want; }
int hg_gfx_hdr_live(void) { return (int)g_live; }
long hg_gfx_hdr_copies(void) { return g_copies; }
