// Our runtime knobs and the soft-shadow filter, shared by actor.hlsl and
// background.hlsl (plan step 8).
//
// Both parameters are added to the stock effects by tools/fx/mkmat.py with an
// all-zero default and are set only by the DLL (src/gfxprobe.c, from the
// panel). All zero is the stock look, which is what tools/fx/matcheck.sh proves;
// every term below is written so that zero reproduces stock exactly.
//
// gvUltraMat.x   shadow fill, 0..1. 0: the dynamic shadow scales all the
//                light (stock). 1: it removes only the sun's direct term, so
//                ambient, SH, light maps and fill lights survive in shadow.
//                Outdoor materials only; indoors there is no sun term.
// gvUltraMat.y   PCSS minimum filter radius, texels: real contact shadows
//                are sharp, but at shadow-map resolution a 1-texel edge
//                reads as aliasing, not as sharpness
// gvUltraMat.z   PCSS penumbra scale for INDOOR materials (gvUltraShadow.y
//                is the outdoor one): the indoor key light is a smaller,
//                nearer source than the sun
// gvUltraMat.w   shadow-map debug view (> 0): backgrounds show red = the
//                near map's term inside its square, green = the wide map's
//                inside its square (bright = lit, dark = shadowed, black
//                = outside both), blue = the mesh reads the zone-wide map
//                rather than the 80-unit one
// gvUltraShadow  PCSS on the colour shadow map (ShadowType 2):
//                .x on (> 0)
//                .y penumbra scale, texels of blur per unit of light-space
//                   depth between blocker and receiver (the sun's size)
//                .z largest filter radius, texels (also the search radius)
//                .w depth bias, light-space depth per texel of filter
//                   radius; the stock compare uses none, and too much
//                   loses the shadow where the caster meets the ground

// gvUltraLook   scene look (the 2018 data is flatter than 2007: ~3x the
//               ambient fill, fog from ~2 m instead of ~10 m; LOG 2026-09-22)
//               .x fill: ambient + SH scale - 1 (dynamic fill only; light
//                  maps are baked and stay)
//               .y fog start: moves the fog's near distance this fraction of
//                  the way to its far distance
//               .z sun: directional light 0 scale - 1
// gvUltraPL     point lights in the base pass (plan: roadmap item 1)
//               .x per pixel (> 0) instead of the stock per-vertex sum
//               .y falloff: 0 the stock linear ramp, 1 a windowed
//                  inverse-square (a hot core that fades smoothly to zero
//                  at the same radius)
//               .z strength - 1
//               .w specular from point lights (0 none, 1 the material's own)
float4 gvUltraMat;
float4 gvUltraShadow;
float4 gvUltraLook;
float4 gvUltraPL;

// fog start pushed out by gvUltraLook.y (0 = stock)
float fog_min()
{
    return gvUltraLook.y * (FogMaxDistance - FogMinDistance) + FogMinDistance;
}

// Tap k of a 16-point Vogel (golden-angle) disk, rotated by `rot`. Computed
// rather than read from a table, so the loops below stay real loops:
// unrolled, the 32 taps multiplied compile time across ~1,000 variants.
float2 vogel16(int k, float rot)
{
    float r = sqrt((k + 0.5) / 16.0);
    float t = k * 2.39996323 + rot;
    return r * float2(cos(t), sin(t));
}

