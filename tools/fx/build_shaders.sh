#!/bin/bash
# Build and install the override effects. Run inside the dev toolbox:
#   toolbox run -c dev tools/fx/build_shaders.sh
#
# Steps: extract the stock effects -> rebuild the six material effects from
# our own source (shaders/*.hlsl via mkmat.py, which also adds the actor
# effects' single-pass _pl5 techniques; parity with stock is checked by
# tools/fx/matcheck.sh, not here) -> validate with the game's D3DX
# (fxload) -> copy into <game>/override.
set -e
cd "$(dirname "$0")/../.."
GAME=${GAME:-$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London}
PFX=${PFX:-$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/compatdata/939520/pfx}
export WINEPREFIX=${WINEPREFIX:-/tmp/hg-wine}
export WINEDEBUG=-all
W=build/shaders
mkdir -p "$W/hg" "$W/fx" "$W/bin" "$W/out" "$WINEPREFIX"
DX="$W/d3dx9_34.dll"; cp "$PFX/drive_c/windows/syswow64/d3dx9_34.dll" "$DX"
[ -f "$W/hg/data/effects/dx9/actoroutdoor30.fxo" ] || python3 tools/data/hgdat.py extract "$W/hg" 'effects\dx9\actor'
[ -f "$W/hg/data/effects/dx9/backgroundoutdoor30.fxo" ] || python3 tools/data/hgdat.py extract "$W/hg" 'effects\dx9\background'
make -s build/fxcomp.exe build/fxload.exe
# our material shaders, in place of the stock blobs (plan step 7)
ROOT="$W/hg"
if [ "${MAT:-1}" = 1 ]; then
    ROOT="$W/mat"; mkdir -p "$ROOT/data/effects/dx9"
    PAIRS="actoroutdoor30:actor actorindoor30:actor backgroundoutdoor30:background backgroundindoor30:background
           backgroundoutdoorprop30:background backgroundindoorprop30:background"
    tools/fx/matcompile.sh $PAIRS
    for pair in $PAIRS; do
        eff=${pair%%:*}
        python3 tools/fx/mkmat.py build "$W/hg/data/effects/dx9/$eff.fxo" ${pair##*:} build/mat/$eff "$ROOT/data/effects/dx9/$eff.fxo"
    done
fi
# our rebuilt effects are the override set (the actor ones carry the
# single-pass _pl5 point-light techniques, see mkmat.py)
rm -rf "$W/out"; mkdir -p "$W/out/data/effects/dx9"
cp "$ROOT"/data/effects/dx9/*.fxo "$W/out/data/effects/dx9/"
# nothing is installed unless every effect loads and validates with the game's D3DX
if ! wine build/fxload.exe "$DX" "$W/out/data/effects/dx9/"*.fxo > "$W/fxload.log" 2>/dev/null; then
    grep -v "^device\|took" "$W/fxload.log"; echo "fxload rejected an effect; not installing" >&2; exit 1
fi
grep -v "^device\|took" "$W/fxload.log"
mkdir -p "$GAME/override/data/effects/dx9"
cp "$W/out/data/effects/dx9/"*.fxo "$GAME/override/data/effects/dx9/"
echo "installed -> $GAME/override/data/effects/dx9/"
