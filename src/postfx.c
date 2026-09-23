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
static volatile LONG g_fog_mist = 50;        /* indoors: ground mist at the floor, per unit x1000 */
static volatile LONG g_fog_mist_h = 60;      /* ... its height, units x100 */
#define FOG_NEAR 8.0f                        /* no fog in the first units from the camera */
static volatile LONG g_bloom_on = 1;         /* bloom */
static volatile LONG g_bloom = 70;           /* intensity, percent */
static volatile LONG g_bloom_thr = 50;       /* threshold, percent of full luma */
static volatile LONG g_grade_on = 1;         /* colour grade */
static volatile LONG g_grade_sat = 120;      /* saturation, percent */
static volatile LONG g_grade_con = 20;       /* contrast around the game's middle (0.15), percent */
static volatile LONG g_grade_tint = 50;      /* shadows towards the fog's colour, percent */
static volatile LONG g_grade_vig = 25;       /* vignette, percent */
#define BLOOM_LEVELS 6

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
    IDirect3DTexture9 *floor_t[2];      /* 1 x 1 R32F: the floor height under the camera, ping-pong */
    int floor_cur, floor_valid;
    int fog_hcur, fog_hvalid;
    float fog_prev_view[16], fog_prev_p11, fog_prev_p22;
    IDirect3DTexture9 *lindepth;        /* R32F, half resolution: soft particles (NULL: none) */
} R;

static LONG g_ao_done, g_smaa_done;         /* this frame */
static LONG g_scene_seen;                   /* this frame reached the end of the opaque scene */
static LONG g_depth_done;                   /* this frame's linear depth is in R.lindepth */
static LONG g_ao_runs, g_smaa_runs, g_fog_runs;

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

static void res_release(void)
{
    REL(R.sb); REL(R.smaa); REL(R.ao); REL(R.cas); REL(R.fog); REL(R.fog_a); REL(R.fog_b); REL(R.bloom);
    { int i; for (i = 0; i < BLOOM_LEVELS; i++) REL(R.bl[i]); }
    REL(R.fog_h[0]); REL(R.fog_h[1]); R.fog_hvalid = 0;
    REL(R.floor_t[0]); REL(R.floor_t[1]); R.floor_valid = 0;
    REL(R.color); REL(R.edges); REL(R.blend); REL(R.area); REL(R.search);
    REL(R.ao_a); REL(R.ao_b); REL(R.ao_col); REL(R.lindepth);
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
    if (R.fog && (FAILED(IDirect3DDevice9_CreateTexture(dev, 1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                                        D3DPOOL_DEFAULT, &R.floor_t[0], NULL)) ||
                  FAILED(IDirect3DDevice9_CreateTexture(dev, 1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                                        D3DPOOL_DEFAULT, &R.floor_t[1], NULL)))) {
        REL(R.floor_t[0]); REL(R.floor_t[1]);        /* optional: no ground mist */
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
void postfx_reset(void)
{
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

/* The scene's linear view depth, half resolution, for soft particles. */
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
}

static void set_mat(ID3DXEffect *fx, const char *name, const float *m);

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
    float p11, p22, p33, p43, pr[12][4], col[12][4], pls[4], pls2[4], sigma;
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
        /* history: last frame's camera; none after a gap or a reset */
        set_mat(R.fog, "gmFogPrevView", R.fog_prev_view);
        set_vec(R.fog, "gvFogPrevProj", R.fog_prev_p11, R.fog_prev_p22, R.fog_hvalid ? 0.08f : 0.0f, 0);
        /* indoors: lamp shafts and ground mist (none without the floor targets) */
        set_mat(R.fog, "gmFogView", v->view);
        set_vec(R.fog, "gvFogIndoor", 1.0f - mix, g_fog_lamp / 100.0f,
                R.floor_t[0] ? g_fog_mist / 1000.0f : 0.0f, g_fog_mist_h / 100.0f);
    }
    save(dev, &s);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);   /* sampled below */
    set_vec(R.fog, "gvFogMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
    set_vec(R.fog, "gvFogProj", p11, p22, p33, p43);
    set_mat(R.fog, "gmFogInvView", v->inv_view);
    set_vec(R.fog, "gvFogEye", v->eye[0], v->eye[1], v->eye[2], 0);
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
    if (R.floor_t[0]) {
        /* the floor under the camera, eased into last frame's (a gap in the
         * fog, a level change: measured afresh) */
        if (!R.fog_hvalid) R.floor_valid = 0;
        target(dev, R.floor_t[R.floor_cur ^ 1]);
        set_tex(R.fog, "floorTex2D", R.floor_t[R.floor_cur]);
        set_vec(R.fog, "gvFogFloorInit", R.floor_valid ? 1.0f : 0.0f, 0, 0, 0);
        run(R.fog, "Floor", dev, 1, 1);
        R.floor_cur ^= 1;
        R.floor_valid = 1;
        set_tex(R.fog, "floorTex2D", R.floor_t[R.floor_cur]);
    }
    target(dev, R.fog_a);
    run(R.fog, "Scatter", dev, hw, hh);
    set_tex(R.fog, "floorTex2D", NULL);
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
        float t[4] = { 0, 1, 1, 0 };
        if (scene != R.color) hdr_tonemap(t);     /* the float scene: tone-mapped here */
        set_vec(fx, "gvHdr", t[0], t[1], t[2], t[3]);
    }
    set_tex(fx, "sceneTex2D", scene);
    set_tex(fx, "bloomTex2D", R.bl[0]);
    hdr_end_scene(dev);                 /* HDR: bb is the real back buffer from here */
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    run(fx, "Composite", dev, R.w, R.h);
    set_tex(fx, "sceneTex2D", NULL);
    set_tex(fx, "bloomTex2D", NULL);
    restore(dev, &s);
}

