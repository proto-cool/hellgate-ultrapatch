/*
 * Our own passes on the scene: ambient occlusion and SMAA.
 *
 * Both need the device created the SMAA way (src/device.c): no MSAA, and the
 * scene's depth in an INTZ texture. The effects are ours, compiled by
 * tools/fx/build_shaders.sh into <game>\override\ultra\ (shaders/ao.fx,
 * shaders/smaa.fx) and created here with the game's own D3DX.
 *
 * Where they run:
 *
 *   AO    after the opaque scene, before the sky and anything transparent.
 *         The main camera draws through the viewer renderer
 *         (dxC_viewer_render.cpp), whose passes run in RPTYPE_* order:
 *         OPAQUE_1P, OPAQUE_(PRE_/POST_)BLOB or OPAQUE_SCENE, OPAQUE_SKYBOX,
 *         ALPHA_SKYBOX, PARTICLES_ENV, ALPHA_SCENE, ALPHA_1P,
 *         PARTICLES_GENERAL. gfxprobe.c calls postfx_before_transparent at
 *         the first skybox or particle pass, or the first material draw
 *         with alpha blending, whichever comes first; the older draw-list
 *         path (FUN_0077ace4) marks the same point with a command of type
 *         0x18, a no-op in dx9_RenderDrawList's switch (jump table
 *         0x7b400c, entry 0x18 -> 0x7b3fe4), whose entry points at
 *         marker_stub. Only with the back buffer bound, once per frame.
 *   SMAA  on the finished 3D frame, just before the UI: at the first
 *         BeginPass of ui.fxo that draws to the back buffer once the
 *         opaque scene is done (gfxprobe.c), or at Present on a frame
 *         without UI or without a 3D scene. Per-frame state resets at
 *         Present: the engine ends a scene about four times a frame.
 *
 * Every pass saves and restores all device state (a D3DSBT_ALL state block
 * plus render target 0 and the depth buffer), so the engine never sees it.
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include "panel.h"
#include "../ref/smaa/Textures/AreaTex.h"
#include "../ref/smaa/Textures/SearchTex.h"
#include "volfog.h"

/* the focus grid (focus) */
#define FOCUS_W 8
#define FOCUS_H 4

IDirect3DDevice9 *device_get(void);
IDirect3DTexture9 *device_depth_texture(void);
IDirect3DSurface9 *device_depth_surface(void);
int gfxprobe_camera_proj(float *m);
int hg_gfx_stock_viewing(void);
void gfxprobe_own_passes(int on);
int plshadow_lights_near(const float eye[3], float margin, float (*pr)[4], float (*col)[4], int max);
LONG plshadow_params(float pls[4], float pls2[4]);
IDirect3DBaseTexture9 *plshadow_texture(void);
int hdr_in_scene(void);
IDirect3DTexture9 *hdr_texture(void);
void hdr_end_scene(IDirect3DDevice9 *dev);
void hdr_finish(IDirect3DDevice9 *dev);
void hdr_tonemap(float v[4]);
void hdr_auto(float v[4]);

#define RVA_DRAWLIST_JUMP_18 0x003B406Cu     /* jump table 0x7b400c, entry 0x18 */
#define RVA_DRAWLIST_NOOP    0x003B3FE4u     /* its stock target */

/* settings */
static volatile LONG g_ao_on = 1;
static volatile LONG g_ao_radius = 120;      /* world units x 100 */
static volatile LONG g_ao_strength = 100;    /* percent */
static volatile LONG g_ao_show;              /* debug: 1 the occlusion alone, 2 the bounce alone (x4) */
static volatile LONG g_ao_sun = 70;          /* share of the occlusion full sun takes away, percent */
static volatile LONG g_ao_bleed = 150;       /* one-bounce colour from the occluding surfaces, percent */
static volatile LONG g_smaa_pass = 1;        /* the SMAA pass, for A/B (the device path stays) */
static volatile LONG g_cas = 50;             /* CAS sharpening after SMAA, percent (0 = off) */
static volatile LONG g_soft = 60;            /* soft particles: fade distance, units x100 (0 = off) */
static volatile LONG g_fog_on = 1;           /* volumetric fog */
static volatile LONG g_fog_density = 60;     /* on the surface (the sun is up), per unit x1000 */
static volatile LONG g_fog_density_in = 12;  /* indoors and underground */
static volatile LONG g_fog_sun = 70;         /* sun shafts: the brightest lit air, percent of the sun's colour */
static volatile LONG g_fog_sky = 60;         /* the sun's share on the sky and far away, percent */
static volatile LONG g_fog_glow = 65;        /* glow around point lights, percent */
static volatile LONG g_fog_dist = 60;        /* how far the sun is marched, units */
static volatile LONG g_fog_show;             /* debug: the scattered light alone */
static volatile LONG g_fog_haze = 4;         /* distance haze on the surface, per unit x1000 (a third indoors) */
static volatile LONG g_fog_lamp = 80;        /* indoors: lamp halos shadowed in screen space, percent */
static volatile LONG g_fog_mist = 30;        /* indoors: ground mist at the floor, per unit x1000 */
static volatile LONG g_fog_mist_h = 60;      /* ... its height, units x100 */
/* the ground mist's volume (shaders/fog.fx VolCopy, VolInject): VOL_N^2
 * cells of VOL_CELL units around the camera, VOL_S slices high, from
 * VOL_BELOW units below the eye to 3 above it (it stopped 2 below the eye,
 * and a camera lowered for a flatter view took the floor out of it: the
 * mist went out, 2026-09-25); the slices side by side in an atlas VOL_T
 * tiles across */
#define VOL_N 96
#define VOL_S 32
#define VOL_BELOW 13.0f
#define VOL_T 6
#define VOL_CELL 0.5f
#define FOG_NEAR 8.0f                        /* no fog in the first units from the camera */
static volatile LONG g_bloom_on = 1;         /* bloom */
static volatile LONG g_bloom = 70;           /* intensity, percent */
static volatile LONG g_bloom_thr = 50;       /* threshold, percent of full luma */
static volatile LONG g_grade_on = 1;         /* colour grade */
static volatile LONG g_grade_sat = 120;      /* saturation, percent */
static volatile LONG g_grade_con = 30;       /* contrast around the game's middle (0.15), percent (the user's pick, 2026-09-24) */
static volatile LONG g_grade_tint = 30;      /* shadows towards the fog's colour, percent (the user's pick, 2026-09-24) */
static volatile LONG g_grade_vig = 30;       /* vignette, percent (the user's pick, 2026-09-24) */
static volatile LONG g_spill = 100;          /* light spill (HDR, indoors): strength, percent (0 = off) */
static volatile LONG g_spill_reach = 200;    /* its reach, percent of each light's radius (it adds past the engine's radius) */
static volatile LONG g_spill_show;           /* debug: 1 the added light alone, 2 the light on black */
static volatile LONG g_rigid16 = 1;          /* draw Rigid16 background meshes' colour (the stock build never did) */
static volatile LONG g_contact = 60;         /* contact shadows (indoors): strength, percent (0 = off) */
static volatile LONG g_contact_len = 30;     /* how far they reach from a surface, units x100 (1 unit reached a hand held over the floor) */
static volatile LONG g_contact_show;         /* debug: the contact shadows alone */
static float g_outdoors = 1.0f;              /* 1 outdoors (the sun and its maps seen), 0 in; eased at Present */
#define BLOOM_LEVELS 6
#define LUM_LEVELS 5                         /* auto exposure: 256, 64, 16, 4, 1 */

/* per device */
static struct {
    IDirect3DDevice9 *dev;
    UINT w, h;
    int failed;                 /* creation failed: no retry until the next Reset */
    IDirect3DStateBlock9 *sb;
    ID3DXEffect *smaa, *ao, *cas, *fog, *bloom;
    IDirect3DTexture9 *bl[BLOOM_LEVELS];    /* bloom chain, 1/2 .. 1/64 (NULL: no bloom) */
    UINT blw[BLOOM_LEVELS], blh[BLOOM_LEVELS];
    IDirect3DTexture9 *color, *edges, *blend, *area, *search, *ao_a, *ao_b;
    IDirect3DTexture9 *ao_col;      /* the lit frame at AO time, half size, for the bounce */
    IDirect3DTexture9 *fog_a, *fog_b;   /* half resolution, 16-bit float (NULL: no fog) */
    IDirect3DTexture9 *fog_h[2];        /* the fog's history, ping-pong */
    IDirect3DTexture9 *level;           /* full size: 1 where the level's own geometry is (NULL: no mist) */
    IDirect3DTexture9 *vol[2];          /* the mist volume's atlas, ping-pong */
    IDirect3DTexture9 *vinj;            /* this frame's floor mist, the same layout */
    IDirect3DVertexBuffer9 *vpts;       /* one point per 4 x 4 pixels of the scene (VolInject) */
    UINT npts;
    int vol_cur, vol_valid;
    float vol_org[3];                   /* last frame's volume corner (whole cells) */
    LONG vol_fr;                        /* the fog frame it was last updated */
    float vol_eye[3];                   /* ... and the eye then */
    int fog_hcur, fog_hvalid;
    float fog_prev_view[16], fog_prev_p11, fog_prev_p22;
    IDirect3DTexture9 *lindepth;        /* R32F, half resolution: soft particles (NULL: none) */
    IDirect3DTexture9 *sp_a, *sp_b;     /* light spill, half resolution, 16-bit float (NULL: no spill) */
    IDirect3DTexture9 *mask;            /* backdrops drawn this frame (backdrop_mask), full size (NULL: none) */
    IDirect3DSurface9 *focus_rt[3];     /* FOCUS_W x FOCUS_H R32F: the view depth around the screen's focus, a ring (NULL: none) */
    IDirect3DSurface9 *focus_mem;       /* ... read back two frames late, so nothing waits */
    unsigned focus_n;
    IDirect3DTexture9 *lum[LUM_LEVELS]; /* HDR auto exposure: G32R32F, the scene's weighted log luminance (NULL: none) */
    IDirect3DTexture9 *adapt[2];        /* 1x1 R32F: the eye's log luminance, ping-pong */
    int adapt_cur, adapt_valid;
    LARGE_INTEGER adapt_t;              /* when it was last eased */
} R;
static float g_eye;                     /* its last read back value (luminance), for the panel */

static LONG g_ao_done, g_smaa_done;         /* this frame */
static LONG g_scene_seen;                   /* this frame reached the end of the opaque scene */
static LONG g_depth_done;                   /* this frame's linear depth is in R.lindepth */
static LONG g_ao_runs, g_smaa_runs, g_fog_runs, g_spill_runs;

/*
 * Frame trace, logged every 10 s: the events of one frame in order, so the
 * AO's place in it can be read off the log. C<n> a clear of the scene depth
 * after n opaque draws; P/B/Z/M a trigger (skybox or particle pass, blended
 * material draw, scene depth clear, draw-list marker) with the opaque count;
 * A AO ran (its mean read back from the GPU); s SMAA ran.
 */
static char g_tr[512];
static int g_tr_n, g_tr_on;
void postfx_trace(char ev, long n)
{
    if (!g_tr_on || g_tr_n > (int)sizeof g_tr - 16) return;
    g_tr_n += wsprintfA(g_tr + g_tr_n, "%c%ld ", ev, n);
}
int gfxprobe_opaque_draws(void);
static unsigned int g_image;
void *g_marker_orig __attribute__((used));  /* the no-op the marker jumped to (read by the stub) */

/* ------------------------------------------------------------------ */
/* resources                                                           */

#define REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

static void dnc_release(void);

static void res_release(void)
{
    REL(R.sb); REL(R.smaa); REL(R.ao); REL(R.cas); REL(R.fog); REL(R.fog_a); REL(R.fog_b); REL(R.bloom);
    { int i; for (i = 0; i < BLOOM_LEVELS; i++) REL(R.bl[i]); }
    REL(R.fog_h[0]); REL(R.fog_h[1]); R.fog_hvalid = 0;
    REL(R.level); REL(R.vol[0]); REL(R.vol[1]); REL(R.vinj); REL(R.vpts); R.npts = 0; R.vol_valid = 0;
    REL(R.color); REL(R.edges); REL(R.blend); REL(R.area); REL(R.search);
    REL(R.ao_a); REL(R.ao_b); REL(R.ao_col); REL(R.lindepth);
    REL(R.sp_a); REL(R.sp_b); REL(R.mask);
    dnc_release();
    REL(R.focus_rt[0]); REL(R.focus_rt[1]); REL(R.focus_rt[2]); REL(R.focus_mem); R.focus_n = 0;
    { int i; for (i = 0; i < LUM_LEVELS; i++) REL(R.lum[i]); }
    REL(R.adapt[0]); REL(R.adapt[1]); R.adapt_valid = 0;
    R.dev = NULL;
    R.w = R.h = 0;
}

