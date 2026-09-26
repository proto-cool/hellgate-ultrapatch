/*
 * Shadows from a point light: a fire, a torch or a spell casts them, you
 * included, when you are near it.
 *
 * The engine has directional shadows only (dxC_ShadowBufferSetupDirectional;
 * even the indoor one is aimed from a light's direction). This adds one cube
 * shadow map for the strongest engine point light near the camera:
 *
 * Casters. The engine redraws its near shadow map (27 units around the
 * player: characters and props) with shadowmap.fxo, whose vertex shaders
 * take View and Projection as plain constants (rigid: c4-c7, c8-c11;
 * skinned: c184-c187, c188-c191, after 180 bone registers), stored
 * transposed. Every caster draw of that pass is recorded in a cache (see
 * "The caster cache" below), and at Present the cube around the light is
 * drawn whole from it with only those eight registers changed: the engine's
 * own shaders still do the skinning and the cut-outs, and write z/w of our
 * projection. The pass is recognised by its orthographic projection's width
 * (2 / c8.x) against the near map's reach.
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
#include <stdlib.h>
#include <string.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include "panel.h"

IDirect3DDevice9 *device_get(void);
int fpview_eye(float out[3]);
ID3DXEffect *postfx_load(IDirect3DDevice9 *dev, const WCHAR *name);
void *hg_player_level(void);
/* the level's geometry as casters (see "the level's geometry" below) */
#define LHASH 16384
static int g_nl, g_lh[LHASH];           /* pieces kept; hash of index + 1, 0 empty */
static LONG g_l_skip;                   /* level draws not kept (a format we do not read, a dynamic buffer, full) */
static void *g_level;                   /* the level they belong to */
static D3DXHANDLE g_cfx_w, g_cfx_vp;    /* plcast.fxo's parameters (g_cfx) */
static int g_l_drawn;                   /* pieces in the last cube */
#define LREJ 8192
static unsigned g_lrej[LREJ];           /* level draws not kept, by key */
static int g_nlrej;
float gfxprobe_near_reach(void);

#define PLS_SIZE 1024           /* cube face size (512 read blurry, 2026-09-24) */
/* near plane: 0.6 units, so the light's own housing does not cast (a fire
 * in a barrel shadowed everything but a wedge, first in-game run) */
#define PLS_NEAR 0.6f
#define MAX_LIGHTS 32
/* How long a light outlives its last sighting in the effects' list (fog,
 * light spill, lit particles). Lights arrive only with the meshes drawn, so
 * turning the camera off a lamp's walls dropped it within 30 frames and
 * turning back faded it in again: the light spill flickered as the camera
 * moved or turned (2026-09-24). Now about 2 s, the last 1 s a fade. */
#define LINGER 120
#define LINGER_FADE 60

static volatile LONG g_on = 1;           /* on (the user, 2026-09-23; off 2026-09-22 as too much for this engine) */
static volatile LONG g_bias = 5;         /* depth bias, world units x100 */
static volatile LONG g_soft = 12;        /* filter radius, x1000 of the distance (1.2%; 4% was "too blurry", 2026-09-24) */

/* per device */
static IDirect3DCubeTexture9 *g_cube;
static IDirect3DSurface9 *g_face[6], *g_ds;
static IDirect3DDevice9 *g_dev;
static int g_failed;

/* the light */
/* follow: a light that moves with the camera (the player's own light): it
 * lit everything the camera looked at, and cast the cube's shadows from
 * the camera, so it is left out of every effect here (see follow_track) */
/* scol, srad: the colour and reach handed to the effects, eased */
static struct { float pos[3], col[3], lum, radius, fw, fsh, last[3], rel[3], scol[3], srad; LONG seen, first; int fol, follow; } g_lights[MAX_LIGHTS];
static int g_nlights;
static LONG g_frame;
static float g_eye[3];
static float g_focus[3];                /* what the camera looks at (src/postfx.c focus) */
static LONG g_focus_frame = -1000;
static int g_have_eye;
static int g_active;                     /* a light is chosen */
static float g_str;                      /* its shadow's strength, 0..1: fades, never pops */
static float g_lpos[3], g_lfar;
static volatile LONG g_params_gen = 1;   /* bumped when the light changes */

/* saved in bin\ultrapatch.ini (src/settings.c); from gfxprobe_install */
void plshadow_settings(void)
{
    settings_var("pointshadow.on", &g_on, 0, 1);
    settings_var("pointshadow.bias", &g_bias, 0, 100);
    settings_var("pointshadow.softness", &g_soft, 0, 200);
}
static LONG g_casts, g_replays;         /* cube draws; casters in the last one */
static int g_nc;                        /* casters cached */
static LONG g_st_dyn;                   /* caster draws skipped: dynamic buffers (refilled each frame) */
static LONG g_cast_mark;        /* g_casts when the cube last moved to another light */
static void cache_sweep(void);
static LONG g_st_frames, g_st_active, g_st_switch;   /* per-second log */
static LONG g_st_noeye, g_st_kept;                  /* frames without the camera; the light kept though nothing qualified */

/* the pass: 0 unknown for this render target, 1 near map, -1 other */
static int g_pass;
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

static void cache_flush(void);
static void level_flush(void);
static ID3DXEffect *g_cfx;
static int g_cfx_failed;
void plshadow_reset(void)
{
    cache_flush();
    level_flush();
    if (g_cfx) { g_cfx->lpVtbl->Release(g_cfx); g_cfx = NULL; }
    g_cfx_failed = 0;
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

/* hp, hc, hf: the effect's point-light parameters (gfxprobe caches them);
 * every draw is read, as the volumetric fog glows around these lights too
 * and reading only a frame's first draws lost lights indoors */
void plshadow_collect(ID3DXEffect *fx, D3DXHANDLE hp, D3DXHANDLE hc, D3DXHANDLE hf)
{
    D3DXVECTOR4 pos[5], col[5], fal[5];
    D3DXHANDLE he;
    int k;
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
            if (dx * dx + dy * dy + dz * dz < 0.25f) break;         /* a flickering fire moves */
            if (g_lights[i].fol > 0 && g_have_eye) {                /* a follower: the same place relative to the camera */
                dx = g_lights[i].rel[0] - (pos[k].x - g_eye[0]);
                dy = g_lights[i].rel[1] - (pos[k].y - g_eye[1]);
                dz = g_lights[i].rel[2] - (pos[k].z - g_eye[2]);
                if (dx * dx + dy * dy + dz * dz < 0.25f) break;
            }
        }
        if (i == g_nlights) {
            if (g_nlights < MAX_LIGHTS) g_nlights++;
            else {                                                           /* replace the stalest */
                int j;
                for (i = 0, j = 1; j < MAX_LIGHTS; j++) if (g_lights[j].seen < g_lights[i].seen) i = j;
            }
            g_lights[i].seen = -1000;                                        /* new: counts as long unseen */
            g_lights[i].fw = g_lights[i].fsh = 0;
            g_lights[i].fol = g_lights[i].follow = 0;
            g_lights[i].last[0] = pos[k].x; g_lights[i].last[1] = pos[k].y; g_lights[i].last[2] = pos[k].z;
        }
        /* first seen, or back after 30 frames: the fog's halo fades in from here */
        if (g_frame - g_lights[i].seen > LINGER) g_lights[i].first = g_frame;
        g_lights[i].pos[0] = pos[k].x; g_lights[i].pos[1] = pos[k].y; g_lights[i].pos[2] = pos[k].z;
        /* each mesh carries its own copy of the light, and the copies differ;
         * the last one drawn won, so a lamp in plain view flickered as the
         * camera moved and the meshes drawn changed (2026-09-24): the
         * brightest copy of the frame instead */
        if (g_lights[i].seen != g_frame || lum > g_lights[i].lum) {
            g_lights[i].col[0] = col[k].x; g_lights[i].col[1] = col[k].y; g_lights[i].col[2] = col[k].z;
            g_lights[i].lum = lum;
        }
        if (g_lights[i].seen != g_frame || radius > g_lights[i].radius) g_lights[i].radius = radius;
        if (g_lights[i].seen < 0) {                                          /* new: nothing to ease from */
            memcpy(g_lights[i].scol, g_lights[i].col, sizeof g_lights[i].scol);
            g_lights[i].srad = g_lights[i].radius;
        }
        g_lights[i].seen = g_frame;
        if (g_have_eye) {
            g_lights[i].rel[0] = pos[k].x - g_eye[0]; g_lights[i].rel[1] = pos[k].y - g_eye[1]; g_lights[i].rel[2] = pos[k].z - g_eye[2];
        }
    }
}

