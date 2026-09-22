#!/bin/bash
# Rebuild one material family effect from tools/shaders/<family>.hlsl and
# check it against the stock effect pixel for pixel. Run in the dev toolbox:
#   toolbox run -c dev tools/matcheck.sh actoroutdoor30 actor [fxdiff args...]
# Output: build/mat/<effect>/<effect>.fxo, diff.txt, dump/ (failing images).
set -e
cd "$(dirname "$0")/.."
EFF=$1; FAM=$2; shift 2
export WINEPREFIX=${WINEPREFIX:-/tmp/hg-wine} WINEDEBUG=-all
DX=build/shaders/d3dx9_34.dll
STOCK=build/shaders/hg/data/effects/dx9/$EFF.fxo
W=build/mat/$EFF
make -s build/fxload.exe build/fxdiff.exe
rm -rf "$W/dump"; mkdir -p "$W/dump"
tools/matcompile.sh "$EFF:$FAM"
python3 tools/mkmat.py build "$STOCK" "$FAM" "$W" "$W/$EFF.fxo"
wine build/fxload.exe $DX "$W/$EFF.fxo" 2>/dev/null | grep -v "^device\|took"
# four seeds: camera light on/off x dim/bright lighting (see tools/fxdiff.c)
for SEED in 0 1 2 3; do
    wine build/fxdiff.exe $DX "$STOCK" "$W/$EFF.fxo" -seed $SEED -dump "$W/dump" "$@" 2>/dev/null > "$W/diff.s$SEED.txt" || true
    echo "seed $SEED: $(tail -1 "$W/diff.s$SEED.txt")"
done
cat "$W"/diff.s?.txt | grep "^DIFF" | sort -u > "$W/diff.txt" || true
