import sys
def blocks(path):
    b={}; cur=None
    for l in open(path):
        l=l.rstrip('\n')
        if l.startswith('TICK '): cur=int(l[5:]); b[cur]=[]; continue
        if cur is None: continue
        if l.startswith(('GLB','ACT','PRJ','AOB')): b[cur].append(l)
        elif l.startswith('== '): cur=None
    return b
o=blocks(sys.argv[1]); p=blocks(sys.argv[2])
common=[t for t in sorted(p) if t in o]
bad=[t for t in common if o[t]!=p[t]]
print("ticks compared",len(common),"differ",len(bad))
for t in bad[:int(sys.argv[3]) if len(sys.argv)>3 else 1]:
    print("first differing tick",t)
    for a,b in zip(o[t],p[t]):
        if a!=b: print(" O",a); print(" P",b)
    if len(o[t])!=len(p[t]): print(" lens",len(o[t]),len(p[t]), o[t][len(p[t]):], p[t][len(o[t]):])
