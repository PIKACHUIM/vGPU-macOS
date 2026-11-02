#!/usr/bin/env python3
"""Exhaustively list every SVGA register index vm3dmp.sys writes to the index port.

This is the evidence base for docs/svga-register-map.md. The upstream svga_reg.h layout
does NOT match this host (it skips a block of indices after BITS_PER_PIXEL), so the only
trustworthy source is what the host's own driver actually emits. Rather than reason about
individual functions one at a time -- which is how 0x14 ended up mislabelled three separate
times -- enumerate every `mov eax, imm` that reaches an `out dx, eax`, and print the
together with how many distinct call sites use it.

Usage: python tools/rev/reg-indices.py
"""
import collections
import struct
import sys
import pathlib

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PATH = pathlib.Path(__file__).resolve().parents[2] / "driver" / "vm3dmp.sys"
IMAGE_BASE = 0x140000000
# How many instructions back to look for the `mov eax, imm` that set the index. The driver
# consistently loads the index and then does `out dx, eax` within a couple of instructions,
# but a `lea edx, [r8+0x20]` form (used by the mode-set function) is a different shape and
# is reported separately rather than matched here.
LOOKBACK = 4


def load_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sec_off = pe + 24 + opt_size
    secs = []
    for i in range(nsec):
        o = sec_off + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, va, vsize, raw, rsize))
    return secs


def va2raw(secs, rva):
    for name, sva, vsize, raw, rsize in secs:
        if sva <= rva < sva + max(vsize, rsize):
            off = raw + (rva - sva)
            if off < raw + rsize:
                return off, name
    return None, None


def main():
    data = PATH.read_bytes()
    secs = load_sections(data)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    # (index) -> list of (va, section) where a write to the value port was seen
    writes = collections.defaultdict(list)
    lea_forms = collections.defaultdict(list)

    for name, sva, vsize, raw, rsize in secs:
        if name != ".text":
            continue
        code = data[raw:raw + rsize]
        insts = list(md.disasm(code, IMAGE_BASE + sva))
        for i, ins in enumerate(insts):
            if ins.mnemonic != "out" or "eax" not in ins.op_str:
                continue
            # Walk back for the last `mov eax, imm` before this write.
            for j in range(i - 1, max(-1, i - 1 - LOOKBACK), -1):
                prev = insts[j]
                if prev.mnemonic == "mov" and prev.op_str.startswith("eax, "):
                    try:
                        imm = int(prev.op_str.split(", ", 1)[1], 0)
                    except ValueError:
                        break
                    # The value port is a small immediate too; an index is anything that is
                    # not a plausible value written right after a `mov eax, <index>`.
                    writes[imm].append(prev.address)
                    break
            # The `lea edx, [r8+0xNN]` shape used by the mode-set function.
            for j in range(i - 1, max(-1, i - 1 - LOOKBACK), -1):
                prev = insts[j]
                if prev.mnemonic == "lea" and ", [" in prev.op_str and "+" in prev.op_str:
                    lea_forms[prev.op_str].append(prev.address)
                    break

    print("=== immediate indices that reach `out <port>, eax` ===")
    for imm in sorted(writes):
        sites = sorted(set(writes[imm]))
        print(f"  0x{imm:02x} ({imm:3d})  {len(sites)} site(s)  "
              f"first @ 0x{min(sites):x}")

    print()
    print("=== lea-with-displacement forms (mode-set style) ===")
    for form, sites in sorted(lea_forms.items(), key=lambda kv: min(kv[1])):
        sites = sorted(set(sites))
        print(f"  {form:24s} {len(sites)} site(s)  first @ 0x{min(sites):x}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
