# hellgate-london-fix

Root-cause investigation into the Hellgate: London (Steam, appid 939520) 1 FPS
stall, and a fix for the cause rather than the symptom.

Augmentrex stubs `hkMoppLongRayVirtualMachine::queryRayOnTree` globally, which
works but breaks AI line-of-sight and makes Ash/Oculis unkillable. The goal
here is to find out *why* the game asks for that much raycast work.

**Nothing in this repo modifies the game.** The only file that goes near the
install is a proxy DLL dropped into `bin/`; delete it to restore.
No game data, and no Havok SDK source, is redistributed here.

Findings so far: [`LOG.md`](LOG.md). Binary facts: [`notes/exe-facts.md`](notes/exe-facts.md).

## Current state

Phases 0–2 are done and verified offline. The instrumentation DLL builds,
loads, forwards, refuses a mismatched host, and honours a kill switch.
Phase 3 (repro under Proton) needs a human at the keyboard.

## Build

Needs a Fedora toolbox with `mingw32-gcc mingw32-binutils` (32-bit only —
the target is a 2018 MSVC8 x86 PE).

```sh
toolbox run -c dev bash -lc 'cd "$PWD" && make'
# -> build/version.dll
```

Offline sanity check, before involving the game:

```sh
toolbox run -c dev bash -lc 'cd build && \
  WINEPREFIX=$HOME/.hgpfx WINEDLLOVERRIDES="version=n,b" wine host.exe'
cat build/hellgate_rays.log     # expect: refuses to hook, wrong sha256
```

That refusal is the correct result: `host.exe` is not the game.

## Install

```sh
GAME=~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London
cp build/version.dll "$GAME/bin/"
```

To remove: `rm "$GAME/bin/version.dll"`. That is the whole uninstall.

## Run

Steam launch options for Hellgate: London:

```
WINEDLLOVERRIDES="version=n,b" PROTON_LOG=1 ggm -e MANGOHUD_CONFIG=fps,frametime,log_duration=0,output_folder=/tmp/mangohud %command%
```

(`ggm` is the local wrapper: gamescale -> gamemoderun -> MangoHud. Its `-e`
takes one `VAR=value`, and leading flags go to gamescale, so the `-e ...` sits
before `%command%`.)

`WINEDLLOVERRIDES` is **mandatory**. Without it Proton loads its own builtin
`version.dll`, our DLL is never mapped, and there is no error to tell you so —
the log file simply never appears.

Output lands in `$GAME/bin/hellgate_rays.log` (override with `HG_RAYS_LOG`).

| Control | Effect |
|---|---|
| `HG_RAYS_DISABLE=1` | load and forward, but do not hook |
| `bin/hellgate_rays.off` | same, without touching launch options |
| `HG_RAYS_STACKDEPTH=n` | frames captured per call site, 1–6 (default 6) |

## Reading the log

Two hooks, correlated per 100 ms window.

```
W <n> qray=<calls> qms=<ms in queryRayOnTree> qavg=<us/call> grays=<game raycasts>
  S tid=<t> n=<calls> nan=<n> huge=<n> zero=<n> len=[min/mean/max] scalarmax=<x> site=<rva<rva<rva...>
```

- `W` — per-window totals. `qray`/`qms` are the cost; `grays` is the demand.
- `Q` — **sampled attribution for `queryRayOnTree` itself**: one call in 32,
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
- `SPIKE` on a `W` line marks a window with more than 5000 `queryRayOnTree`
  calls — a stall, not normal play. `grep SPIKE` to find them.

## Preflight — two minutes, before any long session

Launch, reach the first zone, walk around, then check the log:

```sh
GAME=~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London
grep hooked "$GAME/bin/hellgate_rays.log"    # both hooks installed?
grep -m5 '^W '  "$GAME/bin/hellgate_rays.log" # grays= counting up?
grep -m5 'site=' "$GAME/bin/hellgate_rays.log"
```

`site=` values must look like plausible `.text` RVAs — 6–7 hex digits below
`e82000`. **If they are garbage, stop**: the game was built with frame-pointer
omission, x86 stack walking cannot work, and attribution needs the fallback
described at the end of `LOG.md`. No point grinding for a repro until that is
sorted.

Which hypothesis the numbers support:

| Signature | Reads as |
|---|---|
| one `S` site dominates `n` during spikes; `qray` tracks it | **H1, count** — that site is spamming raycasts |
| `nan` or `huge` nonzero, `len` max absurd | **H2, length** — garbage endpoints; the most likely root cause |
| `qray` flat but `qavg` and `qms` spike | **H3, tree** — degenerate MOPP for specific geometry |
| `grays` climbs monotonically across a long session | **H4, accumulation** — a leak |

Normal play, for comparison (measured over 20 minutes): `qray` p50 1306 /
p99 6177 / max 14417 per window, and at most 13.1ms of any 100ms window spent
in `queryRayOnTree`. Anything in that range is *not* the bug.

## Phase 3 procedure

Baseline **first, without the DLL**, to confirm the bug reproduces under
Proton and record the frametime signature (sustained ~1 s frames vs periodic
spikes). Then, with the DLL, in order:

1. idle in a hub — baseline ray rate
2. large outdoor zone, no combat
3. rockets / explosions in that zone (the Millennium battle area is the
   community's most-repeated repro)
4. a long session, for H4 — ray rate against wall time

Correlate MangoHud frametime spikes to `W` lines by timestamp. Write each run
up in `LOG.md` in the same hypothesis → line → run → conclusion form.

## Layout

```
src/        the proxy DLL (proxy.c forwards, hook.c instruments, target.h pins addresses)
tools/      static-analysis scripts (findpat, callgraph, xref, tagmap, rtti)
tools/ghidra/  headless Ghidra scripts
notes/      binary facts and raw decompilation output
test/       trivial 32-bit host for offline load testing
ref/        augmentrex and MinHook checkouts (not ours)
```
