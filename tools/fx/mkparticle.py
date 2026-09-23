#!/usr/bin/env python3
"""
Soft particles: swap our shaders (shaders/particle.fx, compiled) into the
stock particle.fxo, like mkmat.py does for the materials.

    python3 tools/fx/mkparticle.py <stock particle.fxo> <ours.fxo> <out.fxo>

Technique names, annotations, pass states and parameters stay stock; three
stock techniques' shaders get ours, and every other pass with the same stock
shader (the effect repeats them in separate objects) gets the same one, in
its own object. Adds the gvUltraSoft knob (zero = stock).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import hgfx  # noqa: E402
from mkmat import add_float4, names_read  # noqa: E402

# stock technique -> our technique (pass 0)
MAP = {
    "TVertexAndPixelShader0": "TPlain",
    "TVertexAndPixelShaderAdditive": "TAdditive",
    "TVertexAndPixelShaderAddGlowGlowConstant": "TAddGlow",
}
UNBOUND = {"SoftDepthSampler"}     # the DLL binds it on stage 1


def shaders(eff, name):
    t = next(t for t in eff.techniques if t["name"] == name)
    out = {}
    for st in t["passes"][0]["states"]:
        if st["op"] in (hgfx.ST_VS, hgfx.ST_PS):
            out[st["op"]] = (st["param"].object_id, st["data"])
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stock = hgfx.parse_effect(open(sys.argv[1], "rb").read())
    ours = hgfx.parse_effect(open(sys.argv[2], "rb").read())
    add_float4(stock, "gvUltraSoft")
    params = {p.name for p in stock.params} | UNBOUND
    swap = {}                                   # stock blob -> our blob
    for sname, oname in MAP.items():
        st, ou = shaders(stock, sname), shaders(ours, oname)
        for op, (_, old) in st.items():
            blob = ou[op][1]
            missing = sorted(n for n in names_read(blob) if n not in params)
            if missing:
                sys.exit("%s reads parameters particle.fxo lacks: %s" % (oname, ", ".join(missing)))
            if old in swap and swap[old] != blob:
                sys.exit("one stock shader, two replacements (%s)" % oname)
            swap[old] = blob
    n = 0
    for t in stock.techniques:
        for p in t["passes"]:
            for st in p["states"]:
                if st["op"] in (hgfx.ST_VS, hgfx.ST_PS) and st["data"] in swap:
                    blob = swap[st["data"]]
                    st["data"] = blob
                    stock.objects[st["param"].object_id]["data"] = blob
                    n += 1
    data = hgfx.serialize(stock)
    hgfx.parse_effect(data)                     # must parse back
    open(sys.argv[3], "wb").write(data)
    print("particle.fxo: %d stock shaders replaced in %d pass states -> %s (%d bytes)" %
          (len(swap), n, sys.argv[3], len(data)))


if __name__ == "__main__":
    main()
