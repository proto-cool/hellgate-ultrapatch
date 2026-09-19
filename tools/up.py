import sys, pickle, bisect
d = pickle.load(open('/tmp/claude-1000/-var-home-proto-projects-hellgate-london-fix/729774f4-c7a7-41c9-8899-9729326bbca6/scratchpad/cg.pkl','rb'))
ents, calls, absptrs = d['ents'], d['calls'], d['absptrs']
callers = {}
for src,dst in calls: callers.setdefault(dst, []).append(src)
def fn_of(va):
    i = bisect.bisect_right(ents, va)-1
    return ents[i] if i>=0 else None
start = int(sys.argv[1],16); depth = int(sys.argv[2])
seen=set()
def walk(f, d_, pre):
    if d_>depth: return
    tag = " [VT:%d]"%len(absptrs.get(f,[])) if f in absptrs else ""
    print("%s0x%08x%s  (direct callers: %d)" % (pre, f, tag, len(set(fn_of(s) for s in callers.get(f,[])))))
    if f in seen: print(pre+"  ...seen"); return
    seen.add(f)
    ups = sorted(set(fn_of(s) for s in callers.get(f,[]) if fn_of(s) is not None))
    for u in ups: walk(u, d_+1, pre+"    ")
walk(start, 0, "")
