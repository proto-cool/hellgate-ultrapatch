// Effect wrapper for actor_lights.hlsl: compiled with Microsoft's effect
// compiler (tools/fx/fxcomp.c) so the shader blobs carry everything the game's
// D3DX expects; tools/fx/mkfx.py lifts pass P0's shaders out of the result.
#include "actor_lights.hlsl"
technique T { pass P0 { VertexShader = compile vs_3_0 vs_main(); PixelShader = compile ps_3_0 ps_main(); } }
