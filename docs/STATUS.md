# vGPU-macOS 项目状态

> 自研 macOS 客户机 SVGA3D 驱动（vgpuFramebuffer.kext）——在 VMware Workstation 的
> macOS 客户机内实现 GPU 直通驱动栈。目标是最终支持 macOS 应用级 3D 加速
> （Metal 对象层 → IOAccelerator）。

> 最新进度：见 `docs/cb-findings-2026-10-08.md` §80。真实GPU surface桥接完成清屏、事务性generation回读与pack；非对称三角形整图/非原点子区域的普通及ASan/UBSan验证像素零差异。独立device与有界pixel已验证；系统pixel导出与context/FBO对象链正在复核，多资源及shader接入仍待实现。当前安装的§78诊断bundle仍明确拒绝设备；标准硬件OpenGL/Metal均未实现。

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

### M8：屏幕真的亮了 —— 走 legacy screen object，不是 STDU（11-12）⭐
- **画面来源确定**：`kFbStage=5 + kGbStage=0`，
  `DEFINE_SCREEN` + `DEFINE_GMRFB`(BAR1 直指 framebuffer) + `BLIT_GMRFB_TO_SCREEN`，
  每 33 ms 重发一次。无客人内存参与，每帧 9 个 FIFO dword。
- 实测：锁屏完整实时渲染（时钟自更新）；VNC 输入密码后进入**可用桌面**
  （菜单栏 / Terminal / 系统设置 / Dock）。`present tick` 稳定 30 Hz，FIFO 从不楔死。
- **STDU 在本宿主出不画面，且已证明不是我们的锅**：洋红对照臂（surface 红绿图案 +
  VRAM 洋红）截图为纯黑 → 客人接管 FIFO 后宿主**只呈现 blit 到 screen object 的内容，
  不采样 bind 过去的 screen target**。几何对齐（640x480 全链路）也排除。
- **新发现 `SVGA_REG_ENABLE_HIDE`**：写 `ENABLE|HIDE` 后宿主日志出现
  `SVGA hiding SVGA` → `Screen 0 Defined flags=0x3`（客人屏幕）；不写则是
  `Screen 1 flags=0x2`（legacy 屏）。**ENABLE 是设备状态，跨客人重启存活**，
  详见 `cb-findings` §47.1。
- 细节与作废清单见 `docs/cb-findings-2026-10-08.md` §47。

### M1–M5：寄存器/FIFO/GB 对象（9-29 → 10-06）
- SVGA3D LIVE（DEVCAP_3D=1）；GB 对象模型 UP（MOB/Surface/OTable）。
- DX 命令集 OPEN：DXCONTEXT/SM41/GL43 devcap 全 1，DX context 创建成功。
- 关键坑：`words[]` 溢出（≥12 dwords）、命令 size 必须精确、OTable 条目先于 DEFINE 写。

### M6：STDU 显示路径全通（10-06 23:39，b89c619）**（结论待复核，见 §47.4）**
- 当时记录：锁屏完整实时渲染——FIFO 所有权 + GB/DX 栈 + 显示共存。
- 四要素：①拓扑寄存器 `SVGA_REG_DISPLAY_*`(35-40)；②`UPDATE_GB_IMAGE`(1101)；
  ③3MB MOB 用 PT64_2；④surface 加 `SVGA3D_SURFACE_SCREENTARGET`(1<<16)。
- 呈现循环 30Hz：memcpy(VRAM→surface MOB) + UPDATE_GB_IMAGE + UPDATE_GB_SCREENTARGET。
- ⚠️ 11-12：四要素全部复现仍全黑，且 §47.2 的洋红对照臂证明宿主不采样 surface。
  M6 那次的画面来源未确定（可能是宿主自己的 legacy 扫描输出）。**不要再把 M6 当作
  "STDU 可用"的证据引用。**

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

## 2026-10-29 晚更新（`DEFINE_RTV` 的变量已穷尽，判定宿主侧硬限制）

详见 `cb-findings` §34 / §35。§33 起连续三轮把此前的中间结论逐一撤回，
最终形态是：

**`cbContext` 不是"哪条队列运这个 CB"，而是"这个 DX context 绑在哪条队列上"。**
队列 1 是唯一能承载 DX 的队列；`DEFINE_CONTEXT` 只有在 cid 与队列匹配时才
ACCEPTED（cid 1 在队列 1 上成功，同样的命令在 3/4/5 上首次发包即 STATUS 4）。
由此推论：**一次启动 = 一个新 cid = 一次 `DEFINE_RTV` 的机会。**

