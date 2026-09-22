#!/usr/bin/env python3
"""
Dump a compiled D3DX effect (.fxo, fx_2_0 binary, tag 0xFEFF0901).

    hgfx.py dump  <file.fxo> [--blobs <outdir>]   # parameters, techniques, passes, shaders
    hgfx.py ctab  <shader.bin>                    # constant table of one shader blob
    hgfx.py pres  <shader.bin>                    # its preshader (CPU-side expressions)
    hgfx.py table <extracted-root> > src/fxtable.h # (size, FNV-1a) -> pak path table for the DLL
    hgfx.py roundtrip <file.fxo>                  # parse -> serialize -> parse; dumps must match
    hgfx.py addpass <in.fxo> <out.fxo> <spec.json> # clone techniques with an extra pass

The layout follows Wine's d3dx9 effect parser (dlls/d3dx9_36/effect.c):
a header (tag, offset), then a data block; the structured part starts at
data+offset with parameter/technique/shader/object counts, then a string
section and a resource section that supply the object bodies (shader
bytecode, referenced-parameter names, FXLC expressions).

Shader bytecode is SM1-3 token stream; its CTAB comment gives the constant
layout (name, register set/index/count, type). Use build/fxdis.exe under Wine
for a full disassembly of the extracted blobs.
"""
import os
import struct
import sys

PT = ["void", "bool", "int", "float", "string", "texture", "texture1D", "texture2D",
      "texture3D", "textureCUBE", "sampler", "sampler1D", "sampler2D", "sampler3D",
      "samplerCUBE", "pixelshader", "vertexshader", "pixelfragment", "vertexfragment",
      "unsupported"]
PC = ["scalar", "vector", "matrix_rows", "matrix_cols", "object", "struct"]

STATES = ("D3DRS_ZENABLE D3DRS_FILLMODE D3DRS_SHADEMODE D3DRS_ZWRITEENABLE D3DRS_ALPHATESTENABLE "
          "D3DRS_LASTPIXEL D3DRS_SRCBLEND D3DRS_DESTBLEND D3DRS_CULLMODE D3DRS_ZFUNC D3DRS_ALPHAREF "
          "D3DRS_ALPHAFUNC D3DRS_DITHERENABLE D3DRS_ALPHABLENDENABLE D3DRS_FOGENABLE D3DRS_SPECULARENABLE "
          "D3DRS_FOGCOLOR D3DRS_FOGTABLEMODE D3DRS_FOGSTART D3DRS_FOGEND D3DRS_FOGDENSITY "
          "D3DRS_RANGEFOGENABLE D3DRS_STENCILENABLE D3DRS_STENCILFAIL D3DRS_STENCILZFAIL D3DRS_STENCILPASS "
          "D3DRS_STENCILFUNC D3DRS_STENCILREF D3DRS_STENCILMASK D3DRS_STENCILWRITEMASK D3DRS_TEXTUREFACTOR "
          "D3DRS_WRAP0 D3DRS_WRAP1 D3DRS_WRAP2 D3DRS_WRAP3 D3DRS_WRAP4 D3DRS_WRAP5 D3DRS_WRAP6 D3DRS_WRAP7 "
          "D3DRS_WRAP8 D3DRS_WRAP9 D3DRS_WRAP10 D3DRS_WRAP11 D3DRS_WRAP12 D3DRS_WRAP13 D3DRS_WRAP14 "
          "D3DRS_WRAP15 D3DRS_CLIPPING D3DRS_LIGHTING D3DRS_AMBIENT D3DRS_FOGVERTEXMODE D3DRS_COLORVERTEX "
          "D3DRS_LOCALVIEWER D3DRS_NORMALIZENORMALS D3DRS_DIFFUSEMATERIALSOURCE "
          "D3DRS_SPECULARMATERIALSOURCE D3DRS_AMBIENTMATERIALSOURCE D3DRS_EMISSIVEMATERIALSOURCE "
          "D3DRS_VERTEXBLEND D3DRS_CLIPPLANEENABLE D3DRS_POINTSIZE D3DRS_POINTSIZE_MIN D3DRS_POINTSIZE_MAX "
          "D3DRS_POINTSPRITEENABLE D3DRS_POINTSCALEENABLE D3DRS_POINTSCALE_A D3DRS_POINTSCALE_B "
          "D3DRS_POINTSCALE_C D3DRS_MULTISAMPLEANTIALIAS D3DRS_MULTISAMPLEMASK D3DRS_PATCHEDGESTYLE "
          "D3DRS_DEBUGMONITORTOKEN D3DRS_INDEXEDVERTEXBLENDENABLE D3DRS_COLORWRITEENABLE D3DRS_TWEENFACTOR "
          "D3DRS_BLENDOP D3DRS_POSITIONDEGREE D3DRS_NORMALDEGREE D3DRS_SCISSORTESTENABLE "
          "D3DRS_SLOPESCALEDEPTHBIAS D3DRS_ANTIALIASEDLINEENABLE D3DRS_MINTESSELLATIONLEVEL "
          "D3DRS_MAXTESSELLATIONLEVEL D3DRS_ADAPTIVETESS_X D3DRS_ADAPTIVETESS_Y D3DRS_ADAPTIVETESS_Z "
          "D3DRS_ADAPTIVETESS_W D3DRS_ENABLEADAPTIVETESSELLATION D3DRS_TWOSIDEDSTENCILMODE "
          "D3DRS_CCW_STENCILFAIL D3DRS_CCW_STENCILZFAIL D3DRS_CCW_STENCILPASS D3DRS_CCW_STENCILFUNC "
          "D3DRS_COLORWRITEENABLE1 D3DRS_COLORWRITEENABLE2 D3DRS_COLORWRITEENABLE3 D3DRS_BLENDFACTOR "
          "D3DRS_SRGBWRITEENABLE D3DRS_DEPTHBIAS D3DRS_SEPARATEALPHABLENDENABLE D3DRS_SRCBLENDALPHA "
          "D3DRS_DESTBLENDALPHA D3DRS_BLENDOPALPHA D3DTSS_COLOROP D3DTSS_COLORARG0 D3DTSS_COLORARG1 "
          "D3DTSS_COLORARG2 D3DTSS_ALPHAOP D3DTSS_ALPHAARG0 D3DTSS_ALPHAARG1 D3DTSS_ALPHAARG2 "
          "D3DTSS_RESULTARG D3DTSS_BUMPENVMAT00 D3DTSS_BUMPENVMAT01 D3DTSS_BUMPENVMAT10 D3DTSS_BUMPENVMAT11 "
          "D3DTSS_TEXCOORDINDEX D3DTSS_BUMPENVLSCALE D3DTSS_BUMPENVLOFFSET D3DTSS_TEXTURETRANSFORMFLAGS "
          "D3DTSS_CONSTANT NPatchMode FVF D3DTS_PROJECTION D3DTS_VIEW D3DTS_WORLD D3DTS_TEXTURE0 "
          "MaterialDiffuse MaterialAmbient MaterialSpecular MaterialEmissive MaterialPower LightType "
          "LightDiffuse LightSpecular LightAmbient LightPosition LightDirection LightRange LightFallOff "
          "LightAttenuation0 LightAttenuation1 LightAttenuation2 LightTheta LightPhi LightEnable "
          "Vertexshader Pixelshader VertexShaderConstantF VertexShaderConstantB VertexShaderConstantI "
          "VertexShaderConstant VertexShaderConstant1 VertexShaderConstant2 VertexShaderConstant3 "
          "VertexShaderConstant4 PixelShaderConstantF PixelShaderConstantB PixelShaderConstantI "
          "PixelShaderConstant PixelShaderConstant1 PixelShaderConstant2 PixelShaderConstant3 "
          "PixelShaderConstant4 Texture AddressU AddressV AddressW BorderColor MagFilter MinFilter "
          "MipFilter MipMapLodBias MaxMipLevel MaxAnisotropy SRGBTexture ElementIndex DMAPOffset "
          "Sampler").split()
