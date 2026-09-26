# Graphics: our material shaders

The game draws every surface with six material effects, 1,482 shader
variants in all (`actoroutdoor30`, `actorindoor30`, and four `background…30`
effects; see [reference/stock-shaders.md](reference/stock-shaders.md)). We
rebuild all of them from our own HLSL, pixel-identical to stock, and add
runtime settings on top that default to the stock look. Every graphics
feature in this project is an edit to those sources.

## How it fits together

```
shaders/actor.hlsl, background.hlsl   our source; one compile per feature set
shaders/ultra.hlsl                    runtime settings, PCSS, shared helpers
        │  tools/fx/mkmat.py plan     technique annotations -> #defines, cached per variant
        │  tools/fx/matcompile.sh     Microsoft's effect compiler (fxcomp.exe), in parallel
        ▼
build/mat/<effect>/fx/*.fxo            one compiled variant each
        │  tools/fx/mkmat.py build    swap the blobs into the stock effect, in place
        ▼
<game>/override/data/effects/dx9/*.fxo loaded by the DLL instead of the pak copy
```

The swap keeps technique names, annotations, pass states and the parameter
block exactly as in the stock effect, so the engine's technique lookup and
caches cannot tell the difference. The DLL (`src/gfxprobe.c`) identifies
each effect the engine creates by size and hash, hands D3DX the override
file instead, and writes the runtime settings into it.

`make shaders` runs the whole chain, validates every effect with the game's
own D3DX (`fxload.exe`) and installs only if all of them load. A change to
`ultra.hlsl` recompiles all 1,914 variants, about 11 minutes; nothing
changed takes 13 s.

`make shaders-dev` compiles only the variants the game has drawn. The DLL
appends each material technique it sees to `<game>/bin/ultra_drawn.txt`,
and that list grows across sessions. Variants that were never drawn keep
their last compile, or stock if they were never compiled; a `_pl5`
technique with no compile carries its combination's stock pass. Run
`make shaders` before shipping.

## Runtime settings

`shaders/ultra.hlsl` declares the `float4` parameters that `mkmat.py`
adds to every rebuilt effect with an all-zero default. Only the DLL sets
them (from the panel), and zero reproduces stock exactly.

| Parameter | Components |
|---|---|
| `gvUltraMat` | .x shadow fill 0..1, .y PCSS minimum radius, .z indoor sun size, .w shadow-map debug view |
| `gvUltraShadow` | .x PCSS on, .y outdoor sun size, .z maximum radius, .w bias |
| `gvUltraLook` | .x fill scale −1, .y fog start, .z sun scale −1, .w fine outdoor map per pixel |
| `gvUltraPL` | .x per-pixel point lights, .y smooth falloff, .z strength −1, .w specular |
| `gvUltraAct` | .x characters read the near map (self-shadowing), .y its normal offset |
| `gvUltraChar` | .x character fill, .y skin, .z Fresnel on the level's reflections, .w highlights turned from light 2 towards the sun |

Two matrices ride along the same way (`ULTRA_MATRICES`): `gmUltraFine` in
the background effects and `gmUltraNear` in the actor effects, both
world-space shadow matrices the DLL fills per mesh. The fine map's
texture is bound by the DLL to sampler 12, which the effects do not
declare (D3DX accepts that), and the engine's own sampler-12 texture is
put back after each pass.

To add one: declare it in `ultra.hlsl` so that zero is stock, add it to
`ULTRA_PARAMS` in `mkmat.py`, set it in `ultra_apply()` in `gfxprobe.c`,
and put a control on the panel.

## Testing: parity with stock

```sh
make matcheck                                              # all six effects, ~90 s
toolbox run -c dev tools/fx/matcheck.sh backgroundoutdoor30 background
```

