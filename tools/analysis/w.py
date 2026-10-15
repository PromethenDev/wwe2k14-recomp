import struct,sys,capstone as c
d=open('analysis/wwe2k14-tu-image.bin','rb').read(); B=0x82000000
md=c.Cs(c.CS_ARCH_PPC, c.CS_MODE_32|c.CS_MODE_BIG_ENDIAN)
a,b=int(sys.argv[1],16),int(sys.argv[2],16)
for x in range(a,b,4):
    w=d[x-B:x-B+4]; i=list(md.disasm(w,x)); print(f'{x:08X}: {w.hex().upper()}  {(i[0].mnemonic+" "+i[0].op_str) if i else "??"}')