static ID3DXEffect *load_effect(IDirect3DDevice9 *dev, const WCHAR *name)
{
    typedef HRESULT (WINAPI *create_fn)(IDirect3DDevice9 *, const void *, UINT, const D3DXMACRO *,
                                        ID3DXInclude *, DWORD, ID3DXEffectPool *, ID3DXEffect **,
                                        ID3DXBuffer **);
    static create_fn create;
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD n, got = 0;
    void *buf;
    ID3DXEffect *fx = NULL;
    ID3DXBuffer *err = NULL;
    HRESULT hr;
    if (!create) {
        HMODULE m = GetModuleHandleA("d3dx9_34.dll");
        if (m) create = (create_fn)(void *)GetProcAddress(m, "D3DXCreateEffect");
        if (!create) { hg_log("postfx: no d3dx9_34!D3DXCreateEffect"); return NULL; }
    }
    hg_dll_dir(path, MAX_PATH);
    lstrcatW(path, L"\\..\\override\\ultra\\");
    lstrcatW(path, name);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { hg_log("postfx: %ls missing (make shaders)", path); return NULL; }
    n = GetFileSize(h, NULL);
    buf = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (buf) ReadFile(h, buf, n, &got, NULL);
    CloseHandle(h);
    if (!buf || got != n) { if (buf) HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    hr = create(dev, buf, n, NULL, NULL, 0, NULL, &fx, &err);
    HeapFree(GetProcessHeap(), 0, buf);
    if (err) {
        hg_log("postfx: %ls: %s", name, (const char *)err->lpVtbl->GetBufferPointer(err));
        err->lpVtbl->Release(err);
    }
    if (FAILED(hr) || !fx) { hg_log("postfx: %ls NOT created (hr=0x%08lx)", name, (unsigned long)hr); return NULL; }
    return fx;
}

static int rt_tex(IDirect3DDevice9 *dev, UINT w, UINT h, IDirect3DTexture9 **t)
{
    return SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                    D3DPOOL_DEFAULT, t, NULL)) && *t;
}

/* SMAA's precomputed tables, from its headers: area RG8 -> A8L8 (read as .ra
 * in SMAA_HLSL_3), search R8 -> L8 */
static int lookup_tex(IDirect3DDevice9 *dev, UINT w, UINT h, D3DFORMAT f, const unsigned char *src,
                      UINT pitch, IDirect3DTexture9 **t)
{
    D3DLOCKED_RECT lr;
    UINT y;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, f, D3DPOOL_MANAGED, t, NULL)) || !*t) return 0;
    if (FAILED(IDirect3DTexture9_LockRect(*t, 0, &lr, NULL, 0))) return 0;
    for (y = 0; y < h; y++) memcpy((char *)lr.pBits + y * lr.Pitch, src + y * pitch, pitch);
    IDirect3DTexture9_UnlockRect(*t, 0);
    return 1;
}

static int res_ensure(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    D3DSURFACE_DESC d;
    if (FAILED(IDirect3DSurface9_GetDesc(bb, &d))) return 0;
    if (R.dev == dev && R.w == d.Width && R.h == d.Height) return !R.failed;
    if (R.failed && R.dev == dev) return 0;
    res_release();
    R.dev = dev; R.w = d.Width; R.h = d.Height; R.failed = 1;
    if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &R.sb))) return 0;
    R.smaa = load_effect(dev, L"smaa.fxo");
    R.ao = load_effect(dev, L"ao.fxo");
    R.cas = load_effect(dev, L"cas.fxo");           /* optional: no sharpening without it */
    R.fog = load_effect(dev, L"fog.fxo");           /* optional: no volumetric fog without it */
    R.bloom = load_effect(dev, L"bloom.fxo");       /* optional: no bloom or grade without it */
    if (!R.smaa || !R.ao) return 0;
    if (!rt_tex(dev, d.Width, d.Height, &R.color) || !rt_tex(dev, d.Width, d.Height, &R.edges) ||
        !rt_tex(dev, d.Width, d.Height, &R.blend) ||
        !rt_tex(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, &R.ao_a) ||
        !rt_tex(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, &R.ao_b)) {
        hg_log("postfx: render targets %ux%u NOT created", d.Width, d.Height);
        return 0;
    }
    /* the bounce's copy of the lit frame: float from the float scene (a copy
     * cannot convert float to 8-bit) */
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                              hdr_texture() ? D3DFMT_A16B16G16R16F : D3DFMT_A8R8G8B8,
                                              D3DPOOL_DEFAULT, &R.ao_col, NULL)))
        R.ao_col = NULL;                            /* optional: no bounce */
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                              D3DFMT_R32F, D3DPOOL_DEFAULT, &R.lindepth, NULL)))
        R.lindepth = NULL;                          /* optional: no soft particles */
    if (R.fog && (FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                                        D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &R.fog_a, NULL)) ||
                  FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                                        D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &R.fog_b, NULL)) ||
                  FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                                        D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &R.fog_h[0], NULL)) ||
                  FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                                        D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &R.fog_h[1], NULL)))) {
        hg_log("postfx: fog targets NOT created (no volumetric fog)");
        REL(R.fog_a); REL(R.fog_b); REL(R.fog_h[0]); REL(R.fog_h[1]);
    }
    /* the ground mist: the level mask, the volume and its points (optional) */
    if (R.fog_a) {
        UINT gw = d.Width / 4, gh = d.Height / 4, aw = VOL_T * VOL_N, ah = (VOL_S + VOL_T - 1) / VOL_T * VOL_N, x, y;
        float *pts = NULL;
        if (!rt_tex(dev, d.Width, d.Height, &R.level) ||
            FAILED(IDirect3DDevice9_CreateTexture(dev, aw, ah, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                                  D3DPOOL_DEFAULT, &R.vol[0], NULL)) ||
            FAILED(IDirect3DDevice9_CreateTexture(dev, aw, ah, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                                  D3DPOOL_DEFAULT, &R.vol[1], NULL)) ||
            FAILED(IDirect3DDevice9_CreateTexture(dev, aw, ah, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                                  D3DPOOL_DEFAULT, &R.vinj, NULL)) ||
            FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, gw * gh * 12, D3DUSAGE_WRITEONLY, D3DFVF_XYZ,
                                                       D3DPOOL_DEFAULT, &R.vpts, NULL)) ||
            FAILED(IDirect3DVertexBuffer9_Lock(R.vpts, 0, 0, (void **)&pts, 0))) {
            hg_log("postfx: ground mist targets NOT created (no ground mist)");
            REL(R.level); REL(R.vol[0]); REL(R.vol[1]); REL(R.vinj); REL(R.vpts);
        } else {
            for (y = 0; y < gh; y++)
                for (x = 0; x < gw; x++, pts += 3) {
                    pts[0] = (4 * x + 1.5f) / d.Width;
                    pts[1] = (4 * y + 1.5f) / d.Height;
                    pts[2] = 0;
                }
            IDirect3DVertexBuffer9_Unlock(R.vpts);
            R.npts = gw * gh;
            R.vol_valid = 0;
        }
    }
    if (R.bloom) {
        int i;
        for (i = 0; i < BLOOM_LEVELS; i++) {
            R.blw[i] = d.Width >> (i + 1); R.blh[i] = d.Height >> (i + 1);
            if (!R.blw[i]) R.blw[i] = 1;
            if (!R.blh[i]) R.blh[i] = 1;
            if (FAILED(IDirect3DDevice9_CreateTexture(dev, R.blw[i], R.blh[i], 1, D3DUSAGE_RENDERTARGET,
                                                      D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &R.bl[i], NULL))) {
                hg_log("postfx: bloom targets NOT created (no bloom or grade)");
                for (i = 0; i < BLOOM_LEVELS; i++) REL(R.bl[i]);
                REL(R.bloom);
                break;
            }
        }
    }
    if (hdr_texture() && R.ao_col &&
        (FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                               D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &R.sp_a, NULL)) ||
         FAILED(IDirect3DDevice9_CreateTexture(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, 1, D3DUSAGE_RENDERTARGET,
                                               D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &R.sp_b, NULL)))) {
        hg_log("postfx: spill targets NOT created (no light spill)");      /* optional */
        REL(R.sp_a); REL(R.sp_b);
    }
    if (!rt_tex(dev, d.Width, d.Height, &R.mask)) R.mask = NULL;     /* optional: backdrops get the fog */
    {
        /* the focus depth (optional: without it the shadow light is picked
         * from the camera's position) */
        int i, ok = 1;
        for (i = 0; i < 3 && ok; i++)
            ok = SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(dev, FOCUS_W, FOCUS_H, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                              &R.focus_rt[i], NULL));
        ok = ok && SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, FOCUS_W, FOCUS_H, D3DFMT_R32F, D3DPOOL_SYSTEMMEM,
                                                                          &R.focus_mem, NULL));
        if (!ok) { REL(R.focus_rt[0]); REL(R.focus_rt[1]); REL(R.focus_rt[2]); REL(R.focus_mem); }
        R.focus_n = 0;
    }
    if (R.bloom && hdr_texture()) {
        /* auto exposure (optional: without it, the exposure is as set) */
        int i, ok = 1;
        for (i = 0; i < LUM_LEVELS && ok; i++)
            ok = SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 256 >> (2 * i), 256 >> (2 * i), 1, D3DUSAGE_RENDERTARGET,
                                                          D3DFMT_G32R32F, D3DPOOL_DEFAULT, &R.lum[i], NULL));
        for (i = 0; i < 2 && ok; i++)
            ok = SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                                          D3DPOOL_DEFAULT, &R.adapt[i], NULL));
        if (!ok) {
            hg_log("postfx: auto exposure targets NOT created (exposure stays as set)");
            for (i = 0; i < LUM_LEVELS; i++) REL(R.lum[i]);
            REL(R.adapt[0]); REL(R.adapt[1]);
        }
    }
    if (!lookup_tex(dev, AREATEX_WIDTH, AREATEX_HEIGHT, D3DFMT_A8L8, areaTexBytes, AREATEX_PITCH, &R.area) ||
        !lookup_tex(dev, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, D3DFMT_L8, searchTexBytes, SEARCHTEX_PITCH, &R.search)) {
        hg_log("postfx: SMAA lookup textures NOT created");
        return 0;
    }
    R.failed = 0;
    hg_log("postfx: ready at %ux%u (SMAA 1x high, AO at half resolution)", d.Width, d.Height);
    return 1;
}

/* Before a Reset: everything in the default pool goes (src/device.c). */
static void rigid16_clear(void);

static void so_clear(void);

void postfx_reset(void)
{
    so_clear();
    rigid16_clear();
    volfog_reset();
    res_release();
    R.failed = 0;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

typedef struct { IDirect3DSurface9 *rt, *ds; } saved;

static void save(IDirect3DDevice9 *dev, saved *s)
{
    gfxprobe_own_passes(1);         /* our effect passes stay out of gfxprobe's bookkeeping */
    IDirect3DStateBlock9_Capture(R.sb);
    s->rt = NULL; s->ds = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &s->rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &s->ds);
}

static void restore(IDirect3DDevice9 *dev, saved *s)
{
    IDirect3DDevice9_SetRenderTarget(dev, 0, s->rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, s->ds);
    IDirect3DStateBlock9_Apply(R.sb);
    REL(s->rt); REL(s->ds);
    gfxprobe_own_passes(0);
}

static void target(IDirect3DDevice9 *dev, IDirect3DTexture9 *t)
{
    IDirect3DSurface9 *s = NULL;
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &s);
    IDirect3DDevice9_SetRenderTarget(dev, 0, s);
    REL(s);
}

