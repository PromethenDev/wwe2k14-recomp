import struct,bisect,capstone as c
P=__import__('os').environ.get('ANALYSIS_DIR','analysis')
d=open('analysis/wwe2k14-tu-image.bin','rb').read(); B=0x82000000
md=c.Cs(c.CS_ARCH_PPC, c.CS_MODE_32|c.CS_MODE_BIG_ENDIAN)
funcs=sorted(set(int(x,16) for x in open(P+'/funcs.txt')))
cands=[int(l.split()[0],16) for l in open(P+'/ptr_candidates.txt')]
starts=sorted(set(funcs)|set(cands)); sset=set(starts)
def w(a): return struct.unpack('>I',d[a-B:a-B+4])[0]
def rel(x,a):
    op=x>>26
    if op==18:
        li=x&0x03FFFFFC; li=li-0x04000000 if li&0x02000000 else li
        return (li if x&2 else a+li), bool(x&1), True
    if op==16:
        bd=x&0xFFFC; bd=bd-0x10000 if bd&0x8000 else bd
        return (bd if x&2 else a+bd), bool(x&1), False
    return None
TERM={0x4E800020,0x4E800420}
ok=[];bad=[]
for v in cands:
    i=bisect.bisect_right(starts,v); e=starts[i] if i<len(starts) else v+4
    # trim trailing padding
    last=e-4
    while last>v and w(last)==0: last-=4
    reason=None
    lw=w(last); r=rel(lw,last)
    if not (lw in TERM or (r and r[2] and not r[1])): reason=f'no terminator at end {last:08X} ({lw:08X})'
    for a in range(v,last+4,4):
        x=w(a)
        if not list(md.disasm(struct.pack('>I',x),a)): reason=reason or f'undecodable {a:08X}={x:08X}'; break
        r=rel(x,a)
        if r and not r[1]:
            t=r[0]
            if not (v<=t<e or t in sset): reason=reason or f'branch {a:08X}->{t:08X} leaves range'
    (bad if reason else ok).append((v,e-v,reason))
print('verified',len(ok),'flagged',len(bad))
for v,s,r in bad: print(f'  FLAG {v:08X} size {s:X}: {r}')
open(P+'/verified.txt','w').write(''.join(f'{v:08X} {s:X}\n' for v,s,_ in ok))
print('sizes:',sorted(set(s for _,s,_ in ok))[:20],'... max',max(s for _,s,_ in ok))
