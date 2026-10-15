import struct, bisect, capstone as c
IMG='analysis/wwe2k14-tu-image.bin'; B=0x82000000
d=open(IMG,'rb').read()
md=c.Cs(c.CS_ARCH_PPC, c.CS_MODE_32|c.CS_MODE_BIG_ENDIAN)
pd=0x821D5200; n=468192//8
starts=sorted(set(struct.unpack('>I',d[pd-B+8*i:pd-B+8*i+4])[0] for i in range(n)))
starts=[s for s in starts if s]
print('pdata starts',len(starts))
def w(a): return struct.unpack('>I',d[a-B:a-B+4])[0]
def dis(a):
    x=list(md.disasm(d[a-B:a-B+4],a)); return f'{x[0].mnemonic} {x[0].op_str}' if x else '??'
def pinfo(a):
    i=bisect.bisect_right(starts,a)-1; return starts[i] if i>=0 else None, starts[i+1] if i+1<len(starts) else None
for site,tgt in [(0x82EED6EC,0x82EE50B0),(0x82694E34,0x8276A510),(0x82F592EC,0x82F592BC),(0x82F57ED4,0x82F57EA0)]:
    print(f'\n=== site {site:08X} -> target {tgt:08X}')
    for lab,a in (('site',site),('target',tgt)):
        ps,nx=pinfo(a); print(f'  {lab}: enclosing PDATA start {ps:08X}, next {nx:08X}')
    lo=min(site,tgt); hi=max(site,tgt)
    ps,_=pinfo(lo); _,nx=pinfo(hi)
    if hi-lo<0x200:
        rng=range(max(ps,lo-0x40) if ps else lo-0x40, min(nx,hi+0x40)+4,4)
    else:
        rng=list(range(tgt-0x30,tgt+0x34,4))+[None]+list(range(site-0x14,site+0x18,4))
    for a in rng:
        if a is None: print('   ...'); continue
        mark=' <-PDATA' if a in starts else ''
        mark+=' <-SITE' if a==site else ''; mark+=' <-TARGET' if a==tgt else ''
        print(f'   {a:08X}: {w(a):08X}  {dis(a)}{mark}')
