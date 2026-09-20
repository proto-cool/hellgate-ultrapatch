# Walks the CMD_* keybind table. NOTE: +0x10 is a sequential command enum ID,
# NOT a key code -- an earlier version of this script decoded it as VK and
# produced nonsense. +0x14 is the default key and +0x18 the modifier
# (0x11=Ctrl, 0x10=Shift, seen on the CMD_DEBUG_* entries). The key encoding
# is not fully confirmed: CMD_AUTORUN reads 0x101, outside VK range, so this
# is likely the game's own key enum.
import pefile, struct, re
EXE="/tmp/hg_sp.exe"
pe=pefile.PE(EXE,fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase
d=bytes(pe.__data__)
secs=[(s.Name.decode().rstrip('\0'), base+s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def o(v):
    for n,va,vs,pr,sr in secs:
        if va<=v<va+max(vs,sr):
            f=pr+(v-va)
            return f if f<pr+sr else None
def cs(v):
    f=o(v)
    if f is None: return None
    e=d.find(b'\0',f)
    if e<0 or e-f>80: return None
    s=d[f:e]
    return s.decode('latin1') if s and all(32<=c<127 for c in s) else None
VK={0x01:'LMB',0x02:'RMB',0x04:'MMB',0x08:'BACKSPACE',0x09:'TAB',0x0D:'ENTER',0x10:'SHIFT',
0x11:'CTRL',0x12:'ALT',0x13:'PAUSE',0x14:'CAPS',0x1B:'ESC',0x20:'SPACE',0x21:'PGUP',
0x22:'PGDN',0x23:'END',0x24:'HOME',0x25:'LEFT',0x26:'UP',0x27:'RIGHT',0x28:'DOWN',
0x2D:'INS',0x2E:'DEL',0xC0:'` (GRAVE)',0xBD:'-',0xBB:'=',0xDB:'[',0xDD:']',0xDC:'\\',
0xBA:';',0xDE:"'",0xBC:',',0xBE:'.',0xBF:'/'}
for i in range(0x30,0x3A): VK[i]=chr(i)
for i in range(0x41,0x5B): VK[i]=chr(i)
for i in range(0x70,0x88): VK[i]='F%d'%(i-0x6F)
for i in range(0x60,0x6A): VK[i]='NUM%d'%(i-0x60)
def k(v):
    if v in (0,0xffffffff,-1): return '-'
    return VK.get(v, '0x%02x'%v)
STRIDE=0x38
# anchor: CMD_CONSOLE_TOGGLE name ptr lives at 0x00b9ff78, so entry starts 8 before
anchor=0x00b9ff78-8
start=anchor
while True:
    prev=start-STRIDE
    f=o(prev)
    if f is None: break
    nm=cs(struct.unpack_from('<I',d,o(prev+8))[0])
    if not (nm and nm.startswith('CMD_')): break
    start=prev
rows=[]; e=start
while True:
    f=o(e+8)
    if f is None: break
    nm=cs(struct.unpack_from('<I',d,f)[0])
    if not (nm and nm.startswith('CMD_')): break
    desc=cs(struct.unpack_from('<I',d,o(e))[0]) or ''
    k1=struct.unpack_from('<I',d,o(e+0x10))[0]
    k2=struct.unpack_from('<I',d,o(e+0x14))[0]
    rows.append((nm,desc,k1,k2)); e+=STRIDE
print("table at 0x%08x, %d entries\n"%(start,len(rows)))
import sys
want = sys.argv[1] if len(sys.argv)>1 else None
for nm,desc,k1,k2 in rows:
    if want and want.lower() not in nm.lower(): continue
    print("  %-34s %-10s %-10s  %s"%(nm,k(k1),k(k2),desc))
