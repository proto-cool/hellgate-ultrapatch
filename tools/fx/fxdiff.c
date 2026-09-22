/*
 * Parity check for rewritten material effects: draw every technique of two
 * effects (the game's stock .fxo and our rebuilt one) with identical inputs
 * and compare the pixels.
 *
 *   fxdiff.exe <d3dx9_34.dll> <stock.fxo> <new.fxo> [-dump <dir>] [-only <substr>] [-seed <n>]
 *              [-set name=x,y,z,w ...] [-shadowscene]
 *
 * -shadowscene puts two disc-shaped blockers in every shadow map (depth in
 * .r) instead of noise, with full shadow intensity, so a soft-shadow filter
 * can be looked at: run with -set gvUltraShadow=1,<scale>,16,0.0004 -dump.
 *
 * Our own parameters (gvUltra*) keep their all-zero default on both sides,
 * which is the stock look; -set gives them values on the new effect only,
 * to look at (-dump) or measure a non-stock mode.
 *
 * -seed changes every value; odd seeds also switch the camera light off, so
 * the branches on either side of it both get compared, and seeds 2,3 mod 4
 * light the scene brightly enough to overflow the soft clamp (see
 * fill_values). Run several seeds; tools/fx/matcheck.sh runs 0 to 3. About half of every 2D
 * texture's 8x8 blocks have alpha exactly 1.0, picked independently per
 * texture, because several shader paths test for it (the environment map is
 * masked by self-illumination alpha == 1 and gated by specular alpha).
 *
 * Both effects get the same deterministic values for every parameter, keyed
 * by the parameter's name: floats in [0.1, 1], textures filled with noise,
 * plus a few fixed parameters (matrices, bones, eye, fog range) that keep the
 * geometry on screen. The mesh is a grid carrying every vertex element the
 * material shaders read. A technique passes when no channel of any pixel
 * differs by more than the tolerance (2/255); the output lists the rest with
 * their worst difference and share of differing pixels. Fewer than 0.2% of
 * pixels off is reported as EDGE and passes: those are pixels on the grid's
 * shared triangle edges changing owner, because a reordered vertex shader
 * rounds positions a hair differently after translation to GL.
 *
 * Techniques are matched by name; a name missing from either side is an
 * error. With -dump, both images of every failing technique are written as
 * <dir>/<name>.{a,b}.ppm (RGB) and .alpha.pgm pairs.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <d3d9.h>
#include <d3dx9effect.h>

typedef HRESULT (WINAPI *create_fx_fn)(IDirect3DDevice9 *, const void *, UINT, const D3DXMACRO *,
                                       ID3DXInclude *, DWORD, ID3DXEffectPool *, ID3DXEffect **,
                                       ID3DXBuffer **);

#define W 256
#define H 256
#define GRID 24
#define TOL 2

static IDirect3DDevice9 *dev;
static unsigned int g_seed;
static int g_shadowscene;   /* -shadowscene: shadow maps hold a blocker disc, not noise */

static unsigned int fnv(const char *s)
{
    unsigned int h = 0x811C9DC5u;
    while (*s) h = (h ^ (unsigned char)*s++) * 0x01000193u;
    return h;
}