/* Once a frame, before the choice: which lights move with the camera. When
 * the camera moves, a light that moved the same way (within 30%) scores up;
 * a light that moves on its own (a fire's flicker, a spell) scores down.
 * A lamp that stands still never scores, so it can never be taken for one;
 * the player's light standing still while the camera orbits keeps its
 * score. A follower from 10, no longer one at 3 or below. */
static void follow_track(void)
{
    static float pe[3];
    static int pe_ok;
    float de[3], le;
    int i;
    if (!g_have_eye) return;
    de[0] = g_eye[0] - pe[0]; de[1] = g_eye[1] - pe[1]; de[2] = g_eye[2] - pe[2];
    le = sqrtf(de[0] * de[0] + de[1] * de[1] + de[2] * de[2]);
    for (i = 0; i < g_nlights && pe_ok; i++) {
        float dl[3], ll, dx, dy, dz;
        if (g_lights[i].seen != g_frame) continue;
        dl[0] = g_lights[i].pos[0] - g_lights[i].last[0];
        dl[1] = g_lights[i].pos[1] - g_lights[i].last[1];
        dl[2] = g_lights[i].pos[2] - g_lights[i].last[2];
        ll = sqrtf(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]);
        dx = dl[0] - de[0]; dy = dl[1] - de[1]; dz = dl[2] - de[2];
        if (le > 0.02f && ll > 0.02f && sqrtf(dx * dx + dy * dy + dz * dz) < 0.3f * le + 0.01f) {
            if (g_lights[i].fol < 30) g_lights[i].fol++;
        } else if (ll > 0.05f) {
            if (g_lights[i].fol > 0) g_lights[i].fol--;
        }
        if (!g_lights[i].follow && g_lights[i].fol >= 10) {
            g_lights[i].follow = 1;
            hg_log("plshadow: the light at %.1f %.1f %.1f (reach %.1f) follows the camera: left out of the effects",
                   g_lights[i].pos[0], g_lights[i].pos[1], g_lights[i].pos[2], g_lights[i].radius);
        } else if (g_lights[i].follow && g_lights[i].fol <= 3) {
            g_lights[i].follow = 0;
        }
    }
    for (i = 0; i < g_nlights; i++) memcpy(g_lights[i].last, g_lights[i].pos, sizeof g_lights[i].last);
    memcpy(pe, g_eye, sizeof pe);
    pe_ok = 1;
}

void plshadow_focus(const float p[3])
{
    /* eased (a quarter-second or so), so one frame's depth cannot jerk the
     * choice; a jump of over 10 units (a teleport, a cut) is taken at once */
    float dx = p[0] - g_focus[0], dy = p[1] - g_focus[1], dz = p[2] - g_focus[2];
    if (g_frame - g_focus_frame > 10 || dx * dx + dy * dy + dz * dz > 100.0f) memcpy(g_focus, p, sizeof g_focus);
    else { g_focus[0] += dx * 0.15f; g_focus[1] += dy * 0.15f; g_focus[2] += dz * 0.15f; }
    g_focus_frame = g_frame;
}

/* At Present: choose the light for the next frame.
 *
 * By distance, not brightness: a fire's brightness flickers by design, and
 * scoring on it switched between two barrels once or twice a second even
 * with hysteresis (the log's per-second line). A different light takes over
 * only after being clearly nearer (20%) for 30 frames in a row. */
/* 90 frames and 35% nearer when the choice was measured from the camera:
 * at 30 and 20% the shadow hopped between a row of ceiling lamps as the
 * camera looked about. Measured from the player (the focus, held through
 * gaps) it can follow them: "react faster to player presence" (2026-09-24). */
