# Investigation log

Format per entry: **hypothesis → the log line that would confirm/refute → run → conclusion.**
Nothing here was learned by launching the game and watching. Phase 1 and 2 are
entirely static plus an offline load test.

---

## E1 — Does the Steam build carry anti-cheat or anything that makes hooking unsafe?

**Hypothesis.** A 2018 HanbitSoft single-player re-release has no anti-cheat.

**What would refute it.** An imported DLL, a section, or a sibling binary
belonging to a known protector (Themida, VMProtect, GameGuard, XIGNCODE,
EasyAntiCheat, BattlEye); or a packed/entropy-anomalous section layout.

**Run.** `objdump -x` on `Hellgate_sp_x86.exe`; directory listing of `bin/`.

**Conclusion — no anti-cheat.** Imports are 24 ordinary DLLs (see
`notes/exe-facts.md`). `dbghelp.dll` is present but that is a crash-dump
writer, not a protector. Sibling DLLs are all middleware: Granny 2, Bink,
Miles, FMOD, Umbra/dPVS occlusion. Sections are normal, unpacked.
**Hooking is safe.** Recorded sha256 the same day, as required.

---

## E2 — Which DLL should the proxy impersonate?

**Hypothesis.** One of the small, non-core imports is a safe proxy name.
`d3d9.dll` is out — DXVK owns it under Proton.

**What would decide it.** The import table.

**Run.** `objdump -x`, import directory.

**Conclusion — `version.dll`.** The exe imports exactly two functions from it,
`GetFileVersionInfoW` and `VerQueryValueW`. Tiny surface, no Proton component
owns it, loads early. `DINPUT8.dll` (one import, `DirectInput8Create`) is the
backup if anything about `version.dll` proves awkward.

---

## E3 — Does the binary carry symbols or RTTI? (determines Phase 1 effort)

**Hypothesis.** A release build will have nothing useful.

**What would confirm the optimistic case.** `.?AV` type-descriptor strings, or
retained `__FILE__` paths from asserts.

**Run.** `strings -a` over the exe.

**Conclusion — far better than hoped.** Three separate finds:

1. **~385 MSVC RTTI type descriptors** (`.?AVhkMoppBvTreeShape@@`, etc.).
2. **Retained Havok `__FILE__` assert paths**, including
   `.\collide\mopp\machine\hkMoppLongRayVirtualMachine.cpp`, and SDK paths of
   the form `C:\prime\3rd Party\Havok40\sdk\include\...`.
3. **Havok's own `HK_TIMER` name literals** — `TtRayCstCached`, `TtrcMopp`,
   `TtCapsCaps`, ~70 of them. Each is referenced by the single function it
   times, so xreffing them **labels Havok functions by name**. This is what
   turned the job from "hard" to "medium"; it is how `hkWorld`-level raycast
   entry was found without guessing. See `notes/triage-raw.txt`.

Also: PDB path `f:\P_2017\URC\release\Hellgate_sp_x86.pdb` (the .pdb itself is
not shipped). Game-side `__FILE__` paths survive only for the D3D9 layer
(`f:\p_2017\urc\source\engine\source\dxc\*.h`).

**Correction to the brief.** This is **Havok 4.0**, so classes are `hk*`, not
`hkp*` — the `hkp` prefix arrived in Havok 4.5/5.x. Augmentrex's writeup says
`hkpMoppLongRayVirtualMachine`; in this binary it is
`hkMoppLongRayVirtualMachine`. Havok is statically linked; no Havok DLL ships.

---

## E4 — Locate `queryRayOnTree` in *this* build

**Hypothesis.** Augmentrex's byte pattern still matches the current Steam build
(it should: no content patch since Dec 2018, buildid 3392971).

**What would refute it.** Zero matches, or more than one.

**Run.** `tools/findpat.py` — the pattern from
`Augmentrex.Commands.PatchLongRayVM/PatchLongRayVMCommand.cs`, wildcards
preserved, searched over the raw image.

**Conclusion — exactly one match.**
`hkMoppLongRayVirtualMachine::queryRayOnTree` at **VA 0x00870B10**
(RVA 0x00470B10). Relocations are stripped, so this address is fixed for the
life of the binary. Augmentrex's shortcut held.

---

## E5 — Walk upward from `queryRayOnTree` to game code

**Hypothesis.** A chain of xrefs reaches identifiable game-side callers.

**What would refute it.** The chain dissolves into virtual dispatch with no
recoverable owner.

**Run.** `tools/callgraph.py` + `tools/up.py` (direct `E8 rel32` xrefs and
absolute pointer scan over the whole image), then Ghidra 12.1.3 headless
(`tools/ghidra/*.java`) for function boundaries and decompilation.

**Conclusion — the chain reaches game code, then hits a function-pointer table.**

| VA | Identity | How |
|---|---|---|
| 0x00870B10 | `hkMoppLongRayVirtualMachine::queryRayOnTree` | augmentrex pattern |
| 0x00819ED0 / 0x00819F90 | `hkMoppBvTreeShape::castRay` (+collector variant) | `TtrcMopp` literal |
| 0x00841800 | `hkWorldRayCaster::castRay` | `TtRayCstCached` literal |
| 0x007FB9E0 | Havok-side world cast wrapper | only direct caller of the above |
| **0x005D30DF** | **the game's central world-raycast helper** | only direct caller of 0x7FB9E0 |
| 0x005D3208 | 77-byte gating thunk around it | only direct caller of 0x5D30DF |
| 0x0073292F | installs `0x5D3208` into a module-interface pointer table at `DAT_00EDFAC0` | data xref |

Above `DAT_00EDFAC0` the callers are **indirect** — the pointer is copied into
an interface struct and called through it. Static analysis stops here by
design; that is precisely what the Phase 2 stack backtrace is for.

**The important part.** `0x005D30DF` is a single choke point through which
every game world-raycast passes, and it is where the ray is *constructed*:

```
from = origin
to   = origin + dir * length     ; length is one scalar float from the caller
```

Verified at instruction level (`mulss` by `[ebp+0x10]` into each direction
component, `addss` onto origin), not just from the decompiler.

Recovered ABI:

```
FUN_005d30df(ecx, edx, const float *origin [ebp+08],
                       const float *dir    [ebp+0c],
                       float length        [ebp+10],
                       u32 a6 [ebp+14], u32 a7 [ebp+18])
```

`hkWorldRayCastInput` layout, recovered twice independently (once from
`hkWorldRayCaster::castRay`, once from the stack frame `0x5D30DF` builds):
`m_from` +0x00, `m_to` +0x10, `m_enableShapeCollectionFilter` +0x20,
`m_filterInfo` +0x24.

**This makes H2 a one-scalar hypothesis.** If `length` is garbage — inf, NaN,
or absurdly large — then `to` is garbage and the long-ray VM is asked to walk
the entire MOPP tree. One float, supplied by the caller, at one address.

---

## E6 — Calling conventions of the two hook targets

**Hypothesis.** Both are `__thiscall`; augmentrex declares `queryRayOnTree`
that way.

**What would refute it.** A stack-cleanup mismatch between callee epilogue and
call-site fixup.

**Run.** Disassembly of both epilogues and their call sites.

**Conclusion — they differ, and getting this wrong would corrupt the stack.**

- `queryRayOnTree` ends `ret 0x0c`, call site does no fixup → callee-cleaned
  thiscall. GCC `__fastcall` with five parameters emits exactly `ret 0x0c`.
  Augmentrex was right; the straightforward declaration works.
- `0x005D30DF` ends in a **bare `ret`** and its caller does `add esp, 0x14`
  → register args in ecx/edx but a **caller-cleaned** stack. MSVC does this
  for internal functions. GCC has no matching attribute, so declaring it
  `__fastcall` would emit `ret 0x14` and corrupt the caller's stack on every
  single raycast. The detour for it is therefore a hand-written asm shim
  (`detour_game_shim` in `src/hook.c`); generated code checked against intent.

---

## E7 — Does the proxy DLL load, forward, and refuse a wrong host?

**Hypothesis.** It does all three.

**What would confirm it.** Under a trivial 32-bit host: a real return value
from a forwarded export; a log line with the host's sha256; a refusal to hook.

**Run.** `build/host.exe` under plain Wine, `WINEDLLOVERRIDES="version=n,b"`.

**Conclusion — all three confirmed.**

- `VerLanguageNameA(0x0409)` returned `"English"` through our thunk into the
  real system `version.dll`. Forwarding works.
- Log recorded the host's actual sha256, compared it to the pinned one, and
  **refused to hook**, with a message box. The guard works.
- With `hellgate_rays.off` present: `kill switch active, not hooking`.

**Also learned.** Without `WINEDLLOVERRIDES`, Wine silently loads its *builtin*
`version.dll` and our DLL is never mapped at all — no error, no log, nothing.
The override is mandatory, not optional.

---

## E8 — What did HanbitSoft actually change between 2007 and 2018?

**Hypothesis (from the brief).** One of: Havok version bump, asset re-export,
effect-system change, or compiler/build change.

**What would discriminate.** Build timestamps and linker version of every PE
in the install. Middleware rebuilt alongside the exe would point at a
toolchain/SDK migration; middleware untouched would confine the change to
game code or game data.

**Run.** `objdump -x` over every `.exe`/`.dll` in the install. No disc, no
second build needed.

**Conclusion — the change is confined to game code or game data.**

| Binary | Built |
|---|---|
| `Hellgate_sp_x86.exe` | **2018-11-27** |
| `Hellgate.exe` (launcher) | **2018-09-20** |
| `granny2.dll` | 2006-06-16 |
| `dpvs.dll`, `mss32.dll`, `binkw32.dll` | 2007 |
| `fmodex.dll` | 2008 |
| `umbra.dll`, `umbrad.dll`, `D3DX9_42.dll` | 2009 |
| `steam_api.dll` | 2017 |

Only the game's own two executables were rebuilt. Every piece of middleware is
the original Flagship-era binary.

This eliminates two of the four candidates outright:

- **Not a Havok version bump.** Havok is statically linked, so a bump would
  have to be inside the exe — but the SDK paths say Havok 4.0, which is what
  the 2007 game shipped with. No evidence of a move off 4.0.
- **Not a compiler/build change.** The exe links **MSVC 8.0 (VS2005)** — the
  *original* era's toolchain, deliberately kept for a 2017/2018 rebuild.

That leaves **game code** or **asset/data change**, which is consistent with
E5 having landed on a single game-side scalar.

**Side note, not pursued.** The `data/*.dat` archives have magic `adgh`
version 4 and are readable, but the matching `.idx` indices are **encrypted**
(uniform high entropy, no recoverable path strings). The exe must hold the
key. Extracting effect definitions is a real avenue — it would show whether a
bad ray length is authored in data rather than computed in code, which would
also explain why lowering effect detail helps — but it is a side-quest, and
only worth opening if E9 points at data. (Note: `Augmentrex.Archive` is *not*
an archive reader despite the name; it is a release-zip packaging target.)

---

## E9 — First Proton session: clean baseline, and a refutation

**Hypothesis.** H2: the stall is caused by a garbage `length` scalar reaching
the game's world-raycast helper at `0x005D30DF`, producing an effectively
infinite ray that forces the long-ray VM to walk the whole MOPP tree.

**What would confirm it.** A `!` line in the log: a non-finite or absurd ray,
with the offending `length` printed verbatim, attributed to a specific caller.

**What would refute it.** Either no such ray during a stall, or — the case
that actually happened — evidence that `0x005D30DF` is not what drives
`queryRayOnTree` in the first place.

