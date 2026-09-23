/*
 * Graphics probe: the instrumentation behind plan step 0 (docs/graphics-plan.md).
 *
 * It answers, from one game session and one log:
 *
 *   0.3  which shader tier runs  -- D3DXCreateEffect is hooked in the game's
 *        d3dx9_34.dll and every blob is identified by (size, FNV-1a) against
 *        src/fxtable.h, so the log names each effect the engine actually
 *        creates ("actoroutdoor30.fxo" vs "...20.fxo").
 *   0.4  how to load replacement effects -- if
 *        <game>\override\<pak path> exists (and bin\hellgate_override.off does not),
 *        that file is handed to D3DX in place of the pak blob. CreateFileW
 *        is also hooked and the first distinct data\ paths the engine opens on
 *        disk are logged, which tells us whether loose files are ever
 *        consulted ahead of the archive.
 *   0.5  whether depth is readable -- once per device: back buffer and depth
 *        stencil descriptions, and CheckDeviceFormat for the readable depth
 *        formats (INTZ, RAWZ, DF24, DF16) plus NULL render targets. A frame
 *        capture (flag file bin\hellgate_gfxprobe.frame, or automatically at
 *        frame 900) logs every render-target / depth-target change and the
 *        draw count in between, which locates the ZBuffer pass and the UI pass.
 *
 * ID3DXEffect::SetTechnique is hooked too: the technique names encode the
 * feature bits (PointLights=N, ShadowType, Indoor...), so the per-technique
 * counts say how many shader lights the engine really uses today (plan
 * open question: nEffectLights).
 *
 * Everything here is observation plus the override; nothing changes how the
 * game renders unless an override file is present.
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include "panel.h"
#include "fxtable.h"
#include "../ref/minhook/include/MinHook.h"

#define FXN ((int)(sizeof g_fxtable / sizeof g_fxtable[0]))

/* ------------------------------------------------------------------ */
/* hooks on d3dx9_34: effect creation                                  */

typedef HRESULT (WINAPI *create_fx_fn)(IDirect3DDevice9 *, const void *, UINT,
                                       const D3DXMACRO *, ID3DXInclude *, DWORD,
                                       ID3DXEffectPool *, ID3DXEffect **, ID3DXBuffer **);
typedef HRESULT (WINAPI *create_fxex_fn)(IDirect3DDevice9 *, const void *, UINT,
                                         const D3DXMACRO *, ID3DXInclude *, LPCSTR, DWORD,
                                         ID3DXEffectPool *, ID3DXEffect **, ID3DXBuffer **);
typedef HRESULT (STDMETHODCALLTYPE *set_tech_fn)(ID3DXEffect *, D3DXHANDLE);

static create_fx_fn   g_orig_create;
static create_fxex_fn g_orig_create_ex;
static set_tech_fn    g_orig_set_tech;

#define MAX_EFFECTS 256
static struct {
    ID3DXEffect *fx; int table; unsigned int size, hash; int overridden, stock_max;
    int has_pl5;                       /* override carries our single-pass _pl5 techniques */
    LONG ugen;                         /* g_ultra_gen last written into this effect (ultra_apply) */
} g_effects[MAX_EFFECTS];
static unsigned int g_image;
/* The graphics features default ON (user, 2026-09-22); the panel turns each
 * off, and off is the stock look. */
static volatile LONG g_lights_on = 1;        /* per-pixel point lights */
/* Stock view (the comparison screenshot, src/compare.c): every setting reads
 * as stock while it is on; the settings themselves are left alone. */
static volatile LONG g_stock_view;
static volatile LONG g_n_lit, g_n_clamped;
/* Material knobs (plan step 8), all default to the stock look. Written into
 * each rebuilt effect from the SetTechnique hook, where the effect is known to
 * be alive (effects are recreated per level; a saved pointer may be stale). */
static volatile LONG g_fill_pct = 100;       /* shadow fill 0..100; 0 = stock */
static volatile LONG g_pcss_on = 1;
/* PCSS defaults tuned in game 2026-09-22 (sun sizes again after the
 * per-map normalisation: the near map had been 9x too sharp) */
static volatile LONG g_pcss_scale = 25;      /* outdoor: texels of blur per unit of light-space depth */
static volatile LONG g_pcss_scale_in = 10;  /* indoor materials: a smaller, nearer light */
static volatile LONG g_pcss_bias = 200;       /* millionths of light-space depth per texel of radius */
static volatile LONG g_shadow_dbg;          /* gvUltraMat.w: shadow-map debug view */
static volatile LONG g_pcss_min = 1;         /* texels: the softest a contact shadow gets */
static volatile LONG g_ultra_logged;
/* Look (gvUltraLook), percent deltas; 0 = stock. */
/* default: the 2007 fog and sun; the fill stock outdoors (the 2007 -60%
 * made outdoor shadows too harsh next to live building shadows) and up
 * indoors, where stock read dark (2026-09-23) */
static volatile LONG g_look_fill;            /* ambient + SH fill, % change */
static volatile LONG g_look_fill_in = 15;    /* the same for indoor materials */
static volatile LONG g_look_fog = 20;        /* fog start pushed this % of the way to the far end */
static volatile LONG g_look_sun = 20;        /* sun, % change */
/* Point lights in the base pass (gvUltraPL), on with g_lights_on. */
static volatile LONG g_pl_smooth = 1;        /* falloff: 0 stock linear, 1 windowed inverse-square */
static volatile LONG g_pl_pct = 100;         /* strength, percent of the engine's light colour */
static volatile LONG g_pl_spec = 1;          /* highlights from point lights */
/* Surfaces (gvUltraSurf): percent of stock; 100 = stock. */
static volatile LONG g_surf_gloss = 50;      /* highlight exponent */
static volatile LONG g_surf_spec = 75;       /* highlight strength */
static volatile LONG g_surf_env = 60;        /* cube-map reflection strength */
static volatile LONG g_surf_blur = 150;      /* reflection blur, mip levels x100 */
static volatile LONG g_surf_indoor;          /* also indoors: there the two specular
                                                lights carry the shape, so stock by default */
/* Texture filtering on material draws: anisotropy (1 = stock trilinear) and
 * a mip bias in hundredths (negative = sharper). */
static volatile LONG g_aniso = 16;
/* Normal-map detail on the level (gvUltraDetail), percent; bicubic light
 * maps (gvUltraLM, the texel size set per draw). */
static volatile LONG g_detail_sun = 70;
static volatile LONG g_detail_rest = 50;
static volatile LONG g_lm_bicubic = 1;
static volatile LONG g_mip_bias = -25;
int hg_gfx_shadow_type(void);
static volatile LONG g_ultra_gen = 1;        /* bumped on every change */
static volatile LONG g_ultra_writes;         /* effects that received the knobs (panel shows it) */
#define PCSS_MAX_RADIUS 16.0f                /* texels */
#define RVA_TECH_CACHE_GEN 0x006D3EA8u       /* DAT_00ad3ea8: mesh technique-cache generation */
static LONG g_last_override;    /* set by load_override, consumed by record_effect */
static volatile LONG g_neffects;
static LONG g_effects_unknown;
static LONG g_overrides;

/* technique usage: (effect, handle) -> count, name resolved on first sight */
#define MAX_TECH 1024
static struct { ID3DXEffect *fx; D3DXHANDLE h; volatile LONG n; char name[48]; } g_tech[MAX_TECH];
static volatile LONG g_ntech;
static LONG g_tech_overflow;
static CRITICAL_SECTION g_tech_cs;

static unsigned int fnv1a(const unsigned char *p, unsigned int n)
{
    unsigned int h = 0x811C9DC5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

static int table_lookup(unsigned int size, unsigned int hash)
{
    int i;
    for (i = 0; i < FXN; i++)
        if (g_fxtable[i].size == size && g_fxtable[i].hash == hash) return i;
    return -1;
}

static const char *fx_name(int table)
{
    const char *p, *s;
    if (table < 0) return "?";
    s = p = g_fxtable[table].path;
    while (*p) { if (*p == '\\') s = p + 1; p++; }
    return s;
}

/*
 * <game>\override\<pak path>. The DLL lives in <game>\bin, so the game root
 * is one level up. Returns a heap buffer the effect framework may keep
 * referring to; it is never freed, which for a few effect files is fine.
 */
static void *load_override(int table, unsigned int *size)
{
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD n, got = 0;
    void *buf;
    int len;

    if (table < 0) return NULL;
    /* The replacement effects contain every stock technique unchanged plus the
     * lit variants; the panel toggle (default OFF) decides at request time
     * which get used, so loading them is safe. bin\hellgate_override.off
     * skips them entirely, as the escape hatch. */
    if (hg_flagfile(L"hellgate_override.off")) return NULL;
    hg_dll_dir(path, MAX_PATH);
    lstrcatW(path, L"\\..\\override\\");
    len = lstrlenW(path);
    MultiByteToWideChar(CP_ACP, 0, g_fxtable[table].path, -1, path + len, MAX_PATH);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(h, NULL);
    buf = (n && n != INVALID_FILE_SIZE) ? HeapAlloc(GetProcessHeap(), 0, n) : NULL;
    if (buf && !ReadFile(h, buf, n, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!buf || got != n) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        hg_log("gfxprobe: override %ls unreadable (%lu of %lu bytes)", path, got, n);
        return NULL;
    }
    *size = n;
    InterlockedIncrement(&g_overrides);
    g_last_override = 1;
    hg_log("gfxprobe: OVERRIDE %s <- %ls (%lu bytes)", fx_name(table), path, n);
    return buf;
}

static void hook_set_technique(ID3DXEffect *fx);
static ID3DXEffect *g_ui_fx;    /* ui.fxo: SMAA runs before its first pass */
void postfx_before_ui(void);
void postfx_before_transparent(char why);
int postfx_wants_transparent_check(void);
void postfx_trace(char ev, long n);

/*
 * What kind of effect a pointer is, for the per-pass and per-draw hooks
 * (O(1): a small open-addressed table filled at creation). The scene's
 * passes run opaque -> skybox -> particles / alpha (the viewer renderer's
 * RPTYPE_* order), so AO runs at the first skybox or particle pass, or at
 * the first blended material draw, whichever comes first.
 */
enum { FXK_OTHER, FXK_MATERIAL, FXK_AFTER_OPAQUE, FXK_SHADOW };
#define FXK_SLOTS 512
static struct {
    ID3DXEffect *fx; int kind; D3DXHANDLE hlm; DWORD lmkey; D3DXHANDLE hsoft; int soft_looked;
    D3DXHANDLE hpl[3]; int pl_looked;           /* the point lights (plshadow_collect) */
    D3DXHANDLE hsoft_tech[5];                   /* particle.fxo: the techniques whose pass 0 runs our shaders */
    D3DXHANDLE hpart[8];                        /* particle.fxo: lit particles (part_bind) */
} g_fxk[FXK_SLOTS];

static unsigned fxk_hash(ID3DXEffect *fx) { return ((unsigned)(size_t)fx >> 4) * 2654435761u >> 23; }

static void fxk_set(ID3DXEffect *fx, int kind)
{
    unsigned i, h = fxk_hash(fx);
    for (i = 0; i < FXK_SLOTS; i++) {
        unsigned k = (h + i) & (FXK_SLOTS - 1);
        if (g_fxk[k].fx == fx || g_fxk[k].fx == NULL) {
            g_fxk[k].kind = kind; g_fxk[k].fx = fx;
            g_fxk[k].hlm = NULL; g_fxk[k].lmkey = 0xffffffffu;     /* a new effect at a reused address */
            g_fxk[k].hsoft = NULL; g_fxk[k].soft_looked = 0;
            g_fxk[k].pl_looked = 0;
            return;
        }
    }
}

static int fxk_get(ID3DXEffect *fx)
{
    unsigned i, h = fxk_hash(fx);
    for (i = 0; i < FXK_SLOTS; i++) {
        unsigned k = (h + i) & (FXK_SLOTS - 1);
        if (g_fxk[k].fx == fx) return g_fxk[k].kind;
        if (g_fxk[k].fx == NULL) return FXK_OTHER;
    }
    return FXK_OTHER;
}

static int fxk_slot(ID3DXEffect *fx)
{
    unsigned i, h = fxk_hash(fx);
    for (i = 0; i < FXK_SLOTS; i++) {
        unsigned k = (h + i) & (FXK_SLOTS - 1);
        if (g_fxk[k].fx == fx) return (int)k;
        if (g_fxk[k].fx == NULL) return -1;
    }
    return -1;
}

static int fxk_classify(int table)
{
    const char *n = fx_name(table);
    if (table < 0) return FXK_OTHER;
    if (!_strnicmp(n, "actor", 5) || !_strnicmp(n, "background", 10)) return FXK_MATERIAL;
    if (!lstrcmpiA(n, "skybox.fxo") || !_strnicmp(n, "particle", 8)) return FXK_AFTER_OPAQUE;
    if (!lstrcmpiA(n, "shadowmap.fxo")) return FXK_SHADOW;
    return FXK_OTHER;
}
static volatile int g_cur_kind;
/* Opaque material draws since the scene's depth was last cleared: AO only
 * makes sense after some (an early skybox or particle pass in a separate
 * scene saw empty depth and drew AO = 1 everywhere, first in-game run). */
static volatile LONG g_opaque_draws;
int gfxprobe_opaque_draws(void) { return (int)g_opaque_draws; }
IDirect3DSurface9 *device_depth_surface(void);
typedef HRESULT (STDMETHODCALLTYPE *pls_dip_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
void plshadow_dip(IDirect3DDevice9 *dev, pls_dip_fn draw, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc);
void plshadow_collect(ID3DXEffect *fx, D3DXHANDLE hp, D3DXHANDLE hc, D3DXHANDLE hf);
#include "volfog.h"
void plshadow_technique(int skinned);
void plshadow_rt_changed(void);
void plshadow_bind(IDirect3DDevice9 *dev);
void plshadow_frame(void);
LONG plshadow_params(float pls[4], float pls2[4]);
IDirect3DBaseTexture9 *plshadow_texture(void);

static void record_effect(ID3DXEffect *fx, int table, unsigned int size, unsigned int hash,
                          DWORD flags, ID3DXEffectPool *pool, HRESULT hr)
{
    LONG i = InterlockedIncrement(&g_neffects) - 1;
    if (i < MAX_EFFECTS) {
        LONG k;
        for (k = 0; k < i; k++)                /* a new effect at a reused address */
            if (g_effects[k].fx == fx) g_effects[k].ugen = 0;
        g_effects[i].fx = fx; g_effects[i].table = table;
        g_effects[i].size = size; g_effects[i].hash = hash;
        g_effects[i].overridden = (int)InterlockedExchange(&g_last_override, 0);
    }
    /* Highest PointLights among the STOCK techniques (names without our
     * _plN suffix): what "lights off" clamps requests to. */
    if (i < MAX_EFFECTS && g_effects[i].overridden && fx && SUCCEEDED(hr)) {
        D3DXEFFECT_DESC ed;
        UINT t;
        int mx = 0;
        if (SUCCEEDED(fx->lpVtbl->GetDesc(fx, &ed)))
            for (t = 0; t < ed.Techniques; t++) {
                D3DXHANDLE h = fx->lpVtbl->GetTechnique(fx, t), a;
                D3DXTECHNIQUE_DESC td;
                INT v = 0;
                if (!h || FAILED(fx->lpVtbl->GetTechniqueDesc(fx, h, &td)) || !td.Name) continue;
                if (strstr(td.Name, "_pl5")) { g_effects[i].has_pl5 = 1; continue; }
                a = fx->lpVtbl->GetAnnotationByName(fx, h, "PointLights");
                if (a && SUCCEEDED(fx->lpVtbl->GetInt(fx, a, &v)) && v > mx) mx = v;
            }
        g_effects[i].stock_max = mx;
    }
    if (table >= 0 && fx && SUCCEEDED(hr) && !lstrcmpiA(fx_name(table), "ui.fxo")) g_ui_fx = fx;
    if (fx && SUCCEEDED(hr)) fxk_set(fx, fxk_classify(table));
    if (table < 0) InterlockedIncrement(&g_effects_unknown);
    hg_log("gfxprobe: effect #%ld %s (%u bytes fnv 0x%08x flags 0x%lx pool %p) -> %p hr=0x%08lx",
           i, table >= 0 ? g_fxtable[table].path : "UNKNOWN", size, hash,
           (unsigned long)flags, (void *)pool, (void *)fx, (unsigned long)hr);
    if (fx && SUCCEEDED(hr)) hook_set_technique(fx);
}

static HRESULT WINAPI detour_create(IDirect3DDevice9 *dev, const void *src, UINT len,
                                    const D3DXMACRO *defs, ID3DXInclude *inc, DWORD flags,
                                    ID3DXEffectPool *pool, ID3DXEffect **out, ID3DXBuffer **err)
{
    unsigned int hash = src ? fnv1a((const unsigned char *)src, len) : 0;
    int table = table_lookup(len, hash);
    unsigned int osize = 0;
    void *ovr = load_override(table, &osize);
    ID3DXEffect *fx = NULL;
    HRESULT hr = g_orig_create(dev, ovr ? ovr : src, ovr ? osize : len, defs, inc, flags, pool,
                               out ? out : &fx, err);
    record_effect(out ? *out : fx, table, len, hash, flags, pool, hr);
    return hr;
}

static HRESULT WINAPI detour_create_ex(IDirect3DDevice9 *dev, const void *src, UINT len,
                                       const D3DXMACRO *defs, ID3DXInclude *inc, LPCSTR skip,
                                       DWORD flags, ID3DXEffectPool *pool, ID3DXEffect **out,
                                       ID3DXBuffer **err)
{
    unsigned int hash = src ? fnv1a((const unsigned char *)src, len) : 0;
    int table = table_lookup(len, hash);
    unsigned int osize = 0;
    void *ovr = load_override(table, &osize);
    ID3DXEffect *fx = NULL;
    HRESULT hr = g_orig_create_ex(dev, ovr ? ovr : src, ovr ? osize : len, defs, inc, skip,
                                  flags, pool, out ? out : &fx, err);
    record_effect(out ? *out : fx, table, len, hash, flags, pool, hr);
    return hr;
}

/* ------------------------------------------------------------------ */
/* ID3DXEffect::SetTechnique                                           */

/*
 * Write the material knobs into fx if the panel changed them since. Every
 * effect has two records at the same address (D3DXCreateEffect calls the
 * hooked ...Ex inside, and only the inner record is marked overridden), and
 * a freed effect's address can be reused by the next level's; so the
 * handles are looked up by name on the live effect, which also leaves the
 * stock effects (no such parameters) alone. record_effect clears ugen for a
 * reused address.
 */
/* Both outdoor shadow maps, every 5 s: world units covered per map (from
 * its matrix: uv and depth per world unit) and the texture sizes, to tell
 * which map carries what and how coarse each is. */
static float mcol_len(const D3DXMATRIX *m, int c)
{
    return sqrtf(m->m[0][c] * m->m[0][c] + m->m[1][c] * m->m[1][c] + m->m[2][c] * m->m[2][c]);
}

static void tex_size(ID3DXEffect *fx, const char *name, char *out, int n)
{
    D3DXHANDLE h = fx->lpVtbl->GetParameterByName(fx, NULL, name);
    IDirect3DBaseTexture9 *t = NULL;
    lstrcpynA(out, "-", n);
    if (h && SUCCEEDED(fx->lpVtbl->GetTexture(fx, h, &t)) && t) {
        if (t->lpVtbl->GetType(t) == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(((IDirect3DTexture9 *)t)->lpVtbl->GetLevelDesc((IDirect3DTexture9 *)t, 0, &d)))
                wsprintfA(out, "%ux%u fmt %d", d.Width, d.Height, (int)d.Format);
        }
        t->lpVtbl->Release(t);
    }
}

/* Panel button: write both shadow maps (R32F depth) to bin\shadow_<param>.pgm,
 * depth stretched over the range in use, 1.0 (nothing drawn) white. */
static volatile LONG g_smdump_req;

static void dump_shadow_tex(ID3DXEffect *fx, const char *param)
{
    D3DXHANDLE h = fx->lpVtbl->GetParameterByName(fx, NULL, param);
    IDirect3DBaseTexture9 *bt = NULL;
    IDirect3DSurface9 *rt = NULL, *sys = NULL;
    IDirect3DDevice9 *dev = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (!h || FAILED(fx->lpVtbl->GetTexture(fx, h, &bt)) || !bt) { hg_log("gfxprobe: dump %s: no texture", param); return; }
    if (bt->lpVtbl->GetType(bt) == D3DRTYPE_TEXTURE
        && SUCCEEDED(((IDirect3DTexture9 *)bt)->lpVtbl->GetSurfaceLevel((IDirect3DTexture9 *)bt, 0, &rt))
        && SUCCEEDED(rt->lpVtbl->GetDesc(rt, &d)) && d.Format == D3DFMT_R32F
        && SUCCEEDED(rt->lpVtbl->GetDevice(rt, &dev))
        && SUCCEEDED(dev->lpVtbl->CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))
        && SUCCEEDED(dev->lpVtbl->GetRenderTargetData(dev, rt, sys))
        && SUCCEEDED(sys->lpVtbl->LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        float lo = 1e30f, hi = -1e30f;
        unsigned x, y;
        long drawn = 0;
        WCHAR path[MAX_PATH], wp[64];
        HANDLE f;
        for (y = 0; y < d.Height; y++) {
            const float *row = (const float *)((const char *)lr.pBits + y * lr.Pitch);
            for (x = 0; x < d.Width; x++)
                if (row[x] < 0.9999f) { drawn++; if (row[x] < lo) lo = row[x]; if (row[x] > hi) hi = row[x]; }
        }
        if (hi <= lo) hi = lo + 1e-6f;
        hg_dll_dir(path, MAX_PATH);
        wsprintfW(wp, L"\\shadow_%S.pgm", param);
        lstrcatW(path, wp);
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char hdr[64];
            unsigned char *line = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, d.Width);
            DWORD n;
            int hl = wsprintfA(hdr, "P5 %u %u 255\n", d.Width, d.Height);
            WriteFile(f, hdr, hl, &n, NULL);
            for (y = 0; line && y < d.Height; y++) {
                const float *row = (const float *)((const char *)lr.pBits + y * lr.Pitch);
                for (x = 0; x < d.Width; x++)
                    line[x] = row[x] >= 0.9999f ? 255 : (unsigned char)(230.0f * (row[x] - lo) / (hi - lo));
                WriteFile(f, line, d.Width, &n, NULL);
            }
            if (line) HeapFree(GetProcessHeap(), 0, line);
            CloseHandle(f);
        }
        sys->lpVtbl->UnlockRect(sys);
        hg_log("gfxprobe: dumped %s %ux%u: %ld texels drawn (%.1f%%), depth %.5f..%.5f",
               param, d.Width, d.Height, drawn, 100.0 * drawn / ((double)d.Width * d.Height), lo, hi);
    } else {
        hg_log("gfxprobe: dump %s failed", param);
    }
    if (sys) sys->lpVtbl->Release(sys);
    if (dev) dev->lpVtbl->Release(dev);
    if (rt) rt->lpVtbl->Release(rt);
    bt->lpVtbl->Release(bt);
}

