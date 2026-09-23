# The renderer and its data

Facts about the engine's D3D9 renderer and the environment data it reads,
recovered from the binary and the archives. The shaders themselves are in
[stock-shaders.md](stock-shaders.md).

## How a mesh picks its technique

(From the decompiled renderer, 2026-09-21.)

- Per mesh, `sGetEffectAndTechnique` (`dxC_render.cpp`) calls
  `dxC_AssembleLightsPoint(pModel, tLights)`, which selects and sorts up to
  256 candidate lights (`dx9_MakePointLightList`,
  `e_LightSelectAndSortPointList`) and copies them into the model's light
  slots **until the slot index passes 4** (`if (4 < count) break;` at
  0x788e02). The shader light cap is **5**, matching the `[5]` parameter
  arrays; the count becomes `nPointLights`.
- `dx9_GetEffectAndTechnique` zeroes `nPointLights` unless the mesh draw
  flag 0x2000000 is set, packs the request into a 16-byte feature struct and
  calls `dxC_EffectGetTechniqueByFeatures` (0x7807ff). That lookup first
  tries an **exact** 16-byte match, then scores every technique: for each
  feature, `max(0, requested − technique) × weight × 2`. More of a feature
  than asked costs nothing, less is penalised, and the lowest score wins.
- So a technique that announces more (say `PointLights=5`) is used
  immediately for any mesh that asks, with no CPU-side patch. Going past 5
  lights means widening `tLights` and the `cmp 4` above.
- The player's model has the "wants point lights" draw bit off by design.

## Pipeline and state

