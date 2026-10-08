import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000
data = open(PATH, "rb").read()
pe_off = struct.unpack_from("<I", data, 0x3C)[0]
fh = pe_off + 4
nsec = struct.unpack_from("<H", data, fh+2)[0]
opt_size = struct.unpack_from("<H", data, fh+16)[0]
sec_off = pe_off + 24 + opt_size
secs = []
for i in range(nsec):
    o = sec_off + 40*i
    nm = data[o:o+8].rstrip(b"\0").decode("latin1")
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o+8)
    secs.append((nm, va, vsize, raw, rsize))
def va2raw(va):
    rva = va - IMAGE_BASE
    for nm, sva, vsize, raw, rsize in secs:
        if sva <= rva < sva + max(vsize, rsize):
            off = raw + (rva - sva)
            if off < raw + rsize:
                return off
    return None
md = Cs(CS_ARCH_X86, CS_MODE_64)
va = int(sys.argv[1], 16)
length = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x200
off = va2raw(va)
print(f"VA {va:#x} -> raw {off:#x}")
for ins in md.disasm(data[off:off+length], va):
    print(f"{ins.address:012x}  {ins.mnemonic:8s} {ins.op_str}")
