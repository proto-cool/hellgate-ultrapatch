#!/usr/bin/env python3
"""
Rebuild a material effect with our own shaders (docs/graphics-plan.md step 7).

    python3 tools/fx/mkmat.py plan  <stock.fxo> <family> <workdir>
    python3 tools/fx/mkmat.py build <stock.fxo> <family> <workdir> <out.fxo>

Every stock technique is one point in a feature grid, announced by its
annotations. `plan` turns each technique's annotations into preprocessor
defines for shaders/<family>.hlsl and writes <workdir>/batch.txt, one
compile per distinct define set, for `fxcomp.exe -batch`. `build` lifts the
compiled vertex and pixel shaders and swaps them into the stock effect's
shader objects in place: technique names, annotations, pass states and the
parameter block stay byte-for-byte stock, so the engine's technique lookup
and caches cannot tell the difference.

Before swapping, every constant and sampler our shader (and its preshader)
reads must be a parameter of the stock effect with the same name; anything
else would fail D3DXCreateEffect in the game.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import hgfx  # noqa: E402

# annotation -> define; value used as is
FAMILIES = {
    "actor": {
        "Indoor": "INDOOR", "PointLights": "POINTLIGHTS", "ShadowType": "SHADOWTYPE",
        "Skinned": "SKINNED", "NormalMap": "NORMALMAP", "SelfIllum": "SELFILLUM",
        "Specular": "SPECULAR", "CubeEnvMap": "CUBEENVMAP", "Scatter": "SCATTER",
        "ScrollUV": "SCROLLUV",
    },
    "background": {
        "Indoor": "INDOOR", "LightMap": "LIGHTMAP", "SphericalHarmonics": "SH",
        "PointLights": "POINTLIGHTS", "ShadowType": "SHADOWTYPE", "DiffuseMap2": "DIFFUSEMAP2",
        "NormalMap": "NORMALMAP", "SelfIllum": "SELFILLUM", "Specular": "SPECULAR",
        "CubeEnvMap": "CUBEENVMAP", "ScrollUV": "SCROLLUV",
    },
}
# Annotations the family's source does not implement; a technique that sets
# one is left stock (and reported) rather than drawn wrong.
UNSUPPORTED = {
    "actor": ("SpotLight", "SpecularLUT", "WorldSpaceLight", "SphereEnvMap"),
    "background": ("SpotLight", "SpecularLUT", "WorldSpaceLight", "SphereEnvMap", "Skinned", "Scatter"),
}


def annos(t):
    return {a.name: (a.value[0] if a.value else 0) for a in t["annotations"]}


def variant(t, family):
    a = annos(t)
    if any(a.get(k) for k in UNSUPPORTED[family]):
        return None
    return tuple(sorted((d, int(a.get(k, 0))) for k, d in FAMILIES[family].items()))


def tag(v):
    return "v_" + "_".join("%s%d" % (d[:3].lower(), n) for d, n in v)


def source_hash(family):
    """Hash of everything a variant's compile reads: the family source, its
    wrapper and every shared include."""
    h = hashlib.sha1()
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "shaders")
    for f in sorted(os.listdir(d)):
        if f in (family + ".hlsl", family + ".fx") or (f.endswith(".hlsl") and f not in
                                                       ("actor.hlsl", "background.hlsl", "actor_lights.hlsl")):
            h.update(f.encode() + open(os.path.join(d, f), "rb").read())
    return h.hexdigest()


def plan(stock, family, work):
    """Write batch.txt with the variants whose compiled .fxo is missing or
    was built from different sources (a .key file beside each .fxo records
    the source hash and defines it came from)."""
    eff = hgfx.parse_effect(open(stock, "rb").read())
    os.makedirs(os.path.join(work, "fx"), exist_ok=True)
    seen, skipped = {}, 0
    for t in eff.techniques:
        v = variant(t, family)
        if v is None:
            skipped += 1
            continue
        seen.setdefault(v, tag(v))
    src = source_hash(family)
    stale = 0
    with open(os.path.join(work, "batch.txt"), "w") as f:
        for v, name in sorted(seen.items(), key=lambda kv: kv[1]):
            defs = " ".join("%s=%d" % dv for dv in v)
            key = src + " " + defs
            fxo, kf = (os.path.join(work, "fx", name + ext) for ext in (".fxo", ".key"))
            if os.path.exists(fxo) and os.path.exists(kf) and open(kf).read() == key:
                continue
            if os.path.exists(fxo):
                os.remove(fxo)          # a failed compile must not leave the old one looking fresh
            open(kf, "w").write(key)
            f.write("fx/%s.fxo %s\n" % (name, defs))
            stale += 1
    print("%s: %d techniques, %d variants, %d to compile, %d left stock" %
          (os.path.basename(stock), len(eff.techniques), len(seen), stale, skipped))


def lift(path):
    eff = hgfx.parse_effect(open(path, "rb").read())
    out = {}
    for st in eff.techniques[0]["passes"][0]["states"]:
        if st["op"] == hgfx.ST_VS:
            out["vs"] = st["data"]
        elif st["op"] == hgfx.ST_PS:
            out["ps"] = st["data"]
    return out


def names_read(blob):
    """Parameter names a shader blob reads: its CTAB plus its preshader's inputs."""
    c = hgfx.ctab(blob)
    names = {name.split("[")[0] for name, _, _, _, _ in (c[2] if c else [])}
    return names | hgfx.preshader(blob, inputs=True)


