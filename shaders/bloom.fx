// Bloom and colour grade on the finished 3D frame (src/postfx.c, after the
// volumetric fog, before SMAA).
//
//   Prefilter  full resolution -> half: the bright part of the frame (a soft
//              knee over the threshold), 13-tap downsample with Karis
//              averaging (each 2x2 group weighted by 1 / (1 + luma)), so one
//              hot pixel cannot make a blinking blob
//   Down       13-tap downsample (Jimenez 2014), half -> 1/64 in 6 levels
//   Up         3x3 tent, added into the next larger level
//   Composite  scene + bloom, then the grade: saturation, contrast (a
//              power curve through 0.15, the game's middle), shadows lifted and tinted towards
//              the level's fog colour, a vignette. Colour only: the back
//              buffer's alpha is the engine's glow.
//
// On the stock path the frame is 8-bit, so "bright" is relative: the
// threshold is on luma in the display's 0..1. With HDR (src/hdr.c) the
// scene is read from the float target, brightness above 1 included, and the
// composite tone-maps it (gvHdr) before the grade.

float4 gvBloomSrc;      // the source's texel size xy
float4 gvBloomParams;   // x threshold, y knee, z intensity, w on (> 0)
float4 gvGrade;         // x saturation, y contrast, z shadow tint, w vignette
float4 gvGradeTint;     // rgb shadow tint colour (the fog's hue); w grade on (> 0)
float4 gvHdr;           // the float scene (src/hdr.c): x tone map on (> 0), y exposure, z knee

texture2D srcTex2D;
texture2D sceneTex2D;
texture2D bloomTex2D;