`tools/fx/fxdiff.c` draws every technique of the stock and the rebuilt
effect with identical inputs and compares the pixels. It uses four seeds
(camera light on or off, dim or bright light) and a grid that carries every
vertex element. A technique passes when no channel differs by more than 2/255;
fewer than 0.2% of pixels off is reported as EDGE (shared triangle edges
changing owner) and passes. Failing pairs are dumped to
`build/mat/<effect>/dump/`.

`tools/fx/matmutate.py` checks the check: it breaks one term at a time
and confirms some seed catches it. When the harness first ran it was blind
to the cube map, UV scrolling, the camera light and the glow alpha, and
still reported "0 differ".

Debugging a mismatch:

```sh
tools/fx/fxcmp.sh <effect> <technique> ps     # stock vs ours: constants, preshader, assembly
python3 tools/fx/hgfx.py pres <blob>          # a shader's preshader (CPU-side expressions)
```

`fxdiff -set name=x,y,z,w` sets a runtime parameter on the rebuilt effect
only, and `-shadowscene` puts disc-shaped blockers in the shadow maps with
everything else flat, which is how PCSS was checked offline.

## Rules the game's D3DX enforces

Found by bisecting with `fxload.exe`; breaking any of them fails
`D3DXCreateEffect` with a bare `E_FAIL`.

1. **Layout.** The blob is read sequentially: per parameter the typedef,
   sampler states, value, annotations, name and semantic; per technique the
   annotations, then per pass its states and name; string and resource
   sections in descending object id. The same offsets in another order fail.
2. **Third header count** = shader states with an object + passes + sampler
   parameters.
3. **Shader blobs must come from the effect compiler.** vkd3d and
   `D3DXCompileShader` omit the constant default blocks, and a many-register
   constant such as `Bones[180]` then fails the load. `fxcomp.exe` compiles a
   wrapper `.fx` with the prefix's `d3dx9_34.dll`.
4. **Never share shader objects between techniques.** It loads and
   validates, but D3DX then uploads no constants for any of the techniques
   sharing one.
5. Use the stock sampler registers (s0 diffuse, s1 light map, s2
   self-illumination, s4 diffuse 2, s5 specular, s6 normal, s7 cube, s10/s11
   shadow): the engine binds some textures by stage.

## Stock behaviour worth knowing

The comments at the top of `actor.hlsl` and `background.hlsl` describe what
the stock shaders do. The full list of quirks we reproduced (the swapped
lerp on background highlights, the specular-light tie, the four different
shadow rules) is in the [journal](journal.md) under "material shader
rewrite". The headline for lighting work: the dynamic shadow multiplies all
the light, the baked light map included, which is why stock character
shadows read darker than the world's.

## Outdoor shadows

The engine keeps three outdoor shadow maps, all 2048² R32F (colour map,
type 2): a **near** one, 27 units ahead of the camera, redrawn
continuously, holding every character and prop; an **80-unit** and a
**zone-wide** one, holding only static models with a casting material,
redrawn only when marked dirty (so, in stock, about never). The details
and addresses are in [reference/renderer.md](reference/renderer.md).

What the DLL and shaders change, each on a panel control:

- **Fine map per pixel.** The engine gives each mesh one wide map, the
  80-unit one only if the whole mesh fits inside it; neighbouring meshes
  disagreed in straight seams. `dx9_SetShadowMapParameters` is hooked and
  run twice per background mesh (the fine map with an identity world,
  then the zone-wide one); `background.hlsl` reads the fine map where the
  pixel is inside it, fading into the wide one over its outer 12%. The
  wide maps are marked dirty every 5 s, and the fine one also whenever the
  camera is 8 units from where it was last drawn ("fine map follows",
  Shadow tab): the engine centres it on the camera snapped to its texels,
  so it follows like a cascade and a redraw moves only its edge.
- **Cascades, in effect**: near 27 units (0.013 units a texel), fine 80
  (0.039), zone 240-330 (0.12-0.16), all 2048². The 3x steps are what a
  cascade setup would pick; no new maps were added (spike, 2026-09-23).
