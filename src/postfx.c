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

IDirect3DDevice9 *device_get(void);
IDirect3DTexture9 *device_depth_texture(void);
IDirect3DSurface9 *device_depth_surface(void);
int gfxprobe_camera_proj(float *m);
int hg_gfx_stock_viewing(void);

#define RVA_DRAWLIST_JUMP_18 0x003B406Cu     /* jump table 0x7b400c, entry 0x18 */
#define RVA_DRAWLIST_NOOP    0x003B3FE4u     /* its stock target */

/* settings */
static volatile LONG g_ao_on = 1;
static volatile LONG g_ao_radius = 120;      /* world units x 100 */
static volatile LONG g_ao_strength = 100;    /* percent */
static volatile LONG g_ao_show;              /* debug: the occlusion alone */
static volatile LONG g_smaa_pass = 1;        /* the SMAA pass, for A/B (the device path stays) */

/* per device */
static struct {
    IDirect3DDevice9 *dev;
    UINT w, h;
    int failed;                 /* creation failed: no retry until the next Reset */
    IDirect3DStateBlock9 *sb;
    ID3DXEffect *smaa, *ao;
    IDirect3DTexture9 *color, *edges, *blend, *area, *search, *ao_a, *ao_b;
} R;

static LONG g_ao_done, g_smaa_done;         /* this frame */
static LONG g_scene_seen;                   /* this frame reached the end of the opaque scene */
static LONG g_ao_runs, g_smaa_runs;

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
    REL(R.sb); REL(R.smaa); REL(R.ao);
    REL(R.color); REL(R.edges); REL(R.blend); REL(R.area); REL(R.search);
    REL(R.ao_a); REL(R.ao_b);
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
    if (!R.smaa || !R.ao) return 0;
    if (!rt_tex(dev, d.Width, d.Height, &R.color) || !rt_tex(dev, d.Width, d.Height, &R.edges) ||
        !rt_tex(dev, d.Width, d.Height, &R.blend) ||
        !rt_tex(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, &R.ao_a) ||
        !rt_tex(dev, (d.Width + 1) / 2, (d.Height + 1) / 2, &R.ao_b)) {
        hg_log("postfx: render targets %ux%u NOT created", d.Width, d.Height);
        return 0;
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
    res_release();
    R.failed = 0;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

typedef struct { IDirect3DSurface9 *rt, *ds; } saved;

static void save(IDirect3DDevice9 *dev, saved *s)
{
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

static void smaa(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    IDirect3DSurface9 *cs = NULL;
    saved s;
    save(dev, &s);
    IDirect3DTexture9_GetSurfaceLevel(R.color, 0, &cs);
    IDirect3DDevice9_StretchRect(dev, bb, NULL, cs, NULL, D3DTEXF_NONE);
    REL(cs);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
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

static void ao(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    UINT hw = (R.w + 1) / 2, hh = (R.h + 1) / 2;
    float p11, p22, p33, p43;
    saved s;
    if (!projection(dev, &p11, &p22, &p33, &p43)) return;
    save(dev, &s);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);   /* sampled below */
    set_vec(R.ao, "gvAoMetrics", 1.0f / R.w, 1.0f / R.h, (float)R.w, (float)R.h);
    set_vec(R.ao, "gvAoProj", p11, p22, p33, p43);
    set_vec(R.ao, "gvAoParams", g_ao_radius / 100.0f, g_ao_strength / 100.0f * 2.0f, 40.0f, 90.0f);
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
    run(R.ao, g_ao_show ? "Show" : "Apply", dev, R.w, R.h);
    set_tex(R.ao, "depthTex2D", NULL);                    /* before it is a depth buffer again */
    set_tex(R.ao, "aoTex2D", NULL);
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
                    sum += ((const unsigned char *)lr.pBits)[y * lr.Pitch + x * 4 + 2];
            IDirect3DSurface9_UnlockRect(mem);
            mean = (long)(sum * 1000.0 / (255.0 * ((hh + 3) / 4) * ((hw + 3) / 4)));
        }
        REL(mem); REL(src);
        postfx_trace('A', mean);
    }
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
    if (!g_ao_on || g_ao_done || hg_gfx_stock_viewing() || !dev || !device_depth_texture()) return;
    if (!(bb = bound_back_buffer(dev))) return;           /* e.g. the shadow pass */
    g_ao_done = 1;
    if (res_ensure(dev, bb)) ao(dev, bb);
    REL(bb);
}