ST_VS, ST_PS = STATES.index("Vertexshader"), STATES.index("Pixelshader")


class Cur:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def u32(self):
        v, = struct.unpack_from("<I", self.d, self.p)
        self.p += 4
        return v


def name_at(data, off):
    n, = struct.unpack_from("<I", data, off)
    return data[off + 4:off + 4 + n].split(b"\0")[0].decode("latin1") if n else ""


class Param:
    """One typedef, with default value and (for objects) object id(s)."""
    __slots__ = ("type", "cls", "name", "semantic", "elements", "rows", "cols",
                 "members", "value", "object_id", "annotations", "sampler_states", "flags", "nmem")

    def typestr(self):
        t = PT[self.type] if self.type < len(PT) else "type%d" % self.type
        if self.cls in (0, 1, 2, 3) and self.type in (1, 2, 3):
            if self.cls == 1:
                t += "%d" % self.cols if self.cols > 1 else ""
            elif self.cls in (2, 3):
                t += "%dx%d" % (self.rows, self.cols)
        if self.elements:
            t += "[%d]" % self.elements
        return t


def parse_typedef(data, cur, parent=None, flags=0):
    p = Param()
    p.flags, p.members, p.value, p.object_id, p.annotations, p.sampler_states = flags, [], None, None, [], []
    if parent is None:
        p.type, p.cls = cur.u32(), cur.u32()
        p.name = name_at(data, cur.u32())
        p.semantic = name_at(data, cur.u32())
        p.elements = cur.u32()
        p.rows = p.cols = 1
        nmem = 0
        if p.cls == 1:
            p.cols, p.rows = cur.u32(), cur.u32()
        elif p.cls in (0, 2, 3):
            p.rows, p.cols = cur.u32(), cur.u32()
        elif p.cls == 5:
            nmem = cur.u32()
        p.nmem = nmem
    else:
        p.type, p.cls, p.name, p.semantic = parent.type, parent.cls, parent.name, parent.semantic
        p.elements, p.rows, p.cols = 0, parent.rows, parent.cols
        nmem = p.nmem = parent.nmem
    if p.elements:
        save = cur.p
        for _ in range(p.elements):
            cur.p = save
            p.members.append(parse_typedef(data, cur, p, flags))
    elif nmem:
        if parent is None:
            for _ in range(nmem):
                p.members.append(parse_typedef(data, cur, None, flags))
        else:
            # element of a struct array: the member typedefs follow (Wine
            # re-reads them from the same position for every element)
            for _ in range(nmem):
                p.members.append(parse_typedef(data, cur, None, flags))
    return p


