# CB/DX bring-up findings — 2026-10-08

来源：vm3dmp.sys（12.4.5 == 13.1.5，逐字节）重新逆向 + 三轮 guest 上机矩阵（kCbDump 全开）。

## 1. vm3dmp DX context 创建序列（权威，反汇编实证）

`0x140010c78 CreateDXContext(device, ctx)`：

1. **CB #1（packed，0x20 字节）**：`{0x477,4,cid}{0x479,12,cid,stateMobid,0}`
   = DX_DEFINE_CONTEXT + DX_BIND_CONTEXT，**同一条 CB**，flags=3（NO_IRQ|DX_CONTEXT），header.dxContext=0。
2. **CB #2（若 `[device+0x18ac]` 能力位开）**：`{0x4fd,12,cid,[rbx+0x58],0x2000}`
   = DX_SET_SHADER_IFACE {cid, mobid, 0x2000}——shader iface 位于 state MOB 偏移 0x2000。
3. 之后 cotable 按需绑定：真实 SET_COTABLE 构造（`0x1400113e8`）=
   `{0x4b7,16,cid,mobid,type,validSize}` 单命令 CB。
4. context 销毁：每类型一条 CB = `{SET_COTABLE{cid,0xFFFFFFFF,type,…}}{DESTROY_GB_MOB{…}}`
   （0x446=SVGA_3D_CMD_DESTROY_GB_MOB，解释了历史上的神秘尾随命令）。

命令 ID 已用 Linux svga3d_cmd.h 核实：1143=DX_DEFINE_CONTEXT、1145=DX_BIND_CONTEXT
{cid,mobid,validContents}、1207=DX_SET_COTABLE。SVGA_COTABLE_RTVIEW=0（我们一直写对）。

## 2. PREPEND 寄存器（53/54）语义 = CB 错误恢复队列前插缓冲

- vm3dmp 在**每次向 CB 追加命令**时写：`0x1400169d4(dev, PA=[desc+8], cbContext)` →
  MMIO `[mmio+0xD8]=PA>>32`、`[mmio+0xD4]=PA_low|cbContext`；IO 口模式 `out 0x36(高)` → `out 0x35(低|ctx)`。
- 真正的 CB 提交（`0x14001a76c` 尾部）：`[header+8]` 拆 64 位，低 32|ctx 经
  `0x14001692c` 写 COMMAND 48/49（IO 口先 0x31=HIGH 后 0x30=LOW；MMIO 一次 64 位写 0xC0）。
- 诊断字符串（vm3dmp .rdata）："No room left to **prepend** the command buffer, can not
  recover from command buffer error." / "Fatal command buffer header error!" →
  **PREPEND 是错误/抢占后重放剩余命令的队列前插缓冲**，不是 DX 激活开关。
- 源码路径字符串：`svga\wddm\src\miniport\cmdbuf_commands_3d.c`。
- 宿主符号（vmware-vmx.exe .rsrc 符号池，已可脚本化提取）：
  `SVGAWriteCommandPrependReg`(RVA 0x5f3f0 表值)、`SVGAQueueCommandBuffer`、
  `SVGAWriteCommandReg`、`SVGADCProcessPreempt`、`svgaDCCmdHandlers`、`svgaPortDescs`。

## 3. Guest 实测矩阵（本 MKS，2026-10-08 三次开机）

| 提交（plain CB flags=1） | 结果 |
|---|---|
| device-ctx START_STOP_CONTEXT | COMPLETED |
| DX_DEFINE_CONTEXT{1}（单命令） | COMPLETED |
| DX_BIND_CONTEXT{1,mob1,0} | **COMMAND_ERROR @0** |
| DX_BIND_CONTEXT{1,mob1,1} | **队列楔死**（超时） |
| DX_BIND_CONTEXT{1,mob3,0/1} | 队列楔死 |
| DX_SET_SHADER_IFACE{1,1,0x2000} | 队列楔死 |
| DX_SET_COTABLE{1,5,0,0} | 队列楔死 |
| **packed DEFINE+BIND {mob3}** | **COMMAND_ERROR @12** |

### 关键结论

1. **DEFINE 成功、BIND 确定性被拒**——与 mob 选择（1/3）、validContents（0/1，
   后者未在活队列上评估过）、打包方式（分离/打包，offset 12 证明 DEFINE 部分
   成功）均无关。
2. **⭐ 一条 CB 内的命令错误会楔死 context-0 提交队列**：错误后所有后续 CB
   status 恒 0（超时）。这解释了大量历史"静默"。vm3dmp 的 PREPEND 恢复机制
   正是为此存在——下一实验方向：错误后经 PREPEND 缓冲重放 / PREEMPT。
3. mob 1 同时是 GB surface 1 的 backing MOB——"绑定 surface 占用的 mob"假设
   与 mob 3（专用 512KiB state MOB）同样被拒，不是根因。
4. PREPEND 寄存器注册（已在 kext 实现，每次提交前重写 53/54）无副作用。

## 4. 下一步（按优先级）

1. **楔死恢复**：BIND 错误后试 device-ctx `SVGA_DC_CMD_PREEMPT` +
   PREPEND 缓冲重放（复刻 vm3dmp 错误路径）；确认 CB 能否复活。
2. **validContents=1 + 内容合法的 state MOB**：向 state MOB 写入
   SVGADXContextMobFormat 布局（全零 + 合法 cotable 段）后 BIND。
3. **cbContext 1 上的 CB**：楔死可能是 per-context 的，换 context 提交绕开。
4. 宿主侧：用符号表脚本（`disasm/`，含 vmx_svga_syms.json 提取器）定位
   `SVGAQueueCommandBuffer`，弄清 BIND 校验逻辑（工具链已就绪）。

## 5. 工具链（本轮新增，全部可复用）

- `disasm/vm3dis.py` — vm3dmp.sys 反汇编（VA→raw，capstone）。
- `disasm/full.py` — 全量 .text dump → `vm3dmp.text.asm`。
- `disasm/vmxdis.py` — vmware-vmx.exe 反汇编（支持 PE32+ 头解析）。
- vmware-vmx 符号池格式：`.rsrc` 内 `{u32 nameoff(B=0x10f950c 相对), u16 flag, u16 len, u64 RVA}`，
  变长步进扫描可提取全部 SVGA 符号（注意条目非 16 字节对齐，需容错扫描）。
