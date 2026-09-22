import sys, pefile, struct, re, bisect, pickle
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 're'))
from hgpaths import EXE, CALLGRAPH
pe=pefile.PE(EXE,fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.__data__
secs=[(s.Name.decode().rstrip('\0'), base+s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def off2va(f):
    for n,va,vs,pr,sr in secs:
        if pr<=f<pr+sr: return va+(f-pr)
d=pickle.load(open(CALLGRAPH,'rb'))
ents=d['ents']
def fn_of(v):
    i=bisect.bisect_right(ents,v)-1
    return ents[i] if i>=0 else None
for pat in sys.argv[1:]:
    needle=pat.encode()+b'\x00'
    hits=[m.start() for m in re.finditer(re.escape(needle), data)]
    print("=== %-42s (%d copies)" % (pat, len(hits)))
    for h in hits:
        va=off2va(h)
        if va is None: continue
        refs=[]
        for n,sva,vs,pr,sr in secs:
            b=data[pr:pr+sr]
            for m in re.finditer(re.escape(struct.pack('<I',va)), b):
                refs.append((n, sva+m.start()))
        print("    str@0x%08x refs=%d %s" % (va, len(refs),
              " ".join("%s:0x%08x(fn 0x%08x)"%(n,a,fn_of(a) or 0) if n=='.text' else "%s:0x%08x"%(n,a) for n,a in refs[:6])))
