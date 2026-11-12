"""Resolve every RIP-relative operand in one address window, and count refs to globals.

Needed because the notes may hold stale global addresses. §22.2 recorded
`dev = [0x1414931C0]`, but that address has zero references in this very binary
while 0x1413F31C0 has 891 -- so the notes and the binary disagree, and the binary
wins. The only way to settle which global holds the DX manager is to disassemble
the single write site and resolve its displacement:

    0x14050a077   mov qword ptr [rip + X], rax      ->  the DX manager lives at ...

Usage:  python ripscan.py <from_va> <to_va> [global_hex ...]
"""
import struct
import sys
from collections import Counter

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_OP_IMM, X86_REG_RIP

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

lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)
watch = [int(x, 16) for x in sys.argv[3:]]

ro = v2r(lo)
print(f"--- disasm {lo:#x}..{hi:#x}  (rip targets resolved)")
for ins in md.disasm(data[ro:ro + (hi - lo)], lo):
    extra = ""
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            t = ins.address + ins.size + op.mem.disp
            extra += f"   ; -> {t:#x}"
        elif op.type == X86_OP_IMM and op.imm > 0x140000000:
            extra += f"   ; imm {op.imm:#x}"
    print(f"  {ins.address:#x}  {ins.mnemonic:8s} {ins.op_str}{extra}")

if watch:
    cnt = Counter()
    for b, e in funcs:
        r = v2r(b)
        if r is None or e - b > 0x40000:
            continue
        for ins in md.disasm(data[r:r + (e - b)], b):
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    t = ins.address + ins.size + op.mem.disp
                    if t in watch:
                        cnt[t] += 1
    print("--- rip-relative ref counts")
    for t in watch:
        print(f"    {t:#x}: {cnt[t]}")