**Run.** ~20 minutes of ordinary play under Proton with both hooks live.
11,634 active windows logged. Raw log archived at
`notes/baseline-clean-session.log.gz`.

**Conclusion — three things, one of them a refutation.**

**1. The instrumentation is sound.** Correct binary (sha256 matched), both
hooks installed, and `CaptureStackBackTrace` produced clean, plausible RVAs.
Nine distinct game-raycast call sites resolved, with sane ray lengths
(1.0, 20.6, 35.3, 390.3 world units). **The frame-pointer-omission risk
flagged in E7 is closed — x86 stack walking works in this binary.**

**2. The bug did not reproduce.** Worst window spent 13.1ms of 100ms in
`queryRayOnTree`; no window exceeded 50%. A 1 FPS stall means ~1000ms frames.

Clean-play calibration (per 100ms window):

| | p50 | p90 | p99 | max |
|---|---|---|---|---|
| `queryRayOnTree` calls | 1306 | 3090 | 6177 | 14417 |
| ms in `queryRayOnTree` | 0.53 | 1.48 | 5.60 | 13.10 |
| game raycasts | 1 | 12 | 28 | 299 |

The original `SPIKE` threshold of 5000 calls was therefore **badly
miscalibrated** — it fired on 514 windows of entirely normal play. Replaced
with `qms > 33` (a third of the window spent in the MOPP tree) or
`qray > 50000`.

**3. H2 as framed is refuted, because the attribution hook is on the wrong
path.**

- Totals: **18,903,601** `queryRayOnTree` calls against **48,076** game
  raycasts — a ratio of **393:1**.
- **4,593 of 11,634 windows had zero game raycasts** while still issuing up
  to 12,895 `queryRayOnTree` calls each.
- Every ray that did pass through `0x005D30DF` was finite and short. No
  `nan`, no `huge`, not one `!` line in 20 minutes.

So ~99.7% of MOPP long-ray work does not originate from the game's
world-raycast helper, and a bad `length` at that call site cannot be the
cause of the stall.

**Where the E5 reasoning went wrong.** E5 correctly proved `0x005D30DF` is the
only path to `hkWorldRayCaster::castRay`. I then carried that over to "the only
path to the MOPP tree", which does not follow: `hkMoppBvTreeShape::castRay`
(0x819ED0 / 0x819F90) is also reachable from collision agents and from linear
casts — `hkSymmetricAgentLinearCast<hkMoppAgent>` is in the RTTI — which is
almost certainly character-controller sweeps running every frame. The static
call graph was right; the inference drawn from it was too broad.

**Action.** Sample stacks at `queryRayOnTree` itself rather than guessing
which Havok path feeds it. One call in 32 (power-of-two mask, ~400
captures/sec at baseline, ~6% overhead even at 100x that rate), aggregated
into the same site table and reported on `Q` lines with an estimated true
count. Stack depth raised 6 → 12, since the Havok chain above
`queryRayOnTree` is several frames deep before it reaches game code.

---

## E10 — The DXVK report, and why the Proton session was clean

**Trigger.** A community report: on Windows, dropping DXVK's 32-bit
`d3d9.dll` into `bin/` makes the 1 FPS bug stop happening.

**Question.** Does that invalidate testing under Proton?

**Run.** Read the Proton log from the E9 session.

**Conclusion — we were already running that fix, unknowingly, the whole time.**

```
info:  DXVK: v3.1-12-g8759acd15dc79c8
Loaded L"C:\windows\system32\d3d9.dll" at 77A50000: native
```

Proton 11.0-100 (Experimental) serves d3d9 through DXVK by default. So the
clean E9 session is *exactly what the community report predicts*, and "Nick
did not try hard enough" is the wrong reading of it. **Under stock Proton the
bug may not be reproducible at all.**

This does not invalidate the static analysis, which is renderer-independent.
It does invalidate the plan of reproducing the stall under stock Proton.

---

## E11 — H5: the variable-timestep feedback spiral

**The puzzle E10 creates.** Augmentrex profiled the stall into
`queryRayOnTree` and stubbing it restores the framerate, so the time really is
in Havok. How can swapping the *renderer* fix a *physics* stall?

**Run.** Locate `hkWorld::stepDeltaTime` (timer literal `TtStepDelta`,
0x007F92F0) and examine its one call site.

**Finding.** 0x0049A3EE, and there is **no loop and no fixed timestep**:

```
0x49a3e3   fld   dword [ebp + 8]     ; the frame delta, straight from the
0x49a3e6   push  ecx                 ; calling function's own parameter
0x49a3eb   fstp  dword [esp]
0x49a3ee   call  0x7f92f0
```

Havok is stepped **once per frame with whatever the frame took**.

**H5.** A long frame — from *any* cause, including a native-d3d9 hitch —
produces a large `delta`. A large delta makes every swept body travel further
in a single step. Longer sweeps make Havok's internal linear casts
(`hkSymmetricAgentLinearCast<hkMoppAgent>`, present in the RTTI) walk far more
of the MOPP tree. That makes the frame longer, which makes the next delta
larger. Positive feedback, locking at ~1 FPS.

**What H5 explains that H2 could not:**

| Observation | H5 |
|---|---|
| Time genuinely in `queryRayOnTree` (augmentrex) | yes — the casts really are expensive |
| Stubbing `queryRayOnTree` fixes it | yes — it breaks the feedback loop |
| **DXVK fixes it** | yes — removes the trigger, so the spiral never starts |
| Explosions / large outdoor zones | frame spikes are the trigger |
| Lowering effect detail helps | fewer frame spikes |
| Onset after ~2 hours | more entities → more sweeps → lower tipping point |
| **E9: 393:1 MOPP work vs game raycasts** | yes — the work is agent sweeps, not game raycasts |
| **E9: no malformed ray on the game path** | yes — the long rays are Havok-internal |

Note H5 is still a ray-*length* story, as H2 was. H2 was not wrong about the
mechanism, it was wrong about the location: the long rays are generated inside
Havok by large sweeps, not handed in by the game's raycast helper.

**Status: unverified.** But unlike H2 it is consistent with the DXVK result
rather than contradicted by it.

**What would confirm it.** During a stall: `dt` climbing, `qray` climbing with
it, and `grays` staying flat. Refuted if `dt` stays small while `qray` spikes.

**Instrumented.** Third hook on `hkWorld::stepDeltaTime`. `W` lines now carry
`steps=` and `dt=[min/mean/max]ms`, and a window is marked `SPIKE` if any
single step exceeded 100ms.

**If H5 holds, the fix is small and safe.** Clamp the delta at the call site
to a sane ceiling (~1/20s) — standard max-frame-time clamping, which every
modern engine does. It breaks the feedback loop without touching raycasts at
all: AI line-of-sight intact, Ash and Oculis still killable, physics merely
slows briefly instead of exploding. Strictly better than stubbing
`queryRayOnTree`, and it fixes the cause rather than removing the trigger the
way the DXVK workaround does.

---

## E12 — wined3d session: H5 refuted, and an instrumentation bug found

**Run.** ~32 minutes under `PROTON_USE_WINED3D=1` (renderer confirmed builtin,
no DXVK banner; our proxy confirmed loading `native` in the game process).
19,370 active windows. Archived at `notes/wined3d-session-noQ.log.gz`.
The stall did not occur, but the data settles H5 anyway.

**H5 predicted:** large frame delta → longer sweeps → more MOPP work.

**Observed: the exact opposite.**

Large deltas produce *no* MOPP work — these are loading screens:

| window | dtmax | qray |
|---|---|---|
| W781 | **479 ms** | **0** |
| W821 | 417 ms | 6257 |
| W1610 | 251 ms | 706 |
| W780 | 117 ms | **0** |

Heavy MOPP work happens at entirely *normal* deltas:

| window | qms | qray | dtmax |
|---|---|---|---|
| W3248 | 35.30 | 20008 | **8 ms** |
| W3240 | 35.20 | 20045 | **7 ms** |
| W3734 | 34.30 | 34192 | **8 ms** |
| W3238 | 33.41 | 24710 | **9 ms** |

And bucketing MOPP calls per step by frame delta shows the relationship going
the wrong way: 5.9 calls/step at 0–5ms, 5.0 at 5–10ms, 3.8 at 10–15ms.

**Conclusion: H5 is refuted.** Frame delta does not drive MOPP work. Like H2,
it died on its own predicted signature. The DXVK observation still needs an
explanation, but the variable-timestep spiral is not it.

**Correction: `0x007F92F0` was mislabelled.** It was called
`hkWorld::stepDeltaTime` on the strength of its `TtStepDelta` timer literal.
Measured, it runs ~960 times per 100ms window — roughly 45 calls per frame —
so it is a **per-object** step, not the once-per-frame world step. The float
it receives does behave like a real frame delta, but the counter means
"physics objects updated", not "steps taken". Renamed.

**Instrumentation bug: every `Q` stack came back empty.** 19,338 `Q` lines
were written and every one had `site=` blank, so the attribution added in E9 —
the entire point of that change — produced nothing.

Cause: x86 has no unwind tables, so `CaptureStackBackTrace` walks the EBP
chain. At `-O2` GCC omits the frame pointer and reuses EBP as a scratch
register inside `detour_query`:

```
sub    $0x6c,%esp
mov    %ebp,0x68(%esp)     ; EBP saved as a general register
mov    0x78(%esp),%ebp     ; and reused
```

That destroys the chain before the capture runs. The *game* raycast hook was
unaffected because its detour is hand-written asm that sets up `ebp`
explicitly — which is why E9's `site=` frames looked fine and masked the
problem. Fixed by adding `-fno-omit-frame-pointer`; the flag is load-bearing
here, not a debug nicety.

**Not wasted, though.** The session gives a wined3d baseline and rules out
H5. Ordinary-play figures under wined3d, for comparison with the DXVK
baseline in E9 (different areas, so not a controlled comparison):

| per 100ms window | p50 | p90 | p99 | max |
|---|---|---|---|---|
| `qray` | 5581 | 10437 | 15275 | 34192 |
| `qms` | 3.93 | 7.73 | 16.18 | 35.30 |
| objects stepped | 960 | 1472 | 1850 | 2867 |

One signal worth carrying forward: in the heaviest windows `qavg` rises from
~0.6µs to 1.4–2.0µs. Per-ray cost roughly triples while count also triples.
That is a hint of **H3** (cost per ray, degenerate MOPP) layered on top of
raw volume — and it is exactly what the now-working `Q` lines should resolve.

---

## Note — alexrp's own characterisation

Quoted from the augmentrex author:

> The game makes an excessive **number** of ray cast queries under certain
> circumstances. Disabling ray casting altogether makes the game playable.

He is the only person who has observed the bug with tooling, so this raises
**H1 (count)** relative to H3/H6 (cost per query).

Held lightly, for one reason: a sampling profiler shows `queryRayOnTree`
dominating the frame whether it is called a million times cheaply or a
thousand times expensively. "Excessive number" may be the natural reading of
a hot function rather than a measured call count. He was not looking for the
cause, only for a way to stop the bleeding.

We do not have to settle this from quotes. The log records the two
separately — `qray` is count, `qavg` is per-call cost — and in the heaviest
windows captured so far (E12) *both* rose roughly 3x.

**Revised lead: H1 fed by H4.** Entities accumulating across a session
(debris, corpses, effects never freed), each doing character sweeps against
the MOPP tree every frame. No single caller misbehaving, just steadily more
of them. That fits "excessive number", fits "certain circumstances" being
explosions in open zones, and fits the ~2-hour onset reports. It does **not**
explain the DXVK result, which stays an open loose end rather than something
to bend the theory around.

