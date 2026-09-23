/* The volumetric fog's inputs, gathered by src/volfog.c. */
#ifndef HG_VOLFOG_H
#define HG_VOLFOG_H
#include <windows.h>
#include <d3d9.h>
#include <d3dx9effect.h>

typedef struct {
    LONG cam_frame, sun_frame, maps_frame, fog_frame;   /* frame each part was seen in */
    float view[16], inv_view[16], eye[3];
    float to_sun[3], sun_col[3];
    float fine_m[16], near_m[16];                       /* world -> shadow map (uv, depth) */
    IDirect3DBaseTexture9 *fine, *nearmap;
    float fog_col[3], fog_min, fog_max;
} volfog_state;

void volfog_present(void);
void volfog_reset(void);
void volfog_view(const float *view);
void volfog_maps(ID3DXEffect *fx, const float *fine_m, IDirect3DBaseTexture9 *fine);
void volfog_collect(ID3DXEffect *fx);
const volfog_state *volfog_get(LONG *frame);

#endif
