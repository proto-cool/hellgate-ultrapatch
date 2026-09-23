#!/usr/bin/env python3
"""
Skip the Steam launcher's dialog: Hellgate.exe starts the game at once.

    python3 tools/launcher.py patch   <game dir>
    python3 tools/launcher.py restore <game dir>

Steam runs Hellgate.exe, a small MFC dialog whose only job is to start Steam
(SteamAPI_Init) and, when Play is pressed, run `bin\\Hellgate_sp_x86.exe 1 2`
and wait for it. The game itself never calls Steam, so the launcher has to
stay: the patch makes its OnInitDialog call the Play handler after the Steam
checks, before the dialog is ever shown. The handler hides the window, runs
the game, waits, and ends the dialog, as it does on a click.

OnInitDialog (0x4017e0) ends with the Steam checks; `this` + 0x110 is in esi
just before esi is restored:

  4018b7  add esp,4 ; pop edi ; pop esi ; test al,al
      ->  pop ecx   ; pop edi ; xchg esi,[esp] ; test al,al
          (the same, but this + 0x110 stays on the stack)
  4018df  mov eax,1 ; ret ; int3 x 11      (Steam is up)
      ->  pop ecx ; sub ecx,0x110 ; call 0x4019f0 (Play) ; push 1 ; pop eax ; ret

The failure paths end in exit(), so the extra stack slot never returns.
Only the known 2018-09-20 launcher is touched, and `restore` writes the
original bytes back (Steam's "verify integrity" also restores it).
"""
import hashlib
import os
import sys

STOCK = "66d3898c184e730717028613ad8c361b6597dcd730af4f2ebc866f7ca04e1e94"
SIZE = 2438144


def off(va):
    return va - 0x401000 + 0x400        # .text: VA 0x401000, file 0x400


def call(at, to):
    return b"\xe8" + ((to - (at + 5)) & 0xffffffff).to_bytes(4, "little")


PATCH = [
    (0x4018b7, bytes.fromhex("83c4045f5e84c0"), bytes.fromhex("595f87342484c0")),
    (0x4018df, bytes.fromhex("b801000000c3") + b"\xcc" * 11,
     bytes.fromhex("5981e910010000") + call(0x4018e6, 0x4019f0) + bytes.fromhex("6a0158c3") + b"\xcc"),
]


def apply(d, new):
    d = bytearray(d)
    for va, old, pat in PATCH:
        assert len(old) == len(pat)
        src, dst = (old, pat) if new else (pat, old)
        if d[off(va):off(va) + len(src)] != src:
            return None
        d[off(va):off(va) + len(dst)] = dst
    return bytes(d)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("patch", "restore"):
        sys.exit(__doc__)
    exe = os.path.join(sys.argv[2], "Hellgate.exe")
    if not os.path.exists(exe):
        print("launcher: no %s, skipped" % exe)
        return
    d = open(exe, "rb").read()
    stock = apply(d, False) if len(d) == SIZE else None
    if hashlib.sha256(d).hexdigest() == STOCK:
        state = "stock"
    elif stock is not None and hashlib.sha256(stock).hexdigest() == STOCK:
        state = "patched"
    else:
        print("launcher: %s is not the known 2018 launcher, left alone" % exe)
        return
    if sys.argv[1] == "patch":
        if state == "patched":
            print("launcher: already patched")
            return
        out = apply(d, True)
    else:
        if state == "stock":
            print("launcher: already stock")
            return
        out = stock
    tmp = exe + ".tmp"
    open(tmp, "wb").write(out)
    os.chmod(tmp, os.stat(exe).st_mode)
    os.replace(tmp, exe)
    print("launcher: %s -> %s" % (exe, "no dialog" if sys.argv[1] == "patch" else "stock"))


if __name__ == "__main__":
    main()
