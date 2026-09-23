// Soft particles for particle.fxo (docs/graphics.md, "Soft particles").
//
// The stock effect's sprites cut into walls and floors in hard lines; the
// game's data knows soft particles (the SoftParticles annotation,
// fSoftParticleScale) but only its DX10 path implements them. These are the
// stock shaders, instruction for instruction in effect, plus a fade by the
// distance from each sprite to the scene behind it:
//   VS  stock transform and vertex fog (vs_1_1: the fixed-function fog still
//       applies, as it would not behind a ps_3_0), and the sprite's screen
//       position and view depth in TEXCOORD1
//   PS  stock colour; alpha (and, for the premultiplied additive-glow
//       variant, colour) times saturate((scene z - sprite z) / distance)
// The scene's linear view depth is a half-resolution R32F texture src/postfx.c
// writes after the opaque scene; the DLL binds it on sampler 1 for particle
// passes and sets gvUltraSoft.x = 1 / fade distance. At 0 the fade is 1:
// stock. tools/fx/mkparticle.py swaps these blobs into the stock effect.
//
// Lit particles (gvUltraPart, zero = stock): smoke, dust and ash (the plain
// variant; fire and sparks are light sources and stay as they are) pick up
// the point lights near them, and outdoors darken in the sun's shadow (the
// near shadow map). The DLL fills the effect's own point-light arrays and
// near-map matrix and texture for our passes; the engine does not use them
// for particles. The vertex shaders are vs_2_0 for the light loop; vs_2_0
// still writes the fixed-function fog.

float4x4 WorldViewProjection;
float4   EyeInObject;
float    FogMaxDistance;
float    FogMinDistance;
bool     FogAdditiveParticleLum;
bool     gbDarken;
float4   gvUltraSoft;       // .x 1 / fade distance (0 = stock)
float4   gvUltraPart;       // .x light strength, .y sun-shadow darkening, .z near-map depth bias (0 = stock)
float4   _PointLightsPos_1[5];      // xyz position (world)
float4   PointLightsColor[5];       // rgb (0: unused)
float4   _PointLightsFalloff_1[5];  // x reach
float4x4 gmShadowMatrix2;           // world -> near shadow map

sampler2D DiffuseMapSampler  : register(s0);
sampler2D SoftDepthSampler   : register(s1);    // tUltraSoftDepth, set by the DLL
sampler2D ExtraColorShadowMapSampler : register(s2);   // the near sun shadow map, set by the DLL