/*
 * Near shadow map reach. dxC_ShadowBufferSetupDirectional (0x7e3d05) sizes
 * the near outdoor map (the one every character and prop casts into) from
 * 0xa817f4 = 27.0 world units, centred ahead of the camera; past it, no
 * dynamic shadows. Two other instructions read the same constant, so only
 * this read is repointed.
 */
#define RVA_SHADOW_NEAR_INSN 0x003E3D6Cu   /* movss xmm0,[0xa817f4] */

/* The near shadow map's width in world units (stock 27), read by the
 * engine through the operand patch_shadow_reach repoints. */
static volatile float g_shadow_reach = 27.0f;
static int g_reach_patched;

static void patch_shadow_reach(unsigned int image)
{
    unsigned char *ins = (unsigned char *)(image + RVA_SHADOW_NEAR_INSN);
    static const unsigned char want[8] = { 0xf3, 0x0f, 0x10, 0x05, 0xf4, 0x17, 0xa8, 0x00 };
    unsigned int addr = (unsigned int)&g_shadow_reach;
    DWORD old;
    if (IsBadReadPtr(ins, 8) || memcmp(ins, want, sizeof want) != 0) {
        hg_log("gfxprobe: shadow reach NOT patched -- bytes at %p are not movss xmm0,[0xa817f4]", (void *)ins);
        return;
    }
    if (!VirtualProtect(ins + 4, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(ins + 4, &addr, 4);
    VirtualProtect(ins + 4, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), ins, 8);
    g_reach_patched = 1;
    hg_log("gfxprobe: near shadow map reach patched (%.0f units)", g_shadow_reach);
}

/* d: units; 0 resets to stock */
void hg_gfx_nudge_reach(int d)
{
    float v = d ? g_shadow_reach + (float)d : 27.0f;
    if (v < 15.0f) v = 15.0f;
    if (v > 150.0f) v = 150.0f;
    g_shadow_reach = v;
    hg_log("gfxprobe: near shadow map reach %.0f units (%.1f texels per unit)", v, 2048.0f / v);
}

int hg_gfx_reach(void) { return g_reach_patched ? (int)(g_shadow_reach + 0.5f) : 0; }

/*
 * The fine outdoor shadow map, per pixel. sSetGeneralMeshParameters gives
 * each mesh ONE wide map: the default, zone-wide buffer (DAT_00bb08ec), or
 * a fine 80-unit one when the mesh's bounds fit inside it. The two hold
 * different shadows and are redrawn rarely, so neighbouring meshes that
 * chose differently met in straight seams (debug view, 2026-09-22).
 *
 * dx9_SetShadowMapParameters (0x7e4930; cdecl: engine effect, technique,
 * buffer index, world, view, projection) is hooked. With the toggle on,
 * for a mesh on either wide map it runs twice: first for the fine buffer
 * with an identity world, which leaves the fine map's world-space matrix
 * and texture in the effect (copied to gmUltraFine, and the texture bound
 * to sampler 12), then for the zone-wide buffer with the real world, so
 * every mesh's own map is the same. background.hlsl picks per pixel.
 */
#define RVA_SET_SHADOW_PARAMS   0x003E4930u
#define RVA_SHADOW_BUF_DEFAULT  0x007B08ECu   /* DAT_00bb08ec: default (zone-wide) buffer */
#define RVA_SHADOW_BUF_ARRAY    0x007B08E4u   /* DAT_00bb08e4: buffers, 400 bytes each */
#define RVA_SHADOW_BUF_COUNT    0x007B08F4u   /* DAT_00bb08f4 */
typedef int (__cdecl *set_shadow_params_fn)(void *, void *, int, void *, void *, void *);
static set_shadow_params_fn g_orig_ssmp;
static volatile LONG g_cascade = 1;         /* fine outdoor shadow map per pixel (proven in game 2026-09-22) */
static volatile LONG g_cascade_calls, g_cascade_bad;
/* sampler 12 holds the fine map from dx9_SetShadowMapParameters until the
 * pass ends; left bound, the engine would later render INTO that texture
 * while it is still bound as one (undefined in D3D9) */
static int g_fine_bound;
static IDirect3DBaseTexture9 *g_s12_prev;   /* what the engine had in sampler 12, restored at EndPass */

/* Characters read the near map too (gvUltraAct): default on. */
static volatile LONG g_act_near = 1;
static volatile LONG g_near_ok, g_near_bad;
static volatile LONG g_act_st[4];            /* character technique requests by ShadowType 0/1/2/other */
static volatile LONG g_act_st_up;             /* ... of which raised from 0 to 2 */
/* The engine is drawing shadow maps (the shadow pass ran within the last
 * 30 frames): only then do characters get the shadow technique. Menus and
 * character select have no shadow maps, and the technique read garbage. */
static volatile LONG g_shadows_live;
static volatile LONG g_act_offset = 60;        /* normal offset, thousandths of a world unit */
static volatile LONG g_bg_offset = 40;         /* the same for the level and props (gvUltraAct.z) */

/* finite and not all zero */
static int matrix_ok(const D3DXMATRIX *m)
{
    float sum = 0;
    int k;
    for (k = 0; k < 16; k++) {
        float v = ((const float *)m)[k];
        if (v != v || v > 1e20f || v < -1e20f) return 0;
        sum += v < 0 ? -v : v;
    }
    return sum != 0;
}

void hg_gfx_set_act_near(int on)
{
    int *gen = (int *)(g_image + RVA_TECH_CACHE_GEN);
    InterlockedExchange(&g_act_near, on ? 1 : 0);
    /* the ShadowType request changes with it: re-pick every mesh's technique */
    if (!IsBadWritePtr(gen, 4)) InterlockedIncrement((volatile LONG *)gen);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: characters read the near shadow map %s (offset %ld/1000)", on ? "ON" : "off", g_act_offset);
}
int hg_gfx_act_near(void) { return (int)g_act_near; }
void hg_gfx_nudge_act_offset(int d)
{
    LONG v = g_act_offset + d;
    InterlockedExchange(&g_act_offset, v < 0 ? 0 : v > 300 ? 300 : v);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: character shadow normal offset %ld/1000 units", g_act_offset);
}
int hg_gfx_act_offset(void) { return (int)g_act_offset; }
void hg_gfx_nudge_bg_offset(int d)
{
    LONG v = g_bg_offset + d;
    InterlockedExchange(&g_bg_offset, v < 0 ? 0 : v > 300 ? 300 : v);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: level shadow normal offset %ld/1000 units", g_bg_offset);
}
int hg_gfx_bg_offset(void) { return (int)g_bg_offset; }

/* the fine buffer: a wide one (not the near map, flag 0x20) other than the default */
static int fine_buffer(int def)
{
    const unsigned char *arr = *(unsigned char **)(g_image + RVA_SHADOW_BUF_ARRAY);
    int n = *(int *)(g_image + RVA_SHADOW_BUF_COUNT), k;
    if (!arr || n <= 0 || n > 16 || IsBadReadPtr(arr, (UINT_PTR)n * 400)) return -1;
    for (k = 0; k < n; k++)
        if (k != def && !(arr[k * 400] & 0x20)) return k;
    return -1;
}

/* The camera's projection as the engine last passed it here (per shadowed
 * mesh, so every scene frame): src/postfx.c linearises scene depth with it. */
static float g_cam_proj[16];
static volatile LONG g_cam_proj_ok;
static volatile DWORD g_cam_proj_ms;

/* Only a fresh one: the last shadowed mesh drawn in the past 100 ms. */
int gfxprobe_camera_proj(float *m)
{
    if (!g_cam_proj_ok || GetTickCount() - g_cam_proj_ms > 100) return 0;
    memcpy(m, g_cam_proj, sizeof g_cam_proj);
    return 1;
}

static int __cdecl detour_ssmp(void *efx, void *tech, int buf, void *world, void *view, void *proj)
{
    static const D3DXMATRIX ident = {{{ 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }}};
    ID3DXEffect *fx;
    D3DXHANDLE hm, hu, ht;
    int def, fine, r;
    if (proj && !IsBadReadPtr(proj, 64)) {
        memcpy(g_cam_proj, proj, sizeof g_cam_proj);
        g_cam_proj_ms = GetTickCount();
        g_cam_proj_ok = 1;
    }
    if (view && !IsBadReadPtr(view, 64) && !g_stock_view) volfog_view((const float *)view);
    if (g_stock_view || (!g_cascade && !g_act_near) || !efx || IsBadReadPtr((char *)efx + 0x118, 4))
        return g_orig_ssmp(efx, tech, buf, world, view, proj);
    fx = *(ID3DXEffect **)((char *)efx + 0x118);
    /* characters: the near map's world-space matrix, from a first run with
     * an identity world (their shader reads the near map with it) */
    if (fx && g_act_near) {
        D3DXHANDLE hn = fx->lpVtbl->GetParameterByName(fx, NULL, "gmUltraNear");
        D3DXHANDLE h2 = hn ? fx->lpVtbl->GetParameterByName(fx, NULL, "gmShadowMatrix2") : NULL;
        if (hn && h2) {
            D3DXMATRIX m;
            if (g_orig_ssmp(efx, tech, buf, (void *)&ident, view, proj) >= 0 &&
                SUCCEEDED(fx->lpVtbl->GetMatrix(fx, h2, &m)) && matrix_ok(&m)) {
                fx->lpVtbl->SetMatrix(fx, hn, &m);
                InterlockedIncrement(&g_near_ok);
            } else {
                InterlockedIncrement(&g_near_bad);
                static const D3DXMATRIX zero = {{{ 0 }}};
                fx->lpVtbl->SetMatrix(fx, hn, &zero);          /* the shader skips it */
            }
            return g_orig_ssmp(efx, tech, buf, world, view, proj);
        }
    }
    def = *(int *)(g_image + RVA_SHADOW_BUF_DEFAULT);
    fine = def >= 0 ? fine_buffer(def) : -1;
    if (!g_cascade || fine < 0 || (buf != def && buf != fine) || !fx)
        return g_orig_ssmp(efx, tech, buf, world, view, proj);
    hu = fx->lpVtbl->GetParameterByName(fx, NULL, "gmUltraFine");
    hm = fx->lpVtbl->GetParameterByName(fx, NULL, "gmShadowMatrix");
    ht = fx->lpVtbl->GetParameterByName(fx, NULL, "tShadowMap");
    if (!hu || !hm || !ht)
        return g_orig_ssmp(efx, tech, buf, world, view, proj);
    /* 1. the fine map, in world space */
    if (g_orig_ssmp(efx, tech, fine, (void *)&ident, view, proj) >= 0) {
        D3DXMATRIX m;
        IDirect3DBaseTexture9 *t = NULL;
        int ok = SUCCEEDED(fx->lpVtbl->GetMatrix(fx, hm, &m)) && matrix_ok(&m);
        if (!ok) {
            static const D3DXMATRIX zero = {{{ 0 }}};
            fx->lpVtbl->SetMatrix(fx, hu, &zero);     /* the shader skips the fine map */
            InterlockedIncrement(&g_cascade_bad);
            return g_orig_ssmp(efx, tech, def, world, view, proj);
        }
        fx->lpVtbl->SetMatrix(fx, hu, &m);
        if (SUCCEEDED(fx->lpVtbl->GetTexture(fx, ht, &t)) && t) {
            volfog_maps(fx, (const float *)&m, t);      /* before run 2 makes them per mesh */
            IDirect3DDevice9 *dev = NULL;
            if (SUCCEEDED(t->lpVtbl->GetDevice(t, &dev)) && dev) {
                /* keep the engine's own sampler-12 texture (first build cleared
                 * it at EndPass and the lava's glow went with it) */
                if (!g_fine_bound) {
                    if (g_s12_prev) { g_s12_prev->lpVtbl->Release(g_s12_prev); g_s12_prev = NULL; }
                    dev->lpVtbl->GetTexture(dev, 12, &g_s12_prev);
                }
                dev->lpVtbl->SetTexture(dev, 12, t);
                dev->lpVtbl->SetSamplerState(dev, 12, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                dev->lpVtbl->SetSamplerState(dev, 12, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                dev->lpVtbl->SetSamplerState(dev, 12, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                dev->lpVtbl->SetSamplerState(dev, 12, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                dev->lpVtbl->SetSamplerState(dev, 12, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                g_fine_bound = 1;
                {
                    /* and through the effect (UltraFineSampler is a real
                     * parameter: an unparameterised sampler crashed D3DX) */
                    D3DXHANDLE hft = fx->lpVtbl->GetParameterByName(fx, NULL, "tUltraFine");
                    if (hft) fx->lpVtbl->SetTexture(fx, hft, t);
                }
                dev->lpVtbl->Release(dev);
            }
            t->lpVtbl->Release(t);
        }
    }
    /* 2. the mesh's own map is always the zone-wide one */
    r = g_orig_ssmp(efx, tech, def, world, view, proj);
    InterlockedIncrement(&g_cascade_calls);
    return r;
}

/*
 * The wide maps go stale. After drawing a shadow buffer the engine clears
 * its dirty bit (flags & 1) unless the buffer is always-dirty (flags &
 * 0x10, 0x7ca489): the near map has 0x10, the two wide ones do not, and
 * were redrawn once in 1,200 frames (trace, 2026-09-22) -- so the
 * zone-wide map kept whatever casters it had at load and shadows popped
 * as the fine map's square moved over them. With the fine map per pixel
 * on, mark both wide buffers dirty every g_wide_ms milliseconds.
 *
 * The 80-unit map (flags 0x80) is centred on the camera when drawn, snapped
 * to its texels, so on the clock alone a run carried the camera tens of
 * units off its centre, onto the coarse zone-wide map, until the next
 * redraw moved it. It is also redrawn once the camera is g_fine_follow
 * units from where it was last drawn: it follows the camera like a cascade.
 * Snapped, a redraw moves only its edge; the shadows inside stay put.
 */
static volatile LONG g_wide_ms = 5000;          /* 0.2 Hz, by the clock: independent of frame rate */
static volatile LONG g_fine_follow = 8;         /* units; 0 = the clock only */
static volatile LONG g_fine_follows;            /* redraws for the camera, for the log */
static void wide_refresh(void)
{
    static DWORD last;
    static float at[3];
    static int have_at;
    DWORD now = GetTickCount();
    unsigned char *arr;
    int cnt, k, clock, follow = 0;
    const volfog_state *v;
    LONG fr;
    if (!g_cascade || g_stock_view || !g_image) return;
    clock = now - last >= (DWORD)g_wide_ms;
    v = volfog_get(&fr);
    if (g_fine_follow > 0 && fr - v->cam_frame <= 2) {
        float dx = v->eye[0] - at[0], dy = v->eye[1] - at[1], dz = v->eye[2] - at[2];
        follow = !have_at || dx * dx + dy * dy + dz * dz > (float)(g_fine_follow * g_fine_follow);
    }
    if (!clock && !follow) return;
    arr = *(unsigned char **)(g_image + RVA_SHADOW_BUF_ARRAY);
    cnt = *(int *)(g_image + RVA_SHADOW_BUF_COUNT);
    if (!arr || cnt <= 0 || cnt > 16 || IsBadWritePtr(arr, (UINT_PTR)cnt * 400)) return;
    if (clock) last = now;
    if (follow || clock) {
        at[0] = v->eye[0]; at[1] = v->eye[1]; at[2] = v->eye[2];
        have_at = 1;
    }
    if (!clock && InterlockedIncrement(&g_fine_follows) <= 3)
        hg_log("gfxprobe: fine shadow map redrawn for the camera at %.1f %.1f %.1f", at[0], at[1], at[2]);
    for (k = 0; k < cnt; k++) {
        unsigned int *flags = (unsigned int *)(arr + k * 400);
        if (*flags & 0x20) continue;            /* wide buffers only: the near map is always redrawn */
        if (clock || (*flags & 0x80)) *flags |= 1;
    }
}

/* d in units */
void hg_gfx_nudge_fine_follow(int d)
{
    LONG v = g_fine_follow + d;
    InterlockedExchange(&g_fine_follow, v < 0 ? 0 : v > 40 ? 40 : v);
    hg_log("gfxprobe: fine shadow map follows the camera every %ld units (0: clock only)", g_fine_follow);
}
int hg_gfx_fine_follow(void) { return (int)g_fine_follow; }

void hg_gfx_knobs_changed(void) { InterlockedIncrement(&g_ultra_gen); }

/* d in tenths of a second */
void hg_gfx_nudge_wide_every(int d)
{
    LONG v = g_wide_ms + d * 100;
    InterlockedExchange(&g_wide_ms, v < 200 ? 200 : v > 60000 ? 60000 : v);
    hg_log("gfxprobe: wide shadow maps redrawn every %ld ms", g_wide_ms);
}
int hg_gfx_wide_every(void) { return (int)g_wide_ms; }       /* ms */

static void hook_ssmp(unsigned int image)
{
    static const unsigned char want[6] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0 };
    unsigned char *p = (unsigned char *)(image + RVA_SET_SHADOW_PARAMS);
    if (!IsBadReadPtr(p, 6) && memcmp(p, want, 6) == 0 &&
        MH_CreateHook(p, (void *)detour_ssmp, (void **)&g_orig_ssmp) == MH_OK && MH_EnableHook(p) == MH_OK)
        hg_log("gfxprobe: hooked dx9_SetShadowMapParameters (fine shadow map per pixel)");
    else
        hg_log("gfxprobe: dx9_SetShadowMapParameters NOT hooked (bytes differ)");
}

void hg_gfx_set_fine_map(int on)
{
    InterlockedExchange(&g_cascade, on ? 1 : 0);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: fine shadow map per pixel %s (default buffer %d, fine %d, calls so far %ld, bad matrices %ld)", on ? "ON" : "off",
           *(int *)(g_image + RVA_SHADOW_BUF_DEFAULT), fine_buffer(*(int *)(g_image + RVA_SHADOW_BUF_DEFAULT)),
           g_cascade_calls, g_cascade_bad);
}
int hg_gfx_fine_map(void) { return (int)g_cascade; }

/*
 * Static objects in the near shadow map, outdoors. The shadow-map pass
 * (FUN_007c9d5a) keeps outdoor static models (model bit 5 clear) out of the
 * near map with one branch:
 *     7ca3ea test bl,bl     ; static?
 *     7ca3ee test edi,edi   ; indoors?  (edi = 1 indoors)
 *     7ca3f0 je reject      ; outdoors: static models never cast here
 *     7ca3f2 push 12h / pop edx / call ModelTestFlagbit / je reject
 * so trees, posts and props had only their baked shadows, which do not
 * match the live ones, and cast nothing on characters. A later check
 * (0x7ca424) still requires the material's CastShadow bit (0x12) outdoors.
 *   mode 1, props: edx = 0x15 - 3*edi, i.e. outdoors test bit 21
 *     (MODEL_FLAGBIT_DISTANCE_CULLABLE, which layout props get), indoors
 *     bit 0x12 as before: 6b d7 fd (imul edx,edi,-3) 83 c2 15 (add edx,15h) 90
 *   mode 2, all: the je becomes two nops; buildings and room shells too.
 */
#define RVA_STATIC_CASTER_GATE 0x003CA3EEu
static const unsigned char k_gate_stock[7] = { 0x85, 0xff, 0x74, 0x5d, 0x6a, 0x12, 0x5a };
static const unsigned char k_gate_props[7] = { 0x6b, 0xd7, 0xfd, 0x83, 0xc2, 0x15, 0x90 };
static const unsigned char k_gate_all[7]   = { 0x85, 0xff, 0x90, 0x90, 0x6a, 0x12, 0x5a };
static volatile LONG g_static_casters;       /* 0 stock, 1 props, 2 all */

void hg_gfx_set_static_casters(int mode)
{
    unsigned char *p = (unsigned char *)(g_image + RVA_STATIC_CASTER_GATE);
    const unsigned char *want = mode == 1 ? k_gate_props : mode == 2 ? k_gate_all : k_gate_stock;
    DWORD old;
    if (mode < 0 || mode > 2 || IsBadReadPtr(p, 7)) return;
    if (memcmp(p, k_gate_stock, 7) && memcmp(p, k_gate_props, 7) && memcmp(p, k_gate_all, 7)) {
        hg_log("gfxprobe: static casters NOT patched -- unexpected bytes at %p", (void *)p);
        return;
    }
    if (!VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(p, want, 7);
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    InterlockedExchange(&g_static_casters, mode);
    hg_log("gfxprobe: static objects in the near shadow map: %s", mode == 1 ? "props" : mode == 2 ? "all" : "off (stock)");
}
int hg_gfx_static_casters(void) { return (int)g_static_casters; }

/*
 * Stable casters. The shadow pass (FUN_007c9d5a) takes each buffer's
 * casters from the model proximity map: models whose ORIGIN lies within the
 * buffer's radius x 1.2 of its centre (0x7ca23b loads the 1.2 for the colour
 * shadow map, DAT_00a81b48). A building's origin is at a corner, far from
 * most of its walls, so as the near map followed the player whole shadows
 * popped in and out when an origin crossed that radius. It then rejects a
 * model whose fade alpha (model+0x2f0, the one the camera fades walls in
 * front of the player with) is below 0.5 (0x7ca381 comiss, 0x7ca390 ja):
 * a wall faded for the view lost its shadow too.
 *   on: the scale operand points at g_caster_scale (3), and the ja is
 *   six nops.
 */
#define RVA_CASTER_SCALE_INSN 0x003CA23Bu     /* movss xmm0,[0xa81b48] */
#define RVA_CASTER_FADE_JA    0x003CA390u     /* ja reject (faded below 0.5) */
static const unsigned char k_scale_stock[8] = { 0xf3, 0x0f, 0x10, 0x05, 0x48, 0x1b, 0xa8, 0x00 };
static const unsigned char k_fade_stock[6] = { 0x0f, 0x87, 0xb9, 0x00, 0x00, 0x00 };
static const unsigned char k_fade_off[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
static volatile float g_caster_scale = 3.0f;
static volatile LONG g_stable_casters;

void hg_gfx_set_stable_casters(int on)
{
    unsigned char *si = (unsigned char *)(g_image + RVA_CASTER_SCALE_INSN);
    unsigned char *fj = (unsigned char *)(g_image + RVA_CASTER_FADE_JA);
    unsigned char want[8];
    unsigned int addr = (unsigned int)&g_caster_scale;
    DWORD old;
    if (!g_image || IsBadReadPtr(si, 8) || IsBadReadPtr(fj, 6)) return;
    memcpy(want, k_scale_stock, 4);
    memcpy(want + 4, &addr, 4);
    if ((memcmp(si, k_scale_stock, 8) && memcmp(si, want, 8)) ||
        (memcmp(fj, k_fade_stock, 6) && memcmp(fj, k_fade_off, 6))) {
        hg_log("gfxprobe: stable casters NOT patched -- unexpected bytes at %p / %p", (void *)si, (void *)fj);
        return;
    }
    if (VirtualProtect(si, 8, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(si, on ? want : k_scale_stock, 8);
        VirtualProtect(si, 8, old, &old);
    }
    if (VirtualProtect(fj, 6, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(fj, on ? k_fade_off : k_fade_stock, 6);
        VirtualProtect(fj, 6, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), si, 0x160);
    InterlockedExchange(&g_stable_casters, on ? 1 : 0);
    hg_log("gfxprobe: stable casters %s (search radius x%.1f, faded walls cast)", on ? "ON" : "off (stock: x1.2, faded walls do not)",
           on ? g_caster_scale : 1.2f);
}
int hg_gfx_stable_casters(void) { return (int)g_stable_casters; }

/* Stock view on/off: the knobs, the technique requests, the fine and near
 * map binding and the engine patches all go to stock and come back; the
 * next frames pick it up (technique caches regenerated). */
void hg_gfx_stock_view(int on)
{
    static LONG casters, stable;
    static float reach;
    int *gen = (int *)(g_image + RVA_TECH_CACHE_GEN);
    on = on ? 1 : 0;
    if (on == (int)g_stock_view) return;
    if (on) {
        casters = g_static_casters;
        stable = g_stable_casters;
        reach = g_shadow_reach;
        InterlockedExchange(&g_stock_view, 1);
        hg_gfx_set_static_casters(0);
        hg_gfx_set_stable_casters(0);
        g_shadow_reach = 27.0f;
    } else {
        InterlockedExchange(&g_stock_view, 0);
        hg_gfx_set_static_casters((int)casters);
        hg_gfx_set_stable_casters((int)stable);
        g_shadow_reach = reach;
    }
    InterlockedIncrement(&g_ultra_gen);
    if (g_image && !IsBadWritePtr(gen, 4)) InterlockedIncrement((volatile LONG *)gen);
}
int hg_gfx_stock_viewing(void) { return (int)g_stock_view; }

/* From src/device.c at Present: the point-light shadow's light for the next
 * frame; a new light (or its reach) rewrites the knobs in every effect. */
void gfxprobe_present(void)
{
    static LONG last;
    float a[4], b[4];
    LONG g;
    plshadow_frame();
    volfog_present();
    g = plshadow_params(a, b);
    if (g != last) { last = g; InterlockedIncrement(&g_ultra_gen); }
}
float gfxprobe_near_reach(void) { return g_shadow_reach; }

void hg_gfx_set_shadow_debug(int on)
{
    InterlockedExchange(&g_shadow_dbg, on ? 1 : 0);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: shadow map debug view %s", on ? "ON" : "off");
}
int hg_gfx_shadow_debug(void) { return (int)g_shadow_dbg; }

void hg_gfx_dump_shadowmaps(void)
{
    InterlockedExchange(&g_smdump_req, 1);
}

static void shadow_diag(ID3DXEffect *fx)
{
    static DWORD last;
    static LONG n;
    DWORD now = GetTickCount();
    D3DXHANDLE h1, h2;
    D3DXMATRIX m1, m2;
    char t1[48], t2[48];
    if (n >= 200 && !g_smdump_req) return;
    if (now - last < 5000 && !g_smdump_req) return;
    h1 = fx->lpVtbl->GetParameterByName(fx, NULL, "gmShadowMatrix");
    h2 = fx->lpVtbl->GetParameterByName(fx, NULL, "gmShadowMatrix2");
    if (!h1 || !h2 || FAILED(fx->lpVtbl->GetMatrix(fx, h1, &m1)) || FAILED(fx->lpVtbl->GetMatrix(fx, h2, &m2)))
        return;
    if (mcol_len(&m1, 0) == 0 || mcol_len(&m2, 0) == 0) return;
    if (InterlockedExchange(&g_smdump_req, 0)) {
        dump_shadow_tex(fx, "tShadowMap");
        dump_shadow_tex(fx, "tShadowMapDepth");
        last = 0;
    }
    if (now - last < 5000) return;
    last = now;
    n++;
    tex_size(fx, "tShadowMap", t1, sizeof t1);
    tex_size(fx, "tShadowMapDepth", t2, sizeof t2);
    /* 1/uv-per-unit = world units across the whole map */
    hg_log("gfxprobe: shadow maps: main %.1f x %.1f units, depth %.4f/unit | second %.1f x %.1f units, depth %.4f/unit | tShadowMap %s  tShadowMapDepth %s",
           1.0f / mcol_len(&m1, 0), 1.0f / mcol_len(&m1, 1), mcol_len(&m1, 2),
           1.0f / mcol_len(&m2, 0), 1.0f / mcol_len(&m2, 1), mcol_len(&m2, 2), t1, t2);
}

int hdr_unclamped(void);

static void ultra_apply(ID3DXEffect *fx)
{
    shadow_diag(fx);
    LONG e, ne = g_neffects < MAX_EFFECTS ? g_neffects : MAX_EFFECTS, gen = g_ultra_gen;
    D3DXHANDLE hm, hs;
    int indoor = 0;
    for (e = ne - 1; e >= 0; e--)
        if (g_effects[e].fx == fx) break;
    if (e < 0 || g_effects[e].ugen == gen) return;
    for (e = 0; e < ne; e++)
        if (g_effects[e].fx == fx) {
            g_effects[e].ugen = gen;
            if (g_effects[e].table >= 0 && strstr(fx_name(g_effects[e].table), "indoor")) indoor = 1;
        }
    {
        D3DXHANDLE hl = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraLook");
        if (hl) {
            D3DXVECTOR4 l = { (indoor ? g_look_fill_in : g_look_fill) / 100.0f, g_look_fog / 100.0f, g_look_sun / 100.0f,
                              g_cascade ? 1.0f : 0.0f };
            if (g_stock_view) memset(&l, 0, sizeof l);
            fx->lpVtbl->SetVector(fx, hl, &l);
        }
    }
    {
        D3DXHANDLE hp = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraPL");
        if (hp) {
            D3DXVECTOR4 p = { g_lights_on ? 1.0f : 0.0f, g_pl_smooth ? 1.0f : 0.0f,
                              g_pl_pct / 100.0f - 1.0f, g_pl_spec ? 1.0f : 0.0f };
            if (g_stock_view) memset(&p, 0, sizeof p);
            fx->lpVtbl->SetVector(fx, hp, &p);
        }
    }
    {
        D3DXHANDLE h1 = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraPLS");
        D3DXHANDLE h2 = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraPLS2");
        if (h1 && h2) {
            float pa[4], pb[4];
            D3DXVECTOR4 a, b;
            plshadow_params(pa, pb);
            a.x = pa[0]; a.y = pa[1]; a.z = pa[2]; a.w = g_stock_view ? 0 : pa[3];
            b.x = pb[0]; b.y = pb[1]; b.z = pb[2]; b.w = pb[3];
            fx->lpVtbl->SetVector(fx, h1, &a);
            fx->lpVtbl->SetVector(fx, h2, &b);
            {
                D3DXHANDLE ht = fx->lpVtbl->GetParameterByName(fx, NULL, "tUltraPLShadow");
                if (ht) fx->lpVtbl->SetTexture(fx, ht, g_stock_view ? NULL : plshadow_texture());
            }
        }
    }
    {
        D3DXHANDLE hd = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraDetail");
        if (hd) {
            D3DXVECTOR4 v = { g_detail_sun / 100.0f, g_detail_rest / 100.0f, 0, 0 };
            if (g_stock_view) memset(&v, 0, sizeof v);
            fx->lpVtbl->SetVector(fx, hd, &v);
        }
    }
    {
        D3DXHANDLE hf = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraSurf");
        if (hf) {
            D3DXVECTOR4 f = { g_surf_gloss / 100.0f - 1.0f, g_surf_spec / 100.0f - 1.0f,
                              g_surf_env / 100.0f - 1.0f, g_surf_blur / 100.0f };
            if (g_stock_view || (indoor && !g_surf_indoor)) memset(&f, 0, sizeof f);
            fx->lpVtbl->SetVector(fx, hf, &f);
        }
    }
    {
        D3DXHANDLE ha = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraAct");
        if (ha) {
            D3DXVECTOR4 a = { g_act_near ? 1.0f : 0.0f, g_act_offset / 1000.0f, g_bg_offset / 1000.0f, 0 };
            if (g_stock_view) memset(&a, 0, sizeof a);
            fx->lpVtbl->SetVector(fx, ha, &a);
        }
    }
    {
        D3DXHANDLE hh = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraHDR");
        if (hh) {
            D3DXVECTOR4 h = { hdr_unclamped() && !g_stock_view ? 1.0f : 0.0f, 0, 0, 0 };
            fx->lpVtbl->SetVector(fx, hh, &h);
        }
    }
    hm = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraMat");
    hs = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraShadow");
    if (hm) {
        D3DXVECTOR4 m = { (float)g_fill_pct / 100.0f, (float)g_pcss_min, (float)g_pcss_scale_in, g_shadow_dbg ? 1.0f : 0.0f };
        if (g_stock_view) memset(&m, 0, sizeof m);
        fx->lpVtbl->SetVector(fx, hm, &m);
    }
    if (hs) {
        D3DXVECTOR4 v = { g_pcss_on ? 1.0f : 0.0f, (float)g_pcss_scale, PCSS_MAX_RADIUS, (float)g_pcss_bias * 1e-6f };
        if (g_stock_view) memset(&v, 0, sizeof v);
        fx->lpVtbl->SetVector(fx, hs, &v);
        InterlockedIncrement(&g_ultra_writes);
        /* what the shader will see next to our knobs: the engine's shadow
         * map size (PCSS scales its taps by .z) and the knobs as read back */
        if (InterlockedIncrement(&g_ultra_logged) <= 200) {
            D3DXVECTOR4 ss = {0}, rb = {0};
            D3DXHANDLE hz = fx->lpVtbl->GetParameterByName(fx, NULL, "gvShadowSize");
            if (hz) fx->lpVtbl->GetVector(fx, hz, &ss);
            fx->lpVtbl->GetVector(fx, hs, &rb);
            hg_log("gfxprobe: knobs gen %ld -> fx %p: pcss %.0f sun out %.0f in %ld min %ld bias %ld fill %ld%% | gvShadowSize %g %g %g %g | type %d",
                   gen, (void *)fx, rb.x, rb.y, g_pcss_scale_in, g_pcss_min, g_pcss_bias, g_fill_pct,
                   ss.x, ss.y, ss.z, ss.w, hg_gfx_shadow_type());
        }
    }
}

static volatile LONG g_ours;
static HRESULT STDMETHODCALLTYPE detour_set_tech(ID3DXEffect *fx, D3DXHANDLE h)
{
    LONG i, n = g_ntech;
    if (g_ours) return g_orig_set_tech(fx, h);
    if (h && fxk_get(fx) == FXK_SHADOW) {           /* point-light shadows: skinned or rigid caster */
        D3DXTECHNIQUE_DESC d;
        if (SUCCEEDED(fx->lpVtbl->GetTechniqueDesc(fx, h, &d)) && d.Name) plshadow_technique(strstr(d.Name, "Animated") != NULL);
    }
    ultra_apply(fx);
    for (i = 0; i < n && i < MAX_TECH; i++)
        if (g_tech[i].fx == fx && g_tech[i].h == h) {
            InterlockedIncrement(&g_tech[i].n);
            return g_orig_set_tech(fx, h);
        }
    EnterCriticalSection(&g_tech_cs);
    n = g_ntech;
    for (i = 0; i < n && i < MAX_TECH; i++)
        if (g_tech[i].fx == fx && g_tech[i].h == h) break;
    if (i == n && n < MAX_TECH) {
        D3DXTECHNIQUE_DESC d;
        g_tech[n].fx = fx; g_tech[n].h = h; g_tech[n].n = 0;
        if (h && SUCCEEDED(fx->lpVtbl->GetTechniqueDesc(fx, h, &d)) && d.Name)
            lstrcpynA(g_tech[n].name, d.Name, sizeof g_tech[n].name);
        else
            lstrcpynA(g_tech[n].name, h ? "(no desc)" : "(null)", sizeof g_tech[n].name);
        g_ntech = n + 1;
    } else if (i == n) {
        InterlockedIncrement(&g_tech_overflow);
    }
    if (i < MAX_TECH) InterlockedIncrement(&g_tech[i].n);
    LeaveCriticalSection(&g_tech_cs);
    return g_orig_set_tech(fx, h);
}


/* BeginPass/EndPass: which effect is drawing (the shadow trace reads it). */
typedef HRESULT (STDMETHODCALLTYPE *beginpass_fn)(ID3DXEffect *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *endpass_fn)(ID3DXEffect *);
static beginpass_fn g_orig_beginpass;
static endpass_fn   g_orig_endpass;

static ID3DXEffect *g_cur_fx;

/*
 * Sharper material textures: after the pass has set its own sampler
 * states, every stage below the shadow maps (10-12) that filters linearly
 * gets anisotropic filtering and the mip bias. Point-sampled stages are
 * lookups and stay as they are.
 */
IDirect3DDevice9 *device_get(void);
static void sharpen_samplers(ID3DXEffect *fx)
{
    IDirect3DDevice9 *dev = device_get();
    float bias = g_mip_bias / 100.0f;
    DWORD st, bias_bits;
    memcpy(&bias_bits, &bias, 4);
    (void)fx;
    if (!dev) return;
    for (st = 0; st < 10; st++) {
        DWORD minf = 0;
        IDirect3DDevice9_GetSamplerState(dev, st, D3DSAMP_MINFILTER, &minf);
        if (minf != D3DTEXF_LINEAR && minf != D3DTEXF_ANISOTROPIC) continue;
        if (g_aniso > 1) {
            IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MAXANISOTROPY, (DWORD)g_aniso);
        }
        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MIPMAPLODBIAS, bias_bits);
    }
}

/*
 * Our own passes (src/postfx.c) run in the middle of the engine's: the AO
 * goes just before the first blended material draw, inside that pass. They
 * must not touch the bookkeeping below: our EndPass restored stage 12 early
 * (the fine shadow map's) and cleared the flag, the engine's EndPass then
 * left the shadow map bound, and the next effect reading stage 12 (the
 * glow) read depth as its texture: white flashes outdoors with AO on.
 */
void gfxprobe_own_passes(int on) { InterlockedExchange(&g_ours, on ? 1 : 0); }

/*
 * Soft particles: particle.fxo (ours, shaders/particle.fx) reads the
 * scene's linear depth on sampler 1, which no effect parameter binds; bind
 * it and set gvUltraSoft after the pass has set its own state.
 */
IDirect3DTexture9 *postfx_soft_depth(float *inv_dist);
/*
 * Lit particles (shaders/particle.fx, gvUltraPart): for our particle passes,
 * the nearest lights (the fog's eased list, 5 of them) go into the effect's
 * own point-light arrays, and outdoors the near sun shadow map and its
 * world-space matrix into tShadowMapDepth / gmShadowMatrix2. Particle
 * vertices must be in world space: the effect's EyeInObject has to equal
 * EyeInWorld, or the pass stays stock (counted in the log).
 */
static volatile LONG g_part_light = 60;     /* percent */
static volatile LONG g_part_shadow = 50;    /* percent */
static LONG g_part_lit, g_part_notworld;
int plshadow_lights_near(const float eye[3], float margin, float (*pr)[4], float (*col)[4], int max);

static void part_bind(ID3DXEffect *fx, int k, IDirect3DDevice9 *dev)
{
    D3DXHANDLE *h = g_fxk[k].hpart;
    D3DXVECTOR4 knob = { 0, 0, 0, 0 };
    LONG fr;
    const volfog_state *v = volfog_get(&fr);
    if (!h[0]) {
        static const char *const names[8] = { "gvUltraPart", "_PointLightsPos_1", "PointLightsColor", "_PointLightsFalloff_1",
                                              "gmShadowMatrix2", "tShadowMapDepth", "EyeInObject", "EyeInWorld" };
        int i;
        for (i = 0; i < 8; i++) h[i] = fx->lpVtbl->GetParameterByName(fx, NULL, names[i]);
        if (!h[0]) return;
    }
    if (!g_stock_view && v->cam_frame == fr && (g_part_light > 0 || g_part_shadow > 0)) {
        D3DXVECTOR4 eo, ew;
        int world = h[6] && h[7] && SUCCEEDED(fx->lpVtbl->GetVector(fx, h[6], &eo)) && SUCCEEDED(fx->lpVtbl->GetVector(fx, h[7], &ew)) &&
                    fabsf(eo.x - ew.x) + fabsf(eo.y - ew.y) + fabsf(eo.z - ew.z) < 0.05f;
        if (!world) {
            InterlockedIncrement(&g_part_notworld);
        } else {
            if (g_part_light > 0 && h[1] && h[2] && h[3]) {
                float pr[5][4], col[5][4];
                D3DXVECTOR4 lp[5], lc[5], lf[5];
                int n = plshadow_lights_near(v->eye, 40.0f, pr, col, 5), i;
                for (i = 0; i < 5; i++) {
                    D3DXVECTOR4 z = { 0, 0, 0, 0 };
                    lp[i] = z; lc[i] = z; lf[i] = z;
                    if (i < n) {
                        lp[i].x = pr[i][0]; lp[i].y = pr[i][1]; lp[i].z = pr[i][2];
                        lc[i].x = col[i][0]; lc[i].y = col[i][1]; lc[i].z = col[i][2];
                        lf[i].x = pr[i][3];
                    }
                }
                fx->lpVtbl->SetVectorArray(fx, h[1], lp, 5);
                fx->lpVtbl->SetVectorArray(fx, h[2], lc, 5);
                fx->lpVtbl->SetVectorArray(fx, h[3], lf, 5);
                knob.x = g_part_light / 100.0f;
            }
            if (g_part_shadow > 0 && h[4] && h[5] && fr - v->maps_frame <= 8 && v->nearmap) {
                fx->lpVtbl->SetMatrix(fx, h[4], (const D3DXMATRIX *)v->near_m);
                fx->lpVtbl->SetTexture(fx, h[5], v->nearmap);
                knob.y = g_part_shadow / 100.0f;
                /* a third of a unit of depth: smoke need not be exact */
                knob.z = 0.3f * sqrtf(v->near_m[2] * v->near_m[2] + v->near_m[6] * v->near_m[6] + v->near_m[10] * v->near_m[10]);
            }
            if (knob.x > 0 || knob.y > 0) InterlockedIncrement(&g_part_lit);
        }
    }
    fx->lpVtbl->SetVector(fx, h[0], &knob);
    fx->lpVtbl->CommitChanges(fx);
    if (knob.y > 0) {
        IDirect3DDevice9_SetSamplerState(dev, 2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, 2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 2, D3DSAMP_SRGBTEXTURE, 0);
    }
}

void hg_gfx_nudge_part(int which, int d)
{
    volatile LONG *p = which ? &g_part_shadow : &g_part_light;
    LONG v = *p + d;
    InterlockedExchange(p, v < 0 ? 0 : v > 200 ? 200 : v);
    hg_log("gfxprobe: lit particles: lights %ld%%, sun shadow %ld%% (lit passes %ld, not in world space %ld)",
           g_part_light, g_part_shadow, g_part_lit, g_part_notworld);
}
int hg_gfx_part(int which) { return (int)(which ? g_part_shadow : g_part_light); }

static void soft_bind(ID3DXEffect *fx, UINT pass)
{
    IDirect3DDevice9 *dev = device_get();
    int k = fxk_slot(fx);
    IDirect3DTexture9 *t;
    D3DXVECTOR4 v = { 0, 0, 0, 0 };
    if (k < 0 || !dev) return;
    if (!g_fxk[k].soft_looked) {
        g_fxk[k].soft_looked = 1;
        g_fxk[k].hsoft = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraSoft");
        g_fxk[k].hsoft_tech[0] = fx->lpVtbl->GetTechniqueByName(fx, "TVertexAndPixelShader0");
        g_fxk[k].hsoft_tech[1] = fx->lpVtbl->GetTechniqueByName(fx, "TVertexAndPixelShaderAdditive");
        g_fxk[k].hsoft_tech[2] = fx->lpVtbl->GetTechniqueByName(fx, "TVertexAndPixelShaderAddGlowGlowConstant");
        g_fxk[k].hsoft_tech[3] = fx->lpVtbl->GetTechniqueByName(fx, "TVertexAndPixelShaderGlow");
        g_fxk[k].hsoft_tech[4] = fx->lpVtbl->GetTechniqueByName(fx, "TVertexAndPixelShader");
    }
    if (!g_fxk[k].hsoft) return;                    /* not ours (the skybox, stock particles) */
    /* only the passes that run our shaders (pass 0 of five techniques;
     * mkparticle.py swapped them by stock shader): the rest are the stock
     * shaders or fixed function with their own texture on stage 1, and
     * binding the depth there for every particle pass made swings, impacts
     * and ash vanish. Those get the fade off and stage 1 untouched. */
    {
        D3DXHANDLE cur = fx->lpVtbl->GetCurrentTechnique(fx);
        int i, ours = 0;
        for (i = 0; i < 5 && cur && pass == 0; i++) if (cur == g_fxk[k].hsoft_tech[i]) ours = 1;
        if (!ours) {
            D3DXHANDLE hp = g_fxk[k].hpart[0] ? g_fxk[k].hpart[0] : fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraPart");
            D3DXVECTOR4 z = { 0, 0, 0, 0 };
            fx->lpVtbl->SetVector(fx, g_fxk[k].hsoft, &v);
            if (hp) fx->lpVtbl->SetVector(fx, hp, &z);
            fx->lpVtbl->CommitChanges(fx);
            return;
        }
    }
    part_bind(fx, k, dev);
    t = postfx_soft_depth(&v.x);
    {
        D3DXHANDLE ht = fx->lpVtbl->GetParameterByName(fx, NULL, "tUltraSoftDepth");
        if (ht) fx->lpVtbl->SetTexture(fx, ht, (IDirect3DBaseTexture9 *)t);
    }
    if (t) {
        IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)t);
        IDirect3DDevice9_SetSamplerState(dev, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 1, D3DSAMP_SRGBTEXTURE, 0);
    }
    fx->lpVtbl->SetVector(fx, g_fxk[k].hsoft, &v);
    fx->lpVtbl->CommitChanges(fx);
}

static HRESULT STDMETHODCALLTYPE detour_beginpass(ID3DXEffect *fx, UINT pass)
{
    HRESULT hr;
    if (g_ours) return g_orig_beginpass(fx, pass);
    if (fx == g_ui_fx) postfx_before_ui();
    g_cur_kind = fxk_get(fx);
    if (g_cur_kind == FXK_AFTER_OPAQUE) postfx_before_transparent('P');     /* skybox or particle pass */
    g_cur_fx = fx;
    hr = g_orig_beginpass(fx, pass);
    if (g_cur_kind == FXK_MATERIAL && (g_aniso > 1 || g_mip_bias) && !g_stock_view) sharpen_samplers(fx);
    if (g_cur_kind == FXK_MATERIAL && !g_stock_view) plshadow_bind(device_get());
    if (g_cur_kind == FXK_AFTER_OPAQUE) soft_bind(fx, pass);
    return hr;
}

static HRESULT STDMETHODCALLTYPE detour_endpass(ID3DXEffect *fx)
{
    if (g_ours) return g_orig_endpass(fx);
    if (g_fine_bound) {
        IDirect3DDevice9 *dev = NULL;
        g_fine_bound = 0;
        if (SUCCEEDED(fx->lpVtbl->GetDevice(fx, &dev)) && dev) {
            dev->lpVtbl->SetTexture(dev, 12, g_s12_prev);
            dev->lpVtbl->Release(dev);
        }
        if (g_s12_prev) { g_s12_prev->lpVtbl->Release(g_s12_prev); g_s12_prev = NULL; }
    }
    g_cur_fx = NULL;            /* only valid inside a pass: effects are freed per level */
    g_cur_kind = FXK_OTHER;
    return g_orig_endpass(fx);
}

static void hook_set_technique(ID3DXEffect *fx)
{
    static LONG done;
    void **vt = *(void ***)fx;
    void *target;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    target = vt[offsetof(ID3DXEffectVtbl, SetTechnique) / sizeof(void *)];
    if (MH_CreateHook(target, (void *)detour_set_tech, (void **)&g_orig_set_tech) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        hg_log("gfxprobe: hooked ID3DXEffect::SetTechnique at %p (slot %u)", target,
               (unsigned)(offsetof(ID3DXEffectVtbl, SetTechnique) / sizeof(void *)));
    else
        hg_log("gfxprobe: FAILED to hook ID3DXEffect::SetTechnique at %p", target);
    target = vt[offsetof(ID3DXEffectVtbl, BeginPass) / sizeof(void *)];
    if (MH_CreateHook(target, (void *)detour_beginpass, (void **)&g_orig_beginpass) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        hg_log("gfxprobe: hooked ID3DXEffect::BeginPass at %p", target);
    target = vt[offsetof(ID3DXEffectVtbl, EndPass) / sizeof(void *)];
    if (MH_CreateHook(target, (void *)detour_endpass, (void **)&g_orig_endpass) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        hg_log("gfxprobe: hooked ID3DXEffect::EndPass at %p", target);
}

/* ------------------------------------------------------------------ */
/* technique requests: which feature bytes does the engine ask for?    */

/*
 * dxC_EffectGetTechniqueByFeatures (inner, 0x7807ff): cdecl
 * (effect *, const unsigned char feat[16], int *index). Logging the first
 * distinct requests per effect shows, byte by byte, what the engine wants --
 * in particular whether PointLights is ever non-zero for the player's
 * actoroutdoor30 draws. Feature byte order is the annotation table
 * sFillTechniqueArray reads ("Index", "VSVersion", ...), see LOG.
 */
#define RVA_TECH_BY_FEATURES 0x003807FFu
typedef int (__cdecl *tech_by_feat_fn)(void *, const unsigned char *, int *);
static tech_by_feat_fn g_orig_tech_by_feat;
#define MAX_FEAT 64
static struct { void *fx; unsigned char f[16]; int idx; LONG n; } g_feat[MAX_FEAT];
static volatile LONG g_nfeat;
static volatile LONG g_req_per_effect[MAX_EFFECTS];   /* technique requests per effect, uncapped */
static volatile LONG g_shadow_req_logged;

static int __cdecl detour_tech_by_feat(void *fx, const unsigned char *feat, int *idx)
{
    int r;
    LONG i, n;
    /*
     * The panel toggle. feat is the caller's 16-byte request on its stack:
     * ints Index, PointLights, ShadowType, then a bool bitfield.
     * - Lights on, effect with our _pl5 techniques, mesh has any light: ask
     *   for exactly 5, the single-pass per-pixel technique (the lookup wants
     *   an exact match; the engine zero-pads the unused light colours and
     *   takes all five out of SH).
     * - Lights off: clamp to the stock maximum, so our additions are never
     *   picked and the look is stock.
     */
    if (feat && !IsBadReadPtr(feat, 16) && !IsBadReadPtr((char *)fx + 0x118, 4)) {
        ID3DXEffect *d3dxfx = *(ID3DXEffect **)((char *)fx + 0x118);
        LONG e, ne = g_neffects < MAX_EFFECTS ? g_neffects : MAX_EFFECTS;
        /* the effect has two records; only the inner one is marked overridden */
        for (e = 0; e < ne; e++)
            if (g_effects[e].fx == d3dxfx && g_effects[e].overridden) {
                int *pl = (int *)(feat + 4);
                if (g_effects[e].has_pl5) {          /* characters: which ShadowType do they ask for? */
                    int *st = (int *)(feat + 8);
                    InterlockedIncrement(&g_act_st[*st >= 0 && *st < 3 ? *st : 3]);
                    /* The engine asks for ShadowType 0 for every character (log,
                     * 2026-09-22): characters never receive shadows, not even
                     * their own. Ask for the colour-map technique instead; every
                     * feature combination has one (stock, and our _pl5). */
                    if (g_act_near && !g_stock_view && g_shadows_live && *st == 0 && hg_gfx_shadow_type() == 2) {
                        *st = 2;
                        InterlockedIncrement(&g_act_st_up);
                    }
                }
                if (g_lights_on && !g_stock_view && g_effects[e].has_pl5 && *pl > 0) {
                    *pl = 5;
                    InterlockedIncrement(&g_n_lit);
                } else if (*pl > g_effects[e].stock_max) {
                    *pl = g_effects[e].stock_max;
                    InterlockedIncrement(&g_n_clamped);
                }
                break;
            }
    }
    r = g_orig_tech_by_feat(fx, feat, idx);
    if (!IsBadReadPtr((char *)fx + 0x118, 4)) {
        ID3DXEffect *d3dxfx = *(ID3DXEffect **)((char *)fx + 0x118);
        LONG e, ne = g_neffects < MAX_EFFECTS ? g_neffects : MAX_EFFECTS;
        for (e = 0; e < ne; e++)
            if (g_effects[e].fx == d3dxfx) {
                InterlockedIncrement(&g_req_per_effect[e]);
                /* the shadow effect: log every distinct request, no cap */
                if (g_effects[e].table >= 0 && strstr(fx_name(g_effects[e].table), "shadow")
                    && feat && !IsBadReadPtr(feat, 16) && InterlockedIncrement(&g_shadow_req_logged) <= 40)
                    hg_log("gfxprobe: SHADOW request %s idx=%d pl=%d st=%d bools=%02x%02x -> %d (hr 0x%08x)",
                           fx_name(g_effects[e].table), *(const int *)feat, *(const int *)(feat + 4),
                           *(const int *)(feat + 8), feat[13], feat[12], idx ? *idx : -1, (unsigned)r);
                break;
            }
    }
    n = g_nfeat;
    if (!feat || IsBadReadPtr(feat, 16)) return r;
    for (i = 0; i < n && i < MAX_FEAT; i++)
        if (g_feat[i].fx == fx && memcmp(g_feat[i].f, feat, 16) == 0) { InterlockedIncrement(&g_feat[i].n); return r; }
    i = InterlockedIncrement(&g_nfeat) - 1;
    if (i < MAX_FEAT) {
        /* fx is the engine's effect record: +0x118 ID3DXEffect*, +0x11c
         * technique records (0x34 bytes each, D3DXHANDLE first, the 16
         * feature bytes at +0x1c), +0x120 count, +0x124 sorted index list. */
        LONG e, ne = g_neffects < MAX_EFFECTS ? g_neffects : MAX_EFFECTS;
        const char *owner = "?", *tname = "?";
        ID3DXEffect *d3dxfx = IsBadReadPtr((char *)fx + 0x118, 16) ? NULL : *(ID3DXEffect **)((char *)fx + 0x118);
        char *recs = d3dxfx ? *(char **)((char *)fx + 0x11c) : NULL;
        int count = d3dxfx ? *(int *)((char *)fx + 0x120) : 0;
        int *order = d3dxfx ? *(int **)((char *)fx + 0x124) : NULL;
        D3DXTECHNIQUE_DESC d;
        g_feat[i].fx = fx; memcpy(g_feat[i].f, feat, 16); g_feat[i].idx = idx ? *idx : -1; g_feat[i].n = 1;
        for (e = 0; e < ne; e++) if (g_effects[e].fx == d3dxfx) { owner = fx_name(g_effects[e].table); break; }
        if (d3dxfx && recs && order && idx && *idx >= 0 && *idx < count && !IsBadReadPtr(order, (size_t)count * 4)) {
            int ti = order[*idx];
            if (ti >= 0 && ti < count && !IsBadReadPtr(recs + ti * 0x34, 0x34)) {
                D3DXHANDLE h = *(D3DXHANDLE *)(recs + ti * 0x34);
                if (h && SUCCEEDED(d3dxfx->lpVtbl->GetTechniqueDesc(d3dxfx, h, &d)) && d.Name) tname = d.Name;
            }
        }
        hg_log("gfxprobe: feat %-22s idx=%d pl=%d st=%d bools=%02x%02x -> %d %s",
               owner, *(const int *)feat, *(const int *)(feat + 4), *(const int *)(feat + 8), feat[13], feat[12],
               idx ? *idx : -1, tname);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* shadow pass: does the player's model ever reach dx9_RenderModelShadow? */

#define RVA_RENDER_MODEL_SHADOW 0x003CB550u
void *g_orig_render_shadow;          /* asm-visible as _g_orig_render_shadow */
static volatile LONG g_shadow_calls, g_shadow_player_calls;
static volatile LONG g_shadow_player_rc = 12345;

static volatile LONG g_shadow_rc_ok, g_shadow_rc_fail, g_shadow_rc_other;

/*
 * dx9_RenderModelShadow renders only while [0xedfd14] == 0 and
 * [0xedfcb4] != 0: render flags 91 "wireframe" and 67 "shadows" (array base
 * 0xedfba8; the definition table is 0x4c-byte entries at 0xad40b8 with the
 * NAME first (64 bytes), then type, default, mode-3 default -- an earlier
 * read of the names at +12 of 0xad40f8 was off by one). "shadows" defaults
 * to 1. The panel can force it for an experiment.
 */
#define RVA_RFLAG_SHADOW  0x00ADFCB4u
#define RVA_RFLAG_NOSHADE 0x00ADFD14u
static volatile LONG g_force_shadow_flag;
static int g_shadow_flag_saved = -1;

/*
 * e_SetRenderFlag(index, value), cdecl, 0x778460: the only writer of the
 * flag array (base 0xedfba8, so 0xedfcb4 is flag 0x43 and 0xedfd14 is
 * 0x5b "wireframe") and it runs a per-flag callback (table at 0xedfd38) before storing
 * -- poking the array directly, as the first version of this did, skipped
 * whatever that callback sets up. Go through the setter.
 */
#define RVA_SET_RENDER_FLAG 0x00378460u
#define RFLAG_SHADOW 0x43
typedef int (__cdecl *set_rflag_fn)(unsigned int idx, int value);

static void shadow_flag_apply(void)
{
    int *f = (int *)(g_image + RVA_RFLAG_SHADOW);
    set_rflag_fn setflag = (set_rflag_fn)(g_image + RVA_SET_RENDER_FLAG);
    static const unsigned char sig[3] = { 0x55, 0x8b, 0xec };
    if (IsBadWritePtr(f, 4) || IsBadReadPtr((void *)setflag, 3) || memcmp((void *)setflag, sig, 3) != 0) return;
    if (g_force_shadow_flag) {
        if (g_shadow_flag_saved < 0) g_shadow_flag_saved = *f;
        if (*f == 0) { setflag(RFLAG_SHADOW, 1); hg_log("gfxprobe: e_SetRenderFlag(0x43, 1) -> flag now %d", *f); }
    } else if (g_shadow_flag_saved >= 0) {
        setflag(RFLAG_SHADOW, g_shadow_flag_saved);
        g_shadow_flag_saved = -1;
    }
}

/*
 * dx9_RenderModelShadow(nDrawList, nData, nID) is called with the model id
 * in ECX and two stack arguments that the CALLER pops (dx9_RenderDrawList:
 * push [edi+0x30]; mov ecx,[edi+4]; push [esp+0x40]; call; pop ecx; pop
 * ecx). The first version of this probe was a plain __fastcall detour,
 * which forwarded ECX only: the renderer read its draw-list index from
 * garbage, FUN_007b4135 returned NULL and EVERY call came back E_FAIL. The
 * probe itself killed the shadow pass (2026-09-21, 39,639 failures in one
 * run). This thunk forwards the stack arguments untouched and only looks at
 * the result.
 */
/*
 * e_GetActiveShadowType(): [[0xedff74] + 8] + 0x40, the option state's
 * nShadowType. 1 = depth shadow map (NULL/fake colour target, D24S8 depth
 * texture sampled as a shadow sampler -- the NVIDIA hardware-PCF path),
 * 2 = colour shadow map (R16F/R32F/A8R8G8B8 target, "*ColorShader"
 * techniques in shadowmap.fxo). Under wined3d the engine picks 1 and no
 * shadow ever reaches the screen (A/B 2026-09-21: DXVK draws them, wined3d
 * does not, with and without this DLL; forcing 2 brought them back). 2 is
 * now forced on every renderer, before the shadow buffers are created
 * (dxC_shadow.cpp FUN_007e2be5, cdecl, the option state as its argument),
 * and re-asserted every frame: PCSS needs its readable depth.
 * bin\hellgate_shadowtype2.off keeps the engine's choice.
 */
#define RVA_SETTINGS_PTR       0x00ADFF74u
#define RVA_SHADOW_BUFFERS_NEW 0x003E2BE5u
static LONG g_force_type2;
static LONG g_type2_reforced;
typedef int (__cdecl *shadow_new_fn)(void *state);
static shadow_new_fn g_orig_shadow_new;

static int *shadow_type_slot(void *state)
{
    unsigned int s;
    if (!state) {
        /* 0xedff74 holds a pointer P; the engine passes *P (mov eax,[0xedff74];
         * push [eax]). Two dereferences, not one -- the first read got -1. */
        unsigned int *pp = (unsigned int *)(g_image + RVA_SETTINGS_PTR);
        if (IsBadReadPtr(pp, 4) || !*pp || IsBadReadPtr((void *)*pp, 4)) return NULL;
        state = (void *)**(unsigned int **)pp;
        if (!state) return NULL;
    }
    if (IsBadReadPtr((char *)state + 8, 4)) return NULL;
    s = *(unsigned int *)((char *)state + 8);
    if (!s || IsBadWritePtr((void *)(s + 0x40), 4)) return NULL;
    return (int *)(s + 0x40);
}

/* Wine's builtin d3d9 (wined3d) carries "Wine builtin DLL" in its DOS stub;
 * DXVK's d3d9.dll does not. -1 = d3d9 not loaded (yet). */
static int d3d9_is_wined3d(void)
{
    HMODULE m = GetModuleHandleA("d3d9.dll");
    if (!m || IsBadReadPtr((char *)m + 0x40, 16)) return -1;
    return memcmp((char *)m + 0x40, "Wine builtin DLL", 16) == 0;
}

/*
 * The colour shadow map (ShadowType 2) is the default now: it is the one
 * whose depth our shaders can read, which PCSS needs, and wined3d cannot
 * draw the other one at all. Its shaders' stock filter (2x2 bilinear
 * compare) matches the depth map's hardware PCF, so with PCSS off the look
 * is the same. bin\hellgate_shadowtype2.off keeps the engine's own choice.
 */
static LONG g_type2_mode = 1;   /* 1 always, -1 never (flag file) */

/*
 * The engine takes the first colour format the device supports from
 * { R16F, R32F, A8R8G8B8, X8R8G8B8 } (FUN_007e295e, immediates in its
 * code). R16F keeps about 11 bits of depth: fine for its 2x2 compare, too
 * coarse for a blocker search. Rewrite the first entry to R32F.
 */
#define RVA_SHADOW_FMT0   0x003E2971u   /* imm32 of `mov [ebp-0x10], 0x6f` */
static void shadow_format_r32f(void)
{
    unsigned char *b = (unsigned char *)(g_image + RVA_SHADOW_FMT0);
    DWORD old;
    if (IsBadReadPtr(b - 3, 7) || b[-3] != 0xc7 || b[-2] != 0x45 || b[-1] != 0xf0) {
        hg_log("gfxprobe: shadow format list not where expected; left R16F");
        return;
    }
    if (b[0] == 0x72) return;
    if (b[0] != 0x6f || !VirtualProtect(b, 1, PAGE_EXECUTE_READWRITE, &old)) {
        hg_log("gfxprobe: shadow format list byte is 0x%02x; not patched", b[0]);
        return;
    }
    b[0] = 0x72;                                /* D3DFMT_R32F */
    VirtualProtect(b, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), b, 1);
    hg_log("gfxprobe: colour shadow map format: R32F first (was R16F)");
}

static int __cdecl detour_shadow_new(void *state)
{
    int *t = shadow_type_slot(state);
    int wined3d = d3d9_is_wined3d();
    int force = g_type2_mode > 0;
    if (force) shadow_format_r32f();
    InterlockedExchange(&g_force_type2, force);
    if (t && force && *t != 2) {
        hg_log("gfxprobe: shadow buffers: nShadowType %d -> 2 (colour map, readable depth; d3d9 is %s)", *t,
               wined3d == 1 ? "wined3d" : wined3d == 0 ? "not wined3d (DXVK?)" : "not loaded");
        *t = 2;
    } else if (t) {
        hg_log("gfxprobe: shadow buffers created with nShadowType %d (d3d9 is %s%s)", *t,
               wined3d == 1 ? "wined3d" : wined3d == 0 ? "not wined3d (DXVK?)" : "not loaded",
               g_type2_mode < 0 ? "; hellgate_shadowtype2.off" : "");
    }
    return g_orig_shadow_new(state);
}

int hg_gfx_shadow_type(void)
{
    int *t = shadow_type_slot(NULL);
    return t ? *t : -1;
}

void gfx_shadow_stub(void);
void __fastcall gfx_shadow_note(int model, int r);
static void shadow_target_probe(void);
__asm__(
    ".text\n\t"
    ".globl _gfx_shadow_stub\n"
    "_gfx_shadow_stub:\n\t"
    "pushl %ebx\n\t"
    "movl %ecx, %ebx\n\t"               /* model id, preserved across the call */
    "pushl 12(%esp)\n\t"                /* nData:     4 ebx + 4 ret + 4 -> 12 */
    "pushl 12(%esp)\n\t"                /* nDrawList: 4 ebx + 4 ret -> 8, +4 pushed */
    "call *_g_orig_render_shadow\n\t"
    "addl $8, %esp\n\t"                 /* caller-cleaned, like the real caller */
    "movl %ebx, %ecx\n\t"
    "movl %eax, %edx\n\t"
    "pushl %eax\n\t"
    "call @gfx_shadow_note@8\n\t"
    "popl %eax\n\t"
    "popl %ebx\n\t"
    "ret\n\t"
);

void __fastcall gfx_shadow_note(int model, int r)
{
    InterlockedIncrement(&g_shadow_calls);
    if (r == 0) InterlockedIncrement(&g_shadow_rc_ok);
    else if ((unsigned)r == 0x80004005u) InterlockedIncrement(&g_shadow_rc_fail);
    else InterlockedIncrement(&g_shadow_rc_other);
    if (model >= 0 && model == hg_model_third()) {
        InterlockedIncrement(&g_shadow_player_calls);
        if (r == 0) shadow_target_probe();
        if (r != g_shadow_player_rc) {
            g_shadow_player_rc = r;
            hg_log("gfxprobe: dx9_RenderModelShadow(player model %d) -> 0x%08x", model, (unsigned)r);
        }
    }
}

/* ------------------------------------------------------------------ */
/* CreateFileW: does the engine ever look for data files on disk?      */

typedef HANDLE (WINAPI *create_file_fn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                        DWORD, DWORD, HANDLE);
static create_file_fn g_orig_create_file;
#define MAX_PATHS 96
static unsigned int g_path_hash[MAX_PATHS];
static volatile LONG g_npaths;
static LONG g_paths_dropped;

static WCHAR lowerw(WCHAR c) { return (c >= L'A' && c <= L'Z') ? (WCHAR)(c + 32) : c; }

/* case-insensitive "does hay contain needle" for ASCII needles */
static int containsw(const WCHAR *hay, const char *needle)
{
    const WCHAR *h;
    for (h = hay; *h; h++) {
        const WCHAR *a = h; const char *b = needle;
        while (*a && *b && lowerw(*a) == (WCHAR)*b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int endsw(const WCHAR *s, const char *suffix)
{
    int n = lstrlenW(s), m = (int)strlen(suffix), i;
    if (n < m) return 0;
    for (i = 0; i < m; i++)
        if (lowerw(s[n - m + i]) != (WCHAR)suffix[i]) return 0;
    return 1;
}

static HANDLE WINAPI detour_create_file(LPCWSTR path, DWORD access, DWORD share,
                                        LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD attr,
                                        HANDLE tmpl)
{
    HANDLE h = g_orig_create_file(path, access, share, sa, disp, attr, tmpl);
    if (path && g_npaths < MAX_PATHS
        && (containsw(path, "data\\") || containsw(path, "data_common\\"))
        && !containsw(path, "\\override\\")
        && !endsw(path, ".dat") && !endsw(path, ".idx") && !endsw(path, ".log")) {
        DWORD err = GetLastError();
        unsigned int hash = fnv1a((const unsigned char *)path, lstrlenW(path) * 2);
        LONG i, n = g_npaths;
        for (i = 0; i < n; i++) if (g_path_hash[i] == hash) break;
        if (i == n) {
            i = InterlockedIncrement(&g_npaths) - 1;
            if (i < MAX_PATHS) {
                g_path_hash[i] = hash;
                hg_log("gfxprobe: file %s %ls%s", h == INVALID_HANDLE_VALUE ? "MISS" : "open",
                       path, (access & GENERIC_WRITE) ? " (write)" : "");
            } else {
                InterlockedIncrement(&g_paths_dropped);
            }
        }
        SetLastError(err);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* device: formats and frame structure                                 */

typedef HRESULT (STDMETHODCALLTYPE *set_rt_fn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *set_ds_fn)(IDirect3DDevice9 *, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *dip_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT,
                                            UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *dp_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *clear_fn)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD,
                                              D3DCOLOR, float, DWORD);
static set_rt_fn g_orig_set_rt;
static set_ds_fn g_orig_set_ds;
static dip_fn    g_orig_dip;
static dp_fn     g_orig_dp;
static clear_fn  g_orig_clear;


typedef struct {
    int kind;                 /* 'R' set RT, 'D' set DS, 'C' clear */
    DWORD idx;                /* RT index, or clear flags */
    void *surf;
    unsigned int w, h, fmt;
    LONG draws, prims;        /* draws after this event, before the next */
} seg;
#define MAX_SEG 512
static seg  g_seg[MAX_SEG];
static volatile LONG g_nseg;
static volatile LONG g_capturing;   /* 1 while a frame is being recorded */
static volatile LONG g_capture_req; /* set by the worker; consumed at EndScene */
static LONG g_frames;
static LONG g_auto_done;
static LONG g_frame_draws;
static LONG g_frame_rt_changes;

static const char *fmt_name(unsigned int f, char *buf)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_R16F: return "R16F";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24FS8: return "D24FS8";
    case D3DFMT_D32: return "D32";
    case D3DFMT_A8: return "A8";
    case D3DFMT_L8: return "L8";
    case D3DFMT_UNKNOWN: return "UNKNOWN";
    }
    if (f > 0x01000000) { /* fourcc */
        buf[0] = (char)(f & 0xff); buf[1] = (char)(f >> 8); buf[2] = (char)(f >> 16);
        buf[3] = (char)(f >> 24); buf[4] = 0;
        return buf;
    }
    snprintf(buf, 16, "fmt%u", f);
    return buf;
}

static void seg_add(int kind, DWORD idx, IDirect3DSurface9 *s)
{
    LONG i;
    D3DSURFACE_DESC d;
    if (!g_capturing) return;
    i = InterlockedIncrement(&g_nseg) - 1;
    if (i >= MAX_SEG) return;
    g_seg[i].kind = kind; g_seg[i].idx = idx; g_seg[i].surf = s;
    g_seg[i].w = g_seg[i].h = g_seg[i].fmt = 0;
    g_seg[i].draws = g_seg[i].prims = 0;
    if (s && SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d))) {
        g_seg[i].w = d.Width; g_seg[i].h = d.Height; g_seg[i].fmt = d.Format;
    }
}

static void seg_draw(UINT prims)
{
    LONG i;
    InterlockedIncrement(&g_frame_draws);
    if (!g_capturing) return;
    i = g_nseg - 1;
    if (i >= 0 && i < MAX_SEG) {
        InterlockedIncrement(&g_seg[i].draws);
        InterlockedExchangeAdd(&g_seg[i].prims, (LONG)prims);
    }
}

/*
 * Every distinct surface ever bound as a render target or depth stencil,
 * with how often. The frame captures kept landing on frames where the
 * shadow maps were not re-rendered (they update only when dirty), so this
 * table answers "what shadow targets exist and how big are they" without
 * needing the right frame.
 */
#define MAX_SURF 48
static struct { void *s; int kind; unsigned int w, h, fmt; LONG n; } g_surf[MAX_SURF];
static volatile LONG g_nsurf;
static IDirect3DDevice9 *g_probe_dev;

static void surf_note(int kind, IDirect3DSurface9 *s)
{
    LONG i, n = g_nsurf;
    D3DSURFACE_DESC d;
    if (!s) return;
    for (i = 0; i < n && i < MAX_SURF; i++)
        if (g_surf[i].s == s && g_surf[i].kind == kind) { InterlockedIncrement(&g_surf[i].n); return; }
    i = InterlockedIncrement(&g_nsurf) - 1;
    if (i >= MAX_SURF) return;
    g_surf[i].s = s; g_surf[i].kind = kind; g_surf[i].n = 1;
    g_surf[i].w = g_surf[i].h = g_surf[i].fmt = 0;
    if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d))) { g_surf[i].w = d.Width; g_surf[i].h = d.Height; g_surf[i].fmt = d.Format; }
}

static HRESULT STDMETHODCALLTYPE detour_set_rt(IDirect3DDevice9 *dev, DWORD idx, IDirect3DSurface9 *s)
{
    InterlockedIncrement(&g_frame_rt_changes);
    seg_add('R', idx, s);
    if (idx == 0) { surf_note('R', s); plshadow_rt_changed(); }
    return g_orig_set_rt(dev, idx, s);
}
static HRESULT STDMETHODCALLTYPE detour_set_ds(IDirect3DDevice9 *dev, IDirect3DSurface9 *s)
{
    seg_add('D', 0, s);
    surf_note('D', s);
    return g_orig_set_ds(dev, s);
}

/* What is bound while a model is drawn into the shadow map: queried on the
 * first few successful player-shadow calls. */
static LONG g_shadow_target_logged;
static void shadow_target_probe(void)
{
    IDirect3DSurface9 *rt = NULL, *ds = NULL;
    D3DSURFACE_DESC dr, dd;
    char b1[16], b2[16];
    IDirect3DDevice9 *dev = g_probe_dev;
    if (!dev || InterlockedIncrement(&g_shadow_target_logged) > 3) return;
    memset(&dr, 0, sizeof dr); memset(&dd, 0, sizeof dd);
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)) && rt) { IDirect3DSurface9_GetDesc(rt, &dr); IDirect3DSurface9_Release(rt); }
    if (SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface(dev, &ds)) && ds) { IDirect3DSurface9_GetDesc(ds, &dd); IDirect3DSurface9_Release(ds); }
    {
        DWORD cw = 0;
        IDirect3DDevice9_GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
        hg_log("gfxprobe: during dx9_RenderModelShadow: RT0 %p %ux%u %s, DS %p %ux%u %s, colorwrite 0x%lx",
               (void *)rt, dr.Width, dr.Height, fmt_name(dr.Format, b1), (void *)ds, dd.Width, dd.Height, fmt_name(dd.Format, b2),
               (unsigned long)cw);
    }
}
/*
 * Shadow map trace (panel button, ~300 frames): which textures sit in the
 * shadow samplers (s10, s11) at each material draw, with the effect's two
 * shadow matrices (by the width each covers), and which textures the
 * shadow pass renders into, per frame. Pairs a map's content with the
 * matrix it is read through, to find the outdoor maps' handover bug.
 */
static volatile LONG g_strace_left;
#define ST_MAX 32
static struct { void *t10, *t11; int w1, w2, x1, y1; long n, f0, f1; } g_st[ST_MAX];
static struct { void *rt; long n, f0, f1, last, gaps; } g_rt[ST_MAX];
static int g_nst, g_nrt;

/* IID_IDirect3DTexture9, without linking dxguid */
static const GUID k_iid_tex9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };

