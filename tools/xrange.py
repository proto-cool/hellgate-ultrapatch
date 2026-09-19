import sys, pefile, struct
EXE="/var/home/proto/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London/bin/Hellgate_sp_x86.exe"
pe=pefile.PE(EXE,fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.__data__
lo=int(sys.argv[1],16); hi=int(sys.argv[2],16)
for s in pe.sections:
    n=s.Name.decode().rstrip('\0'); pr,sr,va=s.PointerToRawData,s.SizeOfRawData,base+s.VirtualAddress
    b=data[pr:pr+sr]
    for i in range(len(b)-4):
        v=struct.unpack_from('<I',b,i)[0]
        if lo<=v<=hi:
            print("%-8s ref@0x%08x -> 0x%08x" % (n, va+i, v))
