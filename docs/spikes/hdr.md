# Spike: internal HDR, a float scene tone-mapped to the 8-bit back buffer (2026-09-23)

Goal: render the 3D scene into a 16-bit float target, so light above white
survives, and tone-map it once into the normal back buffer before the UI.
SDR display only. Addresses are for sha256 `401e011d…`.

**Status (2026-09-23): built as planned (section 2), on by default;** see
`docs/graphics.md`, "HDR scene". Differences from the plan below: no
`GetBackBuffer` hook was needed (the engine rebinds the pointer it has); the
engine's glow copy (float to A8R8G8B8) works under DXVK; AO's bounce copy is
float when the scene is; material output is clamped to 0..16 in HDR mode;
the tone map is a hue-preserving shoulder rather than a filmic curve; step 0
(the engine's `nHDRMode 3`) was skipped.

## 1. The engine's own HDR mode: present, but not usable

**It exists.** `dxC_hdrange.cpp` holds a DX9 HDR pipeline:

- **Mode.** `FUN_007d9a89` returns the HDR mode, the global `0x00ee1ca8`.
  It is set at device init by `FUN_007d9a13` (called from `0x77c878`) with
  EDX = `[FUN_00435f13() + 0x124]`, a field of the engine settings. The
  settings table names it `nHDRMode` (string `0xa62d3c`, registered at
  `0x97e510`). It is not in the user's `settings.xml`, so it is 0.
- **Modes** (`FUN_007d9aa3`): 1 and 3 set `SRGBWRITEENABLE` off, 2 sets it
  on. Mode 3 is HDR. `FUN_007d99be` checks device caps per mode.
- **Resources in mode 3** (`FUN_007d9689`, from `FUN_0077c686` at device
  init): `FUN_007d8d84` creates "HDRScene", a render-target texture the
  size of the screen in the float format the caps pick, at `0x00fc1210`.
  `FUN_007d8e0e` creates six luminance textures, 1x1 to 243x243, from
  `0x00fc1214`.
- **Per frame in mode 3** (`FUN_0077a311`, from `e_Render`): instead of the
  glow chain, `FUN_007d8edc` ("Luminance::MeasureLuminance") downsamples
  HDRScene to 1x1 with `hdr.fxo`'s `TLuminance`, then `FUN_007d973a`
  ("HDRPipeline::ApplyLuminance") sets render target 0 and draws
  `TFinalPass`: a Reinhard-style tone map (key / average luminance) and a
  gamma 2.2 encode.
- **Scene target in mode 3.** `FUN_007d6ead` maps the scene target id -2 to
  0 in every mode but 3; in mode 3 it stays -2, which the resolver
  (`FUN_007d6e8e`, table `0x00f91624`, ids 0 to 9) turns into -1.

**Why it is not usable.** Nothing ever renders or copies the scene into
HDRScene. Every reference to `0x00fc1210` is its creation (`0x7d8db4`), its
release (`0x77ce53`, `0x9a1dee`), or the getter `FUN_007d8dfa`, whose only
two callers are the luminance pass (`0x7d8f29`) and the tone map
(`0x7d984b`). Both only read it. So mode 3 would tone-map an unwritten
texture. The path looks like a DX10 port that was never wired up on DX9.
The glow chain (the game's only bloom) is also skipped in mode 3.

**A 15-minute check, not done here:** add `<nHDRMode>3</nHDRMode>` to
`Documents\My Games\Hellgate\Settings\settings.xml` and look. The expected
result is a black or garbage frame, which confirms the above. Also
unconfirmed: that settings field `+0x124` is `nHDRMode` (the registration
writes offset `0x120` nearby).

## 2. Our route: redirect the scene to our own float target

What the frame does now (from the journal's target probe, DXVK):

- The scene draws straight into the back buffer (2560x1600 A8R8G8B8, no
  MSAA since the SMAA device change), with our INTZ depth.
- The glow chain uses three 640x400 A8R8G8B8 targets (gaussian,
  combinelayers, overlay) and one full-size A8R8G8B8 copy of the back
  buffer (`dxC_CopyBackbufferToTexture`, `0x7d8203`, in `dxC_target.cpp`).
- Render targets go through `dx9_SetRenderTarget` (`0x7d6d4b`) and
  `dxC_SetRenderTargetWithDepthStencil` (`0x7d6ef6`). The back buffer comes
  from `dxC_GrabSwapChainAndBackbuffer` (`0x77bb8e`).
- Our postfx already runs AO between the opaque and transparent halves, and
  fog, bloom, the grade, SMAA and CAS at the first `ui.fxo` pass (or at
  Present on a frame without UI).

**The plan.** At device creation (`src/device.c`) make one A16B16G16R16F
render target the size of the back buffer. At the device level, hook
`SetRenderTarget`, `GetRenderTarget`, `GetBackBuffer` and `StretchRect`.
While the frame is in its scene phase, any use of the real back buffer as
a target or a copy source gets our float surface instead. INTZ depth stays
as is. At the first `ui.fxo` pass: bloom on the float scene, tone-map into
the real back buffer, then the grade, SMAA and CAS on 8-bit, as now. The
UI then draws on the real back buffer. Present and screenshots
(`src/compare.c`) see a normal 8-bit frame.

## 3. The glow alpha

- Materials write a glow mask into alpha: the colour's overflow above 1
  (the "soft clamp", see 4), specular glow and self-illumination
  (`shaders/actor.hlsl` around line 494, `background.hlsl` around 614).
- The glow chain reads that alpha through the full-size copy, blurs it at
  640x400 and adds it back: the engine's bloom.
- A float target keeps a 16-bit alpha, so the mask survives and blending
  works as before. Our postfx already writes colour only (the alpha is
  left alone).
- **The catch:** the full-size copy is an A8R8G8B8 texture. D3D9
  `StretchRect` cannot convert float to fixed formats, so once the source
  is our float surface that copy fails. Either make the copy's destination
  float too (hook its `CreateTexture`, identified by size, format and
  render-target usage), or do the copy with a shader.

## 4. Clamps: what keeps values above 1

- **Materials.** Both families apply the stock soft clamp: colour divided
  by `max(1, max channel)`, with the overflow sent to the glow alpha
  (actor.hlsl line 31 describes it). That is the engine's own
  high-dynamic-range trick in an 8-bit target: brightness above 1 becomes
  glow. For HDR, a knob (for example `gvUltraHDR.x`) skips the divide and
  drops or cuts the overflow glow, so bloom comes from real brightness
  instead. At 0 it is stock: `matcheck` parity holds.
- **Fog** is applied in the materials (`lerp(FogColor, colour, f)`), so it
  works on any target.
- **Particles** are stock vs_1_1/vs_2_0 with ps_2_0: `texture x vertex
  colour`, additive and alpha variants. Additive blending onto a float
  target keeps sums above 1, which is a gain for fire and spells. They do
  not clamp.
- **Fixed-function passes and the sky:** no clamp in the shader. Their
  output is whatever the texture and vertex colour give, at most 1 each.
- `fGammaPower 1.15` in `settings.xml` (lifts midtones): find where it is
  applied (a gamma ramp or the overlay pass) before choosing the tone
  curve, so it is not applied twice.

## 5. Risks

1. **Copies from the back buffer to fixed formats** (section 3), and our
   own copies too: AO's colour copy for the bounce (`StretchRect` into a
   half-size A8R8G8B8), the fog and bloom scene copies. Each needs a float
   destination or a shader copy.
2. **Order at the UI point.** The resolve must come after everything that
   belongs to the scene, glow chain included, and before the first UI
   draw. SMAA already relies on that point. Frames without UI resolve at
   Present, as SMAA's fallback does.
3. **DXVK and D3D9 float targets.** A16B16G16R16F targets with blending and
   filtering are supported on DXVK, and the engine's own caps check asks
   for the same. Fog is per-vertex, in the shader, so float targets do not
   affect it.
4. **Double brightness.** With the soft clamp off, stock glow would add on
   top of real brightness. The overflow share of the glow must go to 0 in
   HDR mode.
5. **Memory.** One 2560x1600 A16B16G16R16F surface is 33 MB, and a float
   copy target is 33 MB more. Wine gives the process about 4 GB (journal:
   address space), so this is small.
6. **Bandwidth.** Float targets double the scene's write cost. The game is
   CPU-bound (physics), so there is headroom.

## 6. Plan

| Step | What | Effort |
|---|---|---|
| 0 | Set `nHDRMode 3` in settings.xml and look (confirm it is dead) | 15 min |
| 1 | Device: the float scene target; hook SetRenderTarget, GetRenderTarget, GetBackBuffer and StretchRect for the scene phase; make the full-size copy float | 1-1.5 days |
| 2 | Resolve at the UI point (and at Present without UI): a plain tone map (exposure x filmic curve) into the real back buffer | 0.5 day |
| 3 | Materials: the HDR knob (no soft clamp, no overflow glow), `matcheck` at 0 | 0.5 day |
| 4 | Postfx on float: AO's bounce copy, fog, bloom from linear brightness above a threshold, then the grade, SMAA and CAS after the tone map | 1 day |
| 5 | Tune in game: exposure, curve, bloom; compare with stock | 1 day |

Total: about 4-5 days.

**Off is exact stock.** One setting (saved, also `bin\hellgate_hdr.off`):
off means no float target, no hooks acting, the material knob 0 (stock
soft clamp and glow), and the postfx path as it is today. Like the SMAA
device change, turning it off takes effect from the next start.