static void *tex_id(IDirect3DBaseTexture9 *t) { if (t) t->lpVtbl->Release(t); return t; }

static void strace_draw(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *s = NULL;
    IDirect3DTexture9 *rtt = NULL;
    long f = g_frames;
    int k;
    /* shadow pass: the render target is a texture */
    if (SUCCEEDED(dev->lpVtbl->GetRenderTarget(dev, 0, &s)) && s) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(s->lpVtbl->GetDesc(s, &d)) && d.Format == D3DFMT_R32F &&
            SUCCEEDED(s->lpVtbl->GetContainer(s, &k_iid_tex9, (void **)&rtt)) && rtt) {
            for (k = 0; k < g_nrt && g_rt[k].rt != rtt; k++) ;
            if (k == g_nrt && g_nrt < ST_MAX) { g_rt[k].rt = rtt; g_rt[k].n = 0; g_rt[k].f0 = f; g_rt[k].last = -1; g_rt[k].gaps = 0; g_nrt++; }
            if (k < g_nrt) {
                if (g_rt[k].last != f) { if (g_rt[k].last >= 0 && f - g_rt[k].last > 1) g_rt[k].gaps++; g_rt[k].n++; g_rt[k].last = f; }
                g_rt[k].f1 = f;
            }
            rtt->lpVtbl->Release(rtt);
            s->lpVtbl->Release(s);
            return;
        }
        s->lpVtbl->Release(s);
    }
    /* material draw that reads both outdoor maps */
    if (g_cur_fx) {
        ID3DXEffect *fx = g_cur_fx;
        D3DXHANDLE h1 = fx->lpVtbl->GetParameterByName(fx, NULL, "gmShadowMatrix");
        D3DXHANDLE h2 = fx->lpVtbl->GetParameterByName(fx, NULL, "gmShadowMatrix2");
        D3DXMATRIX m1, m2;
        IDirect3DBaseTexture9 *a = NULL, *b = NULL;
        void *ta, *tb;
        int w1, w2, x1, y1;
        if (!h1 || !h2 || FAILED(fx->lpVtbl->GetMatrix(fx, h1, &m1)) || FAILED(fx->lpVtbl->GetMatrix(fx, h2, &m2))) return;
        if (mcol_len(&m1, 0) == 0 || mcol_len(&m2, 0) == 0) return;
        dev->lpVtbl->GetTexture(dev, 10, &a);
        dev->lpVtbl->GetTexture(dev, 11, &b);
        ta = tex_id(a); tb = tex_id(b);
        if (!ta && !tb) return;
        w1 = (int)(1.0f / mcol_len(&m1, 0) + 0.5f);
        w2 = (int)(1.0f / mcol_len(&m2, 0) + 0.5f);
        /* where the main map's uv origin sits: its translation, in texels */
        x1 = (int)(m1.m[3][0] * 2048.0f);
        y1 = (int)(m1.m[3][1] * 2048.0f);
        for (k = 0; k < g_nst; k++)
            if (g_st[k].t10 == ta && g_st[k].t11 == tb && g_st[k].w1 == w1 && g_st[k].w2 == w2 &&
                g_st[k].x1 == x1 && g_st[k].y1 == y1) break;
        if (k == g_nst && g_nst < ST_MAX) { g_st[k].t10 = ta; g_st[k].t11 = tb; g_st[k].w1 = w1; g_st[k].w2 = w2; g_st[k].x1 = x1; g_st[k].y1 = y1; g_st[k].n = 0; g_st[k].f0 = f; g_nst++; }
        if (k < g_nst) { g_st[k].n++; g_st[k].f1 = f; }
    }
}

