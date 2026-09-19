# Backlog

**Parked until the 1 FPS stall is closed.** Nothing here is started.

Everything below is a *lead*, not a finding. The distinction matters: the main
investigation is producing results because every claim goes through
hypothesis → the log line that would confirm or refute it → run → conclusion
(see `LOG.md`, where E9 refuted my own preferred hypothesis inside 20 minutes
of play). A patch project is a magnet for plausible-sounding tweaks that were
never tested. Each item here gets the same treatment or it does not ship.

Confidence is about *the evidence stated*, not about whether the idea is good:

- **Confirmed** — verified in the binary, behaviour still unverified.
- **Live** — the string/symbol is referenced from real code, so the feature
  exists in some form; what it actually does is unverified.
- **Observation** — noticed, not investigated.

---

## A. Second known stall — capsule/capsule collision agent

**What.** The Blademaster Whirlwind freeze. `hkCapsuleCapsuleAgent::processCollision`
burns the frame, same shape of problem as the MOPP one, different agent.

**Evidence.** Confirmed. Augmentrex ships a byte pattern for it
(`Augmentrex.Commands.PatchCCAgent`), and independently its Havok timer tag
`TtCapsCaps` resolves to 0x0088A890 / 0x0088AD60 / 0x0088B0FF / 0x0088B5A0 in
this build. Separately reported by the community.

**Why it belongs here.** It is the only other *confirmed* stall, the
infrastructure already built handles it, and augmentrex's global stub for it
has the same "treats the symptom" problem as the ray one.

**To verify.** Same method: hook, count, time, attribute, find out who is
feeding it degenerate capsule pairs. Do not stub it globally.

**Effort.** Medium — likely a repeat of the current investigation.
**Risk if done wrong.** Stubbing collision response breaks melee.

---

## B. Global feature flags — 28 of them, all live

**What.** A registry of named boolean flags.

**Evidence.** Live. All 28 `GLOBAL_FLAG_*` strings are referenced from a
single table-builder at **0x009595DB**, i.e. a real registry rather than dead
strings. How flags are *set* (config file? command line? debug only?) is not
yet known and is the first thing to establish.

Directly relevant to the main investigation:

| Flag | Why it matters |
|---|---|
| `GLOBAL_FLAG_NO_CLEANUP_BETWEEN_LEVELS` | If this is on by default it would go a long way to explaining the ~2-hour onset reports (H4, accumulation). **Check this while chasing the main bug, not after.** |
| `GLOBAL_FLAG_HAVOKFX_ENABLED`, `..._RAGDOLL_ENABLED`, `MULTITHREADED_HAVOKFX_ENABLED` | HavokFX was GPU physics that effectively never shipped. If these are on, they may be doing nothing useful at some cost. |
| `GLOBAL_FLAG_NORAGDOLLS` | A blunt but real lever if ragdolls turn out to feed the stall. |
| `GLOBAL_FLAG_FULL_LOGGING`, `SILENT_ASSERT`, `DATA_WARNINGS`, `STRING_WARNINGS` | Diagnostics that might save building our own. |

Also present: `ABSOLUTELYNOMONSTERS`, `NOMONSTERS`, `NOLOOT`, `MAX_POWER`,
`CHEAT_LEVELS`, `SKILL_LEVEL_CHEAT`, `NO_CONVERTS`, `FORCE_SYNCH`,
`NO_POPUPS`, `AUTOMAP_ROTATE`, `INVERTMOUSE`, `CAST_ON_HOTKEY`,
`SKILL_COOLDOWN`, `USE_HQ_SOUNDS`, `UPDATE_TEXTURES_IN_GAME`,
`NO_SHRINKING_BONES`, `LOAD_DEBUG_BACKGROUND_TEXTURES`,
`FORCEBLOCKINGSOUNDLOAD`, `BACKGROUND_WARNINGS`.

**To verify.** Decompile 0x009595DB, recover the registry structure and how
entries are read. That one function probably answers the whole category.

**Effort.** Low — one function.

---

## C. Command-line switches — 8, parse sites confirmed

**What.** Real argument parsing, not leftover strings.

**Evidence.** Live.

| Switch | Parse site | Note |
|---|---|---|
| `-framelimiter` | 0x00404D05 (fn 0x00404CF9) | Most interesting. An engine this old has no modern frame pacing; if this is a real limiter it could be a quality-of-life win by itself. |
| `-nominreq` | 0x004016A9 (fn 0x00401654) | Skip the minimum-spec check. |
| `-nopopups`, `-bgtex`, `-redirect`, `-nomutexchecks`, `-buildserver` | not yet located | |
| `-tugboat` | not yet located | Flagship's codename for the Mythos engine; probably inert here. |

**To verify.** Decompile the two known parse sites; locate the other six.
**Effort.** Low.

---

## D. `d3dx9_34` / `D3DX9_42` mismatch

**What.** The exe imports `d3dx9_34.dll`, but the install ships
`D3DX9_42.dll` (2009). Nothing in `bin/` provides `d3dx9_34`.

**Evidence.** Confirmed from the import table and the directory listing.

**Why it matters.** Under Proton that import resolves from somewhere else
entirely. Worth confirming it is not silently falling back to something
degraded, and worth understanding why a DLL the exe never imports is shipped.

**To verify.** Check what actually satisfies the import at runtime — the
Proton log from a normal session should show it.
**Effort.** Low. **Probably harmless**, but it is a loose end.

---

## E. Dead weight in the install

**What.** `libmysql.dll` (2006) ships with a single-player client — leftover
from the MMO-era codebase. `dpvs.dll` (2007) and `umbra.dll` (2009) both
ship, and dPVS is Umbra's predecessor.

**Evidence.** Observation.

**Why it matters.** Low priority and possibly zero-impact — but if `libmysql`
is genuinely unreferenced in the SP client, that is worth knowing, and if it
*is* referenced, that is worth knowing a great deal more.

**To verify.** Check whether the exe imports from either at all (it imports
`libmysql`, per the import table — so this needs an actual look, not an
assumption).
**Effort.** Low.

---

## F. If this becomes a real unofficial patch

Architectural decisions to make **early**, because they are painful to
retrofit:

1. **Modular from day one.** One ini next to the DLL, one toggle per fix,
   every fix independently disableable. A bad patch must never cost someone
   the whole thing.
2. **Keep the hash guard, extend it.** Addresses are pinned to
   sha256 `401e011d…`. If Steam ever ships a new build, every address is
   wrong; the DLL must keep refusing rather than corrupting a new binary.
   Consider per-fix pattern matching (as augmentrex does) so individual
   fixes can survive a rebuild.
3. **Instrumentation stays, compiled out.** The same source builds the
   diagnostic and the shipping DLL. That is what makes regressions
   diagnosable in the field.
4. **Never ship a patched exe.** Unchanged from the original rules: the
   deliverable is source plus a DLL, and deleting the DLL restores the game.
