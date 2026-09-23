// Actor material shaders (actoroutdoor30 / actorindoor30), rewritten from
// the stock bytecode so we own the source (docs/graphics-plan.md step 7).
//
// Stage 1 of the rewrite is PARITY: every technique compiled from this file
// must draw the same pixels as the stock one (tools/fx/fxdiff.c checks all of
// them). Only once that holds do lighting changes go in, behind #if so a
// parity build stays one define away.
//
// One compile per technique feature set; tools/fx/mkmat.py derives the defines
// from each stock technique's annotations:
//   INDOOR       0/1  actorindoor30 (camera light + point lights, no sun)
//   POINTLIGHTS  0/2  indoor only: lights in the VS (no normal map) or per
//                     pixel in tangent space (normal map)
//   SHADOWTYPE   0/1/2  none / depth map (hardware compare) / colour map (PCF here)
//   SKINNED, NORMALMAP, SELFILLUM, SPECULAR, CUBEENVMAP, SCATTER, SCROLLUV
//   PL_ULTRA     1 in our "_pl5" techniques (tools/fx/mkmat.py): five engine
//                point lights per pixel in world space (ultra.hlsl
//                point_lights), in the one pass; POINTLIGHTS is 0 there
//
// What the stock shaders do, read from the disassembly and the preshaders
// (tools/fx/hgfx.py pres):
// - Vertex colour carries half the diffuse fill: SH + LightAmbient + the
//   second directional light (outdoor) or the point lights (indoor, no
//   normal map). The PS doubles it back.
// - Outdoor: directional light 0 per pixel (tangent space with a normal map),
//   directional light 2 drives the specular highlight.
// - The dynamic shadow multiplies ALL of that fill, ambient and SH included,
//   by lerp(1, s, gvMiscLightingData.y). That is why actor shadows read darker
//   than the baked ones in the world.
// - The camera light is added after the shadow, only when its falloff is set.
// - Soft clamp: colour / max(1, max channel); the overflow feeds the glow
//   alpha together with specular and self-illumination.
// - Fog is per vertex, linear, from the UNSKINNED position.

#ifndef INDOOR
#define INDOOR 0
#endif
#ifndef POINTLIGHTS
#define POINTLIGHTS 0
#endif
#ifndef SHADOWTYPE
#define SHADOWTYPE 0
#endif
#ifndef SKINNED
#define SKINNED 0
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
#ifndef SCATTER
#define SCATTER 0
#endif
#ifndef SCROLLUV
#define SCROLLUV 0
#endif
#ifndef PL_ULTRA
#define PL_ULTRA 0
#endif

// The self-illumination map is also read for its alpha by the environment
// map (mask) and the scatter term (thickness).
#define READ_SELFILLUM (SELFILLUM || CUBEENVMAP || SCATTER)

// ---- parameters (names and types exactly as in the stock effects) --------

float4x4 WorldViewProjection;
float4x4 World;
float4   Bones[180];
float4x4 gmShadowMatrix;
float3   ShadowLightDir;
float4   _DirLightsDir_0[3];
float4   _DirLightsDir_1[3];
float4   DirLightsColor[3];
float4   _PointLightsPos_0[5];
float4   _PointLightsPos_1[5];
float4   PointLightsColor[5];
float4   _PointLightsFalloff_0[5];
float4   _PointLightsFalloff_1[5];
float4   _CameraLightPos_World;
float4   _CameraLightColor;
float4   _CameraLightFalloff_World;
float4   LightAmbient;
float4   EyeInObject;
float4   EyeInWorld;
float    FogMaxDistance;
float    FogMinDistance;
float    gfFogFactor;
float4   FogColor;
float4   cAr, cAg, cAb, cBr, cBg, cBb, cC;
float4   gvSpecularMaterialData;      // .x/.y power range (lerp by spec map alpha), .z scale, .w glow
float4   gvMiscLightingData;          // .y shadow intensity, .z/.w glow alpha mix
float4   gvMiscMaterialData;          // .x spec map bias, .y self-illum glow, .z self-illum scale
float4   gvEnvironmentMapData;        // .x strength, .y/.z mask by spec alpha, .w LOD bias
float4   gvSubsurfaceScatterColorPower;
float4   gvShadowSize;                // colour shadow map: .x size, .z texel
float    gfSpecularPower;
float    gfNormalPower;
struct SCROLL { float2 fTile; float2 fPhase; };
SCROLL   gfScrollTextures[2];
float    gfScrollActive[7];

