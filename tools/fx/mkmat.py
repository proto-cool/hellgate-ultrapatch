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
import copy
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


# Families that get our single-pass five-light techniques ("<name>_pl5",
# PointLights=5, PL_ULTRA in the source): one per feature combination, which
# the DLL asks for whenever a mesh has lights and the panel's per-pixel lights
# are on. The engine zero-pads unused light colours up to the technique's
# count and takes those lights out of SH. PL5=0 in the environment leaves them
# out (tools/fx/matcheck.sh: parity compares stock techniques only).
PL_FAMILIES = ("actor",)


def pl_enabled(family):
    return family in PL_FAMILIES and os.environ.get("PL5", "1") == "1"


def pl_variant(v):
    d = dict(v)
    d["POINTLIGHTS"] = 0
    d["PL_ULTRA"] = 1
    return tuple(sorted(d.items()))


def combo_key(t):
    """All annotations except PointLights: one _pl5 technique per key."""
    return tuple((a.name, tuple(a.value or [])) for a in t["annotations"] if a.name != "PointLights")


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
                                                       ("actor.hlsl", "background.hlsl")):
            h.update(f.encode() + open(os.path.join(d, f), "rb").read())
    return h.hexdigest()


def drawn(stock, eff, family):
    """Dev build (DRAWN=<bin/ultra_drawn.txt>, written by the DLL): the
    variants of the techniques the game has drawn, or None for all."""
    path = os.environ.get("DRAWN")
    if not path:
        return None
    if not os.path.exists(path):
        print("%s: no %s yet (play once with this DLL); compiling everything" % (os.path.basename(stock), path))
        return None
    me = os.path.basename(stock)[:-4]
    names = set()
    for line in open(path, errors="replace"):
        f = line.split()
        if len(f) == 2 and f[0] == me:
            names.add(f[1])
    want = set()
    for t in eff.techniques:
        v = variant(t, family)
        if v is None:
            continue
        if t["name"] in names:
            want.add(v)
        if t["name"] + "_pl5" in names and pl_enabled(family):
            want.add(pl_variant(v))
    return want


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
    if pl_enabled(family):
        for v in list(seen):
            pv = pl_variant(v)
            seen.setdefault(pv, tag(pv))
    src = source_hash(family)
    want = drawn(stock, eff, family)
    stale = held = 0
    with open(os.path.join(work, "batch.txt"), "w") as f:
        for v, name in sorted(seen.items(), key=lambda kv: kv[1]):
            defs = " ".join("%s=%d" % dv for dv in v)
            key = src + " " + defs
            fxo, kf = (os.path.join(work, "fx", name + ext) for ext in (".fxo", ".key"))
            if os.path.exists(fxo) and os.path.exists(kf) and open(kf).read() == key:
                continue
            if want is not None and v not in want:
                held += 1               # dev build: the last compile stands in (stock if none)
                continue
            if os.path.exists(fxo):
                os.remove(fxo)          # a failed compile must not leave the old one looking fresh
            open(kf, "w").write(key)
            f.write("fx/%s.fxo %s\n" % (name, defs))
            stale += 1
    print("%s: %d techniques, %d variants, %d to compile, %d left stock%s" %
          (os.path.basename(stock), len(eff.techniques), len(seen), stale, skipped,
           ", %d not drawn and out of date (dev build)" % held if held else ""))


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
ULTRA_PARAMS = ("gvUltraMat", "gvUltraShadow", "gvUltraLook", "gvUltraPL", "gvUltraAct", "gvUltraSurf", "gvUltraDetail", "gvUltraLM", "gvUltraPLS", "gvUltraPLS2", "gvUltraHDR", "gvUltraChar", "gvUltraVM")
# float4x4 knobs (same zero default), and samplers the DLL binds straight to
# a device stage, so they have no effect parameter of their own
ULTRA_MATRICES = {"background": ("gmUltraFine",),    # the DLL's cue: this effect reads the fine map
                  "actor": ("gmUltraNear", "gmUltraVM")}   # ... or the near map on characters; the view model's own map
ULTRA_SAMPLERS = ()
# samplers that get a real effect parameter (a texture the DLL sets through
# the effect), cloned from an existing one of the same kind: the game's D3DX
# crashed in BeginPass on shaders whose only unknown sampler was an
# unparameterised cube (UltraPLShadowSampler in the point-light, no-shadow-map
# variants; fxdiff, 2026-09-23), and the game itself crashed at the same
# spot (d3dx9_34+0x15384d) with the fine map's as the other unparameterised one
ULTRA_SAMPLER_PARAMS = (("UltraPLShadowSampler", "tUltraPLShadow", "CubeEnvironmentMapSampler", "tCubeEnvironmentMap"),
                        ("UltraFineSampler", "tUltraFine", "DiffuseMapSampler", "tDiffuseMap"),
                        # the view model's own shadow map (INTZ depth): a shadow
                        # map's sampler, so no sRGB read and no mipmaps
                        ("UltraVMShadowSampler", "tUltraVM", "ColorShadowMapSampler", "tShadowMap"))