STATUS 3 / 4 的分工：
- STATUS 4 = 该 cbContext 从未承载过 DX context（header 层，还没看命令体）；
- STATUS 3 + offset 0 = 队列有资格，但命令针对的 cid 在当前状态下不允许做这件事。

**§35（10-29 22:23）：最后一个干净格子也判负。**
cid 1 在队列 1 上 `DEFINE/BIND/SET_COTABLE` 全部 ACCEPTED、sync COMPLETED 之后，
唯一的 `DEFINE_RTV` 只改 `format` 为 `2 (A8R8G8B8)`，仍得
`STATUS 3 (errorOffset 0)`，`hdr[5]=0x24` 说明 36 字节载荷被完整收下才判错。

变量表（均在"cid 在队列 1、DEFINE_CONTEXT 刚 ACCEPTED"前提下排除）：
`format` 0/1/2、`resourceDimension` 0/1/3、`cbContext` 1..5/0x3f、
`validSizeInBytes` 0/128/4096、`surfaceFlags` 有无 `BIND_RENDER_TARGET`、
字段顺序/`cmdSize`/`desc`、单发 vs 批量、`dxContext` 字段。

> §65更正：以下为历史判决，不能外推为DX渲染不可达。旧测试遗漏typed视图格式及surface的ms/mobPitch约束；现在经COTable懒加载已成功绑定RTV、清屏并读回绿色像素。显式DEFINE_RTV需另行复测。

当时判定：**`DEFINE_RTV` 在本宿主上无法成功**，§29 起的这条主线判定不可达。
`cb-findings` §30.2 / §31.3 / §32.2 三条中间结论已撤回，不要引用。

## 当前开放问题

> 最后更新：2026-11-16（§75：GLD framebuffer每插件句柄的创建/load/unbind/destroy/清零生命周期已由调用点确认；FBO descriptor/尺寸字段生产者、texture backing/pitch与Metal type-5服务端及后续队列协议仍未知。§74恢复34项renderer属性映射、像素格式token流和clear/read/finish调用链；CGL结构探针1936项通过；§73 accelerator关联与29帧/1,900,544像素私有回归通过。当前无自研GLD插件或标准Metal设备，硬件OpenGL/Metal仍未实现）

### 两级目标的最终状态（11-15 夜间修订）

| 级 | 目标 | 状态 | 依据 |
|---|---|---|---|
| L1 | SVGA3D 的 **DX 命令流真正执行出像素** | 清屏与VS/PS三角形读回通过 | §68c有效blend/depth对象消除DRAW拒绝；红18432、绿47104、other0、mismatches0，全65536像素吻合；读回后队列健康。§70定长用户顶点→真实VB/透传VS通过，A/B/固定交替与连续更新零差异；通用资源提交、回收及图形API接入仍待实现 |
| L1' | 退一步：**像素通路可用**（2D），且由驱动自证 | ✅ 已交付 | §52 色带/白框/棋盘截图 + 登录界面截图 |
| L2a | **2D 加速的显示驱动**（copy/blit 加速） | ✅ 可达，且下层已实测 | §54.2：`RECT_COPY` 逐位正确；blit 可用；fill 不可用 |
| L2b | **Metal / IOAccelerator** | 服务关联及私有绘制通过；标准Metal未实现 | §73真实IOAccelerator派生服务可发现，转发已有GPU绘制并全像素验证；§75恢复type-5客户端初始化调用方的selector 9/2/0/7及输出容量，但服务端结构、设备资源/队列协议和Metal插件信任链未恢复；type5仍Unsupported、Metal仍零设备 |
| L2c | **OpenGL（软件渲染 GL 2.1）** | ✅ 可用 | §55.3 `Apple Software Renderer`，FBO 三角形读回 255,0,0,255。系统硬件OpenGL仍未实现；§75仅闭合部分GLD FBO句柄生命周期，descriptor生产者、texture backing及bundle信任加载仍阻塞 |

> §65纠正：下层已有DX渲染目标清屏和像素读回证据；三角形光栅化、shader、资源上传及macOS应用级接口仍待实现。
> `MTLCopyAllDevices == 0` 只说明当前无可用Metal驱动，不能证明自研驱动不可实现；不再将换宿主列为唯一前提。