// Same sampler registers as the stock shaders: the engine binds some of
// these by stage, and the texture cache assumes it owns them.
sampler2D   DiffuseMapSampler          : register(s0);
sampler2D   SelfIlluminationMapSampler : register(s2);
sampler2D   SpecularMapSampler         : register(s5);
sampler2D   NormalMapSampler           : register(s6);
samplerCUBE CubeEnvironmentMapSampler  : register(s7);
#if SHADOWTYPE == 2
sampler2D   ColorShadowMapSampler      : register(s10);
#if !INDOOR
// Outdoors every character and prop casts only into the near map, and the
// stock actor shader reads only the wide one: no self-shadowing, and no
// shadow from a big enemy standing over you. With gvUltraAct.x the near map
// is read too, through its world-space matrix (the DLL fills gmUltraNear;
// no interpolator is left for a second coordinate).
sampler2D   ExtraColorShadowMapSampler : register(s11);
float4x4    gmShadowMatrix2;             // read (for the texel ratio) so the engine sets it
float4x4    gmUltraNear;
#endif
#else
sampler2D   ShadowMapSampler           : register(s10);
#endif

#include "ultra.hlsl"

// ---- vertex shader ------------------------------------------------------

struct VS_IN {
    float4 pos  : POSITION;
#if SKINNED
    float4 bidx : BLENDINDICES;
    float4 bw   : BLENDWEIGHT;
#endif
    float2 uv   : TEXCOORD0;
    float3 nrm  : NORMAL;
#if NORMALMAP
    float3 tan  : TANGENT;
    float3 bin  : BINORMAL;      // stored 0..1
#endif
};

// The interpolator layout is the stock one, slot for slot.
struct VS_OUT {
    float4 hpos  : POSITION;
    float4 col   : COLOR0;       // rgb half the fill light, a fog
    float4 uv    : TEXCOORD0;
    float4 tpos  : TEXCOORD1;    // position (tangent space with a normal map)
    float4 nrmw  : TEXCOORD2;    // world normal
    float4 shpos : TEXCOORD3;    // shadow map coordinate
    float4 refl  : TEXCOORD4;    // xyz reflection vector, w "faces the shadow light"
    float4 ldir  : TEXCOORD5;    // light 0 (tangent space); indoor: point light 0, w attenuation
    float4 wpos  : TEXCOORD6;
    float4 eye   : TEXCOORD7;    // eye (tangent space with a normal map)
    float4 sdir  : COLOR1;       // specular light (tangent space); indoor: point light 1
};

#if SKINNED
float3 skin4(float4 p, int3 i, float4 w)
{
    float3 a, b, c;
    a.x = dot(Bones[i.y + 0], p); a.y = dot(Bones[i.y + 1], p); a.z = dot(Bones[i.y + 2], p);
    b.x = dot(Bones[i.x + 0], p); b.y = dot(Bones[i.x + 1], p); b.z = dot(Bones[i.x + 2], p);
    c.x = dot(Bones[i.z + 0], p); c.y = dot(Bones[i.z + 1], p); c.z = dot(Bones[i.z + 2], p);
    return w.z * c + (w.x * b + a * w.y);
}
float3 skin3(float3 d, int3 i, float4 w)
{
    float3 a, b, c;
    a.x = dot(Bones[i.y + 0].xyz, d); a.y = dot(Bones[i.y + 1].xyz, d); a.z = dot(Bones[i.y + 2].xyz, d);
    b.x = dot(Bones[i.x + 0].xyz, d); b.y = dot(Bones[i.x + 1].xyz, d); b.z = dot(Bones[i.x + 2].xyz, d);
    c.x = dot(Bones[i.z + 0].xyz, d); c.y = dot(Bones[i.z + 1].xyz, d); c.z = dot(Bones[i.z + 2].xyz, d);
    return w.z * c + (w.x * b + a * w.y);
}
#endif

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
    float4 p = float4(v.pos.xyz, 1.0);
#if SKINNED
    int3 bi = (int3)v.bidx.xyz;
    float3 pos = skin4(p, bi, v.bw);
    float3 N = normalize(skin3(v.nrm, bi, v.bw));
