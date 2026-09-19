import sys, pefile, struct, bisect, json
EXE = sys.argv[1]
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
text = [s for s in pe.sections if s.Name.decode().rstrip('\0')=='.text'][0]
tva, tpr, tsr = base+text.VirtualAddress, text.PointerToRawData, text.SizeOfRawData
blob = data[tpr:tpr+tsr]
calls = []            # (src_va, dst_va)
targets = set()
for i in range(len(blob)-5):
    if blob[i]==0xE8:
        rel = struct.unpack_from('<i', blob, i+1)[0]
        src = tva+i; dst = src+5+rel
        if tva <= dst < tva+tsr:
            calls.append((src,dst)); targets.add(dst)
# also absolute pointers anywhere in image pointing into .text = vtable/func-ptr entries
absptrs = {}
for s in pe.sections:
    pr, sr, va = s.PointerToRawData, s.SizeOfRawData, base+s.VirtualAddress
    b = data[pr:pr+sr]
    for i in range(0, len(b)-4, 4):
        v = struct.unpack_from('<I', b, i)[0]
        if tva <= v < tva+tsr:
            absptrs.setdefault(v, []).append(va+i)
            targets.add(v)
ents = sorted(targets)
def fn_of(va):
    i = bisect.bisect_right(ents, va)-1
    return ents[i] if i>=0 else None
out = {'base':base,'text':(tva,tsr),'entries':ents,
       'calls':calls,'absptrs':{hex(k):[hex(x) for x in v] for k,v in absptrs.items()}}
import pickle
pickle.dump({'ents':ents,'calls':calls,'absptrs':absptrs}, open(sys.argv[2],'wb'))
print("entries=%d calls=%d absptr_targets=%d" % (len(ents), len(calls), len(absptrs)))