Conveniently this changes nothing about the next run: `Q` stacks say who
issues the queries, and `steps=` already counts active physics objects per
frame. If object count climbs across a long session with `qray` climbing
alongside it, H1+H4 is confirmed.

---

## H7 — floating-point state (current lead)

**Trigger.** A Steam forum report:

> I had an intel processor with a Nvidia GPU card and it was unplayable due
> to the 1 FPS bug. I just got an AMD processor and GPU — not a single
> instance of 1FPS. […] it seems to play this version on AMD or play the
> original (modded) on intel/Nvidia

Combined with the DXVK report, that is **two** independent environmental
dependencies. No hypothesis so far explains either, let alone both. A pure
game-logic bug (H1/H4) should not care what CPU it runs on.

**H7.** Two well-known facts about this era of code:

1. `IDirect3D9::CreateDevice`, without `D3DCREATE_FPU_PRESERVE`, reprograms
   the **x87 control word to single precision**. This is a notorious source
   of D3D9-era numerical bugs, and DXVK's d3d9 does not necessarily
   reproduce native d3d9's behaviour here.
2. **Denormal** floating-point values carry a severe penalty on Intel
   (microcode assist, often 100+ cycles for a single operation), and AMD's
   handling differs.

If the MOPP traversal's arithmetic lands in denormal territory, every ray
gets dramatically more expensive — on Intel, and not on AMD — and the
trigger for entering that regime is the FPU state the renderer left behind.

**What H7 explains that nothing else does:**

| Observation | H7 |
|---|---|
| DXVK fixes it | different FPU control word after device creation |
| **Intel/Nvidia affected, AMD not** | denormal penalties are CPU-vendor specific |
| Time concentrated in `queryRayOnTree` | it is the arithmetic loop |
| **E12: `qavg` tripled while rays looked normal** | per-operation slowdown, not more work |
| "certain circumstances" | specific geometry producing near-zero intermediates |
| Lower effect detail helps | fewer rays, so the penalty is paid less often |

Note this also rehabilitates **H3** — cost per ray — but supplies the
mechanism H3 was missing. It does not fit alexrp's "excessive *number*",
though as noted above a sampling profiler cannot distinguish count from cost.

**Test machine is in the affected class:** Intel Core Ultra 9 275HX +
RTX 5080. So the bug should be reproducible here.

**Instrumented.** MXCSR's exception-status bits are sticky, so a plain read
only says "at some point, yes". To get a *rate*, the sampled path clears the
status bits, runs the call, reads them back, then restores the original
sticky bits exactly. Only status bits are touched, never a control bit, so
the game's own FP behaviour is unchanged. New `F` line reports the denormal
rate, MXCSR with FTZ/DAZ decoded, and the x87 control word with its
precision field decoded.

**What would confirm it.** A non-trivial denormal rate on the physics
thread, rising in the expensive windows. Also: `x87cw` showing `single(24)`
precision under native d3d9.

**What would refute it.** Denormal rate at or near zero while `qavg` climbs.

**If H7 holds the fix is very small.** Set FTZ and DAZ in MXCSR around the
MOPP traversal — denormals flush to zero, the penalty disappears, and the
accuracy cost is irrelevant for collision detection at these magnitudes. No
raycasts disabled, no gameplay changed. Alternatively restore a sane x87
control word. Either is far less invasive than anything previously
considered.

---

## H8 — Continuous collision detection (ROOT CAUSE, strongly supported)

A third-party patched executable ("2026 fix") was supplied for analysis.
Full teardown: **`notes/russian-patch-analysis.md`**. It is a byte patch of
our exact binary — 9 patches, 1800 bytes, and **nothing at `queryRayOnTree`**.

The load-bearing one is a single byte in `hkWorldCinfo::hkWorldCinfo()`:

```
0x82454e   mov   cl, 2
0x824559   mov   byte [eax + 0x95], cl     ; patched to constant 1
```

`+0x95` is `m_simulationType`. Havok 4.0:
`INVALID=0, DISCRETE=1, CONTINUOUS=2, MULTITHREADED=3`.

**The game runs Havok in CONTINUOUS simulation.** Continuous simulation
sweeps every moving body against the world every step to prevent tunnelling,
and each sweep against level geometry is a linear cast into the MOPP tree —
`hkSymmetricAgentLinearCast<hkMoppAgent>` → `hkMoppBvTreeShape::castRay` →
`queryRayOnTree`. Raycast volume therefore scales with the number of moving
bodies, not with anything the game explicitly requests.

**This retro-explains our own measurements**, taken before we had any idea
of the cause:

| Measured (E9 / E12) | Under CCD |
|---|---|
| 393:1 `queryRayOnTree` vs game world raycasts | the work is per-body sweeps |
| thousands of queries in windows with **zero** game raycasts | sweeps need no game query |
| **~5.9 MOPP queries per physics object per step** | textbook swept-collision |
| never a malformed ray on the game path | the long rays are Havok-internal |

It also matches alexrp's "excessive **number** of ray cast queries under
certain circumstances" — the circumstance being many simultaneously moving
bodies, i.e. explosions in large outdoor zones. Lower effect detail spawns
fewer. The ~2-hour onset fits bodies accumulating.

**Status.** Strongly supported, not yet confirmed on this machine. The
enum-value inference is solid but inferred; `hkContinuousSimulation` and
`hkSymmetricAgentLinearCast<hkMoppAgent>` in the RTTI corroborate it.

**Hypothesis scoreboard.** H2 refuted (E9). H5 refuted (E12). H6 and H7
untested and now demoted — H8 explains the evidence they were invented to
explain, with a mechanism, and without needing the FP or address-space
stories. H1 is essentially subsumed: the count really is excessive, and CCD
is why. H3/H4 fold in as amplifiers.

---

## E13 — confirm causation by A/B (ready to run)

Fourth hook added on `hkWorldCinfo::hkWorldCinfo` (0x00824480). It logs a
`C` line with the observed `m_simulationType`, and `HG_SIM_TYPE=n` overrides
it at runtime — reproducing the 2026 fix without patching the binary.

Run the **same route twice**:

1. stock — expect `C ... simulationType=2 (CONTINUOUS)`
2. `HG_SIM_TYPE=1` — expect DISCRETE

If `qray` collapses in run 2, causation is established. If it does not, H8
is wrong and the byte does something else.

This override is an **experiment, not a fix**: global DISCRETE removes
tunnelling protection from everything, which is exactly the tradeoff the
2026 fix accepted.

---

## Next: E14 — a fix that does not trade tunnelling for framerate

Havok has per-body `hkCollidableQualityType`; `DEBRIS` skips most CCD work
while `CRITICAL`/`MOVING` keep it. Cosmetic debris does not need tunnelling
protection, a rocket does. The stock binary contains an `objectQualityType`
string, so quality may already be authored per object type — if it is
reachable through data, that is the cleanest fix available and needs no code
patch at all. Otherwise, set the quality type on bodies at creation.

Either way it is narrower than the 2026 fix, and unlike augmentrex's stub it
disables no raycasting, so AI line-of-sight and boss mechanics are untouched.

---

## E13 — Working stack attribution. H8 premise confirmed; H7 split; a counting error found

~50 minutes under wined3d. No stall again (consistent with this env
suppressing it). 68MB log, 529,617 working `Q` lines. Archived at
`notes/wined3d-session-Q.log.gz`.

**1. H8's premise is confirmed in the live game.** All 35 Havok worlds:

```
C hkWorldCinfo at 0168d3a0 simulationType=2 (CONTINUOUS)
```

**2. H7 splits — half confirmed, half refuted.**

```
F tid=416 sampled=802 denorm=0 (0.0%) mxcsr=1f80 FTZ=0 DAZ=0 x87cw=007f pc=single(24)
```

`pc=single(24)` — **D3D9 did clobber the x87 control word to single
precision**, exactly as H7 predicted. But `denorm=0` in every window, so
the denormal-penalty mechanism is **refuted**. Precision is reduced; nothing
pays a microcode penalty for it. Reduced precision may still matter
numerically, but it is not the performance mechanism proposed.

**3. `queryRayOnTree` is recursive — and this invalidates a number I used.**

The captured stacks show return addresses 0x00471087 and 0x004712CE —
both *inside `queryRayOnTree` itself* — repeating up to three times per
stack, with our MinHook trampoline between levels:

```
  004712ce  <  !786ca51e  <  004712ce  <  !786ca51e  <  00471e35  <  00419f3d
```

The MOPP VM descends the tree by recursing. **A `queryRayOnTree` call is a
tree-node visit, not a ray.**

**Correction.** In the H8 write-up I cited "~5.9 MOPP queries per physics
object per step" as looking like textbook swept collision, and the "393:1
ratio" as evidence the work was per-body sweeps. Both numbers count *node
visits*, not rays, so neither supports what I claimed. H8's premise stands
on the confirmed `C` line and the RTTI; that particular supporting evidence
does not, and is withdrawn.

**4. Where the stacks terminate.** 94.6% of node visits bottom out at
`0x00419F3D`, inside `hkMoppBvTreeShape::castRay`, with no frames above —
that function does not preserve EBP, so the walk stops. But 1.55% of
samples do reach game code, giving a complete chain:

```
00471fac  queryRayOnTree return
0041a010
004070dd  hkPhantom::castRay region (TtrcPhantom, 0x806F40)
000e9b78  GAME
000ed6d9  GAME
000ec8cf  GAME
0009ac85  GAME
0009b972  GAME   <-- adjacent to 0x0049B986, the call the 2026 fix NOPs out
00038500  GAME
```

**Action.** Hook the ray-level entry points directly —
`hkMoppBvTreeShape::castRay` (0x819ED0) and its collector variant
(0x819F90), both `ret 0x0c` thiscall. One call there is one real ray, the
volume is far lower so every call can be captured, and the stack is taken
at a point whose caller chain is intact. New `R` lines; `W` lines now carry
`rays=` and `nodes/ray=`.

That finally measures the thing every hypothesis has been arguing about:
**how many rays, and who asks for them.**

---

## E14 — The A/B, and the body-count correlation

Three runs, same level and same activity each time, ~3 minutes each.
Archived as `notes/run_{corr,a_continuous,b_discrete}.log.gz`.
Run B confirmed `C ... simulationType=2 (CONTINUOUS)` then `overridden to 1`.

### Result 1 — turning CCD off roughly halves the MOPP work

Medians over active windows (`steps>50`):

| | A: CONTINUOUS | B: DISCRETE | change |
|---|---|---|---|
| `qms` (ms/100ms in the tree) | 3.9 | 2.0 | **−49%** |
| `qray` (node visits) | 6977 | 4044 | **−42%** |
| `rays` | 1466 | 1108 | −24% |
| nodes per ray | 4.76 | 3.65 | −23% |
| `qavg` (µs per node) | 0.5 | 0.5 | unchanged |
| `bodies` | 627 | **947** | +51% |
| `steps` | 816 | 828 | ~equal |

Run B did *more* simulation work with *more* bodies and still halved the
MOPP cost. Per body the drop is 53% (2.64 → 1.25 rays/body).

The mechanism is visible in the numbers: CCD produces both **more** rays
and **longer** ones. Swept casts follow a body's motion path, so they walk
more of the tree — hence nodes-per-ray falling 23% alongside ray count. Cost
per node is unchanged, as expected, since the tree itself did not change.