#if NORMALMAP
    float3 T = normalize(skin3(v.tan, bi, v.bw));
    float3 B = normalize(skin3(v.bin * 2.0 - 1.0, bi, v.bw));
#endif
#else
    float3 pos = v.pos.xyz;
    float3 N = v.nrm;
#if NORMALMAP
    float3 T = v.tan;
    float3 B = v.bin * 2.0 - 1.0;
#endif
#endif
    float4 P = float4(pos, 1.0);
    float3 Nw = mul(N, (float3x3)World);
    float3 wpos = mul(P, World).xyz;

    o.hpos = mul(P, WorldViewProjection);
    o.uv = float4(v.uv, 0, 0);
    o.nrmw = float4(Nw, 0);
    o.wpos = float4(wpos, 0);

#if NORMALMAP
    float3x3 TBN = float3x3(T, B, N);
    o.tpos = float4(mul(TBN, pos), 0);
    o.eye = float4(mul(TBN, EyeInObject.xyz), 0);
#else
    o.tpos = float4(pos, 0);
    o.eye = float4(EyeInWorld.xyz, 0);
#endif
#if PL_ULTRA && NORMALMAP
    // the tangent frame in world space, for the point lights to use the
    // normal map: T in uv.zw + tpos.w, B in nrmw.w + wpos.w + eye.w (the
    // only free components; every interpolator slot is taken)
    {
        float3 Tw = mul(T, (float3x3)World), Bw = mul(B, (float3x3)World);
        o.uv.zw = Tw.xy;
        o.tpos.w = Tw.z;
        o.nrmw.w = Bw.x;
        o.wpos.w = Bw.y;
        o.eye.w = Bw.z;
    }
#endif

    // fill light, halved into the colour interpolator
    float3 fill = (sh9(Nw) + LightAmbient.xyz) * (1.0 + gvUltraLook.x);
#if !INDOOR
    fill = DirLightsColor[1].xyz * saturate(dot(Nw, _DirLightsDir_1[1].xyz)) + fill;
#elif POINTLIGHTS && !NORMALMAP
    [loop] for (int k = 0; k < POINTLIGHTS; k++) {
        float3 L = _PointLightsPos_1[k].xyz - wpos;
        float rl = rsqrt(dot(L, L));
        float ndl = saturate(dot(Nw, L * rl));
        float att = saturate(_PointLightsFalloff_1[k].x - (1.0 / rl) * _PointLightsFalloff_1[k].y);
        fill = ndl * (att * PointLightsColor[k].xyz) + fill;
    }
#endif
    float dist = length(EyeInObject.xyz - v.pos.xyz);
    float fog = saturate(saturate((FogMaxDistance - dist) / (FogMaxDistance - fog_min())) + gfFogFactor);
    o.col = float4(fill * 0.5, fog);

    o.refl = 0;
    o.ldir = 0;
    o.sdir = 0;
    o.shpos = 0;
#if CUBEENVMAP
    float3 I = normalize(wpos - EyeInWorld.xyz);
    o.refl.xyz = I - 2.0 * dot(I, Nw) * Nw;
#endif
#if SHADOWTYPE
    float facing = dot(ShadowLightDir, Nw);
    float4 sp = mul(P, gmShadowMatrix);
#if INDOOR
    o.refl.w = 1;          // indoors only the zeroed coordinate marks back faces
#else
    o.refl.w = facing < 0 ? 1 : 0;
#endif
    o.shpos = sp - (facing >= 0 ? 1 : 0) * sp;
    // Stock zeroes the coordinate at vertices facing away from the shadow
    // light and lets the PS read that as "shadowed". Interpolated across a
    // triangle whose corners disagree, the coordinate is garbage, and the
    // shadow follows the triangle edges: faceted light and dark patches on
    // arms and faces (character select, once characters got the shadow
    // technique). Our character shadows keep the real coordinate: a surface
    // facing away gets no direct sun anyway.
    [branch] if (gvUltraAct.x > 0) {
        o.shpos = sp;
        o.refl.w = 1;
    }
#endif

#if NORMALMAP
#if !INDOOR
    o.ldir = float4(mul(TBN, _DirLightsDir_0[0].xyz), 0);