# Our runtime knobs (shaders/ultra.hlsl), appended to every rebuilt
# effect as float4 parameters defaulting to zero = the stock look. Only the
# DLL sets them; the engine ignores names it does not know.
ULTRA_PARAMS = ("gvUltraMat", "gvUltraShadow", "gvUltraLook")


def add_float4(eff, name):
    if any(p.name == name for p in eff.params):
        return
    prm = hgfx.Param()
    prm.type, prm.cls, prm.name, prm.semantic = 3, 1, name, ""
    prm.elements, prm.rows, prm.cols, prm.nmem = 0, 1, 4, 0
    prm.members, prm.annotations, prm.sampler_states = [], [], []
    prm.flags, prm.object_id, prm.value = 0, None, [0.0, 0.0, 0.0, 0.0]
    eff.params.append(prm)


def build(stock, family, work, out):
    eff = hgfx.parse_effect(open(stock, "rb").read())
    for name in ULTRA_PARAMS:
        add_float4(eff, name)
    params = {p.name for p in eff.params}
    blobs, swapped, kept, bad = {}, 0, 0, 0
    for t in eff.techniques:
        v = variant(t, family)
        if v is None:
            kept += 1
            continue
        if v not in blobs:
            blobs[v] = lift(os.path.join(work, "fx", tag(v) + ".fxo"))
            for kind, b in blobs[v].items():
                missing = sorted(n for n in names_read(b) if n not in params)
                if missing:
                    print("%s %s reads parameters the effect lacks: %s" % (tag(v), kind, ", ".join(missing)))
                    bad += 1
        for st in t["passes"][0]["states"]:
            if st["param"].object_id is None or st["op"] not in (hgfx.ST_VS, hgfx.ST_PS):
                continue
            blob = blobs[v]["vs" if st["op"] == hgfx.ST_VS else "ps"]
            eff.objects[st["param"].object_id]["data"] = blob
            st["data"] = blob
        swapped += 1
    if bad:
        sys.exit("refusing to write %s: %d shaders read unknown parameters" % (out, bad))
    data = hgfx.serialize(eff)
    hgfx.parse_effect(data)      # must parse back
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "wb").write(data)
    print("%s: %d techniques rebuilt, %d left stock -> %s (%d bytes)" %
          (os.path.basename(stock), swapped, kept, out, len(data)))


def main():
    if len(sys.argv) < 5 or sys.argv[1] not in ("plan", "build") or sys.argv[3] not in FAMILIES:
        sys.exit(__doc__)
    if sys.argv[1] == "plan":
        plan(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        if len(sys.argv) != 6:
            sys.exit(__doc__)
        build(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])


if __name__ == "__main__":
    main()
