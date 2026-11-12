"""找 [dev|slot + 0xad0 / 0xad8 / 0xadc] 这一组环形队列字段的全部访问点。

背景（docs/cb-findings §28）：0x14050a194 那段是「从队列摘一个元素」的 pop 侧：
    eax = dword[slot+0xad0]          头指针 (& 0x1f)
    rax = qword[dev + (slot*0x23 + eax)*8 + 0x9d0]
    dec dword[slot+0xad8]            长度 -1
    inc dword[slot+0xadc]
    dword[slot+0xad0] = (eax+1) & 0x1f

所以 0xad8 是队列长度、0xad0 是头指针、0xadc 是另一个游标。要找**谁把它写成 >0**
（push 侧），这才是让 [dev+0x9bc]=1、DX 创建链可达的真正条件。

注意：这里必须同时覆盖 `[reg+disp]` 和 `[reg+reg*scale+disp]` 两种寻址 ——
上一轮 scan_devfields.py 只按 mem.disp 匹配，对 `[r15+rdi+0xad8]` 这种会漏掉
（那个形态 disp 仍是 0xad8，能命中；但 `[rax*8+0x9d0]` 这类要另外看）。
输出按 disp 分组，并带上前后的指令，便于判断是 push 还是 pop。
"""
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PATH = r"C:/Program Files/VMware/VMware Workstation/x64/vmware-vmx.exe"
IMAGE_BASE = 0x140000000
WANT = {0xAD0, 0xAD8, 0xADC, 0x9D0}

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


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

text = None
for nm, sva, vsize, raw, rsize in secs:
    if nm == ".text":
        text = (IMAGE_BASE + sva, data[raw:raw + rsize])
        break
base_va, blob = text

# 线性扫描：只对带 mem 操作数的指令取 disp。
# 注意：md.disasm() 遇到第一个无法解码的字节就会停下（.text 里有跳转表、对齐填充、
# 字符串），所以必须自己推进 PC 逐条走，解码失败就 +1 继续 —— 否则扫到 0x1403f7xxx
# 附近的跳转表就整个中断，命中数恒为 0（这坑踩过）。
HITS = []
pos = 0
n = len(blob)
from capstone import CsError
while pos < n - 16:
    got = False
    for ins in md.disasm(blob[pos:pos + 16], base_va + pos, count=1):
        got = True
        if ins.operands:
            for op in ins.operands:
                if op.type == 3:  # X86_OP_MEM
                    d = op.mem.disp
                    if d in WANT:
                        HITS.append((ins.address, d, "%s %s" % (ins.mnemonic, ins.op_str)))
                        break
        pos += ins.size
    if not got:
        pos += 1

print("total hits: %d" % len(HITS))
for want in sorted(WANT):
    sub = [h for h in HITS if h[1] == want]
    print("\n== disp %#x : %d ==" % (want, len(sub)))
    for a, _, s in sub:
        print("  %#x  %s" % (a, s))
