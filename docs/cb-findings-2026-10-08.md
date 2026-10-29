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
