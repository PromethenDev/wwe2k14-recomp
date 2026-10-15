# For each candidate X (verified.txt + existing ptr_ entries in config/functions.toml), trace control flow from the
# preceding translated function start S (direct branches, fallthrough after conditional branches and calls) up to
# the next translated start. If X is reachable, it is an interior label of S -> reject.
import struct,bisect,os,re,sys
A=os.environ.get('ANALYSIS_DIR','analysis')
d=open(A+'/wwe2k14-tu-image.bin','rb').read(); B=0x82000000
funcs=sorted(set(int(x,16) for x in open(A+'/funcs.txt')))
def w(a): return struct.unpack('>I',d[a-B:a-B+4])[0]
def br(x,a):
    op=x>>26
    if op==18:
        li=x&0x03FFFFFC; li-=0x04000000 if li&0x02000000 else 0; return ('b',(li if x&2 else a+li),bool(x&1))
    if op==16:
        bd=x&0xFFFC; bd-=0x10000 if bd&0x8000 else 0; bo=(x>>21)&31; return ('bc',(bd if x&2 else a+bd),bool(x&1),bo&0x14==0x14)
    if op==19 and ((x>>1)&0x3FF) in (16,528): bo=(x>>21)&31; return ('bclr' if ((x>>1)&0x3FF)==16 else 'bcctr',None,bool(x&1),bo&0x14==0x14)
    return None
def reach(S,end):
    seen=set(); work=[S]
    while work:
        a=work.pop()
        while S<=a<end and a not in seen:
            seen.add(a); x=w(a); r=br(x,a)
            if r is None: a+=4; continue
            kind=r[0]
            if kind=='b':
                if r[2]: a+=4; continue            # bl: call, fall through
                t=r[1]
                if S<=t<end: a=t; continue
                break                               # tail call out
            if kind=='bc':
                if r[2]: a+=4; continue
                if S<=r[1]<end: work.append(r[1])
                if r[3]: break                      # branch always
                a+=4; continue
            # bclr / bcctr
            if r[2] or not r[3]: a+=4; continue    # conditional return or link -> fall through
            break
    return seen
cands=[(int(l.split()[0],16),l.split()[1]) for l in open(A+'/verified.txt')]
toml=sys.argv[1] if len(sys.argv)>1 else None
prior=[]
if toml:
    for m in re.finditer(r'^0x([0-9A-F]+) = \{ name = "wwe2k14_ptr_[0-9A-F]+", size = 0x([0-9A-F]+)',open(toml).read(),re.M):
        prior.append((int(m.group(1),16),m.group(2)))
fs=[f for f in funcs]
bad=[]; good=[]
for X,s in cands+[(p,s) for p,s in prior]:
    i=bisect.bisect_left(fs,X)-1
    while i>=0 and fs[i] in {p for p,_ in prior}: i-=1      # skip earlier ptr_ entries when finding S
    S=fs[i]; j=bisect.bisect_right(fs,X); end=fs[j] if j<len(fs) else X+0x10000
    if X in reach(S,end): bad.append((X,S))
    else: good.append((X,s))
print('checked',len(cands)+len(prior),'interior (reject)',len(bad))
for X,S in bad[:30]: print(f'  interior {X:08X} of {S:08X}')
open(A+'/verified_reach.txt','w').write(''.join(f'{X:08X} {s}\n' for X,s in good if (X,s) in cands))
open(A+'/prior_interior.txt','w').write(''.join(f'{X:08X}\n' for X,S in bad if any(X==p for p,_ in prior)))
