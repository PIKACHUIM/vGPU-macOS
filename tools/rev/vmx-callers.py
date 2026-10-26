import struct,sys,re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
IB=0x140000000
data=open(r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe","rb").read()
pe=struct.unpack_from("<I",data,0x3C)[0]
nsec=struct.unpack_from("<H",data,pe+6)[0]
optsz=struct.unpack_from("<H",data,pe+20)[0]
so=pe+24+optsz
secs={}
for i in range(nsec):
    o=so+40*i
    nm=data[o:o+8].rstrip(b"\0").decode("latin1")
    vs,va,rs,raw=struct.unpack_from("<IIII",data,o+8)
    secs[nm]=(va,vs,raw,rs)
def v2r(va):
    rva=va-IB
    for nm,(sva,vs,raw,rs) in secs.items():
        if sva<=rva<sva+max(vs,rs):
            o=raw+(rva-sva)
            if o<raw+rs: return o
    return None
pv,ps,pr,psz=secs[".pdata"]
funcs=[]
for i in range(psz//12):
    b,e,u=struct.unpack_from("<III",data,pr+i*12)
    if b and e>b: funcs.append((IB+b,IB+e))
funcs.sort()
targets=[int(x,16) for x in sys.argv[1:]]
md=Cs(CS_ARCH_X86,CS_MODE_64)
hits={t:[] for t in targets}
for b,e in funcs:
    ro=v2r(b)
    if ro is None: continue
    for ins in md.disasm(data[ro:ro+(e-b)], b):
        if ins.mnemonic not in ("call","jmp"): continue
        try: tgt=int(ins.op_str,16)
        except ValueError: continue
        if tgt in hits:
            hits[tgt].append((ins.address,ins.mnemonic))
for t in targets:
    print(f"=== callers of {t:#x}: {len(hits[t])}")
    for a,m in sorted(hits[t]): print(f"   {m} at {a:#x}")
