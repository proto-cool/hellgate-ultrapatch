import sys, pefile, struct, re, bisect, pickle
EXE="/var/home/proto/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London/bin/Hellgate_sp_x86.exe"
pe=pefile.PE(EXE,fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.__data__
secs=[(s.Name.decode().rstrip('\0'), base+s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def off2va(f):
    for n,va,vs,pr,sr in secs:
        if pr<=f<pr+sr: return va+(f-pr)
d=pickle.load(open('/tmp/claude-1000/-var-home-proto-projects-hellgate-london-fix/729774f4-c7a7-41c9-8899-9729326bbca6/scratchpad/cg.pkl','rb'))
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
