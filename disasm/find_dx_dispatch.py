"""找 vmware-vmx.exe 里 DX 命令 ID 的分派表 / 比较链。

背景：kext 发的 DX_DEFINE_CONTEXT(1143) / DX_BIND_CONTEXT(1145) / DX_SET_COTABLE(1207)
全部被接受，而 DX_DEFINE_RTV(1187) 稳定返回 COMMAND_ERROR 且 errorOffset=0、
与载荷长度无关 —— 典型的"命令 ID 根本没被分派表登记"。

若确认，则 DEFINE_RTV 在本宿主上不存在，kext 侧再调参数也没用。

做法：逐条解码 .text（解码失败 +1 重试，避开数据岛），收集所有立即数等于
目标命令 ID 的指令，按 ID 分组打印，便于看出哪几条被同一个函数处理。
"""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_IMM

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000
NEEDLES = {1143: "DEFINE_CONTEXT", 1145: "BIND_CONTEXT", 1207: "SET_COTABLE",
           1187: "DEFINE_RTV", 1277: "SET_SHADER_IFACE", 1080: "DEFINE_GB_SURFACE"}

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

text = None
for nm, sva, vsize, raw, rsize in secs:
    if nm == ".text":
        text = (IMAGE_BASE + sva, data[raw:raw + rsize])
        break
base_va, blob = text

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

HITS = {k: [] for k in NEEDLES}
pos = 0
n = len(blob)
while pos < n - 16:
    got = False
    for ins in md.disasm(blob[pos:pos + 16], base_va + pos, count=1):
        got = True
        for op in ins.operands:
            if op.type == CS_OP_IMM and op.imm in HITS:
                HITS[op.imm].append((ins.address, "%s %s" % (ins.mnemonic, ins.op_str)))
        pos += ins.size
    if not got:
        pos += 1

for k, name in NEEDLES.items():
    lst = HITS[k]
    print("\n== %d %s : %d imm hits ==" % (k, name, len(lst)))
    if not lst:
        print("   (none -- id never appears as an immediate)")
    for a, s in lst[:40]:
        print("   %#x  %s" % (a, s))