/* A fullscreen quad, shifted half a pixel for D3D9's pixel centres. */
static void quad(IDirect3DDevice9 *dev, UINT w, UINT h)
{
    float px = 1.0f / w, py = 1.0f / h;
    float q[4][5] = {
        { -1.0f - px,  1.0f + py, 0.5f, 0.0f, 0.0f },
        {  1.0f - px,  1.0f + py, 0.5f, 1.0f, 0.0f },
        { -1.0f - px, -1.0f + py, 0.5f, 0.0f, 1.0f },
        {  1.0f - px, -1.0f + py, 0.5f, 1.0f, 1.0f },
    };
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

static void run(ID3DXEffect *fx, const char *tech, IDirect3DDevice9 *dev, UINT w, UINT h)
{
    UINT n = 0;
    fx->lpVtbl->SetTechnique(fx, fx->lpVtbl->GetTechniqueByName(fx, tech));
    if (FAILED(fx->lpVtbl->Begin(fx, &n, D3DXFX_DONOTSAVESTATE))) return;
    fx->lpVtbl->BeginPass(fx, 0);
    quad(dev, w, h);
    fx->lpVtbl->EndPass(fx);
    fx->lpVtbl->End(fx);
}

static void set_vec(ID3DXEffect *fx, const char *name, float x, float y, float z, float w)
{
    D3DXVECTOR4 v = { x, y, z, w };
    fx->lpVtbl->SetVector(fx, fx->lpVtbl->GetParameterByName(fx, NULL, name), &v);
}

static void set_tex(ID3DXEffect *fx, const char *name, IDirect3DTexture9 *t)
{
    fx->lpVtbl->SetTexture(fx, fx->lpVtbl->GetParameterByName(fx, NULL, name), (IDirect3DBaseTexture9 *)t);
}

/* The back buffer, if it is render target 0 now; the caller releases it. */
static IDirect3DSurface9 *bound_back_buffer(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL, *rt = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    if (rt != bb) REL(bb);
    REL(rt);
    return bb;
}

/* The finished 3D frame: SMAA, then CAS sharpening (either may be off). */
static void smaa(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    IDirect3DSurface9 *cs = NULL;
    saved s;
    save(dev, &s);
    IDirect3DTexture9_GetSurfaceLevel(R.color, 0, &cs);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    if (g_smaa_pass) {
        IDirect3DDevice9_StretchRect(dev, bb, NULL, cs, NULL, D3DTEXF_NONE);
        set_vec(R.smaa, "gvSmaaMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
        set_tex(R.smaa, "colorTex2D", R.color);
        set_tex(R.smaa, "edgesTex2D", R.edges);
        set_tex(R.smaa, "blendTex2D", R.blend);
        set_tex(R.smaa, "areaTex2D", R.area);
        set_tex(R.smaa, "searchTex2D", R.search);
        target(dev, R.edges);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        run(R.smaa, "LumaEdgeDetection", dev, R.w, R.h);
        target(dev, R.blend);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        run(R.smaa, "BlendWeightCalculation", dev, R.w, R.h);
        IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
        run(R.smaa, "NeighborhoodBlending", dev, R.w, R.h);
    }
    if (g_cas > 0 && R.cas) {
        IDirect3DDevice9_StretchRect(dev, bb, NULL, cs, NULL, D3DTEXF_NONE);
        IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
        set_vec(R.cas, "gvCasMetrics", 1.0f / R.w, 1.0f / R.h, g_cas / 100.0f, 0);
        set_tex(R.cas, "colorTex2D", R.color);
        run(R.cas, "Sharpen", dev, R.w, R.h);
    }
    REL(cs);
    restore(dev, &s);
    InterlockedIncrement(&g_smaa_runs);
}

static int projection(IDirect3DDevice9 *dev, float *p11, float *p22, float *p33, float *p43)
{
    D3DMATRIX m;
    float c[16];
    static LONG logged, last_src;
    int src = 0;
    /* the engine's own camera projection first: it draws through effects,
     * so the device transform can be stale (character select left a 19
     * degree one behind) */
    if (gfxprobe_camera_proj(c) && fabsf(c[11] - 1.0f) < 1e-4f && fabsf(c[15]) < 1e-4f && c[0] > 0) {
        *p11 = c[0]; *p22 = c[5]; *p33 = c[10]; *p43 = c[14];
        src = 2;
    } else if (SUCCEEDED(IDirect3DDevice9_GetTransform(dev, D3DTS_PROJECTION, &m)) &&
        fabsf(m._34 - 1.0f) < 1e-4f && fabsf(m._44) < 1e-4f && m._11 > 0 && m._33 > 0) {
        *p11 = m._11; *p22 = m._22; *p33 = m._33; *p43 = m._43;
        src = 1;
    }
    if (src != last_src) { last_src = src; logged = 0; }   /* log each change of source */
    if (InterlockedIncrement(&logged) <= 2)
        hg_log("postfx: camera projection from %s: _11 %.3f _22 %.3f _33 %.5f _43 %.4f",
               src == 1 ? "the device" : src == 2 ? "the shadow parameters" : "nowhere (AO off)",
               src ? *p11 : 0, src ? *p22 : 0, src ? *p33 : 0, src ? *p43 : 0);
    return src != 0;
}

void plshadow_focus(const float p[3]);

/* The point the camera looks at (for the point-light shadow's choice of
 * light, src/plshadow.c): the view depth a little below the screen's centre,
 * where the player stands in the third-person view, copied into a small ring
 * and read back two frames later (by then the GPU is done with it, so the
 * read does not stall); placed along the camera's forward axis.
 *
 * The nearest of an 8x4 grid over the middle of the screen, not one pixel:
 * on the character select the preview swayed across that pixel, the depth
 * jumped between the character (2.5) and the wall behind it (8), and the
 * shadow went back and forth between the lamp by the character and the one
 * on the wall (the log, 2026-09-24). The nearest thing in the middle is the
 * player, or what stands right in front of them. */
static void focus(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *ld = NULL;
    RECT r;
    LONG fr;
    const volfog_state *v = volfog_get(&fr);
    UINT hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    if (!R.focus_mem || !R.lindepth) return;
    r.left = hw * 40 / 100; r.right = hw * 60 / 100; r.top = hh * 48 / 100; r.bottom = hh * 62 / 100;
    if (r.right - r.left < FOCUS_W || r.bottom - r.top < FOCUS_H) return;
    IDirect3DTexture9_GetSurfaceLevel(R.lindepth, 0, &ld);
    if (ld) IDirect3DDevice9_StretchRect(dev, ld, &r, R.focus_rt[R.focus_n % 3], NULL, D3DTEXF_NONE);
    REL(ld);
    R.focus_n++;
    if (R.focus_n >= 3 && v->cam_frame == fr) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, R.focus_rt[R.focus_n % 3], R.focus_mem)) &&
            SUCCEEDED(IDirect3DSurface9_LockRect(R.focus_mem, &lr, NULL, D3DLOCK_READONLY))) {
            float z = 1e30f, p[3];
            const float *m = v->inv_view;               /* row 2: the camera's forward axis in the world */
            int x, y;
            for (y = 0; y < FOCUS_H; y++)
                for (x = 0; x < FOCUS_W; x++) {
                    float d = ((const float *)((const char *)lr.pBits + y * lr.Pitch))[x];
                    if (d > 0.1f && d < z) z = d;
                }
            IDirect3DSurface9_UnlockRect(R.focus_mem);
            if (z > 0.1f && z < 1e6f) {
                /* no further than the player stands: down a corridor the
                 * centre is 20 units off, and the shadow went to the lamps
                 * there (2026-09-24) */
                if (z > 8.0f) z = 8.0f;
                p[0] = v->eye[0] + m[8] * z; p[1] = v->eye[1] + m[9] * z; p[2] = v->eye[2] + m[10] * z;
                plshadow_focus(p);
            }
        }
    }
}

static void set_mat(ID3DXEffect *fx, const char *name, const float *m);

/* Backdrops (shaders/ao.fx BackdropMask): the engine draws painted
 * backdrops such as the character select's skyline card with simple.fxo,
 * and the fog covered them (the ground mist, half the card). From
 * gfxprobe, right after each simple.fxo draw into the main view: the same
 * draw again, the engine's vertex shader still bound, into a mask, white;
 * the fog leaves masked pixels as drawn. Cleared at the first draw of a
 * frame; a frame with none gives the fog no mask at all. */
static LONG g_pfx_frame, g_mask_frame = -1;

void postfx_backdrop(IDirect3DDevice9 *dev, void *orig, int dp, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                     UINT si, UINT pc)
{
    typedef HRESULT (STDMETHODCALLTYPE *dip_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    typedef HRESULT (STDMETHODCALLTYPE *dp_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
    IDirect3DSurface9 *ds = NULL, *main = NULL;
    IDirect3DTexture9 *dt = device_depth_texture();
    UINT np = 0;
    int is_main;
    saved s;
    if (!R.mask || !R.ao || !R.sb || !dt) return;
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DTexture9_GetSurfaceLevel(dt, 0, &main);
    is_main = ds && ds == main;
    REL(ds); REL(main);
    if (!is_main) return;
    save(dev, &s);
    target(dev, R.mask);
    if (g_mask_frame != g_pfx_frame) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        g_mask_frame = g_pfx_frame;
    }
    R.ao->lpVtbl->SetTechnique(R.ao, R.ao->lpVtbl->GetTechniqueByName(R.ao, "BackdropMask"));
    if (SUCCEEDED(R.ao->lpVtbl->Begin(R.ao, &np, D3DXFX_DONOTSAVESTATE))) {
        R.ao->lpVtbl->BeginPass(R.ao, 0);
        if (dp) ((dp_fn)orig)(dev, t, (UINT)bv, pc);
        else ((dip_fn)orig)(dev, t, bv, mi, nv, si, pc);
        R.ao->lpVtbl->EndPass(R.ao);
        R.ao->lpVtbl->End(R.ao);
    }
    restore(dev, &s);
}

/* Debug view: meshes only the shadow pass draws (invisible geometry that
 * casts shadows, as the barbed wire on the character select, 2026-09-24).
 * The shadow pass's rigid casters are queued (their vertex shader, world
 * rows c0-c3, buffers) and drawn again after the opaque scene with the main
 * camera in the shadow shader's View (c4-c7) and Projection (c8-c11), flat
 * magenta, only where the depth buffer holds nothing as near: what shows is
 * in the shadow pass and missing from the view. */
#define SO_MAX 512
static volatile LONG g_so_on;
static struct {
    IDirect3DVertexShader9 *vs; IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexBuffer9 *vb; UINT off, stride; IDirect3DIndexBuffer9 *ib;
    float world[16];
    D3DPRIMITIVETYPE t; INT bv; UINT mi, nv, si, pc;
} g_so[SO_MAX];
static int g_nso;
int plshadow_caster_kind(void);
int gfxprobe_camera_proj(float *m);

static void so_clear(void)
{
    int i;
    for (i = 0; i < g_nso; i++) {
        REL(g_so[i].vs); REL(g_so[i].decl); REL(g_so[i].vb); REL(g_so[i].ib);
    }
    g_nso = 0;
}

void hg_gfx_set_shadow_only(int on) { InterlockedExchange(&g_so_on, on ? 1 : 0); }
int  hg_gfx_shadow_only(void) { return (int)g_so_on; }

/* From gfxprobe, at every shadow-pass draw */
void postfx_shadow_caster(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
    int n = g_nso;
    if (!g_so_on || n >= SO_MAX || plshadow_caster_kind() != 1) return;
    ZeroMemory(&g_so[n], sizeof g_so[n]);
    IDirect3DDevice9_GetVertexShader(dev, &g_so[n].vs);
    IDirect3DDevice9_GetVertexDeclaration(dev, &g_so[n].decl);
    IDirect3DDevice9_GetStreamSource(dev, 0, &g_so[n].vb, &g_so[n].off, &g_so[n].stride);
    IDirect3DDevice9_GetIndices(dev, &g_so[n].ib);
    IDirect3DDevice9_GetVertexShaderConstantF(dev, 0, g_so[n].world, 4);
    if (!g_so[n].vs || !g_so[n].decl || !g_so[n].vb || !g_so[n].ib) {
        REL(g_so[n].vs); REL(g_so[n].decl); REL(g_so[n].vb); REL(g_so[n].ib);
        return;
    }
    g_so[n].t = t; g_so[n].bv = bv; g_so[n].mi = mi; g_so[n].nv = nv; g_so[n].si = si; g_so[n].pc = pc;
    g_nso = n + 1;
}

static void so_draw(IDirect3DDevice9 *dev)
{
    LONG fr;
    const volfog_state *v = volfog_get(&fr);
    float P[16], vt[16], pt[16];
    UINT np = 0;
    int i, r, c;
    saved s;
    if (!g_nso || !R.ao || !R.sb || v->cam_frame != fr || !gfxprobe_camera_proj(P)) { so_clear(); return; }
    /* the registers hold each matrix transposed: register i is column i */
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) { vt[r * 4 + c] = v->view[c * 4 + r]; pt[r * 4 + c] = P[c * 4 + r]; }
    save(dev, &s);
    IDirect3DDevice9_SetDepthStencilSurface(dev, s.ds);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, 0);
    R.ao->lpVtbl->SetTechnique(R.ao, R.ao->lpVtbl->GetTechniqueByName(R.ao, "ShadowOnly"));
    if (SUCCEEDED(R.ao->lpVtbl->Begin(R.ao, &np, D3DXFX_DONOTSAVESTATE))) {
        union { float f; DWORD d; } bias = { 0.0002f };  /* equal depth (the mesh itself, drawn) fails */
        R.ao->lpVtbl->BeginPass(R.ao, 0);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, bias.d);
        for (i = 0; i < g_nso; i++) {
            IDirect3DDevice9_SetVertexShader(dev, g_so[i].vs);
            IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, g_so[i].world, 4);
            IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, vt, 4);
            IDirect3DDevice9_SetVertexShaderConstantF(dev, 8, pt, 4);
            IDirect3DDevice9_SetVertexDeclaration(dev, g_so[i].decl);
            IDirect3DDevice9_SetStreamSource(dev, 0, g_so[i].vb, g_so[i].off, g_so[i].stride);
            IDirect3DDevice9_SetIndices(dev, g_so[i].ib);
            IDirect3DDevice9_DrawIndexedPrimitive(dev, g_so[i].t, g_so[i].bv, g_so[i].mi, g_so[i].nv, g_so[i].si, g_so[i].pc);
        }
        R.ao->lpVtbl->EndPass(R.ao);
        R.ao->lpVtbl->End(R.ao);
    }
    restore(dev, &s);
    {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 5000) { last = now; hg_log("postfx: shadow-only view: %d rigid casters redrawn", g_nso); }
    }
    so_clear();
}

/* Debug view: depth without colour. Every depth pre-pass draw into the
 * main view marks dnc_z, every colour draw marks dnc_c (the same draw
 * again into the mask, the engine's vertex shader bound); after the opaque
 * scene, magenta where only depth landed: geometry the scene holds for
 * depth (contact shadows, AO) that never shows (a razor-wire coil's spiral
 * contact shadow on the character select with no wire in sight,
 * 2026-09-24). Alpha test is not kept, so cut-out colour draws count whole. */
static volatile LONG g_dnc_on;
static IDirect3DTexture9 *g_dnc_z, *g_dnc_c;
static LONG g_dnc_frame = -1;
static struct { void *vs; int v3; } g_vsver[64];
static int g_nvsver;
static volatile LONG g_dnc_z_n, g_dnc_c_n, g_dnc_shows;
void hg_gfx_set_dnc(int mode) { InterlockedExchange(&g_dnc_on, mode < 0 || mode > 3 ? 0 : mode); }
int  hg_gfx_dnc(void) { return (int)g_dnc_on; }

static int vs_is3(IDirect3DDevice9 *dev)
{
    IDirect3DVertexShader9 *vs = NULL;
    int i, v3 = 0;
    UINT size = 0;
    IDirect3DDevice9_GetVertexShader(dev, &vs);
    if (!vs) return 0;
    for (i = 0; i < g_nvsver; i++) if (g_vsver[i].vs == vs) { v3 = g_vsver[i].v3; REL(vs); return v3; }
    if (SUCCEEDED(IDirect3DVertexShader9_GetFunction(vs, NULL, &size)) && size >= 4) {
        DWORD *code = (DWORD *)HeapAlloc(GetProcessHeap(), 0, size);
        if (code && SUCCEEDED(IDirect3DVertexShader9_GetFunction(vs, code, &size)))
            v3 = ((code[0] >> 8) & 0xff) >= 3;
        if (code) HeapFree(GetProcessHeap(), 0, code);
    }
    if (g_nvsver < 64) { g_vsver[g_nvsver].vs = vs; g_vsver[g_nvsver].v3 = v3; g_nvsver++; }
    REL(vs);
    return v3;
}

