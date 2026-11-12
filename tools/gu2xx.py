"""MHGU 1.4.0 -> MHXX 1.5.1 code ranges for runtime/game/gu2xx.h (plugins made for MHGU: runtime/gucompat.c).

  python gu2xx.py <GU main> <MHXX main> <generated/main> > ranges.txt

Both NSOs are your own dumps; nothing of them is kept, only {GU offset, MHXX offset, bytes} of the code the two share.
MHXX's functions (generated/main's f_<address>) are looked for in GU by their instructions with the distances of
calls and jumps left out: first where the last function was (the code is in the same order), then anywhere by the
first six instructions. A function of fewer than 8 instructions counts only at the distance of a larger neighbour.
"""
import bisect, glob, re, sys
import numpy as np
from nso import Nso

gu, xx, gen = sys.argv[1:4]
g = Nso(gu); x = Nso(xx)
G = np.frombuffer(bytes(g.image[:g.text[1] & ~3]), dtype='<u4')
starts = sorted({int(a, 16) - 0x200000 for f in glob.glob(gen + '/main_*.c') for a in re.findall(r'^void f_([0-9a-f]+)\(Cpu', open(f).read(), re.M)})
XT = x.text[1] & ~3
X = np.frombuffer(bytes(x.image[:XT]), dtype='<u4')
def mask(a):
    a=a.copy(); br=((a&0x0E000000)==0x0A000000)&((a>>28)!=0xF); a[br]&=0xFF000000; return a
Gm,Xm=mask(G),mask(X)
# index of GU by hash of 6 masked words (for global search)
K=6
def hashes(m):
    h=np.zeros(len(m)-K+1,dtype=np.uint64)
    for i in range(K): h=h*np.uint64(1000003)+m[i:len(m)-K+1+i].astype(np.uint64)
    return h
Gh=hashes(Gm)
order=np.argsort(Gh,kind='stable'); Ghs=Gh[order]
def find_gu(words):
    if len(words)<K: return []
    h=np.uint64(0)
    for w in words[:K]: h=h*np.uint64(1000003)+np.uint64(w)
    l=np.searchsorted(Ghs,h,'left'); r=np.searchsorted(Ghs,h,'right')
    return [int(order[i])*4 for i in range(l,min(r,l+50))]
def score(s,e,g):
    n=(e-s)//4
    if g<0 or g//4+n>len(Gm): return 0
    return int((Xm[s//4:e//4]==Gm[g//4:g//4+n]).sum())
res=[]  # (xx_start,len,gu_start,equal,n)
d=0; miss=0
for i,s in enumerate(starts):
    e=starts[i+1] if i+1<len(starts) else XT
    n=(e-s)//4
    if n==0: continue
    best=(score(s,e,s-d),s-d)
    if best[0]<n*0.9:
        cands=set(find_gu(Xm[s//4:e//4]))
        # nearby deltas
        for g in list(cands):
            sc=score(s,e,g)
            if sc>best[0] or (sc==best[0] and abs((s-g)-d)<abs((s-best[1])-d)): best=(sc,g)
    if best[0]>=max(n*0.8,min(n,3)) and n>=1:
        res.append((s,e-s,best[1],best[0],n)); 
        if n>=8: d=s-best[1]
    else: miss+=1; res.append((s,e-s,None,best[0],n))
# drop non-injective / suspicious: tiny function whose delta differs from both matched neighbours
m=[]
for i,r in enumerate(res):
    if r[2] is None: continue
    d=r[0]-r[2]
    if r[4]<8:
        nb=[]
        for step in (-1,1):
            j=i+step
            while 0<=j<len(res) and abs(j-i)<40:
                if res[j][2] is not None and res[j][4]>=8: nb.append(res[j][0]-res[j][2]); break
                j+=step
        if d not in nb: continue
    m.append(r)
rg=[]
for s,l,g,eq,n in m:
    if rg and rg[-1][1]+rg[-1][2]==s and rg[-1][0]+rg[-1][2]==g: rg[-1][2]+=l
    else: rg.append([g,s,l])
# GU side must not overlap
rg.sort(); ok=[]; 
for r in rg:
    if ok and r[0]<ok[-1][0]+ok[-1][2]:
        if r[2]>ok[-1][2]: ok[-1]=r
        continue
    ok.append(r)
for a, b, l in ok: print('{0x%x,0x%x,0x%x},' % (a, b, l))
print(len(ok), 'ranges', sum(r[2] for r in ok), 'bytes', file=sys.stderr)