static unsigned int rng(unsigned int *s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

static float frand(unsigned int *s) { return (rng(s) & 0xFFFFFF) / (float)0xFFFFFF; }

/* One noise texture per parameter name, shared by both effects. */
#define MAX_TEX 64
static struct { unsigned int key; IDirect3DBaseTexture9 *tex; } g_tex[MAX_TEX];
static int g_ntex;

static IDirect3DBaseTexture9 *noise_texture(const char *name, int cube)
{
    unsigned int key = (fnv(name) ^ (cube ? 0x5A5A5A5Au : 0)) + g_seed * 0x9E3779B9u;
    for (int i = 0; i < g_ntex; i++) if (g_tex[i].key == key) return g_tex[i].tex;
    unsigned int s = key | 1;
    IDirect3DBaseTexture9 *bt = NULL;
    D3DLOCKED_RECT lr;
    if (cube) {
        IDirect3DCubeTexture9 *t = NULL;
        if (FAILED(IDirect3DDevice9_CreateCubeTexture(dev, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) return NULL;
        for (int f = 0; f < 6; f++) {
            IDirect3DCubeTexture9_LockRect(t, (D3DCUBEMAP_FACES)f, 0, &lr, NULL, 0);
            for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++)
                ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = rng(&s);
            IDirect3DCubeTexture9_UnlockRect(t, (D3DCUBEMAP_FACES)f, 0);
        }
        bt = (IDirect3DBaseTexture9 *)t;
    } else if (g_shadowscene && strstr(name, "Shadow")) {
        /* depth in .r: open ground at 1.0, a disc-shaped blocker at 0.1 in
         * the middle and a smaller one at 0.25, so penumbrae of two widths */
        IDirect3DTexture9 *t = NULL;
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) return NULL;
        IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
        for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) {
            float dx = x - 110.0f, dy = y - 120.0f, ex = x - 190.0f, ey = y - 70.0f;
            unsigned int d = 255;
            /* the same blockers in every shadow map: outdoor backgrounds
             * take min(second map, (main + 1) / 2), and the second map is
             * where their full-strength shadow comes from */
            if (dx * dx + dy * dy < 40 * 40) d = 26;
            else if (ex * ex + ey * ey < 18 * 18) d = 64;
            ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xFF000000u | (d << 16);
        }
        IDirect3DTexture9_UnlockRect(t, 0);
        bt = (IDirect3DBaseTexture9 *)t;
    } else if (g_shadowscene) {
        /* everything else flat, so the image shows the shadow and nothing else */
        IDirect3DTexture9 *t = NULL;
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) return NULL;
        IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
            ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = strstr(name, "Normal") ? 0xFF8080FFu : 0xFFA0A0A0u;
        IDirect3DTexture9_UnlockRect(t, 0);
        bt = (IDirect3DBaseTexture9 *)t;
    } else {
        IDirect3DTexture9 *t = NULL;
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) return NULL;
        IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
        /* smooth-ish noise: 8x8 blocks, so normal maps are not pure static */
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            /* non-linear block hash: xorshift alone is linear, and two keys
             * would then give identical or inverted patterns */
            unsigned int b = key + (x / 8) * 73856093u + (y / 8) * 19349663u;
            b ^= b >> 16; b *= 0x7FEB352Du; b ^= b >> 15; b *= 0x846CA68Bu; b ^= b >> 16;
            b |= 1;
            DWORD c = rng(&b);
            /* opaque blocks chosen per texture, so that masks read from two
             * textures (self-illumination alpha, specular alpha) do not line up */
            if (rng(&b) & 0x10000u) c |= 0xFF000000u;
            ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = c;
        }
        IDirect3DTexture9_UnlockRect(t, 0);
        bt = (IDirect3DBaseTexture9 *)t;
    }
    if (g_ntex < MAX_TEX) { g_tex[g_ntex].key = key; g_tex[g_ntex].tex = bt; g_ntex++; }
    return bt;
}

/* Deterministic values for one float parameter (or struct member), keyed by
 * its name. */
