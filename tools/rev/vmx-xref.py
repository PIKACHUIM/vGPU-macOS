import struct, sys, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IB=0x140000000
data=open(PATH,"rb").read()
pe=struct.unpack_from("<I",data,0x3C)[0]
nsec=struct.unpack_from("<H",data,pe+6)[0]
optsz=struct.unpack_from("<H",data,pe+20)[0]
so=pe+24+optsz
secs={}
order=[]
for i in range(nsec):
    o=so+40*i
    nm=data[o:o+8].rstrip(b"\0").decode("latin1")
    vs,va,rs,raw=struct.unpack_from("<IIII",data,o+8)
    secs[nm]=(va,vs,raw,rs); order.append(nm)
def v2r(va):
    rva=va-IB
    for nm,(sva,vs,raw,rs) in secs.items():
        if sva<=rva<sva+max(vs,rs):
            o=raw+(rva-sva)
            if o<raw+rs: return o
    return None
print("sections:", {k:(hex(v[0]),hex(v[3])) for k,v in secs.items()})
targets=[int(x,16) for x in sys.argv[1:]]
res={t:[] for t in targets}
md=Cs(CS_ARCH_X86,CS_MODE_64)
rx=re.compile(r"\[rip ([+-]) (0x[0-9a-f]+)\]")
# read .pdata
pv,ps,pr,psz = secs[".pdata"]
pdata=data[pr:pr+psz]
n=len(pdata)//12
print("pdata entries:", n)
for i in range(n):
    b,e,u=struct.unpack_from("<III",pdata,i*12)
    if b==0: continue
    ro=v2r(IB+b); 
    if ro is None: continue
    ln=e-b
    if ln<=0 or ln>0x40000: continue
    for ins in md.disasm(data[ro:ro+ln], IB+b):
        m=rx.search(ins.op_str)
        if not m: continue
        d=int(m.group(2),16)
        if m.group(1)=='-': d=-d
        tgt=ins.address+ins.size+d
        if tgt in res: res[tgt].append(f"{ins.address:#x}  {ins.mnemonic:8s} {ins.op_str}")
for t in targets:
    print(f"=== {t:#x}: {len(res[t])} refs")
    for s in sorted(res[t]): print("  ",s)