sampler2D srcTex {
    Texture = <srcTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D sceneTex {
    Texture = <sceneTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};
sampler2D bloomTex {
    Texture = <bloomTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};

struct VO { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VO QuadVS(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VO o;
    o.pos = pos;
    o.uv = uv;
    return o;
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 tap(float2 uv, float x, float y)
{
    return tex2Dlod(srcTex, float4(uv + float2(x, y) * gvBloomSrc.xy, 0, 0)).rgb;
}

// the soft-knee bright part
float3 bright(float3 c)
{
    float l = luma(c);
    float t = gvBloomParams.x, k = gvBloomParams.y;
    float s = clamp(l - t + k, 0.0, 2.0 * k);
    s = s * s / (4.0 * k + 1e-5);
    return c * (max(s, l - t) / max(l, 1e-4));
}

float kw(float3 c) { return 1.0 / (1.0 + luma(c)); }

// 13 taps: a centre 2x2 box and four corner boxes, overlapping
float3 down13(float2 uv, bool karis)
{
    float3 a = tap(uv, -2, -2), b = tap(uv, 0, -2), c = tap(uv, 2, -2);
    float3 d = tap(uv, -1, -1), e = tap(uv, 1, -1);
    float3 f = tap(uv, -2, 0), g = tap(uv, 0, 0), h = tap(uv, 2, 0);
    float3 i = tap(uv, -1, 1), j = tap(uv, 1, 1);
    float3 k = tap(uv, -2, 2), l = tap(uv, 0, 2), m = tap(uv, 2, 2);
    if (karis) {
        a = bright(a); b = bright(b); c = bright(c); d = bright(d); e = bright(e);
        f = bright(f); g = bright(g); h = bright(h); i = bright(i); j = bright(j);
        k = bright(k); l = bright(l); m = bright(m);
        float3 g0 = (d + e + i + j) * 0.25, g1 = (a + b + f + g) * 0.25, g2 = (b + c + g + h) * 0.25;
        float3 g3 = (f + g + k + l) * 0.25, g4 = (g + h + l + m) * 0.25;
        float w0 = kw(g0) * 0.5, w1 = kw(g1) * 0.125, w2 = kw(g2) * 0.125, w3 = kw(g3) * 0.125, w4 = kw(g4) * 0.125;
        return (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    }
    return (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625 + g * 0.125;
}

float4 PrefilterPS(float2 uv : TEXCOORD0) : COLOR { return float4(down13(uv, true), 1); }
float4 DownPS(float2 uv : TEXCOORD0) : COLOR { return float4(down13(uv, false), 1); }

float4 UpPS(float2 uv : TEXCOORD0) : COLOR
{
    float3 s = tap(uv, 0, 0) * 4.0;
    s += (tap(uv, -1, 0) + tap(uv, 1, 0) + tap(uv, 0, -1) + tap(uv, 0, 1)) * 2.0;
    s += tap(uv, -1, -1) + tap(uv, 1, -1) + tap(uv, -1, 1) + tap(uv, 1, 1);
    return float4(s / 16.0, 1);
}

float4 CompositePS(float2 uv : TEXCOORD0) : COLOR
{
    float3 c = tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb;
    [branch] if (gvBloomParams.w > 0)
        c += tex2Dlod(bloomTex, float4(uv, 0, 0)).rgb * gvBloomParams.z;
    // HDR: exposure, then a shoulder on the brightest channel, the colour
    // scaled with it so its hue stays (as the stock soft clamp keeps it; a
    // per-channel curve washed lit skin to grey, 2026-09-23): the identity
    // up to the knee, so the frame below it stays as stock drew it, then an
    // exponential roll-off to white with the slope kept at the knee.
    [branch] if (gvHdr.x > 0) {
        float k = gvHdr.z;
        float3 x = max(c * gvHdr.y, 0.0);
        float p = max(x.r, max(x.g, x.b));
        float s = k + (1.0 - k) * (1.0 - exp(-(p - k) / (1.0 - k)));
        c = p > k ? x * (s / p) : x;
    }
    [branch] if (gvGradeTint.w > 0) {
        float l = luma(c);
        c = lerp(l.xxx, c, gvGrade.x);                              // saturation
        // contrast around this game's own middle, not mid grey: its frames
        // sit around 0.15 (medians 0.11-0.15, highlights about 0.35), where
        // an S-curve about 0.5 only darkened (2026-09-23). A power curve
        // through the pivot brightens what is above it and deepens what is
        // below, so highlights gain and the average stays put; 0 is stock.
        {
            const float pivot = 0.15;
            float lc = luma(c);
            float k = pivot * pow(max(lc, 1e-4) / pivot, 1.0 + gvGrade.y) / max(lc, 1e-4);
            c *= lerp(1.0, k, step(1e-4, lc));
        }
        float sh = (1.0 - saturate(l)) * (1.0 - saturate(l));        // shadow weight
        c = lerp(c, c * gvGradeTint.rgb * 2.0 + gvGradeTint.rgb * 0.06, sh * gvGrade.z);
        float2 v = uv - 0.5;
        c *= 1.0 - gvGrade.w * saturate(dot(v, v) * 2.0);             // vignette
    }
    return float4(saturate(c), 1);
}

#define FULLSCREEN ZEnable = false; ZWriteEnable = false; StencilEnable = false; \
    AlphaTestEnable = false; CullMode = None; FogEnable = false; SRGBWriteEnable = false

technique Prefilter { pass p0 { VertexShader = compile vs_3_0 QuadVS(); PixelShader = compile ps_3_0 PrefilterPS();
    AlphaBlendEnable = false; ColorWriteEnable = 0xf; FULLSCREEN; } }
technique Down { pass p0 { VertexShader = compile vs_3_0 QuadVS(); PixelShader = compile ps_3_0 DownPS();
    AlphaBlendEnable = false; ColorWriteEnable = 0xf; FULLSCREEN; } }
technique Up { pass p0 { VertexShader = compile vs_3_0 QuadVS(); PixelShader = compile ps_3_0 UpPS();
    AlphaBlendEnable = true; SrcBlend = One; DestBlend = One; BlendOp = Add; ColorWriteEnable = 0xf; FULLSCREEN; } }
technique Composite { pass p0 { VertexShader = compile vs_3_0 QuadVS(); PixelShader = compile ps_3_0 CompositePS();
    AlphaBlendEnable = false; ColorWriteEnable = 0x7; FULLSCREEN; } }
