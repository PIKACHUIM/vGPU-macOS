"""Dump a VA range with RIP-relative targets resolved. Usage: python dump.py <lo> <hi>"""
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IB = 0x140000000
data = open(PATH, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
so = pe + 24 + optsz
secs = {}
for i in range(nsec):
    o = so + 40 * i
    nm = data[o:o + 8].rstrip(b"\0").decode("latin1")
    vs, va, rs, raw = struct.unpack_from("<IIII", data, o + 8)
    secs[nm] = (va, vs, raw, rs)


def v2r(va):
    rva = va - IB
    for nm, (sva, vs, raw, rs) in secs.items():
        if sva <= rva < sva + max(vs, rs):
            o = raw + (rva - sva)
            if o < raw + rs:
                return o
    return None


def cstr(va):
    o = v2r(va)
    if o is None:
        return None
    e = data.find(b"\0", o)
    if e < 0 or e - o > 200:
        return None
    try:
        s = data[o:e].decode("latin1")
    except Exception:
        return None
    return s if s.isprintable() and s else None


lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
o = v2r(lo)
for ins in md.disasm(data[o:o + (hi - lo)], lo):
    extra = ""
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            t = ins.address + ins.size + op.mem.disp
            extra = f"   ; -> {t:#x}"
            s = cstr(t)
            if s:
                extra += f' "{s}"'
        elif op.type == X86_OP_IMM and op.imm > 0x140000000:
            extra += f"   ; imm {op.imm:#x}"
    print(f"  {ins.address:#x}  {ins.mnemonic:8s} {ins.op_str}{extra}")