#define SWITCH_FRAMES 30
static LONG g_settle_until;            /* frames until which the best light wins at once (settling) */
#define FOCUS_HOLD 120      /* frames a focus stays usable after its last update */
#define SWITCH_RATIO 1.2f
void plshadow_frame(void)
{
    static int cand = -1, cand_frames;
    int i, best = -1, cur = -1;
    float best_score = 0, cur_score = 0;
    cache_sweep();
    follow_track();
    g_frame++;
    /* the camera from the render context when no point-lit draw carried
     * EyeInWorld: character select and menus had none most frames (43 of
     * 85, 107 of 109), and with no camera no light was chosen and the
     * shadow came and went (2026-09-26) */
    if (!g_have_eye && fpview_eye(g_eye)) g_have_eye = 1;
    {   /* the level's pieces belong to the level they were drawn in */
        void *lv = hg_player_level();
        if (lv != g_level) { level_flush(); g_level = lv; }
    }
    if (g_on && g_have_eye) {
        /* measured from what the camera looks at (the player, in the
         * third-person view), not from the camera: the camera rides up near
         * the ceiling, and the lamp nearest it threw the player's shadow
         * forward from above and behind, onto floor that lamp barely lit,
         * hopping lamp to lamp as the camera moved (2026-09-24) */
        /* The focus is measured only on frames with the linear depth and
         * the camera both known, and some are without: after 10 frames the
         * choice fell back to the camera, which rides behind and above the
         * player, and a small light 7 units off took the shadow for a second
         * at a time, the player out of its reach (Covent Garden depot,
         * 2026-09-24). The last focus holds for 2 s (the player is still
         * about there), and no light is changed on a stale one. */
        int age = g_frame - g_focus_frame;
        int foc = age <= FOCUS_HOLD, fresh = age <= 10;
        const float *ref = foc ? g_focus : g_eye;
        for (i = 0; i < g_nlights; i++) {
            float dx, dy, dz, d, reach, score;
            if (g_frame - g_lights[i].seen > 30) continue;                   /* gone */
            if (g_lights[i].radius < 1.5f) continue;                         /* a glint, not a fire */
            if (g_lights[i].follow) continue;                                /* the camera's own: shadows from the camera */
            dx = g_lights[i].pos[0] - ref[0]; dy = g_lights[i].pos[1] - ref[1]; dz = g_lights[i].pos[2] - ref[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            reach = g_lights[i].radius + (foc ? 1.0f : 6.0f);               /* from the camera: it sits behind the player */
            if (d > reach) continue;
            score = 1.0f - d / reach;
            if (g_active) {
                float cx = g_lights[i].pos[0] - g_lpos[0], cy = g_lights[i].pos[1] - g_lpos[1], cz = g_lights[i].pos[2] - g_lpos[2];
                if (cx * cx + cy * cy + cz * cz < 0.25f) { cur = i; cur_score = score; }
            }
            if (score > best_score) { best_score = score; best = i; }
        }
        /* Settling: for 1.5 s after the known lights grew (a scene just
         * entered), the best light wins at once. The first choice is made in
         * the first frame or two with part of the lights known, and the rule
         * below kept it: on the character select a lamp two units from the
         * right one held the shadow until the option was toggled, which
         * chose afresh with all eleven known (2026-09-26). */
        {
            static int last_n;
            if (g_nlights > last_n) g_settle_until = g_frame + 90;
            last_n = g_nlights;
        }
        if (cur >= 0 && best != cur && g_frame < g_settle_until && best_score > cur_score * 1.02f) {
            cand = -1;
            cand_frames = 0;
        /* keep the current light unless another has been clearly nearer for
         * a while; with a focus held but stale, hold. Without any focus (the
         * camera's position; the character select never has one) the ratio
         * rule below still applies: holding there froze the first choice */
        } else if (cur >= 0 && best != cur && !fresh && foc) {
            best = cur;                                  /* stale focus: hold */
            cand = -1;
            cand_frames = 0;
        } else if (cur >= 0 && best != cur) {
            if (best_score > cur_score * SWITCH_RATIO && best == cand) {
                if (++cand_frames < SWITCH_FRAMES) best = cur;
            } else {
                cand = best_score > cur_score * SWITCH_RATIO ? best : -1;
                cand_frames = 1;
                best = cur;
            }
        } else {
            cand = -1;
            cand_frames = 0;
        }
    } else if (g_on) {
        g_st_noeye++;
    }
    /* A frame without the camera, or without the current light among the
     * candidates, lets go of nothing: the lamp is only reported with the
     * meshes drawn, and those gaps faded a fixed lamp's shadow out and in
     * every second or two on the character select (the log, 2026-09-24).
     * The current light is kept while it was seen within LINGER frames. */
    if (best < 0 && g_on && g_active) {
        for (i = 0; i < g_nlights; i++) {
            float cx = g_lights[i].pos[0] - g_lpos[0], cy = g_lights[i].pos[1] - g_lpos[1], cz = g_lights[i].pos[2] - g_lpos[2];
            if (cx * cx + cy * cy + cz * cz < 0.25f && g_frame - g_lights[i].seen <= LINGER && !g_lights[i].follow) {
                best = i;
                g_st_kept++;
                break;
            }
        }
    }
    {
        static DWORD last;
        static LONG last_casts;
        DWORD now = GetTickCount();
        g_st_frames++;
        if (g_active) g_st_active++;
        if (now - last >= 1000) {
            if (g_st_active || g_st_switch)
                hg_log("plshadow: %ld frames, %ld with a light, %ld light changes, %ld cube redraws, %d lights known; "
                       "%ld without the camera, %ld kept the light through a gap; %d casters cached, %ld in the cube, "
                       "%ld dynamic skipped; level: %d pieces kept, %d in the cube, %ld not kept",
                       g_st_frames, g_st_active, g_st_switch, g_casts - last_casts, g_nlights, g_st_noeye, g_st_kept,
                       g_nc, g_replays, g_st_dyn, g_nl, g_l_drawn, g_l_skip);
            last = now; last_casts = g_casts;
            g_st_frames = g_st_active = g_st_switch = g_st_noeye = g_st_kept = g_st_dyn = 0;
        }
    }
    /* A change of light crossfades: the current shadow fades out over about
     * 8 frames, the cube moves, and the new one fades in. Switching at once
     * made one set of shadows vanish and another appear in a frame, up to
     * twice a second outdoors among the street lamps (the log). */
    {
        int same = 0;
        float want_str;
        if (best >= 0 && g_active) {
            float *p = g_lights[best].pos;
            float dx = p[0] - g_lpos[0], dy = p[1] - g_lpos[1], dz = p[2] - g_lpos[2];
            same = dx * dx + dy * dy + dz * dz < 1.0f;
        }
        if (!g_active && best >= 0) g_str = 0;                  /* nothing to fade out */
        if (g_active && !same) {
            want_str = 0;                                        /* fade the old one out first */
        } else {
            /* only once the cube has been drawn for this light (at this
             * Present, from the cache): a new light's shadow faded in from
             * the last light's cube, or none (2026-09-24) */
            want_str = best >= 0 && g_casts > g_cast_mark ? 1.0f : 0.0f;
            /* and by how far into the light's reach the player stands: full
             * from a third of the way in, nothing at its edge, so walking up
             * to a lamp the shadow grows with every step instead of
             * switching on a moment late ("react faster to player
             * presence", 2026-09-24) */
            if (want_str > 0) {
                const float *ref = g_frame - g_focus_frame <= FOCUS_HOLD ? g_focus : g_eye;
                float *lp = g_lights[best].pos, reach = g_lights[best].radius + 1.0f;
                float dx = lp[0] - ref[0], dy = lp[1] - ref[1], dz = lp[2] - ref[2];
                float prox = (reach - sqrtf(dx * dx + dy * dy + dz * dz)) / (0.35f * reach);
                want_str *= prox < 0 ? 0 : prox > 1 ? 1 : prox;
            }
        }
        if (g_active && !same) {
            /* changing light: out at a fixed pace, then in */
            if (g_str > want_str) g_str = g_str - 0.125f < want_str ? want_str : g_str - 0.125f;
        } else {
            /* the same light: ease (about a quarter-second), no steps */
            g_str += (want_str - g_str) * 0.12f;
            if (fabsf(want_str - g_str) < 0.002f) g_str = want_str;
        }
        if (g_active && !same && g_str <= 0) {                   /* faded out: let go */
            g_active = 0;
            g_st_switch++;
        }
        if (best >= 0 && (!g_active || same)) {
            float *p = g_lights[best].pos;
            float dx = p[0] - g_lpos[0], dy = p[1] - g_lpos[1], dz = p[2] - g_lpos[2];
            /* sticky: a flickering fire jitters every frame; following each
             * jitter redrew the cube every frame and made the receivers and
             * the cube disagree on alternate frames */
            if (!g_active || dx * dx + dy * dy + dz * dz > 0.01f || fabsf(g_lfar - g_lights[best].radius) > 0.5f) {
                if (!g_active) g_cast_mark = g_casts;      /* another light: its cube is not drawn yet */
                if (!g_active)
                    hg_log("plshadow: light at %.1f %.1f %.1f, reach %.1f (camera at %.1f %.1f %.1f, looking at %.1f %.1f %.1f%s)",
                           p[0], p[1], p[2], g_lights[best].radius, g_eye[0], g_eye[1], g_eye[2],
                           g_focus[0], g_focus[1], g_focus[2], g_frame - g_focus_frame <= 10 ? "" :
                           g_frame - g_focus_frame <= FOCUS_HOLD ? ", held" : ", stale");
                memcpy(g_lpos, p, sizeof g_lpos);
                g_lfar = g_lights[best].radius;
                g_active = 1;
                InterlockedIncrement(&g_params_gen);
            }
        }
        {
            /* in steps: eased every frame, a change of generation every
             * frame had every effect's knobs rewritten every frame */
            static float last_str = -1;
            if (fabsf(g_str - last_str) >= 0.02f || (g_str != last_str && (g_str == 0 || g_str == 1))) {
                last_str = g_str;
                InterlockedIncrement(&g_params_gen);
            }
        }
    }
    g_have_eye = 0;
}

/* For the volumetric fog, once a frame: the lights to glow, up to max,
 * strongest first. pr: position and reach; col: colour times its weight,
 * and in [3] how much it is the shadowing light.
 *
 * The nearest 8 lights (fires and lamps: reach 3 units or more) within
 * `margin` of their reach are the target; each light's weight eases towards
 * 1 or 0 over about 10 frames, so a light dropping out of the nearest 8 (the
 * list reshuffled as you walked) fades instead of popping. The shadowing
 * share is the shadow's own crossfade strength. The weight is
 * also faded in over 20 frames from first seen, out over the last 10 unseen,
 * and down over the outer 10 units of the margin. */
#define FOG_TARGET 10
#define FOG_KEEP 12         /* a light in the target stays until it drops past this rank */
static int lights_near(const float eye[3], float margin, float (*pr)[4], float (*col)[4], int max);

/* Once a frame (the weights ease per call): the particles ask first, the
 * fog later in the same frame gets the same list. */
int plshadow_lights_near(const float eye[3], float margin, float (*pr)[4], float (*col)[4], int max)
{
    static LONG frame = -1;
    static float cpr[12][4], ccol[12][4];
    static int cn;
    if (frame != g_frame) {
        frame = g_frame;
        cn = lights_near(eye, margin, cpr, ccol, 12);
    }
    if (max > cn) max = cn;
    memcpy(pr, cpr, max * sizeof cpr[0]);
    memcpy(col, ccol, max * sizeof ccol[0]);
    return max;
}

static int lights_near(const float eye[3], float margin, float (*pr)[4], float (*col)[4], int max)
{
    float d[MAX_LIGHTS];
    int idx[MAX_LIGHTS], n = 0, i, j, out = 0;
    /* once a frame: the colour and reach the effects see ease towards the
     * frame's brightest copy (a quarter-second or so), so what is left of
     * the per-mesh differences does not flicker either */
    for (i = 0; i < g_nlights; i++) {
        for (j = 0; j < 3; j++) g_lights[i].scol[j] += (g_lights[i].col[j] - g_lights[i].scol[j]) * 0.15f;
        g_lights[i].srad += (g_lights[i].radius - g_lights[i].srad) * 0.15f;
    }
    for (i = 0; i < g_nlights; i++) {
        float dx = g_lights[i].pos[0] - eye[0], dy = g_lights[i].pos[1] - eye[1], dz = g_lights[i].pos[2] - eye[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        d[i] = dist;
        if (g_frame - g_lights[i].seen > LINGER || g_lights[i].radius < 3.0f) continue;
        if (g_lights[i].follow) continue;                   /* the camera's own light */
        if (dist > g_lights[i].radius + margin) continue;
        for (j = n; j > 0 && d[idx[j - 1]] > dist; j--) idx[j] = idx[j - 1];
        idx[j] = i; n++;
    }
    for (i = 0; i < g_nlights; i++) {
        float cx = g_lights[i].pos[0] - g_lpos[0], cy = g_lights[i].pos[1] - g_lpos[1], cz = g_lights[i].pos[2] - g_lpos[2];
        float t = 0, s = g_on && g_active && g_cube && cx * cx + cy * cy + cz * cz < 0.25f ? 1.0f : 0.0f;
        /* hysteresis and a half-second ease: a far lamp swapping in and out
         * of the nearest 8 as you walked, over 10 frames, flickered the light
         * spill on distant walls (2026-09-24) */
        for (j = 0; j < n && j < FOG_KEEP; j++)
            if (idx[j] == i && (j < FOG_TARGET || g_lights[i].fw > 0.5f)) t = 1;
        g_lights[i].fw += (t - g_lights[i].fw) * 0.05f;
        if (t == 0 && g_lights[i].fw < 0.01f) g_lights[i].fw = 0;
        /* eases in only: once the cube moves to another light it no longer
         * holds this one's shadows, so reading it would be garbage */
        g_lights[i].fsh = s > 0 ? g_str : 0.0f;
    }
    /* every light with weight, strongest first */
    n = 0;
    for (i = 0; i < g_nlights; i++) {
        if (g_lights[i].fw <= 0) continue;
        for (j = n; j > 0 && g_lights[idx[j - 1]].fw < g_lights[i].fw; j--) idx[j] = idx[j - 1];
        idx[j] = i; n++;
    }
    for (j = 0; j < n && out < max; j++) {
        int k = idx[j];
        float age = (float)(g_frame - g_lights[k].seen), life = (float)(g_frame - g_lights[k].first);
        float f = age <= LINGER - LINGER_FADE ? 1.0f : (LINGER - age) / (float)LINGER_FADE, fin = life >= 20 ? 1.0f : life / 20.0f;
        float edge = (g_lights[k].radius + margin - d[k]) / 10.0f;
        f *= fin * (edge < 0 ? 0 : edge > 1 ? 1 : edge) * g_lights[k].fw;
        if (f <= 0) continue;
        pr[out][0] = g_lights[k].pos[0]; pr[out][1] = g_lights[k].pos[1]; pr[out][2] = g_lights[k].pos[2];
        pr[out][3] = g_lights[k].srad;
        col[out][0] = g_lights[k].scol[0] * f; col[out][1] = g_lights[k].scol[1] * f; col[out][2] = g_lights[k].scol[2] * f;
        col[out][3] = g_lights[k].fsh;
        out++;
    }
    return out;
}

/* The receiver knobs (gfxprobe's ultra_apply); returns the generation. */
LONG plshadow_params(float pls[4], float pls2[4])
{
    float f = g_lfar, n = PLS_NEAR;
    int on = g_on && g_active && g_cube;
    pls[0] = g_lpos[0]; pls[1] = g_lpos[1]; pls[2] = g_lpos[2]; pls[3] = on ? g_str : 0.0f;
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
int plshadow_caster_kind(void) { return g_sm_kind; }     /* 1 rigid, 2 skinned, 0 not known */
void plshadow_rt_changed(void) { g_pass = 0; }

typedef HRESULT (STDMETHODCALLTYPE *dip_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

/*
 * The caster cache. The cube used to be filled as a side effect of the
 * engine's near-map pass, and that pass runs only when something in it
 * changed: a quiet scene never refilled the cube, a pass that drew only
 * part of the casters left part of a cube, a pass the width test missed
 * left none, and casters the engine keeps out of its 27 units were never
 * in (2026-09-24). Now every caster that pass draws is recorded here with
 * all it needs to be drawn again (shaders, buffers, its cut-out texture,
 * its constants), and at Present the whole cube is drawn from the cache,
 * whenever the light or a caster in its reach changed. Never a part.
 *
 * shadowmap.fxo's colour techniques read World c0-c3 (rigid) or 180 bone
 * registers and World c180-c183 (skinned); View and Projection follow
 * (c4-c11 / c184-c191) and are ours. The pixel shader has no constants,
 * only the diffuse map's alpha for the cut-out (texkill).
 *
 * A caster is known by its buffers, draw range, shaders and texture; the
 * same mesh drawn twice (a row of barrels, two zombies) is told apart by
 * where it stands, the nearest match taking each draw, so a moving one is
 * updated in place, never duplicated.
 */
#define MAXC 1536                   /* 768 filled up in a station (2026-09-25) */
#define MAXS 4                      /* vertex streams kept */
#define CREGS 184                   /* skinned: bones and World */
typedef struct {
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexBuffer9 *vb[MAXS];
    UINT off[MAXS], stride[MAXS];
    IDirect3DIndexBuffer9 *ib;
    IDirect3DBaseTexture9 *tex;
    DWORD samp[5], cull;
    D3DPRIMITIVETYPE t;
    INT bv;
    UINT mi, nv, si, pc;
    int kind, nreg;                 /* 1 rigid (4 registers), 2 skinned (184) */
    float pos[3];                   /* where it stands: World (x a skinned mesh's first bone) */
    LONG seen, pass;                /* frame last drawn by the engine; its pass */
    float c[CREGS * 4];
} caster;
static caster g_c[MAXC];
static dip_fn g_draw;               /* the device's own DrawIndexedPrimitive */
static IDirect3DStateBlock9 *g_sb;
static int g_dirty = 1;             /* the cube needs drawing */
static float g_drawn_pos[3], g_drawn_far = -1;   /* the light it was drawn for */
static LONG g_pass_id, g_pass_frame = -1, g_pass_claims, g_pass_seen_frame = -1;
static float g_pass_vp[8][4];       /* the near map's View, Projection (its box) */
static LONG g_pass_hist[8];         /* casters in the last passes */

static const DWORD k_samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER };

static void caster_free(caster *e)
{
    int i;
    if (e->vs) IDirect3DVertexShader9_Release(e->vs);
    if (e->ps) IDirect3DPixelShader9_Release(e->ps);
    if (e->decl) IDirect3DVertexDeclaration9_Release(e->decl);
    for (i = 0; i < MAXS; i++) if (e->vb[i]) IDirect3DVertexBuffer9_Release(e->vb[i]);
    if (e->ib) IDirect3DIndexBuffer9_Release(e->ib);
    if (e->tex) IDirect3DBaseTexture9_Release(e->tex);
    memset(e, 0, sizeof *e - sizeof e->c);
}

static void caster_drop(int i)
{
    caster_free(&g_c[i]);
    if (i != --g_nc) memcpy(&g_c[i], &g_c[g_nc], sizeof g_c[i]);
    memset(&g_c[g_nc], 0, sizeof g_c[g_nc] - sizeof g_c[g_nc].c);
}

/* the buffers and shaders must go before the device resets (a D3DPOOL_DEFAULT
 * buffer kept alive fails the Reset) */
static void cache_flush(void)
{
    while (g_nc) caster_drop(g_nc - 1);
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
    g_dirty = 1;
}

/* A mesh's origin can lie off its geometry, so a caster whose origin is a
 * little beyond the light's reach can still cast into it. The margin was 40
 * units when each engine draw was re-issued: with the cache that took in a
 * whole street, 700 casters with 180-390 of them drawn six times a frame,
 * and the game fell to 15-30 frames a second (Covent Garden, 2026-09-24). */
#define REACH_MARGIN 12.0f
#define CUBE_MAX 128        /* the most casters drawn into the cube: the nearest the light */
static float reach_d2(const float p[3])
{
    float dx = p[0] - g_lpos[0], dy = p[1] - g_lpos[1], dz = p[2] - g_lpos[2];
    return dx * dx + dy * dy + dz * dz;
}
static int in_reach(const float p[3])
{
    float r = g_lfar + REACH_MARGIN;
    return g_active && reach_d2(p) <= r * r;
}

static void caster_pos(const float *c, int kind, float out[3])
{
    const float *w = kind == 2 ? c + 180 * 4 : c;       /* World, transposed: rows are registers */
    float b[4] = { 0, 0, 0, 1 };
    int i;
    if (kind == 2) { b[0] = c[3]; b[1] = c[7]; b[2] = c[11]; }   /* bone 0's translation */
    for (i = 0; i < 3; i++) out[i] = w[i * 4] * b[0] + w[i * 4 + 1] * b[1] + w[i * 4 + 2] * b[2] + w[i * 4 + 3] * b[3];
}

static int buffer_dynamic(IDirect3DVertexBuffer9 *vb, IDirect3DIndexBuffer9 *ib)
{
    D3DVERTEXBUFFER_DESC vd;
    D3DINDEXBUFFER_DESC id;
    if (vb && SUCCEEDED(IDirect3DVertexBuffer9_GetDesc(vb, &vd)) && (vd.Usage & D3DUSAGE_DYNAMIC)) return 1;
    if (ib && SUCCEEDED(IDirect3DIndexBuffer9_GetDesc(ib, &id)) && (id.Usage & D3DUSAGE_DYNAMIC)) return 1;
    return 0;
}

/*
 * The casters that must be in the cache and the cube: every character, and
 * the NEAR_KEEP nearest the player (the focus, else the camera). A full
 * cache let the player's shadow go, or never come in (2026-09-25).
 */
#define NEAR_KEEP 12
static const float *player_ref(void)
{
    return g_frame - g_focus_frame <= FOCUS_HOLD ? g_focus : g_eye;
}

/* mark[i] = 1 for the NEAR_KEEP casters nearest the player, and characters */
static void mark_kept(unsigned char *mark)
{
    const float *p = player_ref();
    float best[NEAR_KEEP];
    int who[NEAR_KEEP], n = 0, i, j;
    memset(mark, 0, g_nc);
    for (i = 0; i < g_nc; i++) {
        float dx = g_c[i].pos[0] - p[0], dy = g_c[i].pos[1] - p[1], dz = g_c[i].pos[2] - p[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (g_c[i].kind == 2) mark[i] = 1;
        if (n < NEAR_KEEP) n++;
        else if (d >= best[n - 1]) continue;
        for (j = n - 1; j > 0 && best[j - 1] > d; j--) { best[j] = best[j - 1]; who[j] = who[j - 1]; }
        best[j] = d; who[j] = i;
    }
    for (i = 0; i < n; i++) mark[who[i]] = 1;
}

/* From gfxprobe's DrawIndexedPrimitive hook, after the engine's own draw of
 * a shadow-map caster: record it if this is the near map's pass. */
void plshadow_dip(IDirect3DDevice9 *dev, dip_fn draw, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                  UINT si, UINT pc)
{
    static float cst[CREGS * 4];
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    IDirect3DVertexBuffer9 *vb0 = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    IDirect3DBaseTexture9 *tex = NULL;
    UINT off0 = 0, stride0 = 0;
    float pos[3], best_d = 1e30f;
    int kind = g_sm_kind, nreg, vreg, i, best = -1;
    if (!g_on || kind == 0 || g_pass < 0) return;
    vreg = kind == 2 ? 184 : 4;
    if (g_pass == 0) {
        /* is this the near map? its orthographic width is the near reach */
        IDirect3DSurface9 *rt = NULL;
        D3DSURFACE_DESC d;
        float c[4], width;
        g_pass = -1;
        if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)) || !rt) return;
        IDirect3DSurface9_GetDesc(rt, &d);
        IDirect3DSurface9_Release(rt);
        if (d.Format != D3DFMT_R32F) return;
        IDirect3DDevice9_GetVertexShaderConstantF(dev, vreg + 4, c, 1);
        width = c[0] > 1e-6f ? 2.0f / c[0] : 0;
        if (fabsf(width - gfxprobe_near_reach()) > 1.0f) return;
        g_pass = 1;
    }
    g_draw = draw;
    if (g_pass_frame != g_frame) {            /* a new pass: its box, for telling who left it */
        g_pass_frame = g_frame;
        g_pass_id++;
        g_pass_claims = 0;
        IDirect3DDevice9_GetVertexShaderConstantF(dev, vreg, &g_pass_vp[0][0], 8);
    }
    g_pass_claims++;
    nreg = kind == 2 ? CREGS : 4;
    if (FAILED(IDirect3DDevice9_GetVertexShaderConstantF(dev, 0, cst, nreg))) return;
    caster_pos(cst, kind, pos);
    IDirect3DDevice9_GetVertexShader(dev, &vs);
    IDirect3DDevice9_GetPixelShader(dev, &ps);
    IDirect3DDevice9_GetStreamSource(dev, 0, &vb0, &off0, &stride0);
    IDirect3DDevice9_GetIndices(dev, &ib);
    IDirect3DDevice9_GetTexture(dev, 0, &tex);
    for (i = 0; i < g_nc; i++) {
        caster *e = &g_c[i];
        float dx, dy, dz, d;
        if (e->pass == g_pass_id || e->vs != vs || e->ps != ps || e->vb[0] != vb0 || e->off[0] != off0 ||
            e->ib != ib || e->tex != tex || e->kind != kind || e->t != t || e->bv != bv || e->mi != mi ||
            e->nv != nv || e->si != si || e->pc != pc)
            continue;
        dx = e->pos[0] - pos[0]; dy = e->pos[1] - pos[1]; dz = e->pos[2] - pos[2];
        d = dx * dx + dy * dy + dz * dz;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best >= 0) {                          /* known: refresh it */
        caster *e = &g_c[best];
        if (memcmp(e->c, cst, nreg * 4 * sizeof(float))) {
            if (in_reach(e->pos) || in_reach(pos)) g_dirty = 1;
            memcpy(e->c, cst, nreg * 4 * sizeof(float));
            memcpy(e->pos, pos, sizeof e->pos);
        }
        e->seen = g_frame;
        e->pass = g_pass_id;
    } else if (!vs || !vb0 || !ib || buffer_dynamic(vb0, ib)) {
        if (vb0 && ib) g_st_dyn++;
    } else {                                  /* new */
        caster *e;
        IDirect3DVertexDeclaration9 *decl = NULL;
        IDirect3DDevice9_GetVertexDeclaration(dev, &decl);
        if (decl) {
            if (g_nc == MAXC) {               /* full: the one longest unseen goes, never a kept one */
                static unsigned char kept[MAXC];
                int j, old = -1;
                mark_kept(kept);
                for (j = 0; j < g_nc; j++)
                    if (!kept[j] && (old < 0 || g_c[j].seen < g_c[old].seen)) old = j;
                if (old < 0) old = 0;
                if (in_reach(g_c[old].pos)) g_dirty = 1;
                caster_drop(old);
            }
            e = &g_c[g_nc++];
            e->vs = vs; vs = NULL;
            e->ps = ps; ps = NULL;
            e->decl = decl;
            e->vb[0] = vb0; vb0 = NULL; e->off[0] = off0; e->stride[0] = stride0;
            {
                D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
                UINT n = 0, k;
                int used = 1;
                if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration(decl, el, &n)))
                    for (k = 0; k < n && el[k].Stream != 0xff; k++) if (el[k].Stream < MAXS) used |= 1 << el[k].Stream;
                for (k = 1; k < MAXS; k++)
                    if (used & (1 << k)) IDirect3DDevice9_GetStreamSource(dev, k, &e->vb[k], &e->off[k], &e->stride[k]);
            }
            e->ib = ib; ib = NULL;
            e->tex = tex; tex = NULL;
            for (i = 0; i < 5; i++) IDirect3DDevice9_GetSamplerState(dev, 0, k_samp[i], &e->samp[i]);
            IDirect3DDevice9_GetRenderState(dev, D3DRS_CULLMODE, &e->cull);
            e->t = t; e->bv = bv; e->mi = mi; e->nv = nv; e->si = si; e->pc = pc;
            e->kind = kind; e->nreg = nreg;
            memcpy(e->c, cst, nreg * 4 * sizeof(float));
            memcpy(e->pos, pos, sizeof e->pos);
            e->seen = g_frame;
            e->pass = g_pass_id;
            if (in_reach(pos)) g_dirty = 1;
        }
    }
    if (vs) IDirect3DVertexShader9_Release(vs);
    if (ps) IDirect3DPixelShader9_Release(ps);
    if (vb0) IDirect3DVertexBuffer9_Release(vb0);
    if (ib) IDirect3DIndexBuffer9_Release(ib);
    if (tex) IDirect3DBaseTexture9_Release(tex);
}

