# What the graphics data exposes (plan step 0.1)

Written 2026-09-21. Read out of the game's `.xml.cooked` files with
`tools/hguncook.py`, a stdlib-only Python port of Reanimator-steam's cooked
reader (`ref/Reanimator-steam/hellgate/XmlCookedFile*.cs`). No Mono needed.

```sh
python3 tools/hgdat.py extract /tmp/hg "data\lights\"          # get files out
python3 tools/hguncook.py "/tmp/hg/data/lights/muzzle flash light.xml.cooked"
python3 tools/hguncook.py --defaults <file>   # also list absent fields + defaults
python3 tools/hguncook.py --xml <file>        # Reanimator-style XML
python3 tools/hguncook.py --check /tmp/hg     # parse everything, report failures
```

## Parse coverage

Every file must end exactly at the end of its data section or it counts as a
failure, so a pass means the whole layout was consumed correctly.

| Set (unique files after extracting `hellgate000` + `x_` overlays) | ok | fail |
|---|---|---|
| `data\lights\*` (LIGHT_DEFINITION) | 119 | 0 |
| `data\background\**\*_env.xml.cooked` (ENVIRONMENT_DEFINITION) | 184 | 0 |
| `data\screenfx\*` (SCREEN_EFFECT_DEFINITION) | 11 | 0 |
| `data\particles\skill particles\cabalist_zapper`, `weapons\shock rail pistol` (PARTICLE_SYSTEM_DEFINITION) | 68 | 0 |
| `data\skills\cabalist\*` (SKILL_EVENTS_DEFINITION) | 60 | 0 |
| `data\materials\*` (MATERIAL) | 726 | 0 |

The format is self-describing except for element names (32-bit hashes, CRC-32
poly 0x04C11DB7 fed MSB-first). The tool embeds the 922 names Reanimator
knows; an unknown hash prints as hex. Definitions are NOT nested per file by
class: the definition section in each file lists the elements that file's
cooker knew about (196 for environments, 141 for particles), and the data
section carries a presence bit per element.

**Curve convention.** All `t*` fields are keyframe paths. A float3 key is
`(time, min, max)`: the engine picks a random value in `[min, max]` per
particle / per light. A float4 key is `(time, r, g, b)`. Time runs 0..1 over
the object's life. For one-key colour paths the time slot is unused and often
holds junk (`tFogColor` time = -1.47 etc.); do not read anything into it.

## Lights (`LIGHT_DEFINITION`, 12 fields)

| Field | Type | Default | Meaning |
|---|---|---|---|
| `eType` | int | 0 | 0 = point (117 files), 1 = spot (2 files) |
| `nDurationType` | int | 1 | seen 0 (6), 1 (68), 2 (10), 3 (34). 3 goes with a non-zero `fLoopTime` (looping); 1/2 are one-shots that read `fStartTime`/`fEndTime` |
| `fStartTime` / `fLoopTime` / `fEndTime` | float | 0.5 each | attack / loop period / decay, seconds |
| `fSpotAngleDeg` | float | 45 | spot cone |
| `tColor` | float4 path | 0.5 | colour keys |
| `tFalloff` | float3 path | 0 | `(t, near, far)` in metres: full brightness to `near`, zero at `far` — **this is the light radius** |
| `tIntensity` | float3 path | 1 | `(t, min, max)` intensity keys over the light's life |
| `dwFlags` | not cooked | 0 | never present |
| `szSpotUmbraTexture` | string | – | spot projector texture |

Examples (present fields only):