- **Characters receive shadows.** The engine asks for ShadowType 0 for
  every character; while the shadow pass runs the DLL asks for 2, and
  `actor.hlsl` also reads the near map (world-space matrix from the same
  identity-world trick) with a noise-free 3×3 filter and a normal offset.
  The stock vertex shader's zeroed coordinate at back-facing vertices is
  replaced by the real one (it made faceted patches).
- **Static objects cast** (`all` by default): one branch at `0x7ca3f0`
  keeps outdoor static models out of the near map; `props` and `all`
  modes patch it, `off` is stock.
- **Known, left alone:** a lamp post's shadow on the ground starts a short
  way from its base. It is in the engine's wide map (not our fine map; the
  caster alpha test is ruled out, and the shadow matrix has no depth bias).
- **Near map reach**: the 27-unit width is repointed at a DLL float.

Debugging: the panel's shadow-map view colours the ground by map (red
near, green wide, blue fine-map weight; dark is shadow); *Dump maps*
writes both bound maps to `bin/shadow_*.pgm`; *Trace maps* logs which
texture and matrix every draw reads for 1200 frames.

## Per-pixel point lights

Up to five engine point lights per pixel, in the base pass, on the panel's
per-pixel lights toggle; `point_lights()` in `shaders/ultra.hlsl` is shared.

- **Backgrounds**: their stock PL 3/5 techniques already receive the
  lights; `gvUltraPL.x` switches the vertex-shader sum off and the pixel
  shader on. No new techniques.
- **Characters**: stock has no five-light technique, so `mkmat.py` adds a
  single-pass `<name>_pl5` technique per feature combination, compiled with
  `PL_ULTRA`. The DLL asks for exactly five lights whenever a mesh has any
  (the lookup wants an exact match); the engine zero-pads the unused light
  colours up to the technique's count and takes all five out of SH.
- Outdoors the lights skip the sun's shadow. Indoors they take it as
  stock's vertex lights did, unless the shadow fill is on: the indoor map
  is aimed along the environment's light direction, not from any of them.

This replaced an additive second pass (`actor_lights.hlsl`, removed): two
passes meant two depth tests, forced render states, and a shimmer on
characters under PCSS.

## Scene depth, SMAA and ambient occlusion

- **Device** (`src/device.c`). `IDirect3D9::CreateDevice` is hooked before
  the game creates its device. With SMAA on (default; `bin/hellgate_smaa.off`
  turns it off from the next start) the device is created without MSAA and
  without its automatic depth buffer, and an INTZ depth texture is bound in
  its place before `dx9_SaveAutoDepthStencil` asks for it. The engine then
  draws into a depth buffer shaders can read. The same file hooks the
  device's EndScene, Reset and CreateQuery, so the per-frame graphics work
  runs with or without the dev panel.
- **Effects** (`shaders/smaa.fx`, `shaders/ao.fx`), compiled by
  `make shaders` into `<game>/override/ultra/`. SMAA is Jimenez et al.'s
  reference (`ref/smaa`, MIT) at the HIGH preset, luma edges, no stencil;
  d3dx9_34's compiler predates `mad()`, so it is a macro.
