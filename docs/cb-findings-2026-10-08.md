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
