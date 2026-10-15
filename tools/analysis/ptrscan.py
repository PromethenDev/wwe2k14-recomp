# Scan data sections for pointers into code that are neither known function starts nor inside a PDATA range.
import struct,bisect,sys,collections
P=__import__('os').environ.get('ANALYSIS_DIR','analysis')
d=open('analysis/wwe2k14-tu-image.bin','rb').read(); B=0x82000000
secs=[]
for line in open('analysis/wwe2k14-tu-image.bin.sections.txt'):
    n,a,z,x=line.split(); secs.append((n,int(a,16),int(z,16),x=='1'))
for s in secs: print(f'  {s[0]:8} {s[1]:08X} size {s[2]:X} exec={s[3]}')
code=[(a,a+z) for n,a,z,x in secs if x]
def incode(v): return any(a<=v<b for a,b in code)
pd=0x821D5200; ent={}
for i in range(468192//8):
    s,v=struct.unpack('>II',d[pd-B+8*i:pd-B+8*i+8])
    if s: ent[s]=((v>>8)&0x3FFFFF)*4
pst=sorted(ent)
def inpdata(v):
    i=bisect.bisect_right(pst,v)-1; return i>=0 and pst[i]<=v<pst[i]+ent[pst[i]]
funcs=sorted(set(int(x,16) for x in open(P+'/funcs.txt')))
fset=set(funcs)
def w(a): return struct.unpack('>I',d[a-B:a-B+4])[0]
def terminator(x):
    op=x>>26
    if x==0: return True
    if op==18 and (x&1)==0: return True                      # b (no link)
    if x in (0x4E800020,0x4E800420): return True            # blr, bctr
    return False
refs=collections.defaultdict(list)
for n,a,z,x in secs:
    if x: continue
    for o in range(a,a+z-3,4):
        v=w(o)
        if v&3 or not incode(v) or v in fset: continue
        refs[v].append(o)
# Code-materialized addresses: lis rX,hi ; ... ; addi/ori rY,rX,lo within 16 instructions (rX not redefined).
for n,a,z,x in secs:
    if not x: continue
    for o in range(a,a+z-3,4):
        ins=w(o)
        if ins>>26!=15 or (ins>>16)&31!=0: continue
        rx=(ins>>21)&31; hi=ins&0xFFFF
        for k in range(1,17):
            q=o+4*k
            if q>=a+z: break
            y=w(q); op=y>>26
            if op in (14,24) and (y>>16)&31==rx:
                lo=y&0xFFFF
                v=((hi<<16)+(lo-0x10000 if (op==14 and lo&0x8000) else lo))&0xFFFFFFFF
                if not (v&3) and incode(v) and v not in fset: refs[v].append(q)
                if (y>>21)&31==rx: break                 # rX redefined by this addi/ori
                continue
            if op in (14,15,24,25,32,33) and (y>>21)&31==rx: break   # rX overwritten by addi/lis/ori/oris/lwz
# Pattern pass: standalone vtable forwarders  lwz r12,0(r3); lwz r11,N(r12); mtctr r11; bctr
for n,a,z,x in secs:
    if not x: continue
    for o in range(a+4,a+z-15,4):
        if w(o)==0x81830000 and (w(o+4)&0xFFFF0000)==0x816C0000 and w(o+8)==0x7D6903A6 and w(o+12)==0x4E800420 \
           and o not in fset:
            refs[o].append(o)
cand=[]; inside=0; notstart=0
for v,rs in sorted(refs.items()):
    if inpdata(v): inside+=1; continue
    if not terminator(w(v-4)): notstart+=1; continue
    i=bisect.bisect_right(funcs,v); nxt=funcs[i] if i<len(funcs) else None
    cand.append((v,len(rs),nxt))
print(f'unknown code pointers: {len(refs)}  inside PDATA ranges: {inside}  not after a terminator: {notstart}  candidates: {len(cand)}')
with open(P+'/ptr_candidates.txt','w') as f:
    for v,n,nx in cand: f.write(f'{v:08X} refs={n} next_known={nx:08X}\n')
for v,n,nx in cand[:40]: print(f'  {v:08X} refs={n} gap_to_next=0x{nx-v:X}')
print('0x82D2B818 candidate:', any(c[0]==0x82D2B818 for c in cand))
