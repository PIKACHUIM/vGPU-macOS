"""Every reference to a global, classified as write or read, with its function.

Complements objfields.py: that one only sees fields reached through a register that
was loaded from the global *in the same function*, so it loses writes that go through
a parameter or through [global+8] (a neighbouring pointer, e.g. 0x1413F31C8 next to
0x1413F31C0). This one does not care how the address got into the register.

Usage: python globalrefs.py 140f4363a 1405098c0
"""
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

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


pv, ps, pr, psz = secs[".pdata"]
funcs = []
for i in range(psz // 12):
    b, e, u = struct.unpack_from("<III", data, pr + i * 12)
    if b and e > b:
        funcs.append((IB + b, IB + e))
funcs.sort()

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

WRITE_MNEM = {"mov", "movzx", "movsx", "movups", "movdqu", "movaps", "movdqa", "movd",
              "and", "or", "xor", "add", "sub", "inc", "dec", "bts", "btr", "btc",
              "xchg", "cmpxchg", "shl", "shr", "sar", "not", "neg", "sete", "setne"}

watch = [int(x, 16) for x in sys.argv[1:]]
for g in watch:
    print("=" * 70)
    print(f"GLOBAL {g:#x}")
    n = 0
    for b, e in funcs:
        ro = v2r(b)
        if ro is None:
            continue
        for ins in md.disasm(data[ro:ro + (e - b)], b):
            hit = False
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    if ins.address + ins.size + op.mem.disp == g:
                        hit = True
            if not hit:
                continue
            dst_is_mem = (ins.operands[0].type == X86_OP_MEM
                          and ins.operands[0].mem.base == X86_REG_RIP)
            kind = "W" if (dst_is_mem and ins.mnemonic in WRITE_MNEM) else "R"
            print(f"  {kind}  {ins.address:#x} (fn {b:#x})  {ins.mnemonic:8s} {ins.op_str}")
            n += 1
    print(f"  total {n}")