**H8 is confirmed as a major contributor.** Continuous collision detection
is responsible for roughly half the MOPP work.

### Result 2 — but ray volume does *not* track body count

Over 2601 active windows in the correlation run:

```
corr(bodies, rays) = +0.089      (i.e. none)
corr(bodies, qray) = -0.150
corr(steps,  rays) = +0.339
```

| bodies | median rays | median qms |
|---|---|---|
| 100–199 | 1478 | 5.10 |
| 500–599 | 1266 | 5.41 |
| 800–899 | 1231 | 3.66 |

Flat, or faintly negative. **H8's specific prediction — that ray volume
scales with the number of bodies — is refuted.**

The reconciliation is that *total* body count is the wrong variable. Most
of those 600–900 entities are static or deactivated: walls, props, sleeping
objects. Only actively simulated bodies sweep. `steps` (per-object physics
updates) is the better proxy and does correlate, at +0.339 — moderate, not
strong. My measurement was wrong, which is not the same as H8 being wrong,
but the clean scaling law H8 predicted is not there.

### The uncomfortable conclusion

**A 2x effect is not a 1 FPS bug.** Going from 60fps to 1fps needs
something that explodes by ~60x. CCD costs a factor of two.

Neither run came close to stalling — `qms` median 3.9ms of a 100ms window
is about 4% of one core, nowhere near pathological. So everything measured
here describes the *healthy* regime, and we still have no measurement of
the pathological one.

That reframes all three known workarounds. Augmentrex's stub, the DXVK
swap, and the 2026 fix's DISCRETE switch may each be **reducing load below
a tipping point rather than removing a cause**. That would explain why
three unrelated-looking changes all "work", why the bug is
hardware-dependent, and why it takes ~2 hours to appear. Something makes
the system tip; we have never observed it tipping.

**Where that leaves us.** We have a real, shippable *improvement* — per-body
CCD demotion, roughly halving MOPP work with no tunnelling risk for
gameplay bodies — but not a proven root-cause fix. Those should not be
conflated, and the fix should not be described as curing the 1 FPS bug
until someone who can reproduce it confirms that it does.

---

## Tooling — the spawn button never worked, and why

Recorded because it was a design error, not a coding one, and the shape of
it is worth not repeating.

**Symptom.** The panel's spawn control did nothing. It had never once
produced a spawn across any session.

**Cause.** It was not a spawn command. `panel_burst(n)` set a counter, and
the counter was only ever drained *inside the spawn detours* — that is, it
rode along with a spawn the game was already performing. In a quiet room the
game spawns nothing, so the counter sat there and the panel reported "10
queued" indefinitely. It was a multiplier with no input, presented as a
button.

The old `X spawns=` log line would have said so, and it appears in **no**
session log in `notes/` — because the spawn hooks are only installed when
the harness or the panel is on, and neither was on for any recorded run. So
there was also never any evidence that the spawn primitive fires at all.

**Fix.** Record rather than ride. The detours now capture every argument of
the last real spawn — all thirteen dwords of the primitive at `0x0061c8f1`,
or the single context pointer of a script action — and the panel replays
that exact call on demand from the pump, on the game thread. One real spawn
anywhere in the zone arms the buttons.

The limit is unchanged and unavoidable: the context cannot be synthesised,
so until the game spawns once there is nothing to replay. What changed is
that the panel now *says* which state it is in — hooks installed or not,
calls seen per hook, template captured or not, and the thread ids of the
capture and the pump — so "nothing happened" always resolves to a reason.
A silent no-op is a worse bug than a loud failure.

**Also worth noting:** replaying from the pump means calling a game spawn
from inside the Havok step, which is not obviously safe. Piggyback mode is
kept as the fallback and is one click away, and the trade-off is stated on
the tab rather than buried.

## Tooling — the dev panel is now a panel

Tabs and clickable buttons instead of a fixed text dump: Live, Player,
Memory, Spawn, Physics, Log. Mouse-driven, draggable, with `ctrl`+key
equivalents throughout because the click also reaches the game and exclusive
fullscreen can pin the cursor.

Two things in it are directly useful to this investigation rather than to
cheating:

- **Live** now graphs `qms` and frame time over the last ~6 seconds. The
  whole question is an excursion in those two numbers and we have never
  watched one arrive.
- **Memory** diffs against a marked baseline. Mark, take a hit, and the
  bytes that moved light up — which is the entire method for recovering the
  unit offsets that E-series work keeps needing.

The layout and hit testing live in `src/ui.c` and `src/panel_ui.c`, which
have no D3D and no `windows.h`, and `test/ui.c` builds every tab natively
and clicks it. `make test` answers in a second; the alternative is a Proton
launch and a walk to somewhere interesting per iteration. `./build/uitest
--dump` renders each tab as text for the same reason.

That split is also what the tests are for. "Fire 10 queues ten spawns" is a
claim about wiring, and wiring is precisely what broke silently last time.

---

## E15 — 75 minutes, 10x the bodies, and the scaling law is dead

**Run.** Longest session so far: 44,923 windows, 44,702 of them with Havok
actually stepping — 74.5 minutes of play. No 1 FPS stall. Body count ranged
from 25 to a peak of 1,927, a ~10x span within one run, which is the widest
range of H8's independent variable we have ever had.

**The game was healthy throughout.**

| | |
|---|---|
| frame time p50 / p90 / p99 / p99.9 | 4.3 / 5.6 / 7.6 / 11.2 ms |
| windows averaging worse than 60 FPS | 20 of 44,702 (0.04%) |
| MOPP share of wall time | 3.8% |
| mean MOPP nodes / rays per window | 7,783 / 1,355 |

The `dt` outliers (51,044 ms at window 32695, 16,521 ms at 1127) are load
screens, not frames: they are single steps handed a giant delta, and the
window either side of them is normal.

**H8's scaling law is refuted, this time properly.** Binning every active
window by body count:

| bodies | windows | mopp ms | rays/win | frame ms | µs mopp per body |
|---|---|---|---|---|---|
| 0–199 | 1,944 | 2.57 | 837 | 12.87 | 23.51 |
| 200–399 | 8,684 | 4.12 | 1,235 | 5.64 | 13.06 |
| 400–599 | 7,363 | 4.05 | 1,080 | 4.34 | 7.45 |
| 600–799 | 6,690 | 3.75 | 2,155 | 6.93 | 5.77 |
| 1200–1399 | 2,093 | 4.16 | 1,167 | 28.71 | 3.36 |
| 1400–1599 | 4,178 | 4.53 | 1,024 | 4.58 | 2.84 |
| 1600–1799 | 7,458 | 3.58 | 1,523 | 4.81 | 2.07 |
| 1800–1999 | 5,436 | 2.99 | 1,303 | 5.23 | 1.59 |

MOPP cost is **flat** across a tenfold increase in bodies — lowest, in fact,
at the highest body counts. Cost per body falls monotonically by 15x. The
correlation is explicit:

```
rays vs bodies        r = +0.018
rays vs steps/window  r = +0.273
```

+0.018 is a flat line. Adding a physics body does not add raycast work in
any measurable amount. E14 suspected this from a much narrower range; it is
now settled. **Body count is not the independent variable, and the repro
harness built to raise it is aimed at the wrong quantity.** (The 28.71 ms in
the 1200–1399 row is the 51-second load screen landing in that bin, not a
property of that body count.)

**Where the rays do come from.** Game-side raycasts are a rounding error:

```
mean game raycasts (grays)  24.6 / window
mean real rays (castRay)    1355.1 / window
game-side share             1.82%
```

98% of all rays originate **inside Havok**, during the step. The dominant
stack (31,652 samples) returns to `0x008417e3`, which is the instruction
after `call *%edx` where `edx` is vtable slot 6 of a collision object — a
Havok shape-type virtual dispatch, matching the dispatch table already
recorded at `0x009AB62C`. The second stack (15,096 samples) descends through
`0x0049AC85` / `0x0049B972`, adjacent to the known physics-step call site at
`0x0049A3EE`. Both are the collision agent path, which is exactly what
`hkSymmetricAgentLinearCast<hkMoppAgent>` doing CCD sweeps looks like, and
consistent with E14's finding that CCD is about half the MOPP cost.

**Caveat on that attribution, and it matters.** The innermost frame of every
sampled stack is `!786aa5de` or `!786aa6be` — the `!` means outside the game
image, and `0x786a....` is inside `d3d9.dll` (the overlay logged DXVK's
vtable at `0x78028520`). d3d9 does not call Havok. This is the
frame-pointer-omission risk recorded under "Open risk" below finally biting:
the EBP walk picks up stale stack contents for the first frame. The
*remaining* frames are plausible `.text` RVAs and agree with each other
across 47,000 samples, so they are probably genuine — but "probably" is
doing work, and the first frame is definitely junk. Stack attribution here
should be treated as corroborating, not as proof.

**What this leaves.** The stall still has not been observed. What has been
measured is that the two mechanisms we can drive — body count and game-side
raycast volume — are not what produces MOPP load in a healthy session. The
load is internal to Havok's collision step and roughly constant. Whatever
tips the system into the pathological regime does not appear to be "more
bodies", which removes the most intuitive candidate and the harness built
for it.

---

## Next: E16 — measure the pathological regime, or accept the improvement

Two honest options:

1. **Get the bug observed.** Either from someone who reproduces it
   reliably (Intel/Nvidia, native d3d9, long session), or by finding what
   tips the system. Until then no fix can be verified against the actual
   bug.
2. **Ship the improvement on its own terms.** Halving physics raycast cost
   is worth having, is low-risk when scoped per-body, and is honest about
   what it does and does not claim.

Stock Proton gives DXVK, which appears to suppress the bug. To study it we
must first *cause* it. Force the slower, hitchier renderer path:

```
PROTON_USE_WINED3D=1 WINEDLLOVERRIDES="version=n,b" ... %command%
```

If H5 is right, a hitchier renderer should make the spiral **easier** to
trigger, not harder. A stall under wined3d that does not occur under DXVK,
with `dt` and `qray` climbing together, confirms H5 on this machine without
needing a Windows install.

Not yet run. See `README.md` for the exact procedure. Baseline **without** the
DLL first, to confirm the bug reproduces under Proton at all and to record the
frametime signature.

### Open risk, to check on the first real run

x86 has no unwind tables, so `CaptureStackBackTrace` walks the EBP chain. If
the game was built with frame-pointer omission, the `site=` frames in the log
will be garbage. Both `0x5D30DF` and `0x5D3208` do use EBP frames, which is
encouraging, but it is not proof for their callers. **Tell-tale:** `site=`
values that are not plausible `.text` RVAs (the image runs to 0x00E82000), or
every site collapsing to one bogus chain. **Fallback if so:** hook the
interface-table copy sites around `0x00734F51` / `0x007408B3` to recover which
subsystem holds each copy of the pointer, and discriminate that way.

### Open question still outstanding

Can you get the original 2007 Flagship client (retail DVD, or the
London 2038 / Revival community client)? Diffing its Havok version and effect
code against this one would shortcut the remaining work considerably. Nothing
below depends on it, but it would make E9 much cheaper.

## Graphics — step 0 groundwork (2026-09-21)

The brief is `notes/graphics-plan.md`; this is what its step 0 turned up
before the first instrumented run.

**Data.** Reanimator-steam has no Mono here, so its cooked-XML reader was
ported: `tools/hguncook.py`, stdlib only. The format turned out to be
self-describing (each file carries its element table with hashes, types and
defaults), so the port only needed Reanimator for the hash→name table. All
1,168 extracted light / environment / screenfx / particle / skill / material
files parse to EOF. `notes/data-tunables.md` records the fields.

