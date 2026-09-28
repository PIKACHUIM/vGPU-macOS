# 黑苹果 GPU 加速项目方案书

**版本** v0.4（草案，待评审） · **日期** 2026-09-27
**范围** Windows / Linux 宿主上的 macOS 客体图形加速；不含已受 macOS 官方支持的硬件

---

## 0. 摘要

### 0.1 版本差异

| 项 | 早期结论 | 本次修正 |
|---|---|---|
| OpenCore PR #600 | 未核实，判断"与主线无关" | **已核实**：它是「引导期注入」路线的**前置条件**，对轨道 B 直接相关（§1.4.0） |
| 路线归属 | 把"VMware 逆向驱动""Hyper-V 注册虚拟设备"当作路线 A | **两者本质都是路线 B**。真正的路线 A 只有 QEMU 系（§1.2） |
| 路线 B 成本 | "单位成本 = 一个 GPU 型号 × 一个 macOS 版本" | **对物理 GPU 成立，对虚拟 GPU 设备不成立**；设备后端可复用（§1.2.3） |
| VMware 判定 | 不可行，建议放弃 | **不可行的是"不写驱动"，不是"加速"**；作为路线 B 的**首选设备后端**（§2.4） |
| Hyper-V 判定 | 不可行（理由：ExHyperV 不是虚拟总线方案，GPU-PV 是 Windows 客体专属） | **前提更正**：macOS **确有** VMBus 客户端驱动（`MacHyperVSupport`）；但可用的 3D 通路仍为零，**判定不变**（§1.4.4） |
| B0 的做法 | 从零证明"能否注册第三方图形加速器" | 改用 `MacHyperVSupport` 的图形 kext 骨架作起点，风险与工作量显著下降（§3.2） |
| 设备来源 | 隐含假设"虚拟 GPU 设备必须由 VMM 提供" | **已被源码推翻**：客体内核可凭空伪造 PCI 设备 → 引出假设 **H1**（§1.4.6） |
| Hyper-V 的 3D | "synthetic graphics 没有 3D 命令流"（推断） | **源码级确证**：该 VMBus 协议全部消息仅 11 条，无一条与 3D 相关（§1.4.4） |
| Intel 核显 | 需拆 R3a / R3b | 同前，并补充：Intel 有**公开 PRM + 开源 Mesa 实现**，是原生驱动实验的最优第一站（§1.4.3） |

### 0.2 结论

需求要落成工程，必须先接受一个二分：**客体靠谁来理解那块 GPU。**

- **路线 A —— 客体用 Apple 现成驱动。** 客体侧不写任何驱动，直接使用系统自带的 `AppleParavirtGPU.kext`。代价是：主机侧必须实现 **Apple 的**设备协议，并且必须以 macOS 能识别的 PCI / `vmapple` 设备形式出现 → **必须能改 VMM 的设备模型** → **只有 QEMU 系**。
- **路线 B —— 客体用你自己写的驱动。** 设备是什么协议都行（SVGA3D / VMBus / virtio / 物理 GPU 都算），但你必须交付一个 macOS 图形加速驱动。代价高，但**对所有 VMM 开放**，包括 VMware 和 Hyper-V。

两条路线的覆盖率（这是本方案的核心表）：

| VMM | 路线 A（不写驱动） | 路线 B（写自己的驱动） |
|---|---|---|
| QEMU / KVM / WHPX / HVF | ✔ 已有实现（reims） | ✔ 可行 |
| **VMware Workstation / Fusion** | ✗ | **✔ 设备现成（SVGA3D），无需改 VMM** |
| Hyper-V | ✗ | ✗ | △ 客体侧 VMBus + VSC 已有（MacHyperVSupport），但**宿主侧无法注册新合成设备**，且既有设备中无 3D 通路（§1.4.4） |
| VirtualBox | ✗ | △ 要改其自有 3D 管线 |
| ESXi / Proxmox | ✗ | △ 同 Hyper-V 一类的自建设备问题 |

**一句话结论**：你列的"VMware 逆向驱动方案"和"Hyper-V 注册虚拟设备"**都属于路线 B**；"qemu/pve 那个方案"才是路线 A。把 A1/A2 归到路线 A 会让人以为"每个 VMM 各做一套、成本线性叠加"，而实际上——**路线 B 里最贵的那层（Metal 驱动接口）只写一次**，见 §1.2.3。

---

## 1. 需求评估

### 1.1 需求拆解

| 编号 | 命题 | 实际归属 | 备注 |
|---|---|---|---|
| A1 | VMware：给客体一个能调用宿主 GPU 的设备，逆向其 Windows 驱动 | **路线 B** | 设备已存在（VEN_15AD:DEV_0405），缺的是 macOS 驱动 |
| A2 | Hyper-V：注册虚拟设备，逻辑复用 A1 | **路线 B** | 客体侧 VMBus/VSC 已存在（MacHyperVSupport）；障碍在**宿主侧无法注册新的合成设备 class GUID** |
| A3 | VirtualBox | **路线 B** | 除非能换掉 VBox 的宿主渲染 API |
| A4 | QEMU / PVE | **路线 A** | 唯一真正走 Apple paravirt 通道的一支 |
| B1 | NVIDIA 5060 / 2080Ti 原生驱动；AMD 方向跟进 | 路线 B | 已有先例（null-moth），物理 GPU |
| B2 | Intel 核显原生驱动（长期目标） | 路线 B | 无人做过，文档条件最好 |
| B3 | Hyper-V DDA / GPU-PV | **建议删除** | 见 §1.4.4——对"支持任意显卡"零增量 |

### 1.2 核心技术约束

#### 1.2.1 为什么只有两条路

macOS 把渲染交给 GPU，只有两种可能的消费方式：

1. **客体侧有 Apple 的驱动**（`AppleParavirtGPU.kext`、`AMDRadeonX*`、`AppleIntel*Graphics`、`AGXAccelerator` 等）。
   这些驱动**只认特定的设备身份**——PCI vendor/device id，或 `vmapple` 平台设备。
2. **客体侧有你自己的驱动**。你必须注册成一个 macOS 图形加速器，并实现 Metal 驱动侧的私有接口。

没有第三种：macOS 的 WindowServer / SkyLight / CoreDisplay 全都直连内核里的加速器，不存在"纯用户态 GPU 后端"这种可能（用户态只能挂到内核已注册的加速器上）。

#### 1.2.2 路线 A 的硬条件

Apple 的 paravirt 设备身份是固定的（PG_PCI_VENDOR_ID / PG_PCI_DEVICE_ID，或 `vmapple` 平台设备）。因此：