def parse_value(eff, p, data, cur):
    if p.elements:
        for m in p.members:
            parse_value(eff, m, data, cur)
        return
    if p.cls == 5:
        for m in p.members:
            parse_value(eff, m, data, cur)
        return
    if p.cls in (0, 1, 2, 3):
        n = p.rows * p.cols
        fmt = "<%d%s" % (n, "f" if p.type == 3 else "i")
        p.value = list(struct.unpack_from(fmt, data, cur.p))
        cur.p += 4 * n
        return
    if p.cls == 4:
        if p.type in (4, 5, 6, 7, 8, 9, 15, 16):
            p.object_id = cur.u32()
            eff.objects.setdefault(p.object_id, {})["param"] = p
        elif p.type in (10, 11, 12, 13, 14):
            n = cur.u32()
            for _ in range(n):
                p.sampler_states.append(parse_state(eff, data, cur))


def parse_state(eff, data, cur):
    op, idx, toff, voff = cur.u32(), cur.u32(), cur.u32(), cur.u32()
    p = parse_typedef(data, Cur(data, toff))
    parse_value(eff, p, data, Cur(data, voff))
    if p.object_id is not None:
        eff.objects.setdefault(p.object_id, {})["state"] = True
    return {"op": op, "index": idx, "param": p, "usage": None, "data": None}


def parse_annotation(eff, data, cur):
    toff, voff = cur.u32(), cur.u32()
    p = parse_typedef(data, Cur(data, toff), None, 2)
    parse_value(eff, p, data, Cur(data, voff))
    return p


class Effect:
    pass


def parse_effect(buf):
    tag, off = struct.unpack_from("<II", buf, 0)
    if tag != 0xFEFF0901:
        sys.exit("not an fx_2_0 binary (tag 0x%08x)" % tag)
    data = buf[8:]
    eff = Effect()
    eff.objects, eff.params, eff.techniques = {}, [], []
    cur = Cur(data, off)
    nparam, ntech, nshader, nobj = cur.u32(), cur.u32(), cur.u32(), cur.u32()
    eff.nshader, eff.nobj = nshader, nobj
    for _ in range(nparam):
        toff, voff, flags, nanno = cur.u32(), cur.u32(), cur.u32(), cur.u32()
        p = parse_typedef(data, Cur(data, toff), None, flags)
        parse_value(eff, p, data, Cur(data, voff))
        for _ in range(nanno):
            p.annotations.append(parse_annotation(eff, data, cur))
        eff.params.append(p)
    for _ in range(ntech):
        t = {"name": name_at(data, cur.u32()), "annotations": [], "passes": []}
        nanno, npass = cur.u32(), cur.u32()
        for _ in range(nanno):
            t["annotations"].append(parse_annotation(eff, data, cur))
        for _ in range(npass):
            ps = {"name": name_at(data, cur.u32()), "annotations": [], "states": []}
            nanno, nstate = cur.u32(), cur.u32()
            for _ in range(nanno):
                ps["annotations"].append(parse_annotation(eff, data, cur))
            for _ in range(nstate):
                ps["states"].append(parse_state(eff, data, cur))
            t["passes"].append(ps)
        eff.techniques.append(t)
    nstr, nres = cur.u32(), cur.u32()
    for _ in range(nstr):
        oid = cur.u32()
        size = cur.u32()
        eff.objects.setdefault(oid, {})["data"] = data[cur.p:cur.p + size]
        cur.p += (size + 3) & ~3
    for _ in range(nres):
        ti, idx, ei, si, usage = cur.u32(), cur.u32(), cur.u32(), cur.u32(), cur.u32()
        if ti == 0xFFFFFFFF:
            p = eff.params[idx]
            if ei != 0xFFFFFFFF and p.elements:
                p = p.members[ei]
            st = p.sampler_states[si]
        else:
            st = eff.techniques[ti]["passes"][idx]["states"][si]
        size = cur.u32()
        body = data[cur.p:cur.p + size]
        cur.p += (size + 3) & ~3
        st["usage"], st["data"] = usage, body
        oid = st["param"].object_id
        if oid is not None:
            eff.objects.setdefault(oid, {})["data"] = body
    eff.tail = len(data) - cur.p
    return eff


# ---- shader bytecode -------------------------------------------------------

REGSET = ["bool", "int4", "float4", "sampler"]


def shader_version(blob):
    if len(blob) < 4:
        return None
    v, = struct.unpack_from("<I", blob, 0)
    kind = {0xFFFF: "ps", 0xFFFE: "vs"}.get(v >> 16)
    return "%s_%d_%d" % (kind, (v >> 8) & 0xFF, v & 0xFF) if kind else None


