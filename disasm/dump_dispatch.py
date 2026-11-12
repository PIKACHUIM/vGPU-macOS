"""dump 已定位的 DX 命令分派表（0x140bde74c 附近）。

表项形态待定，两种候选：
  A) 交错对 { id, handlerVA }  —— 每 8 字节一项
  B) 两个平行数组 —— 前半是 id，后半是 handler

先按 8 字节步长把每对打出来，人工看 handler 是否都落在 .text 内。
"""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000
data = open(PATH, "rb").read()

pe_off = struct.unpack_from("<I", data, 0x3C)[0]
fh = pe_off + 4
nsec = struct.unpack_from("<H", data, fh + 2)[0]
opt_size = struct.unpack_from("<H", data, fh + 16)[0]
sec_off = pe_off + 24 + opt_size
secs = []
for i in range(nsec):
    o = sec_off + 40 * i
    nm = data[o:o + 8].rstrip(b"\0").decode("latin1")
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((nm, va, vsize, raw, rsize))


def va2raw(va):
    rva = va - IMAGE_BASE
    for nm, sva, vsize, raw, rsize in secs:
        if sva <= rva < sva + max(vsize, rsize):
            off = raw + (rva - sva)
            if off < raw + rsize:
                return off
    return None


def in_text(v):
    return 0x140001000 <= v < 0x1403f0000 or 0x140400000 <= v < 0x140a36000


md = Cs(CS_ARCH_X86, CS_MODE_64)
START = 0x140bde74c
LEN = 0x200
off = va2raw(START)
print("dump %#x .. %#x" % (START, START + LEN))
for i in range(0, LEN, 8):
    a = struct.unpack_from("<I", data, off + i)[0]
    b = struct.unpack_from("<Q", data, off + i + 0)[0] >> 32
    lo = struct.unpack_from("<I", data, off + i + 4)[0]
    print("  +%#04x  a=%#010x  b=%#010x %s" % (
        i, a, lo, "(handler?)" if in_text(lo) else ""))
