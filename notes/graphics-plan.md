# Graphics enhancement plan — handoff

## Step 0 status (2026-09-21, later the same day)

Done offline, from the archives:

- **0.1** `tools/hguncook.py` reads every `.xml.cooked` (1,168/1,168 sample
  files parse to EOF). What is tunable is in `notes/data-tunables.md`.
  Headline: spell lights are all 3–5 m radius; only 2 of 20 cabalist particle
  systems attach a light; soft-particle and distortion knobs exist in data but
  are unset everywhere (DX10-only).
- **0.2** `tools/hgfx.py` dumps `.fxo` (params, techniques, passes, CTAB
  constant layouts) and `build/fxdis.exe` disassembles the blobs under Wine.
  Findings in `notes/shaders.md`. Headline: **point lights are vertex-lit**;
  `actoroutdoor30` has *zero* shader point lights (SH only), indoor actors 2,
  backgrounds up to 5; fog is per-vertex linear; `_ZBuffer.fxo` is not a depth
  pass, so depth must come from INTZ or our own pass.

Built into the DLL, awaiting one game run (`src/gfxprobe.c`, deployed
18:24:53):

- **0.3** every `D3DXCreateEffect` call is logged with the pak path of the
  blob (`gfxprobe: effect #N data\effects\dx9\actoroutdoor30.fxo` …) — read
  the tier off the names. `ID3DXEffect::SetTechnique` counts per technique
  name (`gfxprobe:   <count> <effect> TActor_22_1_…`) show the `PointLights`
  values actually in use.
- **0.4** route A is implemented: `<game>\override\<pak path>` replaces the
  blob at creation. A byte-identical `override\data_common\effects\dx9\
  particle.fxo` is staged, so the first run must look unchanged and log
  `gfxprobe: OVERRIDE particle.fxo`. `CreateFileW` is logged for `data\`
  paths (`gfxprobe: file open|MISS <path>`) to see whether loose files are
  ever consulted (route B).
- **0.5** once per device: back buffer + depth stencil descriptions, caps,
  and `CheckDeviceFormat` for INTZ / RAWZ / DF24 / DF16 / NULL / R32F /
  A16B16G16R16F. A frame capture (automatic at frame 900, or on demand by
  creating `bin\hellgate_gfxprobe.frame`) lists every render-target /
  depth-target change with draw counts in between: that locates the shadow,
  HDR and UI passes for steps 2 and 3.

**First instrumented run (2026-09-21 18:39, wined3d, 2560x1600, 4x MSAA):**

- **0.3 answered: the SM3 tier runs.** 42 distinct effects created (each
  twice, 84 total, 0 unknown): `actoroutdoor30`, `actorindoor30`,
  `backgroundindoor30`, `backgroundindoorprop30`, `backgroundoutdoor30`,
  `backgroundoutdoorprop30`, plus the non-material set. Techniques in use
  confirm the shader reading: outdoors the player used only
  `TActor_00_…` (0 point lights, 3,844 sets), indoors `TActor_22_…` (2),
  backgrounds `TBackground_64_{0,3,5}_…`.
- **0.4 answered: route A works, route B is dead.** The byte-identical
  `override\…\particle.fxo` was substituted at both creations
  (`gfxprobe: OVERRIDE particle.fxo`) and the game ran normally. The engine
  opened exactly two loose files under `data\` all session
  (`serverlist.xml`, a miss on `serverlistoverride.xml`): it never looks
  for data files on disk, so replacement effects go through the
  `D3DXCreateEffect` hook and nothing else.
- **0.5 answered: INTZ is supported under wined3d** (RAWZ/DF24/DF16 are not;
  NULL RT, R32F and A16B16G16R16F render targets and D24S8 depth textures
  are). Caps: vs/ps 3.0, 4 simultaneous RTs. **Complication:** the back
  buffer and depth stencil are **4x MSAA** (`A8R8G8B8` / `D24S8`,
  2560x1600). An INTZ texture cannot be multisampled and D3D9 cannot
  resolve depth, so reading depth means one of: run the scene with MSAA
  off (and add a post AA instead), or render a depth-only pass of our own
  into a non-MSAA INTZ/R32F target. Decide when step 2/3 starts.
- The automatic frame capture fired on a loading screen (46 draws); the
  next build captures the first busy frame instead. The on-demand flag file
  works as designed.

Everything else below is the original brief.


Written 2026-09-21 at the end of a long session (camera, animation, 1 FPS
work). This is the brief for the next session, which picks up the graphics
work. Everything under **Known** was read out of the binary or the archives
this session; everything under **Assumed** / **Open** still needs checking
before code depends on it.

## What the user wants

In their words: **"more updated dynamic lighting, fog, better particles,
spell effects."** In-engine improvements, not a post-process filter. ReShade
and DXVK tweaks were offered and set aside; they can still be a quick
baseline, but they are not the goal.

Priorities, in the user's order: dynamic lighting → fog → particles → spell
effects.

## How to work in this repo (read first)

- **Deploy after every build.** `make` builds *and* installs
  `build/version.dll` into the game's `bin/`. The user tests in the real
  game; a DLL left in `build/` is untested. After building, **check the
  timestamp of the installed DLL** — this session once reported a feature as
  deployed from a stale reading.
- Build inside the toolbox: `toolbox run -c dev bash -lc 'cd "$PWD" && make'`.
  32-bit mingw, `-O2 -fno-omit-frame-pointer`, **no `-msse`** (inline asm
  that calls game code cannot list xmm clobbers because of this; see
  `src/shoulder.c` `game_ray`).
- `make` also builds and `build/uitest` runs the native test suite
  (`test/ui.c`, 193 checks). Keep game-independent math in the
  `#ifndef _WIN32`-free top half of each module so it can be tested there.
