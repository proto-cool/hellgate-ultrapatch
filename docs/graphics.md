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
own D3DX (`fxload.exe`) and installs only if all of them load: about 25 s,
or 13 s when nothing changed.

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
  wide maps are marked dirty every 5 s.
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
- Outdoors the lights skip the sun's shadow; indoors the shadow map is cast
  from one of them, so they take it as stock's vertex lights did.

This replaced an additive second pass (`actor_lights.hlsl`, removed): two
passes meant two depth tests, forced render states, and a shimmer on
characters under PCSS.
