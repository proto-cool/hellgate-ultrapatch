import sys, pefile
A, B = sys.argv[1], sys.argv[2]
a = open(A,'rb').read(); b = open(B,'rb').read()
pe = pefile.PE(A, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
secs=[(s.Name.decode().rstrip('\0'), base+s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def loc(off):
    for n,va,vs,pr,sr in secs:
        if pr<=off<pr+sr: return n, va+(off-pr)
    return '?', None
print("sizes: %d vs %d" % (len(a), len(b)))
runs=[]; i=0; n=min(len(a),len(b))
while i < n:
    if a[i]!=b[i]:
        j=i
        while j<n and (a[j]!=b[j] or (j+8<n and a[j:j+8]!=b[j:j+8])): j+=1
        runs.append((i,j)); i=j
    else: i+=1
print("changed runs: %d   total bytes: %d" % (len(runs), sum(y-x for x,y in runs)))
print()
for s,e in runs:
    sec, va = loc(s)
    print("--- %-8s VA 0x%08x  off 0x%06x  len %d" % (sec, va or 0, s, e-s))
    print("    orig: %s" % a[s:e][:48].hex(' '))
    print("    new : %s" % b[s:e][:48].hex(' '))
