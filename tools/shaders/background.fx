// Effect wrapper for background.hlsl: compiled once per feature set with
// Microsoft's effect compiler (tools/fxcomp.c); tools/mkmat.py lifts the two
// shaders and swaps them into the stock background effects.
#include "background.hlsl"
technique T { pass P0 { VertexShader = compile vs_3_0 vs_main(); PixelShader = compile ps_3_0 ps_main(); } }
