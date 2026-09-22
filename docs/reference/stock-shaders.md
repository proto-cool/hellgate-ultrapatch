# The D3D9 shaders, read out of the archives (plan step 0.2)

Written 2026-09-21. Everything here was read from the 120 `data*\effects\dx9\`
`.fxo` files with the two tools below; nothing is inferred from the exe.

```sh
python3 tools/data/hgdat.py extract /tmp/hg ".fxo"                      # all 225 effects out of the paks
python3 tools/fx/hgfx.py dump /tmp/hg/data_common/effects/dx9/particle.fxo --blobs /tmp/blobs
python3 tools/fx/hgfx.py table /tmp/hg > src/fxtable.h                 # (size, fnv1a) -> pak path, for the DLL
toolbox run -c dev bash -lc 'WINEPREFIX=/tmp/wpfx wine build/fxdis.exe d3dx9_43.dll /tmp/blobs/*.bin'
```

`hgfx.py dump` prints every parameter (type, semantic, default, annotations),
every technique with its annotations, every pass with its render states, and
for each shader the constant table (register, count, type, name) straight out
of the bytecode's CTAB. `--blobs` writes each distinct shader as
`<effect>.t<N>.p<M>.vs|ps.bin`; `fxdis.exe` turns those into assembly. Use
Wine's own `d3dx9_43.dll`: the game's `D3DX9_42.dll` loads under Wine but its
`D3DXDisassembleShader` returns S_OK with no buffer (it forwards to a
`d3dcompiler_42` that is not there).

Format: fx_2_0 binary, tag `0xFEFF0901`, laid out as Wine's
`dlls/d3dx9_36/effect.c` parses it. All 120 D3D9 effects parse to the last
byte (0 tail bytes), 1,760 distinct shader blobs.

## Tiers

| Suffix | Shaders | Techniques (actoroutdoor / backgroundoutdoor) |
|---|---|---|
| `…1x` (in `dx9\1x\`) | vs_1_1 / ps_1_x | fallback set |
| `…20`, `…20lod`, `…20_low` | vs_2_0 / ps_2_x (`ps_2_1` in the version token) | 108 / 108 |
| `…30` | vs_3_0 / ps_3_0 | 240 / 216 |

Which tier the engine picks on this machine is what `src/gfxprobe.c` logs
(`gfxprobe: effect #N <pak path>`), plan step 0.3. The non-material effects
(`HDR`, `_ZBuffer`, `particle`, `UI`, ...) live in `data_common\effects\dx9\`
and have no tier; they are vs_1_1 / ps_2_0 with `ps_1_1` for `_ZBuffer`.

## Technique naming = the feature bits

Every material technique carries the same annotation set, and its name encodes
them, e.g. `TActor_22_1_1011000200103300000`:

```
TActor_<PointLights><SpecularLights>_<ShadowType>_<Skinned><SpotLight><Indoor>...
 <VSVersion={4}> <PSVersion={8}> <PointLights={2}> <ShadowType={1}> <Skinned={1}>
 <SpotLight={0}> <Indoor={1}> <NormalMap={1}> <SelfIllum={0}> <Specular={1}>
 <SpecularLUT={0}> <SphericalHarmonics={1}> <WorldSpaceLight={0}> <CubeEnvMap={0}>
 <SphereEnvMap={0}> <Scatter={0}> <ScrollUV={0}>
TBackground_64_5_1_000110220001100000   adds <VertexFormat=> <LightMap=> <DiffuseMap2=>
```

The engine's `dx9_GetEffectAndTechnique(..., nPointLights, ...)` matches these
annotations, so **the technique set is the ceiling on shader point lights**:

| Effect | `PointLights` values present |
|---|---|
| `actoroutdoor30` | **0 only** (240 techniques, all `SphericalHarmonics=1`) |
| `actoroutdoor20` | 0, 2 |
| `actorindoor30` | 0, 2 |
| `backgroundindoor30`, `backgroundoutdoor30`, `backgroundoutdoorprop30` | 0, 3, 5 |
| `backgroundoutdoor20` | 0, 3, 5 |

Outdoor actors (the player, in most of the game) get **no shader point
lights at all** in the SM3 path; every spell light reaches them through the
SH coefficients. Indoor actors get two. Backgrounds get up to five. The
effect-level parameter arrays are sized `_PointLightsPos_0/1[5]`,
`PointLightsColor[5]`, `_PointLightsFalloff_0/1[5]`, `_SpecularLights*[2]`,
`_DirLightsDir_0/1[3]`, `DirLightsColor[3]`, `_SpotLights*[1]` in every
material effect (the `_0` / `_1` pairs are object- and world-space copies).

## Where the lighting happens

**Point lights are vertex lit.** In every material technique the point-light
constants are bound in the *vertex* shader only. `backgroundoutdoor30`,
technique `TBackground_64_5_1_000110220001100000`:

```
vs_3_0  85 instr                          ps_3_0  80 instr
 c0  [5] _PointLightsPos_1                 c0  [3] DirLightsColor
 c5  [5] PointLightsColor                  c6      ShadowLightDir
 c10 [5] _PointLightsFalloff_1             c7      gvSpecularMaterialData
 c15 [4] gmShadowMatrix                    c8      gvMiscLightingData
 c19 [4] gmShadowMatrix2                   c9      gvMiscMaterialData
 c23 [4] WorldViewProjection               c10     gfSpecularPower
 c27 [3] _DirLightsDir_0                   c11     FogColor
 c30 [3] World                             s0 Diffuse s1 LightMap s5 Specular
 c33 [2] _DirLightsDir_1                   s6 Normal s10 ShadowMap s11 ShadowMapDepth
 c35 [2] DirLightsColor
 c37     LightAmbient      c38 gvMiscLightingData
 c39     EyeInObject       c40 FogMaxDistance   c41 gfFogFactor
