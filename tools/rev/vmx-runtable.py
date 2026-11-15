"""Find contiguous runs of function pointers in .rdata.

Usage:
    python vmx-runtable.py 0x14054e000 0x140562000

Every dword that is a .pdata function entry counts as a pointer; we print any
run of >=8 consecutive such dwords together with its base VA.  A long run is a
command dispatch table, and the entry order is the command order.
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

pv, ps, pr, psz = secs[".pdata"]
entries = {}
for i in range(psz // 12):
    b, e, u = struct.unpack_from("<III", data, pr + i * 12)
    if b:
        entries[IB + b] = IB + e

lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)

for nm in (".rdata", ".data"):
    if nm not in secs:
        continue
    sva, vs, raw, rs = secs[nm]
    n = rs // 4
    run_start = 0
    run_len = 0
    i = 0
    while i < n:
        v = struct.unpack_from("<I", data, raw + i * 4)[0]
        va = v + IB
        ok = lo <= va < hi and va in entries
        if ok:
            if run_len == 0:
                run_start = i
            run_len += 1
        else:
            if run_len >= 8:
                base = IB + sva + run_start * 4
                print(f"[{nm}] run of {run_len} @ VA {base:#x}")
                for k in range(run_len):
                    vv = struct.unpack_from("<I", data, raw + (run_start + k) * 4)[0] + IB
                    print(f"    [{k:3d}] {base+4*k:#x} -> {vv:#x}")
                print()
            run_len = 0
        i += 1
    if run_len >= 8:
        base = IB + sva + run_start * 4
        print(f"[{nm}] run of {run_len} @ VA {base:#x} (tail)")
