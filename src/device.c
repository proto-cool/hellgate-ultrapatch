/*
 * The game's D3D9 device: how we reach it, and what we change at creation.
 *
 * IDirect3D9::CreateDevice is hooked before the game creates its device
 * (gfxprobe_install runs ahead of the renderer's start), on the vtable of an
 * IDirect3D9 of our own; every IDirect3D9 shares it. When the game's device
 * appears, its EndScene, Reset and CreateQuery are hooked straight from its
 * own vtable. This used to be the overlay's job, from a throwaway probe
 * device, and only with the panel on: without the panel the per-frame
 * graphics work (gfxprobe_frame) never ran.
 *
 * Scene depth (docs/graphics-plan.md, "Scene depth"). The engine renders
 * straight into a 4x MSAA back buffer with the device's automatic depth
 * buffer, which no shader can read. With SMAA on (the default) the device is
 * created without MSAA and without the automatic depth buffer, and an INTZ
 * depth texture is bound in its place before the engine asks for it
 * (dx9_SaveAutoDepthStencil, right after creation). The engine then draws
 * into a depth buffer our passes can sample. SMAA replaces the MSAA; the
 * setting applies at the next device creation, i.e. a restart.
 */
#include <windows.h>
#include <stddef.h>
#include <d3d9.h>
#include "panel.h"
#include "../ref/minhook/include/MinHook.h"

void gfxprobe_frame(IDirect3DDevice9 *dev);
void gfxprobe_hook_create_query(void **vt);
void overlay_endscene(IDirect3DDevice9 *dev);
void overlay_reset(void);
void altlatch_attach(void *hwnd);
void postfx_reset(void);
void compare_present(IDirect3DDevice9 *dev);
void invsort_tick(void);
int  compare_hides_overlay(void);
int  postfx_present(IDirect3DDevice9 *dev);
void postfx_present_draw(IDirect3DDevice9 *dev);
int  brand_wanted(void);
void brand_draw(IDirect3DDevice9 *dev);
void brand_reset(void);
void gfxprobe_present(void);
void plshadow_reset(void);
void hdr_install(void);
void hdr_create(IDirect3DDevice9 *dev);
void hdr_release(IDirect3DDevice9 *dev);
void hdr_begin_frame(IDirect3DDevice9 *dev);
void hdr_finish(IDirect3DDevice9 *dev);

#define FOURCC_INTZ ((D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'))

typedef HRESULT (WINAPI *create_device_fn)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *endscene_fn)(IDirect3DDevice9 *);
typedef HRESULT (WINAPI *reset_fn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT (WINAPI *present_fn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (WINAPI *sc_present_fn)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND,
                                        const RGNDATA *, DWORD);

static create_device_fn g_orig_create_device;
static endscene_fn      g_orig_endscene;
static reset_fn         g_orig_reset;
static present_fn       g_orig_present;
static sc_present_fn    g_orig_sc_present;
static LONG g_present_depth;    /* DXVK's device Present calls the swap chain's */

static volatile LONG g_smaa_want = 1;   /* the setting: bin\\hellgate_smaa.off clears it */
static LONG g_smaa_live;                /* this device: no MSAA, INTZ depth */
static IDirect3DDevice9 *g_dev;         /* the game's device */
static IDirect3DTexture9 *g_depth_tex;  /* INTZ, bound as the depth buffer */
static IDirect3DSurface9 *g_depth_surf;
static D3DFORMAT g_auto_fmt;            /* what the game asked for */

/* ------------------------------------------------------------------ */

static int depth_create(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC d;
    HRESULT hr;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return 0;
    hr = IDirect3DSurface9_GetDesc(bb, &d);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr)) return 0;
    hr = IDirect3DDevice9_CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_INTZ,
                                        D3DPOOL_DEFAULT, &g_depth_tex, NULL);
    if (FAILED(hr) || !g_depth_tex) {
        hg_log("device: INTZ depth %ux%u NOT created (hr=0x%08lx)", d.Width, d.Height, (unsigned long)hr);
        g_depth_tex = NULL;
        return 0;
    }
    IDirect3DTexture9_GetSurfaceLevel(g_depth_tex, 0, &g_depth_surf);
    IDirect3DDevice9_SetDepthStencilSurface(dev, g_depth_surf);
    hg_log("device: scene depth is an INTZ texture, %ux%u", d.Width, d.Height);
    return 1;
}

static void depth_release(IDirect3DDevice9 *dev)
{
    if (!g_depth_tex) return;
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    if (g_depth_surf) IDirect3DSurface9_Release(g_depth_surf);
    IDirect3DTexture9_Release(g_depth_tex);
    g_depth_surf = NULL;
    g_depth_tex = NULL;
}

/* The game's parameters, changed for SMAA: no MSAA, no automatic depth
 * buffer. The caller's struct keeps MultiSampleType NONE (the engine sizes
 * nothing else from it) but its own auto-depth fields. */
