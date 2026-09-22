# The "1 FPS" stall

The most-reported bug in the Steam release: every few minutes the game drops
to about one frame per second. Augmentrex "fixes" it by stubbing Havok's
`hkMoppLongRayVirtualMachine::queryRayOnTree` everywhere, which breaks AI
line of sight and makes Ash and Oculis unkillable. The aim here is the
cause, not the symptom. The full record is in [journal.md](journal.md)
(entries E1–E16).

## What is known

- **The raycast volume is Havok's own.** 98% of the rays into the MOPP tree
  come from inside the physics step, not from game code (E15). The game's
  own world raycasts are about 2%.
- **The cause is continuous collision detection (H8).** The game creates
  its Havok world in CONTINUOUS simulation mode, which sweeps moving bodies
  against the level every step. A third-party "2026 fix" changes exactly
  that one byte to DISCRETE ([reference/russian-patch-analysis.md](reference/russian-patch-analysis.md)).
- **Switching to DISCRETE halves the cost** (E14): median MOPP time 3.9 →
  2.0 ms per 100 ms window. The price is tunnelling protection, everywhere.
- **Body count is not what drives it** (E15): over 75 minutes and a 10×
  range of physics bodies, MOPP cost stayed flat (r = 0.018).
- **The stall does not reproduce under DXVK.** 75 minutes of play, no stall
  (E15). Under wined3d it was captured once: per-ray cost rose 30–40× for
  about 110 s, driven by a phantom cast from the per-unit update.

Status: no fix is shipped. `HG_SIM_TYPE=1` forces DISCRETE as an
experiment. The next step (E16) is to capture the pathological regime,
from a machine that reproduces it or by finding what tips it, and verify a
fix that keeps tunnelling protection.

## Instrumenting a session

The DLL hooks the MOPP ray machine and the game's raycast helper and writes
per-100 ms windows to `bin/hellgate_rays.log`. Preflight after two minutes of
play:

```sh
GAME=~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London
grep hooked "$GAME/bin/hellgate_rays.log"     # both hooks installed?
grep -m5 '^W '  "$GAME/bin/hellgate_rays.log"  # grays= counting up?
grep -m5 'site=' "$GAME/bin/hellgate_rays.log" # plausible .text RVAs?
```

## The log format

Two hooks, correlated per 100 ms window.

```
W <n> qray=<calls> qms=<ms> qavg=<us/call> grays=<game raycasts> steps=<havok steps> dt=[min/mean/max]ms
  S tid=<t> n=<calls> nan=<n> huge=<n> zero=<n> len=[min/mean/max] scalarmax=<x> site=<rva<rva<rva...>
```

- `W` — per-window totals. `qray`/`qms` are the cost; `grays` is the demand.
- `R` — **ray-level attribution**: one entry per `hkMoppBvTreeShape::castRay`
  call, i.e. one real ray, with the stack that asked for it. This is the
  line to read for "who is raycasting".
- `Q` — sampled attribution for `queryRayOnTree`, which is **recursive**:
  one call is a tree-node visit, not a ray. `nodes/ray` on the `W` line is
  the ratio. Do not treat `qray` as a ray count: one call in 32,
  `est=` is the scaled estimate, `ms=` is time attributed to that stack.
  This is the important line. A 20-minute clean session showed 393
  `queryRayOnTree` calls per game raycast, so the game's world-raycast helper
  is *not* what drives the MOPP tree; `Q` lines say what does.
- `S` — one line per distinct call site, innermost frame first, as RVAs
  (add 0x400000 for a VA). `nan` counts non-finite origin/direction/length;
  `huge` counts rays longer than 1e5 world units; `scalarmax` is the largest
  `length` argument that site asked for.
- `!` — the single worst ray that site asked for, printed verbatim
  (origin, direction, the `length` scalar, resulting ray length). Emitted
  only when a site logged a `nan` or a `huge`. This is the line that turns a
  hypothesis into a root cause: `grep '!' hellgate_rays.log`.
- `M` — address space and memory, sampled once a second (walking every VA
  region is not free). `largestfree` matters more than `free`: 400MB in 4MB
  shards is far worse than 400MB in one block, and that is what would scatter
  Havok's structures. **`private=` is always 0 under Wine** — psapi does not
  populate it; trust the VA-walk figures.
