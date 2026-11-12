import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000
data = open(PATH,"rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]; fh = pe+4
nsec = struct.unpack_from("<H", data, fh+2)[0]; opt = struct.unpack_from("<H", data, fh+16)[0]
so = pe+24+opt
secs=[]
for i in range(nsec):
    o=so+40*i
    nm=data[o:o+8].rstrip(b"\0").decode("latin1")
    vsize,va,rsize,raw=struct.unpack_from("<IIII",data,o+8)
    secs.append((nm,va,vsize,raw,rsize))
def va2raw(v):
    r=v-IMAGE_BASE
    for nm,sva,vsize,raw,rsize in secs:
        if sva<=r<sva+max(vsize,rsize):
            off=raw+(r-sva)
            if off<raw+rsize: return off
    return None
md=Cs(CS_ARCH_X86,CS_MODE_64)
va=int(sys.argv[1],16); ln=int(sys.argv[2],16)
off=va2raw(va)
for ins in md.disasm(data[off:off+ln], va):
    print("%#011x  %-8s %s"%(ins.address, ins.mnemonic, ins.op_str))
