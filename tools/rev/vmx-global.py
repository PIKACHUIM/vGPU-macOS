"""Classify every rip-relative reference to a global in vmware-vmx.exe.

Usage:
    python vmx-global.py 0x1414930A0

For each reference we report the instruction, whether the global is the
destination (WRITE) or a source (READ / CMP), and the containing .pdata
function start, so the store site and its callers become obvious.
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


def containing(va):
    best = None
    for b, e in funcs:
        if b <= va < e:
            return (b, e)
    return None


G = int(sys.argv[1], 16)
md = Cs(CS_ARCH_X86, CS_MODE_64)
hits = []
for b, e in funcs:
    ro = v2r(b)
    if ro is None:
        continue
    ln = e - b
    if ln <= 0 or ln > 0x40000:
        continue
    for ins in md.disasm(data[ro:ro + ln], b):
        s = ins.op_str
        if "rip " not in s:
            continue
        # [rip + 0xNNN]
        try:
            i = s.index("[rip ")
            j = s.index("]", i)
            expr = s[i + 5:j]
        except ValueError:
            continue
        expr = expr.replace(" ", "")
        sign = 1
        if expr.startswith("-"):
            sign = -1
            expr = expr[1:]
        elif expr.startswith("+"):
            expr = expr[1:]
        try:
            d = sign * int(expr, 16)
        except ValueError:
            continue
        tgt = ins.address + ins.size + d
        if tgt != G:
            continue
        # destination detection: "mov/lea/or/and/add/sub/xor qword ptr [rip ...], reg"
        kind = "READ"
        if s.startswith("qword ptr [rip") or s.startswith("dword ptr [rip") or \
           s.startswith("byte ptr [rip") or s.startswith("xmmword ptr [rip"):
            kind = "WRITE"
        elif s.startswith("cmp qword ptr [rip") or s.startswith("cmp dword ptr [rip") or \
                s.startswith("test "):
            kind = "CMP"
        hits.append((ins.address, kind, ins.mnemonic, s))

writes = [h for h in hits if h[1] == "WRITE"]
print(f"=== {G:#x}: {len(hits)} refs ({len(writes)} writes)")
for a, kind, mn, s in hits:
    f = containing(a)
    fs = f"{f[0]:#x}" if f else "?"
    print(f"  {kind:5s} {a:#x}  fn {fs:>12s}  {mn:8s} {s}")