- `F` — floating-point state on the physics thread: denormal rate, MXCSR
  (with FTZ/DAZ decoded) and the x87 control word (with precision decoded).
  A non-zero `denorm=` rate that rises in expensive windows is **H7**.
- `SPIKE` on a `W` line marks a window with more than 5000 `queryRayOnTree`
  calls — a stall, not normal play. `grep SPIKE` to find them.

## Which hypothesis the numbers support

| Signature | Reads as |
|---|---|
| one `S` site dominates `n` during spikes; `qray` tracks it | **H1, count** — that site is spamming raycasts |
| `nan` or `huge` nonzero, `len` max absurd | **H2, length** — garbage endpoints; *refuted, E9* |
| `qray` flat but `qavg` and `qms` spike | **H3, tree** — degenerate MOPP for specific geometry |
| `grays` climbs monotonically across a long session | **H4, accumulation** — a leak |
| `dt` climbing and `qray` climbing with it, `grays` flat | **H5, feedback spiral** — *refuted, E12* |
| `largestfree` falling over a session while `qavg` rises | **H6, address-space pressure** |
| `steps` (active objects) climbing over a session with `qray` | **H1+H4** |
| `denorm=` non-zero and rising while `qavg` climbs | **H7, FP state** — demoted |
| `C` line says CONTINUOUS; DISCRETE halves `qms` | **H8, continuous collision detection**: the cause of the base load (E14). Its scaling with body count was refuted (E15) |

Normal play, for comparison (measured over 20 minutes): `qray` p50 1306 /
p99 6177 / max 14417 per window, and at most 13.1ms of any 100ms window spent
in `queryRayOnTree`. Anything in that range is *not* the bug.

## The repro harness

Built to provoke the stall by raising the number of moving bodies, which
H8 predicted would drive the raycast volume. E15 showed body count does not
(flat over a 10× range), so the harness is aimed at the wrong quantity. It
is kept because it still verifies a fix: same route, same multiplier,
compare `rays=`.

`HG_SPAWN_MULT=n` hooks the shared spawn primitive and, whenever the game
legitimately spawns something, spawns n-1 more using the identical context
it just used. That context is known-good and cannot go stale, so no struct
layout has to be reverse-engineered. (The `SpawnObject` script action is
hooked too, but across a whole session it never fired once — the primitive
at `0x0061c8f1` is the one that does the work.)

The multiplier needs the game to be spawning already. The dev panel's Spawn
tab drives the same machinery by hand, from a recorded spawn, and it is the
easier way to walk the body count up on purpose.

Walk it up gently — 2, then 5, then 10 — in an open zone, watching `rays=`
and `qms=` per window:

```
HG_SPAWN_MULT=5 PROTON_USE_WINED3D=1 WINEDLLOVERRIDES="version=n,b" PROTON_LOG=1 %command%
```

(`PROTON_USE_WINED3D=1` because the stall has only been seen on wined3d;
DXVK is the everyday renderer. wined3d cannot draw the engine's depth shadow
map, which is why the DLL selects the colour shadow map.)

`X spawns prim=N script=N amplified=M panel=N owed=N mult=n` lines record
what it did, split by which hook saw the spawn and whether it came from the
multiplier or from the panel's buttons.

**This deliberately destabilises the game.** It is a diagnostic, never
shipped, and it is budgeted per window so a runaway cannot wedge the
process. Do not use a save you care about.

## Capturing the stall

Baseline **first, without the DLL**, to confirm the bug reproduces under
Proton and record the frametime signature (sustained ~1 s frames vs periodic
spikes). Then, with the DLL, in order:

1. idle in a hub — baseline ray rate
2. large outdoor zone, no combat
3. rockets / explosions in that zone (the Millennium battle area is the
   community's most-repeated repro)
4. a long session, for H4 — ray rate against wall time

Correlate MangoHud frametime spikes to `W` lines by timestamp. Write each run
up in [journal.md](journal.md) in the same hypothesis → log line → run →
conclusion form.

