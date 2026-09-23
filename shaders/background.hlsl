// Background material shaders (backgroundoutdoor30 / backgroundindoor30 /
// backgroundoutdoorprop30 / backgroundindoorprop30), rewritten from the
// stock bytecode like actor.hlsl (docs/graphics-plan.md step 7). Stage 1 is
// parity with stock, checked by tools/fx/matcheck.sh.
//
// Defines, derived by tools/fx/mkmat.py from each technique's annotations:
//   INDOOR       no directional lights (indoor effects)
//   LIGHTMAP     baked light map (the non-prop effects); props light from SH
//   SH           spherical harmonics fill
//   POINTLIGHTS  0/3/5 world-space point lights, summed per vertex (per
//                pixel with gvUltraPL.x)
//   SHADOWTYPE   0/1/2; DIFFUSEMAP2, NORMALMAP, SELFILLUM, SPECULAR,
//   CUBEENVMAP, SCROLLUV
//
// What the stock shaders do:
// - Vertex colour holds half the dynamic light: point lights + ambient
//   (+ SH) (+ directional lights 0 and 1 outdoors), scaled per vertex by a
//   baked sun visibility in the normal's w, lerp(1, w, gvMiscLightingData.y).
//   The PS doubles it and adds the light map.
// - The dynamic shadow multiplies ALL of it, light map included, by
//   lerp(1, s, gvMiscLightingData.y), where s = min(second map, (main + 1) / 2)
//   and surfaces facing away from the shadow light get s = 0.5. The main map
//   is where characters cast; a character's shadow on a lit floor therefore
//   also darkens the light already baked into the light map.
// - The normal map is only read for the specular highlight.
// - Diffuse map 2 blends over the base by its own alpha, with its uv carried
//   in the w of TEXCOORD5 / TEXCOORD7.

#ifndef INDOOR
#define INDOOR 0
#endif
#ifndef LIGHTMAP
#define LIGHTMAP 0
#endif
#ifndef SH
#define SH 0
#endif
#ifndef POINTLIGHTS
#define POINTLIGHTS 0
#endif
#ifndef SHADOWTYPE
#define SHADOWTYPE 0
#endif
#ifndef DIFFUSEMAP2
#define DIFFUSEMAP2 0
#endif
#ifndef NORMALMAP
#define NORMALMAP 0
#endif
#ifndef SELFILLUM
#define SELFILLUM 0
#endif
#ifndef SPECULAR
#define SPECULAR 0
#endif
#ifndef CUBEENVMAP
#define CUBEENVMAP 0
#endif
#ifndef SCROLLUV
#define SCROLLUV 0
#endif

// The normal map feeds only the tangent-space highlight, and stock drops it
// when a cube map and a shadow map are both on (out of interpolators): those
// techniques use the world-space highlight instead.
#define NM_SPEC (NORMALMAP && SPECULAR && !(CUBEENVMAP && SHADOWTYPE && !INDOOR))
// The reflection vector goes per vertex in TEXCOORD4 whenever that slot is
// free, i.e. unless the outdoor second shadow coordinate occupies it.
#define VS_REFL (CUBEENVMAP && !(SHADOWTYPE && !INDOOR))
// The world position in the PS, for per-pixel point lights (gvUltraPL.x)
// and the fine outdoor shadow map (gvUltraLook.w). Without the normal map
// it is tpos.xyz; with it tpos is in tangent space, so it rides in t5.xyz
// where that is free (only the outdoor shadow fill uses it) and otherwise
// in its own TEXCOORD8, which fits: those variants read 9 of the 10 ps_3_0
// inputs and write 11 of the 11 vs_3_0 outputs besides the position.
#define WP8 (NM_SPEC && SHADOWTYPE && !INDOOR && (POINTLIGHTS || SHADOWTYPE == 2))
// the outdoor colour-map variants read the fine map too
#define FINE_MAP (SHADOWTYPE == 2 && !INDOOR)

// ---- parameters (names and types exactly as in the stock effects) --------

