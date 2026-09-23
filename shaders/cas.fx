// Contrast-adaptive sharpening after SMAA (src/postfx.c), the sharpen-only
// form of AMD FidelityFX CAS (Lottes 2019, MIT). Each pixel is pushed away
// from its four neighbours by an amount that shrinks where the 3x3
// neighbourhood already has high contrast, so edges do not ring and flat
// areas do not gain noise; it brings back what SMAA, bilinear filtering and
// the game's 512-1024 texel textures soften.

float4 gvCasMetrics;    // 1/width, 1/height, sharpness 0..1, 0
texture2D colorTex2D;

sampler2D colorTex {
    Texture = <colorTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
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

float3 at(float2 uv, float x, float y)
{
    return tex2Dlod(colorTex, float4(uv + float2(x, y) * gvCasMetrics.xy, 0, 0)).xyz;
}

float4 CasPS(float2 uv : TEXCOORD0) : COLOR
{
    //  a b c
    //  d e f
    //  g h i
    float3 a = at(uv, -1, -1), b = at(uv, 0, -1), c = at(uv, 1, -1);
    float3 d = at(uv, -1,  0), e = at(uv, 0,  0), f = at(uv, 1,  0);
    float3 g = at(uv, -1,  1), h = at(uv, 0,  1), i = at(uv, 1,  1);

    // soft min and max: the cross, plus the whole 3x3
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, i)));

    // amount: small where the neighbourhood is near black or near white
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));
    float peak = -1.0 / lerp(8.0, 5.0, gvCasMetrics.z);
    float3 w = amp * peak;
    float3 o = ((b + d + f + h) * w + e) / (1.0 + 4.0 * w);
    return float4(saturate(o), 1);
}

technique Sharpen {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 CasPS();
        ZEnable = false; ZWriteEnable = false; StencilEnable = false;
        AlphaBlendEnable = false; AlphaTestEnable = false; CullMode = None;
        FogEnable = false; SRGBWriteEnable = false;
        ColorWriteEnable = 0x7;         // the back buffer's alpha is the game's glow
    }
}
