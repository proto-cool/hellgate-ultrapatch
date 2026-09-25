/*
 * Offline check of the volumetric fog (shaders/fog.fx) on synthetic scenes:
 * the chain src/postfx.c runs, frame after frame (the mist volume is kept
 * between frames), on a depth buffer and a level mask ray-traced here.
 *
 *   fogtest.exe <d3dx9_34.dll> <fog.fxo> <out.raw> [options]
 *
 * Writes the final image (the fog applied over a flat 0.3 grey scene) as
 * W x H luminance floats; tools/fx/fogimg.py turns it into a PNG.
 *
 * Options (name=value):
 *   scene=  floor      a floor at z 0 and a wall at y 30
 *           stairs     the floor steps down 0.3 per unit from y 12 to 24,
 *                      then a lower floor at -3.6
 *           trench     the floor drops 4 units for x > 1 (a ledge running
 *                      away from the camera)
 *           well       a spiral stairwell's well: a round hole 3 units
 *                      across, 6 deep, 8 ahead
 *   player=y           a player (a box 0.8 x 0.6 x 1.8, not level) y ahead
 *   prop=y             a prop (a box 3 x 2 x 1.2, not level) y ahead
 *   pitch=, eyez=      the camera: degrees down, height (default 35, 5)
 *   lamp=x,y,z         one lamp (default 0,15,4), reach 12.8
 *   mist=, h=          mist density per unit (default 0.5), height (0.6)
 *   frames=            frames run (default 30)
 *   reveal=n           no floor is seen before frame n (as round a corner):
 *                      prints the mist cover mid-screen every frame
 *   level=all          the player and prop count as level (a broken mask)
 *   move=k             the volume's origin moves k cells in y each frame
 *                      (the camera stands still: the image must not change)
 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 360
#define VN 96          /* the volume: cells across */
#define VS 32          /* slices */
#define VT 6           /* tiles across the atlas */
#define VC 0.5f        /* cell size */

typedef HRESULT (WINAPI *create_fx_fn)(IDirect3DDevice9 *, const void *, UINT, const D3DXMACRO *,
                                       ID3DXInclude *, DWORD, ID3DXEffectPool *, ID3DXEffect **, ID3DXBuffer **);

static IDirect3DDevice9 *dev;
static ID3DXEffect *fx;
static char g_scene[32] = "floor";
static float g_player = -1, g_prop = -1, g_pitch = 35, g_eyez = 5, g_lamp[3] = { 0, 15, 4 };
static float g_mist = 0.5f, g_h = 0.6f;
static int g_frames = 30, g_level_all, g_move, g_reveal = -1;

static void vec(const char *n, float x, float y, float z, float w)
{
    D3DXVECTOR4 v = { x, y, z, w };
    fx->lpVtbl->SetVector(fx, fx->lpVtbl->GetParameterByName(fx, NULL, n), &v);
}

static void tex(const char *n, void *t)
{
    fx->lpVtbl->SetTexture(fx, fx->lpVtbl->GetParameterByName(fx, NULL, n), (IDirect3DBaseTexture9 *)t);
}

static IDirect3DSurface9 *surf(IDirect3DTexture9 *t)
{
    IDirect3DSurface9 *s;
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &s);
    IDirect3DSurface9_Release(s);          /* the texture keeps it */
    return s;
}

static IDirect3DTexture9 *rt(int w, int h, D3DFORMAT f)
{
    IDirect3DTexture9 *t = NULL;
    IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &t, NULL);
    return t;
}

/* a fullscreen pass of technique tech into t (w x h), half-pixel shifted */
static void pass(const char *tech, IDirect3DTexture9 *t, int w, int h)
{
    float px = 1.0f / w, py = 1.0f / h;
    float q[4][6] = { { -1 - px, 1 + py, 0, 1, 0, 0 }, { 1 - px, 1 + py, 0, 1, 1, 0 },
                      { -1 - px, -1 + py, 0, 1, 0, 1 }, { 1 - px, -1 + py, 0, 1, 1, 1 } };
    UINT np;
    IDirect3DDevice9_SetRenderTarget(dev, 0, surf(t));
    fx->lpVtbl->SetTechnique(fx, fx->lpVtbl->GetTechniqueByName(fx, tech));
    fx->lpVtbl->Begin(fx, &np, 0);
    fx->lpVtbl->BeginPass(fx, 0);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    fx->lpVtbl->EndPass(fx);
    fx->lpVtbl->End(fx);
}