/* The finished 3D frame: the fog, bloom and the grade, then SMAA and CAS. */
static void post_scene(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, int scene)
{
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
    if (!g_ao_on && (!g_soft || g_depth_done)) return;
    if (!(bb = bound_back_buffer(dev))) return;           /* e.g. the shadow pass */
    if (res_ensure(dev, bb)) {
        if (g_soft && !g_depth_done) lin_depth(dev);
        if (g_ao_on) { g_ao_done = 1; ao(dev, bb); }
    }
    REL(bb);
}

int postfx_wants_transparent_check(void)
{
    return !g_scene_seen || (g_ao_on && !g_ao_done) || (g_soft && !g_depth_done);
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
    settings_var("fog.mist", &g_fog_mist, 0, 200);
    settings_var("fog.mist_height", &g_fog_mist_h, 5, 400);
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
long hg_gfx_postfx_runs(int which) { return which == 2 ? g_fog_runs : which ? g_smaa_runs : g_ao_runs; }

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
           which == 7 ? &g_fog_lamp : which == 8 ? &g_fog_mist : which == 9 ? &g_fog_mist_h : &g_fog_haze;
}
void hg_gfx_nudge_fog(int which, int d)
{
    static const LONG hi[10] = { 200, 400, 400, 200, 100, 200, 100, 100, 200, 400 };
    volatile LONG *p = fog_knob(which);
    LONG v;
    if (which < 0 || which > 9) return;
    v = *p + d;
    InterlockedExchange(p, v < 0 ? 0 : v > hi[which] ? hi[which] : v);
    hg_log("postfx: fog haze %.3f/unit, density %.3f/unit (indoors %.3f), sun shafts %ld%% (sky %ld%%), light glow %ld%%, sun marched %ld units",
           g_fog_haze / 1000.0f, g_fog_density / 1000.0f, g_fog_density_in / 1000.0f, g_fog_sun, g_fog_sky, g_fog_glow, g_fog_dist);
}
int hg_gfx_fog_val(int which)
{
    return which < 0 || which > 9 ? 0 : (int)*fog_knob(which);
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