#if SPECULAR
    o.sdir = float4(mul(TBN, _DirLightsDir_0[2].xyz), 0);
#endif
#elif POINTLIGHTS
    {
        float3 L = _PointLightsPos_0[0].xyz - pos;
        float d = length(L);
        o.ldir = float4(mul(TBN, L), saturate(d * -_PointLightsFalloff_0[0].y + _PointLightsFalloff_0[0].x));
        L = _PointLightsPos_0[1].xyz - pos;
        d = length(L);
        o.sdir = float4(mul(TBN, L), saturate(d * -_PointLightsFalloff_0[1].y + _PointLightsFalloff_0[1].x));
    }
#endif
#endif
    return o;
}

// ---- pixel shader -------------------------------------------------------

#if SHADOWTYPE
// Shadow map sample, 1 lit / 0 shadowed; surfaces turned away from the
// shadow light count as shadowed.
float shadow_sample(VS_OUT i, float2 vpos)
{
#if SHADOWTYPE == 1
    float s = tex2D(ShadowMapSampler, i.shpos).x;
#elif SHADOWTYPE == 2
    // 2x2 compare with bilinear weights (the colour map holds depth in .r);
    // the taps sit at and BEHIND uv, as in the stock shader
    float rw = 1.0 / i.shpos.w;
    float2 uv = i.shpos.xy * rw;
    float z = i.shpos.z * rw;
    float t = gvShadowSize.z;
    float s00 = (z - tex2D(ColorShadowMapSampler, uv).x) <= 0 ? 1 : 0;
    float s10 = (z - tex2D(ColorShadowMapSampler, uv - float2(t, 0)).x) <= 0 ? 1 : 0;
    float s01 = (z - tex2D(ColorShadowMapSampler, uv - float2(0, t)).x) <= 0 ? 1 : 0;
    float s11 = (z - tex2D(ColorShadowMapSampler, uv - float2(t, t)).x) <= 0 ? 1 : 0;
    float2 f = frac(uv * gvShadowSize.x);
    float s = lerp(lerp(s11, s01, f.x), lerp(s10, s00, f.x), f.y);
    [branch] if (gvUltraShadow.x > 0)
        s = pcss(ColorShadowMapSampler, i.shpos, vpos, 1.0);
#if !INDOOR
    [branch] if (gvUltraAct.x > 0) {
        // offset along the normal (gvUltraAct.y world units) against self-shadow acne
        float3 P = i.wpos.xyz + normalize(i.nrmw.xyz) * gvUltraAct.y;
        float4 np = mul(float4(P, 1.0), gmUltraNear);
        float2 nu = np.w > 1e-6 ? np.xy / np.w : float2(-1, -1);
        float2 ne = min(nu, 1.0 - nu);
        float nw = saturate(min(ne.x, ne.y) / 0.12);
        [branch] if (nw > 0) {
            // noise-free and a little more bias: this is mostly the
            // character shadowing itself, animated, redrawn every other frame
            float sn = pcf9(ExtraColorShadowMapSampler, np, 2.0 / max(map_ratio(gmShadowMatrix2, gmShadowMatrix), 1.0));
            sn = sn >= 0 && sn <= 1 ? sn : 1.0;
            s = min(s, lerp(1.0, sn, nw));
        }
    }
#endif
#endif
    return i.refl.w ? s : i.refl.w;
}
#endif

