import sys, pefile, struct
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 're'))
from hgpaths import EXE, CALLGRAPH
pe=pefile.PE(EXE,fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.__data__
lo=int(sys.argv[1],16); hi=int(sys.argv[2],16)
for s in pe.sections:
    n=s.Name.decode().rstrip('\0'); pr,sr,va=s.PointerToRawData,s.SizeOfRawData,base+s.VirtualAddress
    b=data[pr:pr+sr]
    for i in range(len(b)-4):
        v=struct.unpack_from('<I',b,i)[0]
        if lo<=v<=hi:
            print("%-8s ref@0x%08x -> 0x%08x" % (n, va+i, v))