/* From gfxprobe after every main-view draw: marks the depth mask if it
 * wrote depth and the colour mask if it wrote colour, whatever the effect
 * (first cut: only _zbuffer counted as depth, so a depth-only draw from any
 * other effect showed in no mode, 2026-09-24). */
static void dnc_one(IDirect3DDevice9 *dev, void *orig, int colour, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                    UINT si, UINT pc);

void postfx_dnc_mark(IDirect3DDevice9 *dev, void *orig, int unused, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                     UINT si, UINT pc)
{
    DWORD cw = 0, zw = 0, ze = 0;
    (void)unused;
    if (!g_dnc_on) return;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &ze);
    if (zw && ze) dnc_one(dev, orig, 0, t, bv, mi, nv, si, pc);
    if (cw) dnc_one(dev, orig, 1, t, bv, mi, nv, si, pc);
}

static void dnc_one(IDirect3DDevice9 *dev, void *orig, int colour, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                    UINT si, UINT pc)
{
    typedef HRESULT (STDMETHODCALLTYPE *dip_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    IDirect3DSurface9 *ds = NULL, *main = NULL;
    IDirect3DTexture9 *dt = device_depth_texture();
    DWORD cw = 0;
    UINT np = 0;
    int is_main;
    saved s;
    (void)cw;
    if (!g_dnc_on || !R.ao || !R.sb || !dt) return;
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DTexture9_GetSurfaceLevel(dt, 0, &main);
    is_main = ds && ds == main;
    REL(ds); REL(main);
    if (!is_main) return;
    if (!g_dnc_z && (!rt_tex(dev, R.w, R.h, &g_dnc_z) || !rt_tex(dev, R.w, R.h, &g_dnc_c))) { REL(g_dnc_z); REL(g_dnc_c); return; }
    save(dev, &s);
    if (g_dnc_frame != g_pfx_frame) {
        g_dnc_frame = g_pfx_frame;
        target(dev, g_dnc_z); IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        target(dev, g_dnc_c); IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    }
    target(dev, colour ? g_dnc_c : g_dnc_z);
    InterlockedIncrement(colour ? &g_dnc_c_n : &g_dnc_z_n);
    R.ao->lpVtbl->SetTechnique(R.ao, R.ao->lpVtbl->GetTechniqueByName(R.ao, vs_is3(dev) ? "DncMark3" : "DncMark2"));
    if (SUCCEEDED(R.ao->lpVtbl->Begin(R.ao, &np, D3DXFX_DONOTSAVESTATE))) {
        R.ao->lpVtbl->BeginPass(R.ao, 0);
        ((dip_fn)orig)(dev, t, bv, mi, nv, si, pc);
        R.ao->lpVtbl->EndPass(R.ao);
        R.ao->lpVtbl->End(R.ao);
    }
    restore(dev, &s);
}

static void dnc_release(void) { REL(g_dnc_z); REL(g_dnc_c); g_nvsver = 0; }

static void dnc_show(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    saved s;
    if (!g_dnc_on) return;
    {
        static DWORD last;
        DWORD now = GetTickCount();
        if ((!g_dnc_z || g_dnc_frame != g_pfx_frame) && now - last >= 5000) {
            last = now;
            hg_log("postfx: depth-without-colour view: nothing marked this frame (targets %s, %ld depth / %ld colour marks so far)",
                   g_dnc_z ? "made" : "NOT made", g_dnc_z_n, g_dnc_c_n);
        }
    }
    if (!g_dnc_z || g_dnc_frame != g_pfx_frame) return;
    save(dev, &s);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    set_vec(R.ao, "gvDnc", (float)g_dnc_on, 0, 0, 0);
    set_tex(R.ao, "dncZTex2D", g_dnc_z);
    set_tex(R.ao, "dncCTex2D", g_dnc_c);
    run(R.ao, "DncShow", dev, R.w, R.h);
    set_tex(R.ao, "dncZTex2D", NULL);
    set_tex(R.ao, "dncCTex2D", NULL);
    restore(dev, &s);
    InterlockedIncrement(&g_dnc_shows);
    {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 5000) {
            last = now;
            hg_log("postfx: depth-without-colour view (mode %ld): %ld depth draws and %ld colour draws marked, shown %ld times",
                   g_dnc_on, g_dnc_z_n, g_dnc_c_n, g_dnc_shows);
        }
    }
}

static LONG g_contact_runs;
int hg_in_game(void);           /* src/panel.c: a local player exists (not a menu scene) */
int hg_charselect(void);        /* src/uiext.c: the character select is open */

/* Contact shadows (shaders/ao.fx Contact), indoors, after the AO and before
 * the transparent half: a short march from each surface towards its light
 * through the depth buffer, darkening where something close blocks it (feet
 * and props that floated over the coarse indoor shadow maps). The light is
 * the nearby lights' directions weighted by how much each lights the
 * surface, plus some from straight above: smooth across a room. */
static void contact(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    UINT hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    float p11, p22, p33, p43, k, pr[12][4], col[12][4];
    D3DXVECTOR4 lp[12], lc[12];
    const volfog_state *v;
    LONG fr;
    int i, n;
    saved s;
    k = g_contact / 100.0f * (1.0f - g_outdoors);
    if (k < 0.01f && !g_contact_show) return;
    v = volfog_get(&fr);
    if (v->cam_frame != fr || !projection(dev, &p11, &p22, &p33, &p43)) return;
    n = plshadow_lights_near(v->eye, 40.0f, pr, col, 12);
    {
        const float *m = v->view;
        for (i = 0; i < 12; i++) {
            D3DXVECTOR4 z = { 0, 0, 0, 0 };
            lp[i] = z; lc[i] = z;
            if (i >= n) continue;
            lp[i].x = pr[i][0] * m[0] + pr[i][1] * m[4] + pr[i][2] * m[8] + m[12];
            lp[i].y = pr[i][0] * m[1] + pr[i][1] * m[5] + pr[i][2] * m[9] + m[13];
            lp[i].z = pr[i][0] * m[2] + pr[i][1] * m[6] + pr[i][2] * m[10] + m[14];
            lp[i].w = pr[i][3];
            lc[i].x = col[i][0]; lc[i].y = col[i][1]; lc[i].z = col[i][2];
        }
        R.ao->lpVtbl->SetVectorArray(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "gvSpillLights"), lp, 12);
        R.ao->lpVtbl->SetVectorArray(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "gvSpillCol"), lc, 12);
        /* the world's up (z) in view space */
        set_vec(R.ao, "gvContactUp", m[8], m[9], m[10], 0);
    }
    save(dev, &s);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);   /* sampled below */
    set_vec(R.ao, "gvAoMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
    set_vec(R.ao, "gvAoProj", p11, p22, p33, p43);
    set_vec(R.ao, "gvContact", g_contact_show ? g_contact / 100.0f : k, g_contact_len / 100.0f, (float)n, 0);
    set_tex(R.ao, "depthTex2D", device_depth_texture());
    /* the AO's half-size targets are free again once it has applied */
    target(dev, R.ao_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 0);
    run(R.ao, "Contact", dev, hw, hh);
    target(dev, R.ao_b);
    set_tex(R.ao, "aoTex2D", R.ao_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 1.0f / hw, 0);
    run(R.ao, "Blur", dev, hw, hh);
    target(dev, R.ao_a);
    set_tex(R.ao, "aoTex2D", R.ao_b);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 1.0f / hh);
    run(R.ao, "Blur", dev, hw, hh);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    set_tex(R.ao, "aoTex2D", R.ao_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 0);
    run(R.ao, g_contact_show ? "ContactShow" : "ContactApply", dev, R.w, R.h);
    set_tex(R.ao, "depthTex2D", NULL);
    set_tex(R.ao, "aoTex2D", NULL);
    restore(dev, &s);
    InterlockedIncrement(&g_contact_runs);
}

int gfxprobe_ambient(float a[3]);

/* Rigid16 background meshes (shaders/ao.fx Wire): position and one uv, no
 * normals, and no colour technique in any 2018 background effect, so the
 * engine drew their depth and never their colour (the barbed wire on the
 * character select, missing in everyone's game). Each _zbuffer RigidShader16
 * draw into the main view is queued (its buffers, layout, matrix, texture)
 * and drawn again in colour once the opaque scene is done, beside the AO:
 * drawn at once, in the depth pre-pass, the engine's clear of the colour
 * target before its colour pass wiped it (2026-09-24). Only the ones the
 * engine did not draw in colour itself (postfx_note_draw): London's skyline
 * backdrop is Rigid16 too and has a colour draw of its own, and drawing it
 * again fogged it to a grey wall (2026-09-24). */
#define RIGID16_MAX 32
static struct {
    IDirect3DVertexBuffer9 *vb; UINT off, stride;
    IDirect3DIndexBuffer9 *ib;
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DBaseTexture9 *tex;
    D3DPRIMITIVETYPE t; INT bv; UINT mi, nv, si, pc;
    D3DXMATRIX wvp;
    int drawn;                          /* the engine drew it in colour after all */
} g_r16[RIGID16_MAX];
static int g_nr16;

static void rigid16_clear(void)
{
    int i;
    for (i = 0; i < g_nr16; i++) {
        REL(g_r16[i].vb); REL(g_r16[i].ib); REL(g_r16[i].decl); REL(g_r16[i].tex);
    }
    g_nr16 = 0;
}

void postfx_rigid16(IDirect3DDevice9 *dev, ID3DXEffect *zfx, void *orig_dip, D3DPRIMITIVETYPE t, INT bv,
                    UINT mi, UINT nv, UINT si, UINT pc)
{
    IDirect3DSurface9 *ds = NULL, *main = NULL;
    IDirect3DTexture9 *dt = device_depth_texture();
    D3DXHANDLE h;
    int is_main;
    (void)orig_dip;
    if (!g_rigid16 || !dt || g_nr16 >= RIGID16_MAX) return;
    /* the main view only (not an occlusion test or a shadow) */
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DTexture9_GetSurfaceLevel(dt, 0, &main);
    is_main = ds && ds == main;
    REL(ds); REL(main);
    if (!is_main) return;
    {
        int n = g_nr16;
        ZeroMemory(&g_r16[n], sizeof g_r16[n]);
        h = zfx->lpVtbl->GetParameterByName(zfx, NULL, "WorldViewProjection");
        if (!h || FAILED(zfx->lpVtbl->GetMatrix(zfx, h, &g_r16[n].wvp))) return;
        IDirect3DDevice9_GetTexture(dev, 0, &g_r16[n].tex);
        if (!g_r16[n].tex) return;                      /* nothing to cut it out with */
        IDirect3DDevice9_GetStreamSource(dev, 0, &g_r16[n].vb, &g_r16[n].off, &g_r16[n].stride);
        IDirect3DDevice9_GetIndices(dev, &g_r16[n].ib);
        IDirect3DDevice9_GetVertexDeclaration(dev, &g_r16[n].decl);
        if (!g_r16[n].vb || !g_r16[n].ib || !g_r16[n].decl) {
            REL(g_r16[n].vb); REL(g_r16[n].ib); REL(g_r16[n].decl); REL(g_r16[n].tex);
            return;
        }
        g_r16[n].t = t; g_r16[n].bv = bv; g_r16[n].mi = mi; g_r16[n].nv = nv; g_r16[n].si = si; g_r16[n].pc = pc;
        g_nr16 = n + 1;
    }
}

/* From gfxprobe, after every draw that is not the depth pre-pass: a colour
 * draw of a queued mesh into the main view marks it drawn (the engine had
 * a technique for it after all). */
void postfx_note_draw(IDirect3DDevice9 *dev, INT bv, UINT si, UINT pc)
{
    int i;
    for (i = 0; i < g_nr16; i++) {
        IDirect3DVertexBuffer9 *vb = NULL;
        IDirect3DIndexBuffer9 *ib = NULL;
        IDirect3DSurface9 *ds = NULL, *main = NULL;
        IDirect3DTexture9 *dt;
        UINT off, stride;
        DWORD cw = 0;
        int same;
        if (g_r16[i].drawn || g_r16[i].pc != pc || g_r16[i].si != si || g_r16[i].bv != bv) continue;
        IDirect3DDevice9_GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
        if (!cw) return;
        IDirect3DDevice9_GetStreamSource(dev, 0, &vb, &off, &stride);
        IDirect3DDevice9_GetIndices(dev, &ib);
        same = vb == g_r16[i].vb && ib == g_r16[i].ib;
        REL(vb); REL(ib);
        if (!same) continue;
        /* the main view, not a shadow map drawing the same mesh */
        dt = device_depth_texture();
        if (!dt) return;
        IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
        IDirect3DTexture9_GetSurfaceLevel(dt, 0, &main);
        same = ds && ds == main;
        REL(ds); REL(main);
        if (same) g_r16[i].drawn = 1;
        return;
    }
}

