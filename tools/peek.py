import sys, pefile, struct
EXE="/var/home/proto/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London/bin/Hellgate_sp_x86.exe"
pe=pefile.PE(EXE,fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.__data__
secs=[(s.Name.decode().rstrip('\0'), base+s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def o(v):
    for n,va,vs,pr,sr in secs:
        if va<=v<va+max(vs,sr):
            f=pr+(v-va)
            return f if f<pr+sr else None
def sec(v):
    for n,va,vs,pr,sr in secs:
        if va<=v<va+max(vs,sr): return n
v0=int(sys.argv[1],16); n=int(sys.argv[2])
for i in range(n):
    v=v0+i*4; f=o(v)
    w=struct.unpack_from('<I',data,f)[0] if f is not None else 0
    print("0x%08x: 0x%08x  %s" % (v,w, sec(w) or ''))