- The user's launch options (decided 2026-09-21, DXVK from now on):
  `WINEDLLOVERRIDES="version=n,b" gg %command%`.
  **DXVK, not wined3d.** wined3d cannot draw the engine's depth shadow map
  (type 1); the DLL forces the colour map there, but DXVK renders the stock
  path and supports depth textures with hardware PCF, which the shadow
  overhaul will use. Add `PROTON_USE_WINED3D=1` only for the 1 FPS
  raycast repro, which never reproduced on DXVK.
- The game must be **restarted** to load a new DLL. Say so every time.
- Log: `bin/hellgate_rays.log`; the previous session's is `.log.1`, and any
  session that recorded a stall is kept as `hellgate_rays.spike-*.log`.
- Dev panel: Shift+\` in game, Ctrl+1..8 for tabs. New features get a
  toggle and a counter there so the user can A/B them.
- Every address in `src/target.h` was recovered statically. **Verify the
  bytes before hooking or patching** (every existing install does), and
  never patch a shared constant — `0xa0086c` (5.0) has 102 readers; the zoom
  ceiling was changed by repointing one instruction's operand instead.
- Uncommitted work: most of `src/` is uncommitted, including all of this
  session's camera, animation and Alt-latch modules. Ask before committing.

## Known: the renderer

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

**A depth pass exists** (`_ZBuffer.fxo`, `NONMATERIAL_EFFECT_ZBUFFER`).
Soft particles and depth fog both need a readable depth; this is the first
thing to confirm (see Open).

**Quality presets** are the engine's "feature lines" and option states
(`e_FeatureLine_*`, stops such as `MSAA`, `SHDR`; `e_OptionState_*`).
Shadow resolution, distances and similar ceilings are data values, not
code — a cheap first lever.

## Known: the data

`tools/hgdat.py` (this session) lists and extracts every `.idx/.dat` pair:

```sh
python3 tools/hgdat.py list  "particles"          # substring filter
python3 tools/hgdat.py extract /tmp/out ".fxo"
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
`LightDefinition`, `ScreenEffectDefinition`, `Material`. Reuse it rather
than reversing the format again. It is Windows C# — run it under Wine/Mono,
or port the relevant readers to Python if that is painful.

There are other archives besides `hellgate000` (`hellgate_graphicshigh000`,
`hellgate_bghigh000`, the `x_*` set, ...); `hgdat.py` reads them all.

## The plan

Ordered so each step is useful on its own and unblocks the next.

### 0. Groundwork (do first)

1. **Uncook** a spell's particle and light definitions, one environment and
   one screen effect with Reanimator-steam. Record what is tunable in data
   (light radius/intensity, particle counts and lifetimes, fog near/far/
   colour). A lot of the "spell effects" wish may be reachable in data alone.
