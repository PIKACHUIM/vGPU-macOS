"""Walk callers upward through a .text-wide E8/E9 scan, resolving thunks.

The pdata-restricted caller scanners (`tools/rev/vmx-callers.py`) miss two things
that matter here:

  1. calls emitted from code with no RUNTIME_FUNCTION entry (hand-written asm),
  2. CFG thunks -- 16-byte `jmp target` pads that exist so that an indirect call
     can be validated. A function that is *only* ever called indirectly shows zero
     pdata callers, which is exactly what 0x1404c5ee0 looked like until a raw
     scan found 0x1401565f0 jmp'ing to it, and 0x14014c278 calling the thunk.

So: raw scan, then fold each hit up to its containing pdata function, then repeat.
Prints one indented tree so the chain to a registration point is visible at a glance.

Usage:  python caller_chain.py 0x1404c5ee0 [depth]
"""
import struct
import sys

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

tva, tvs, traw, trs = secs[".text"]
TEXT_VA = IB + tva
BLOB = data[traw:traw + trs]

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
            best = (b, e)
        elif b > va:
            break
    return best


# Precompute every relative call/jmp edge once: target -> [sources]
EDGES = {}
for i in range(len(BLOB) - 5):
    if BLOB[i] not in (0xE8, 0xE9):
        continue
    rel = struct.unpack_from("<i", BLOB, i + 1)[0]
    src = TEXT_VA + i
    dst = src + 5 + rel
    if dst < IB or dst >= IB + 0x2000000:
        continue
    EDGES.setdefault(dst, []).append(src)


def is_thunk(va):
    """A CFG pad: `jmp rel32` followed by int3 up to the next 16-byte boundary."""
    o = va - IB - tva
    if o < 0 or o + 5 > len(BLOB):
        return None
    if BLOB[o] != 0xE9:
        return None
    rel = struct.unpack_from("<i", BLOB, o + 1)[0]
    return va + 5 + rel


def walk(va, depth, indent, seen):
    pad = "  " * indent
    if depth == 0:
        return
    th = is_thunk(va)
    if th is not None:
        print(f"{pad}[thunk] {va:#x} -> {th:#x}")
        # Walk BOTH sides: the thunk's own callers are the real callers of the target.
        for src in sorted(EDGES.get(va, [])):
            walk(src, depth - 1, indent + 1, seen)
        return walk(th, depth, indent, seen)
    fn = containing(va)
    label = f"fn {fn[0]:#x}..{fn[1]:#x}" if fn else "NO-PDATA"
    if va in seen:
        print(f"{pad}{va:#x} ({label})  [cycle]")
        return
    seen = seen | {va}
    print(f"{pad}{va:#x} ({label})  callers={len(EDGES.get(va, []))}")
    entry = fn[0] if fn else va
    for src in sorted(EDGES.get(va, [])):
        srcfn = containing(src)
        if srcfn and srcfn[0] == entry:
            print(f"{pad}  (self-recursive edge from {src:#x})")
            continue
        walk(src, depth - 1, indent + 1, seen)


target = int(sys.argv[1], 16)
depth = int(sys.argv[2]) if len(sys.argv) > 2 else 12
walk(target, depth, 0, frozenset())