**Shaders.** `tools/hgfx.py` parses the fx_2_0 binaries (layout from Wine's
`effect.c`) and `build/fxdis.exe` disassembles the embedded SM1–3 blobs with
Wine's own d3dx9_43 — the game's D3DX9_42 returns S_OK and no buffer under
Wine. 120/120 D3D9 effects parse cleanly. The finding that reshapes step 1:
point lights are summed in the *vertex* shader and only the directional
lights, the camera light and the specular term are per pixel; and the SM3
outdoor-actor effect has no point-light techniques at all (`PointLights=0`
in all 240), so the player is lit by spells only through SH. Backgrounds
have 0/3/5-light techniques, indoor actors 0/2. `_ZBuffer.fxo` writes an
alpha-modulated per-object constant, not depth. `notes/shaders.md`.

**Probe.** `src/gfxprobe.c` hooks `D3DXCreateEffect(Ex)` in the prefix's
native d3dx9_34 (identifies blobs by size + FNV-1a against the generated
`src/fxtable.h`), `ID3DXEffect::SetTechnique` (per-technique counts),
`CreateFileW` (loose-file probes), and five device methods for a one-frame
render-target trace; it also runs `CheckDeviceFormat` for INTZ and friends.
Effects can be replaced from `<game>\override\<pak path>`; an identical
`particle.fxo` is staged as the harmless first test. Deployed 18:24:53, not
yet run. Vtable slots checked against the mingw headers: SetTechnique 58,
SetRenderTarget 37, SetDepthStencilSurface 39, DrawPrimitive 81,
DrawIndexedPrimitive 82, Clear 43.

**Also asked this session:** DX11 / 64-bit. The exe is 32-bit MSVC8 with
Havok 4.0 statically linked and imports d3d9 only; the DX10 renderer and an
x64 build existed for the MP client (`MP_x64\hellgate_mp_dx10_x64.exe` in
the strings) but are not in this SP binary. Neither is reachable from a DLL;
they would mean a source-level or full-decompile rebuild. Everything in the
graphics plan fits inside D3D9 SM3 as it stands. The stated end goal is a
"Hellgate London 2.0" that installs over the Steam build (memory:
`project-goal-hellgate-2`).

## Animation pass — first in-game report: regressions, shield snap not fixed

The user's verdict on the 2026-09-21 animation pass (`src/animfix.c` stance
ease, seam inertialization, phase match; `src/animwatch.c` ease-out floor):
**it introduced bugs and did not fix the Guardian's shield snap on the run
cycle.** Which bugs is not yet described.

Response: all four modifications now default **off**. The hooks stay in as
passthroughs, so the panel's Anim tab (Stance ease / Ease-out floor / Seam
smoothing / Phase match, with per-fix counters) can turn each on for an A/B
against stock behaviour without a restart. Deployed 18:33:49.

One thing the logs already say: the `anim: JUMP tm_3p_idle.hkx 0.03 -> 0.00`
flood is not a regression — the pre-pass session (`hellgate_rays.log.1`) has
1,104 of them to the post-pass session's 674. The game rewinds the idle's
playhead itself.

The user's description of the regressions, in their words: "some animations
getting floaty or weird, if you hit shift for the sprint the legs would go
weird", and "animations stopping randomly". Floaty motion is what the seam
inertialization does when it captures an offset it should not (every
weight change was treated as an event); legs going wrong on the sprint
switch is the phase match putting a cycle of a different length at the
wrong time; animations stopping fits the stance ease holding a control the
game meant to drop to zero. None of the three is re-enabled.

The shield problem is not a blend snap: "it is a walking animation bug. the
shield has a frame where it angles backwards" — one bad key in the walk
asset. None of the four fixes could touch that. Added fix 4 in
`src/animfix.c`, logging only and on by default: after every sample, each
bone's local rotation is compared with the previous frame and a jump far
beyond that bone's usual motion is logged as
`animfix: spike bone <i> <name> <deg> | <file>@<t>/<dur> w<weight> ...`
for the player's skeleton only (rate-limited to 40 lines per 5 s). One walk
with the shield pins the bone and the local-time window; the fix is then a
per-bone re-sample of that control just outside the window, which needs
none of the blend machinery. Panel: Anim tab, "Spike log".

## Graphics step 0 — first instrumented run (18:39), and the shield hunt