float4x4 WorldViewProjection;
float4x4 World;
float4x4 gmShadowMatrix;
float4x4 gmShadowMatrix2;
float3   ShadowLightDir;
float4   _DirLightsDir_0[3];
float4   _DirLightsDir_1[3];
float4   DirLightsColor[3];
float4   _PointLightsPos_1[5];
float4   PointLightsColor[5];
float4   _PointLightsFalloff_1[5];
float4   _SpecularLightsPos_0[2];
float4   _SpecularLightsPos_1[2];
float4   SpecularLightsColor[2];
float4   _SpecularLightsFalloff_0[2];
float4   _SpecularLightsFalloff_1[2];
float4   LightAmbient;
float4   EyeInObject;
float4   EyeInWorld;
float    FogMaxDistance;
float    FogMinDistance;
float    gfFogFactor;
float4   FogColor;
float4   cAr, cAg, cAb, cBr, cBg, cBb, cC;
float4   gvSpecularMaterialData;
float4   gvMiscLightingData;
float4   gvMiscMaterialData;
float4   gvEnvironmentMapData;
float4   gvShadowSize;
float    gfSpecularPower;
struct SCROLL { float2 fTile; float2 fPhase; };
SCROLL   gfScrollTextures[2];
float    gfScrollActive[7];

sampler2D   DiffuseMapSampler           : register(s0);
sampler2D   LightMapSampler             : register(s1);
sampler2D   SelfIlluminationMapSampler  : register(s2);
sampler2D   DiffuseMapSampler2          : register(s4);
sampler2D   SpecularMapSampler          : register(s5);
sampler2D   NormalMapSampler            : register(s6);
samplerCUBE CubeEnvironmentMapSampler   : register(s7);
#if SHADOWTYPE == 2
sampler2D   ColorShadowMapSampler       : register(s10);
sampler2D   ExtraColorShadowMapSampler  : register(s11);
#if !INDOOR
// The engine gives each mesh ONE of its two wide outdoor maps: a fine
// 80-unit one when the whole mesh fits inside it, else the zone-wide one
// (sSetGeneralMeshParameters). They hold different shadows, so neighbouring
// meshes disagreed in straight seams. The DLL binds the fine map here and
// its world-space matrix below for every mesh, and gmShadowMatrix is then
// always the zone-wide one: the choice is made per pixel instead.
sampler2D   UltraFineSampler            : register(s12);
float4x4    gmUltraFine;
#endif
#else
sampler2D   ShadowMapSampler            : register(s10);
sampler2D   ShadowMapDepthSampler       : register(s11);
#endif

#include "ultra.hlsl"

// ---- vertex shader ------------------------------------------------------

struct VS_IN {
    float4 pos  : POSITION;
    float4 uv   : TEXCOORD0;     // xy light map, zw diffuse
    float4 nrm  : NORMAL;        // xyz packed 0..1, w baked sun visibility
#if DIFFUSEMAP2
    float2 uv2  : TEXCOORD1;
#endif
#if NM_SPEC
    float3 tan  : TANGENT;
    float3 bin  : BINORMAL;
#endif
};

struct VS_OUT {
    float4 hpos   : POSITION;
    float4 col    : COLOR0;      // rgb half the vertex light, a fog
    float4 uv     : TEXCOORD0;
    float4 tpos   : TEXCOORD1;   // world position (tangent-space position with NM+spec), w visibility
    float4 nrmw   : TEXCOORD2;
    float4 shpos  : TEXCOORD3;
    float4 shpos2 : TEXCOORD4;
    float4 t5     : TEXCOORD5;   // w: diffuse map 2 u
    float4 sdir   : TEXCOORD6;   // specular light, tangent space; indoor w: light chosen
    float4 eye    : TEXCOORD7;   // w: diffuse map 2 v
    float4 scol   : COLOR1;      // indoor: specular light colour * attenuation
#if WP8
    float4 wp     : TEXCOORD8;   // world position
#endif
};

