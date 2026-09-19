import pefile, struct, re, bisect, pickle
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
# locate Tt* string literals
strs={}
for m in re.finditer(rb'Tt[A-Za-z0-9_]{2,20}\x00', data):
    va=off2va(m.start())
    if va: strs[va]=m.group(0)[:-1].decode()
# xref each
refs={}
for n,va,vs,pr,sr in secs:
    b=data[pr:pr+sr]
    for i in range(len(b)-4):
        v=struct.unpack_from('<I',b,i)[0]
        if v in strs:
            refs.setdefault(strs[v],set()).add(fn_of(va+i) if n=='.text' else None)
for k in sorted(refs):
    fns=sorted(x for x in refs[k] if x)
    print("%-18s %s" % (k, " ".join("0x%08x"%f for f in fns) or "(no .text ref)"))
