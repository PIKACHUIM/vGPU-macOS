"""扫 vmware-vmx.exe 里所有 call 到指定目标地址的调用点。

用法: python vmx_callers.py 0x1404125a0 [0x1404c7a14 ...]
只做 E8 rel32（near call）的直接调用扫描，够用于本项目的判定。
"""
import struct
import sys

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000

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


def raw2va(off):
    for nm, sva, vsize, raw, rsize in secs:
        if raw <= off < raw + rsize:
            return IMAGE_BASE + sva + (off - raw)
    return None


def va2raw(va):
    rva = va - IMAGE_BASE
    for nm, sva, vsize, raw, rsize in secs:
        if sva <= rva < sva + max(vsize, rsize):
            off = raw + (rva - sva)
            if off < raw + rsize:
                return off
    return None


def scan(target):
    hits = []
    for off in range(len(data) - 5):
        if data[off] != 0xE8:
            continue
        disp = struct.unpack_from("<i", data, off + 1)[0]
        va = raw2va(off)
        if va is None:
            continue
        if va + 5 + disp == target:
            hits.append(va)
    return hits


for arg in sys.argv[1:]:
    t = int(arg, 16)
    hits = scan(t)
    print("callers of %#x : %d" % (t, len(hits)))
    for h in hits:
        print("   %#x" % h)
