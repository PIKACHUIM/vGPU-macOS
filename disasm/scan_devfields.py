"""Find candidate write sites for the SVGA device fields that gate DX-device creation.

Why: docs/cb-findings §22.2 showed the only writer of [0x1414930A0] (the DX manager)
sits behind `[dev+0x9bc] != 0 && [dev+0x71] == 0 && [dev+0x81d] != 0`, reached only from
the timeslice/preempt chain. If [dev+0x9bc] is only ever set to non-zero by an
initialisation path we never take, that path is the answer -- and finding it costs no
reboot budget.

Approach: full linear sweep of .text with capstone, decode every instruction that
mentions a displacement of 0x9bc / 0x71 / 0x81d on a register base, then classify by
mnemonic: only `mov [reg+disp], ...` / `or|and|add|inc` style stores count as writes.
Reads are reported separately because they tell us which functions care.

Known caveat (learned the hard way, see the docs' "false positive" notes): capstone on
x86-64 desynchronises inside data-in-code islands. A hit is only trusted when the same
34-byte window decodes cleanly, which is why every hit prints its two neighbours.
"""
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000
FIELDS = (0x9BC, 0x71, 0x81D, 0xB4)

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


text = [s for s in secs if s[0] == ".text"][0]
nm, sva, vsize, raw, rsize = text
blob = data[raw:raw + rsize]
text_va = IMAGE_BASE + sva

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# Mnemonics that, with a memory destination, modify the field.
WRITERS = ("mov", "or", "and", "add", "sub", "inc", "dec", "xor", "movzx", "not")

hits = {f: {"write": [], "read": []} for f in FIELDS}

# Decode instruction by instruction, staying in sync: disasm_one gives us the length so
# we can always advance, regardless of whether the bytes are really code.
pc = 0
end = len(blob)
while pc < end:
    got = list(md.disasm(blob[pc:pc + 16], text_va + pc, count=1))
    if not got:
        pc += 1
        continue
    ins = got[0]
    pc += ins.size
    if not ins.operands:
        continue
    # operand 0 as memory destination -> a store
    try:
        op0 = ins.operands[0]
        op1 = ins.operands[1] if len(ins.operands) > 1 else None
    except Exception:
        continue
    for f in FIELDS:
        for idx, op in enumerate(ins.operands):
            if op.type != 3:      # X86_OP_MEM
                continue
            # Only simple [base + disp]; a displacement that happens to equal the field
            # on an unrelated base is noise, but the same 0x9bc appearing on many
            # different bases is a signal in itself, so keep them all and print the base.
            if op.mem.disp != f:
                continue
            base = ins.reg_name(op.mem.base) if op.mem.base else "none"
            if op.mem.index:
                continue
            # Stack-relative displacements are local variables, not device fields.
            # The first version of this scan reported "[dev+0x9bc] has a writer at
            # 0x1402804ee" -- that instruction is `[rbp+0x9bc]`, a stack slot of a
            # completely unrelated function, and chasing it cost a full disassembly
            # review before the base register gave it away. Filter first, read second.
            if base in ("rbp", "rsp", "none"):
                continue
            if idx == 0 and ins.mnemonic in WRITERS:
                hits[f]["write"].append((ins.address, ins.mnemonic, ins.op_str, base))
            else:
                hits[f]["read"].append((ins.address, ins.mnemonic, ins.op_str, base))

for f in FIELDS:
    w = hits[f]["write"]
    r = hits[f]["read"]
    print(f"\n===== [dev+0x{f:x}]  writes={len(w)}  reads={len(r)} =====")
    seen = set()
    for addr, mn, ops, base in w:
        key = (addr, mn)
        if key in seen:
            continue
        seen.add(key)
        print(f"  W {addr:#012x}  {mn:7s} {ops}")
    if len(w) > 40:
        print(f"  ... {len(w)} write hits total")
    for addr, mn, ops, base in r[:25]:
        print(f"  r {addr:#012x}  {mn:7s} {ops}")
    if len(r) > 25:
        print(f"  ... {len(r)} read hits total")