struct VS_IN  { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct VS_IN5 { float4 pos : POSITION; float4 col : COLOR0; float4 spc : COLOR1; float2 uv : TEXCOORD0; };
struct VS_OUT {
    float4 hpos : POSITION;
    float4 col  : COLOR0;
    float4 spc  : COLOR1;
    float2 uv   : TEXCOORD0;
    float4 scr  : TEXCOORD1;    // projective screen uv in xy/w, view depth in w
    float4 shp  : TEXCOORD2;    // near-map uv, depth; w the shadow's weight (0: none)
    float3 lit  : TEXCOORD3;    // light multiplier (1: stock)
    float  fog  : FOG;
    float  psz  : PSIZE;        // stock writes 0 (plain and additive)
};

float fog_of(float3 p)
{
    return saturate((FogMaxDistance - length(EyeInObject.xyz - p)) / (FogMaxDistance - FogMinDistance));
}

float4 screen(float4 h)
{
    return float4(h.x * 0.5 + h.w * 0.5, h.w * 0.5 - h.y * 0.5, 0, h.w);
}

// the point lights at p: the surfaces' smooth falloff, windowed to zero at
// each light's reach
float3 lights_at(float3 p)
{
    float3 L = 0;
    for (int k = 0; k < 5; k++) {
        float3 d = p - _PointLightsPos_1[k].xyz;
        float R = max(_PointLightsFalloff_1[k].x, 1e-3);
        float q2 = dot(d, d) / (R * R);
        float w = saturate(1.0 - q2 * q2);
        L += PointLightsColor[k].rgb * (w * w / (1.0 + 8.0 * q2));
    }
    return L;
}

// plain: vertex fog
VS_OUT vs_plain(VS_IN v)
{
    VS_OUT o;
    o.hpos = mul(float4(v.pos.xyz, 1.0), WorldViewProjection);
    o.fog = fog_of(v.pos.xyz);
    o.psz = 0;
    o.col = v.col;
    o.spc = 0;
    o.uv = v.uv;
    o.scr = screen(o.hpos);
    // smoke near a fire takes its colour; 1 exactly at zero strength
    o.lit = 1.0 + gvUltraPart.x * lights_at(v.pos.xyz);
    float4 sp = mul(float4(v.pos.xyz, 1.0), gmShadowMatrix2);
    o.shp = float4(sp.xy, sp.z - gvUltraPart.z, gvUltraPart.y);
    return o;
}

// additive: fog darkens the colour instead (when FogAdditiveParticleLum)
VS_OUT vs_additive(VS_IN v)
{
    VS_OUT o;
    o.hpos = mul(float4(v.pos.xyz, 1.0), WorldViewProjection);
    float f = fog_of(v.pos.xyz);
    o.col = FogAdditiveParticleLum * (v.col * f - v.col) + v.col;
    o.spc = 0;
    o.fog = 0;
    o.psz = 0;
    o.uv = v.uv;
    o.scr = screen(o.hpos);
    o.lit = 1;                  // additive: a light source itself
    o.shp = 0;
    return o;
}

// additive glow, glow constant: the same for colour and the glow in COLOR1.w
struct VS_OUT5 {
    float4 hpos : POSITION;
    float4 col  : COLOR0;
    float4 spc  : COLOR1;
    float2 uv   : TEXCOORD0;
    float4 scr  : TEXCOORD1;
    float  fog  : FOG;
};
VS_OUT5 vs_addglow(VS_IN5 v)
{
    VS_OUT5 o;
    o.hpos = mul(float4(v.pos.xyz, 1.0), WorldViewProjection);
    float f = fog_of(v.pos.xyz);
    o.col = FogAdditiveParticleLum * (v.col * f - v.col) + v.col;
    o.spc = FogAdditiveParticleLum * (v.spc.w * f - v.spc.w) + v.spc.w;
    o.fog = 0;
    o.uv = v.uv;
    o.scr = screen(o.hpos);
    return o;
}

// 1 when off (exactly: stock parity), else the fade by the scene behind
float soft(float4 scr)
{
    float scene = tex2Dproj(SoftDepthSampler, scr).x;
    float f = saturate((scene - scr.w) * gvUltraSoft.x);
    return gvUltraSoft.x > 0 ? f : 1.0;
}

float4 ps_plain(float4 col : COLOR0, float2 uv : TEXCOORD0, float4 scr : TEXCOORD1, float4 shp : TEXCOORD2,
                float3 lit : TEXCOORD3) : COLOR
{
    float4 c = tex2D(DiffuseMapSampler, uv) * col;
    c.xyz = gbDarken ? 0.5 : c.xyz;
    c.w *= soft(scr);
    // outdoors: darker in the sun's shadow (outside the near map: lit)
    float inmap = all(shp.xy >= 0 && shp.xy <= 1) ? 1.0 : 0.0;
    float vis = shp.z <= tex2D(ExtraColorShadowMapSampler, shp.xy).x ? 1.0 : 1.0 - inmap;
    c.xyz *= lit * lerp(1.0, vis, shp.w);
    return c;
}

float4 ps_addglow(float4 col : COLOR0, float4 spc : COLOR1, float2 uv : TEXCOORD0, float4 scr : TEXCOORD1) : COLOR
{
    float4 c = tex2D(DiffuseMapSampler, uv) * col;
    float k = soft(scr);
    return float4(c.xyz * c.w * k, c.w * spc.w * k);
}

technique TPlain    { pass P0 { VertexShader = compile vs_2_0 vs_plain();    PixelShader = compile ps_2_0 ps_plain(); } }
technique TAdditive { pass P0 { VertexShader = compile vs_2_0 vs_additive(); PixelShader = compile ps_2_0 ps_plain(); } }
technique TAddGlow  { pass P0 { VertexShader = compile vs_1_1 vs_addglow();  PixelShader = compile ps_2_0 ps_addglow(); } }