def add_float4(eff, name):
    if any(p.name == name for p in eff.params):
        return
    prm = hgfx.Param()
    prm.type, prm.cls, prm.name, prm.semantic = 3, 1, name, ""
    prm.elements, prm.rows, prm.cols, prm.nmem = 0, 1, 4, 0
    prm.members, prm.annotations, prm.sampler_states = [], [], []
    prm.flags, prm.object_id, prm.value = 0, None, [0.0, 0.0, 0.0, 0.0]
    eff.params.append(prm)


def add_float4x4(eff, name):
    if any(p.name == name for p in eff.params):
        return
    prm = hgfx.Param()
    prm.type, prm.cls, prm.name, prm.semantic = 3, 2, name, ""
    prm.elements, prm.rows, prm.cols, prm.nmem = 0, 4, 4, 0
    prm.members, prm.annotations, prm.sampler_states = [], [], []
    prm.flags, prm.object_id, prm.value = 0, None, [0.0] * 16
    eff.params.append(prm)


def add_sampler(eff, name, tex, like_sampler, like_tex):
    """A sampler parameter `name` reading texture parameter `tex`, both
    copies of an existing pair (their Texture state names the texture)."""
    params = {p.name: p for p in eff.params}
    if name in params or like_sampler not in params or like_tex not in params:
        return
    oid = max(eff.objects) + 1
    t = copy.deepcopy(params[like_tex])
    t.name, t.object_id, t.annotations = tex, oid, []
    eff.objects[oid] = {"param": t, "data": b""}
    smp = copy.deepcopy(params[like_sampler])
    smp.name, smp.annotations = name, []
    for st in smp.sampler_states:
        if st["op"] == 164:                         # Texture = <tex>
            st["param"].object_id = oid + 1
            st["usage"], st["data"] = 1, tex.encode() + b"\0"
            eff.objects[oid + 1] = {"param": st["param"], "state": True, "data": st["data"]}
    eff.params.append(t)
    eff.params.append(smp)


def build(stock, family, work, out):
    eff = hgfx.parse_effect(open(stock, "rb").read())
    for name in ULTRA_PARAMS:
        add_float4(eff, name)
    for args in ULTRA_SAMPLER_PARAMS:
        add_sampler(eff, *args)
    for name in ULTRA_MATRICES.get(family, ()):
        add_float4x4(eff, name)
    params = {p.name for p in eff.params} | set(ULTRA_SAMPLERS)
    blobs, swapped, kept, bad = {}, 0, 0, 0
    for t in eff.techniques:
        v = variant(t, family)
        if v is None:
            kept += 1
            continue
        if v not in blobs and not os.path.exists(os.path.join(work, "fx", tag(v) + ".fxo")):
            kept += 1                   # dev build, never compiled: stock
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
    added = 0
    if pl_enabled(family):
        next_oid = max(eff.objects) + 1
        have = set()
        for t in list(eff.techniques):
            v = variant(t, family)
            if v is None or not any(a.name == "PointLights" for a in t["annotations"]):
                continue
            key = combo_key(t)
            if key in have:
                continue
            have.add(key)
            pv = pl_variant(v)
            src_t = t
            if pv not in blobs and not os.path.exists(os.path.join(work, "fx", tag(pv) + ".fxo")):
                # dev build, never compiled: the DLL still asks for _pl5, so
                # it carries the pass of this combination with the most lights
                # (the engine zero-pads the rest)
                pv = None
                src_t = max((u for u in eff.techniques if not u["name"].endswith("_pl5")
                             and variant(u, family) is not None and combo_key(u) == key),
                            key=lambda u: annos(u).get("PointLights", 0))
            elif pv not in blobs:
                blobs[pv] = lift(os.path.join(work, "fx", tag(pv) + ".fxo"))
                for kind, b in blobs[pv].items():
                    missing = sorted(n for n in names_read(b) if n not in params)
                    if missing:
                        print("%s %s reads parameters the effect lacks: %s" % (tag(pv), kind, ", ".join(missing)))
                        bad += 1
            nt = {"name": t["name"] + "_pl5", "annotations": copy.deepcopy(t["annotations"]),
                  "passes": [copy.deepcopy(src_t["passes"][0])]}
            for a in nt["annotations"]:
                if a.name == "PointLights":
                    a.value = [5]
            for st in nt["passes"][0]["states"]:
                if st["param"].object_id is None:
                    continue
                # every technique gets its OWN shader objects: D3DX uploads no
                # constants for techniques that share them (fxload -bind)
                if pv is not None and st["op"] in (hgfx.ST_VS, hgfx.ST_PS):
                    blob = blobs[pv]["vs" if st["op"] == hgfx.ST_VS else "ps"]
                else:
                    blob = eff.objects[st["param"].object_id]["data"]
                prm = copy.deepcopy(st["param"])
                prm.object_id = next_oid
                eff.objects[next_oid] = {"param": prm, "data": blob, "state": True}
                next_oid += 1
                st["param"], st["usage"], st["data"] = prm, 0, blob
            eff.techniques.append(nt)
            added += 1
    if bad:
        sys.exit("refusing to write %s: %d shaders read unknown parameters" % (out, bad))
    data = hgfx.serialize(eff)
    hgfx.parse_effect(data)      # must parse back
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "wb").write(data)
    print("%s: %d techniques rebuilt, %d left stock, %d _pl5 added -> %s (%d bytes)" %
          (os.path.basename(stock), swapped, kept, added, out, len(data)))


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