/* At Present, before the light is chosen: who is gone.
 *
 * After a pass that drew about as many casters as the passes before it
 * (not a part of one), a caster it did not draw although it stands well
 * inside the near map's box has left (died, moved on, unloaded): it goes at
 * once. Out of the box the engine does not draw it at all, so a prop
 * stays while it can cast into this light's cube; unseen for 10 s it goes
 * anyway. A character (skinned) goes whenever a whole pass leaves it out:
 * one walking off the near map would otherwise leave its last pose
 * standing in the cube. */
static void cache_sweep(void)
{
    int i, full = 0;
    if (g_pass_frame == g_frame && g_pass_seen_frame != g_frame) {
        LONG most = 0;
        g_pass_seen_frame = g_frame;
        for (i = 0; i < 8; i++) if (g_pass_hist[i] > most) most = g_pass_hist[i];
        full = g_pass_claims * 2 >= most;
        memmove(g_pass_hist + 1, g_pass_hist, 7 * sizeof g_pass_hist[0]);
        g_pass_hist[0] = g_pass_claims;
    }
    for (i = g_nc - 1; i >= 0; i--) {
        caster *e = &g_c[i];
        int drop = 0;
        if (full && e->pass != g_pass_id) {
            float v[4], x, y, w, p[4] = { e->pos[0], e->pos[1], e->pos[2], 1 };
            int k;
            for (k = 0; k < 4; k++) v[k] = g_pass_vp[k][0] * p[0] + g_pass_vp[k][1] * p[1] + g_pass_vp[k][2] * p[2] + g_pass_vp[k][3];
            x = g_pass_vp[4][0] * v[0] + g_pass_vp[4][1] * v[1] + g_pass_vp[4][2] * v[2] + g_pass_vp[4][3] * v[3];
            y = g_pass_vp[5][0] * v[0] + g_pass_vp[5][1] * v[1] + g_pass_vp[5][2] * v[2] + g_pass_vp[5][3] * v[3];
            w = g_pass_vp[7][0] * v[0] + g_pass_vp[7][1] * v[1] + g_pass_vp[7][2] * v[2] + g_pass_vp[7][3] * v[3];
            if (e->kind == 2 || (w > 1e-6f && fabsf(x / w) < 0.8f && fabsf(y / w) < 0.8f)) drop = 1;
        }
        if (g_frame - e->seen > 600) drop = 1;
        if (drop) {
            if (in_reach(e->pos)) g_dirty = 1;
            caster_drop(i);
        }
    }
}

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