- 你必须能让 VMM **对外呈现这个设备** → 必须能扩展 VMM 的设备模型
- QEMU 开源 → 可以（reims 就是 QEMU 设备模型）
- VMware / Hyper-V / VirtualBox 都无法往里加一个任意的 PCI 设备
  - Hyper-V 有 VPCI，但它的定位是 SR-IOV / DDA，不对外开注册接口
  - VirtualBox 开源，理论上可以加设备，但它的宿主侧 3D 走自己的 DX 管线，要接到 Vulkan 等于重写

**→ 路线 A 的可行集合 = {QEMU 系}，这是封闭的。**

#### 1.2.3 路线 B 的成本结构（本方案最重要的结论）

路线 B 可以分成两层：

```
┌─ 上层：macOS 图形驱动接口（与设备无关）──────────────┐
│  · IOAccelerator / IOAcceleratorFamily2 注册         │
│  · Metal 驱动插件（私有 MTLDriver 面）                │
│  · 显示/帧缓冲管理、AGPM 交互、资源与 fence 管理       │
└──────────────────────┬───────────────────────────────┘
                       │  这一层只写一次
┌─ 下层：设备后端（可替换）────────────────────────────┐
│  · SVGA3D 后端（VMware）                             │
│  · GSP 后端（NVIDIA，null-moth 已做）                 │
│  · VMBus 后端（Hyper-V：宿主侧无注册接口，暂不可行）   │
│  · virtio-gpu 后端（QEMU 的路线 B 变体）              │
│  · Intel Gen12+ 后端（长期目标）                      │
└──────────────────────────────────────────────────────┘
```

**推论：**

- 对**物理 GPU**（NVIDIA / AMD / Intel），成本确实 ∝ GPU 型号数——因为每个 GPU 的命令流、固件、ISA 都不同。
- 对**虚拟 GPU 设备**（SVGA3D / VMBus / virtio-gpu），成本**与宿主装什么显卡无关**。一份 SVGA3D 后端，覆盖所有 VMware 宿主、任意宿主显卡、任意宿主 OS。
- **这正是 VMware 路线真正的价值所在**，而且它在覆盖面上一度超过路线 A：路线 A 被锁死在 QEMU 上；一份成熟的 SVGA3D 驱动能同时给出"任意显卡 + Windows/Linux/macOS 三种宿主 + 不需要容器式改 VMM"。

### 1.3 VMM / 宿主可行性矩阵（含依赖链）

| VMM | 设备模型可扩展 | 路线 A | 路线 B | 路线 B 的前置依赖 |
|---|---|---|---|---|
| QEMU (KVM/WHPX/HVF) | ✔ | ✔ 已有实现 | ✔ | 无 |
| VMware Workstation (Win/Linux 宿主) | ✗（但设备现成） | ✗ | ✔ | 只需自写 SVGA3D 驱动 |
| VMware Fusion (macOS 宿主) | 同上 | ✗ | ✔ | 同上 |
| Hyper-V | ✗ | ✗ | △ | **先写 macOS VMBus 客户端驱动** |
| VirtualBox | △ 开源但 3D 自有管线 | ✗ | △ | 改 VBox 3D 管线 |
| ESXi / Proxmox | ✗ | ✗ | △ | 自建设备 + macOS 驱动 |

### 1.4 外部依赖核查

#### 1.4.0 OpenCore PR #600（已核实）

**基本信息**（来自 GitHub API，核查时间 2026-09-27）

| 项 | 值 |
|---|---|
| 标题 | `OcAppleKernelLib: System KC loading and cross-KC dependency resolution` |
| 作者 / 分支 | MattJackson / `system-kc-loading` → `acidanthera:master` |
| 状态 | **open（未合并）**，mergeable / clean |
| 时间 | 创建 2026-04-20，最后更新 2026-05-10 |
| 规模 | +665 / −31，6 个文件，6 个 commit，7 条评论 |

**它做什么**

给 `OcAppleKernelLib` 增加**可选的 System Kernel Collection（System KC）加载**，并接到 `OcMainLib`。目的是让 OpenCore 在 prelink 阶段，能把**注入到 Boot KC 的 kext 的 `OSBundleLibraries` 依赖，解析到 System KC 里的类**——PR 描述里点名的例子就是 `IOGraphicsFamily`、`IOUSBFamily`、`AGPM`。

**为什么需要**

macOS 11 起，原本在 prelinkedkernel 里的大量 kext 库被搬进了 System KC。而 OpenCore **只注入 Boot KC**，于是任何 `OSBundleLibraries` 指向 System-KC 类的注入 kext，都会在 prelink 阶段直接失败：

```
library kext ... not found
```

**它怎么做的**（这才是你说的"重建内存地址"）

把 `EFI/OC/SystemKernelExtensions.kc` 放到 ESP，加载后遍历 chained fixups（x86_64 与 arm64e 两种指针格式），**重算 System KC 的 KASLR slide 并把注入 kext 的绑定翻译到实际 VA**：

```
OC:  System KC loaded from EFI partition (365821088 bytes)
OCAK: System KC ready (365821088 bytes, 81247 chained fixups)
OCAK: System KC resolved com.apple.iokit.IOGraphicsFamily (VA 0xffffff8149b20000, 1590 fixups)
OCAK: System KC resolved com.apple.driver.AppleGraphicsDeviceControl (VA 0xffffff8149c80000, 56 fixups)
OC:  Prelinked status - Success
```

**关键澄清**：对象是 **System KC（内核态 Mach-O）**，不是 **dyld shared cache（用户态）**。两者都是"预重定位 + chained fixups"，但完全不同的数据结构，不要混。另外它**没有改变注入时机**——OpenCore 依然在 ExitBootServices **之前**把 Boot KC 预链接好交给 boot.efi；PR 做的是**符号解析**，不是"EB 之后注入内核"。

**它没解决什么**（作者自己在 PR 里列的边界）

- opt-in，且**版本锁定**：staged `.kc` 必须匹配所复制的 macOS 构建；**当前 patch 不校验 build UUID**
- 升级后失效；Recovery 用自己的 KC（ESP 那份必然不对）；安装/升级进行中失效；一 ESP 多系统（N 个 KC）最难处理
- 需要在 ESP 上放一个 **约 366 MB** 的文件
- **arm64e 指针格式路径只在编译层验证过，作者没有 Apple Silicon 实机做过真实引导测试**
- 依赖 PR #603 / #606（`KcWalkChainedFixupsInImage`）；本 PR 自带一份临时 walker，等 #606 合并后重构
- 维护者讨论过把它拆小（选项 B/C），**目前悬而未决**

**与本项目的关系（修正 v0.1）**

- 对**轨道 A 无关**：轨道 A 不注入任何 kext。
- 对**轨道 B 是前置条件**——这一点你的直觉是对的，我上一轮说"与主线无关"只对 A 轨成立。

但要补一个关键区分，**你要走的到底是哪种注入**：