/* once per frame from EndScene while a trace runs */
static void strace_frame(void)
{
    int k;
    if (!g_strace_left || InterlockedDecrement(&g_strace_left) > 0) return;
    hg_log("gfxprobe: ---- shadow trace, frames up to %ld ----", g_frames);
    for (k = 0; k < g_nrt; k++)
        hg_log("  shadow pass renders into tex %p: %ld frames (%ld..%ld), %ld gaps of more than a frame",
               g_rt[k].rt, g_rt[k].n, g_rt[k].f0, g_rt[k].f1, g_rt[k].gaps);
    for (k = 0; k < g_nst; k++)
        hg_log("  material draws: s10 %p (matrix %d units, origin %d,%d texels)  s11 %p (matrix2 %d units): %ld draws, frames %ld..%ld",
               g_st[k].t10, g_st[k].w1, g_st[k].x1, g_st[k].y1, g_st[k].t11, g_st[k].w2, g_st[k].n, g_st[k].f0, g_st[k].f1);
    hg_log("gfxprobe: ---- end of shadow trace ----");
}

void hg_gfx_trace_shadows(void)
{
    g_nst = g_nrt = 0;
    InterlockedExchange(&g_strace_left, 1200);     /* ~5 s at 240 fps */
    hg_log("gfxprobe: shadow trace started (1200 frames)");
}