```
muzzle flash light      nDurationType 2, fStartTime 0
  tColor      (0.70, 0.83, 0.98)   cyan-white
  tFalloff    near 1.47  far 3.12 m
  tIntensity  2.31 @0 → 1.7 @0.17 → 2.56 @0.37 → 1.46 @0.73 → 2.04 @1   (flicker)

fire light 1m           nDurationType 3, fStartTime 0.1, fLoopTime 2
  tColor      (1.00, 0.71, 0.27)   orange
  tFalloff    near 4.13 far 5.32 m @0 → near 4.22 far 5.32 @1
  tIntensity  11 keys between 0.16 and 0.27 (min) / 0.65 and 0.87 (max)   (flicker)

firebolt explosion      all timing defaults (0.5 s in / 0.5 s out)
  tColor      (1.00, 0.71, 0.42)
  tFalloff    near 1.47 far 3.39 m
  tIntensity  0.92–1.18 @0 → 0.86–1.14 @0.16 → 0 @0.99

peacemaker explosion    (the light 'cabalist impact 1' and 'cabalist muzzle 1v' attach)
  tColor      purple (0.47,0.29,0.71) @0 → grey 0.75 @0.65 → grey @0.99
  tFalloff    near 1.10 far 3.67 m
  tIntensity  1.05–1.65 @0 → 1.40–2.15 @0.10 → 0.95–1.54 @0.22 → 0.44–0.70 @0.33 → 0–0.18 @0.64
```

So: every spell light today reaches 3–5 m and peaks around intensity 1–2.5.
Radius (`tFalloff`), brightness (`tIntensity`), colour and duration are all
plain data edits.

## Particle systems (`PARTICLE_SYSTEM_DEFINITION`, 141 fields)

Fields that matter for the plan (all `t*` are `(t, min, max)` paths):

- **Light attachment:** `pszLightName` — a `data\lights\` file name, e.g.
  `Peacemaker explosion.xml`. This is how a spell gets its light: a particle
  system names a light. Of the 20 cabalist_zapper systems, only
  `cabalist impact 1` and `cabalist muzzle 1v` attach one; `conjure fire`
  (a fire spell) attaches **none**. Giving lights to more systems is a
  one-string data edit. `vLightOffset` positions it.
- **Density:** `nLaunchParticleCount` (burst on spawn), `tParticlesPerSecondPath`,
  `tParticlesPerMeter`, `tParticlesPerMeterPerSecond`, `tParticleBurst`,
  `fMinParticlesPercentDropRate` (LOD drop rate, TestCentre field).
- **Life / size:** `fDuration` (system, s), `tParticleDurationPath` (per particle),
  `tParticleScale`, `tLaunchScale`, `tParticleStretchBox/Diamond`.
- **Look:** `tParticleColor` (float4), `tParticleAlpha`, `tParticleGlow`
  (bloom contribution; set on 15 of 68 systems), `nLighting` (0 in every
  extracted file = unlit sprites), `pszTextureName`, `nShaderType` (excel row
  in `EFFECTS_SHADERS`; unset everywhere here, so the engine's default
  `particle` shader), `nGPUShader`, `nDrawOrder`, `tAlphaRef`, `tAlphaMin`.
- **Distortion:** `tParticleDistortionStrength` — present in the definition,
  set in none of the 68 files. Widening heat distortion means setting this
  plus whatever flag selects `particledistortion.fxo` (see flags below).
- **Soft particles already exist as fields:** `fSoftParticleScale`,
  `fSoftParticleContrast` — in the definition, set nowhere here. They are
  almost certainly consumed only by the DX10 path (the DX10 `particle.fxo`
  set); worth checking in step 0.2 whether the DX9 `particle.fxo` reads them.
- **Chaining:** `pszNextParticleSystem`, `pszFollowParticleSystem`,
  `pszDyingParticleSystem`, `pszRopeEndParticleSystem` — systems are chains
  of files (`cabalist impact 1` → `Cabalist Impact 2.xml`).
- **Culling:** `fCullDistance` (20–40 m on these), `nCullPriority`,
  `nViewParticleSpawnThrottle`.
- **Fluid/smoke sim block** (`nGridWidth/Depth/Height`, `tFluidSmoke*`,
  `pszTextureDensityName`…) — DX10-only GPU smoke; ignore on D3D9.
- Flags: `dwFlags`, `dwFlags2`, `dwFlags3`, `dwUpdateFlags` are raw dwords.
  Reanimator has no names for the bits and the exe carries no
  `PARTICLE_SYSTEM_FLAG_*` strings, so bit meanings need reversing
  (`cabalist impact 1` = 0x040280A0, `conjure fire` = 0x040000E1 / flags2
  0x1002).

Two systems in full:

```
cabalist impact 1        dwFlags 0x040280A0, nLaunchParticleCount 12, fDuration 1
  pszTextureName        flame 4frame spectral.tga
  pszLightName          Peacemaker explosion.xml
  pszNextParticleSystem Cabalist Impact 2.xml
  nSoundGroup           CabFocusBoltImpact
  tParticleScale        0 @0 → 1.09–1.85 @0.99
  tParticleDurationPath 0.43–0.68 s
  tLaunchDirRotation    ±69°;  tLaunchDirPitch -90;  tLaunchSpeed 1–1.4;  tLaunchRotation ±360
  tParticleWorldAccelerationZ 0.42–0.83
  tParticleAlpha        0.51 @0 → 0.38 @0.38 → 0 @1
  tParticleColor        white
  nDrawOrder 1, fCullDistance 40

