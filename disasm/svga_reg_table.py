"""解 vmware-vmx.exe 里 SVGA 寄存器写分派跳转表。

背景：0x1403f6fd0 附近是「写 SVGA 寄存器」的处理函数（入参 edx==1 走写路径，
r10d = 寄存器索引，要求 < 0x20）。当 r10 in [0x11, 0x1e] 时用
`mov ecx,[rdi+rax*4+0x3f749c]; add rcx,rdi; jmp rcx` 分派，其中
rdi = IMAGE_BASE（`lea rdi,[rip-0x3f7015]` 在 0x1403f700e，下一条 0x1403f7015）。
表项是相对 IMAGE_BASE 的 32 位有符号偏移。

用途：确认 [dev+0xad8] / [dev+0xada] / [dev+0xacc] 这些"设备字段"到底是
哪几个 SVGA 寄存器的镜像槽 —— 它们不是上下文计数，是寄存器影子值。
"""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

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


def va2raw(va):
    rva = va - IMAGE_BASE
    for nm, sva, vsize, raw, rsize in secs:
        if sva <= rva < sva + max(vsize, rsize):
            off = raw + (rva - sva)
            if off < raw + rsize:
                return off
    return None


def rd_i32(va):
    return struct.unpack_from("<i", data, va2raw(va))[0]


TABLE = IMAGE_BASE + 0x3f749c
print("jump table @ %#x  (regs 0x11..0x1e)" % TABLE)
md = Cs(CS_ARCH_X86, CS_MODE_64)
for i in range(0x1e - 0x11 + 1):
    reg = 0x11 + i
    tgt = IMAGE_BASE + rd_i32(TABLE + i * 4)
    # 反汇编目标处 8 条，看它写哪个字段
    off = va2raw(tgt)
    body = []
    for ins in md.disasm(data[off:off + 0x30], tgt):
        body.append("%s %s" % (ins.mnemonic, ins.op_str))
        if ins.mnemonic in ("ret", "jmp") and len(body) > 2:
            break
    print("  SVGA_REG_0x%02x -> %#x : %s" % (reg, tgt, " ; ".join(body)))
