#!/bin/bash
# Build and install the override effects (per-pixel light pass on the actor
# materials). Run inside the dev toolbox:  toolbox run -c dev tools/fx/build_shaders.sh
#
# Steps: extract the stock effects -> rebuild the actor effects from our own
# source (shaders/actor.hlsl via mkmat.py; parity with stock is checked
# by tools/fx/matcheck.sh, not here; MAT=0 skips this and starts from stock) ->
# compile every light-pass variant as a tiny effect with Microsoft's effect
# compiler (d3dx9_34, from the Proton prefix) -> lift the shader blobs ->
# clone the techniques with the extra pass (mkfx.py) -> validate with the
# game's D3DX (fxload) -> copy into <game>/override.
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
( cd shaders
  for sk in 0 1; do for nm in 0 1; do for sp in 0 1; do for fl in 0 2; do
    for n in 1 2 3 4 5; do
      [ "$n" -gt "$fl" ] || continue
      tag=s${sk}n${nm}p${sp}f${fl}c${n}
      [ -f "../$W/fx/al_$tag.fxo" ] && continue
      wine ../build/fxcomp.exe "../$DX" actor_lights.fx "../$W/fx/al_$tag.fxo" \
          SKINNED=$sk NORMALMAP=$nm SPECULAR=$sp FIRST_LIGHT=$fl LIGHT_COUNT=$n > /dev/null
    done
  done; done; done; done )
python3 - "$W" <<'PY'
import sys, os; sys.path.insert(0, 'tools/fx'); import hgfx
w = sys.argv[1]
for f in sorted(os.listdir(w + '/fx')):
    eff = hgfx.parse_effect(open(w + '/fx/' + f, 'rb').read())
    for st in eff.techniques[0]['passes'][0]['states']:
        if st['op'] in (hgfx.ST_VS, hgfx.ST_PS):
            open('%s/bin/%s.%s.bin' % (w, f[:-4], 'vs' if st['op'] == hgfx.ST_VS else 'ps'), 'wb').write(st['data'])
PY
python3 tools/fx/mkfx.py "$ROOT" "$W/bin" "$W/out"
# the background effects get no light pass: our shaders as rebuilt above
if [ "${MAT:-1}" = 1 ]; then cp "$ROOT"/data/effects/dx9/background*.fxo "$W/out/data/effects/dx9/"; fi
# nothing is installed unless every effect loads and validates with the game's D3DX
if ! wine build/fxload.exe "$DX" "$W/out/data/effects/dx9/"*.fxo > "$W/fxload.log" 2>/dev/null; then
    grep -v "^device\|took" "$W/fxload.log"; echo "fxload rejected an effect; not installing" >&2; exit 1
fi
grep -v "^device\|took" "$W/fxload.log"
mkdir -p "$GAME/override/data/effects/dx9"
cp "$W/out/data/effects/dx9/"*.fxo "$GAME/override/data/effects/dx9/"
echo "installed -> $GAME/override/data/effects/dx9/"