static void pp_adjust(D3DPRESENT_PARAMETERS *pp, D3DPRESENT_PARAMETERS *out)
{
    *out = *pp;
    if (!g_smaa_live) return;
    if (pp->MultiSampleType != D3DMULTISAMPLE_NONE)
        hg_log("device: MSAA %u off (SMAA replaces it)", (unsigned)pp->MultiSampleType);
    pp->MultiSampleType = out->MultiSampleType = D3DMULTISAMPLE_NONE;
    pp->MultiSampleQuality = out->MultiSampleQuality = 0;
    if (pp->EnableAutoDepthStencil) g_auto_fmt = pp->AutoDepthStencilFormat;
    out->EnableAutoDepthStencil = FALSE;
}

static void pp_copy_back(D3DPRESENT_PARAMETERS *pp, const D3DPRESENT_PARAMETERS *used)
{
    BOOL ads = pp->EnableAutoDepthStencil;
    D3DFORMAT adf = pp->AutoDepthStencilFormat;
    *pp = *used;          /* D3D fills in a 0 width/height, the format, ... */
    pp->EnableAutoDepthStencil = ads;
    pp->AutoDepthStencilFormat = adf;
}

/* ------------------------------------------------------------------ */

static HRESULT WINAPI detour_endscene(IDirect3DDevice9 *dev)
{
    /* The Alt latch needs the game's window from the first frame. */
    static int latch_tried;
    if (!latch_tried) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        HWND w = NULL;
        latch_tried = 1;
        if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)))
            w = cp.hFocusWindow;
        if (!w) w = GetActiveWindow();
        altlatch_attach(w);
    }
    /* The engine ends a scene about four times a frame: per-frame work
     * belongs in Present (frame_end), not here. */
    if (dev == g_dev) gfxprobe_frame(dev);
    if (!compare_hides_overlay()) overlay_endscene(dev);
    return g_orig_endscene(dev);
}

/* Once per frame, the frame complete, before it is shown. */
static void frame_end(IDirect3DDevice9 *dev)
{
    int smaa, brand;
    if (dev != g_dev) return;
    gfxprobe_present();
    smaa = postfx_present(dev);
    brand = brand_wanted();
    if (smaa || brand) {
        /* a frame without UI still gets its SMAA, the menu its name: draws
         * need a scene */
        IDirect3DDevice9_BeginScene(dev);
        if (smaa) postfx_present_draw(dev);
        hdr_finish(dev);                /* the float scene, if nothing resolved it */
        if (brand) brand_draw(dev);
        g_orig_endscene(dev);
    } else {
        hdr_finish(dev);
    }
    compare_present(dev);
    invsort_tick();                     /* the inventory sort, one step a frame */
    settings_present();                 /* save what changed */
}

static HRESULT WINAPI detour_present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND w,
                                     const RGNDATA *dirty)
{
    HRESULT hr;
    if (InterlockedIncrement(&g_present_depth) == 1) frame_end(dev);
    hr = g_orig_present(dev, src, dst, w, dirty);
    if (g_present_depth == 1) hdr_begin_frame(dev);
    InterlockedDecrement(&g_present_depth);
    return hr;
}

static HRESULT WINAPI detour_sc_present(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst, HWND w,
                                        const RGNDATA *dirty, DWORD flags)
{
    HRESULT hr;
    if (InterlockedIncrement(&g_present_depth) == 1) {
        IDirect3DDevice9 *dev = NULL;
        if (SUCCEEDED(IDirect3DSwapChain9_GetDevice(sc, &dev)) && dev) {
            frame_end(dev);
            IDirect3DDevice9_Release(dev);
        }
    }
    hr = g_orig_sc_present(sc, src, dst, w, dirty, flags);
    if (g_present_depth == 1) {
        IDirect3DDevice9 *dev = NULL;
        if (SUCCEEDED(IDirect3DSwapChain9_GetDevice(sc, &dev)) && dev) {
            hdr_begin_frame(dev);
            IDirect3DDevice9_Release(dev);
        }
    }
    InterlockedDecrement(&g_present_depth);
    return hr;
}

static HRESULT WINAPI detour_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    D3DPRESENT_PARAMETERS used;
    HRESULT hr;
    int mine = dev == g_dev && pp;
    overlay_reset();
    brand_reset();
    plshadow_reset();
    if (!mine) return g_orig_reset(dev, pp);
    postfx_reset();
    hdr_release(dev);
    depth_release(dev);
    pp_adjust(pp, &used);
    hr = g_orig_reset(dev, &used);
    pp_copy_back(pp, &used);
    if (SUCCEEDED(hr) && g_smaa_live && !depth_create(dev)) {
        /* no depth texture: without a depth buffer nothing would draw */
        hg_log("device: no INTZ after Reset; SMAA path off until restart");
    }
    if (SUCCEEDED(hr) && g_smaa_live && g_depth_tex) hdr_create(dev);
    return hr;
}

static int hook_vt(void **vt, size_t off, void *detour, void **orig, const char *name)
{
    void *target = vt[off / sizeof(void *)];
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        hg_log("device: FAILED to hook %s at %p", name, target);
        return 0;
    }
    hg_log("device: hooked %s at %p", name, target);
    return 1;
}

