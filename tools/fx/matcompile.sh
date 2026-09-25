#!/bin/bash
# Compile the shader variants of one or more material effects, in parallel
# and incrementally. Run in the dev toolbox:
#   tools/fx/matcompile.sh <effect>:<family> ...     e.g. actoroutdoor30:actor
# For each: tools/fx/mkmat.py plan lists the variants whose sources changed
# (build/mat/<effect>/batch.txt), all of them are split into chunks across
# the CPU cores and compiled by fxcomp.exe -batch, one Wine process per
# chunk. Output: build/mat/<effect>/fx/*.fxo. Nothing to do is a no-op.
set -e
cd "$(dirname "$0")/../.."
export WINEPREFIX=${WINEPREFIX:-/tmp/hg-wine} WINEDEBUG=-all
DX=build/shaders/d3dx9_34.dll
JOBS=${JOBS:-$(nproc)}
# the user tests in the game while this runs: at full width on every core
# it lagged the game unplayably (2026-09-24). Lowest priority always, and a
# third of the cores while the game is up.
if pgrep -fi 'hellgate_london.*[.]exe' >/dev/null 2>&1 && [ -z "${JOBS_FIXED:-}" ]; then
    JOBS=$(( $(nproc) / 3 )); [ "$JOBS" -lt 2 ] && JOBS=2
    echo "matcompile: the game is running; $JOBS workers at low priority"
fi
make -s build/fxcomp.exe
Q=build/mat/queue; rm -rf "$Q"; mkdir -p "$Q"
for pair in "$@"; do
    eff=${pair%%:*}; fam=${pair##*:}; M=build/mat/$eff
    python3 tools/fx/mkmat.py plan "build/shaders/hg/data/effects/dx9/$eff.fxo" "$fam" "$M"
    # one queue per family (the batch compiles a single source file)
    sed "s|^fx/|../$M/fx/|" "$M/batch.txt" >> "$Q/$fam.all"
done
n=0
for all in "$Q"/*.all; do
    [ -s "$all" ] || continue
    fam=$(basename "$all" .all)
    lines=$(wc -l < "$all"); per=$(( (lines + JOBS - 1) / JOBS )); [ "$per" -lt 4 ] && per=4
    split -l "$per" -d -a 3 "$all" "$Q/$fam.chunk."
    n=$((n + lines))
done
[ "$n" = 0 ] && { echo "matcompile: all variants up to date"; exit 0; }
echo "matcompile: $n variants on $JOBS workers"
ls "$Q"/*.chunk.* | xargs -P "$JOBS" -I{} sh -c '
    fam=$(basename {} | cut -d. -f1)
    cd shaders && nice -n 19 wine ../build/fxcomp.exe ../'"$DX"' $fam.fx -batch ../{} > ../{}.log 2>&1 || true'
# a variant compiled iff its .fxo exists; report the first errors otherwise
missing=0
for pair in "$@"; do
    M=build/mat/${pair%%:*}
    while read -r out _; do [ -f "$M/fx/$(basename "$out")" ] || missing=$((missing + 1)); done < "$M/batch.txt"
done
if [ "$missing" != 0 ]; then
    cat "$Q"/*.log | grep -a "error" | sort | uniq -c | sort -rn | head -10
    echo "matcompile: $missing variants failed" >&2; exit 1
fi
