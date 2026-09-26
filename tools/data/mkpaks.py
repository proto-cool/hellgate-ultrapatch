#!/usr/bin/env python3
"""
Build the restore paks into the game's data dir (docs/reference/2007-vs-2018.md).

    mkpaks.py <game dir> [2007 Data dir]

- sp_hellgate_2337 (family hellgate): two stock tables, patched in place.
  sku.txt: lowQualityMoviesOnly (row +0x80) cleared on every SKU, so the
  movie player may take the high-quality file. movielists.txt: movie 34
  (LogoHanbitSoft) dropped from every list, so the start-up logo is gone.
  inventory.txt (data_common): colorSetPriority (row +0x38) set to -1, the
  bags' "never", on every equipment slot but the torso. The whole outfit
  takes one colour set, from the highest-priority equipped item that has
  one (dye kit 5, torso 4, helm and pants 2, belt, boots, shoulders, arms
  and trinkets 1); a chest piece without one wore the belt's colours, and
  changing the belt recoloured it (2026-09-25). The dye kit and the
  fashion slots keep theirs.
  particles/get hit shield * rune: the rune sphere a hit on your shields
  throws around you, faded to a third (its alpha and glow keys scaled in
  place): on a melee character it went off with every hit and got in the
  way of seeing the fight (2026-09-25).
- sp_hellgate_movieslow_2337 (family hellgate_movieslow): the 2007 disc's
  1920x1088 story movies (Scene 1-5/6/7/8, Truth 1-5) under their _high and
  their _low names. The player (FUN_004b6552) takes the low file first when
  the platform caps read low (DAT_00eda0fc < 400 or DAT_00eda100 < 1700,
  which they do under Wine), and a path is looked up within its own pak
  family, so the HD body has to answer the low name in the low family.
- sp_hellgate_movies_2337 (family hellgate_movies): the six 1680x1056 end
  credits movies, both names, in the family that holds the stock low ones;
  and the main menu backgrounds: the stock titlescreen(_wide)_high files
  (1600x1200, 1920x1088) under their _low names, which the menu plays for
  the same reason.

- sp_hellgate_bghigh_2337 (family hellgate_bghigh): full-detail textures
  2018 replaced with the low-detail copy (eight; seven only at half size). background/city/razorwire.dds
  was a 128x128 DXT1 with a 1-bit cut-out in 2007; 2018 put the 64x64 DXT5
  low texture there, its definition still converts it to DXT1, and the
  conversion drops the alpha: every wire card drew as an opaque black bar
  (the barbed wire on the character select, in everyone's game,
  2026-09-24). The 2007 files, from the disc.
- sp_hellgate_localized_2337 (family hellgate_localized): the English
  string tables that strings/english.tsv changes, rebuilt from the newest
  stock copy with every row of each listed key replaced (Timecode rows kept).

The 2007 movies come from the retail disc (ref/retail-2007, see
docs/reference/2007-vs-2018.md); without it only the tables pak is built.
Stock tables are taken from the newest copy in the stock paks.
"""
import os
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hgdat  # noqa: E402
import hgpak  # noqa: E402
import hgstrings  # noqa: E402

DISC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ref", "retail-2007",
                    "extract", "ProgramFiles", "Flagship Studios", "Hellgate London", "Data")
SUFFIX = "2337"
FIXES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "strings", "english.tsv")
LOGO_HANBIT = 34
STORY = ["scene 1-5", "scene 6", "scene 7", "scene 8",
         "truth 1", "truth 2", "truth 3", "truth 4", "truth 5"]
MENU = ["titlescreen", "titlescreen_wide"]
CREDITS = ["coventgarden", "hell", "picadilly", "picadillynew", "stpauls", "thames"]
CX = b"cxeh"


def read_entries(idx):
    """(path, offset, size, compressed size, file time) for every entry, in order."""
    d = hgdat.decrypt(open(idx, "rb").read())
    _, _, n = struct.unpack_from("<III", d, 0)
    _, nstr, blk = struct.unpack_from("<III", d, 12)
    strs = [s.decode("latin1") for s in d[24:24 + blk].split(b"\0")[:nstr]]
    p = 24 + blk + 4 + nstr * 6 + 4
    for i in range(n):
        f = struct.unpack_from("<IIIqiiiiiqiiiiiiiI", d, p + 80 * i)
        yield strs[f[7]] + strs[f[8]], f[3], f[4], f[5], f[9]


def newest(data, pak, path):
    best = None
    for e in read_entries(os.path.join(data, pak + ".idx")):
        if e[0].lower() == path and (best is None or e[4] > best[4]):
            best = e
    if best is None:
        sys.exit("%s: %s not found" % (pak, path))
    _, off, usz, csz, _ = best
    with open(os.path.join(data, pak + ".dat"), "rb") as fh:
        fh.seek(off)
        body = fh.read(csz or usz)
    return zlib.decompress(body) if csz else body


