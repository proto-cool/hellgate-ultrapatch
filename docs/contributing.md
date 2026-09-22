# Working on this project

## Rules

- **A change is not done until it is tested in the game.** `make` builds
  *and* installs the DLL; `make shaders` builds and installs the effects.
  The game must be restarted to load a new DLL.
- **Stock by default.** A visual or behavioural change ships behind a panel
  control that starts at the stock behaviour, with a counter or status line
  so it can be A/B'd live. Shader settings must reproduce stock at zero;
  `make matcheck` proves it.
- **Verify bytes before patching.** Every address in `src/target.h` was
  recovered statically. Each hook or patch checks the bytes it expects
  before touching them. Never patch a shared constant: `0xa0086c` (5.0) has
  102 readers, so the zoom limit was changed by repointing one instruction's
  operand instead.
- **Record experiments in the journal** ([journal.md](journal.md)): the
  hypothesis, the log line that would confirm or refute it, the run, the
  conclusion. Refuted ideas stay in.
- **No game content in the repository.** No extracted data, no decompiled
  code (`make decomp` writes to the gitignored `decomp/`), no Havok code.

## Build details

- 32-bit MinGW, `-O2 -fno-omit-frame-pointer` (the stack walker needs the
  EBP chain), no `-msse`: inline assembly that calls game code cannot list
  XMM clobbers.
- Game-independent logic (panel layout, camera math, UI) is kept free of
  Windows headers so `test/ui.c` can run it natively: `make test`.
- Shader tooling runs under Wine in the `dev` toolbox; see
  [graphics.md](graphics.md).

## Reverse engineering

- A headless Ghidra project of the executable is expected at
  `~/ghidra_proj` (`GHIDRA=` and `GPROJ=` override the Makefile's paths).
- `make codemap` names functions from the exe's assert strings and writes
  [codemap/](codemap/) (functions grouped by source file). `make decomp
  F="name addr ..."` decompiles functions into `decomp/`.
- `tools/re/` has scripts for pattern search, call graphs, cross-references,
  RTTI and string references; they read the exe from `HG_EXE`
  (`tools/re/hgpaths.py`).
- The log is `bin/hellgate_rays.log`; the previous session's is `.log.1`,
  and sessions that recorded a stall are kept as `hellgate_rays.spike-*.log`.

## Commits

Local history is linear on `master`. Commit messages say what changed and
why. Each commit should build and pass `make test`.