### P0'：显示已经可用，接下来（11-12 起）
1. **分辨率锁在 1024x768**：VRAM 只有 4 MB（`gfbSize` 写在 `vmware-vmx.exe 0x14050aa98`），
   1920x1080 要 8.29 MB。要更高就必须再打一次宿主补丁（流程见 §19 的 1 字节补丁）。
   **11-13 补注**：Windows 客人日志 `WDDM VRAM 4096KB` 说明 4 MB 是 VMX 全局限制，
   不是 macOS 路径特有。
2. **颜色格式未逐像素校验**：GMRFB 用 `BGRX32`，肉眼看正确，但没有色卡回归。
3. **脏矩形**：现在每 33 ms 全屏 blit；有 damage 来源后再优化。
4. **3D 仍不通**：见下面的 P0-3D。

### P0-3D：DX 加速（11-13 重新打开，用户目标改为「完整支持 DX3D 加速」）

**前提未丢**：§19 的 1 字节宿主补丁（`x64\vmware-vmx.exe` 0x87293 `FE→FC`）**仍在位**，
备份 `vmware-vmx.exe.bak-dxflag` 也在。但 §22 已证明光放开 flags 不够。

**11-13 这一轮的净产出（详见 `cb-findings` §48）**：

| # | 结论 |
|---|---|
| 1 | **地址基线纠错**：`dev = [0x1413F31C0]`（891 引用），不是笔记里的 `0x1414931C0`（**0 引用**）。§28 关于 `[dev+0xad8]` 的"影子寄存器"结论撤回 |
| 2 | **tick 是活的**：`0x1404c5ee0` ← CFG thunk `0x1401565f0` ← `0x14014c278`（VMX 主事件循环忙轮询）。§23.1 的"0 callers"是扫描器盲区 |
| 3 | **四道闸门落到具体写点**：(A) `[dev+0x81d]` ← `SVGA_REG_ENABLE` bit0；(C) `[0x14140B043]` 开机即 1；(D) `[dev+0x9bc]` = 两个 DC 槽计数 > 0；(E) `[dev+0x71] == 0` |
| 4 | **`[dev+0xb4]` = `SVGA_REG_ENABLE` 影子**（写点 `0x140415cef`，日志 `SVGA hiding/enabling SVGA`）⇒ (A) 与 ENABLE 是同一条线，**客人可控** |
| 5 | ⚠️ **静态模型自相矛盾**：创建块 `0x140509fe8` 静态不可达（`r14b` 恒 0 + 无跳转指向），可 Windows 客人确实在跑 DX（`vm3d: WDDM DX10 context is on` + `Grow OTable`）。⇒ **不要再据此设计补丁实验** |
| 6 | **DX 命令在 vCPU 线程处理**（日志 `threadName="vcpu-6"`），主循环 tick 跑在 `vmx` 线程 ⇒ 原理上"tick 能看到在飞槽"成立 |

**下一步（先测再补）**：提权跑 `tools/vmxmem.py all`，读两台 VM 的 `[0x1414930A0]`
与 `dev` 各闸门字段。Windows 侧应当非空（校验理解），macOS 侧缺哪一项就补哪一项。
**未测之前不打新补丁。**

**11-13 已测（`vmxmem.py`，20 s / 397 采样，详见 §48.9）**：