/*
 * Bicubic light maps need the bound light map's texel size, which a ps_3_0
 * shader cannot ask for: read it off sampler 1 per material draw and write
 * gvUltraLM when it (or the setting) changed for this effect.
 */
static void lm_update(IDirect3DDevice9 *dev, ID3DXEffect *fx)
{
    int k = fxk_slot(fx);
    IDirect3DBaseTexture9 *bt = NULL;
    DWORD key = 0;
    D3DXVECTOR4 v = { 0, 0, 0, 0 };
    if (k < 0) return;
    if (g_lm_bicubic && !g_stock_view &&
        SUCCEEDED(IDirect3DDevice9_GetTexture(dev, 1, &bt)) && bt) {
        if (IDirect3DBaseTexture9_GetType(bt) == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)bt, 0, &d)) && d.Width && d.Height) {
                key = 0x80000000u | (d.Width & 0x7fff) << 15 | (d.Height & 0x7fff);
                v.x = 1.0f; v.y = 1.0f / d.Width; v.z = 1.0f / d.Height;
            }
        }
        IDirect3DBaseTexture9_Release(bt);
    }
    if (g_fxk[k].lmkey == key) return;
    if (!g_fxk[k].hlm) g_fxk[k].hlm = fx->lpVtbl->GetParameterByName(fx, NULL, "gvUltraLM");
    g_fxk[k].lmkey = key;
    if (!g_fxk[k].hlm) return;
    fx->lpVtbl->SetVector(fx, g_fxk[k].hlm, &v);
    fx->lpVtbl->CommitChanges(fx);
}