static HRESULT WINAPI detour_create_device(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                           DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                           IDirect3DDevice9 **out)
{
    D3DPRESENT_PARAMETERS used;
    HRESULT hr;
    static LONG hooked;
    if (!pp || !out) return g_orig_create_device(d3d, adapter, type, wnd, flags, pp, out);
    /* a later device replaces an earlier one (e_DeviceCreateMinimal): our
     * texture would otherwise keep the old device alive */
    if (g_dev) { postfx_reset(); plshadow_reset(); hdr_release(g_dev); depth_release(g_dev); }
    g_dev = NULL;
    g_smaa_live = g_smaa_want;
    pp_adjust(pp, &used);
    hr = g_orig_create_device(d3d, adapter, type, wnd, flags, &used, out);
    if (FAILED(hr) && g_smaa_live) {
        hg_log("device: CreateDevice without MSAA failed (hr=0x%08lx); stock parameters", (unsigned long)hr);
        g_smaa_live = 0;
        return g_orig_create_device(d3d, adapter, type, wnd, flags, pp, out);
    }
    pp_copy_back(pp, &used);
    if (FAILED(hr) || !*out) return hr;
    g_dev = *out;
    hg_log("device: the game's device %p, %ux%u, %s", (void *)g_dev, used.BackBufferWidth,
           used.BackBufferHeight, g_smaa_live ? "SMAA path (no MSAA, INTZ depth)" : "stock (MSAA as set)");
    if (g_smaa_live && !depth_create(g_dev)) {
        /* fall back to a plain depth buffer so the game still draws */
        IDirect3DSurface9 *ds = NULL;
        if (SUCCEEDED(IDirect3DDevice9_CreateDepthStencilSurface(g_dev, used.BackBufferWidth,
                used.BackBufferHeight, g_auto_fmt ? g_auto_fmt : D3DFMT_D24S8,
                D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL)) && ds) {
            IDirect3DDevice9_SetDepthStencilSurface(g_dev, ds);
            IDirect3DSurface9_Release(ds);
        }
        g_smaa_live = 0;
    }
    if (InterlockedCompareExchange(&hooked, 1, 0) == 0) {
        void **vt = *(void ***)g_dev;
        hook_vt(vt, offsetof(IDirect3DDevice9Vtbl, EndScene), (void *)detour_endscene,
                (void **)&g_orig_endscene, "IDirect3DDevice9::EndScene");
        hook_vt(vt, offsetof(IDirect3DDevice9Vtbl, Reset), (void *)detour_reset,
                (void **)&g_orig_reset, "IDirect3DDevice9::Reset");
        hook_vt(vt, offsetof(IDirect3DDevice9Vtbl, Present), (void *)detour_present,
                (void **)&g_orig_present, "IDirect3DDevice9::Present");
        {
            IDirect3DSwapChain9 *sc = NULL;
            if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(g_dev, 0, &sc)) && sc) {
                hook_vt(*(void ***)sc, offsetof(IDirect3DSwapChain9Vtbl, Present), (void *)detour_sc_present,
                        (void **)&g_orig_sc_present, "IDirect3DSwapChain9::Present");
                IDirect3DSwapChain9_Release(sc);
            }
        }
        gfxprobe_hook_create_query(vt);
    }
    if (g_smaa_live) hdr_create(g_dev);
    return hr;
}

/* From gfxprobe_install, on the worker thread, before the renderer starts. */
void device_install(void)
{
    HMODULE m = GetModuleHandleA("d3d9.dll");
    IDirect3D9 *(WINAPI *create9)(UINT);
    IDirect3D9 *d3d;
    if (!m) m = LoadLibraryA("d3d9.dll");
    create9 = m ? (IDirect3D9 *(WINAPI *)(UINT))(void *)GetProcAddress(m, "Direct3DCreate9") : NULL;
    d3d = create9 ? create9(D3D_SDK_VERSION) : NULL;
    if (!d3d) {
        hg_log("device: no IDirect3D9; device hooks NOT installed");
        return;
    }
    if (hg_flagfile(L"hellgate_smaa.off")) g_smaa_want = 0;
    hdr_install();
    if (g_dev == NULL)
        hook_vt(*(void ***)d3d, offsetof(IDirect3D9Vtbl, CreateDevice), (void *)detour_create_device,
                (void **)&g_orig_create_device, "IDirect3D9::CreateDevice");
    IDirect3D9_Release(d3d);
}

/* Panel. */
void hg_gfx_set_smaa(int on)
{
    InterlockedExchange(&g_smaa_want, on ? 1 : 0);
    hg_set_flagfile(L"hellgate_smaa.off", !on);
    hg_log("device: SMAA %s from the next start (this run: %s)", on ? "ON" : "off",
           g_smaa_live ? "SMAA path" : "stock MSAA");
}
int hg_gfx_smaa(void) { return (int)g_smaa_want; }
int hg_gfx_smaa_live(void) { return (int)g_smaa_live; }

/* For the passes: the game's device, and the scene's depth texture (NULL on
 * the stock path). */
IDirect3DDevice9 *device_get(void) { return g_dev; }
IDirect3DTexture9 *device_depth_texture(void) { return g_depth_tex; }
IDirect3DSurface9 *device_depth_surface(void) { return g_depth_surf; }