- **AO** (`src/postfx.c`) runs after the opaque scene: the main camera draws
  through the viewer renderer, whose passes run in `RPTYPE_*` order
  (`OPAQUE_1P`, `OPAQUE_*BLOB` or `OPAQUE_SCENE`, `OPAQUE_SKYBOX`,
  `ALPHA_SKYBOX`, `PARTICLES_ENV`, `ALPHA_SCENE`, `ALPHA_1P`,
  `PARTICLES_GENERAL`), so it runs at the first skybox or particle pass or
  the first blended material draw. Half resolution, normals from depth, two
  8-tap spirals (the radius, for contact, and 4x it, for building-scale
  shading outdoors, where the radius alone averaged 0.97), two depth-aware
  blur passes, a depth-aware upsample (the four half-resolution texels
  weighted by depth agreement, as the fog's), multiplied into the back
  buffer, faded out from 60 to 150 units. Occlusion belongs to the ambient
  light, but the frame it multiplies has the direct sun in it too: where
  the sun reaches the surface (N·L from the depth normals, x2, times the
  near then the fine sun map, as the fog reads them from `volfog.c`) the
  occlusion is eased off, by 70% by default ("less in sun"). Indoors, or
  without the sun's maps that frame, it is the plain occlusion. The
  engine's distance fog is already in the frame (the materials apply it),
  so the occlusion fades with it: only the share of the surface the fog
  left is darkened, between the level's fog start (moved by the LOOK fog
  start) and end, as volfog.c reads them (it drew dark creases through the
  fog). The log's
  `postfx: frame trace` line (every 10 s) shows where in the frame it ran
  and its mean. Depth is linearised with the camera projection the engine
  hands `dx9_SetShadowMapParameters` (the device transform can be stale).
- **Colour bounce** (one bounce, after SSDO): just before the AO pass the
  lit frame is copied to a half-size target; each occlusion sample also
  reads its colour there, weighted like its occlusion, so the surfaces that
  block the ambient light reflect their colour back. The AO targets carry
  the bounce in rgb and the occlusion in alpha through the blur and the
  upsample, and Apply blends frame x (occlusion + bounce) (DESTCOLOR,
  SRCALPHA): the bounce scales what is already lit, so the sky and black
  stay put. 150% by default ("colour bounce"); "show it alone" shows both,
  "show bounce x4" the bounce alone, four times as bright, on black.