Graphics answers are in `notes/graphics-plan.md` ("First instrumented run"):
SM3 tier confirmed by name; the `override\` substitution worked on a
byte-identical `particle.fxo` (game unchanged, `OVERRIDE` logged twice — the
engine creates every effect twice back to back, #18/#19 for `ui.fxo`, not a
Reset); the engine opened only `serverlist.xml` loose, so the archive is the
only data source and route B is dead; INTZ is supported under wined3d but the
scene is 4x MSAA, which blocks a straight depth read. Technique counts match
the shader reading exactly: outdoors the player's 3,844 technique sets were
all `TActor_00_…` (zero point lights).

The spike logger resolved bone names (`ThighLf`, `HandLf`, `PauldronHingeRt1`
…) and found 74 spikes, all of them transitions (new controls at t≈0.005 at
full weight) or run-cycle wraps on the *right* arm at t≈0.004 while
`tm_3p_JumpRecRun.hkx` sat at weight 1.00 on its last frame — that control
looped twice at full weight during the run and was only removed 6 s later
with a 0 s ease-out, a stock quirk worth remembering. **No left-arm spike
inside the run cycle**, so the shield's bad frame is not in the player's
sampled pose at 20°+. Hypothesis: the shield is a separate model with its own
hkAnimatedSkeleton, whose controls animwatch never traced and which the
logger's player-only filter therefore dropped. Build 18:42:04 logs every
skeleton once (`animfix: skeleton <ptr> nbones=<n> root=<bone>`) and spikes
over 40° on non-player skeletons as `spike (other)`. Auto frame capture now
waits for the first frame with >300 draws.

## Shield: the attachment bone, and a trace for it

`male_3p_appearance.xml.cooked` (uncooked with `hguncook.py`) says the left
weapon slot attaches to **`Bip01 prop2`** (fallback `ForearmLfC`); the right to
`Bip01 prop1` / `ForearmRtC`. Both prop bones are in `tm_3p_skeleton.hkx`,
after the finger bones. Shields are plain models (`shield01_mesh.GR2`, no
skeleton of their own in their appearance), so the flip must be in the prop
bone's animation or in how the engine places the attachment. Build 18:45:21
adds `animfix: trace <run file>@<t> w<w> Bip01 prop2=[i](pos | quat)
ForearmLfC=… HandLf=…` for the first 400 samples with a run/walk control over
weight 0.5; `bin\hellgate_animtrace.on` re-arms it. Plot quat against local
time to find the frame.

## Decompilation: the survey

The user asked whether to decompile the game fully. Survey of the existing
Ghidra 12.1.3 project (`~/ghidra_proj/HG.gpr`, program `hg_sp.exe`,
`tools/ghidra/AssertSurvey.java`): 25,395 functions; 1,978 assert-expression
strings and 91 `__FILE__` strings referenced from 800 / 708 functions; 834
distinct callee names in the expressions. The 91 files are almost all
renderer (`Source\Dx9\*.cpp`, `Source\DxC\*.cpp`: `dxC_light`, `dxC_effect`,
`dxC_environment`, `dxC_particle`, `dxC_hdrange`, `dxC_obscurance`, …), so
assert-based naming will name the renderer well and the game logic barely.
`dxC_EffectGetTechniqueByFeatures` appears in 12 expressions — that is the
function that decides `PointLights` per draw, the plan's open question.
Recommendation recorded in the reply: no recompilable decompile; a symbol
recovery pass (asserts + RTTI + Havok class names + string literals) into a
browsable code map, then decompile-on-demand with `Show.java`.

## Symbol recovery: 598 renderer functions named from the asserts

`tools/ghidra/NameFromAsserts.java` on `~/ghidra_proj/HG.gpr`: for each of
the 2,236 assert-expression string references, walk back to the nearest
direct call in the same function and vote that its target is called by the
expression's head identifier. 612 targets voted, **596 renamed** (2 already
named, 14 conflicts left alone — the biggest, 36 votes for 36 different
names, is the assert reporter itself, `FUN_00407269`), 708 functions tagged
with their `__FILE__` (91 files, 215 with a `__LINE__`). Output:
`notes/codemap/` (README + a page per file; `tools/codemap.py`), names live
in the Ghidra project, `make codemap` regenerates, `make decomp F="..."`
writes `notes/decomp/<name>.c`.

First harvest, the plan's open question "how many lights and where":
`dxC_AssembleLightsPoint` caps the model's shader lights at 5 (`cmp 4`),
`dx9_GetEffectAndTechnique` turns the count into a feature request, and
`dxC_EffectGetTechniqueByFeatures` picks the technique with the lowest
"missing feature" penalty — extra features are free. So new techniques with
`PointLights=5` in the material effects are used without touching the exe.
Written into `notes/graphics-plan.md` step 1.

## Shield: it is the run cycle's loop seam

Second trace run (19:01): 400 samples of `tm_3p_run.hkx` covering the whole
0.667 s cycle. `Bip01 prop2` (the shield's attachment bone) is constant in
local space for the entire cycle -- the asset has no bad key on it -- and
`ForearmLfC` never jumps. The only discontinuity is **`HandLf` jumping ~17°
at the wrap (t 0.665 → 0.002)**: the last frame of the run does not meet the
first. The user also reports the flip only on forward/back movement, never on
the strafes, i.e. only in `tm_3p_run.hkx`. The "shield angles backwards for a
frame" reading fits an attachment placed from the previous frame's bone
matrix: on an ordinary frame that lag is invisible, at a 17° discontinuity
it shows for exactly one frame.

Fix: seam smoothing (fix 2) is back ON, but the event set is now **loop wraps
only** (playhead went backwards on a control past 0.3 s with weight ≥ 0.5).
Retimes, weight changes and controls appearing or leaving no longer capture
offsets; those are the transitions the game eases itself, and re-smoothing
them is what made animations floaty. The first eight wraps log the seam per
bone (`animfix: wrap seam tm_3p_run.hkx: HandLf=17 ...`), which is the
measurement to check the fix against. Deployed 19:05.

## Per-pixel lighting, build 1: additive light pass on the actor materials

Design change from the plan: instead of rewriting the material shaders,
every technique gets a `_pp5` sibling with the stock pass untouched and a
second additive pass that lights per pixel with the model's five shader
lights (`tools/shaders/actor_lights.hlsl`, 16 variants over Skinned /
NormalMap / Specular / FIRST_LIGHT). `sRenderModel` loops over all passes
and sets the light parameters for each (decompiled), and the technique
lookup never penalises a surplus feature, so the `PointLights=5` sibling is
chosen whenever lights are near and the stock zero-light technique
otherwise. Indoor actors keep their two per-vertex lights and get slots 2-4
per pixel on top.

Getting the game's D3DX to load the result took most of the evening: the
fx_2_0 writer had to reproduce the compiler's sequential layout, the third
header count had to be derived (shader states + passes + sampler params),
and the injected shaders had to come from Microsoft's effect compiler
(`fxcomp`, d3dx9_34 from the Proton prefix) -- standalone compiles lack the
constant default blocks and `Bones[180]` then sinks the load. Bisection
tools: `fxload.exe` (loads an .fxo with the game's D3DX on a Wine device and
validates every technique), `hgfx.py roundtrip`. All in `notes/shaders.md`.

Installed: `override\data\effects\dx9\actoroutdoor30.fxo` and
`actorindoor30.fxo` (480 techniques each, 0 invalid in fxload), DLL rebuilt
with `bin\hellgate_override.off` as the A/B switch. Not yet seen in game.
Expected first impressions to check: spell and muzzle light on the player
and monsters outdoors (previously none), highlights moving with the light,
no z-fighting sparkle on the second pass, no change with the flag file on.

## Lighting build 1 in game: monsters lit, player not, particle glow lost

User's verdict: "looks worse, we lost particles". Log (19:45): the override
loaded; indoor monsters used the `_pp5` techniques (5,300 selections) but the
player's `actoroutdoor30` draws chose the stock zero-light technique every
time (10,635), so the request had `PointLights = 0` for the player.

"Lost particles": the extra pass set `D3DRS_COLORWRITEENABLE = 7` and the
engine never resets that state; particle glow (and actor glow) is the alpha
channel, so alpha writes stayed off for everything drawn afterwards and the
bloom vanished. Fix: the pass now sets only blend enable/factors and Z write,
which the engine re-sets per mesh; alpha is preserved by outputting 0 with
ONE:ONE. Rebuilt and installed 19:49.

Why the player asks for no lights: `dx9_GetEffectAndTechnique` zeroes
`nPointLights` unless bit 25 of a flags dword is set, and that byte comes
from `FUN_007e7b6c(model)` = `(model+0x12cc & g_bb0b58 | model+0x12cd) &
g_bb0b59` -- per-MODEL render-feature bits with two global masks, driven by
the "dynamic lights" feature line (`FUN_007e7ad3`: nibble 0 = off / player
only / all). With "all" the player should have the bit, so either the option
is at a stop that excludes him or no lights are assembled for his model.
Added a probe (`gfxprobe: feat <effect> [16 bytes] -> technique`) that logs
each distinct feature request; the feature byte order is the exe's table:
Index, PointLights, ShadowType, then the bool features. DLL 19:5x.

Wishlist grew: AO, parallax, volumetric fog, god rays -- recorded as plan
step 5 with prerequisites (all but parallax need depth, i.e. the MSAA
decision; parallax needs height maps derived from the normal maps).

## Lighting build 2: the state leak, properly

Still "breaking existing effects" after removing COLORWRITEENABLE. Root
cause confirmed by reasoning about the engine's cache: `dxC_SetRenderState`
skips a state whose cached value equals the request, and the engine calls
`ID3DXEffect::Begin` with `D3DXFX_DONOTSAVESTATE`, so nothing restores what a
pass sets. Any state in my extra pass (blend enable, ONE/ONE factors, Z write
off) therefore stayed on the device after each lit monster while the cache
believed the old values -- later meshes drew additive and without Z writes.
The stock effects get away with it because the engine knows which states
*they* touch.

Fix (DLL 19:59:50): hook `ID3DXEffect::Begin` (slot 63) and clear
`D3DXFX_DONOTSAVESTATE` for the overridden effects only; D3DX then captures
the states the effect touches at Begin and restores them at End. Counter
`g_begin_restored`. Everything else untouched.

Player still zero lights. The screenshot shows Dynamic Lights as a plain
checkbox, ON, so the feature-line theory is out; the request itself must be
zero for the player's meshes. The probe now names the effect via the engine
record (+0x118) and the chosen technique, and decodes the 16-byte request as
Index / PointLights / ShadowType ints plus a bool bitfield.
`bin\hellgate_override.off` remains the escape hatch.

## Lighting build 3, and the override goes opt-in

User's report on build 2: "textures were being reassigned to meshes live",
black shield indoors, the player's torso missing outdoors -- "you didn't
improve the graphics just broke the existing ones worse". All three are
mine:

- Textures swapping: the light pass declared its samplers at s0/s1/s2 while
  the stock actor shaders use s0/s5/s6; D3DX rebound stages 1 and 2 behind
  the engine's texture cache. Fixed: samplers pinned to s0/s5/s6.
- Wrong shader on the wrong mesh: `dxC_EffectGetTechniqueByFeatures` tries
  an exact 16-byte match first, otherwise scores *missing* features only. A
  request for 1-4 lights had no exact match, so every 5-light clone tied at
  zero cost -- including ones with the wrong Skinned / NormalMap -- and the
  last one won: rigid shield with a skinned VS (black), skinned torso with a
  rigid VS (gone). Fixed: `mkfx.py` now emits an exact technique for every
  feature combination and every light count 1-5 (`_plN`), with a light pass
  compiled per (first, count) so no stale light slot is ever read; clones
  share the stock shader objects (D3DX allows it), so the files stay small
  (4.0 / 3.7 MB, 1440 / 1056 techniques, 0 invalid in fxload). 64 shader
  variants now.
- State leak: fixed in build 2 via the `Begin` hook (state restore).

Policy change: the override loads only while `bin\hellgate_override.on`
exists (was: unless `.off`). Stock game by default until the user chooses
to look. Installed 20:1x.

## Panel toggle for the lights; seam report was never the player's

DLL 21:28:00. Anim tab (Ctrl+7 / "viewmodel") now has a GRAPHICS group:
"Per-pixel lights (5 per model)", default OFF. The replacement effects load
whenever `override\` has them (no flag file needed; `hellgate_override.off`
still skips them). The toggle acts at technique-request time in the
`dxC_EffectGetTechniqueByFeatures` hook: off clamps the requested
PointLights to the effect's stock maximum (computed at load from the
techniques without our `_plN` suffix), so the lookup lands on stock passes
only; on lets the request through. Each flip bumps the mesh technique-cache
generation (`DAT_00ad3ea8`) so it applies on the next draw. Counters "lit"
and "clamped" show it working.

Shield: the user reports the seam smoothing did not fix the frame. The log
explains why nothing can be concluded yet: all eight "wrap seam" reports came
from NPC skeletons (idle and run loops wrap too) and showed no bone over 3
degrees; the player's own wrap never got logged. The report is now
player-only (control with a traced file), twelve of them, and always names
the largest bone. Blender is not a route: the assets are Havok 4.0 binary
packfiles and the available hkx importers target the 2010 format.

## Build 4: the pass states were never in effect

The user: "are you just scattergunning". Fair question; the log for build 3
answers it: technique selection is now exact (`pl=5 bools=0a83 ->
TActor_22_0_0011100200103301000_pl5`, right combo every time), so the
remaining damage -- black character, a giant black triangle -- is the Lights
pass itself. `sRenderModel` (decompiled) calls `dxC_EffectBeginPass` and
*then* `sSetGeneralMeshStates`, which re-applies the mesh's blend and Z
states over whatever the pass declared. So the additive pass never was
additive: it drew opaque, replacing the finished model with a lights-only
image (black where nothing lit). That is what every build looked like, and
what "looked worse" meant on the first run.

Fix: `ID3DXEffect::BeginPass`/`EndPass` hooks mark "inside pass 1 of an
overridden effect"; the device's `SetRenderState` is shadowed; the
DrawIndexedPrimitive/DrawPrimitive detours force ONE:ONE blending, no Z
write, LESSEQUAL, alpha test off around the draw and restore the shadowed
values, so the engine's cache stays right. The pass's own render states are
now irrelevant. Panel counter "lit draws" counts these. DLL 21:4x.

Still unknown: the giant triangle (a vertex flung to infinity) may be the
skinned Lights VS on some mesh; visible only once the opaque overwrite is
gone. Toggle off = stock, verified by the "clamped" counter.

Addendum, build 4b (21:3x): the user reports missing model pieces and
artifacts; the build-2 `Begin` hook (state save/restore) is the likely cause
-- D3DX restores textures and shaders at End() behind the engine's caches,
which also explains the earlier "textures reassigned live". Removed; the
engine's contract that effects never restore anything is honoured again, and
the additive states live only around the light pass's draws.

## Build 4 was still broken with the toggle off. Root cause, at last: shared shader objects

"broke broke": artifacts, missing pieces, flung triangles, with the toggle
off as well. `fxload -bind` (new: set a marker matrix, begin a technique's
pass, read the vertex constants back through the game's own D3DX):

    TActor_22_0_1011100200103301030      pass 0  WorldViewProjection at c180, EyeInObject at c192
    TActor_22_0_1011100200103301030_pl5  pass 0  nothing        pass 1  nothing
    TActor_22_0_1011100200103301030_pl3  pass 0  nothing        pass 1  nothing

Any technique whose pass references a shader object that another technique
also references gets NO constants uploaded -- including the clone's copied
stock pass. Every model drawn through a clone (which, with the toggle off,
still includes the `_plN` clones with N at or below the stock count) ran
its shaders with garbage matrices. That is the whole list of symptoms since
build 3, and it also retroactively explains why sharing "validated" fine:
ValidateTechnique never uploads constants.

Action: `override\` renamed to `override.disabled` immediately so the game
is stock; mkfx now gives every clone's every pass its own shader object
(bigger files), verified with -bind before anything is reinstalled.

Rebuilt with unique objects (18.2 MB / 12.1 MB, 1440 / 1056 techniques,
0 invalid). `fxload -bind` now: `_pl5` pass 0 WorldViewProjection at c180,
pass 1 at c180 with EyeInObject at c184; rigid `_pl3` pass 0 at c0, pass 1
at c0. Every pass gets its constants. Staged in `override.disabled\`, NOT
installed: the user re-enables by renaming the folder to `override\` when
they choose to look again. Toggle default off; off = stock techniques only
(clamped requests hit stock or base-only clones, whose base pass now has its
own constants).

## Build 5 in game: lights render; the animation "fuckery" was the seam smoother catching restarts

User: "still have animation fuckery. the lighting is slightly better,
everything is slightly brighter, there's FPS spikes now." Screenshot: the
player folded over at the waist.

- The player-only wrap report finally arrived. The shield seam is in the
  **torso layer**: `tm_3p_Torso_Shld1HMLow_run.hkx: HandLf=17`, not the base
  run. And the folded body: `tm_3p_run.hkx: ThighLf=22 ToesLf=34 ThighRt=46
  CalfRt=59 KneeRt=30 …` -- 46-59 degrees is not a loop seam, it is the run
  cycle *restarting* mid-stride (time went backwards from >0.3 s), which the
  wrap test accepted; the smoother then applied the whole pose difference as
  an offset. Fixed: a wrap now requires the previous time within 10% of the
  highest time the control has reached and the new time within 10% of the
  start (`ctl_tmax` tracked per control). Seam smoothing default OFF again
  until this version has been seen; the panel toggle tries it.
- Lights: the toggle works ("slightly better, everything slightly
  brighter"). Brightness level is a tuning matter now, not a correctness one.
- FPS spikes: 70 SPIKE windows this session, 48 of them in the first
  ~100 s (loading), the 200-700 ms ones all *before* the first toggle. See
  the cross-session comparison below.

Spike comparison across sessions (SPIKE windows are >33 ms of MOPP time or
a frame dt worth flagging; ">200ms" counts windows whose max dt exceeded
200 ms):

| session | windows | spikes | >200 ms |
|---|---|---|---|
| this one (build 5, overrides on) | 3,077 | 70 | 46 |
| previous (stock effects) | 244 | 0 | 0 |
| wined3d 30-min session (before graphics) | 19,400 | 10 | 3 |
| DXVK baseline | 11,635 | 514 | 0 |

New and real. Not physics (qray ~0 in the bad windows). Suspects, in
order: the 18 MB / 12 MB effects (5,280 shader objects created twice at
load; per-mesh technique cache thrash across 1,440 techniques), then my
per-draw SetRenderState traffic, then the per-skeleton spike scanner.
Measuring D3DXCreateEffect cost offline first.

## Build 6: the hitches were the effect files

`fxload` timing with the game's D3DX: stock actoroutdoor30 creates in 19 ms;
build 5's 18 MB version in 2,416 ms (indoor: 16 ms vs 743 ms), and the
engine creates every effect twice, at load and on level transitions. That
is the "FPS spikes now".

Fix: one five-light clone per feature combination instead of five. The
technique-request hook rewrites any lit request (1-4) to exactly 5 so it
hits that clone, and the BeginPass hook zeroes PointLightsColor[n..4]
before the light pass so the unfilled slots contribute nothing (the engine
fills only n). Off still clamps to the stock count. Result: 5.6 MB / 5.2 MB,
480 techniques each, 116 ms / 112 ms to create, constants bound in both
passes of the clones (-bind). Installed 22:1x with the DLL.

Also: the retail 2007 DVD (`Disc01.iso`, from the user's mounted copy) is
archived at `ref/retail-2007/` with a sha256 and a content listing, for
diffing the original client's effects and data against the 2018 build.

## Seam blend and light strength

- Seam: the user's read is right -- inertialization makes the shield *drift*
  for 0.2 s after the wrap, which is more visible than the one-frame snap.
  Replaced by a pre-wrap blend (`seam_preblend`): the pose at the start of
  each cycle of the dominant looping control is remembered and, over the
  last 20% of the cycle, the sampled pose is blended toward it, so the wrap
  lands on frame 0 with nothing left to correct. Default on; panel "Seam
  blend (pre-wrap)". Inertialization kept behind `g_seam_inertial = 0`.
- Brightness: the user's hypothesis was "more lights than before". The
  decompile (`sEffectSetSHLightingParams`) shows the ambient gets the
  per-slot SH sets only for slots the technique lacks (`5 - PointLights`),
  so with the 5-slot clones there is no double counting; a sharp light is
  just brighter than its soft SH stand-in. Added `gvUltraLight` (our own
  parameter, appended to the effect by mkfx) as a strength multiplier, set
  by the DLL before the light pass from a panel value (default 60%, +/-10).

## Player shadow: where to look

User: "we also don't have the player casting a shadow yet". The shadow-map
pass draws animated models thousands of times per session (`shadowmap.fxo
AnimatedShader` in the technique counts), so monsters cast; the player is
excluded per model. `MODEL_FLAGBIT_NOSHADOW` is bit 4 (paperdoll setter at
0x4b9236; FIRST_PERSON_PROJ = 7 at 0x4d3112 confirms the reading).
`dx9_RenderDrawList` dispatches `dx9_RenderModelShadow` per list command;
that function early-outs on `DAT_00c48e9c == 2 || FUN_00778dc6()` and on
model lookup. First experiment, no code: panel Model tab, flag bit 4,
"Set 0" on the third-person model, and see whether a shadow appears. If it
does, the DLL clears the bit on the player's model at spawn; if not, the
gate is in the draw-list builder and needs a hook.

## Player model id, and a shadow toggle

The panel's Model tab could not poke the player's model: the local-player
getter returns a unit whose `+0x160` pGfx is NULL (very likely the
server-side copy of the character -- single player runs both in one
process), so `UnitGetModelIdThirdPerson` gives -1. The animation trace
already filters on `model+0x18 == player unit id`, and the model record's
id is at `+8` (what the model hash table keys on), so `animwatch` now
records the player's model id as it sees it, and `hg_model_chain` falls
back to it. Model-tab pokes work again.

Panel GRAPHICS: "Player casts shadow" clears MODEL_FLAGBIT_NOSHADOW (4) on
that model from the game-thread pump and re-applies when the id changes.
Whether the shadow appears tells whether the flag is the gate or the
draw-list builder is.

## Shadow, round 2: the model id was wrong, and the gate is in the renderer

The last run's log: `model: third-person model 5248, flagbit 4 := 0 (rc
-2147024809)` -- E_INVALIDARG: the id the animation trace carries (its
"model" +8) is not a dxC model id, so nothing was ever poked. Now the chain
falls back to the client game's control unit (`game+0x238`, the camera's
unit, tracked by shoulder.c), which should own the pGfx.

Read so far in the renderer: `dx9_RenderModelShadow` skips a model unless
`DAT_00c48e9c == 2` (the first-person check the skill code also uses) or
`FUN_00778dc6` (model drawable in the current region and `model+0x14 !=
-1`). `FUN_00778d67` is the model flag-bit test used everywhere. Data flags
exist too: the EFFECTS table has per-material `CastShadow`/`ReceiveShadow`
(+0x21c bits), and a unit table has `bNoCastShadow`. A probe now logs
whether the player's model reaches `dx9_RenderModelShadow` at all and what
it returns; that splits "not in the list" from "rejected inside".

User's direction, in their words: "like the new WoW Forever -- keep the old
2007 era graphics but really juice up effects like particles, shadows,
lighting, fog, light rays, water". Blizzard's Forever relights the classic
world (GI, volumetric fog, god rays, water, terrain shadows) without
touching models or textures. Same brief as ours.

## Shadows: the engine-level gate

User (screenshot, Tottenham Court Road, zombies): "nothing at all really
casts dynamic shadows. maybe we put cart before the horse, we need more
dynamic shadow *casters*". Log: `dx9_RenderModelShadow` was called 92,369
times (2,207 for the player), the player's call returned E_FAIL, and the
gameplay frame capture showed no shadow-map render target ever bound. The
renderer's gate is `[0xedfd14] == 0 && [0xedfcb4] != 0`, two entries of
the engine's render-flag array (`e_GetRenderFlag`) with no direct writers.
Environments carry `fShadowIntensity` 0.3-0.8, so if the pass ran, shadows
would show. Hypothesis: the "shadows" render flag is off under this
configuration (wined3d caps? shadow buffer creation?), so nothing casts.
Build 22:3x: the frame capture logs those flags and every nonzero render
flag, the shadow pass result histogram, and the panel has "Force engine
shadow flag" to set [0xedfcb4] = 1 as an experiment (off restores).

## The shadow pass is dead in this build (under Proton/wined3d at least)

Run 22:40: `dx9_RenderModelShadow` 17,207 calls, 0 succeeded, all E_FAIL;
the gameplay frame binds a 2048x2048 shadow colour target and a 2048x2048
D24X8 depth target, clears them, draws nothing into them; no shadow-map
technique was set all session. `shadowmap.fxo` loads and validates under
Wine and its technique matrix is complete (Index 0/1/2 for ShadowType 1
and 2, plus DepthCopy). The render flag at 0xedfcb4 was 0 and forcing it to
1 did not help, so the bail-out is later: the effect-record lookup, the
"effect ready" bit (0x10000 at record+0x114), the three technique lookups,
or the per-mesh vertex-format checks (1..4). The user confirms every
Windows screenshot of the game shows dynamic shadows, so this is a
platform or build failure, not a missing feature. A pasted "AI overview"
claiming shadows were DX10-only is wrong for casting; DX10 only added PCSS
softening.

Probe (DLL 22:5x): every technique request on the shadow effect is logged
with the requested ShadowType, and requests are counted per effect without
a cap. That splits "never asked" from "asked and rejected".

## Shadow gate = render flag 0x43; the seam blend never ran

Run 22:49: zero technique requests on the shadow effect all session while
`dx9_RenderModelShadow` was called 3,431 times, all E_FAIL. So it bails at
the top, at `[0xedfcb4] != 0`. `e_SetRenderFlag` (0x778460, cdecl) is the
only writer of the flag array (base 0xedfba8): 0xedfcb4 is render flag
0x43, 0xedfd14 is 0x5b, and the setter runs a per-flag callback from a
table at 0xedfd38 before storing. The panel's "Force engine shadow flag"
poked the array directly and so skipped that callback; it now calls the
setter (DLL 22:52:03). No caller sets 0x43 with a constant; one caller
sets flags from a computed index (feature-line application), which is
where the decision to leave shadows off is made -- reading it next.

Shield: the user reports the seam still not fixed; the panel screenshot
showed "seams 0", and the cause is mine: the per-skeleton clock was only
advanced by the (disabled) inertialization block, so the pre-wrap blend and
the wrap/cycle bookkeeping never ran. Fixed 22:51:00.

## 2026-09-21 23:08 — the dead shadow pass was our own probe

The user confirmed shadows render when the game is launched without
`version.dll`. Cause: `dx9_RenderModelShadow(nDrawList, nData, nID)` takes
the model id in ECX plus two stack arguments that the caller pops
(`push [edi+0x30]; mov ecx,[edi+4]; push [esp+0x40]; call; pop ecx; pop ecx`).
The probe's `__fastcall(int model, int edx)` detour forwarded ECX only, so
the renderer read its draw-list index from garbage, `FUN_007b4135` returned
NULL and every call exited E_FAIL (39,639 of 39,639). Replaced with an asm
thunk (`gfx_shadow_stub`) that forwards the stack arguments and calls the
original with caller cleanup, then hands the result to `gfx_shadow_note`.

Two more probe errors found on the way: the render-flag array reads used
RVA 0x6df… instead of 0xadf… (0xedfba8 − 0x400000), and the definition
table entries are `{name[64], type, default, mode-3 default}` starting at
0xad40b8, so the names printed earlier were off by one. Correct reading of
the gate in `dx9_RenderModelShadow`: `wireframe (91) == 0 && shadows (67)
!= 0`, and `shadows` defaults to 1. The "SHADOW request count: 0" line was
also meaningless: the shadow path uses the 0x78078f overload of
`dxC_EffectGetTechniqueByFeatures`, not the hooked 0x7807ff one.

## 2026-09-21 23:16 — shadows: wined3d drops them, the shim does not

A/B with the fixed DLL loaded both times, same install, same options:

| launch line | shadows |
|---|---|
| `PROTON_USE_WINED3D=1 WINEDLLOVERRIDES="version=n,b" gg %command%` | none |
| `WINEDLLOVERRIDES="version=n,b" gg %command%` (DXVK) | player and monsters cast shadows |

Both runs log the same hooks, the same 2560x1600 4x MSAA A8R8G8B8 back
buffer and D24S8 depth, the same effect override, and a healthy shadow pass
(every `dx9_RenderModelShadow` call returns S_OK). So the shadow map is
rendered and sampled identically from the engine's side; whatever wined3d
does with the shadow-map render target or its depth compare produces
nothing on screen. Not a DLL bug and not an engine bug. The `shadows`
render flag defaults to 1 and needs no forcing; the "Force engine shadow
flag" panel toggle is moot.

Decision (user): keep wined3d as the default launch line, since the 1 FPS
raycast bug reproduces only there. Shadow work therefore has two options:
find the wined3d shadow-map failure (probe the shadow RT format and the
technique's sampler states under both renderers) or do the relighting on
DXVK and keep wined3d for the physics repro only.

## 2026-09-21 23:40 — shadows on wined3d: force the colour shadow map

`e_GetActiveShadowType()` is the option state's `nShadowType` at
`[[0xedff74]+8]+0x40`, chosen from caps at startup (not in settings.xml,
which only stores the SHDW quality). Type 1 = depth shadow map: a fake
colour target plus a D24S8 depth texture sampled as a shadow sampler
(NVIDIA hardware PCF); type 2 = colour shadow map: an R16F/R32F/A8R8G8B8
target drawn with shadowmap.fxo's `*ColorShader` techniques. On the RTX
5080 both DXVK and wined3d pick type 1; DXVK draws it, wined3d produces
nothing. Hooking the shadow-buffer creation (`FUN_007e2be5`, cdecl, option
state as its argument) and writing 2 into the field, then re-asserting it
each frame, brought shadows back under wined3d — user confirmed
("it is working now").

Shipped as automatic: the DLL forces type 2 only when `d3d9.dll` is Wine's
builtin (the "Wine builtin DLL" marker in its DOS stub); DXVK keeps its
native depth path. `bin\hellgate_shadowtype2.on` / `.off` override the
detection. The frame capture now waits for a gameplay frame that ran the
shadow pass and prints the active shadow type next to the pass summary.

Two loose ends: which part of the depth path wined3d breaks (the NULL
render target or the shadow sampler), and whether the colour map loses the
hardware PCF softness -- compare screenshots on both renderers.


## 2026-09-21 23:50 — renderer decision: DXVK

User: "i should just use dxvk anyway it's more well trodden at this point
and has less friction with rendering." Launch line is now
`WINEDLLOVERRIDES="version=n,b" gg %command%`. Consequences for the shadow
overhaul: depth textures with hardware PCF are available (DXVK implements
the NVIDIA depth-sampling path the engine already uses), INTZ is supported,
and the wined3d colour-map workaround stays in the DLL, dormant unless
`d3d9.dll` is the Wine builtin. The 1 FPS raycast repro keeps its
`PROTON_USE_WINED3D=1` line.

## 2026-09-22 00:05 — 2007 vs 2018 environment data, and why the picture is flat

The retail disc's `hellgate000.idx/.dat` (from `ref/retail-2007/Disc01.iso`,
extracted with 7z; the mounted loop device gives I/O errors) decrypts with
the Steam key: 24,456 files, environment definitions under
`data\background\<level>\*_env.xml.cooked` (the 2018 build moved them to
`_environments\` and renamed all of them; zero name overlap). `hgdat.py`
now honours `HG_GAME=<root>` to read another install.

Aggregate over 60 (2007) / 72 (2018) environments, after `hguncook`:

| | 2007 | 2018 |
|---|---|---|
| fShadowIntensity median | 0.70 | 0.70 |
| direct light (sum intensity x luminance) median | 1.31 | 1.47 |
| flat ambient (intensity x luminance) mean | 0.03 | 0.11 |
| hemisphere intensity mean | 0.00 | 0.35 |
| fog start median | 10 m | 2 m |
| clip median | 132 m | 100 m |
| envs with baked SH (bg / appearance) | 13 / 17 | 35 / 29 |

Matched pairs (by light vectors and colours) show the same shadow and
direct values, so the shadow darkness is not a data regression. What did
change in 2018: more fill (ambient, hemisphere, SH on twice as many
environments) and fog starting almost at the camera. Both flatten contrast.
Add `fGammaPower 1.15` in settings.xml (lifts midtones) and the fact that
`hdr.fxo` is created but never has a technique set (no tone map or bloom
runs on the DX9 path; the per-frame post stack is `gaussian` +
`combinelayers` + `overlay`), and the washed-out look in the side-by-side is
explained without any shader difference.

Data relight, first pass: push fog start back out (10-20 m), cut hemisphere
and flat ambient roughly in half on the 2018 envs, gamma 1.0 in
settings.xml. Shader side: a tone map of our own at EndScene, since the
engine's never runs.

## 2026-09-22 00:20 — the engine's shadow pass, measured (DXVK)

From the surface table and the in-pass target probe:

- **One shadow map, 2048x2048**: RT0 = a 2048x2048 A8R8G8B8 "fake colour
  target" with `COLORWRITEENABLE = 0`, depth = a 2048x2048 **D24X8**
  texture (the type-1 depth map). No MID/LOW map is used in play; a second
  2048x2048 colour surface was bound twice and a 1024x1024 once, at level
  start (particle lighting map / setup).
- **Re-rendered every other frame**: busy frames alternate (7286, 7288,
  7290 ... 11 render-target changes, ~360 draws, shadow calls = 1), the
  frames between draw the scene only. Every automatic capture so far landed
  on an off frame; the trigger now arms on a scene frame without shadow
  calls so the next capture is the shadow frame.
- The rest of the per-frame off-screen work: three 640x400 A8R8G8B8
  targets (the gaussian / combinelayers glow chain) and one full-size
  2560x1600 A8R8G8B8 copy. No HDR target, consistent with hdr.fxo never
  being used.

For the overhaul this means: replace or supplement a single 2048 map that
covers the player's surroundings, refreshed at 30 Hz, with cascades from
the Z-pass replay; the engine's depth texture and matrices can be reused
as the near cascade at first.

## 2026-09-22 — material shader rewrite: all 1,482 techniques at parity

**Hypothesis.** The six SM3 material effects can be rebuilt from our own HLSL
with pixel-identical output, which makes every later lighting change a
source edit instead of a bytecode patch.

**What would refute it.** Any technique whose pixels differ from stock
beyond 2/255 when both are drawn with identical inputs, or an effect the
game's D3DX will not load.

**Run.** New tooling:
- `tools/fxdiff.c` draws every technique of the stock and the rebuilt effect
  on a grid carrying every vertex element, with the same deterministic
  parameters and noise textures, and compares the pixels. Four seeds: camera
  light on/off x dim/bright lighting.
- `tools/mkmat.py` turns each technique's annotations into defines, and
  `fxcomp -batch` compiles the variants (240 in 8 s). The blobs are swapped
  into the stock effect in place, so names, annotations and pass states stay
  byte-for-byte stock.
- `tools/matcheck.sh <effect> <family>` does the whole loop.
  `tools/matmutate.py` breaks one term at a time and confirms the harness
  catches it.
- `tools/hgfx.py pres` decodes preshaders, the CPU-side expressions the
  effect compiler hoists out of shaders (fog scale, specular power range,
  camera-light enable, glow alpha mix).

**Result.** `tools/shaders/actor.hlsl` and `background.hlsl` reproduce all
six effects: actoroutdoor30 240, actorindoor30 288, backgroundoutdoor30
216, backgroundindoor30 252, backgroundoutdoorprop30 243,
backgroundindoorprop30 243. That is 0 differing techniques on every seed, and
all 17 deliberate breaks are caught. The harness itself needed four fixes
before those claims meant anything:
- the cube map is declared as a plain `texture`, so it has to be bound by name;
- per-texture alpha masks must be independent, and xorshift is linear, so two
  keys gave identical or inverted patterns;
- struct parameters (`gfScrollTextures`) have to be set, or UV scroll is dead;
- light levels have to vary, or the soft clamp hides small terms.

Installed into `<game>\override` together with the actor light-pass clones.
Expected in game: no visible change. The DLL now restores state only for
effects that carry the light pass, so backgrounds keep the engine's
DONOTSAVESTATE.

**Stock behaviour now on record** (all reproduced, quirks included):
- *Actor shadows* multiply the whole fill (2 x vertex colour: SH, ambient,
  directional light 1) plus directional light 0 by `lerp(1, s, y)`. The
  highlight takes the raw sample s. Indoors every vertex counts as facing
  the light.
- *Background shadows* differ per effect:
  - outdoor and outdoor prop: `s = min(second map, (main + 1) / 2)`, and
    surfaces facing away get 0.5;
  - indoor: the main map only;
  - indoor prop: the main map only, and facing away gives 0.

  The factor `lerp(1, s, y)` multiplies the vertex light AND the light map.
  **This is the "character shadows darker than world shadows" cause:** a
  character's shadow also removes light that is already baked, including
  the baked shadow's own fill.
- Background highlights outdoors take `lerp(y, 1, s)`, the same factor with
  its ends swapped (a stock bug). Indoors they ignore the shadow.
- Backgrounds: the normal map only feeds the highlight, and is dropped when
  a cube map and a shadow map are both on (out of interpolators).
  Self-illumination is not tinted by the albedo (it is on actors).
- Indoor background highlights come from whichever of two "specular lights"
  is stronger at the vertex. A tie switches the highlight off.

**Conclusion.** Confirmed. The material shaders are ours. Next: the shadow
fill fix and PCSS, behind a runtime parameter that defaults to stock.

## 2026-09-22 — shadow fill + PCSS, behind panel toggles; 25 s shader builds

**Change.** Two new effect parameters, `gvUltraMat` and `gvUltraShadow`
(`tools/shaders/ultra.hlsl`), are added by mkmat with an all-zero default,
which is stock. `make matcheck` still reports 0 differ on all six effects.
The DLL writes them from the SetTechnique hook, where the effect is known to
be alive (effects are recreated per level).
- **Shadow fill** (panel toggle and 25% steps). Outdoor actors: the shadow
  removes only the per-pixel sun term (DirLightsColor[0]·N·L). Outdoor
  backgrounds: it removes only the dynamic sun term, sent from the VS in
  TEXCOORD5.xyz, uncapped (no (s+1)/2), instead of scaling the light map.
  Indoor materials are unchanged, since there is no sun term to separate.
- **PCSS** on the colour shadow map: a 16-tap blocker search, then 16 filter
  taps on a Vogel disk rotated per pixel by interleaved gradient noise.
  Penumbra = (receiver − blocker depth) × "sun size", 1..16 texels. It
  replaces the main-map lookup; the outdoor second map keeps stock PCF.
- **The colour shadow map is now the default** (it has readable depth);
  `hellgate_shadowtype2.off` opts out. Its format list is patched so R32F
  comes first: the engine took R16F, whose ~11 bits of depth are too coarse
  for a blocker search.

**Checked offline.** `fxdiff -shadowscene` puts two disc blockers into the
main map, with everything else flat grey. Stock shows hard discs at half
strength. PCSS at sun size 15/40 gives soft, grain-free penumbrae, wider for
the far blocker. With fill at 100% the shadow almost vanishes: in that scene
the sun is a small share of the light. The in-game share depends on each
environment's data, hence the slider.

**Build time.** A full shader build went from 9 min to 25 s, and a no-change
one takes 13 s:
- the PCSS loops are real loops (a computed Vogel disk instead of a table
  forcing unroll);
- `tools/matcompile.sh` compiles in parallel across all cores;
- `mkmat.py plan` skips variants whose sources and defines are unchanged.

`make matcheck` takes 85 s.

**Follow-ups, same day (after the first in-game run).**
- *Knobs never arrived.* Each effect is recorded twice at one address
  (D3DXCreateEffect calls the hooked ...Ex), and only the inner record is
  marked overridden. `ultra_apply` took the newest record and skipped. It now
  looks the parameters up by name on the live effect. The panel shows
  "knob writes", and the log records each write with `gvShadowSize`.
- *PCSS worked indoors but not outdoors.* Outdoor backgrounds take
  min(second map, (main + 1) / 2); the remap caps the main map at half
  strength, so the full-strength ground shadow comes from the SECOND map,
  which still had the stock 2x2 filter. PCSS now runs on both. The
  `-shadowscene` test had hidden this by blocking only the main map.
- *Contact shadows looked aliased.* Added a minimum PCSS radius
  (`gvUltraMat.y`, panel "min softness", default 2 texels).
- *Tuned in game:* contact shadows fixed by a smaller search bias, plus a
  depth-bias control on the panel; separate indoor and outdoor sun size.
  User-chosen defaults: outdoor sun size 450, indoor 66, min softness 1,
  bias 200e-6 per texel of radius. PCSS and fill stay off by default.
