#!/bin/bash
# Side-by-side disassembly of one technique, stock vs our rebuilt effect:
#   tools/fxcmp.sh <effect> <technique> <vs|ps>
# Reads build/shaders/hg/... (stock) and build/mat/<effect>/<effect>.fxo
# (tools/matcheck.sh output); prints each shader's constant table, preshader
# and assembly. Blobs and .asm land in build/mat/{stock,ours}.<vs|ps>.*
set -e
cd "$(dirname "$0")/.."
EFF=$1; TECH=$2; K=$3
python3 - "$EFF" "$TECH" "$K" <<'PY'
import sys; sys.path.insert(0, 'tools'); import hgfx
eff, tech, k = sys.argv[1:4]
for tag, path in (('stock', 'build/shaders/hg/data/effects/dx9/%s.fxo' % eff), ('ours', 'build/mat/%s/%s.fxo' % (eff, eff))):
    e = hgfx.parse_effect(open(path, 'rb').read())
    t = [t for t in e.techniques if t['name'] == tech][0]
    for st in t['passes'][0]['states']:
        if st['op'] == (hgfx.ST_VS if k == 'vs' else hgfx.ST_PS):
            open('build/mat/%s.%s.bin' % (tag, k), 'wb').write(st['data'])
PY
toolbox run -c dev bash -lc "cd ~/projects/hellgate-ultrapatch/build/mat && WINEPREFIX=\${WINEPREFIX:-/tmp/hg-wine} WINEDEBUG=-all wine ../fxdis.exe d3dx9_43.dll stock.$K.bin ours.$K.bin" 2>/dev/null
for t in stock ours; do
    echo "=== $t"
    python3 tools/hgfx.py ctab build/mat/$t.$K.bin | tail -n +2
    python3 tools/hgfx.py pres build/mat/$t.$K.bin | sed 's/^/pres: /'
    cat build/mat/$t.$K.asm
done
