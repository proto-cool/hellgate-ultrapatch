# Graphics roadmap

The aim: keep the 2007 art, and bring its lighting up to date: lights that
light, shadows that behave, fog and effects with depth. Changes are made in
the engine's own pipeline, not with a post-process filter, and each ships
behind a panel setting that defaults to the stock look.

How the work is done: [graphics.md](graphics.md). Engine facts:
[reference/renderer.md](reference/renderer.md) and
[reference/stock-shaders.md](reference/stock-shaders.md).

## Done

- **Instrumentation and override loading.** Every effect the engine creates
  is identified and can be replaced from `override/`.
- **Own material shaders.** All 1,482 variants rebuilt from HLSL,
  pixel-identical to stock, with a parity harness and a 25-second build.
- **Soft shadows (PCSS)** with contact hardening, indoor and outdoor sun
  sizes, minimum softness and bias, on an R32F colour shadow map.
- **Shadow fill.** Outdoors, a dynamic shadow removes only the sun's light,
  not the fill or the baked light.
- **Look controls.** Fill, fog start and sun strength, with a 2007 preset.
- **Per-pixel point lights on characters**, as an additive pass.
- **DXVK as the renderer.** wined3d cannot draw the depth shadow map.

## Next, in order

1. **The double darkening** where a live shadow lands on a baked one (static
   casters are on by default since 2026-09-22). The engine's cap is 5 lights per mesh; raising it
   means widening `tLights` and one compare (see renderer.md).
2. **Shadow fill indoors.** Indoor materials have no sun term to separate,
   so a different split of the shadowed light is needed.
3. **Tune and default the look.** Match the 2007 screenshots with the LOOK
   controls, then consider data edits (environment fog and ambient) once
   cooked data can be written back.
4. **Scene depth: done** (2026-09-22): MSAA off, SMAA 1x, INTZ depth, and
   screen-space AO on it (see graphics.md). Next on it: AO on the ambient
   light only (the materials report their ambient share), a depth-aware
   upsample.
5. **Screen-space GI: SSDO with one-bounce colour bleed**, then bloom and our
   own tone map (the stock `hdr.fxo` never runs).
6. **Soft particles and lit particles.** Fade sprites where they meet
   geometry; light smoke and debris with the same point lights.
7. **Fog with depth.** Height fog and in-scattering around bright lights,
   driven by each environment's fog colour and distances.
8. **Sun cascades.** Two or three cascades instead of one 2048 map
   refreshed at 30 Hz; the stock map can serve as the near cascade.
9. **Shadows from interior lights.** Contact shadows in the light pass
   first, then shadow maps for the one or two dominant lights.
10. **Spell effects.** Mostly content on top of 1, 6 and 7: brighter lights
    on spells, particle density and lifetime. Needs cooked data written
    back (Reanimator-steam can repack; the round trip is untested).

Later, lower priority: god rays and volumetric fog (need depth and the
sun's screen position), parallax mapping from height fields integrated out
of the normal maps, higher shadow-map resolution and draw distances (data
values).

## Constraints

- **Stock is one click away.** Every change is a runtime setting whose zero
  value reproduces stock, proven by `make matcheck`. Finished features
  default on.
- **The user tests in the real game.** Each feature gets a panel control
  and a counter so it can be A/B'd live.
- **Performance headroom is on the GPU.** The game's limits are CPU-side
  (physics), so per-pixel work is affordable; the PCSS taps are the largest
  cost so far.
