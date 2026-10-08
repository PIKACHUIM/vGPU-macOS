#!/usr/bin/env python3
"""Disassemble vm3dmp.sys regions around given VAs, and locate MMIO
register accesses (offsets = reg_index * 4) for registers of interest."""
import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PATH = r"D:/Codes/vGPU-macOS/driver/vm3dmp.sys"
IMAGE_BASE = 0x140000000

def load_sections():
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
    return data, secs

def va2raw(secs, va):
    rva = va - IMAGE_BASE
    for name, sva, vsize, raw, rsize in secs:
        if sva <= rva < sva + max(vsize, rsize):
            off = raw + (rva - sva)
            if off < raw + rsize:
                return off
    return None

def disasm(data, secs, va, length=0x300):
    off = va2raw(secs, va)
    if off is None:
        print(f"  [!] VA {va:x} not mapped"); return
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    code = data[off:off+length]
    for ins in md.disasm(code, va):
        print(f"  {ins.address:012x}  {ins.mnemonic:8s} {ins.op_str}")

if __name__ == "__main__":
    data, secs = load_sections()
    print("sections:", [(n, hex(va), hex(vsz)) for n, va, vsz, r, s in secs])
    targets = [int(x, 16) for x in sys.argv[1:]]
    for t in targets:
        print(f"\n===== around VA 0x{t:x} =====")
        disasm(data, secs, t, 0x280)
