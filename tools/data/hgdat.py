#!/usr/bin/env python3
"""
List and extract files from the game's .idx/.dat archives.

    hgdat.py list    [pattern]          # substring match on the path, case-insensitive
    hgdat.py extract <outdir> [pattern]

The .idx is encrypted with a byte-wise additive LCG stream. Keys and layout
come from Reanimator (maeyan-zero/reanimator, and the Steam-era fork
ti360gh/Reanimator-steam, hellgate/Crypt.cs and IndexFile.cs); they decrypt
the 2018 Steam index unchanged. File bodies are zlib when compressed size > 0.

Extracted .xml.cooked files are the engine's binary cooked format ('CO0k'),
not XML; Reanimator-steam's hellpack uncooks them. .fxo are compiled D3D
effects: fx_2_0 for dx9, DXBC for dx10.
"""
import os
import struct
import sys
import zlib

GAME = os.environ.get("HG_GAME") or os.path.expanduser(
    "~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London")
# HG_GAME=<root with a data/ dir> points the tool at another install, e.g.
# the 2007 retail disc (ref/retail-2007) for diffing against the Steam build.

# IDX stream cipher (Reanimator Crypt.CryptState).
K1, K2, K3, BLOCK = 0x10DCD, 0xF4559D5, 666, 32
TABLE = BLOCK * 4

TOK_HEAD, TOK_SECT, TOK_INFO = 0x6867696E, 0x68677073, 0x6867696F


def decrypt(buf):
    out = bytearray(buf)
    for base in range(0, len(out), TABLE):
        x = (base + K3) & 0xFFFFFFFF
        tab = bytearray()
        for _ in range(BLOCK):
            x = (x * K1 + K2) & 0xFFFFFFFF
            tab += struct.pack("<I", x)
        for i in range(min(TABLE, len(out) - base)):
            out[base + i] = (out[base + i] - tab[i]) & 0xFF
    return bytes(out)


def read_index(idx_path):
    d = decrypt(open(idx_path, "rb").read())
    head, ver, nfiles = struct.unpack_from("<III", d, 0)
    if head != TOK_HEAD:
        sys.exit("%s: bad header 0x%08x after decrypt -- different key?" % (idx_path, head))
    tok, nstr, blk = struct.unpack_from("<III", d, 12)
    p = 24
    strs = [s.decode("latin1") for s in d[p:p + blk].split(b"\0")[:nstr]]
    p += blk + 4 + nstr * 6 + 4          # string details section, then the file section token
    files = []
    for i in range(nfiles):
        f = struct.unpack_from("<IIIqiiiiiqiiiiiiiI", d, p)
        if f[0] != TOK_INFO or f[17] != TOK_INFO:
            sys.exit("entry %d: bad tokens 0x%08x/0x%08x" % (i, f[0], f[17]))
        _, _, _, off, usz, csz, _, di, ni = f[:9]
        files.append((strs[di] + strs[ni], off, usz, csz))
        p += 80
    return files


def archives():
    data = os.path.join(GAME, "data")
    for n in sorted(os.listdir(data)):
        if n.endswith(".idx") and os.path.exists(os.path.join(data, n[:-4] + ".dat")):
            yield os.path.join(data, n), os.path.join(data, n[:-4] + ".dat")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("list", "extract"):
        sys.exit(__doc__)
    cmd = sys.argv[1]
    outdir = sys.argv[2] if cmd == "extract" else None
    if cmd == "extract" and not outdir:
        sys.exit(__doc__)
    pat = (sys.argv[3] if cmd == "extract" else sys.argv[2] if len(sys.argv) > 2 else "")
    pat = pat.lower()

    n = 0
    for idx, dat in archives():
        files = read_index(idx)
        fh = open(dat, "rb") if cmd == "extract" else None
        for name, off, usz, csz in files:
            if pat not in name.lower():
                continue
            n += 1
            if cmd == "list":
                print("%10d  %s" % (usz, name))
                continue
            fh.seek(off)
            body = fh.read(csz if csz > 0 else usz)
            if csz > 0:
                body = zlib.decompress(body)
            dst = os.path.join(outdir, *name.split("\\"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(body)
    print("%d file(s)" % n, file=sys.stderr)


if __name__ == "__main__":
    main()
