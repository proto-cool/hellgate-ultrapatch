/*
 * Shadows from a point light: a fire, a torch or a spell casts them, you
 * included, when you are near it.
 *
 * The engine has directional shadows only (dxC_ShadowBufferSetupDirectional;
 * even the indoor one is aimed from a light's direction). This adds one cube
 * shadow map for the strongest engine point light near the camera:
 *
 * Casters. The engine redraws its near shadow map (27 units around the
 * player: characters and props) every other frame with shadowmap.fxo, whose
 * vertex shaders take View and Projection as plain constants (rigid: c4-c7,
 * c8-c11; skinned: c184-c187, c188-c191, after 180 bone registers), stored
 * transposed. Every caster draw of that pass is re-issued into the six faces
 * of a cube map around the light with only those eight registers changed:
 * the engine's own shaders still do the skinning and the alpha test, and
 * write z/w of our projection. The pass is recognised by its orthographic
 * projection's width (2 / c8.x) against the near map's reach; casters out of
 * the light's reach (world position in c0-c2.w) are skipped.
 *
 * Receivers. The per-pixel point lights in our material shaders
 * (point_lights, shaders/ultra.hlsl) multiply the one light whose position
 * is gvUltraPLS.xyz by a 4-tap lookup in the cube (sampler 13, bound here for
 * every material pass).
 *
 * The light. Material draws carry the engine's chosen lights
 * (_PointLightsPos_1, PointLightsColor, _PointLightsFalloff_1); a few are
 * read each frame into a small table, and at Present the brightest one whose
 * reach covers the camera's surroundings is picked.
 */
#include <windows.h>
#include <math.h>
#include <string.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include "panel.h"

IDirect3DDevice9 *device_get(void);
float gfxprobe_near_reach(void);

#define PLS_SIZE 512            /* cube face size */
#define PLS_NEAR 0.1f
#define MAX_LIGHTS 32

static volatile LONG g_on = 1;
static volatile LONG g_bias = 5;         /* depth bias, world units x100 */
static volatile LONG g_soft = 15;        /* filter offset, percent of the distance x10 (1.5%) */

/* per device */
static IDirect3DCubeTexture9 *g_cube;
static IDirect3DSurface9 *g_face[6], *g_ds;
static IDirect3DDevice9 *g_dev;
static int g_failed;

/* the light */
static struct { float pos[3], lum, radius; LONG seen; } g_lights[MAX_LIGHTS];
static int g_nlights;
static LONG g_frame;
static float g_eye[3];
static int g_have_eye;
static int g_active;                     /* a light is chosen */
static float g_lpos[3], g_lfar;
static volatile LONG g_params_gen = 1;   /* bumped when the light changes */
static LONG g_casts, g_replays;

/* the pass: 0 unknown for this render target, 1 near map, -1 other */
static int g_pass;
static LONG g_cleared_frame = -1;
static int g_sm_kind;                    /* current shadowmap technique: 1 rigid, 2 skinned */

/* ------------------------------------------------------------------ */

static void release(void)
{
    int i;
    for (i = 0; i < 6; i++) if (g_face[i]) { IDirect3DSurface9_Release(g_face[i]); g_face[i] = NULL; }
    if (g_ds) { IDirect3DSurface9_Release(g_ds); g_ds = NULL; }
    if (g_cube) { IDirect3DCubeTexture9_Release(g_cube); g_cube = NULL; }
    g_dev = NULL;
}

void plshadow_reset(void)
{
    release();
    g_failed = 0;
}

static int ensure(IDirect3DDevice9 *dev)
{
    int i;
    if (g_dev == dev && g_cube) return 1;
    if (g_failed) return 0;
    release();
    g_failed = 1;
    if (FAILED(IDirect3DDevice9_CreateCubeTexture(dev, PLS_SIZE, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                                  D3DPOOL_DEFAULT, &g_cube, NULL)) || !g_cube) {
        hg_log("plshadow: R32F cube %d NOT created", PLS_SIZE);
        g_cube = NULL;
        return 0;
    }
    for (i = 0; i < 6; i++)
        IDirect3DCubeTexture9_GetCubeMapSurface(g_cube, (D3DCUBEMAP_FACES)i, 0, &g_face[i]);
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(dev, PLS_SIZE, PLS_SIZE, D3DFMT_D24X8,
                                                         D3DMULTISAMPLE_NONE, 0, TRUE, &g_ds, NULL))) {
        hg_log("plshadow: depth surface NOT created");
        release();
        return 0;
    }
    g_dev = dev;
    g_failed = 0;
    InterlockedIncrement(&g_params_gen);            /* a new texture for the effects */
    hg_log("plshadow: cube shadow map %dx%d x6 ready", PLS_SIZE, PLS_SIZE);
    return 1;
}