- **SMAA** runs on the finished 3D frame, at the first `ui.fxo` pass on the
  back buffer after the opaque scene (or at Present on a frame without UI),
  so the UI stays sharp. Per-frame work (SMAA's fallback, screenshots)
  happens at Present (`IDirect3DDevice9` and `IDirect3DSwapChain9`, outermost
  only): the engine calls EndScene about four times a frame.
- **Surfaces and textures** (Light tab): `gvUltraSurf` scales the highlight
  exponent (energy-normalised, so lower gloss is rougher, not brighter), the
  highlight and cube-map strengths, and blurs reflections by extra mip
  levels; defaults 50/75/60%, +1.5 mips. Material draws get 16x anisotropic
  filtering and a -0.25 mip bias on every linearly filtered stage below the
  shadow maps (set after the effect's own BeginPass).
- **Normal-map detail on the level** (`gvUltraDetail`, Light tab): the stock
  background shaders read their normal maps only for the highlight. In the
  normal-map-and-spec variants the diffuse light now takes them too, against
  the dominant light the VS already carries in tangent space (`i.sdir`: the
  sun's specular light outdoors, the chosen nearby light indoors): the direct
  sun by the Lambert ratio bumped/flat (70%), light map, ambient and SH by a
  half-Lambert ratio (50%), point lights by the normal's tilt. Flat normals
  give exactly 1, so the average brightness stays.
- **Bicubic light maps** (`gvUltraLM`): a B-spline read from four bilinear
  taps. The DLL writes the bound light map's texel size per material draw
  (sampler 1; only when it changes for that effect), since ps_3_0 cannot ask.
- **CAS sharpening** (`shaders/cas.fx`, Post tab): AMD's contrast-adaptive
  sharpening, sharpen-only form (MIT), right after SMAA; 50% by default.
- **Comparison screenshots**: Ctrl+Alt+Shift+P; Ctrl+Alt+Shift+S holds stock view, see
  [panel.md](panel.md#graphics-light-shadow-and-post-tabs).

## Soft particles

`particle.fxo` (7 techniques, vs_1_1 / ps_2_0) is rebuilt by
`tools/fx/mkparticle.py`: our shaders (`shaders/particle.fx`) are swapped
into the stock effect's shader objects, matched by the stock blob they
replace (the effect repeats identical shaders in separate objects: 5 shaders
in 13 passes); names, annotations, states and parameters stay stock, plus
`gvUltraSoft`. The shaders stay on vs_1_1 / ps_2_0 so fixed-function fog
still applies (a ps_3_0 would have to do its own, and the fog colour is a
render state no shader can read). They are the stock instructions (checked
by disassembly: transform, vertex fog with the same preshader
`rcp(FogMax - FogMin)`, colour, darken, point size 0) plus the sprite's
screen position and view depth, and a fade of alpha (colour too for the
premultiplied additive-glow variant) by `saturate((scene z - sprite z) *
gvUltraSoft.x)`, exactly 1 when the knob is 0.

The scene's linear depth comes from `src/postfx.c` at the AO point (a
half-resolution R32F copy of the INTZ depth, AO on or off); `gfxprobe.c`
binds it on sampler 1 after the BeginPass of each pass that runs our
shaders (pass 0 of the five techniques mkparticle swapped; the others keep
their own stage-1 texture, the particle light map or a glow texture, and
get the fade off: binding it for every particle pass made swings, impacts
and weather ash vanish) and sets the fade
(0.6 units by default, Post tab). fxdiff does not handle this effect (it
crashes in D3DX on the fixed-function technique), so parity was checked by
comparing the disassembly.

## Lit particles

Smoke, dust and ash (the plain particle variant; fire, sparks and glows are
light sources and stay as they are) pick up the point lights near them and,
outdoors, darken in the sun's shadow. `shaders/particle.fx` (vertex shaders
now vs_2_0, which still write the fixed-function fog) reads the effect's
own `_PointLightsPos_1` / `PointLightsColor` / `_PointLightsFalloff_1` (5
lights; the engine does not use them for particles), `gmShadowMatrix2` and
`tShadowMapDepth` on `ExtraColorShadowMapSampler` (s2): all real
parameters. `gfxprobe.c`'s `part_bind` fills them for our passes: the 5
strongest of the fog's eased light list (`plshadow_lights_near`, now
computed once a frame and shared), the near sun map and its world matrix
from `volfog.c`, and `gvUltraPart` (x light strength 60%, y shadow 50%, z
depth bias). A pass stays stock unless the effect's `EyeInObject` equals
`EyeInWorld` (particle vertices in world space; counted in the log). At
zero the multipliers are exactly 1 (checked by reading the shader, not by
fxdiff, which cannot run this effect). Post tab: both strengths.

## Point-light shadows

**Off by default** (2026-09-22: "trying to hamfist too much onto an old
dx9 engine"); the Shadow tab still turns it on.

The engine has directional shadows only. `src/plshadow.c` adds one cube
shadow map (6 x 512^2 R32F) for the strongest engine point light near the
camera, so a fire or torch casts the shadows of the characters and props
around it, the player's included.

- **Casters**: the engine's near shadow map pass (the 27-unit map) draws
  characters and props with `shadowmap.fxo`, whose vertex shaders take View
  and Projection as plain constants (rigid c4-c7 / c8-c11, skinned c184-c187
  / c188-c191, transposed). Each caster draw of that pass is recorded in a
  cache (shaders, decl, streams, indices, the cut-out texture, World or
  bones+World, cull). At Present the whole cube is drawn from the cache
  whenever the light moved or a caster in its reach changed, so a quiet
  scene, a partial engine pass or a missed pass no longer leaves the cube
  stale or half-filled. A prop leaves the cache when a full pass skips it
  inside the near box (or after 10 s unseen); a character when any full pass
  skips it. Dynamic buffers are not cached. The pass is recognised by its
  orthographic width (`2 / c8.x`) against the near reach. The cache holds
  1,536 casters; when it is full, the one longest unseen goes, but never a
  character or one of the 12 nearest the player, and those are drawn into
  the cube first, ahead of the 128 nearest the light (768 filled up in a
  station and the player's shadow went, 2026-09-25).
- **Receivers**: `point_lights()` (`shaders/ultra.hlsl`, level and
  characters) multiplies the light at `gvUltraPLS.xyz` by a 4-tap lookup
  compared in linear depth (`gvUltraPLS2`: projection terms, bias, filter
  size). Every other light is untouched (exact stock with the knob off).
- **The cube's sampler is a real effect parameter** (`tUltraPLShadow` /
  `UltraPLShadowSampler`, cloned by `mkmat.py` from the environment map's
  pair): an unparameterised cube sampler crashed the game's D3DX in
  `BeginPass` for the point-light variants without a shadow map (found with
  fxdiff; `FXDIFF_TRACE=1` names the technique a crash is in).
- **The light**: material draws carry the engine's per-mesh lights
  (`_PointLightsPos_1`, `PointLightsColor`, `_PointLightsFalloff_1`); up to
  64 draws a frame are read into a small table, and at Present the
  brightest light whose reach (falloff x / y) plus 6 units covers the
  camera is chosen. Shadow tab: on/off, bias, softness, and which light.
- **Crossfade**: a change of light fades the shadow out over 8 frames
  (`gvUltraPLS.w` is its strength, `lerp(1, pl_shadow, w)`), then moves the
  cube and fades the new one in; switching at once popped shadows up to
  twice a second among the street lamps.


## Volumetric fog

`shaders/fog.fx`, run by `src/postfx.c` on the finished 3D frame just
before SMAA (after the sky and the particles, so shafts show against the
sky), adds the light the air scatters towards the camera. The engine's own
distance fog is untouched.

- **Inputs** (`src/volfog.c`, each stamped with the frame it was seen in and
  used only in that frame): the camera's view matrix from
  `dx9_SetShadowMapParameters` (inverted: pixel back to world); the sun's
  direction (`ShadowLightDir`, the way the light travels) and colour
  (`DirLightsColor[0]`) from an outdoor background material at draw time;
  the near and fine sun shadow maps in world space (`tShadowMapDepth` /
  `gmShadowMatrix2`, `tShadowMap` / `gmShadowMatrix`), read right after the
  fine-map detour's identity-world run; fog colour and distances.
- **Sun shafts**: each half-resolution pixel marches its view ray (to the
  scene, at most 60 units) in 24 jittered steps, lit or not by the near map,
  else the fine map, else lit. The lit air's share of the view is at most
  1 - exp(-density x distance), times a Henyey-Greenstein phase (g 0.5)
  scaled to 1 looking into the sun over a 25% floor, times the sun's colour
  x the strength (70%). Unscaled (x 2.1 into the sun) at density 0.05 it
  went to white. Needs the fine map per pixel (Shadow tab) for the maps.
- **Near and far**: no sun fog in the first 8 units from the camera
  (smoothstep; it sat on the player like a veil). Distance haze: geometry
  fades towards the engine's fog colour (from level geometry only, the
  materials with a light map, and eased over about a second: taken from
  whichever material came first it switched between effects with other
  fog settings every frame and the haze went in and out), transmittance
  exp(-0.004 x (distance - 8)) on the surface (0.010 grayed the distance), a third of that indoors
  (the sky is left as drawn); applied as scene x T + scattered light
  (blend ONE, SRCALPHA).
- **Stability**: the march's noise offset changes every frame (golden
  ratio), and a temporal pass blends 8% of each frame into a history
  reprojected with last frame's camera (8% per frame) and clamped to the current 3x3
  neighbourhood. The sun's maps are used up to 8 frames after they were
  last seen (a missed frame made the shafts blink); `volfog_reset` drops
  them before a device Reset.
