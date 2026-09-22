# Analysis: the "2026 fix" patched executable

**Source:** `Hellgate_sp_x86_2026_fix_git/Hellgate_sp_x86.exe`
sha256 `468da1b5ed709c1498ea629a6db24ff65241c6131ab9b051073d1ab813226c15`

Same size (11,345,920), same PE timestamp, same entry point as the stock
Steam binary — it is a **byte patch of the exact binary we have analysed**,
so every RVA recovered in `docs/journal.md` applies directly and a byte diff gives
the complete change set.

**9 patches, 1800 bytes.** Notably **nothing at `0x870B10`** — he did *not*
stub `queryRayOnTree`. This is a different and more interesting fix than
augmentrex's.

| # | VA | Change | What it is |
|---|---|---|---|
| 1 | 0x0040E9FC | `push ebp` → `ret 8` | stubs the `"HLOCK logic error!!"` debug reporter |
| 2 | 0x00499953 | function body → trampoline + zeros | body reused as a code cave |
| 3 | 0x0049A537 | `comiss` operand `0xA06FF0` → `0xA0086C` | threshold **15.0 → 5.0** |
| 4 | 0x0049B986 | `call 0x499953` → 5× `nop` | disables the Granny animation-clock update |
| 5 | 0x005B9FE0 | 9 bytes → `jmp 0x499953` | null-pointer guard via the cave |
| 6 | 0x007062B2 | `je` → `jne` | inverts a validity check |
| 7 | 0x00706329 | `jne`→`je` ×2 | same function |
| 8 | 0x0070633A | `jne` → `je` | same function |
| 9 | **0x00824559** | `mov [eax+0x95], cl` → `mov byte [eax+0x95], 1` | **the real fix** |

---

## Patch 9 is the root cause

`FUN_00824480` is **`hkWorldCinfo::hkWorldCinfo()`** — confirmed by the
`hkWorldCinfo::vftable` store at its head. Immediately before the patched
instruction:

```
0x82454e   mov   cl, 2
0x824550   mov   byte [eax + 0x68], cl
0x824559   mov   byte [eax + 0x95], cl     ; <-- patched to constant 1
```

So `+0x95` is a byte enum whose stock value is **2**, changed to **1**.

In Havok 4.0, `hkWorldCinfo::SimulationType` is
`INVALID=0, DISCRETE=1, CONTINUOUS=2, MULTITHREADED=3`.

**The game runs Havok in CONTINUOUS simulation; the patch switches it to
DISCRETE.**

Corroborated three ways in the stock binary:

- `.?AVhkContinuousSimulation@@` is in the RTTI — the continuous simulation
  class is linked in and instantiated.
- `.?AV?$hkSymmetricAgentLinearCast@VhkMoppAgent@@@@` is in the RTTI — this
  is the swept (CCD) cast of a moving body against MOPP geometry, and it is
  precisely the path into `hkMoppBvTreeShape::castRay` →
  `hkMoppLongRayVirtualMachine::queryRayOnTree`.
- `objectQualityType` appears as a string, so per-body collision quality is
  already a concept in this engine.

### Why this explains the bug

Continuous simulation does **swept** collision detection to stop fast bodies
tunnelling: for each moving body, each step, it casts its motion path
against the world. Against level geometry that cast is a MOPP ray query. So
the raycast volume scales with *the number of moving bodies*, not with
anything the game explicitly asks for.

That matches alexrp's description exactly — "an excessive **number** of ray
cast queries under certain circumstances" — and, more importantly, it
matches **our own measurements**, which were made before we had any idea
this was the cause:

| Our measurement (E9/E12) | Under CCD |
|---|---|
| 393:1 `queryRayOnTree` vs game world raycasts | the work is per-body sweeps, not game raycasts |
| thousands of queries in windows with **zero** game raycasts | sweeps happen with no game query at all |
| **~5.9 MOPP queries per physics object per step** | exactly what per-body swept collision looks like |
| no malformed ray ever seen on the game path | the long rays are Havok-internal |

