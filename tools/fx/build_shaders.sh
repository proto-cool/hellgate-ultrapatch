#!/bin/bash
# Build and install the override effects. Run inside the dev toolbox:
#   toolbox run -c dev tools/fx/build_shaders.sh
#
# Steps: extract the stock effects -> rebuild the six material effects from
# our own source (shaders/*.hlsl via mkmat.py, which also adds the actor
# effects' single-pass _pl5 techniques; parity with stock is checked by
# tools/fx/matcheck.sh, not here) -> validate with the game's D3DX
# (fxload) -> copy into <game>/override. The post-process effects (SMAA, AO)
# go to <game>/override/ultra.
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
# our own post-process effects (src/postfx.c loads them from override/ultra):
# SMAA compiles next to a copy of the reference SMAA.hlsl (ref/smaa, MIT)
P=build/postfx; mkdir -p "$P" "$W/out/ultra"
tr -d '\r' < ref/smaa/SMAA.hlsl > "$P/SMAA.hlsl"
for fx in smaa ao cas fog; do
    cp "shaders/$fx.fx" "$P/"
    (cd "$P" && wine ../fxcomp.exe ../shaders/d3dx9_34.dll "$fx.fx" "$fx.fxo") | grep -v "^$"
    cp "$P/$fx.fxo" "$W/out/ultra/"
done
# soft particles: our shaders swapped into the stock particle.fxo
[ -f "$W/hg/data_common/effects/dx9/particle.fxo" ] || python3 tools/data/hgdat.py extract "$W/hg" 'effects\dx9\particle'
mkdir -p "$W/out/data_common/effects/dx9"
cp shaders/particle.fx "$P/"
(cd "$P" && wine ../fxcomp.exe ../shaders/d3dx9_34.dll particle.fx particle_ours.fxo) | grep -v "^$"
python3 tools/fx/mkparticle.py "$W/hg/data_common/effects/dx9/particle.fxo" "$P/particle_ours.fxo" \
    "$W/out/data_common/effects/dx9/particle.fxo"
# nothing is installed unless every effect loads and validates with the game's D3DX
if ! wine build/fxload.exe "$DX" "$W/out/data/effects/dx9/"*.fxo "$W/out/data_common/effects/dx9/"*.fxo "$W/out/ultra/"*.fxo > "$W/fxload.log" 2>/dev/null; then
    grep -v "^device\|took" "$W/fxload.log"; echo "fxload rejected an effect; not installing" >&2; exit 1
fi
grep -v "^device\|took" "$W/fxload.log"
mkdir -p "$GAME/override/data/effects/dx9"
cp "$W/out/data/effects/dx9/"*.fxo "$GAME/override/data/effects/dx9/"
mkdir -p "$GAME/override/data_common/effects/dx9" "$GAME/override/ultra"
cp "$W/out/data_common/effects/dx9/"*.fxo "$GAME/override/data_common/effects/dx9/"
cp "$W/out/ultra/"*.fxo "$GAME/override/ultra/"
echo "installed -> $GAME/override/data/effects/dx9/ and override/ultra/"
