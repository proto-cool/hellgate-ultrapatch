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
  not the fill or the baked light; indoors, only the light above the
  ambient and SH floor. No more double darkening on baked shadows.
- **Look controls.** Fill, fog start and sun strength, with a 2007 preset.
- **Per-pixel point lights on characters**, as an additive pass.
- **DXVK as the renderer.** wined3d cannot draw the depth shadow map.

## Next, in order

0. **Interiors, next session** (planned 2026-09-23; shader and lighting
   only, no texture changes; interiors look least different from stock):
   - **Light spill (first): second cut in** (2026-09-24, `shaders/ao.fx`
     Spill*, `spill()` in `src/postfx.c`). The first cut read emitters off
     the screen (bright pixels into a pyramid, gathered in screen space):
     it only worked close to a lamp and at some angles, and swam with the
     camera, so it was redone in world space. Now the engine's nearby
     lights (src/plshadow.c's list: lamps, fires, portals, spells) light
     each surface by position and normal over their radius x *reach*,
     blocked by a march through the depth buffer (or the shadow cube),
     gathered at half resolution, applied as frame x (1 + light / local
     brightness). Indoors only. Emissive surfaces without an engine light
     (a screen with no light near it) light nothing; that would need the
     materials to report their emission.
   - **Contact shadows indoors: first cut in** (2026-09-24,
     `shaders/ao.fx` Contact, `contact()` in `src/postfx.c`): full
     resolution after the AO, 16 steps over about a unit towards a light
     direction blended from the nearby lights plus some from above; only a
     thin band of depth in front of the ray blocks, so the foreground casts
     nothing (the spill's long march drew silhouettes). Previously planned:
     **Contact shadows indoors.** Indoor shadow maps are coarse, so props
     and feet float. A short march through the depth buffer towards the
     room's main light darkens where something close blocks it: sharp
     grounding shadows over a few units, fading out, complementing the
     soft map shadows. One full-screen pass; care at depth edges (as AO);
     a panel setting, 0 stock.
1. **Test the indoor shadow fill in game** (2026-09-23). The double
   darkening (a live shadow on a baked one) is gone outdoors since the
   shadow fill; indoors the fill now keeps the ambient and SH floor. Tune
   or back out from what it looks like.
2. **Tune and default the look.** Match the 2007 screenshots with the LOOK
   controls, then consider data edits (environment fog and ambient) once
   cooked data can be written back.
3. **AO on the ambient light only: first cut in** (2026-09-23), eased off
   where the sun lights the surface, plus a depth-aware upsample. To tune
   in game; the exact version (the materials writing their ambient share
   to a second target) stays unbuilt unless this falls short.
4. **Screen-space GI: first cut in** (2026-09-23): one-bounce colour from
   the AO samples, frame x (occlusion + bounce). To judge in game.
5. **Sun cascades: spiked** (2026-09-23). The engine already has three
   nested maps at 3x steps (near 27, fine 80, zone ~300 units); the fine
   one now follows the camera. What is left is tuning the near reach
   (characters' shadows end at its edge) against its redraw cost.
6. **Spell effects.** Mostly content on top of the point lights, lit
   particles and fog: brighter lights on spells, particle density and
   lifetime. Needs cooked data written back (Reanimator-steam can repack;
   the round trip is untested).

Done since the list was written: scene depth with SMAA and AO, bloom and
colour grade, soft and lit particles, volumetric fog, point-light shadows
(off by default), the outdoor shadow fill. The engine's cap of 5 lights per
mesh stands; raising it means widening `tLights` and one compare (see
renderer.md).

Later, lower priority: parallax mapping from height fields integrated out
of the normal maps, higher shadow-map resolution and draw distances (data
values).

## Open: the character select's razor wire (2026-09-24)

A concertina coil casts a spiral contact shadow on the London character
select but never shows, in stock and for everyone. Ruled out: HDR, Umbra
occlusion (all visible), the alpha test (No alpha test), face culling and
the depth test (debug toggles), the depth pre-pass's own draws (per-draw
capture and the Depth w/o colour view's depth mask), missing vertex
streams, and being a shadow-pass-only mesh (Shadow-only meshes view: only
the dead tree's cut-out cards show). Restored along the way: the 2007
city\razorwire.dds and seven other full-detail textures 2018 had replaced
with their low copies (sp_hellgate_bghigh_2337). Next: the Depth w/o
colour view now marks any draw by what it writes (depth, colour) whatever
its effect; if the coil shows there it is a depth-only draw outside
_zbuffer. Otherwise a pick-at-cursor capture (a 1x1 scissor and an
occlusion query per draw) names the draws that touch the coil's pixels.

## Constraints

- **Stock is one click away.** Every change is a runtime setting whose zero
  value reproduces stock, proven by `make matcheck`. Finished features
  default on.
- **The user tests in the real game.** Each feature gets a panel control
  and a counter so it can be A/B'd live.
- **Performance headroom is on the GPU.** The game's limits are CPU-side
  (physics), so per-pixel work is affordable; the PCSS taps are the largest
  cost so far.
