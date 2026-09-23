/*
 * What the volumetric fog pass (src/postfx.c, shaders/fog.fx) needs from the
 * engine, gathered while the frame is drawn:
 *
 *   camera   the view matrix, from dx9_SetShadowMapParameters (gfxprobe.c's
 *            detour gets it with the projection for every shadowed mesh);
 *            inverted here, so the pass can take a pixel back to the world
 *   sun      its direction (ShadowLightDir, the direction the light
 *            travels) and colour (DirLightsColor[0]) from an outdoor
 *            background material at draw time
 *   maps     the two sun shadow maps near the camera in world space: the
 *            near one (every character and prop, tShadowMapDepth,
 *            gmShadowMatrix2) and the fine 80-unit one (tShadowMap,
 *            gmShadowMatrix), both read from the identity-world run the
 *            fine-map-per-pixel detour already makes
 *   fog      FogColor, FogMinDistance, FogMaxDistance, from any material
 *
 * Everything is stamped with the frame it was seen in and used only in that
 * frame, so the texture pointers (the engine's, not referenced here) are
 * always alive when the pass reads them.
 */
#include <windows.h>
#include <string.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include "panel.h"
#include "volfog.h"

static LONG g_frame = 1;
static volfog_state S;

void volfog_present(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last >= 10000 && S.cam_frame == g_frame) {
        last = now;
        hg_log("volfog: eye %.1f %.1f %.1f | sun %s dir %.2f %.2f %.2f col %.2f %.2f %.2f | maps %s | fog %.1f..%.1f col %.2f %.2f %.2f",
               S.eye[0], S.eye[1], S.eye[2], S.sun_frame == g_frame ? "yes" : "no",
               S.to_sun[0], S.to_sun[1], S.to_sun[2], S.sun_col[0], S.sun_col[1], S.sun_col[2],
               S.maps_frame == g_frame ? "yes" : "no", S.fog_min, S.fog_max,
               S.fog_col[0], S.fog_col[1], S.fog_col[2]);
    }
    g_frame++;
}

/* rigid view: inverse = transposed rotation, translation -t R^T */
void volfog_view(const float *v)
{
    int i, j;
    if (S.cam_frame == g_frame) return;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) S.inv_view[i * 4 + j] = v[j * 4 + i];
    S.inv_view[3] = S.inv_view[7] = S.inv_view[11] = 0;
    for (j = 0; j < 3; j++)
        S.inv_view[12 + j] = -(v[12] * S.inv_view[0 + j] + v[13] * S.inv_view[4 + j] + v[14] * S.inv_view[8 + j]);
    S.inv_view[15] = 1;
    memcpy(S.eye, S.inv_view + 12, sizeof S.eye);
    S.cam_frame = g_frame;
}

/* From the fine-map detour, right after its identity-world run: fx holds
 * both maps' world-space matrices; fine is the fine map's texture. */
void volfog_maps(ID3DXEffect *fx, const float *fine_m, IDirect3DBaseTexture9 *fine)
{
    D3DXHANDLE h2, ht;
    D3DXMATRIX m2;
    IDirect3DBaseTexture9 *tn = NULL;
    if (S.maps_frame == g_frame) return;
    h2 = fx->lpVtbl->GetParameterByName(fx, NULL, "gmShadowMatrix2");
    ht = fx->lpVtbl->GetParameterByName(fx, NULL, "tShadowMapDepth");
    if (!h2 || !ht || FAILED(fx->lpVtbl->GetMatrix(fx, h2, &m2)) ||
        FAILED(fx->lpVtbl->GetTexture(fx, ht, &tn)) || !tn)
        return;
    tn->lpVtbl->Release(tn);                /* the engine keeps it; used this frame only */
    memcpy(S.fine_m, fine_m, sizeof S.fine_m);
    memcpy(S.near_m, &m2, sizeof S.near_m);
    S.fine = fine;
    S.nearmap = tn;
    S.maps_frame = g_frame;
}

/* From gfxprobe at a material draw: fog from any, the sun from an outdoor
 * background material (the ones with the fine-map matrix). */
void volfog_collect(ID3DXEffect *fx)
{
    D3DXHANDLE h;
    /* the camera from any material, when no shadowed mesh gave it (rooms
     * without shadow maps: the fog switched off there) */
    if (S.cam_frame != g_frame && (h = fx->lpVtbl->GetParameterByName(fx, NULL, "View"))) {
        D3DXMATRIX m;
        if (SUCCEEDED(fx->lpVtbl->GetMatrix(fx, h, &m)) && (m._11 != 0 || m._12 != 0 || m._13 != 0)) volfog_view((const float *)&m);
    }
    if (S.fog_frame != g_frame) {
        D3DXHANDLE hc = fx->lpVtbl->GetParameterByName(fx, NULL, "FogColor");
        D3DXHANDLE hn = fx->lpVtbl->GetParameterByName(fx, NULL, "FogMinDistance");
        D3DXHANDLE hx = fx->lpVtbl->GetParameterByName(fx, NULL, "FogMaxDistance");
        D3DXVECTOR4 c;
        if (hc && hn && hx && SUCCEEDED(fx->lpVtbl->GetVector(fx, hc, &c)) &&
            SUCCEEDED(fx->lpVtbl->GetFloat(fx, hn, &S.fog_min)) && SUCCEEDED(fx->lpVtbl->GetFloat(fx, hx, &S.fog_max))) {
            S.fog_col[0] = c.x; S.fog_col[1] = c.y; S.fog_col[2] = c.z;
            S.fog_frame = g_frame;
        }
    }
    if (S.sun_frame != g_frame && (h = fx->lpVtbl->GetParameterByName(fx, NULL, "gmUltraFine"))) {
        D3DXHANDLE hd = fx->lpVtbl->GetParameterByName(fx, NULL, "ShadowLightDir");
        D3DXHANDLE hc = fx->lpVtbl->GetParameterByName(fx, NULL, "DirLightsColor");
        float d[3];
        D3DXVECTOR4 c[3];
        if (hd && hc && SUCCEEDED(fx->lpVtbl->GetFloatArray(fx, hd, d, 3)) &&
            SUCCEEDED(fx->lpVtbl->GetVectorArray(fx, hc, c, 3))) {
            S.to_sun[0] = -d[0]; S.to_sun[1] = -d[1]; S.to_sun[2] = -d[2];
            S.sun_col[0] = c[0].x; S.sun_col[1] = c[0].y; S.sun_col[2] = c[0].z;
            S.sun_frame = g_frame;
        }
    }
}

/* This frame's state; which parts are fresh is in the *_frame stamps. */
const volfog_state *volfog_get(LONG *frame)
{
    *frame = g_frame;
    return &S;
}