/* ------------------------------------------------------------------ */
/* the level's geometry                                                */

/*
 * Walls, pillars and floors cast too (2026-09-26). The engine's near-map
 * pass, the only casters above, holds characters and props: lamp light went
 * through walls. The level's own opaque draws (fxk flag 1, its colour pass)
 * are recorded as they are drawn, once each for the level: buffers,
 * declaration, World, and a bounding sphere from the vertex positions (read
 * once). They are static, so a piece seen once stays until the level
 * changes. At the cube's redraw those within the light's reach go in with
 * our own caster (override/ultra/plcast.fxo: positions only, z/w out, as
 * shadowmap.fxo writes), nearest first, both sides of every triangle.
 */
#define MAXL 6144                   /* level pieces kept */
#define LVL_CUBE_MAX 384            /* the most drawn into the cube */
typedef struct {
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexBuffer9 *vb[MAXS];
    UINT off[MAXS], stride[MAXS];
    IDirect3DIndexBuffer9 *ib;
    D3DPRIMITIVETYPE t;
    INT bv;
    UINT mi, nv, si, pc;
    float W[16], c[3], r;
    unsigned key;
} lpiece;
static lpiece *g_l;

static void level_flush(void)
{
    int i, k;
    for (i = 0; i < g_nl; i++) {
        lpiece *e = &g_l[i];
        if (e->decl) IDirect3DVertexDeclaration9_Release(e->decl);
        for (k = 0; k < MAXS; k++) if (e->vb[k]) IDirect3DVertexBuffer9_Release(e->vb[k]);
        if (e->ib) IDirect3DIndexBuffer9_Release(e->ib);
    }
    if (g_l) memset(g_l, 0, sizeof(lpiece) * MAXL);
    memset(g_lh, 0, sizeof g_lh);
    memset(g_lrej, 0, sizeof g_lrej);
    g_nlrej = 0;
    if (g_nl) g_dirty = 1;
    g_nl = 0;
}

