# vGPU-macOS 项目状态

> 自研 macOS 客户机 SVGA3D 驱动（vgpuFramebuffer.kext）——在 VMware Workstation 的
> macOS 客户机内实现 GPU 直通驱动栈。目标是最终支持 macOS 应用级 3D 加速
> （Metal 对象层 → IOAccelerator）。

## 环境与工具

| 项 | 值 |
|---|---|
| 宿主 | Windows，VMware Workstation |
| VM | `E:\Vboxs\MacOSX\MacOS-R15\`（SSH 192.168.9.131 pika/IM0612） |
| 仓库 | `D:\Codes\vGPU-macOS` |
| 客体构建 | `/var/tmp/vgpu-fb/build.sh`（SSH 推送源码构建） |
| 安装 | `install.sh`（签名 + KextPolicy + auxKC；`Code=28` = 重启生效） |
| VNC 截屏 | `tools/vncsnap.py`（127.0.0.1:5901） |
| VNC 键盘 | `tools/vncsend.py` |
| SSH 工具 | `tools/mssh.py`（宿主 `.venv`：paramiko/pyDes） |
| 阶段开关 | `kFbStage`（FIFO）/ `kGbStage`（GB/DX）/ `kDxTest`（DX 实验） |

## 已达成的里程碑

### M1–M5：寄存器/FIFO/GB 对象（9-29 → 10-06）
- SVGA3D LIVE（DEVCAP_3D=1）；GB 对象模型 UP（MOB/Surface/OTable）。
- DX 命令集 OPEN：DXCONTEXT/SM41/GL43 devcap 全 1，DX context 创建成功。
- 关键坑：`words[]` 溢出（≥12 dwords）、命令 size 必须精确、OTable 条目先于 DEFINE 写。

### M6：STDU 显示路径全通（10-06 23:39，b89c619）
- 锁屏完整实时渲染——FIFO 所有权 + GB/DX 栈 + 显示共存。
- 四要素：①拓扑寄存器 `SVGA_REG_DISPLAY_*`(35-40)；②`UPDATE_GB_IMAGE`(1101)；
  ③3MB MOB 用 PT64_2；④surface 加 `SVGA3D_SURFACE_SCREENTARGET`(1<<16)。
- 呈现循环 30Hz：memcpy(VRAM→surface MOB) + UPDATE_GB_IMAGE + UPDATE_GB_SCREENTARGET。

### M7：CB（Command Buffer）机制打通（10-07，86bbdb9）⭐
- **根因确认**：vGPU10 的 DX 命令必须经 CB 提交（`SVGA_REG_COMMAND_LOW=48/HIGH=49`），
  legacy FIFO 只处理显示/GB 命令——DX/SURFACE_DMA 在 legacy FIFO 被静默忽略。
- CB 实现已通：64B `SVGACBHeader`（status/errorOffset/id u64/flags/length/ptr.pa/offset/
  dxContext/mustBeZero[6]）+ payload；提交 = 写 HIGH(PA>>32) → LOW(PA低32|cbContext)，
  NO_IRQ 轮询 status。
- **bootstrap 成功**：`SVGA_DC_CMD_START_STOP_CONTEXT{enable=1,context=0}` 经 device
  context (0x3f) 提交被宿主执行。CB device command 无 size 字段。
- SURFACE_DMA 必须原子提交（拆分提交 → NEXT_CMD 越过不完整命令 → 宿主 abort）。

## 2026-10-25 更新（上下文重建后）

- **CB header flags 判决完成**：host 的 CB validator 无条件拒绝 `flags & 0xFFFFFFFE`
  （判决实验见 `cb-findings` §18.4：flags=2/3 + len 0 → COMPLETED，len 12 → STATUS 4）。
  ⇒ `SVGA_CB_FLAG_DX_CONTEXT` 在本机不可用，CB header 无法携带 dxContext。
- **"队列 1"路线作废**：`START_STOP_CONTEXT` 硬编码只接受 `context == 0`；
  DC cmd 3/4/5 是上下文索引 0/1 的状态记账，与"建队列"无关（§18.1–18.3）。
- **★ 渲染命令被拒的最终根因（§19）**：所有 DX 渲染 handler 的第一道门是
  `[0x1414930A0]` 这颗 DX 管理器指针非空；x64dbg 实测它是 **NULL**。
  而该指针全程序只有一个写入点（0x14050a077），前置条件正是
  `test descriptor.bit1 (= SVGA_CB_FLAG_DX_CONTEXT)` —— 也就是说：
  **DX 管理器要"带 DX_CONTEXT 标志的 CB"才会被惰性创建，而那个标志被 header
  validator 封死。本 build 上不修改宿主就永远打不通渲染链。**
- **破局方案（待批准执行）**：`vmware-vmx.exe` 1 字节补丁
  file offset `0x87293` `FE → FC`（mask 0xFFFFFFFE → 0xFFFFFFFC，只放行 bit1）。见 §19.5。
- 实验版开关：`kFbStage=2 / kGbStage=4 / kDxTest=1 / kCbDump=1 / kFlagsProbe=1`，`kProbeEveryS=0`。

## 2026-10-26 更新（补丁已生效，但创建点走的是另一条路径）

- **1 字节宿主补丁已在位并验证有效**：`vmware-vmx.exe` file offset `0x87293` 现在是 `FC`
  （备份 `vmware-vmx.exe.bak-dxflag`）。客机实测 `probe: flags 2/3 … -> COMPLETED`，
  说明 header validator 已放行 `DX_CONTEXT`。
- **★ 但 DX 管理器指针仍为 NULL，而且原因被重新表述**（详见 `cb-findings` §22）：
  唯一写入点 `0x14050a077` 所在的片段 `0x140509fe8` **只有一个调用者** `0x1404c7a2b`，
  它的判据是**时间片/抢占**（`[dev+0x81d]`、`[dev+0x9bc]`、`[dev+0x71]` + 若干计数器），
  **不是"CB 完成"**。所以"带 DX flag 的 CB 一完成就会建管理器"这个假设是错的。
- **NULL 有两种可能，只读全局分不出来**：`0x140a17350(dxContext)` 未命中时走
  `0x140a15b90` → `0x1404cde70`，后者一旦失败就返回 NULL（于是 0x14050a077 写了 NULL）。
  `0x1404cde70` 的判据：cid<=0xFFFF → 查 DXCONTEXT cotable(type5) → `entry[0]==cid`
  → 用 `entry[1]` 当 mobid 读回 0x2000 字节。
- **实测新事实：第一个 DX 命令报 COMMAND_ERROR 之后，整条命令引擎就不再处理任何 CB**
  （不是"队列 0 失败 4~5 次才楔死"）。⇒ 旧的"四组扫描 matrix"只有第一组是真数据。
- **头部校验层也管 dxContext，且与 cbContext 绑定**：同样 `flags=3, dx=1`，
  `ctx1` 过校验（STATUS 3），`ctx63` 直接 STATUS 4。⇒ 带 dx 的 CB 一律发 ctx1/2。
- **已证伪**：`.vmx` 的 `svga.enableDXContext/enableDX/DX2/DX3/enableCmdBuffer2` 不是门
  （全部被接受、能开机，`SVGA3dClamp` 与 devcap 一字未变）；`svga.loghw`/`svga.forceTraces`
  在本机无输出；空 DX CB 的完成不会惰性创建管理器。
- 本轮 kext 变更：新增 `PRIME` 探测（空载荷 + DX flag + dxContext=cid）；RTV 扫描顺序改为
  `(ctx1,dx=cid)` 优先；`install.sh` 增加 **weak 符号硬检查**（kmutil Code=31 的静默失败）。
- 实验版开关：`kFbStage=2 / kGbStage=4 / kDxTest=1 / kCbDump=1 / kCbMobMode=1`，`kProbeEveryS=0`。

## 2026-10-29 更新（CB 命令通道打通，卡点收敛到 `DEFINE_RTV` 一条）

详见 `cb-findings` §25 / §26。三条硬结论：

1. **唯一合法的 CB 形态 = `dxContext=0` + PA 形态 + `flags=NO_IRQ(1)`。**
   `dxContext != 0` 一律 STATUS 4（`CB_HEADER_ERROR`），与 MOB/PA、bit1、载荷全部无关
   （四组启动数据交叉验证）。`SVGA_CB_FLAG_DX_CONTEXT`(bit1) 和 `SVGA_CB_FLAG_MOB`(bit2)
   都会被 `test flags,0xFFFFFFFE` 拒，已在 `cbSubmitCtx()` 内集中剥离前者。
2. **CB 命令通道已打通**：`DX_DEFINE_CONTEXT` / `DX_BIND_CONTEXT` / `DX_SET_COTABLE`
   三条 ACCEPTED，13 条命令打包 72 dwords 的 CB 也 `submitted` + sync CB COMPLETED。
   **这是自 §22 以来第一次进入命令解析层。**
3. **唯一稳定失败的 DX 命令是 `DEFINE_RTV`**，形态 `STATUS 3 (COMMAND_ERROR)`、
   `errorOffset 0`，两次独立启动复现。根因与 `dxContext` 的 STATUS 4 相同：
   宿主侧 DX 设备对象（`0x1414930A0`）从未创建，而全部 97 条渲染命令都在它的 NULL 检查上退出。

本轮修掉的真缺陷（都不是宿主问题）：
- 模板块因裸 `return;` 在运行期不可达 + 编译期被优化掉（10-28 遗留）。
- `tpl[80]` 写进 `tpl[64]`（栈越界，`n` 却已统计成 80，日志完全看不出来）。
- `cbSubmitCtx()` 三个静默 `return false`（header/pci/payload 三条路径都不打日志），
  以及新增的入口状态 dump（`mobMode/ready/virt/hdr/pci/dataVirt`）。

**本轮学到的实验纪律**：一条 COMMAND_ERROR 之后同一队列只剩超时，所以
**任何诊断性重发必须排在被判定那一笔之前**；在失败之后拆分探测 = 白烧一次启动。

## 当前开放问题

### P0：`DEFINE_RTV` 的 COMMAND_ERROR（2026-10-29 收窄后）
- 已排除：CB header 合法性、命令通道、MOB 校验、desc 排列（arraySize/cid/queue 全部穷尽）。
- 根因定位：`0x1414930A0`（DX 管理器）从未被创建。唯一创建点 `0x14050a077` 被
  `[dev+0x9bc] != 0 && [dev+0x71] == 0` 门控，且**只有时间片/抢占链会调用**。
- 下一步优先级：
  1. **HP command queue（`SVGA_CAP2_HP_CMD_QUEUE` = caps2 bit7 = 1 已置）**——唯一还没试过的入口。
     需先解出队列 MMIO 寄存器组、环描述符格式、doorbell 语义。
  2. **静态扫 `[dev+0x9bc]` / `[dev+0x71]` / `[dev+0x81d]` 的全部写入点**（零重启成本）。
  3. 从 vm3dmp.sys 抠"第一帧之前我们还缺的那些命令/cotable"。
- 预算提醒：**一条 DX 错误 = 一次重启**，每次实验只放一条 DX 命令。

### P1：~~context-0 CB 里的 SVGA3D 命令解析失败~~（10-29 已解决）
- 原现象：DEFINE_GB_MOB64（context-0 CB 第一条）`COMMAND_ERROR at offset 0`。
- 实际原因：header 不合法（`dxContext != 0` 或 flags 非 bit0），命令从未被解析。
  改成 `dxContext=0 + flags=1 + PA` 后，DEFINE/BIND/SET_COTABLE 全部 ACCEPTED（§26）。

### P2：DX 激活之外的验证
- CLEAR → SURFACE_DMA(READ_HOST_VRAM) → 客体校验品红。现在只差 `DEFINE_RTV` 过。

### P3：STDU 显示的 fresh-MKS 冻结
- 拓扑写位置矩阵已完整测绘（fresh MKS 上 topo 写在 CONFIG_DONE 后任何位置都会楔死宿主）。
- 唯一成功组合 = 10-06 23:39 的长寿 MKS。方向：CB 打通后 STDU 命令也走 CB，可能绕开
  legacy FIFO 的拓扑竞态。

## 实验开关（`vgpuFramebuffer.cpp` 顶部）

| 配置 | 效果 |
|---|---|
| `kFbStage=0`（出货默认） | 只读 kext，MKS legacy 呈现，显示正常 |
| `kFbStage=2 + kGbStage=4 + kDxTest=1` | DX/CB 实验（headless，SSH 活） |
| `kFbStage=2 + kGbStage=5` | STDU 显示实验（fresh MKS 有冻结风险） |

10-29 新增的三个独立开关（一次只动一个）：

| 开关 | 值 | 含义 |
|---|---|---|
| `kDxTemplateOnly` | 1 | 跳过整条 legacy GB/DX 链，模板在干净引擎上首发 |
| `kCbMobMode` | 0 | 0 = PA 形态（**本宿主唯一可用**）；1 = MOB 形态（header 必拒） |
| `kCbKeepDxContext` | 0 | 0 = 剥掉 bit1（**唯一可用**）；1 = 保留（header 必拒） |

部署窗口注意：DX 实验版下 SSH 间歇死亡——硬复位后前 2 分钟抢 put+build+install。

## vm3dmp.sys 逆向发现（10-08，Windows SVGA3D WDDM 驱动，Tools 12.4.5）

对 `driver/vm3dmp.sys`（用户提供）的 capstone 反汇编已完成核心路径解码：

### CB 提交架构（三层）
1. **提交 helper** `0x1400095d8(device, cbCtxIdx, dxCtx, payload, [qword0, len, ?, flags=2, ...])`：
   查设备结构 `+0x18a4` 能力位 bit5（COMMAND_BUFFERS）→ 走 CB（`0x14001a76c`→`0x140013990`）
   或 legacy FIFO（`0x14001a900`）。
2. **CB header 构造**（`0x140013990`，共 32 个 CB 上下文槽，每个 0xb38 字节）：
   - `flags`：DX 类调用（arg8=2）→ **1|2 = 3**（NO_IRQ|DX_CONTEXT）；条件位 4=MOB。
   - `dxContext`（header+0x24）= 第 3 寄存器参数，**所有 DX 调用恒 0**。
   - ptr（+0x18）：arg10 字节非 0 时为 {mobid(+0x18), offset(+0x1c)} + flags|4。
3. **payload = 多条 {id,size,body} 命令连续拼接**，无填充、无前导命令。

### 关键序列（反汇编实证）
- **DEFINE+BIND 打包单 CB**：`{0x477,4,cid}{0x479,12,cid,stateMob,0}`。
- **SET_COTABLE**：`{0x4b7,16,cid,type,mobid,validSize}`；**初始 validSize=0**。
- **COTABLE MOB 按类型精确分配**（`0x140011178` 条目数表）：RTVIEW=32 条目 →
  `round_up(32*4,4096)` = 4 KiB 单页 **PT64_0**（基页即数据页）。
- **DEFINE_GB_MOB64 也经 CB**（`0x14000fd70`：`{0x46f,20,mobid,ptDepth,ppn64,size}`，flags=3）。
- 解绑路径（context 销毁）：`SET_COTABLE{mobid=0,validSize=0}` + `DESTROY_GB_MOB{mobid}`——
  解释了 10-07 看到的神秘 `{0x446,4,mobid}` 尾随命令。
- CB device commands 无 size 字段（header.length 定界）。
- HWVERSION 仍写 0x20001（与我们一致，非差异点）。

### 本 MKS（WS14/15 时代 vGPU10）实测对照矩阵
| 提交 | vm3dmp 预期 | 本 MKS 实测 |
|---|---|---|
| DX 命令 + flags3 + dx0 | 执行 | DEFINE ✓，SET_COTABLE ✗（命令级） |
| DX 命令 + flags2（无 NO_IRQ） | — | CB_HEADER_ERROR |
| GB define + flags3 CB | 执行 | CB_HEADER_ERROR |
| GB define + plain CB（flags1） | — | COMMAND_ERROR @0 |
| GB define + legacy FIFO | — | fence 通过（显示证明执行） |
| DX 命令 + legacy FIFO | — | fence 通过但惰性（哨兵存活） |

> **宿主实为 WS 26.0**（vmware.log 实证），且宿主上 Windows VM 同样 BaseCapsLevel 9——
> level 9 是 WS26 新沙箱渲染器（mksSandbox/ISBRenderer-VK）对所有客户机的设定，**3D 照常工作**。
> Tools 13.1.5 的 vm3dmp.sys 与 12.4.5 逐字节相同（md5 一致）——逆向对象即现役驱动。
> **因此 MKS 完全支持该协议；剩余差异在我们自己的初始化/提交序列里。**
> darwin 的 UI 警告"此客户机操作系统不支持 3D 加速"只是产品策略文案，设备能力不受影响。

**结论**：本 MKS 的 CB 是 vGPU10 早期实现，与 vm3dmp 12.4.5（WS17 MKS）语义分叉——
GB define 只认 legacy FIFO、DX 命令集需要 12.x MKS 才完整支持。**时代匹配的 10.3.10 驱动
（已从 packages.vmware.com 下载其安装器）是正确参照**；其 vm3dmp.sys 位于 LZX 压缩的
MSI media CAB 内（cab_003，expand/7z 均失败，需 msiexec /a 走正式 MSI 数据库——
安装器只内嵌 48KB 引导 MSI，完整 DB 的提取路径待继续）。

### 10-08 晚新增：legacy DX + CB bootstrap = 启动死锁
DC START_STOP_CONTEXT 之后经 legacy FIFO 提交 DX 链（带 fence）→ guest 启动卡死
（进度条 1/3，SSH 死）。**CB bootstrap 与 legacy DX 链不能共存**——下次实验二选一。

## 参考资料与提取路径

- `vmwgfx`（Linux 4.19）：vmwgfx_stdu.c / vmwgfx_cmdbuf.c / vmwgfx_mob.c / vmwgfx_cotable.c
- SVGA 头文件（v4.19 device_include）：svga_reg.h / svga3d_cmd.h / svga3d_dx.h / svga3d_types.h
- `windows.iso`（ISO9660，isols.py 可提取）：SETUP.EXE（140MB，内嵌 OLE/MSI @1405740）
  → msiexec /a 解包 → vm3dmp.sys（Windows SVGA3D WDDM 驱动）→ 逆向 DX 激活序列
- `darwin.iso`：APM/HFS 格式（isols.py 不支持；guest hdiutil 挂载）
- metal2vulkan 项目：候选的 Metal 对象层复用参考（调研中）

## 提交链（10-07）

```
481c62f DX 实验状态 + 对象销毁
a8b7c53 自包含 DX 实验 + STDU 计时器修复
d6ea50d DX_BIND_CONTEXT 激活 + DMA 双向实验
83f2071 恢复显示优先配置
86bbdb9 CB 机制打通（device command 级）
83f2071..（当前）显示优先 + 实验开关
```