2. **Disassemble the D3D9 `.fxo`** effects (fx_2_0 binaries). `fxc /dumpbin`
   under Wine, or D3DX's `D3DXDisassembleEffect` from the game's own
   `d3dx9_42.dll` in a small Windows helper. Target first:
   `particle.fxo`, `actoroutdoor30.fxo`, `backgroundoutdoor30.fxo`,
   `hdr.fxo`. Record the constant layout — light arrays, fog params,
   matrices — per shader.
3. **Establish which shader tier runs** (1x/20/20lod/30) on this machine
   under wined3d. Log `dx9_EffectNew` arguments, or hook
   `D3DXCreateEffect*` in `d3dx9_42.dll` and log the names as they load.
4. **Find a way to load replacement effects.** Two routes; pick one:
   - intercept effect creation (`D3DXCreateEffect` / `...FromMemory` in
     `d3dx9_42.dll`, or the engine's `dx9_EffectNew`) and substitute our
     compiled blob by name; or
   - loose-file override: the launch build's strings mention
     `FILE_UPDATE_UPDATE_IF_NOT_IN_PAK` / "Not found in pak", i.e. the
     engine may load files from disk ahead of the archive. **Unverified in
     the current build** — test with a harmless file before relying on it.
5. **Confirm depth is readable.** Find where `_ZBuffer.fxo` renders, which
   render target it writes (and its format), and whether that texture can
   be bound in a later pass. If not, render our own depth in the EndScene
   hook path or use INTZ (wined3d support for INTZ must be checked).

### 1. Dynamic lighting (highest priority)

**Status 2026-09-21 evening: first build shipped to `override\`, awaiting the
user's eyes.** `actoroutdoor30.fxo` and `actorindoor30.fxo` now carry a
`_pp5` sibling of every zero-light (outdoor) / two-light (indoor) technique
with `PointLights=5`: the stock pass untouched, plus an additive "Lights"
pass (`tools/shaders/actor_lights.hlsl`) that computes the five shader
point lights per pixel with the normal and specular maps, ONE:ONE blend, no
Z write, Z LESSEQUAL, RGB only. Both files validate in the game's own D3DX
(480 techniques each, 0 invalid). `bin\hellgate_override.off` switches the
overrides off for an A/B; `make shaders` rebuilds and reinstalls them.
Backgrounds are next: their point lights are per vertex in the stock pass,
so the additive pass would double them — those need a replacement pass, not
an extra one.

Hard-won toolchain facts (see `notes/shaders.md`): D3DX reads the fx_2_0
blob sequentially, so the writer must reproduce the compiler's layout; the
third header count is shader states + passes + sampler parameters; and
injected shaders must come from Microsoft's *effect* compiler (`fxcomp`,
d3dx9_34), because standalone-compiled shaders lack constant default blocks
and any constant spanning many registers (Bones[180]) makes the loader fail
with a bare E_FAIL.

**What the decompiled renderer says (2026-09-21, `notes/decomp/`):**

- Per mesh, `sGetEffectAndTechnique` (`dxC_render.cpp`) calls
  `dxC_AssembleLightsPoint(pModel, tLights)`, which selects and sorts up to
  256 candidates (`dx9_MakePointLightList`, `e_LightSelectAndSortPointList`)
  and then copies them into the model's light slots **until the slot index
  passes 4** (`if (4 < count) break;` at 0x788e02): the shader light cap is
  **5**, matching the `[5]` parameter arrays. The count becomes
  `nPointLights` for `dx9_GetEffectAndTechnique`.
- `dx9_GetEffectAndTechnique` zeroes `nPointLights` unless the mesh draw flag
  0x2000000 is set, packs the request into a 16-byte feature struct and
  calls `dxC_EffectGetTechnique` → `dxC_EffectGetTechniqueByFeatures`
  (0x7807ff). That lookup first tries an **exact** 16-byte match, then scores
  every technique of the effect: for each feature, `max(0, requested −
  technique) × weight × 2`. A technique with *more* of a feature than asked
  costs nothing; one with less is penalised. Lowest score wins.
- Consequence: **adding `PointLights=5` techniques to `actoroutdoor30.fxo`
  (which has only `PointLights=0` today) makes the engine use them at once**
  for any actor with lights nearby, with no CPU-side patch. The same goes
  for per-pixel variants: the technique is chosen by annotation, the shader
  body is ours. Going past 5 lights means widening `tLights`'s fixed arrays
  and the `cmp 4` in `dxC_AssembleLightsPoint`, so the first milestone is
  **5 per-pixel lights on everything**, not more lights.

Goal: spells, muzzle flashes, fires and explosions visibly light their
surroundings, per pixel, instead of washing into SH ambient.

- Replace the actor and background material shaders (the tier found in 0.3)
  with versions that light **per pixel** and accept **more point lights**
  (8–16), with a proper falloff.
- Raise the CPU-side cap so fewer lights are folded into SH: find where
  `nEffectLights` is chosen for the technique
  (`dx9_GetEffectAndTechnique(..., nPointLights, ...)`) and lift it, with
  matching techniques in the new shaders. The sort already keeps up to 256
  candidates, so the gathering side should not need work.
- Stretch: shadows from the one or two brightest dynamic lights (a cube or
  dual-paraboloid shadow map per light). That is weeks of work on its own;
  defer until per-pixel lighting lands.
- Watch cost: more lights is more GPU work, and the GPU has large headroom
  here — the game's limit is CPU-side physics (see below).

### 2. Fog

- A depth-based fog pass before the UI draws: **height fog** (density
  falling off with altitude) and **in-scattering** around bright lights,
  driven by each environment's existing fog colour and distances so art
  direction is kept. Needs depth (0.5).
- Where to insert: after the scene and before UI — find the UI pass
  (`UI.fxo`) and draw immediately before it, or hook the HDR resolve.

### 3. Particles

- **Soft particles**: modify `particle.fxo` (and `particlefadein`,
  `particledistortion`, `particlehellrift`) to fade alpha where the
  particle's depth approaches scene depth. Removes the hard lines where
  sprites cut into walls and floors. Needs depth (0.5). This is the most
  visible single change for spell-heavy combat.
- **Lit particles**: sample the same point-light set as the materials so
  smoke and debris pick up spell and fire light.
- **Heat distortion** for fire spells: `particledistortion.fxo` exists —
  find which systems use it and widen its use.
- Density / lifetime: data edits in the particle definitions (step 0.1).

### 4. Spell effects

Mostly content, on top of 1–3:

- Stronger and more numerous lights attached to spells (light definitions
  and the particle systems that spawn them), so step 1 has something to
  show.
- Particle density, lifetime and size passes on the big spells.
- Screen effects (`data\screenfx`) for impacts — used sparingly.

Needs cooking and **writing data back**. Reanimator-steam can in principle
repack; test the round trip (extract → uncook → cook → repack → game loads
it unchanged) before editing anything real. Or use the loose-file route if
0.4 confirms it.

### 5. The rest of the wishlist (added 2026-09-21 evening, in the user's words)

"add AO, parallax, the whole nine", "volumetric fog, god rays outside".
Where each one stands:

- **Screen-space AO, volumetric fog, god rays** all need scene depth in a
  texture. Step 0.5 found INTZ works under wined3d but the scene is 4x MSAA,
  which INTZ cannot be. So all three wait on one decision: run the game with
  MSAA off and add a post-process AA (FXAA/SMAA in our EndScene pass), or
  render a depth-only pass of our own. God rays additionally want the sun's
  screen position and an occlusion mask (sky vs geometry), both derivable
  from depth + the environment's directional light.
- **Parallax mapping** needs height maps. The assets ship normal maps only
  (`NormalMapSampler`, x in .a, y in .g). A height field can be integrated
  from a normal map offline (Poisson/Frankot-Chellappa) and stored in the
  normal map's unused blue channel via `override\` textures, then a
  parallax-offset pass replaces the normal-map lookup. Doable, moderate
  effort, mostly tooling; visually meaningful on walls and floors, not on
  characters.
- **Lights**: build 1 lit indoor monsters only; the player's model has the
  "wants point lights" bit off by design (see LOG). Fixing that is a DLL
  change, not a shader change.

### 6. Shadows and "fake path tracing" (added 2026-09-21 night, user's words)

User: "we need better dynamic shadows in general ... exteriors truly have
one exterior sun light, and lights interiors also cast shadows"; "prevent
... character and enemy shadows are darker than standard shadows"; "more
drastic lighting in general, fake path tracing". Renderer is DXVK now.

**Why actor shadows look darker than the world's.** The world's shadows
are baked (lightmaps, background SH) and carry bounce light; the dynamic
shadow map zeroes the directional term with no fill, scaled only by the
environment's `fShadowIntensity` (default 1 = black; `ongoing_a01f` uses
0.9). Two fixes, in order:
1. Data: lower `fShadowIntensity` per environment (0.55-0.7 range) via
   `override\` cooked XML, so dynamic shadows sit at the lightmap's level.
2. Shader: shadow term = `lerp(fill, 1, s)` with `fill` from the ambient /
   SH-to-direct ratio of the environment, plus PCF penumbra (PCSS later).
   Our additive light pass must multiply by the same shadow term; today it
   adds light into shadowed pixels, which is part of "everything got
   brighter".

**"Fake path tracing" on SM3 = a G-buffer plus screen-space GI.** The
Z-pass capture/replay (step 5 foundation) gives depth and, with our own
replay shaders, normals. On that:
- SSDO (screen-space directional occlusion) with one-bounce colour bleed:
  the cheapest thing that reads as GI. HBAO as the fallback.
- Sun: 2-3 cascades from the replayed stream, PCSS-style soft edges,
  depth textures with hardware PCF (DXVK supports the NVIDIA path). Stock
  today (measured 2026-09-22): ONE 2048x2048 D24X8 depth map behind a
  colour-write-disabled A8R8G8B8 dummy target, re-rendered every other
  frame; no MID/LOW maps in play. The stock map can serve as the near
  cascade at first.
- Interior lights: contact shadows in the additive pass first, then true
  shadow maps for 1-2 dominant lights (dual-paraboloid).
- Data relight, the WoW-Forever move: retune `ENVIRONMENT_DEFINITION`
  dir-light colours/intensities, hemisphere, ambient, fog start and shadow
  intensity per zone. Cheapest, largest visual change, no shader risk; do
  it alongside the code work. The 2007-vs-2018 diff (LOG 2026-09-22 00:05)
  says where to start: 2018 added fill (ambient x3, hemisphere on many
  envs, SH on twice as many) and pulled fog start from 10 m to 2 m; shadow
  and direct light are unchanged. Gamma 1.15 in settings.xml and the
  never-run `hdr.fxo` finish the flat look.
- Bloom / tone map: `hdr.fxo` exists (Reinhard + gamma); check in the
  frame capture whether `TFinalPass` runs on DX9, and drive it if not.
- God rays and volumetric fog per step 5, both from the same depth.

Order: data relight + shadow intensity -> Z-pass replay -> SSDO + bloom ->
sun cascades + shadow fill -> contact shadows -> point-light shadow maps.

### Also available, lower priority

- Raise feature-line ceilings: shadow map resolution, draw/LOD distances,
  particle caps. Data values; cheap.
- Screen-space AO (the existing obscurance is per-model at load).
- Quick baseline without code: ReShade on D3D9, or DXVK's forced 16x AF.

## Open questions

- Which shader tier runs under wined3d (0.3) — **answered: SM3 (`…30`).**
- Whether effects can be replaced by name at load (0.4) — **answered: yes,
  via the `D3DXCreateEffect` hook**; loose files are never consulted.
- Whether the depth pass output is bindable (0.5) — **answered: there is no
  depth pass; INTZ is available but the scene is 4x MSAA** (see status).
- Whether Reanimator-steam's cook/repack round-trips cleanly on this build —
  open; `hguncook.py` is read-only so far.
- How many lights `nEffectLights` allows per technique today — **answered
  from the shaders**: 0 (outdoor actors), 2 (indoor actors), 3/5
  (backgrounds); where the CPU side decides it is still open.

## Context the next session should not trip over

- **The 1 FPS bug** was captured once this session under wined3d: per-ray
  cost rose 30–40x for ~110s, driven by a phantom cast from the per-unit
  update (`0x49b954 → 0x49a286 → 0x4ec793 → … → hkPhantom::castRay`) — the
  same loop the Russian "2026 fix" patches. The raw log was lost to log
  truncation (fixed since). Graphics work does not touch this, but a
  frame-rate collapse during testing is more likely that bug than a new
  shader; check the log for `SPIKE` windows before blaming graphics.
- `RVA_PHYS_OBJ_STEP` (`0x7f92f0`) is **hkAnimatedSkeleton::stepDeltaTime**,
  not physics (corrected in `target.h`).
- Animation is Havok Animation 4.0; Granny 2 is only the model file format
  (meshes and material bindings). Relevant if higher-quality meshes or
  textures ever come up.
- Modules added this session: `src/shoulder.c` (action camera, melee
  impulse), `src/animwatch.c` (animation event logging), `src/animfix.c`
  (stance ease, seam inertialization, phase match), `src/altlatch.c`
  (double-tap Alt), `tools/hgdat.py`. Their fixes are awaiting the user's
  in-game verification at the time of writing.

### 7. Material shader rewrite (option 1 groundwork, sized 2026-09-22)

The six SM3 material effects hold 1,482 techniques: actoroutdoor30 240,
actorindoor30 288, backgroundindoor30 252, backgroundoutdoor30 216,
backgroundoutdoorprop30 243, backgroundindoorprop30 243. Every technique is
one point in a feature grid (annotations: Skinned, NormalMap, Specular,
SpecularLUT, SelfIllum, SphericalHarmonics, WorldSpaceLight, CubeEnvMap,
SphereEnvMap, Scatter, ScrollUV, Indoor, SpotLight, PointLights, plus
VertexFormat / LightMap / DiffuseMap2 on backgrounds) times ShadowType
0/1/2, a third each. So the rewrite is one HLSL source per family (actor,
background) with `#if` on those names, and a generator that walks each
stock effect, derives the defines from the annotations, compiles the
technique set with fxcomp and swaps the shader blobs in place -- names,
annotations and pass states untouched, so the engine's technique selection
and cache never notice. The frame's technique-set log shows ~30 techniques
in actual use per zone; validate those first, but generate all.

**Status 2026-09-22: done to parity.** `tools/shaders/actor.hlsl` and
`background.hlsl` rebuild all 1,482 techniques pixel-identically. This is
checked by `make matcheck` (tools/fxdiff.c, four seeds), and the harness's
coverage by `tools/matmutate.py`. `tools/build_shaders.sh` installs the
rebuilt effects (the actor ones with the light-pass clones on top). Every
stock quirk reproduced is listed in LOG.md under this date; read that list
before changing a lighting term. How to work from here: keep parity
reachable. Each new look sits behind a runtime effect parameter that only
the DLL sets, defaulting to stock, so `make matcheck` stays at 0 differ with
the parameter at 0.

### 8. Next on the owned shaders (in order)

Status 2026-09-22: items 1 and 2 are implemented behind panel toggles
(default off) and await in-game tuning; see LOG.md. Iteration loop:
edit `tools/shaders/*.hlsl` → `make shaders` (~25 s) → reload a level or
restart. Runtime knobs (fill %, sun size) need no rebuild at all.

1. **Shadow fill.** The dynamic shadow should remove only the sun's share.
   Actors: shadow only directional light 0 (and the highlight), not the SH /
   ambient fill. Backgrounds: the light map already contains the sun, so
   darken toward the environment's shadowed level, not multiply the whole
   light map. Estimate the unshadowed/shadowed ratio from the vertex sun
   visibility (normal.w) and the SH fill. Panel toggle + strength.
2. **PCSS** (user ask, 2026-09-22). Force the colour shadow map (type 2,
   depth in R32F; the flag file already exists) so depth is readable. Then:
   16-tap blocker search on a Poisson disk rotated per pixel
   (interleaved-gradient noise); penumbra = (receiver - blocker) x sun size
   (orthographic sun); 16-32 PCF taps over that radius; slope-scaled receiver
   bias. One `pcf()` in each source file. Also consider forcing the shadow
   map to render every frame (today every other frame).
   Risk: the single 2048 map's texel footprint; the fix is a tighter
   frustum, then cascades.
3. Per-pixel point lights in the base pass (replacing the additive clone
   pass), then the G-buffer outputs for SSDO / bloom / tone map.