/* the draws not kept, by key: an open table that is emptied when full */
static int lrej_has(unsigned key)
{
    int h;
    for (h = key & (LREJ - 1); g_lrej[h]; h = (h + 1) & (LREJ - 1)) if (g_lrej[h] == key) return 1;
    return 0;
}
static void lrej_add(unsigned key)
{
    int h;
    if (g_nlrej >= LREJ / 2) { memset(g_lrej, 0, sizeof g_lrej); g_nlrej = 0; }
    for (h = key & (LREJ - 1); g_lrej[h]; h = (h + 1) & (LREJ - 1)) ;
    g_lrej[h] = key;
    g_nlrej++;
}

static unsigned lkey(void *vb, void *ib, INT bv, UINT mi, UINT nv, UINT si, UINT pc, const float *W)
{
    unsigned h = 2166136261u, v[10];
    int i;
    v[0] = (unsigned)(size_t)vb; v[1] = (unsigned)(size_t)ib; v[2] = (unsigned)bv; v[3] = mi; v[4] = nv;
    v[5] = si; v[6] = pc;
    memcpy(&v[7], W + 12, 12);                  /* where it stands */
    for (i = 0; i < 10; i++) { h ^= v[i]; h *= 16777619u; }
    return h ? h : 1;
}

/* the bounding sphere of the drawn vertices, in world space; 0 if unread */
static int level_bounds(IDirect3DDevice9 *dev, IDirect3DVertexDeclaration9 *decl, INT bv, UINT mi, UINT nv,
                        const float *W, float c[3], float *r)
{
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = 0, e, i, off, stride;
    IDirect3DVertexBuffer9 *vb = NULL;
    void *p = NULL;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, m[3], s2, sc;
    int pe = -1, k, ok = 0;
    if (FAILED(IDirect3DVertexDeclaration9_GetDeclaration(decl, el, &n))) return 0;
    for (e = 0; e < n && el[e].Stream != 0xff; e++)
        if (el[e].Usage == D3DDECLUSAGE_POSITION && el[e].UsageIndex == 0) pe = e;
    if (pe < 0 || (el[pe].Type != D3DDECLTYPE_FLOAT3 && el[pe].Type != D3DDECLTYPE_FLOAT4)) return 0;
    IDirect3DDevice9_GetStreamSource(dev, el[pe].Stream, &vb, &off, &stride);
    if (!vb) return 0;
    if (nv && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, D3DLOCK_READONLY)) && p) {
        for (i = (UINT)(bv + (INT)mi); i < (UINT)(bv + (INT)mi) + nv; i++) {
            const float *q = (const float *)((const unsigned char *)p + off + i * stride + el[pe].Offset);
            for (k = 0; k < 3; k++) { if (q[k] < lo[k]) lo[k] = q[k]; if (q[k] > hi[k]) hi[k] = q[k]; }
        }
        IDirect3DVertexBuffer9_Unlock(vb);
        ok = lo[0] <= hi[0];
    }
    IDirect3DVertexBuffer9_Release(vb);
    if (!ok) return 0;
    for (k = 0; k < 3; k++) m[k] = (lo[k] + hi[k]) * 0.5f;
    for (k = 0; k < 3; k++) c[k] = m[0] * W[k] + m[1] * W[4 + k] + m[2] * W[8 + k] + W[12 + k];
    s2 = 0;
    for (k = 0; k < 3; k++) s2 += (hi[k] - lo[k]) * (hi[k] - lo[k]);
    sc = sqrtf(W[0] * W[0] + W[1] * W[1] + W[2] * W[2]);
    sc = fmaxf(sc, sqrtf(W[4] * W[4] + W[5] * W[5] + W[6] * W[6]));
    sc = fmaxf(sc, sqrtf(W[8] * W[8] + W[9] * W[9] + W[10] * W[10]));
    *r = 0.5f * sqrtf(s2) * sc;
    return 1;
}