int postfx_wants_transparent_check(void) { return !g_scene_seen || (g_ao_on && !g_ao_done); }

/* From marker_stub: the older scene path's opaque/transparent marker. */
void __cdecl postfx_marker(void) { postfx_before_transparent('M'); }

/* From gfxprobe's BeginPass hook: ui.fxo is about to draw. */
void postfx_before_ui(void)
{
    IDirect3DDevice9 *dev = device_get();
    IDirect3DSurface9 *bb;
    /* not before the 3D scene (UI drawn early, e.g. name plates): frames
     * without one get their SMAA at Present */
    if (!g_scene_seen || !g_smaa_pass || g_smaa_done || hg_gfx_stock_viewing() || !dev || !device_depth_texture()) return;
    if (!(bb = bound_back_buffer(dev))) return;
    g_smaa_done = 1;
    if (res_ensure(dev, bb)) smaa(dev, bb);
    REL(bb);
}

/* From src/device.c at Present, once a frame: 1 if the frame never reached
 * the UI and still needs its SMAA (postfx_present_draw, inside a scene the
 * caller opens). Resets the per-frame flags either way. */
static int g_smaa_pending;
int postfx_present(IDirect3DDevice9 *dev)
{
    int need = g_smaa_pass && !g_smaa_done && !hg_gfx_stock_viewing() && device_depth_texture() != NULL;
    static DWORD last;
    DWORD now = GetTickCount();
    (void)dev;
    if (g_tr_on) {
        hg_log("postfx: frame trace (C clear after n opaque draws; P/B/Z/M trigger; A AO ran, mean x1000): %s", g_tr_n ? g_tr : "(nothing)");
        g_tr_on = 0;
    }
    if (now - last > 10000) { last = now; g_tr_on = 1; g_tr_n = 0; g_tr[0] = 0; }
    g_ao_done = g_smaa_done = g_scene_seen = 0;
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
    if (res_ensure(dev, bb)) smaa(dev, bb);
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

void postfx_install(unsigned int image)
{
    void **slot = (void **)(image + RVA_DRAWLIST_JUMP_18);
    DWORD old;
    g_image = image;
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
void hg_gfx_set_ao_show(int on) { InterlockedExchange(&g_ao_show, on ? 1 : 0); }
int  hg_gfx_ao_show(void) { return (int)g_ao_show; }

/* which: 0 radius (x100 units), 1 strength (%) */
void hg_gfx_nudge_ao(int which, int d)
{
    volatile LONG *p = which ? &g_ao_strength : &g_ao_radius;
    LONG v = *p + d;
    if (v < 10) v = 10;
    if (v > (which ? 300 : 800)) v = which ? 300 : 800;
    InterlockedExchange(p, v);
    hg_log("postfx: AO radius %.2f strength %ld%%", g_ao_radius / 100.0f, g_ao_strength);
}
int hg_gfx_ao_radius(void) { return (int)g_ao_radius; }
int hg_gfx_ao_strength(void) { return (int)g_ao_strength; }

void hg_gfx_set_smaa_pass(int on) { InterlockedExchange(&g_smaa_pass, on ? 1 : 0); hg_log("postfx: SMAA pass %s", on ? "ON" : "off"); }
int  hg_gfx_smaa_pass(void) { return (int)g_smaa_pass; }
long hg_gfx_postfx_runs(int which) { return which ? g_smaa_runs : g_ao_runs; }