/* After the opaque scene (postfx_before_transparent): the queued meshes in colour. */
static void rigid16_draw(IDirect3DDevice9 *dev)
{
    float amb[3];
    UINT np = 0;
    int i;
    saved s;
    const volfog_state *v;
    LONG fr;
    if (!g_nr16 || !R.ao || !R.sb) { rigid16_clear(); return; }
    save(dev, &s);
    if (!gfxprobe_ambient(amb)) amb[0] = amb[1] = amb[2] = 0.25f;
    /* no normals: the ambient and a fixed share for the sun and bounce */
    set_vec(R.ao, "gvWireLight", amb[0] + 0.35f, amb[1] + 0.35f, amb[2] + 0.35f, 0);
    v = volfog_get(&fr);
    if (v->fog_seen && v->fog_max > v->fog_min) {
        set_vec(R.ao, "gvWireFog", v->fog_min, v->fog_max, 1, 0);
        set_vec(R.ao, "gvWireFogCol", v->fog_col[0], v->fog_col[1], v->fog_col[2], 0);
    } else {
        set_vec(R.ao, "gvWireFog", 0, 0, 0, 0);
    }
    IDirect3DDevice9_SetDepthStencilSurface(dev, s.ds);
    R.ao->lpVtbl->SetTechnique(R.ao, R.ao->lpVtbl->GetTechniqueByName(R.ao, "Wire"));
    if (SUCCEEDED(R.ao->lpVtbl->Begin(R.ao, &np, D3DXFX_DONOTSAVESTATE))) {
        R.ao->lpVtbl->BeginPass(R.ao, 0);
        for (i = 0; i < g_nr16; i++) {
            if (g_r16[i].drawn) continue;
            R.ao->lpVtbl->SetMatrix(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "gmWireWVP"), &g_r16[i].wvp);
            R.ao->lpVtbl->SetTexture(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "wireTex2D"), g_r16[i].tex);
            R.ao->lpVtbl->CommitChanges(R.ao);
            IDirect3DDevice9_SetVertexDeclaration(dev, g_r16[i].decl);
            IDirect3DDevice9_SetStreamSource(dev, 0, g_r16[i].vb, g_r16[i].off, g_r16[i].stride);
            IDirect3DDevice9_SetIndices(dev, g_r16[i].ib);
            IDirect3DDevice9_DrawIndexedPrimitive(dev, g_r16[i].t, g_r16[i].bv, g_r16[i].mi, g_r16[i].nv,
                                                  g_r16[i].si, g_r16[i].pc);
        }
        R.ao->lpVtbl->EndPass(R.ao);
        R.ao->lpVtbl->End(R.ao);
    }
    R.ao->lpVtbl->SetTexture(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "wireTex2D"), NULL);
    restore(dev, &s);
    {
        static DWORD last;
        DWORD now = GetTickCount();
        int k, missing = 0;
        for (k = 0; k < g_nr16; k++) missing += !g_r16[k].drawn;
        if (now - last >= 10000) {
            last = now;
            hg_log("postfx: Rigid16: %d queued, %d drawn by the engine, %d drawn here (depth only in stock)",
                   g_nr16, g_nr16 - missing, missing);
        }
    }
    rigid16_clear();
}

/* The scene's linear view depth, half resolution, for soft particles and
 * the focus. */
static void lin_depth(IDirect3DDevice9 *dev)
{
    UINT hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    float p11, p22, p33, p43;
    saved s;
    if (!R.lindepth || !projection(dev, &p11, &p22, &p33, &p43)) return;
    save(dev, &s);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);   /* sampled below */
    set_vec(R.ao, "gvAoMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
    set_vec(R.ao, "gvAoProj", p11, p22, p33, p43);
    set_tex(R.ao, "depthTex2D", device_depth_texture());
    target(dev, R.lindepth);
    run(R.ao, "LinearDepth", dev, hw, hh);
    set_tex(R.ao, "depthTex2D", NULL);
    restore(dev, &s);
    g_depth_done = 1;
    focus(dev);
}

/* The sun for the occlusion (ao.fx sun_share): its direction and shadow
 * maps as the fog uses them, seen in the last few frames; none, no sun. */
static void ao_sun(void)
{
    LONG fr;
    const volfog_state *v = volfog_get(&fr);
    int sun = g_ao_sun > 0 && v->cam_frame == fr && fr - v->sun_frame <= 8 &&
              fr - v->maps_frame <= 8 && v->fine && v->nearmap;
    if (!sun) { set_vec(R.ao, "gvAoSun", 0, 0, 0, 0); return; }
    set_mat(R.ao, "gmAoInvView", v->inv_view);
    set_vec(R.ao, "gvAoSun", v->to_sun[0], v->to_sun[1], v->to_sun[2], g_ao_sun / 100.0f);
    set_mat(R.ao, "gmAoNear", v->near_m);
    set_mat(R.ao, "gmAoFine", v->fine_m);
    R.ao->lpVtbl->SetTexture(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "nearTex2D"), v->nearmap);
    R.ao->lpVtbl->SetTexture(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "fineTex2D"), v->fine);
}

/* The engine's distance fog for the AO: its start moved by the LOOK fog
 * start as the materials move it (fog_min in shaders/ultra.hlsl). */
static void ao_fog(void)
{
    LONG fr;
    const volfog_state *v = volfog_get(&fr);
    volatile long *look = settings_find("look.fog_start", NULL, NULL);
    float lo, hi;
    if (!v->fog_seen || v->fog_max <= v->fog_min) { set_vec(R.ao, "gvAoFog", 0, 0, 0, 0); return; }
    hi = v->fog_max;
    lo = v->fog_min + (look ? *look / 100.0f : 0.0f) * (hi - v->fog_min);
    set_vec(R.ao, "gvAoFog", lo, hi, 1, 0);
}

static void ao(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    UINT hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    float p11, p22, p33, p43;
    saved s;
    if (!projection(dev, &p11, &p22, &p33, &p43)) return;
    /* the lit frame so far, half size, before anything is drawn over it */
    {
        IDirect3DSurface9 *cs = NULL;
        int bleed = g_ao_bleed > 0 && R.ao_col;
        if (bleed) {
            IDirect3DTexture9_GetSurfaceLevel(R.ao_col, 0, &cs);
            bleed = cs && SUCCEEDED(IDirect3DDevice9_StretchRect(dev, bb, NULL, cs, NULL, D3DTEXF_LINEAR));
            REL(cs);
        }
        set_tex(R.ao, "colTex2D", bleed ? R.ao_col : NULL);
        set_vec(R.ao, "gvAoBleed", bleed ? g_ao_bleed / 100.0f : 0.0f, 0, 0, 0);
    }
    save(dev, &s);
    ao_sun();
    ao_fog();
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);   /* sampled below */
    set_vec(R.ao, "gvAoMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
    set_vec(R.ao, "gvAoProj", p11, p22, p33, p43);
    set_vec(R.ao, "gvAoParams", g_ao_radius / 100.0f, g_ao_strength / 100.0f * 1.5f, 60.0f, 150.0f);
    set_tex(R.ao, "depthTex2D", device_depth_texture());
    target(dev, R.ao_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 0);
    run(R.ao, "Occlusion", dev, hw, hh);
    target(dev, R.ao_b);
    set_tex(R.ao, "aoTex2D", R.ao_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 1.0f / hw, 0);
    run(R.ao, "Blur", dev, hw, hh);
    target(dev, R.ao_a);
    set_tex(R.ao, "aoTex2D", R.ao_b);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 1.0f / hh);
    run(R.ao, "Blur", dev, hw, hh);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    set_tex(R.ao, "aoTex2D", R.ao_a);
    run(R.ao, g_ao_show == 2 ? "ShowBounce" : g_ao_show ? "Show" : "Apply", dev, R.w, R.h);
    set_tex(R.ao, "depthTex2D", NULL);                    /* before it is a depth buffer again */
    set_tex(R.ao, "aoTex2D", NULL);
    set_tex(R.ao, "nearTex2D", NULL);
    set_tex(R.ao, "fineTex2D", NULL);
    set_tex(R.ao, "colTex2D", NULL);
    restore(dev, &s);
    InterlockedIncrement(&g_ao_runs);
    if (g_tr_on) {
        /* the blurred occlusion, averaged on the CPU (trace frames only) */
        IDirect3DSurface9 *src = NULL, *mem = NULL;
        D3DLOCKED_RECT lr;
        double sum = 0;
        long mean = -1;
        IDirect3DTexture9_GetSurfaceLevel(R.ao_a, 0, &src);
        if (src && SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, hw, hh, D3DFMT_A8R8G8B8,
                                                                      D3DPOOL_SYSTEMMEM, &mem, NULL)) &&
            SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, src, mem)) &&
            SUCCEEDED(IDirect3DSurface9_LockRect(mem, &lr, NULL, D3DLOCK_READONLY))) {
            UINT x, y;
            for (y = 0; y < hh; y += 4)
                for (x = 0; x < hw; x += 4)
                    sum += ((const unsigned char *)lr.pBits)[y * lr.Pitch + x * 4 + 3];   /* occlusion: alpha */
            IDirect3DSurface9_UnlockRect(mem);
            mean = (long)(sum * 1000.0 / (255.0 * ((hh + 3) / 4) * ((hw + 3) / 4)));
        }
        REL(mem); REL(src);
        postfx_trace('A', mean);
    }
}

static int cube_used(const D3DXVECTOR4 *lc, int n)
{
    int i;
    for (i = 0; i < n; i++) if (lc[i].w > 0) return 1;
    return 0;
}

static void set_mat(ID3DXEffect *fx, const char *name, const float *m)
{
    fx->lpVtbl->SetMatrix(fx, fx->lpVtbl->GetParameterByName(fx, NULL, name), (const D3DXMATRIX *)m);
}

/* Volumetric fog (shaders/fog.fx): the sun through its shadow maps and a
 * halo around the point lights near the camera, added to the finished 3D
 * frame. Needs this frame's camera (volfog.c); the sun part also its maps. */
