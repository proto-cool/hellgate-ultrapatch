# Marcus Fidelius Ultrapatch

Fixes and a graphics overhaul for the 2018 Steam release of *Hellgate:
London* (single player, appid 939520), delivered as a proxy `version.dll`
and a set of replacement shader effects installed next to the game. The
only game file changed is the Steam launcher (17 bytes, so it starts the
game without its Play dialog), and uninstalling puts it back. The main
menu shows the name and version (`v0.<commit count>`) in its bottom right
corner.

## What it does

| Area | State |
|---|---|
| **Soft shadows (PCSS)** | Contact-hardening sun shadows on every material, with separate indoor and outdoor sun size. |
| **Shadow fill** | Dynamic shadows remove only the sun's light, so characters' shadows match the world's baked shadows instead of going black. |
| **Look controls** | Ambient fill, fog start and sun strength, live, with a "2007 look" preset for the flat 2018 lighting data. |
| **Per-pixel point lights** | Up to five spell and torch lights per pixel, on the level and on characters, with smooth falloff. |
| **Post and atmosphere** | HDR with auto exposure, bloom and a colour grade; volumetric fog with sun shafts and light halos; SMAA, sharpening, ambient occlusion with a colour bounce; soft and lit particles. |
| **HD cinematics** | The story movies and end credits from the 2007 disc at 1920×1088 instead of 640×368, the main menu background in HD, no HanbitSoft logo at start-up, and English fixes for text the 2018 build left as placeholders or mismatched (`make paks`, needs the 2007 disc; [docs/reference/2007-vs-2018.md](docs/reference/2007-vs-2018.md)). |
| **Own material shaders** | All 1,482 material shader variants rebuilt from our HLSL, pixel-identical to stock until a setting is changed. The base for everything above. |
| **Action camera** | Over-the-shoulder offset, true orbit, own camera collision, melee impulse. |
| **Dev panel** | In-game overlay (Shift+\`) with all of the above as live controls, plus memory and physics tools. |
| **The "1 FPS" stall** | Root cause traced to Havok continuous collision detection doubling the raycast load ([docs/fps-bug.md](docs/fps-bug.md)). The stall does not reproduce under DXVK; no fix is shipped yet. |

The graphics features are on by default.
Each one switched off gives the stock rendering for it, and
Ctrl+Alt+Shift+S holds the whole stock view for comparison.

## Quick start

Needs Linux with Proton, DXVK (Proton's default) and a Fedora `toolbox`
named `dev` with `mingw32-gcc mingw32-binutils wine python3` for building.
Tested only under Proton; Windows is untested.

```sh
git clone --recursive https://github.com/proto-cool/hellgate-ultrapatch.git
cd hellgate-ultrapatch
toolbox run -c dev make             # build and install bin/version.dll
make shaders                        # build and install the shader effects (~25 s)
make paks                           # HD movies, no Hanbit logo, English fixes (movies need the 2007 disc)
```

Steam launch options:

```
WINEDLLOVERRIDES="version=n,b" %command%
```

In game, **Shift+\`** opens the panel (set `HG_PANEL=1` or create
`bin/hellgate_panel.on` first). The graphics controls are on the **Model**
tab.

The install also patches the launcher to skip its Play dialog. To uninstall,
run `toolbox run -c dev make uninstall` (or delete `bin/version.dll` and the
`override/` folder, and let Steam verify the game files to restore the
launcher).

## Documentation

- [docs/install.md](docs/install.md) — building, installing, launch options, every flag file and environment variable
- [docs/panel.md](docs/panel.md) — the dev panel, tab by tab
- [docs/graphics.md](docs/graphics.md) — the shader pipeline: how the materials are rebuilt, tested and tuned
- [docs/graphics-plan.md](docs/graphics-plan.md) — the graphics roadmap
- [docs/fps-bug.md](docs/fps-bug.md) — the 1 FPS investigation: findings, log format, repro harness
- [docs/journal.md](docs/journal.md) — the full investigation journal, every experiment in order
- [docs/backlog.md](docs/backlog.md) — leads not yet worked
- [docs/contributing.md](docs/contributing.md) — working rules, build details, reverse-engineering setup
- [docs/reference/](docs/reference/) — binary facts, the stock shaders, data tunables, script actions, other patches, community research

## Layout

```
src/            the proxy DLL: proxy.c forwards, hook.c instruments, gfxprobe.c
                graphics, panel*/ui*/overlay.c the panel, the rest one feature each
shaders/        our material shader sources (HLSL)
tools/fx/       shader tooling: compiler, loader, parity harness, effect writer
tools/re/       reverse-engineering scripts and headless Ghidra scripts
tools/data/     readers for the game's paks, cooked data and key bindings
test/           offline tests (panel UI, D3D9 vtable, DLL load)
docs/           documentation; docs/codemap/ lists the exe's functions by source file
ref/            submodules: MinHook (build dependency), augmentrex and Reanimator-steam (reference)
```

## Notes

- No game data, Havok code or decompiled game code is in this repository.
  `make decomp` writes decompiled functions to a local, gitignored
  `decomp/` for research.
- Single player only. The panel's memory and spawn tools poke a live
  process and can damage a save; the graphics and animation features do not
  touch game state.

## License

MIT; see [LICENSE](LICENSE). The submodules under `ref/` keep their own
licenses. *Hellgate: London* and its data belong to their owners; nothing
from the game is included here.