def ctab(blob):
    """Return (creator, target, [(name, regset, index, count, typestr)])."""
    p = 4
    while p + 4 <= len(blob):
        tok, = struct.unpack_from("<I", blob, p)
        if tok == 0x0000FFFF:
            break
        if (tok & 0xFFFF) == 0xFFFE:                  # comment
            n = (tok >> 16) & 0x7FFF
            if blob[p + 4:p + 8] == b"CTAB":
                base = p + 8
                size, creator, ver, nconst, cinfo, flags, target = struct.unpack_from("<7I", blob, base)
                cstr = lambda o: blob[base + o:blob.index(b"\0", base + o)].decode("latin1")
                consts = []
                for i in range(nconst):
                    noff, rset, ridx, rcnt, _, toff, _ = struct.unpack_from("<IHHHHII", blob, base + cinfo + 20 * i)
                    cls, typ, rows, cols, elems, smem, _ = struct.unpack_from("<6HI", blob, base + toff)
                    ts = PT[typ] if typ < len(PT) else "t%d" % typ
                    if cls == 1 and cols > 1:
                        ts += "%d" % cols
                    elif cls in (2, 3):
                        ts += "%dx%d" % (rows, cols)
                    elif cls == 5:
                        ts = "struct{%d}" % smem
                    if elems > 1:
                        ts += "[%d]" % elems
                    consts.append((cstr(noff), REGSET[rset] if rset < 4 else str(rset), ridx, rcnt, ts))
                consts.sort(key=lambda c: (c[1], c[2]))
                return cstr(creator), cstr(target), consts
            p += 4 + 4 * n
            continue
        p += 4 + 4 * ((tok >> 24) & 0xF)
    return None


# Preshader: the CPU-side expressions the effect compiler hoists out of a
# shader (a 'PRES' comment: CTAB of effect parameters it reads, CLIT literal
# doubles, FXLC instructions, PRSI output mapping). Layout as in Wine's
# d3dx9_36/preshader.c. Output registers are the shader's own constants.
PRES_OPS = {0x100: "mov", 0x101: "neg", 0x103: "rcp", 0x104: "frc", 0x105: "exp", 0x106: "log",
            0x107: "rsq", 0x108: "sin", 0x109: "cos", 0x10a: "asin", 0x10b: "acos", 0x10c: "atan",
            0x200: "min", 0x201: "max", 0x202: "lt", 0x203: "ge", 0x204: "add", 0x205: "mul",
            0x206: "atan2", 0x208: "div", 0x300: "cmp", 0x301: "movc", 0x500: "dot", 0x502: "noise",
            0x70e: "dotswiz6", 0x70f: "dotswiz8"}
PRES_TABLES = {1: "lit", 2: "in", 4: "c", 5: "b", 6: "i", 7: "t"}