static void volfog(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    UINT hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    float p11, p22, p33, p43, pr[12][4], col[12][4], pls[4], pls2[4], sigma, mist = 0;
    const volfog_state *v;
    LONG fr;
    int n = 0, sun;
    D3DXVECTOR4 lp[12], lc[12];
    saved s;
    static LONG skip_cam, skip_proj;
    if (!R.fog || !R.fog_a) return;
    v = volfog_get(&fr);
    if (v->cam_frame != fr) { skip_cam++; R.fog_hvalid = 0; return; }
    {
        static LONG last_fr;
        if (fr != last_fr + 1) R.fog_hvalid = 0;       /* a frame without fog: the history is stale */
        last_fr = fr;
    }
    if (!projection(dev, &p11, &p22, &p33, &p43)) { skip_proj++; return; }
    /* the sun and its maps seen in the last few frames: a frame that missed
     * them made the shafts blink */
    sun = g_fog_sun > 0 && fr - v->sun_frame <= 8 && fr - v->maps_frame <= 8 && v->fine && v->nearmap;
    /* lights well beyond their reach too: a halo is seen from outside it,
     * and a 2-unit margin switched halos on and off as you walked */
    if (g_fog_glow > 0) n = plshadow_lights_near(v->eye, 40.0f, pr, col, 12);
    /* runs with nothing to scatter too: skipping those frames made the
     * debug view (and the blend) blink off indoors */
    {
        /* outdoors (the sun and its maps this frame) or in, eased over about
         * half a second so a doorway does not jump */
        static float mix = 1.0f;
        static unsigned frame;
        mix += ((sun ? 1.0f : 0.0f) - mix) * 0.05f;
        sigma = (g_fog_density_in + (g_fog_density - g_fog_density_in) * mix) / 1000.0f;
        frame++;
        set_vec(R.fog, "gvFogHaze", g_fog_haze / 1000.0f * (0.33f + 0.67f * mix), FOG_NEAR,
                (float)(frame % 64) * 0.618034f - floorf((float)(frame % 64) * 0.618034f), 0);
        /* the last level fog colour seen (eased); none yet: no haze colour */
        set_vec(R.fog, "gvFogColor", v->fog_col[0], v->fog_col[1], v->fog_col[2], 0);
        /* the engine's own fog end: beyond it, backdrops are left as drawn,
         * in menu scenes only (.w): in a game the far skyline kept its
         * clear, dark look behind hazed streets and read as nearer than
         * they were (Covent Garden, 2026-09-24) */
        set_vec(R.fog, "gvFogEngine", v->fog_min, v->fog_max, v->fog_seen && v->fog_max > v->fog_min ? 1.0f : 0.0f,
                hg_in_game() && !hg_charselect() ? 0.0f : 1.0f);
        /* history: last frame's camera; none after a gap or a reset */
        set_mat(R.fog, "gmFogPrevView", R.fog_prev_view);
        set_vec(R.fog, "gvFogPrevProj", R.fog_prev_p11, R.fog_prev_p22, R.fog_hvalid ? 0.08f : 0.0f, 0);
        /* indoors: lamp shafts, ground mist (none without a player: on the
         * character select it covered the backdrop behind the character) */
        set_mat(R.fog, "gmFogView", v->view);
        mist = R.vpts && hg_in_game() && !hg_charselect() && mix < 0.99f ? g_fog_mist / 1000.0f : 0.0f;
        set_vec(R.fog, "gvFogIndoor", 1.0f - mix, g_fog_lamp / 100.0f, mist, g_fog_mist_h / 100.0f);
    }
    save(dev, &s);
    if (mist > 0) {
        /* the level mask: stencil bit 0x80 of the scene's depth, set by the
         * level's own opaque draws and cleared by every other (gfxprobe
         * lvl_stencil); then cleared for the next frame */
        target(dev, R.level);
        IDirect3DDevice9_SetDepthStencilSurface(dev, device_depth_surface());
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        run(R.fog, "LevelMask", dev, R.w, R.h);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, 0);
    }
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);   /* sampled below */
    set_vec(R.fog, "gvFogMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
    set_vec(R.fog, "gvFogProj", p11, p22, p33, p43);
    set_mat(R.fog, "gmFogInvView", v->inv_view);
    set_vec(R.fog, "gvFogEye", v->eye[0], v->eye[1], v->eye[2], (float)(GetTickCount() % 3600000u) / 1000.0f);
    set_vec(R.fog, "gvFogSky", g_fog_sky / 100.0f, 0, 0, 0);
    set_vec(R.fog, "gvFogParams", sigma, (float)g_fog_dist, g_fog_glow / 100.0f, (float)n);
    if (sun) {
        float k = g_fog_sun / 100.0f;
        set_vec(R.fog, "gvFogSun", v->to_sun[0], v->to_sun[1], v->to_sun[2], 1);
        /* g 0.7: strongly forward, so the beams stand out facing the sun */
        set_vec(R.fog, "gvFogSunCol", v->sun_col[0] * k, v->sun_col[1] * k, v->sun_col[2] * k, 0.7f);
        set_mat(R.fog, "gmFogNear", v->near_m);
        set_mat(R.fog, "gmFogFine", v->fine_m);
        R.fog->lpVtbl->SetTexture(R.fog, R.fog->lpVtbl->GetParameterByName(R.fog, NULL, "nearTex2D"), v->nearmap);
        R.fog->lpVtbl->SetTexture(R.fog, R.fog->lpVtbl->GetParameterByName(R.fog, NULL, "fineTex2D"), v->fine);
    } else {
        set_vec(R.fog, "gvFogSun", 0, 0, 0, 0);
    }
    {
        int i;
        IDirect3DBaseTexture9 *cube = plshadow_texture();
        plshadow_params(pls, pls2);
        for (i = 0; i < 12; i++) {
            D3DXVECTOR4 z = { 0, 0, 0, 0 };
            lp[i] = z; lc[i] = z;
            if (i < n) {
                lp[i].x = pr[i][0]; lp[i].y = pr[i][1]; lp[i].z = pr[i][2]; lp[i].w = pr[i][3];
                lc[i].x = col[i][0]; lc[i].y = col[i][1]; lc[i].z = col[i][2];
                lc[i].w = cube ? col[i][3] : 0;
            }
        }
        R.fog->lpVtbl->SetVectorArray(R.fog, R.fog->lpVtbl->GetParameterByName(R.fog, NULL, "gvFogLights"), lp, 12);
        R.fog->lpVtbl->SetVectorArray(R.fog, R.fog->lpVtbl->GetParameterByName(R.fog, NULL, "gvFogLightCol"), lc, 12);
        set_vec(R.fog, "gvFogPLS", pls2[0], pls2[1], pls2[2], 0);
        R.fog->lpVtbl->SetTexture(R.fog, R.fog->lpVtbl->GetParameterByName(R.fog, NULL, "plsTexCube"), cube);
    }
    set_tex(R.fog, "depthTex2D", device_depth_texture());
    set_tex(R.fog, "backTex2D", g_mask_frame == g_pfx_frame ? R.mask : NULL);
    if (mist > 0) {
        /* The mist volume, in world space around the camera: this frame's
         * floor points drawn into their own target, one draw per slice
         * above the floor, then last frame's volume moved by the whole
         * cells the camera moved and eased towards them (a gap in the fog,
         * a level change: started afresh). shaders/fog.fx VolInject,
         * VolCopy. */
        const UINT aw = VOL_T * VOL_N, ah = (VOL_S + VOL_T - 1) / VOL_T * VOL_N;
        float org[3];
        UINT np = 0, k, nk = (UINT)ceilf(3.0f * (g_fog_mist_h / 100.0f) / VOL_CELL);
        org[0] = floorf(v->eye[0] / VOL_CELL) - VOL_N / 2;
        org[1] = floorf(v->eye[1] / VOL_CELL) - VOL_N / 2;
        org[2] = floorf((v->eye[2] - VOL_BELOW) / VOL_CELL);
        /* The volume is world space and the floor in it static, so a
         * frame without fog does not spoil it: dropping it then (with the
         * fog's history) threw away the mist of every floor out of view,
         * which popped out and faded back in at random (2026-09-25). Only a
         * jump of the camera (a teleport, a new level) or five seconds
         * without fog (loading) start it afresh. */
        {
            float dx = v->eye[0] - R.vol_eye[0], dy = v->eye[1] - R.vol_eye[1], dz = v->eye[2] - R.vol_eye[2];
            if (fr - R.vol_fr > 300 || dx * dx + dy * dy + dz * dz > 30.0f * 30.0f) R.vol_valid = 0;
        }
        set_vec(R.fog, "gvFogVol", org[0] * VOL_CELL, org[1] * VOL_CELL, org[2] * VOL_CELL, VOL_CELL);
        set_vec(R.fog, "gvFogVolDim", (float)VOL_N, (float)VOL_S, (float)VOL_T, 0);
        set_vec(R.fog, "gvFogVolAtlas", 1.0f / aw, 1.0f / ah, (float)aw, (float)ah);
        set_tex(R.fog, "levelTex2D", R.level);
        set_tex(R.fog, "depthTex2D", device_depth_texture());
        target(dev, R.vinj);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        R.fog->lpVtbl->SetTechnique(R.fog, R.fog->lpVtbl->GetTechniqueByName(R.fog, "VolInject"));
        for (k = 0; k <= nk; k++) {
            set_vec(R.fog, "gvFogVolStep", (float)k, 0, 0, 0);
            if (FAILED(R.fog->lpVtbl->Begin(R.fog, &np, D3DXFX_DONOTSAVESTATE))) break;
            R.fog->lpVtbl->BeginPass(R.fog, 0);
            IDirect3DDevice9_SetStreamSource(dev, 0, R.vpts, 0, 12);
            IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ);
            IDirect3DDevice9_DrawPrimitive(dev, D3DPT_POINTLIST, 0, R.npts);
            IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0);
            R.fog->lpVtbl->EndPass(R.fog);
            R.fog->lpVtbl->End(R.fog);
        }
        /* new floor fades in over about a third of a second (a tenth a
         * frame); floor out of view halves in about two seconds */
        set_tex(R.fog, "volTex2D", R.vol[R.vol_cur]);
        set_tex(R.fog, "injTex2D", R.vinj);
        set_vec(R.fog, "gvFogVolStep", org[0] - R.vol_org[0], org[1] - R.vol_org[1], org[2] - R.vol_org[2],
                R.vol_valid ? 0.995f : 0.0f);
        set_vec(R.fog, "gvFogVolRate", R.vol_valid ? 0.1f : 1.0f, 0, 0, 0);
        target(dev, R.vol[R.vol_cur ^ 1]);
        run(R.fog, "VolCopy", dev, aw, ah);
        R.vol_cur ^= 1;
        set_tex(R.fog, "injTex2D", NULL);
        memcpy(R.vol_org, org, sizeof org);
        memcpy(R.vol_eye, v->eye, sizeof R.vol_eye);
        R.vol_fr = fr;
        R.vol_valid = 1;
        set_tex(R.fog, "volTex2D", R.vol[R.vol_cur]);
    }
    target(dev, R.fog_a);
    run(R.fog, "Scatter", dev, hw, hh);
    set_tex(R.fog, "volTex2D", NULL);
    set_tex(R.fog, "levelTex2D", NULL);
    {
        IDirect3DTexture9 *prev = R.fog_h[R.fog_hcur], *cur = R.fog_h[R.fog_hcur ^ 1];
        target(dev, cur);
        set_tex(R.fog, "fogTex2D", R.fog_a);
        set_tex(R.fog, "histTex2D", prev);
        set_vec(R.fog, "gvFogPass", 1.0f / hw, 1.0f / hh, 0, 0);
        run(R.fog, "Temporal", dev, hw, hh);
        set_tex(R.fog, "histTex2D", NULL);
        R.fog_hcur ^= 1;
        memcpy(R.fog_prev_view, v->view, sizeof R.fog_prev_view);
        R.fog_prev_p11 = p11; R.fog_prev_p22 = p22;
        R.fog_hvalid = 1;
    }
    target(dev, R.fog_b);
    set_tex(R.fog, "fogTex2D", R.fog_h[R.fog_hcur]);
    set_vec(R.fog, "gvFogPass", 1.0f / hw, 1.0f / hh, 1.0f / hw, 0);
    run(R.fog, "Blur", dev, hw, hh);
    target(dev, R.fog_a);
    set_tex(R.fog, "fogTex2D", R.fog_b);
    set_vec(R.fog, "gvFogPass", 1.0f / hw, 1.0f / hh, 0, 1.0f / hh);
    run(R.fog, "Blur", dev, hw, hh);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    set_tex(R.fog, "fogTex2D", R.fog_a);
    set_vec(R.fog, "gvFogPass", 1.0f / hw, 1.0f / hh, 0, 0);     /* the upsample's source texel */
    run(R.fog, g_fog_show ? "Show" : "Apply", dev, R.w, R.h);
    /* nothing of the engine's stays referenced past this frame */
    set_tex(R.fog, "depthTex2D", NULL);
    set_tex(R.fog, "backTex2D", NULL);
    set_tex(R.fog, "fogTex2D", NULL);
    set_tex(R.fog, "nearTex2D", NULL);
    set_tex(R.fog, "fineTex2D", NULL);
    R.fog->lpVtbl->SetTexture(R.fog, R.fog->lpVtbl->GetParameterByName(R.fog, NULL, "plsTexCube"), NULL);
    restore(dev, &s);
    InterlockedIncrement(&g_fog_runs);
    {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 10000) {
            last = now;
            hg_log("postfx: fog: sun %s, %d lights (shadowing: %s), density %.3f, %ld runs; skipped: %ld no camera, %ld no projection",
                   sun ? "marched" : "none", n, cube_used(lc, n) ? "yes" : "no", sigma, g_fog_runs, skip_cam, skip_proj);
        }
    }
}

/* Light spill (shaders/ao.fx, Spill*): the engine's nearby lights (lamps,
 * fires, portals, spells: src/plshadow.c's list, faded in and out there)
 * light the surfaces around them wider and softer than the engine does, in
 * world space. After the transparent half, before the fog. Indoors only
 * for now; eased over half a second at a doorway, as the fog's indoor mix. */
int fpview_flash(float pos[3], float rgb[3], float *radius);
static void spill(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    UINT hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    float p11, p22, p33, p43, k, kl, pr[12][4], col[12][4], pls[4], pls2[4], fpos[3], frgb[3], frad;
    D3DXVECTOR4 lp[12], lc[12];
    const volfog_state *v;
    LONG fr;
    int i, n, flash;
    saved s;
    /* why a frame went without, logged every 10 s */
    static long calls, no_tgt, no_hdr, no_cam, outside, no_lights, no_proj, no_copy;
    {
        static DWORD last;
        DWORD now = GetTickCount();
        calls++;
        if (now - last >= 10000) {
            last = now;
            hg_log("postfx: spill: %ld calls, %ld drawn; skipped: %ld no targets, %ld not in the HDR scene, %ld no camera, "
                   "%ld outdoors, %ld no lights, %ld no projection, %ld copy failed; show %ld",
                   calls, g_spill_runs, no_tgt, no_hdr, no_cam, outside, no_lights, no_proj, no_copy, g_spill_show);
        }
    }
    if (!R.sp_a) { no_tgt++; return; }
    if (!hdr_in_scene()) { no_hdr++; return; }
    v = volfog_get(&fr);
    if (v->cam_frame != fr) { no_cam++; return; }   /* no camera this frame: the lights cannot be placed */
    k = (g_spill > 0 || g_spill_show ? g_spill / 100.0f : 0.0f) * (1.0f - g_outdoors);
    /* the muzzle flash (src/fpview.c) rides along as one more light, outdoors
     * too: the pass runs at full strength for it, the lamps scaled by k */
    flash = fpview_flash(fpos, frgb, &frad);
    if (k < 0.01f && !flash) { outside++; return; }

    n = k < 0.01f ? 0 : plshadow_lights_near(v->eye, 40.0f, pr, col, 12);
    if (flash) {
        if (n == 12) n = 11;
        pr[n][0] = fpos[0]; pr[n][1] = fpos[1]; pr[n][2] = fpos[2]; pr[n][3] = frad;
        col[n][0] = frgb[0]; col[n][1] = frgb[1]; col[n][2] = frgb[2]; col[n][3] = 0;
        n++;
    }
    if (!n) { no_lights++; return; }
    kl = 1.0f;
    if (flash) {                            /* the pass at the flash's strength, the lamps at their own */
        float kf = g_spill > 0 ? g_spill / 100.0f : 1.0f;
        kl = k / kf;
        k = kf;
    }
    if (!projection(dev, &p11, &p22, &p33, &p43)) { no_proj++; return; }
    {
        /* the scene so far, half size: the receivers' brightness */
        IDirect3DSurface9 *cs = NULL;
        int ok;
        IDirect3DTexture9_GetSurfaceLevel(R.ao_col, 0, &cs);
        ok = cs && SUCCEEDED(IDirect3DDevice9_StretchRect(dev, bb, NULL, cs, NULL, D3DTEXF_LINEAR));
        REL(cs);
        if (!ok) { no_copy++; return; }
    }
    {
        /* the lights into view space (row vectors, as the effects' mul) */
        IDirect3DBaseTexture9 *cube = plshadow_texture();
        const float *m = v->view;
        plshadow_params(pls, pls2);
        for (i = 0; i < 12; i++) {
            D3DXVECTOR4 z = { 0, 0, 0, 0 };
            lp[i] = z; lc[i] = z;
            if (i >= n) continue;
            lp[i].x = pr[i][0] * m[0] + pr[i][1] * m[4] + pr[i][2] * m[8] + m[12];
            lp[i].y = pr[i][0] * m[1] + pr[i][1] * m[5] + pr[i][2] * m[9] + m[13];
            lp[i].z = pr[i][0] * m[2] + pr[i][1] * m[6] + pr[i][2] * m[10] + m[14];
            lp[i].w = pr[i][3];
            {
                float kk = flash && i == n - 1 ? 1.0f : kl;
                lc[i].x = col[i][0] * kk; lc[i].y = col[i][1] * kk; lc[i].z = col[i][2] * kk;
            }
            lc[i].w = cube ? col[i][3] : 0;
        }
        R.ao->lpVtbl->SetVectorArray(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "gvSpillLights"), lp, 12);
        R.ao->lpVtbl->SetVectorArray(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "gvSpillCol"), lc, 12);
        set_mat(R.ao, "gmSpillInvView", v->inv_view);
        set_vec(R.ao, "gvSpillPLS", pls2[0], pls2[1], pls2[2], 0);
        R.ao->lpVtbl->SetTexture(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "plsTexCube"), cube);
    }
    save(dev, &s);
    ao_fog();
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);   /* sampled below */
    set_vec(R.ao, "gvAoMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
    set_vec(R.ao, "gvAoProj", p11, p22, p33, p43);
    set_vec(R.ao, "gvAoParams", g_ao_radius / 100.0f, g_ao_strength / 100.0f * 1.5f, 60.0f, 150.0f);
    /* 100%: next to a lamp (its colour ~0.8, half its falloff) the frame
     * gains about 80% (0.6 over the local brightness was far too bright;
     * then 40% was subtle even at 400%, 2026-09-24) */
    /* the tail peaks near 0.2 about the engine's radius: x3 there lifts a
     * surface by some 50% of the lamp's colour */
    set_vec(R.ao, "gvSpill", k * 3.0f, g_spill_reach / 100.0f, (float)n, 0);
    set_tex(R.ao, "depthTex2D", device_depth_texture());
    set_tex(R.ao, "colTex2D", R.ao_col);
    /* gathered at half resolution, blurred, applied */
    target(dev, R.sp_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 0);
    run(R.ao, "SpillGather", dev, hw, hh);
    target(dev, R.sp_b);
    set_tex(R.ao, "aoTex2D", R.sp_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 1.0f / hw, 0);
    run(R.ao, "Blur", dev, hw, hh);
    target(dev, R.sp_a);
    set_tex(R.ao, "aoTex2D", R.sp_b);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 1.0f / hh);
    run(R.ao, "Blur", dev, hw, hh);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    set_tex(R.ao, "aoTex2D", R.sp_a);
    set_vec(R.ao, "gvAoPass", 1.0f / hw, 1.0f / hh, 0, 0);
    run(R.ao, g_spill_show == 2 ? "SpillShowLight" : g_spill_show ? "SpillShow" : "SpillApply", dev, R.w, R.h);
    set_tex(R.ao, "depthTex2D", NULL);
    set_tex(R.ao, "colTex2D", NULL);
    set_tex(R.ao, "aoTex2D", NULL);
    R.ao->lpVtbl->SetTexture(R.ao, R.ao->lpVtbl->GetParameterByName(R.ao, NULL, "plsTexCube"), NULL);
    restore(dev, &s);
    InterlockedIncrement(&g_spill_runs);
    {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 10000) {
            last = now;
            hg_log("postfx: spill: outdoors %.2f, strength %.2f, %d lights (nearest radius %.1f, colour %.2f %.2f %.2f)",
                   g_outdoors, k, n, pr[0][3], col[0][0], col[0][1], col[0][2]);
        }
    }
}