**API.** Direct3D 9 through an engine abstraction layer (`DxC` / `Dx9`
source paths in asserts: `dxC_effect.cpp`, `dx9_device.cpp`,
`dxC_occlusion.cpp`, `dxC_hdrange.cpp`, ...). A DX10 path exists in the
code and data (`DX10 is enabled`, `bDX10ScreenFX`, a full `effects\dx10\`
shader set) but the Steam build runs D3D9. The DLL already hooks the D3D9
device's `EndScene` and `Reset` (`src/overlay.c`, vtable probed via a
throwaway device).

**Shaders.** All shading is compiled D3DX effects (`.fxo`), loaded by name
through `dx9_EffectNew(...)`. In `hellgate000.dat`:

- `data\effects\dx9\` (55), `data_common\effects\dx9\` (71), plus
  `...\dx9\1x\` fallbacks (14 + 22), and the DX10 set (27 + 36). 225 total.
- Material families come in **shader-model tiers**: `…1x`, `…20`,
  `…20lod`, `…20_low`, `…30` — e.g. `actorindoor20/30`,
  `actoroutdoor20/30`, `backgroundindoor/outdoor(prop|grid)20/30`,
  `…spotlight…`, `backgroundwater`, `skybox`.
- Non-material effects, by the names the exe loads them with:
  `HDR.fxo`, `Obscurance.fxo`, `ShadowMap.fxo` / `ShadowMap11.fxo`,
  `ShadowBlob.fxo`, `Gaussian.fxo`, `Blurr.fxo`, `CombineLayers.fxo`,
  `ScreenEffects.fxo`, `Silhouette.fxo`, `OcclusionMap.fxo`, `_ZBuffer.fxo`,
  `Overlay.fxo`, `UI.fxo`, `WindowGamma.fxo`, `MoviePlayer.fxo`, `Debug.fxo`.
- Particle shaders: `particle`, `particlefadein`, `particledistortion`,
  `particlehellrift`, `particlemesh`, `particlemeshfallout`.

**Lighting model** (from the effect-parameter asserts in the exe):

- Forward rendering. Per object, nearby point lights are gathered and
  sorted (`dx9_MakePointLightList(..., 256)`,
  `e_LightSelectAndSortPointList`). Only a few become real shader lights
  (`EFFECT_PARAM_POINTLIGHTPOS/COLOR/FALLOFF_*`, count capped by
  `nEffectLights`); **2** get specular (`EFFECT_PARAM_SPECULARLIGHT*`,
  array of 2).
- The rest are folded into **spherical harmonics** —
  `dx9_ComputeSHPointLights`, `dx9_SHCoefsAddPointLight` — a soft ambient
  approximation. This is why spell and muzzle lights look flat.
- Spotlights exist (`dx9_EffectSetSpotlightParams`, flashlight map,
  `…spotlight` shader variants). Static geometry uses lightmaps
  (`TEXTURE_LIGHTMAP`). Normal and specular maps are supported as static
  shader branches (`EFFECT_PARAM_BRANCH_NORMALMAP/SPECULAR`).
- **Shadows are directional only**, three tiers
  (`SHADOWMAP_LOW/MID/HIGH`, `dxC_ShadowBufferSetupDirectional`). Dynamic
  point lights cast no shadows.
- "Obscurance" is ambient occlusion computed **per model at load**
  (`dxC_ObscuranceComputeForModelDefinition`), not screen-space.
- There is an HDR pass with luminance downsampling (`HDR.fxo`,
  `HDR_LUMINANCE_PASS_*`) and bloom-style blur passes.

**Fog.** Linear distance fog per environment: `EFFECT_PARAM_FOGDISTANCEMIN
/ MAX`, `FOGCOLOR`, and a per-effect enable (`FOG_FACTOR`).
`D3DRS_FOGENABLE` is also touched. Particles have their own fog term
(`FOGADDITIVEPARTICLELUM`).

**Particles.** CPU-simulated systems drawn from one shared dynamic vertex
buffer (`sgtParticleSystemVertexBuffers[PARTICLE_VERTEX_BUFFER_BASIC]`),
plus mesh particles with texture scrolling
(`EFFECT_PARAM_PARTICLE_MESH_TEXSLIDE_X/Y`). Definitions are data.

**There is no scene depth texture.** `_ZBuffer.fxo` writes an ID/coverage
value, not depth (see [stock-shaders.md](stock-shaders.md)). INTZ depth
textures are supported under DXVK, but the scene renders with 4x MSAA,
which an INTZ surface cannot be; SSAO, soft particles and depth fog need
either MSAA off plus a post-process AA, or our own depth pass.

**Shadows (DXVK, 2026-09-22).** The DLL selects the colour shadow map
(type 2, R32F, depth in .r) instead of the depth map (type 1, D24X8 with
hardware compare); see [../graphics.md](../graphics.md). The map size is
capped at 2048 for type 2 and 4096 otherwise (`sComputeShadowMapSize`).

Outdoors there are **three** buffers, in an array at `DAT_00bb08e4`
(400 bytes each, count at `DAT_00bb08f4`), created at `0x7e2be5`:

| Buffer | Flags | Covers | Holds |
|---|---|---|---|
| 0, near | 0x38 | 27 units ahead of the camera (`0xa817f4`) | characters, props (model bit 5) |
| 1 | 0x84 | 80 units (40 × 2) | static models with CastShadow |
| 2, default | 0x44 | fitted to the region | static models with CastShadow |

- **Caster lists**: `FUN_007c9d5a` queries the model proximity map around
  each buffer (origin point within 1.2 × radius), filters, and adds
  draw-list command 2 (`dx9_RenderModelShadow`). In the near buffer,
  static models (bit 5 clear) are rejected outdoors by the branch at
  `0x7ca3f0`; indoors they need the material's CastShadow (bit `0x12`).
  The wide buffers reject bit-5 models.
- **Redraw**: a buffer is drawn while its dirty bit (flags & 1) is set;
  after drawing, the bit is cleared unless the buffer is always-dirty
  (flags & 0x10, `0x7ca489`). Only the near map has 0x10.
- **Which map a mesh reads** (`sSetGeneralMeshParameters`): the default
  buffer (`DAT_00bb08ec`), or buffer 1 if the mesh's bounds fit inside
  it, then `dx9_SetShadowMapParameters` (`0x7e4930`, cdecl: effect,
  technique, buffer, world, view, projection) sets `gmShadowMatrix`
  (`sShadowMapSetMatrix` slot 99) and, outdoors, the near map's
  `gmShadowMatrix2` (slot 100). An override at `DAT_00edfcc0` forces one
  buffer for every mesh.
- **Characters** are requested with ShadowType 0 (never receive shadows).
- **GPU queries**: the engine creates types 5 and 6, which DXVK refuses;
  in-process that is harmless (see the render server in the journal).

**Quality presets** are the engine's "feature lines" and option states
(`e_FeatureLine_*`, stops such as `MSAA`, `SHDR`; `e_OptionState_*`).
Shadow resolution, distances and similar ceilings are data values, not
code — a cheap first lever.

## Environment data

`tools/data/hgdat.py` lists and extracts every `.idx/.dat` pair:

```sh
python3 tools/data/hgdat.py list  "particles"          # substring filter
python3 tools/data/hgdat.py extract /tmp/out ".fxo"
```

Keys and layout come from **Reanimator-steam**
(`github.com/ti360gh/Reanimator-steam`, C#, 2021), which decrypts the Steam
index unchanged. Relevant contents of `hellgate000`:

| What | Where | Count |
|---|---|---|
| Particle systems | `data\particles\*.xml.cooked` | ~6,000 |
| Light definitions | `data\lights\*.xml.cooked` (e.g. `muzzle flash light`) | 134 |
| Environments (fog, ambient, lights) | `data\background\_environments\*_env.xml.cooked` | 75 |
| Screen effects | `data\screenfx\*.xml.cooked` (`motionblur`, `dof_test`, `sniper`, `player hurt red`, ...) | 15 |
| Spell / skill definitions | `data\skills\**\*.xml.cooked` (e.g. `cabalist\stormlightning`) | many |
| Shaders | `.fxo` (above) | 225 |

`.xml.cooked` is the engine's binary cooked format (magic `CO0k`), **not
XML**. Reanimator-steam's `hellpack` uncooks and cooks it, and its
`hellgate/Xml/*.cs` has the field layouts for exactly the definitions that
matter here: `ParticleSystemDefinition`, `EnvironmentDefinition`,
`LightDefinition`, `ScreenEffectDefinition`, `Material`. `tools/data/hguncook.py`
is a Python port of its readers (read-only; writing cooked data back is
not done yet).

There are other archives besides `hellgate000` (`hellgate_graphicshigh000`,
`hellgate_bghigh000`, the `x_*` set, ...); `hgdat.py` reads them all.

**2018 vs 2007.** Against the 2007 disc's environments, the Steam data
has about 3× the ambient, SH fill on twice as many environments, and fog
starting at ~2 m instead of ~10 m; shadow intensity and direct light are
unchanged ([journal](../journal.md), 2026-09-22 00:05). That is most of
why the 2018 picture looks flat.

