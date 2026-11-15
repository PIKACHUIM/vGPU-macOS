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

0. **MKS 代次假说（10-08 深夜升级为首位）**：DX 行为随 MKS 进程代次变化——
   14:04（上一 MKS 进程）：flags=3 DX CB 通过、BIND{1,1,0} 成功、卡 SET_COTABLE；
   19:15 起的 MKS 进程：flags=3 CB → CB_HEADER_ERROR、BIND 一律 COMMAND_ERROR
   （cid=1/2、mob=1/3、valid=0/1、分离/打包全试遍，含 22:44 全新 power cycle）。
   两代之间 guest 代码仅差 PREPEND 注册（已排除：kCbPrepend=0 下 BIND 仍拒）。
   → 下次实验：逐行 diff 两代 vmware.log 的 SVGA3dCaps 段（尤其 "guest, compatibility
   level" 与 vmotion.svga.* 钳位），并 dump FIFO 寄存器对比 GUEST_3D_HWVERSION(288)。
1. **mksSandbox.exe 静态分析**：DX 命令号 0x477 在 .text 仅出现于 NOT_REACHED 断言
   分支——真正的 DX 分派走基址差跳转表。找到 DXBindContext 处理函数即可读出校验条件。
   DX 命令名表：.data 0x36f2xx（名字指针数组）；DXBindContext 名串在 .rdata 0x2832f8。
2. **楔死恢复**：BIND 错误后试 device-ctx SVGA_DC_CMD_PREEMPT + PREPEND 重放。
3. **validContents=1 + 内容合法的 state MOB**（至今未在活队列上评估过）。

## 5b. 补充实验记录（10-08 深夜）

| boot | 配置 | 结果 |
|---|---|---|
| 21:02 | PREPEND 注册 + DEFINE 单发 + BIND 矩阵 | BIND{1,1,0}=ERROR@0，其余全楔死 |
| 21:11 | packed DEFINE+BIND{mob3} | ERROR@12（DEFINE 过、BIND 拒），队列楔死 |
| 22:34 | kCbPrepend=0（排除 PREPEND）+ BIND{1,1,0} | 仍 ERROR@0 |
| 22:44 | 全新 power cycle（vmrun start，MKS 全新） | 仍 BIND 拒 |
| 22:56 | cid=2（排除 context-1 污染） | 仍 BIND 拒 |

- MKS 对 CB 命令错误**完全静默**：mks.sandbox.log.vmxShadowAll=TRUE 加进 vmx
  （备份 .vmx.bak-shadow）也未捕获任何 BIND 拒绝日志。
- **日志读取坑：guest 默认 shell 是 zsh，`log` 是 zsh 内建**——必须 `sh -c 'log show ...'`。
- 14:04 成功记录与今晚矛盾的唯一剩变量 = MKS 进程代次（guest 代码侧已无差异）。

## 5. 工具链（本轮新增，全部可复用）

- `disasm/vm3dis.py` — vm3dmp.sys 反汇编（VA→raw，capstone）。
- `disasm/full.py` — 全量 .text dump → `vm3dmp.text.asm`。
- `disasm/vmxdis.py` — vmware-vmx.exe 反汇编（支持 PE32+ 头解析）。
- vmware-vmx 符号池格式：`.rsrc` 内 `{u32 nameoff(B=0x10f950c 相对), u16 flag, u16 len, u64 RVA}`，
  变长步进扫描可提取全部 SVGA 符号（注意条目非 16 字节对齐，需容错扫描）。
## 6. 深夜终局修正（23:07-23:45）：14:04 证据被推翻

关键发现：调取客体会话日志库（跨重启持久）中 14:04 boot 的原始行，发现当时日志里
**没有任何 BIND 结果行**——"BIND 成功"只是"走到了 SET_COTABLE"的推断，而那个版本的
代码在 BIND 失败时可能并不提前返回、也不打日志。**BIND_CONTEXT 很可能从未被任何
MKS 代次接受过。**

由此"MKS 代次假说"降级（四代 MKS 行为一致的证据链）：
- 23:34 全新 MKS（冷启动、无 driver id、无 otable 预写、无 PREPEND、flags=1）
  → DEFINE 接受、BIND 仍拒。
- 23:21 全新 MKS + flags=3 → DEFINE 直接 CB_HEADER_ERROR。
- 17:15 前后的 vmx diff = 无 SVGA 差异；四代 vmware.log 的 compatibility level 全 = 10；
  FIFO caps（0x77f/3D_HWVERSION=0/3dcaps 全零）两代完全一致。

**最终确定的事实基线（不再有幽灵变量）**：
1. DX_DEFINE_CONTEXT（plain CB flags=1）是唯一被接受的 DX 命令。
2. 其余一切 DX 命令（BIND/SET_COTABLE/SET_SHADER_IFACE/GB-define-via-CB）一律
   COMMAND_ERROR@0；DX flag CB 一律 CB_HEADER_ERROR。
3. DEFINE 是唯一不引用 MOB 的 DX 命令 → **工作假设：MKS 的 CB 路径对 DX 命令中
   的 MOB 引用做了某种校验并拒绝**（或 DX 分派器要求 DX_CONTEXT flag 而该 flag 又
   被头校验拒绝——两者必居其一，mksSandbox 静态分析可裁决）。
4. CB 命令错误楔死 ctx-0 队列（已多次复现）。

**方向判定（回答"方向是不是歪了"）**：
- 没歪。老 SVGA3D 管线不存在（vGPU10 设备 3D_HWVERSION 恒 0），DX/CB 是唯一 3D 路径。
- 今晚的"回归恐慌"是方法论教训：bisect 必须给每个命令加正向日志（成功也要打），
  不能靠"没有失败日志"推断成功。已纠正。
- 最高杠杆下一步 = mksSandbox DX 分派器静态分析（DXBindContext 校验条件），
  而非继续盲试字节序列。
## 7. 00:05-00:30 补充：精确尺寸假设也被否

- 从 svga3d_dx.h 精确计算 sizeof(SVGADXContextMobFormat) = **8964 B**（SHADERTYPE=7、
  QUERY=64、COTABLE=12、UAV=64）→ 页对齐 12288 B / 3 页。
- 全新 12288 B state MOB（mob 6, PT64_1, 3 页）+ otable {2,6} + BIND{2,6,0}
  → **仍 COMMAND_ERROR@0**。
- 过程坑：3 页缓冲的 getPhysicalSegment 循环误用 _cotableMem（4KB）导致 "page 1
  not resolvable"，已修（_ctxStateMem）。IOBufferMemoryDescriptor mask 0xFFF 对
  多页非连续分配的逐页 getPhysicalSegment 与 gbBringUp 512KB 用法相同，可用。

**客体侧单变量已全部穷尽**：flags(1/3)、打包(分离/packed)、cid(1/2)、state mob
(16KB/512KB/12288B)、otable 预写(开/关)、driver id(开/关)、PREPEND(开/关)、
冷启动/热重启。BIND 一律 COMMAND_ERROR@0，DEFINE 一律通过。

**P1 剩余唯一路径 = mksSandbox DX 分派器静态分析**。已建立的线索：
- DX 命令名表（.data 0x36ef50 起 2120 项，index = cmdId - 1040）；
- DXBindContext 名串 .rdata 0x2832f8；
- 0x477 在 .text 只出现在 NOT_REACHED 断言 → 分派走基址差跳转表；
- .rdata 四个大跳转表（93/105/80/80 项）已定位，均未确认归属。
- 下一步具体动作：以 SVGA_CB_STATUS_COMMAND_ERROR(3)/CB_HEADER_ERROR(4) 的
  写入点为锚，回溯到命令解析循环，再进 DX 分派分支；或在 Windows 客体上抓
  vm3dmp 的完整 CB 序列做金标准对照。
## 8. 10-09 深夜：VMX DX 分派表与 BIND 处理链完整定位（突破）

**vmware-vmx.exe 自带完整 DX 命令分派表**（.rdata，stride 24B）：
`{u32 cmdId; u32 pad; u64 handlerVA(.text); u32 minDwords; u32 maxDwords}`
- 表起点 ≈ VA 0x140BDCF00（首条目 0x477）
- 0x477 DEFINE_CONTEXT → 0x14055B900
- 0x479 BIND_CONTEXT   → 0x14055E5B0（min=1, max=0xE dwords）

**BIND 处理链（全部已定位）**：
```
handler 0x14055E5B0:
  read 4 dwords body (0x140508BB0)
  hash(cid) 查全局哈希表 (0x140A0DF40)
  ├─ 命中 → 0x1404D1740 真正绑定工人:
  │    0x1404CFD60(cid): cid≤0xFFFF + otable(type5) 条目存在 + entry.cid==cid
  │      + 0x1404CA530(...,0x2000) MOB 校验
  │    [record+0x5B74]==-1 → 直接成功
  │    全局字节门 [BSS 0x1410F3A6D]==0 → FAIL   ← 头号嫌疑
  │    0x1404CA530(mobid, validContents, …, 0x4000) MOB 校验
  └─ 未命中 → 0x1404CBC80(5,cid,1,&out) otable 兜底查找
       entry.cid!=-1 && entry.mobid!=-1 → 0x100(成功) else 0x101
```
错误码：0x100=成功, 0x101=失败(COMMAND_ERROR), 0x102=分配失败。
DEFINE(0x14055B900) 同构：hash 未命中 → otable 创建/置位 → 0x100。

**头号嫌疑**：BSS 字节门 0x1410F3A6D（"DX context mob binding enabled" 类开关）——
未找到直接写它的指令（可能被整块 memcpy 初始化）。次要：0x1404C7D30（对 guest otable
内存的读取校验，参数含 PT 深度）。

**意义**：DX 协议在 VMX 内有完整本机实现——不再是黑盒。下会话收尾动作：
1. 反汇编 0x1404C7D30 与 0x1404CA530 内层，确定 MOB 校验的确切条件；
2. 找 0x1410F3A6D 的初始化者（memcpy 模式扫描 / 启动路径回溯）；
3. 若门是 driver-id/沙箱握手设置 → 补齐对应 guest 侧步骤即可让 BIND 通过。
## 9. 10-10 下午：宿主子系统恢复 + GB context 渲染链全通（渲染里程碑）

**宿主恢复**：VM 无法启动的真凶 = 残留 `.vmx.lck` 目录（每次失败启动都会刷新它），
删除后 vmrun 正常启动。"Module CPUID initialization failed" 是锁冲突的伴随症状，
不是 Hyper-V 问题（本机一直跑 WHP 模式，属正常）。

**kext 修复**：
- panic 解码：读 `/Library/Logs/DiagnosticReports/Kernel-*.panic`，故障 PC = kext+0x6707
  `mov eax,[rax]`，rax = this+0x2a8 = `_dxVirt`——PT 修复重构时丢失了 `_dxVirt` 赋值，
  读回验证解引用 NULL → 崩溃循环（uptime 123s = bring-up+90s 延迟测试触发时刻）。
  **已修**（`_dxVirt = _dxMem->map()->getVirtualAddress()`）。
- 延迟 DX 测试设施：thread_call 延迟 kDxTestDelayS=90s（dxTestC 双参），presentTick
  以 _dxBusy 互斥——冻结/panic 后日志仍可从持久库读出，恢复流程安全化。

**⭐ 渲染里程碑：GB context 渲染链全链路被宿主接受**（第一个在 darwin guest 上被
接受的渲染路径命令组，vmwgfx/QEMU 同款老管线）：
```
DEFINE_GB_CONTEXT(1107){cid=1}        -> accepted
BIND_GB_CONTEXT(1109){1, mob7, valid} -> accepted (64KB state MOB)
DEFINE_GB_MOB64{mob7, PT64_1}
SETRENDERTARGET(1050){1, COLOR0, sid3} -> accepted
SETVIEWPORT(1055){1, 0,0,256,256}      -> accepted
CLEAR(1057){1, COLOR, white}           -> accepted
READBACK_GB_IMAGE(1103){sid3}          -> accepted
```

**读回判别实验**（预填充品红 → CLEAR 白 → READBACK）：
- 结果：缓冲区**原封不动（品红）** → CLEAR 没有实际渲染（或 READBACK 是 no-op）。
- 状态 MOB 探针：宿主不向 guest 状态 MOB 回写状态（全零，状态在宿主内部）。
- 未决二选一：①MKS 老管线 CLEAR 未实现渲染（仅分派接受）；②READBACK_GB_IMAGE
  为接受但不落盘（类似 legacy UPDATE 的表现）。

**下一步（优先级序）**：
1. **显示侧验证**：把被 CLEAR 的 surface 接到 STDU screen target（b89c619 已证明
   STDU 显示路径可用），VNC 截屏看 CLEAR 颜色是否上屏——上屏 = 宿主渲染实证，
   READBACK 是否 no-op 就无关紧要（渲染→显示已是可用管线）。
2. UPDATE_GB_IMAGE(1101) guest→host 方向同步测试，隔离 DEFINE 快照行为。
3. 若 CLEAR 确未实现：查 MKS 二进制老管线分派表（.data 命令号 1057 处理项），
   或试 SETSCISSORRECT/ZRANGE 等更多状态命令。
## 9. 10-10 晚：GB context 渲染路径全链路实测——accepted but inert 的终审

kext 新增 **gbContextTest**（老 SVGA3D 渲染路径，vmwgfx/QEMU vmsvga3d 同款，绕开 DX）+
**延迟测试框架**（thread_call 延迟 kDxTestDelayS=90s 执行 + presentTick _dxBusy 互斥，
保证 SSH/logd 就绪后测试结果必定落盘，MKS 楔死也不丢数据）。

**23:39 boot（driver-id 关）与 23:49 boot（driver-id 早写 + 晚写）结果逐字节一致**：
```
DEFINE_GB_CONTEXT accepted
BIND_GB_CONTEXT accepted (mob 7, 65536 B)
SETRENDERTARGET accepted (sid 3)
SETVIEWPORT accepted
CLEAR accepted
readback p0/pMid/pLast = ffff00ff（= 预填品红，非 CLEAR 目标色）
state mob all zero（宿主从不写 context 状态机）
=> CLEAR did not render (buffer unchanged)
```

**结论（终审）**：
1. MKS 对 darwin 客体**接受一切** SVGA3D/GB 命令（fence 全 ack），但**从不调用 3D
   渲染引擎**——既不执行 context 状态机，也不画一个像素。与 DX 路径（DEFINE 过、
   引用 MOB 的全拒）在更高层汇合为同一件事：这个客体的 3D 被宿主整体关闭。
2. 客体侧可变量**全部穷尽且全部无效**：driver-id（早/晚）、PREPEND、MOB 尺寸、
   context id、打包方式、flags、冷/热启动。渲染门不在客体可达的任何寄存器/命令里。
3. 剩余唯一可动变量 = **宿主 VM 配置层**（guestOS=darwin19 → MKS 初始化时的渲染
   使能决策）。这解释了全部现象：VMware 从未发布过 macOS 客体的 3D 驱动，MKS 对
   darwin 客体禁用 3D 是产品决策，大概率是配置期的静态分支。

**下一步（唯一有杠杆的路线）**：
- 静态分析 mksSandbox/vmware-vmx 中渲染使能决策：以 VMX DX 分派表（已定位）与
  BSS 门 0x1410F3A6D 为锚，回溯其初始化者——它大概率由 VM 配置（guestOS/能力
  协商）设置。找到条件 = 找到开关。
- 或 vmx 实验线：guestOS 变体（unlocker 已补丁的平台位）+ SVGA 能力钳位对比。

**工程资产（本轮新增，全部已入库）**：
- gbContextTest：老 SVGA3D 全链路测试（GB context + RT + CLEAR + READBACK 品红校验），
  命令体布局来自 Linux svga3d_cmd.h，可作为未来任何渲染通道的自检门。
- 延迟测试框架：kDxTestDelayS + dxTestC + _dxBusy 互斥——任何会楔死 MKS 的实验都
  应该用这个模式跑。
- 部署工具链：mssh 内置重试、断言式部署脚本（grep 断言开关状态后再 build）。
## 10. 10-11/12 深夜：分派表重析 + BIND handler 全解码 + CB flags 真凶 + gate 补丁实验

**宿主日志证实 3D 设备层是开的**（vmware.log）：
`MKSRenderMain: PowerOn allowed BasicOps ISB DX12 DX11 DX11Basic VK`、
`svga.supports3D bool 1`、`SVGA3dCaps: host, at power on (3d enabled)`。
渲染门不在设备能力层。

**gbc 经 CB 被拒**：DEFINE_GB_CONTEXT via plain CB → CB 拒（legacy FIFO 则接受但惰性）。
vGPU10/DX 设备上 legacy "GB context" 概念已废弃——渲染唯一通道 = DX 命令经 CB。

**分派表重新解析（此前错位）**：stride 24，布局 {u32 id; u32 pad; u32 pad2;
u64 handler @+12; u32 min/max}：
- 0x477 DEFINE_CONTEXT → 0x14055b900（读 1 dword body）
- 0x478 DESTROY_CONTEXT → 0x1405591b0
- 0x479 BIND_CONTEXT → 0x14055e5b0
- 0x47a READBACK_CONTEXT → 0x14055cb50

**BIND handler 全解码（0x14055e5b0）**：
```
读 body {cid, mobid, valid}——但只使用 cid（mobid/valid 被无视！）
hash(cid) 查记录：
├─ 存在 → worker 0x1404d1740:
│    ctx-validate([record+0])（otable type5 条目 + entry.cid==cid
│      + mob-validate(entry.mobid, 0, buf, 0x2000)）
│    [record+0x5b74]==-1 → 直接成功（无 gate、无 mob 校验！）
│    gate byte [0x1410F3A6D]==0 → 拒
│    mob-validate(record.mob, record.5b78, buf2, 0x4000)
└─ 不存在 → OTABLE FALLBACK：0x1404cbc80(5,cid,1,&out) 读条目，
     entry[0]!=-1 && entry[1]!=-1 → 0x100 成功！
```
**要点**：BIND 命令的 mobid 被宿主无视；record 的 mob 绑定来自其他路径；
无 hash 记录时 BIND 只查 otable 条目（客体内存，我们预写）。

**gate 补丁实验**：vmware-vmx.exe .data 0x10f246d 0→1（备份 .bak-gatepatch，
UAC 提权补丁）。BIND 仍拒 → gate 不充分（或未到达该阶段）。

**⭐ CB flags 真凶**：cbSubmitDx 一直发 flags=3（NO_IRQ|DX_CONTEXT），
vm3dmp 是 **flags=2（仅 DX_CONTEXT）**——所有历史 CB_HEADER_ERROR 的来源。
已修（`(dxContext != 0) ? 2 : 1`）。STEP1 BIND-no-define（flags=3 末版）→
CB_HEADER_ERROR 实证。flags=2 版已部署但 guest crash-loop 未取到结果。

**稳定性问题**：安装后 guest 间歇 crash-loop（2min20s 周期重启、auxKC 重建期），
且 vmrun hard stop 后 vmware-authd 反复楔死（非管理员无法修，需 GUI 开机或管理员
Restart-Service VMAuthdService）。

**下一步（按优先级）**：
1. 开机读 flags=2 boot 的三步序列结果（STEP1 fallback BIND 是关键判据）；
2. 若 STEP1 成功：DEFINE → BIND(worker) → SET_COTABLE → RTV → CLEAR → READBACK
   全链（_dxBusy 延迟框架已就位）；
3. 若 STEP1 仍拒：读 CB status 细分（COMMAND_ERROR vs HEADER_ERROR vs timeout），
   查 0x1404cbc80 对 otable 条目的解析（条目尺寸是否真是 8B）。
## 11. 10-13：DX otable 16B 步长 + guest 侧再次穷尽 + 处理进程归属修正

- **DXCONTEXT otable 条目步长 = 16B**（VMX 0x1404d1810：0x1404c9fb0(edx=cid*16, r9d=0x10)
  读 16B/条目）——kext kGbDxCtxEntryBytes 已 8→16 修正（kGbDxCtxTableEnts 256→128）。
- **guest 侧再穷尽**：BIND-no-define（fallback）/ BIND-after-define（worker）/ DEFINE 后
  重写 otable 条目 / 16B 步长 / gate=1 补丁——全部 COMMAND_ERROR@0。
- **关键推理**：宿主的 DX otable 读取不来自 guest MOB 预写（与 surface otable 的懒读
  不同）——宿主维护内部影子，仅由宿主侧操作更新。BIND handler 无视命令 mobid，而
  VMX 内没有任何路径把 {cid,mob} 写进影子 → **BIND 处理不在 VMX，而在 mksSandbox.exe**
  （独立渲染进程，有 DXBindContext trace 名；VMX 的 DX 分派表属于 legacy/replay 路径，
  mksSandbox 日志 "SVGA3dDevCaps: replayFifo host" 佐证）。
- **CB header 语义修正**：DX_CONTEXT flag 下 header.dxContext 必须有效——但 dxContext=1
  （DEFINE 后有效）仍 HEADER_ERROR → 该 MKS 对 DX flag CB 直接头拒绝（或校验更严）。
- **DEFINE 语义修正**：0x14055b900 = "若条目已绑定则重置 {-1,-1}"（unbind），不创建
  hash 记录。
- **下一步唯一主线：mksSandbox.exe 的 DX 分派器逆向**（DX 名字表 .data 0x36ef50 起
  2120 项，index=cmdId-1040；DXBindContext 名串 .rdata 0x2832f8；命令处理函数经
  基址差跳转表分派）。或：Windows 客体 vm3dmp 金标准对照（WIN10-IOA 可用）。
- VM 现状：实验 kext（kFbStage=2/kGbStage=4/kDxTest=1，测试已跑完）稳定运行中。
## 12. 10-13 深夜：vm3dmp 金标准对齐——cbContext/flags/DC cmd 3 全复刻仍 HEADER_ERROR

**vm3dmp 提交路径精读（0x140013990 header builder + 0x14001a76c submit）**：
- header 布局与我们一致（status/err/id q64/flags@0x10/length@0x14/PA q64@0x18/
  offset@0x20/dxContext@0x24）。
- **DX CB 走 cbContext=1/2（从不 0）**，flags = NO_IRQ|DX_CONTEXT（=3，另有条件 |4），
  header.dxContext = 真实 cid（cbContext∈{1,2} 且 dxCtx!=-1 时写）。
- flags 计算含条件 **|4**（bpl 参数置位时）——位 4 语义未知。
- 设备命令（cbContext 0x3f）：cmd 1 = START_STOP_CONTEXT{enable,ctx} 12B、
  cmd 2 = PREEMPT 12B、**cmd 3 = {cid} 8B**（devcap bit12 门控）——我们补发了 cmd 3。

**kext 实测矩阵（全部 CB_HEADER_ERROR）**：
| 变体 | 结果 |
|---|---|
| cbContext=1 + flags=3 + dx=cid + packed DEFINE+BIND | HEADER_ERROR |
| 同上 + flags=7（|4） | HEADER_ERROR |
| 同上 + PREPEND 注册启用 | HEADER_ERROR |
| 同上 + DC cmd 3 先行 | HEADER_ERROR（但 cmd 3 本身 ACCEPTED ✓）|
| plain CB DEFINE（对照） | COMPLETED（恒定）|

**结论**：DX flag CB 在 header 校验层被无条件拒绝——与 cbContext/flags/dxContext
取值、PREPEND、DC cmd 3 均无关。validator 检查的东西在我们控制之外（大概率是
mksSandbox 侧的上下文状态：DX context 存在性/绑定状态在 sandbox，VMX 只转发）。

**下一步**：
1. mksSandbox.exe DX 分派器逆向（名字表 0x14036ef50 idx=cmdId-272 已确认；找
   SVGA3D 命令处理循环与其 CB 接收路径——sandbox 从 VMX 收什么、缺什么状态）；
2. Windows 侧金标准 trace：WIN10-IOA 上用内核调试器记录 vm3dmp 实际 port/MMIO 写
   序列（含 HEADER 内容），与我们逐字节 diff；
3. 或检查 VMX 头校验函数（CB_HEADER_ERROR 写 status=4 处）——锚点：搜写 status=4
   的代码。
## 13. 10-14 凌晨：设备队列不楔死 + 队列启动语义待解

- **⭐ 设备队列（cbContext 0x3f）的 COMMAND_ERROR 不楔死队列**——错误后同队列后续 CB
  照常 COMPLETED（ctx-0 队列则会楔死）。迭代实验应尽量走设备队列！
- **DC cmd 3 {cid 1} 恒 ACCEPTED**（DEFINE 前/后均可）。
- **START_STOP_CONTEXT{enable=1, ctx=1} 恒 COMMAND_ERROR@0**（cmd 3 前/后都试过）——
  payload 语义不对或队列 1 不可用此命令启动。vm3dmp 的 cmd 1 发送点 0x140016550
  （body {1, (signed)dl, 0} 12B），其调用者的 dl 值待查。
- 失败 header 全量 dump 已加（宿主只写 status/errorOffset，无额外诊断）。
- **DX flag CB（flags 含 bit1）仍无条件 CB_HEADER_ERROR**，与 cbContext/flags/dxContext/
  PREPEND/DC cmd 3 无关。
- **kext 提交现状**：DX CB 走 cbContext=1（cbSubmitDx），flags=7（含实验 bit4），
  header.dxContext=cid；设备队列引导含 q0 启动 + cmd3；PREPEND 注册开启。
- **待解主线**：
  1. vm3dmp 0x140016550（cmd1 发送）的调用者与 dl 值——确认队列启动的正确 payload；
  2. mksSandbox DX 分派器（CB 接收后到 DX 处理的路径——header 校验可能就在 sandbox）；
  3. 用设备队列做 DX 命令探测（不楔死！）——例如 DEFINE/BIND 直接发 0x3f 队列看错误类型。
## 14. 10-14：设备队列探测——DX flag CB 拒绝与队列无关，guest 侧穷尽完毕

- DX CB（packed DEFINE+BIND）发 **cbContext=0x3f（设备队列）**：同样 STATUS 4
  （CB_HEADER_ERROR）→ 拒绝与队列无关，是 DX_CONTEXT flag + dxContext 组合本身
  在 header 校验层被拒。
- guest 侧可变量 100% 穷尽：flags{2,3,7}、cbContext{0,1,0x3f}、dxContext{0,1}、
  PREPEND 注册、DC cmd 3、队列启动命令、gate 补丁、otable 步长。
- **结论**：拒绝条件在宿主内部状态（与 guest 提交内容无关）。剩余手段：
  ①宿主侧动态分析（管理员 x64dbg 附着 vmware-vmx.exe，断 COMMAND 写处理链/或对
  status=4 写下断点）；②mksSandbox DX 分派器全量逆向；③Windows 金标准 trace。
- kext 现状：DX CB 走 cbContext=1 + PROBE-A（设备队列）；下次部署应按实验矩阵切换。
## 15. 10-21 凌晨：🏆 CB_HEADER_ERROR 根因实锤——flags 只允许 bit0（NO_IRQ）

**工具链**：x64dbg MCP（80 工具，SSE 127.0.0.1:3000）+ 自研 `disasm/mcp_x64dbg.py` 客户端。
VMX 有 DACL 自我保护（OpenProcess ALL_ACCESS 被拒，mksSandbox 允许）→
**必须以管理员运行 x64dbg 才能附着 vmware-vmx.exe**（SeDebugPrivilege 越过 DACL）。

**定位方法（可复用）**：
1. kext 在 data 缓冲 +0x800 写标记 "VGPU-CB-DATA-AT-0x800-DEADBEEF!"（宿主不会碰）→
   在 VMX 进程内存搜到标记 → data 缓冲宿主地址 → header 地址 = data + (headerPA-dataPA)；
2. 在 header+0（status dword）下**硬件写断点** → 每次 CB 完成都会命中；
3. 命中现场：rax=header VA、rcx=status 值、返回地址在 [rsp] → 静态反汇编调用者。

**捕获到的写入链**：
```
wrapper 0x140603530(dst, &status, len)  —— 把状态写回 guest 内存
  ↑ 调用者 0x140087df1（CB 完成路径）
  ↑ 0x140087dd1: mov ebx,4   ← CB_HEADER_ERROR 的产生处
```
**校验器 (0x140087c40..d50) 逻辑**：
```
eax = header.length(+0x14)
if (len == 0) -> status 1 (COMPLETED)
page-crossing check on header.ptr.pa(+0x18) + len  -> fail: status 4
call 0x140607760                                     -> fail: status 4
test dword header.flags(+0x10), 0xFFFFFFFE           -> != 0: status 4   ★★★
...
call DC process(0x140603b30) -> 命令层错误 -> status 3 (+errorOffset)
```

**结论（实锤）**：**SVGACBHeader.flags 只允许 0 或 1（SVGA_CB_FLAG_NO_IRQ）**。
任何其它位（包括我们一直用的 DX_CONTEXT=2）都会在 header 校验处以
CB_HEADER_ERROR(4) 被拒——因此 flags=2/3/7 的全部历史实验注定失败。
vm3dmp 的 DX 路径不走这个 validator（或本产品的 DX flag 被禁用）。

**实测验证**：probe 改 flags=1 后，状态立刻从 **4 → 3（COMMAND_ERROR）**：
header 层完全通过，现在只剩命令处理层（DEFINE/BIND handler）的拒绝。
设备队列（0x3f）与 ctx-0 都是 status 3。

**下一步**：在 0x140603b30（DC 命令处理）处断点，看 DEFINE 与 BIND 谁被拒、
被哪条规则拒（命令层错误码 3 的来源）；然后修命令序列。
## 16. 10-22 凌晨：🏆 DX 上下文全通 —— DEFINE + BIND + SET_COTABLE 全部被接受

### 胜出配置（可复现）

| 环节 | 取值 |
|---|---|
| DXCONTEXT otable 注册 | `SET_OTABLE_BASE64 {type=5, base=ppnDxCtxPt(PT 页), size=2048, valid=0, ptDepth=PT64_0(4)}` |
| otable 条目 | `{cid=1, mobid=3}`，写在 DX 表数据页 `cid*8` 处（条目 8 字节，type5 由 `0x140a080c0` 确认） |
| CB 通道 | queue **0**、`flags=1`（NO_IRQ）、`header.dxContext=0` |
| DEFINE_CONTEXT | body `{cid=1}`，4 字节 |
| BIND_CONTEXT | body `{cid=1, mobid=3, valid=0}`，12 字节（宿主只读 cid） |
| SET_COTABLE | body `{cid, mobid=5, type=0(RTVIEW), validSize=4096}`，16 字节 |

实测日志（2026-10-22 00:56）：
```
dx: VAR0 cid 1 entry {1,3} DEFINE ACCEPTED entry-after {1,3}
dx: VAR0 cid 1 entry {1,3} BIND -> ACCEPTED
dx: *** BIND ACCEPTED cid 1 ***
dx: SET_COTABLE mob5 valid4096 -> ACCEPTED
```

### 决定性修复（两个真 bug）

1. **CB flags 只允许 0/1**（10-21 已实锤）：任何 DX_CONTEXT(2) 位 → CB_HEADER_ERROR(4)。
   DX 命令因此必须走 **queue 0 + flags=1**，上下文由命令体里的 cid 指定（DEFINE/BIND/
   SET_COTABLE 都带 cid）。
2. **`SVGA3dCmdDXSetCOTable` 字段顺序是 `{cid, mobid, type, validSizeInBytes}`**
   （svga3d_dx.h:1555）。此前写成 `{cid, type, mobid, valid}` → 连 unbind 形式
   （mobid=INVALID_ID）都被拒。修正后立刻 ACCEPTED。**教训：结构体字段顺序必须逐字
   对照头文件，不能凭语义猜。**

### 关键静态事实（本轮新增）

- `0x140a080c0(type)` = 每条目字节数分派：type0=16、type1=64、type2=8、type3=16、
  type4=64、**type5(DXCONTEXT)=8**。
- DEFINE handler（0x14055b900）：`ebx` 初值 0x101，只有"读到条目且 entry[0] != -1
  → 把条目写成 {-1,-1}"这一条路径才置 0x100。即 **DEFINE 需要条目可读且非空**。
- BIND handler（0x14055e5b0）：hash 记录不存在 → fallback 读 otable 条目，
  `entry[0] != -1 && entry[1] != -1` → 0x100。
- vmwgfx 金标准（vmwgfx_mob.c:141-166）：`size <= PAGE_SIZE` 时 `pt_level = PT64_0`、
  `baseAddress = pt_root_page >> PAGE_SHIFT`，`sizeInBytes = PFN_ALIGN(size)`，
  `validSizeInBytes = 0`。我们的注册与该逻辑一致。

### 尚未解释的现象（诚实记录）

- 同一注册下 **cid=1 通过，cid=9/10/11/12 全部被拒**（条目内容分别为 {0,0}/{0,3}/
  {11,0}/{3,12}），原因未明（可能与宿主内部 DX 上下文表容量/惰性初始化有关）。
  当前策略：先用 cid=1 把渲染链打通，之后再回头查 cid 上限。
- 数据页基址（PT64_0 + 数据页）与 PT64_1 + PT 页两种"更自洽"的组合反而全拒——
  只有"PT 页 + PT64_0"这一组能让 cid=1 通过。宿主读取的具体路径仍待动态确认。

### 下一步

1. **RTV / CLEAR / READBACK**：这些 DX 命令（svga3d_dx.h:939/386/566）**不带 cid**，
   依赖"当前上下文"。需要在 flags=1 的前提下测试 `header.dxContext=cid` 是否被接受
   （validator 只查 flags，dxContext 可能仍传给命令层）。
2. 若 dxContext 无效：改用 DC 设备命令（cmd 1 START_STOP_CONTEXT/RESUME）把 queue 1
   启动起来，让 DX CB 能带上下文。
3. 之后才是 READBACK 品红校验 → Metal 对象层。
## 17. 10-23 凌晨：上下文创建链全通，渲染命令（DEFINE_RTV）仍被拒

### 本轮通过的命令（按此顺序，全部 ACCEPTED）

```
DEFINE_CONTEXT   {cid 1}                                   queue0 flags1
BIND_CONTEXT     {cid 1, mob 3, valid 0}                    queue0 flags1
SET_COTABLE      {cid 1, mobid 5, type 0(RTVIEW), 4096}     queue0 flags1
DXBindShaderIface{cid 1, mob 3, 0x2000}   (cmd 1277)        queue0 flags1
```

顺序很重要：**SET_SHADER_IFACE 必须在 SET_COTABLE 之后**。放在之前会让
SET_COTABLE 被拒（实测 00:32 那轮）。

### 仍被拒的命令

`DEFINE_RENDERTARGET_VIEW`（1187）在所有组合下都拒：

| 变体 | 结果 |
|---|---|
| queue0 / dxContext 0 / arraySize 1 | refused |
| queue1 / dxContext 0 / arraySize 1 | refused |
| queue0 / dxContext cid / arraySize 1 | refused |
| queue1 / dxContext cid / arraySize 0 | refused |

所以与 cbContext、header.dxContext、desc 都无关。这条命令**不带 cid**，属于
"渲染状态类"，怀疑需要上下文被激活（START_STOP_CONTEXT 或 DC cmd 3 真正建出
`[ctx+0x150]`）。

### 本轮修掉的两个结构错误

1. **surface otable 条目尺寸 72 → 64**。`SVGAOTableSurfaceEntry` 是 64 字节
   （format, surface1Flags, numMipLevels, multisampleCount, autogenFilter,
   size{w,h,d}, mobid, arraySize, mobPitch, surface2Flags, msPattern, quality,
   byteStride, minLOD, pad2）；我们原来按 72 字节写、还把 flags 当 64 位，
   导致 sid>0 的条目全部错位。改用正确布局后 RTV 仍拒，说明这不是 RTV 的根因，
   但条目错位本身必须修（其它 DX 命令会读它）。
2. **命令名核对**（mksSandbox 名表，idx = cmdId-272）：1143 DXDefineContext、
   1145 DXBindContext、1187 DXDefineRenderTargetView、1161 DXSetRenderTargets、
   1176 DXClearRenderTargetView、1183 DXReadbackSubResource、1207 DXSetCOTable、
   **1277 DXBindShaderIface**（不是 SetShaderIface）。ID 全部正确。

### DC 设备命令分派（0x14054b860）解码

```
rdx = 0x140000000
byte = [rdx + cmd + 0x54bb0c]; off = [rdx + 4*byte + 0x54baf0]; jmp rdx+off
cmd 0,3 -> 0x14054b8f7   ; 建对象：cmp [rdi+0x150],0; jne err0xd; call 0x1404f2f20
cmd 1,2 -> 0x14054b92d   ; 要求 [rdi+0x150] != 0，否则 ebx=0xd 失败
cmd 4   -> 0x14054b955   ; 建 [rdi+0x158]（16 字节，xmm 拷贝自 body）
cmd 5   -> 0x14054b978
```
**推论**：`START_STOP_CONTEXT{1,1,1}` 恒拒 = `[rdi+0x150]` 仍为 0 = 我们发的
DC cmd 3 虽然报 ACCEPTED，但并没有真正建出上下文对象（或 payload 不对）。
下一步应解码 cmd 3 的 body 语义（0x14054b8de 分支会拿 body 第一个 dword 与
`[rdi+0x10]` 比较，不等则 err 0xe）。

### 队列楔死现象（重要）

**queue 0 连续失败约 4~5 次后会楔死**：之后所有 CB 的 status 一直是 0，kext 侧
表现为 "timeout waiting for completion"（约 0.4 s/次）。每次开机只有有限次
试错预算，实验设计要把最可能的组合排在最前。

### 下一步（按优先级）

1. 解码 DC cmd 3 的 body（0x14054b8de / 0x1404f2f20），让 `[ctx+0x150]` 真正建立；
   之后 START_STOP_CONTEXT 应能通过 → queue 1 拥有"当前上下文" → RTV/CLEAR 才有望通过。
2. 或从 mksSandbox 侧确认渲染命令的上下文来源（CB header.dxContext 在本轮被证明
   无效，除非 flags 有 DX 位，而 flags 又被 validator 限制为 0/1）。
3. RTV 通过后再接 CLEAR_RTV → READBACK_SUBRESOURCE → 品红/绿色校验。
### 17.1 START_STOP 的 id 扫描（01:08）

设备队列上 `{cmd 1, enable 1, id N}` 扫描结果：

| id | 结果 |
|---|---|
| 0 | **ACCEPTED** |
| 1 | refused |
| 2 | refused |
| 3 | refused |

即宿主只有一个可服务的设备上下文（id 0），我们想要的 "queue 1" 根本不存在。
DC 分派里 cmd 4 = 建 `[rdi+0x158]`（16 字节描述符，xmm 拷贝自 body）、cmd 5 使用它，
cmd 6/7 会重建 `[rdi+0x150]` —— **cmd 4/5 很可能就是"建队列/提交队列"**，
是让 cid-less 渲染命令有上下文可依附的关键。下一轮从解码 cmd 4/5 开始。

## 18. 10-25 凌晨：DC 命令表实证 + flags 判决实验（§17 的"队列 1 / cmd 4-5 建队列"路线作废）

> 本节由新会话（上下文重建后）完成。所有地址基于 `vmware-vmx.exe`
> （Tools 13.1.5 / WS 26.0，22,967,216 B，imagebase 0x140000000）。

### 18.1 DC 设备命令的真实分派（静态，可复核）

CB 里设备命令的处理函数在 **0x140087c10**（.pdata: begin 0x87c10 / end 0x87cd4），
命令循环在 0x140087cf0：

```
rdx = 4B 读出的 cmd id
cmp id, 6 ; jae -> 错误路径
16B/条的命令表 @ 0x140A4F6A0 = {u32 bodyBytes; u32 pad; u64 handler}
  cmd 0 -> body 0 B  handler 0x140017ec0
  cmd 1 -> body 8 B  handler 0x140087170    (START_STOP_CONTEXT)
  cmd 2 -> body 8 B  handler 0x140086e60    (PREEMPT)
  cmd 3 -> body 4 B  handler 0x140087090
  cmd 4 -> body 4 B  handler 0x140086c40
  cmd 5 -> body 4 B  handler 0x140086d70
推进量 = 4 (id) + bodyBytes
```

**⇒ §17 的"cmd 4 = 16 字节队列描述符（xmm 从 body 拷入）"不成立**：DC body 长度由
本表给定，cmd 3/4/5 都是 4 字节。§17 那段 0x14054b860 的分派属于另一条路径
（legacy / replayFifo），不是 CB 设备命令通道。

### 18.2 START_STOP_CONTEXT 硬编码只接受 context 0（静态 + 实测双证）

handler 0x140087170：

```
body = 8 B 读入 [rsp+0x20] = {u32 enable | u32 context}
cmp eax, 1        ; enable
ja  FAIL          ; enable > 1 -> 失败
cmp [rsp+0x24], 0 ; context
jne FAIL          ; ★ context != 0 一律失败
```

⇒ 任何 `START_STOP_CONTEXT{enable=1, context=N>0}` **恒** COMMAND_ERROR。
这解释了 10-23 的 id 扫描（只有 0 通过）——不是"队列 1 还没建出来"，而是
**本宿主根本不存在 context 1**。vmwgfx 侧同样是单上下文：`vmw_cmdbuf.c:1343`
只调用 `vmw_cmdbuf_startstop(man, 0, true)`，即 `SVGA_CB_CONTEXT_0`，另加设备上下文 0x3f。

### 18.3 cmd 3/4/5 = 每上下文 0/1 的状态记账，不是"建队列"

三个 handler 结构完全同构：

```
if (bit29 of dword [perVM+0xf4] == 0) -> FAIL      ; 能力门
read body dword = idx
if (idx > 1) -> FAIL                               ; 只允许 0 或 1
if (idx == 1 && bit29 == 0) -> FAIL
entry = perVM_ctxBase + idx*0x118
  cmd3: word[entry+0xae0] = 1
  cmd4: if (byte[entry+0xae0]) { if (dword[+0xad8]==0 && dword[+0xadc]==0)
                                     word[+0xae0] = 0;  else byte[+0xae1] = 1; }
  cmd5: qword[entry+0xad0] = 0; dword[entry+0xad8] = 0
```

- `perVM_ctxBase = 0x141775a60`（.per_vm 段），能力 dword = `0x141775b54` = base+0xf4。
- body 是**上下文索引 0/1**，不是 cid —— 我们一直在发 `{3, cid=1}`，实际含义是"索引 1"。
- ⇒ 这是 2 个上下文槽的激活/挂起记账，**没有任何"创建队列"的语义**。

### 18.4 ★ 判决实验：DX_CONTEXT flag 在本宿主被 header 层无条件拒绝

利用静态读出的校验器顺序（0x140087c10 内）：

```
if (header.length == 0) -> COMPLETED          ← 在 flags 检查之前
page-crossing check on header.ptr.pa + len
call 0x140607760
test dword header.flags, 0xFFFFFFFE  -> CB_HEADER_ERROR(4)
```

新增 `kFlagsProbe`（kext 内置，设备队列 0x3f，不楔死），同一 boot 实测（2026-10-25 00:43）：

| flags | len 0 | len 12（DC cmd1 START_STOP{1,0}） |
|---|---|---|
| 0 | COMPLETED | COMPLETED |
| 1 | COMPLETED | COMPLETED |
| 2 | **COMPLETED** | **STATUS 4 (CB_HEADER_ERROR)** |
| 3 | **COMPLETED** | **STATUS 4** |

**结论（实锤，不再有解释空间）**：
1. flags=2/3 的 CB **确实**走同一个提交函数（len==0 的捷径在同一个函数里先返回
   COMPLETED），所以不存在"DX flag 走了另一条通道"这回事。
2. 本宿主的 CB header validator 对所有队列**无条件**拒绝 `flags & 0xFFFFFFFE`。
   ⇒ **CB header 无法表达 dxContext，`SVGA_CB_FLAG_DX_CONTEXT` 这条 vmwgfx 正路在本机被封死。**
3. flags=0 也合法（测试用的是 `flags & 0xFFFFFFFE`，bit0 是唯一被允许的位）。

### 18.5 由此得到的矛盾与新的头号假设

矛盾：Windows 客体 vm3dmp 在同一宿主上 3D 正常，但若它真用 flags=3 提交 DX CB
就会被同一个 validator 拒。二者只能有一个成立。

静态新线索（关键）：`svga.enableDXContext` 是一个**可读的布尔配置项**
（xref 0x1404bfe3c：`lea rdx,str; mov ecx,1; call 0x1400a95d0` = 默认 TRUE 的读配置），
读完立刻检查 **perVM 能力 dword 的 bit28**：

```
0x1404bfe58  mov rax,[0x1404f31c0]      ; per-VM 结构指针
0x1404bfe5f  mov ecx,[rax+0xf4]         ; = 0x141775b54（与 18.3 的 bit29 同一个 dword）
0x1404bfe65  shr ecx,0x1c ; test cl,1   ; ★ bit28 = "DX context 可用"
```

同一个 dword 的 bit28 管 DX context、bit29 管 DC cmd 3/4/5。
**头号假设：bit28/bit29 就是 darwin 客体拿不到 DX 的宿主侧开关**，
而它们由 SVGA 能力协商（hwVersion / capLevel / capabilitiesMask / guestOS）在
power-on 时算出来。

**其他可能被忽略的宿主侧配置项**（vmware-vmx.exe 字符串表，均可写进 .vmx 试探）：

```
svga.enableDXContext      svga.enableDX / enableDX2 / enableDX3
svga.enableCmdBuffer2     svga.enableRegCommandBuffers
svga.allowHB3DOnPowerOn   svga.allowHB3DOnResume
svga.capabilitiesMask     svga.unsafeFeatures
svga.baseCapsLevel        svga.dump3dCaps / verbose3dCapsLogging
svga.enableCOTableCopy    svga.enableHPCmdQueue
```

### 18.6 下一步（按优先级）

1. **读运行期 perVM dword 0x141775b54 的真实值**（x64dbg 附 vmware-vmx.exe，或
   静态补丁把 bit28/bit29 测试改成恒真）——确认 bit28 是否为 0。这一步能直接判定
   "darwin 客体被关掉 DX context"是否成立。
2. 在 .vmx 里试 `svga.enableDXContext` / `svga.enableDX3` / `svga.allowHB3DOnPowerOn`
   + `svga.dump3dCaps=TRUE` + `svga.verbose3dCapsLogging=TRUE`（需要整机冷启动，
   不是客体 reboot），重跑 `kFlagsProbe`：若 flags=2 len12 变 COMPLETED，则找到总开关。
3. 若 bit28 恒置不上（产品策略写死）：退到"绕开 CB header"的路线 —— 用 cbContext
   寄存器位表达上下文，或改走 legacy FIFO（vm3dmp 之外的第二次 oracle 对照）。

### 18.7 本轮工程改动

- `kexts/framebuffer/vgpuFramebuffer.cpp`：
  `cbSubmitCtx()` 增加 `flags` 参数（默认 1，调用点语义不变）；新增 `kFlagsProbe`
  （设备队列上跑 flags 0/1/2/3 × len 0/12 的 8 点矩阵，不楔死、不渲染）；
  `kProbeEveryS` 置 0（4 s 一次的 DEFINE+BIND 循环会白耗队列错误预算）。
- 状态：`kFbStage=2 / kGbStage=4 / kDxTest=1 / kCbDump=1 / kFlagsProbe=1`（实验版）。
- 本轮同 boot 其余结果：DEFINE/BIND/SET_COTABLE/SET_SHADER_IFACE 依旧全 ACCEPTED，
  DEFINE_RTV 四种路由（q0/q1 × dx0/dx1）依旧全部 STATUS 3。

## 19. 10-25 凌晨（二）：★ 渲染命令被拒的最终根因 —— DX 管理器对象从未被创建

> 冷启动（`vmrun stop hard` → `start`，加了 `svga.dump3dCaps` / `verbose3dCapsLogging`）
> + x64dbg 附 `vmware-vmx.exe` 实测。ASLR 后模块基址每次不同（本次 0x7FF7369B0000），
> 下面给的是**静态 VA**（imagebase 0x140000000），运行时 = 基址 + (VA - 0x140000000)。

### 19.1 冷启动复测：结论稳定，packed-CB 也无效

- `kFlagsProbe` 与热启动完全一致（flags 2/3 + len>0 → STATUS 4）→ 不是缓存/脏状态问题。
- 新增 packed-CB 实验（`{BIND,RTV}` / `{BIND,SET_COTABLE,RTV}` / `{DEFINE,BIND,RTV}` 打包单 CB）
  全部 refused ⇒ **"当前上下文只在一个 CB 内有效"的假设也被否掉**。

### 19.2 DX 渲染命令 handler 的公共前置检查

`DEFINE_RENDER_TARGET_VIEW`(1187) 的 handler = **0x14055be80**（分派表条目
VA 0x140bde944，stride 24，{u32 id; u32 pad; u32 pad2; u64 handler@+12; u32 min; u32 max}）：

```
read 4 dwords body          ; {rtvId, sid, format, resourceDimension}
cmp eax,4 / jne FAIL
rbx = qword [0x1414930A0]   ; ★ DX 管理器对象
test rbx,rbx ; je FAIL      ; → 返回 0x101 (COMMAND_ERROR)
...
call 0x1404d1940(rbx, type=0, rtvId)         ; 读 RTVIEW cotable 条目
call 0x1404d19e0(rbx, type=0, rtvId, &entry, 0x20)  ; 写 32 字节 RTVIEW 条目
```

对 `0x1414930A0` 做全 .text rip 相对引用扫描：**0x14055a3xx–0x140560xxx 之间几十个
handler 全部引用它**（0x14055beb1 / 0x14055bf97 / 0x14055c076 …），即**所有 DX 渲染命令
的第一道门都是这颗指针非空**。DEFINE/BIND/SET_COTABLE 的 handler 不查它，所以那几条一直通过。

### 19.3 实测：这颗指针是 NULL

x64dbg attach 到在跑的 vmware-vmx.exe（PID 34264，基址 0x7FF7369B0000）读
`[0x1414930A0]` → **0x0000000000000000**。

⇒ 我们的每一条渲染命令（RTV / CLEAR / READBACK …）在进入任何真正校验之前就返回
COMMAND_ERROR。之前所有"body/尺寸/顺序/queue/flags 穷尽"的实验注定无效，因为它们
都没能跨过这道门。

### 19.4 谁应该创建它？——只有带 DX_CONTEXT flag 的 CB

全 .text 只有**一个**写入点：`0x14050a077  mov qword [0x1414930A0], rax`，其前因：

```
0x14050a010  call 0x140603b30            ; 命令批处理器
0x14050a015  rax = [rbp-0x30]            ; 返回的描述符（0x40 字节）
0x14050a04e  test r8b, 2                 ; ★ 描述符 bit1
0x14050a052  je   0x14050a20d            ; bit1 == 0 → 不创建
0x14050a058  cmp  dword [0x141493802], 0 ; 第二道门（文件初值 0x40903 ≠ 0，不是拦路者）
0x14050a05f  je   0x14050a222
0x14050a072  call 0x140a17350            ; 建对象
0x14050a077  mov  [0x1414930A0], rax     ; 存起来
```

描述符 bit1 = **`SVGA_CB_FLAG_DX_CONTEXT`**。也就是说：**DX 管理器是"第一次遇到带
DX_CONTEXT 标志的 CB 时"才惰性创建的**。

而 §18.4 已实锤：header validator 无条件拒绝 `flags & 0xFFFFFFFE`。
**两者合起来 ⇒ 在本 build 上，只要不修改宿主，"CX 上下文/渲染"这条链在构造上就不可能打通。**
（Windows 客体的 3D 必然是走了本路径之外的东西，或是这套 vGPU10 sandbox 只在特定
能力组合下才启用——这正是下一步要用 vm3dmp trace 澄清的点。）

### 19.5 破局方案：给 header validator 放开 bit1（1 字节补丁）

```
文件: C:/Program Files\VMware\VMware Workstation\x64\vmware-vmx.exe   (22,967,216 B)
目标: 0x140087c90  test dword ptr [rbp-0x59], 0xFFFFFFFE
机器码: F7 45 A7 FE FF FF FF   0F 85 34 01 00 00
              ^^ file offset 0x87293
补丁: 0x87293  FE → FC          → mask 变成 0xFFFFFFFC（放行 bit1，bit2+ 仍拒绝）
```

- 语义最小、可逆（改回 FC→FE 即可），且只影响"CB flags 是否放行 DX_CONTEXT"。
- 打完补丁后重跑 `kFlagsProbe`：flags=2 + len12 应变 COMPLETED；
  然后 DX 链（DEFINE→BIND→SET_COTABLE→RTV→CLEAR→READBACK）应能一路到像素。
- 备份：先 `copy vmware-vmx.exe vmware-vmx.exe.bak-dxflag`（需管理员）。
- 风险：影响本机所有 VM 的 VMX 进程（含 Windows 客体）；二进制被改后签名不符，
  但 VMware Workstation 运行时不校验自身签名（10-13 的 gate patch 已有先例）。

### 19.6 x64dbg 实测踩坑（下次直接用）

- 模块基址每次启动都变（ASLR）：先 `module_get_main` 取 base，再 `base + (VA - 0x140000000)`。
- `module_get_main` 在未 attach 状态不可用；VM 重启后 PID 会变，必须重新 `tasklist` 取 PID。
- 读完必须 `script_execute {"command":"detach"}`，否则 VM 停在调试态 / vmrun stop 会卡。
- `memory_read` 的返回是 `{address,data(hex),size}`，`data` 里可能带空格，需先去掉再 fromhex。

## 20. 10-25 下午：host 补丁生效（flags bit1 放行），但暴露出"两条 CB 提交路径"

### 20.1 补丁已打并验证（vmware-vmx.exe, 1 字节）

```
file offset 0x87293  FE → FC        ; test [rbp-0x59], 0xFFFFFFFE -> 0xFFFFFFFC
备份: vmware-vmx.exe.bak-dxflag（22,967,216 B，同日创建）
```

同一 boot 实测（2026-10-25 15:27）：

| flags | len 0 | len 12 |
|---|---|---|
| 0 / 1 | COMPLETED | COMPLETED |
| 2 / 3 | COMPLETED | **COMPLETED** ← 之前是 STATUS 4 |

⇒ header 层对 `DX_CONTEXT` 的封禁确实只由那一个 mask 决定，**补丁有效**。

### 20.2 但 RTV 仍然被拒，DX 管理器仍是 NULL

kext 侧把 RTV 扫成 4 个组合（cbContext × dxContext × flags，全部带 DX flag）：

```
DEFINE_RTV q1 dx1 flags3 -> refused
DEFINE_RTV q0 dx1 flags3 -> refused
DEFINE_RTV q1 dx1 flags2 -> refused
DEFINE_RTV q1 dx0 flags3 -> refused
```

x64dbg 复读 `[0x1414930A0]` → **仍然是 0x0**（PID 16600，base 0x7ff64bc60000）。
即：flags bit1 现在合法了，但**创建 DX 管理器的那段代码根本没被执行到**。

### 20.3 结论：CB 提交存在两条路径，我们的 CB 走的是"另一条"

`[0x1414930A0]` 唯一的创建点在 0x14050a000 附近，它的上下文是：

```
0x14050a010  call 0x140603b30           ; 处理一批命令
0x14050a015  rax = [rbp-0x30]           ; ← 返回的 "header" 指针
0x14050a019  xmm2 = [rax+0x10]          ; header+0x10 = SVGACBHeader.flags
0x14050a024  movd r8d, xmm2
0x14050a04e  test r8b, 2                ; flags & DX_CONTEXT
0x14050a052  je   0x14050a20d           ; 未置位 → 不创建
0x14050a077  mov [0x1414930A0], rax     ; 唯一创建点
```

也就是说"`flags & DX_CONTEXT` → 建管理器"这条规则确实存在，但**只在 0x14050a000 这条
提交路径上生效**；我们一直用的那条路径（header validator 0x140087c10 → 同样的
0x140603b30，但 r8d=1 而非 3）不建管理器。

### 20.4 头号假设：缺的是 **MOB 形态的 CB**（`SVGA_CB_FLAG_MOB` = bit2）

三条独立证据指向同一件事：

1. **vmwgfx**：`vmw_cmdbuf.c:893  cb_hdr->flags = SVGA_CB_FLAG_MOB;` —— 主路径是 MOB 形态，
   `header->handle` 在 MOB 模式下是 mob 句柄而不是物理地址；submit 仍旧写
   `REG_COMMAND_HIGH/LOW`（`vmw_cmdbuf.c:310-318`）。
2. **vm3dmp**：逆向早已发现 —— arg10 字节非 0 时 ptr 写成 `{mobid(+0x18), offset(+0x1c)}`
   且 **flags |= 4**（`cb-findings` §1）。
3. **本机行为**：我们全部 CB 都用 `ptr.pa`（物理地址）形态 + flags 无 bit2，正好落在
   "不建 DX 管理器"的那条路径上。

⇒ **下一步必须实现 MOB 形态 CB**：把 header+payload 放进一个 GB MOB（PT64_0 单页足够），
`ptr = {mobid, offset}`、`flags |= 4`，提交仍走 `REG_COMMAND_HIGH/LOW`。
补丁的 mask 目前是 0xFFFFFFFC（只放行 bit1），若 MOB 形态仍被 header 层拒，
则把 mask 再放宽到 0xFFFFFFF8（放行 bit2），同法 1 字节。

### 20.5 工具状态

- x64dbg 的 MCP 在本次 attach/detach 反复之后**再 attach 会失败**
  （`attach failed`，`debug_get_state` = stopped）。需要重启 x64dbg 才能继续动态调试；
  静态分析不受影响。
- 本轮 kext 改动：RTV 扫描改成 (cbContext, dxContext, flags) 四组、`rtvFlags` 一路传给
  CLEAR/READBACK、`kPackedTest=0`（打包实验退役，省 queue-0 错误预算）。

## 21. 10-25 下午（二）：MOB 形态 CB 落地 —— 换了一条路径，但仍未创建 DX 管理器

### 21.1 实现（按 vmwgfx 逐字对齐）

`vmw_cmdbuf.c:891-897`：CB **header 本身仍在普通 DMA 内存**（照旧用 PA 提交到
`REG_COMMAND_LOW/HIGH`），只有 **payload 放进 GB MOB**，ptr 变成
`{u32 mobid, u32 mobOffset}` 且 flags 加 `SVGA_CB_FLAG_MOB`(4)。

kext 实现：`cbMobSetup()` 用 legacy FIFO 定义 MOB 6（1 页、PT64_0），
`cbSubmitCtx()` 在 `_cbMobReady` 后把 payload 写进 MOB 页、`flags |= 4`。
实测 header dump 已确认：

```
cb[19] ctx1 dx1 len36 hdr: ... flags=0x00000007  length=0x24  ptr={6,0}   ; NO_IRQ|DX|MOB
cb[20] ctx0 dx1 len36 hdr: ... flags=0x00000007  ptr={6,0}
cb[21] ctx1 dx1 len36 hdr: ... flags=0x00000006  ptr={6,0}                ; 无 NO_IRQ
cb[22] ctx1 dx0 len36 hdr: ... flags=0x00000007  ptr={6,0}
```

**关键判据**：这 4 张 CB 的状态是 **STATUS 3（COMMAND_ERROR）而不是 4**
—— 但补丁后的 mask 是 0xFFFFFFFC，`7 & 0xFFFFFFFC = 4 ≠ 0`，走老校验器必然返回 4。
⇒ **MOB 形态确实走了另一条提交路径**（不动那道 flags 检查），假设前半段成立。

### 21.2 但 DX 管理器仍然是 NULL

x64dbg 复读 `[0x1414930A0]` = 0x0（PID 33804 / 33804→后来的实例，base 0x7ff64bc60000）。
其创建处的第二道门也已排除：

```
[0x141493802] = 0x00100000  (≠ 0)  → 不是拦路者
[0x1414937FC] = 0x00000001
[0x141775B54] = 0xfdffc3e2  (bit28/29 都是 1)
```

⇒ 0x14050a077 那句仍然没被执行。要么 0x14050a000 那段也不是我们这条 MOB 路径，
要么它前面还有别的门（例如需要 DX 上下文表里有有效记录）。

### 21.3 给宿主进程下断点会冻住整个 VM（重要教训）

在 `0x14050a04e / 0x14050a077 / 0x140087c90` 三个点下 INT3 之后：
`debug_pause` 返回 success=false、`debug_run` 反复回到 paused（RIP 在系统 DLL），
**客机网络栈直接死掉**（ping 不通），而且 `vmware-vmx.exe` 有 DACL 自我保护，
`vmrun stop/reset` 报 "not powered on"、`Stop-Process` 无权结束 —— 只能让用户在
VMware 窗口里手动 Power Off。

结论：**以后对 vmware-vmx.exe 不要用会真正暂停进程的断点**；要么用只记录不停下的
条件断点/日志断点，要么先把 handler 地址确定到"几乎肯定只被我们触发"的位置，
并准备好立即 `debug_run` 恢复。冻结超过几十秒，客机就起不来了。

### 21.4 kext 构建陷阱：weak `____chkstk_darwin` → kmutil Code=31

往 `dxReadbackTest()` 里内联加局部变量后，clang 给该函数生成了栈探针，
二进制里出现 **weak external `____chkstk_darwin`**，于是：

```
kmutil load -z -p .../vgpuFramebuffer.kext
  Error Domain=KMErrorDomain Code=31
  "Could not use 'com.vgpu.framebuffer' because: Has weak references but does not
   test for them.  Test for weak references with OSKextSymbolIsResolved()."
```

后果极其隐蔽：**install.sh 仍然打印 "OK: staged and present in the collection"**（它只
grep kext 名字，不看版本），auxKC 时间戳不变，**下一次启动跑的还是旧代码**——我为此
白跑了两轮（15:44 / 15:50 那两次的 "no payload MOB log" 就是这个原因）。

修法：把新增块抽成独立成员函数 `cbMobSetup()`，让 `dxReadbackTest()` 的栈帧回到阈值
以下，weak 引用消失。
**排查口诀**：`nm -m <.kext binary> | grep -i weak` 必须为空；不为空就别 install。
另：`ls -la /Library/KernelCollections/AuxiliaryKernelExtensions.kc` 的 mtime 是判断
"新代码到底进没进 auxKC" 的唯一可靠依据。

### 21.5 下一步

1. 重新找创建点：`0x14050a000` 那段多半不是 guest CB 路径。应改为从
   **"谁在 CB 处理时调用 0x140603b30 且拿到 header 指针"** 入手，或直接在
   `0x14050a077` 上**一次性断点 + 立即恢复**，看它到底会不会命中。
2. 或者反向：从 DX 渲染 handler 的失败点（0x14055be80 的 `test rbx,rbx`）往上找
   是否有别的"惰性创建"入口（例如 `SVGA_3D_CMD_DX_DEFINE_CONTEXT` 的 handler 0x14055b900
   是否本该同时建管理器）。
3. 每条实验都必须先过 §21.4 的两道自检（weak 符号为空 + auxKC mtime 更新）。

## 22. 10-26 夜：创建点的唯一性、它与调度器的连接方式，以及三条被证伪的假设

### 22.1 先把"唯一写入点"钉死（工具修正）

以前的 xref 扫描是**线性反汇编**，`.text` 里夹着跳转表，一乱就整段失步，所以只找到过一两条。
正确的做法是**按 `.pdata`（42880 个 `RUNTIME_FUNCTION`）逐函数反汇编**（`scratch/refscan3.py`），
这样每个函数的指令边界都是对的。重扫 `0x1414930A0` 得到 **98 条引用**：

- 97 条是读（`mov rbx/rdi/rsi/rbp,[0x1414930A0]`，紧接着 `test r?b,?b; je bail`）；
- **1 条是写**：`0x14050a077  mov qword ptr [rip+0xf89022], rax`；
- 另 1 条是开机时的 16 字节清零（`0x1404cc013  movdqu [rip+0xfc7085], xmm0`，属于 BSS 初始化）。

⇒ 结论不变（唯一写入点），但现在有了可信的扫描工具，后续任何"还有别的写入点吗"都不必再猜。

### 22.2 创建函数的真身与它的**唯一调用者**

`0x14050a077` 落在 pdata 片段 `0x140509fe8..0x14050a194` 里；它不是一个独立函数，而是大函数的一截
（其它片段：`0x140509f20`、`0x14050a194`、`0x14050a20d`、`0x14050a4c1`）。核心序列：

```
0x140509ff4  mov [0x1414939xx], esi          ; 存一个索引
0x14050a002  mov edx, 0x40                   ; 64 = sizeof(SVGACBHeader)
0x14050a010  call 0x140603b30                ; 取一个 CB header（第 5 参数是 out 指针）
0x14050a015  rax = [rbp-0x30]
0x14050a019  xmm2 = [rax+0x10]               ; {flags, length, ptr.lo, ptr.hi}
0x14050a024  movd r8d, xmm2                  ; r8d = flags
0x14050a04e  test r8b, 2                     ; ← DX_CONTEXT
0x14050a052  je 0x14050a20d
0x14050a058  cmp dword ptr [0x1414937FC], 0  ; 门 1，实测 = 1，放行
0x14050a065  movdqa xmm0, xmm1 ; psrldq 4    ; xmm1 = [rax+0x20] = {offset, dxContext, ...}
0x14050a06e  movd ecx, xmm0                  ; ecx = header->dxContext (+0x24)
0x14050a072  call 0x140a17350                ; lookup-or-create(dxContext)
0x14050a077  mov [0x1414930A0], rax          ; ← 唯一写入，可能写 NULL
0x14050a07e  test rax, rax ; je 0x14050a222  ; 拿不到就放弃这批命令
```

**调用者只有一个**：`0x1404c7a2b  call 0x140509f20`，位于函数 `0x1404c7950..0x1404c7a71`。它的判据是
**时间片/抢占**，而不是"CB 完成"：

```
0x1404c796a  call 0x140508750                 ; 取当前 tick 相关状态
0x1404c7979  cmp byte [dev+0x81d], 0 ; je out ; 设备处于某模式（非 0 才继续）
0x1404c7987  ... [dev+0xd10] 与若干全局计数器比较（"跑够久了没有"）
0x1404c7a14  ecx = [dev+0x9bc]                ; ← 关键字段
0x1404c7a21  test ecx, ecx ; je out
0x1404c7a25  cmp byte [dev+0x71], 0 ; jne out
0x1404c7a2b  call 0x140509f20                 ; ← 只有这里会创建 DX 设备
```

`dev = [0x1414931C0]`（SVGA 设备对象；`[dev+0xf4]` 是能力 dword，bit28 = DX context 可用）。

### 22.3 lookup 的失败路径（这是"写了 NULL"的现实可能）

`0x140a17350(cid)`：哈希表 `0x141492EB0`（大小 `[0x141492ED0]`），命中就直接返回；
**未命中 → `jmp 0x140a15b90`**，而它开头的分支允许失败：

```
0x140a15b90  ... call 0x140156e50(cid, &buf)   ; = 0x1404cde70
0x140a15bbd  test al, al
0x140a15bbf  jne  0x140a15bc8                  ; 成功才继续创建
0x140a15bc1  xor  eax, eax                     ; ← 失败返回 NULL
```

`0x1404cde70(cid, out)` 的判据逐条如下（全部可核对）：

1. `cid` 必须 <= 0xFFFF；
2. `0x1404ca6b0(type=5, cid, 1, 0, &buf)` —— 查 **DXCONTEXT cotable（type 5，8 字节条目）**；
3. `entry[0]` **必须等于 cid**（不是 mobid！`cmp edi, ebx; jne FAIL`）；
4. 用 `entry[1]`（条目第二 dword）当 mobid，`0x1404c9fb0(mobid, off=0, out, 0x2000)` 读回 8 KiB。

⇒ 所以 `[0x1414930A0]` 为 NULL 有**两种**可能，而只读全局是分不出来的：
(a) 那段代码根本没执行；**(b) 执行了，但 `0x1404cde70` 失败，于是写了 NULL。**

### 22.4 DX handler 的 null 门（复核，确认无误）

`0x14055be80`（DEFINE_RTV）：

```
0x14055be99  call 0x140508bb0(&arg, 4, &arg)   ; 取 rtvId
0x14055bea8  cmp eax, 4 ; jne fail
0x14055beb1  mov rbx, [0x1414930A0]
0x14055beb8  test rbx, rbx ; je fail            ; ← 就是这个门
0x14055bec3  call 0x140a1e9c0 / 0x1404d1940     ; RTV cotable（type 0）
```

### 22.5 客机实测：**第一个 DX 错误就会楔死整条命令引擎**

本轮把完整体验日志（`log show --start … | grep vgpu-fb`）逐条对过，得到一个此前判断错的事实：

```
cb[19] ctx1 dx1 flags7 (DEFINE_RTV)  -> STATUS 3 (COMMAND_ERROR, errorOffset 0)
cb[20] ctx0 dx1 flags7               -> 超时（status 一直 0）
cb[21] ctx1 dx1 flags6               -> 超时
cb[22] ctx1 dx0 flags7               -> 超时
cb[23] ctx0 dx0 flags5 (CLEAR)       -> 超时
之后的 DEFINE_GB_CONTEXT             -> refused
```

**不是"队列 0 失败 4~5 次才楔死"，而是首个 DX 命令返回 COMMAND_ERROR 之后，后续所有 CB 都不再被处理。**
⇒ 那条"四组扫描 matrix"实际上只有第一组是真数据，后面三组都是超时噪声。**以后一条 DX 错误 = 一次重启的预算。**

同一轮里被**接受**的（全部 `dxContext=0`，且都不带 DX 渲染语义）：

| 命令 | 结果 |
|---|---|
| DC cmd 3 `{body 1}` | ACCEPTED（`word[slot1+0xae0]=1`） |
| START_STOP `{1,1,0}` | ACCEPTED；`{1,1,1..3}` refused（context 只允许 0） |
| DX_DEFINE_CONTEXT `{cid 1}` (1143) | ACCEPTED，otable 条目 `{1,3}` |
| DX_BIND_CONTEXT `{cid 1, mob 3, 0}` (1145) | ACCEPTED |
| DX_SET_COTABLE `{cid1, mob5, type0, 4096}` (1207) | ACCEPTED |
| DX_SET_SHADER_IFACE | ACCEPTED |
| DEFINE_RTV (1187) | **COMMAND_ERROR** |

### 22.6 本轮证伪的三条假设

1. **`.vmx` 的 DX 开关是门。❌** 加了 `svga.enableDXContext/enableDX/DX2/DX3/enableCmdBuffer2`
   （全部被接受，能正常开机），`SVGA3dClamp` 三个值一字未变
   （`BC67Level 9(9,9)`、`BaseCapsLevel 9(9,9)`、`ClampLevel` renderer 7 / guest 10(7,10)），
   DXCONTEXT devcap 仍为 1。**结论：这些键不是拦路者，别再当候选。**
   顺带：`svga.loghw`、`svga.forceTraces` 在本机**没有任何额外输出**（应是 debug 构建才生效）。
2. **空 DX CB 的"完成"会惰性创建 DX 设备。❌** 新增 PRIME 探测（空载荷 + `flags=3` + `dxContext=1`，
   分别发在 ctx1 与 ctx63）：CB 本身 COMPLETED，但随后 RTV 仍然 COMMAND_ERROR。
   ⇒ 创建不在"普通完成"路径上。
3. **`dxContext≠0` 只是命令层的事。❌** 头部校验层也管它，而且**跟 cbContext 绑定**：
   同样 `flags=3 dx=1`，`ctx1` 过校验（走到 STATUS 3），`ctx63` 直接 **STATUS 4 (CB_HEADER_ERROR)**。
   ⇒ 以后所有带 dx 的 CB 必须发在 ctx1（vm3dmp 的 1/2），别发 0x3f。

### 22.7 下一步（按性价比排序）

1. **抢占路径**：既然 `0x140509f20` 只被"时间片/抢占"那条链调用，那就主动制造一次抢占 ——
   发一个带 DX_CONTEXT 的 CB（载荷放**不会失败**的命令），紧接着发 `SVGA_CMD_PREEMPT`（cmd 2）
   或 DC 的 PREEMPT（cmd 2/4），观察 `[0x1414930A0]`。这是唯一还没试过的入口。
2. **反过来验证 (b) 假设**：让 kext 把 DXCONTEXT cotable 的条目改成 `{cid, mobid}` 的**另一种排列**
   （`{1,3}` vs `{3,1}` 之外，还要试 `entry[1]` 指到一个**真能读**的 8 KiB MOB），
   确认 `0x1404cde70` 的第 3、4 步能不能过。注意 `0x1404c9fb0` 要 0x2000 字节。
3. 若要继续动态调试：**不要用 INT3**。宁可把 `0x14050a04e` 的
   `41 f6 c0 02`（`test r8b,2`）就地改成 `41 80 c8 02`（`or r8b,2`，**同长度 2 字节**，
   file offset 对应 `0x14050a050`/`0x14050a051`），让每个 CB 都当 DX CB 处理，然后只看客机日志判断。

## 23. 10-27：抢占 tick 的入口结构，以及"拖长 CB"也被证伪

### 23.1 那条链是**周期性 tick**，不是一个可订阅的事件

用 `tools/rev/vmx-callers.py` 一直往上追，链条只有一条，而且顶部没有直接调用者：

```
0x1404c5ee0   ← 0 callers（tick 回调/定时器，函数体里有 inc dword [0xf2d48c] 计数）
  └─ `[dev+0xb4] != 0` 才继续
  └─ call 0x1404c7950                        ; 抢占判定
        ├─ `[dev+0x81d] != 0`
        ├─ 若干计数器比较（"跑够久了没有"，[dev+0xd10] 等）
        ├─ `[dev+0x9bc] != 0`
        ├─ `[dev+0x71] == 0`
        └─ call 0x140509f20                  ; ← 唯一会写 [0x1414930A0] 的代码在这
```

另一处同款判据（`0x1404d7498/0x1404d74b0`）也是同一组条件，说明这是**"宿主正在执行客人 CB、且还没让出去"**
的通用让出（yield/preempt）判定，不是 DX 专属。

`[dev+0x9bc]` 的位布局线索：`0x14028048b  test dword ptr [rbp+0x9bc], 0x3f00` ——
**掩码 0x3f00 = bits 8..13，正好 6 位，就是 cbContext（0..63）的宽度**。所以这个字段只在
「某个 cbContext 的命令正被宿主执行」时非零。同一函数的写法
`eax = (x & 0xfe0f00e4) | 0xf00e4; mov [rbp+0x9bc], eax` 会把 bits 8..13 清掉 —— 那是**收尾/复位**路径。

### 23.2 新实验：把宿主按在 in-flight 里，等 tick 抓到（❌ 证伪）

推论：既然 tick 只在 CB 执行期间才有机会命中，而我们的 CB 只有几微秒，就把它**拖长**。
做法（kext 里的 `SPIN` 段）：用一条**已知被接受且幂等**的命令做填充 ——
`DX_SET_COTABLE`（1207）重复 170 次（6 dwords/条 = 4080 B，刚好卡在 4096 B 的载荷 MOB 之内），
每张 CB 带回 `flags=3 + dxContext=cid`，连续发 40 张。

实测（本次 boot）：

```
vgpu-fb: dx: SPIN 40 long DX CBs (q1 dx1) -> all COMPLETED
vgpu-fb: dx: PRIME (q1 dx1 flags3 len0)     -> COMPLETED
vgpu-fb: dx: DEFINE_RTV q1 dx1 flags3       -> refused      (COMMAND_ERROR)
```

40 张 × 1020 dwords = **40800 条命令都在 DX_CONTEXT 语义下跑过**，管理器依然没建起来。
⇒ 「把 DX CB 拖长让 tick 命中」也不成立。结合 §22.6 的结论，可以确认：

> **只要走 `REG_COMMAND_LOW/HIGH` 的单张 CB 提交，不管 flags/dxContext/MOB/长度怎么组合，
> 宿主都不会创建 DX 管理器。** 剩下能改的只有 `[dev+0xb4]`、`[dev+0x81d]`、`[dev+0x71]`
> 这三个"外部状态"，它们不由 CB 内容决定。

### 23.3 顺手修掉的一个真实越界

`cbSubmitCtx()` 原来只按 `kCbDataBytes`（64 KiB，DMA 形态的上限）检查载荷，
但 MOB 形态的载荷区 `_cbMobMem` 只有 **1 页（4096 B）**。任何 >4 KiB 的载荷都会
`memcpy` 越过一个物理连续的 1 页分配。已加上 `mobMode && byteCount > kGbPageBytes` 的硬检查
（正是写 SPIN 这个"大载荷"调用者时才暴露的）。

### 23.4 现在的判断：还剩下的两条路

1. **HP command queue / `SVGA_CAP2_HP_CMD_QUEUE`（caps2 bit7，本机已是 1）**
   —— 如果 DX CB 必须经由"队列 + 环"提交（而不是单张 CB 触发寄存器），
   那么 `[dev+0x9bc]` 之类的状态由队列驱动，逻辑才自洽。这条路要先解出队列的寄存器/命令接口。
2. **从 vm3dmp.sys 里抠"第一帧之前它还发了什么"**
   —— 它在**同一台宿主**上是能跑 DX 的，所以它的初始化序列必然足够。
   重点不是我们已对齐的 DEFINE_CONTEXT/BIND/COTABLE，而是**我们没发的那些**（例如全部 cotable 类型、
   以及任何与"上下文激活"相关的命令）。这是纯静态工作，不需要重启预算。

## 25. 10-29：DX 管理器缺席的判决点收窄到 `dxContext != 0` 这一条路径

本轮把"模板之所以从未成功"的所有混淆变量逐层剥掉，最后只剩一个。

### 25.1 先修掉的三个真实缺陷（都不是宿主的问题）

1. **模板从未执行**（10-28 遗留）：`gbc: DEFINE_GB_CONTEXT refused` 分支是裸 `return;`，
   模板块在运行期不可达、在编译期也被优化掉。改成 `goto gbFailed;` + 标签页。
   `build.sh` 的 reachability marker 机制就是为这类事故加的，这次它生效了。
2. **`tpl[80]` 写进 `tpl[64]`**：模板要发 8 + 12×6 = 80 dwords，数组只有 64。
   越界覆盖了同帧的 `sync[3]` / `okTpl` / 返回地址以上区域。日志照样打印"80 dwords"
   （`n` 已经数完了），而宿主看到的部分内容来自相邻栈槽。现在数组尺寸由同一段算式推导，
   并在填充后断言 `n == kTplDwords`。
3. **`cbSubmitCtx()` 三个静默 `return false`**：`_cbHeaderVirt == nullptr`、
   `_pci == nullptr`、`byteCount > kCbDataBytes` 三条路径都不打日志，
   调用点只能看到 "refused"。全部补上日志，并在 dump 里打一行入口状态
   （`mobMode/ready/virt/hdr/pci/dataVirt`）。**"refused 但没有任何后续日志"这个中间态
   从此不存在**，这一点是本节后面所有推理的基础。

### 25.2 flags 的真实规则（复核 §18.4，结论不变）

```
test dword header.flags, 0xFFFFFFFE  -> CB_HEADER_ERROR(4)
```

除 bit0 外任何位非零即拒。因此：

- `SVGA_CB_FLAG_DX_CONTEXT`(bit1) **不能**出现在 header 里 → 已改成在 `cbSubmitCtx()`
  内集中剥离（`kCbKeepDxContext` 开关控制），调用方传 2/3 也不会再踩。
- `SVGA_CB_FLAG_MOB`(bit2) 同样不能出现 → **MOB 形态在本宿主根本无法表达**。
  之前那条"vm3dmp 用 MOB 形态"的注释是从 Linux `vmw_cmdbuf.c` 推断的，
  不是本宿主行为；vm3dmp 在**这台**宿主上必然走 PA 形态（它渲染正常，而这是唯一的校验器）。
- `len == 0` 在 flags 检查**之前**返回 COMPLETED（§18.4 表格已证），
  所以"空 CB 能过"不代表 header 合法。

### 25.3 三次启动的判决矩阵（每次一个变量）

| boot | 形态 | flags 实测 | dxContext | 结果 |
|---|---|---|---|---|
| 20:04 | MOB，`tpl[64]` 越界 | 6 | 0 | STATUS 4（0 ms 返回）|
| 20:17 | MOB | 5 = NO_IRQ\|MOB | 1 | **STATUS 4** |
| 20:22 | PA | 1 = NO_IRQ | 1 | **STATUS 4** |
| 20:28 | PA | 1 = NO_IRQ | 1 | **STATUS 4** |

三次 STATUS 4 的共同点只有一个：**`dxContext != 0`**。MOB/PA、bit1 有无、载荷内容、
载荷长度全都变了，拒绝方式一模一样（`errorOffset 0`，立即返回，不轮询）。

对照 §18.4 的判定实验表：`ctx63` + `flags∈{0,1}` + `dxContext=0` + `len=12` 是 **COMPLETED**。
⇒ **`dxContext=0` 从未被 header 拒过，`dxContext≠0` 从未被接受过。**

### 25.4 机制解释（与 §22.2 的反汇编一致）

```
0x14050a04e  test r8b, 2          ; bit1 决定分支
0x14050a052  je 0x14050a20d       ; bit1 清 → 普通命令流分支
0x14050a058  cmp [0x1414937FC], 0
0x14050a072  call 0x140a17350     ; lookup-or-create(dxContext)
0x14050a077  mov [0x1414930A0], rax
0x14050a07e  test rax, rax ; je → 放弃整批
```

把 bit1 剥掉之后，这一批走的是 `0x14050a20d`（普通命令流）。**但 header 层仍然以 STATUS 4 拒绝**，
说明还有一条**先于** `0x14050a04e` 的检查读到了 `header->dxContext`，并且对 `dxContext != 0`
且**找不到对应 DX 设备**的情形直接判 header 错。这与 §22.2 的 `lookup-or-create` 是同一件事，
只是它在更早的地方被调用了。

结论：**`dxContext` 不是"提示字段"，而是要求一个已存在的 DX 设备对象。**
没有设备 → 任何非零值都是 header 错误。这解释了 §22 的"被接受清单全是 `dxContext=0`"。

### 25.5 拆分诊断踩到的坑（记下来，别再犯）

`STATUS 4` 之后我把同一条 CB 拆成三条单命令 CB 重发，想定位是哪条命令被拒。结果三条全部
**414 ms 超时**、`status` 一直为 0 —— 因为 §22.1 的规律在这里又生效了：

> **首次 header/命令错误之后，后续所有 CB 都不再被处理。**

所以"在失败之后再做拆分探测"是无效实验，白花一次启动预算。规则更新：
**任何诊断性重发都必须排在"被判定的那一笔"之前，或者单独一次启动。**

### 25.6 下一步（唯一没有被试过、且不与已实锤规则冲突的组合）

已实锤：`dxContext=0` 能过 header；`dxContext≠0` 必挂（没有 DX 设备时）。

⇒ 下一轮实验：**`dxContext=0` + 不设 bit1 + PA 形态**，载荷用 §22 的"已被接受"命令序列
（`DX_DEFINE_CONTEXT` → `DX_BIND_CONTEXT` → `DX_SET_COTABLE`），**逐条单发、每条自带日志**，
确认它们在"干净引擎 + 目前唯一合法的 header 形态"下能否全部 ACCEPTED。

- 若全部 ACCEPTED：说明命令层是通的，DX 管理器缺席完全由宿主侧
  `[dev+0x9bc]`/`[dev+0x71]`/`[dev+0x81d]` 决定 → 转 §23.4 的两条路（HP queue / vm3dmp 静态）。
- 若某条 COMMAND_ERROR：得到第一个**命令级**（而非 header 级）的真实错误码，那是新的信息。

`kCbKeepDxContext`、`kCbMobMode`、模板的 `dxContext` 参数都已是独立开关，
下一轮只需改这三个值 + 把拆分诊断移到批量提交**之前**。

## 26. 10-29 深夜：CB 命令通道打通，卡点精确到 `DEFINE_RTV` 一条

§25.6 的实验做完了，结果是本项目自 §20 以来最干净的一组数据。

### 26.1 两次启动的日志（可复现）

```
20:35  SPLIT DX_DEFINE_CONTEXT {cid 1}            -> ACCEPTED     cb[9] COMPLETED
       SPLIT DX_BIND_CONTEXT {cid 1 mob 6}        -> ACCEPTED
       SPLIT DX_SET_COTABLE {cid 1 mob 5 type 0}  -> ACCEPTED
       TEMPLATE DEFINE+BIND+12 COTABLES (72 dw)   -> submitted
       TEMPLATE sync CB (q0 flags1)               -> COMPLETED
       TEMPLATE SET_SHADER_IFACE (q1)             -> STATUS 3      ← 首次命令级错误
       TEMPLATE DEFINE_RTV                        -> timeout       （队列已封）

20:40  （同上三条 ACCEPTED + 批量 submitted + sync COMPLETED）
       TEMPLATE DEFINE_RTV {rtv 1, sid 3}         -> STATUS 3      ← 复现
```

两次都通过了 header 校验，`cb[9]` 这次是 `len 12, flags 1, ptr=PA, dxContext=0`，**COMPLETED**。

### 26.2 由此确立的三条事实

1. **`dxContext = 0` + PA 形态 + flags=NO_IRQ 是本宿主唯一合法的 CB 形态。**
   这一形态下 `DX_DEFINE_CONTEXT` / `DX_BIND_CONTEXT` / `DX_SET_COTABLE` 三条**全部 ACCEPTED**，
   且 13 条命令打包成 72 dwords 的一张 CB 也 **submitted**（+ 后续 sync CB COMPLETED）。
   ⇒ **CB 命令通道（header + 解析 + DX 命令分派）已经打通。** 这是自 §22 以来第一次。
2. **`dxContext != 0` 一律 STATUS 4**，与 MOB/PA、bit1、载荷无关（§25.3 四组数据）。
   ⇒ `dxContext` 字段要求一个已存在的 DX 设备对象，而该对象从未被创建。
3. **`DEFINE_RTV` 是本宿主上唯一稳定失败的 DX 命令**，且失败形态是 **STATUS 3
   (COMMAND_ERROR)**，`errorOffset 0`。20:35 与 20:40 两次独立启动复现，
   与 §22.7 表格（`DEFINE_RTV (1187) → COMMAND_ERROR`）一致。

### 26.3 `SET_SHADER_IFACE` 曾被误判

20:35 的 boot 里 `SET_SHADER_IFACE` 回 STATUS 3，看上去像"它坏了导致 RTV 超时"。
20:40 把 RTV 提到它前面之后，**`SET_SHADER_IFACE` 根本没机会执行，RTV 自己就 STATUS 3**。
⇒ 之前那次是"两条命令都错"，不是"前者害了后者"。顺带确认 §22.1 的规律依然成立：
**一条 COMMAND_ERROR 之后，同一队列上的后续 CB 只剩超时。**

（`SET_SHADER_IFACE` 由 `[device+0x18ac]` 能力位门控，本机 3D caps 全 0，
所以它在本机很可能本身就是 unsupported。已把它移到 RTV 之后，作为"跑分项"而非"门槛项"。）

### 26.4 卡点的性质变了

现在的判断不再是"CB 通道不通"，而是"**`DEFINE_RTV` 需要的前置状态没建立**"。
按 §8 反汇编，RTV 的 handler 链路是：

```
0x1404CFD60(cid): cid≤0xFFFF + otable(type5) 条目存在 + entry.cid == cid
0x1404CA530(..., 0x2000)  MOB 校验
```

而 §22.7 的第三条怀疑是 `0x1414930A0`（DX 管理器）为空——97 个渲染命令都在 `je bail` 上退出，
`DEFINE_RTV` 正是其中之一。而该对象唯一的创建点在 `0x14050a077`，
被 `[dev+0x9bc] != 0 && [dev+0x71] == 0` 门控，且**只有时间片/抢占那条链会调用**。

⇒ **`DEFINE_RTV` 的 COMMAND_ERROR 与 `dxContext` 的 STATUS 4 是同一个根因的两个症状：
宿主侧的 DX 设备从未被创建。** 而创建它需要宿主内部状态，不是 guest 能通过 CB 内容触发的。

### 26.5 下一步（优先级已重排）

1. **HP command queue（§23.4 第 1 条）现在是最优先。**
   既然单张 CB 路径已被证明"能跑命令但触发不了 DX 设备创建"，而 HP queue 的提交路径不同
   （`SVGA_CAP2_HP_CMD_QUEUE` = caps2 bit7 = 1 已置），它是唯一还没试过的"入口"。
   需要先解出：queue 的 MMIO 寄存器组、环描述符格式、doorbell 语义。
   起点：`vmware-vmx.exe` 里搜 `SVGA_REG_QUEUE` 相关字符串与 `0x14050xxxx` 段内对
   `[dev+0x9bc]` 的写入点（98 条引用里除去 97 条读，剩下那条写就是 `0x14050a077`；
   但 `[dev+0x9bc]` 自己的写入点还没扫过）。
2. **扫 `[dev+0x9bc]` / `[dev+0x71]` / `[dev+0x81d]` 的全部写入点**（纯静态，不占重启预算）。
   如果 `[dev+0x9bc]` 只在某个我们没走过的初始化函数里被写成非零，那个函数就是答案。
3. 暂时不动的：`DEFINE_RTV` 的 desc 各种排列（§22.7 已穷尽 arraySize / cid / queue）。

### 26.6 工程侧的两条规则（本轮付了学费）

- **诊断性提交必须排在被判定那一笔之前。** 20:28 那次在 STATUS 4 之后才拆分重发，
  三条全部 414 ms 超时，等于白烧一次启动（§25.5）。
- **`build.sh` 的 marker 校验要覆盖"这次实验的判定行"。** 本轮一开始把 marker 写成
  `TEMPLATE setup`（不存在的字符串），构建直接 FATAL，反而省了一次误烧；
  但 marker 列表本身也要跟着实验走，别让它变成"永远校验上一轮的实验"。

## 27. 10-29 静态突破：`[dev+0x9bc]` 是"上下文活跃"的缓存，不是开关

§26.5 的第 2 条做完了（纯静态，零重启成本）。结论改变了整个问题的形态。

### 27.1 扫描方法（以及第一个坑）

写 `disasm/scan_devfields.py`：capstone 线性扫 `.text`，只保留 `[reg+disp]` 形态、
`disp ∈ {0x9bc, 0x71, 0x81d, 0xb4}` 的指令，按助记符分成读/写。

**必须过滤 `rbp`/`rsp` base。** 第一版没滤，报出"`[dev+0x9bc]` 在 `0x1402804ee` 有写入"，
实际那条指令是 `mov [rbp+0x9bc], eax` —— 一个**完全无关函数的栈槽**。
同一处还有 `test [rbp+0x9bc], 0x3f00`，`0x3f00` 正好像 6 位 cbContext 掩码，
差点被当成"找到了"，实际是那个栈结构自己的标志位。**基址寄存器先看，再看 disp。**

### 27.2 `[dev+0x9bc]` 全项目只有 2 个写入点，且都在同一个函数里

```
W 0x1404125e3  mov [rdx+0x9bc], eax     ; eax = 0
W 0x1404125ef  mov [rdx+0x9bc], eax     ; eax = 1
读 0x1404c7a14                          ; ← §22.2 那个 DX 创建链的条件
```

函数体（`0x1404125a0`，`rdx = [0x1414931C0]` = SVGA 设备对象）：

```
0x1404125a9  movzx r8d, byte [rdx+0xbf8]      ; 看槽1 的 "active" 字节
0x1404125b1  lea   rcx,   [rdx+0xad8]         ; 槽0 基址，步长 0x118
0x1404125b8  test  r8b, r8b ; je 0x1404125d1
0x1404125bd  cmp   byte [rdx+0xae0], 0
0x1404125c4  jne   0x1404125cc
0x1404125c6  cmp   rax, 1 ; jne 0x1404125d1
0x1404125cc  cmp   dword [rcx], 0 ; ja 0x1404125ea   ; ★ [slot+0] > 0 → 返回 1
0x1404125d1  inc   rax ; add rcx,0x118 ; cmp rax,2 ; jl 0x1404125b8
0x1404125e1  xor   eax, eax
0x1404125e3  mov   [rdx+0x9bc], eax            ; 无活跃槽 → 0
0x1404125ea  mov   eax, 1
0x1404125ef  mov   [rdx+0x9bc], eax            ; 有活跃槽 → 1
```

⇒ **`[dev+0x9bc]` 不是初始化时置位的开关，而是一个每次重算的缓存：
"两个 DC 上下文槽里有没有一个是活跃的"。** 判据是 **dword `[dev+0xad8]`（槽0）
或 `[dev+0xbf0]`（槽1）> 0**，另外槽1 还要求 `[dev+0xbf8] != 0`。

这解释了为什么此前所有"改 CB 内容"的努力都不可能奏效：`[dev+0x9bc]` 根本不由 CB 内容决定，
而由**DC 上下文的活跃计数**决定。而 DX 创建链 `0x1404c7a14` 读的正是它。

### 27.3 上下文槽的零化函数 = `stopAllContexts`

`0x140509a0d` 同时写 `[dev+0xad8]`、`[dev+0xae0]`、`[dev+0xbf0]`、`[dev+0xbf8]`：

```
0x1405098ca  mov  rdi, [dev]
0x1405098d1  movzx ebx, cl
0x1405098d4  mov  byte [rdi+0x81d], cl      ; ★ [dev+0x81d] 直接来自入参
0x1405098da  test cl, cl ; je 0x140509906   ; cl == 0 → 走关闭分支
0x1405098f1  jae 0x140509906                ; [dev+0xb0] >= 0x90000003 也走关闭
0x1405098f5  mov  byte [rdi+0x81c], 1
...
0x140509a06  mov  qword [rdi+0xad0], rsi    ; esi = 0
0x140509a0d  mov  dword [rdi+0xad8], esi    ; 槽0 活跃计数 = 0
0x140509a13  mov  word  [rdi+0xae0], si
0x140509a1a  mov  qword [rdi+0xbe8], rsi
0x140509a21  mov  dword [rdi+0xbf0], esi    ; 槽1 活跃计数 = 0
0x140509a27  mov  word  [rdi+0xbf8], si
0x140509a2e  call 0x1404125a0               ; 重算 [dev+0x9bc] → 0
```

所以 `[dev+0x81d]` 与 `[dev+0x9bc]` 是**同一条生命周期上的两个字段**：前者表示"上下文子系统处于
启动态"，后者表示"至少有一个槽活跃"。DX 创建链要求 `[dev+0x81d] != 0 && [dev+0x9bc] != 0`。

### 27.4 §18.3 的旧结论需要修正

§18.3 说 DC cmd 3/4/5 "是 2 个上下文槽的激活/挂起记账，没有任何'创建队列'的语义"。
**前半句对，后半句现在要收窄**：既然 `[dev+0x9bc]` 由槽活跃度决定，而槽活跃度是 DX 创建链的
必要条件，那么**让槽真正活跃起来就是创建 DX 设备的先决条件**。我们此前发的
`START_STOP_CONTEXT{enable=1, context=0}` 只写了 `[entry+0xae0]`（byte 标志），
**没有让 `[entry+0]` 这个 dword 计数变非零** —— 这就是 `[dev+0x9bc]` 一直是 0 的原因。

### 27.5 下一轮实验（一次启动验证一件事）

目标：让 `dword [dev+0xad8] > 0`。已定位的候选写入点（需在宿主里确认哪一个是"启动"语义）：

| 地址 | 指令 | 备注 |
|---|---|---|
| `0x1403f7091` | `mov word [rbx+0xad8], r9w` | 写低 16 位，可能不够（判据是 dword）|
| `0x1403fb3b0` | `mov qword [rcx+0xad8], rax` | 整 8 字节，最像"初始化/启动"|
| `0x140509a0d` | `mov dword [rdi+0xad8], esi` | 已知是清零路径 |

实验设计：在 kext 里发 **`SVGA_DC_CMD_START_STOP_CONTEXT{enable=1, context=0}`，
然后立刻读宿主那个 dword**（用 backdoor/DMA 访问 `0xad8`）——但那需要宿主地址，
guest 拿不到。**替代方案（零成本）**：让 `0x1404125a0` 的返回值直接可见 ——
它写 `[dev+0x9bc]`，而这个字段本身没有 guest 可见的侧信道。

⇒ 现实做法：**先扫 `0x1403fb3b0` 所在函数**，确认它是"启动槽"还是别的记账，
再决定要不要为它设计一次 CB 侧实验。这一步仍然是纯静态的，先做完再花重启预算。

---

## 28. 纠正 §27：`[dev+0xad8]` 不是"活跃计数"，且它在所有路径上恒为 0

§27.5 留下的第一件事是"扫 `0x1403fb3b0` 所在函数"。扫完的结果是 **§27 的核心结论必须整体作废**，
所以这一节先做纠正，再给新的定向。

### 28.1 `[dev+0xad8]` 是 SVGA 寄存器影子槽，不是计数器

`0x1403f6fd0` 是"写 SVGA 寄存器"的处理函数（`edx==1` 走写路径，`r10d = 寄存器索引`，
要求 `< 0x20`）。当 `r10 ∈ [0x11, 0x1e]` 时用跳转表分派：

```
0x1403f700e  lea  rdi, [rip-0x3f7015]        ; → rdi = IMAGE_BASE
0x1403f703f  mov  ecx, [rdi+rax*4+0x3f749c]  ; rax = reg-0x11
0x1403f7046  add  rcx, rdi
0x1403f7049  jmp  rcx
```

`disasm/svga_reg_table.py` 解出整张表，其中 **`SVGA_REG_0x12` → `0x1403f706b`**，该分支：

```
0x1403f7075  sub  ecx, 2 ; je 0x1403f709f    ; (edx>2 路径写 [rbx+0xacc])
0x1403f7088  mov  eax, 0xff0f
0x1403f708d  and  r9w, ax
0x1403f7091  mov  word [rbx+0xad8], r9w      ; ← 只写低 16 位，且掩码 0xff0f
```

`edx` 不是 0/2/3 时直接撞断言（`0x1403f744b: call __assert; int3`，行号 0x17c0）。
也就是说 `[dev+0xad8]` 是**寄存器 0x12 的影子**，写入时被 `0xff0f` 掩码裁到只剩
高字节的 `0xff` 和低半字节 `0xf` —— **它连任意值都存不住**。

### 28.2 `0x1403fb3b0` 写的是 0（"最像初始化"的猜测被证伪）

```
0x1403fb3a4  xorps xmm0, xmm0
0x1403fb3a7  xor   eax, eax
0x1403fb3a9  movups [rcx+0xac8], xmm0
0x1403fb3b0  mov   qword [rcx+0xad8], rax    ; rax == 0 ⇒ 写 0，且一次清 8 字节
```

这段是把 `[rcx+0xa88 .. 0xae0]` 一整片**重置为默认值**（后面紧跟一串立即数
`0x79491140`、`0xcb10141`、`0x1e1`、`0x300`…，明显是复位默认配置）。
所以它不是"启动槽"，是"复位/默认值"。§27.5 表里"最像初始化/启动"这一行判断错误。

### 28.3 全量扫描：`0xad8` 的写入点只有 3 个，全是清 0

`disasm/scan_queue_fields.py`（逐条推进 PC、解码失败 +1 重试 —— `md.disasm()` 整块扫会在
`.text` 里第一个数据岛处停下，命中数恒为 0，这个坑写在这里）扫出 `disp ∈ {0xad0,0xad8,0xadc,0x9d0}`
共 192 处，其中 `0xad8` 21 处，真正的**写入**只有三处：

| 地址 | 指令 | 值 |
|---|---|---|
| `0x140086e01` | `mov dword [rdx+r8+0xad8], eax` | `xor eax,eax` 之后 ⇒ **0** |
| `0x140086f4b` | `mov dword [r9+rbx+0xad8], r14d` | `r14d` 在函数头 `xor r14d,r14d` ⇒ **恒 0** |
| `0x140509a0d` | `mov dword [rdi+0xad8], esi` | `esi` 来自 `stopAllContexts` 的清零 ⇒ **0** |

再加 28.1 那处 `0xff0f` 掩码的寄存器影子写，以及 `0x140835fb9` 的 `mov qword [rbx+0xad8], -1`
（基址不同，别的结构体）。

⇒ **`[dev + slot*0x118 + 0xad8]` 在 vmware-vmx.exe 的全部代码路径上恒为 0。**
静态可证，不需要再花重启预算去试。

### 28.4 那 `0x140509fa3` 的判据为什么写得像"计数"

那一段其实是**环形缓冲的 drain**，不是活性检查：

```
0x140086ef0  movsxd eax, dword [rsp+0x130]        ; 槽索引
0x140086efe  imul   r9, rax, 0x118
0x140086f08  mov    r8d, dword [r9+rbx+0xad8]     ; 上界
0x140086f15  mov    r10d, dword [r9+rbx+0xad0]    ; 头指针
0x140086f1d  imul   r11, rax, 0x23                ; 0x23 = 35，每槽 35 个 qword
0x140086f21  lea    eax, [r10+rdx] ; and eax,0x1f ; 下标 (head+i) & 31
0x140086f31  mov    rax, qword [rbx+rax*8+0x9d0] ; 取元素
0x140086f43  mov    qword [r9+rbx+0xad0], r14     ; 头指针归 0
0x140086f4b  mov    dword [r9+rbx+0xad8], r14d    ; 上界归 0
```

队列本体 `[dev + slot*0x23 + 0x9d0 .. +0x9d0+0x100)`（32 个 qword = 256 B/槽），
`0xad0` 是头、`0xad8` 是长度上界、`0xadc` 是另一个游标。取出的元素随后由
`call 0x1406032f0`（`rcx=客户机地址, rdx=目标, r8=4/8`）**从客户机内存拉取字段** ——
所以这个队列装的是"客户机内存里的结构地址"，不是上下文对象。

由于长度上界恒为 0，drain 循环 `test ebp,ebp; je 0x14008701d` 永远直接跳出。
**这条队列在本宿主上是死代码。**

### 28.5 `0x1404125a0` 的真实判据（重写）

```
0x1404125a0  mov  rdx, [rip+0xfe0c19]          ; rdx = dev
0x1404125a9  movzx r8d, byte [rdx+0xbf8]       ; 槽1 的 base 标志
0x1404125b1  lea  rcx, [rdx+0xad8]             ; 槽0 的 0xad8
0x1404125b8  test r8b, r8b ; je  0x1404125d1   ; 槽1 未起 → 跳过
0x1404125bd  cmp  byte [rdx+0xae0], 0 ; jne chk
0x1404125c6  cmp  rax, 1 ; jne 0x1404125d1     ; 只接受槽索引 1
0x1404125cc  cmp  dword [rcx], 0 ; ja 0x1404125ea  ; ★ 唯一的 set-1 出口
0x1404125e3  mov  dword [rdx+0x9bc], 0
0x1404125ef  mov  dword [rdx+0x9bc], 1
```

`rax` 从 1 起、循环 2 次、`rcx += 0x118`。`ja`（strictly above）在 `dword` 上等价于 `> 0`，
而该字段恒为 0 ⇒ **只有一种情况能让 `[dev+0x9bc] = 1`：内存被外部改写。**

`0x1404125a0` 的调用者有 **12 个**（`disasm/vmx_callers.py` 扫 `E8 rel32`）：

```
0x140086cf7  0x140086d24  0x140086e09  0x140086f53  0x140087121  0x140087215
0x1404d7396  0x14050979e  0x140509a2e  0x140509fcc  0x14050a439  0x14050a486
```

§27 只找到 `0x140509a2e` 一处，是漏扫。其中 `0x14050a439 / 0x14050a486` 就在
DX 管理器唯一写入点 `0x14050a077` 的同一个函数里 —— 那一侧是**对象池**（`[rcx+0x100]` 空闲槽、
`[rcx+0x108]`/`[rcx+0x10c]` 已用/空闲计数），与 `0xad8` 无关。

### 28.6 结论与新的定向

1. **§27.2 / §27.4 的"让槽活跃就能创建 DX 设备"路线到此为止。**
   `[dev+0x9bc]` 的置位条件是"某个恒为 0 的字段 > 0"，CB 侧无法表达，DC 命令侧也无法表达。
2. §27.3（`0x140509a0d` 所在函数 = `stopAllContexts`，`[dev+0x81d]` 来自入参 `cl`）**仍然成立**，
   这部分反汇编没有错，错的只是它对 `0xad8` 语义的解释。
3. 因此 DX 设备创建链（`0x1404c7a14`）在本宿主上**大概率是宿主侧逻辑的既有缺陷/死分支**，
   而不是我们没走到。继续在这条路上花重启预算的期望收益很低。

⇒ **下一步改走 HP command queue**（`SVGA_CAP2_HP_CMD_QUEUE` = caps2 bit7，实测已置 1）。
HP queue 是绕过 `SVGA_REG_COMMAND_LOW/HIGH` 的 CB 提交路径，宿主对它走另一套解析，
不经过本轮撞到的 `DEFINE_RTV COMMAND_ERROR` 之前的同一批 header 校验。

---

## 29. 三个官方头文件级纠错 + HP queue 路线关闭

本节全部来自 `svga_reg.h` / `svga3d_cmd.h` / `svga3d_dx.h` 原始定义（Linux vmwgfx
`device_include/`），不是推断。三条纠错都直接影响此前的判断。

### 29.1 命令 ID 错了一个：`SET_SHADER_IFACE` 是 1274，不是 1277

`svga3d_cmd.h` 这一段是连续的，所以不会看错：

```
SVGA_3D_CMD_DX_SET_SHADER_IFACE              = 1274,
SVGA_3D_CMD_DX_BIND_STREAMOUTPUT             = 1275,
SVGA_3D_CMD_SURFACE_STRETCHBLT_NON_MS_TO_MS  = 1276,
SVGA_3D_CMD_DX_BIND_SHADER_IFACE             = 1277,   ← 我们一直在发这个
SVGA_3D_CMD_MAX                              = 1278,
```

⇒ 20:35 那条 `SET_SHADER_IFACE -> STATUS 3` **不是"能力不支持"，是发错了命令**。
1277 是 BIND_SHADER_IFACE，期望 `{cid, shaderType, shaderId}`，我们发的是
`{cid, stateMobId, 0x2000}`。已把常量改成 1274。

### 29.2 `SET_COTABLE` 第 4 字段是 `validSizeInBytes`，不是 0/1 标志

```c
typedef struct SVGA3dCmdDXSetCOTable {
    uint32 cid;
    uint32 mobid;
    SVGACOTableType type;
    uint32 validSizeInBytes;      // ← 字节数
} SVGA3dCmdDXSetCOTable;
```

`svga_reg.h` 也印证了"0 字节 = 解绑"是合法语义：vmwgfx 的 `vmw_cotable_scrub()`
解绑一张表就是写 `mobid = SVGA3D_INVALID_ID; validSizeInBytes = 0`。

字段顺序 `{cid, mobid, type, validSize}` 我们其实是对的，错的是把第 4 个当成了布尔值
并统一填 0 —— 等于**给每张 COTable 都发了一条"解绑"**，而 `SET_COTABLE` 因此
永远 ACCEPTED，看起来像成功。已改为 `kGbPageBytes`（4096，等于我们定义的 MOB 大小）。

**但这并没有让 `DEFINE_RTV` 变成 ACCEPTED**（21:27 那轮实测仍 STATUS 3）。
如实记录：这条纠错是对的，但不是卡点的成因。

### 29.3 `SVGA_CAP_HP_CMD_QUEUE` 是 caps1 的 bit29，而且**没有 guest 接口**

之前几份文档（§22.8、§26.4、STATUS.md）都把 HP command queue 写成
"`SVGA_CAP2_HP_CMD_QUEUE` = caps2 bit7"，并把它列为"唯一还没试过的入口"。两处都错：

1. 它在 **caps1**（`SVGA_REG_CAPABILITIES`）里，`SVGA_CAP_HP_CMD_QUEUE = 0x20000000`，
   即 **bit29**。实测本机 `caps = 0xfdffc3e2`，bit29 **确实为 1**。
2. **`svga_reg.h` 的寄存器枚举里没有 `SVGA_REG_HP_CMD_QUEUE`**（枚举到
   `SVGA_REG_TOP = 85` 结束，中间没有 HP queue 相关项），vmwgfx 也从不引用它。

⇒ HP command queue 是 **ESXi/vSphere 宿主机内部**的提交通道（Hypervisor Performance
queue，配合 vGPU/直通使用），guest 侧没有可写的寄存器或 FIFO 槽去驱动它。
**这条路对 guest kext 不存在，从候选列表里删掉。** 之前把它当"最后一个入口"是误判。

顺带确认同一份头文件里的其它位，实测 `caps = 0xfdffc3e2`：

| bit | 宏 | 值 |
|---|---|---|
| 26 | `SVGA_CAP_CMD_BUFFERS_2` | 1 |
| 27 | `SVGA_CAP_GBOBJECTS` | 1 |
| 28 | `SVGA_CAP_DX` | 1 |
| 29 | `SVGA_CAP_HP_CMD_QUEUE` | 1（但无接口）|
| 30 | `SVGA_CAP_NO_BB_RESTRICTION` | 1 |
| 31 | `SVGA_CAP_CAP2_REGISTER` | 1 |

### 29.4 还确认了一件互相矛盾的事

`gb: devcaps DXCONTEXT=1 SM41=1 GL43=1`、`SVGA3D LIVE (DEVCAP_3D=1)`，
但 **`SVGA_FIFO_3D_CAPS[0..255]` 整段读回全 0**。

一个说支持 3D、一个说没有任何 3D 能力。这解释了为什么 `DEFINE_RTV` 之后的
`SET_SHADER_IFACE` 这类**依赖 caps 数组**的命令一律失败，而 `DEFINE_CONTEXT` /
`BIND_CONTEXT` / `SET_COTABLE` 这类**纯对象表记账**的命令能过。

`DEFINE_RTV` 属于前者还是后者，取决于宿主是否用 caps 数组去校验 format/resourceType。
这条线索比继续猜载荷更值得追，见 29.5。

### 29.5 下一轮实验：`DEFINE_RTV` 的最小化 + 参数枚举（一次启动）

已排除的变量：载荷字段顺序（与 `svga3d_dx.h` 逐字段对上）、`cmdSize`（本宿主不校验）、
CTable 大小（改成 4096 无效）、`dxContext`（0 是唯一合法值）、flags（只能 0/1）。

剩下的可疑项只有 **RTV 命令体里那两个枚举值**：`format=1 (X8R8G8B8)` 与
`resourceDim=3 (TEXTURE2D)`。本机 3D caps 全 0，而宿主的 RTV 校验很可能要查
"这个 format 是否被支持"。

所以下一轮应该一次启动内枚举 format/resourceDim 的组合，而不是再改单个值：

| # | format | resourceDim | 备注 |
|---|---|---|---|
| 1 | 1 (X8R8G8B8) | 3 (TEXTURE2D) | 现状 |
| 2 | 2 (A8R8G8B8) | 3 | 相邻 format |
| 3 | 1 | 1 (BUFFER) | 换维度 |
| 4 | 0 (UNKNOWN) | 0 | 看是否报不同的错 |

**关键**：每条用**独立的 CB**、且**每条都在前一条失败后立刻换队列**（0x3f 设备队列，
不楔死）——§22.1 说过一条 COMMAND_ERROR 会封死队列，所以只有第一条的数据可信。
这需要先确认 0x3f 队列上真的能连续送 DX 命令（ERROR 不会传播到设备队列）。

如果四组全都是 `STATUS 3 + errorOffset 0`，那就可以下结论：
**`DEFINE_RTV` 在本宿主上只对"-1/无效"形态的命令有反应，宿主侧 DX 对象创建路径本身残缺**，
kext 侧再怎么调也没用，应当转向"不用 DX 对象也能出像素"的路径（GB 表面 + 2D blit）。

---

## §30 10-29 21:37 启动：§29.5 四组枚举的实测结果（唯一决定性的一次）

### 30.0 结论先行

**§22.1 那个"一次 DX 错误 = 引擎楔死"的前提是错的。** 引擎没有楔死。

这一轮在**同一个启动、同一个 DX context (cid=1)** 上连续提交了三条 `DEFINE_RTV`，
全部返回了**不同的状态码**，而且都是即时的：

| # | queue | format | dim | STATUS | 含义 |
|---|---|---|---|---|---|
| 14 | 1 (DX) | 1 X8R8G8B8 | 3 TEXTURE2D | **3** | COMMAND_ERROR |
| 15 | 2 (DX) | 2 A8R8G8B8 | 3 TEXTURE2D | **4** | **CB_HEADER_ERROR** |
| 16 | 0x3f (DEV) | 1 X8R8G8B8 | 1 BUFFER | **3** | COMMAND_ERROR |
| 17 | 0x3f (DEV) | 0 UNKNOWN | 0 UNKNOWN | **3** | COMMAND_ERROR |

三条 STATUS 3 的 `errorOffset` **全部是 0**，且 header 其它字段（id、flags=1、
length=0x24）完全一致——也就是说**宿主在任何一条命令体被解析之前就已经判死了这个 CB**。
`errorOffset = 0` 在这里不是"第一条命令出错"，而是"错误位置未填"。

### 30.1 STATUS 4 是这一轮唯一的新事实，也是最有用的一个

CB[15] 的构成与 CB[14] **逐字节等价**，唯一差异是 `cbContext`：

```
cb[14]  ctx1  ... pay: 000004a3 0000001c 00000001 00000003 00000001 00000003 ...
cb[15]  ctx2  ... pay: 000004a3 0000001c 00000001 00000003 00000002 00000003 ...
                                                    ^^^^^^^^ format 字段
```

所以 STATUS 4 不是命令体的问题，是**这个队列本身拒绝 DX 命令**：`cbContext=2`
这个 DX 队列没有对应的活跃 DX 上下文槽，header 校验阶段就把整个 CB 判成
`CB_HEADER_ERROR`。

这条把"header 校验"和"命令解析"两层的分工确定下来了：

- **STATUS 4** = CB header / 队列归属层面被拒（还没看命令）
- **STATUS 3 + offset 0** = header 过了、队列归属过了，但命令在执行前被拒
  （不是解析错，是**这个命令在当前状态下不允许执行**）

### 30.2 顺带证伪了 §22.1 与"0x3f 才是安全队列"

- `cbContext=1` 上连续三条 DX 命令（cb9/10/11 是 DEFINE/BIND/SET_COTABLE，
  cb12 是 72 dword 批量，cb14 是失败的 RTV）——**队列 1 完全没被前面的失败影响**，
  每次提交都在下一次轮询立刻回状态。
- `0x3f` 上送的两条 RTV 也都是即时 STATUS 3，没有超时、没有把设备队列拖垮，
  之后的 `log show` / SSH 一切正常。
- 也就是说 **STATUS 3 不会楔死引擎**；楔死现象的历史观察（§22.x 那些"后来的探测
  全部超时"）另有原因，很可能是当时代码在失败后仍在同一 CB 上追加命令、
  导致同一个 CB 的 header 被复用/覆盖。这个需要回头单独核对，
  **但"一条错误 = 一次重启"这个假设可以正式划掉**，实验节奏能快一个数量级。

### 30.3 那么 `DEFINE_RTV` 到底卡在哪

现在可以把所有已知信息收窄成一个很具体的判断：

`SECTION_COMBO` 里 cb12（DEFINE+BIND+12 COTABLES，72 dwords）**COMPLETED**。
那批里 12 条 `SET_COTABLE` 中有 11 条的 `mob = 0xFFFFFFFF`（无效 MOB）、
`validSize = 0`，只有 `type 0 (RTVIEW)` 是真的 `mob 5 / validSize 4096`。
批量能过，说明：

- COTable 记账是纯软件表，**不校验 MOB 是否真的存在**；
- `validSize` 也不校验；
- 宿主对 `SET_COTABLE` 一律接受。

而 `DEFINE_RTV` **立刻**在 offset 0 被拒，说明它不是走"记账"路径，
它要访问 **RTVIEW COTable 所在的那个 MOB 的真实内容/元数据**。
而这套 DX context 是在一个"没有 3D caps"的设备上建立起来的（`3d capabilities
advertised: 0 of 256 non-zero`，而 devcaps 说 DXCONTEXT=1 / SM41=1 / GL43=1）。

于是最合理的解释是：**宿主上的 DX context 对象建起来了（能记账），
但它背后的 3D 设备子对象没有初始化**（因为 guest 从未提供 3D caps 表、
`SVGA_REG_3D_HWVERSION` 读回 0）。`DEFINE_RTV` 需要那个子对象，于是必败。

### 30.4 §30.3 原本的下一步（补 3D caps）——**已证伪，不要做**

写完 30.3 之后我打算"往 `SVGA_FIFO_3D_CAPS` 里填一份 caps 表"。查了 vmwgfx 的
`vmw_get_cap_3d_ioctl()`（linux-stable v5.4.210，`vmwgfx_ioctl.c`）后这条路线直接作废：

```c
if (gb_objects && vmw_fp->gb_aware) {
        /* 逐个 SVGA3D_DEVCAP 走 SVGA_REG_DEV_CAP 寄存器现读 */
} else if (gb_objects) {
        ret = vmw_fill_compat_cap(dev_priv, bounce, size);   /* 合成 */ 
} else {
        fifo_mem = dev_priv->mmio_virt;
        memcpy(bounce, &fifo_mem[SVGA_FIFO_3D_CAPS], size);  /* <-- 唯一读它的分支 */
}
```

`SVGA_FIFO_3D_CAPS` 只在 **`gb_objects == false`** 的分支里被读。我们的 `caps`
是 `0xfdffc3e2`，bit27 `SVGA_CAP_GBOBJECTS` = 1，走的是前两个分支 ——
在 GB 设备上这份共享内存**根本没人读**，它是宿主写给**老式（非 GB）驱动**的兼容块。
同理 `vmw_fifo_have_3d()` / `DRM_VMW_PARAM_FIFO_HW_VERSION` 在 GB 设备上都是直接
返回常量 `SVGA3D_HWVERSION_WS8_B1`，不看 `SVGA_FIFO_3D_HWVERSION`。

**结论**：§29.4 那个"devcap 有值、3D_CAPS 全 0 互相矛盾"的观察，不是矛盾，是
**正常且符合规范**的。GB 设备上 3D 能力的唯一权威通道就是 `SVGA_REG_DEV_CAP`
（我们已经批量读过，`devcap[0..31]` 有完整的内容）。这条线索作废，
`fifoDump()` 里那 32 行 `3dcaps[...]` 日志也就没有了诊断价值，保留但不再解读。

### 30.5 真正的下一步：`SET_COTABLE` 到底该按什么顺序、绑什么 size

回到 30.3 的推理链，它的前半段仍然成立（`SET_COTABLE` 一路被接受、`DEFINE_RTV`
在 offset 0 立刻被拒）。既然不是 caps 的问题，那就要看**宿主拿到 RTVIEW COTable
之后到底读到了什么**。三个具体怀疑点，按代价排序：

1. **COTable 的 MOB 内容必须是全 0 且 `validSize` 必须与宿主预期一致。**
   我们现在 `mob 5` 是 4 KiB、`validSize = 4096`。按 `svga3d_dx.h`，
   `SVGA3D_COTABLE_RTVIEW` 的条目是 `SVGA3dRenderTargetView`，32 字节，
   128 条 × 32 = 4096，自洽。但宿主要的是**条目数**还是**字节数**，
   `validSizeInBytes` 这个字段名说的是字节，值得用"`validSize = 0` 但仍然给它
   一个 4 KiB 的 MOB"做一次对照。
2. **`DEFINE_CONTEXT`/`BIND_CONTEXT` 之后是否还需要 `SET_COTABLE` 之外的一步**——
   比如 `SVGA_3D_CMD_DX_SET_VS/PS/GS/...` 或 `DX_SET_SINGLE_CONST_BUF`。
   `vm3dmp` 的实际命令流可以从宿主侧 `.rdata` 的命令描述符表反推，但更省事的是
   直接看 vmwgfx 的 `vmw_cotable.c` 里 COTable 的**绑定时机**。
3. **RTVIEW 的 `sid` 指向的 surface 是否必须先 `DEFINE_GB_SURFACE` 且带 GB 标志。**
   我们的 sid 3 是 legacy `DEFINE_GB_SURFACE` 建的（`gb: GB SURFACE 3 defined`）。
   在 vGPU10 下 `DEFINE_GB_SURFACE` 和 `DEFINE_GB_SURFACE_EXT` 的语义差别、
   以及 `surfaceFlags` 里是否需要 `SVGA3D_SURFACE_BIND_RENDER_TARGET`，
   是这条路径上还没检查过的一环 —— **这是下一个要查的地方**。

### 30.6 实验节奏可以恢复

§30.2 已经把"一条错误 = 一次重启"划掉。这意味着从下一轮起可以**在一次启动内
连续做多组对照**（每组独立 CB、独立 `cbContext`），拿到全部结果再一起判定，
不必再为每条命令重启一次 VM。单次实验的信息量提升一个数量级。

---

## §31 10-29 21:47 启动：`validSize` 扫描 + surfaceFlags 对照（§30.5 三个怀疑点全灭）

### 31.0 结果表

| 步骤 | 内容 | 结果 |
|---|---|---|
| SPLIT | DEFINE_CONTEXT / BIND_CONTEXT / SET_COTABLE(4096) | 三条全 ACCEPTED |
| STEP 2 | 批量 12 cotables（72 dwords） | submitted，sync COMPLETED |
| 3a-1 | `SET_COTABLE validSize=128` + `DEFINE_RTV` @q1 | set ok → **RTV refused (3)** |
| 3a-2 | `SET_COTABLE validSize=0`（解绑） | **SET_COTABLE 本身 refused** |
| 3a-3 | `SET_COTABLE validSize=4096` | **SET_COTABLE 本身 refused** |
| 3b | legacy FIFO 重定义 sid 3（只留 HINT_RENDER_TARGET） | accepted |
| 3b | 重绑 4096 + `DEFINE_RTV` @0x3f | **refused (3)** |
| 3b | 4 组 format/dim 枚举（§29.5 复跑） | 全部 refused（3/4/3/3，与 §30 完全一致） |

### 31.1 `validSize` 不是卡点（怀疑点 1 灭）

128（vm3dmp 的 32 条目 × 4 B）和 4096（整个 MOB）拿到**同一个 STATUS 3**。
也就是说宿主根本没在这个字段上做判断——`DEFINE_RTV` 是**先**失败在别的地方。

### 31.2 `surfaceFlags` 不是卡点（怀疑点 3 灭）

去掉 `BIND_RENDER_TARGET` 后重定义、重绑，结果一模一样。
宿主不查 surface 的 bind flag。这一条也可以划掉。

### 31.3 ★ 本轮最重要的发现：**队列 1 在 3a-1 之后失效了**

3a-2 和 3a-3 的 `SET_COTABLE` 是**以前从来都 ACCEPTED 的命令**，但这次都被拒了，
而它们的 `cbContext` 都是 `tcid`(=1)，也就是**紧接在 3a-1 那条失败的 DEFINE_RTV 之后**。

对照一下同一轮里的后来者：

- 3b 在 legacy FIFO 重建 surface 后，往 **`cbContext=1`** 发 `SET_COTABLE` → 没有打印
  （`(void)cbSubmitCtx` 丢了返回值，这行没日志，是我的疏忽）；
- 3b 最后那组枚举里 `fmt1/t2d` 又是一条 **q1** 的 DEFINE_RTV → 照样 **STATUS 3**。

再看 §30 那一轮：cb14（q1 RTV，STATUS 3）之后，q1 就不再有任何一次成功。

**结论修正 §30.2**：更准确的表述是——

- **`cbContext=0x3f`（设备队列）确实不会因为命令错误而失效**：本轮 3b 之后
  在 0x3f 上连发两条失败 RTV，仍然每条都**即时**返回 STATUS 3，没有超时。
- **`cbContext=1`（DX 队列 1）会在第一次 COMMAND_ERROR 之后拒绝后续 CB**，
  而且拒绝形态是 `SET_COTABLE` 也一起被拒 —— 这跟 §22.1 的历史观察其实是一致的，
  只是 §30 的样本里 q1 只发了失败命令、后面紧跟的是 q2/q63，所以看不出来。

所以完整规则是 **"错误按 cbContext 记账，0x3f 除外"**。这比原来"全局楔死、
要重启"宽松得多，也比 §30.2 写的"完全不会失效"准确。实验设计上：
**多组对照全部走 0x3f，DX 队列每轮只赌一次。**

### 31.4 至此 DEFINE_RTV 的全部已知变量都排除了

- 载荷字段顺序 ✓（§29，逐字段对 svga3d_dx.h）
- `cmdSize` ✓（宿主不校验，多 4 字节也接受）
- `dxContext` ✓（0 是唯一合法值）
- flags ✓（只能 0/1；bit1/bit2 会触发 STATUS 4）
- format / resourceDim 两个枚举 ✓（4×2 组合全试）
- `validSizeInBytes` ✓（128 / 0 / 4096）
- COTable 的 MOB 是否存在、大小、PT 深度 ✓（`DEFINE_GB_MOB64` 4 KiB PT64_0，fence 通过）
- surface 的 surfaceFlags ✓（有/无 BIND_RENDER_TARGET）
- 3D caps 通告 ✓（§30.4 证明在 GB 设备上这条通道根本不存在）

**剩下唯一没被排除的差异，是"谁在这个 DX context 上做过什么"**：
我们的 cid 1 上只跑过 DEFINE/BIND/SET_COTABLE，而真实的 `vm3dmp`
在 `DEFINE_RTV` 之前跑了整套 device-context 命令（`START_STOP_CONTEXT`、
`DC cmd 3`）以及 shader/state 绑定。§30.3 里"DX 上下文对象建起来了但背后 3D
子对象没初始化"的判断，仍然是对这一现象最简洁的解释。

### 31.5 下一轮该做的事（首次尝试"不只是调参数"）

1. **把失败记账规则落实到代码**：STEP 3a 的循环改成每组都换到 0x3f，
   不再往已经报过错的队列上追加命令（本轮 `COTSZ 0/4096` 那两条无效数据就是
   这么浪费掉的）。顺手把那句丢了返回值的 `(void)cbSubmitCtx` 补上日志。
2. **查 `vm3dmp` 在第一条 `DEFINE_RTV` 之前的完整命令序列**。
   宿主 `.rdata` 从 `0x140bde74c` 起的命令描述符表已经扫出来了，
   但"序列"信息不在表里；更可行的做法是在宿主上对 `0x1404c5ee0`
   （DX 管理器的周期性 tick）和 RTV handler `0x14055be80` 的入口下条件断点，
   在客体真跑一次 Windows guest 时抓 `cid`、`[dev+0x9bc]`、`0x1414930A0` 三者的关系。
   这条路要用 x64dbg + 一个能跑 DX 的 Windows guest，成本明显高于之前的静态分析。
3. **务实的替代路线**：既然 DX 对象创建这条路已经把所有 guest 侧变量都排除干净，
   而 GB surface + 2D blit 这条路径在早期是验证过的（`docs §10` 一带），
   值得先把"能出像素"这个里程碑收掉，再回头啃 DX。

---

## §32 10-29 21:54 启动：队列记账规则的正确版本（§31.3 被自己的实验推翻）

### 32.0 先说一件事：§31.3 那次运行**不是**新版二进制

§31 记录的 21:47 那一轮，代码里 `COTSZ` 的日志串是
`"COTSZ %u (set ok, q%u) DEFINE_RTV -> %s\n"`，所以它打的 `q1` 就是真的走了
`cbContext=1`。我当时据此写下"队列 1 会后继失效"，**那个观察本身是对的**。

然后我按这个结论把代码改成了"全部变体都走 0x3f"，装上、重启，读日志——
读到的仍然是 `(set ok, q1)`。第一反应是"改动没生效"，查了 md5、`strings`、
时间戳，又在 `cbSubmitCtx` 里翻了三处静默 `return false` 和 4 条 timeout。

实际原因是 **`log show` 的返回顺序不是时间顺序**：`--predicate` 用两个不同的
字符串做 OR 时，两批结果各自成块返回，21:54 的新版在那次查询里被排到了后面。
`--start '2026-10-29 21:54:40'` 一加，旧新立刻分清。

教训（值得记下来）：**判定实验结果前，先核对"跑的是哪个二进制"**——
`log show --start` 比 `--last Nm` + grep 可靠，因为后者会把多次启动混在一起；
而且新版/旧版混读时，旧版的行看起来就像"新改动没生效"。

### 32.1 新版的实际结果：0x3f 也会被"记账"

```
cb: submit 6 dwords ctx63 dx0 flags1     -> STATUS 3      COTSZ 128
cb: submit 6 dwords ctx63 dx0 flags1     -> STATUS 3      COTSZ 0
cb: submit 6 dwords ctx63 dx0 flags1     -> STATUS 3      COTSZ 4096
cb: submit 6 dwords ctx63 dx0 flags1     -> STATUS 3      SFLAGS SET_COTABLE
cb: submit 9 dwords ctx1  dx0 flags2     -> STATUS 3      RTV fmt1/t2d
cb: submit 9 dwords ctx2  dx0 flags2     -> STATUS 4      RTV fmt2/t2d
cb: submit 9 dwords ctx63 dx0 flags2     -> STATUS 3      RTV fmt1/buffer
cb: submit 9 dwords ctx63 dx0 flags2     -> STATUS 3      RTV fmt0/unknown
```

`SET_COTABLE` 是**这条链上从第一轮起就被接受过十几次**的命令，现在在
`cbContext=0x3f` 上被拒。而同一轮里，0x3f 上在它**之前**的 4 次探测
（flags 0/1/2/3 × len 0/12）**全部 COMPLETED**。

所以 **0x3f 不是免记账的**。§30.2 的"0x3f 不会失效"和 §31.3 的"错误按 cbContext
记账、0x3f 除外"都不成立。

### 32.2 那到底是什么在记账——一条更简单也更吓人的假设

把这一轮的顺序摆出来：

```
0x3f  探测 ×4           全部 COMPLETED
0x3f  cb1 .. cb8        全部 COMPLETED
q1    DEFINE/BIND/COT   全部 COMPLETED
q1    cb12 (72 dwords)  COMPLETED      <-- DEFINE+BIND+12 COTABLES 批量
q0    sync              COMPLETED
q1    RTV                STATUS 3      <-- §31 里也在这里失败
0x3f  SET_COTABLE        STATUS 3      <-- 第一次在 0x3f 上失败
...
```

时间顺序上，**第一条失败之前唯一一条"新东西"就是 cb12**。它 COMPLETED，
但它是唯一一次**把 11 张 `mob=0xFFFFFFFF` 的无效 COTable 塞进同一个 DX context**
的操作。它返回 COMPLETED，却很可能在宿主侧给这个 DX context 打了一个
"状态已坏"的标记，此后**任何**带这个 context 语义的命令（在**任何** cbContext 上）
都返回 STATUS 3 / offset 0。

这只是一种解释，但它有个能直接检验的预言，而且这个预言和我们已经有的数据一致：
**§30 那一轮（21:37）在 q2 上还能拿到 STATUS 4，本轮 q2 也是 STATUS 4**
—— 说明 STATUS 4 是纯粹的 header/队列判定，没有被"记账"污染；
而被污染的是"命令能不能执行"，STATUS 3 是它的表达。

### 32.3 下一轮：把 cb12 那段批量拆掉，这是唯一该动的变量

现有代码在 STEP 1 已经**逐条**发过 DEFINE / BIND / SET_COTABLE 并且全过，
STEP 2 的批量纯粹是"复刻 vm3dmp 的打包形态"，信息增量本来就很低，
现在看它还是污染源。所以：

1. **删掉 STEP 2 的 12-cotables 批量**（`kTplDwords` 那段），
   只保留 STEP 1 的逐条版本 —— 逐条版本在 §30/§31/§32 三轮里从未失败过。
2. STEP 3a 的 COTable 尺寸扫描改成**每条之间插一次 sync**，
   并且**每组用全新的 cid**（cid 2、3、4…），而不是在同一个 cid 上反复重绑。
   一个新 cid 就是一份干净的状态，这比纠结"哪个 cbContext 免记账"可靠得多。
3. 如果换成新 cid 之后 `DEFINE_RTV` 依然 STATUS 3 / offset 0，
   那就说明"这个 cid 第一次发 RTV 就被拒"，与历史污染无关，
   §30.3 的"宿主侧 3D 子对象没初始化"就是唯一的解释了。

### 32.4 顺带纠正 §30.2 与 §31.3

| 版本 | 说法 | 状态 |
|---|---|---|
| §22.1（旧） | 一条 DX 错误 = 引擎楔死 = 一次重启 | 过强，但方向对 |
| §30.2 | STATUS 3 不会楔死，多组可连续跑 | **错** |
| §31.3 | 按 cbContext 记账，0x3f 除外 | **错** |
| §32.2 | 失败的**副作用**（而非失败本身）污染整个 DX context，所有队列都受害 | 待验证 |

真正可操作的一条是：**每个想独立观测的 DX 实验，都要用新的 cid**。
同一 cid 上的第二次尝试，不管走哪个 cbContext，都可能已经在读别人的墓碑。

---

## §33 10-29 22:04 启动：**污染源不是那个批量**（§32.2 被证伪），并且找到一个硬约束

### 33.0 结果

我去掉了 12-cotables 批量、给每个尺寸变体一个全新的 cid（2/3/4）、
每条 `SET_COTABLE` 都改走 `0x3f`。结果：

```
SPLIT DEFINE/BIND/COT  cid1  q1   -> ACCEPTED ×3
TEMPLATE sync          q0        -> COMPLETED
COTSZ valid128  cid2 (def REFUSED bind REFUSED cot REFUSED) DEFINE_RTV -> refused
COTSZ valid4096 cid3 (def REFUSED bind REFUSED cot REFUSED) DEFINE_RTV -> refused
COTSZ valid0    cid4 (def REFUSED bind REFUSED cot REFUSED) DEFINE_RTV -> refused
```

三个全新 cid 的 **`DEFINE_CONTEXT` 本身**就被拒了。

这一条同时否掉两件事：

1. **§32.2 的"批量污染整个 DX context"错了。** 这一轮压根没发那个批量，
   cid 1 的三条命令全部 ACCEPTED，然后全新 cid 2/3/4 依旧一条都进不去。
2. **"新 cid 就是干净状态"这个假设也错了。**

### 33.1 但数据里有一个必须解释的矛盾：`DEFINE_CONTEXT` 在 cbContext 63 上被拒

看提交细节：

```
cb[13] ctx63 dx0 len 12  pay: 00000477 00000004 00000002   <- DEFINE_CONTEXT cid 2
                  STATUS 3
cb[14] ctx63 dx0 len 12  pay: 00000477 00000004 00000003   <- DEFINE_CONTEXT cid 3
```

而历史记录里**所有**成功的 `DEFINE_CONTEXT` 都是走 `cbContext=1`：
本轮 cid 1 的三条（q1）全 ACCEPTED；§24 的反汇编也写着
"DEFINE_CONTEXT with flags=1 (NO_IRQ only) is ACCEPTED on **queue 1**"。

所以最直接的读法不是"新 cid 不干净"，而是：

> **`cbContext` 不是"哪条队列"，而是"这个 DX context 绑在哪条队列上"。**
> `DEFINE_CONTEXT` 只有在 **cbContext 与该 cid 将要绑定的队列一致**时才被接受；
> 把 cid 2 的 DEFINE 发到 63 号队列，等于给一个不存在的队列建上下文。

这条如果成立，就把之前所有"q1 之后失效"的观察串起来了：
q1 上第一次 `DEFINE_RTV` 失败之后，q1 上再发 `DEFINE_CONTEXT` 给任何 cid 都会失败，
因为它仍然要求"队列 1 的状态是好的"。**坏的是队列本身，不是 cid。**

这也是 §31.3 观察到的现象（q1 上的 `SET_COTABLE` 后继被拒）的正确解释，
同时解释了为什么 q2 一直回 STATUS 4（队列 2 从未 DEFINE 过任何 cid）。

### 33.2 因此下一轮的正确形状

**一个 cid 配一条独立的 cbContext，从 DEFINE 到 RTV 全程不换队列。**
本轮的设计恰恰把它拆散了（DEFINE 走 63、RTV 也走 63），所以三个变体全废。
重新设计：

| 变体 | cbContext | cid | 动作 |
|---|---|---|---|
| 0 | 1 | 1 | DEFINE / BIND / SET_COTABLE(4096) / **DEFINE_RTV** ← 唯一的一次赌 |
| 1 | 3 | 3 | 同上，validSize = 128 |
| 2 | 4 | 4 | 同上，validSize = 0 |
| 3 | 5 | 5 | 同上，format = A8R8G8B8 |

前提是队列 1/3/4/5 各自独立记账。这一点本身也要验证——如果队列 1 的失败
会让队列 3 也失效，那就回到"一次启动只能测一个变量"，
只是这个结论要在**正确的**实验形状下得出，而不是在 §31、§33 这种拆散了的形状下。

### 33.3 已确认可以复用的两个"平台事实"

1. `DEFINE_CONTEXT` / `BIND_CONTEXT` / `SET_COTABLE` 这三条，在**队列 1** 上
   连续发送从未失败（四轮、十几次）。
2. `DEFINE_RTV` 至今**一次都没有成功过**，在所有 cbContext（1/2/63）、
   所有载荷变体、所有 COTable 尺寸、所有 surfaceFlags 组合下都是
   `STATUS 3 + errorOffset 0` 或 `STATUS 4`。

第 2 条现在是整个项目最硬的一个事实。

---

## §34 10-29 22:10 启动：**`cbContext` 的真实语义找到了**（§33.1 的猜测是对的，且能解释全部历史）

### 34.0 PAIR 实验（每个变体 cid 与 cbContext 全程自洽、不换队列）

| 变体 | cid | cbContext | DEFINE_CONTEXT 结果 | 后续 |
|---|---|---|---|---|
| ctrl 4096/t2d | 1 | **1** | **STATUS 3** | bind/cot/RTV 全 refused |
| size 0128/t2d | 3 | 3 | **STATUS 4** | 同上 |
| size 0 /t2d | 4 | 4 | **STATUS 4** | 同上 |
| fmt A8R8G8B8 | 5 | 5 | **STATUS 4** | 同上 |

同一轮更早，**cid 1 在 cbContext 1 上的同一条 `DEFINE_CONTEXT` 是 ACCEPTED**：

```
SPLIT DX_DEFINE_CONTEXT {cid 1}   ctx1  -> ACCEPTED     <- STEP 1
PAIR ctrl ... cid1 q1             ctx1  -> STATUS 3     <- 20 毫秒之后，同一条命令
```

payload 逐字节相同（`00000477 00000004 00000001`），header 只有 id 不同。
**差别只在于：中间跑了一次 STEP 1 的 `BIND_CONTEXT` 和 `SET_COTABLE`。**

### 34.1 结论：`DEFINE_CONTEXT` 是"创建"，不是"重置"

对同一个 cid 第二次 `DEFINE_CONTEXT` 会失败，因为 cid 1 已经被创建了。
§24 那条注释（"DEFINE_CONTEXT with flags=1 is ACCEPTED on queue 1"）
是**第一次**调用时的观察，被当成了无条件成立 —— 这正是从 10-21 到 10-29
所有"后备探测全被拒"的根本原因：那些探测都在**已经 DEFINE 过的 cid** 上重发 DEFINE。

同理，`SET_COTABLE` 在后继被拒也是因为它在测一个已经坏掉的 cid 1。

### 34.2 STATUS 3 与 STATUS 4 的分工（现在没有任何反例）

- **STATUS 4 / CB_HEADER_ERROR** —— `cbContext` 这个队列从未承载过任何
  DX context。cbContext 3/4/5 上第一次发包就是 4；cbContext 2 每次都是 4；
  cbContext 63（设备队列）从来不会给 4。
  → **队列必须是"已经有过 DX context 的队列"，而本项目里只有队列 1 是。**
- **STATUS 3 / COMMAND_ERROR(offset 0)** —— 队列本身有资格承载 DX，但
  **命令所针对的 cid 在当前状态下不允许做这件事**。重复 DEFINE 属于这一类，
  在坏 cid 上发 RTV 也属于这一类。

这条分工把此前四轮里所有互相矛盾的现象一次性解释完了，包括
"§30.2 说 0x3f 免记账、§31.3 说按 cbContext 记账、§32.2 说 context 被污染"——
三者都只是"同一个 cid 被反复使用"的不同侧面。

### 34.3 因此：**一次启动只能得到一条可信的 `DEFINE_RTV` 结果**

可用资源极其有限：

- 队列 1 是唯一能承载 DX 的队列（已验证）；
- 队列 1 上一次启动内只能成功 DEFINE 一个新 cid（第二次 DEFINE 就 STATUS 3）；
- 所以 **一次启动 = 一个新 cid = 一次 `DEFINE_RTV` 的机会**。

后面所有 `DEFINE_RTV` 的变体实验必须回到"一次启动一个变体"的节奏。
§30.6 那句"实验节奏可以恢复"是错的，撤回。

### 34.4 下一轮的唯一动作

把 STEP 1 之后那条 `DEFINE_RTV` 的载荷定死，然后**只改一个东西**：
`format`。因为 `format/resourceDim` 是唯一还没在"干净 cid"上被测过的变量
（§30 的 4 组枚举全部发生在 cid 1 已经 DEFINE 过、且已经有一次失败 RTV 之后）。

具体：STEP 3 的首发用 `format = 2 (A8R8G8B8)`、`dim = 3`、`desc = {0,0,1}`，
cid 1，队列 1。如果它 **ACCEPTED**，说明 `X8R8G8B8` 不是合法的 RTV 格式；
如果仍是 STATUS 3，说明格式不是原因，`DEFINE_RTV` 在这台宿主上确实无法成功，
应当转向 2D 路径（§31.5-3）并把这个结论固化进 STATUS.md。

---

## §35 单发 `format = A8R8G8B8`：矩阵里最后一个干净格子

### 35.1 代码改动

按 §34.4 执行，本轮把整个 STEP 3 收敛成一条命令：

| 项 | 值 | 说明 |
|---|---|---|
| cid | `1` | STEP 1 刚在队列 1 上 DEFINE 成功的那个 |
| cbContext | `1` | 唯一承载过 DX context 的队列（§34.1） |
| flags | `2` | `kCbFlagDxContext` |
| cmdSize | `28` | `svga3d_dx.h` 实打实 |
| rtvId / sid | `trtv` / `surfId` | STEP 1 里 `SET_COTABLE` 已绑好 COTable |
| **format** | **`2` `A8R8G8B8`** | **本轮唯一变量**（此前一直是 `1` `X8R8G8B8`） |
| resourceDimension | `3` `texture2d` | 不变 |
| desc | `{0, 0, 1}` | 不变 |

同时**退役**了三段代码，它们都是被 §33/§34 证伪的：

- **STEP 3a**（`PAIR` 四组枚举）：三组打在 cbContext 3/4/5 上，首次发包即
  STATUS 4 —— 那些队列永远不可能承载 DX context，测的是空气；留下的一组
  cid 1 又发生在 cid 1 已经失败过一次 `DEFINE_RTV` 之后。
- **STEP 3b**（`SFLAGS redefine`）：`surfaceFlags` 去掉 `BIND_RENDER_TARGET`
  的重定义，同样在坏掉的 cid/队列上。
- 与它们配套的旧注释（"STATUS 3 不会楔死引擎，多组可连续跑"）一并删除 ——
  留着只会误导下一个读代码的人。

`build.sh` 的 `MARKERS` 相应更新：删 `PAIR %s` / `SFLAGS redefine`，
`dx: RTV %s` 改成更精确的 `dx: RTV fmt2 A8R8G8B8/t2d`。
（改完第一次构建确实被 FATAL 拦下了 —— 这正是可达性标记该有的行为。）

### 35.2 判据

- **ACCEPTED** → `X8R8G8B8` 不是合法的 RTV 格式，枚举就是卡点，DX 通路重新打开，
  接着走 `CLEAR_RTV` + `READBACK` 验像素。
- **STATUS 3 / offset 0** → 格式不是卡点。结合 §30–§33 的全部结果，
  **`DEFINE_RTV` 在本宿主上无法成功**，这条主线到此为止，转 2D 出像素路径
  （§31.5-3：GB surface + `SURFACE_DMA` / blit）。

### 35.3 实测

（结果见 §35.4。）

### 35.4 实测结果：`STATUS 3 (errorOffset 0)`，`format` 不是卡点

> **§62 更正**：下文“格式不是卡点/变量已经穷尽”撤回。0/1/2没有覆盖合法 typed DX 视图格式；本机代码明确拒绝 RTV format1。另有 §61 已撤回的 NULL 上下文归因。保留旧实验数据用于审计。

一次完整的启动序列（22:23 重启，`log show --last 6m`）：

```
vgpu-fb: dx: SPLIT DX_DEFINE_CONTEXT {cid 1} -> ACCEPTED
vgpu-fb: dx: SPLIT DX_BIND_CONTEXT  {cid 1 mob 6} -> ACCEPTED
vgpu-fb: dx: SPLIT DX_SET_COTABLE   {cid 1 mob 5 type 0} -> ACCEPTED
vgpu-fb: dx: TEMPLATE sync CB (q0 flags1) -> COMPLETED
vgpu-fb: cb[1..12] COMPLETED (errorOffset 0)
vgpu-fb: cb: STATUS 3 (errorOffset 0) -- full header dump
vgpu-fb: cb hdr[ 0.. 7] 00000003 00000000 0000000d 00000000 00000001 00000024 161b9000 00000000
vgpu-fb: dx: RTV fmt2 A8R8G8B8/t2d {rtv 1 sid 3 q1} -> refused
vgpu-fb: dx: TEMPLATE verdict -> still refused
```

读法：

- `hdr[0]=3` 即 `SVGA_CB_STATUS_COMMAND_ERROR`，`hdr[1]=0` 即 `errorOffset 0`；
- `hdr[2..3]=0x0000000d` 是 id，`hdr[5]=0x24` = **36 字节** = 9 dword = 送去的那条
  `DEFINE_RTV` 本身（cmdSize 28 + 头部 8），说明宿主**收全了这条命令**才判错；
- `hdr[4]=1`（NO_IRQ，`cbSubmitCtx` 里强制 OR 上去的）；
- 前 12 条 CB 全部 COMPLETED，说明 STEP 1 的三条 DX 命令和 sync 屏障在**同一个
  进程、同一秒**内都是健康地被宿主接受的。

**结论（按 §35.2 第二条判据）：`format` 不是 `DEFINE_RTV` 的卡点。**

到这里，`DEFINE_RTV` 的失败变量已经穷尽。下列每一项都在"cid 在队列 1 上、且
DEFINE_CONTEXT 刚刚 ACCEPTED"的前提下单独测过并排除：

| 变量 | 测过的值 | 结果 |
|---|---|---|
| `format` | 0 / 1 / **2** | 全拒（2 为本轮，且是最干净的上下文） |
| `resourceDimension` | 0 / 1 / 3 | 全拒 |
| `cbContext` | 1 / 2 / 3 / 4 / 5 / 0x3f | 1 给 3，其余给 3 或 4 |
| COTable `validSizeInBytes` | 0 / 128 / 4096 | 全拒 |
| `surfaceFlags` 含 `BIND_RENDER_TARGET` | 有 / 无 | 全拒 |
| 载荷字段顺序 / `cmdSize` / `desc` | 与 `svga3d_dx.h` 逐字节对齐 | 全拒 |
| 单发 vs 批量 | 单条 CB / 12 条合批 | 全拒 |
| `dxContext` 字段 | 0 | 全拒 |

所以 §29 起的这条主线（"用 `DEFINE_RTV` 让 DX 管理器活过来"）**到此判定为在本宿主上
不可达**。§30.2 的"STATUS 3 不会楔死引擎"、§31.3 的"错误按 cbContext 记账"、
§32.2 的"批量污染 context"这三条中间结论，都已在此前的 §33/§34 里撤回，
不要再引用。

### 35.5 一个值得记下的形状差异

`DEFINE_RTV` 是**整个模板里唯一一条"引用既有资源"的命令**：`DEFINE_CONTEXT`
引用的是一个新 cid，`BIND_CONTEXT` 引用的是 state MOB，`SET_COTABLE` 引用的是
COTable MOB —— 宿主对这三条都只看"MOB 存在且可映射"，所以全过。
而 `DEFINE_RTV` 要求宿主**查询 sid 3 这张 surface 的实际属性**，并据此校验
view 描述是否合法。sid 3 是走**legacy FIFO** `DEFINE_GB_SURFACE` 建的
（`legacy DEFINE_GB_SURFACE accepted (sid 3 on mob 4)`），这是本模板里唯一
一个由 legacy 路径创建、却被 DX 路径引用的对象。

这条"跨路径引用"是当前唯一还没被单独隔离的变量，也是下一轮唯一值得做的实验：
**改用 `SVGA_3D_CMD_DX_DEFINE_GB_SURFACE`（DX 路径）在同一个 cid 上重建 sid**，
再用这个 sid 做 `DEFINE_RTV`。如果仍然 STATUS 3，跨路径引用也被排除，
`DEFINE_RTV` 的封锁就真的是宿主侧的硬限制。

---

## §36 §35.5 的"下一轮"在动手前被证伪 —— 2D 路径早已存在且已验证

§35.5 提出的下一个动作是"改用 `SVGA_3D_CMD_DX_DEFINE_GB_SURFACE` 重建 sid"。
在动手之前先按惯例核对上游定义，结果这条路从一开始就不存在：

### 36.1 不存在 `SVGA_3D_CMD_DX_DEFINE_GB_SURFACE` 这个命令

从 `svga3d_cmd.h` 逐条核过 `SVGA_3D_CMD_DX_MIN(1143) … DX_MAX(1227)` 的整个区间，
**没有任何一条 DX 前缀的 surface 定义命令**。GB surface 的定义只有四种形态：

| 命令 | ID | 载荷 | vmwgfx 表中的条目 |
|---|---|---|---|
| `DEFINE_GB_SURFACE` | 1097 | 8 dword（surfaceFlags 为 **32 位**） | `vmw_cmd_invalid` |
| `DEFINE_GB_SURFACE_V2` | 1134 | + arraySize, pad | `vmw_cmd_invalid` |
| `DEFINE_GB_SURFACE_V3` | 1239 | surfaceFlags 为 **64 位**，+ MS pattern/quality | — |
| `DEFINE_GB_SURFACE_V4` | 1267 | V3 + bufferByteStride | `vmw_cmd_invalid` |

`svga3d_cmd.h` 里 `SVGA3D_SURFACE_FLAG_MAX = 1<<44`，我们用的最高位是
`BIND_RENDER_TARGET = 1<<24`，远在 32 位标志空间的范围内 —— 也就是说
**V1（32 位 surfaceFlags）和我们发送的内容是兼容的，不存在"必须升级到 V3/V4"的问题**。

所以 §35.5 那条实验设计是错的，作废。教训：**在写"下一轮做什么"之前先核对协议定义，
不要凭"应该有这么一个命令"的直觉**。这次是一个 curl 就否掉了一次烧启动的实验。

### 36.2 `DEFINE_GB_SURFACE` 本来就不经 CB —— 它走 legacy FIFO

这一点更重要，它把 §35 的"跨路径引用"从"可疑"变成了"正常"：

- vmwgfx 在 `vmwgfx_execbuf.c` 的命令表里把 `DEFINE_GB_SURFACE` / `_V2` / `_V4`
  全部标为 `vmw_cmd_invalid`，意思是**这个驱动自己从不发送它**（表的语义见
  `VMW_CMD_DEF(_cmd,_func,_user_allow,_gb_disable,_gb_enable)`）。
- 反过来看我们自己的代码：M6 里程碑（10-06，`b89c619`）**屏幕完整实时渲染**
  的那一版，surface 就是走 legacy FIFO 的 `DEFINE_GB_SURFACE` 定义的
  （`gb: GB SURFACE 3 defined (256x256 X8R8G8B8 on MOB 1)`），而
  `SURFACE_DMA` / `UPDATE_GB_IMAGE` / 屏靶绑定同样走 legacy FIFO，全部工作正常。

⇒ **"legacy 定义的 surface 被后续命令引用"本身不是异常，是被验证过的正常路径。**
§35.5 把这一点列为"唯一还没隔离的变量"，方向就是错的。
`DEFINE_RTV` 的失败应当归因于 DX 命令族本身（DX 管理器未创建），
而不是它引用的 sid 的出身。

### 36.3 因此 2D 出像素路径不是"将来要做"，是"已经做过"

`STATUS.md` M6 写得很清楚，而且这版代码里还留着完整实现：

- 呈现循环 30 Hz：`memcpy(VRAM → surface MOB)` →
  `SURFACE_DMA(1044)` guest→host → `UPDATE_GB_IMAGE(1101)` →
  `UPDATE_GB_SCREENTARGET`；
- surface 必须带 `SVGA3D_SURFACE_SCREENTARGET (1<<16)`，MOB 用 `PT64_2`；
- 当前 `kGbStage = 4`，**STDU 通路是关闭状态**（本次启动日志里没有 `stdu:` 行，
  只有 `DXCONTEXT otable base set` / `DX context state MOB 3 ready`）。
  要恢复它只需把 `kGbStage` 提到 5。

`kDxTest` 里那段 `SURFACE_DMA` 上传 → 下载 → 校验像素的往返测试
（`DMA ROUND TRIP VERIFIED`）也是现成的，它测的正是 2D 数据通路的正确性。

### 36.4 结论修正

- §35 关于"`DEFINE_RTV` 在本宿主上无法成功"的判定**不变**，仍然成立。
- §35.5 引出的"下一轮实验"**作废**，它基于一个不存在的命令和一个错误的前提。
- 下一步不再是"找 DX 的另一个变量"，而是**确认 2D 通路的状态**：
  把 `kGbStage` 提到 5 恢复 STDU，观察显示是否恢复、像素是否正确。
  这是把项目从"研究 DX"拨回"产出可见像素"的那个动作 —— 而且它花费的是
  一次改开关 + 一次重启，不是又一轮猜谜。

### 36.5 STDU 已恢复并跑起来，但呈现是黑的（10-29 22:40 boot）

`kGbStage = 4 → 5`、`kDxTest = 1 → 0`，重启后日志：

```
vgpu-fb: gb: GB SURFACE 1 defined (256x256 X8R8G8B8 on MOB 1) -- GB object model is UP
vgpu-fb: stdu: backing store 3145728 bytes at pt ppn 154848 (PT64_2)
vgpu-fb: stdu: screen target 0 (1024x768) bound to surface 2 on MOB 2 -- STDU present path is UP
vgpu-fb: present: STDU re-present started (33 ms)
vgpu-fb: present tick 1 (next 1644 stop 1644)
vgpu-fb: present tick 300 (next 48288 stop 48288)
```

判读：

- **STDU 通路建立成功**：背衬 3 MB 用 `PT64_2`，屏靶 0 绑定到 surface 2，
  30 Hz 循环在跑。
- **FIFO 健康**：tick 1 → 300 期间 `next_cmd == stop` 一路推进（1644 → 48288），
  说明 `SURFACE_DMA` / `UPDATE_GB_IMAGE` / `UPDATE_GB_SCREENTARGET` 三条命令
  **每帧都被宿主消费了，没有楔死**。
- **但屏幕是黑的**：VNC 截图 1024x768，采样 12288 个点，**distinct colors = 1，
  全是 (0,0,0)**。
- 拓扑寄存器（35–40）在 CONFIG_DONE 前后各写了一遍，M6 四要素齐备；
  `kGuestDriverId` 也在 CONFIG_DONE 前写了 WDDM。

也就是说：**命令通道、数据搬运、屏靶绑定全部正常，唯独"内容"没有出现在屏上。**
这与 `STATUS.md` P3 记的"fresh-MKS 冻结 / 呈现不出内容"是同一个老问题，
不是本轮改动引入的回归 —— 但这次拿到了一条此前没有的证据：
**呈现循环本身是健康的，问题在内容来源（VRAM 那一份）或屏靶的取用侧。**

下一步该查的是"VRAM 里到底有没有内容"，而不是继续动 STDU 命令序列：
如果宿主根本没有在往 `SVGA_REG_FB_START` 写东西（guest 侧无窗口系统画图，
framebuffer 自身就是黑的），那黑屏就是**正确**的 —— 真正要验证的是
"我们主动往 surface 写像素，屏上能不能看见"，这是一个确定性的实验，不依赖宿主投递内容。

### 36.6 build.sh 的标记改成了条件式

`kDxTest = 0` 会让整个 DX 模板块不可达，旧的固定标记集必然 FATAL。
处理方式不是删标记（删了等于让退役的实验下次悄悄复活），而是
**让标记集跟着它所属的开关走**：`build.sh` 用
`grep -qE 'kDxTest[[:space:]]*=[[:space:]]*1'` 读源码里同一个开关，
为 1 时才要求 DX 模板的五个标记 + 三个 verdict 字面量；STDU 的两个标记
则改为**始终必检**，因为它们对应的是现在要出货的路径。实测输出：

```
kDxTest = 0 -> DX template retired, markers not required
present: STDU re-present started
present: screen target 0 (%ux%u) bound to surface %u
```

### 36.7 决定性实验：我们自己的像素也上不了屏（10-29 22:47 boot）

§36.5 留下两个相反的假设，靠观察分不开：

- (a) 呈现通路坏了 —— 我们的像素到不了屏幕；
- (b) 呈现通路好着 —— 只是 VRAM 本身就是黑的（guest 侧没有窗口系统在画）。

于是加 `kStduPaint`：每个 present tick 在 `memcpy(VRAM → _stduVirt)` **之后**
再覆盖一层已知图案（红边 32 px、上半绿、下半蓝），**完全绕开 VRAM**，
内容由我们自己提供。首帧打印一次，证明填充真的跑了：

```
present: STDU paint armed (red border, green top, blue bottom) first=0xffff0000 mid=0xff0000ff
present tick 1 (next 1644 stop 1644)
present tick 2 (next 1800 stop 1800)
present tick 3 (next 1956 stop 1956)
```

`first=0xffff0000`（红边）、`mid=0xff0000ff`（下半蓝）—— 缓冲区里的内容
**与设计完全一致**，填充路径没有问题。

然后 VNC 截图（1024×768，采样 12288 点）：

```
distinct: 1
  (0, 0, 0) 12288
```

**仍然是纯黑，一个彩色像素都没有。**

### 36.8 结论

**(b) 被证伪，(a) 成立：呈现通路是坏的。**

这一条把 P3 从"猜测"变成了"定位"：

- 不再是"VRAM 里没东西所以黑屏"——内容由我们自己供给了，还是不显示；
- 不再是"命令没被消费"——`next_cmd == stop` 一路推进到 188688，宿主每帧都在取；
- 所以问题在**命令被取走之后到屏上出现之间**：要么 DMA 拿到的 MOB 内容
  不是我们写的那份（PT64_2 的映射对不上），要么屏靶绑的 surface 2 与
  我们 DMA 的目标不是同一个对象，要么 MKS 在 fresh-MKS 上没有把屏靶
  接到显示输出（P3 记录的"fresh-MKS 冻结"）。

下一轮该做的实验，按成本从低到高：

1. **回读验证 DMA 的目标**：`SURFACE_DMA` 用 `READ_HOST_VRAM` 把 surface 2
   读回 `_stduVirt`，比对是否为我们画的图案。这一步就能区分
   "DMA 没写进去" 与 "写进去了但屏靶不显示"。
2. 若回读也是黑的 ⇒ `guest.ptr.gmrId = _stduMobId` 这一路的 MOB 映射有问题
   （M6 当年用的是同一份代码，但那是**长寿 MKS**）。
3. 若回读正确 ⇒ 问题在屏靶/显示侧，回到 P3 的 fresh-MKS 拓扑问题，
   届时 `UPDATE_GB_SCREENTARGET` 的 rect/绑定时序是主要嫌疑。

---

## §37 回读实验：宿主根本没往我们的缓冲区写一个字节

### 37.1 设计

§36.8 的第 1 步。`SURFACE_DMA` 有两种 transfer：
`SVGA3D_WRITE_HOST_VRAM (1)`（guest→host，呈现用的那条）和
`SVGA3D_READBACK (0)`（host→guest）。把前者换成后者，就能问宿主
"**你以为 surface 2 里现在是什么**"。

几个刻意的设计点：

- 回读目标是一个**独立的 scratch 缓冲区**（3 MB，`kStduMobId + 100 = mobid 102`），
  不是 `_stduVirt`。读回自己的源缓冲区是自证：宿主若静默丢弃 DMA，
  里面仍然是我们画的图案，看起来像 PASS。
- scratch 缓冲区先 `bzero`，所以"一个字节都没被写"与"写错了内容"
  在结果里可区分。
- page table 单独分配 3 页（root 1 + L1 2，768 项 × 8 B = 6144 B）。
  GB 工作缓冲区的 4 KiB 页装不下 768 项的 L1 表，复用它会把
  surface 1/2 已经在用的表踩坏。
- MOB otable 条目在 `DEFINE_GB_MOB64` **之前**写入（宿主在处理 DEFINE 时快照条目，
  这是 §26 踩过两次的坑）。
- 回读由 `fenceAck` 夹住，比较在 fence 之后执行。

### 37.2 实测（11-03 01:32 boot）

```
stdu: screen target 0 (1024x768) bound to surface 2 on MOB 2 -- STDU present path is UP
present tick 1 (next 1644 stop 1644)
present: STDU paint armed (red border, green top, blue bottom) first=0xffff0000 mid=0xff0000ff
present tick 2 (next 1800 stop 1800)
present tick 3 (next 1956 stop 1956)
rb: px[0] src 0xffff0000 rb 0x00000000 DIFF
rb: px[1] src 0xff00ff00 rb 0x00000000 DIFF
rb: px[2] src 0xff0000ff rb 0x00000000 DIFF
rb: px[3] src 0xffff0000 rb 0x00000000 DIFF
rb: VERDICT *** MISMATCH *** (0/768 nonzero)
```

没有 `fence NOT acked`，说明 `DEFINE_GB_MOB64 mob 102` 和那条
`SURFACE_DMA` 都被宿主收下并执行完了；之后呈现循环一路推进到
tick 1800（`next 262136 stop 262136`），FIFO 也没被楔死。

### 37.3 读法与它排除了什么

关键在于 **`0/768 nonzero`**，而不只是"4 个采样点不同"：

- 如果宿主写了缓冲区但内容不对（比如把别的 surface 送来），
  nonzero 会是一个正常的大数字，采样点也会是别的颜色；
- **全是 0 意味着那块物理内存从头到尾没有被 DMA 引擎碰过。**

所以这不是"surface 2 的内容与我们以为的不同"，而是
**`SURFACE_DMA` 在 guest→host 方向和 host→guest 方向上对 mobid 102 / sid 2
这对组合都没有实际搬运**。

结合 §36.7（同样的 `SURFACE_DMA` 用 mobid 2 写 sid 2，命令被消费、
FIFO 推进，但屏上全黑），两条方向一致地指向同一个结论：

> **这条 FIFO 上的 `SURFACE_DMA` 是被"接受"了，但没有完成任何内存搬运 ——
> 宿主把它当作 no-op 吞掉了。**

fence 能通过，说明命令流本身在推进；没有任何内存被写，说明
`guest.ptr.gmrId` 指向的 MOB 没有被宿主解析成一张有效的页表。
两种可能：

1. **MOB otable 的 16 字节条目格式不对**，宿主读到的 root PPN 是垃圾或 0，
   于是一次搬运都没有发生，且不报错（vmwgfx 里 gmrId 查不到时会直接
   `return -EINVAL`，但那条路径属于 host 的另一套实现）；
2. **`guest.ptr.gmrId` 在 GB 设备上不是 mobid**，而是需要先经过
   `SVGA_3D_CMD_SET_MOB_VISIBILITY` / `DEFINE_MOB` 之外的某种注册。
   M6 能出画面，说明当年这条路径是通的 —— 那么差异就在
   **MOB 表条目格式**或 **surface 2 的 MOB 绑定方式**上。

### 37.4 下一步：直接验证 MOB 表条目

上面第 1 条可以用一个几乎零成本的实验定死：**把回读改成读 mobid 2
自己**（也就是呈现用的那条 MOB），如果连它都返回全 0，那 MOB 表条目格式就是
根因；如果它返回了正确图案，那问题只在 sid 2 的绑定侧。

不过在动手前，应该先去 `vmwgfx_mob.c` 核对 `SVGAOTableMobEntry` 的真实布局 ——
我们只在 §26 里按"depth/numPages/rootPpn"推断过，16 字节的推断从未与
上游结构体逐字段比对过。**这正是 §36.1 的教训该用的地方：先读定义，再花启动周期。**

---

# §38 PT64_1 对照实验 + 上游逐字段核对：四个真 bug、一个假结论

§37 结束时的问题很尖锐：`SURFACE_DMA` 两个方向都不搬字节，屏上全黑，
而命令全被接受。§38 的任务是把它拆开。

## 38.1 PT64_1 单级页表对照（完成）

§38 的第一刀砍向"层级遍历"。PT64_2 需要两级、768 页；改成 **PT64_1 单级、
1024x512、正好 512 页**，一次启动就能区分"两级表被我写错"和"层级不是卡点"。

结果（`kStduPt1 = 1`）：

```
stdu: backing store 2097152 bytes (512 pages, 1024x512) at pt ppn 106980 (PT64_1)
pt: page 0   (lvl 0 idx 0)   want ppn 106983, got 106983 OK
pt: page 1   (lvl 0 idx 1)   want ppn 106984, got 106984 OK
pt: page 256 (lvl 0 idx 256) want ppn 107239, got 107239 OK
pt: page 511 (lvl 0 idx 511) want ppn 107494, got 107494 OK
pt: VERDICT *** page table resolves ***
```

**结论：层级不是卡点。** PT64_2 时代的"页表可能有错"这条线彻底关闭。
（PT64_1 保留为主配置：页更少、表更简单，排除一个变量。）

## 38.2 宿主侧日志第一次给出正面证据（完成）

这一轮查看了 `E:\Vboxs\MacOSX\MacOS-R15\vmware.log`，第一次拿到宿主对
屏靶的**主动确认**：

```
SVGA-ScreenMgr: Screen type changed to ScreenTarget
SWBScreen: Screen 1 Defined: xywh(0, 0, 1024, 512) flags=0x2
MVNCEncode: MVNCEncode_ServerSetTopology - size: (1024, 512)
MVNCEncode: Number of screens changed from 0 to 1
MVNCEncode: screen: 0 BoundingBox: (1024x512) bytesPerLine: 4096
MVNCRegionEncoder_Create: region encoder static. Screen 0/1 @ Resolution: 1024 x 512
```

**屏靶链路是完全激活的**：宿主切了 ScreenTarget、按我们的 1024x512 重建了
VNC 拓扑、bytesPerLine = 4096 与我们的 pitch 一致。**"屏靶没生效"这条排除。**

而 `region encoder static` 是这里最刺眼的一词：编码器认为画面静止，
**宿主从未认为内容更新过** —— 这与"命令全接受、零报错"是同一件事的两面。

## 38.3 逐字段核对上游结构体：全部一致（完成）

把 `/tmp` 下的上游头文件与代码逐字段对了一遍，以下**全部正确**，不再怀疑：

| 结构 / 命令 | 上游尺寸 | 我们的值 |
|---|---|---|
| `SVGAOTableSurfaceEntry` | 64 | 64 OK |
| `SVGAOTableMobEntry` | 16 | 16 OK |
| `SVGAOTableScreenTargetEntry` | 64 | 64 OK |
| `SVGA3dCmdDefineGBScreenTarget` | 28 | 28 OK |
| `SVGA3dCmdDefineGBSurface` (v1) | 40 | 40 OK |
| `SVGA3dCmdBindGBScreenTarget` | 16 | 16 OK |
| `SVGA3dCmdUpdateGBScreenTarget` | 20 | 20 OK |
| X8R8G8B8 `pitchBytesPerBlock` | 4 | 4096 = 1024x4 OK |

顺带核实了两条一直靠猜的语义：

- **`mobPitch` 在整个上游驱动里只出现在结构体定义中，从来没有任何代码给它赋值**
  （cotable 由宿主侧自己维护）。我们填 `width * 4` 无害但也无意义 —— 它不是差异点。
- **GBOBJECTS 设备上判断 3D 就绪的唯一判据是 `SVGA3D_DEVCAP_3D != 0`**
  （`vmwgfx_cmd.c:vmw_fifo_have_3d`），**完全不看 `SVGA_FIFO_3D_HWVERSION`**。
  所以日志里 `3d hw version 0x0` / `3d capabilities advertised: 0 of 256`
  是正常现象，不是问题。这条排除了"宿主认为 3D 未就绪所以命令无效"的猜想。

## 38.4 真 bug 一：`dataMobId = 1` 的 MOB depth 违规（已修）

上游 `vmw_mob_pt_populate()` 的规则是硬性的：

```c
if (likely(num_data_pages == 1)) {
        mob->pt_level = VMW_MOBFMT_PTDEPTH_0;      /* base 页就是数据页 */
        mob->pt_root_page = vmw_piter_dma_addr(&data_iter);
} else {
        ret = vmw_mob_pt_populate(dev_priv, mob);   /* 建页表 */
        vmw_mob_pt_setup(mob, data_iter, num_data_pages);
        mob->pt_level += VMW_MOBFMT_PTDEPTH_1 - SVGA3D_MOBFMT_PT_1;
}
```

我们的 `dataMobId = 1` 是 **4 页**（`kGbDataPages = 4`），却写了 `PT64_0`，
并且 base 指向 `ppnDataPt` —— **一张内容全是 PPN64 的页表页**。
宿主于是把那张页当成"第一个数据页"读：不崩、不报错、fence 照过，
surface 1 的内容就是垃圾。

改成 `PT64_1` + `ptData[]`（本来就已经按页表格式填好了）：

```cpp
UInt32 mobDepth = kSvga3dMobFmtPt64_1;      // 上游：>1 页只能 PT64_1
```

这是全项目里**最后一处 MOB depth 违规**。其余（`ptSurface`/`ptOtable`/`ptSt`/
DXCONTEXT 的 2 KiB 表）都是 <=1 页 + `PT64_0`，符合上游 `vmw_setup_otable_base()`
的 `otable->size <= PAGE_SIZE -> PTDEPTH_0` 分支。

**修完这一步，屏还是黑的** —— 说明它不是（唯一的）根因，但它确实是错的，
而且它解释了为什么 `surface 1` 从来读不出东西。

## 38.5 真 bug 二：FIFO 回绕死锁（已修，会复现）

这一轮第一次看到 FIFO 彻底卡死：

```
present tick 1800 (next 262136 stop 262136)
fifo submit refused: 84 bytes needed, 8 free      <-- 之后无限重复
fifo submit refused: 12 bytes needed, 8 free
fifo submit refused: 44 bytes needed, 8 free
fifo submit refused: 28 bytes needed, 8 free
```

根因在 `fifoSubmitWords()` 的空闲量计算：

```cpp
/* 错： */
const UInt32 bytesFree = (next >= stop) ? (fmax - next) : (stop - next);
```

上游（`vmwgfx_cmd.c:vmw_fifo_is_full` / `vmw_local_fifo_reserve`）是：

```c
return ((max - next_cmd) + (stop - min) <= bytes);     /* 满 */
if (next_cmd >= stop) { ... (max - next_cmd) + (stop - min) ... }
else                  { ... (next_cmd + bytes < stop) ... }
```

`next >= stop` 是**已回绕**的状态，此时空闲区由两段组成：
`[next, max)` 和 `[min, stop)`。我们漏掉了第二段。于是 `next` 一旦冲到
`fmax` 附近，`fmax - next` 变成个位数，我们就永远认为"没空间"并拒绝一切命令，
**而实际上 `[min, stop)` 整段是空的**。

修正后：

```cpp
const UInt32 bytesFree = (reNext >= stop) ? ((fmax - reNext) + (stop - fmin))
                                          : (stop - reNext);
```

同时补了三处与上游对齐的行为：

1. **真正满时等待** `STOP` 前进（上限约 3 s，对齐 `vmw_fifo_wait`），而不是直接丢命令。
   丢一条多命令序列的中间一条，会让后半个命令被当成新命令头解析 —— 这是
   "黑屏"变成"FIFO 中止"的经典路径。
2. **`NEXT_CMD` 精确落在 `fmax` 时折回 `fmin`**（上游 `vmw_fifo_commit`：
   `if (next_cmd >= max) next_cmd -= max - min;`）。留在 `fmax` 本身合法但让下一次
   预算的 `(max - next)` 项归零。
3. **检测 `NEXT_CMD` 被别人移动**（重启/控制台接管）时拒绝提交，不去追。

> 这个 bug 之前一直没暴露，是因为呈现循环每帧只写约 90 字节，
> 得跑约 4 分钟才绕一圈。§38 中 `SURFACE_DMA` 每帧 21 dword 把速度提上去，
> 才把它逼出来。**它是真 bug，修掉之后 FIFO 才能长时间稳定。**

## 38.6 真 bug 三：STDU 尺寸常量用错高度（已修，含堆越界写）

`kStduBytes` 和 `kStduPages` 是从 `kStduHeight`（768）算的，而 STDU 相关的一切
实际按 `kStduSurfHeight`（PT64_1 时 = 512）走。后果：

- `kStduPages` 说是 768，而 MOB 和页表只装了 512 页 —— 页表末尾 256 项指向**不存在**的页；
- `stduReadbackProbe()` 里 `bzero(virt, kStduBytes)` 按 3 MiB 清零一个
  **2 MiB** 的分配 —— **越过分配边界写了 1 MiB**。这是内核堆破坏，只是没立刻崩。
- `stduBringUp()` 里 `UPDATE_GB_SCREENTARGET` 的 rect.h 也是 768，
  超出一个只有 512 行的 surface。

全部改为 `kStduSurfHeight`。`kStduHeight` 只保留为注释里的历史值，禁止再用。

## 38.7 真 bug 四：`SVGA3dTransferType` 用了未定义值 0（§37 结论作废）

这是我这一轮最想记下来的错误。`svga3d_types.h`：

```c
typedef enum {
        SVGA3D_WRITE_HOST_VRAM = 1,     /* guest -> host */
        SVGA3D_READ_HOST_VRAM  = 2,     /* host -> guest (readback) */
} SVGA3dTransferType;
```

**没有 0。** 而 `stduReadbackProbe()` 里写的是：

```cpp
dma[8] = 0;     /* "transfer = SVGA3D_READBACK"  <-- 这个枚举值不存在 */
```

§37 的整个结论 —— "READBACK 进清零缓冲区，768 个采样点全 0，所以
`SURFACE_DMA` 在本宿主是 no-op" —— **测的是一条畸形命令**。

更直接的旁证在宿主校验器里（`vmwgfx_execbuf.c:vmw_cmd_dma`）：

```c
dirty = (cmd->body.transfer == SVGA3D_WRITE_HOST_VRAM) ? VMW_RES_DIRTY_SET : 0;
```

它正是按这个字段分派。而 `vmwgfx_stdu.c` 的注释明确说：xorg 的 vmware 驱动
会针对 pixmap 更新发 `CMD_SURFACE_DMA` —— 即这条命令**在屏靶上是被实际使用、
能工作的**。

改成 `dma[8] = 2` 之后重测……**回读依然是全零**。所以：
§37 的**方法**是错的，但它的**结论在这个宿主上碰巧还没被推翻**。
留待 §39 用别的手段验证。

## 38.8 上游 STDU 呈现路径到底发什么（推翻了 §37.4 的推测）

读完 `vmwgfx_stdu.c` 的两条 commit 路径，结论很干脆：
**上游 STDU 呈现链路里没有 `UPDATE_GB_IMAGE`。**

- **CPU 路径**（`vmw_stdu_bo_cpu_commit`）：`vmw_bo_cpu_blit()` 直接 CPU 拷进 BO，
  然后**只发** `UPDATE_GB_SCREENTARGET`。`fifo_reserve_size = 0`，一个命令都不发。
- **GPU 路径**（`vmw_kms_stdu_surface_fifo_commit`）：`SURFACE_COPY`（源 sid -> 显示 sid）
  + `UPDATE_GB_SCREENTARGET`；源 sid 就是显示 sid 时**连 COPY 都不发**。

所以 §37.4 抄的那句"M6 靠 UPDATE_GB_IMAGE 生效"的推测**是错的**，
`UPDATE_GB_IMAGE` 在呈现链上是个纯噪声命令。当前循环真正的形状应当对齐
**CPU 路径**：写 MOB 内存 -> 一个 `UPDATE_GB_SCREENTARGET`。

`vmw_stdu_populate_update()` 也顺带核实了 rect 的语义是 `{x, y, w, h}`
（不是 l/t/r/b），我们传 `0,0,1024,512` 是对的。

## 38.9 对照实验（失败但信息量大）：直接写 VRAM + legacy UPDATE

为了把"宿主读不到 GB surface"和"宿主读不到任何东西"分开，
加了 `kStduVramControl`：每帧把同样的图案写进 **BAR1（legacy framebuffer）**，
并额外发 `SVGA_CMD_UPDATE`。

**结果：VNC 依然纯黑，1 种颜色。**

这个结果本身就有结论：宿主一旦把 screen type 切成 `ScreenTarget`，
**legacy 的 `UPDATE` 就被忽略**，像素只从绑定到屏靶的 surface 来。
所以对照组设计有缺陷（它验证的是一个已知结论），
不过它也排除了"VNC/编码器坏了"这一类底层猜测 —— 通路本身没坏，
只是内容源被换了。

> `kStduVramControl` 是诊断开关，会把屏内容毁掉，答完就该关回 0。

## 38.10 §38 净结论

**已修（确定是 bug，与黑屏的因果未定）**

1. `dataMobId = 1` 用 `PT64_0` 描述 4 页 —— 改为 `PT64_1`。
2. FIFO 回绕时漏算 `[min, stop)` 段 —— 回绕后永久拒绝提交（**会复现的死锁**）。
3. `kStduBytes`/`kStduPages` 用 768 而非 512 —— 页表多 256 项悬空 +
   `bzero` 越界写 1 MiB。
4. `SURFACE_DMA.transfer = 0`（未定义枚举）—— 改为 `READ_HOST_VRAM = 2`。

**已确定排除**

- PT64_2 两级表遍历（§38.1）
- 屏靶 / VNC 拓扑未生效（§38.2）
- 所有结构体尺寸与字段（§38.3）
- `mobPitch`（上游从不赋值）
- 宿主认为 3D 未就绪（GBOBJECTS 只看 `DEVCAP_3D`，=1）
- IOMMU（§37 已排除：`IOMapper=0`、`AppleVTD=0`）

**还没解释的核心事实**

宿主接受每一条命令、零报错、`stop` 每次都瞬间追平 `next`、屏靶定义成功，
但**内容永远是零**（VNC 全黑 + `READ_HOST_VRAM` 回读全零）。
`region encoder static` 说明宿主从未认为画面有更新。

## 38.11 下一步（§39 的候选，按信息量排序）

1. **`UPDATE_GB_SCREENTARGET` 的 rect 是否被宿主用来做"脏区"判定**：
   若宿主只在 rect 与"上次已呈现区域"求差时才搬像素，`region encoder static`
   就说得通了 —— 每一帧都是同一个全屏 rect，差集为空。改为交替发不同子矩形
   （例如奇数帧左半、偶数帧右半）可以验证。
2. **用 `SURFACE_COPY` 走 GPU 路径**：造一个 sid 3 的普通 surface，
   把图案写进去 -> `SURFACE_COPY {src: 3, dest: 2}` -> `UPDATE_GB_SCREENTARGET`。
   这条路径命令语义更"重"，宿主必须真的读一块 surface 内存才能完成 copy，
   如果它能出画面，就说明"直写 MOB 被宿主忽略"而 COPY 不会。
3. **验证 `_stduVirt` 的物理页在宿主 DMA 视角是否可达**：
   当前是 `inTaskWithPhysicalMask(..., 0xFFFULL)` 的普通分配。
   上游 MOB 背后是 `TTM_PL_MOB` 的 BO，走 GMR/TTM 管理。
   可以在 `_stduMem` 上挂 `IODMACommand` 拿到设备视角地址，
   与 `getPhysicalSegment()` 的 PPN 对比 —— 若 gfx 侧的地址翻译层不是 1:1，
   这就是最后一块拼图。

## 38.12 决定性割离：完全不用页表的 MOB，屏仍全黑（2026-11-03 03:22）

§38 之前的所有实验都在同一个阴影下：宿主读零，可能是**页表解析失败**（PPN64 编码、
页数取整、PT 深度），也可能是**呈现链不采样**，两者混合在一条路径上，任何测量都分不清。

`kStduFlat` 把前者从画面里彻底删除：

- MOB 定义成 `PT64_0`，`size = 4096`，`basePpn` 就是这个 4 KiB 页本身。
  宿主走的是 `otable 条目 -> basePpn -> 数据`，**中间没有任何页表**。
  （mksSandbox 显示宿主接受 PT64_0——DX 上下文的 MOB 就是 PT64_0。）
- surface 声明为 `1024x1`（pitch 4096×1 = 4096 = 一个页），全部 payload 正好一页。
- 屏靶仍是完整的 1024×512，`struct` 其余字段、sid、mobid、stid、命令顺序
  与正常路径逐字节相同。

结果（03:22:01 那次启动）：

```
flat: MOB 2 defined (PT64_0, base ppn 114147, 4096 bytes)
flat: surface 2 defined 1024x1 on mob 2, bound and updated
flat: screen target 0 (1024x512) bound to surface 2 -- STDU present path is UP, page table NOT involved
flat: present armed (1024x512 panel, 4 KiB MOB, colour 0xff00ff00) first word 0xff00ff00
```

VNC 截图：1024×512，**distinct = 1，纯黑 (0,0,0) 524288 px**。
宿主 `vmware.log` 同一次启动内仍然只有：

```
SVGA-ScreenMgr: Screen type changed to ScreenTarget
SWBScreen: Screen 1 Defined: xywh(0, 0, 1024, 512) flags=0x2
MVNCEncode: screen: 0 BoundingBox: (1024x512) bytesPerLine: 4096
MVNCRegionEncoder_Create: region encoder static. Screen 0/1 @ Resolution: 1024 x 512
```

之后零条 region update。

**结论：页表是清白的。** 无论 §38 里那些 PT64_1/PT64_2/页表布局的分析有多细，
它们都不是黑屏的原因。剩下的可能性只存在于「宿主是否采样我们绑定的 surface」
这一步。

顺带记一个副作用：`kStduFlat = 1` 让 `stduBringUp` 的前半段成为不可达代码，
优化器把它们整个删掉，`build.sh` 的可达性检查因此报 5 条 `MISSING`
（`surface %u bound to mob %u`、`UPDATE_GB_SURFACE accepted`、
`UPDATE_GB_SURFACE refused`、`pt: VERDICT %s`、`UPDATE_GB_MOB_MAPPING refused`）。
这不是构建坏了 —— 是那两个模式互斥。`build.sh` 已改为按开关二选一：
`kStduFlat = 1` 时检查 flat 的 4 条标记，否则检查原来那 8 条。
保留这个检查比删掉它更有价值，因为它恰好证明了「mode B 真的是死代码」，
而不是「我以为编译器会删、其实它没删」。

## 38.13 回读宿主副本：一条被自己的对照实验推翻的证据（2026-11-03 03:33）

思路：`READBACK_GB_IMAGE`（1103，`{sid, face, mipmap}` = 12 字节）让宿主把它
**内部持有的 surface 内容**写回该 surface 当前绑定的 MOB。所以造一个独立的
scratch MOB（mobid 102，同样 PT64_0，独立分配），把 surface 重绑上去再回读：

- 先把页填成一个从未显示过的橙色 marker `0xFF00A5FF`
- 发 `UPDATE_GB_IMAGE` + `UPDATE_GB_SCREENTARGET`（与呈现循环完全相同的两条）
- 重绑 surface 2 -> mobid 102
- 请求 readback，fence 保证比较发生在宿主写完之后

结果：

```
flatrb: host surface 2 -> 0/1024 words nonzero, 0 match the marker 0xff00a5ff
        VERDICT *** host holds nothing ***
```

第一眼这像是重磅证据：宿主自己的副本里一个字节都没有，连刚写进去的 marker 都没有。

**然后加了对照组，它就作废了。** 紧接其后，在**不修改源页**的情况下再发一次
readback（此时 surface 仍绑在 scratch MOB 上，内容仍是 marker）：

```
flatrb: control readback -> 0/1024 words nonzero
        -- READBACK writes nothing on this host
```

对照组的结论是：**`READBACK_GB_IMAGE` 在这台宿主上根本不写入任何东西。**

于是第一次的「0/1024」只能说明 readback 不工作，**不能**说明宿主副本是空的。
这条证据被自己的对照组推翻了 —— 记下来是因为它是个容易重复踩的坑：
上一版 §37.3 的「READBACK 全零 ⇒ DMA 是 no-op」也建立在同一个未经验证的前提上，
同样不成立。**凡是靠 readback 得出的结论，在这台宿主上都不可信。**

`GB_SCREEN_DMA`（1131，宿主把屏幕内容写进 guest MOB）本来是同方向的另一条通路，
但被宿主拒绝（`sd: GB_SCREEN_DMA refused`）——这与上游 `vmwgfx_cmd.c` 里
`SVGA_3D_CMD_GB_SCREEN_DMA` 标 `vmw_cmd_invalid`（Linux 自己也从不发）一致。

**所以现在「宿主内部有没有我们的像素」这个问题，在 guest 侧没有任何可用的
读取手段。** 只能从宿主侧看。

## 38.14 当前状态与下一步

已经确定的事实（每一条都有对照）：

1. GB 对象模型起来了：otable、MOB、surface、屏靶全部定义成功，宿主零报错。
2. 页表清白（§38.12 的无页表对照）。
3. 宿主的 MKS 确实切到了 ScreenTarget 模式，并设好了 1024×512 拓扑。
4. 宿主的 region encoder 此后**再也没有产生过一次 region update**。
5. guest 侧无法回读宿主副本（`READBACK` 不写入，`GB_SCREEN_DMA` 被拒）。

第 4 条是唯一还站得住的、指向呈现链的硬证据。

下一步只剩两个方向，都不再依赖 guest 侧回读：

**A. 让宿主自己说。** 宿主日志里 `svga.forceTraces = "TRUE"` 已开，
但 `vmware.log` 里没有任何 FIFO 命令级 trace。需要找到 Workstation 26 里
把 svga 命令流写进日志的开关（候选：`svga.traceLevel`、`svga.commandLog`、
hostd 侧的 `mks.forceTraces`），让宿主打印它实际处理了哪些命令、
以及 `UPDATE_GB_SCREENTARGET` 之后它往编码器丢了什么。

**B. 换一条完全不同的呈现通路做对照。** §38.12 的 `SURFACE_COPY` GPU 路径
（建 sid 3 普通 surface，`SURFACE_COPY {src:3, dest:2}` + `UPDATE_GB_SCREENTARGET`）
还没跑过。它和 CPU 路径的区别在于：宿主必须真的解引用一块 surface 内存才能完成
copy，如果它能出画面，说明「直写 MOB」这条路本身被宿主忽略。

**优先级：A > B。** 因为 B 仍然落在同一条 screen target 链上，
如果 4 号事实的原因在「宿主为什么不刷新编码器」，B 会以同样的方式黑屏；
而 A 能把这个问题从猜测变成读日志。

**另一个必须重新评估的前提**：`svga.guestBackedPrimaryAware = "TRUE"` 是
Workstation 的屏幕管理策略开关，社区里大量「VMware 黑屏」报告的第一处置办法就是
把它改成 `FALSE`。它和 `ScreenTarget` 模式直接相关。§38 全程都是 TRUE。
在启动 B 之前，先用它做一次单变量对照是更低成本的选择 —— 注意改 `=FALSE` 会让
宿主回到 RegisterMode（legacy 帧缓冲），那时 §38 之前的 legacy 呈现路径
（`SVGA_CMD_UPDATE` + BLIT_GMRFB_TO_SCREEN，M6 曾成功的那条）才是对的那条路。

## 38.15 宿主侧 trace 开关：被接受，但没有任何命令级输出（2026-11-03 03:37）

在 vmx 里加了四个候选键，虚拟机**关机状态下**写入（`vmrun -T ws start ... nogui` 之前）：

```
svga.traceLevel = "9"
svga.commandLog = "TRUE"
svga.logCmdStream = "TRUE"
mks.sandbox.log.fifoLevel = "9"
```

`vmware.log` 的 `DICT --- CONFIGURATION ... .vmx` 段里四行都出现了
（清单在 500/506/507/508 行），说明 **vmx 语法有效、被读进了配置字典**。
但整份日志里没有一行 FIFO 命令级 trace：没有 `SVGA` cmd id、没有 `UPDATE_GB_SCREENTARGET`、
没有 FIFO pointer 移动记录。`svga.forceTraces = "TRUE"` 本来也一直开着，同样没有输出。

**结论：这个 Workstation build 的命令级 trace 不经过 `vmware.log`。** 方向 A 到此关闭，
不再花时间找开关。

**顺带发现一条更重要的东西**：`svga.guestBackedPrimaryAware` 在**开机瞬间被重写**。
把 vmx 改成 `"FALSE"`、虚拟机确认关机，`vmrun start` 之后该键又变回 `"TRUE"`，
vmx 的 mtime 正好等于 `vmware.log` 里 `19:49:25.028`（PowerOn 那一刻）。
日志 493 行的 DICT 打的就是 `"TRUE"`，也就是说 §38.16 第一次尝试**根本没生效**，
不是「改了没用」，而是「改了被改回去了」。

它是 Workstation 的**规范化键**（类似 `cleanShutdown`、`ethernet0.generatedAddress`），
`vmx` 里写什么都会被规范化写回覆盖。结论：**这个开关在本机上无法用 vmx 改**，
§38.16 的单变量对照不能靠它做。

## 38.16 决定性对照：宿主 legacy 通路在同一颗 kext 下能出画面，ScreenTarget 不能（2026-11-03 04:0x）

既然 vmx 改不动 `guestBackedPrimaryAware`，就换一个问法 —— 这个问法**不需要改任何宿主配置**：

> 宿主在开机后**自己**会先进 RegisterMode（legacy 帧缓冲，1024×768，
> `SWBScreen: Screen 1 Defined`），约 25 秒后才因为我们发出的
> `DEFINE_GB_SCREENTARGET` 切到 ScreenTarget（1024×512）。
> **那 25 秒里，屏是黑的还是亮的？**

如果是亮的：legacy 通路在同一颗 kext、同一台宿主上本来就工作，
那么「黑屏」是 ScreenTarget 这条链的属性，而不是更上游（FB/VRAM 映射）的问题。
如果是黑的：问题在 kext 加载之前，跟呈现通路无关。

**做法**：关机 → 冷启动 → 从 t0 起每秒 VNC 采样一次，共 45 次
（`tools/vncsnap.py`，脚本 `/tmp/regcap.sh`）。采样起点就是 `vmrun start` 返回的那一刻。

**结果（`.shots/reg/t*.png`，按实际秒数命名）**：

| 采样 | 尺寸 | 颜色数 | 主要内容 |
|---|---|---|---|
| t00 | 640×480 | 1 | 纯黑（VNC 刚连上，还没画面） |
| t03 | 1024×768 | 1 | 纯黑 |
| **t04** | 1024×768 | **92** | **"VMware" 引导 logo**（bbox x[292..743] y[349..418]） |
| t06–t08 | 1024×768 | 17 | 切换到 Apple 启动界面 |
| **t10–t27** | 1024×768 | **29** | **Apple logo + 底部进度条**（进度条逐秒变长：906→618 px） |
| t29 起 | 1024×512 | 1 | **纯黑**（已切到 ScreenTarget） |

t04 的内容用 ASCII 位图还原出来是清清楚楚的 "VMware" 五个字母；
t16 是 Apple logo 的大圆环 + 屏幕底部一条进度条。两者都不是噪声。

**这是 §38 全程第一个「屏上有东西」的观测。**

### 判定

1. **legacy 帧缓冲通路可用**，而且是在**同一颗 kext 加载的同一台机器**上、
   同一份 FB 映射、同一段 VRAM 下可用。上游（PCI 映射、VRAM、`enableController`）
   全部清白 —— 这一条把 §38 之前所有「FB 没映射上」的猜测一次性排除了。
2. **黑屏是 ScreenTarget 这条链的属性。** 宿主一旦被我们的
   `DEFINE_GB_SCREENTARGET` 切过去，屏就再没亮过，哪怕 24 秒前它还在正常输出
   Apple logo。§38 里「region encoder 从未产出 region update」这条硬事实，
   根因不在编码器，而在 **ScreenTarget 模式本身在这个 Workstation build 上不出图**。
3. **§38.14 的优先级判断要改**：当时把方向 A（让宿主说话）排在 B（换通路）前面。
   现在 A 已确认关闭，而 B 的变体 —— **回到 legacy 通路** —— 从「候选」升级为
   **唯一已知能出画面的方案**。

### 代价与下一步

legacy 通路的代价是：**分辨率必须由宿主在 RegisterMode 里决定**（1024×768），
我们不能再自己 `DEFINE_GB_SCREENTARGET` 成 1024×512。M6（commit b89c619）
当年成功的那条路就是 `SVGA_CMD_UPDATE` + `SVGA_CMD_BLIT_GMRFB_TO_SCREEN`，
它不需要 screen target 对象，直接操作 host 端的可见帧缓冲。

下一步（§39）：把 kext 的呈现后端从 `kStduFlat` 切回 legacy：
- 不调 `DEFINE_GB_SCREENTARGET`（让宿主留在 RegisterMode）
- 用 `SVGA_CMD_BLIT_GMRFB_TO_SCREEN` 从我们的 MOB/GMR 往屏上 blit
- `SVGA_CMD_UPDATE` 声明脏矩形

这样做的直接后果：**宿主不会切到 ScreenTarget，那 24 秒能显示 Apple logo 的通路会保持可用。**

## 38.17 采样器本身在制造它想测的故障（2026-11-03 04:00）

§38.16 的结论要先打一个折扣，因为第一次对照不干净，第二次才找到真凶。

### 第一次对照为什么不干净

`/tmp/regcap.sh` 里用 `tools/mssh.py --sudo "/sbin/shutdown -h now"` 关机，
然后轮询 `vmrun list` 等它消失。结果：`off after 30s` 立刻打印，`vmrun start` 紧跟着执行。
但客户机的 `kern.boottime` 显示它当时**根本没关过**（`up` 时间连续）。
macOS 经 SSH 收到的 `shutdown -h now` 需要 admin 授权，静默失败。
`vmrun list` 那 30 秒里返回空只是关机过程的短暂空窗，脚本据此判定「已关」是错的，
`vmrun start` 于是重新 attach 到**同一个活实例**上。

那次采到的 t04「VMware」logo 和 t16「Apple logo + 进度条」是真实的，
但它们属于**一次正在进行的启动**，不是冷启动对照。

**教训**：关机一律用 `vmrun -T ws stop "$VMX" hard`，它不问客户机、立即生效。
轮询确认要等 `vmrun list` **稳定**返回 0 条，并且和客户机 `kern.boottime` 对得上。

### 第二次对照（`/tmp/regcap2.sh`，强制关机 + 每秒单帧采样）

时间轴（客户机 boot `03:59:0x`，宿主日志 UTC `19:59:0x`）：

| 宿主时刻 | 事件 | 采样 | 画面 |
|---|---|---|---|
| 19:59:07.493 | `Screen type changed to RegisterMode`，Screen 1 = 1024×768 | t04– | 黑 |
| 19:59:0x | 客户机 boot，Apple 阶段 | **t06–t33** | **Apple logo + 进度条，27 秒** |
| 19:59:38.284 | `Guest display topology changed: numDisplays 0` | | |
| 19:59:38.284 | `SWBScreen: Screen 1 Destroyed: 1024x768` | | |
| 19:59:38.459 | `Screen type changed to ScreenTarget`，Screen 1 = 1024×512 | **t35–** | **全黑** |

看起来是干净的「legacy 亮 / ScreenTarget 黑」。**但紧邻 `numDisplays 0` 的上一行才是关键：**

```
19:59:38.019  mks  SOCKET 31 (2952) recv detected remote shutdown
19:59:38.019  mks  SOCKET 31 (2952) VNC Remote Disconnect: socket closed.
19:59:38.019  mks  MKSControlMgr: Remove VNC connection 22
19:59:38.020  mks  MKSRemoteMgr: CleanupConnection (numConsoles=0)   <-- 角标归零
19:59:38.284  svga Guest display topology changed: numDisplays 0
19:59:38.284  svga SWBScreen: Screen 1 Destroyed: xywh(0,0,1024,768)
19:59:38.459  svga Screen type changed to ScreenTarget
```

`SOCKET 31` 是 **`vncsnap.py` 自己的连接**。它拍完一帧就 `close()`，
MKS 把「最后一个控制台断开」当成没人看了，于是摘掉显示器 → `numDisplays 0`
→ ScreenMgr fallback 到 1024×512 ScreenTarget。

**也就是说：每秒重连一次的采样循环，每拍一帧就把宿主往 ScreenTarget 踢一次。**
它测到的不是驱动的行为，是它自己的副作用。VNC 断开与切换之间隔了 265 ms，
因果顺序明确。

这个结论往回追溯，对 §38 全程都有影响：凡是「宿主的屏幕模式不对」的观测，
都要先问一句「采样器当时连着吗、连了几个」。

### 修正后的实验纪律（以后所有屏幕实验都适用）

1. **采样必须长连接**。为此新增 `tools/vncloop.py`：一次握手，连续取 N 帧，
   整个过程不关 socket。（`vncsnap.py` 仍适合拍单张，但**不能在循环里用**。）
2. **每次实验前记录 `numConsoles`**。宿主日志里 `AddRemoteConsole` /
   `CleanupConnection (numConsoles=N)` 是硬指标。
3. 判定「宿主切了模式」时，必须同时看**断开事件**和 `numDisplays`，
   不能只看 `Screen type changed`。

### 尚未回答的问题

- 如果全程保持一条连接，宿主会不会一直停在 RegisterMode、画面一直亮？
  → 正在跑 `/tmp/regcap3.sh`（强制关机 → 启动 → `vncloop` 单连接 60 帧）。
- 如果会，那么「ScreenTarget 不出图」这件事**有可能从头到尾就不存在**，
  §38 的整个方向要重估。

## 38.18 真凶：这台客户机从未安装 VMware Tools，宿主因心跳超时放弃 legacy 模式（2026-11-03 04:06）

§38.17 里我把「切 ScreenTarget」归因于采样器的 VNC 反复断连。**这个归因是错的**，
`vncloop.py` 单连接重跑一次就推翻了它。真正的触发者在这份日志里写得很直白。

### 单连接对照（`/tmp/regcap3.sh` → `tools/vncloop.py`，一条 socket 贯穿启动）

宿主 `vmware.log`（UTC，客户机本地 = UTC+8）：

| 宿主时刻 | 客户机本地 | 事件 |
|---|---|---|
| 20:04:09.276 | 04:04:09 | `Tools: sending 'OS_PowerOn'`，`Delaying state change request` |
| 20:04:10.190 | 04:04:10 | `Tools: Running status rpc handler: 0 => 1`（宿主给的初始值） |
| 20:04:10.652 | 04:04:10 | `Screen type changed to RegisterMode`，Screen 1 = **1024×768** |
| 20:04:16.357 | 04:04:16 | `AddRemoteConsole (numConsoles=0)` ← **唯一的 VNC 连接，全程未断** |
| — | 04:04:18 | 客户机 kernel boot（`kern.boottime`） |
| 20:04:35.136 | 04:04:35 | **`Tools: Tools heartbeat timeout`**（上次心跳 20 秒前） |
| 20:04:35.136 | 04:04:35 | `Running status rpc handler: 1 => 0` |
| 20:04:35.756 | 04:04:35 | `Guest display topology changed: numDisplays 0` |
| 20:04:35.756 | 04:04:35 | `SWBScreen: Screen 1 Destroyed: 1024x768` |
| 20:04:35.902 | 04:04:35 | `Screen type changed to **ScreenTarget**`，Screen 1 = **1024×512** |
| — | 04:04:37.328 | **kext `start`** —— 比切换晚 1.6 秒 |
| — | 04:04:37.491 | kext 发 `DEFINE_GB_SCREENTARGET`，绑到那个已被抛弃的空靶上 |
| 20:04:36.413 | 04:04:36 | `MVNCEncodeBackend: Bogus expose rectangle 0,0 1024x768 mode 1024x512` |
| 20:04:36.421 | 04:04:36 | `MVNCEncode: pump failed, error 5` → `VNC Error (5)` |
| 20:04:36.422 | 04:04:36 | `CleanupConnection (numConsoles=0)` ← **宿主踢掉连接**（不是我们主动关） |

三个关键点：

1. **VNC 全程只有一条连接，`numConsoles` 从未归零直到被宿主踢掉。**
   `numDisplays 0` 出现在 `20:04:35.756`，而连接是 `20:04:36.422` 才被清理 ——
   **切换在前，断连在后，因果顺序和 §38.17 说的相反。**
2. **触发者是 `Tools heartbeat timeout`。** 这跟 VNC 无关、跟我们的 kext 也无关。
3. **kext 是切完之后 1.6 秒才加载的。** 我们从未「让」宿主切模式 —— 我们只是
   接手了一个宿主已经放弃的、几何被改成 1024×512 的空靶。

### 根因取证

追到这一步只剩一个问题：为什么 Tools 心跳会超时？答案：

```
$ ls "/Library/Application Support/VMware Tools/"   -> 不存在
$ ps aux | grep -i vmware                            -> 空
$ sudo launchctl list | grep -i vmware               -> 空
$ kmutil showloaded | grep -i vmware                 -> 空
$ ls /Library/LaunchDaemons/ | grep -i vm               -> 空
```

**这台客户机上从来没有安装过 VMware Tools。** `/Library/Application Support/`
下只有 Apple、BTServer、CrashReporter、Mozilla、ProApps、Script Editor 六个目录。

`Tools: Running status` 是 guest 通过 **GuestRpc backdoor** 主动上报的，
而唯一的常规上报者就是 VMware Tools。Tools 不存在 ⇒ 宿主在开局 20 秒后收不到心跳
⇒ 判定「客户机没有活着的显示」⇒ `numDisplays 0` ⇒ 销毁 legacy Screen 1
⇒ fallback 到 ScreenTarget 1024×512 ⇒ 我们所有 3D 呈现都画在一个死靶上。

### 这条结论把 §38 整段重新定性

§38.1–§38.14 试过的所有变量 —— MOB 格式（PT64_0/PT64_1/PT64_2）、页表层次、
surface 尺寸、`UPDATE_GB_SURFACE` vs `UPDATE_GB_IMAGE`、rect 语义、
`READBACK` / `GB_SCREEN_DMA`、DX 上下文 —— **全部不是根因**。
它们的共同前提是「宿主正用 ScreenTarget 呈现我们的 surface」，而这个前提从一开始就不成立：

- 宿主进 ScreenTarget 是因为**它以为客户机死了**，不是为了配合我们；
- 那个 ScreenTarget 是 fallback 产物，几何 1024×512 与 legacy 的 1024×768 不一致，
  VNC encoder 在两者之间来回撞（`Bogus expose rectangle`），pump 直接失败；
- 于是**哪怕我们的 GB 对象模型完全正确、宿主零报错，屏上也不可能有东西**。

§38.12 的「页表清白」、§38.16 的「legacy 能出画面」两条结论依然成立，
而且现在有了统一的解释：**legacy 能出画面正是因为那 25 秒里 Tools 心跳还没超时。**

### 下一步：两条候选，优先 B

**A. 装 VMware Tools。**
`darwin.iso` 在宿主 `C:\Program Files\VMware\VMware Workstation\darwin.iso`，
客户机 macOS 15.7.3 + SIP kext 签名已关，装得上。
**但代价很高**：Tools 会带 `VMwareGfx.kext`，它极可能抢绑 `pci15ad,405`，
把自研 `vgpuFramebuffer` 挤出局 —— 直接毁掉本项目的测试环境。
**否决**，除非只装用户态部分（安装包不支持拆分）。

**B. 在 kext 里直接发 GuestRpc 心跳。**
心跳走 **GuestRpc backdoor**，不需要 Tools 的任何组件：

```
eax = 0x564D5868 ("VMXh")      magic
ecx = cmd                      BDOOR_CMD_MESSAGE = 30，低 16 位是命令
ebx = channel                  RPCI channel number
edx = 0x5658 (low bandwidth port)
执行 inl (%dx), %eax           一次调用；返回 eax = 状态
```

Tools 上报 running status 用的 RPC 是 `Set_Option` / 或直接的
`vmx.capability.unity`-系调用；最小可行集合是发一个
`tools.capability.running_status` 系的心跳包，让宿主持续收到心跳即可。

kext 里已经有 `outl`（`vgpuOut32`）和完整的端口 IO 前提，
加一个 `inl` 包装和一条 `BDOOR_CMD_MESSAGE` 就够了。**代价远低于装 Tools，且不碰显卡绑定。**

**B 必须先验证的一件事**：宿主现在就处于 ScreenTarget。
如果 kext 开始发心跳，宿主会不会**主动切回** RegisterMode？
如果会 —— 那 §38 的整个方向可以收尾，呈现改走 legacy + `BLIT_GMRFB_TO_SCREEN`（M6 那条路）。
如果不会 —— 说明宿主「一旦 fallback 就不回头」，那就必须保证**心跳在第一次超时之前就起来**，
也就是 kext 要尽早加载（现在 kext 加载时刻 04:04:37 晚于超时 04:04:35 只有 2 秒，
把 kext 提前到 boot 后 20 秒内即可，可行性取决于 IOKit 的匹配时机）。

## 38.19 kext 自己发 GuestRpc 心跳 —— 通路已验证可用（2026-11-03 04:16）

### 实现

`vgpuFramebuffer.cpp` 里新增：

```c
static const UInt32 kBdoorMagic  = 0x564D5868;   // "VMXh"
static const UInt16 kBdoorPort   = 0x5658;
static const UInt32 kBdoorCmdMsg = 30;           // BDOOR_CMD_MESSAGE

static inline UInt32 vmwareBackdoor(UInt32 cmd, UInt32 arg1, UInt32 arg2) {
    UInt32 eax = kBdoorMagic, ebx = arg1, ecx = cmd, edx = (UInt32)kBdoorPort;
    __asm__ volatile ("inl %%dx, %%eax"
                      : "+a" (eax), "+b" (ebx), "+c" (ecx), "+d" (edx) : : "memory");
    return eax;
}
```

挂点放在 `presentTick()` 的**最前面**，在所有 early return 之前 —— 因为 `_presentFifo == nullptr`
或 `!_deviceModeValid` 恰恰是最需要心跳保活的状态。

### 第一次运行：错误在节流方式，不在通路

节流最初用 `_presentTicks % 150 == 0`（按 33 ms/tick 算「5 秒」）。实测心跳日志：

```
04:14:25.174  rpc: heartbeat #1 -> status 0x0
04:15:00.461  rpc: heartbeat #2 -> status 0x0      <- 间隔 35 秒
04:15:05.613  rpc: heartbeat #3 -> status 0x0
04:15:15.958  rpc: heartbeat #5 -> status 0x0      <- 每 10 秒
04:15:26.251  rpc: heartbeat #7 -> status 0x0
```

**`presentTick` 的真实周期远大于 33 ms**（present 路径自身要跑命令，机器又只有 2 vCPU），
所以「150 tick」根本不是「5 秒」。宿主超时是 20 秒，第一次心跳到第二次之间空了 35 秒，
中间必然超时 —— 日志也确实是：

```
20:14:23.263  Tools: Tools heartbeat timeout.
20:14:23.718  Guest display topology changed: numDisplays 0
20:14:23.854  Screen type changed to ScreenTarget
```

改成**绝对时间节流**（`mach_absolute_time()` + `absolutetime_to_nanoseconds()`，
2 秒一次，对 20 秒超时有 10 倍余量），不再依赖 tick 周期。

### 但有一个**决定性的好消息**：宿主确实把我们的心跳当成 Tools 心跳

同一次启动，宿主日志里：

```
20:14:24.264  Tools: Running status rpc handler: 0 => 1.
20:14:24.264  Tools: Changing running status: 0 => 1.
20:14:24.264  Tools: [RunningStatus] Last heartbeat value 5 (last received 0s ago)
```

`20:14:24.264` 对应客户机本地 `04:14:25.1` —— 正是 kext 心跳 #1 的时刻
（`04:14:25.174`，两个时钟差不到 1 秒）。**宿主接受了这个由我们自研 kext 发出的、
零长度的 `BDOOR_CMD_MESSAGE`，并据此把 running status 从 0 翻回 1。**

这条验证的价值：**方案 B 可行，不需要安装 VMware Tools。**
剩下的问题只是「心跳要足够密、且要在第一次超时之前就开始」——
前者已经用绝对时间节流解决，后者是加载时机问题（kext 目前比超时晚 1.6 秒加载）。

## 38.20 心跳生效了，宿主不再切 ScreenTarget —— 但画面仍被 CONFIG_DONE 掐断（2026-11-03 04:31）

### 把心跳从 presentTick 里搬出来

§38.19 的心跳挂在 `presentTick()` 上，实测**间隔 35 秒**，远超宿主 20 秒超时。原因是
`presentTick` 本身会长时间不被进入：

```
04:25:05.722  heartbeat #1  tick 1
04:25:07.745  heartbeat #2  tick 60     <- 2 s，正常
04:25:38.662  heartbeat #3  tick 61     <- 31 s 之后，tick 只前进了一个
```

`thread_call_enter_delayed` 在同一个 call 已 pending 时返回 FALSE 不排队，而 present 主体
本身也不便宜（要跑 FIFO 命令）。**骑在 present 上的心跳会继承这个停摆。**

改成**独立的 thread_call**（`rpcTickC`），在自己的回调里重新 arm，与 present 完全解耦；
并且在 `start()` 里 `super::start()` 之后**立刻**启动（backdoor 端口不需要 FIFO、
不需要 aperture、不需要模式设置），这样即使后面的设备初始化全失败，心跳也照发。

### 结果 1：宿主不再切 ScreenTarget

本次启动的完整时间轴（客户机本地 = 宿主 UTC + 8）：

| 客户机时刻 | 事件 |
|---|---|
| 04:29:54.5 | 客户机 boot |
| 04:29:45.7 | `Screen type changed to RegisterMode`，Screen 1 = 1024×768 |
| 04:29:44.6 | `Running status rpc handler: 0 => 1`（EFI 固件发的那一次） |
| 04:30:10.6 | `Tools heartbeat timeout`（boot+16 s，最后一次 RPC 是 26 秒前的固件那次） |
| **04:30:13.2** | **`Screen type changed to ScreenTarget`** |
| 04:30:14.8 | kext `start`（boot+20.3 s） |
| 04:30:14.8 | **`rpc: heartbeat armed (every 2000 ms)`** |
| 04:30:15.6 | `Running status rpc handler: 0 => 1` ← **心跳把状态拉回** |
| 04:30:16.8 | `rpc: heartbeat #1 -> status 0x0` |
| 04:30:28.4 | `#2` |
| 04:30:45.9 | `#3` |
| 04:31:39.9 | `#30`（30 次心跳 / 83 秒 ≈ 2.8 s/次，比设定的 2 s 慢但不影响） |

VNC 采样（单连接，`.shots/rpc4/`）**全程 1024×768**，一次都没有出现 1024×512：

| 帧 | 尺寸 | 非黑像素 | 内容 |
|---|---|---|---|
| f00 | 1024×768 | 4620 | Apple logo（4317 个 254,254,254） |
| f01 | 1024×768 | 1404 | 启动界面（38,38,38 灰） |
| f03–f22 | 1024×768 | 96–192 | 只有几十个灰像素在动（光标/进度微光） |

**这是 §37/§38 全程第一次做到「宿主整段没有切走」**。心跳确实把宿主钉在了 legacy 模式上。
（严格说，`04:30:13` 宿主切过去了一次，`04:30:15` 心跳把它拉回 running status=1，
之后 `Screen type` 再没变过，采样尺寸也稳定在 1024×768。）

### 结果 2：画面仍然在 Apple logo 之后死掉

legacy 模式回来了，但 f01 之后基本全黑。这和 §38.16 那个「能完整显示 Apple logo + 进度条
直到 27 秒」的对照**不同** —— 差别正是 kext 本身：**这一次 kext 写了 `CONFIG_DONE`**。

kext 源码里 2026-10-06 的实测注释早就写下了这件事：

> MEASURED (2026-10-06 bisect): with 3D enabled, **stage 2 alone freezes presentation** --
> the moment the guest writes CONFIG_DONE, the MKS switches to the screen-target
> presentation model and the legacy framebuffer auto-scanout is disabled.
> `SVGA_CMD_UPDATE` (acked!) does not restore it.

现在这条老观测和本轮的新观测拼成了一个完整的机制：

1. 宿主开机进 RegisterMode（legacy 自动扫描输出），画面正常；
2. **Tools 心跳超时**（本机没装 Tools）→ 宿主判客户机死 → 切 ScreenTarget；
3. kext 迟到 1.6–4 秒加载，写 `CONFIG_DONE` → 又**主动**加固了「不用 legacy 扫描输出」这个状态；
4. 我们的 GB/ScreenTarget 呈现全部画在一个宿主已经抛弃的靶上 → 黑屏。

也就是说 **`CONFIG_DONE` 和「心跳超时」是同一个方向的两道门**，任何一道关上画面就没了。

### 下一步：单变量关掉 CONFIG_DONE（`kFbStage = 0`）

这是现在唯一没做过、且成本最低的对照：

- `kFbStage = 0` 时 `probeFifo()` 只读不写，**不写 `CONFIG_DONE`**，不发任何 FIFO 命令；
- 心跳照发（它已经独立于 FIFO，在 `start()` 最前面启动）；
- 如果画面能一路走到登录界面 ⇒ **legacy 通路 + 心跳就是可用方案**，
  我们只需要在 legacy 下用 VRAM + `SVGA_CMD_UPDATE` 呈现，彻底绕开 ScreenTarget；
- 如果仍然死 ⇒ 说明不是 `CONFIG_DONE`，是 IOFramebuffer 接管本身与宿主 legacy 扫描输出
  的冲突，需要看 `enableController` 阶段做了什么。

---

## 38.21（结论）`kFbStage = 0` + 独立心跳 ⇒ 桌面真的出来了，可复现

上一节留下的二选一，答案是**第一条：legacy 通路 + 心跳就是可用方案**。
而且不是偶然 —— 冷启动 + 全程采样复现了两次。

### 实验配置

```c
static const UInt32 kFbStage           = 0;   // no FIFO writes at all, so no CONFIG_DONE
static const UInt32 kEnableRpcHeartbeat = 1;
static const UInt64 kRpcHeartbeatNs     = 2ULL * 1000000000ULL;
static const UInt32 kStduFlat           = 1;
```

心跳跑在**独立的 `thread_call`** 上，在 `start()` 里 `super::start()` 之后、
`readDeviceMode()` 之前就武装好。它不依赖 `presentTick`（后者实测会停摆 30 秒以上，§38.19）。

### 冷启动时间轴（`vmware.log` + kext 日志，2026-11-03 04:42）

| 时刻 | 事件 | 谁 |
|---|---|---|
| 04:42:02.692 | `Running status rpc handler: 0 => 1` | EFI 固件的一次 RPC |
| 04:42:03.073 | `Screen type changed to RegisterMode`，`Screen 1 Defined xywh(0,0,1024,768)` | 宿主 |
| 04:42:10 | `=== system boot` | 客户机内核 |
| 04:42:28.656 | **`Tools: Tools heartbeat timeout`** → `1 => 0` | 宿主（无 Tools） |
| 04:42:30.031 | kext `start`，心跳武装 | 我们 |
| 04:42:32.031 | `rpc: heartbeat #1 -> status 0x0` | 我们 |
| 04:42:30.655 | `Running status rpc handler: 0 => 1` | 宿主：**心跳把超时救回来了** |
| 04:42:45.504 | `vgpu-fb: enableController` | 我们接管 IOFramebuffer |
| 之后 | heartbeat #30 / #60 / #90 每 30 秒一次，全 `status 0x0` | 我们 |

宿主日志里 **`Tools heartbeat timeout` 只出现 1 次**（`grep -c` 计数），
之后就再没超时过 —— 心跳把它永久维持住了。

### VNC 采样（`.shots/cold1/`，1024×768，单连接，60 帧 / 188 秒）

| 时刻 | 状态 | distinct | 判定 |
|---|---|---|---|
| t+0 | FULL | 26 | **Apple logo**（6024 白像素，位置居中） |
| t+3 … t+34 | delta 512 px | 29 | 停在 Apple logo |
| t+37 | delta 16640 px | 201 | 启动切换中 |
| **t+43** | **FULL** | **99073** | **桌面出现** |
| **t+47** | **FULL** | **145464** | **桌面完整** |
| t+50 … t+188 | delta 512 px（20×20 小方块） | 1455xx | 只有时钟/光标类小区域在动 |

全程 **1024×768，一次 `ScreenTarget` / `1024×512` 都没有**，连接也没有被踢断。

ASCII 渲染（`tools/pngascii.py`）在 t+47 和 t+188 上的结果完全一致：
顶部整条菜单栏的细碎纹理、中部窗口的 `%@@*%*` 高亮块、右侧滚动条竖线、
底部 `=========` 的 Dock 条。`f47` 与 `f188` 的差异只在菜单栏时钟那几列 —— 也就是对比表里的 512 px delta。

### 顺带修掉的一个观测陷阱：`vncloop.py` 会谎报黑屏

之前 `vncloop.py` 报「`distinct=180, nonzero=512`」，看起来像屏幕全黑，
实际屏幕上是完整桌面（`vncsnap.py` 单张快照 `distinct 145779`）。
根因不是采样器的连接方式，而是它对 **Raw 矩形**的处理：

- VNC 服务端对 `FramebufferUpdate` 请求**只回它认为变了的矩形**，即使 `incremental=0`；
- 旧实现把每个矩形都写到 `rows[y+row]`，然后用 `x+col` 做列偏移 —— 一旦矩形不在 x=0，
  列偏移就错了；而且返回的行本身就是稀疏的（未覆盖区域是 0）；
- 于是「一个 20×20 的小更新」被当成「整屏只有 512 个非零像素」。

修法（`tools/vncloop.py` 重写）：引入持久 `FrameBuffer`，按 `(x, y)` 把矩形 patch 进正确位置；
`fresh` 判定改成「本次更新触达的行跨度并集是否铺满整屏」，而不是看单个矩形是否满行。
输出也从「颜色数」升级成 `FULL` / `delta(rects=…,px=…)` + `since=…`，
一眼能区分「新画面」和「没有任何变化」：

```
t+  0s 1024x768 FULL distinct=26 nonzero=6024/786432 since=screen -> f00.png
t+  3s 1024x768 delta(rects=1,px=512) distinct=29 ... since=t+frame 0 -> f03.png
t+ 43s 1024x768 FULL distinct=99073 nonzero=785760/786432 since=screen -> f43.png
```

另外补了 `tools/pngascii.py`：把 PNG 渲染成 96×30 的亮度 ASCII，
这样纯文本的智能体也能判断「菜单栏/窗口/Dock 在不在」，而不是只看统计数字。

### 结论

1. **`CONFIG_DONE` 是那道致命的门。** 不写它，宿主的 legacy 自动扫描输出就一直有效，
   guest 的 VRAM 写什么宿主就扫什么，画面全程正常。
2. **心跳是另一道门，但它可以被打开。** 没装 Tools 时宿主 20 秒判死；
   kext 每 2 秒发一次零长度 `BDOOR_CMD_MESSAGE` 就能把它永久钉在 `running status = 1`。
3. **两条合起来 = 一个可用的呈现通路**，而且完全绕开 ScreenTarget / GB surface / STDU，
   也就是绕开了 §35 判定为死路的整条 DX 主线。
4. 我们**不需要** `SVGA_CMD_UPDATE` 就能出图 —— 宿主自己在扫 VRAM。
   这一点很重要：意味着呈现通路上我们一行 FIFO 命令都不用发，也就没有任何楔死队列的风险。

### 下一步（按优先级）

1. **验证「不接管也能出图」**：把 `kFbStage` 保持 0，但临时让 kext 不发布 framebuffer
   （或直接 `kextunload`），看画面是否仍然正常。用来分离「宿主 legacy 扫描输出」
   与「我们的 IOFramebuffer 接管」各自的贡献。当前证据里 `enableController` 发生在
   t+15 左右（04:42:45），而桌面出现在 t+43（04:42:53），所以**至少最后这段是宿主自己扫出来的**。
2. **确认分辨率协商**：`svga max 1176x885` 而实际 `1024x768`，宿主 VRAM 只有 4 MB。
   要上更高分辨率得先在 vmx 里抬 `svga.vramSize` / `svga.maxWidth/maxHeight`。
3. **接上面板尺寸变化的通路**：现在画面能出，但换分辨率（`SetMode`）还没验证过。
4. 清理 vmx 里 §38.15 留下的四个无效 trace 键。
5. 修 `build.sh` 的 reachability 检查：它对 `kFbStage` 级死代码无效，
   因为字符串字面量即使代码不可达也会留在二进制里。

---

## 38.22 对照实验：**桌面不是我们画出来的**，而且 `CONFIG_DONE` 才是切 ScreenTarget 的唯一原因

§38.21 的结论里有一句需要撤回：我当时写「我们不需要 `SVGA_CMD_UPDATE` 就能出图 ——
宿主自己在扫 VRAM」，把它当成了我们方案的功劳。做了「无 kext」对照之后，真相比这更彻底：
**画面跟我们的 kext 一点关系都没有。**

### 实验：把 kext 从内核集合里彻底摘掉

热卸被拒（`kext is in use or retained`，这是接管生效的证据），所以走冷启动路径：

```sh
sudo mv /Library/Extensions/vgpuFramebuffer.kext{,.off}
sudo rm -f /Library/KernelCollections/AuxiliaryKernelExtensions.kc && sudo kextcache -i /
# 结果：/Library/KernelCollections/ 里只剩 .file，aux KC 不再生成，
#       strings 里 'com.vgpu.framebuffer.' 计数 = 0
```

注：`kmutil install --update-all` 在 sealed 根卷上会以 `Read-only file system` 失败
（`/` 是 `apfs, sealed, read-only`）。可行路径是 `sudo kextcache -i /`，
但它**不会重建 aux KC**；要让 aux KC 真正消失，得先 `rm` 掉旧的那份。

### 四次启动的屏幕模式序列（同一台机、同一份 vmx）

| 启动 | kext | `CONFIG_DONE` | 心跳 | `Screen type` 序列 | 画面 |
|---|---|---|---|---|---|
| `vmware-2.log` 20:29 | 有（stage 2+） | 写 | 无 | RegisterMode → **ScreenTarget(1024×512)** | **黑** |
| `vmware-1.log` 20:33 | 有（stage 5） | 写 | 有 | RegisterMode → **ScreenTarget(1024×512)** | **黑** |
| `vmware-0.log` 20:42 | 有（stage 0） | 不写 | 有 | RegisterMode →（**从不切走**） | **桌面** |
| `vmware.log` 20:49 | **无** | — | — | RegisterMode →（**从不切走**） | **桌面** |

第 4 行（无 kext）是关键：`grep -c "Tools heartbeat timeout"` = **1**，
心跳超时照常发生（20:50:09），`running status` 归 0，**但 `Screen type` 没有再变过**，
`Screen 1 Destroyed` 也从未出现。桌面在 t+50 以 `distinct=145743` FULL 帧出现，
`tools/pngascii.py` 渲染出与有 kext 时**完全相同**的菜单栏 / 窗口 / Dock。
之后 74 秒内只有 512 px 的时钟类 delta。

### 结论 1：心跳不是必须的，`CONFIG_DONE` 才是那道门

§38.18–§38.20 把「Tools 心跳超时 → 切 ScreenTarget」当成因果关系，这个解释是**错的**。
真正的因果链是：

```
Tools heartbeat timeout  -->  running status 1 => 0        （独立事件，只影响状态标志）
CONFIG_DONE = 1          -->  MKS 切 screen-target 呈现模型  （这才是切屏幕的触发点）
                              -->  Screen 1 Destroyed
                              -->  Screen type changed to ScreenTarget (1024x512)
```

`vmware-0` 和 `vmware.log` 两次都**经历了** heartbeat timeout，都没有切走 ——
区别只在 `CONFIG_DONE`。反过来说，`vmware-1` 有心跳、有 `CONFIG_DONE`，照样切走了。
「心跳把它拉回来」这个说法也不成立：`vmware-0` 里 `0 => 1` 发生在 20:42:30，
而 `Screen 1 Destroyed` 直到 20:49:37（关机）才出现，中间没有任何切屏动作。

### 结论 2：macOS 自己就有可用的显示通路

没有我们的 kext 时，macOS 用固件留下的 EFI framebuffer + 宿主 legacy 扫描输出
就能把完整桌面画出来，分辨率 1024×768，pitch 4096，也就是 `SVGA_REG_WIDTH/HEIGHT/PITCH`
在启动时被固件设好的那组值。宿主在 legacy 模式下持续扫描 VRAM，macOS 的
`IOFramebuffer` 走 EFI/VBE 路径写这块 VRAM —— 中间完全没有 VMware 驱动参与。

这解释了为什么之前所有「我们呈现失败」的实验都观察不到任何差异：
**我们的呈现根本没被需要，画面来自另一个（我们已经绕开的）通路。**
也就意味着 §35–§38 里「DEFINE_RTV 失败 ⇒ 没画面」这条推理链从来就不成立 ——
没画面的时候，是 `CONFIG_DONE` 把宿主那条通路关掉了。

### 结论 3：项目真正的定位需要重新表述

原来的目标是「写一个 macOS 客机 vGPU 驱动，让客户机可以 3D 加速」。
现在的事实分层是：

1. **2D 桌面呈现**：已经可用，**不需要我们**（只要不写 `CONFIG_DONE`）。
   我们能提供的增量是「更高分辨率」和「动态改分辨率」，因为当前 max 只有 1176×885、
   VRAM 只有 4 MB，全是宿主默认值。
2. **3D 加速**：仍然需要驱动，而且 §35 已经证明 DX 主线在 guest 侧够不到宿主门控。
   但既然 2D 不需要 `CONFIG_DONE`，那么 3D 是否也**不必**走 screen-target 模型？
   这是 §35 之后第一次出现的新可能 —— 值得回去重新审一遍
   「3D 命令是否需要 `CONFIG_DONE`」这个前提，而不是假设它必须。

### 下一步

1. **先把这个「不写 `CONFIG_DONE` 也能有 2D」当作基线固化下来**：
   在 vmx 里抬 `svga.vramSize` / `svga.maxWidth` / `svga.maxHeight`，
   看 macOS 在没有我们驱动的情况下能不能吃下 1920×1080 —— 如果能，
   那么 2D 需求就只剩「让 WindowServer 知道有更高分辨率可选」。
2. **重新审 §35**：`CONFIG_DONE` 是 STDU/DX 命令的硬前提，还是只是我们当时的假设？
   去 vmwgfx 上游源码里找 `SVGA_CMD_DEFINE_GB_SCREENTARGET` 之前是否有
   `CONFIG_DONE` 检查。（`DEFINE_RTV` 失败是 host 侧 `0x1414930A0` 未创建，
   与 `CONFIG_DONE` 未必相关，需要分开验证。）
3. 把丢失的 kext 装回去（`mv *.off`）之前，先量一次「无 kext + 高分辨率」，
   避免又被我们自己的驱动干扰。

## 39 分辨率是可以在 guest 侧自己改的（2026-11-03 05:32）

§38.22 留下的下一步是「先抬 vmx，看 macOS 能不能吃下 1920×1080」。
这条没走通（`svga.maxWidth/maxHeight` 不被 GFB 读取，`vmotion.checkpointFBSize`
每次开机被写回 4 MB），于是换了另一条路 —— **直接在 guest 里写 SVGA 寄存器**。
结论：**这条路通，而且是我们第一次真正改变了宿主行为。**

### 39.1 载体：把 vgpuProbe 改成一个寄存器实验台

`vgpuFramebuffer.kext` 现在是 `.off` 状态（§38.22 的对照实验），不能拿它做实验。
`/Library/Extensions/vgpuProbe.kext` 是早期验证「未签名 kext 能否加载」留下的桩，
挂在 `IOResources` 上、什么都不做 —— 正好拿来当载体：它不接管任何硬件，
加载失败也不会改变驱动匹配顺序。

改造后的 `kexts/probe/vgpuProbe.cpp` 自己走 PCI 树找 `15ad:0405`，把 BAR0 当 I/O
端口基址，然后按编译期常量 `VGPU_PROBE_MODE` 跑一种实验：

| mode | 行为 |
|---|---|
| 0 | 只读快照（安全） |
| 1 | 写 `SVGA_REG_WIDTH/HEIGHT/BITS_PER_PIXEL` 再回读 |
| 2 | 写 `SVGA_REG_MAX_WIDTH/MAX_HEIGHT` 再回读 |

`tools/probe-cycle.py` 把「构建 → 安装 → 重启 → 收日志」串成一条命令。
**每次实验必须重启一次** —— macOS 15 对被改动过的 AuxKC kext 一律返回
`KMErrorDomain Code=28`，`kmutil load -z`、`kextutil`、`kmutil load --bundle-path`
三条路径全都一样。测过 `kmutil unload` 再 `load` 也不行：二进制 UUID 一变就必须重建集合。

### 39.2 三个构建期踩的坑

1. **`OSClassLoadInformation` 未定义**：`IOPCIDevice.h` 会无条件 `#include`
   `<PCIDriverKit/IOPCIDevice.h>`（只要 SDK 里有这个文件），而 DriverKit 那版依赖
   IOKit 的类机制。真正的开关是 **`-DKERNEL`** —— `-mkernel` 不会带上它。
   `kexts/framebuffer/build.sh` 里早有一行注释写着这件事，probe 的构建脚本照抄时漏了。
2. **缺 `com.apple.iokit.IOPCIFamily` 依赖**：直接把 `IOPCIDevice` 用在
   `IOResources` 的 client 里，就必须在 `OSBundleLibraries` 里显式声明，
   否则链接期报 `could not find a kext which exports '__ZN11IOPCIDevice9metaClassE'`。
   主 kext 是经 `IOGraphicsFamily` 传递拿到的，所以它的 plist 里没有这一项。
3. **BAR0 的低位是标志位不是地址**：`BAR0 = 0x2041`，bit0=1 表示 I/O 空间，
   端口基址是 `0x2041 & ~3 = 0x2040`。直接用 `0x2041` 会让索引端口和值端口都偏一位，
   **所有寄存器都读回 0** —— 看起来完全像「设备被禁用」，白花了一次重启才找到。

### 39.3 寄存器快照，以及它的两个坑

mode 0 在开机后读到的值（每次重启都一样，五次采样无漂移）：

```
ID               [ 0] = 0x90000002      <- SVGA_ID_2
ENABLE           [ 1] = 0x1
WIDTH            [ 2] = 0x400  (1024)
HEIGHT           [ 3] = 0x300  (768)
MAX_WIDTH        [ 4] = 0x498  (1176)
MAX_HEIGHT       [ 5] = 0x375  (885)
DEPTH            [ 6] = 0x18   (24)
BITS_PER_PIXEL   [ 7] = 0x20   (32)
BYTES_PER_LINE   [ 8] = 0x0            <- 读不到
FB_START         [ 9] = 0x00ff0000     <- 读不到
FB_OFFSET        [10] = 0x0000ff00     <- 读不到
MEM_START        [13] = 0xf0000000     <- 对的
CONFIG_DONE      [15] = 0x00400000     <- 读不到
SYNC             [16] = 0x00300000     <- 读不到
GUEST_ID         [19] = 0x00040000     <- 读不到
VRAM_SIZE        [20] = FLAGS/20/21/22/23 = 0
```

`WIDTH/HEIGHT/MAX_*/DEPTH/BPP/MEM_START` 全对，其余不对。两个坑都排查过了：

- **不是 I/O 时序**：在 `outl(index)` 和 `inl(value)` 之间插 `outb $0, $0x80`
  （ISA 经典 settle delay）之后，结果逐字节不变。
- **不是竞争**：索引/值端口是设备级单个锁存器，理论上别的读者（固件、NDRV、
  另一个 vCPU）能挤进来。但同一索引连读 5 次全部一致，`port-width latch`
  测试（`0x1234 -> inl 0x1234 / inw 0x1234 / inb 0x34`）也证明 `inl` 确实是 32 位。

仍在对不上的那几个值上有一个可疑规律：`SYNC[16]` 读到的是 `0x300000`，
而主 kext 的历史日志写着 `fb size 0x300000` —— 也就是**读 16 拿到了 FB_SIZE 的值**。
编号是否存在偏移，还是这套 SVGA 变体的布局和上游 `svga_reg.h` 不同，**尚未定论**。
这一条不影响下面的结论，因为结论只依赖已经读对的那几个寄存器。

### 39.4 mode 1：写宽高，宿主真的跟着改了

写 `WIDTH=1920, HEIGHT=1080, BPP=32`，然后回读：

```
WIDTH            [ 2] = 0x498  (1176)      <- 写的 1920，被钳到 1176
HEIGHT           [ 3] = 0x375  (885)       <- 写的 1080，被钳到 885
SYNC             [16] = 0x3f9000 (4165632) <- 从 0x300000 变成 4165632
```

`4165632` 与 `1176 × 885 × 4 = 4163040` 同量级（差值是行对齐），
说明宿主接受了新尺寸并重算了帧缓冲大小。

宿主 `vmware.log` 给出了决定性证据 —— **`SWBScreen` 真的重新定义了这个屏**：

```
21:31:37.989  SWBScreen: Screen 1 Destroyed: xywh(0, 0, 1024, 768) flags=0x2
21:31:37.989  SWBScreen: Screen 1 Defined:   xywh(0, 0, 1176, 768) flags=0x2
21:31:37.991  SWBScreen: Screen 1 Destroyed: xywh(0, 0, 1176, 768) flags=0x2
21:31:37.991  SWBScreen: Screen 1 Defined:   xywh(0, 0, 1176, 885) flags=0x2
```

中间那次 `1176×768` 是我先写 WIDTH 后写 HEIGHT 的中间态，宿主对每次写入都响应了。

VNC 侧也确认了：连接直接报 **1176×885**，首帧 `FULL distinct=145552`，
与 1024×768 时的 `145464` 同量级，像素总数从 786432 涨到 1040760。

### 39.5 但是画面是撕裂的 —— pitch 没跟着改

对首帧做行亮度剖面：

```
row   0- 659  mean-luma 25..72   <- 有内容
row 660- 679  mean-luma 12.4
row 680- 899  mean-luma 0.0      <- 全黑
col   0-1199  mean-luma ~33      <- 横向铺满
```

边界在 **第 668 行左右**。算一下：macOS 仍在按老 pitch 4096 往 VRAM 里写，
老帧缓冲占 `4096 × 768 = 3145728` 字节；宿主现在按新 pitch `1176 × 4 = 4704` 读，
`3145728 / 4704 = 668.7` 行。**完全吻合。**

所以现在的状态是：

- **宿主侧：切分辨率成功**，1176×885 生效，这是本轮真正的突破。
- **guest 侧：没人告诉 macOS 尺寸变了。** `IONDRVFramebuffer` 的
  `IOFBConfig` 里只有一个模式（`DM` 解出来是 1024×768，`DF`=3），
  `IOFBMemorySize = 3145728`（3 MB），它不会因为我们写了寄存器就重新协商。

### 39.6 这一节改写了什么

1. **§38.22「下一步」第 1 条作废**：分辨率不是只能由宿主在 RegisterMode 里决定。
   `SVGA_REG_WIDTH/HEIGHT` 对 guest 可写，写完宿主立刻重建 screen。
   上限是 `MAX_WIDTH/MAX_HEIGHT`，而它们来自 `gfbSize = 4 MB` 这个宿主硬编码。
2. **`svga.maxWidth` / `svga.maxHeight` 是死键**：宿主日志里始终是
   `maxWidth=2560, maxHeight=1600`，`SVGA-GFB` 不读这两个 vmx 键。
3. **真正的上限是 VRAM 4 MB**：`SVGA-GFB: Using Initial gfbSize = 4194304` 之后
   `Truncated maximum resolution ... max wh(1176, 885)`。要更高分辨率，
   必须解决这 4 MB —— 而它是 `vmware-vmx.exe` 里的常量，不是配置键
   （二进制 GFB 段前后只找到 `svga.enableNoBBRestriction` 等，没有 gfbSize 的键）。
4. **`kFbStage` 之外多了一个新开关面**：即使不做 3D，我们也能提供
   「改分辨率 / 提供更高分辨率」这个真实功能 —— 前提是补上 pitch 协商这一半。

### 下一步（按价值排序）

1. **补上 guest 侧的一半**：在 kext 里同时做「写寄存器切模式」和「让
   `IOFramebuffer` 重新读 `BYTES_PER_LINE`/`FB_OFFSET` 并更新模式表」。
   后者等价于把 `getDisplayModeCount` 从 1 个模式扩成一张表、
   在 `setDisplayMode` 里真正编程寄存器并 `IOFramebuffer::performWakeup`。
   这一步做完，「动态改分辨率」就完整了。
2. **搞清 `BYTES_PER_LINE` 读不到的原因**：写完之后 pitch 一定是变了（撕裂边界
   精确对应 4096→4704），但读不出来意味着我们没法在 guest 侧校验。
   先试「写之前读」和「写完立刻读」的差异，再考虑是不是要走 MMIO 高寄存器。
3. **4 MB 的解**：`vmware-vmx.exe` 里 `Using Initial gfbSize = 4194304` 附近找
   写入点。已经有 `vmxset.py` 和两个 `.bak-*` 备份，说明这条路上做过补丁；
   需要重新确认补丁是否还在、是否还有效。

---

## 40 §39.3 的结论作废：寄存器没有偏移，是 probe 的索引表错了

§39.3 我写了两条「已排除」：不是 I/O 时序、不是竞争。两条都对，但我跳过了第三种可能——
**索引本身写错了**。而 §39 里「`BYTES_PER_LINE` 读不到」这个悬案，答案就是它。

### 40.1 判据：同一组索引，两个驱动，两套答案

决定性证据来自**同一次启动**里的两条日志。主 kext `vgpuFramebuffer` 在 04:34 / 04:42
两次启用时读到的值是自洽的：

```
vgpu-fb: svga mode 1024x768 depth 24 bpp 32 pitch 4096
vgpu-fb: svga fb offset 0x0 fb size 0x300000
vgpu-fb: svga vram 0x400000 fb start 0xf0000000 caps 0xfdffc3e2
```

而 probe 在同一台机的索引 8..16 上读到 `0`、`0xff0000`、`0xff00`、`0x300000` 这种
错位的数字。两个驱动用的是**同一对端口、同一批 `inl`/`outl` 指令**
（`probe-cycle.py` 与 `vgpuFramebuffer.cpp` 的实现逐字节可以对照），所以差异只能来自
**索引用的是哪一套编号**。

主 kext 的常量表：

```c
kSvgaRegBytesPerLine = 0x0C,   kSvgaRegFbStart  = 0x0D,   kSvgaRegFbOffset = 0x0E,
kSvgaRegVramSize     = 0x0F,   kSvgaRegFbSize   = 0x10,   kSvgaRegCaps     = 0x11,
kSvgaRegMemStart     = 0x12,   kSvgaRegMemSize  = 0x13,   kSvgaRegConfigDone = 0x14,
```

probe 当时抄的是上游 `svga_reg.h` 的「顺序编号」（`BYTES_PER_LINE=8`、`FB_OFFSET=10`、
`CONFIG_DONE=15`）。`svga_reg.h` 在 `BITS_PER_PIXEL`(7) 之后**有一段被跳过的编号**，
我照着它推断 8 起的连续值，于是从索引 8 开始每个名字都对到了下一个/上一个寄存器上。
这就是为什么「8 以上全错、7 以下全对」——错得如此有规律，反而看起来像设备行为。

`§39.3` 里那个「可疑规律」现在有了确定解释：`SYNC[16]` 读到 `0x300000` 并不是编号偏移
的巧合，而是**索引 16 就是 `FB_SIZE`**，值本来就是 `0x300000`。

### 40.2 修好之后，mode 0 的完整快照

```
ID               [ 0/0x00] = 0x90000002      SVGA_ID_2
ENABLE           [ 1/0x01] = 0x00000001
WIDTH            [ 2/0x02] = 0x00000400  (1024)
HEIGHT           [ 3/0x03] = 0x00000300  ( 768)
MAX_WIDTH        [ 4/0x04] = 0x00000498  (1176)
MAX_HEIGHT       [ 5/0x05] = 0x00000375  ( 885)
DEPTH            [ 6/0x06] = 0x00000018  (24)
BITS_PER_PIXEL   [ 7/0x07] = 0x00000020  (32)
BYTES_PER_LINE   [12/0x0c] = 0x00001000  (4096)      <- 之前「读不到」
FB_START         [13/0x0d] = 0xf0000000
FB_OFFSET        [14/0x0e] = 0x00000000              <- 之前「读不到」
VRAM_SIZE        [15/0x0f] = 0x00400000  (4 MB)      <- 之前「读不到」
FB_SIZE          [16/0x10] = 0x00300000  (3 MB)      <- 之前「读不到」
CAPABILITIES     [17/0x11] = 0xfdffc3e2
MEM_START        [18/0x12] = 0xfb800000
MEM_SIZE         [19/0x13] = 0x00040000  (256 KB)
CONFIG_DONE      [20/0x14] = 0x00000000              <- 之前「读不到」
SYNC             [22/0x16] = 0x00000000
GUEST_ID         [23/0x17] = 0x00000000
MEM_SIZE_ACTUAL  [27/0x1b] = 0x00000000
```

`CONFIG_DONE` 读出来是 **0**，且这个值是真的 0 —— 主 kext 的 `kFbStage=0` 从来不写它，
所以宿主一直停在 legacy 呈现模式。这也从寄存器层面坐实了 §38.22 的因果链。

### 40.3 给 probe 加了自检：错编号不许再静默通过

快照后面跟一段几何交叉验证（`_checkGeometry`）：

```
geometry before: pitch 4096, 32 bpp x 1024 wide = 4096 bytes/row, fb size 3145728, vram 4194304
  fb size == pitch*height rounded to a page: CONSISTENT
  pitch is exactly one row, no scanline padding
```

判据是 `FB_SIZE == round_up(pitch * height, 4096)`。**第一版判据是错的**：
我写了 `pitch*height == fbSize`，在 1024×768 下恰好成立（`4096*768 = 0x300000`
本身就是页对齐的），切到 1176×885 后立刻报 INCONSISTENT（`4163040 != 4165632`）。
差值 2592 字节说明宿主**把帧缓冲大小向上取整到 4 KB 页**，不是算错了。
判据改成取整后成立。这条留在这里是因为它演示了一件事：
**在唯一一个数据点上成立的等式不叫判据。**

### 40.4 mode 1 重跑：pitch 确实是 4704，撕裂的解释彻底闭合

```
写前: BYTES_PER_LINE[0x0c] = 4096   FB_SIZE[0x10] = 0x300000
写 1920x1080 后:
  WIDTH[0x02] = 1176 (钳)    HEIGHT[0x03] = 885 (钳)
  BYTES_PER_LINE[0x0c] = 4704  <- 1176 * 4，宿主重算了 pitch
  FB_SIZE[0x10] = 0x3f9000 (4165632) = round_up(4704*885, 4096)
```

§39.5 的撕裂边界是从像素统计反推的：老帧缓冲 `4096*768 = 3145728` 字节按新 pitch
`4704` 读，`3145728 / 4704 = 668.7` 行，实测边界在第 668 行。现在 pitch 直接读出来了，
`1176*4 = 4704` 一字不差 —— 反推和直读互证，这条推理链不再依赖像素统计。

**同时确认：`BYTES_PER_LINE` 对 guest 只读。** 宿主在写 `WIDTH` 时自己重算 pitch，
guest 没有办法指定它（本节没有尝试写；下一节要做的 modeset 必须知道这件事：
**我们只能选尺寸，不能选 pitch**）。

### 40.5 顺手修的工具坑：`--last 6m` 会跨过重启

`probe-cycle.py` 原本用 `log show --last 6m` 收日志，理由是「刚重启过，窗口不可能
够到上一次实验」。但这次改动后的第一轮输出里立刻混进了上一次启动的
`pitch*height = ... INCONSISTENT` 行 —— 因为宿主机重启只要 **39 秒**，
6 分钟窗口轻松跨过它。这正是项目记忆里那条纪律（「判定结果前先确认跑的是哪个二进制」）
的同一个坑，只是换了个入口。

修法：读 `sysctl -n kern.boottime`，在 Python 侧按时间戳把 boot 之前的行全部丢掉
（`_since_boot`），不再依赖窗口长度猜测。`--last` 只用来让查询有界（无界查询在新开机
的日志量下会 SSH 超时）。

### 40.6 这一节对「下一步」的改写

1. ~~搞清 `BYTES_PER_LINE` 读不到的原因~~ —— **完成**，是索引表错了，现在能读。
2. **guest 侧 modeset 的写法确定了**：写 `WIDTH`/`HEIGHT` 让宿主重算 pitch，
   然后**回读** `BYTES_PER_LINE` / `FB_OFFSET` / `FB_SIZE` 拿到宿主给的几何，
   再把这组值灌进 `IOFramebuffer`。不能自己算 pitch。
3. **4 MB `gfbSize` 仍是硬上限**：`VRAM_SIZE = 0x400000`，`MAX = 1176x885`。
   1920×1080 需要 `1920*1080*4 = 8.29 MB`，超了一倍。这个限制与 modeset 正交，
   modeset 做完也只能在 1176×885 以内切换。

---

## §41 probe 的寄存器**语义**也修完了：三个标签全错，且显示块从未被宿主使用

§40 修的是**编号**（读哪个口），这一节修的是**含义**（读到的数是什么）。
两者是同一场误诊的两半：编号错的时候读数全是垃圾，编号对了之后才发现其中三个
读数的**名字**也一直是错的 —— 那些错误标签来自同一份上游 `svga_reg.h` 的推断。

### 41.1 证据来源换了：不再"看头文件"，改为反汇编 VMware 自己的 Windows 驱动

`vm3dmp.sys`（VMware SVGA miniport，9.17.11.0003）里有三个寄存器访问器，是同一份
SVGA 协议的官方实现：

| 函数 | 作用 | 指令序列 |
|---|---|---|
| `0x140016a30` | 写 | `out dx,eax`（索引）→ `inc dx; out dx,eax`（值） |
| `0x140015f54` | 读 | `out dx,eax`（索引）→ `in eax,dx`（在 `portBase+1`） |
| `0x140015fe0` | 写后回读 | `out dx,eax`（索引）→ `inc dx; in eax,dx` |

拿这三个访问器回溯调用点，就能给每个立即数配上**行为**，而不再靠命名猜测。

### 41.2 三个被推翻的标签

**(a) `0x14` 是 `RESET`，不是 `CONFIG_DONE`。**
`VM3DCtrlResetDevice`(`0x1400124f0`) 的序列是：

```asm
0x14001255b: mov eax, 1
0x14001255b: out dx, eax            ; ENABLE = 1
0x14001256a: mov eax, 0x14
0x14001256f: out dx, eax            ; 写索引 0x14
            (随后写值 0)
```

`ENABLE` 先置 1，再动 `0x14`，随后把两个环指针置 `-1`。一个"配置完成"标志不会在
使能之后被清零，也不会顺手清环指针 —— 只有"重置设备"讲得通。所以 `0x14` 是 `RESET`。

**(b) `0x15` 是 `SYNC`（只写），`0x16` 是它的状态位 `BUSY`。**

```asm
0x140013d9b: mov eax, 0x15
0x140013da3: out dx, eax            ; 索引 = SYNC
0x140013da4: mov eax, 2
0x140013dad: out dx, eax            ; 值 = 2  → flush and wait
...
0x140013de9: mov edx, 0x16
0x140013df1: call 0x140015fe0       ; 回读 0x16
0x140013df6: test eax, eax
0x140013df8: jne 0x140013de9        ; 非零就继续等
```

写 `1` 立即返回（flush），写 `2` 后再轮询 `read(0x16)` 到零（flush and wait）。
所以 `0x16` 是 `BUSY`，不是 `SYNC` —— 它是**轮询用的状态**，写它没有意义。

**(c) display 块从 `0x20` 起，不是 `0x22`。**
模式设置函数 `0x14001b1e8`：

```asm
0x14001b20a: lea edx, [r8+0x20]     ; 0x20 —— 块使能
0x14001b216: mov edx, 2             ; WIDTH
0x14001b226: mov edx, 3             ; HEIGHT
0x14001b236: mov edx, 7             ; BITS_PER_PIXEL
0x14001b248: mov edx, 0x20          ; 0x20 —— 收尾再写一次
```

首尾都是 `0x20`，说明它是**该块的使能/门控位**，`0x21` 是这个块的大小。
`0x14001444e` 里 `write(0x21, 0)` 然后立刻屏蔽中断（`out dx,eax` 于 `portBase+8`），
证实写 `0` 的语义是"屏蔽并静默"。

### 41.3 修好语义之后，mode 0 的新快照（每次重启一致）

```
RESET            [20/0x14] = 0x00000000 (0)
SYNC             [21/0x15] = 0x00000000 (0)
BUSY             [22/0x16] = 0x00000000 (0)
CONFIG_DONE      [25/0x19] = 0x00000000 (0)
MEM_SIZE_ACTUAL  [27/0x1b] = 0x00000000 (0)
DISPLAY_BASE     [32/0x20] = 0x00000000 (0)
DISPLAY_MAX      [33/0x21] = 0x00000000 (0)
```

几何两行自检仍然 `CONSISTENT`，其余读数与 §40.2 完全一致，说明改的只是标签，没动读数。

### 41.4 这组值说明了什么（真正的新信息）

- **`DISPLAY_BASE = 0` 和 `DISPLAY_MAX = 0` 是重要结论，不是"没读对"**：guest 侧
  `0x20`/`0x21` 都是 0，意味着宿主**从未通过 display 块驱动显示**。宿主的显示链路走
  SWBScreen（`SWBScreen: Screen 1 Defined` 那条日志），即 FIFO 命令 + surface 呈现，
  寄存器窗口里的 display 块在这台宿主上根本没被启用。
  ⇒ **沿 display 块做 modeset 这条线不用再试了**，它在这个配置下是死的。
- **`CONFIG_DONE = 0`**：FIFO 尚未被 guest 配置好。这正是下一步要做的事
  （分配 FIFO + 环缓冲 + 写 `MIN`/`MAX`/`NEXT_CMD`/`STOP`/`CONFIG_DONE`），
  只有它变成 1，3D 命令通路才算通了。
- **`MEM_SIZE_ACTUAL = 0`**：guest 从未请求过额外的命令缓冲，256 KB 的 `MEM_SIZE`
  是宿主给的默认值。这也解释了 §39 里为什么各种 3D 命令都撞 FIFO 中止 ——
  那个 256 KB 区域里还没有我们的环。

### 41.5 工具坑：`--sudo` 编译会把 `build/` 变成 root 所有

`cycle.sh` 里同时包含"编译"和"装到 `/Library/Extensions`"，而装需要 root。
用 `--sudo` 整体跑一次之后，`build/vgpuProbe.kext` 归 root 所有，下一次普通用户编译
在 `rm -rf build` 处 `Permission denied`，`buildx.sh` 静默失败（`set -eu` 下 `rm`
的失败被 `tail` 吞掉）。表现是"编译成功但产物是上一轮的"，非常容易误判。

修法：编译与安装分开 —— 编译永远用普通用户，只有 `cp`/`chown`/`kmutil` 走 root。
`probe-cycle.py` 本来就是这个结构，别再用 `cycle.sh` 做自动化。

### 41.6 下一步

1. **修 `vgpuFramebuffer.cpp` 里同源的三个错标**（`CONFIG_DONE=0x14` 应为 `RESET`；
   补 `SYNC=0x15`、`BUSY=0x16`、`CONFIG_DONE=0x19`、`DISPLAY_BASE=0x20`、
   `DISPLAY_MAX=0x21`）。这个被误标的 `0x14` 如果哪天被当成"写 1 表示配置完成"去写，
   就是一次**设备复位**。
2. **guest 侧 modeset**：只写 `WIDTH`/`HEIGHT`/`BPP`，然后回读 pitch 与几何（§40.6.2）。
   display 块这条路按 41.4 排除。
3. **FIFO 配置**：让 `CONFIG_DONE` 从 0 变 1。这是 3D 通路真正的前置条件，
   比 modeset 更靠底层。

---

## §42 回填：§38.21–§38.22 那条「`CONFIG_DONE` 是黑屏元凶」的因果链**是误诊**

§41 定下 `0x14 = RESET` 之后，必须回头改一条历史结论 —— 这是本次审计里最重要的发现，
因为它把过去几轮的方向判断全推翻了一半。

### 42.1 当时实际写下去的是什么

`vgpuFramebuffer.cpp` 里 `kSvgaRegConfigDone` 一直被定义成 `0x14`，并在
`probeFifo()` 里被写两次：

```cpp
svgaWriteRegister(_svgaPortBase, kSvgaRegConfigDone, 0);   // 行 1063
svgaWriteRegister(_svgaPortBase, kSvgaRegConfigDone, 1);   // 行 1096
```

按 §41 的反汇编结论，`0x14` 是 `RESET`，写 `0` 才是"复位"，写 `1` 无定义。
所以 `kFbStage >= 1` 的每一次启动，我们都在**把 SVGA 设备复位一次**（1063 行的写 0）。

而真正的 `CONFIG_DONE` 是 `0x19` —— **我们从来没有写过它**。
`CONFIG_DONE` 在这台机器上的实际值一直就是 §41.3 快照里的那个 **0**。

### 42.2 于是 §38.22 的对照表要重读

| 启动 | kext | 当时以为写了 `CONFIG_DONE` | 实际写了 | 画面 |
|---|---|---|---|---|
| `vmware-2.log` 20:29 | stage 2+ | 是 | **复位了一次** | 黑 |
| `vmware-1.log` 20:33 | stage 5 | 是 | **复位了一次** | 黑 |
| `vmware-0.log` 20:42 | stage 0 | 否 | 什么都没写 | 桌面 |
| `vmware.log` 20:49 | 无 | — | — | 桌面 |

相关性**依然成立**（"stage≥1 ⇒ 黑"），但归因错了。真实的触发点不是
"MKS 看到 `CONFIG_DONE` 就切 ScreenTarget 呈现模型"，而是
**"guest 把设备复位了，宿主把它已建立的显示状态（包括 legacy 自动扫描输出）一并丢掉"**。
这也解释了当时那条怎么都解释不顺的观测：

> `SVGA_CMD_UPDATE` (acked!) does not restore it.

如果只是"呈现模型切走了"，UPDATE 命令到达的是新的呈现模型，至少该有点反应；
但设备被复位后，`WIDTH/HEIGHT/PITCH/FB_OFFSET` 全部回到宿主默认值，我们发 UPDATE
时描述的是**一块已经不对应当前扫描参数的帧缓冲**，宿主自然什么都没做。

### 42.3 `ScreenTarget(1024x512)` 这个证据也换了个解释

当时记录的是 `Screen type changed to ScreenTarget (1024x512)`，并把它当作
"MKS 因为 `CONFIG_DONE` 切模型" 的直接证据。但注意那个 **512**：
1024×512 不是我们要求的任何尺寸，也不是 `MAX_HEIGHT`（885）。
它是 **`VRAM_SIZE / pitch` 的一半** —— 复位后宿主用默认 pitch 重新描述一块 4 MB
VRAM 能容纳的扫描区域，宽 1024 保持不变，高被 512 这个"安全值"截断。

即：那不是"切换到 ScreenTarget 呈现模型"，而是"复位后宿主按默认参数重建 Screen 1"。
两种解释都能产出同一行日志，当时的证据不足以区分 —— 而现在有了寄存器语义，就能区分了。

### 42.4 这次误诊的代价与教训

代价很直接：**§38 之后的所有工作都建立在一个错误的因果关系上**。
如果当时就知道 `0x14 = RESET`，那么第一个该做的实验不是"关掉 `CONFIG_DONE`"，
而是"**永远不要碰 `0x14`**，然后看 stage 1 能不能稳定" —— 这个实验从来没人做过。

教训和 §40 是同一条，只是换了一层：

> §40：**编号**不能从上游头文件推断，要读驱动。
> §42：**语义**也不能从上游头文件推断，要读驱动。

两次错的都是"上游 `svga_reg.h` 的布局可以照着猜"。而这份头文件在本宿主上是
**版本不匹配**的 —— VMware 自己的 Windows 驱动用的是另一套编号和语义。

### 42.5 已经做的修正

`kexts/framebuffer/vgpuFramebuffer.cpp` 的寄存器常量已改正：

```cpp
kSvgaRegReset        = 0x14,   // write 0 resets the device; never write this
kSvgaRegSync         = 0x15,   // write 1 = flush, 2 = flush and wait
kSvgaRegBusy         = 0x16,   // polled to zero after a flush-and-wait
kSvgaRegConfigDone   = 0x19,   // the real FIFO-configured flag
kSvgaRegDisplayBase  = 0x20,
kSvgaRegDisplayMax   = 0x21,
```

并把 1063 行那次 `write(0x14, 0)` 改成了 `write(0x19, 0)` —— 那才是"取消配置"。
这一改的含义是：**下一次 `kFbStage >= 1` 的启动不再复位设备**，
是 §38 以来第一次可以干净地观察"MKS 面对一个真的配好 FIFO 的 guest"的行为。

---

## §43 实验：`CONFIG_DONE=1` 不再黑屏 —— §38.22 的因果链正式作废

按 §42 修完寄存器后跑的第一轮 `kFbStage = 2`（只配 FIFO，不发任何命令）。

### 43.1 结果：画面完好，宿主从未切 ScreenTarget

| 观测 | 值 |
|---|---|
| `kextstat` | `com.vgpu.framebuffer` 已加载 |
| `fifo[before]` | `min 0 max 0 next_cmd 0 stop 0` —— 全新设备 |
| FIFO 写入后 | `min 1152 max 262144 next_cmd 1152 stop 1152` |
| `CONFIG_DONE` | **1**（写进 `0x19`，历史第一次） |
| `fifo caps` | **`0x0`** ← 异常，见 43.3 |
| `3d hw version` | **`0x0`** ← 异常 |
| `3d capabilities` | **`0 of 256 non-zero`** ← 异常 |
| `DEVCAP_3D` | **1** —— `SVGA3D LIVE` |
| VNC 截图颜色数 | **145262**，菜单栏 / 窗口 / Dock 全在 |
| 宿主 `Screen type` | 开机一次 `RegisterMode`，**此后再无变化** |
| 宿主 `SWBScreen` | 只有 Screen 1 的 Def/Destroy，没有切型号 |

**结论：`CONFIG_DONE = 1` 与黑屏无关。** §38.20–§38.22 用了四小节论证的
"写 `CONFIG_DONE` ⇒ MKS 切 screen-target 呈现模型 ⇒ 黑屏"这条链，
在寄存器修正后**一次也复现不出来**。

### 43.2 那条链其实一直在测量设备复位

§42 已经推出 `0x14` 是 `RESET`。这里再加一条正面证据：
本轮唯一改变的就是"不再写 `0x14`"，而黑屏就消失了。
换句话说，过去所有"stage ≥ 1 就黑"的实验，变量其实一直是**设备复位**，不是 FIFO 配置。

这也把 §38.22 里那个怎么都解释不通的现象解释掉了：

> `ScreenTarget (1024×512)`——1024 是我们当时设的宽度，512 不是我们设的任何值。

复位后宿主按默认参数重建 Screen 1，高度被 VRAM/默认 pitch 截断到一个安全值。
那不是"切呈现模型"，是"重建显示状态丢了一半信息"。

### 43.3 新的未解问题：FIFO caps 与 3D 能力块全零

`CONFIG_DONE` 写了 1，宿主**没有**把它的 FIFO 能力位公布出来：

```
fifo[after ] min 1152 max 262144 next_cmd 1152 stop 1152 caps 0x0 busy 0
fifo[after ] 3d hw version 0x0
3d capabilities advertised: 0 of 256 non-zero
```

但同一时刻 `SVGA_REG_DEVCAP`（`0x34`）却逐个返回真实值，`DEVCAP_3D = 1`：

```
devcap[ 0.. 7] 00000001 00000008 00000008 00000008 00000007 00000001 0000000d 00000001
devcap[16..23] 00000001 433d0000 00000014 00008000 00008000 00004000 00008000 00008000
```

两条通道给出**互相矛盾**的 3D 立场：DEVCAP 说 3D 是有的，FIFO 能力块说一个也没有。
可能性有三种，下一步要区分：

1. **`MIN` 设得太小/太大**：我们写 `1152`（288 个 dword）。若宿主期望的扩展寄存器区
   比 288 更大，能力块会落在宿主不写的位置上。
2. **能力块需要先读 `3D_HWVERSION` 才被填充**：vm3dmp 在初始化早期读它，我们读到的也是 0。
3. **`caps` 在 `CONFIG_DONE` 之后要被"读一次"才更新**：我们是直接读内存，
   而宿主可能只在某个时机刷新。

第 1 条最可疑，因为 `regs 291` 这个读数（`SVGA_REG_MEM_REGS = 0x1E`）比我们设的
288 大 3 —— 差值恰好是 3D 扩展的头部。之前从未注意过这个 291。

### 43.4 净收益

- 主 kext 现在可以在 `kFbStage = 2` 下**带着画面**持有 FIFO。这是整个项目第一次。
- 也就是说"2D 呈现 + FIFO 所有权"可以并存，§38.22 结论 3 里
  "2D 只需要不写 CONFIG_DONE"这个前提**不再必要** —— 2D 与 FIFO 不再互斥。
- 剩下的问题收敛成一个具体的数字：**`MEM_REGS = 291` 而 `MIN = 1152 = 288*4`**。
  先把 `MIN` 对齐到宿主公布的 `MEM_REGS`，再看能力块是否被填充。

## §44 寄存器号的真相：`CONFIG_DONE` 一直写在一个别的寄存器上

这一节推翻 §40、§41 和 §42 里关于寄存器编号的结论。**先说明为什么会错，再给证据。**

### 44.1 错误的来源：`svga_reg.h` 换了目录

前几轮一直用
`https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/vmwgfx/svga_reg.h`
去核对寄存器编号。这个 URL 现在返回 404，因为文件已经搬到
`drivers/gpu/drm/vmwgfx/device_include/svga_reg.h`。
拿不到权威表之后，编号是靠对 `vm3dmp.sys` 反汇编里的立即数**猜语义**来的 ——
而"某个索引出现在 out 指令里"只能证明它被写过，证明不了它是什么。

正确地址（可直接 curl）：
```
https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/vmwgfx/device_include/svga_reg.h
```

### 44.2 上游权威表（`device_include/svga_reg.h` 原文）

| 常量 | 值 | 我们之前用的 |
|---|---|---|
| `SVGA_REG_CONFIG_DONE` | **20 (0x14)** | 0x19 (25) ❌ |
| `SVGA_REG_SYNC` | 21 (0x15) | 0x15 ✅ |
| `SVGA_REG_BUSY` | 22 (0x16) | 0x16 ✅ |
| `SVGA_REG_MEM_REGS` | 30 (0x1E) | 0x1E ✅ |
| `SVGA_REG_COMMAND_LOW` | **48 (0x30)** | 0x35 ❌ |
| `SVGA_REG_COMMAND_HIGH` | **49 (0x31)** | 0x36 ❌ |
| `SVGA_REG_DEV_CAP` | 52 (0x34) | 0x34 ✅ |
| `SVGA_REG_CMD_PREPEND_LOW/HIGH` | 53/54 | 误当成命令口 ❌ |
| `SVGA_REG_GUEST_DRIVER_ID` | 61 | §41 误判为"不存在" ❌ |
| `SVGA_REG_FIFO_CAPS` | **68** | 从未读过 |
| `SVGA_REG_FENCE` | **69** | 从未读过 |

**`SVGA_REG_RESET` 在上游定义里根本不存在。** §41 里"0x14 是 RESET，写 0 会复位设备"
这个结论是纯粹的误读 —— `0x14` 就是 `CONFIG_DONE`，写 0 再写 1 正是配置 FIFO 的
标准动作，而它被"修复"成了 25。

25 是 `SVGA_REG_CURSOR_X`。

### 44.3 为什么这一个错就足以让一切失效

宿主不理解"FIFO 已配置"之前，不会去排空这个环。于是：

- `NEXT_CMD` 由 guest 推进（写进去的值一直在，看起来"提交成功"）
- `SYNC` 回了但没有意义（没有 FIFO 需要 flush）
- `STOP` 永远不动
- FIFO 内存里的 caps dword 永远是 0
- 每条命令都在内存里躺着，然后被下一次开机当成脏数据丢弃

这与 §43/§44 前实测的每一条症状完全吻合：`next_cmd 1176 / stop 1168 / fence 0x0`。

### 44.4 修正后的实测结果（2026-11-03 07:01 那次启动）

```
fifo[after ] min 4096 max 262144 next_cmd 4096 stop 4096 caps 0x77f busy 0
fence test: fifo caps 0x77f, SVGA_REG_FIFO_CAPS(68) 0x77f (reserve yes, fence yes)
fence 0x31415926 written at 4096, next_cmd -> 4104, host pinged (SYNC=1), polling
FIFO command stream VERIFIED: fence 0x31415926 acked after ~1000 us (next_cmd 4104 stop 4104)
gb: MOB otable base set (ppn 51049, 16384 bytes)
gb: SURFACE otable base set (ppn 51050, 4096 bytes)
gb: data MOB 1 defined (pt ppn 51051, 16384 bytes, PT64_1)
gb: GB SURFACE 1 defined (256x256 X8R8G8B8 on MOB 1) -- GB object model is UP
gb: DXCONTEXT otable base set (pt ppn 51061, 2048 bytes, PT64_0)
flat: MOB 2 defined (PT64_0, base ppn 49129, 4096 bytes)
flat: surface 2 defined 1024x1 on mob 2, bound and updated
flat: screen target 0 (1024x512) bound to surface 2 -- STDU present path is UP
present tick 1200 (next 120048 stop 120048)
```

**项目史上第一次**：

1. 宿主确认了 FIFO：`caps 0x77f`（此前恒为 0），`reserve yes / fence yes`
2. 宿主消费了我们的命令：`fence acked after ~1000 us`，`STOP` 从 4096 推进到 4104
3. `GB` 对象模型建立成功：MOB otable / SURFACE otable / MOB 1 / GB SURFACE 1 全部通过
   （§44 之前这些命令全部超时，`MOB object table base refused`）
4. STDU 呈现链绑定成功并稳定运行：1200 个 tick，`next == stop` 说明宿主一直在追

同时把 `MIN` 对齐到 4096（`vmw_fifo_create()` 里 `if (min < PAGE_SIZE) min = PAGE_SIZE`），
并在配置时把 `SVGA_FIFO_BUSY` 清零 —— 这两条也照抄了 vmwgfx。

### 44.5 新暴露的问题：呈现链"通"了但宿主读到的是零

```
flatrb: host surface 2 -> 0/1024 words nonzero, 0 match the marker 0xff00a5ff
        VERDICT *** host holds nothing ***
flatrb: control readback -> 0/1024 words nonzero -- READBACK writes nothing on this host
```

现在这个读回**第一次可信**：以前它跑在一个宿主根本没读的 FIFO 上，
"宿主读到零"不能区分"页表错"和"呈现链错"。现在页表彻底不参与
（`flat` 路径用 PT64_0，MOB 的"页表"就是数据页本身），宿主仍然读到零，
**所以问题在呈现链，不在页表。** 这正是 §38.12 设计 `kStduFlat` 时要分离的两件事。

另外 `GB_SCREEN_DMA refused`（`fence 21 NOT acked`）说明 `SVGA_3D_CMD_DX_PRED_COPY_REGION`
那条 DMA 路径仍然不通，但它与主呈现路径是独立的。

### 44.6 屏幕变成全黑：`DEFINE_GB_SCREENTARGET` 真的接管了显示

VNC 截图从 1024x768 变成 **1024x512 全黑**。原因清楚：screen target 绑定了一块
`1024x1` 的 surface，其余 511 行是未定义区域。这不是"命令没生效"，
恰恰是**命令生效了**——显示分辨率真的被我们改成了 1024x512。
（此前画面能保住，是因为 screen target 从未成功建立。）

**教训：在 surface 有完整内容之前不要 bind 到 screen target。**
后续要么先用正常路径（512 页真 surface）再 bind，要么把 flat 实验挪到
不影响主显示的 STID 上。

### 44.7 §40/§41/§42 作废清单

- §40「寄存器无偏移，probe 索引表错了」——错，probe 表本来就是对的（0..N 连续索引）
- §41「0x14 是 RESET」「命令口是 53/54」「61..64 不存在」——全部错，见 44.2
- §42「§38.21–38.22 是误诊，真因是设备复位」——不成立，从来没发生过复位
- §43「MIN 踩掉宿主 3D 版本字」——`MIN=288*4` 确实太小，但版本字在重配后本来就会被
  宿主写成 0（44.4 里 `3d hw version 0x0`），这条的因果链也不对

## 46. 撤回：「宿主从未离开 RegisterMode」是误读（2026-11-12）

上一轮的核心假设是错的，原因很具体：只 grep 了 `vmware.log` 的**尾部一行**，
看到 `Screen type changed to RegisterMode` 就断定宿主一直待在 RegisterMode。
把整个文件读完，同一个 boot 里其实是：

```
11:56:13.367Z  SVGA-ScreenMgr: Screen type changed to RegisterMode      (上一次 panic 之后)
11:56:54.515Z  SWBScreen: Screen 1 Destroyed: xywh(0, 0, 1024, 768) flags=0x2
11:56:54.521Z  SVGA-ScreenMgr: Screen type changed to ScreenTarget      ← stduBringUp 之后 1.3 s
11:56:54.521Z  SWBScreen: Screen 1 Defined: xywh(0, 0, 1024, 768) flags=0x2
```

也就是说宿主**确实**被我们推进了 ScreenTarget 模式，
"切换到 ScreenTarget 就会黑"这条老结论方向是对的，
"宿主还在 RegisterMode，所以 BAR1 才是扫描源"这条新结论作废。

同一轮里还有一个**实验设计本身无效**的臂：`kStduVramControl` 当时写的是
`memcpy(VRAM <- surface)`，两个来源内容完全一样，无论屏上出现什么都无法区分是谁。
这也是 §47.2 重做这个对照臂（洋红 vs 红绿）的原因——重做之后它立刻给出了答案。

**教训**：`grep | tail -1` 不是日志分析；判据必须能区分两个假设，
不能两个假设下都"符合预期"。

## 47. 显示通了：不是 STDU，是 legacy screen object（2026-11-12）

这一轮把"屏幕上有画面"做成了，路径和过去三周的方向**完全不同**。结论先写：

> **本宿主在客人接管 FIFO 之后，只呈现驱动 blit 到 screen object 的内容。
> 它不会去采样我们 bind 过去的 screen target。**
> 所以 STDU（`DEFINE_GB_SCREENTARGET` + surface + MOB）在这台机器上不可能出画面，
> 而 `DEFINE_SCREEN` + `DEFINE_GMRFB` + `BLIT_GMRFB_TO_SCREEN` 可以。

### 47.1 `SVGA_REG_ENABLE` 的 HIDE 位：宿主"把显示交出来"的开关

vmwgfx 的顺序（`vmwgfx_drv.c:423-434`，一字不改）：

```c
dev_priv->enable_state = vmw_read(dev_priv, SVGA_REG_ENABLE);
...
vmw_write(dev_priv, SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE |
                                      SVGA_REG_ENABLE_HIDE);
vmw_write(dev_priv, SVGA_REG_TRACES, uses_fb_traces);
dev_priv->fifo = vmw_fifo_create(dev_priv);      /* CONFIG_DONE 在里面 */
```

**HIDE 在 FIFO 建立之前就写。** 我们从来没写过它——本机启动时读到的是 `enable 1`。
写 `ENABLE|HIDE`（=3）之后再 CONFIG_DONE，宿主日志第一次出现：

```
SVGA hiding SVGA
SWBScreen: Screen 1 Destroyed: xywh(0, 0, 1024, 768) flags=0x2
SWBScreen: Screen 0 Defined:   xywh(0, 0,  640, 480) flags=0x3
```

不写 HIDE 时同一时刻是：

```
SVGA-ScreenMgr: Screen type changed to ScreenTarget
SWBScreen: Screen 1 Defined: xywh(0, 0, 1024, 768) flags=0x2
```

两条路径的差别就在 **Screen 0 flags=0x3（客人屏幕）** 与 **Screen 1 flags=0x2（legacy 屏）**。
宿主自建的那个 Screen 0 是 **640x480**——不是它刚销毁的 1024x768，
也不是拓扑寄存器 39/40 回读到的 1024x768。

**踩过的坑（重要）**：`SVGA_REG_ENABLE` 是**设备状态，不是客人状态**，它跨客人重启存活。
把 HIDE 打开一次之后，下一次启动（哪怕新二进制不写 HIDE）仍然是隐藏态，
表现为"换回旧配置也黑屏"，而任何客人日志都解释不了。
所以现在**两条分支都写 ENABLE**（开=3，关=1），并且写在 `kFbStage == 0` 早退之前。

### 47.2 洋红对照臂：宿主什么都不呈现

有 Screen 0 之后仍然全黑，于是做了一个之前一直做错的对照：把**两种来源写成不同颜色**。

- surface 里画红边/上半绿/下半蓝 + 游走白块（原有 `kStduPaint`）
- VRAM 整屏填洋红 `0xFFFF00FF`，再发 legacy `SVGA_CMD_UPDATE`

（旧版这个臂是 `memcpy(VRAM <- surface)`，两边内容完全一样，看什么都分不出来——
这就是它长期挂在 0 的原因。）

结果：**1024x768 全黑，distinct colors = 1**。既没有红绿，也没有洋红。
STDU 每帧 `UPDATE_GB_IMAGE` + `UPDATE_GB_SCREENTARGET` 都被 fence 消费，
呈现循环 12900 tick 不楔死，绑定和 otable 回读全对——**宿主就是不采样**。

同时把几何对齐也排掉了：`kStduMatchHost=1`（surface 改成 640x480、pitch 2560、
300 页，和宿主 Screen 0 一致）仍然全黑。

### 47.3 换路：legacy screen object 一次就通

`kFbStage = 5` 这条通路早在项目第一天就写好了，但一直被"STDU 才是正路"压着，
而且它从来没在**寄存器修正之后**跑过（§41/§42 那次 bisect 的全部结论都建立在
命令发到 48/49、`0x14` 当 CONFIG_DONE 上，等于没跑）。三条命令：

```
DEFINE_SCREEN        {screenId 0, PRIMARY, 1024x768, root 0,0}         body 28 B
DEFINE_GMRFB         {gmrId FRAMEBUFFER, offset FB_OFFSET, pitch, BGRX32} body 16 B
BLIT_GMRFB_TO_SCREEN {src 0,0; dest 0,0-1024,768; screenId 0}          body 28 B
```

然后 `thread_call` 每 33 ms 重发一次 blit。**没有任何客人内存参与**：
GMRFB 直接指向 BAR1 里的 framebuffer，blit 是宿主侧 DMA。每帧成本 9 个 FIFO dword。

配合 `kGbStage = 0`（完全不碰 GB 对象栈）第一次启动即出现：

- 锁屏完整实时渲染（壁纸、日期 `11月12日 星期四`、时钟、`Pika` 用户、密码框、右上菜单栏）
- VNC 输入密码后进入**桌面**：菜单栏、Terminal 里正在跑的 shell、系统设置窗口、Dock、崩溃报告对话框
- `present tick` 稳定 30 Hz（300 tick / 10 s），FIFO `next/stop` 同步前进，从不楔死
- 时钟每分钟自更新 —— 画面是活的，不是一帧贴图

### 47.4 为什么之前的 M6 "成功" 不能算作 STDU 有效

M6（`b89c619`，10-06）记录"锁屏完整实时渲染"，四要素是拓扑寄存器 + `UPDATE_GB_IMAGE`
+ PT64_2 + SCREENTARGET flag。今天把这四要素全部复现（§45 起）却始终黑，
而且洋红对照臂证明宿主根本不看 surface。

诚实地说：M6 那次画面**来源未确定**。当时同样没有写 HIDE，宿主日志里也是
`Screen type changed to ScreenTarget` + `Screen 1`，也就是说宿主那一侧的状态和今天
没有任何区别。两种解释都成立：(a) 当时某条我们已删掉/改写的路径真的生效过；
(b) 当时看到的画面来自宿主自己的 legacy 扫描输出，而不是我们的 surface。
**在找到可复现的对照之前，不要把 M6 当作"STDU 可用"的证据引用。**

### 47.5 出货配置

| 开关 | 值 | 说明 |
|---|---|---|
| `kFbStage` | **5** | legacy screen-object 呈现（唯一出画面的通路） |
| `kGbStage` | **0** | 不建 GB 对象栈；STDU 代码保留，改回去只需改这一个数 |
| `kStduEnableHide` | 0 | 但**仍然写** `ENABLE=1`，防止上一次实验留下的隐藏态 |
| `kStduVramControl` | 0 | 洋红对照臂（实验用；开着会在每次启动闪一下洋红） |
| `kDxTest` | 0 | DX 主线已关闭（§35） |

STDU 全套代码、`kStduFlat`、`kStduDma`、`kStduImage`、`kStduPt1`、`kStduMatchHost`
都保留。回到 STDU 实验只需 `kGbStage=5`，但先回答一个问题：
**这台宿主到底在什么条件下才会采样 screen target？** 目前没有任何一个 boot 观察到过。

### 47.6 工具与流程上的两个修正

1. **`build.sh` 的开关判定必须锚定声明行。**
   `grep -E 'kGbStage[[:space:]]*=[[:space:]]*5'` 命中了注释里的
   "never together with kGbStage = 5 on a boot we care about"，
   于是用 `kGbStage=0` 的二进制去查 STDU 标记，构建报 `FATAL` 但原因和代码无关。
   现在统一走 `sw <名字> <值>`，正则锚在 `static const <类型> <名字> = <值>;`。
   这个文件通篇都是"某开关 = 某值"的注释，不锚定就等于随机。

2. **`build.sh` 的 STDU 标记改成 `kGbStage == 5` 条件成立时才要求。**
   在此之前它是无条件的，等于**禁止构建另一条呈现通路**——而那条通路正是答案。
   可达性守卫不该锁死合法配置。

3. **Windows 侧 `tools/mssh.py --put` 的 ENOENT 是 Git Bash 路径转换。**
   `/var/tmp/x` 被 MSYS 改写成 `C:/Users/.../PortableGit/var/tmp/x` 之后才交给 paramiko，
   SFTP 当然说"没有这个文件"。`MSYS2_ARG_CONV_EXCL='*'` 或在 `--push`
   （走 exec 通道 + base64）之间选一个。**不要把它误诊成客机 sftp-server 的问题。**

### 47.7 下一步（按价值排序）

1. **分辨率**：现在锁死 1024x768，因为 VRAM 只有 4 MB（`gfbSize` 写在
   `vmware-vmx.exe 0x14050aa98`）。1920x1080 需要 8.29 MB。这是纯宿主侧限制，
   要改就得再打一次宿主补丁（参照 §19 的 1 字节补丁流程）。
2. **`ABGR`/`BGRA` 校验**：GMRFB 用的是 `BGRX32`，屏幕颜色看着正确，但没做逐像素比对。
   便宜的做法：填一块已知 RGB 的色卡，截图读回数值。
3. **脏矩形**：现在每 33 ms 全屏 blit。改成按 `SVGA_CMD_UPDATE` 区域 blit 能省带宽，
   但先要有 vblank/damage 来源，暂不做。
4. **回到 3D**：GMRFB 通路只能搬像素，vGPU10 的 3D 仍然卡在 DX 管理器从不创建（§19/§22）。
   显示通了不代表 3D 通了，不要混为一谈。

## §48 3D 重开（2026-11-13）：地址基线纠错、tick 是活的、以及静态模型的自相矛盾

用户把目标重新定为「完整支持 DX3D 加速」之后，第一件事是回去核对 §19 那个 1 字节补丁
还在不在 —— 结果是**还在位**：

```
C:/Program Files\VMware\VMware Workstation\x64\vmware-vmx.exe   22,967,216 B
  26.0.0 build-25388281
  0x87293 = FC   （周围机器码 F7 45 A7 FC FF FF FF = test dword [rbp-0x59], 0xFFFFFFFC）
  备份 vmware-vmx.exe.bak-dxflag（原始版本，同尺寸）仍在
```

也就是说 §19.5 的前提没有丢。但 §22 已经证明：**光放开 flags 不够**，真正挡住的是
「DX 管理器只在时间片 tick 里被惰性创建」。所以这一轮的全部工作都花在把那条链问清楚上。
结论分两部分：一批**必须撤回的地址错误**，和一个**连自相矛盾都没解决**的现状。

### 48.1 纠错：`dev` 不是 `0x1414931C0`（该地址零引用）

§22.2 / §27 / §28 全部以 `dev = [0x1414931C0]` 为基准。用新的引用统计脚本
（`disasm/globalrefs.py`）扫全 `.text`：

| 全局 | rip 相对引用数 | 结论 |
|---|---|---|
| `0x1414930A0` | **98** | DX 管理器，§22 记的地址是对的 |
| `0x1414931C0` | **0** | 不存在的东西，§22.2 写错了 |
| `0x1413F31C0` | **891** | 真正的 SVGA 设备对象；150 个字段被访问 |

证据不是"引用数多"，而是两道闸门本身就读它：

```
0x1404c796f  mov r10,[rip+0xf2b84a]   -> 0x1413F31C0   ; cmp byte [r10+0x81d],0   ← gate
0x1404c7a0d  mov rax,[rip+0xf2b7ac]   -> 0x1413F31C0   ; mov ecx,[rax+0x9bc]      ← gate
0x1404c7a1a  mov rax,[rip+0xf2b79f]   -> 0x1413F31C0   ; cmp byte [rax+0x71],0    ← gate
0x1404125a0  mov rdx,[rip+0xfe0c19]   -> 0x1413F31C0   ; 槽计数重算
```

**受影响的历史结论**（不是全盘作废，但要按新地址重读）：

- §22.2 的"创建点在 `0x140509f20`、唯一写入点 `0x14050a077`"——**成立**，这两个地址是用
  写指令自己反解出来的（`mov [rip+0xf89022],rax` → `0x1414930A0`），不依赖 `dev`。
- §27 "`[dev+0x9bc]` 是上下文活跃缓存"——**方向对**，但它说的"只有 2 个写入点"是按错误
  对象扫的。
- §28 "`[dev+0xad8]` 是 SVGA 寄存器 0x12 的影子、恒为 0"——**应当撤回**。新证据：
  `[dev+0xad8]` 是**两个槽的 dword 计数数组**（槽0 `+0xad8`、槽1 `+0xbf0`，步长 `0x118`），
  重算函数 `0x1404125a0` 逐个比较"`dword [slot] > 0`"来决定 `[dev+0x9bc]`。
  §28.1 看到的 `mov word [rbx+0xad8], r9w` 是另一个结构（同一个偏移、不同的基址）。

### 48.2 tick 是活的：之前"0 callers"是扫描器的盲区

§23.1 记「`0x1404c5ee0` ← 0 callers（tick 回调/定时器）」。用 `.pdata` 逐函数扫确实看不到，
因为真正的引用形式是：

```
0x1401565f0  jmp 0x1404c5ee0          ; CFG thunk 垫片（jmp + int3 补齐到 16 字节）
0x14014c278  call 0x1401565f0         ; ← 真调用者
0x14014c140..0x14014c286              ; ← VMX 主事件循环（忙轮询）
```

`0x14014c278` 位于一个循环体里：`call 0x140156610` → `call 0x14017cee0` →
原子 `or`/`xchg` 位图 → 位图空时 `call 0x1401565f0`（进入 tick）→ `je 0x14014c140` 回到循环顶。
所以 tick 跑得很勤，**它不是"永不触发"，而是每一次主循环都可能触发**。

两条扫描教训（已固化成工具）：

1. **`.pdata` 覆盖不到 CFG thunk 和非 pdata 的汇编**。必须再补一遍全 `.text` 的
   `E8/E9 rel32` 原始扫描（`disasm/caller_chain.py` 就是这么做的，并且会把 thunk 两侧都走）。
2. **capstone 的 RIP 相对寻址 `mem.base` 是 `X86_REG_RIP`，不是 0。**
   第一版脚本按 `base == 0` 判断，于是两个全局都报 "0 load site(s)"，
   差点又得出一个"这个地址没被用过"的错误结论。改过来后立刻从 0 → 891。

### 48.3 四道闸门的真实语义（全部落到了具体写点）

创建调用点的完整判据（`0x1404c7950`）：

```
0x1404c7979  cmp byte [dev+0x81d],0 ; je  bail      (A) 必须 != 0
0x1404c7987.. 若干计数器比较（"跑够久了没有"）        (B) 必须都不超时
0x1404c7a02  movzx eax,[0x14140B043] ; test al,al; je bail   (C) 必须 != 0
0x1404c7a14  mov ecx,[dev+0x9bc] ; test ecx,ecx ; je bail    (D) 必须 != 0
0x1404c7a25  cmp byte [dev+0x71],0 ; jne bail        (E) 必须 == 0
0x1404c7a2b  call 0x140509f20                        ★ 创建
```

| 条件 | 字段 | 唯一写点 | 语义 | 客人可控？ |
|---|---|---|---|---|
| (A) | `[dev+0x81d]` | `0x1405098d4`（在 `0x1405098c0(bool)` 里） | `SVGA_REG_ENABLE` 的 bit0 状态 | **是**，写 ENABLE=1 |
| (C) | `[0x14140B043]` | `0x1404d8ff9`（置 1） | 配置初始化标志 | 开机即 1，不用管 |
| (D) | `[dev+0x9bc]` | `0x1404125e3` / `0x1404125ef` | 两个 DC 槽是否有计数 > 0 | 只能间接 |
| (E) | `[dev+0x71]` | `0x140508b0b`(清) / `0x14050a74f`、`0x14050a7fc`(置 1) | 设备停止/忙 | 运行时通常 0 |

**(A) 的来源是一处重要发现**：`[dev+0xb4]` 原来是 `SVGA_REG_ENABLE` 的影子，
唯一写点 `0x140415cef`，写完立刻打印：

```
0x140415d0c  lea rax,[rip+0x77c2c5]   -> 0x140b91fd8 "hiding"
0x140415d16  lea rdx,[rip+0x77c2c3]   -> 0x140b91fe0 "enabling"
0x140415d...  "SVGA %s SVGA"
```

这正是 §47 在宿主日志里看到的 `SVGA hiding SVGA` / `SVGA enabling SVGA` 的出处。
也就是说 **ENABLE 同时喂给两道门**：`[dev+0xb4]`（tick 的 `test ecx,ecx`）和
`[dev+0x81d]`（`0x1405098c0` 里从 `cl` 直写）。这一条把 §47 的实验
和 DX 链**接上了**：当时写 `ENABLE|HIDE=3` 时，DX 闸门的 (A) 其实是开着的。

### 48.4 但创建函数自己也查同一个槽，而且创建块静态不可达

`0x140509f20` 的开头就是另一个槽扫描，和 tick 里那道一模一样：

```
0x140509f87  movzx r8d,[rdx+0xbf8]
0x140509f90  test r8b,r8b ; je  0x140509fb1          ; 槽1 的 active 字节
0x140509f95  cmp byte [rdx+0xae0],r14b ; jne 0x140509fa3
0x140509f9e  cmp rax,rbx ; jne 0x140509fb1
0x140509fa3  cmp dword [rcx+rdi+0xad8],0 ; ja 0x14050a194   ; 任槽 count>0 → 走收尾
0x140509fb1  dec esi / dec rax / sub rcx,0x118 / jns loop
0x140509fcc  call 0x1404125a0      ; 重算 [dev+0x9bc]
0x140509fd9  test r14b,r14b ; je 0x14050a4c1                ; r14b 恒 0 → 一定跳走
0x140509fe2  mov r9d,3 → 落进 0x140509fe8 …  ★ 真正创建在这里
```

问题就在这里：`r14b` 在入口 `0x140509f53` 被 `xor` 成 0，之后到 `0x140509fd9` 之前
**没有任何指令写它**，所以 `je` 恒成立；而 `0x140509fe8`（含 `0x14050a077` 那个唯一写入）
**没有任何跳转指向它**（`caller_chain.py` 报 0 callers，全 `.text` 原始扫描也没有）。

换成一句直白的话：**按静态阅读，DX 管理器永远不可能被创建。**

### 48.5 这与已知事实矛盾 —— 说明静态模型不完整，立即停止据此设计实验

同一台宿主、同一个 `vmware-vmx.exe`（pid 3024 是 Windows 那台 VM 的 vmx），
Windows 10 客人日志里有 38 行 `Guest: vm3d:`，包括：

```
vm3d: WDDM SVGA WDDM Full Display driver, Version: 9.17.11.0003  Device: SVGA2
vm3d: WDDM 3D is on.          vm3d: WDDM DX10 context is on.
vm3d: WDDM Preemption Aware is on.        vm3d: WDDM HP Queue is on.
vm3d: WDDM Guest backed surface is on.    vm3d: WDDM GL3 is on.
vm3d: WDDM VRAM 4096KB.
vm3d: VM3DDevAdapterGrowOTableItem: Grow OTable 0 from 1024 entries to 2048, use 8 pages
     [threadName="vcpu-6"]
```

三条可用的推论：

1. **DX 在这台宿主上是真的能跑的**，包括 otable 增长这种 vGPU10 操作。所以
   「本宿主不支持 DX」这个说法到此为止，不要再写。
2. **DX 命令在 vCPU 线程上处理**（`vcpu-6`）。主循环 tick 跑在 `vmx` 线程，
   两者并发 —— 这才让"tick 观察到在飞槽"在原理上成立。
3. `WDDM VRAM 4096KB` 与 macOS 侧 `SVGA_REG_VRAM_SIZE=4194304` 一致 ⇒
   **4 MB 是 VMX 全局限制**，不是 macOS 路径特有（§47.7 第 1 条成立，范围更大）。

既然静态模型自己打架，继续拿它推导"该补哪个字节"就是在赌。**下一步必须先拿实测状态**，
见 §48.8。

### 48.6 本轮新增的分析工具（`disasm/`，此前未入库）

| 文件 | 作用 |
|---|---|
| `gate_probe.py` | 一次跑完三问：tick 的取址引用、闸门前后 0x120 字节反汇编、三个闸门字段的读写点 |
| `objfields.py` | 给定全局，列出"从它加载的寄存器"访问过的所有字段（W/R 计数 + 写点地址） |
| `globalrefs.py` | 给定全局，列出**全部**引用（不管地址怎么进寄存器），标 W/R 与所在函数 |
| `ripscan.py` | 反汇编一个窗口并把每条 RIP 相对操作数解析成绝对地址；附带全局引用计数 |
| `caller_chain.py` | 全 `.text` 的 `E8/E9` 原始扫描 + thunk 折叠，上溯调用链（能穿过 CFG 垫片） |
| `dump.py` | 反汇编任意窗口，解析 RIP 目标并尝试把目标当字符串打出来 |

`tools/vmxmem.py` 是**新的方向**：直接读活的 `vmware-vmx.exe` 进程内存
（`ReadProcessMemory`，读 `[dev]`、`[0x1414930A0]`、各闸门字段）。
**它需要提权**：vmx 以高完整性级别运行，非管理员 `OpenProcess` 返回 5。
脚本对失败是降级处理而不是崩，`list` / `<pid>` / `all` / `--out` 四种用法。

### 48.7 流程教训

1. **别用"0 引用"当结论，先怀疑扫描器。** `0x1414931C0` 的"零引用"是真的，
   但同一批脚本对 `0x1413F31C0` 也报过零引用 —— 那是 capstone 的 `X86_REG_RIP` 造成的。
   两个零引用长得一样，含义完全不同。
2. **pdata 不是代码的全集。** 漏掉 CFG thunk 会让一个活函数看起来像死代码。
3. **静态结论撞上实测事实时，实测优先。** §48.4 的矛盾花掉的时间，比直接读一次
   宿主内存多得多。

### 48.8 下一步：先测，再补

1. **提权跑 `tools/vmxmem.py all`**，一次拿到两台 VM 的：
   `[0x1414930A0]`（DX 管理器）、`dev+0xb4 / +0x71 / +0x81d / +0x9bc / +0xad8 / +0xbf0`。
   **这一步同时验证两件事**：Windows 那边应当非空（校验我们的理解），
   macOS 那边缺的是哪一项（定位真正的卡点）。
2. 如果 Windows 侧 `dxManager != 0`：对比它的四个闸门值，找出 macOS 缺的那一项，
   再决定是"客人补一条命令"还是"宿主补一个字节"。
3. 如果 macOS 侧 `[dev+0x9bc]` 恒 0 且 Windows 侧也只在处理期短暂为 1：说明这就是一条
   **竞争路径**，那时才轮到宿主补丁，且补丁点在 `0x1404c7a14` 与 `0x140509fa3` 两处，
   不是 §19 那种单字节。

### 48.9 实测：宿主内存直读（提权后，20 秒 / 397 采样）

有了 `tools/vmxmem.py` 就能直接看活进程，不必再靠 x64dbg attach。两次测量：

**(1) 一次性快照（两台 VM 都在跑）**

| 字段 | Windows VM (pid 3024) | macOS VM (pid 39348) |
|---|---|---|
| `[0x1414930A0]` DX 管理器 | **0** | **0** |
| `[dev+0xb4]` ENABLE 影子 | 1 | 1 |
| `[dev+0x81d]` ENABLE bit0 | 1 | 1 |
| `[dev+0x71]` stopped | 0 | 0 |
| `[dev+0x9bc]` 任一槽活跃 | 0 | 0 |
| `[dev+0xad8]` 槽0 计数 | 0 | 0 |
| `[dev+0xbf0]` 槽1 计数 | 0 | 0 |
| `[dev+0xae0]` 槽0 标志 | **1** | **0** |
| `[dev+0xbf8]` 槽1 标志 | **1** | **0** |
| `[dev+0xf4]` caps | `0xfdffc3e2` | `0xfdffc3e2` |
| `[dev+0x1c0]` caps2 | `0x77f` | `0x77f` |

两台 VM 的 `dev` 指针**相同**（`0x7ff749a15a60`）、模块基址也相同（`0x7ff7482a0000`）。
这不是读错进程：VMware 的 `.per_vm_` 段就是按固定布局映射的，两个 VM 布局一致是设计如此。

**(2) 连续采样 20 s（每 50 ms，各 397 次）**

```
pid 3024 (Windows)              pid 39348 (macOS)
  dxManager : 0x0 x397            dxManager : 0x0 x397
  9bc       : 0x0 x394, 0x1 x3    9bc       : 0x0 x397
  ad8       : 0x0 x395, 0x2 x2    ad8       : 0x0 x397
  bf0       : 0x0 x395, 0x1 x2    bf0       : 0x0 x397
  ae0       : 0x1 x397            ae0       : 0x0 x397
  bf8       : 0x1 x397            bf8       : 0x0 x397
```

**三条结论：**

1. **闸门 (D) 的机制是真的**：Windows 那边 `[dev+0x9bc]` 确实会短暂变 1（3/397 ≈ 0.8%），
   槽计数也出现过 2/1。所以 §48.3 对 `[dev+0x9bc]` 的解读成立，它不是死字段。
2. **但 `[0x1414930A0]` 在 Windows VM 里 397 次全为 0** —— 而同一台 VM 的日志明确写着
   `vm3d: WDDM DX10 context is on` 并跑过 `Grow OTable`。
   ⇒ **「DX 管理器为 NULL ⇒ DX 不能用」这个因果链站不住**。要么这台 VM 当时确实没有 DX
   负载（锁屏/DWM 空闲），要么那条路径根本不看这个指针。**这一点还没分辨出来。**
3. **macOS 侧与 Windows 侧唯一的实质差异是槽标志 `ae0` / `bf8`**：
   Windows 都是 1，macOS 都是 0。按 `0x1404125a0` 的重算逻辑，
   `[dev+0x9bc]` 想要变 1 必须 `[dev+0xbf8] != 0` 且某个槽计数 > 0。
   **macOS 连第一步都没做到** —— 也就是说 §17/§22 那些 `START_STOP_CONTEXT`
   在设备态上并没有留下痕迹（本次量到的就是 0）。

**因此下一步最该做的实验（不需要宿主补丁）**：让 kext 正确地
`SVGA_DC_CMD_START_STOP_CONTEXT{enable=1, context=0}`，然后用 `vmxmem.py`
**在宿主侧直接验证 `ae0`/`bf8` 是否变 1**。这个闭环以前做不到（客人看不到宿主字段），
现在一条命令就能判。只有这一步成立，才轮得到 `[dev+0x9bc]` 和创建路径。

**工具使用前提**：`vmxmem.py` 必须提权，而且**光管理员不够** —— 还要显式启用
`SeDebugPrivilege`（默认是 present-but-disabled）。另外 `GetCurrentProcess()` 必须声明
`restype = HANDLE`，否则 `OpenProcessToken` 在 x64 上会报 `ERROR_INVALID_HANDLE (6)`。
两个坑都已在脚本里注释。

### 48.10 槽结构被读清楚了，而且它可能根本不是"DX 的东西"

`[dev+0xae0]` 是个 **word**（`0xae0` 有效字节 + `0xae1` 待停标志），不是单字节。
每槽布局（步长 `0x118`，槽0 `+0xad8`、槽1 `+0xbf0`）：

| 偏移 | 宽度 | 语义 | 写点 |
|---|---|---|---|
| `+0xad8` | dword | 在飞命令计数 | `0x140086e01` / `0x140086f4b` 增、`0x14050a1e6` 减 |
| `+0xadc` | dword | 另一路引用计数 | `0x1404d7364` 减 |
| `+0xae0` | byte | **已启用** | **置位 `0x14050a47b`**、清 `0x1404d738e` |
| `+0xae1` | byte | 待停 | `0x1404d7372` 判 |

收尾逻辑（`0x1404d7364` 起）只有三个计数全归零才清 `ae0`：

```
dec  dword [slot+0xadc]
cmp  byte [slot+0xae1],0 ; je out
cmp  dword [slot+0xadc],0 ; jne out
cmp  dword [slot+0xad8],0 ; jne out
mov  word [slot+0xae0],0        ; ← 三个都 0 才清"已启用"
call 0x1404125a0                ; 重算 [dev+0x9bc]
```

**关键线索**：置位 `0xae0` 的 `0x14050a47b` 位于片段 `0x14050a20d`，而进入那个片段的
唯一分支是 `0x14050a052 je 0x14050a20d` —— 也就是 **"这个 CB 不是 DX CB"** 的那一边。

如果这条读法成立，那么 `[dev+0x9bc]`（DC 槽活跃）是 **legacy GB/3D 命令处理时**才会置位的，
而不是 DX 命令。这也解释了 Windows 那边为什么 `ae0/bf8` 常驻 1：
`vm3d` 的日志里明确有 `WDDM Guest backed surface is on` —— 它同时用 GB 通路。

⇒ **下一个判据应该换**：不要再去"让 DX CB 把槽点亮"，而是先让 **legacy GB 命令**
把槽点亮，再在同一段时间里发 DX CB，看 `[0x1414930A0]` 会不会变。
这也是我们既有代码里 `kGbStage` 那条线本来就在做的事。

**我现在的判断（写下来备查）**：`[0x1414930A0]` 可能不是"DX 管理器"，
而是 legacy 3D/GB 处理器的一个附属对象。§19 把它命名成 DX 管理器，
后续 §20–§35 全部围着这个名字转 —— 名字一旦错，方向就跟着错。
下一步值得花 20 分钟做的是**用 `vmxmem.py` 在 `kGbStage=5` 的启动里采样**：
如果那时 `[0x1414930A0]` 非空，就证明它是 GB 侧的对象，与 DX 无关。

## §49 空 COTable ≠ 空视图：RTV 应该由表行定义，而不是 1187

**这一节推翻的是整场 §26–§35 战役的前提**。

`SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW`（1187）不是创建 RTV 的唯一途径，而且
**vm3dmp 从来不发它**：把 `driver/vm3dmp.sys` 的 `.text` 整段扫一遍，`1185..1199`
（全部 `DEFINE_*_VIEW` / `DEFINE_*_STATE`）**一个立即数都没有**。它用的是
1143/1145/1207/1274，RTV 靠 **COTable 行**定义，再用 `SET_COTABLE` 绑定整张表。

权威结构（`svga3d_dx.h`，`pack(1)`，32 字节）：

```c
typedef struct {
    SVGA3dSurfaceId     sid;              // +0
    SVGA3dSurfaceFormat format;           // +4
    SVGA3dResourceType  resourceDimension;// +8
    SVGA3dRenderTargetViewDesc desc;      // +12  mipSlice/firstArraySlice/arraySize
    uint32 pad[2];                        // +24
} SVGACOTableDXRTViewEntry;
```

我们此前用 `bzero()` 清空 4 KiB 的 RTVIEW 表，于是 `rtvId 0` 解析成
`{sid 0, format 0, dimension 0}` —— 一个"什么都没有"的视图。然后一直发 1187 去
*创建*它，而 1187 的 handler 又卡在一个本构建从未创建的状态对象上。两头都错。

改法是：`SET_COTABLE` 之前在表里写好行。**实测 `SET_COTABLE {cid 1 mob 5 type 0,
validSize 4096} -> ACCEPTED`**（有史以来第一次），说明表绑定这一环一直是好的。

两个顺带修掉的错位：

* 行写在 index 0，而 CLEAR 打 index 1（`trtv`）。即使 CLEAR 通了，它引用的也是一行
  全零的视图。现在两边共用 `kDxTemplateRtvId`。
* 视图 format 写的是 A8R8G8B8(2)，可它指向的 surface 定义成 X8R8G8B8(1)。已对齐。

## §50 `_gbVirt == NULL`：那个 panic 的真身是 `0x3040`

把 1187 改成非致命之后，客体第一次走进了 CLEAR/READBACK 段，随即 kernel panic，
并陷入每 ~72 秒一次的重启循环。panic 报文里两条信息是关键：

```
CR2: 0x0000000000003040        RDI: 0x0000000000003040
Error code: 0x0000000000000002   <- bit1 = 写故障，RDI 就是被写的目的地址
RIP: 0xffffff8003701092          <- 在内核 __HIB 段，不在 kext 里
```

`RDI = 0x3040` 是目的地址、长度 4 —— 这是 `memcpy(dst, &local, 4)`，不是空指针解引用。
对一下常量：

```
kGbOffMobTable = 0x3000, kGbMobEntryBytes = 16, mobId = 4
0x3000 + 4*16 = 0x3040          ← 精确命中
```

即 `dxTemplateSetupSurface()` 里的 `memcpy(mobTable + mobEntryOff, &mobDepth, 4)`，
而 `mobTable = (char*)_gbVirt + kGbOffMobTable`。**`_gbVirt` 是 NULL**，因为
`kGbStage = 0` 时 `gbBringUp()` 在分配工作缓冲之前就 `return` 了，而 DX 实验要往
对象表里写东西。所以这是"我没给实验分配它的工作区"，不是宿主的问题。

三处改动：

1. `gbBringUp()` 引入**有效 stage**：`kGbStage == 0 && kDxTest` 时按 stage 4 拉起
   GB 对象栈（对象表、数据 MOB、surface、**DXCONTEXT otable**），但绝不进 stage 5
   （STDU 与 DX 实验同开会复现 `DEFINE_GB_SCREENTARGET` 处的 FIFO 中止）。
   显示仍然走 screen-object 呈现通路（`kFbStage = 2`），与 GB 无关。
2. `dxTemplateSetupSurface()` 开头加空指针护栏，把"panic + 重启循环"降级成一行日志。
3. 客体抢救手法（以后别再花四个启动去摸）：**删掉 `/Library/Extensions/vgpuFramebuffer.kext`
   再重启**，kext 不再加载 → 不再 panic → 拿到不受限的时间窗，然后从容 build+install。

## §51 DX 派发器缺席：一次分级探测把"3D 到底能不能渲染"问死了

§35 的结论是"DX 主线关闭"，依据是宿主侧 `[0x1414930A0] == 0`。§48.5 已经证明这个
依据不成立（跑 DX 的 Windows 客体里 397 次采样也全是 0），所以 §35 一直是**结论对、
理由错**。本节把它换成可复现的证据链。

### 51.1 先排除配置层

宿主日志同时给出两件事：

```
MKSRenderMain: PowerOn allowed BasicOps ISB DX12 DX11 DX11Basic VK
Vulkan Renderer: Created with LUID: 0x0000000000014cf3
SVGA3dCaps: guest, compatibility level: 10
```

渲染器是真的（DX12/DX11/VK 都允许），客体被告知 vGPU10。`.vmx` 里
`mks.enable3d = TRUE` + `mks.enable3d.available = TRUE` 就是
`docs/vmware-svga-capabilities.md` 记录的正确组合。

试过一个额外旋钮：把 `vmotion.svga.baseCapsLevel` 从 `"9"` 改成 `"11"`（渲染器
自报 11，日志里 `after clamping` 变成 9）。**无效** —— VMware 在 power-on 时把它
写回 9，重启后仍是 `after clamping num 9`。这个键是派生的，不是输入。
（`svga.allowSVGA3` 按文档本来就不需要，至今仍保持注释状态。）

结论：**平台侧已经到顶，不能再从配置里榨出 3D。**

### 51.2 上下文管理命令是通的，渲染命令不是

同一次启动内，全部走 CB：

| 命令 | 号 | 结果 |
|---|---|---|
| `DX_DEFINE_CONTEXT` | 1143 | **ACCEPTED**（COMPLETED） |
| `DX_BIND_CONTEXT` | 1145 | **ACCEPTED** |
| `DX_SET_COTABLE` | 1207 | **ACCEPTED**（首次） |
| `DX_CLEAR_RENDERTARGET_VIEW` | 1176 | **COMMAND_ERROR, errorOffset 0** |
| `DX_DEFINE_RENDERTARGET_VIEW` | 1187 | **COMMAND_ERROR, errorOffset 0** |
| `DX_SET_SHADER_IFACE` | 1274 | refused |

### 51.3 `dxContext` 假设 —— 干净地证伪

翻 `svga3d_dx.h` 时发现一条被忽略的事实：

```c
typedef struct SVGA3dCmdDXClearRenderTargetView {
    SVGA3dRenderTargetViewId renderTargetViewId;   // 没有 cid
    SVGA3dRGBAFloat rgba;
} SVGA3dCmdDXClearRenderTargetView;
```

1176 和 1187 **都不带 cid**。能告诉宿主"这条命令属于哪个 DX 上下文"的只有
CB 头里的 `dxContext`，而我们出于 §15/§22 的历史结论一直把它强制成 0
（`kCbKeepDxContext = 0`）。这完美解释了 51.2 的表：**自带 cid 的命令过，不带 cid 的
被拒**。

于是打开 `kCbKeepDxContext = 1`，并给渲染命令传 `flags = DX_CONTEXT`、`dxContext = 1`。
历史记录说这样会撞 `STATUS 4 / CB_HEADER_ERROR`（header 层的 `lookup-or-create`
失败）—— 但那些测量**都发生在 `DEFINE_CONTEXT` 首次成功之前**。

实测结果：

```
cb: STATUS 3 (errorOffset 0)          <- 不是 4！CB 头通过了
dx: TEMPLATE CLEAR_RTV {rtv 1 green, dxContext 1} -> refused
```

**头没被拒**，说明宿主确实解析出了 `dxContext = 1`。但 CLEAR 依旧 `COMMAND_ERROR`。
假设证伪，而且是好事：它把"头层没有 DX 上下文"这条路排掉了。

### 51.4 分级探测 + 二选一对照：派发器缺席

`errorOffset 0` 只说明"CB 里第一条命令失败"。把四条命令排进**同一个** CB，让
`errorOffset` 指认第一条被拒的：

```
byte  0   DX_SET_TOPOLOGY (trianglelist)      无 COTable、无资源
byte 12   DX_SET_RASTERIZER_STATE (none)      无 COTable
byte 24   DX_SET_DEPTHSTENCIL_STATE (none)    无 COTable
byte 40   DX_CLEAR_RTV {rtv 1, green}         需要 RTVIEW 表行
-> SURVEY status 3 errorOffset 0 -> first refused: SET_TOPOLOGY (byte 0)
```

`SET_TOPOLOGY` 是能想到的最简单的一条 DX 命令：4 字节 body、无 id、无表、无视图。
它在 byte 0 就被拒。

那么问题可能根本不在 DX ——或许是**传输层**根本没有 3D 派发。用同一条 CB 通路发一条
**legacy** 命令（`SVGA_3D_CMD_CLEAR`，1057，自带 cid、不需要任何 DX 概念）作对照：

```
dx: BISECT legacy SVGA_3D_CMD_CLEAR {cid 1} -> refused (no 3D dispatch through CB at all)
```

**legacy 也被拒。** 于是结论收敛成一句可复现的话：

> 这台宿主的 SVGA3D 设备对外宣称 vGPU10（caps 全置、DXCONTEXT/SM41/SM5 都为 1），
> CB/fence 机制完全可用，DX 上下文管理命令全部 ACCEPTED ——
> 但**任何 3D 渲染/状态命令都不被派发**，无论 DX 还是 legacy，
> 一律 `COMMAND_ERROR / errorOffset 0`。

这与 §10-10 在 legacy FIFO 上的观察（"parse+fence 每条 3D 命令，但从不执行渲染管线"）
是同一件事的两种测法。**§35 的结论成立，但理由要从"DX 管理器为 NULL"改成"3D 派发器
不在此配置中"。** 在拿到一台 3D 真能派发的宿主之前，Metal/IOAccelerator→SVGA3D 这条
Level-2 路线没有可依托的下层。

复现方式：`kDxTest = 1`（且 `kGbStage` 保持 0），重启，看 `dx: SURVEY` / `dx: BISECT` 两行。

## §52 可交付的像素：2D 通路 + 驱动自绘图案（已截图验证）

既然 3D 引擎不可达，"像素"这份交付必须落在 2D 上，而且要**证明**而不是声称。

`kPaintTestPattern = 1` 时，`presentSetup()` 在**第一次 blit 之前**往 VRAM 里画：

* 8 条整高竖色带（白/黄/青/绿/品红/红/蓝/黑）—— `BYTES_PER_LINE` 只要错一点，色带立刻变斜；
* 1 像素白框 —— 高度或首行偏移错就会缺边；
* y = 1 处一条 2 像素红线 —— 顶部 pitch 探针；
* (8,8) 起 72×72 的 8 像素棋盘 —— 证明子区域寻址。

日志回读与截图互相印证：

```
present: TEST PATTERN painted into VRAM (1024x768, pitch 4096 B, 1024 dwords/row)
         first=0xffffffff mid=0xffff00ff last=0xffffffff
present: screen 0 defined (1024x768), GMRFB = BAR1+0, blit acked -- screen-object present path is UP
```

开机早期截图（`docs/shots/2026-11-14-test-pattern.png`）：八条色带**完全垂直**、
白框完整、棋盘就位、顶部红线可见。也就是说客体的写、宿主的扫描输出、以及我们
回读的 pitch/FB_OFFSET 三者一致。

再过十几秒 WindowServer 把它整个覆盖掉，屏幕变成 macOS 登录界面
（`docs/shots/2026-11-14-login-screen.png`）—— 这反过来是更强的证据：
**WindowServer 正在用我们的 framebuffer 合成**，显示通路是活的、可用的，
缺的只是加速而不是"能不能出画面"。

## §53 2D 引擎的判决：屏幕对象模式下只有 blit，没有绘制（2026-11-14）

§51 把 3D 问死之后，"Level-2（Metal / IOAccelerator）能不能落"就只剩一个问题：
**下层还有没有任何能让宿主替我们画东西的能力。** 这一节回答它，并且先把
"我有三次答案都是错的"写清楚 —— 因为错的地方本身是可复用的教训。

### 53.1 三个方法论错误，一个比一个隐蔽

| 版本 | 做法 | 为什么不算数 |
|---|---|---|
| v1 | 屏幕回读进 BAR1，再和 BAR1 比 | BAR1 **就是** blit 到屏幕的源。`BLIT_SCREEN_TO_GMRFB` 落到自己的源上，"宿主画了"和"我们把自己的缓冲区拷回自己"字节完全相同；对照像素还挑了同一根色带里 |
| v2 | 回读进 FB 之后的 scratch（0x300000） | 映射窗口**只到 FB 结束**：`IODeviceMemory::withSubRange(vram, fbOffset, pitch*height)`。0x300000 正好是一格外，读到全 0；于是 `src==dst` 把 0 和 0 比成"拷贝成功" |
| v3 | scratch 移进映射内（§53.2） | 结构对了，但 `capture -> refused`：回读命令根本拿不到 fence，于是只能报 UNVERIFIED |

教训收敛成两条：

1. **回读的落点必须与被比较的缓冲区无关**，否则测的是自拷贝；
2. **"读到的值都是 0 / 都是同一个值"必须先当成通道故障**，而不是当成结论。
   v2 就是这么把一个不存在的 "COPY EXECUTED" 写进日志的。

顺带记下本次真正踩到的地基事实（以后所有 scratch/readback 实验都要用它）：

```
_aperture = IODeviceMemory::withSubRange(vram, _deviceFbOffset, _apertureLength)
_apertureLength = _devicePitch * _deviceHeight          // = 0x300000 @1024x768
VRAM_SIZE       = 0x400000                              // 4 MiB
```

**4 MiB 的 VRAM 里只有 3 MiB 可被 kext 直接寻址。** 第 4 MB 存在但不在这张映射里。

### 53.2 通道不可用：屏幕是只写的

v3 实测（`kProbe2D = 1`，`2d: scratch` / `2d: CHANNEL` 两行）：

```
2d: scratch 0x200000 (row 512, 256x256, 1024 B/line) -> mapped+writable, sentinel -> 0x5a5a5a5a
2d: CHANNEL blit-> acked, capture -> refused, painted 0xFF00FF00 read back 0xdeaddead
            -> READBACK UNUSABLE
```

* scratch 在映射内、CPU 可写（哨兵读回原值），所以不是地址问题；
* `BLIT_GMRFB_TO_SCREEN`（写屏幕）acked；
* `BLIT_SCREEN_TO_GMRFB`（读屏幕）**拿不到 fence** —— 命令被解析、被跳过、从不完成。

一句话：**在 `SVGA_FIFO_CAP_SCREEN_OBJECT` 模式下，屏幕对象是只写的。**
宿主不保留可回读的前台内容，所以"把屏幕读回来做数值判据"这条路在本宿主上关闭，
唯一的 oracle 是外部截图。

### 53.3 判决：fill / copy 不改变显示内容

既然只能靠截图，就必须让"2D 引擎不动手"和"2D 引擎动了手但被覆盖"长得不一样。
做法是把**已知有效**的操作摆在最前面当对照，绘制操作排在它后面：

```
每 tick（33 ms）三条命令，顺序固定：
  1. BLIT_GMRFB_TO_SCREEN  全屏      <- 对照：§52 已证明它能把 VRAM 送上屏
  2. RECT_COPY  200x200 (100,100)->(600,400)
  3. FRONT_ROP_FILL 200x200 洋红 @ (400,100)   <- 最后一笔，没有后续覆盖
```

实测（本次启动 tick 2400 / 3000）：

```
2d: LOOP tick 3000 -- blit control acked, copy acked, magenta fill acked
```

三条全部 fence 成功。截图（`docs/shots/2026-11-14-2d-ctl-b.png`）逐像素核对：

| 判据 | 期望 | 实测 | 结论 |
|---|---|---|---|
| 对照：8 条竖色带 | 存在 | 存在（x=160 黄 / 300 青 / 450 绿 / 570 洋红 / 700 红 / 830 蓝） | blit 通路活着 |
| fill：(400..600, 100..300) 洋红 | 0xFFFF00FF | (450,150) = 绿（色带本色） | **fill 不改显示** |
| copy：(600..800, 400..600) | 白/黄/青 | (650,450) = 红（色带本色） | **copy 不改显示** |
| 我们自己写进 VRAM 的绿色 64×64 @ (600,300) | 出现 | 出现 | 证实上面的"存在"都是 VRAM 内容被 blit 上去 |

绿色方块是个好用的反证：它是 §53.2 通道自检时用 **CPU 写 VRAM** 画的那块
（`kParkX=600, kParkY=300, kParkW=64`），它出现在屏幕上，说明
"屏幕上有东西"只能归因于 blit + VRAM，2D 绘制没有贡献。

另外截图下部 y≥600 处色带整体错位，那是 scratch（rows 512..767 × x<256，按 256 像素
步距写入）以 1024 像素步距重新 blit 造成的斜切 —— 又一个"我们的写入确实上屏了"的旁证。

（第一版 tick 是"整屏涂绿 → 蓝块 → 拷贝"，结果三条命令全 acked、屏幕变**全黑**。
那次没法定论：全屏黑既可能是"宿主执行了填充但颜色被吃成黑"，也可能是"显示停止更新"。
所以才有本节这个带对照的顺序。全黑那次不再作为证据引用。）

### 53.4 结论与它意味着什么

> 在这台宿主 + 屏幕对象模式下，2D 引擎对外**只提供传输**（GMRFB → 屏幕的 blit），
> **不提供绘制**：`FRONT_ROP_FILL` 与 `RECT_COPY` 会被 FIFO 接收、会被 fence，
> 但对屏幕内容没有任何影响；屏幕还不可回读。

对 Level-2 的直接后果：

* IOAccelerator 需要的"宿主替我做 fill / copy / 合成"没有下层可站；
  我们唯一能做的就是**在客体内存里 memcpy 出像素再 blit**，那已经不是加速，
  而且 §52 已经做到了；
* 3D 派发器缺席（§51）意味着 Metal 需要的三角形光栅化更不可能；
* 于是 **Level-2 在本宿主上是"有条件不可实现"**：不是我们没找到调用方式，
  而是这台设备的模型根本没给客体一套能绘制、能回读的图形引擎。

要把它变成"可实现"，需要换一个前提，而不是再试一个参数：

1. 一台 3D 命令真能派发的宿主（VMware 对 Windows/Linux 客体给的就是这套，
   macOS 客体档案里没有）；
2. 或者拿到可用的 STDU/ScreenTarget 合成通路（§38.22 的因果链还没被推翻），
   让宿主直接消费 GB surface —— 这条路是"把显示交给宿主合成"，
   与"客体画"是两件不同的事，也是 §36 之后一直没走通的那条；
3. 或者在宿主侧改 `vmware-vmx.exe` 的 3D 门控（§48 的静态模型自相矛盾，
   在拿到第三台机器交叉验证前不再据此设计实验）。

复现方式：`kProbe2D = 1` 重启，等 `2d: LOOP tick` 出现后任意时刻 VNC 截图，
对照 §53.3 的四行判据。出货配置已恢复 `kProbe2D = 0`。

## §54 撤回 §53 的一半：2D 引擎的 `RECT_COPY` **是**能用的（2026-11-14 深夜）

§53 的结论有一半是错的，原因是我把命令线格式写错了。这一节记撤回、以及撤回之后
重新测出来的东西 —— 后者比原来的错误结论重要得多。

### 54.1 错在哪：多了一个「长度字」

`svga_reg.h` 里有一类命令的**第一个字段就是 `size`**（`SVGAFifoCmdDefineGMRFB`、
`SVGAFifoCmdDefineScreen` 里的 `SVGAScreenObject.structSize`、`SVGAFifoCmdEscape`），
也有另一类**完全没有 size 字段**：

```c
typedef struct { uint32 color, x, y, width, height, rop; } SVGAFifoCmdFrontRopFill;
typedef struct { uint32 srcX, srcY, destX, destY, width, height; } SVGAFifoCmdRectCopy;
typedef struct { uint32 x, y, width, height; } SVGAFifoCmdUpdate;
```

我按前者的习惯给后三者各塞了一个长度字（`24` / `24` / `12`），于是宿主把它们解析成：

```
发出的:  [3][24][100][100][600][400][200][200]
宿主读到: RECT_COPY{srcX=24, srcY=100, destX=100, destY=600, w=400, h=200}
```

也就是一条**完全合法的、被正常执行了的**拷贝，只是矩形不是我要的那个。而
`FRONT_ROP_FILL` 因为 `x` 收到 `0xFFFF00FF` 被裁掉，看起来像"无效果"。

**证据在截图上就能量出来**（`docs/shots/2026-11-14-2d-ctl-b.png`，§53 那张"判决"图）。
逐行找颜色边界：

```
y = 590 之前:  白|黄 在 x=128, 黄|青 在 256, 青|绿 在 384   （正常色带）
y = 610 之后:  白|黄 在 x=204, 黄|青 在 332, 青|绿 在 460   （+76）
而 x ≥ 512 的边界不动
```

+76 = `destX(100) - srcX(24)`，只影响 `destX .. destX+400 = 100..500`。这与
`RECT_COPY{24,100,100,600,400,200}` 的预测**逐像素吻合**，而我当时把它当成
"scratch 步距错位"解释过去了 —— 这是本轮最该早一步抓住的地方：
**截图里已经写着答案，我却用了另一个更复杂的假设。**

### 54.2 正确的线格式下重测（legacy 寄存器模式，纯数值）

`kProbe2D = 2`：不定义 screen object、不发 blit，直接在 FB 上测，判据全部来自 VRAM 回读：

```
2d2: COPY 100x100 (160,100)->(400,400) -> acked | src 0xffffff00, dst 0xff00ff00 -> 0xffffff00
2d2: FILL rop 0xcc 100x100@(600,500) -> acked | 0xffff00ff -> 0xffff00ff
2d2: FILL rop 0xf0 100x100@(720,500) -> acked | 0xffff0000 -> 0xffff0000
2d2: FILL rop 0x00 100x100@(840,500) -> acked | 0xff0000ff -> 0xff0000ff
2d2: FILL rop 0xff 100x100@(600,620) -> acked | 0xffff00ff -> 0xffff00ff
2d2: VERDICT copy=EXECUTED fill=NOT EXECUTED (4 rop encodings, no change)
```

| 能力 | 结论 | 判据 |
|---|---|---|
| `RECT_COPY`(3) | ✅ **执行** | 目标像素 `0xff00ff00 → 0xffffff00`，与源像素**逐位相同**；目标原值不同，排除"本来就是这个色" |
| `FRONT_ROP_FILL`(29) | ❌ 不写内存 | 四种 rop（`0xCC` SRCCOPY / `0xF0` PATCOPY / `0x00` BLACKNESS / `0xFF` WHITENESS）四个不同矩形，前后全部一致 |
| `UPDATE`(1) | acked | 五字格式 `[1][x][y][w][h]`，无长度字 |

顺带修掉一个判据 bug：上一次我把"拷贝成功"写成 `dst == 0xFFFF00FF`（我**以为**
fill 会画出的洋红），而实际源像素是黄色 —— 日志里那行 `0xff00ff00 -> 0xffffff00`
明明已经证明拷贝成功，结论却写成 NOT EXECUTED。**期望值必须从被测内存里读出来，
不能从"我以为"里读。**

### 54.3 更重要的意外：显示**根本不需要** screen object

`kProbe2D = 2` 这一轮在 `DEFINE_SCREEN` 之前就 `return` 了，也就是说：

* 没有 `DEFINE_SCREEN`、没有 `DEFINE_GMRFB`、没有 `BLIT_GMRFB_TO_SCREEN`；
* 没有 present 定时器，一个 tick 都没有；

而屏幕上是**完整的 macOS 登录界面**（壁纸、时钟、用户名、密码框、鼠标指针），
截图见 `docs/shots/2026-11-14-legacy-noscreen.png`。

所以 §47 的因果归因是错的：

> 显示能工作，靠的是宿主**直接扫描输出 BAR1 上 `FB_OFFSET` 处、按 `BYTES_PER_LINE`
> 排布的那块内存**（寄存器模式）。screen object 与全屏 blit 从来不是显示的前提。

这解释了之前几件一直没归位的事实：为什么"色带"能出现在屏幕上（它们就在被扫描的
内存里）、为什么 WindowServer 一画就把它们盖掉（同一个缓冲区）、以及为什么 §47 的
A/B 里那些"没画面"的配置其实是被别的开关（模式设置/ENABLE）弄坏的。

### 54.4 出货配置随之简化

既然 blit 与 screen object 都不是显示的前提，它们只剩成本：每 33 ms 一次全屏 FIFO
提交，把客人内存重新推一遍。新增开关 `kSkipScreenObject = 1` 后，驱动退化成一台
**纯 framebuffer**：

```
probe() 里做模式设置  ->  SVGA_REG_ENABLE 去 HIDE  ->  客人 WindowServer 直接画进 VRAM
                                                      ->  宿主自己扫描输出
```

不再发 `DEFINE_SCREEN`/`DEFINE_GMRFB`/`BLIT_GMRFB_TO_SCREEN`，不再有 present 定时器
（心跳仍在，它本来就是独立的 thread_call）。验证：重启后登录界面出现并**持续存在**。

### 54.5 对 Level-2 的重新评估

§53.4 说"2D 引擎只提供传输、没有绘制"，现在要改成分级结论：

| 需要的原语 | 状态 |
|---|---|
| blit GMRFB→屏幕 | 可用（其实连它都不必要，宿主自己扫） |
| **屏幕内矩形复制 `RECT_COPY`** | ✅ **可用**（数值判据） |
| 纯色填充 `FRONT_ROP_FILL` | ❌ 四种 rop 都无效 |
| 3D 三角形 | ❌ 派发器缺席（§51） |
| 屏幕回读 | ❌ 只写（§53.2） |

也就是说下层**有**一个真能跑的拷贝引擎。它够做滚动、够做窗口搬移、够做
"memcpy 一次 + 一次 RECT_COPY" 的合成，但**不够做 Metal**：Metal 的核心是三角形
光栅化与着色器，那仍然只能来自 §51 判定缺席的 3D 派发器。

所以 Level-2 的准确表述变成：**能做出一个 2D 加速的显示驱动（copy 可用），
做不出 Metal/IOAccelerator。** 前者是这次可以交付的边界。

## §55 Metal / OpenGL 的真实门槛（2026-11-15）

用户目标改成"直到 Metal/OpenGL 可用"，并给了两个参考仓库。这一节是**测量结果**，
不是判断：先问死"缺什么"，再说"能不能补"。

### 55.1 现状：有显示、没有加速器

```
system_profiler SPDisplaysDataType:
    VMware SVGA 3D GPU   Device ID 0x0405   Vendor 0x15ad   VRAM 3 MB
ioreg -rc IOAccelerator -> 空
kextstat | grep IOGraphics -> 只有 com.apple.iokit.IOGraphicsFamily
```

显示器是我们驱动的 framebuffer；**系统里没有任何 IOAccelerator 实例**。

### 55.2 Metal：0 个设备

`kexts/probes/metal_probe.m`（`MTLCopyAllDevices` + `MTLCreateSystemDefaultDevice`）：

```
MTLCopyAllDevices count = 0
MTLCreateSystemDefaultDevice = nil  -> Metal has NO device
```

原因不是"没注册"，而是系统里现成的 Metal 驱动**没有一个匹配我们的 PCI ID**：

| 驱动 | 匹配条件 | 能用吗 |
|---|---|---|
| `AppleParavirtGPUIOGPUFamily` | `IONameMatch = paravirtualizedgraphics,gpu`，`IOProviderClass = AppleARMIODevice` | ❌ 只在 Apple Silicon + Virtualization.framework 下存在 |
| `AGXG13/14/15/16*.kext` | Apple Silicon 内置 GPU | ❌ |
| `AppleIntel*Graphics.kext` | 具体 Intel iGPU 的 PCI ID | ❌ 我们是 0x15ad:0x0405 |

### 55.3 OpenGL：只有软件渲染器，但它是真的

`kexts/probes/gl_probe.c`（CGL + FBO 画一个三角形再读回像素）：

```
software: RENDERER=Apple Software Renderer
software: VENDOR=Apple Inc. VERSION=2.1 APPLE-21.1.1 GLSL=1.20
software: triangle centre pixel = 255,0,0,255 -> OPENGL RASTERISES
default:  CGLChoosePixelFormat failed (invalid CoreGraphics connection)
accelerated: 同上
```

* 软件渲染器**能出像素**（不是只返回一个字符串）；
* 上限是 **GL 2.1 / GLSL 1.20**（Apple Software Renderer 的固有限制，没有 3.2 core）；
* 后两行失败是**会话问题不是能力问题**：SSH 里没有 WindowServer 连接（CGS），
  要判"加速路径能不能建上下文"必须在已登录的 Aqua 会话里跑。

### 55.4 为什么你给的两个参考都补不上这个缺口

**MoltenVK** 是 Vulkan → **Metal**。它要求先有一个可用的 Metal 设备；§55.2 是 0 个。
它是 Metal 的下游，不能替代 Metal。而 Vulkan 自己若要在本机落地，还得有一份针对
SVGA 的 Vulkan 驱动，底层又回到 §51 判定缺席的 3D 派发器。

**nullmoth/nvidia-macos-driver**（读它的 HOW-IT-WORKS 与安装要求）：

```
Metal app -> NVMTLDriver.bundle -> (Apple AIR -> SPIR-V) -> NVK(Mesa Vulkan) -> NVRM/NVAccel kexts -> NVIDIA GPU
```

它是**真的 Metal 驱动**，但四个前提我们一个都不满足：

1. 硬件必须是 NVIDIA Turing 及以后（GTX16/RTX20~50）；我们的"GPU"是 VMware SVGA；
2. 内核侧复用的是 **NVIDIA 自己的开源 GPU 内核模块 r610** —— 没有 NVIDIA 硬件就没有可用内核侧；
3. 需要 OpenCore + `csr-active-config 0x0A43` + `amfi=0x80` 等引导层改动（我们是 VMware 引导）；
4. 它的 `NVAccel` 里是与 NVIDIA 显存/GPUVA ABI 强绑定的实现（`nvrm_vram_abi.h`）。

它对我们的价值是**架构参考**：一个 Metal 驱动 = ①发布 `MetalPluginName` 的
IOAccelerator kext + ②实现 Metal 驱动 ABI 的用户态 bundle + ③AIR→后端 IR 的翻译器
+ ④能真正执行着色器的内核后端。第 ④ 项在本宿主不存在。

至于"改成支持 DX 的驱动"：DX 命令最终也要落到同一套执行后端上（§51：3D 派发器缺席），
所以它不是一条替代路径，而是同一个缺口的另一种说法。

### 55.5 一次 panic，以及它说明的事

在已登录桌面里启动一个 GL 窗口程序（`kexts/probes/gl_window.m`）时客体 panic
（`Kernel-2026-11-15-010713.panic`，01:07）：

```
Panic(CPU 0): NMIPI for spinlock acquisition timeout
Ticket spinlock timeout after 54067273133 ticks ... current owner: kernel_task (on CPU 0)
Panicked task: pid 188 contextstored
```

与 §50 那次 `0x3040` 页错误**不是同一个签名**（11-13 两次都是
`Kernel trap ... type = 14=page fault`）。这次是**CPU 0 持锁 54 秒不释放**，
落在内核地址（`0xffffff800e...`），不在我们的 kext 里。

归因还没做，如实记下：可能是宿主侧 mks 停顿导致我们的同步 FIFO/RPC 调用长时间不返回，
也可能是纯负载（当时 load average 11）。**在继续加功能前，这条稳定性问题排在前面。**

### 55.6 结论

| 目标 | 状态 | 依据 |
|---|---|---|
| 显示（framebuffer） | ✅ | §54.4 登录界面 + 桌面 |
| OpenGL（软件） | ✅ 可用，上限 GL 2.1 | §55.3 |
| OpenGL（硬件加速） | ❌ 需要 GL 驱动 + IOAccelerator | §55.2 |
| Metal | ❌ 0 个设备，且无匹配驱动 | §55.2 |
| Metal via 自研驱动 | ❌ 需要 ④ 执行后端，本宿主没有 | §51 + §55.4 |

**一句话**：这台虚拟机可以提供"能显示、能软件渲染 GL 2.1"的图形栈；Metal 缺的不是
配置项，而是一台有 3D 执行后端的设备。

---

## §56 撤回 §51：线格式错了，而且"超时"根本不是判据（2026-11-15）

§51 的结论是「这台宿主从不派发 3D 渲染命令」。**该结论作废。** 它是三个方法论错误叠出来的，
而且这一次错误不在推理里，在**字节**里。

### 56.1 SVGA3dCmdHeader.size 是 body 字节数 —— 四条命令三条写错

`svga3d_cmd.h:333` 的定义没有任何歧义：

```c
typedef struct { uint32 id; uint32 size; } SVGA3dCmdHeader;   // size = body 字节数
```

宿主的解析器**不拿 size 和结构体比对**，它只是按 `size` 跳过。所以 size 写错不会让这条命令
报错，只会让**下一条命令从错误的偏移开始解析**。§51 那四条：

| 命令 | 结构体 body | §51 写的 size | 结果 |
|---|---|---|---|
| `DX_SET_TOPOLOGY` 1160 | `{topology}` = 4 | 8 | 多吃一个字 |
| `DX_SET_RASTERIZER_STATE` 1164 | `{rasterizerId}` = 4 | 8 | 多吃一个字 |
| `DX_SET_DEPTHSTENCIL_STATE` 1163 | `{id, stencilRef}` = 8 | 12 | 多吃一个字 |
| `SVGA_3D_CMD_CLEAR` 1057 | `{cid, flag, color, depth, stencil}` = 20 | 20 ✅ | 但只交了 16 字节 |

最后一条最能说明问题：它声明 20 字节、实际只提供 4 个字，宿主读 stencil 时读到了数组之外。
这正是"每条都 STATUS 3 / errorOffset 0"的来源 —— 第一条的 size 就把后面全部错位，而报告
偏移时宿主只认它自己算出来的那条边界。

**修法不是改数字，是让数字不可能写错**：新增 `cbCmd(p, id, bodyWords)`，size 由 body 字数
推出，调用方只负责 body。同时把 payload 直接从 CB 头的 dump 里核对：
`00000421 00000014 00000001 00000001 ff00ff00 3f800000 00000000`（1057 / 20 / cid 1 /
CLEAR_COLOR / 绿 / depth 1.0 / stencil 0），逐字节对得上。

> 这是本项目第三次同类错误（§53 的 `RECT_COPY` / `FRONT_ROP_FILL` / `UPDATE` 多塞了长度字）。
> 教训重复出现说明它不是疏忽，是**缺一个不让人手写长度的构造器**。

### 56.2 修正后结果变了：从"逐条拒绝"变成"根本没应答"

同一个测试、同一个二进制结构，只改线格式，CB 头的表现完全不同：

| | 修正前（§51） | 修正后（§56） |
|---|---|---|
| 4 条 DX 命令一批 | `STATUS 3`, errorOffset 0 | header status **保持 0**，从不写 |
| `SET_SHADER_IFACE` 单条 | `STATUS 3` | header status 保持 0 |

"status 一直是 0"不是"拒绝"，是"我们没等到"。而当时判定它是拒绝的代码，把 `status != 1`
一律叫 refused —— 一个连 `SVGA_CB_STATUS_NONE` 都没区分的判据。

### 56.3 轮询是 200000000 次自旋，不是时间预算

原来的等待循环是 `for (spin < 200000000)`，没有 delay，实测约 **130 ms**。两个后果：

1. 预算是**循环次数**，改一行循环体它就变了；
2. 130 ms 对宿主可能推迟处理的批次远远不够，于是"没等到"被读成"被拒绝"。

改成墙面时钟 3 s 预算 + 超时后每 1 s 复查一次、共 2 s（`kCbWaitMs` / `kCbLatePollMs`）。
前 2 ms 仍然热自旋（正常路径是微秒级，不该为它付 `IOSleep`）。

复查机制是有意义的，因为它区分"慢"和"没有"：§56d 里 SURVEY 与 SET_SHADER_IFACE 都是
**3 s + 2 s 之后 status 仍是 0**，从此"不是慢"这句话有依据了。

### 56.4 单一头页会凭空制造"完成"

所有 CB 共用一个 64 字节头。一旦某批可以不答，就出现这个序列：超时 → 放弃 → 下一批写进同一
64 字节 → 宿主这时才把上一批的 COMPLETED 写进去 → 我们把它读成"这一批完成了"。
每次提交前清零**不能**区分这两者。

改成**一页 64 个槽轮转**，每次提交先领自己的槽，答案就不会张冠李戴。这条不是猜测：调查过程中
真的出现过"新加的可达代码没打印、而同函数其他行都打印"的现象，让我先怀疑了二进制，
顺带把这类"状态归属"隐患一起修掉。

### 56.5 构建标签：那次启动跑的确实不是我们以为的二进制

现象：源码里可达、`strings` 在 `/Library/Extensions` 的二进制里能查到 2 处 `DXBRANCH`、
同函数其它 IOLog 全都打印了，**只有新块没有输出**。

原因是 aux KC：`install.sh` 只能把请求**排队**（`kmutil load` 返回 `Code=28 requires a reboot`，
这是正常路径），集合在下一次启动时由 kernelmanagerd 从 `/Library/Extensions` 重建 —— 但
没有任何保证说"这次启动用的集合里有这个文件"。加一条构建标签横幅：

```
vgpu-fb: start, provider=IOPCIDevice [build §57b]
```

并且 `build.sh` 无条件校验它（不放进 `$MARKERS`：2d2 / plain-framebuffer 那几条分支会**整体
覆盖** `$MARKERS`，共享项在正好要紧的配置里会被悄悄丢掉）。加完之后 §56c 那条启动自报
`[build §56c]`，此前那次启动没有标签可比对 —— 这个盲区从此关掉。

### 56.6 撤回"DX 批次冻住扫描输出"

第一次跑修正版时，屏幕冻在启动进度 35%，两次截图**逐字节相同**，而客体的 RPC 心跳还在打，
所以我当时归因给"DX 批次把宿主 svga 线程卡住了"。

**不成立。** 后面两次启动（§56c / §56d）跑的是同一段 DX 测试，桌面/登录界面全程正常刷新
（两张相隔 20 s 的截图 MD5 不同，1.8 MB 的完整桌面）。

更可能的解释是**我们自己的自旋**：旧代码在 thread_call 里连跑三次 200M 次不带 delay 的循环，
而那一刻正好是 WindowServer 起来的当口。换成 `IOSleep` 之后就没再出现 —— 这只是相关性，
但比"宿主卡死"更符合已知事实，且**可以解释为什么心跳还在**。

### 56.7 本轮更正表

| §51 的说法 | 现在的状态 | 依据 |
|---|---|---|
| "宿主从不派发 3D 渲染命令" | **作废** | §57.2：`SET_TOPOLOGY` 单条 COMPLETED |
| "四条 DX 命令都被拒在 offset 0" | **作废** | §56.1 线格式错位 |
| "legacy CLEAR 也被拒 ⇒ 传输层不行" | **作废** | 这个对照本身无效：legacy 3D 走 FIFO，没有驱动用 CB 发它 |
| "DX manager 仍然缺席" | **待定** | 结论来自错位批次，需在正确批次上重测 |

---

## §57 DX 派发存在；断点定位到"第一个解析 COTable 的命令"（2026-11-15）

### 57.1 空批次完成 ⇒ 分支与 dxContext 查找都没问题

只带 CB 头、零字节载荷的 DX 批次（`dxContext=1`、DX 位已置）**立刻 COMPLETED (status 1)**，
紧随其后的普通 sync 也 COMPLETED。所以：

- 头的 `DX_CONTEXT` 位被接受；
- `dxContext != 0` 的 lookup-or-create 路径不卡；
- "分支本身卡死"这个假设被自己的实验否掉。

### 57.2 单条 `SET_TOPOLOGY` 完成 ⇒ §51 的核心结论作废

`DX_SET_TOPOLOGY (1160)` 单条、独立 CB、干净队列：**COMPLETED (status 1, errorOffset 0)**。

这台宿主**会**派发 DX 命令。§51 那张"设备没有 3D"的结论表，是错位批次 + 130 ms 预算 + 单一
头页三件事合起来的产物。

### 57.3 任何 STATUS 3 之后，队列在 5 s 内彻底不再应答

这是决定实验顺序的硬约束，§56e 量得很清楚：

```
MATRIX a) DEFINE_CONTEXT via DX branch -> NO ANSWER (status 3 errorOffset 0)
MATRIX a) device after it -> DEAD          <- 紧跟的普通 sync，status 停在 0 满 5 s
```

所以**探针本身会花掉这次启动唯一的一条干净队列**。而修正前的代码把 BISECT / SURVEY /
DXBRANCH / MATRIX 全排在真正要测的 CLEAR+READBACK **之前** —— 那些"解释性"实验一直在消耗
它们本该解释的那个测量。修法：目标测量提到最前（`dx: CLEAR alone`），探索批次压到
`kDxExplore` 后面，默认关。

### 57.4 阶梯：断在第 3 级

`CLEAR_RTV` 单条在干净队列上得到 `STATUS 3 / errorOffset 0`。逐级上爬，每条独立 CB，出错即停：

| 级 | 命令 | 结果 |
|---|---|---|
| 1 | `SET_RASTERIZER_STATE` {INVALID_ID} | **COMPLETED** |
| 2 | `SET_DEPTHSTENCIL_STATE` {INVALID_ID, 0} | **COMPLETED** |
| 3 | `SET_RENDER_TARGETS` {dsv=INVALID, rtv 1} | **STATUS 3 / offset 0** |
| 4 | `CLEAR_RTV` | 未尝试（第 3 级失败后队列已不可用，按设计停） |

第 1、2 级证明：状态类命令通过，**即便它要解析一个 COTable 里的 id**（INVALID_ID 也是 id）。
第 3 级是第一个真正去**取一行有效视图数据**的命令，它失败。

### 57.5 已经排除掉的候选

| 候选 | 为什么不是它 |
|---|---|
| 命令号写错 | 1160/1163/1164/1176/1207 逐个对过 `svga3d_cmd.h` |
| body size 写错 | §56.1 已改由 `cbCmd()` 推出，payload dump 逐字核对 |
| COTable `type` 用错 | `SVGA_COTABLE_RTVIEW = 0`（`svga3d_types.h:1493`），我们发的就是 0 |
| `validSizeInBytes` 太小 | 发的是 4096，等于整页 MOB |
| 表面没有 render-target 用途 | `sflags = SURFACE_HINT_RENDERTARGET \| SURFACE_BIND_RENDER_TARGET`（`1<<24`） |
| 缺 DX 能力 | `DEVCAP DXCONTEXT=1 SM41=1 GL43=1`；宿主日志 `SVGA3dCaps: host, at power on (3d enabled)` |

### 57.6 下一个候选（未测）

按可能性排序，每条都只要一次启动：

1. **视图行对宿主不可见**：绑定的 COTable MOB 与我们写入条目的页是不是同一页。
   `_cotableMem` 的 PPN 注册成 MOB、`SET_COTABLE` 绑 `kCotableMobId`，路径上有一处
   `ctEntryOff = kCotableMobId * kGbMobEntryBytes` 的间接，PT 深度写错会让宿主把 PPN 表
   当表内容读 —— 现象正是"表是空的"。
2. **需要先 `DX_GROW_COTABLE` (1237)**：vmwgfx 是先 grow 再填行，我们直接写行。
3. **`SET_RENDER_TARGETS` 的数组约定**：头文件里 `SVGA3dCmdDXSetRenderTargets` 只有
   `depthStencilViewId` 一个字段加尾随数组，但 `SET_VIEWPORTS` / `SET_SCISSORRECTS` 都有
   `uint32 pad0`。若这里也隐含 pad，`rtv 1` 会被读成一个越界 id。

### 57.7 这一轮的状态

| 项 | 状态 | 依据 |
|---|---|---|
| 显示（纯 framebuffer） | ✅ | 登录界面 / 桌面多次确认 |
| DX 命令派发 | ✅ **存在** | §57.2 |
| DX 状态类命令 | ✅ | §57.4 第 1、2 级 |
| DX 视图绑定（渲染前置） | ❌ 断在第 3 级 | §57.4 |
| CLEAR + READBACK（出像素） | ⏸ 未测（队列前提未满足） | §57.4 |
| Metal / 硬件加速 GL | ❌ 结论不变 | §55 |

**一句话**：阻塞点从"宿主不支持 3D"缩小到"一条视图绑定命令被拒"，而前者是死路、后者是
可以继续拆的工程问题。

### §58 GROW_COTABLE 被拒：这台宿主是 DX10 代设备

§57.6 的第 2 号候选当场测掉了。vmwgfx 在填充视图行之前会先发
`SVGA_3D_CMD_DX_GROW_COTABLE {cid, mobid, type, validSizeInBytes}`（1237），
我们直接跳到 `SET_COTABLE`；如果宿主只认自己维护的表长度，那么 `SET_COTABLE` 会
"接受但无效"（它不校验），表内任何 id 查找仍然失败 —— 这正好是阶梯那个形状。

实测：

```
dx: SPLIT DX_BIND_CONTEXT {cid 1 mob 6} -> ACCEPTED
cb: STATUS 3 (errorOffset 0)
dx: SPLIT DX_GROW_COTABLE {cid 1 type 0 size 4096} -> refused
dx: SPLIT DX_SET_COTABLE {cid 1 mob 5 type 0} -> refused      <- 被上一条的 STATUS 3 带走
```

`GROW_COTABLE` 在 vGPU10.1 才加入，而这台宿主自己的钳位行就写着
`Renderer Provides BaseCapsLevel: 9 (9,9) / ClampLevel: 7 (7,10)`。
**结论：本设备的 DX 能力停在 10.0，没有表增长机制，`SET_COTABLE` 一次绑定就是全部。**

已收回 `kDxGrowCotable = 0`（保留代码与横幅，因为"测过并被拒"和"从没测过"是两件事）。
关闭后 §58b 完整复现 §57b：LADDER 1/2 COMPLETED，LADDER 3 `SET_RENDER_TARGETS` STATUS 3 ——
**这条断点可重复**。

于是 §57.6 的候选缩减为两条，下次直接从这里开始：

1. **视图行对宿主不可见**（`_cotableMem` 的页 / MOB 注册 / PT 深度那条间接）；
2. **`SET_RENDER_TARGETS` 的数组约定**（尾随数组 vs 固定 8 项；`SET_VIEWPORTS` /
   `SET_SCISSORRECTS` 都有 `uint32 pad0`，这里可能也隐含一个）。

两条都不需要重开死路，都只要一次启动。

---

## §60（部分撤回）：`[0x1414930A0]` 是当前 DX 上下文，但门控可达性待运行态判定（2026-11-15）

> **撤回提示：§60 下文保留当时的推理以供审计，不是当前结论。§61 的运行态与新实验已否决“门恒零”“上下文从未出现”“COMPLETED 是空转”等断言。以 §61 为准。**

§57.2 曾据 `SET_TOPOLOGY → COMPLETED` 判断派发存在；§60 试图用静态扫描与 NULL 快照反驳。
后续验证发现该反驳忽略了间接初始化及批次收尾清零，不能成立。

### 60.1 那个全局到底是什么

宿主侧 `vmxmem.py` 的实测（用户提供）是 `[0x1414930A0] = 0`，而所有门控字段健康。当时
的解释是「DX 管理器从未被创建」。把写入点反汇编出来之后，那个解释不成立：

```
0x14050a04e  test r8b, 2                ; r8b = CB header 的 dword @ +0x10 = flags
0x14050a052  je   0x14050a20d           ; 没有 DX_CONTEXT 位 -> 走普通命令流
0x14050a058  cmp  dword ptr [0x1414937FC], 0
0x14050a05f  je   0x14050a222           ; 门为 0 -> ebx = 4，整批按 STATUS 4 结束
0x14050a06e  movd ecx, xmm0             ; ecx = CB header 的 dword @ +0x24 = dxContext
0x14050a072  call 0x140a17350           ; find_or_create(dxContext)
0x14050a077  mov  [0x1414930A0], rax    ; <-- 全二进制唯一一处真写入
0x14050a07e  test rax, rax
0x14050a081  je   0x14050a222
```

`0x140a17350` 内部是**哈希探测**，不是构造：

```
mov  r9d, [0x141492ED0]     ; 桶数量
shr  edx, 0x10
imul r8d, edx, 0x83         ; hash 高位
lea  eax, [rax + rax*4]     ; hash 低位 * 5
add  r8d, eax
and  r8d, r9d - 1           ; & (count-1)
lea  rcx, [0x1414930B0]     ; 桶数组
call 0x140a0df40            ; 探测
test rax, rax ; jne ret
jmp  0x140a15b90            ; 未命中 -> 插入
```

所以 `[0x1414930A0]` 存的是「**当前 DX 上下文对象**」，键是 `cbHeader.dxContext`，
**只有一条带 `SVGA_CB_FLAG_DX_CONTEXT` 的 command buffer 被提交时才会被赋值**。
`DX_DEFINE_CONTEXT` 不碰它 —— 这就解释了为什么那次启动里 DEFINE_CONTEXT 跑过、
它仍然是 0。

字段偏移也对得上我们自己的 `cbSubmitCtx`：`+16 = flags`、`+36 = dxContext`、
`+20 = length`、`+24 = ptr.pa`。

### 60.2 那道 header 校验的现在时：`FC` 是我们自己补丁的产物

同一个函数的入口校验在 `0x140087c90`：

```
test dword ptr [rbp - 0x59], 0xfffffffc    ; [rbp-0x59] = header + 0x10 = flags
jne  <reject>
```

盘上 `vmware-vmx.exe` 的文件偏移 **0x87293** 正是这条指令 imm32 的首字节：

```
vmware-vmx.exe               0x87290: f7 45 a7 fc ff ff ff   ; mask 0xFFFFFFFC
vmware-vmx.exe.bak-dxflag    0x87290: f7 45 a7 fe ff ff ff   ; mask 0xFFFFFFFE
```

所以 **§15 的描述对原版是准确的**（原版只放行 bit0，bit1 一律 CB_HEADER_ERROR），
是 §19 的 1 字节补丁 `FE→FC` 把 bit1 放开的，而且补丁**现在仍在位**。
这一条不是撤回，而是澄清：补丁有效，`flags = 3` 现在能过 header 校验，所以 STATUS 4
不来自这里。

现在的开关组合也是对的：`kCbKeepDxContext = 1`、`kCbMobMode = 0`、
`kCbFlagDxContext = 2` → 实发 `flags = 3`（NO_IRQ | DX_CONTEXT）。
注意 PA 形式下 **不能再带 bit2**（`SVGA_CB_FLAG_MOB`），`0xFFFFFFFC` 会拒 ——
这大概就是当年「flags=5/7 被拒」的真实原因。

### 60.3 三条路全断

| 发出什么 | 走到哪 | 结果 |
|---|---|---|
| `flags` 带 bit1（我们的现状，3） | `0x14050a058` 的门 | 门恒为 0 → `ebx=4` → **STATUS 4** |
| `flags` 不带 bit1、`dxContext != 0` | `0x14050a20d` 读 +0x24，非 0 | `ebx=4` → **STATUS 4** |
| `flags` 不带 bit1、`dxContext == 0` | 普通命令流，逐条进 handler | handler 里 `mov rbx,[0x1414930A0]; test; je bail` → `eax=0x101` |

第三条解释了 §57.2 / §57.4 的全部现象：`[0x1414930A0]` 只可能被第一条路赋值，而第一条
路死在门上，于是它**永远是 NULL**，于是每个 handler 一进门就退。`SET_TOPOLOGY`、
`SET_RASTERIZER_STATE` 那种「通过」，是 handler 只把 id 记进状态块、不做任何设备工作
的分支 —— 它们不需要上下文，所以成功；`SET_RENDER_TARGETS` 要把 rtv id 解析成表面，
需要上下文，所以 STATUS 3。**这不是「命令不支持」，是「没有上下文」。**

### 60.4 关键：那道门没有任何指令写它

对 `0x1414937FC` 做全 `.text` 引用扫描（`tools/rev/vmx-refs2.py`，重同步版）：

```
--- 49 refs into [0x1414937fc, 0x141493800)
   42 x  cmp dword ptr [rip + ...], 0     ; 读
    7 x  mov eax, dword ptr [rip + ...]   ; 读
    0 x  任何形式的写
```

镜像里这个 VA 落在零初始化区（virtual size > raw size），也就是**加载时 0，之后没人改**。
`cmp X, 0 ; je 拒绝` 意味着它必须非零才能放行 → **DX 分支在这个 vmx 构建里是死代码**。

把窗口放大到 `[0x1414937F0, 0x141493810)` 也没有候选：这 52 条引用里唯一的写是
`mov byte ptr [0x1414937F4], bl`（`0x1404c0bf2`），落在门前 8 字节，与我们这个 dword
无关。也就是说这一小片是一组独立的小标志，**别的标志有人在写，这个没有**。

这条结论有一个必须说明的漏洞：我的扫描只看得见「指令里直接出现该地址」的写。
如果这个变量长在一个大结构体里，代码通过 `lea reg, [结构体基址]` + `mov [reg+off]`
或者整块 `memcpy` 写它，扫描就看不见。所以它是**强证据，不是判决**。

判决只需要一次读数：**在客体重启后读 `[0x1414937FC]`**。

- 读到 0 → 门从未打开，DX 在本宿主上不可达，DX 线应整体关闭；
- 读到非 0 → 门是开的，那么还需分别核对 `find_or_create` 结果及后续校验，不能直接指定根因。

**2026-11-15 实测已推翻本节核心推断**：用户的实时快照为 `dxGate=1`，所以「门恒为 0」不成立；
而 `dxCtx=0` 是单一时刻的取样，不能证明历史上从未走过 DX_CONTEXT 分支。客体启动日志还证明
§58b 的 DX 测试中带 `flags=3, dxContext=1` 的 CB 确实获得过 COMPLETED / STATUS 3，进一步排除
「所有带 DX 标志的 CB 都卡在这道门」这一说法。见 §61。

`tools/vmxmem.py` 输出字段应按采样值理解：`dxCtx` / `dxGate` 均非历史记录；地址 `0x1414930B0`
是内嵌哈希桶区域的起始位置，不是桶数组指针，首个槽为 0 也不能说明整表未初始化。

### 60.5 状态表（覆盖 §57.7）

| 项 | §57.7 的说法 | 现在 | 依据 |
|---|---|---|---|
| DX 命令派发 | ⏸ 部分验证 | 带 `flags=3, dxContext=1` 的若干批次获得应答，但具体命令副作用尚未完整证明 | §61 启动日志 |
| DX 状态类命令 | ✅ 获得 COMPLETED | COMPLETED 仅说明批次被应答；不能推断其内部副作用 | §57 / §61 |
| DX 视图绑定 | ❌ 有效 RTV 绑定失败 | `rtv=INVALID` 的解绑成功，`rtv=1` 失败 | §61.2 |
| `GROW_COTABLE` 被拒 | 结果已测，原因未定 | DX 版本能力、上下文和命令前置条件仍需区分 | §58 / §61 |
| 宿主 DX 分支可达 | ✅ 运行态采到 gate=1 | 单次值；不说明所有提交时刻状态 | 用户提供的 §61 快照 |

### 60.6 这轮得到的方法论

- **「某条命令 COMPLETED」不等于副作用已证实**；反过来，单次读取一个工作态全局为 NULL 也不能证明此前从未派发。应查处理器的完整生命周期，并将“收到应答”和“完成目标操作”分开记录。
- **静态引用扫描只能约束直接寻址写入**；用户实测 `dxGate=1` 已推翻「无直接写入即可判恒为 0」。对运行态全局的结论必须带上访问范围与采样时刻。
- **内嵌表不应以首地址的 8 字节冒充指针**；`0x1414930B0` 是桶区起点，单槽读值不能推出哈希表状态。

---

## §61 用户快照纠正 §60；§59 确认失败只出现在有效 RTV 绑定路径（2026-11-15）

### 61.1 运行态快照撤回了什么

用户在完整断电重启后（宿主 PID 38384）于客体启动约 20 秒取样：

```
dxGate = 1
 dxCtx = 0
 dxBuckets = 0
 dxHashCnt = 0x100
```

这足以**撤回 §60.4「gate 恒为 0」、§60.3「带 DX 标志的 CB 必然 STATUS 4」以及“桶数组指针为 NULL”**。
但它仍只是一个时刻的快照：`dxCtx=0` 不能作为历史上从未进入 DX_CONTEXT 分支的证据；`0x1414930B0`
是内嵌桶区的起始地址，不是指针，读其首 8 字节为 0 不代表哈希表没有已初始化的其他桶。

随后同次启动的客体日志自报实际二进制为 `[build §58b]`。20:18:49 的测试发出 `flags=3, dxContext=1`
的 CB，分别观察到 `COMPLETED` 与 `STATUS 3`；因此当时 gate=1 与真实 DX 标志流能够获得宿主应答并不矛盾。

### 61.2 §59 对照实验

为区分 `SET_RENDER_TARGETS` 命令/解绑能力与有效 RTV 绑定路径，新启动加载 `[build §59]` 后运行：

```
LADDER 1 SET_RASTERIZER_STATE {INVALID}      -> COMPLETED
LADDER 2 SET_DEPTHSTENCIL_STATE {INVALID}    -> COMPLETED
LADDER 3a SET_RENDER_TARGETS {rtv INVALID}   -> COMPLETED (status 1, errorOffset 0)
LADDER 3b SET_RENDER_TARGETS {rtv 1}         -> STATUS 3 (errorOffset 0)
```

该结果说明命令能被接受，清空渲染目标也能被接受；**加入有效 RTV 1 后才出现失败**。它将下一步缩小到有效视图解析/绑定相关路径，但不能单凭这组数据断言 COTable 行不可见：还需排查 RTV 行内容、其引用 surface 的描述与生命周期、格式兼容性，以及宿主在有效视图上的额外前置条件。STATUS 3 后队列可能毒化，本轮没有用后续 CB 的状态作进一步判据。

`§59` 已通过客体 build.sh 的编译、kmod、无弱引用和 marker 检查，`install.sh` 报 `Code=28 requires a reboot`（预期），重启后启动日志确认 `[build §59]`，故测试版本可信。

### 61.3 `dxCtx` 的清理生命周期（已核实）

CB 处理器在 `0x14050a458` / `0x14050a4a0` 调用 `0x1404cc000`，后者执行：

```
0x1404cc009 xorps  xmm0, xmm0
0x1404cc013 movdqu [0x1414930A0], xmm0
```

这会把 `dxCtx` 及相邻 8 字节一起清零。因此正常批次收尾后读到 NULL 完全符合实现，
50 ms 周期采样也可能漏掉不足 1 ms 的批次。本轮全段重扫描另见 `0x140a11910`、
`0x140a140a4`、`0x140a15821` 的标量清零；§60 的“唯一真写入”说法同样撤回。
真实命令分派指令在 `0x14050a375`，按命令 ID 索引运行态表 `0x14141D000 + id*8`；
`SET_RENDER_TARGETS` 对应槽位 `0x14141F448`，该表位于非文件支撑区，不能拿此前的 236 项 RVA 表代替。

### 61.4 真实命令映射及 RTV 缓存 miss 路径

已从初始化代码而非尺寸猜测确认：`0x140508FA0` 读 `0x140BDDB70 + (id-1040)*24` 的 handler 指针和能力分类，填入运行态表。命令 1161 的 handler 为 `0x14055F450`，进一步调用 `0x140A25250`。

`0x140A25250` 首先检查当前 dxCtx 非空，RTV 数量 <=8。对每个非 INVALID 的 RTV ID 调用 `0x140A1EB30`，若返回 NULL 就返回 0x101；INVALID 则跳过这次查询。故 §59 解绑成功证明该次确实拥有上下文，不支持“空转成功”说法。

`0x140A1EB30` 先以 `(cid<<16)|rtvId` 查缓存，缓存未命中时在 `0x140A1EB9E` 读字节 `0x141494261`：为 0 就返回 NULL；非零才调用 `0x140156CF0(ctx,type=0,id,buffer,size=32)` 读取 COTable 行，然后调用 `0x140A20120` 验证 surface、BIND_RENDER_TARGET bit24、格式/维度及子资源范围。

因此剩余分叉是：缓存未命中且懒加载开关关闭；COTable 读取失败；或读出的视图行/关联 surface 验证失败。`tools/vmxmem.py` 已加入只读字段 `rtvLazyGate` 与 `rtvHashCnt`。**随后用户只读快照得到 `rtvLazyGate=1`、`rtvHashCnt=0x800`，不支持采样时懒加载关闭；后者是桶数，不表示 RTV 1 已缓存。后续见 §62，未修改门控。**

## §62 DX 视图旧格式被拒；仅改为 typed format 142 仍不足（2026-11-15）

### 62.1 实际 COTable reader 和绑定语义

`0x140156CF0` 是跳板，实际 reader 为 `0x1404D1350`。它检查 `ctx+0x5F51+type*4` 的 MOB ID 非 INVALID、行 ID <=65535，然后调用 `0x1404C9FB0(mob,id*entrySize,dst,entrySize)`。RTVIEW 的 entrySize=32，ID1 对应 offset32、length32。后者调用 `0x1404C7D30`，检查 MOB 存在、范围及映射；仅凭 guest 写入日志不能确认该读操作成功。

`SET_COTABLE` handler `0x14055EAF0 -> 0x140558C60` 检查 validSize 的行对齐及不超过 MOB 大小。`0x140558E14–0x140558E45` 只初始化 [validSize/rowSize, mobSize/rowSize) 的行。故 validSize=4096、MOB=4096 时保留全页；validSize=0 会初始化全页，不应再解释为“容量为零”。`0x1404D1A30` 将 MOB ID 写入 DX state MOB 的 `0x19E0+type*4`，并更新已缓存 context 的 `+0x5F51+type*4`。

### 62.2 格式校验的确定性问题

RTV validator `0x140A20120` 先找 surface、检查 BIND_RENDER_TARGET bit24，再调用 `0x140A084D0`。视图格式由 row+4 读取，在 `0x140A085B9` 调 `0x140A08130`，返回非零则在 `0x140A085C0` 拒绝。format1 在该分类函数直接返回1，因此旧 RTV 行 `X8R8G8B8=1` 不合法。此为静态条件，不等于已捕获 §59 实际失败指令。

上游 `drivers/gpu/drm/vmwgfx/device_include/svga3d_types.h` 确认 `B8G8R8X8_UNORM=142`（https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/vmwgfx/device_include/svga3d_types.h）。本机分类表对142返回0。surface格式另经 `0x140A081B0` 规范化，format1转换到116；所以 view与surface要求兼容，而非整数相等。

复核修正：`0x140A201C5` 检查的是 **row+4 的格式支持位**，不是 mip 位；2D mipSlice 在 row+0xC、arraySlice在+0x10、arraySize在+0x14。surface 类型函数 `0x140A1ED50` 的返回5来自 flags bit0（cube），不是 bit24；当前非cube、depth1且bit18未置的2D surface返回3。

### 62.3 单变量实测 §62-view142

仅将模板 RTV row 的 format1改为142；surface格式仍1、surface定义/绑定、MOB、CB flags和顺序不变。客体构建通过（120256 bytes，26个已有警告），kmod/marker/弱引用检查通过。旧安装备份 `/var/tmp/vgpu-fb/backup-59.kext`。

新启动 boottime=2026-11-15 21:00:58，21:01:26 自报 `[build §62-view142]`，21:01:56 测试：

| 阶段 | 结果 |
|---|---|
| RTV row1 | sid3、format142、TEXTURE2D、mip0、array0+1 |
| DEFINE_CONTEXT / BIND_CONTEXT / SET_COTABLE | COMPLETED |
| LADDER1 / LADDER2 / 3a解绑 | COMPLETED |
| 3b绑定RTV1 | STATUS3 / errorOffset0 |
| CLEAR_RTV / READBACK | 未执行（前置失败） |

因此 **修正视图旧格式并不足以解决当前绑定失败**，不能倒推旧格式不是错误，也不能证明读行已成功。后续 SET_SHADER_IFACE 在错误队列上无应答不作为独立证据。旧版输出“DX manager still absent”和“CLEAR refused”不能作判据，已将源码与构建 marker 改为报告目标未达成/清屏未完成。

### 62.4 共享表项读回 §62-observe

第二次启动添加只读观测，不改变命令序列。124368-byte 构建通过，26个已有警告、弱引用0、全部marker通过；旧安装备份 `/var/tmp/vgpu-fb/backup-62-view142.kext`。boottime=21:10:30，21:10:57.728 自报 `[build §62-observe]`；21:11:27 实测：

```
OBSERVE surface after DEFINE sid3 fmt1 flags0x1000040 mips1 ms1 size256x256x1 mob4 array1 pitch1024
OBSERVE COTable after SET mob5 row1 00000003 0000008e 00000003 00000000 00000000 00000001 00000000 00000000
LADDER 1/2/3a COMPLETED
LADDER 3b REFUSED
```

context state MOB 的 `+0x19E0` 读到5，与 RTV COTable MOB一致；该字段是宿主 SET_COTABLE 路径写入的位置。RTV行仍完整，surface共享表项也仍为预填值。**这些是guest共享内存观测，不等价于宿主缓存对象/实际读取buffer的快照，不能据此排除所有MOB读取或缓存问题。**

DEFINE V1 handler `0x14054F5B0` 确会构造 mobid=INVALID 的描述；但实际 helper `0x14054DB40` 在现有 surface 行 format 非零时直接拒绝，不会执行其64字节覆盖。当前先预填非零format，可能正走该路径：FIFO fence完成只证明消费进度，旧日志“DEFINE accepted”不能作为语义成功证据。`0x140544310` 是另一路整行复制函数，不能混同本次 DEFINE V1 handler。

下一判别点是宿主读取是否成功、surface懒加载/格式能力及view兼容检查的具体返回。需要把guest预填状态与host缓存状态区分开，不再从单一NULL或格式同名猜根因。

## §63 surface 与 RTV 同为142仍失败；发现非多采样表项的 count 矛盾（2026-11-15）

相对 §62-observe 仅将模板 surface 的共享表项及 DEFINE payload 格式改为142；RTV142、flags、几何、MOB和命令顺序保持。旧安装备份 `/var/tmp/vgpu-fb/backup-62-observe.kext` 并经 diff 比对一致。安装124368-byte二进制，弱引用0，Code=28后重启；boottime=21:21:20，21:21:46.733自报 `[build §63-surface142]`。

21:22:16 实测：

```
OBSERVE surface after DEFINE sid3 fmt142 flags0x1000040 mips1 ms1 size256x256x1 mob4 array1 pitch1024
OBSERVE COTable after SET mob5 row1 00000003 0000008e 00000003 00000000 00000000 00000001 00000000 00000000
LADDER 1/2/3a COMPLETED
LADDER 3b SET_RENDER_TARGETS {rtv 1} STATUS3 / errorOffset0
CLEAR_RTV not attempted
```

21:22:46 heartbeat #30 status0，SSH正常。故将两种格式统一为142仍不足；失败后的SET_SHADER_IFACE超时不作独立归因。

继续反汇编：RTV验证器经 `0x1404D1B30` 查surface缓存，miss转 `0x140A0D2B0`。后者从type1 OTable读64B描述，在启用相应检查时调用 `0x140A0BDF0`。该验证器在 `0x140A0C07A` 检查flags bit32；未置时，`0x140A0C081–0x140A0C085` 要求描述 `+0x0C` 的multisampleCount为0，否则返回false。当前flags=0x1000040（bit32未置）却ms=1，构成确定的字段矛盾；尚未捕获运行态实际到达此分支，不能直接宣称这是唯一根因。

下一轮只对DX模板把共享表项及DEFINE payload的multisampleCount改为0，保持142/142与其他属性不变。暂不改共享helper的其他调用者，以免同时改变显示通路。

## §64 非多采样count改0仍不足；mobPitch也违反未置bit12的约束（2026-11-15）

`§64-ms0` 仅覆盖DX模板的surface entry[3]与DEFINE words[6]为0；构建26个已有警告、marker通过、弱引用0。旧安装备份 `/var/tmp/vgpu-fb/backup-63-surface142.kext`，boottime=21:26:04，21:26:29.815新标签确认。

21:26:59读回surface为fmt142/flags0x1000040/mips1/ms0/256x256x1/mob4/array1/pitch1024，RTV行及context COTable MOB5保持；LADDER 1/2/3a完成，3b仍STATUS3/errorOffset0，CLEAR跳过。说明ms0不足以解决全部问题，不等于ms1原本合法。

继续核对surface验证器：`0x140A0C4A3`检查flags bit12，未置时跳到`0x140A0C56B`，要求描述+0x28的mobPitch为0；当前flags未置bit12而pitch1024，是另一处确定的描述矛盾。下一轮相对§64只把DX模板mobPitch改0；这与屏幕寄存器BYTES_PER_LINE不同，不应混为显示pitch限制。

## §65 首次 DX CLEAR + READBACK 取得预期绿色像素（2026-11-15）

`§65-pitch0` 相对§64只将模板surfaceEntryFill的pitch参数1024改0；surface/RTV格式142、multisampleCount0、flags0x1000040保持。构建26个已有警告、marker通过、弱引用0；旧安装备份 `/var/tmp/vgpu-fb/backup-64-ms0.kext`。boottime=21:30:46，21:31:20.784确认新标签。

21:31:50.818–.834实测：

```
OBSERVE surface after DEFINE sid3 fmt142 flags0x1000040 mips1 ms0 size256x256x1 mob4 array1 pitch0
LADDER 1/2/3a COMPLETED
LADDER 3b SET_RENDER_TARGETS {rtv 1} COMPLETED
LADDER 4 CLEAR_RTV COMPLETED (status1 errorOffset0)
TEMPLATE READBACK {sid3} ACCEPTED
TEMPLATE pixels first=0xff00ff00 mid=0xff00ff00 (want0xff00ff00)
```

这是首个DX命令执行与预期像素读回的联合证据，不再仅有COMPLETED状态。相对§64的单变量变化表明，当前组合中mobPitch1024是剩余阻塞之一；不能反推此前ms1或legacy视图格式1合法。21:32:20 heartbeat#30 status0，SSH正常。

尾部独立SET_SHADER_IFACE仍STATUS3，发生在成功读回之后，不推翻清屏像素结果；下一轮将其隔离到探索开关，增加全65536像素校验与队列健康检查，并新启动复测。当前只验证首/中两点，尚不宣称完整绘制流水线、Metal、硬件OpenGL或屏幕呈现已实现。

## §66 新启动复测：65536像素全绿且队列继续响应（2026-11-15）

`§66-verify` 保持§65成功的资源与命令参数，新增全图读回校验及随后一条INVALID rasterizer state命令；SET_SHADER_IFACE隔离到kDxExplore=1，相关marker也跟随开关保留。函数返回值收紧为CLEAR、READBACK、全像素和队列检查全部通过。

构建124392 bytes，26个已有警告，marker通过、弱引用0；旧成功版备份 `/var/tmp/vgpu-fb/backup-65-pitch0.kext`。boottime=21:36:26，21:36:55.817确认新标签。21:37:25.855–.867结果：

```
OBSERVE surface sid3 fmt142 flags0x1000040 mips1 ms0 size256x256x1 mob4 array1 pitch0
LADDER 1/2/3a/3b/4 COMPLETED
TEMPLATE READBACK ACCEPTED
TEMPLATE pixel verification total65536 mismatches0 last0xff00ff00 -> PASS
TEMPLATE post-readback queue health -> COMPLETED
TEMPLATE verified round trip -> PASS
```

backing MOB在setup时由CPU预填洋红0xffff00ff，成功读回后全部65536像素为绿色，排除仅查看未变化预填buffer的假阳性。§65与§66为两次独立启动的成功结果；本轮没有执行尾部SET_SHADER_IFACE，实验链没有STATUS3。

**L1的清屏像素门槛已达成。** 证据覆盖DX上下文、COTable懒加载RTV绑定、渲染目标清屏、MOB读回和后续队列响应；不覆盖三角形光栅化、shader、资源上传、显示呈现或应用级Metal/硬件OpenGL。未新增宿主补丁，既有FE→FC仍是环境前提。

后续优先复用此健康基线做shader/三角形绘制；显式DEFINE_RTV、规范的DEFINE/BIND资源生命周期及共享helper其他调用者的旧ms/pitch字段需分别复测，不应把历史FIFO fence误作语义成功。

## §67：最小 SM4 PS 对象生命周期（2026-11-15）

### 协议与实验设计

本轮从§66基线继续。Mesa源码核实了三处容易混淆的协议细节：

- DX shader使用MOB里的原始SM4 token流，不是DXBC容器，也不是旧版SHADER_DEFINE的inline字节码。`DX_DEFINE_SHADER`(1201) body仅12B `{shaderId,type,sizeInBytes}`；`DX_BIND_SHADER`(1203) body16B `{cid,shid,mobid,offset}`。两条含header共44B。
- `SVGA_COTABLE_DXSHADER=10`，每项32B `{type,sizeInBytes,offsetInBytes,mobid,pad[4]}`。旧tc[]注释把type3–11错标为UAV/VB/IB等，现按协议纠正；此前只发送type0，故这些注释不影响§66结果。
- Mesa SM4路径signature长度为0；token流头两个DWORD分别是版本和总DWORD数。`ps_4_0; dcl_output o0.xyzw; mov o0,l(1,0,0,1); ret`共14 DWORD/56B。opcode分别为101/54/62，指令长度3/8/1；常量红色仅是shader内容，本轮不发DRAW，不能据对象接受声称画出了红色。

证据来源（均为本轮实际读取的实现）：

- [Mesa svga_cmd_vgpu10.c](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/src/gallium/drivers/svga/svga_cmd_vgpu10.c)：`SVGA3D_vgpu10_DefineAndBindShader`、`SetShader`、`DestroyShader`。
- [Mesa svga_shader.c](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/src/gallium/drivers/svga/svga_shader.c)：`define_gb_shader_vgpu10`，长度为codeLen+signatureLen。
- [Mesa vmw_shader.c](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/src/gallium/winsys/svga/drm/vmw_shader.c)：MOB顺序复制code与可选signature。
- [Mesa svga_tgsi_vgpu10.c](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/src/gallium/drivers/svga/svga_tgsi_vgpu10.c)与[VGPU10ShaderTokens.h](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/src/gallium/drivers/svga/include/VGPU10ShaderTokens.h)：版本、长度、PS颜色输出、立即数与指令编码。

探针先要求§66全部像素及queue health通过，然后分配两个4KiB PT64_0 MOB：7为shader COTable、8为code。type10以validSize0初始化，再同一CB发DEFINE+BIND，随后单独SET/UNSET/DESTROY/queue health；首个失败立即返回，保留backing直到设备生命周期结束，避免失败后释放宿主仍可能引用的页。独立开关`kDxShaderProbe=1`，`kDxExplore=0`；延迟90s，构建标签`§67-shader`。没有新增宿主补丁。

### 验证

客体编译链接通过，所有新旧可达性marker存在，安装检查weak references=0；AuxKC暂存返回预期Code=28。宿主读取真实源码中的psRed数组验证14 DWORD以及3/8/1指令边界通过；`git diff --check`通过。

本次`kern.boottime`为2026-11-15 23:40:49，驱动23:41:26加载并打印`[build §67-shader]`；90秒延迟后23:42:56执行。日志查询限定在本次启动之后：

```
TEMPLATE pixel verification total65536 mismatches0 last0xff00ff00 -> PASS
TEMPLATE post-readback queue health -> COMPLETED
TEMPLATE verified round trip -> PASS
SHADER MOB 7 FIFO fence -> COMPLETED
SHADER MOB 8 FIFO fence -> COMPLETED
SHADER SET_COTABLE type10 mob7 valid0 -> COMPLETED
SHADER DEFINE+BIND PS1 mob8 bytes56 -> COMPLETED
SHADER SET PS1 -> COMPLETED
SHADER UNSET PS -> COMPLETED
SHADER DESTROY PS1 -> COMPLETED
SHADER lifecycle queue health -> COMPLETED
SHADER lifecycle verdict -> PASS (no DRAW)
```

**本轮净结果：最小PS的显式DEFINE/BIND/SET/UNSET/DESTROY对象生命周期完成，且§66清屏读回基线保留。** 这里第一次获得shader对象链的实际客体执行证据；不能把接受/选择shader扩展为已验证编译、光栅化或红色像素输出（实现可能推迟到DRAW）。下一步在此基础上加入VS与确定性三角形，补齐viewport、拓扑、栅格化/混合状态并做区域像素校验。Metal/硬件OpenGL应用级接口仍未实现。

资源约束：两个新增MOB合计8KiB，本轮成功与失败均保留backing以免宿主悬挂引用；这个单次启动探针不支持热卸载/反复创建，正式资源管理需补DESTROY_GB_MOB和设备静止后的回收。

## §68：VertexID 三角形与 DRAW 前置状态（2026-11-16）

### §68a：全部状态接受，首个 DRAW 被拒

从§66/§67成功基线继续，新增`dxDrawProbe`，只在清屏全像素验证、PS生命周期均通过后执行。VS为54 DWORD的SM4原始token流：`dcl_input_siv v0.x, vertex_id`、`dcl_output_siv o0, position`、两次IEQ/MOVC和RET，按VertexID 0/1/2输出(-0.75,-0.75,0,1)、(0,0.75,0,1)、(0.75,-0.75,0,1)。PS复用§67的常量红色代码，重建已销毁的PS1。

协议依据为Mesa `svga_tgsi_vgpu10.c`的`TGSI_SEMANTIC_VERTEXID`分支与`emit_input_declaration`/`emit_output_declaration`，及`VGPU10ShaderTokens.h`的操作码、操作数位域。宿主独立解析真实C++数组，确认54 DWORD以及4/4/2/7/15/7/12/1指令边界；解释实际token得到上述三个顶点。此检查仅验证编码和本地语义，不代替宿主shader执行证据。

新增MOB9承载VS，MOB10承载type6 rasterizer COTable；显式DEFINE/SET solid、no-cull状态；设置VS/PS、TRIANGLELIST、default blend/sampleMask=0xffffffff、256x256 viewport。§68a使用INVALID input layout，无VB。计划在DRAW完成后将读回MOB填洋红色，再READBACK，并用整数像素中心边界检查全部65536像素：预期18432红、47104绿，无像素中心恰好在三角形边界。

实测启动`kern.boottime=2026-11-16 00:17:30 CST`，00:17:57加载`[build §68-draw]`，00:19:27执行90秒延迟实验。§66和§67基线通过；MOB9/10、raster COTable、DEFINE/SET raster、重建PS、DEFINE/BIND VS、SET VS/PS、SET_INPUT_LAYOUT(INVALID)、TOPOLOGY、BLEND、VIEWPORT全部COMPLETED。首个`DX_DRAW=1152 {vertexCount=3,startVertexLocation=0}`，CB39，返回`STATUS 3 / errorOffset 0`，立即停止，**没有执行DRAW后的READBACK，没有三角形像素证据**。

同一轮宿主对所有graphics-shader COTable `SET_INPUT_LAYOUT(INVALID)`返回COMPLETED，仍不能据此认为它满足draw。上游 Mesa 的`SVGA3D_vgpu10_DefineElementLayout(swc,count,id,elements)`发包长度为`sizeof(cmd)+count*sizeof(desc)`，VertexID-only shader 可使用明确的零元素layout；据此做单一变量对照。

构建及marker检查通过，26条既有警告，weak references=0，安装Code=28。首轮SFTP因Git Bash转换`/var/tmp/...`路径而ENOENT，远端目录实际存在；加`MSYS_NO_PATHCONV=1`后上传成功。

### §68b：零元素 input layout 对照

上游`SVGA3D_vgpu10_DefineElementLayout`按`4 + count*sizeof(desc)`发包，count0的body只有layout ID。对照增加MOB11/type3 COTable，`DEFINE_ELEMENTLAYOUT {id1,count0}`后SET id1，替换§68a的INVALID layout；其余VS/PS、状态、DRAW与像素判据保持不变。

实测启动`kern.boottime=2026-11-16 00:22:26 CST`，加载`§68b-layout`。§66/§67基线、MOB11、type3 COTable、零元素DEFINE_ELEMENTLAYOUT和SET_INPUT_LAYOUT(1)全部完成；首个DRAW仍返回`STATUS 3 / errorOffset 0`。立即停止，无DRAW后READBACK，无三角形像素证据。结论仅为：补零元素layout不足以使当前绘制通过，不能据此证明所有layout条件均满足。

### §68c：有效 blend/depth 状态对象对照

只读复核当前宿主`vmware-vmx.exe`：文件偏移`0xbdcff0`的函数指针为`0x14055C4D0`，同表项`+12`为`0x480=1152`；相邻表项命令号为1151/1153。该DRAW handler要求读取8字节，再调用`0x140A24700`，与协议的vertexCount/startVertexLocation吻合；此前另一份报告的`0x14055B0D0`和44字节包长是错误映射，不能作为DRAW证据。

`0x140A24700`先取当前context，携带mask `0x3ffff`调用`0x140A18D90`，校验false时返回`0x101`；通过后才调用绘制后端并返回`0x100`。`0x140A18D90`状态校验器在`0x140A18E2F`读取`context+0x5c04`，ID为-1时跳到`0x140A18E65`返回false；后续`0x140A18EEE`调用`0x140A121E0`验证depth/stencil。这与当前SET_BLEND(INVALID)及§66遗留的SET_DEPTHSTENCIL_STATE(INVALID)冲突；静态拒绝条件不等于已确认本轮运行首先命中的分支。

新增MOB12/type4 blend与MOB13/type5 depth COTable，validSize=0初始化；DEFINE_BLEND(1193)的body为104字节：id、4字节flags、8个12字节perRT，blend关闭、ONE/ZERO/ADD、RGBA write mask；DEFINE_DEPTHSTENCIL_STATE(1195)的body为20字节：id和16个uint8字段，depth/stencil关闭，休眠字段保留合法ALWAYS/KEEP枚举。分别单条CB定义/绑定id1，其余VS/PS、layout、DRAW和像素判据不变。构建标签`§68c-states`，构建与新增marker通过，26条既有警告，weak references=0，安装Code=28。

实测新启动`kern.boottime=2026-11-16 00:45:17 CST`，00:45:40加载`§68c-states`，00:47:10执行延迟探针。§66/§67通过；全部新增COTable/DEFINE/SET完成，DRAW首次完成，READBACK和随后的同步完成：

```text
DRAW 3 vertices -> COMPLETED
DRAW READBACK -> COMPLETED
DRAW pixels red18432 green47104 other0 expectedRed18432 mismatches0 center0xffff0000 corner0xff00ff00 -> PASS
DRAW post-readback queue health -> COMPLETED
DRAW triangle verdict -> PASS
```

独立复测启动`kern.boottime=2026-11-16 00:48:20 CST`，00:48:43加载同一`§68c-states`，00:50:13再次得到相同的18432红/47104绿、other0、mismatches0及健康队列。两次启动均按本次boottime过滤日志，未复用旧启动结果。

这是首次**VS/PS实际着色、光栅化、全像素区域读回均通过**的客体证据；不再仅限于清屏或shader对象接受。改变的状态集合消除了§68b失败，但没有分别隔离blend与depth单项贡献。MOB9–13合计20KiB均保留到重启，尚无通用资源回收/用户态提交接口。应用级硬件OpenGL和Metal仍未实现，不能据本探针声称二者可用。

## §69：用户态固定三角形提交与独立像素验收（2026-11-16）

### 接口与同步边界

`vgpuDrawUserClient` 通过 framebuffer 的专用连接类型 `0x56475055` 暴露两个同步 selector，要求管理员权限。selector 0 无输入，返回三个 UInt64（generation、256、256）；selector 1 接收 generation/row，复制精确1024字节的一行BGRA像素。未知selector、异步调用、错误参数数量/尺寸和越界行被拒绝；其他连接类型转交IOFramebuffer原有四参数newUserClient，保持显示连接路径。

接口仅在§68启动三角形像素及后续队列健康验证通过、queue1/cid1/sid3/256x256匹配、无present timer且周期probe关闭时就绪。延迟启动实验与用户提交共用设备锁，用户行读取也持此锁；generation为设备全局值，任一连接的新绘制使旧帧失效，读取失败必须丢弃整帧。每次调用重新绑定rasterizer、清绿、DRAW、CPU填洋红但不上传、READBACK及同步。失败后禁用接口，不继续复用可能仍在被设备读取的CB。

### 构建及生命周期修正

- 初次编译发现IOFramebuffer隐藏了五参数newUserClient重载，普通连接转交改用父类四参数版本后编译通过。
- SDK声明的`thread_call_cancel_wait`在本客体内核不向kext导出；集合构建报Code=31及Failed to bind。改为调度锁下计数待执行/执行中回调，用公开thread_call_cancel取消待执行项，IOLockSleep/Wakeup等待运行项完成。五类回调均禁止停止后重排，停止后再释放映射，锁直到free才释放。
- 原install.sh管道掩盖了Code=31；现保留完整kmutil输出/退出码，仅Code=28（需要重启）作为预期非零结果，其余立即失败。
- CB状态QUEUE_FULL原先continue跳过超时，现纳入统一wall-clock期限，避免同步用户请求永久持锁。
- 设备对象销毁/复位仍未实现。停止CPU回调不等于DMA停止，因此CB启用后stop保留设备可见backing到重启；这是明确的资源保留局限，不能声称支持热卸载/重载。并发stop及超时注入尚未实测。

修正版构建26条既有警告、marker全通过、weak references=0；安装Code=28。`tools/vgpu-user-draw.c`以`clang -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation`编译，无警告。

### 第一次启动实测

`kern.boottime=2026-11-16 01:18:01 CST`，01:18:28.985加载`§69-userclient`。非root打开返回NotPrivileged；01:18:43管理员调用时尚处90秒延迟窗口，返回NotReady，属于预期行为。01:20:04运行10轮+第二连接+关闭后重开，总计12次用户触发绘制：

```text
round 1/10 generation=1 red=18432 green=47104 other=0 mismatch=0
round 10/10 generation=10 red=18432 green=47104 other=0 mismatch=0
cross-connection generation=11 red=18432 green=47104 other=0 mismatch=0
reopen generation=12 red=18432 green=47104 other=0 mismatch=0
PASS all checks
```

12帧均逐行读取全部65536像素，用户态以三条有向边的cross product计算像素中心归属，独立于内核的绝对值不等式判据。同时通过generation0、旧generation（含跨连接）、未知selector、错误scalar数量、错误结构输出长度、row256/UINT64_MAX拒绝检查。这里证明的是应用进程→自研IOUserClient→DX→像素读回；尚未接入OpenGL framework或Metal设备/驱动插件。

### 独立复测与启动异常

01:20:52客体请求重启后，VNC持续显示Apple启动画面，SSH先超时后握手EOF，宿主日志无新驱动心跳。约四分钟后对实验VM执行一次hard reset恢复。查询01:20:50–01:25:10的持久日志未取得vgpu-fb或panic记录；根因未定，不能宣称已排除驱动因素，也不能把这次计为成功复测。

恢复后的独立启动`kern.boottime=2026-11-16 01:25:11 CST`（客体墙钟与宿主略有偏差），01:25:41加载驱动。权限检查再次PASS；01:27:11在初始化截止前调用返回NotReady，01:27:21再运行相同10轮+跨连接+重开，generation重新从1到12，每帧同样red18432/green47104/other0/mismatch0，最终`PASS all checks`。两次成功启动共24帧、1572864像素独立比较通过；启动停滞的稳定性问题仍保留开放。

## §70：用户顶点经真实 vertex buffer 绘制（2026-11-16）

### ABI 和资源路径

新增同步 selector2：结构输入精确56字节，`uint32 version=1, reserved=0; float positions[3][4]`；无scalar输入，输出仍为三个UInt64（generation、256、256）。仅管理员可连接。内核复制到局部UInt32数组，以IEEE754位模式检查x/y在[-1,1]、z在[0,1]、w精确+1，拒绝NaN/Inf，不在内核执行浮点运算。selector0/1保持原ABI，未知selector测试迁移到99。

新路径为用户顶点→MOB14→sid4 BUFFER→input layout2→VS3→既有PS1/RTV→DRAW→READBACK。MOB14/15分别保留4KiB顶点与shader backing，首次有效selector2惰性初始化，后续原位复制48字节并发`DX_BUFFER_UPDATE {sid4,x0,width48}`。不通过修改VertexID shader常量模拟输入，不开放任意命令、物理地址或shader。

协议实测参数：

- BUFFER format37，flags=0x00100010（HINT_VERTEXBUFFER|BIND_VERTEX_BUFFER），size=48×1×1，mips1、multisampleCount0、mobPitch0；DEFINE_GB_SURFACE命令体36字节，经FIFO定义和绑定并等待fence。
- layout2含一个24字节InputElementDesc：slot0、offset0、format122（R32G32B32A32_FLOAT）、per-vertex、step0、register0；DEFINE_ELEMENTLAYOUT体28字节，无count字段。
- SET_VERTEX_BUFFERS体16字节：startBuffer0、sid4、stride16、offset0。shader/layout沿用已创建COTable，未重新初始化table。
- VS3为60字节裸SM4 token：dcl_input v0.xyzw；dcl_output_siv o0.xyzw position；mov o0,v0；ret。无需追加signature；MOB15承载代码，shaderId3。
- 两条绘制路径每次明确重绑VS、layout、VB和rasterizer；selector0使用VS2/layout1并解绑VB，避免受上一次selector2残留状态影响。

### 构建和当前启动实测

构建标签`§70-vertex-buffer`，26条既有warning、全部reachability marker通过、weak references=0。旧§69 bundle备份在客体`/var/tmp/vgpu-fb/backup-69.kext`。安装返回预期Code28，正常重启后`kern.boottime=2026-11-16 01:44:28 CST`，01:44:55.677加载新标签。01:46:25.773启动固定三角形PASS，接口READY；此次重启没有复现§69的启动停滞。

01:49:44首个顶点请求中MOB14、MOB15、BUFFER define/bind、VS3、layout2全部COMPLETED，日志每项出现一次。用户程序以clang -Wall -Wextra -Werror编译，无警告；非root打开被拒绝。第一次验收17帧，补齐连续顶点和跨连接顶点测试后再验收22帧，generation连续1..39，无重启、无每帧MOB分配。

| 几何 | 屏幕顶点 | 红色像素 | 绿色像素 | other / mismatch |
|---|---|---:|---:|---:|
| 固定VertexID三角形 | (32,224)、(224,224)、(128,32) | 18432 | 47104 | 0 / 0 |
| 用户几何A | (40,200)、(168,200)、(40,100) | 6400 | 59136 | 0 / 0 |
| 用户几何B | (32,32)、(192,32)、(112,192) | 12800 | 52736 | 0 / 0 |

39次真实DRAW各读取并比较全部65536像素，共2555904次新帧像素比较；另每轮80个非法selector2输入均拒绝，并逐帧复读原generation/图像确认没有副作用（这160次旧帧复读不算新增DRAW）。最终工具输出PASS all checks。覆盖：

- A/固定/B/固定反复切换；额外连续A→B→A，不依赖固定绘制作为中间重置。
- 第二连接提交B，第一连接读取该generation；旧generation失效；关闭重开后固定绘制和A均通过。
- 对三个顶点的每个分量检查NaN/Inf和越界、w不为1；错误version/reserved、55/57字节结构、错误scalar数量和非零结构输出均被拒。
- CPU独立整数边函数参考，测试几何边不经过像素中心；每帧先清绿，GPU读回前CPU填洋红但不上传。新几何出现且旧几何消失，排除固定图像和旧CPU缓冲伪成功。

### 范围与后续

本节已证明应用提供的坐标经真实GPU顶点输入产生像素，仍是受限单三角形专用接口，不等于硬件OpenGL或Metal可用。尚缺通用buffer/index/texture/shader生命周期、呈现及系统用户态驱动接入。§69的启动停滞根因仍未解决；本节仅一次新启动，两个用户程序运行，不能写成两次独立启动。

保留失败关闭策略：初始化即使只因本地分配失败，也会禁用全部绘制到重启；未发布命令前可恢复的内存失败尚未细分。设备可见backing仍保留至重启，无DMA停止证明、热卸载/重载和超时注入验收。后续优先接通可识别的应用级OpenGL/Metal入口，并逐项补齐其实际需要的资源与shader能力。

## §71：系统图形API验收失败语义与接入缺口（2026-11-16）

### 修正探针假成功

原gl_probe.c和metal_probe.m无论失败都退出0，不能用于最终目标验收。现OpenGL默认/软件模式仅作诊断；仅强制accelerated + NoRecovery上下文、实际virtualScreen的accelerated属性非0、红色中心与黑色背景像素都正确且无GL/CGL错误时返回0。增加对象清理和上下文恢复检查。Metal逐项检查设备、library/function/pipeline/buffer/queue/encoder，要求command buffer Completed且无error，全部16个float均为42才返回0；这仍只是compute验收，不代表render pipeline通过。

两探针在客体clang以-Wall -Wextra -Werror构建通过（GL关闭系统已弃用API警告）。当前SSH会话实测：

```text
software: virtualScreen=0 kCGLPFAAccelerated=0
software: RENDERER=Apple Software Renderer
software: centre(32,32)=255,0,0,255 background(4,60)=0,0,0,255 -> OPENGL RASTERISES
accelerated: CGLChoosePixelFormat failed (invalid CoreGraphics connection), npix=0
GL process exit = 1
MTLCopyAllDevices count = 0
FAIL: Metal device enumeration returned no devices
Metal process exit = 1
```

软件GL像素正确不计入硬件成功；默认CGL也返回invalid CoreGraphics connection。独立环境观察：ioreg查询IOAccelerator为空，/dev/console属主root，launchctl gui/501不存在。因此CGL失败有未登录Aqua会话的条件限制，不能仅凭该错误断言驱动不支持；Metal零设备及无加速器服务分别记录。

### 接入取证与阻塞

当前仅IOFramebuffer personality，专用管理员IOUserClient不是标准GL用户客户端；普通应用权限、独立context/资源及任意绘制状态尚缺。§70固定256×256、固定红色PS和每次清绿也不能冒充任意OpenGL语义。

客体实际存在ICL/KBL与AMD X4000/X5000/X6000的GLDriver.bundle目录。HD4000目录不存在；KBL的Info.plist声明AppleIntelKBLGraphicsGLDriver执行文件，但磁盘MacOS目录只有libigdmd.dylib，主映像需要进一步在进程/dyld shared cache中定位，不能由磁盘缺文件推断驱动不存在。

尝试用客体LLDB启动自有glprobe观察GLD入口，收到`non-interactive debug session, cannot get permission to debug processes`，本轮未绕过许可或更改系统调试权限。VMsvga2 SourceForge源码页面被Cloudflare拦截，未取得可复用GLD ABI。下一实验应取得目标系统真实驱动发现/加载契约，再实现最小服务和诊断GLD插件，验收系统CGL实际调用插件，不以替换系统gl符号、renderer字符串或独立shim冒充接入。

Apple历史官方文档明确存在第三方GLD plug-in接口：https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_pg_concepts/opengl_pg_concepts.html 。这不证明现代macOS兼容，但足以校正ARCHITECTURE.md中“Apple GL不挂外部GL驱动”的无条件断言。该文旧有“模板已有/OpenGL免费获得”等条目保留为历史假设并添加显著校正；当前OpenGL硬件与Metal设备目标均未完成。