| 方式 | 依赖 PR #600 | 代价 |
|---|---|---|
| **引导期注入**（OpenCore 把 kext 注入 Boot KC） | **需要** | 需在 ESP 放 366 MB 的 KC 且版本锁定；好处是不动系统卷、易回滚 |
| **系统内注入**（改根卷 + `kmutil` 重建 AuxKC，即 OCLP 路线） | 不需要 | 需要 KDK、每次系统更新重打、要动系统卷 |

也就是说：**PR #600 是"引导期注入"这条路的前置条件；如果你接受 OCLP 式改根卷，可以绕开它。** 这决定了轨道 B 的地基选择，建议在 B0 spike 里一并实测。

#### 1.4.1 "2080Ti / 5060 黑苹果驱动"——两件不同的事

- NVIDIA Turing（2080Ti）与 Blackwell（5060）在 macOS 上**从无官方驱动**；Maxwell/Pascal 的 Web Driver 止于 High Sierra，Turing 及以后从未有过。
- 传闻实际指向两件不同的事：
  - **(a) 轨道 A**：hackintosh-forum 上那位报告者是在 **Linux 宿主 + 2080 Ti** 上跑起了 Sonoma。那不是驱动，是虚拟机里的命令流转译层。
  - **(b) 轨道 B**：**null-moth 的 RTX 5060 原生驱动栈**（macOS Sequoia 15.7）。这是真驱动：内核扩展（跑 GSP 处理器固件、分发中断）+ 显示管理模块（含帧缓冲）+ 低层 userspace 驱动 + **Metal 插件**；另有一个内建转译器，把 Apple 的内部格式即时编译成 Blackwell 机器码（不修改可执行文件）。已知状态：桌面（WindowServer / Dock / Finder）由该卡渲染；Core Image、Vision、MPS 稳定；Metal 硬件光追与《No Man's Sky》已验证；同帧内 6.5 GB 纹理 + 40000 个活跃 buffer；用 40 项自动化基准对 AMD 硬件比对过计算精度。
  - 已知缺口：**无硬件编解码**（媒体全程 CPU）、MPSGraph 部分任务会崩系统、多屏与睡眠未测。**仅覆盖一张卡、一个系统版本**，目前只有二手报道、未找到公开仓库。
- **意义**：它把"门 B 是死路"改写成"门 B 可行，成本 = 上层一次性 + 下层每设备一份"（§1.2.3）。

#### 1.4.2 VMware：不可行的不是"加速"，而是"不写驱动"

- 官方立场：3D 加速**不支持 macOS 客体**（KB 78592；KB 2007143 只把 3D 加速保留给 x86_64 的 Linux/Windows 客体）。
- 但这只是"**当前没有 macOS 驱动**"的同义反复——SVGA3D 设备本身**就是**一个"把 3D 命令转发给宿主 GPU"的代理设备，宿主侧能力真实存在。
- **SVGA3D 的关键事实（决定了这条路的生死）**：
  - 协议**开源**：Mesa 的 `svga` gallium 驱动 + Linux 侧 `vmwgfx`、`libdrm`，两边都是公开源码 → 等于一份免费的协议规范与参考实现。
  - 功能集：Workstation 17 / Fusion 13 + VM 硬件版本 20 + VGPU10 → **OpenGL 4.3**。而 GL 4.3 正是引入 **compute shader** 的版本 → **Metal 必需的 compute 有落点，生死判据通过。**
  - **先例**：SoftGPU 项目曾为 Windows 9x 自写 SVGA3D 客体驱动（vGPU9 / vGPU10），证明"自写 SVGA3D 客体驱动"是被走通过的路。
  - Windows 侧 VMware 的 3D 驱动可作为逆向参考，但**优先看 Mesa**——那是源码级的。
  - 短板：GL 4.3 是 2012 年的功能集。Metal 3 的许多特性（mesh shader、argument buffer、tile memory、光追）在 SVGA3D 上没有对应，需逐项降级或模拟 → **必须先定"Metal 特性等级目标"**。
  - **新增工作项**：VGPU10 的着色器格式是 DXBC/DXIL 家族，不是 SPIR-V。所以着色器链路从"AIR → SPIR-V"变成"**AIR → SPIR-V → DXBC/DXIL**"（或直接 AIR → DXBC）。这一段**不能直接复用 metal2vulkan 的产物**，需要专项验证；但 Mesa 的 `svga` 里已有 DXBC 发射代码（MIT 许可），可直接移植，避免从零写编译器。

#### 1.4.3 Intel 核显：天花板在 Ice Lake，但它是原生驱动实验的最优第一站

| CPU 代 | 核显 | macOS 支持 |
|---|---|---|
| 8/9/10 代 | UHD 620/630、Iris Plus 645/655 | ✔ 完整（10.13.6 起） |
| 10 代 Ice Lake | Iris Plus G4 / G7 | ✔（10.15.4 起，需 `-igfxcdc -igfxdvmt`） |
| 10 代 Ice Lake | UHD Graphics (G1, GT1) | ✗ |
| **11 代 Tiger/Rocket Lake 及以后** | UHD / Iris Xe / Arc（全部 Xe 架构） | **✗ No drivers available** |

Dortania、OpenCore 安装指南、黑苹果星球三处表述一致：Xe 架构核显在任何 macOS 版本上都没有驱动。

**但 Intel 有一个别的厂商没有的优势**：Intel Graphics Programmer's Reference Manual（PRM）是**公开发布**的，命令流与 ISA 有文档；Mesa 的 `anv` / `i915` / `iris` 是完整开源实现。相比之下 NVIDIA 的 GSP 固件闭源（null-moth 得先让 GSP 跑起来再自建转译器）。

**→ 结论**：把 Intel 定为长期目标是合理的，但要认识到理由不是"Intel 简单"，而是"**Intel 的可参考资料最多**"。如果要做第一个原生驱动实验，Intel 比 NVIDIA 更适合当起点。

**同时保留 R3a（不需要写驱动的那一半）**：把 Intel 11 代+ 核显当作**宿主侧 Vulkan 后端**（Linux 走 Mesa ANV，Windows 走 Intel 官方 Vulkan 驱动），macOS 客体走轨道 A。这条零架构改动，可以先落地。

#### 1.4.4 Hyper-V：VMBus 客户端是有的，但可用的 3D 通路仍然为零

**先更正一处错误**：早期草稿称"macOS 根本没有 VMBus 客户端驱动"，这是错的。
**`acidanthera/MacHyperVSupport`（作者 Goldfish64、vit9696）实现了完整的 VMBus 传输与一批 VSC**，并把 Hyper-V 的合成设备 class GUID 全部列成了文档（`Docs/HyperV-devices.md`）。

已实现的部分：