/* From gfxprobe's DrawIndexedPrimitive hook: an opaque draw of the level's
 * own geometry, World its world matrix. */
void plshadow_level_dip(IDirect3DDevice9 *dev, dip_fn draw, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                        UINT si, UINT pc, const float *W)
{
    IDirect3DVertexBuffer9 *vb0 = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    IDirect3DVertexDeclaration9 *decl = NULL;
    UINT off0, stride0;
    unsigned key;
    int h;
    lpiece *e;
    if (!g_on || !W) return;
    if (!g_l && !(g_l = (lpiece *)calloc(MAXL, sizeof(lpiece)))) return;
    g_draw = draw;
    IDirect3DDevice9_GetStreamSource(dev, 0, &vb0, &off0, &stride0);
    IDirect3DDevice9_GetIndices(dev, &ib);
    key = lkey(vb0, ib, bv, mi, nv, si, pc, W);
    if (lrej_has(key)) {
        if (vb0) IDirect3DVertexBuffer9_Release(vb0);
        if (ib) IDirect3DIndexBuffer9_Release(ib);
        return;
    }
    for (h = key & (LHASH - 1); g_lh[h]; h = (h + 1) & (LHASH - 1))
        if (g_l[g_lh[h] - 1].key == key) {              /* known */
            if (vb0) IDirect3DVertexBuffer9_Release(vb0);
            if (ib) IDirect3DIndexBuffer9_Release(ib);
            return;
        }
    if (g_nl >= MAXL || !vb0 || !ib || buffer_dynamic(vb0, ib) ||
        FAILED(IDirect3DDevice9_GetVertexDeclaration(dev, &decl)) || !decl) {
        g_l_skip++;
        if (vb0) IDirect3DVertexBuffer9_Release(vb0);
        if (ib) IDirect3DIndexBuffer9_Release(ib);
        if (decl) IDirect3DVertexDeclaration9_Release(decl);
        lrej_add(key);                                  /* not tried again every frame */
        return;
    }
    e = &g_l[g_nl];
    memset(e, 0, sizeof *e);
    memcpy(e->W, W, sizeof e->W);
    if (!level_bounds(dev, decl, bv, mi, nv, W, e->c, &e->r)) {
        g_l_skip++;
        lrej_add(key);
        IDirect3DVertexBuffer9_Release(vb0);
        IDirect3DIndexBuffer9_Release(ib);
        IDirect3DVertexDeclaration9_Release(decl);
        return;
    }
    e->decl = decl;
    e->vb[0] = vb0; e->off[0] = off0; e->stride[0] = stride0;
    {
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
        UINT n = 0, k;
        int used = 1;
        if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration(decl, el, &n)))
            for (k = 0; k < n && el[k].Stream != 0xff; k++) if (el[k].Stream < MAXS) used |= 1 << el[k].Stream;
        for (k = 1; k < MAXS; k++)
            if (used & (1 << k)) IDirect3DDevice9_GetStreamSource(dev, k, &e->vb[k], &e->off[k], &e->stride[k]);
    }
    e->ib = ib;
    e->t = t; e->bv = bv; e->mi = mi; e->nv = nv; e->si = si; e->pc = pc;
    e->key = key;
    for (h = key & (LHASH - 1); g_lh[h]; h = (h + 1) & (LHASH - 1)) ;
    g_lh[h] = ++g_nl;
    if (g_active) {
        float dx = e->c[0] - g_lpos[0], dy = e->c[1] - g_lpos[1], dz = e->c[2] - g_lpos[2], rr = g_lfar + e->r;
        if (dx * dx + dy * dy + dz * dz < rr * rr) g_dirty = 1;
    }
}

/* Can a sphere (centre c, radius r) reach cube face f's 90-degree pyramid
 * from the light? Conservative: along the face's axis a = c.d, and across
 * each of the other two |c.e| <= a + r sqrt(2). Each level piece went into
 * all six faces, six times the draws for most of them. */
static int in_face(int f, const float c[3], float r)
{
    float p[3] = { c[0] - g_lpos[0], c[1] - g_lpos[1], c[2] - g_lpos[2] }, a;
    int ax = f >> 1, e1 = (ax + 1) % 3, e2 = (ax + 2) % 3;
    a = (f & 1) ? -p[ax] : p[ax];
    if (a < -r) return 0;
    return fabsf(p[e1]) <= a + r * 1.4143f && fabsf(p[e2]) <= a + r * 1.4143f;
}

