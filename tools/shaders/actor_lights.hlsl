// Additive per-pixel point-light pass for the actor materials.
//
// This is drawn as a SECOND pass on top of the game's untouched material pass
// (see notes/graphics-plan.md step 1 and tools/mkfx.py). Blend ONE:ONE, no Z
// write, Z test LESSEQUAL, RGB only. It adds the contribution of the model's
// shader point lights (up to five, the engine's cap) computed per pixel with
// the normal map, and nothing else: no ambient, no directional light, no fog
// colour. The base pass keeps supplying all of that, so materials look exactly
// as before plus sharp lights.
//
// Parameter names match the game's effects so the engine fills them by name:
// Bones (3 float4 rows per bone, blend indices pre-multiplied by 3), the
// object-space light arrays, EyeInObject, the fog scalars, the material
// scalars and the same samplers.
//
// Variants (preprocessor defines, one compile each):
//   SKINNED     0/1   blend-indexed skinning like the game's skinned VS
//   NORMALMAP   0/1   perturb the normal with NormalMapSampler (x in .a, y in .g)
//   SPECULAR    0/1   Blinn-Phong highlight from SpecularMapSampler
//   FIRST_LIGHT n     first light slot this pass handles: 0 for materials whose
//                     base pass has no point lights, 2 for the indoor actor
//                     techniques that already light slots 0 and 1 themselves
//   LIGHT_COUNT n     how many slots the engine filled for this technique (its
//                     PointLights annotation); slots beyond it hold stale data

#ifndef SKINNED
#define SKINNED 0
#endif
#ifndef NORMALMAP
#define NORMALMAP 0
#endif
#ifndef SPECULAR
#define SPECULAR 0
#endif
#ifndef FIRST_LIGHT
#define FIRST_LIGHT 0
#endif
#ifndef LIGHT_COUNT
#define LIGHT_COUNT 5
#endif

float4x4 WorldViewProjection;
#if SKINNED
float4   Bones[180];
#endif
float4   _PointLightsPos_0[5];
float4   PointLightsColor[5];
float4   _PointLightsFalloff_0[5];
float4   EyeInObject;
float    FogMaxDistance;
float    FogMinDistance;
float    gfFogFactor;
float    gfNormalPower;
float4   gvSpecularMaterialData;   // .x specular power, .z specular scale (as the base pass uses them)
float4   gvMiscMaterialData;       // .x specular map bias
float4   gvUltraLight;             // ours (added to the effect by mkfx.py): .x diffuse strength,
                                   // .y specular strength; set per pass by the DLL from the panel

// Same sampler stages as the stock actor shaders (s0 / s5 / s6), so this pass
// never rebinds a stage the engine's texture cache thinks it owns.
sampler2D DiffuseMapSampler  : register(s0);
#if NORMALMAP
sampler2D NormalMapSampler   : register(s6);
#endif
#if SPECULAR
sampler2D SpecularMapSampler : register(s5);
#endif

struct VS_IN {
    float4 pos  : POSITION;
#if SKINNED
    float4 bidx : BLENDINDICES;
    float4 bw   : BLENDWEIGHT;
#endif
    float2 uv   : TEXCOORD0;
    float3 nrm  : NORMAL;
#if NORMALMAP
    float3 tan  : TANGENT;         // only the normal-mapped materials declare these,
    float3 bin  : BINORMAL;        // stored 0..1, expanded here like the game does
#endif
};

struct VS_OUT {
    float4 hpos : POSITION;
    float2 uv   : TEXCOORD0;
    float4 opos : TEXCOORD1;       // object-space position, fog factor in .w
    float3 T    : TEXCOORD2;
    float3 B    : TEXCOORD3;
    float3 N    : TEXCOORD4;
};