float3 sh9(float3 n)
{
    float4 n1 = float4(n, 1.0);
    float3 a = float3(dot(cAr, n1), dot(cAg, n1), dot(cAb, n1));
    float4 q = n.xyzz * n.yzzx;
    float3 b = float3(dot(cBr, q), dot(cBg, q), dot(cBb, q));
    return cC.xyz * (n.x * n.x - n.y * n.y) + (a + b);
}

VS_OUT vs_main(VS_IN v)
{
    VS_OUT o;
    float4 P = float4(v.pos.xyz, 1.0);
    float3 N = v.nrm.xyz * 2.0 - 1.0;
    float3 Nw = mul(N, (float3x3)World);
    float3 wpos = mul(P, World).xyz;
    float vis = gvMiscLightingData.y * (v.nrm.w - 1.0) + 1.0;

    o.hpos = mul(P, WorldViewProjection);
    o.uv = v.uv;
    o.nrmw = float4(Nw, 0);
#if NM_SPEC
    float3x3 TBN = float3x3(v.tan, v.bin, N);
    o.tpos = float4(mul(TBN, v.pos.xyz), vis);
    o.eye = float4(mul(TBN, EyeInObject.xyz), 0);
#else
    o.tpos = float4(wpos, vis);
    o.eye = float4(EyeInWorld.xyz, 0);
#endif

    float3 light = 0;
#if POINTLIGHTS
    [branch] if (gvUltraPL.x <= 0) {            // else the PS lights per pixel
        [loop] for (int k = 0; k < POINTLIGHTS; k++) {
            float3 L = _PointLightsPos_1[k].xyz - wpos;
            float rl = rsqrt(dot(L, L));
            float ndl = saturate(dot(Nw, L * rl));
            float att = saturate(_PointLightsFalloff_1[k].x - (1.0 / rl) * _PointLightsFalloff_1[k].y);
            light = (att * PointLightsColor[k].xyz) * ndl + light;
        }
    }
#endif
    float fillk = 1.0 + gvUltraLook.x, sunk = 1.0 + gvUltraLook.z;
    light += LightAmbient.xyz * fillk;
#if SH
    light += sh9(Nw) * fillk;
#endif
#if !INDOOR
    light = DirLightsColor[0].xyz * (saturate(dot(Nw, _DirLightsDir_1[0].xyz)) * sunk) + light;
    light = DirLightsColor[1].xyz * saturate(dot(Nw, _DirLightsDir_1[1].xyz)) + light;
#endif
    float dist = length(EyeInObject.xyz - v.pos.xyz);
    float fog = saturate(saturate((FogMaxDistance - dist) / (FogMaxDistance - fog_min())) + gfFogFactor);
    o.col = float4(light * 0.5, fog);

    o.shpos = 0;
    o.shpos2 = 0;
#if SHADOWTYPE
    // normal offset against shadow acne (gvUltraAct.z world units; 0 is
    // stock): along the normal, most where the surface is edge-on to the
    // shadow light, where the depth map is coarsest across it (stripes on
    // indoor props, 2026-09-23)
    float4 Ps = P;
    {
        float ndl = dot(Nw, ShadowLightDir);
        Ps.xyz += N * (gvUltraAct.z * (0.3 + 0.7 * sqrt(saturate(1.0 - ndl * ndl))));
    }
    o.shpos = mul(Ps, gmShadowMatrix);
#if !INDOOR
    o.shpos2 = mul(Ps, gmShadowMatrix2);
#endif
#endif
#if VS_REFL
    {
        float3 I = normalize(wpos - EyeInWorld.xyz);
        o.shpos2 = float4(I - 2.0 * dot(I, Nw) * Nw, 0);
    }
#endif
    o.t5 = 0;
#if SHADOWTYPE && !INDOOR
    // the dynamic sun alone (halved like the colour), for the shadow fill
    o.t5.xyz = DirLightsColor[0].xyz * (saturate(dot(Nw, _DirLightsDir_1[0].xyz)) * sunk) * 0.5;
#endif
#if POINTLIGHTS && NM_SPEC && !WP8
    o.t5.xyz = wpos;
#endif
#if WP8
    o.wp = float4(wpos, 0);
#endif
#if DIFFUSEMAP2
    o.t5.w = v.uv2.x;
    o.eye.w = v.uv2.y;
#endif
    o.sdir = 0;
    o.scol = 0;
#if NM_SPEC
#if !INDOOR
    o.sdir = float4(mul(TBN, _DirLightsDir_0[2].xyz), 0);
#else
    {
        // the stronger of the two specular lights at this vertex
        float3 L0 = _SpecularLightsPos_0[0].xyz - v.pos.xyz;
        float3 L1 = _SpecularLightsPos_0[1].xyz - v.pos.xyz;
        float a0 = saturate(length(L0) * -_SpecularLightsFalloff_0[0].y + _SpecularLightsFalloff_0[0].x);
        float a1 = saturate(length(L1) * -_SpecularLightsFalloff_0[1].y + _SpecularLightsFalloff_0[1].x);
        float4 c0 = float4(a0 * SpecularLightsColor[0].xyz, a0);
        float4 c1 = float4(a1 * SpecularLightsColor[1].xyz, a1);
        // equal attenuation picks neither (w = 0 switches the highlight off)
        float4 L = a1 < a0 ? float4(L0, 1) : (a0 < a1 ? float4(L1, 1) : float4(0, 0, -1, 0));
        o.scol = a1 < a0 ? c0 : (a0 < a1 ? c1 : 0);
        o.sdir = float4(mul(TBN, L.xyz), L.w);
    }
#endif
#elif SPECULAR && INDOOR
    {
        // no normal map: the same choice in world space
        float3 L0 = _SpecularLightsPos_1[0].xyz - wpos;
        float3 L1 = _SpecularLightsPos_1[1].xyz - wpos;
        float a0 = saturate(length(L0) * -_SpecularLightsFalloff_1[0].y + _SpecularLightsFalloff_1[0].x);
        float a1 = saturate(length(L1) * -_SpecularLightsFalloff_1[1].y + _SpecularLightsFalloff_1[1].x);
        float4 c0 = float4(a0 * SpecularLightsColor[0].xyz, a0);
        float4 c1 = float4(a1 * SpecularLightsColor[1].xyz, a1);
        o.sdir = a1 < a0 ? float4(L0, 1) : (a0 < a1 ? float4(L1, 1) : float4(0, 0, -1, 0));
        o.scol = a1 < a0 ? c0 : (a0 < a1 ? c1 : 0);
    }
#endif
    return o;
}

