"""Ask the binary, not the notes: can the DX-manager creation chain ever run?

The chain (§22.2 / §23.1) is

    0x1404c5ee0  tick callback   --[dev+0xb4] != 0-->  0x1404c7950
       --[dev+0x81d]!=0, counters, [dev+0x9bc]!=0, [dev+0x71]==0--> 0x140509f20
       --> writes the one and only DX manager pointer [0x1414930A0]

vmx-callers.py reported "0 callers" for 0x1404c5ee0, which only means nobody
`call`s it -- a timer/thread callback is registered by *address*, so it shows up
as a `lea reg,[rip+x]` or as a data pointer, not as a call. This script looks for
that, and then hunts the writers of the three gate fields so we know whether the
gate is ever open. Without that we would be patching blind.

Three questions, three sections (A/B/C) -- keep them separate so a failure in one
does not cost the others.
"""
import struct
import re
import sys

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

DEV_GLOBAL = 0x1413F31C0          # SVGA device object; NOT 0x1414931C0 (0 refs, see §48)
TICK = 0x1404c5ee0
YIELD = 0x1404c7950
CREATE = 0x140509f20
GATE_FIELDS = (0xB4, 0x71, 0x81D)

argv = sys.argv[1:]


def resolve_mem(ins):
    """Absolute VA of a RIP-relative memory operand, else None."""
    for op in ins.operands:
        if op.type != X86_OP_MEM:
            continue
        if op.mem.base != X86_REG_RIP:   # capstone uses X86_REG_RIP, not 0
            continue
        return ins.address + ins.size + op.mem.disp
    return None


def section_a():
    """Who holds the tick's address? A callback registration, not a call."""
    print("=" * 20, "A: address-taken refs to tick/yield/create", "=" * 20)
    want = {TICK: "TICK", YIELD: "YIELD", CREATE: "CREATE"}
    hits = {k: [] for k in want}
    for b, e in funcs:
        ro = v2r(b)
        if ro is None or e - b > 0x40000:
            continue
        for ins in md.disasm(data[ro:ro + (e - b)], b):
            tgt = None
            if ins.mnemonic in ("lea", "mov"):
                t = resolve_mem(ins)
                if t in want:
                    tgt = t
            if tgt is None:
                for op in ins.operands:
                    if op.type == X86_OP_IMM and op.imm in want:
                        tgt = op.imm
            if tgt is not None:
                hits[tgt].append((ins.address, f"{ins.mnemonic} {ins.op_str}"))
    for t, name in want.items():
        print(f"--- {name} {t:#x}: {len(hits[t])} address-taken ref(s)")
        for a, s in sorted(hits[t]):
            print(f"    {a:#x}  {s}")
    print()


def section_b():
    """The gate itself, plus enough of the tick prologue to see [dev+0xb4]."""
    print("=" * 20, "B: gate prologue", "=" * 20)
    for lo, hi, label in ((TICK, TICK + 0x120, "tick 0x1404c5ee0 (first 0x120)"),
                          (YIELD, YIELD + 0x140, "yield 0x1404c7950 (full-ish)")):
        ro = v2r(lo)
        print(f"--- {label}")
        n = 0
        for ins in md.disasm(data[ro:ro + (hi - lo)], lo):
            print(f"    {ins.address:#x}  {ins.mnemonic:8s} {ins.op_str}")
            n += 1
        print(f"    ({n} instructions)")
    print()


def section_c():
    """Writers/readers of the three gate fields, with dev tracked per function."""
    print("=" * 20, "C: gate-field accessors", "=" * 20)
    found = {f: {"W": [], "R": []} for f in GATE_FIELDS}
    for b, e in funcs:
        ro = v2r(b)
        if ro is None or e - b > 0x40000:
            continue
        devregs = set()
        for ins in md.disasm(data[ro:ro + (e - b)], b):
            # dev = [DEV_GLOBAL]
            if ins.mnemonic == "mov" and len(ins.operands) == 2:
                dst, src = ins.operands
                if dst.type == X86_OP_REG and src.type == X86_OP_MEM:
                    if resolve_mem(ins) == DEV_GLOBAL:
                        devregs.add(ins.reg_name(dst.reg))
                        continue
            for op in ins.operands:
                if op.type != X86_OP_MEM:
                    continue
                if op.mem.disp not in GATE_FIELDS:
                    continue
                base = ins.reg_name(op.mem.base)
                if base not in devregs:
                    continue
                # is this operand the destination?
                is_dst = len(ins.operands) >= 1 and ins.operands[0].type == X86_OP_MEM \
                    and ins.operands[0].mem.base == op.mem.base \
                    and ins.operands[0].mem.disp == op.mem.disp
                kind = "W" if is_dst else "R"
                found[op.mem.disp][kind].append(
                    (ins.address, b, f"{ins.mnemonic} {ins.op_str}"))
    for f in GATE_FIELDS:
        for kind in ("W", "R"):
            lst = found[f][kind]
            tag = "writes" if kind == "W" else "reads"
            print(f"--- [dev+{f:#x}] {tag}: {len(lst)}")
            for a, fnb, s in sorted(lst):
                print(f"    {a:#x} (fn {fnb:#x})  {s}")
    print()


section_a()
section_b()
section_c()
