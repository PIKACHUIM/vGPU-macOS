"""Resync-capable rip-relative reference scanner.

Usage:
    python vmx-refs2.py 0x140a3d400 0x140a3d800

vmx-refs-in.py used a straight linear sweep, which aborts a whole function at
the first undecodable byte and therefore reports far too few hits.  This one
decodes one instruction at a time and advances a single byte on failure.
"""
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

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

lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)
md = Cs(CS_ARCH_X86, CS_MODE_64)
nref = 0
for b, e in funcs:
    ro = v2r(b)
    if ro is None:
        continue
    end = min(e, b + 0x40000)
    va = b
    off = ro
    while va < end:
        got = False
        for ins in md.disasm(data[off:off + 16], va):
            got = True
            s = ins.op_str
            if "rip " in s:
                try:
                    i = s.index("[rip ")
                    j = s.index("]", i)
                    expr = s[i + 5:j].replace(" ", "")
                    sign = 1
                    if expr.startswith("-"):
                        sign = -1
                        expr = expr[1:]
                    elif expr.startswith("+"):
                        expr = expr[1:]
                    d = sign * int(expr, 16)
                    tgt = va + ins.size + d
                    if lo <= tgt < hi:
                        print(f"  {va:#x}  fn {b:#x}  {ins.mnemonic:8s} {s}   -> {tgt:#x}")
                        nref += 1
                except (ValueError, IndexError):
                    pass
            off += ins.size
            va += ins.size
            break
        if not got:
            off += 1
            va += 1
print(f"--- {nref} refs into [{lo:#x}, {hi:#x})")