/* Auto exposure (HDR): the float scene's centre-weighted log-average
 * luminance, eased into the eye's (R.adapt): it takes about half a second
 * to follow into light and a second and a half into the dark, as eyes do.
 * Returns the texture the composite reads, NULL if it is off. */
static IDirect3DTexture9 *auto_exposure(IDirect3DDevice9 *dev, IDirect3DTexture9 *scene, const float au[4])
{
    ID3DXEffect *fx = R.bloom;
    LARGE_INTEGER now, f;
    float dt;
    int i;
    IDirect3DTexture9 *prev, *cur;
    if (au[0] <= 0 || !R.adapt[0]) { R.adapt_valid = 0; return NULL; }
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    dt = R.adapt_valid ? (float)(now.QuadPart - R.adapt_t.QuadPart) / (float)f.QuadPart : 0.0f;
    if (dt > 0.25f) dt = 0.25f;         /* a loading screen: carry on from where it was */
    R.adapt_t = now;
    set_tex(fx, "sceneTex2D", scene);
    set_vec(fx, "gvBloomSrc", 1.0f / 256, 1.0f / 256, 0, 0);
    target(dev, R.lum[0]);
    run(fx, "LumLog", dev, 256, 256);
    for (i = 1; i < LUM_LEVELS; i++) {
        UINT sz = 256u >> (2 * (i - 1));
        set_tex(fx, "lumTex2D", R.lum[i - 1]);
        set_vec(fx, "gvBloomSrc", 1.0f / sz, 1.0f / sz, 0, 0);
        target(dev, R.lum[i]);
        run(fx, "LumDown", dev, sz / 4, sz / 4);
    }
    prev = R.adapt[R.adapt_cur];
    cur = R.adapt[R.adapt_cur ^ 1];
    set_tex(fx, "lumTex2D", R.lum[LUM_LEVELS - 1]);
    set_tex(fx, "adaptTex2D", prev);
    set_vec(fx, "gvHdrAdapt", 1.0f - expf(-dt / 0.5f), 1.0f - expf(-dt / 1.5f), R.adapt_valid ? 0.0f : 1.0f, 0);
    target(dev, cur);
    run(fx, "Adapt", dev, 1, 1);
    set_tex(fx, "lumTex2D", NULL);
    R.adapt_cur ^= 1;
    R.adapt_valid = 1;
    {
        /* for the panel and the log: read back every few seconds (a stall) */
        static DWORD last;
        DWORD t = GetTickCount();
        if (t - last >= 3000) {
            IDirect3DSurface9 *src = NULL, *mem = NULL;
            D3DLOCKED_RECT lr;
            last = t;
            IDirect3DTexture9_GetSurfaceLevel(cur, 0, &src);
            if (src && SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 1, 1, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &mem, NULL)) &&
                SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, src, mem)) &&
                SUCCEEDED(IDirect3DSurface9_LockRect(mem, &lr, NULL, D3DLOCK_READONLY))) {
                float a = *(const float *)lr.pBits, e;
                IDirect3DSurface9_UnlockRect(mem);
                g_eye = expf(a);
                e = au[0] * (au[1] - a);
                e = e < -au[2] ? -au[2] : e > au[2] ? au[2] : e;
                {
                    static DWORD logged;
                    if (t - logged >= 10000) {
                        logged = t;
                        hg_log("hdr: eye at %.3f (log-average luminance), auto exposure x%.2f", g_eye, expf(e));
                    }
                }
            }
            REL(mem); REL(src);
        }
    }
    return cur;
}
float hg_gfx_hdr_eye(void) { return g_eye; }

/* Bloom and the colour grade (shaders/bloom.fx): the frame is copied, its
 * bright part blurred down a chain of halving targets and back up, and the
 * composite writes scene + bloom, graded, over the back buffer. With HDR
 * (src/hdr.c) this is the resolve: the float scene is read as it is, and
 * the composite is the first thing on the real back buffer. */
static void bloom_grade(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    ID3DXEffect *fx = R.bloom;
    IDirect3DSurface9 *cs = NULL;
    IDirect3DTexture9 *scene = hdr_in_scene() ? hdr_texture() : NULL;
    LONG fr;
    const volfog_state *v = volfog_get(&fr);
    float tint[3] = { 0.5f, 0.5f, 0.5f };
    int i;
    saved s;
    if (!fx || !R.bl[0]) return;
    save(dev, &s);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    if (!scene) {
        IDirect3DTexture9_GetSurfaceLevel(R.color, 0, &cs);
        IDirect3DDevice9_StretchRect(dev, bb, NULL, cs, NULL, D3DTEXF_NONE);
        REL(cs);
        scene = R.color;
    }
    {
        /* HDR: bloom from real brightness, a threshold in scene units (1 =
         * white, before exposure) with a knee half as wide; so only what is
         * brighter than white glows, the more the hotter. Stock: the
         * threshold is on the display's 0..1. */
        float t[4] = { 0, 1, 1, 0 };
        if (scene != R.color) hdr_tonemap(t);
        if (t[0] > 0)
            set_vec(fx, "gvBloomParams", t[3] / t[1], 0.5f * t[3] / t[1], g_bloom / 100.0f * 0.15f, g_bloom_on ? 1.0f : 0.0f);
        else
            set_vec(fx, "gvBloomParams", g_bloom_thr / 100.0f, 0.2f, g_bloom / 100.0f * 0.15f, g_bloom_on ? 1.0f : 0.0f);
    }
    if (g_bloom_on) {
        set_tex(fx, "srcTex2D", scene);
        set_vec(fx, "gvBloomSrc", 1.0f / R.w, 1.0f / R.h, 0, 0);
        target(dev, R.bl[0]);
        run(fx, "Prefilter", dev, R.blw[0], R.blh[0]);
        for (i = 1; i < BLOOM_LEVELS; i++) {
            set_tex(fx, "srcTex2D", R.bl[i - 1]);
            set_vec(fx, "gvBloomSrc", 1.0f / R.blw[i - 1], 1.0f / R.blh[i - 1], 0, 0);
            target(dev, R.bl[i]);
            run(fx, "Down", dev, R.blw[i], R.blh[i]);
        }
        for (i = BLOOM_LEVELS - 1; i > 0; i--) {
            set_tex(fx, "srcTex2D", R.bl[i]);
            set_vec(fx, "gvBloomSrc", 1.0f / R.blw[i], 1.0f / R.blh[i], 0, 0);
            target(dev, R.bl[i - 1]);
            run(fx, "Up", dev, R.blw[i - 1], R.blh[i - 1]);
        }
        set_tex(fx, "srcTex2D", NULL);
    }
    /* the shadow tint: the fog's hue at half luma (neutral grey without one) */
    if (v->fog_seen) {
        float l = 0.299f * v->fog_col[0] + 0.587f * v->fog_col[1] + 0.114f * v->fog_col[2];
        if (l > 0.01f)
            for (i = 0; i < 3; i++) {
                tint[i] = v->fog_col[i] * 0.5f / l;
                if (tint[i] > 1.0f) tint[i] = 1.0f;
            }
    }
    set_vec(fx, "gvGrade", g_grade_sat / 100.0f, g_grade_con / 100.0f, g_grade_tint / 100.0f, g_grade_vig / 100.0f);
    set_vec(fx, "gvGradeTint", tint[0], tint[1], tint[2], g_grade_on ? 1.0f : 0.0f);
    {
        float t[4] = { 0, 1, 1, 0 }, au[4] = { 0, 0, 0, 0 };
        IDirect3DTexture9 *eye = NULL;
        if (scene != R.color) { hdr_tonemap(t); hdr_auto(au); }    /* the float scene: tone-mapped here */
        if (t[0] > 0) eye = auto_exposure(dev, scene, au);
        set_vec(fx, "gvHdr", t[0], t[1], t[2], t[3]);
        set_vec(fx, "gvHdrAuto", eye ? au[0] : 0.0f, au[1], au[2], au[3]);
        set_tex(fx, "adaptTex2D", eye);
    }
    set_tex(fx, "sceneTex2D", scene);
    set_tex(fx, "bloomTex2D", R.bl[0]);
    hdr_end_scene(dev);                 /* HDR: bb is the real back buffer from here */
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    run(fx, "Composite", dev, R.w, R.h);
    set_tex(fx, "sceneTex2D", NULL);
    set_tex(fx, "bloomTex2D", NULL);
    set_tex(fx, "adaptTex2D", NULL);
    restore(dev, &s);
}

/* The finished 3D frame: the fog, bloom and the grade, then SMAA and CAS. */
static void post_scene(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, int scene)
{
    if (scene) dnc_show(dev, bb);                   /* debug: depth without colour */
    if (scene) spill(dev, bb);          /* lamps (spill.strength), the muzzle flash, or nothing */
    if (g_fog_on && scene) volfog(dev, bb);
    if ((g_bloom_on || g_grade_on || hdr_in_scene()) && scene) bloom_grade(dev, bb);
    hdr_finish(dev);                    /* HDR the composite did not resolve: copied over as it is */
    if (g_smaa_pass || g_cas) smaa(dev, bb);
}

/* ------------------------------------------------------------------ */
/* entry points                                                        */

/* From gfxprobe: the opaque scene is done (first skybox or particle pass,
 * or the first blended material draw) -- on the back buffer, once a frame. */
void postfx_before_transparent(char why)
{
    IDirect3DDevice9 *dev = device_get();
    IDirect3DSurface9 *bb;
    postfx_trace(why, gfxprobe_opaque_draws());
    if (!gfxprobe_opaque_draws()) return;           /* no world in the depth buffer yet */
    g_scene_seen = 1;
    if (g_ao_done || hg_gfx_stock_viewing() || !dev || !device_depth_texture()) return;
    if (!g_ao_on && g_depth_done) return;
    if (!(bb = bound_back_buffer(dev))) return;           /* e.g. the shadow pass */
    if (res_ensure(dev, bb)) {
        if (g_nr16) rigid16_draw(dev);                  /* before the AO reads the frame */
        if (g_nso) so_draw(dev);                        /* debug: shadow-only meshes */
        if (!g_depth_done) lin_depth(dev);             /* soft particles, the focus */
        if (g_ao_on) { g_ao_done = 1; ao(dev, bb); }
        if (g_contact > 0 || g_contact_show) contact(dev, bb);
    }
    REL(bb);
}

int postfx_wants_transparent_check(void)
{
    return !g_scene_seen || (g_ao_on && !g_ao_done) || !g_depth_done;
}

/* For gfxprobe, at each particle pass: this frame's linear depth and the
 * fade factor (1 / distance), or NULL and 0 when there is none. */