// ---- pixel shader -------------------------------------------------------

#if SHADOWTYPE == 2
// 2x2 compare with bilinear weights, taps at and behind uv as in stock
float pcf(sampler2D smp, float4 sp)
{
    float rw = 1.0 / sp.w;
    float2 uv = sp.xy * rw;
    float z = sp.z * rw;
    float t = gvShadowSize.z;
    float s00 = (z - tex2D(smp, uv).x) <= 0 ? 1 : 0;
    float s10 = (z - tex2D(smp, uv - float2(t, 0)).x) <= 0 ? 1 : 0;
    float s01 = (z - tex2D(smp, uv - float2(0, t)).x) <= 0 ? 1 : 0;
    float s11 = (z - tex2D(smp, uv - float2(t, t)).x) <= 0 ? 1 : 0;
    float2 f = frac(uv * gvShadowSize.x);
    return lerp(lerp(s11, s01, f.x), lerp(s10, s00, f.x), f.y);
}

// the same compare with explicit-LOD reads, usable inside a dynamic branch
float pcf_lod(sampler2D smp, float4 sp)
{
    float rw = 1.0 / sp.w;
    float2 uv = sp.xy * rw;
    float z = sp.z * rw;
    float t = gvShadowSize.z;
    float s00 = (z - tex2Dlod(smp, float4(uv, 0, 0)).x) <= 0 ? 1 : 0;
    float s10 = (z - tex2Dlod(smp, float4(uv - float2(t, 0), 0, 0)).x) <= 0 ? 1 : 0;
    float s01 = (z - tex2Dlod(smp, float4(uv - float2(0, t), 0, 0)).x) <= 0 ? 1 : 0;
    float s11 = (z - tex2Dlod(smp, float4(uv - float2(t, t), 0, 0)).x) <= 0 ? 1 : 0;
    float2 f = frac(uv * gvShadowSize.x);
    return lerp(lerp(s11, s01, f.x), lerp(s10, s00, f.x), f.y);
}
#endif

