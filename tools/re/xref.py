import sys, pefile, struct
EXE = sys.argv[1]; TARGETS = [int(a,16) for a in sys.argv[2:]]
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
secs = [(s.Name.decode().rstrip('\0'), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def off2va(off):
    for n,va,vs,pr,sr in secs:
        if pr <= off < pr+sr: return base+va+(off-pr)
def sec_of_off(off):
    for n,va,vs,pr,sr in secs:
        if pr <= off < pr+sr: return n
for T in TARGETS:
    print("=== xrefs to 0x%x ===" % T)
    n=0
    for n_,va,vs,pr,sr in secs:
        blob = data[pr:pr+sr]
        # direct call/jmp rel32
        for i in range(len(blob)-5):
            op = blob[i]
            if op in (0xE8,0xE9):
                rel = struct.unpack_from('<i', blob, i+1)[0]
                src = base+va+i
                if src+5+rel == T:
                    print("  %s  0x%08x  %s rel32" % (n_, src, 'call' if op==0xE8 else 'jmp'))
                    n+=1
        # absolute pointer (vtables / data)
        for i in range(0, len(blob)-4):
            if struct.unpack_from('<I', blob, i)[0] == T:
                print("  %s  0x%08x  abs ptr" % (n_, base+va+i, ))
                n+=1
    print("  total: %d" % n)