#if SKINNED
float3 skin_point(float4 p, float4 bidx, float4 bw)
{
    int3 i = (int3)bidx.xyz;
    float3 a, b, c;
    a.x = dot(Bones[i.y + 0], p); a.y = dot(Bones[i.y + 1], p); a.z = dot(Bones[i.y + 2], p);
    b.x = dot(Bones[i.x + 0], p); b.y = dot(Bones[i.x + 1], p); b.z = dot(Bones[i.x + 2], p);
    c.x = dot(Bones[i.z + 0], p); c.y = dot(Bones[i.z + 1], p); c.z = dot(Bones[i.z + 2], p);
    return a * bw.y + b * bw.x + c * bw.z;      // same accumulation order as the game's VS
}
float3 skin_dir(float3 d, float4 bidx, float4 bw)
{
    int3 i = (int3)bidx.xyz;
    float3 a, b, c;
    a.x = dot(Bones[i.y + 0].xyz, d); a.y = dot(Bones[i.y + 1].xyz, d); a.z = dot(Bones[i.y + 2].xyz, d);
    b.x = dot(Bones[i.x + 0].xyz, d); b.y = dot(Bones[i.x + 1].xyz, d); b.z = dot(Bones[i.x + 2].xyz, d);
    c.x = dot(Bones[i.z + 0].xyz, d); c.y = dot(Bones[i.z + 1].xyz, d); c.z = dot(Bones[i.z + 2].xyz, d);
    return a * bw.y + b * bw.x + c * bw.z;
}
#endif

VS_OUT vs_main(VS_IN v)
{
    VS_OUT o;
    float4 p = float4(v.pos.xyz, 1.0);
#if NORMALMAP
    float3 tan = v.tan, bin = v.bin * 2.0 - 1.0;
#else
    float3 tan = float3(1, 0, 0), bin = float3(0, 1, 0);   // unused without a normal map
#endif
#if SKINNED
    float3 pos = skin_point(p, v.bidx, v.bw);
    float3 N = normalize(skin_dir(v.nrm, v.bidx, v.bw));
    float3 T = normalize(skin_dir(tan, v.bidx, v.bw));
    float3 B = normalize(skin_dir(bin, v.bidx, v.bw));
#else
    float3 pos = p.xyz;
    float3 N = normalize(v.nrm);
    float3 T = normalize(tan);
    float3 B = normalize(bin);
#endif
    o.hpos = mul(float4(pos, 1.0), WorldViewProjection);
    o.uv = v.uv;
    // Fog exactly as the base pass: distance from the eye to the unskinned
    // vertex, linear over [min, max], plus the environment's factor.
    float dist = length(EyeInObject.xyz - v.pos.xyz);
    float fog = saturate(saturate((FogMaxDistance - dist) / max(FogMaxDistance - FogMinDistance, 0.001)) + gfFogFactor);
    o.opos = float4(pos, fog);
    o.T = T; o.B = B; o.N = N;
    return o;
}

float4 ps_main(VS_OUT i) : COLOR
{
    float3 albedo = tex2D(DiffuseMapSampler, i.uv).rgb;
    float3 T = normalize(i.T), B = normalize(i.B), N = normalize(i.N);
#if NORMALMAP
    float4 nm = tex2D(NormalMapSampler, i.uv);
    float2 nxy = nm.wy * 2.0 - 1.0;
    float3 nts = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
    float3 n = normalize(T * nts.x + B * nts.y + N * nts.z) * gfNormalPower;
#else
    float3 n = N * gfNormalPower;
#endif
    float3 V = normalize(EyeInObject.xyz - i.opos.xyz);
#if SPECULAR
    float4 sm = tex2D(SpecularMapSampler, i.uv);
    float3 scol = (sm.rgb + gvMiscMaterialData.x) * gvSpecularMaterialData.z;
    float  spow = max(gvSpecularMaterialData.x, 1.0);
#endif
    float3 diffuse = 0, spec = 0;
    [unroll]
    for (int k = FIRST_LIGHT; k < LIGHT_COUNT; k++) {
        float3 L = _PointLightsPos_0[k].xyz - i.opos.xyz;
        float  d = length(L);
        float3 Ln = L / max(d, 1e-4);
        float  att = saturate(d * -_PointLightsFalloff_0[k].y + _PointLightsFalloff_0[k].x);
        float3 col = PointLightsColor[k].rgb * att;
        diffuse += col * saturate(dot(n, Ln));
#if SPECULAR
        float3 H = normalize(Ln + V);
        spec += col * pow(max(dot(H, n), 0.0), spow);
#endif
    }
    float3 c = diffuse * albedo * gvUltraLight.x;
#if SPECULAR
    c += spec * scol * gvUltraLight.y;
#endif
    // The base pass soft-clamps its diffuse to 1.0 per channel; keep this
    // pass from blowing out at point-blank range in the same spirit.
    c = c / (1.0 + max(max(c.r, c.g), c.b) * 0.5);
    return float4(c * saturate(i.opos.w), 0.0);
}
