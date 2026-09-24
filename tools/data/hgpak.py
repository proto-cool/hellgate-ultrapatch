#!/usr/bin/env python3
"""
Write a pak (.idx + .dat) the game mounts next to its own.

    hgpak.py build <data dir> <name> <src dir>   # every file under src dir, at its relative path
    hgpak.py list  <idx>                         # read one back

The loader (FUN_00426795) globs <data>\\<family>*.idx for each family
(hellgate, hellgate_localized, ...), with the prefixes "", x_, sp_ and mp_,
and FUN_0042667b accepts a file only if what follows the family name is
digits, '_' and '.': "sp_hellgate_1337" is mounted, "sp_hellgate_ultra" is
not. The sp_ paks mount after the stock ones.

Index layout as tools/data/hgdat.py reads it (Reanimator-steam IndexFile.cs):
'nigh', version 4, file count; 'spgh', string count, string bytes, the
NUL-separated directory and file names; 'spgh', per string its length (i16)
and hash (the engine's CRC, hguncook.strhash); 'spgh'; then 80-byte entries
between 'oigh' tokens. Entry hashes are the first 4 bytes of SHA-1 of the
directory (with its trailing backslash) and of the name. The two words after
the file time are unknown and left 0, as Reanimator does. The .dat starts
with a 512-byte header copied from a stock pak, and bodies (zlib, as stock)
sit at 512-byte boundaries. Media (.bik .ogg .mp2 .wav) are stored
uncompressed, as stock stores them, and the .dat is written as it goes.
Paths that name the same source file share one body in the .dat.
"""
import hashlib
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hgdat  # noqa: E402
from hguncook import strhash  # noqa: E402

TOK_SECT, TOK_INFO = hgdat.TOK_SECT, hgdat.TOK_INFO
ALIGN = 512
RAW = (".bik", ".ogg", ".mp2", ".wav")


def encrypt(buf):
    out = bytearray(buf)
    for base in range(0, len(out), hgdat.TABLE):
        x = (base + hgdat.K3) & 0xFFFFFFFF
        tab = bytearray()
        for _ in range(hgdat.BLOCK):
            x = (x * hgdat.K1 + hgdat.K2) & 0xFFFFFFFF
            tab += struct.pack("<I", x)
        for i in range(min(hgdat.TABLE, len(out) - base)):
            out[base + i] = (out[base + i] + tab[i]) & 0xFF
    return bytes(out)


def sha4(s):
    return struct.unpack("<I", hashlib.sha1(s.encode("latin1")).digest()[:4])[0]


def filetime(t):
    return int((t + 11644473600) * 10**7)


def build(data_dir, name, files):
    """files: list of (pak path with backslashes, source file path)."""
    header = open(os.path.join(data_dir, "hellgate000.dat"), "rb").read(ALIGN)
    strs, index = [], {}

    def sid(s):
        if s not in index:
            index[s] = len(strs)
            strs.append(s)
        return index[s]

    base = os.path.join(data_dir, name)
    entries, written = [], {}
    now = filetime(time.time())
    with open(base + ".dat", "wb") as dat:
        dat.write(header)
        for path, src in files:
            d, n = path.rsplit("\\", 1)
            d += "\\"
            if src not in written:
                body = open(src, "rb").read()
                packed = body if n.lower().endswith(RAW) else zlib.compress(body, 9)
                off = dat.tell()
                dat.write(packed)
                dat.write(b"\0" * (-dat.tell() % ALIGN))
                written[src] = (off, len(body), 0 if packed is body else len(packed),
                                body[:8].ljust(8, b"\0"))
            off, usz, csz, head = written[src]
            entries.append(struct.pack(
                "<IIIqiiiiiqiiiiiiiI", TOK_INFO, sha4(d), sha4(n), off, usz, csz,
                0, sid(d), sid(n), now, 0, 0, 0, 0, 0, *struct.unpack("<ii", head),
                TOK_INFO))

    blob = b"".join(s.encode("latin1") + b"\0" for s in strs)
    idx = struct.pack("<III", hgdat.TOK_HEAD, 4, len(entries))
    idx += struct.pack("<III", TOK_SECT, len(strs), len(blob)) + blob
    idx += struct.pack("<I", TOK_SECT)
    idx += b"".join(struct.pack("<hI", len(s), strhash(s)) for s in strs)
    idx += struct.pack("<I", TOK_SECT) + b"".join(entries)
    open(base + ".idx", "wb").write(encrypt(idx))
    return base


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "list":
        for n, off, usz, csz in hgdat.read_index(sys.argv[2]):
            print("%10d  %s" % (usz, n))
        return
    if len(sys.argv) != 5 or sys.argv[1] != "build":
        sys.exit(__doc__)
    data_dir, name, src = sys.argv[2:]
    files = []
    for root, _, names in os.walk(src):
        for n in sorted(names):
            p = os.path.join(root, n)
            rel = os.path.relpath(p, src).replace(os.sep, "\\")
            files.append((rel, p))
    base = build(data_dir, name, files)
    print("%s.idx/.dat: %d file(s)" % (base, len(files)))


if __name__ == "__main__":
    main()