| 设备 / 服务 | class GUID | 实现模块 |
|---|---|---|
| Synthetic graphics（合成显卡框缓冲） | `da0a7802-e377-4aac-8e77-0558eb1073f8` | `HyperVGraphics`（+`HyperVFramebuffer` 提供完整功能） |
| Synthetic SCSI / IDE | `ba6163d9-…` / `32412632-…` | `HyperVStorage` |
| Synthetic network | `f8615163-df3e-46c5-913f-f2d2f965ed0e` | `HyperVNetwork` |
| Synthetic keyboard / mouse | `f912ad6d-…` / `cfa8b69e-…` | `HyperVKeyboard` / `HyperVMouse` |
| **PCI passthrough (DDA)** | `44c4f61d-4444-4400-9d52-802e27ede19f` | `HyperVPCIBridge` |
| Heartbeat / Shutdown / TimeSync / FileCopy / Guest services | 各自 GUID | 对应模块 + 三个用户态 daemon |

**但判定不变，而且理由比之前硬——能带来 3D 加速的候选设备一个都不成立：**

1. **Synthetic graphics** —— 已被驱动（2D 帧缓冲 + 脏矩形呈现），但该设备协议里**没有 3D 命令流**。加速无处可走。"点亮"和"加速"是两件事。
2. **GPU-PV** —— **不在这张 VMBus 设备表里**。它是 WDDM 2.4+ / Dxgkrnl 层面的分区机制：宿主把 GPU 驱动目录**整套注入客体**，客体侧得到的是 Dxgkrnl 的渲染设备，还需另配一个 Display Device。**要求客体有 WDDM 栈**，macOS 没有。宿主要求 Windows 11 / Server 2022+（build ≥ 22000），客体只能是 Windows。
3. **DDA / PCI passthrough** —— macOS 侧已经有 `HyperVPCIBridge` 客户端，所以直通在协议上是通的。但客体拿到的是一块**真实 PCI 设备**，仍需该卡的 macOS 原生驱动 → 回到轨道 B，且**对"支持任意显卡"零增量**（能直通的卡本来就能用）。

**宿主侧无法注册新的合成设备**：VMBus 的设备 offer 由虚拟机配置决定，每种合成设备对应**固定的 class GUID**（参见 Linux `drivers/hv` 对 offer / rescind 流程的说明）；而 Hyper-V 的合成设备目录是微软私有栈（VMMS / VMWP 与各 VSP）里的封闭集合，**没有对外注册新 class GUID 的接口**。

- 所以"用虚拟总线注册一个虚拟设备给 macOS"这条路，**在宿主侧没有着落**。
- 唯一"能自建 VSP"的开放栈是微软自家的 **OpenVMM / OpenHCL**（开源 Rust 虚拟化栈，重写了 VSP）——但那已经是"换一个 VMM"，等于回到 QEMU 那一类选择，不再是 Hyper-V。
- **ExHyperV（Micro-ATP/ExHyperV）不提供虚拟总线**：它是 GPU-PV / DDA 的配置 GUI，包装 `Set-VMGpuPartitionAdapter` / `Add-VMAssignableDevice` / `Mount-VMHostAssignableDevice` 这套 PowerShell，另加 MMIO 空间探测、驱动注入等细节。

**→ 判定：Hyper-V 保持"不可行"，B3（DDA / GPU-PV）继续建议删除。**
但 MacHyperVSupport 对轨道 B 有另一层重要价值，见 §3.2。

#### 1.4.5 VirtualBox

开源，但宿主侧 3D 走自有管线（VMSVGA + 宿主 GL/DX 后端）。要让它接 Vulkan 等于重写其 3D 子系统；即便做了，收益与 VMware 路线重叠而成本更高。**保持研究项，不排主线。**

#### 1.4.6 源码精读发现：客体内核可以凭空伪造 PCI 设备 ⭐

对 `MacHyperVSupport` 做源码精读（详见独立报告 `MacHyperVFramebuffer_源码精读报告.md`）后，推翻了一个我们一直隐含使用的假设：

> ~~要呈现一个 macOS 认得的 GPU，必须由 VMM 提供那个设备~~

**`HyperVGraphicsBridge` 证明客体内核可以自己造**。它做的事是：

1. 在合成 PCI 根桥 `HyperVPCIRoot` 上注册一个子桥（`registerChildPCIBridge()`，总线号从 0x10 起共 256 槽）
2. `bzero` 一块假配置空间，**手工填** `VendorID/DeviceID = 0x1414:0x5353`、`BAR0 = 帧缓冲基址`
3. 实现 8/16/32 位 `configRead/Write`，全部落到那块假空间；并在探测写 `0xFFFFFFFF` 时返回 BAR0 尺寸
4. `addBridgeMemoryRange(帧缓冲范围)`
5. 整个假设备由 VSC 提供（`IOProviderClass = HyperVGraphics`），只在 Gen2 启动

**→ 这个能力引出一个高价值假设 H1（详见报告 §11）：**

> 用同样的手法伪造一个 PCI 设备，填上 **Apple paravirt GPU 的 vendor/device ID**，
> 让**系统自带的 `AppleParavirtGPU.kext` 自己挂上去**。那么最难的 Metal 驱动层由 Apple 提供，
> 我们只需实现 paravirt 协议的对端（**reims 已经把这一层逆好了**）+ 一个设备后端。

如果 H1 成立，原方案的形态会发生实质变化：

| 项 | 原方案（路线 B） | 假设 H1 |
|---|---|---|
| Metal 驱动 / 私有 `MTLDriver` ABI | **必须自己写（最贵的一层）** | **由 Apple 的 `AppleParavirtGPU.kext` 提供** |
| paravirt 协议解码 | 不涉及 | 复用 `reims-vgpu-wire` + `runtime::decode`（已有开源实现） |
| 每设备后端 | 需要 | 仍需要（SVGA3D / VMBus / virtio-gpu） |
| 与 VMM 的关系 | 需要 VMM 允许加设备 | **不需要**（设备在客体内部伪造） |

**但 H1 是假设，不是结论。** 必须先在 B0 里回答 5 个问题：

1. `AppleParavirtGPU.kext` 的 `IOPCIMatch` 是什么？是否还要求 ACPI/_DSM 属性、特定 BAR 布局、或 `PGCopyOptionROMURL()` 的 option ROM？← **读它的 Info.plist 即可，成本最低、信息量最大**
2. 该 kext 在 x86_64 / arm64 各 macOS 版本上是否存在？在不在 System KC？
3. 时机与竞态：真设备存在时能否抢到（`IOProbeScore`）？不存在时能否独立启动到被 WindowServer 使用？
4. paravirt 协议的 `PGPhysicalMemoryRange` / trace range 监控是否隐含"这些页由 VMM 映射"？客体内部能否等价替代？
5. 正确性门槛：Apple 的驱动对协议合规性要求远高于"能点亮"，reims 的 decode 目前是 Alpha、覆盖不全——H1 成立后，问题会从"能不能跑"变成"要补多少个 opcode"。

