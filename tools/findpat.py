import sys, pefile, re
EXE = sys.argv[1]
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
secs = [(s.Name.decode().rstrip('\0'), s.VirtualAddress, s.Misc_VirtualSize,
         s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def off2va(off):
    for n,va,vs,pr,sr in secs:
        if pr <= off < pr+sr: return base+va+(off-pr)
    return None
def va2off(v):
    r = v-base
    for n,va,vs,pr,sr in secs:
        if va <= r < va+max(vs,sr): return pr+(r-va)
    return None

PAT = ("55 8b ec 83 e4 f0 6a ff 68 ?? ?? ?? ?? 64 a1 00 00 00 00 50 "
       "81 ec e8 04 00 00 a1 ?? ?? ?? ?? 33 c4 89 84 24 e0 04 00 00 "
       "53 56 57 a1 ?? ?? ?? ?? 33 c4 50 8d 84 24 f8 04 00 00 "
       "64 a3 00 00 00 00 80 3d ?? ?? ?? ?? 00")
rx = b''.join(b'.' if t=='??' else re.escape(bytes([int(t,16)])) for t in PAT.split())
for m in re.finditer(rx, data, re.DOTALL):
    print("file_off=0x%x  VA=0x%x  RVA=0x%x" % (m.start(), off2va(m.start()), off2va(m.start())-base))
