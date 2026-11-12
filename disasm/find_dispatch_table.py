"""在 vmware-vmx.exe 的 .rdata/.data 里找 DX 命令分派表。

思路：CASES-1 已知 1143(0x477) / 1145(0x479) / 1207(0x4b7) 三条命令的 handler
确实存在（它们被接受），而 1187(0x4a3) 被 COMMAND_ERROR 拒绝。若宿主用一张
(id, handler) 对的表来分派，那么三张已知的 id 会以 dword 形式出现在同一张表里，
而 0x4a3 出现与否决定它是否被登记。

做法：按 4 字节对齐窗口扫描，找「一个 256B 窗口内同时出现 >=3 个已知 DX cmd id」
的位置，把整个窗口 dump 成 (id, value) 对，人工辨认是分派表还是别的。
"""
import struct

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000

KNOWN = {0x477, 0x479, 0x4B7, 0x4A3, 0x4FD, 0x438, 0x4A2, 0x4A0, 0x49F, 0x4AC, 0x4AD}
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

# 只在数据段里扫
for nm, sva, vsize, raw, rsize in secs:
    if nm not in (".rdata", ".data"):
        continue
    blob = data[raw:raw + rsize]
    print("=== section %s  va %#x size %#x ===" % (nm, IMAGE_BASE + sva, rsize))
    # 滑动窗口 0x200，步长 4
    best = []
    W = 0x200
    for start in range(0, len(blob) - W, 4):
        win = blob[start:start + W]
        seen = set()
        for off in range(0, W - 4, 4):
            v = struct.unpack_from("<I", win, off)[0]
            if v in KNOWN:
                seen.add(v)
        if len(seen) >= 4:
            best.append((start, sorted(seen)))
    # 合并相邻
    merged = []
    for start, seen in best:
        if merged and start - merged[-1][1] <= W:
            merged[-1] = (merged[-1][0], start, merged[-1][2] | set(seen))
        else:
            merged.append((start, start, set(seen)))
    for a, b, seen in merged[:20]:
        print("  region %#x..%#x  ids: %s" % (
            IMAGE_BASE + sva + a, IMAGE_BASE + sva + b,
            " ".join("%#x" % s for s in sorted(seen))))