**处置**：H1 登记为 B0 的**首要验证项**（第 1、2 问几乎零成本），在结论出来之前不下注于任何一条路线。

### 1.5 非功能需求

| 类别 | 要求 | 备注 |
|---|---|---|
| 性能 | 桌面交互可用（WindowServer 不卡顿）；2D 合成 ≥ 宿主等效机型 30 fps | 先定基线再谈目标 |
| 稳定性 | 连续 8 小时无 WindowServer 重启；睡眠/唤醒可用 | reims 与 null-moth 都未过这一关 |
| 兼容性 | 客体 macOS 13～26 逐 rail 验证；轨道 B 还需逐 macOS 版本重新适配私有 ABI | 轨道 B 的适配成本随版本走 |
| 可维护性 | 轨道 B 的上/下层解耦，新增设备后端不触碰 Metal 驱动层 | §1.2.3 的分层 |
| 可测试性 | 全部验证可无人工干预跑完 | reims 已有 snapshot-revert + 截图脚本 |
| 法律 | 见 §5 R1/R2/R10/R11 | 立项阶段书面确认 |

### 1.6 明确不做（Out of Scope）

- Hyper-V GPU-PV / DDA（§1.4.4）
- PCI 直通类方案（要求 macOS 原生已支持该卡，无增量）
- Apple Silicon 宿主的 macOS 客体（Virtualization.framework 已原生支持）
- 为 macOS 已支持的 AMD 显卡提供加速

---

## 2. 项目方案

### 2.1 双轨架构

**轨道 A（复用 reims，不写驱动）**

```
客体 macOS → AppleParavirtGPU.kext → PCI BAR / vmapple MMIO
   → QEMU + reims 设备模型 → decode（命令流 → Command/Kind）
   → metal2vulkan（AIR → SPIR-V）→ Vulkan ICD → 宿主 GPU
```

**轨道 B（自写 macOS 驱动，分层）**

```
客体 macOS
 └─ Metal / WindowServer
     └─ 【上层 · 一次性】自研 macOS 图形驱动
          · IOAccelerator 注册 + Metal 驱动插件
          · 显示 / 帧缓冲 / AGPM / 资源与 fence 管理
          · 着色器编译：Metal AIR → SPIR-V → DXBC/DXIL（直译到设备后端）
          └─ 【下层 · 每设备一份】设备后端（可插拔）
               ├─ SVGA3D（VMware）        ← 首选
               ├─ GSP（NVIDIA 物理卡）
               ├─ Intel Gen12+（长期目标）
               ├─ VMBus（Hyper-V，需先有 macOS VMBus 客户端）
               └─ virtio-gpu（QEMU 的 B 轨变体）
```

**两条轨道的共同资产**：`metal2vulkan` 的 AIR 解析与 SPIR-V 发射（轨道 B 的着色器前半段可复用）；reims 的 snapshot-revert 测试设施。

### 2.2 轨道 A 落地方案（沿用 v0.1）

| 项 | 内容 |
|---|---|
| 基线 | 复用 `steelbrain/reims-vgpu`（decode / wire / metal2vulkan） |
| Windows 宿主 | QEMU + WHPX；把 memfd/dma-buf 换成 Windows 等价物；重写显示呈现 |
| 后端矩阵 | NVIDIA / AMD / Intel 核显（R3a）/ Mesa 兜底 |
| 通道补齐 | video decoder / display / audio（reims 目前只接了 GPU 一条） |
| 性能 | pipeline cache；评估"少转一层" |

### 2.3 轨道 B 落地方案

**B0 · 风险闸门（最优先，独立于一切）**

在写任何设备后端之前，先证明两件事：

1. **能否在 macOS 上注册一个第三方图形加速器**，让 WindowServer 认到"有 GPU"，哪怕只驱动一个 2D 帧缓冲。
2. **Metal 驱动插件的接口面到底有多大**——把 `IOAccelerator` / `Metal` 侧需要实现的入口枚举出来，形成一份接口清单与覆盖率估计。

**现成的起点：`acidanthera/MacHyperVSupport`。** 它已经走通了轨道 B 最关键的一小步——在 macOS 客体里加载一个图形 kext 并被系统接受（`MacHyperVFramebuffer.kext` 提供 2D 帧缓冲）。做法调整为：**以它为骨架，把"帧缓冲"向上推进到"加速器"，而不是从零开始。**

产出：一份最小可运行驱动 + 接口清单 + 成本重估。**这道闸门不过，轨道 B 全部作废。**

**B1 · 地基选择（与 B0 并行）**

实测两种注入方式，选定其一（§1.4.0 的表格）：
- 引导期注入（OpenCore + PR #600）→ 若选此条，需评估在 ESP 上维护 366 MB 版本锁定 KC 的可行性，并注意 arm64e 路径未在实机验证过
- 系统内注入（改根卷 + `kmutil` 重建 AuxKC）→ OCLP 路线，需 KDK + 每次系统更新重打

**B2 · SVGA3D 后端（VMware）**

分三步走，每步都可独立验收：

| 步 | 目标 | 验收 |
|---|---|---|
| B2.1 | 2D / FIFO 通道打通 | 客体认到显示设备，分辨率可调 |
| B2.2 | 3D 命令子集 | 基本图元与纹理正确渲染，与 Mesa svga 输出比对一致 |
| B2.3 | compute + 系统框架 | `MTLCreateSystemDefaultDevice()` 非 nil；Core Image / MPS 基础用例通过 |

技术要点：
- 协议以 **Mesa `svga` + `vmwgfx`** 为规范（源码级）
- 着色器：移植 Mesa `svga` 的 DXBC 发射部分（MIT），避免从零写编译器
- 目标设备要求：Workstation 17 / Fusion 13 起，VM 硬件版本 ≥ 20，走 VGPU10
- 明确 **Metal 特性等级目标**（哪些特性降级、哪些用多 pass 模拟）

**B3 · 物理 GPU 后端（NVIDIA / Intel）**

复用 B2 的上层，替换设备后端。Intel 优先于 NVIDIA（资料公开度，§1.4.3）。

### 2.4 优先级建议

| 优先级 | 事项 | 理由 |
|---|---|---|
| P0 | 轨道 B 的 **B0 风险闸门** | 决定项目形态；不通过则整个 B 轨作废，越早知道越好 |
| P0 | 轨道 B 的 **B1 地基选择** | 决定 B 轨的维护模型 |
| P1 | **B2 SVGA3D 后端** | 覆盖面最大（任意宿主显卡 + 三种宿主 OS），设备现成，协议开源 |
| P1 | 轨道 A 的 Windows/WHPX bring-up | 覆盖 Windows 宿主的另一条路，且是已有实现 + 移植 |
| P2 | 轨道 A 的后端矩阵（含 Intel 核显 R3a） | 改动小、见效快 |
| P3 | 轨道 A 的通道补齐 | 工程量最大 |
| P4 | B3 物理 GPU 后端 | 依赖 B0/B2 |
| P5 | VirtualBox | 需改其 3D 管线，成本高；Hyper-V 已判定宿主侧无设备注册接口，不列入 |

