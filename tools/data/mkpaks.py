#!/usr/bin/env python3
"""
Build the restore paks into the game's data dir (docs/reference/2007-vs-2018.md).

    mkpaks.py <game dir> [2007 Data dir]

- sp_hellgate_2337 (family hellgate): two stock tables, patched in place.
  sku.txt: lowQualityMoviesOnly (row +0x80) cleared on every SKU, so the
  movie player may take the high-quality file. movielists.txt: movie 34
  (LogoHanbitSoft) dropped from every list, so the start-up logo is gone.
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

DISC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ref", "retail-2007",
                    "extract", "ProgramFiles", "Flagship Studios", "Hellgate London", "Data")
SUFFIX = "2337"
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
        print(hgpak.build(data, "sp_hellgate_" + SUFFIX, tables))

        if not os.path.exists(os.path.join(disc, "hellgate_movieshigh000.dat")):
            print("no 2007 disc data at %s: HD movies skipped" % disc)
            return
        movie_pak(data, "hellgate_movieslow", disc, "hellgate_movieshigh000", STORY, tmp)
        movie_pak(data, "hellgate_movies", disc, "hellgate_movies000",
                  ["endcredits_" + c for c in CREDITS], tmp, MENU)


if __name__ == "__main__":
    main()
