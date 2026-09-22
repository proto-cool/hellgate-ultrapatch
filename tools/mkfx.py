#!/usr/bin/env python3
"""
Build the override effects: the game's actor materials plus an additive
per-pixel point-light pass (notes/graphics-plan.md step 1).

    python3 tools/mkfx.py <extracted-root> <shader-bin-dir> <override-root>

For each of actoroutdoor30.fxo / actorindoor30.fxo, every feature
combination gets one exact five-light technique ("<name>_pl5"):
the stock pass (shared shader objects) plus, when N exceeds the stock count,
a "Lights" pass that adds slots [stock, N) per pixel with ONE:ONE blending
and no Z write. See build() for why every count needs an exact match.

Shader blobs come from tools/shaders/actor_lights.fx compiled by the
Microsoft effect compiler (tools/build_shaders.sh) as
al_s{0,1}n{0,1}p{0,1}f{0,2}c{1..5}.{vs,ps}.bin.
"""
import copy
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import hgfx  # noqa: E402

TARGETS = [
    ("data\\effects\\dx9\\actoroutdoor30.fxo", None),
    ("data\\effects\\dx9\\actorindoor30.fxo", None),
]

# Render states of the additive pass. Only states the engine itself re-sets
# per mesh (blend enable and factors, Z write) may appear here: a state D3DX
# leaves behind that the engine never touches leaks into every later draw.
# The first build also set COLORWRITEENABLE=7, and particle glow -- which
# lives in the alpha channel -- vanished from the whole scene. The pass
# outputs alpha 0 with ONE:ONE blending, so destination alpha is preserved
# without touching the write mask.
PASS_STATES = [
    ("D3DRS_ALPHABLENDENABLE", 2, 1),
    ("D3DRS_SRCBLEND", 2, 2),          # D3DBLEND_ONE
    ("D3DRS_DESTBLEND", 2, 2),         # D3DBLEND_ONE
    ("D3DRS_ZWRITEENABLE", 2, 0),
]


def anno_value(t, name):
    for a in t["annotations"]:
        if a.name == name:
            return a.value[0] if a.value else 0
    return 0


def combo_key(t):
    """All annotations except PointLights, as a hashable key."""
    return tuple((a.name, tuple(a.value or [])) for a in t["annotations"] if a.name != "PointLights")


def scalar_param(ptype, value):
    p = hgfx.Param()
    p.type, p.cls, p.name, p.semantic = ptype, 0, "", ""
    p.elements, p.rows, p.cols, p.nmem = 0, 1, 1, 0
    p.members, p.annotations, p.sampler_states = [], [], []
    p.flags, p.object_id = 0, None
    p.value = [value]
    return p


def object_param(ptype, oid):
    p = scalar_param(ptype, 0)
    p.cls, p.value, p.object_id = 4, None, oid
    return p


def build(eff, blobs, next_oid):
    """
    For every feature combination in the effect and every light count 1..5
    that has no stock technique, add an exact-match technique. The engine's
    lookup tries an exact 16-byte match first and otherwise scores only
    *missing* features, so without exact matches a request for, say, three
    lights ties between every five-light clone -- including ones with the
    wrong Skinned or NormalMap -- and the last one wins (build 2 drew a rigid
    shield with a skinned shader and lost the player's torso that way).

    Every pass of every clone gets its OWN shader objects (a copy of the
    blob). Sharing objects between techniques loads and validates, but D3DX
    then uploads the constants for none of the sharing techniques (fxload
    -bind: stock pass has WorldViewProjection at c180, every clone's passes
    have nothing), which drew models with garbage matrices. The files are
    bigger (tens of MB) and that is the price.
    """
    stock = list(eff.techniques)
    have = {(combo_key(t), anno_value(t, "PointLights")) for t in stock}
    added = 0

    # Our own effect parameter for the light pass (the engine ignores names
    # it does not know; the DLL sets it by name before the pass).
    if not any(p.name == "gvUltraLight" for p in eff.params):
        prm = hgfx.Param()
        prm.type, prm.cls, prm.name, prm.semantic = 3, 1, "gvUltraLight", ""
        prm.elements, prm.rows, prm.cols, prm.nmem = 0, 1, 4, 0
        prm.members, prm.annotations, prm.sampler_states = [], [], []
        prm.flags, prm.object_id, prm.value = 0, None, [1.0, 1.0, 1.0, 1.0]
        eff.params.append(prm)

    def new_object(ptype, blob):
        nonlocal next_oid
        prm = object_param(ptype, next_oid)
        eff.objects[next_oid] = {"param": prm, "data": blob, "state": True}
        next_oid += 1
        return prm

    def light_states(tag):
        states = []
        for kind, ptype, op in (("vs", 16, hgfx.ST_VS), ("ps", 15, hgfx.ST_PS)):
            blob = blobs[tag + "." + kind]
            states.append({"op": op, "index": 0, "param": new_object(ptype, blob), "usage": 0, "data": blob})
        for name, ptype, value in PASS_STATES:
            states.append({"op": hgfx.STATES.index(name), "index": 0,
                           "param": scalar_param(ptype, value), "usage": None, "data": None})
        return states

    for t in stock:
        key, p0 = combo_key(t), anno_value(t, "PointLights")
        sk, nm, sp = anno_value(t, "Skinned"), anno_value(t, "NormalMap"), anno_value(t, "Specular")
        # One clone per feature combination, at the engine's cap of 5 lights.
        # The DLL rewrites every lit request to exactly 5 (and clamps to the
        # stock count when lights are off), so every request has an exact
        # match and no other count is needed. Build 5 carried all of 1..5 and
        # its 18 MB effect took 2.4 s to create, twice per level: the hitches.
        for n in (5,):
            if (key, n) in have:
                continue
            have.add((key, n))
            nt = {"name": "%s_pl%d" % (t["name"], n), "annotations": copy.deepcopy(t["annotations"]),
                  "passes": [copy.deepcopy(t["passes"][0])]}
            for a in nt["annotations"]:
                if a.name == "PointLights":
                    a.value = [n]
            for st in nt["passes"][0]["states"]:
                if st["param"].object_id is not None:
                    # own copy of the stock pass's shader: see the docstring
                    blob = eff.objects[st["param"].object_id]["data"]
                    st["param"] = new_object(st["param"].type, blob)
                    st["usage"], st["data"] = 0, blob
            if n > p0:
                tag = "al_s%dn%dp%df%dc%d" % (sk, nm, sp, p0, n)
                nt["passes"].append({"name": "Lights", "annotations": [], "states": light_states(tag)})
            eff.techniques.append(nt)
            added += 1
    return added, next_oid


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    root, bindir, out = sys.argv[1:4]
    blobs = {}
    for f in os.listdir(bindir):
        if f.startswith("al_") and f.endswith(".bin"):
            blobs[f[:-4]] = open(os.path.join(bindir, f), "rb").read()
    for rel, first in TARGETS:
        src = os.path.join(root, *rel.split("\\"))
        eff = hgfx.parse_effect(open(src, "rb").read())
        next_oid = max(eff.objects) + 1
        added, _ = build(eff, blobs, next_oid)
        data = hgfx.serialize(eff)
        dst = os.path.join(out, *rel.split("\\"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(data)
        # self-check: the file must parse back with the added techniques
        chk = hgfx.parse_effect(data)
        print("%s: +%d techniques (%d total), %d bytes -> %s" % (rel, added, len(chk.techniques), len(data), dst))


if __name__ == "__main__":
    main()