> 注意：B0 的排序**高于** B2。在没确认上层接口可实现之前，不要投入设备后端。

---

## 3. 开发计划

### 3.1 里程碑总览

| 阶段 | 轨道 | 目标 | 出口准则 | 粗估 |
|---|---|---|---|---|
| B0 | B | 风险闸门：Metal 驱动接口可行性（以 `MacHyperVSupport` 骨架为起点） | WindowServer 走自研加速器；接口清单与覆盖率；两条注入路线对比结论；go/no-go | 4～8 人周 |
| B1 | B | 地基选择：注入方式实测 | 两种注入方式各跑通一次，产出取舍报告 | 3～5 人周 |
| A-P0 | A | 基线复现与度量 | 在自有硬件重现 reims 通路，采到基线 | 3～4 人周 |
| A-P1 | A | Windows 宿主 bring-up | Windows 上客体进桌面，A/B 设备对照可跑 | 8～12 人周 |
| B2 | B | SVGA3D 后端 | B2.1～B2.3 三步全部通过 | 20～35 人周 |
| A-P2 | A | 后端矩阵（含 Intel 核显 R3a） | 三厂商宿主 GPU 均达 A-P1 状态 | 3～5 人周 |
| A-P3 | A | 通道补齐 | 4K 硬解；多屏；系统音频 | 12～20 人周 |
| A-P4 / B4 | A+B | 性能与硬化 | 8 小时无 WindowServer 重启 | 8～12 人周 |
| B3 | B | 物理 GPU 后端（Intel 优先） | 同类验收 | 依 B0 结论重估 |
| P5 | — | VirtualBox | 可行性报告 | 按需 |

> 估算基于 1～2 名熟练的图形/虚拟化工程师。B0 与 B2 的不确定性最大——B0 的结论可能直接砍掉 B2/B3。

### 3.2 B0 · 风险闸门（4～8 人周）

**起点：`MacHyperVSupport` 的图形 kext 骨架**

它已经交付了轨道 B 最难的"第一公里"，可以直接当模板用：

- **证明"第三方图形 kext 能被 `IOGraphicsFamily` 接纳"在现代 macOS 上可行**（虽然只做到 2D 帧缓冲）。
- 给出了 System-KC 依赖问题的**另一种解法**：README 明确写 *"If injecting MacHyperVFramebuffer on supported versions, `IOGraphicsFamily` (com.apple.iokit.IOGraphicsFamily) must also be injected with `Force`"* —— 即**用 `Force` 直接注入整个 family**，而不是像 PR #600 那样去解析 System KC 符号。**两条路要在 B0 里对比实测。**
- 走的是**系统内安装**路线：*"must be installed and kext signing disabled in SIP on macOS 11.0 and newer"* —— 可作为 B1 地基选择的既有样本（放宽 kext 签名策略，而非引导期注入）。
- 附带一整套 OpenCore 配置样本（`SSDT-HV-VMBUS` / `SSDT-HV-DEV` / `SSDT-HV-PLUG`、Booter quirks、`Kernel→Force`、`Kernel→Patch`），可直接当配置模板。

**B0 要做的事**

- **目标零（最先做，成本最低）**：验证假设 **H1**——读 `AppleParavirtGPU.kext` 的 Info.plist 确认
  `IOPCIMatch` / 依赖 / 是否在 System KC；再做一次最小实验：在客体里伪造一个 PCI 设备，验证能否被枚举
  并让自家 driver 接管（手法照抄 `HyperVGraphicsBridge`）。**H1 的结论会决定整个 B 轨的形态。**
- 目标一：把 `MacHyperVFramebuffer` 从"帧缓冲"推进到"最小加速器"（先软件光栅填充，只求 WindowServer 走我们的路径）
- 目标二：枚举 Metal 驱动侧接口面，产出接口清单（入口列表 + 覆盖率 + 未实现项的影响面）
- 目标三：对比 `Force` 注入 `IOGraphicsFamily` 与「PR #600 式 System-KC 符号解析」两条路

- **出口准则**：H1 的 go/no-go（若成立则重写上层方案）；WindowServer 使用自研加速器渲染；接口清单与覆盖率估计；两条注入路线的对比结论

### 3.3 B1 · 地基选择（3～5 人周）

- 路线一：OpenCore 引导期注入（含 PR #600 或自建等价实现）
  - 复核 PR #600 是否已合并；若未合并，评估自行实现跨 KC 解析的代价
  - 验证版本锁定 KC 在系统更新 / Recovery / 多系统下的实际失效率
- 路线二：改根卷 + `kmutil` 重建 AuxKC
- **出口准则**：两条各跑通一次加载自研 kext；产出取舍报告（含更新存活率、可回滚性、ESP/磁盘开销）
- **注意**：arm64e 路径在 PR #600 中未做实机验证，若目标是 Apple Silicon 宿主需单独评估

### 3.4 B2 · SVGA3D 后端（20～35 人周）

- B2.1 2D/FIFO（3～5 人周）
- B2.2 3D 命令子集（8～14 人周）
- B2.3 compute + 系统框架（8～16 人周）
- 每步的验收都要求与 Mesa `svga` 的输出做一致性比对（见 §4.2）

### 3.5 轨道 A（沿用 v0.1 的 A-P0～A-P4）

细节见 v0.1 章节，本次无实质变化，仅优先级下调至 B0/B1 之后。

### 3.6 资源与团队

| 角色 | 数量 | 职责 |
|---|---|---|
| 图形/驱动工程师 | 1～2 | 轨道 B 的上下层；轨道 A 的 decode / 后端 |
| 虚拟化工程师 | 1 | QEMU 设备模型、WHPX、`MacHyperVSupport` 复用评估 |
| 多媒体工程师 | 1（A-P3 起） | 视频解码 / 音频通道 |
| 测试工程师 | 1 | 自动化框架、一致性比对、兼容性矩阵 |

**硬件**：Windows 宿主（NVIDIA）/ Linux 宿主（NVIDIA 或 AMD）/ Intel 11 代+ 核显机 / Apple Silicon 机各一。**若 B0 通过，建议再加一台纯 Intel + 一张 NVIDIA 用于 B2/B3。**

---

## 4. 测试方法

### 4.1 分层

