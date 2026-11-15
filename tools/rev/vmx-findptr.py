"""Search vmware-vmx.exe for 32-bit RVAs / 64-bit VAs pointing at a target VA.

Usage:
    python vmx-findptr.py 0x140509fe8

Finds the data slots (function-pointer tables, jump tables, vtables) that
reference the address.  This is how we discover the handler that is only
reachable through a table and therefore has zero direct callers.
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
secs = []
for i in range(nsec):
    o = so + 40 * i
    nm = data[o:o + 8].rstrip(b"\0").decode("latin1")
    vs, va, rs, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((nm, va, vs, raw, rs))


def r2v(off):
    for nm, sva, vs, raw, rs in secs:
        if raw <= off < raw + rs:
            return nm, IB + sva + (off - raw)
    return None, None


tgt = int(sys.argv[1], 16)
rva = tgt - IB
pats = [
    ("rva32", struct.pack("<I", rva)),
    ("va64", struct.pack("<Q", tgt)),
]
for name, pat in pats:
    print(f"--- {name} {pat.hex()}")
    pos = 0
    n = 0
    while True:
        i = data.find(pat, pos)
        if i < 0:
            break
        pos = i + 1
        nm, va = r2v(i)
        print(f"  file {i:#x}  {nm}  VA {va:#x}" if va else f"  file {i:#x}  ?")
        n += 1
        if n > 60:
            print("  ...")
            break
    print(f"  total {n}")