- **Light halos** span half each light's reach (the whole reach made
  every street lamp a glowing ball) and saturate: one light adds at most
  half its colour x the strength (65%), 1 - exp(-2 x integral x density).
- **Light halos**: the nearest 8 engine point lights with a reach of 3
  units or more (sparks and spell flashes came and went as spheres) are the
  target set; each light's weight eases to 1 or 0 over about 10 frames, and
  up to 12 are drawn, so a light leaving the nearest 8 fades rather than
  pops. Also faded in over 20 frames from first seen, out over the last 10
  of 30 unseen, and down over the outer 10 units of the 40-unit margin;
  from the point-light shadow's table (`plshadow_lights_near`, fed from
  every material draw, the handles cached in gfxprobe's effect table),
  integrated in closed form along the ray with the surfaces' smooth
  falloff, less its value at the reach so each halo falls to zero at its
  edge (cut off at 1/9, it was a hard disc, like a sprite); the one light with a cube shadow map multiplies its halo by the lit
  share along the ray (16 steps through its cube), eased in over 10 frames,
  so a fire casts shafts past whoever stands in front of it and handing the
  shadow to another fire changes no brightness.
- **Indoors** (weighted by the same eased outdoors/indoors mix, so
  outdoors is unchanged):
  - *Lamp shafts*: each halo is shadowed in screen space. From the pixel
    towards the lamp on screen, 8 depth samples (jittered per frame, the
    temporal pass smooths them); a sample nearer than the lamp, less half
    its glow radius, blocks. Beams radiate past pillars, machines and
    people with no extra draws; faded out as the lamp nears the screen
    edge. 80% by default.
  - *Ground mist*: a 3D volume around the camera (96 x 96 cells of 0.5
    units, 32 slices from 13 units below the eye to 3 above, kept as an atlas of
    slices in a float target). Each frame the level's own geometry is
    marked in stencil bit 0x80 (every draw clears it, only the level's
    opaque draws set it: `gfxprobe.c lvl_stencil`), read into a mask
    (LevelMask), and one point per 4 x 4 pixels is drawn per slice
    (VolInject): a pixel that is level floor facing up adds
    exp(-(height above it) / H) to the cells above it, max-blended. So
    each floor keeps its own layer and a drop has nothing between its
    floors. Each frame's floor goes into its own target, and last frame's
    volume, moved by whole cells as the camera moves, eases towards it
    (VolCopy: a tenth a frame where floor was seen, 99.5% kept where
    none was), so floor coming round a corner fades in rather than
    popping and floor hidden for a moment keeps its mist. The Scatter
    pass marches 16 steps through it (trilinear), with a gentle noise variation (0.5 to 1.5,
    drifting), lit by the lamps and a dim half-grey fog colour, cover
    eased towards 60%. 0.030 per unit, H 0.6 units by default. Replaced a
    slab under the player's floor, a per-pixel ground guessed from the
    screen and a heightfield of the ground (a sheet of mist hung off every
    ledge), 2026-09-25. `make build/fogtest.exe` runs the whole fog chain
    on synthetic scenes (floor, stairs, a ledge, a well, a player, a prop).