- 两台 VM 的 (A)`b4=1`、(A')`81d=1`、(E)`71=0` **都是开的**；(C) 开机即 1。唯一关着的是 (D)`9bc=0`。
- Windows 侧 `9bc` 会短暂变 1（3/397），槽计数出现过 2/1 ⇒ (D) 机制为真。
- ⚠️ **`[0x1414930A0]` 在两个进程里 397 次全为 0**，包括在跑 DX 的 Windows VM
  ⇒ 「管理器 NULL ⇒ DX 不通」这条因果**站不住**，要么当时无 DX 负载，要么路径不看它。
- **唯一的实质差异**：槽标志 `ae0`/`bf8` Windows 是 1、macOS 是 0 ⇒ 我们的
  `START_STOP_CONTEXT` 在设备态上没留下痕迹。

**新的下一步（不需要宿主补丁）**：让 kext 正确发 `START_STOP_CONTEXT{enable=1,ctx=0}`，
再用 `vmxmem.py` 在宿主侧验证 `ae0`/`bf8` 是否变 1。这一步成立之前不要碰创建路径。

---

### P0-3D 历史关闭判决（11-14；已由§56、§65/§66撤回）

> 以下保留当时的调查记录，不代表当前状态。§66已验证DX清屏及全部65536像素读回，不能再据本节推断宿主无3D派发能力。

上面的宿主侧补丁路线**全部作废**，不需要再往 `vmware-vmx.exe` 上打任何补丁。理由不是
"又猜了一次"，而是客体侧把问题问死了。四条可复现的事实：

| # | 事实 | 来源 |
|---|---|---|
| 1 | **COTable 行定义 RTV 成立**：`SET_COTABLE {cid 1, mob 5, type 0( RTVIEW), validSize 4096}` → **ACCEPTED**（首次）。RTV 不该靠 1187 创建，vm3dmp 的 `.text` 里 1185–1199 一个立即数都没有 | §49 |
| 2 | **1187 是有害的**：它的 `STATUS 3` 会把 CB 队列打死，之后所有命令都变成 400 ms 超时。以前读到的"CLEAR 被拒"其实是读了条被 1187 杀掉的队列 | §49/§51 |
| 3 | **`dxContext` 假设已证伪**：给渲染命令带 `flags=DX_CONTEXT, dxContext=1` 之后 CB **头**通过（不再是 STATUS 4），说明宿主解析出了上下文；但 CLEAR 仍然 `COMMAND_ERROR / errorOffset 0` | §51.3 |
| 4 | **3D 派发器缺席**：`DX_SET_TOPOLOGY`(1160，4 字节 body、无表无视图) 在 byte 0 被拒；同一 CB 通路上的 **legacy `SVGA_3D_CMD_CLEAR`(1057) 也被拒** ⇒ 不是 DX 特有的问题，是这台宿主不派发任何 3D 渲染/状态命令 | §51.4 |

配置层也已到顶：宿主日志 `MKSRenderMain: PowerOn allowed ... DX12 DX11 DX11Basic VK`、
`Vulkan Renderer: Created with LUID 0x14cf3`、`SVGA3dCaps: guest, compatibility level: 10`；
把 `vmotion.svga.baseCapsLevel` 从 9 改成 11 会被 VMware 在 power-on 时写回 9（派生的键）。

**因此**：Level-2（Metal / IOAccelerator → SVGA3D）没有可依托的下层。它不是"还没写"，
而是"下层不给"。要重开这条线，必须先有一台 3D 命令真能派发的宿主。

### P0-2D 绘图引擎 ✅ 已关闭（11-14，`cb-findings` §53 + **§54 更正**）

3D 关闭之后，唯一还能当 Level-2 下层的是 2D 引擎，所以单独问了一次：
**宿主能不能替我们画东西？** 边界现在很清楚（判据全部来自 VRAM 回读，不靠截图）：

| 能力 | 结论 | 证据 |
|---|---|---|
| GMRFB → 屏幕（blit 写屏） | ✅ 可用（但**不必要**，见下） | §52 色带截图 |
| **`RECT_COPY`（矩形复制）** | ✅ **可用** | §54.2 `src 0xffffff00, dst 0xff00ff00 -> 0xffffff00`，与源像素逐位相同 |
| `FRONT_ROP_FILL`（纯色填充） | ❌ 四种 rop（CC/F0/00/FF）四个矩形，前后全部一致 | §54.2 |
| 屏幕 → GMRFB（回读） | ❌ 拿不到 fence、从不完成 | §53.2 |
| 3D 三角形 / 着色器 | §68c首次VS/PS光栅化全像素验证通过 | 有效blend/depth状态后DRAW/READBACK完成；红18432、绿47104、other0、mismatches0；§70专用IOUserClient用户顶点输入通过；三种几何39帧，连续VB更新、跨连接读回、重开和80种非法输入均验收；尚未接入OpenGL/Metal |

> ⚠️ **§56 撤回了本节对 3D 的判断**。「派发器缺席」来自一个线格式错位的批次：
> 四条命令里三条 `size` 与结构体不符，而宿主只按 `size` 跳、不校验，于是错位的是**下一条**
> 而不是本条，errorOffset 永远报 0。同一个测试只改线格式，表现就从「逐条 STATUS 3」变成
> 「头状态根本不写」—— 而当时的判据把 `status != 1` 一律当拒绝，130 ms 自旋也不足以区分
> 「慢」和「没有」。详见 §56.1–56.3。

> ⚠️ **§53 曾判"copy 也不执行"，那是错的**：命令线格式多塞了一个长度字，
> `RECT_COPY` 被解析成 `{24,100,100,600,400,200}` 并**正常执行**了 —— 证据就在
> §53 那张截图里（色带边界在 y≥610 处整体 +76）。§54.1 记下了这次误判。

#### 附带校正：显示**不需要** screen object（§54.3）

`kProbe2D = 2` 那一轮在 `DEFINE_SCREEN` 之前就返回：没有 screen object、没有 GMRFB、
没有 blit、一个 present tick 都没有 —— 屏幕上是**完整的 macOS 登录界面**。
所以 §47 的因果归因要改：**宿主直接扫描输出 BAR1 上 `FB_OFFSET` 处按
`BYTES_PER_LINE` 排布的内存**，screen object 与 33 ms 全屏 blit 从来不是前提。

#### 出货配置已简化（§54.4）

新增 `kSkipScreenObject = 1`：不发 `DEFINE_SCREEN`/`DEFINE_GMRFB`/`BLIT`，不再有
present 定时器（心跳仍在），驱动退化成一台纯 framebuffer。验证截图
`docs/shots/2026-11-15-plain-fb-login.png`：登录界面完整、无失真。

### P0：像素通路 ✅ 已关闭（11-14 补强，见 `cb-findings` §52）

除 §47 的 2D 桌面之外，现在还有一份**驱动自绘**的像素证据：`kPaintTestPattern = 1` 时
`presentSetup()` 在首次 blit 前往 VRAM 画 8 条竖色带 + 白框 + 顶部红色 pitch 探针 +
左上棋盘。截图 `docs/shots/2026-11-14-test-pattern.png` 里色带完全垂直，证明
`BYTES_PER_LINE = 4096` 与宿主一致；十几秒后 WindowServer 把它覆盖成登录界面
（`docs/shots/2026-11-14-login-screen.png`），反证 WindowServer 正在用我们的 framebuffer 合成。

### P0：像素通路 ✅ 已关闭（11-12，见 M8 / `cb-findings` §47）

### P0：像素通路 ✅ 已关闭（11-12，见 M8 / `cb-findings` §47）

> 结论：2D 像素通路**已打通**，但走的不是 STDU。`kFbStage=5 + kGbStage=0`
> （DEFINE_SCREEN + DEFINE_GMRFB + BLIT_GMRFB_TO_SCREEN）实测出现完整可用桌面。
> 下面保留的是当时的推理过程，其中"STDU 呈现链是健康的"这一句已被 §47.2 推翻——
> 宿主根本不采样 bind 过去的 screen target。

### P0-旧记录：回到 2D 路径（§35.5 的设想已作废）
§35.5 曾提出"用 `SVGA_3D_CMD_DX_DEFINE_GB_SURFACE` 重建 sid"来隔离"跨路径引用"。
**这条实验在动手前被 §36 证伪，作废：**

- `svga3d_cmd.h` 的 `SVGA_3D_CMD_DX_MIN(1143)…DX_MAX(1227)` 区间里
  **根本没有 DX 前缀的 surface 定义命令**；GB surface 只有 V1/V2/V3/V4 四态，
  且我们的标志位（最高 `1<<24`）完全落在 V1 的 32 位空间内。
- vmwgfx 把 `DEFINE_GB_SURFACE` / `_V2` / `_V4` 全标为 `vmw_cmd_invalid` ——
  它本来就不经 CB，走 legacy FIFO。而 M6 那版**屏幕完整实时渲染**的代码
  正是这么做的。⇒ "legacy 定义、DX 引用"不是异常，是已验证的正常路径。

**真正的下一步：把 2D 通路打开，而不是继续找 DX 的变量。**

- 2D 出像素通路 = M6 已经验证过的东西，代码仍在：
  `memcpy(VRAM → surface MOB)` → `SURFACE_DMA(1044)` → `UPDATE_GB_IMAGE(1101)`
  → `UPDATE_GB_SCREENTARGET`，30 Hz；surface 带 `SCREENTARGET(1<<16)`、MOB 用 `PT64_2`。
- 当前 `kGbStage = 4`，STDU 关闭（启动日志里没有 `stdu:` 行）。提到 5 即可恢复。
- 待确认：恢复后显示是否正常、像素是否正确；若 M6 之后引入的问题复现，再逐项定位。
- 预算提醒：**一条 DX 错误 = 一次重启**；DX 主线已判定不可达，不要再往里投实验。

**进展（10-29 22:40 boot，`cb-findings` §36.5）：STDU 已恢复，通路健康，但呈现是黑的。**
- `stdu: screen target 0 (1024x768) bound to surface 2 on MOB 2 -- STDU present path is UP`
- `present tick 1 (next 1644 stop 1644)` → `present tick 300 (next 48288 stop 48288)`
  —— 每帧 `SURFACE_DMA` / `UPDATE_GB_IMAGE` / `UPDATE_GB_SCREENTARGET` 都被宿主消费，
  **FIFO 没有楔死**（这是此前没有的证据：呈现循环本身是健康的）。
- VNC 截图 1024x768，采样 12288 点，**distinct colors = 1，全 (0,0,0)**。
- 拓扑 35–40 在 CONFIG_DONE 前后各写一遍，`kGuestDriverId` 在 CONFIG_DONE 前写，
  M6 四要素齐备 —— 所以不是"少写了哪一步"。

⇒ 命令通道、数据搬运、屏靶绑定全部正常，卡的是**内容的来源**。下一步改成确定性实验：
**主动往 surface 写已知像素，看屏上能不能出现**，而不是等宿主往 VRAM 投递内容
（guest 侧没有窗口系统在画图时，VRAM 本来就是黑的，黑屏可能是"正确"的）。

### P0-旧：`DEFINE_RTV` 的 COMMAND_ERROR（已由 §35 判定，保留作历史）
- 已排除：CB header 合法性、命令通道、MOB 校验、desc 排列、`format`/`dim`、
  COTable 尺寸、surfaceFlags、队列选择。
- 曾定位到：`0x1414930A0`（DX 管理器）从未被创建，唯一创建点 `0x14050a077`
  被 `[dev+0x9bc] != 0 && [dev+0x71] == 0` 门控，且只有时间片/抢占链会调用。
  注意：这条逆向结论**与 §35 的实测不冲突**——管理器没建起来，`DEFINE_RTV`
  自然被拒；而所有"从 guest 侧触发管理器创建"的尝试（HP queue、CB 形态、
  抢占时机）都没有成功。逆向结论保留，但它不再是"下一个动作"，因为从
  guest 侧够不到那个门控。


### P1：~~context-0 CB 里的 SVGA3D 命令解析失败~~（10-29 已解决）
- 原现象：DEFINE_GB_MOB64（context-0 CB 第一条）`COMMAND_ERROR at offset 0`。
- 实际原因：header 不合法（`dxContext != 0` 或 flags 非 bit0），命令从未被解析。
  改成 `dxContext=0 + flags=1 + PA` 后，DEFINE/BIND/SET_COTABLE 全部 ACCEPTED（§26）。

### P2：DX 激活之外的验证
- CLEAR → SURFACE_DMA(READ_HOST_VRAM) → 客体校验品红。DX 路径已判定不可达（§35），
  改走 P0 的 2D 路径做同样的验证。

### P3：STDU 显示的 fresh-MKS 冻结
- 拓扑写位置矩阵已完整测绘（fresh MKS 上 topo 写在 CONFIG_DONE 后任何位置都会楔死宿主）。
- 唯一成功组合 = 10-06 23:39 的长寿 MKS。方向：CB 打通后 STDU 命令也走 CB，可能绕开
  legacy FIFO 的拓扑竞态。

## 实验开关（`vgpuFramebuffer.cpp` 顶部）

| 配置 | 效果 |
|---|---|
| **`kFbStage=5 + kGbStage=0`（当前出货）** | **legacy screen-object 呈现，画面可用** |
| `kFbStage=0` | 只读 kext，宿主自己扫 VRAM，显示正常但驱动不参与 |
| `kFbStage=3 + kGbStage=5` | STDU 实验（宿主不采样 surface，屏幕必黑） |
| `kFbStage=2 + kGbStage=4 + kDxTest=1` | DX/CB 实验（headless，SSH 活；§68c先复核全像素清屏和PS生命周期，再由kDxDrawProbe执行三角形及区域读回） |
| `kStduEnableHide=1` | 写 `ENABLE\|HIDE` → 宿主 `SVGA hiding SVGA`、建 Screen 0(640x480) |

11-12 新增（§47）：

| 开关 | 值 | 含义 |
|---|---|---|
| `kStduEnableHide` | 0 | 但**两条分支都写 ENABLE**：开=3(HIDE)，关=1。设备状态跨重启存活 |
| `kStduVramControl` | 0 | 洋红对照臂：STDU 臂往 VRAM 灌洋红、legacy 臂启动时灌一次 |
| `kStduMatchHost` | 0 | 1 = surface 跟宿主 Screen 0 对齐成 640x480（已证伪，仍全黑） |

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