conjure fire             dwFlags 0x040000E1, dwFlags2 0x1002, fDuration 1
  pszTextureName        Fire_Anim.tga
  pszLightName          (none)
  pszNextParticleSystem Conjure fire 2.xml
  tParticlesPerSecondPath 40
  tParticleScale        1.57–1.95 @0 → 1.21–1.38 @0.15 → 0.62–0.68 @0.46 → 0.32–0.36 @0.69 → 0 @0.99
  tParticleDurationPath 1 s
  tLaunchOffsetX/Y      ±0.5 m;  tLaunchOffsetZ 0.97–1.38
  tLaunchDirPitch       ±90;  tLaunchRotation -45
  tParticleAlpha        0 @0 → 0.04–0.09 @0.14 → 0.21–0.32 @0.47 → 0.44–0.59 @0.76 → 0.89 @1
  tParticleColor        (0.5,0,0) @0.21 → (1,0.5,0) @0.36 → (1,0.75,0) @0.53
  tParticleAttractorAcceleration 3;  tAttractorDestructionRadius 0–0.23
  fCullDistance 20
```

## Environments (`ENVIRONMENT_DEFINITION`, 196 fields; 148 of them are SH coefficients)

| Field | Notes |
|---|---|
| `nFogStartDistance` (int, default 30) | fog begins here (m) |
| `nClipDistance` (int, default 100) | far clip **and** where fog reaches full — there is no separate fog-end field. Seen 60…3000; most common pairs: clip 200 / fog 0 (13 envs), clip 200 / fog 10 (7), clip 60 / fog 3 (6) |
| `tFogColor` (float4 path) | fog colour |
| `tAmbientColor`, `fAmbientIntensity` (default 0.25) | flat ambient |
| `tHemiLightColors[0]`, `[1]`, `fHemiLightIntensity` | hemisphere sky/ground |
| `tDirLights[3]` (ENV_LIGHT_DEFINITION: `tColor`, `fIntensity`, `vVec`) | the three directional lights; `ENVIRONMENTDEF_FLAG_DIR1_OPPOSITE_DIR0` mirrors light 1 from light 0 |
| `fShadowIntensity` (default 1) | directional shadow darkness |
| `fBackgroundSHIntensity`, `fAppearanceSHIntensity` + `tBackgroundSHCoefs*`, `tAppearanceSHCoefs*` | baked SH ambient, gated by `ENVIRONMENTDEF_FLAG_HAS_*_SH_COEFS` |
| `fCharacterLight_Distance/FalloffStart/FalloffEnd`, `tCharacterLight_Color` | the always-on light on the player |
| `szEnvMapFileName`, `sz{Background,Appearance}LightingEnvMapFileName` | cube maps (`GeneratedCubeMap.dds`) |
| `fWindMin/Max`, `vWindDirection` | wind for particles / cloth |
| `nSilhouetteDistance`, `tBackgroundColor`, `szSkyBoxFileName`, `nLocation` | misc |
| `ENVIRONMENTDEF_FLAG_FLASHLIGHT_EMISSIVE`, `..._SPECULAR_FAVOR_FACING` | shading toggles |

Example, `ongoing_a01f_env` (the richest one):
```
dwDefFlags 0x7 (DIR1_OPPOSITE_DIR0, HAS_APP_SH, HAS_BG_SH)
skybox Patch_One_skybox.xml, env/lighting cubemaps GeneratedCubeMap.dds
wind 4–8, clip 75, silhouette 70, fog start 2, fog colour (0.016, 0.11, 0.12)
ambient (0,0,0) x 0, background 0.376 grey, hemi both black
shadow 0.9, SH intensities 0.5 / 0.5
dir0 (0.45,0.54,0.60) x 1.6 dir (0.47,-0.47,-0.75)
dir1 (0.44,0.58,0.62) x 1.6 dir (0.49,-0.78,0.39)
dir2 (0.52,0.60,0.69) x 0.8 dir (0.60,-0.60,-0.52)
```
`outdoor_hunterbasea_env` by contrast sets only skybox, silhouette 150, fog
colour 0.5 grey, ambient (0.24,0.4,0.5), background white and three dir
lights at intensity 0 — everything else is default (fog start 30, clip 100).

The environment is where fog and ambient art direction lives, so a depth-fog
pass (plan step 2) can read `nFogStartDistance`, `nClipDistance` and
`tFogColor` from the active environment rather than inventing values.

## Screen effects (`SCREEN_EFFECT_DEFINITION`, 18 fields)

`szTechniqueName` (a technique in `ScreenEffects.fxo`), flags
`SCREEN_EFFECT_DEF_FLAG_EXCLUSIVE` / `_DX10_ONLY`, four generic
`fFloats[0..3]`, two colour paths `tColor0/1`, two optional textures
`szTextureFilenames[0..1]`, `nPriority`, `fTransitionIn/Out` (seconds).
Meaning of the floats is per technique.

```
motionblur   technique MotionBlur, DX10_ONLY = 1 (!), floats 0.05 / 0.1 / 0.3,
             colour (0.57,0.29,0.17), priority 0, in 0.3 s, out 2.4 s