static HRESULT STDMETHODCALLTYPE detour_dip(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT bv,
                                            UINT mi, UINT nv, UINT si, UINT pc)
{
    seg_draw(pc);
    if (g_strace_left) strace_draw(dev);
    if (g_cur_kind == FXK_MATERIAL && g_cur_fx) {
        int k = fxk_slot(g_cur_fx);
        lm_update(dev, g_cur_fx);
        if (k >= 0) {
            /* handles cached in the effect table, which forgets them when an
             * effect is created at a reused address (a cache of its own did
             * not, and a stale handle is a crash in D3DX) */
            if (!g_fxk[k].pl_looked) {
                g_fxk[k].pl_looked = 1;
                g_fxk[k].hpl[0] = g_cur_fx->lpVtbl->GetParameterByName(g_cur_fx, NULL, "_PointLightsPos_1");
                g_fxk[k].hpl[1] = g_cur_fx->lpVtbl->GetParameterByName(g_cur_fx, NULL, "PointLightsColor");
                g_fxk[k].hpl[2] = g_cur_fx->lpVtbl->GetParameterByName(g_cur_fx, NULL, "_PointLightsFalloff_1");
            }
            plshadow_collect(g_cur_fx, g_fxk[k].hpl[0], g_fxk[k].hpl[1], g_fxk[k].hpl[2]);
        }
        volfog_collect(g_cur_fx);
    }
    if (g_cur_kind == FXK_MATERIAL) {
        DWORD ab = 0;
        IDirect3DDevice9_GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &ab);
        if (!ab) g_opaque_draws++;
        else if (postfx_wants_transparent_check()) postfx_before_transparent('B');   /* blended material */
    }
    {
        HRESULT hr = g_orig_dip(dev, t, bv, mi, nv, si, pc);
        if (g_cur_kind == FXK_SHADOW && SUCCEEDED(hr) && !g_stock_view)
            plshadow_dip(dev, (pls_dip_fn)g_orig_dip, t, bv, mi, nv, si, pc);
        return hr;
    }
}
static HRESULT STDMETHODCALLTYPE detour_dp(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT sv, UINT pc)
{
    seg_draw(pc);
    return g_orig_dp(dev, t, sv, pc);
}
static HRESULT STDMETHODCALLTYPE detour_clear(IDirect3DDevice9 *dev, DWORD n, const D3DRECT *r,
                                              DWORD flags, D3DCOLOR c, float z, DWORD st)
{
    seg_add('C', flags, NULL);
    if (flags & D3DCLEAR_ZBUFFER) {
        /* The scene's depth starts over (not a shadow map's). Outdoors the
         * engine clears it after the world, before the skyline (drawn with
         * the skybox projection and our materials): AO goes first, while
         * the world is still in the depth buffer. */
        IDirect3DSurface9 *ds = NULL;
        IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
        if (ds && ds == device_depth_surface()) {
            postfx_trace('C', g_opaque_draws);
            if (g_opaque_draws && postfx_wants_transparent_check()) postfx_before_transparent('Z');
            g_opaque_draws = 0;
        }
        if (ds) IDirect3DSurface9_Release(ds);
    }
    return g_orig_clear(dev, n, r, flags, c, z, st);
}

