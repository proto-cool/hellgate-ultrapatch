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

## Next: E13 — rerun with working stack attribution

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
