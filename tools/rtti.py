import sys, pefile, struct, pickle, bisect
EXE="/var/home/proto/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/HELLGATE_London/bin/Hellgate_sp_x86.exe"
pe = pefile.PE(EXE, fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.__data__
secs=[(s.Name.decode().rstrip('\0'), base+s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def o(v):
    for n,va,vs,pr,sr in secs:
        if va<=v<va+max(vs,sr):
            f=pr+(v-va)
            return f if f < pr+sr else None
def u32(v):
    f=o(v); return struct.unpack_from('<I',data,f)[0] if f is not None else None
def cstr(v):
    f=o(v); e=data.index(b'\0',f); return data[f:e].decode('latin1')
def vt_name(vt):
    col=u32(vt-4)
    if not col: return None
    td=u32(col+12)
    if not td: return None
    try: return cstr(td+8)
    except Exception: return None
d=pickle.load(open('/tmp/claude-1000/-var-home-proto-projects-hellgate-london-fix/729774f4-c7a7-41c9-8899-9729326bbca6/scratchpad/cg.pkl','rb'))
absptrs=d['absptrs']
for fn in [int(x,16) for x in sys.argv[1:]]:
    print("== fn 0x%08x ==" % fn)
    for loc in absptrs.get(fn,[]):
        # walk back to vtable start: find first slot whose [-4] is a valid COL with a name
        for back in range(0, 400, 4):
            vt = loc-back
            nm = vt_name(vt)
            if nm:
                print("   vtable 0x%08x  slot %d  class %s" % (vt, back//4, nm)); break
        else:
            print("   loc 0x%08x : no RTTI within 100 slots" % loc)