/*
 * CreateQuery under the 64-bit render server (tools/bridge.sh). In-process,
 * DXVK refuses query types it does not implement and the engine copes; the
 * bridge client reports success without asking, the server's CreateQuery
 * fails silently, and the engine's first GetData on that query dereferences
 * a null in NvRemixBridge.exe (first bridge run, 2026-09-22). Refuse the
 * types DXVK does not support, as it would have.
 */
typedef HRESULT (STDMETHODCALLTYPE *create_query_fn)(IDirect3DDevice9 *, D3DQUERYTYPE, IDirect3DQuery9 **);
static create_query_fn g_orig_create_query;
static HRESULT STDMETHODCALLTYPE detour_create_query(IDirect3DDevice9 *dev, D3DQUERYTYPE t, IDirect3DQuery9 **q)
{
    static LONG seen;
    int ok = t == D3DQUERYTYPE_EVENT || t == D3DQUERYTYPE_OCCLUSION || t == D3DQUERYTYPE_VCACHE ||
             t == D3DQUERYTYPE_TIMESTAMP || t == D3DQUERYTYPE_TIMESTAMPDISJOINT || t == D3DQUERYTYPE_TIMESTAMPFREQ;
    if (!(seen & (1L << (t & 31)))) {
        InterlockedOr(&seen, 1L << (t & 31));
        hg_log("gfxprobe: CreateQuery type %d %s", (int)t, ok ? "passed through" : "refused (not in DXVK)");
    }
    if (!ok) return D3DERR_NOTAVAILABLE;
    return g_orig_create_query(dev, t, q);
}

/* From src/device.c, on the game's device as it is created, before the game
 * creates its queries. */
void gfxprobe_hook_create_query(void **vt)
{
    void *fn = vt[offsetof(IDirect3DDevice9Vtbl, CreateQuery) / sizeof(void *)];
    if (MH_CreateHook(fn, (void *)detour_create_query, (void **)&g_orig_create_query) == MH_OK &&
        MH_EnableHook(fn) == MH_OK)
        hg_log("gfxprobe: hooked CreateQuery at %p", fn);
    else
        hg_log("gfxprobe: CreateQuery NOT hooked");
}

static int hook_slot(void **vt, size_t off, void *detour, void **orig, const char *name)
{
    void *target = vt[off / sizeof(void *)];
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        hg_log("gfxprobe: FAILED to hook IDirect3DDevice9::%s at %p", name, target);
        return 0;
    }
    hg_log("gfxprobe: hooked IDirect3DDevice9::%s at %p (slot %u)", name, target,
           (unsigned)(off / sizeof(void *)));
    return 1;
}

static void check_fmt(IDirect3D9 *d3d, UINT adapter, D3DFORMAT adapterfmt, D3DFORMAT f,
                      DWORD usage, D3DRESOURCETYPE rt, const char *label)
{
    HRESULT hr = IDirect3D9_CheckDeviceFormat(d3d, adapter, D3DDEVTYPE_HAL, adapterfmt, usage, rt, f);
    hg_log("gfxprobe: format %-28s %s (hr=0x%08lx)", label,
           SUCCEEDED(hr) ? "SUPPORTED" : "no", (unsigned long)hr);
}

#define FOURCC(a, b, c, d) ((D3DFORMAT)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))

static void probe_device_once(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *s = NULL;
    IDirect3D9 *d3d = NULL;
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DDISPLAYMODE mode;
    D3DSURFACE_DESC d;
    D3DCAPS9 caps;
    char b[16];
    UINT adapter = 0;
    D3DFORMAT afmt = D3DFMT_X8R8G8B8;

    if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &s)) && s) {
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d)))
            hg_log("gfxprobe: back buffer %ux%u %s msaa=%u usage=0x%lx", d.Width, d.Height,
                   fmt_name(d.Format, b), (unsigned)d.MultiSampleType, (unsigned long)d.Usage);
        IDirect3DSurface9_Release(s); s = NULL;
    }
    if (SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface(dev, &s)) && s) {
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d)))
            hg_log("gfxprobe: depth stencil %ux%u %s msaa=%u usage=0x%lx", d.Width, d.Height,
                   fmt_name(d.Format, b), (unsigned)d.MultiSampleType, (unsigned long)d.Usage);
        IDirect3DSurface9_Release(s); s = NULL;
    } else {
        hg_log("gfxprobe: no depth stencil surface bound at EndScene");
    }
    if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp))) adapter = cp.AdapterOrdinal;
    if (SUCCEEDED(IDirect3DDevice9_GetDisplayMode(dev, 0, &mode))) afmt = mode.Format;
    if (SUCCEEDED(IDirect3DDevice9_GetDeviceCaps(dev, &caps)))
        hg_log("gfxprobe: caps vs=0x%08lx ps=0x%08lx maxRTs=%lu maxTexW=%lu ps30=%s mrt_indep_bits=%s",
               (unsigned long)caps.VertexShaderVersion, (unsigned long)caps.PixelShaderVersion,
               (unsigned long)caps.NumSimultaneousRTs, (unsigned long)caps.MaxTextureWidth,
               caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? "yes" : "no",
               (caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) ? "yes" : "no");
    if (SUCCEEDED(IDirect3DDevice9_GetDirect3D(dev, &d3d)) && d3d) {
        check_fmt(d3d, adapter, afmt, FOURCC('I','N','T','Z'), D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, "INTZ depth texture");
        check_fmt(d3d, adapter, afmt, FOURCC('R','A','W','Z'), D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, "RAWZ depth texture");
        check_fmt(d3d, adapter, afmt, FOURCC('D','F','2','4'), D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, "DF24 depth texture");
        check_fmt(d3d, adapter, afmt, FOURCC('D','F','1','6'), D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, "DF16 depth texture");
        check_fmt(d3d, adapter, afmt, FOURCC('N','U','L','L'), D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, "NULL render target");
        check_fmt(d3d, adapter, afmt, D3DFMT_R32F, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, "R32F render target");
        check_fmt(d3d, adapter, afmt, D3DFMT_A16B16G16R16F, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, "A16B16G16R16F render target");
        check_fmt(d3d, adapter, afmt, D3DFMT_D24S8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, "D24S8 depth texture");
        check_fmt(d3d, adapter, afmt, D3DFMT_A8R8G8B8, D3DUSAGE_QUERY_SRGBWRITE | D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, "A8R8G8B8 sRGB write RT");
        IDirect3D9_Release(d3d);
    }
    {
        void **vt = *(void ***)dev;
        hook_slot(vt, offsetof(IDirect3DDevice9Vtbl, SetRenderTarget), (void *)detour_set_rt,
                  (void **)&g_orig_set_rt, "SetRenderTarget");
        hook_slot(vt, offsetof(IDirect3DDevice9Vtbl, SetDepthStencilSurface), (void *)detour_set_ds,
                  (void **)&g_orig_set_ds, "SetDepthStencilSurface");
        hook_slot(vt, offsetof(IDirect3DDevice9Vtbl, DrawIndexedPrimitive), (void *)detour_dip,
                  (void **)&g_orig_dip, "DrawIndexedPrimitive");
        hook_slot(vt, offsetof(IDirect3DDevice9Vtbl, DrawPrimitive), (void *)detour_dp,
                  (void **)&g_orig_dp, "DrawPrimitive");

        hook_slot(vt, offsetof(IDirect3DDevice9Vtbl, Clear), (void *)detour_clear,
                  (void **)&g_orig_clear, "Clear");
    }
}

static void dump_capture(void)
{
    LONG i, n = g_nseg < MAX_SEG ? g_nseg : MAX_SEG;
    char b[16];
    hg_log("gfxprobe: ---- frame %ld capture: %ld events (%s), %ld draws ----", g_frames, g_nseg,
           g_nseg > MAX_SEG ? "TRUNCATED" : "complete", g_frame_draws);
    for (i = 0; i < n; i++) {
        seg *s = &g_seg[i];
        if (s->kind == 'C')
            hg_log("gfxprobe:   clear%s%s%s", (s->idx & D3DCLEAR_TARGET) ? " target" : "",
                   (s->idx & D3DCLEAR_ZBUFFER) ? " z" : "", (s->idx & D3DCLEAR_STENCIL) ? " stencil" : "");
        else if (!s->surf)
            hg_log("gfxprobe:   %s%lu = NULL                              draws=%ld prims=%ld",
                   s->kind == 'R' ? "RT" : "DS", s->kind == 'R' ? s->idx : 0, s->draws, s->prims);
        else
            hg_log("gfxprobe:   %s%lu = %p %ux%u %-14s draws=%ld prims=%ld",
                   s->kind == 'R' ? "RT" : "DS", s->kind == 'R' ? s->idx : 0, s->surf, s->w, s->h,
                   fmt_name(s->fmt, b), s->draws, s->prims);
    }
    hg_log("gfxprobe: ---- shadow pass: %ld calls, ok %ld, E_FAIL %ld, other %ld, player %ld ----",
           g_shadow_calls, g_shadow_rc_ok, g_shadow_rc_fail, g_shadow_rc_other, g_shadow_player_calls);
    hg_log("gfxprobe: nShadowType (e_GetActiveShadowType) = %d%s", hg_gfx_shadow_type(),
           g_force_type2 ? " [forced 2]" : "");
    {
        LONG k, ns = g_nsurf < MAX_SURF ? g_nsurf : MAX_SURF;
        char b[16];
        hg_log("gfxprobe: ---- distinct RT0 / depth surfaces bound since start (%ld) ----", g_nsurf);
        for (k = 0; k < ns; k++)
            hg_log("gfxprobe:   %s %p %5ux%-5u %-14s bound %ld times", g_surf[k].kind == 'R' ? "RT0" : "DS ",
                   g_surf[k].s, g_surf[k].w, g_surf[k].h, fmt_name(g_surf[k].fmt, b), g_surf[k].n);
    }
    {
        /* The render-flag array (e_SetRenderFlag base 0xedfba8) and the
         * definition table (0xad40b8, 0x4c bytes per entry: name[64], type,
         * default, mode-3 default). Print every flag that differs from its
         * default. */
        const int *a = (const int *)(g_image + 0x00ADFBA8u);
        const char *tab = (const char *)(g_image + 0x006D40B8u);
        int k;
        char line[900];
        int len = 0;
        if (!IsBadReadPtr(a, 99 * 4) && !IsBadReadPtr(tab, 99 * 0x4c)) {
            len = snprintf(line, sizeof line, "gfxprobe: render flags (name=value):");
            for (k = 0; k < 99 && len < (int)sizeof line - 40; k++) {
                const char *nm = tab + k * 0x4c;
                int def = *(const int *)(tab + k * 0x4c + 0x44);
                if (a[k] != def)   /* differs from its default */
                    len += snprintf(line + len, sizeof line - len, " %.24s=%d(def %d)", nm, a[k], def);
            }
            hg_log("%s", line);
            hg_log("gfxprobe: render flags shadows=%d wireframe=%d dynamiclights=%d fog=%d shadows_showarea=%d (67/91/22/26/68)",
                   a[67], a[91], a[22], a[26], a[68]);
        }
    }
    hg_log("gfxprobe: ---- effects: %ld created (%ld unknown, %ld overridden) ----",
           g_neffects, g_effects_unknown, g_overrides);
    n = g_neffects < MAX_EFFECTS ? g_neffects : MAX_EFFECTS;
    for (i = 0; i < n; i++)
        hg_log("gfxprobe:   %p %-45s technique requests %ld", (void *)g_effects[i].fx,
               g_effects[i].table >= 0 ? g_fxtable[g_effects[i].table].path : "UNKNOWN", g_req_per_effect[i]);
    hg_log("gfxprobe: ---- techniques set since start (%ld distinct%s) ----", g_ntech,
           g_tech_overflow ? ", table overflowed" : "");
    n = g_ntech < MAX_TECH ? g_ntech : MAX_TECH;
    for (i = 0; i < n; i++) {
        LONG e, ne = g_neffects < MAX_EFFECTS ? g_neffects : MAX_EFFECTS;
        const char *owner = "?";
        for (e = 0; e < ne; e++)
            if (g_effects[e].fx == g_tech[i].fx) { owner = fx_name(g_effects[e].table); break; }
        hg_log("gfxprobe:   %8ld  %-28s %s", g_tech[i].n, owner, g_tech[i].name);
    }
    hg_log("gfxprobe: ---- end of capture ----");
}

/* Render thread, from the overlay's EndScene detour, every frame. */
void gfxprobe_frame(IDirect3DDevice9 *dev)
{
    static LONG probed;
    g_probe_dev = dev;
    InterlockedIncrement(&g_frames);
    strace_frame();
    {
        /* shadow pass alive? a change re-picks every mesh's technique */
        static LONG last_calls, idle;
        LONG c = g_shadow_calls, live;
        idle = c != last_calls ? 0 : idle + 1;
        last_calls = c;
        live = idle < 30;
        if (live != g_shadows_live) {
            int *gen = (int *)(g_image + RVA_TECH_CACHE_GEN);
            InterlockedExchange(&g_shadows_live, live);
            if (g_act_near && !IsBadWritePtr(gen, 4)) InterlockedIncrement((volatile LONG *)gen);
            hg_log("gfxprobe: shadow pass %s", live ? "running: characters get the shadow technique" : "stopped");
        }
    }
    wide_refresh();
    if (InterlockedCompareExchange(&probed, 1, 0) == 0) probe_device_once(dev);
    if (g_force_shadow_flag) shadow_flag_apply();
    if (g_force_type2) {
        int *t = shadow_type_slot(NULL);
        if (t && *t != 2) {
            if (InterlockedIncrement(&g_type2_reforced) <= 5)
                hg_log("gfxprobe: nShadowType went back to %d, re-forcing 2", *t);
            *t = 2;
        }
    }
    if (g_capturing) {
        g_capturing = 0;
        dump_capture();
    }
    /*
     * Automatic capture: the first frame after 900 that follows a frame with
     * more than 300 draws, i.e. a scene and not the menu (frame 901 caught 46
     * draws of loading screen on the first run). One automatic capture only;
     * the flag file asks for more.
     */
    /* Auto capture the frame after one that both ran the shadow pass and
     * drew a scene's worth (the first version fired on a 47-draw loading
     * frame because the player's shadow call had happened earlier). */
    {
        static LONG seen, busy_logged;
        LONG sc = g_shadow_calls;
        int shadow_this_frame = (sc != seen);
        seen = sc;
        /* Frames with off-screen target work: the first few, with counts, to
         * learn when the engine re-renders its shadow maps (the first
         * gameplay capture showed 187 draws on the back buffer only). */
        if (g_frame_rt_changes > 5 && g_frame_draws > 300 && busy_logged < 12) {
            busy_logged++;
            hg_log("gfxprobe: frame %ld: %ld render-target changes, %ld draws, shadow calls this frame %d",
                   g_frames, g_frame_rt_changes, g_frame_draws, shadow_this_frame);
        }
    /* The shadow map is re-rendered every OTHER frame (busy-frame log,
     * 2026-09-22), so arm on a scene frame WITHOUT shadow calls: the next
     * one is the frame that draws the shadow map. */
    if (g_capture_req || (!g_auto_done && !shadow_this_frame && g_frame_draws > 300 && g_frames > 600)) {
        if (!g_capture_req) g_auto_done = 1;
        g_capture_req = 0;
        g_nseg = 0;
        g_frame_draws = 0;
        g_capturing = 1;      /* record the next frame, dump at its EndScene */
    } else {
        g_frame_draws = 0;
    }
    }
    g_frame_rt_changes = 0;
}

