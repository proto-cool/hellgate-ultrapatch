#!/usr/bin/env python3
"""
Read and write the game's cooked string tables (.xls.uni.cooked).

    hgstrings.py dump <file> [pattern]      # key<TAB>text, one row per line
    hgstrings.py check <file> [...]         # parse, rewrite, compare bytes

Layout (Reanimator-steam, hellgate/StringsFile.cs; version 6 in both the
2007 retail and the 2018 Steam build): header 'hfst' (0x68667374), version,
row count; then per row: reference id, an unknown int, key length, the
ASCII key and its NUL, a reserved int, the text's byte count (including its
UTF-16 NUL) and the text, an attribute count, and per attribute its length
in characters and the UTF-16 text with its NUL. A key can appear on two
rows (the cinematic table's text and its Timecode row), so rows are a list.
"""
import struct
import sys

MAGIC, VERSION = 0x68667374, 6


class Row:
    __slots__ = ("ref", "unk", "key", "reserved", "text", "attrs")

    def __init__(self, ref, unk, key, reserved, text, attrs):
        self.ref, self.unk, self.key = ref, unk, key
        self.reserved, self.text, self.attrs = reserved, text, attrs


def parse(buf):
    magic, ver, n = struct.unpack_from("<III", buf, 0)
    if magic != MAGIC:
        raise ValueError("not a string table (magic 0x%08x)" % magic)
    o, rows = 12, []
    for _ in range(n):
        ref, unk, klen = struct.unpack_from("<iii", buf, o)
        o += 12
        key = buf[o:o + klen].decode("latin1")
        o += klen + 1
        reserved, tlen = struct.unpack_from("<ii", buf, o)
        o += 8
        text = buf[o:o + tlen].decode("utf-16le")
        o += tlen
        if not text.endswith("\0"):
            raise ValueError("row %r: text without NUL" % key)
        (na,) = struct.unpack_from("<i", buf, o)
        o += 4
        attrs = []
        for _ in range(na):
            (c,) = struct.unpack_from("<i", buf, o)
            o += 4
            attrs.append(buf[o:o + (c + 1) * 2].decode("utf-16le")[:-1])
            o += (c + 1) * 2
        rows.append(Row(ref, unk, key, reserved, text[:-1], attrs))
    if o != len(buf):
        raise ValueError("%d trailing bytes" % (len(buf) - o))
    return ver, rows


def build(ver, rows):
    out = bytearray(struct.pack("<III", MAGIC, ver, len(rows)))
    for r in rows:
        k = r.key.encode("latin1")
        t = (r.text + "\0").encode("utf-16le")
        out += struct.pack("<iii", r.ref, r.unk, len(k)) + k + b"\0"
        out += struct.pack("<ii", r.reserved, len(t)) + t
        out += struct.pack("<i", len(r.attrs))
        for a in r.attrs:
            out += struct.pack("<i", len(a)) + (a + "\0").encode("utf-16le")
    return bytes(out)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("dump", "check"):
        sys.exit(__doc__)
    if sys.argv[1] == "dump":
        pat = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for r in parse(open(sys.argv[2], "rb").read())[1]:
            if pat in r.key.lower() or pat in r.text.lower():
                print("%s\t%s" % (r.key, r.text.replace("\n", "\\n")))
        return
    bad = 0
    for f in sys.argv[2:]:
        buf = open(f, "rb").read()
        ok = build(*parse(buf)) == buf
        bad += not ok
        print("%s  %s" % ("ok  " if ok else "DIFF", f))
    sys.exit(bad and 1)


if __name__ == "__main__":
    main()