// world position of this pixel (see WP8)
float3 world_pos(VS_OUT i)
{
#if !NM_SPEC
    return i.tpos.xyz;
#elif WP8
    return i.wp.xyz;
#else
    return i.t5.xyz;
#endif
}

#if SHADOWTYPE
// x: the stock shadow term; y: the uncapped one the shadow fill uses (the
// main map is not remapped to (s + 1) / 2, a cap stock needed only because
// its shadow darkens all the light)
// inside a shadow map's square (1) or outside it (0)
float in_map(float4 sp)
{
    float2 u = sp.xy / sp.w;
    return all(u >= 0 && u <= 1) ? 1 : 0;
}

// dbg: the debug view (gvUltraMat.w): r the near map's term inside its
// square, g the wide map's inside its square, b a constant
float2 shadow_sample(VS_OUT i, float2 vpos, out float3 dbg)
{
    float fw = 0;       // weight of the fine outdoor map (gvUltraLook.w)
#if SHADOWTYPE == 1
    float m = tex2D(ShadowMapSampler, i.shpos).x;
#else
    float m = pcf(ColorShadowMapSampler, i.shpos);
    [branch] if (gvUltraShadow.x > 0)
        m = pcss(ColorShadowMapSampler, i.shpos, vpos, 1.0);
#endif
#if INDOOR
    // indoors: the main map alone, no remap, no second map; indoor props
    // treat surfaces facing away from the shadow light as fully shadowed
#if !LIGHTMAP
    m = dot(ShadowLightDir, i.nrmw.xyz) >= 0 ? 0 : m;
#endif
    // the map covers a square ahead of the camera; beyond it the lookup
    // clamped to the border texels and drew a straight-edged false shadow
    // across whatever sat there (a tunnel door at a distance, 2026-09-23):
    // fade to lit over its outer 8%, lit outside (with the level's shadow
    // fixes on, gvUltraAct.z; 0 is stock)
    [branch] if (gvUltraAct.z > 0) {
        float2 mu = i.shpos.xy / i.shpos.w;
        float2 me = min(mu, 1.0 - mu);
        m = lerp(1.0, m, saturate(min(me.x, me.y) / 0.08));
    }
    dbg = float3(0, m * in_map(i.shpos), 0.25);
    return float2(m, m);
#else
#if SHADOWTYPE == 1
    float s2 = tex2D(ShadowMapDepthSampler, i.shpos2).x;
#else
    // the second map carries the full-strength outdoor shadow (the main
    // one is capped at half by the remap below), so it needs PCSS too
    [branch] if (gvUltraLook.w > 0) {
        // the fine map where this pixel is inside it, faded out over its
        // outer 12% into the zone-wide one
        float3 fN = normalize(i.nrmw.xyz);
        float fndl = dot(fN, ShadowLightDir);
        float4 fp = mul(float4(world_pos(i) + fN * (gvUltraAct.z * (0.3 + 0.7 * sqrt(saturate(1.0 - fndl * fndl)))), 1.0),
                        gmUltraFine);
        // no valid matrix yet (the DLL fills it per mesh): w is 0, skip
        float2 fu = fp.w > 1e-6 ? fp.xy / fp.w : float2(-1, -1);
        float2 fe = min(fu, 1.0 - fu);
        fw = saturate(min(fe.x, fe.y) / 0.12);
        [branch] if (fw > 0) {
            float mf = pcf_lod(UltraFineSampler, fp);
            [branch] if (gvUltraShadow.x > 0)
                mf = pcss(UltraFineSampler, fp, vpos, map_ratio(gmUltraFine, gmShadowMatrix));
            // a NaN here would spread over the whole screen through the
            // glow (first in-game run: white-out); treat it as lit
            mf = mf >= 0 && mf <= 1 ? mf : 1.0;
            m = lerp(m, mf, fw);
        }
    }
    float s2 = pcf(ExtraColorShadowMapSampler, i.shpos2);
    [branch] if (gvUltraShadow.x > 0) {
        s2 = pcss(ExtraColorShadowMapSampler, i.shpos2, vpos, map_ratio(gmShadowMatrix2, gmShadowMatrix));
        // this near map holds every character and prop but covers only a
        // small square ahead of the camera: fade its shadows out over the
        // outer 15% instead of cutting them off at the edge
        float2 u2 = i.shpos2.xy / i.shpos2.w;
        float2 e2 = min(u2, 1.0 - u2);
        s2 = lerp(1.0, s2, saturate(min(e2.x, e2.y) / 0.15));
    }
#endif
    float s = min(s2, (m + 1.0) * 0.5);
    // b: with the fine map per pixel, its weight here; otherwise which wide
    // map the engine gave this mesh (bright = the zone-wide one)
    dbg = float3(s2 * in_map(i.shpos2), m * in_map(i.shpos),
                 gvUltraLook.w > 0 ? 0.1 + 0.8 * fw :
                 1.0 / length(float3(gmShadowMatrix._11, gmShadowMatrix._21, gmShadowMatrix._31)) > 120 ? 0.9 : 0.1);
    return float2(dot(ShadowLightDir, i.nrmw.xyz) >= 0 ? 0.5 : s, min(s2, m));
#endif
}
#endif

