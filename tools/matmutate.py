#!/usr/bin/env python3
"""
Prove the parity harness can see each material feature path: break one term
at a time in tools/shaders/<family>.hlsl, run tools/matcheck.sh, and report
whether any seed catches it. A break nobody catches means fxdiff never
exercises that path, so a real regression there would pass unnoticed.

    toolbox run -c dev python3 tools/matmutate.py [actor|background]

The source file is restored afterwards (and on Ctrl-C). Each break must match
the source exactly once; update the list when the source changes.
"""
import os
import subprocess
import sys

MUTATIONS = {
    "actor": [
        ("scatter", "col += (f * (si.w", "col += 0.5*(f * (si.w", "actoroutdoor30"),
        ("scroll", "gfScrollTextures[0].fPhase) *", "gfScrollTextures[0].fPhase*0.5) *", "actoroutdoor30"),
        ("camlight", "(att * _CameraLightColor.xyz)", "(att * 0.5 * _CameraLightColor.xyz)", "actorindoor30"),
        ("glow", "float glow = over * 0.5;", "float glow = over * 0.25;", "actoroutdoor30"),
        ("fog", "float3 rgb = saturate(i.col.w)", "float3 rgb = saturate(i.col.w*0.5)", "actoroutdoor30"),
        ("vtxlights", "fill = ndl * (att * PointLightsColor[k].xyz) + fill;",
         "fill = ndl * (att * 0.5*PointLightsColor[k].xyz) + fill;", "actorindoor30"),
        ("selfillum", "float3 emis = albedo.xyz * si.xyz * gvMiscMaterialData.z;",
         "float3 emis = albedo.xyz * si.xyz * gvMiscMaterialData.z*0.5;", "actoroutdoor30"),
        ("envmap", "c = envamt * (env - albedo.xyz * light) + c;",
         "c = envamt * (env * 0.5 - albedo.xyz * light) + c;", "actoroutdoor30"),
        ("pcf", "float s01 = (z - tex2D(ColorShadowMapSampler, uv - float2(0, t)).x) <= 0 ? 1 : 0;",
         "float s01 = 0;", "actorindoor30"),
        ("specglow", "specglow = p * DirLightsColor[2].w;", "specglow = p * DirLightsColor[2].w*0.5;", "actoroutdoor30"),
    ],
    "background": [
        ("shremap", "float s1 = (tex2D(ShadowMapSampler, i.shpos).x + 1.0) * 0.5;",
         "float s1 = (tex2D(ShadowMapSampler, i.shpos).x + 1.0) * 0.45;", "backgroundoutdoor30"),
        ("lightmap", "light += tex2D(LightMapSampler, i.uv.xy).xyz;",
         "light += 0.5*tex2D(LightMapSampler, i.uv.xy).xyz;", "backgroundindoor30"),
        ("sunvis", "float vis = gvMiscLightingData.y * (v.nrm.w - 1.0) + 1.0;", "float vis = 1.0;",
         "backgroundoutdoorprop30"),
        ("diffuse2", "albedo = lerp(albedo, d2.xyz, d2.w);", "albedo = lerp(albedo, d2.xyz, d2.w*0.5);",
         "backgroundoutdoor30"),
        ("vtxlights", "light = (att * PointLightsColor[k].xyz) * ndl + light;",
         "light = (att * 0.5*PointLightsColor[k].xyz) * ndl + light;", "backgroundindoorprop30"),
        ("indoorspec", "float4 lc = i.sdir.w * i.scol;\n#else\n        float3 H = normalize(V + _DirLightsDir_1[2].xyz);",
         "float4 lc = 0.5*i.sdir.w * i.scol;\n#else\n        float3 H = normalize(V + _DirLightsDir_1[2].xyz);",
         "backgroundindoor30"),
        ("scroll", "float2 suv = gfScrollActive[2] * ((uv + gfScrollTextures[0].fPhase)",
         "float2 suv = gfScrollActive[2] * ((uv + 0.5*gfScrollTextures[0].fPhase)", "backgroundindoorprop30"),
    ],
}


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    fams = sys.argv[1:] or list(MUTATIONS)
    uncaught = 0
    for fam in fams:
        src = os.path.join(root, "tools", "shaders", fam + ".hlsl")
        good = open(src).read()
        try:
            for name, a, b, eff in MUTATIONS[fam]:
                if good.count(a) != 1:
                    print("%-10s %-24s pattern found %d times; update tools/matmutate.py" % (name, eff, good.count(a)))
                    uncaught += 1
                    continue
                open(src, "w").write(good.replace(a, b))
                out = subprocess.run([os.path.join(root, "tools", "matcheck.sh"), eff, fam],
                                     capture_output=True, text=True, errors="replace").stdout
                res = [l.split(",")[1].strip().split()[0] for l in out.splitlines() if l.startswith("seed")]
                hit = any(r != "0" for r in res)
                uncaught += not hit
                print("%-10s %-24s differ per seed: %s%s" % (name, eff, " ".join(res) or "(build failed)",
                                                           "" if hit else "   <-- NOT CAUGHT"))
        finally:
            open(src, "w").write(good)
    sys.exit(1 if uncaught else 0)


if __name__ == "__main__":
    main()
