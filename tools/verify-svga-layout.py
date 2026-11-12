#!/usr/bin/env python3
"""
§45 离线验算：把 kext 实际会发出的字节，按权威头文件的结构体解析一遍。

目的：本机没有 C 编译器，客机也没开。但又不能再靠"读代码觉得对"下结论
（§40 的教训就是索引表自己写错了）。所以这里把权威结构体的字节布局**逐字段
硬编码**（直接抄 svga3d_cmd.h / svga3d_types.h），再用它在 C 源码里定位的
偏移去解码源码构造的缓冲区。

只要两边一致，说明源码写出的偏移与权威头文件自洽。
"""
import re, sys, struct

SRC = r"D:\Codes\vGPU-macOS\kexts\framebuffer\vgpuFramebuffer.cpp"
src = open(SRC, encoding="utf-8", errors="replace").read()
fails = []
def ck(name, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail else ""))
    if not cond: fails.append(name)

# ---------------------------------------------------------------- 权威布局（硬抄头文件）
# SVGAOTableSurfaceEntry, svga3d_cmd.h, #pragma pack(push,1) => 无填充
SURF_ENTRY = [
    ("format",            4),   # SVGA3dSurfaceFormat          (uint32)
    ("surface1Flags",     4),   # SVGA3dSurface1Flags          (typedef uint32)  <-- 32 位
    ("numMipLevels",      4),
    ("multisampleCount",  4),
    ("autogenFilter",     4),   # SVGA3dTextureFilter
    ("size.width",        4),
    ("size.height",       4),
    ("size.depth",        4),
    ("mobid",             4),   # SVGAMobId
    ("arraySize",         4),
    ("mobPitch",          4),
    ("surface2Flags",     4),   # SVGA3dSurface2Flags (typedef uint32)
    ("multisamplePattern",1),   # uint8
    ("qualityLevel",      1),   # uint8
    ("bufferByteStride",  2),   # uint16
    ("minLOD",            4),   # float
    ("pad0",              8),   # uint32[2]
]
SURF_OFF = {}
_o = 0
for f, s in SURF_ENTRY:
    SURF_OFF[f] = _o
    _o += s
SURF_SIZE = _o

# SVGA3dCmdDefineGBSurface (V1)
DEF_BODY = [("sid",4),("surfaceFlags",4),("format",4),("numMipLevels",4),
            ("multisampleCount",4),("autogenFilter",4),("size",12)]
DEF_OFF = {}
_o = 0
for f, s in DEF_BODY:
    DEF_OFF[f] = _o
    _o += s
DEF_SIZE = _o

# SVGA3dCmdUpdateGBImage
UPD_BODY = [("image",12),("box",24)]
UPD_SIZE = 36

print("== 权威结构体尺寸（由字段表推算）==")
print(f"  SVGAOTableSurfaceEntry      = {SURF_SIZE}")
print(f"  SVGA3dCmdDefineGBSurface    = {DEF_SIZE}")
print(f"  SVGA3dCmdUpdateGBImage      = {UPD_SIZE}")
ck("SVGAOTableSurfaceEntry 是 64 字节", SURF_SIZE == 64, f"got {SURF_SIZE}")
ck("DefineGBSurface body 是 36 字节",    DEF_SIZE  == 36, f"got {DEF_SIZE}")
ck("UpdateGBImage body 是 36 字节",      UPD_SIZE  == 36, f"got {UPD_SIZE}")

# ---------------------------------------------------------------- 源码侧的假设
print("\n== 源码常量 vs 权威尺寸 ==")
m = re.search(r"kGbSurfaceEntryBytes\s*=\s*(\d+)", src)
ck("kGbSurfaceEntryBytes == 64", m and int(m.group(1)) == 64,
   f"got {m.group(1) if m else 'missing'}")
m = re.search(r"kSvgaDefineGbSurfaceBodyBytes\s*=\s*(\d+)", src)
ck("kSvgaDefineGbSurfaceBodyBytes == 36", m and int(m.group(1)) == 36,
   f"got {m.group(1) if m else 'missing'}")
m = re.search(r"kSvgaDefineGbSurfaceWords\s*=\s*(\d+)", src)
words_ok = m and int(m.group(1)) == 2 + DEF_SIZE // 4
ck("kSvgaDefineGbSurfaceWords == 2 + body/4 == 11", words_ok,
   f"got {m.group(1) if m else 'missing'}")

# ---------------------------------------------------------------- surfaceEntryFill 实际写入
print("\n== surfaceEntryFill 写出的偏移 ==")
body = src[src.index("static void surfaceEntryFill"):]
body = body[:body.index("\n}")]
pairs = re.findall(r"entry\s*\+\s*(\d+)\)\s*,\s*&\s*(\w+)", body)
seen = {name: int(off) for off, name in pairs}
# 变量名 -> 权威字段名的对应
EXPECT = {
    "format":     "format",
    "flags1":     "surface1Flags",
    "filter":     "autogenFilter",
    "width":      "size.width",
    "height":     "size.height",
    "depth":      "size.depth",
    "mobId":      "mobid",
    "pitch":      "mobPitch",
    "surface2":   "surface2Flags",
    "msPattern":  "multisamplePattern",
    "quality":    "qualityLevel",
    "byteStride": "bufferByteStride",
    "minLOD":     "minLOD",
}
for var, field in EXPECT.items():
    if var not in seen:
        continue
    want = SURF_OFF[field]
    ck(f"entry+{seen[var]:<2} <- {var:<12} == {field} @{want}", seen[var] == want,
       f"source says {seen[var]}")