static void fill_values(const char *nm, float *v, UINT n)
{
    unsigned int s = (fnv(nm) + g_seed * 0x9E3779B9u) | 1;
    for (UINT k = 0; k < n; k++) v[k] = 0.1f + 0.9f * frand(&s);
    if (!strcmp(nm, "WorldViewProjection") || !strcmp(nm, "World") || !strcmp(nm, "WorldView")) {
        memset(v, 0, n * sizeof *v);
        v[0] = v[5] = v[10] = v[15] = 1.0f;
    } else if (!strcmp(nm, "Bones")) {
        /* 3 rows per bone: identity plus a small per-bone offset */
        for (UINT b = 0; b + 12 <= n; b += 12) {
            memset(v + b, 0, 12 * sizeof *v);
            v[b + 0] = v[b + 5] = v[b + 10] = 1.0f;
            v[b + 3] = 0.05f * frand(&s); v[b + 7] = 0.05f * frand(&s);
        }
    } else if (!strncmp(nm, "gmShadowMatrix", 14)) {
        memset(v, 0, n * sizeof *v);
        v[0] = 0.5f; v[5] = -0.5f; v[10] = 1.0f; v[15] = 1.0f; v[12] = 0.5f; v[13] = 0.5f;
    } else if (g_shadowscene && !strcmp(nm, "gvShadowSize")) {
        v[0] = 256.0f; v[1] = 256.0f; v[2] = 1.0f / 256.0f; v[3] = 1.0f / 256.0f;
    } else if (g_shadowscene && !strcmp(nm, "gvMiscLightingData")) {
        v[1] = 1.0f;                            /* full shadow intensity */
    } else if (g_shadowscene && !strcmp(nm, "ShadowLightDir")) {
        v[0] = 0; v[1] = 0; v[2] = 1.0f;        /* every grid normal faces it */
    } else if (!strcmp(nm, "EyeInObject") || !strcmp(nm, "EyeInWorld")) {
        v[0] = 0.2f; v[1] = -0.3f; v[2] = -2.0f; v[3] = 1.0f;
    } else if (!strcmp(nm, "FogMaxDistance")) {
        v[0] = 3.0f;
    } else if (!strcmp(nm, "FogMinDistance")) {
        v[0] = 0.5f;
    } else if (!strcmp(nm, "_CameraLightPos_World")) {
        /* in front of the grid, so the camera light reaches all of it */
        v[0] = 0.3f * frand(&s) - 0.15f; v[1] = 0.3f * frand(&s) - 0.15f; v[2] = -0.6f;
    } else if (!strcmp(nm, "_CameraLightFalloff_World")) {
        if (g_seed & 1) memset(v, 0, n * sizeof *v);           /* camera light off */
        else { v[0] = 1.2f + 0.3f * frand(&s); v[1] = 0.4f + 0.2f * frand(&s); }
    } else if (!strcmp(nm, "_CameraLightColor")) {
        /* stays bright: a weak camera light hides inside the tolerance */
    } else if ((strlen(nm) == 3 && nm[0] == 'c' && (nm[1] == 'A' || nm[1] == 'B' || nm[1] == 'C')) ||
               !strcmp(nm, "LightAmbient") || strstr(nm, "Color")) {
        /* Light colours and SH. Seeds 0,1 / 4,5 / ... keep them at game-like
         * levels, where small terms (highlights, camera light) show; seeds
         * 2,3 / 6,7 / ... leave them bright, so colours overflow the soft
         * clamp and the glow alpha it feeds is exercised. */
        if (!((g_seed >> 1) & 1))
            for (UINT k = 0; k < n; k++) v[k] *= 0.25f;
    } else if (strstr(nm, "Dir")) {
        /* directions: signed, normalised per float4 */
        for (UINT k = 0; k + 3 <= n; k += 4) {
            float x = frand(&s) * 2 - 1, y = frand(&s) * 2 - 1, z = frand(&s) * 2 - 1;
            float l = sqrtf(x * x + y * y + z * z) + 1e-3f;
            v[k] = x / l; v[k + 1] = y / l; v[k + 2] = z / l;
        }
    } else if (strstr(nm, "Pos")) {
        for (UINT k = 0; k < n; k++) v[k] = frand(&s) * 2 - 1;
    }
}

static void set_float(ID3DXEffect *fx, D3DXHANDLE h, const char *key, const D3DXPARAMETER_DESC *pd)
{
    UINT n = pd->Rows * pd->Columns * (pd->Elements ? pd->Elements : 1);
    float *v = calloc(n + 16, sizeof *v);
    fill_values(key, v, n);
    fx->lpVtbl->SetFloatArray(fx, h, v, n);
    if (getenv("FXDIFF_TRACE") && strstr(key, getenv("FXDIFF_TRACE")))
        printf("TRACE %s = %g %g %g %g\n", key, v[0], v[1], v[2], v[3]);
    free(v);
}