"Certain circumstances" = many simultaneously moving bodies. Explosions in
large outdoor zones spawn crowds of debris; every piece sweeps against the
level every step. Lowering effect detail helps because it spawns fewer.
Onset after ~2 hours fits bodies accumulating without being cleaned up.

### The tradeoff he accepted

Switching to DISCRETE removes continuous collision detection **globally**.
That is what CCD exists to prevent, so the expected new failure modes are
**tunnelling**: fast projectiles passing through walls, bodies and dropped
items falling through floors, ragdolls clipping into geometry, and thrown or
knocked-back objects escaping the level. This is very likely the source of
the "other classes of bugs" reported.

---

## The other eight, and what they cost

**1 — stub the HLOCK error reporter (0x40E9FC).** The function does
`OutputDebugStringA` plus formatted prints when a lock-ordering error is
detected. Stubbing it is plausibly a *real* speed win, because
`OutputDebugStringA` is slow (on Windows it takes a global mutex; under Wine
it is worse) and a per-frame lock error would spam it. But it treats the
symptom: the lock errors still happen, they are now silent. **Cost:** a real
engine bug is hidden, and with it any chance of diagnosing it.

**4 — disable the Granny animation-clock update (0x49B986).** The removed
function accumulates the frame delta into `+0x164`, and while that exceeds a
threshold it repeatedly subtracts and calls
`GrannyRecenterAllControlClocks`, then walks an object list doing per-object
work. Deleting the call outright is drastic: clock recentering exists to stop
animation time growing without bound and losing float precision over a long
session. **Cost:** plausibly animation drift, stutter or precision loss in
very long sessions — and note this is a *long-session* mechanism, the same
regime as the "~2 hours" reports.

**2 + 5 — null guard at 0x5B9FE0.** The displaced instructions run in the
cave, then `test eax,eax` skips a block when the pointer is null. This is a
straightforward **crash fix**, unrelated to performance. Worth keeping in
mind as a separate known-good bug fix.

**3 — threshold 15.0 → 5.0 (0x49A537).** `movss xmm0,[esi+0x74]; comiss
xmm0, K; jbe skip`, in the physics/animation update path. Lowering K means
fewer objects take the skip branch. Direction of effect **not established**;
needs the surrounding function identified before any conclusion.

**6, 7, 8 — invert a validity predicate (0x7062B2 / 0x706329 / 0x70633A).**
All inside `FUN_0070629d`, which returns 1 only when a chain of checks
agree. The primary guard `cmp edi,-1; je bail` becomes `jne bail`, which
makes the function bail in the *common* case — effectively disabling
whatever path it gates. Function not yet identified. Inverting a validity
check is the highest-risk change in the set and the most likely source of
subtle misbehaviour; it should not be copied without understanding it.

---

## What we should do differently

**Do not copy patch 9.** Global DISCRETE buys the framerate by giving up
tunnelling protection everywhere, including for projectiles and the player.

The targeted version keeps continuous simulation for bodies that need it and
demotes only the ones that do not. Havok's per-body
`hkCollidableQualityType` exists for exactly this: `DEBRIS` skips most CCD
work while `CRITICAL`/`MOVING` keep it. Cosmetic debris and gibs do not need
tunnelling protection; a rocket does.

Two routes, in order of preference:

1. **Data, not code** — the `objectQualityType` string suggests quality may
   already be authored per object type. If it is reachable without patching
   code, that is the cleanest fix available.
2. **Boundary** — set the quality type on bodies as they are created,
   classifying cosmetic bodies as debris. Keeps CCD where it matters.

Either way the fix is narrower than his, and unlike augmentrex's stub
neither disables raycasting, so AI line-of-sight and boss mechanics are
untouched.

## Still unexplained

The **DXVK** and **Intel-vs-AMD** reports. CCD cost scales with how far a
body travels per step, so longer frames mean longer sweeps and more tree
walked — which would make a slower renderer or CPU tip the game over sooner.
That is a plausible amplifier rather than a cause, and it is a corrected
form of the refuted H5. It is **not** confirmed, and E12's data (large
deltas with zero MOPP work) argues against a simple version of it.