def rows(buf):
    """(first row offset, row size, row count) of a cooked excel table."""
    o = 28
    assert buf[o:o + 4] == CX
    (sl,) = struct.unpack_from("<i", buf, o + 4)
    o += 8 + sl
    assert buf[o:o + 4] == CX
    (n,) = struct.unpack_from("<i", buf, o + 4)
    start = o = o + 8
    while True:  # rows end at the next token whose distance is a multiple of n
        o = buf.find(CX, o)
        if o < 0:
            sys.exit("table layout not recognised")
        if (o - start) % n == 0:
            return start, (o - start) // n, n
        o += 1


def patch_sku(buf):
    buf = bytearray(buf)
    start, rs, n = rows(buf)
    for i in range(n):
        struct.pack_into("<i", buf, start + i * rs + 0x80, 0)
    return bytes(buf)


def patch_movielists(buf):
    buf = bytearray(buf)
    start, rs, n = rows(buf)
    for i in range(n):
        for base in (0x50, 0x70):  # list1a..h, list2a..h
            at = start + i * rs + base
            ids = [m for m in struct.unpack_from("<8i", buf, at) if m != LOGO_HANBIT]
            struct.pack_into("<8i", buf, at, *(ids + [-1] * (8 - len(ids))))
    return bytes(buf)


NO_COLORSET = {b"helm", b"goggles", b"shoulders", b"rhand", b"lhand", b"arms", b"belt", b"boots",
               b"pants", b"trinket_ring", b"trinket_necklace", b"trinket_bracelet"}


def patch_inventory(buf):
    buf = bytearray(buf)
    start, rs, n = rows(buf)
    done = 0
    for i in range(n):
        o = start + i * rs
        name = bytes(buf[o + 16:o + 48]).split(b"\0")[0]
        typ, _, pri = struct.unpack_from("<iii", buf, o + 48)
        if typ == 1 and name in NO_COLORSET and pri >= 0:
            struct.pack_into("<i", buf, o + 0x38, -1)
            done += 1
    if done != len(NO_COLORSET):
        sys.exit("inventory.txt: %d of %d slots found" % (done, len(NO_COLORSET)))
    return bytes(buf)


RUNES = ["get hit shield fp rune", "get hit shield monster rune", "get hit shield monster rune s",
         "get hit shield monster rune l", "get hit shield monster rune xl", "get hit shield monster rune xxl"]
RUNE_ALPHA, RUNE_GLOW = 0.35, 0.3


