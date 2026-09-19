#!/usr/bin/env bash
# Which d3d9 implementation did Proton actually load this run?
# The Proton log is tens of MB, so only look at what matters.
LOG="$HOME/.var/app/com.valvesoftware.Steam/steam-939520.log"
[ -f "$LOG" ] || { echo "no Proton log at $LOG -- is PROTON_LOG=1 set?"; exit 1; }

echo "log: $(du -h "$LOG" | cut -f1), modified $(date -r "$LOG" '+%H:%M:%S')"

if grep -qm1 'info:  DXVK:' "$LOG"; then
    echo "RENDERER: DXVK  -> $(grep -m1 'info:  DXVK:' "$LOG" | sed 's/.*DXVK: //')"
    echo "  the bug is expected to be SUPPRESSED. Not the run you want."
elif grep -qm1 -i 'wined3d' "$LOG"; then
    echo "RENDERER: wined3d"
    echo "  no DXVK banner found. This is the run you want."
else
    echo "RENDERER: inconclusive -- neither a DXVK banner nor a wined3d mention."
fi

echo
echo "d3d9 load line:"
grep -m2 'Loaded .*d3d9\.dll' "$LOG" || echo "  (none seen yet)"
echo "our proxy:"
grep -m1 'Loaded .*version\.dll' "$LOG" || echo "  (not seen -- WINEDLLOVERRIDES missing?)"