static void set_params(ID3DXEffect *fx)
{
    D3DXEFFECT_DESC ed;
    fx->lpVtbl->GetDesc(fx, &ed);
    for (UINT i = 0; i < ed.Parameters; i++) {
        D3DXHANDLE h = fx->lpVtbl->GetParameter(fx, NULL, i);
        D3DXPARAMETER_DESC pd;
        fx->lpVtbl->GetParameterDesc(fx, h, &pd);
        /* The material effects declare every texture as plain `texture`, so
         * the cube map is known only by its name. A 2D texture bound to a
         * cube sampler reads black and hides the whole reflection path. */
        if (pd.Type == D3DXPT_TEXTURE || pd.Type == D3DXPT_TEXTURE2D || pd.Type == D3DXPT_TEXTURECUBE) {
            int cube = pd.Type == D3DXPT_TEXTURECUBE || strstr(pd.Name, "Cube") != NULL;
            fx->lpVtbl->SetTexture(fx, h, noise_texture(pd.Name, cube));
            continue;
        }
        if (pd.Class == D3DXPC_STRUCT) {
            /* e.g. gfScrollTextures[2] { float2 fTile; float2 fPhase; }:
             * left alone it stays zero and the UV scroll path is dead */
            for (UINT e = 0; e < (pd.Elements ? pd.Elements : 1); e++) {
                D3DXHANDLE he = pd.Elements ? fx->lpVtbl->GetParameterElement(fx, h, e) : h;
                for (UINT m = 0; m < pd.StructMembers; m++) {
                    D3DXHANDLE hm = fx->lpVtbl->GetParameter(fx, he, m);
                    D3DXPARAMETER_DESC md;
                    fx->lpVtbl->GetParameterDesc(fx, hm, &md);
                    if (md.Type != D3DXPT_FLOAT) continue;
                    char key[256];
                    snprintf(key, sizeof key, "%s[%u].%s", pd.Name, e, md.Name);
                    set_float(fx, hm, key, &md);
                }
            }
            continue;
        }
        /* our own knobs (shaders/ultra.hlsl) keep their all-zero
         * default, the stock look; -set overrides them on the new effect */
        if (!strncmp(pd.Name, "gvUltra", 7)) continue;
        if (pd.Type == D3DXPT_FLOAT)
            set_float(fx, h, pd.Name, &pd);
    }
}

#pragma pack(push, 1)
typedef struct {
    float pos[3];
    unsigned char bidx[4];
    float bw[4];
    float uv0[4];        /* backgrounds: lightmap uv in xy, diffuse uv in zw */
    float nrm[4];        /* backgrounds read .w (baked sun visibility) */
    float tan[3];
    float bin[3];
    float uv1[2];
    float uv2[4];
    DWORD color;
} VERT;
#pragma pack(pop)

static const D3DVERTEXELEMENT9 g_decl[] = {
    { 0, 0,   D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12,  D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_BLENDINDICES, 0 },
    { 0, 16,  D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_BLENDWEIGHT, 0 },
    { 0, 32,  D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 48,  D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_NORMAL, 0 },
    { 0, 64,  D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_TANGENT, 0 },
    { 0, 76,  D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_BINORMAL, 0 },
    { 0, 88,  D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 1 },
    { 0, 96,  D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2 },
    { 0, 112, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 },
    D3DDECL_END()
};

static VERT g_mesh[GRID * GRID * 6];

static void unit(float *v, unsigned int *s)
{
    float x = frand(s) * 2 - 1, y = frand(s) * 2 - 1, z = -(0.3f + frand(s));
    float l = sqrtf(x * x + y * y + z * z);
    v[0] = x / l; v[1] = y / l; v[2] = z / l;
}