/* Panel: surfaces and textures. which: 0 gloss, 1 highlight strength,
 * 2 reflection strength, 3 reflection blur (all x100) */
void hg_gfx_nudge_surf(int which, int d)
{
    volatile LONG *p = which == 0 ? &g_surf_gloss : which == 1 ? &g_surf_spec : which == 2 ? &g_surf_env : &g_surf_blur;
    LONG v = *p + d, lo = which == 0 ? 10 : 0, hi = which == 3 ? 600 : 200;
    if (d == 0) v = which == 3 ? 0 : 100;           /* stock */
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    InterlockedExchange(p, v);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: surfaces gloss %ld%% highlights %ld%% reflections %ld%% blur %.2f",
           g_surf_gloss, g_surf_spec, g_surf_env, g_surf_blur / 100.0f);
}
void hg_gfx_set_surf_indoor(int on)
{
    InterlockedExchange(&g_surf_indoor, on ? 1 : 0);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: surfaces %s", on ? "indoors and outdoors" : "outdoors only");
}
int hg_gfx_surf_indoor(void) { return (int)g_surf_indoor; }

int hg_gfx_surf(int which)
{
    return (int)(which == 0 ? g_surf_gloss : which == 1 ? g_surf_spec : which == 2 ? g_surf_env : g_surf_blur);
}
void hg_gfx_set_aniso(int n)
{
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    InterlockedExchange(&g_aniso, n);
    hg_log("gfxprobe: anisotropic filtering %dx", n);
}
int hg_gfx_aniso(void) { return (int)g_aniso; }
/* which: 0 bump on the sun, 1 bump on the rest (percent) */
void hg_gfx_nudge_detail(int which, int d)
{
    volatile LONG *p = which ? &g_detail_rest : &g_detail_sun;
    LONG v = d ? *p + d : 0;
    InterlockedExchange(p, v < 0 ? 0 : v > 100 ? 100 : v);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: normal-map detail: sun %ld%%, rest %ld%%", g_detail_sun, g_detail_rest);
}
int hg_gfx_detail(int which) { return (int)(which ? g_detail_rest : g_detail_sun); }
void hg_gfx_set_lm_bicubic(int on)
{
    InterlockedExchange(&g_lm_bicubic, on ? 1 : 0);
    hg_log("gfxprobe: bicubic light maps %s", on ? "ON" : "off");
}
int hg_gfx_lm_bicubic(void) { return (int)g_lm_bicubic; }
void hg_gfx_nudge_mip_bias(int d)
{
    LONG v = d ? g_mip_bias + d : 0;
    if (v < -150) v = -150;
    if (v > 100) v = 100;
    InterlockedExchange(&g_mip_bias, v);
    hg_log("gfxprobe: texture mip bias %.2f", v / 100.0f);
}
int hg_gfx_mip_bias(void) { return (int)g_mip_bias; }

/* Panel. */
void hg_gfx_force_shadow_flag(int on)
{
    InterlockedExchange(&g_force_shadow_flag, on ? 1 : 0);
    shadow_flag_apply();
    hg_log("gfxprobe: force shadow render flag %s", on ? "ON" : "off");
}
int hg_gfx_shadow_flag_forced(void) { return (int)g_force_shadow_flag; }

void hg_gfx_set_fill(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    InterlockedExchange(&g_fill_pct, pct);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: shadow fill %d%%", pct);
}

void hg_gfx_set_pcss(int on)
{
    InterlockedExchange(&g_pcss_on, on ? 1 : 0);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: PCSS soft shadows %s (penumbra scale %ld)", on ? "ON" : "off", g_pcss_scale);
}

/* which: 0 outdoor sun size, 1 indoor sun size, 2 depth bias; up/down by 1.5x */
void hg_gfx_scale_pcss(int which, int up)
{
    volatile LONG *p = which == 0 ? &g_pcss_scale : which == 1 ? &g_pcss_scale_in : &g_pcss_bias;
    LONG v = up ? *p * 3 / 2 + 1 : *p * 2 / 3;
    LONG lo = which == 2 ? 0 : 1, hi = which == 2 ? 20000 : 5000;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    InterlockedExchange(p, v);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: PCSS %s %ld", which == 0 ? "sun size outdoor" : which == 1 ? "sun size indoor" : "bias (1e-6/texel)", v);
}

void hg_gfx_nudge_pcss_min(int d)
{
    LONG v = g_pcss_min + d;
    if (v < 1) v = 1;
    if (v > 12) v = 12;
    InterlockedExchange(&g_pcss_min, v);
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: PCSS minimum softness %ld texels", v);
}

/* which: 0 fill, 1 fog start, 2 sun, 3 fill indoors; d in percent. which -1: preset
 * (d = 1 the 2007 look, 0 stock). */
void hg_gfx_nudge_look(int which, int d)
{
    if (which < 0) {
        /* 2007 disc vs 2018 data (LOG 2026-09-22 00:05): ambient x3 and SH on
         * twice as many environments in 2018, fog start 2 m vs 10 m. */
        InterlockedExchange(&g_look_fill, d ? -60 : 0);
        InterlockedExchange(&g_look_fill_in, d ? -60 : 0);
        InterlockedExchange(&g_look_fog, d ? 20 : 0);
        InterlockedExchange(&g_look_sun, d ? 20 : 0);
    } else {
        volatile LONG *p = which == 0 ? &g_look_fill : which == 1 ? &g_look_fog :
                           which == 3 ? &g_look_fill_in : &g_look_sun;
        LONG v = *p + d, lo = which == 1 ? 0 : -90, hi = which == 1 ? 90 : 200;
        InterlockedExchange(p, v < lo ? lo : v > hi ? hi : v);
    }
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: look fill %+ld%% (indoors %+ld%%)  fog start %ld%%  sun %+ld%%", g_look_fill,
           g_look_fill_in, g_look_fog, g_look_sun);
}

/* which: 0 falloff (d toggles), 1 specular (d toggles), 2 strength (d percent) */
void hg_gfx_nudge_pl(int which, int d)
{
    if (which == 0) InterlockedExchange(&g_pl_smooth, !g_pl_smooth);
    else if (which == 1) InterlockedExchange(&g_pl_spec, !g_pl_spec);
    else {
        LONG v = g_pl_pct + d;
        InterlockedExchange(&g_pl_pct, v < 0 ? 0 : v > 400 ? 400 : v);
    }
    InterlockedIncrement(&g_ultra_gen);
    hg_log("gfxprobe: point lights falloff %s  specular %s  strength %ld%%",
           g_pl_smooth ? "smooth" : "linear", g_pl_spec ? "on" : "off", g_pl_pct);
}

void hg_gfx_status(hg_gfx_state *o)
{
    o->pl_smooth = (int)g_pl_smooth;
    o->pl_spec = (int)g_pl_spec;
    o->pl_pct = (int)g_pl_pct;
    o->look_fill = (int)g_look_fill;
    o->look_fill_in = (int)g_look_fill_in;
    o->look_fog = (int)g_look_fog;
    o->look_sun = (int)g_look_sun;
    o->pcss_min = (int)g_pcss_min;
    o->fill_pct = (int)g_fill_pct;
    o->pcss_on = (int)g_pcss_on;
    o->pcss_scale = (int)g_pcss_scale;
    o->pcss_scale_in = (int)g_pcss_scale_in;
    o->pcss_bias = (int)g_pcss_bias;
    o->shadow_type = hg_gfx_shadow_type();
    o->ultra_writes = g_ultra_writes;
    o->overrides = (int)g_overrides;
    o->lights_on = (int)g_lights_on;
    o->n_lit = g_n_lit;            /* technique requests sent to our _pl5 */
    o->n_clamped = g_n_clamped;
}

void hg_gfx_set_lights(int on)
{
    int *gen = (int *)(g_image + RVA_TECH_CACHE_GEN);
    InterlockedExchange(&g_lights_on, on ? 1 : 0);
    InterlockedIncrement(&g_ultra_gen);
    /* Every mesh caches its last technique choices keyed by this generation
     * counter (dxC_EffectGetTechnique); bumping it re-evaluates them all on
     * the next draw, so the toggle is immediate. */
    if (!IsBadWritePtr(gen, 4)) InterlockedIncrement((volatile LONG *)gen);
    hg_log("gfxprobe: per-pixel lights %s (technique caches flushed)", on ? "ON" : "off");
}

/* Worker thread, about once a second: flag file requests a capture. */
void gfxprobe_poll(void)
{
    static WCHAR path[MAX_PATH];
    if (!path[0]) {
        hg_dll_dir(path, MAX_PATH);
        lstrcatW(path, L"\\hellgate_gfxprobe.frame");
    }
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(path);
        g_capture_req = 1;
        hg_log("gfxprobe: frame capture requested");
    }
}

static int hook_export(const char *dll, const char *name, void *detour, void **orig)
{
    HMODULE m = GetModuleHandleA(dll);
    void *target;
    if (!m) m = LoadLibraryA(dll);
    if (!m) { hg_log("gfxprobe: %s not loadable", dll); return 0; }
    target = (void *)GetProcAddress(m, name);
    if (!target) { hg_log("gfxprobe: %s has no %s", dll, name); return 0; }
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        hg_log("gfxprobe: FAILED to hook %s!%s at %p", dll, name, target);
        return 0;
    }
    {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, sizeof path);
        hg_log("gfxprobe: hooked %s!%s at %p (%s)", dll, name, target, path);
    }
    return 1;
}

/* Worker thread, after MH_Initialize. */
void device_install(void);
void postfx_install(unsigned int image);
void brand_install(unsigned int image);
void crashlog_install(void);
void invprobe_install(void);
void uiext_install(void);

void plshadow_settings(void);

/* what a player can change, saved in bin\ultrapatch.ini (src/settings.c) */
static void gfx_settings(void)
{
    settings_var("shadow.fill", &g_fill_pct, 0, 100);
    settings_var("shadow.pcss", &g_pcss_on, 0, 1);
    settings_var("shadow.sun_size_out", &g_pcss_scale, 0, 20000);
    settings_var("shadow.sun_size_in", &g_pcss_scale_in, 0, 20000);
    settings_var("shadow.bias", &g_pcss_bias, 0, 20000);
    settings_var("shadow.min_softness", &g_pcss_min, 1, 16);
    settings_var("shadow.fine_map", &g_cascade, 0, 1);
    settings_var("shadow.characters", &g_act_near, 0, 1);
    settings_var("shadow.character_offset", &g_act_offset, 0, 1000);
    settings_var("shadow.surface_offset", &g_bg_offset, 0, 300);
    settings_var("shadow.wide_every_ms", &g_wide_ms, 200, 60000);
    settings_var("shadow.fine_follow", &g_fine_follow, 0, 40);
    settings_var("look.fill", &g_look_fill, -90, 200);
    settings_var("look.fill_indoors", &g_look_fill_in, -90, 200);
    settings_var("look.fog_start", &g_look_fog, 0, 90);
    settings_var("look.sun", &g_look_sun, -90, 200);
    settings_var("lights.per_pixel", &g_lights_on, 0, 1);
    settings_var("lights.smooth", &g_pl_smooth, 0, 1);
    settings_var("lights.strength", &g_pl_pct, 0, 400);
    settings_var("lights.highlights", &g_pl_spec, 0, 1);
    settings_var("surface.gloss", &g_surf_gloss, 0, 400);
    settings_var("surface.highlight", &g_surf_spec, 0, 400);
    settings_var("surface.reflection", &g_surf_env, 0, 400);
    settings_var("surface.reflection_blur", &g_surf_blur, 0, 800);
    settings_var("surface.indoors", &g_surf_indoor, 0, 1);
    settings_var("texture.anisotropy", &g_aniso, 1, 16);
    settings_var("texture.mip_bias", &g_mip_bias, -300, 300);
    settings_var("detail.sun", &g_detail_sun, 0, 100);
    settings_var("detail.rest", &g_detail_rest, 0, 100);
    settings_var("lightmap.bicubic", &g_lm_bicubic, 0, 1);
    settings_var("particles.light", &g_part_light, 0, 300);
    settings_var("particles.shadow", &g_part_shadow, 0, 100);
    plshadow_settings();
}

void gfxprobe_install(unsigned int image)
{
    g_image = image;
    gfx_settings();
    crashlog_install();
    invprobe_install();                 /* inventory sort spike: logging only */
    uiext_install();                    /* UI XML overrides, our strings and buttons */
    InitializeCriticalSection(&g_tech_cs);
    device_install();                   /* before the game creates its device */
    postfx_install(image);
    brand_install(image);
    patch_shadow_reach(image);
    hg_gfx_set_static_casters((int)settings_get("shadow.static_casters", 2, 0, 2));  /* default: every static model casts */
    hg_gfx_set_stable_casters((int)settings_get("shadow.stable_casters", 1, 0, 1));  /* default: none lost to origin distance or fading */
    settings_watch("shadow.static_casters", &g_static_casters);
    settings_watch("shadow.stable_casters", &g_stable_casters);
    hook_ssmp(image);
    hg_log("gfxprobe: %d effect signatures in table; override root <game>\\override\\", FXN);
    hook_export("d3dx9_34.dll", "D3DXCreateEffect", (void *)detour_create, (void **)&g_orig_create);
    hook_export("d3dx9_34.dll", "D3DXCreateEffectEx", (void *)detour_create_ex, (void **)&g_orig_create_ex);
    hook_export("kernel32.dll", "CreateFileW", (void *)detour_create_file, (void **)&g_orig_create_file);
    {
        static const unsigned char ssig[6] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0 };
        unsigned char *q = (unsigned char *)(image + RVA_RENDER_MODEL_SHADOW);
        if (!IsBadReadPtr(q, 6) && memcmp(q, ssig, 6) == 0 &&
            MH_CreateHook(q, (void *)gfx_shadow_stub, &g_orig_render_shadow) == MH_OK &&
            MH_EnableHook(q) == MH_OK)
            hg_log("gfxprobe: hooked dx9_RenderModelShadow at %p", q);
        else
            hg_log("gfxprobe: shadow probe not installed");
    }
    {
        static const unsigned char nsig[6] = { 0x83, 0xec, 0x18, 0x53, 0x55, 0x56 };   /* sub esp,18; push ebx/ebp/esi */
        unsigned char *q = (unsigned char *)(image + RVA_SHADOW_BUFFERS_NEW);
        g_type2_mode = hg_flagfile(L"hellgate_shadowtype2.off") ? -1 : 1;
        if (!IsBadReadPtr(q, 6) && memcmp(q, nsig, 6) == 0 &&
            MH_CreateHook(q, (void *)detour_shadow_new, (void **)&g_orig_shadow_new) == MH_OK &&
            MH_EnableHook(q) == MH_OK)
            hg_log("gfxprobe: hooked shadow buffer creation at %p (colour shadow map: %s)", q,
                   g_type2_mode > 0 ? "always (R32F)" : "never (hellgate_shadowtype2.off)");
        else
            hg_log("gfxprobe: shadow buffer creation hook not installed");
    }
    {
        static const unsigned char sig[5] = { 0x55, 0x8b, 0xec, 0x83, 0xec };   /* push ebp; mov ebp,esp; sub esp,.. */
        unsigned char *p = (unsigned char *)(image + RVA_TECH_BY_FEATURES);
        if (!IsBadReadPtr(p, 5) && memcmp(p, sig, 3) == 0 &&
            MH_CreateHook(p, (void *)detour_tech_by_feat, (void **)&g_orig_tech_by_feat) == MH_OK &&
            MH_EnableHook(p) == MH_OK)
            hg_log("gfxprobe: hooked dxC_EffectGetTechniqueByFeatures at %p", p);
        else
            hg_log("gfxprobe: technique-request probe not installed (bytes %02x %02x %02x at %p)", p[0], p[1], p[2], p);
    }
}

long hg_shadow_calls(void)        { return g_shadow_calls; }
long hg_shadow_player_calls(void) { return g_shadow_player_calls; }
