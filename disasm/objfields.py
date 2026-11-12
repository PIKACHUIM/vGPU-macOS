"""Per-global object field map: what does [0x1414931C0] and [0x1413F31C0] look like?

Why this exists: §22.2 recorded `dev = [0x1414931C0]`, and §27 hung a whole
conclusion on that object's +0x9bc. But the yield gate that decides whether the DX
manager is created reads the *same three offsets* through a different global,
`[0x1413F31C0]` (0x1404c796f / 0x1404c7a0d / 0x1404c7a1a). Either those two globals
point at the same object at runtime, or §27 analysed the wrong object. Nothing in
the notes settles it, so measure instead of assuming:

  * how many places load each global,
  * which displacements each global's object is accessed at (write vs read),
  * whether one global is ever stored into the other (alias detection).

Usage:  python objfields.py 1414931C0 1413F31C0
"""
import struct
import sys
from collections import defaultdict

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

GLOBALS = [int(x, 16) for x in sys.argv[1:]] or [0x1414931C0, 0x1413F31C0]


def rip_target(ins):
    # capstone reports RIP-relative operands with base == X86_REG_RIP, NOT base == 0.
    # Getting that wrong makes the whole scan silently return nothing; it already cost
    # one round on 2026-11-13 (both globals reported "0 load site(s)").
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            return ins.address + ins.size + op.mem.disp
    return None


loads = defaultdict(list)        # global -> [(addr, fn, text)]
fields = defaultdict(lambda: defaultdict(lambda: {"W": [], "R": []}))
stores = []                      # any write whose value came from a load of a watched global

for b, e in funcs:
    ro = v2r(b)
    if ro is None or e - b > 0x40000:
        continue
    tracked = {}                 # reg name -> global VA it holds
    for ins in md.disasm(data[ro:ro + (e - b)], b):
        t = rip_target(ins)
        if t in GLOBALS and ins.mnemonic in ("mov", "lea"):
            dst = ins.operands[0]
            if dst.type == X86_OP_REG:
                tracked[ins.reg_name(dst.reg)] = t
                loads[t].append((ins.address, b, f"{ins.mnemonic} {ins.op_str}"))
                continue
            # store *into* the global: alias detection
            if ins.mnemonic == "mov" and len(ins.operands) == 2:
                src = ins.operands[1]
                if src.type == X86_OP_REG:
                    who = tracked.get(ins.reg_name(src.reg))
                    stores.append((ins.address, b, t, who, f"{ins.mnemonic} {ins.op_str}"))
                elif src.type == X86_OP_IMM:
                    stores.append((ins.address, b, t, None,
                                   f"{ins.mnemonic} {ins.op_str} (imm {src.imm:#x})"))
                continue

        for idx, op in enumerate(ins.operands):
            if op.type != X86_OP_MEM or op.mem.base == 0:
                continue
            base = ins.reg_name(op.mem.base)
            if base not in tracked:
                continue
            g = tracked[base]
            kind = "W" if idx == 0 and ins.mnemonic not in ("cmp", "test") else "R"
            fields[g][op.mem.disp][kind].append((ins.address, b, f"{ins.mnemonic} {ins.op_str}"))

for g in GLOBALS:
    print("=" * 70)
    print(f"GLOBAL {g:#x}: {len(loads[g])} load site(s)")
    for a, fn, s in sorted(loads[g])[:20]:
        print(f"    {a:#x} (fn {fn:#x})  {s}")
    if len(loads[g]) > 20:
        print(f"    ... {len(loads[g]) - 20} more")
    fl = fields[g]
    print(f"  fields touched: {len(fl)}")
    for disp in sorted(fl):
        w = fl[disp]["W"]
        r = fl[disp]["R"]
        wtxt = ",".join(f"{a:#x}" for a, _, _ in w[:4]) + (" ..." if len(w) > 4 else "")
        print(f"    +{disp:#06x}  W={len(w):<3} R={len(r):<3}  writes@ {wtxt}")

print("=" * 70)
print(f"stores into a watched global: {len(stores)}")
for a, fn, t, who, s in sorted(stores):
    print(f"    {a:#x} (fn {fn:#x}) -> {t:#x}  from={'none' if who is None else hex(who)}  {s}")