| 层级 | 对象 | 手段 | 频次 |
|---|---|---|---|
| L1 | wire 解析、decode、SPIR-V / DXBC 发射 | 单元测试 + 合成 fixture | 每次提交 |
| L2 | 单 opcode 族命令流回放 | 录制/回放 + oracle 比对 | 每日 |
| L3 | 完整客体启动到桌面 | 快照回滚 + 自动截图 + 图像比对 | 每日 |
| L4 | 长稳 8 小时 | 定时截图 + 崩溃日志 | 每周 |
| L5 | 兼容性矩阵 | 矩阵调度 | 每里程碑 |

### 4.2 正确性测试

**轨道 A**

- **图像一致性比对（最强杠杆）**：reims 自带 `--device reims-vgpu-mmio` 与 `--device apple-gfx-mmio` 的 A/B 开关，等于免费获得 Apple 实现的 oracle。固定场景（壁纸、Finder 窗口、Safari 固定页、`MTLDevice` 探测工具）分别截图、逐像素比对 + 容差 + diff 可视化。
- **计算精度**：复刻 null-moth 做法，用卷积 / 矩阵乘 / 归约 / 原子操作等一组基准，比对 GPU 与 CPU 参考实现；覆盖 Core Image、Vision、MPS。

**轨道 B（多一个 oracle，但要自己造）**

- **协议一致性比对**：把自研 SVGA3D 后端产生的命令流，与 **Mesa `svga` 驱动**在同一场景下产生的命令流做**逐记录比对**（字段级 diff）。Mesa 是源码级的规范，这条比对比截图像素比对更早发现问题、更容易定位。
- **端到端**：与轨道 A 共用截图比对框架；轨道 B 的参照物为同设备下 Mesa/Linux 客体的渲染结果。
- **Metal 特性等级验证**：逐项列出目标特性（argument buffer、tile memory、mesh shader、光追…）的实现状态（原生 / 降级 / 模拟 / 不支持），并给出每项的实测误差。
- **版本回归**：轨道 B 依赖私有 ABI，**每次 macOS 版本更新后必须重跑 L3 全量**，并把失败项按 S1～S5 分级。

### 4.3 性能测试

| 指标 | 采集方式 | 目标 |
|---|---|---|
| 桌面合成帧率 | 客体固定动画 + 宿主侧帧计数 | ≥ 宿主等效机型 30 fps |
| 端到端延迟 | 输入事件到画面更新的时间戳差 | 与基线比较不劣化 |
| 命令流吞吐 | decode / 后端每帧记录数与耗时 | 定位热点 |
| shader 编译次数 | pipeline cache 命中率 | ≥ 90% |
| 宿主 CPU 占用（4K 播放） | 采样 | 证明硬解生效（A-P3 / B 轨视频通道） |
| 宿主 GPU 占用 | 采样 | 无明显争用（尤其 iGPU 场景） |

### 4.4 兼容性矩阵

```
轨道      : A(QEMU) / B(SVGA3D) / B(GSP) / B(Intel)
宿主 OS   : Windows 11 / Ubuntu LTS / macOS
VMM       : QEMU(WHPX/KVM/HVF) / VMware Workstation / VMware Fusion
宿主 GPU  : NVIDIA / AMD / Intel 核显(Gen12+) / Mesa(lavapipe)
客体 rail : macos-13 / 14 / 15 / 26
注入方式  : 仅轨道 B 适用 —— 引导期注入 / 系统内注入
```

### 4.5 自动化与 CI

**直接复用 reims 的三件设施**（这是最大的杠杆）：

1. **快照回滚**：每条 rail 独立快照，`--testing` 限时启动且必然回滚 → 端到端可重复、无状态污染
2. **A/B 设备开关**：`apple-gfx-mmio` 作为 oracle
3. **截图脚本**：`scripts/screenshot/screenshot.sh`

需要补的：

- 图像比对服务（基准库 + 容差 + diff 可视化）
- **SVGA3D 命令流比对器**（轨道 B 专用，字段级 diff）
- 矩阵调度器
- 失败自动留证：截图 + 失败日志 + 快照 ID + 提交号
- **版本回归看板**：轨道 B 在每次 macOS 更新后自动重跑 L3

### 4.6 缺陷分级

| 级别 | 定义 | 示例 | 处置 |
|---|---|---|---|
| S1 | 客体无法启动 / 内核崩溃 | 启动即 panic | 阻断发布 |
| S2 | 桌面不可用 | WindowServer 重启、全黑 | 阻断发布 |
| S3 | 功能缺失 | Metal 特性不可用、特定应用渲染错误 | 记入已知问题 |
| S4 | 显示瑕疵 | 撕裂、闪烁、颜色偏差 | 排期修复 |
| S5 | 性能不达标 | 帧率低于基线 | 排期修复 |

### 4.7 完成定义（DoD）

- 出口准则全部由**自动化**验证通过（非人工目视）
- 兼容性矩阵中该里程碑涉及的必测组合全绿
- 无未解决的 S1/S2
- 已知问题清单更新，每条标注影响范围与绕过方式
- 度量数据入库，可与上一里程碑对比

---

## 5. 风险登记册

