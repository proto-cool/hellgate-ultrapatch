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
Community complaints and existing mods (web research): [`notes/community-research.md`](notes/community-research.md).

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
| `HG_RAYS_STACKDEPTH=n` | frames captured per call site, 1–12 (default 12) |
| `HG_RAYS_SELFTEST=1` | run the address-space walk once at startup, even on a non-matching host |
| `HG_SPAWN_MULT=n` | **repro harness.** Every real spawn becomes n. Off at 1. Deliberately destabilising — see below |
| `HG_SPAWN_CAP=n` | max extra spawns per 100ms window (default 200) |
| `HG_SPAWN_MONSTERS=1` | also amplify `SpawnMonsterNearby`. Much more disruptive than objects |
| `HG_SIM_TYPE=1` | force Havok `m_simulationType` to DISCRETE. **Experiment only** — this is what the "2026 fix" does, and it removes tunnelling protection globally |
| `HG_PANEL=1` | the in-game dev panel. `bin/hellgate_panel.on` does the same without touching launch options |
| `HG_PANEL_HTTP=1` | also serve the panel over `http://127.0.0.1:7777/` |
| `HG_PANEL_PORT=n` | move that off 7777 |
| `HG_OVERLAY_OFF=1` | panel on, in-game overlay off (HTTP only) |
| `HG_FP_MELEE=1` | let melee weapons use first person (see below). Also a toggle on the panel's Camera tab |

## Dev panel

`HG_PANEL=1`, then **Shift+`** in game. That is `CMD_CONSOLE_TOGGLE`'s own
binding, recovered from the keybind table; the console it used to open is
compiled out of this build, so the binding is dead and free to take. A bare
`` ` `` is the chatbox and is left alone.

A draggable window with six tabs. It is mouse-driven — click the tabs, click
the buttons — and everything also has a `ctrl`+key, because the click reaches
the game as well (there is no way to swallow a DINPUT8 button from an
EndScene hook) and because exclusive fullscreen can pin the cursor.

| Tab | What it does |
|---|---|
| Live | frame and MOPP counters, plus the last 6 s as a graph. Reset counters |
| Player | name, unit pointer, `+0x110` flags, the watch list |
| Memory | hex window over the player unit: move it, mark a baseline, click a dword, poke or watch it |
| Spawn | fire spawns on demand; see below |
| Physics | observe and override `hkWorldCinfo::m_simulationType` |
| Log | the last two dozen log lines, so a button's outcome is visible without alt-tabbing |

Keys, all with `ctrl`: `1`–`6` tabs, `up`/`down` move the memory window by
0x10, `pgup`/`pgdn` by 0x100, `home` back to the top, `m` mark the baseline,
`b` queue ten spawns.

**Finding an offset** is what the Memory tab is for, because the unit struct
is almost entirely unmapped — only `+0x110` (flags) and `+0x120` (name) are
known. Put the window where you want it, press **Mark**, go take a hit, and
every byte that changed is highlighted. Click one to select its dword, then
**Watch it** to keep an eye on it. Confirming an offset that way is what
turns it into a named button here.

Off by default. Loopback-bound. Single player only — it pokes memory in a
live process and will happily corrupt a save.

### First person with a melee weapon

First person is fully alive in this build — camera mode 0, with engine
handlers, a dedicated branch in the camera update, its own appearance group
and even first-person footstep data. What blocks it is the **weapon**.

`SetCameraMode` enforces the rule itself, at `0x004DC095`: after the
requested mode is in `ebx` it looks up the items in both weapon slots and
asks `CanUseFirstPerson` (`0x004DBFB5`) about each. That predicate reads two
flags off the weapon's type row (`ExcelGetBool(table 0x17, row, 0x2C / 0x29)`)
and, if either is set, the request is rewritten to third person — *every*
request, including the engine's own `FirstPersonCamera` event. A second kick
fires on skill start at `0x0062B31B`, so even past the first gate, swinging
would throw you back out.