/* row-major view-projection of a face (the helpers give their transposes) */
static void face_vp(const float *vt, const float *pt, float vp[16])
{
    int r, c, k;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            float s = 0;
            for (k = 0; k < 4; k++) s += vt[k * 4 + r] * pt[c * 4 + k];      /* V[r][k] P[k][c], V = vt^T, P = pt^T */
            vp[r * 4 + c] = s;
        }
}

/* From src/device.c at Present, once the light is chosen: does the cube
 * need drawing? Then it opens a scene and calls plshadow_redraw. */
int plshadow_pending(void)
{
    if (!g_on || !g_active || !g_draw || g_failed) return 0;
    if (g_drawn_far != g_lfar || memcmp(g_drawn_pos, g_lpos, sizeof g_lpos)) g_dirty = 1;
    return g_dirty;
}

/* The whole cube from the cache: every caster in the light's reach into
 * all six faces, the engine's own shaders skinning and cutting out. */
void plshadow_redraw(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *rt = NULL, *ds = NULL;
    D3DVIEWPORT9 vp, fvp = { 0, 0, PLS_SIZE, PLS_SIZE, 0.0f, 1.0f };
    float view[16], proj[16];
    static int pick[MAXC];
    static float pd[MAXC];
    int f, i, k, n = 0, drawn = 0, ln = 0;
    const int *lp = NULL;
    if (!plshadow_pending() || !ensure(dev)) return;
    /* the casters in reach, nearest the light first, at most CUBE_MAX; the
     * kept ones (characters, the nearest the player) first of all */
    static unsigned char kept[MAXC];
    mark_kept(kept);
    for (i = 0; i < g_nc; i++) {
        float d = kept[i] ? -1.0f : reach_d2(g_c[i].pos);
        int j;
        if (!in_reach(g_c[i].pos)) continue;
        for (j = n; j > 0 && pd[j - 1] > d; j--) { pd[j] = pd[j - 1]; pick[j] = pick[j - 1]; }
        pd[j] = d; pick[j] = i; n++;
    }
    if (n > CUBE_MAX) n = CUBE_MAX;
    /* the level's pieces in reach, nearest first */
    {
        static int lpick[MAXL];
        static float lpd[MAXL];
        int j;
        ln = 0;
        for (i = 0; i < g_nl; i++) {
            float dx = g_l[i].c[0] - g_lpos[0], dy = g_l[i].c[1] - g_lpos[1], dz = g_l[i].c[2] - g_lpos[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz) - g_l[i].r;
            if (d > g_lfar) continue;
            for (j = ln; j > 0 && lpd[j - 1] > d; j--) { lpd[j] = lpd[j - 1]; lpick[j] = lpick[j - 1]; }
            lpd[j] = d; lpick[j] = i;
            if (ln < LVL_CUBE_MAX) ln++;
        }
        lp = lpick;
    }
    if (ln && !g_cfx && !g_cfx_failed) {
        g_cfx = postfx_load(dev, L"plcast.fxo");
        g_cfx_failed = !g_cfx;
        if (g_cfx) {
            g_cfx_w = g_cfx->lpVtbl->GetParameterByName(g_cfx, NULL, "gWorld");
            g_cfx_vp = g_cfx->lpVtbl->GetParameterByName(g_cfx, NULL, "gViewProj");
            g_cfx->lpVtbl->SetTechnique(g_cfx, g_cfx->lpVtbl->GetTechniqueByName(g_cfx, "Rigid"));
            hg_log("plshadow: the level's geometry casts (plcast.fxo)");
        } else
            hg_log("plshadow: plcast.fxo not loaded: the level's geometry does not cast");
    }
    if (!g_cfx) ln = 0;
    if (!g_sb && FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &g_sb))) { g_sb = NULL; return; }
    IDirect3DStateBlock9_Capture(g_sb);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DDevice9_GetViewport(dev, &vp);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xf);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
    for (k = 1; k < 16; k++) IDirect3DDevice9_SetTexture(dev, k, NULL);   /* the cube itself is on s13 */
    for (k = 0; k < MAXS; k++) IDirect3DDevice9_SetStreamSourceFreq(dev, k, 1);
    face_proj(proj);
    for (f = 0; f < 6; f++) {
        face_view(f, view);
        IDirect3DDevice9_SetRenderTarget(dev, 0, g_face[f]);
        IDirect3DDevice9_SetDepthStencilSurface(dev, g_ds);
        IDirect3DDevice9_SetViewport(dev, &fvp);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffffffff, 1.0f, 0);
        for (i = 0; i < n; i++) {
            caster *e = &g_c[pick[i]];
            int vreg = e->kind == 2 ? 184 : 4;
            IDirect3DDevice9_SetVertexShader(dev, e->vs);
            IDirect3DDevice9_SetPixelShader(dev, e->ps);
            IDirect3DDevice9_SetVertexDeclaration(dev, e->decl);
            for (k = 0; k < MAXS; k++) IDirect3DDevice9_SetStreamSource(dev, k, e->vb[k], e->off[k], e->stride[k]);
            IDirect3DDevice9_SetIndices(dev, e->ib);
            IDirect3DDevice9_SetTexture(dev, 0, e->tex);
            for (k = 0; k < 5; k++) IDirect3DDevice9_SetSamplerState(dev, 0, k_samp[k], e->samp[k]);
            IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, e->cull);
            IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, e->c, e->nreg);
            IDirect3DDevice9_SetVertexShaderConstantF(dev, vreg, view, 4);
            IDirect3DDevice9_SetVertexShaderConstantF(dev, vreg + 4, proj, 4);
            g_draw(dev, e->t, e->bv, e->mi, e->nv, e->si, e->pc);
            if (f == 0) drawn++;
        }
        if (ln) {                                       /* the level, positions only, both sides */
            UINT np = 0;
            float fvpm[16];
            face_vp(view, proj, fvpm);
            if (SUCCEEDED(g_cfx->lpVtbl->Begin(g_cfx, &np, D3DXFX_DONOTSAVESTATE)) && np > 0) {
                g_cfx->lpVtbl->BeginPass(g_cfx, 0);
                g_cfx->lpVtbl->SetMatrix(g_cfx, g_cfx_vp, (const D3DXMATRIX *)fvpm);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
                IDirect3DDevice9_SetTexture(dev, 0, NULL);
                for (i = 0; i < ln; i++) {
                    lpiece *e = &g_l[lp[i]];
                    if (!in_face(f, e->c, e->r)) continue;      /* not in this face's quarter */
                    g_cfx->lpVtbl->SetMatrix(g_cfx, g_cfx_w, (const D3DXMATRIX *)e->W);
                    g_cfx->lpVtbl->CommitChanges(g_cfx);
                    IDirect3DDevice9_SetVertexDeclaration(dev, e->decl);
                    for (k = 0; k < MAXS; k++) IDirect3DDevice9_SetStreamSource(dev, k, e->vb[k], e->off[k], e->stride[k]);
                    IDirect3DDevice9_SetIndices(dev, e->ib);
                    g_draw(dev, e->t, e->bv, e->mi, e->nv, e->si, e->pc);
                }
                g_cfx->lpVtbl->EndPass(g_cfx);
                g_cfx->lpVtbl->End(g_cfx);
            }
        }
    }
    g_l_drawn = ln;
    IDirect3DStateBlock9_Apply(g_sb);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    IDirect3DDevice9_SetViewport(dev, &vp);
    if (rt) IDirect3DSurface9_Release(rt);
    if (ds) IDirect3DSurface9_Release(ds);
    memcpy(g_drawn_pos, g_lpos, sizeof g_drawn_pos);
    g_drawn_far = g_lfar;
    g_dirty = 0;
    g_replays = drawn;
    InterlockedIncrement(&g_casts);
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
/* A setting changed through the panel's generic path (src/settings.c). */
void plshadow_touch(void) { InterlockedIncrement(&g_params_gen); }
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
    *casts = g_nc; *replays = g_replays;
    return g_active;
}