IDirect3DTexture9 *postfx_soft_depth(float *inv_dist)
{
    int ok = g_soft > 0 && g_depth_done && R.lindepth && !hg_gfx_stock_viewing();
    *inv_dist = ok ? 100.0f / g_soft : 0.0f;
    return ok ? R.lindepth : NULL;
}

/* From marker_stub: the older scene path's opaque/transparent marker. */
void __cdecl postfx_marker(void) { postfx_before_transparent('M'); }

/* From gfxprobe's BeginPass hook: ui.fxo is about to draw. */
void postfx_before_ui(void)
{
    IDirect3DDevice9 *dev = device_get();
    IDirect3DSurface9 *bb;
    /* not before the 3D scene (UI drawn early, e.g. name plates): frames
     * without one get their SMAA at Present */
    if (!g_scene_seen || !(g_smaa_pass || g_cas || g_fog_on || g_bloom_on || g_grade_on || hdr_in_scene()) || g_smaa_done || hg_gfx_stock_viewing() || !dev || !device_depth_texture()) return;
    if (!(bb = bound_back_buffer(dev))) return;
    g_smaa_done = 1;
    if (res_ensure(dev, bb)) post_scene(dev, bb, 1);
    REL(bb);
}

/* From src/device.c at Present, once a frame: 1 if the frame never reached
 * the UI and still needs its SMAA (postfx_present_draw, inside a scene the
 * caller opens). Resets the per-frame flags either way. */
static int g_smaa_pending, g_pending_scene;
int postfx_present(IDirect3DDevice9 *dev)
{
    rigid16_clear();                        /* a frame whose opaque end never came */
    so_clear();
    g_pfx_frame++;
    int need = (g_smaa_pass || g_cas || ((g_fog_on || g_bloom_on || g_grade_on || hdr_in_scene()) && g_scene_seen)) && !g_smaa_done && !hg_gfx_stock_viewing() && device_depth_texture() != NULL;
    static DWORD last;
    DWORD now = GetTickCount();
    (void)dev;
    if (g_tr_on) {
        hg_log("postfx: frame trace (C clear after n opaque draws; P/B/Z/M trigger; A AO ran, mean x1000): %s", g_tr_n ? g_tr : "(nothing)");
        g_tr_on = 0;
    }
    if (now - last > 10000) { last = now; g_tr_on = 1; g_tr_n = 0; g_tr[0] = 0; }
    g_pending_scene = g_scene_seen;
    g_ao_done = g_smaa_done = g_scene_seen = g_depth_done = 0;
    {
        /* outdoors (the sun and its maps seen lately) or in, eased over about
         * half a second so a doorway does not jump: the light spill and the
         * contact shadows are indoor effects */
        LONG fr;
        const volfog_state *v = volfog_get(&fr);
        int sun = fr - v->sun_frame <= 8 && fr - v->maps_frame <= 8 && v->fine && v->nearmap;
        g_outdoors += ((sun ? 1.0f : 0.0f) - g_outdoors) * 0.05f;
    }
    g_smaa_pending = need;
    return need;
}

void postfx_present_draw(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL;
    if (!g_smaa_pending) return;
    g_smaa_pending = 0;
    /* the back buffer need not be bound at Present */
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (res_ensure(dev, bb)) post_scene(dev, bb, g_pending_scene);
    REL(bb);
    g_smaa_done = 0;
}

void postfx_marker_stub(void);
__asm__(
    ".text\n\t"
    ".globl _postfx_marker_stub\n"
    "_postfx_marker_stub:\n\t"
    "pushal\n\t"
    "pushfl\n\t"
    "movl %esp, %ebp\n\t"              /* ebp is saved by pushal */
    "andl $-16, %esp\n\t"
    "call _postfx_marker\n\t"
    "movl %ebp, %esp\n\t"
    "popfl\n\t"
    "popal\n\t"
    "jmp *_g_marker_orig\n\t"
);

/* what a player can change, saved in bin\ultrapatch.ini (src/settings.c) */
static void postfx_settings(void)
{
    settings_var("ao.on", &g_ao_on, 0, 1);
    settings_var("ao.radius", &g_ao_radius, 10, 800);
    settings_var("ao.strength", &g_ao_strength, 10, 300);
    settings_var("ao.less_in_sun", &g_ao_sun, 0, 100);
    settings_var("ao.colour_bounce", &g_ao_bleed, 0, 300);
    settings_var("smaa.pass", &g_smaa_pass, 0, 1);
    settings_var("sharpen", &g_cas, 0, 100);
    settings_var("particles.soft", &g_soft, 0, 500);
    settings_var("fog.on", &g_fog_on, 0, 1);
    settings_var("fog.density", &g_fog_density, 0, 500);
    settings_var("fog.density_indoors", &g_fog_density_in, 0, 500);
    settings_var("fog.sun_shafts", &g_fog_sun, 0, 300);
    settings_var("fog.sky", &g_fog_sky, 0, 100);
    settings_var("fog.light_glow", &g_fog_glow, 0, 300);
    settings_var("fog.shaft_reach", &g_fog_dist, 10, 200);
    settings_var("fog.haze", &g_fog_haze, 0, 200);
    settings_var("fog.lamp_shafts", &g_fog_lamp, 0, 100);
    settings_var("fog.ground_mist", &g_fog_mist, 0, 500);
    settings_var("fog.ground_mist_height", &g_fog_mist_h, 20, 150);
    settings_var("spill.strength", &g_spill, 0, 400);
    settings_var("spill.reach", &g_spill_reach, 50, 300);
    settings_var("fixes.rigid16", &g_rigid16, 0, 1);
    settings_var("contact.strength", &g_contact, 0, 100);
    settings_var("contact.length", &g_contact_len, 10, 150);
    settings_var("bloom.on", &g_bloom_on, 0, 1);
    settings_var("bloom.intensity", &g_bloom, 0, 300);
    settings_var("bloom.threshold", &g_bloom_thr, 0, 100);
    settings_var("grade.on", &g_grade_on, 0, 1);
    settings_var("grade.saturation", &g_grade_sat, 0, 300);
    settings_var("grade.contrast", &g_grade_con, 0, 100);
    settings_var("grade.shadow_tint", &g_grade_tint, 0, 100);
    settings_var("grade.vignette", &g_grade_vig, 0, 100);
}

void postfx_install(unsigned int image)
{
    void **slot = (void **)(image + RVA_DRAWLIST_JUMP_18);
    DWORD old;
    g_image = image;
    postfx_settings();
    if (IsBadReadPtr(slot, 4) || *slot != (void *)(image + RVA_DRAWLIST_NOOP)) {
        hg_log("postfx: draw-list marker NOT hooked -- unexpected jump-table entry %p",
               IsBadReadPtr(slot, 4) ? NULL : *slot);
        return;
    }
    g_marker_orig = *slot;
    if (!VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    *slot = (void *)postfx_marker_stub;
    VirtualProtect(slot, 4, old, &old);
    hg_log("postfx: hooked the scene's opaque/transparent marker (draw-list command 0x18)");
}

/* ------------------------------------------------------------------ */
/* panel                                                               */

void hg_gfx_set_ao(int on) { InterlockedExchange(&g_ao_on, on ? 1 : 0); hg_log("postfx: AO %s", on ? "ON" : "off"); }
int  hg_gfx_ao(void) { return (int)g_ao_on; }
void hg_gfx_set_ao_show(int mode) { InterlockedExchange(&g_ao_show, mode == 2 ? 2 : mode ? 1 : 0); }
int  hg_gfx_ao_show(void) { return (int)g_ao_show; }

/* which: 0 radius (x100 units), 1 strength (%) */
void hg_gfx_nudge_ao(int which, int d)
{
    volatile LONG *p = which == 3 ? &g_ao_bleed : which == 2 ? &g_ao_sun : which ? &g_ao_strength : &g_ao_radius;
    LONG lo = which >= 2 ? 0 : 10, hi = which == 3 ? 300 : which == 2 ? 100 : which ? 300 : 800, v = *p + d;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    InterlockedExchange(p, v);
    hg_log("postfx: AO radius %.2f strength %ld%% less in sun %ld%% bounce %ld%%", g_ao_radius / 100.0f,
           g_ao_strength, g_ao_sun, g_ao_bleed);
}
int hg_gfx_ao_bleed(void) { return (int)g_ao_bleed; }
int hg_gfx_ao_sun(void) { return (int)g_ao_sun; }
int hg_gfx_ao_radius(void) { return (int)g_ao_radius; }
int hg_gfx_ao_strength(void) { return (int)g_ao_strength; }

void hg_gfx_set_smaa_pass(int on) { InterlockedExchange(&g_smaa_pass, on ? 1 : 0); hg_log("postfx: SMAA pass %s", on ? "ON" : "off"); }
int  hg_gfx_smaa_pass(void) { return (int)g_smaa_pass; }
void hg_gfx_nudge_cas(int d)
{
    LONG v = g_cas + d;
    InterlockedExchange(&g_cas, v < 0 ? 0 : v > 100 ? 100 : v);
    hg_log("postfx: CAS sharpening %ld%%", g_cas);
}
int  hg_gfx_cas(void) { return (int)g_cas; }
void hg_gfx_nudge_soft(int d)
{
    LONG v = g_soft + d;
    InterlockedExchange(&g_soft, v < 0 ? 0 : v > 400 ? 400 : v);
    hg_log("postfx: soft particles %s (fade over %.2f units)", g_soft ? "ON" : "off", g_soft / 100.0f);
}
int  hg_gfx_soft(void) { return (int)g_soft; }
long hg_gfx_postfx_runs(int which) { return which == 4 ? g_contact_runs : which == 3 ? g_spill_runs : which == 2 ? g_fog_runs : which ? g_smaa_runs : g_ao_runs; }
void hg_gfx_set_contact_show(int on) { InterlockedExchange(&g_contact_show, on ? 1 : 0); }
int  hg_gfx_contact_show(void) { return (int)g_contact_show; }
void hg_gfx_set_spill_show(int mode) { InterlockedExchange(&g_spill_show, mode == 2 ? 2 : mode ? 1 : 0); }
int  hg_gfx_spill_show(void) { return (int)g_spill_show; }

void hg_gfx_set_fog(int on) { InterlockedExchange(&g_fog_on, on ? 1 : 0); hg_log("postfx: volumetric fog %s", on ? "ON" : "off"); }
int  hg_gfx_fog(void) { return (int)g_fog_on; }
void hg_gfx_set_fog_show(int on) { InterlockedExchange(&g_fog_show, on ? 1 : 0); }
int  hg_gfx_fog_show(void) { return (int)g_fog_show; }
/* which: 0 density on the surface (per unit x1000), 1 sun shafts (%), 2 light glow (%), 3 distance (units),
 * 4 sky (%), 5 density indoors, 6 distance haze (per unit x1000) */
static volatile LONG *fog_knob(int which)
{
    return which == 0 ? &g_fog_density : which == 1 ? &g_fog_sun : which == 2 ? &g_fog_glow :
           which == 3 ? &g_fog_dist : which == 4 ? &g_fog_sky : which == 5 ? &g_fog_density_in :
           which == 7 ? &g_fog_lamp : &g_fog_haze;
}
void hg_gfx_nudge_fog(int which, int d)
{
    static const LONG hi[8] = { 200, 400, 400, 200, 100, 200, 100, 100 };
    volatile LONG *p = fog_knob(which);
    LONG v;
    if (which < 0 || which > 7) return;
    v = *p + d;
    InterlockedExchange(p, v < 0 ? 0 : v > hi[which] ? hi[which] : v);
    hg_log("postfx: fog haze %.3f/unit, density %.3f/unit (indoors %.3f), sun shafts %ld%% (sky %ld%%), light glow %ld%%, sun marched %ld units",
           g_fog_haze / 1000.0f, g_fog_density / 1000.0f, g_fog_density_in / 1000.0f, g_fog_sun, g_fog_sky, g_fog_glow, g_fog_dist);
}
int hg_gfx_fog_val(int which)
{
    return which < 0 || which > 7 ? 0 : (int)*fog_knob(which);
}

/* Bloom and the grade. which: 0 bloom intensity, 1 threshold, 2 saturation, 3 contrast, 4 shadow tint,
 * 5 vignette (all percent) */
void hg_gfx_set_bloom(int on) { InterlockedExchange(&g_bloom_on, on ? 1 : 0); hg_log("postfx: bloom %s", on ? "ON" : "off"); }
int  hg_gfx_bloom(void) { return (int)g_bloom_on; }
void hg_gfx_set_grade(int on) { InterlockedExchange(&g_grade_on, on ? 1 : 0); hg_log("postfx: colour grade %s", on ? "ON" : "off"); }
int  hg_gfx_grade(void) { return (int)g_grade_on; }
static volatile LONG *look_knob(int which)
{
    static volatile LONG *const k[6] = { &g_bloom, &g_bloom_thr, &g_grade_sat, &g_grade_con, &g_grade_tint, &g_grade_vig };
    return which >= 0 && which < 6 ? k[which] : NULL;
}
void hg_gfx_nudge_post(int which, int d)
{
    static const LONG hi[6] = { 200, 100, 200, 100, 100, 100 };
    volatile LONG *p = look_knob(which);
    LONG v;
    if (!p) return;
    v = *p + d;
    InterlockedExchange(p, v < 0 ? 0 : v > hi[which] ? hi[which] : v);
    hg_log("postfx: bloom %ld%% over %ld%%; grade saturation %ld%%, contrast %ld%%, shadow tint %ld%%, vignette %ld%%",
           g_bloom, g_bloom_thr, g_grade_sat, g_grade_con, g_grade_tint, g_grade_vig);
}
int hg_gfx_post_val(int which) { volatile LONG *p = look_knob(which); return p ? (int)*p : 0; }