float4 ps_main(VS_OUT i, float2 vpos : VPOS) : COLOR
{
    float2 uv = i.uv.zw;

    float3 light = i.col.xyz * 2.0 * i.tpos.w;
#if LIGHTMAP
    {
        float3 lm = tex2D(LightMapSampler, i.uv.xy).xyz;
        float2 lmdx = ddx(i.uv.xy), lmdy = ddy(i.uv.xy);     // outside the branch
        [branch] if (gvUltraLM.x > 0)
            lm = tex2D_bicubic(LightMapSampler, i.uv.xy, gvUltraLM.yz, lmdx, lmdy);
        light += lm;
    }
#endif
    // normal-map detail in the diffuse light (gvUltraDetail): kd for the
    // direct sun, ka for everything else; both exactly 1 when off
    float kd = 1.0, ka = 1.0, kp = 1.0;
#if NM_SPEC
    float2 nmxy = tex2D(NormalMapSampler, uv).wy * 2.0 - 1.0;
    float3 nm = normalize(float3(nmxy, sqrt(saturate(1.0 - dot(nmxy, nmxy)))));
    [branch] if (gvUltraDetail.x > 0 || gvUltraDetail.y > 0) {
        // the dominant light in tangent space: the sun's specular light
        // outdoors, the chosen nearby light indoors (straight above if none)
        float3 L = i.sdir.xyz;
        L = dot(L, L) > 1e-8 ? normalize(L) : float3(0, 0, 1);
#if INDOOR
        L = i.sdir.w > 0 ? L : float3(0, 0, 1);
#endif
        float dirr = saturate(dot(nm, L)) / max(saturate(L.z), 0.1);
        float halfr = (dot(nm, L) * 0.5 + 0.5) / max(L.z * 0.5 + 0.5, 0.1);
        kd = lerp(1.0, min(dirr, 2.0), gvUltraDetail.x);
        ka = lerp(1.0, min(halfr, 2.0), gvUltraDetail.y);
        kp = lerp(1.0, nm.z, gvUltraDetail.y);       // point lights: tilt only
    }
#endif
#if SPECULAR || CUBEENVMAP
    float4 sm = tex2D(SpecularMapSampler, uv) + gvMiscMaterialData.x;
#endif
#if SHADOWTYPE
    float3 sdbg;
    float2 ssh = shadow_sample(i, vpos, sdbg);
    float sraw = ssh.x;
    float sf = sraw * gvMiscLightingData.y + (1.0 - gvMiscLightingData.y);
#if !INDOOR
    // shadow fill (gvUltraMat.x): at 1 the shadow removes the dynamic sun
    // term and nothing else; the light map keeps its baked light and shadows
    float3 sun = i.t5.xyz * 2.0 * i.tpos.w;
    [branch] if (ka != 1.0 || kd != 1.0) {          // normal-map detail; skipped when off: stock parity
        light = (light - sun) * ka + sun * kd;
        sun *= kd;
    }
    light = lerp(light * sf, light - sun * (1.0 - ssh.y), gvUltraMat.x);
#else
    light *= ka;
    // shadow fill indoors: the shadow is aimed along the environment's light
    // direction (no light of the scene), so at 1 it only takes the light
    // above the flat ambient. A surface in a baked shadow is already near
    // that floor and is not darkened a second time. Not the SH: props have
    // no light map and SH is most of their light, so with it in the floor
    // they stopped shadowing themselves (2026-09-23).
    float3 flo = LightAmbient.xyz;
    flo = min(light, flo * ((1.0 + gvUltraLook.x) * i.tpos.w) * ka);
    light = lerp(light * sf, flo + (light - flo) * sf, gvUltraMat.x);
#endif
#else
    light *= ka;
#endif
    // per-pixel point lights, added after the sun's shadow outdoors (which
    // in stock darkened them too) and without the baked sun visibility in
    // tpos.w
    float3 plspec = 0;
#if POINTLIGHTS
    [branch] if (gvUltraPL.x > 0) {
#if !NM_SPEC
        float3 P = i.tpos.xyz;
#elif WP8
        float3 P = i.wp.xyz;
#else
        float3 P = i.t5.xyz;
#endif
#if SPECULAR
        float plpw = surf_power(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);
#else
        float plpw = 16;
#endif
        float3 pl = point_lights(POINTLIGHTS, P, normalize(i.nrmw.xyz), normalize(EyeInWorld.xyz - P), plpw, plspec);
#if SHADOWTYPE && INDOOR
        // indoors they take the shadow, as stock's vertex lights did: they
        // are most of a prop's light, and exempt they took its shadow away
        // (per-pixel lights on, 2026-09-23)
        pl *= sf;
        plspec *= sf;
#endif
        light += pl * kp;
    }
#endif

    float4 d1 = tex2D(DiffuseMapSampler, uv);
    float3 albedo = d1.xyz;
#if DIFFUSEMAP2
    float4 d2 = tex2D(DiffuseMapSampler2, float2(i.t5.w, i.eye.w));
    albedo = lerp(albedo, d2.xyz, d2.w);
#endif
    float3 c = light * albedo;

#if CUBEENVMAP
    {
#if !VS_REFL
        float3 I = normalize(i.tpos.xyz - EyeInWorld.xyz);
        float3 Nn = normalize(i.nrmw.xyz);
        float3 R = I - 2.0 * dot(I, Nn) * Nn;
#else
        float3 R = normalize(i.shpos2.xyz);
#endif
        float3 env = texCUBEbias(CubeEnvironmentMapSampler, float4(R, gvEnvironmentMapData.w + gvUltraSurf.w)).xyz;
        float amt = length(sm.xyz) * gfSpecularPower * gvEnvironmentMapData.x * (1.0 + gvUltraSurf.z);
        amt = (gvEnvironmentMapData.y - sm.w * gvEnvironmentMapData.z) >= 0 ? amt : 0;
        c = amt * (env - albedo * light) + c;
    }
#endif

    float m = max(max(c.x, max(c.y, c.z)), 1.0);
    float over = m - 1.0;

    float3 spec = 0;
    float specglow = 0;
#if NM_SPEC
    {
        float2 nxy = nmxy;
        float3 n = normalize(float3(nxy, sqrt(1.0 - nxy.x * nxy.x - nxy.y * nxy.y)));
        float3 V = (i.eye.xyz - i.tpos.xyz) * rsqrt(dot(i.eye.xyz - i.tpos.xyz, i.eye.xyz - i.tpos.xyz));
        float3 H = normalize(V + normalize(i.sdir.xyz));
        float pw = surf_power(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);
        float p = pow(max(dot(H, n), 0), pw);
#if INDOOR
        float4 lc = i.sdir.w * i.scol;
#else
        float4 lc = DirLightsColor[2];
#endif
        spec = (sm.xyz * gvSpecularMaterialData.z) * p * lc.xyz;
        specglow = p * lc.w;
    }
#endif

#if SPECULAR && !NM_SPEC
    {
        // no normal map: world-space highlight, from directional light 2
        // outdoors and from the chosen specular light indoors
        float3 V = (i.eye.xyz - i.tpos.xyz) * rsqrt(dot(i.eye.xyz - i.tpos.xyz, i.eye.xyz - i.tpos.xyz));
#if INDOOR
        float3 H = normalize(V + normalize(i.sdir.xyz));
        float4 lc = i.sdir.w * i.scol;
#else
        float3 H = normalize(V + _DirLightsDir_1[2].xyz);
        float4 lc = DirLightsColor[2];
#endif
        float pw = surf_power(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);
        float p = pow(max(dot(H, normalize(i.nrmw.xyz)), 0), pw);
        spec = (sm.xyz * gvSpecularMaterialData.z) * p * lc.xyz;
        specglow = p * lc.w;
    }
#endif
#if SHADOWTYPE && !INDOOR          // indoors the highlight ignores the shadow
    // stock quirk: the highlight takes lerp(y, 1, s), the light factor with
    // its ends swapped, so a strong shadow intensity barely dims it
    spec *= lerp(sraw * (1.0 - gvMiscLightingData.y) + gvMiscLightingData.y, ssh.y, gvUltraMat.x);
#endif
#if SPECULAR
    spec += (sm.xyz * gvSpecularMaterialData.z) * plspec;
#endif
#if SPECULAR
    spec *= surf_spec(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);   // gloss, strength (gvUltraSurf)
#endif
    float3 col = c * (1.0 / m) + spec;
    float glow = over * 0.5;
    glow = glow * glow;
#if SPECULAR
    glow = specglow * (length(sm.xyz) * gfSpecularPower * gvSpecularMaterialData.w * gvSpecularMaterialData.z) + glow;
#endif

#if SELFILLUM
    float4 si = tex2D(SelfIlluminationMapSampler, uv);
#if SCROLLUV
    float2 suv = gfScrollActive[2] * ((uv + gfScrollTextures[0].fPhase) * gfScrollTextures[0].fTile - uv) + uv;
    float3 emis = tex2D(SelfIlluminationMapSampler, suv).xyz * (si.w * gvMiscMaterialData.z);
#else
    float3 emis = si.xyz * gvMiscMaterialData.z;     // unlike actors: not tinted by the albedo
#endif
    col += emis;
    glow = dot(emis, emis) * gvMiscMaterialData.y + glow;
#endif

    float3 rgb = saturate(i.col.w) * (col - FogColor.xyz) + FogColor.xyz;
#if SHADOWTYPE
    [branch] if (gvUltraMat.w > 0)
        rgb = lerp(rgb, sdbg, 0.75);
#endif
    glow = max(glow * i.col.w, 0.004);
    float a = glow * gvMiscLightingData.w + (1.0 - gvMiscLightingData.w) * gvMiscLightingData.z;
    return float4(rgb, d1.w * a);
}