| 编号 | 风险 | 等级 | 影响 | 应对 |
|---|---|---|---|---|
| R1 | **B0 闸门不通过**：无法在现代 macOS 上注册可用的第三方图形加速器 | 高 | 轨道 B 全部作废 | 把 B0 排在一切之前；先做最小 2D 验证 |
| R2 | **法律**：`reims-vgpu-wire` 自述 "derived from Apple's own encoder"，与"用 Apple 硬件生成 golden 再在异机对齐输出"的干净做法性质不同 | 高 | 可能阻断交付 | 立项阶段法务确认；评估"仅用公开抓包 + 独立推导"重建 wire 层的可行性 |
| R3 | **法律**：在非 Apple 硬件上运行 macOS 违反 EULA | 高 | 同上 | 同上；交付物不内置任何 Apple 二进制 |
| R4 | **法律**：轨道 B 需实现私有 `MTLDriver` 接口；若参考 Apple 反汇编代码，性质比 black-box 复现严重 | 高 | 同上 | 明确采用 clean-room / black-box 证据；记录方法论 |
| R5 | **法律**：SVGA3D 侧风险低于其他路线（协议开源），但若逆向 VMware Windows 驱动需评估 | 中 | — | 优先只用 Mesa 源码，不碰 VMware 二进制 |
| R6 | PR #600 长期未合并；arm64e 路径无实机验证 | 中 | 轨道 B 的引导期注入路线无可靠地基 | 在 B1 实测；必要时自建等价实现；或改走系统内注入 |
| R7 | 版本锁定 KC 的维护成本（366 MB、随系统更新失效、Recovery/多系统失效） | 中 | 用户体验差 | B1 实测失效频率；评估自动重建流程 |
| R8 | SVGA3D 功能集停在 GL 4.3，Metal 特性大量缺失 | 中 | 需大幅降级 | B2 前先定"Metal 特性等级目标"并写入验收标准 |
| R9 | 着色器链路新增 AIR → DXBC/DXIL 一跳，不能直接复用 metal2vulkan 产物 | 中 | 新增工作量 | 移植 Mesa `svga` 的 DXBC 发射代码（MIT） |
| R10 | reims 处于 Alpha，ABI 与 crate 布局随时变动 | 中 | 上游变更导致返工 | 锁定 commit；自有改动放独立 crate |
| R11 | 轨道 B 随 macOS 版本漂移，每次更新需重新适配 | 高 | 长期维护成本 | 建立版本回归看板；把适配成本显式计入排期 |
| R12 | null-moth 项目仅有二手报道、无公开仓库 | 中 | 参照不可靠 | 优先找原始报告；把 B0 作为独立验证 |
| R13 | Windows/WHPX 的 CPU 与共享内存开销可能使轨道 A 不可用 | 中 | A-P1 落空 | A-P1 设决策点，预案为 WSL2 + KVM |
| R14 | 轨道 A 的通道补齐工作量被低估 | 高 | 排期失控 | A-P0 先做通道摸底，用实测数据重估 |
| R15 | **外部结论被新项目推翻**（本轮已发生一次：曾误判"macOS 无 VMBus 客户端驱动"，实为 `MacHyperVSupport`） | 中 | 误砍或误保整条路线 | 每条"不存在"式判断必须标注**核查日期 + 检索范围**；影响路线取舍的判断要求**两处独立来源** |
| R16 | 复用 `MacHyperVSupport` 骨架时，其 kext 需在 macOS 11+ 放宽 SIP 的 kext 签名策略 | 中 | 与产品形态/安全要求可能冲突 | B0 阶段确认**最小必要的 SIP 放宽项**，并评估能否收窄 |
| R17 | **假设 H1 不成立**（Apple 的 paravirt 设备存在未公开的 VMM 侧契约，客体自造无法满足） | 高 | 方案需回退到原路线 B（自写 Metal 驱动） | B0 目标零先验证；两条路线的方案草稿保持并行，不提前投入 |
| R18 | H1 成立但**协议正确性门槛**高：reims 的 decode 是 Alpha、覆盖不全，Apple 驱动合规要求远高于"能点亮" | 中 | 工期不可控 | 用 A/B 对照（Apple 实现作 oracle）先量化 opcode 覆盖缺口，再估工期 |

---

## 6. 附录

### 6.1 关键技术参考

| 主题 | 参考 |
|---|---|
| ParavirtualizedGraphics 官方接口 | Apple Developer Documentation — ParavirtualizedGraphics |
| 轨道 A 基线 | `steelbrain/reims-vgpu`（LGPL-3.0-or-later） |
| AIR → SPIR-V | `steelbrain/metal2vulkan`（LGPL-3.0-or-later） |
| OpenCore 跨 KC 依赖解析 | OpenCorePkg PR #600（open）+ PR #603 / #606 |
| SVGA3D 协议与参考实现 | Mesa `docs/drivers/svga3d.rst`、Mesa `svga` gallium 驱动、`vmwgfx` |
| SVGA3D 自写客体驱动先例 | SoftGPU（Windows 9x，vGPU9 / vGPU10） |
| SVGA3D 功能集上限 | GL 4.3 需 Workstation 17 / Fusion 13 + 硬件版本 20 + VGPU10 |
| Hyper-V GPU-PV 机制 | WDDM 2.4+；`Add-VMGpuPartitionAdapter`；ExHyperV（配置 GUI，非虚拟总线） |
| macOS 侧 VMBus + VSC 实现 | `acidanthera/MacHyperVSupport`；`Docs/HyperV-devices.md` 列出全部合成设备 class GUID 与实现状态 |
| Hyper-V 合成设备与 VMBus 协议 | Microsoft Hypervisor Top-Level Functional Specification（TLFS）；Linux `drivers/hv`（offer / rescind、GPADL、ring buffer） |
| 轨道 B 起点骨架 | `MacHyperVFramebuffer.kext`（2D 帧缓冲 + `IOGraphicsFamily` Force 注入示例 + OpenCore 配置样本） |
| 客体侧伪造 PCI 设备的手法 | `MacHyperVSupport` → `GraphicsBridge/HyperVGraphicsBridge.cpp`（手工填配置空间 + config 读写 + BAR0） |
| 合成 PCI 根桥 | `PCIRoot/HyperVPCIRoot.cpp`（`registerChildPCIBridge`、`allocateRange`，BSD 风格许可可直接参考） |
| VMBus VSC 扩展点 | `MacHyperVSupport/Info.plist` 的 `HVType` 匹配 + `VMBusDevice/HyperVVMBusDevice.hpp` 的通道/GPADL API |
| 源码精读报告 | `MacHyperVFramebuffer_源码精读报告.md`（本仓库内，含逐条代码位置） |
| Intel 核显支持天花板 | Dortania GPU Buyers Guide / OpenCore Install Guide |
| Intel 原生驱动资料 | Intel Graphics Programmer's Reference Manual；Mesa `anv` / `iris` |
| 物理 GPU 原生驱动先例 | null-moth 的 RTX 5060 驱动栈（单一来源，待复核） |

### 6.2 待办

- [x] 核实 OC PR #600 —— 已完成，见 §1.4.0
- [x] 核实 macOS 侧 Hyper-V 支持现状 —— 已完成，`MacHyperVSupport` 存在，见 §1.4.4 / §3.2
- [ ] **B0 风险闸门**：以 `MacHyperVFramebuffer` 为骨架的 Metal 驱动接口可行性 spike（最高优先）
- [ ] 精读 `MacHyperVSupport` 源码与 OpenCore 配置样本，确认可否直接复用为 B0 骨架
- [ ] **交叉验证**：Hyper-V 宿主侧能否注册新的合成设备 class GUID（目前结论"无接口"，需第二处独立来源）
- [ ] **B0 目标零（最高优先）**：读 `AppleParavirtGPU.kext` 的 Info.plist，确认匹配条件与位置（假设 H1 第 1、2 问）
- [ ] **B0 目标零**：做一次"客体内核伪造 PCI 设备"最小实验（照抄 `HyperVGraphicsBridge` 手法），验证可枚举、可被接管
- [ ] 梳理 `IOFramebuffer` 的 15 个方法，产出 B0 最小 2D 加速器的实现清单
- [ ] **B1**：实测两种 kext 注入方式，产出取舍报告
- [ ] 复核 PR #600 是否已合并；跟踪 #603 / #606 的进展
- [ ] 复核 null-moth 项目的公开代码或原始报告
- [ ] 定"Metal 特性等级目标"（轨道 B 的验收基线）
- [ ] 向法务提交 R2/R3/R4/R5 的书面确认
- [ ] 实测 QEMU/WHPX 的 CPU 开销，决定 A-P1 是否继续
- [ ] A-P0 阶段完成 A-P3 的工作量重估