```

The VS sums the five point lights into `o0.rgb` (a colour interpolator) and
the PS multiplies it in; the PS does per-pixel work only for the directional
lights (with the normal map), specular, lightmap and shadow. The same holds for
actors, which have in addition a **single per-pixel point light**: the camera
light (`_CameraLightPos_World`, `_CameraLightColor`,
`_CameraLightFalloff_World` in the PS). `actoroutdoor30`, technique
`TActor_00_1_1001032203100000000` (skinned, shadowed, normal + specular map):

```
vs_3_0  136 instr                          ps_3_0  88 instr
 c0   [180] Bones                           c5  [3] DirLightsColor
 c180 [4] gmShadowMatrix                    c8      _CameraLightPos_World
 c184 [4] WorldViewProjection               c9      _CameraLightColor
 c188 [3] _DirLightsDir_0                   c10     _CameraLightFalloff_World
 c191 [3] World                             c11     gvSpecularMaterialData
 c194 [2] _DirLightsDir_1                   c12     gvMiscLightingData
 c196 [2] DirLightsColor                    c13     gvMiscMaterialData
 c198     ShadowLightDir                    c14     gfSpecularPower
 c199     LightAmbient                      c15     gfNormalPower
 c200     EyeInObject                       c16     FogColor
 c201     FogMaxDistance                    s0 Diffuse s5 Specular s6 Normal s10 ShadowMap
 c202     gfFogFactor
 c203..c209 cAr cAg cAb cBr cBg cBb cC      (SH: 9 coefficients per channel packed as 7 float4)
```

So "per-pixel dynamic lighting" (plan step 1) is not a constant-count tweak:
the point-light loop has to move from the VS to the PS in new material
effects, with new techniques announcing higher `PointLights` values, and the
CPU side has to be persuaded to pick them.

## Fog

Linear, **per vertex**: the VS writes `saturate(dist * k + gfFogFactor)` into
`o0.w` from `FogMaxDistance` / `gfFogFactor` (`FogMinDistance` is declared but
the VS uses only the max plus the precomputed factor); the PS lerps to
`FogColor`. No fixed-function fog. Particles do the same in their VS
(`FogMaxDistance`, `FogAdditiveParticleLum`, `EyeInObject` → `oD0`), which is
why the plan's depth-based height fog has to be a separate full-screen pass:
nothing in the material shaders can be re-driven for it.

## Particles

`particle.fxo`: 7 techniques, vs_1_1 / ps_2_0, all
`<SoftParticles={0}>`. The annotation exists (so the engine knows how to ask
for soft particles) but no D3D9 technique implements it; the data-side knobs
`fSoftParticleScale` / `fSoftParticleContrast` (see `data-tunables.md`) are
DX10-only in practice. Additive / Glow / GlowConstant variants; the pixel
shader is 9 instructions (`texld * vertex colour`, optional darken). The
constants a soft-particle version needs (depth texture, projection scale) are
simply not there yet.

## HDR

`hdr.fxo`: `TLuminance` (greyscale downsample, downsample, debug) and
`TFinalPass`, ps_2_0. The final pass is a Reinhard-style tone map: scene
colour scaled by `key / avg-luminance` from a 1x1 texture in `s1`, then
`pow(x, 0.4545)` (a gamma 2.2 encode). Everything is 8-bit unless the frame
capture shows a float back buffer.

## `_ZBuffer.fxo` is not a depth pass

Seven techniques (`SimpleShader`, `RigidShader`, `RigidShader16`,
`AnimatedShader_NoSkin/Skin`, `ParticleShader`, `ParticleMeshShader`) share
one `ps_1_1` that outputs `diffuse.a * (c0.z * (1 - c0.w) + 0.004 * c0.w)` —
a per-object constant modulated by texture alpha, with the VS emitting only
`oPos` and a texcoord. It writes an ID / coverage value, not depth (the
"fake colour target" shadow setup and occlusion map are the likely users).
**There is no engine pass that leaves scene depth in a texture.** Step 0.5
therefore comes down to what the probe reports for INTZ under wined3d: if
INTZ is supported we create the depth-stencil as an INTZ texture ourselves
(hook `CreateDepthStencilSurface` / the auto depth stencil at `Reset`) and
bind it in a post pass; otherwise we render our own depth.

## Loose ends

- The `1x` and `20_low` tiers are untouched; if the probe shows the engine
  picking a `20` effect, the SM3 work above moves to the `20` files.
- `spotlight` variants (`…spotlight20/30`) carry the same layout plus
  `_SpotLights*`; the flashlight goes through them.
- `dx10\` effects are DXBC and not parsed by `hgfx.py`; the DX10 path does not
  run in this build.

## Rebuilding them

How our replacement effects are built, tested and installed, and the rules
the game's D3DX imposes on them: [../graphics.md](../graphics.md).