`HG_FP_MELEE=1` bypasses both: it detours `CanUseFirstPerson` to return 1
(it has exactly two callers, both the slot checks, so nothing else is
affected) and flips one byte at `0x0062B322` from `jne` to `jmp` to skip the
skill-start block. The byte is restored when you toggle it back off.

Expect cosmetic trouble — first-person models are a separate appearance
group and melee weapons may have no entries in it.

### Spawning on demand

The panel cannot synthesise a spawn: the game's spawn primitive takes
thirteen dwords of context nobody has mapped. What it does instead is
**record** a spawn the game performs — every argument, verbatim — and replay
that exact call on demand. One real spawn anywhere in the zone arms the
panel; after that **Fire 1 / 10 / 100** work immediately.

Until that first spawn is seen there is no template and nothing can fire, and
the tab says exactly that, along with the per-hook call counts and the thread
ids involved. The previous version of this hid that case: it queued the
request and waited for a spawn that, in a quiet room, never came, so the
panel sat on "10 queued" indefinitely and looked broken.

Two fire modes:

- **Immediate** (default) replays from the physics pump, so a button does
  something now. It creates an entity part-way through a Havok step.
- **Piggyback** replays only when the game itself spawns — a context the game
  has just proved safe, but only as often as the game spawns.

## Reading the log

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
| `dt` climbing and `qray` climbing with it, `grays` flat | **H5, feedback spiral** — *refuted, E12* |
| `largestfree` falling over a session while `qavg` rises | **H6, address-space pressure** |
| `steps` (active objects) climbing over a session with `qray` | **H1+H4** |
| `denorm=` non-zero and rising while `qavg` climbs | **H7, FP state** — demoted |
| `qray` scaling with moving-body count; `C` line says CONTINUOUS | **H8, continuous collision detection** — the root cause; see `notes/russian-patch-analysis.md` |

Normal play, for comparison (measured over 20 minutes): `qray` p50 1306 /
p99 6177 / max 14417 per window, and at most 13.1ms of any 100ms window spent
in `queryRayOnTree`. Anything in that range is *not* the bug.

## The repro harness

Three sessions failed to provoke the stall by hand, and this environment
appears to suppress it (Proton serves d3d9 through DXVK, which the community
reports as a workaround). So provoke it deliberately.

H8 predicts the raycast volume comes from continuous collision detection
sweeping every *moving body* against the world. So push the moving-body
count up and `rays=` should climb with it, superlinearly once the frame
starts to lose.

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
HG_SPAWN_MULT=5 PROTON_USE_WINED3D=1 WINEDLLOVERRIDES="version=n,b" PROTON_LOG=1 ggm %command%
```

(`PROTON_USE_WINED3D=1` is for this repro only; the 1 FPS bug never
reproduced on DXVK, and DXVK is the everyday renderer. On wined3d the DLL
also has to force the engine's colour shadow map, because wined3d draws
nothing from the depth shadow map: see `bin\hellgate_shadowtype2.on/.off`
and the LOG entry of 2026-09-21 23:40.)

`X spawns prim=N script=N amplified=M panel=N owed=N mult=n` lines record
what it did, split by which hook saw the spawn and whether it came from the
multiplier or from the panel's buttons.

**This deliberately destabilises the game.** It is a diagnostic, never
shipped, and it is budgeted per window so a runaway cannot wedge the
process. Do not use a save you care about.

Once it reproduces, the same harness verifies any fix: same route, same
multiplier, compare `rays=`.

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
tools/      static-analysis scripts (findpat, callgraph, xref, tagmap, rtti);
            game-data tools: hgdat.py (dat/idx), hguncook.py (.xml.cooked),
            hgfx.py (.fxo effects), fxdis.c (shader disassembly under Wine)
tools/ghidra/  headless Ghidra scripts (NameFromAsserts, Decomp, Show, ...); see notes/codemap/
notes/codemap/ functions by source file, names recovered from the exe's asserts (`make codemap`)
notes/decomp/  decompiled functions of interest (`make decomp F="name ..."`)
notes/      binary facts and raw decompilation output
test/       trivial 32-bit host for offline load testing
ref/        augmentrex and MinHook checkouts (not ours)
```