static void build_mesh(void)
{
    VERT grid[(GRID + 1) * (GRID + 1)];
    unsigned int s = 12345;
    for (int y = 0; y <= GRID; y++) for (int x = 0; x <= GRID; x++) {
        VERT *v = &grid[y * (GRID + 1) + x];
        memset(v, 0, sizeof *v);
        v->pos[0] = -0.95f + 1.9f * x / GRID;
        v->pos[1] = -0.95f + 1.9f * y / GRID;
        v->pos[2] = 0.3f + 0.4f * frand(&s);
        for (int k = 0; k < 3; k++) v->bidx[k] = (unsigned char)(3 * (rng(&s) % 60));
        float a = frand(&s), b = frand(&s) * (1 - a);
        v->bw[0] = a; v->bw[1] = b; v->bw[2] = 1 - a - b; v->bw[3] = 0;
        v->uv0[0] = (float)x / GRID * 2.0f; v->uv0[1] = (float)y / GRID * 2.0f;
        v->uv0[2] = (float)y / GRID * 1.5f + 0.1f; v->uv0[3] = (float)x / GRID * 1.5f + 0.3f;
        unit(v->nrm, &s); unit(v->tan, &s);
        v->nrm[3] = frand(&s);
        float bn[3]; unit(bn, &s);
        for (int k = 0; k < 3; k++) v->bin[k] = bn[k] * 0.5f + 0.5f;   /* stored 0..1 like the game's */
        v->uv1[0] = frand(&s); v->uv1[1] = frand(&s);
        for (int k = 0; k < 4; k++) v->uv2[k] = frand(&s);
        v->color = rng(&s);
    }
    int n = 0;
    for (int y = 0; y < GRID; y++) for (int x = 0; x < GRID; x++) {
        int i = y * (GRID + 1) + x;
        int q[6] = { i, i + 1, i + GRID + 1, i + 1, i + GRID + 2, i + GRID + 1 };
        for (int k = 0; k < 6; k++) g_mesh[n++] = grid[q[k]];
    }
}

static int draw(ID3DXEffect *fx, const char *tech, IDirect3DSurface9 *rt, IDirect3DSurface9 *sys, DWORD *out)
{
    D3DXHANDLE t = fx->lpVtbl->GetTechniqueByName(fx, tech);
    if (!t) return -1;
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x40404040, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    fx->lpVtbl->SetTechnique(fx, t);
    UINT passes = 0;
    fx->lpVtbl->Begin(fx, &passes, 0);
    for (UINT p = 0; p < passes; p++) {
        fx->lpVtbl->BeginPass(fx, p);
        /* every material state the parity check must not depend on */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, GRID * GRID * 2, g_mesh, sizeof(VERT));
        fx->lpVtbl->EndPass(fx);
    }
    fx->lpVtbl->End(fx);
    IDirect3DDevice9_EndScene(dev);
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys))) return -2;
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    for (int y = 0; y < H; y++) memcpy(out + y * W, (char *)lr.pBits + y * lr.Pitch, W * 4);
    IDirect3DSurface9_UnlockRect(sys);
    return 0;
}

static void dump_img(const char *dir, const char *tech, const char *tag, const DWORD *px)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/%s.%s.ppm", dir, tech, tag);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { unsigned char c[3] = { px[i] >> 16, px[i] >> 8, px[i] }; fwrite(c, 1, 3, f); }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/%s.%s.alpha.pgm", dir, tech, tag);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P5\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { unsigned char c = px[i] >> 24; fwrite(&c, 1, 1, f); }
        fclose(f);
    }
}

