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
float4 gvUltraMat;
float4 gvUltraShadow;
float4 gvUltraLook;

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
float pcss(sampler2D smp, float4 sp, float2 vpos)
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
    float sbias = gvUltraShadow.w;
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
    float r = clamp((z - zsum / nb) * scale, max(1.0, gvUltraMat.y), maxr);

    // 3. filter over that radius, each tap a bilinear compare
    float zref = z - gvUltraShadow.w * r;
    float lit = 0;
    [loop] for (int j = 0; j < 16; j++)
        lit += cmp_bilinear(smp, uv + vogel16(j, rot) * (r * texel), zref);
    return lit / 16;
}