/* ------------------------------------------------------------------ */
/* the light                                                           */

/* From gfxprobe at a material draw (a few a frame): the engine's lights. */
void plshadow_collect(ID3DXEffect *fx)
{
    static LONG last_frame;
    static int n_this_frame;
    D3DXVECTOR4 pos[5], col[5], fal[5];
    D3DXHANDLE hp, hc, hf, he;
    int k;
    if (!g_on) return;
    if (last_frame != g_frame) { last_frame = g_frame; n_this_frame = 0; }
    if (++n_this_frame > 24) return;
    hp = fx->lpVtbl->GetParameterByName(fx, NULL, "_PointLightsPos_1");
    hc = fx->lpVtbl->GetParameterByName(fx, NULL, "PointLightsColor");
    hf = fx->lpVtbl->GetParameterByName(fx, NULL, "_PointLightsFalloff_1");
    if (!g_have_eye && (he = fx->lpVtbl->GetParameterByName(fx, NULL, "EyeInWorld"))) {
        D3DXVECTOR4 e;
        if (SUCCEEDED(fx->lpVtbl->GetVector(fx, he, &e))) {
            g_eye[0] = e.x; g_eye[1] = e.y; g_eye[2] = e.z;
            g_have_eye = 1;
        }
    }
    if (!hp || !hc || !hf) return;
    if (FAILED(fx->lpVtbl->GetVectorArray(fx, hp, pos, 5)) || FAILED(fx->lpVtbl->GetVectorArray(fx, hc, col, 5)) ||
        FAILED(fx->lpVtbl->GetVectorArray(fx, hf, fal, 5)))
        return;
    for (k = 0; k < 5; k++) {
        float lum = 0.3f * col[k].x + 0.59f * col[k].y + 0.11f * col[k].z, radius;
        int i;
        if (lum <= 0.01f || fal[k].y <= 1e-5f || fal[k].x <= 0) continue;   /* unused slot */
        radius = fal[k].x / fal[k].y;                                        /* where att reaches 0 */
        for (i = 0; i < g_nlights; i++) {
            float dx = g_lights[i].pos[0] - pos[k].x, dy = g_lights[i].pos[1] - pos[k].y, dz = g_lights[i].pos[2] - pos[k].z;
            if (dx * dx + dy * dy + dz * dz < 1e-4f) break;
        }
        if (i == g_nlights) {
            if (g_nlights < MAX_LIGHTS) g_nlights++;
            else {                                                           /* replace the stalest */
                int j;
                for (i = 0, j = 1; j < MAX_LIGHTS; j++) if (g_lights[j].seen < g_lights[i].seen) i = j;
            }
        }
        g_lights[i].pos[0] = pos[k].x; g_lights[i].pos[1] = pos[k].y; g_lights[i].pos[2] = pos[k].z;
        g_lights[i].lum = lum;
        g_lights[i].radius = radius;
        g_lights[i].seen = g_frame;
    }
}