- **Density**: 0.060 per unit on the surface, 0.012 indoors and
  underground (outdoors is when the sun and its maps are seen that frame),
  eased over about half a second at a doorway. The pass runs every scene
  frame, with nothing to scatter too (skipping those made it blink off
  indoors).
- Two depth-aware 9-tap blurs and a depth-aware upsample (the four
  half-resolution texels weighted by depth agreement: plain bilinear drew
  edges against the sky in 2-pixel steps), onto the colour only (the back
  buffer's alpha is the glow). The sky gets 60% of the sun's share, and
  so does anything far beyond the march distance, fading in with distance
  (a hard switch at the sky made far walls whiter than the sky).
- Post tab: on/off, show alone, density outdoors and indoors, distance
  haze, sun strength and reach, sky share, halo strength.

## Bloom and colour grade

`shaders/bloom.fx`, run by `src/postfx.c` after the volumetric fog and
before SMAA, on a copy of the 8-bit frame.

- **Bloom**: the bright part (soft knee over a luma threshold, 50%) goes
  down a chain of six half-size 16-bit targets (1/2 to 1/64) with the
  13-tap downsample of Jimenez 2014, the first with Karis averaging so a
  single hot pixel cannot blink as a blob, and back up with a 3x3 tent
  added into each larger level. Intensity 70%.
- **Grade**, in the same composite: saturation (120%), contrast as a share
  of a power curve through 0.15, this game's middle (30%; an S-curve about
  mid grey only darkened frames whose medians are 0.11-0.15), shadows lifted and tinted towards
  the level's fog hue at half luma (30%; 50% was the user's pick on 2026-09-23 with contrast 10%; at 35% with the old
  contrast it had washed hell levels red), and a
  vignette (30%). Contrast, tint and vignette at 30% are the user's pick of 2026-09-24.
  After the contrast, a **lift** of the shadows and mids (`grade.lift`, 15%, the user's pick):
  a gamma curve on luma, most in the shadows, less in the mids, none at white
  (at 10%: 0.036 to 0.049, 0.15 to 0.178, 0.4 to 0.435), hue kept. The remaster read
  too dark (2026-09-26); ambient occlusion eased from 100% to 80% with it.
- Colour only: the back buffer's alpha is the engine's glow. Atmos tab (the
  fog moved there too; the Post tab no longer fit the window).

## HDR scene

`src/hdr.c`, on by default (needs the SMAA device path); switching it makes
or drops the float target between frames, no restart. The design and the engine's own unfinished HDR mode are in
`docs/spikes/hdr.md`.

- **The float scene.** The 3D scene draws into an A16B16G16R16F target the
  size of the back buffer. The redirect is at the device: while the frame is
  in its scene phase, `SetRenderTarget(0, back buffer)` binds ours,
  `GetRenderTarget` answers the back buffer, and `StretchRect` reads and
  writes ours for it (the engine's glow copy, AO's bounce copy). Our passes
  go through the same hooks, so the fog and AO draw on the float scene
  unchanged.
- **The resolve.** The bloom/grade composite reads the float scene and is
  the first thing on the real back buffer, at the first UI pass (or at
  Present on a frame without UI). A frame our passes never reached (menus,
  loading screens) is copied over as it is at Present. SMAA and CAS run on
  the 8-bit result as before.
- **Materials** (`gvUltraHDR.x`): no soft clamp, so light above 1 stays as
  brightness; colour kept to 0..16 (above 65504 a half float is infinite,
  and a later multiply made black NaN monsters). The alpha is stock, glow
  overflow included, and behaves as on the 8-bit target: clamped to 0..1
  and rounded to 8 bits (plus a quarter step). The engine alpha-tests on it
  (>= a reference, mostly 1, against a glow floored at 0.004), and DXVK
  rounds alpha before the test only on 8-bit targets: on the float one,
  lamp-lit car paint got holes and foliage thinned.
- **Tone map**: exposure (100%), then a shoulder on the brightest channel
  with the colour scaled along (the hue stays, as with the stock clamp; a
  per-channel curve washed lit skin grey): the identity up to the knee (80%
  of white), an exponential roll-off above it.
- **Bloom** with the tone map on starts at real brightness: 100% of white,
  a knee half as wide, instead of 50% of display luma. Only what is brighter
  than white glows.
- **Auto exposure** (`bloom.fx` LumLog, LumDown, Adapt): the float scene's
  log luminance, centre-weighted, averaged to 1x1, eased into the eye's
  value (about 0.5 s into light, 1.5 s into the dark). The composite moves
  exposure by a share (50%) of the log gap to a target middle (0.080, a
  first guess to calibrate from the logged "eye at" values), at most one
  stop either way. Dark places brighten a little, bright ones dim a little.
- Post tab: the HDR toggle, *tone map* (off: the stock clamp, an instant
  A/B), exposure, shoulder, bloom threshold, auto exposure (strength,
  middle with the scene's current value beside it, range), and *scan the float scene*,
  which logs its NaN, infinite, negative and above-white pixels and the peak.