float4 ps_main(VS_OUT i, float2 vpos : VPOS) : COLOR
{
    float2 uv = i.uv.xy;
#if NORMALMAP
    float2 nxy = tex2D(NormalMapSampler, uv).wy * 2.0 - 1.0;
    float3 n = normalize(float3(nxy, sqrt(1.0 - nxy.x * nxy.x - nxy.y * nxy.y))) * gfNormalPower;
#else
    float3 n = normalize(i.nrmw.xyz);
#endif

#if SPECULAR || CUBEENVMAP
    float4 sm = tex2D(SpecularMapSampler, uv) + gvMiscMaterialData.x;
#endif

    // ---- diffuse light
    float3 light = i.col.xyz * 2.0;
    float ndl0 = 0;
    float3 spec = 0;
    float specglow = 0;
#if !INDOOR
#if NORMALMAP
    ndl0 = saturate(dot(n, normalize(i.ldir.xyz)));
#else
    ndl0 = saturate(dot(n, _DirLightsDir_1[0].xyz));
#endif
    float3 direct = DirLightsColor[0].xyz * (ndl0 * (1.0 + gvUltraLook.z));    // the sun, per pixel
    light = direct + light;
#elif NORMALMAP && POINTLIGHTS
    {
        float3 V = (i.eye.xyz - i.tpos.xyz) * rsqrt(dot(i.eye.xyz - i.tpos.xyz, i.eye.xyz - i.tpos.xyz));
        float3 L0 = normalize(i.ldir.xyz), L1 = normalize(i.sdir.xyz);
        float4 c0 = PointLightsColor[0] * i.ldir.w, c1 = PointLightsColor[1] * i.sdir.w;
        light = saturate(dot(n, L0)) * c0.xyz + saturate(dot(n, L1)) * c1.xyz + light;
#if SPECULAR
        float pw = surf_power(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);
        float p1 = pow(max(dot(normalize(V + L1), n), 0), pw);
        float p0 = pow(max(dot(normalize(V + L0), n), 0), pw);
        float3 ss = sm.xyz * gvSpecularMaterialData.z;
        spec = c0.xyz * (ss * p0) + (ss * p1) * c1.xyz;
        specglow = p0 * c0.w + p1 * c1.w;
#endif
    }
#elif POINTLIGHTS && SPECULAR
    {
        // no normal map: the point lights are in the vertex colour, but
        // their highlights are per pixel in world space
        float3 N = normalize(i.nrmw.xyz);
        float3 V = normalize(EyeInWorld.xyz - i.wpos.xyz);
        float pw = surf_power(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);
        float3 ss = sm.xyz * gvSpecularMaterialData.z;
        [unroll] for (int k = 0; k < POINTLIGHTS; k++) {
            float3 L = _PointLightsPos_1[k].xyz - i.wpos.xyz;
            float rl = rsqrt(dot(L, L));
            float p = pow(max(dot(normalize(L * rl + V), N), 0), pw);
            float4 lc = saturate((1.0 / rl) * -_PointLightsFalloff_1[k].y + _PointLightsFalloff_1[k].x) * PointLightsColor[k];
            spec = lc.xyz * (ss * p) + spec;
            specglow = p * lc.w + specglow;
        }
    }
#endif

#if SHADOWTYPE
    // the shadow darkens all the fill (ambient and SH too), then the
    // highlight takes the raw sample
    float sraw = shadow_sample(i, vpos);
#if !INDOOR
    // shadow fill (gvUltraMat.x): at 1 the shadow takes away the sun and
    // nothing else, like the baked shadows in the world around it
    float3 lit_stock = light * ((sraw * gvMiscLightingData.y - gvMiscLightingData.y) + 1.0);
    float3 lit_fill = (light - direct) + direct * sraw;
    light = lerp(lit_stock, lit_fill, gvUltraMat.x);
#else
    // shadow fill indoors: only the light above the flat ambient, as on the
    // backgrounds around the character (SH is most of a character's light:
    // in the floor, it took their shadows away)
    float sfi = (sraw * gvMiscLightingData.y - gvMiscLightingData.y) + 1.0;
    float3 flo = min(light, LightAmbient.xyz * (1.0 + gvUltraLook.x));
    light = lerp(light * sfi, flo + (light - flo) * sfi, gvUltraMat.x);
    sfi = lerp(sfi, 1.0, gvUltraMat.x);         // for the point lights below
#endif
#endif

    // the engine's point lights, per pixel (our _pl5 techniques): after the
    // shadow outdoors, where it is the sun's; indoors they take it as stock's
    // vertex lights did, unless the shadow fill is on
    float3 plspec = 0;
#if PL_ULTRA
    {
        float3 P = i.wpos.xyz;
#if SPECULAR
        float plpw = surf_power(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);
#else
        float plpw = 16;
#endif
#if NORMALMAP
        // the normal-mapped normal, tangent space to world (the stock
        // point lights with a normal map were lit with it too)
        float3 Tw = float3(i.uv.zw, i.tpos.w), Bw = float3(i.nrmw.w, i.wpos.w, i.eye.w);
        float3 Nw = normalize(n.x * Tw + n.y * Bw + n.z * i.nrmw.xyz);
#else
        float3 Nw = normalize(i.nrmw.xyz);
#endif
        float3 pl = point_lights(5, P, Nw, normalize(EyeInWorld.xyz - P), plpw, plspec);
#if SHADOWTYPE && INDOOR
        pl *= sfi;
        plspec *= sfi;
#endif
        light += pl;
    }
#endif

    // camera light, after the shadow
    if (_CameraLightFalloff_World.x != 0 || _CameraLightFalloff_World.y != 0) {
        float3 L = _CameraLightPos_World.xyz - i.wpos.xyz;
        float rl = rsqrt(dot(L, L));
        float att = saturate((1.0 / rl) * -_CameraLightFalloff_World.y + _CameraLightFalloff_World.x);
        light = saturate(dot(normalize(i.nrmw.xyz), L * rl)) * (att * _CameraLightColor.xyz) + light;
    }

    float4 albedo = tex2D(DiffuseMapSampler, uv);
    float3 c = light * albedo.xyz;

#if READ_SELFILLUM
    float4 si = tex2D(SelfIlluminationMapSampler, uv);
#endif
#if CUBEENVMAP
    float envamt = length(sm.xyz) * gfSpecularPower * gvEnvironmentMapData.x * (1.0 + gvUltraSurf.z);
    envamt = (gvEnvironmentMapData.y - sm.w * gvEnvironmentMapData.z) >= 0 ? envamt : 0;
    float3 env = texCUBEbias(CubeEnvironmentMapSampler, float4(normalize(i.refl.xyz), gvEnvironmentMapData.w + gvUltraSurf.w)).xyz;
    if (si.w != 1.0) { env = 0; envamt = 0; }
    c = envamt * (env - albedo.xyz * light) + c;
#endif

    // soft clamp; the overflow becomes glow
    float m = max(max(c.x, max(c.y, c.z)), 1.0);
    float over = m - 1.0;

    // ---- specular (outdoor: directional light 2)
#if SPECULAR && !INDOOR
    {
#if NORMALMAP
        float3 V = (i.eye.xyz - i.tpos.xyz) * rsqrt(dot(i.eye.xyz - i.tpos.xyz, i.eye.xyz - i.tpos.xyz));
        float3 H = normalize(V + normalize(i.sdir.xyz));
#else
        float3 V = (i.eye.xyz - i.wpos.xyz) * rsqrt(dot(i.eye.xyz - i.wpos.xyz, i.eye.xyz - i.wpos.xyz));
        float3 H = normalize(V + _DirLightsDir_1[2].xyz);
#endif
        float pw = surf_power(sm.w * (gvSpecularMaterialData.y - gvSpecularMaterialData.x) + gvSpecularMaterialData.x);
        float p = pow(max(dot(H, n), 0), pw);
        spec = (sm.xyz * gvSpecularMaterialData.z) * p * DirLightsColor[2].xyz;
#if SHADOWTYPE
        spec *= sraw;
#endif
        specglow = p * DirLightsColor[2].w;
    }
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
#if SCROLLUV
    float2 suv = gfScrollActive[2] * ((uv + gfScrollTextures[0].fPhase) * gfScrollTextures[0].fTile - uv) + uv;
    float3 emis = albedo.xyz * tex2D(SelfIlluminationMapSampler, suv).xyz * (si.w * gvMiscMaterialData.z);
#else
    float3 emis = albedo.xyz * si.xyz * gvMiscMaterialData.z;
#endif
    col += emis;
    glow = dot(emis, emis) * gvMiscMaterialData.y + glow;
#endif

#if SCATTER
    {
        float3 V = normalize(EyeInWorld.xyz - i.wpos.xyz);
        float f = saturate(1.0 - dot(normalize(i.nrmw.xyz), V));
        f = gvMiscMaterialData.w * (f * f - f) + f;
        col += (f * (si.w * gvSubsurfaceScatterColorPower.w)) * gvSubsurfaceScatterColorPower.xyz;
    }
#endif

    float3 rgb = saturate(i.col.w) * (col - FogColor.xyz) + FogColor.xyz;
    glow = max(glow * i.col.w, 0.004);
    float a = glow * gvMiscLightingData.w + (1.0 - gvMiscLightingData.w) * gvMiscLightingData.z;
    return float4(rgb, albedo.w * a);
}