static ID3DXEffect *load(create_fx_fn create, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
    ID3DXEffect *fx = NULL; ID3DXBuffer *err = NULL;
    HRESULT hr = create(dev, buf, (UINT)n, NULL, NULL, 0, NULL, &fx, &err);
    free(buf);
    if (FAILED(hr) || !fx) { fprintf(stderr, "%s: D3DXCreateEffect failed hr=0x%08lx\n", path, hr); return NULL; }
    return fx;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: fxdiff <d3dx9_34.dll> <stock.fxo> <new.fxo> [-dump dir] [-only substr]\n"); return 2; }
    const char *dumpdir = NULL, *only = NULL;
    const char *sets[8]; int nsets = 0;
    for (int i = 4; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-dump")) dumpdir = argv[++i];
        else if (!strcmp(argv[i], "-only")) only = argv[++i];
        else if (!strcmp(argv[i], "-seed")) g_seed = (unsigned int)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-set") && nsets < 8) sets[nsets++] = argv[++i];
    for (int i = 4; i < argc; i++)
        if (!strcmp(argv[i], "-shadowscene")) g_shadowscene = 1;
    }
    HMODULE dx = LoadLibraryA(argv[1]);
    if (!dx) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    create_fx_fn create = (create_fx_fn)GetProcAddress(dx, "D3DXCreateEffect");
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND wnd = CreateWindowExA(0, "STATIC", "fxdiff", WS_OVERLAPPED, 0, 0, 16, 16, NULL, NULL, GetModuleHandleA(NULL), NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { fprintf(stderr, "CreateDevice failed hr=0x%08lx\n", hr); return 1; }

    IDirect3DSurface9 *rt = NULL, *sys = NULL;
    IDirect3DDevice9_CreateRenderTarget(dev, W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DDevice9_CreateVertexDeclaration(dev, g_decl, &decl);
    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    build_mesh();

    ID3DXEffect *a = load(create, argv[2]), *b = load(create, argv[3]);
    if (!a || !b) return 1;
    set_params(a);
    set_params(b);
    /* -set name=x,y,z,w on the new effect only: look at a non-stock mode */
    for (int k = 0; k < nsets; k++) {
        char nm[64]; float v[4] = {0};
        const char *eq = strchr(sets[k], '=');
        if (!eq || eq - sets[k] >= (int)sizeof nm) continue;
        memcpy(nm, sets[k], eq - sets[k]); nm[eq - sets[k]] = 0;
        sscanf(eq + 1, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
        D3DXHANDLE h = b->lpVtbl->GetParameterByName(b, NULL, nm);
        if (!h) { fprintf(stderr, "-set: no parameter %s\n", nm); return 2; }
        b->lpVtbl->SetFloatArray(b, h, v, 4);
    }

    static DWORD pa[W * H], pb[W * H];
    D3DXEFFECT_DESC ed;
    a->lpVtbl->GetDesc(a, &ed);
    int checked = 0, failed = 0, missing = 0, blank = 0, edge = 0;
    for (UINT t = 0; t < ed.Techniques; t++) {
        D3DXTECHNIQUE_DESC td;
        a->lpVtbl->GetTechniqueDesc(a, a->lpVtbl->GetTechnique(a, t), &td);
        if (only && !strstr(td.Name, only)) continue;
        if (draw(a, td.Name, rt, sys, pa) || draw(b, td.Name, rt, sys, pb)) {
            printf("MISSING %s\n", td.Name); missing++; continue;
        }
        int worst = 0, bad = 0, lit = 0;
        for (int i = 0; i < W * H; i++) {
            int d = 0;
            for (int c = 0; c < 32; c += 8) {
                int x = abs((int)((pa[i] >> c) & 255) - (int)((pb[i] >> c) & 255));
                if (x > d) d = x;
            }
            if (d > worst) worst = d;
            if (d > TOL) bad++;
            if ((pa[i] & 0xFFFFFF) != 0x404040) lit++;
        }
        checked++;
        if (lit < W * H / 4) { blank++; printf("BLANK   %s (stock drew %d px)\n", td.Name, lit); }
        if (bad && bad * 500 < W * H) {
            edge++;
            printf("EDGE    %s  max %d  %d px\n", td.Name, worst, bad);
        } else if (bad) {
            failed++;
            printf("DIFF    %s  max %d  %.2f%% px\n", td.Name, worst, 100.0 * bad / (W * H));
            if (dumpdir) { dump_img(dumpdir, td.Name, "a", pa); dump_img(dumpdir, td.Name, "b", pb); }
        }
    }
    printf("%d techniques checked, %d differ, %d edge-only, %d missing, %d mostly blank\n",
           checked, failed, edge, missing, blank);
    return (failed || missing) ? 1 : 0;
}
