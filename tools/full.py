import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
PATH = r"D:/Codes/vGPU-macOS/driver/vm3dmp.sys"
IMAGE_BASE = 0x140000000
data = open(PATH, "rb").read()
pe_off = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe_off + 6)[0]
opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
sec_off = pe_off + 24 + opt_size
secs = []
for i in range(nsec):
    o = sec_off + 40 * i
    name = data[o:o+8].rstrip(b"\0").decode(errors="replace")
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((name, va, vsize, raw, rsize))
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
out = open(r"C:/Users/Pika/WorkBuddy/2026-09-27-20-30-43/disasm/vm3dmp.text.asm", "w", encoding="utf-8")
# .text
name, va, vsize, raw, rsize = secs[0]
code = data[raw:raw+rsize]
for ins in md.disasm(code, IMAGE_BASE + va):
    out.write(f"{ins.address:012x}  {ins.mnemonic:8s} {ins.op_str}\n")
out.close()
print("done")