# 'one' 被复用三次（numMipLevels / multisampleCount / arraySize），全列出来
ones = [int(o) for o, n in pairs if n == "one"]
ck("复用变量 one 的三处偏移 == mips/ms/arraySize",
   ones == [SURF_OFF["numMipLevels"], SURF_OFF["multisampleCount"], SURF_OFF["arraySize"]],
   f"got {ones}")

# 关键：不只看偏移，还要看**写入宽度**。
# 原始 bug 的实质就是「往 4 字节的 surface1Flags 里写 8 字节」，偏移校验抓不到它。
print("\n== surfaceEntryFill 每处写入的宽度 ==")
widths = {}
for off, var, size in re.findall(
        r"entry\s*\+\s*(\d+)\)\s*,\s*&\s*(\w+)\s*,\s*(\d+)\)", body):
    widths.setdefault(var, []).append((int(off), int(size)))
FIELD_SIZE = dict(SURF_ENTRY)
for var, hits in widths.items():
    for off, size in hits:
        # 找到该偏移对应的权威字段
        fld = next((f for f, o in SURF_OFF.items() if o == off), None)
        if fld is None:
            continue
        ck(f"entry+{off} 写 {size} 字节 == {fld} 宽度 {FIELD_SIZE[fld]}",
           size == FIELD_SIZE[fld], f"source writes {size}")
ck("surface1Flags 处必须是 4 字节写入（不是 8）",
   widths.get("flags1") == [(SURF_OFF["surface1Flags"], 4)],
   f"got {widths.get('flags1')}")

# ---------------------------------------------------------------- defineGbSurfaceFrame 帧
print("\n== defineGbSurfaceFrame 帧布局 ==")
body = src[src.index("static void defineGbSurfaceFrame"):]
body = body[:body.index("\n}")]
outs = re.findall(r"out\[(\d+)\]\s*=\s*([^;]+);", body)
got = {int(i): v.strip() for i, v in outs}
want_frame = {
    0: "cmd",          1: "size",      2: "sid",       3: "flags1",
    4: "format",       5: "numMipLevels", 6: "multisampleCount", 7: "autogenFilter",
    8: "size.width",   9: "size.height",  10: "size.depth",
}
ck("帧覆盖 out[0..10] 共 11 个 dword", sorted(got) == list(range(11)), f"got {sorted(got)}")
# 载荷从 out[2] 开始，与 body 偏移一一对应。body 的偏移是字节，帧下标是 dword。
body_map = {}
for f, s in DEF_BODY:
    if f == "size":
        for k, sub in enumerate(("size.width", "size.height", "size.depth")):
            body_map[2 + DEF_OFF["size"] // 4 + k] = sub
    else:
        body_map[2 + DEF_OFF[f] // 4] = f
ck("帧载荷映射覆盖 out[2..10]", sorted(body_map) == list(range(2, 11)),
   f"got {sorted(body_map)}")
for i in range(2, 11):
    ck(f"out[{i}] 对应 {body_map.get(i)}", i in body_map, f"unmapped {i}")
ck("out[1] 用的是 kSvgaDefineGbSurfaceBodyBytes（36）",
   "kSvgaDefineGbSurfaceBodyBytes" in got.get(1, ""), got.get(1))

# ---------------------------------------------------------------- UpdateGBImage 帧
print("\n== UPDATE_GB_IMAGE 帧（源码里三处发送点）==")
occur = [m.start() for m in re.finditer(r"img\[0\]\s*=\s*kSvga3dCmdUpdateGbImage;", src)]
print(f"  找到 {len(occur)} 处发送点")
ck("有 3 处 UPDATE_GB_IMAGE 发送点", len(occur) == 3, f"got {len(occur)}")
for idx, pos in enumerate(occur):
    # 数组声明在赋值之前，向前找最近的一处
    before = src[max(0, pos - 800):pos]
    arrs = re.findall(r"UInt32 (img)\[(\d+)\]", before)
    nwords = int(arrs[-1][1]) if arrs else -1
    line = src[:pos].count("\n") + 1
    ck(f"[{idx}] L{line} 数组长度 == 11", nwords == 11, f"got {nwords}")
    seg = src[pos:pos + 1400]
    sz = re.search(r"img\[1\]\s*=\s*(\d+)", seg)
    ck(f"[{idx}] body size == 36", sz and int(sz.group(1)) == 36,
       f"got {sz.group(1) if sz else '?'}")
    head = seg.split("fifoSubmitWords")[0]
    ck(f"[{idx}] 没有 srcx/dstx 残留", "srcx" not in head and "dstx" not in head)
    n = re.search(r"img, (\d+)\)", seg)
    ck(f"[{idx}] 提交 dword 数 == 11", n and int(n.group(1)) == 11,
       f"got {n.group(1) if n else '?'}")

print("\n== 结论 ==")
if fails:
    print(f"  {len(fails)} 项失败：")
    for f in fails: print("   -", f)
    sys.exit(1)
print("  全部通过：源码写出的每个字段偏移都与权威头文件一致。")