/* At Present: choose the light for the next frame. */
void plshadow_frame(void)
{
    int i, best = -1;
    float best_score = 0;
    g_frame++;
    if (g_on && g_have_eye) {
        for (i = 0; i < g_nlights; i++) {
            float dx, dy, dz, d, reach, score;
            if (g_frame - g_lights[i].seen > 30) continue;                   /* gone */
            if (g_lights[i].radius < 1.5f) continue;                         /* a glint, not a fire */
            dx = g_lights[i].pos[0] - g_eye[0]; dy = g_lights[i].pos[1] - g_eye[1]; dz = g_lights[i].pos[2] - g_eye[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            reach = g_lights[i].radius + 6.0f;                               /* the camera sits behind the player */
            if (d > reach) continue;
            score = g_lights[i].lum * (1.0f - d / reach);
            if (score > best_score) { best_score = score; best = i; }
        }
    }
    if (best < 0) {
        if (g_active) { g_active = 0; InterlockedIncrement(&g_params_gen); }
    } else {
        float *p = g_lights[best].pos;
        float dx = p[0] - g_lpos[0], dy = p[1] - g_lpos[1], dz = p[2] - g_lpos[2];
        if (!g_active || dx * dx + dy * dy + dz * dz > 1e-6f || fabsf(g_lfar - g_lights[best].radius) > 0.01f) {
            if (!g_active || dx * dx + dy * dy + dz * dz > 1.0f)
                hg_log("plshadow: light at %.1f %.1f %.1f, reach %.1f", p[0], p[1], p[2], g_lights[best].radius);
            memcpy(g_lpos, p, sizeof g_lpos);
            g_lfar = g_lights[best].radius;
            g_active = 1;
            InterlockedIncrement(&g_params_gen);
        }
    }
    g_have_eye = 0;
}

/* The receiver knobs (gfxprobe's ultra_apply); returns the generation. */
LONG plshadow_params(float pls[4], float pls2[4])
{
    float f = g_lfar, n = PLS_NEAR;
    int on = g_on && g_active && g_cube;
    pls[0] = g_lpos[0]; pls[1] = g_lpos[1]; pls[2] = g_lpos[2]; pls[3] = on ? 1.0f : 0.0f;
    pls2[0] = f / (f - n);
    pls2[1] = f * n / (f - n);
    pls2[2] = g_bias / 100.0f;
    pls2[3] = g_soft / 1000.0f;
    return g_params_gen + (on ? 0 : 0x40000000);
}

/* The cube, for the effects' tUltraPLShadow (gfxprobe's ultra_apply). */
IDirect3DBaseTexture9 *plshadow_texture(void) { return g_active ? (IDirect3DBaseTexture9 *)g_cube : NULL; }

/* At every material pass: sampler 13's filtering (the effect sets only its
 * texture; R32F needs point sampling). */
void plshadow_bind(IDirect3DDevice9 *dev)
{
    if (!g_cube || !g_active) return;
    IDirect3DDevice9_SetTexture(dev, 13, (IDirect3DBaseTexture9 *)g_cube);
    IDirect3DDevice9_SetSamplerState(dev, 13, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 13, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 13, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 13, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 13, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 13, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 13, D3DSAMP_SRGBTEXTURE, 0);
}

/* ------------------------------------------------------------------ */
/* casters                                                             */

void plshadow_technique(int skinned) { g_sm_kind = skinned ? 2 : 1; }
void plshadow_rt_changed(void) { g_pass = 0; }

/* the six faces: look directions and ups (D3D cube map convention) */
static const float k_dir[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
static const float k_up[6][3]  = { {0,1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {0,1,0}, {0,1,0} };

/* transpose(M) for M = LookAtLH(light, light + dir, up), rows = registers */
static void face_view(int f, float out[16])
{
    const float *z = k_dir[f], *up = k_up[f];
    float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    const float *e = g_lpos;
    /* LookAtLH rows: (x.x y.x z.x 0) (x.y y.y z.y 0) (x.z y.z z.z 0) (-x.e -y.e -z.e 1);
     * its transpose, register i = column i: */
    out[0] = x[0]; out[1] = x[1]; out[2] = x[2]; out[3] = -(x[0] * e[0] + x[1] * e[1] + x[2] * e[2]);
    out[4] = y[0]; out[5] = y[1]; out[6] = y[2]; out[7] = -(y[0] * e[0] + y[1] * e[1] + y[2] * e[2]);
    out[8] = z[0]; out[9] = z[1]; out[10] = z[2]; out[11] = -(z[0] * e[0] + z[1] * e[1] + z[2] * e[2]);
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
}

/* transpose(PerspectiveFovLH(90 degrees, 1, n, f)) */
static void face_proj(float out[16])
{
    float f = g_lfar, n = PLS_NEAR, q = f / (f - n);
    memset(out, 0, 16 * sizeof(float));
    out[0] = 1.0f;                  /* column 0: (1, 0, 0, 0) */
    out[5] = 1.0f;                  /* column 1 */
    out[10] = q; out[11] = -n * q;  /* column 2: (0, 0, q, -nq) */
    out[14] = 1.0f;                 /* column 3: (0, 0, 1, 0) */
}

typedef HRESULT (STDMETHODCALLTYPE *dip_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

/* From gfxprobe's DrawIndexedPrimitive hook, after the engine's own draw. */
void plshadow_dip(IDirect3DDevice9 *dev, dip_fn draw, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                  UINT si, UINT pc)
{
    UINT vreg, preg, wreg;
    float c[4], w[12], saved[32], view[16], proj[16];
    IDirect3DSurface9 *rt = NULL, *ds = NULL;
    D3DVIEWPORT9 vp, fvp = { 0, 0, PLS_SIZE, PLS_SIZE, 0.0f, 1.0f };
    int f;
    if (!g_on || !g_active || g_sm_kind == 0 || g_pass < 0) return;
    vreg = g_sm_kind == 2 ? 184 : 4;
    preg = vreg + 4;
    wreg = g_sm_kind == 2 ? 180 : 0;
    if (g_pass == 0) {
        /* is this the near map? its orthographic width is the near reach */
        float width;
        D3DSURFACE_DESC d;
        g_pass = -1;
        if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)) || !rt) return;
        IDirect3DSurface9_GetDesc(rt, &d);
        IDirect3DSurface9_Release(rt); rt = NULL;
        if (d.Format != D3DFMT_R32F) return;
        IDirect3DDevice9_GetVertexShaderConstantF(dev, preg, c, 1);
        width = c[0] > 1e-6f ? 2.0f / c[0] : 0;
        if (fabsf(width - gfxprobe_near_reach()) > 1.0f) return;
        g_pass = 1;
    }
    if (!ensure(dev)) return;
    /* casters out of the light's reach cast nothing into this cube */
    IDirect3DDevice9_GetVertexShaderConstantF(dev, wreg, w, 3);
    {
        float dx = w[3] - g_lpos[0], dy = w[7] - g_lpos[1], dz = w[11] - g_lpos[2];
        float r = g_lfar + 4.0f;
        if (dx * dx + dy * dy + dz * dz > r * r) return;
    }
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DDevice9_GetViewport(dev, &vp);
    IDirect3DDevice9_GetVertexShaderConstantF(dev, vreg, saved, 8);
    if (g_cleared_frame != g_frame) {
        g_cleared_frame = g_frame;
        for (f = 0; f < 6; f++) {
            IDirect3DDevice9_SetRenderTarget(dev, 0, g_face[f]);
            IDirect3DDevice9_SetDepthStencilSurface(dev, g_ds);
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffffffff, 1.0f, 0);
        }
        InterlockedIncrement(&g_casts);
    }
    face_proj(proj);
    for (f = 0; f < 6; f++) {
        face_view(f, view);
        IDirect3DDevice9_SetRenderTarget(dev, 0, g_face[f]);
        IDirect3DDevice9_SetDepthStencilSurface(dev, g_ds);
        IDirect3DDevice9_SetViewport(dev, &fvp);
        IDirect3DDevice9_SetVertexShaderConstantF(dev, vreg, view, 4);
        IDirect3DDevice9_SetVertexShaderConstantF(dev, preg, proj, 4);
        draw(dev, t, bv, mi, nv, si, pc);
    }
    InterlockedIncrement(&g_replays);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, vreg, saved, 8);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    IDirect3DDevice9_SetViewport(dev, &vp);
    if (rt) IDirect3DSurface9_Release(rt);
    if (ds) IDirect3DSurface9_Release(ds);
    g_pass = 1;            /* SetRenderTarget above reset it; this is still the near pass */
}

/* ------------------------------------------------------------------ */
/* panel                                                               */

void hg_gfx_set_plshadow(int on)
{
    InterlockedExchange(&g_on, on ? 1 : 0);
    InterlockedIncrement(&g_params_gen);
    hg_log("plshadow: point-light shadows %s", on ? "ON" : "off");
}
int hg_gfx_plshadow(void) { return (int)g_on; }
void hg_gfx_nudge_plshadow(int which, int d)
{
    volatile LONG *p = which ? &g_soft : &g_bias;
    LONG v = *p + d;
    InterlockedExchange(p, v < 0 ? 0 : v > 200 ? 200 : v);
    InterlockedIncrement(&g_params_gen);
}
int hg_gfx_plshadow_val(int which) { return (int)(which ? g_soft : g_bias); }
/* "light at x y z" / counters for the panel */
int hg_gfx_plshadow_status(float *pos, long *casts, long *replays)
{
    pos[0] = g_lpos[0]; pos[1] = g_lpos[1]; pos[2] = g_lpos[2];
    *casts = g_casts; *replays = g_replays;
    return g_active;
}