sniper       technique Vignette, floats 1 / 0.7 / 1, colour (0,0,0), priority 2, in 0.4 s
```
Techniques seen across the 11 files are what `ScreenEffects.fxo` offers; the
motion blur one is flagged DX10-only and so never runs on this build.

## Skills (`SKILL_EVENTS_DEFINITION`) — how lights reach spells

Skill files are event lists (`Add Attachment`, `Fire Missile`, `Fire Laser`,
`State - Set`, …) with a `tAttachmentDef` per event: `eType` (1 or 3 seen —
particle system vs. other), `pszAttached` (e.g. `storm lightning`,
`Electric Lasers`), offsets and normals. No skill event attaches a light
directly; lights come through the particle system's `pszLightName`. So the
step-4 "stronger, more numerous spell lights" is: (a) edit the light files,
(b) set `pszLightName` on more of the spell's particle systems.

## Surprises / open items

- Soft-particle and distortion parameters already exist in the particle
  definition but are unset in every file looked at; check in 0.2 whether the
  D3D9 `particle.fxo` has any constant for them or whether they are DX10-only.
- Particle `dwFlags*` bits are unnamed anywhere (Reanimator or exe strings).
- There is no fog-end distance; fog saturates at `nClipDistance`.
- Colour-path time slots hold junk on one-key paths; a writer must not
  round-trip them through validation that expects 0..1.
- Writing back is untested: `hguncook.py` only reads. Cooking is the inverse
  (same definition section, presence bitfield, values) and is straightforward
  to add once the loose-file / repack question (plan 0.4) is settled.
- File counts differ from `hgdat list` (134 lights listed vs 119 unique):
  the `x_hellgate000` overlay archive repeats files, and `extract` writes
  both to the same path.