def preshader(blob, inputs=False):
    """Return the preshader as readable lines, or [] when the shader has none.
    With inputs=True, return the set of effect parameter names it reads."""
    p = 4
    while p + 8 <= len(blob):
        tok, = struct.unpack_from("<I", blob, p)
        if (tok & 0xFFFF) != 0xFFFE:
            return set() if inputs else []
        n = (tok >> 16) & 0x7FFF
        if blob[p + 4:p + 8] == b"PRES":
            break
        p += 4 + 4 * n
    else:
        return set() if inputs else []
    q, end, subs = p + 12, p + 4 + 4 * n, {}
    while q < end:
        t, = struct.unpack_from("<I", blob, q)
        if (t & 0xFFFF) != 0xFFFE:
            break
        m = (t >> 16) & 0x7FFF
        subs[blob[q + 4:q + 8]] = (q + 8, m - 1)
        q += 4 + 4 * m
    lit = ()
    if b"CLIT" in subs:                      # absent when no literal is used
        o, _ = subs[b"CLIT"]
        nl, = struct.unpack_from("<I", blob, o)
        lit = struct.unpack_from("<%dd" % nl, blob, o + 4)
    regs = {}
    base = subs[b"CTAB"][0] if b"CTAB" in subs else None   # absent when no parameter is read
    nconst = cinfo = 0
    if base is not None:
        _, _, _, nconst, cinfo, _, _ = struct.unpack_from("<7I", blob, base)
    cstr = lambda x: blob[base + x:blob.index(b"\0", base + x)].decode("latin1")
    for i in range(nconst):
        noff, _, ridx, rcnt, _, _, _ = struct.unpack_from("<IHHHHII", blob, base + cinfo + 20 * i)
        for r in range(rcnt):
            regs[ridx + r] = cstr(noff) + ("[%d]" % r if rcnt > 1 else "")
    if inputs:
        return {v.split("[")[0] for v in regs.values()}
    if b"FXLC" not in subs:
        return []
    o, m = subs[b"FXLC"]
    f = struct.unpack_from("<%dI" % m, blob, o)

    def operand(k, nc):
        if f[k]:
            return k + 5, "<indexed>"
        t, off = f[k + 1], f[k + 2]
        sw = "xyzw"[off % 4:off % 4 + nc]
        if t == 1:
            return k + 3, "(%s)" % ", ".join("%g" % lit[off + j] for j in range(nc))
        if t == 2:
            return k + 3, "%s.%s" % (regs.get(off // 4, "in%d" % (off // 4)), sw)
        return k + 3, "%s%d.%s" % (PRES_TABLES.get(t, "?%d" % t), off // 4, sw)

    out, k = [], 1
    for _ in range(f[0]):
        w, ni = f[k], f[k + 1]
        k += 2
        op, nc, scalar = (w >> 20) & 0x7FF, w & 0xFFFF, bool(w & 0x80000000)
        args = []
        for j in range(ni):
            k, a = operand(k, 1 if (scalar and j == 0) else nc)
            args.append(a)
        k, dst = operand(k, nc)
        out.append("%s = %s(%s)" % (dst, PRES_OPS.get(op, hex(op)), ", ".join(args)))
    return out


def fix_ctab(blob, target=None):
    """Make a standalone-compiled shader acceptable inside an effect.

    Both compilers we can run (vkd3d-shader, and Microsoft's D3DXCompileShader
    from d3dx9_34) emit a CTAB without per-constant default values, so every
    constant's DefaultValue offset is 0, and vkd3d also leaves the Target
    string empty. The game's D3DX (d3dx9_34) rejects an effect whose shader
    has a constant whose default block would run past the table -- which is
    exactly what a 180-element Bones array does -- with a bare E_FAIL. The
    effect compiler always writes zeroed defaults, so we do the same, and
    fill in the target string when it is missing."""
    ver = shader_version(blob)
    target = target or ver
    p = 4
    while p + 4 <= len(blob):
        tok, = struct.unpack_from("<I", blob, p)
        if tok == 0x0000FFFF:
            break
        if (tok & 0xFFFF) == 0xFFFE:
            n = (tok >> 16) & 0x7FFF
            if blob[p + 4:p + 8] == b"CTAB":
                base = p + 8
                body = bytearray(blob[base:p + 4 + 4 * n])
                size, creator, cver, nconst, cinfo, flags, toff = struct.unpack_from("<7I", body, 0)
                changed = False
                if not toff or not body[toff:body.index(b"\0", toff)]:
                    toff = len(body)
                    body += target.encode("ascii") + b"\0"
                    while len(body) & 3:
                        body += b"\0"
                    struct.pack_into("<I", body, 24, toff)
                    changed = True
                for i in range(nconst):
                    rec = cinfo + 20 * i
                    noff, rset, ridx, rcnt, _, tyoff, doff = struct.unpack_from("<IHHHHII", body, rec)
                    if doff or rset == 3:          # has defaults, or a sampler
                        continue
                    cls, typ, rows, cols, elems, smem, _ = struct.unpack_from("<6HI", body, tyoff)
                    nbytes = 4 * rows * cols * max(elems, 1)
                    doff = len(body)
                    body += b"\0" * nbytes
                    struct.pack_into("<I", body, rec + 16, doff)
                    changed = True
                if not changed:
                    return blob
                newtok = 0xFFFE | ((len(body) // 4 + 1) << 16)      # + the 'CTAB' dword
                return blob[:p] + struct.pack("<I", newtok) + b"CTAB" + bytes(body) + blob[p + 4 + 4 * n:]
            p += 4 + 4 * n
            continue
        p += 4 + 4 * ((tok >> 24) & 0xF)
    return blob


fix_ctab_target = fix_ctab


def instr_count(blob):
    p, n = 4, 0
    while p + 4 <= len(blob):
        tok, = struct.unpack_from("<I", blob, p)
        if tok == 0x0000FFFF:
            break
        if (tok & 0xFFFF) == 0xFFFE:
            p += 4 + 4 * ((tok >> 16) & 0x7FFF)
            continue
        n += 1
        p += 4 + 4 * ((tok >> 24) & 0xF)
    return n



# ---- serializer -------------------------------------------------------------
#
# The layout below reproduces the Microsoft effect compiler's output byte for
# byte (checked with `hgfx.py roundtrip` on all 120 D3D9 effects). It matters:
# D3DX reads the blob sequentially and rejects a file whose pieces are in a
# different order even though every offset is valid.
#
#   blob:  [u32 0 = the empty name]
#          per parameter:  typedef | (sampler: per state value,typedef) | value
#                          | per annotation: value,typedef,name | name | semantic
#          per technique:  per annotation: value,typedef,name
#                          | per pass: per state value,typedef | pass name
#                          | technique name
#   strings section:   value objects (textures, strings), descending object id
#   resource section:  shader / texture-reference / expression objects,
#                      descending object id

class Writer:
    def __init__(self):
        self.b = bytearray(b"\0\0\0\0")     # offset 0: the shared empty name
        self.padding = set()                # blob positions that are only padding

    def pad(self):
        while len(self.b) & 3:
            self.padding.add(len(self.b))
            self.b += b"\0"

    def u32(self, v):
        self.b += struct.pack("<I", v)

    def name(self, sname):
        """Write a name blob and return its offset; empty names share offset 0."""
        if not sname:
            return 0
        raw = sname.encode("latin1") + b"\0"
        self.pad()
        off = len(self.b)
        self.u32(len(raw))
        self.b += raw
        self.pad()
        return off

    def typedef(self, p, fix):
        """Write a typedef header. Name/semantic offsets are patched later via
        fix(list): the strings come after the value and annotations."""
        self.pad()
        off = len(self.b)
        self.u32(p.type); self.u32(p.cls)
        fix.append(len(self.b)); self.u32(0)     # name offset, patched
        fix.append(len(self.b)); self.u32(0)     # semantic offset, patched
        self.u32(p.elements)
        if p.cls == 1:
            self.u32(p.cols); self.u32(p.rows)
        elif p.cls in (0, 2, 3):
            self.u32(p.rows); self.u32(p.cols)
        elif p.cls == 5:
            self.u32(p.nmem)
            members = p.members[0].members if p.elements else p.members
            for m in members:
                mfix = []
                self.typedef(m, mfix)
                self.member_fixes.append((m, mfix))
        return off

    def value_bytes(self, p):
        if p.elements or p.cls == 5:
            return b"".join(self.value_bytes(m) for m in p.members)
        if p.cls in (0, 1, 2, 3):
            n = p.rows * p.cols
            vals = p.value if p.value is not None else [0] * n
            return struct.pack("<%d%s" % (n, "f" if p.type == 3 else "i"), *vals)
        if p.cls == 4 and p.type in (4, 5, 6, 7, 8, 9, 15, 16):
            return struct.pack("<I", p.object_id)
        return b""

    def state(self, st):
        """value, then typedef (states have no names). Returns (toff, voff)."""
        prm = st["param"]
        self.pad()
        voff = len(self.b)
        self.b += self.value_bytes(prm)
        fix = []
        toff = self.typedef(prm, fix)
        self.finish_names(prm, fix)
        return toff, voff

    def finish_names(self, p, fix):
        noff = self.name(p.name)
        soff = self.name(p.semantic)
        struct.pack_into("<I", self.b, fix[0], noff)
        struct.pack_into("<I", self.b, fix[1], soff)

    def annotation(self, a):
        """value, typedef, name. Returns (toff, voff)."""
        self.pad()
        voff = len(self.b)
        self.b += self.value_bytes(a)
        fix = []
        toff = self.typedef(a, fix)
        self.finish_names(a, fix)
        return toff, voff

    def parameter(self, p):
        """Returns (toff, voff, [(anno toff, voff)...])."""
        self.member_fixes = []
        fix = []
        toff = self.typedef(p, fix)
        state_recs = []
        if p.sampler_states:
            for st in p.sampler_states:
                st_toff, st_voff = self.state(st)
                state_recs.append(struct.pack("<IIII", st["op"], st["index"], st_toff, st_voff))
        self.pad()
        voff = len(self.b)
        if p.sampler_states:
            self.u32(len(state_recs))
            for r in state_recs:
                self.b += r
        else:
            self.b += self.value_bytes(p)
        annos = [self.annotation(a) for a in p.annotations]
        for m, mfix in self.member_fixes:
            self.finish_names(m, mfix)
        self.finish_names(p, fix)
        return toff, voff, annos

    def technique(self, t):
        annos = [self.annotation(a) for a in t["annotations"]]
        passes = []
        for ps in t["passes"]:
            pannos = [self.annotation(a) for a in ps["annotations"]]
            states = []
            for st in ps["states"]:
                toff, voff = self.state(st)
                states.append(struct.pack("<IIII", st["op"], st["index"], toff, voff))
            pname = self.name(ps["name"])
            passes.append((pname, pannos, states))
        tname = self.name(t["name"])
        return tname, annos, passes


def serialize(eff):
    """Rebuild the fx_2_0 file from a parsed Effect, in the compiler's layout."""
    w = Writer()
    prec = [w.parameter(p) for p in eff.params]
    trec = [w.technique(t) for t in eff.techniques]
    w.pad()
    blob = bytes(w.b)
    serialize.padding = {8 + k for k in w.padding}    # file positions, for roundtrip

    body = bytearray()
    nobj = max(eff.objects) + 1 if eff.objects else 0
    # Third header count: D3DX sizes an internal table with it. It is the
    # number of shader states that carry an object, plus one per pass, plus
    # one per sampler parameter (fits all 120 game effects exactly). Too
    # small and the big effects fail to load with E_FAIL.
    nshader = (sum(1 for t in eff.techniques for ps in t["passes"] for st in ps["states"]
                   if st["op"] in (ST_VS, ST_PS) and st["param"].object_id is not None)
               + sum(len(t["passes"]) for t in eff.techniques)
               + sum(len(p.members) if p.elements else 1 for p in eff.params if p.type in (10, 11, 12, 13, 14)))
    body += struct.pack("<IIII", len(eff.params), len(eff.techniques), nshader, nobj)
    for (toff, voff, annos), p in zip(prec, eff.params):
        body += struct.pack("<IIII", toff, voff, p.flags, len(annos))
        for atoff, avoff in annos:
            body += struct.pack("<II", atoff, avoff)
    for (tname, annos, passes), t in zip(trec, eff.techniques):
        body += struct.pack("<III", tname, len(annos), len(passes))
        for atoff, avoff in annos:
            body += struct.pack("<II", atoff, avoff)
        for pname, pannos, states in passes:
            body += struct.pack("<III", pname, len(pannos), len(states))
            for atoff, avoff in pannos:
                body += struct.pack("<II", atoff, avoff)
            for r in states:
                body += r

    # strings: every object that is a parameter/annotation value (textures
    # carry no data, strings do); resources: everything referenced from a
    # state. Both by descending object id.
    strings, resources = [], []
    for ti, t in enumerate(eff.techniques):
        for pi, ps in enumerate(t["passes"]):
            for si, st in enumerate(ps["states"]):
                if st.get("data") is not None:
                    resources.append((st["param"].object_id, (ti, pi, 0xFFFFFFFF, si, st["usage"], st["data"])))
    for pi, p in enumerate(eff.params):
        elems = p.members if p.elements else [p]
        for ei, e in enumerate(elems):
            for si, st in enumerate(e.sampler_states):
                if st.get("data") is not None:
                    resources.append((st["param"].object_id, (0xFFFFFFFF, pi, ei, si, st["usage"], st["data"])))
    state_ids = {oid for oid, _ in resources}
    for oid, o in eff.objects.items():
        if o.get("param") is not None and oid not in state_ids and not o.get("state"):
            strings.append((oid, o.get("data") or b""))
    strings.sort(key=lambda x: -x[0])
    resources.sort(key=lambda x: -x[0])
    body += struct.pack("<II", len(strings), len(resources))
    for oid, data in strings:
        body += struct.pack("<II", oid, len(data)) + data
        while len(body) & 3:
            body += b"\0"
    for _, (ti, idx, ei, si, usage, data) in resources:
        body += struct.pack("<IIIIII", ti, idx, ei, si, usage, len(data)) + data
        while len(body) & 3:
            body += b"\0"
    return struct.pack("<II", 0xFEFF0901, len(blob)) + blob + bytes(body)


def dump_text(path_or_bytes):
    """The dump as a string, header line dropped, for comparisons."""
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        if isinstance(path_or_bytes, bytes):
            tmp = "/tmp/hgfx_rt.fxo"
            open(tmp, "wb").write(path_or_bytes)
            dump(tmp, None)
        else:
            dump(path_or_bytes, None)
    return "\n".join(l for l in buf.getvalue().splitlines() if not l.startswith("== "))


def roundtrip(path):
    orig = open(path, "rb").read()
    eff = parse_effect(orig)
    out = serialize(eff)
    if out == orig:
        print("roundtrip OK: byte-identical (%d bytes)" % len(orig))
        return 0
    if len(out) == len(orig):
        diffs = [k for k in range(len(orig)) if orig[k] != out[k]]
        # the compiler leaves uninitialised bytes in its name padding; ours are zero
        if all(k in serialize.padding for k in diffs):
            print("roundtrip OK: identical except %d padding bytes (%d bytes)" % (len(diffs), len(orig)))
            return 0
        i = next(k for k in diffs if k not in serialize.padding)
    else:
        i = next((k for k in range(min(len(orig), len(out))) if orig[k] != out[k]), min(len(orig), len(out)))
    print("roundtrip: NOT byte-identical, first difference at %d (orig %d bytes, mine %d bytes)" % (i, len(orig), len(out)))
    print("  orig:", orig[max(0, i - 8):i + 24].hex())
    print("  mine:", out[max(0, i - 8):i + 24].hex())
    a, b = dump_text(path), dump_text(out)
    # blob tags carry the file's base name; normalise
    b = b.replace("hgfx_rt.", os.path.splitext(os.path.basename(path))[0] + ".")
    if a == b:
        print("roundtrip OK: %d -> %d bytes, dumps identical" % (len(orig), len(out)))
        return 0
    import difflib
    for l in list(difflib.unified_diff(a.splitlines(), b.splitlines(), lineterm="", n=1))[:40]:
        print(l)
    print("roundtrip MISMATCH")
    return 1


# ---- output ----------------------------------------------------------------

def fmt_value(p):
    if p.value is not None:
        v = p.value
        if p.type == 3:
            return "{" + ", ".join("%g" % x for x in v) + "}"
        if p.type == 1:
            return "{" + ", ".join("true" if x else "false" for x in v) + "}"
        return "{" + ", ".join(str(x) for x in v) + "}"
    if p.elements or p.cls == 5:
        return "{" + ", ".join(fmt_value(m) for m in p.members) + "}"
    if p.object_id is not None:
        return "obj#%d" % p.object_id
    return ""


def dump_param(eff, p, indent="  "):
    line = "%s%-14s %-40s" % (indent, p.typestr(), p.name)
    if p.semantic:
        line += " : %s" % p.semantic
    v = fmt_value(p)
    if p.type == 4 and p.object_id in eff.objects and "data" in eff.objects[p.object_id]:
        v = '"%s"' % eff.objects[p.object_id]["data"].split(b"\0")[0].decode("latin1")
    if v and v != "{0}" and len(v) < 90:
        line += " = " + v
    if p.annotations:
        line += "  <" + " ".join("%s=%s" % (a.name, fmt_value(a)) for a in p.annotations) + ">"
    print(line)
    if p.cls == 5 and not p.elements:
        for m in p.members:
            dump_param(eff, m, indent + "    .")
    if p.sampler_states:
        for st in p.sampler_states:
            print("%s    %s = %s" % (indent, STATES[st["op"]], fmt_value(st["param"])))


def dump(path, blobdir):
    buf = open(path, "rb").read()
    eff = parse_effect(buf)
    base = os.path.splitext(os.path.basename(path))[0]
    print("== %s  (%d bytes, %d params, %d techniques, %d shader slots, %d objects, %d tail bytes)" %
          (path, len(buf), len(eff.params), len(eff.techniques), eff.nshader, eff.nobj, eff.tail))
    print("-- parameters")
    for p in eff.params:
        dump_param(eff, p)
    seen = {}
    for ti, t in enumerate(eff.techniques):
        print("-- technique %s" % t["name"], "".join(" <%s=%s>" % (a.name, fmt_value(a)) for a in t["annotations"]))
        for pi, ps in enumerate(t["passes"]):
            print("   pass %s" % (ps["name"] or pi))
            for st in ps["states"]:
                op = STATES[st["op"]] if st["op"] < len(STATES) else "op%d" % st["op"]
                p = st["param"]
                if st["op"] in (ST_VS, ST_PS):
                    body = st["data"]
                    if st["usage"] == 1:
                        ref = body.split(b"\0")[0].decode("latin1")
                        print("      %s = <%s>" % (op, ref))
                        continue
                    if not body:
                        print("      %s = (none)" % op)
                        continue
                    ver = shader_version(body)
                    key = hash(body)
                    tag = seen.get(key)
                    if tag is None:
                        tag = "%s.t%d.p%d.%s" % (base, ti, pi, "vs" if st["op"] == ST_VS else "ps")
                        seen[key] = tag
                        if blobdir:
                            os.makedirs(blobdir, exist_ok=True)
                            open(os.path.join(blobdir, tag + ".bin"), "wb").write(body)
                        c = ctab(body)
                        print("      %s = %s  %d bytes, %d instr  [%s]" % (op, ver, len(body), instr_count(body), tag))
                        if c:
                            for name, rset, idx, cnt, ts in c[2]:
                                print("         %-8s c%-3d n=%-3d %-14s %s" % (rset, idx, cnt, ts, name))
                    else:
                        print("      %s = %s  (same as %s)" % (op, ver, tag))
                elif st["usage"] == 1:
                    print("      %s[%d] = <%s>" % (op, st["index"], st["data"].split(b"\0")[0].decode("latin1")))
                elif st["usage"] == 2:
                    print("      %s[%d] = <array selector, %d bytes>" % (op, st["index"], len(st["data"] or b"")))
                elif st["usage"] == 0 and st["data"]:
                    print("      %s[%d] = <expression, %d bytes>" % (op, st["index"], len(st["data"])))
                else:
                    idx = "[%d]" % st["index"] if st["index"] else ""
                    print("      %s%s = %s" % (op, idx, fmt_value(p)))


def fnv1a(b):
    h = 0x811C9DC5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def table(root):
    """C table of every .fxo under root keyed by (size, FNV-1a of the blob).
    Identical blobs under different names collapse to one row (first name)."""
    rows = {}
    for dp, _, fs in os.walk(root):
        for f in sorted(fs):
            if not f.lower().endswith(".fxo"):
                continue
            path = os.path.join(dp, f)
            b = open(path, "rb").read()
            rel = os.path.relpath(path, root).replace("/", "\\")
            rows.setdefault((len(b), fnv1a(b)), rel)
    print("/* Generated by tools/fx/hgfx.py table -- do not edit. (size, fnv1a) -> pak path. */")
    print("static const struct { unsigned int size, hash; const char *path; } g_fxtable[] = {")
    for (n, h), rel in sorted(rows.items(), key=lambda kv: kv[1].lower()):
        print('    { %u, 0x%08xu, "%s" },' % (n, h, rel.replace("\\", "\\\\")))
    print("};")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if sys.argv[1] == "table":
        table(sys.argv[2])
    elif sys.argv[1] == "roundtrip":
        sys.exit(roundtrip(sys.argv[2]))
    elif sys.argv[1] == "dump":
        blobdir = sys.argv[sys.argv.index("--blobs") + 1] if "--blobs" in sys.argv else None
        dump(sys.argv[2], blobdir)
    elif sys.argv[1] == "ctab":
        blob = open(sys.argv[2], "rb").read()
        c = ctab(blob)
        print(shader_version(blob), c[0] if c else "", c[1] if c else "")
        for name, rset, idx, cnt, ts in (c[2] if c else []):
            print("%-8s c%-3d n=%-3d %-14s %s" % (rset, idx, cnt, ts, name))
    elif sys.argv[1] == "pres":
        for line in preshader(open(sys.argv[2], "rb").read()):
            print(line)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
