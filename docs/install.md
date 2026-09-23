# Building and installing

## Requirements

- The Steam release of *Hellgate: London* (appid 939520), run through
  Proton. The DLL checks the executable's size and SHA-256 and refuses to
  hook anything else.
- A Fedora `toolbox` named `dev` with `mingw32-gcc mingw32-binutils wine
  python3`. Everything is 32-bit: the target is a 2018 MSVC8 x86 PE.
- For the shader build: one Proton launch of the game first, so its prefix
  holds `d3dx9_34.dll` (the effect compiler the game itself uses).
- Submodules: `git submodule update --init`. `ref/minhook` is needed to
  build; the other two are reference only.

The default install path is the Flatpak Steam library; set `GAME=` on any
`make` command for another one.

## Build and install

```sh
toolbox run -c dev make        # build/version.dll -> $GAME/bin/, plus the test tools
toolbox run -c dev make test   # panel UI checks, run natively
make shaders                   # extract the stock effects, rebuild ours, validate, install to $GAME/override/
make matcheck                  # prove the rebuilt effects match stock pixel for pixel (~90 s)
```

`make` always installs: a DLL left in `build/` cannot be tested, and a
stale one in the game directory looks like a change that did not work.

Installing also patches the Steam launcher, `Hellgate.exe`, so it starts the
game straight away instead of showing its Play dialog (`tools/launcher.py`:
17 bytes, only on the known 2018 launcher). The launcher still starts Steam,
which the game itself never does.

## Launch options

```
WINEDLLOVERRIDES="version=n,b" %command%
```

The override is required: without it Proton loads its builtin
`version.dll`, ours is never mapped, and nothing reports it. The log,
`$GAME/bin/hellgate_rays.log`, then never appears.

DXVK (Proton's default) is the supported renderer. `PROTON_USE_WINED3D=1`
is only for reproducing the 1 FPS stall ([fps-bug.md](fps-bug.md)).

## Uninstall

```sh
toolbox run -c dev make uninstall   # or delete $GAME/bin/version.dll and $GAME/override/
```

`make uninstall` also puts the launcher's original bytes back. When deleting
by hand, run `python3 tools/launcher.py restore $GAME` or let Steam verify
the game files.

## Flag files

Empty files in the game's `bin/` directory, next to the DLL. Most are read
at startup.

| File | Effect |
|---|---|
| `hellgate_panel.on` | enable the dev panel (same as `HG_PANEL=1`) |
| `hellgate_rays.off` | load and forward, hook nothing |
| `hellgate_override.off` | ignore `override/`: stock shader effects |
| `hellgate_gfxprobe.off` | disable all graphics hooks and overrides |
| `hellgate_shadowtype2.off` | keep the engine's own shadow map type (PCSS needs the colour map the DLL selects by default) |
| `hellgate_smaa.off` | MSAA as the game sets it instead of SMAA; no scene depth, so no AO (the panel's Post tab writes it) |
| `hellgate_gfxprobe.frame` | capture the next frame's render-target changes to the log |

## Environment variables

Set in the launch options before `%command%`.

| Variable | Effect |
|---|---|
| `HG_PANEL=1` | the in-game dev panel |
| `HG_PANEL_HTTP=1` | also serve the panel on `http://127.0.0.1:7777/` (`HG_PANEL_PORT=n` to move it) |
| `HG_OVERLAY_OFF=1` | panel on, in-game overlay off (HTTP only) |
| `HG_FP_MELEE=1` | allow first person with melee weapons (also a panel toggle) |
| `HG_RAYS_LOG=path` | log file (default `bin/hellgate_rays.log`) |
| `HG_RAYS_DISABLE=1` | load and forward, hook nothing |
| `HG_RAYS_STACKDEPTH=n` | frames captured per raycast call site, 1–12 (default 12) |
| `HG_RAYS_SELFTEST=1` | run the address-space walk once at startup |
| `HG_SIM_TYPE=1` | force Havok's simulation type to DISCRETE. Experiment: it removes tunnelling protection everywhere |
| `HG_SPAWN_MULT=n` | repro harness: every real spawn becomes n. Destabilising on purpose |
| `HG_SPAWN_CAP=n` | extra spawns allowed per 100 ms window (default 200) |
| `HG_SPAWN_MONSTERS=1` | also multiply monster spawns |

## Offline check

Before involving the game:

```sh
toolbox run -c dev bash -lc 'cd build && WINEPREFIX=$HOME/.hgpfx WINEDLLOVERRIDES="version=n,b" wine host.exe'
cat build/hellgate_rays.log     # expect: refuses to hook, wrong sha256
```

That refusal is the correct result: `host.exe` is not the game.