def patch_rune(buf):
    """Scale a particle definition's tParticleAlpha and tParticleGlow keys in
    place: each path is its (time, min, max) floats back to back, found by
    value (tools/data/hguncook.py reads them) and rewritten, same size."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import hguncook
    buf = bytearray(buf)
    c = hguncook.Cooked(bytes(buf))
    done = 0
    for e in c.data["values"]:
        name, val = e[0].name, e[1]
        k = RUNE_ALPHA if name == "tParticleAlpha" else RUNE_GLOW if name == "tParticleGlow" else None
        if k is None or not isinstance(val, list) or not val:
            continue
        old = b"".join(struct.pack("<fff", *key) for key in val)
        new = b"".join(struct.pack("<fff", key[0], key[1] * k, key[2] * k) for key in val)
        at = bytes(buf).find(old)
        if at < 0 or bytes(buf).find(old, at + 1) >= 0:
            sys.exit("rune particle: %s not found once" % name)
        buf[at:at + len(old)] = new
        done += 1
    if done != 2:
        sys.exit("rune particle: %d of 2 paths patched" % done)
    return bytes(buf)


def extract(disc, pak, names, tmp):
    """Write the named 2007 files to tmp; return {name: path}."""
    want = {"data\\cinematic\\" + n: n for n in names}
    out = {}
    with open(os.path.join(disc, pak + ".dat"), "rb") as fh:
        for path, off, usz, csz, _ in read_entries(os.path.join(disc, pak + ".idx")):
            if path.lower() in want:
                fh.seek(off)
                dst = os.path.join(tmp, want[path.lower()])
                with open(dst, "wb") as o:
                    left = usz
                    while left:
                        chunk = fh.read(min(left, 1 << 24))
                        o.write(chunk)
                        left -= len(chunk)
                out[want[path.lower()]] = dst
    missing = set(names) - set(out)
    if missing:
        sys.exit("2007 %s lacks %s" % (pak, ", ".join(sorted(missing))))
    return out


def movie_pak(data, family, disc, disc_pak, stems, tmp, stock=()):
    """stems: 2007 _high movies, under both names; stock: stock _high movies
    from the same family, under their _low name only."""
    src = extract(disc, disc_pak, [s + "_high.bik" for s in stems], tmp)
    files = []
    for s in stems:
        for q in ("_high.bik", "_low.bik"):
            files.append(("data\\cinematic\\" + s + q, src[s + "_high.bik"]))
    if stock:
        own = extract(data, family + "000", [s + "_high.bik" for s in stock], tmp)
        src.update(own)
        for s in stock:
            files.append(("data\\cinematic\\" + s + "_low.bik", own[s + "_high.bik"]))
    print(hgpak.build(data, "sp_%s_%s" % (family, SUFFIX), files))
    for p in src.values():
        os.remove(p)


def read_fixes(path):
    fixes = {}
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        table, key, text = line.split("\t", 2)
        if len(text) > 1 and text[0] == text[-1] == '"':
            text = text[1:-1]
        fixes.setdefault(table, {})[key] = text.replace("\\n", "\n")
    return fixes


def strings_pak(data, tmp):
    files = []
    for table, keys in sorted(read_fixes(FIXES).items()):
        path = "data\\excel\\strings\\english\\%s.xls.uni.cooked" % table
        ver, rows = hgstrings.parse(newest(data, "hellgate_localized000", path))
        seen = set()
        for r in rows:
            if r.key in keys and "Timecode" not in r.attrs:
                r.text = keys[r.key]
                seen.add(r.key)
        missing = set(keys) - seen
        if missing:
            sys.exit("%s: no such key: %s" % (table, ", ".join(sorted(missing))))
        dst = os.path.join(tmp, table)
        open(dst, "wb").write(hgstrings.build(ver, rows))
        files.append((path, dst))
        print("  %s: %d key(s)" % (table, len(keys)))
    print(hgpak.build(data, "sp_hellgate_localized_" + SUFFIX, files))


# 2007 full-detail textures 2018 replaced (2007 pak, path)
# (found by comparing every 2018 full-detail texture that is a byte copy of
# its low one against 2007: razorwire lost its cut-out alpha, the rest are
# half the 2007 resolution)
BGHIGH_2007 = [
    ("hellgate000", "data\\background\\city\\razorwire.dds"),
    ("hellgate000", "data\\background\\city\\park_boulders.dds"),
    ("hellgate000", "data\\background\\hell\\hell_gradient.dds"),
    ("hellgate000", "data\\background\\hell\\hell_gradient_glow.dds"),
    ("hellgate000", "data\\background\\hell\\hell_runes.dds"),
    ("hellgate000", "data\\background\\hell\\hell_terrain_d.dds"),
    ("hellgate000", "data\\background\\hell\\hell_terrain_e.dds"),
    ("hellgate000", "data\\background\\props\\britishmuseum\\pottery_a_dffuse.dds"),
]


def bghigh_2007(disc, tmp):
    out = []
    for pak, path in BGHIGH_2007:
        p = os.path.join(tmp, "2007_" + path.rsplit("\\", 1)[1])
        open(p, "wb").write(newest(disc, pak, path))
        out.append((path, p))
    return out


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    data = os.path.join(sys.argv[1], "data")
    disc = sys.argv[2] if len(sys.argv) == 3 else DISC
    with tempfile.TemporaryDirectory() as tmp:
        tables = []
        for name, fix in (("sku", patch_sku), ("movielists", patch_movielists)):
            p = os.path.join(tmp, name)
            open(p, "wb").write(fix(newest(data, "hellgate000", "data\\excel\\%s.txt.cooked" % name)))
            tables.append(("data\\excel\\%s.txt.cooked" % name, p))
        p = os.path.join(tmp, "inventory")
        open(p, "wb").write(patch_inventory(newest(data, "hellgate000", "data_common\\excel\\inventory.txt.cooked")))
        tables.append(("data_common\\excel\\inventory.txt.cooked", p))
        for i, r in enumerate(RUNES):
            path = "data\\particles\\%s.xml.cooked" % r
            p = os.path.join(tmp, "rune%d" % i)
            open(p, "wb").write(patch_rune(newest(data, "hellgate000", path)))
            tables.append((path, p))
        print(hgpak.build(data, "sp_hellgate_" + SUFFIX, tables))
        strings_pak(data, tmp)

        if not os.path.exists(os.path.join(disc, "hellgate_movieshigh000.dat")):
            print("no 2007 disc data at %s: HD movies and 2007 textures skipped" % disc)
            return
        print(hgpak.build(data, "sp_hellgate_bghigh_" + SUFFIX, bghigh_2007(disc, tmp)))
        movie_pak(data, "hellgate_movieslow", disc, "hellgate_movieshigh000", STORY, tmp)
        movie_pak(data, "hellgate_movies", disc, "hellgate_movies000",
                  ["endcredits_" + c for c in CREDITS], tmp, MENU)


if __name__ == "__main__":
    main()