// Interleaved gradient noise (Jimenez 2014): a per-pixel rotation that turns
// 16 taps' banding into fine grain.
float ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Depth compare with bilinear weights over the 4 nearest texels: smooth
// where a plain compare steps from lit to shadowed at texel edges. On this
// point-sampled R32F map a texel spans several screen pixels, so without it
// the rotated taps show up as speckle.
float cmp_bilinear(sampler2D smp, float2 uv, float zref)
{
    float2 t = uv * gvShadowSize.x - 0.5;
    float2 f = frac(t);
    float2 b = (floor(t) + 0.5) * gvShadowSize.z;
    float e = gvShadowSize.z;
    float s00 = zref <= tex2Dlod(smp, float4(b, 0, 0)).x ? 1 : 0;
    float s10 = zref <= tex2Dlod(smp, float4(b + float2(e, 0), 0, 0)).x ? 1 : 0;
    float s01 = zref <= tex2Dlod(smp, float4(b + float2(0, e), 0, 0)).x ? 1 : 0;
    float s11 = zref <= tex2Dlod(smp, float4(b + float2(e, e), 0, 0)).x ? 1 : 0;
    return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Percentage-closer soft shadows (Fernando 2005) on a map that stores
// light-space depth in .r (the engine's colour shadow map; the sun is
// orthographic, so depth is linear and the penumbra is simply proportional
// to the blocker-receiver distance). Returns 1 lit .. 0 shadowed.
// Texels of this map per unit of its depth, relative to the main map's:
// the penumbra is (receiver - blocker depth) x sun size, and the same world
// gap spans different depth and texel counts in maps that cover different
// areas. The main map is 1 (the unit the sun-size knobs were tuned in).
float map_ratio(float4x4 M, float4x4 Mmain)
{
    float a = length(float3(M._11, M._21, M._31)) / length(float3(M._13, M._23, M._33));
    float b = length(float3(Mmain._11, Mmain._21, Mmain._31)) / length(float3(Mmain._13, Mmain._23, Mmain._33));
    return a / b;
}

// k: map_ratio of this map (1 for the main one)
float pcss(sampler2D smp, float4 sp, float2 vpos, float k)
{
    float2 uv = sp.xy / sp.w;
    float z = sp.z / sp.w;
    float texel = gvShadowSize.z;
    float maxr = gvUltraShadow.z;
    float rot = ign(vpos) * 6.2831853;

    // 1. blocker search over the widest possible penumbra. The bias here is
    //    the small one: near contact the caster is barely above the ground,
    //    and a wide bias skipped it on some pixels and not others (a dotted
    //    fringe along the feet side of every shadow, first in-game run).
    float zsum = 0, nb = 0;
    // bias per texel of this map: a finer map (k > 1) has proportionally
    // less depth change per texel, so it needs 1/k of the main map's
    float sbias = gvUltraShadow.w / k;
    [loop] for (int k = 0; k < 16; k++) {
        float d = tex2Dlod(smp, float4(uv + vogel16(k, rot) * (maxr * texel), 0, 0)).x;
        if (d < z - sbias) { zsum += d; nb += 1; }
    }
    if (nb == 0) return 1;

    // 2. penumbra from the average blocker depth: sharp at contact
#if INDOOR
    float scale = gvUltraMat.z;
#else
    float scale = gvUltraShadow.y;
#endif
    float r = clamp((z - zsum / nb) * scale * k, max(1.0, gvUltraMat.y), maxr);

    // 3. filter over that radius, each tap a bilinear compare
    float zref = z - sbias * r;
    float lit = 0;
    [loop] for (int j = 0; j < 16; j++)
        lit += cmp_bilinear(smp, uv + vogel16(j, rot) * (r * texel), zref);
    return lit / 16;
}

// Attenuation of one point light at distance d. The engine's falloff is
// linear, saturate(F.x - d * F.y), reaching zero at d0 = F.x / F.y; F.x > 1
// gives a flat plateau near the light. The smooth curve keeps that radius:
// 1 / (1 + 8 q^2) with q = d / d0, windowed to zero at q = 1 (Karis 2013)
// and scaled so it matches the linear ramp around half the radius.
float pl_atten(float4 F, float d)
{
    float lin = saturate(F.x - d * F.y);
    float q = saturate(d * F.y / max(F.x, 1e-4));
    float w = saturate(1.0 - q * q * q * q);
    float sm = saturate(F.x) * (w * w) * 1.7 / (1.0 + 8.0 * q * q);
    return lerp(lin, sm, gvUltraPL.y);
}

// The first n engine point lights, per pixel, at world position P with
// world normal N (normalised) and view vector V (normalised). Returns the
// diffuse light; spec gets the highlight colour before the material's
// specular map (the caller multiplies), with exponent pw. Unused slots have
// zero colour, so looping over all n costs time, never correctness.
float3 point_lights(int n, float3 P, float3 N, float3 V, float pw, out float3 spec)
{
    float3 diff = 0;
    spec = 0;
    [loop] for (int k = 0; k < n; k++) {
        float3 L = _PointLightsPos_1[k].xyz - P;
        float d = length(L);
        L /= max(d, 1e-4);
        float ndl = saturate(dot(N, L));
        float3 c = PointLightsColor[k].xyz * pl_atten(_PointLightsFalloff_1[k], d);
        diff += c * ndl;
        float3 H = normalize(L + V);
        spec += c * (pow(saturate(dot(N, H)), pw) * ndl);
    }
    float k = 1.0 + gvUltraPL.z;
    spec *= k * gvUltraPL.w;
    return diff * k;
}
