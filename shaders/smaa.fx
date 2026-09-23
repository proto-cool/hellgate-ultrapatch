// SMAA 1x for the game's back buffer (src/postfx.c), in place of the 4x MSAA
// the device is no longer created with (src/device.c).
//
// The three passes of Jimenez et al.'s SMAA (ref/smaa, MIT), after their D3D9
// demo effect (ref/smaa/Demo/DX9/Shaders/SMAA.fx) with two changes:
//  - no stencil: the depth-stencil buffer is the scene's (INTZ), so every
//    pass runs with it unbound and the blend-weight pass on every pixel;
//  - the render-target metrics are a parameter, not a compile-time macro,
//    so one compiled effect serves every resolution.
// tools/fx/build_shaders.sh compiles it next to a copy of SMAA.hlsl.

float4 gvSmaaMetrics;   // 1/width, 1/height, width, height
#define SMAA_RT_METRICS gvSmaaMetrics
#define SMAA_HLSL_3
#define SMAA_PRESET_HIGH
// d3dx9_34's compiler (the game's, 2007) predates the mad() intrinsic
#define mad(a, b, c) ((a) * (b) + (c))
#include "SMAA.hlsl"

texture2D colorTex2D;
texture2D edgesTex2D;
texture2D blendTex2D;
texture2D areaTex2D;
texture2D searchTex2D;

sampler2D colorTex {            // linear light, for the final blend
    Texture = <colorTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = Point; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = true;
};
sampler2D colorGammaTex {       // gamma, for luma edges
    Texture = <colorTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D edgesTex {
    Texture = <edgesTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D blendTex {
    Texture = <blendTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D areaTex {
    Texture = <areaTex2D>;
    AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
    MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D searchTex {
    Texture = <searchTex2D>;
    AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
    MipFilter = Point; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};

void EdgeVS(inout float4 position : POSITION, inout float2 texcoord : TEXCOORD0,
            out float4 offset[3] : TEXCOORD1)
{
    SMAAEdgeDetectionVS(texcoord, offset);
}

void WeightVS(inout float4 position : POSITION, inout float2 texcoord : TEXCOORD0,
              out float2 pixcoord : TEXCOORD1, out float4 offset[3] : TEXCOORD2)
{
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
}

void BlendVS(inout float4 position : POSITION, inout float2 texcoord : TEXCOORD0,
             out float4 offset : TEXCOORD1)
{
    SMAANeighborhoodBlendingVS(texcoord, offset);
}

float4 EdgePS(float2 texcoord : TEXCOORD0, float4 offset[3] : TEXCOORD1) : COLOR
{
    return float4(SMAALumaEdgeDetectionPS(texcoord, offset, colorGammaTex), 0.0, 0.0);
}

float4 WeightPS(float2 texcoord : TEXCOORD0, float2 pixcoord : TEXCOORD1,
                float4 offset[3] : TEXCOORD2) : COLOR
{
    return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edgesTex, areaTex, searchTex, 0.0);
}

float4 BlendPS(float2 texcoord : TEXCOORD0, float4 offset : TEXCOORD1) : COLOR
{
    return SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex);
}

#define FULLSCREEN ZEnable = false; ZWriteEnable = false; StencilEnable = false; \
    AlphaBlendEnable = false; AlphaTestEnable = false; CullMode = None; \
    ColorWriteEnable = 0xf; FogEnable = false

technique LumaEdgeDetection {
    pass p0 {
        VertexShader = compile vs_3_0 EdgeVS();
        PixelShader = compile ps_3_0 EdgePS();
        SRGBWriteEnable = false;
        FULLSCREEN;
    }
}

technique BlendWeightCalculation {
    pass p0 {
        VertexShader = compile vs_3_0 WeightVS();
        PixelShader = compile ps_3_0 WeightPS();
        SRGBWriteEnable = false;
        FULLSCREEN;
    }
}

technique NeighborhoodBlending {
    pass p0 {
        VertexShader = compile vs_3_0 BlendVS();
        PixelShader = compile ps_3_0 BlendPS();
        SRGBWriteEnable = true;
        FULLSCREEN;
    }
}