/* the scene's floor height at (x, y) */
static float ground(float x, float y)
{
    if (!strcmp(g_scene, "stairs")) return y < 12 ? 0 : y < 24 ? -0.3f * (1 + (int)(y - 12)) : -3.6f;
    if (!strcmp(g_scene, "trench")) return x > 1 ? -4.0f : 0.0f;
    if (!strcmp(g_scene, "well")) return x * x + (y - 8) * (y - 8) < 2.25f ? -6.0f : 0.0f;
    return 0;
}

static int in_box(const float *p, float x0, float x1, float y0, float y1, float z0, float z1)
{
    return p[0] > x0 && p[0] < x1 && p[1] > y0 && p[1] < y1 && p[2] > z0 && p[2] < z1;
}

/* march the ray until it is inside something: t, and whether that is level */
static float trace(const float *E, const float *d, int *level)
{
    float t;
    for (t = 0.3f; t < 80; t += 0.02f) {
        float p[3] = { E[0] + d[0] * t, E[1] + d[1] * t, E[2] + d[2] * t };
        *level = 1;
        if (p[2] <= ground(p[0], p[1]) || p[1] >= 30 || fabsf(p[0]) > 12) return t;
        if (g_player >= 0 && in_box(p, -0.4f, 0.4f, g_player, g_player + 0.6f, ground(0, g_player), ground(0, g_player) + 1.8f)) {
            *level = g_level_all;
            return t;
        }
        if (g_prop >= 0 && in_box(p, -1.5f, 1.5f, g_prop, g_prop + 2, 0, 1.2f)) {
            *level = g_level_all;
            return t;
        }
    }
    *level = 0;
    return 1e9f;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: fogtest <d3dx9_34.dll> <fog.fxo> <out.raw> [name=value ...]\n"); return 2; }
    for (int i = 4; i < argc; i++) {
        char *v = strchr(argv[i], '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(argv[i], "scene")) lstrcpynA(g_scene, v, sizeof g_scene);
        else if (!strcmp(argv[i], "player")) g_player = (float)atof(v);
        else if (!strcmp(argv[i], "prop")) g_prop = (float)atof(v);
        else if (!strcmp(argv[i], "pitch")) g_pitch = (float)atof(v);
        else if (!strcmp(argv[i], "eyez")) g_eyez = (float)atof(v);
        else if (!strcmp(argv[i], "lamp")) sscanf(v, "%f,%f,%f", &g_lamp[0], &g_lamp[1], &g_lamp[2]);
        else if (!strcmp(argv[i], "mist")) g_mist = (float)atof(v);
        else if (!strcmp(argv[i], "h")) g_h = (float)atof(v);
        else if (!strcmp(argv[i], "frames")) g_frames = atoi(v);
        else if (!strcmp(argv[i], "level")) g_level_all = !strcmp(v, "all");
        else if (!strcmp(argv[i], "move")) g_move = atoi(v);
        else if (!strcmp(argv[i], "reveal")) g_reveal = atoi(v);
    }
    HMODULE dx = LoadLibraryA(argv[1]);
    create_fx_fn create = (create_fx_fn)GetProcAddress(dx, "D3DXCreateEffect");
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND wnd = CreateWindowExA(0, "STATIC", "fogtest", WS_OVERLAPPED, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        fprintf(stderr, "no device\n");
        return 1;
    }
    FILE *f = fopen(argv[2], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
    if (FAILED(create(dev, buf, (UINT)n, NULL, NULL, 0, NULL, &fx, NULL)) || !fx) { fprintf(stderr, "effect failed\n"); return 1; }

    /* the camera (left-handed, row vectors) */
    const float a = g_pitch * 3.14159265f / 180.0f, zn = 0.5f, zf = 1000.0f;
    const float p22 = 1.0f / tanf(0.6f), p11 = p22 / ((float)W / H);
    const float p33 = zf / (zf - zn), p43 = -zn * zf / (zf - zn);
    const float E[3] = { 0, 0, g_eyez };
    const float R[3] = { 1, 0, 0 }, U[3] = { 0, sinf(a), cosf(a) }, F[3] = { 0, cosf(a), -sinf(a) };
    float inv[16] = { R[0], R[1], R[2], 0, U[0], U[1], U[2], 0, F[0], F[1], F[2], 0, E[0], E[1], E[2], 1 };

    /* depth (R32F, as the INTZ reads) and the level mask */
    IDirect3DTexture9 *depth, *level, *nolevel;
    D3DLOCKED_RECT ld, ll;
    IDirect3DDevice9_CreateTexture(dev, W, H, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &depth, NULL);
    IDirect3DDevice9_CreateTexture(dev, W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &level, NULL);
    IDirect3DDevice9_CreateTexture(dev, W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &nolevel, NULL);
    {
        D3DLOCKED_RECT l0;
        IDirect3DTexture9_LockRect(nolevel, 0, &l0, NULL, 0);
        for (int y = 0; y < H; y++) memset((char *)l0.pBits + y * l0.Pitch, 0, W * 4);
        IDirect3DTexture9_UnlockRect(nolevel, 0);
    }
    IDirect3DTexture9_LockRect(depth, 0, &ld, NULL, 0);
    IDirect3DTexture9_LockRect(level, 0, &ll, NULL, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float vx = ((x + 0.5f) / W * 2 - 1) / p11, vy = (1 - (y + 0.5f) / H * 2) / p22, d[3];
            int lv;
            for (int k = 0; k < 3; k++) d[k] = R[k] * vx + U[k] * vy + F[k];
            float s = trace(E, d, &lv);
            ((float *)((char *)ld.pBits + y * ld.Pitch))[x] = s > 900 ? 1.0f : p33 + p43 / s;
            ((DWORD *)((char *)ll.pBits + y * ll.Pitch))[x] = lv ? 0xffffffffu : 0;
        }
    IDirect3DTexture9_UnlockRect(depth, 0);
    IDirect3DTexture9_UnlockRect(level, 0);

    /* the points: one per 4 x 4 pixels, each its texel centre */
    IDirect3DVertexBuffer9 *vb;
    const int npts = (W / 4) * (H / 4);
    float *pts;
    IDirect3DDevice9_CreateVertexBuffer(dev, npts * 12, D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb, NULL);
    IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&pts, 0);
    for (int y = 0; y < H / 4; y++)
        for (int x = 0; x < W / 4; x++, pts += 3) {
            pts[0] = (4 * x + 1.5f) / W; pts[1] = (4 * y + 1.5f) / H; pts[2] = 0;
        }
    IDirect3DVertexBuffer9_Unlock(vb);

    const int AW = VT * VN, AH = ((VS + VT - 1) / VT) * VN;
    IDirect3DTexture9 *vol[2] = { rt(AW, AH, D3DFMT_A16B16G16R16F), rt(AW, AH, D3DFMT_A16B16G16R16F) };
    IDirect3DTexture9 *inj = rt(AW, AH, D3DFMT_A16B16G16R16F);
    IDirect3DSurface9 *fsys;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W / 2, H / 2, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &fsys, NULL);
    IDirect3DTexture9 *fa = rt(W / 2, H / 2, D3DFMT_A16B16G16R16F), *fb = rt(W / 2, H / 2, D3DFMT_A16B16G16R16F);
    IDirect3DTexture9 *out = rt(W, H, D3DFMT_A32B32G32R32F);

    /* the parameters src/postfx.c sets */
    vec("gvFogMetrics", 1.0f / W, 1.0f / H, W, H);
    vec("gvFogProj", p11, p22, p33, p43);
    fx->lpVtbl->SetMatrix(fx, fx->lpVtbl->GetParameterByName(fx, NULL, "gmFogInvView"), (D3DXMATRIX *)inv);
    vec("gvFogEye", E[0], E[1], E[2], 10.0f);
    vec("gvFogParams", 0.012f, 60, 1, 1);
    vec("gvFogSun", 0, 0, 0, 0);
    vec("gvFogHaze", 0.001f, 8, 0, 0);
    vec("gvFogColor", 0.15f, 0.18f, 0.20f, 0);
    vec("gvFogEngine", 0, 160, 1, 0);
    vec("gvFogPrevProj", 0, 0, 0, 0);
    vec("gvFogIndoor", 1, 0, g_mist, g_h);
    {
        D3DXVECTOR4 lp = { g_lamp[0], g_lamp[1], g_lamp[2], 12.8f }, lc = { 1, 1, 1, 0 };
        fx->lpVtbl->SetVectorArray(fx, fx->lpVtbl->GetParameterByName(fx, NULL, "gvFogLights"), &lp, 1);
        fx->lpVtbl->SetVectorArray(fx, fx->lpVtbl->GetParameterByName(fx, NULL, "gvFogLightCol"), &lc, 1);
    }
    tex("depthTex2D", depth);
    tex("levelTex2D", level);
    const float ox = floorf(E[0] / VC) * VC - VN * VC * 0.5f, oy = floorf(E[1] / VC) * VC - VN * VC * 0.5f;
    const float oz = floorf((E[2] - 13) / VC) * VC;
    vec("gvFogVolDim", VN, VS, VT, 0);
    vec("gvFogVolAtlas", 1.0f / AW, 1.0f / AH, AW, AH);

    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetRenderTarget(dev, 0, surf(vol[0]));
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    tex("levelTex2D", level);
    int cur = 0;
    for (int fr = 0; fr < g_frames; fr++) {
        /* as src/postfx.c: this frame's floor into its own target, one
         * draw per slice, then last frame's volume moved and eased to it */
        vec("gvFogVol", ox, oy + fr * g_move * VC, oz, VC);
        tex("levelTex2D", fr < g_reveal ? nolevel : level);
        IDirect3DDevice9_SetRenderTarget(dev, 0, surf(inj));
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        for (int k = 0; k <= (int)ceilf(3 * g_h / VC); k++) {
            UINT np;
            vec("gvFogVolStep", (float)k, 0, 0, 0);
            fx->lpVtbl->SetTechnique(fx, fx->lpVtbl->GetTechniqueByName(fx, "VolInject"));
            fx->lpVtbl->Begin(fx, &np, 0);
            fx->lpVtbl->BeginPass(fx, 0);
            IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, 12);
            IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ);
            IDirect3DDevice9_DrawPrimitive(dev, D3DPT_POINTLIST, 0, npts);
            fx->lpVtbl->EndPass(fx);
            fx->lpVtbl->End(fx);
        }
        tex("volTex2D", vol[cur]);
        tex("injTex2D", inj);
        vec("gvFogVolStep", 0, fr ? (float)g_move : 0, 0, fr ? 0.995f : 0.0f);
        vec("gvFogVolRate", fr ? 0.1f : 1.0f, 0, 0, 0);
        pass("VolCopy", vol[cur ^ 1], AW, AH);
        cur ^= 1;
        tex("injTex2D", NULL);
        tex("volTex2D", vol[cur]);
        vec("gvFogHaze", 0.001f, 8, (float)fr * 0.618034f - floorf((float)fr * 0.618034f), 0);
        pass("Scatter", fa, W / 2, H / 2);
        if (g_reveal >= 0) {
            D3DLOCKED_RECT lf;
            IDirect3DDevice9_GetRenderTargetData(dev, surf(fa), fsys);
            IDirect3DSurface9_LockRect(fsys, &lf, NULL, D3DLOCK_READONLY);
            unsigned short a16 = ((unsigned short *)((char *)lf.pBits + (H / 3) * lf.Pitch))[(W / 4) * 4 + 3];
            int e = (a16 >> 10) & 31, m = a16 & 1023;
            float T = e ? (1024 + m) * powf(2, e - 25) : m * powf(2, -24);
            printf("%d:%.0f ", fr, 100 * (1 - T));
            IDirect3DSurface9_UnlockRect(fsys);
        }
    }
    if (g_reveal >= 0) printf("\n");
    /* Temporal (no history), Blur across, Blur down, Apply over grey */
    tex("fogTex2D", fa);
    vec("gvFogPass", 2.0f / W, 2.0f / H, 0, 0);
    pass("Temporal", fb, W / 2, H / 2);
    tex("fogTex2D", fb);
    vec("gvFogPass", 2.0f / W, 2.0f / H, 2.0f / W, 0);
    pass("Blur", fa, W / 2, H / 2);
    tex("fogTex2D", fa);
    vec("gvFogPass", 2.0f / W, 2.0f / H, 0, 2.0f / H);
    pass("Blur", fb, W / 2, H / 2);
    IDirect3DDevice9_SetRenderTarget(dev, 0, surf(out));
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff4c4c4cu, 1.0f, 0);
    tex("fogTex2D", fb);
    vec("gvFogPass", 2.0f / W, 2.0f / H, 0, 0);
    pass("Apply", out, W, H);
    IDirect3DDevice9_EndScene(dev);

    IDirect3DSurface9 *sys;
    D3DLOCKED_RECT lr;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_GetRenderTargetData(dev, surf(out), sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    FILE *fo = fopen(argv[3], "wb");
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float *c = (float *)((char *)lr.pBits + y * lr.Pitch) + 4 * x;
            float l = (c[0] + c[1] + c[2]) / 3;
            fwrite(&l, 4, 1, fo);
        }
    fclose(fo);
    IDirect3DSurface9_UnlockRect(sys);
    return 0;
}
