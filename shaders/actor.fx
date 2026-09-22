// Effect wrapper for actor.hlsl: compiled once per feature set with
// Microsoft's effect compiler (tools/fx/fxcomp.c); tools/fx/mkmat.py lifts the two
// shaders and swaps them into the stock actor effects.
#include "actor.hlsl"
technique T { pass P0 { VertexShader = compile vs_3_0 vs_main(); PixelShader = compile ps_3_0 ps_main(); } }
