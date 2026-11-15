"""Extract the payload size each DX command handler reads.

Usage:
    python vmx-tablesizes.py

The DX dispatcher lives at .rdata 0x140a3d4e4 (236 entries).  Every handler
starts with:
        lea r8, [rsp+X]
        mov edx, SIZE
        call 0x140508bb0        ; read SIZE bytes of command payload
        cmp eax, SIZE
so the immediate before that call is the exact struct size the host expects.
Printing the vector lets us align the table against the command enum.
"""
import struct

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IB = 0x140000000
TABLE = 0x140a3d4e4
N = 236

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


sizes = []
for i in range(N):
    o = v2r(TABLE + i * 4)
    fn = struct.unpack_from("<I", data, o)[0] + IB
    ro = v2r(fn)
    if ro is None:
        sizes.append(None)
        continue
    # look for the first `mov edx, imm32` (= BA imm32) in the first 64 bytes
    # then require `call rel32` to 0x140508bb0 within the next 12 bytes
    found = None
    for k in range(0, 48):
        if data[ro + k] == 0xBA and k + 5 <= 64:
            imm = struct.unpack_from("<I", data, ro + k + 1)[0]
            # find call rel32 (E8) in [k+5, k+17]
            for j in range(k + 5, min(k + 17, 64)):
                if data[ro + j] == 0xE8:
                    rel = struct.unpack_from("<i", data, ro + j + 1)[0]
                    tgt = fn + j + 5 + rel
                    if tgt == 0x140508bb0:
                        found = imm
                        break
            if found is not None:
                break
    sizes.append(found)

print("idx  size")
for i in range(N):
    print(f"{i:4d}  {sizes[i] if sizes[i] is not None else '-'}")
