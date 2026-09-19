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

## Next: E8 — baseline repro under Proton (needs a human at the keyboard)

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
