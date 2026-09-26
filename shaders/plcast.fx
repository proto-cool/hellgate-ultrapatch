// The level's geometry into the point-light shadow cube (src/plshadow.c):
// walls, pillars and floors recorded from the level's own draws, drawn from
// the lamp with only their positions. Writes what the engine's shadowmap.fxo
// casters write there, z/w of the face's projection, so the receivers
// (shaders/ultra.hlsl pl_shadow) read both alike.
float4x4 gWorld;
float4x4 gViewProj;

struct VO { float4 hpos : POSITION; float2 zw : TEXCOORD0; };

VO vs_main(float4 pos : POSITION)
{
    VO o;
    o.hpos = mul(mul(float4(pos.xyz, 1.0), gWorld), gViewProj);
    o.zw = o.hpos.zw;
    return o;
}

float4 ps_main(float2 zw : TEXCOORD0) : COLOR
{
    return zw.x / zw.y;
}

technique Rigid { pass p0 { VertexShader = compile vs_3_0 vs_main(); PixelShader = compile ps_3_0 ps_main(); } }
