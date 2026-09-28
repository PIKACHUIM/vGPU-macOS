# MacHyperVSupport / MacHyperVFramebuffer 源码精读报告

**对象** `acidanthera/MacHyperVSupport`（作者 Goldfish64，BSD-3-Clause 风格许可，2021–2025）
**日期** 2026-09-27 · **用途** 为「黑苹果 GPU 加速方案」的 B0 风险闸门与 B1 地基选择提供一手证据

---

## 0. 结论速览

| # | 结论 | 对本项目的价值 |
|---|---|---|
| 1 | 图形栈是**三段式**：VMBus VSC →（伪造 PCI 设备）→ 独立 framebuffer kext | 照抄这个分层即可 |
| 2 | `IOFramebuffer` 只需实现 **15 个方法**，且头文件是 SDK 公开件 | 2D 显示驱动无私有 ABI 障碍 |
| 3 | **客体内核可以凭空伪造一个 PCI 设备**（`HyperVGraphicsBridge` 手工填配置空间） | ⚠️ 推翻了我们"必须由 VMM 提供设备"的隐含假设，见 §9 假设 H1 |
| 4 | 此 kext **零** `IOAccelerator` / Metal 代码，只交出一个线性帧缓冲 | "点亮"与"加速"的边界在源码里肉眼可见 |
| 5 | 主 kext 用**注入**，framebuffer kext 装到 **/L/E**，两者靠 `serviceMatching` + `callPlatformFunction` 解耦 | B1 地基选择的现成先例（§7） |
| 6 | `Kernel→Force` 注入 `IOGraphicsFamily` 的确切原因已定位（`OSBundleLibraries` 依赖） | 与 PR #600 是同一问题的两种解法（§8） |
| 7 | 合成显卡协议的 **11 条消息里没有一条与 3D 有关** | 源码级确证"Hyper-V 无 3D 通路"（§10） |
| 8 | VMBus 设备扩展点**完全开放**（纯 plist + `HVType` 匹配），VSC API 含 GPADL/DMA | 客体侧无障碍；卡点确在宿主侧（§6） |

---

## 1. 完整调用链

```
┌─ Hyper-V 宿主 ────────────────────────────────────────────┐
│  合成显卡 VMBus 设备（class GUID da0a7802-e377-…）          │
└───────────────────────┬───────────────────────────────────┘
                        │ VMBus 通道（环形缓冲 + GPADL）
┌─ macOS 客体 ──────────▼───────────────────────────────────┐
│  HyperVController  ← ACPI VMBS 节点                        │
│    └── HyperVVMBus   （VMBus 根，Docs/modules.md: VMBus Controller）│
│         └── HyperVVMBusDevice （设备 nub，发布 HVType/HVInstance/HVChannel）│
│              └── HyperVGraphics  （VSC；Info.plist HVType = da0a7802-…）│
│                   ├── 协商版本 / 申请显存 / 设分辨率 / 光标 / 图像更新    │
│                   └── HyperVGraphicsBridge                              │
│                        · 在 HyperVPCIRoot 上注册一个子 PCI 桥            │
│                        · 手工伪造 PCI 配置空间 0x1414:0x5353, BAR0=显存  │
│                        · 实现 config 读写                                  │
│                              │                                            │
│                              ▼（挂在合成 PCI 根总线上）                    │
│  HyperVGraphicsFramebuffer.kext（独立 kext，装 /Library/Extensions）      │
│    · IOPCIMatch = 0x53531414, IOMatchCategory = IOFramebuffer             │
│    · 继承 IOFramebuffer，通过 serviceMatching("HyperVGraphics") 找后端     │
│    · callPlatformFunction(Init / SetResolution / SetCursorShape / …)      │
│                              │                                            │
│                              ▼                                            │
│                        WindowServer（2D 桌面）                             │
└───────────────────────────────────────────────────────────────────────────┘
```

代码位置：`MacHyperVSupport/Info.plist` L51-59 / L73-94 / L167-190；`GraphicsBridge/HyperVGraphicsBridge.cpp` L42-135；
`MacHyperVFramebuffer/HyperVGraphicsFramebuffer.cpp` L81-137。

---

## 2. `HyperVGraphicsFramebuffer`：接口面清单

`HyperVGraphicsFramebuffer.hpp` L22-75，`#include <IOKit/graphics/IOFramebuffer.h>`（**SDK 公开头文件**）。
全部 override 只有 15 个：

| 分组 | 方法 |
|---|---|
| IOService | `start` / `stop` |
| IOFramebuffer | `enableController`、`isConsoleDevice`、`getApertureRange`、`getPixelFormats`、`getDisplayModeCount`、`getDisplayModes`、`getInformationForDisplayMode`、`getPixelFormatsForDisplayMode`、`getPixelInformation`、`getCurrentDisplayMode`、`setDisplayMode`、`getAttribute`、`setCursorImage`、`setCursorState`、`flushCursor` |

几个关键实现：

- **显存就是一段物理区间**：`getApertureRange()` 直接 `IODeviceMemory::withRange(_gfxBase, _gfxLength)`（.cpp L144-150）。`_gfxBase/_gfxLength` 来自后端 `platformInitGraphics()`（Private.cpp L14-33）。
- **模式表来自 Info.plist**：`buildGraphicsModes()` 读 `SupportedResolutions` 数组（Private.cpp L35-93），失败则回落 1024×768（L95-109）。
- **分辨率切换是平台函数调用**：`setDisplayMode()` → `callPlatformFunction(kHyperVGraphicsPlatformFunctionSetResolution, …)`（.cpp L240-256）。
- **硬件光标**：`getAttribute(kIOHardwareCursorAttribute)` 返回 1；`setCursorImage()` 用 `convertCursorImage()` 转换后调后端（.cpp L258-320）。光标上限 32×32。
- **完全没有任何 3D 相关代码**：没有 `IOAccelerator`、没有 Metal、没有命令队列。它只交出一块线性帧缓冲。

**→ 量化结论**：2D 显示驱动的公开接口面约 15 个方法；"加速"这件事在这份源码里一行都没有。

---

## 3. 抢设备的标准手法（`Info.plist`）

`MacHyperVFramebuffer/Info.plist` L21-77：

```xml
<key>IOClass</key>          <string>HyperVGraphicsFramebuffer</string>
<key>IOMatchCategory</key>  <string>IOFramebuffer</string>   ← 关键
<key>IOPCIMatch</key>       <string>0x53531414</string>       ← vendor 0x1414 (Microsoft) / device 0x5353
<key>IOProbeScore</key>     <integer>50000</integer>          ← 压分
<key>IOProviderClass</key>  <string>IOPCIDevice</string>
<key>SupportedResolutions</key> <array> … 640×480 … 1280×1024 … </array>
```

`IOMatchCategory = IOFramebuffer` 让第三方 framebuffer kext 可以接管一个本该由系统驱动持有的设备；
`IOProbeScore = 50000` 是常见的压分手段。这两点对我们要接手的设备同样适用。

---

## 4. `HyperVGraphicsBridge`：客体内核伪造 PCI 设备的完整手法 ⭐

这是本次精读**最重要的发现**。`GraphicsBridge/HyperVGraphicsBridge.cpp`：

**(1) 在合成 PCI 根桥上注册一个子桥**（L85）

```cpp
status = hvPCIRoot->registerChildPCIBridge(this, &_pciBusNumber);
```

**(2) 纯手工填一块假的 PCI 配置空间**（L100-106）

```cpp
bzero(_fakePCIDeviceSpace, sizeof (_fakePCIDeviceSpace));
OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigVendorID,  kHyperVPCIVendorMicrosoft);
OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigDeviceID,  kHyperVPCIDeviceHyperVVideo);
OSWriteLittleInt32(_fakePCIDeviceSpace, kIOPCIConfigRevisionID, 0x3000000);
OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigSubSystemVendorID, kHyperVPCIVendorMicrosoft);
OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigSubSystemID,       kHyperVPCIDeviceHyperVVideo);
OSWriteLittleInt32(_fakePCIDeviceSpace, kIOPCIConfigBaseAddress0, (UInt32)_fbInitialBase);
```

注释写得非常清楚（L93-99）：

> PCI bridge will contain a single PCI graphics device with the framebuffer memory at BAR0.
> The vendor/device ID is the same as what a generation 1 Hyper-V VM uses for the emulated graphics.

**(3) 实现 8/16/32 位 config 读写，全部落到那块假空间**（L137-236）；并在 `configWrite32(BAR0, 0xFFFFFFFF)` 时返回 BAR0 尺寸（L166-169），这是 PCI 探测的标准握手。

**(4) 把显存区间注册进桥**（L128-135）

```cpp
addBridgeMemoryRange(_fbInitialBase, _fbInitialLength, true);
```

**(5) 由 VSC 提供**：`IOProviderClass = HyperVGraphics`（Info.plist L84-94），只在 Gen2 上启动（L32-39）。

**→ 意义**：一个 macOS 内核扩展可以**凭空造出一个 macOS 自己会去枚举、探测、并允许其他 driver 接管的 PCI 设备**。这不需要 VMM 配合，也不需要改固件。

---

## 5. `HyperVPCIRoot`：合成 PCI 根桥

`PCIRoot/HyperVPCIRoot.hpp`：

- 继承 `IOPCIBridge`，匹配 ACPI 节点：Gen1 用 `PNP0A03`，Gen2 用 `VMOD`（Info.plist L167-190）
- 单例入口 `static HyperVPCIRoot* getPCIRootInstance()`
- `registerChildPCIBridge(IOPCIBridge*, UInt8 *busNumber)`：从 **0x10** 起找空闲总线号，共 **256** 个槽位（.cpp 实现）
- `allocateRange(size, alignment, maxAddress)` / `freeRange()`：低/高 MMIO 区间分配器（`IORangeAllocator`）
- 包装了 `IOPlatformExpert::setConsoleInfo`（`wrapSetConsoleInfo`），并 `reserveFramebufferArea()`
- 另有 `HyperVPCIProvider` 模块：在 Gen2 上提供 `IOACPIPlatformDevice` nub 给这个假根桥（`Docs/modules.md`）

**→ 意义**：从"合成根总线 → 子桥 → 设备配置空间 → 显存 BAR → MMIO 区间分配"这条完整链路，已经有一份可读、可复用的开源实现，许可为 BSD 风格。

---

## 6. VMBus 设备扩展点是完全开放的

`MacHyperVSupport/Info.plist` L73-83 回答了"能不能加新设备"：

```xml
<key>HyperVGraphics</key>
<dict>
  <key>HVType</key>          <string>da0a7802-e377-4aac-8e77-0558eb1073f8</string>
  <key>IOClass</key>         <string>HyperVGraphics</string>
  <key>IOProviderClass</key> <string>HyperVVMBusDevice</string>
</dict>
```

匹配靠**自定义键 `HVType` = 设备 class GUID 字符串**，`HyperVVMBusDevice` 覆盖了 `matchPropertyTable` 做匹配，
并发布 `HVType` / `HVInstance` / `HVChannel` / `HVMMIOByteCount`（`HyperVVMBusDevice.hpp` L20-23）。

**→ 加一个新的 VSC 处理器是纯 plist 工作**（前提是能收到 offer）。

`HyperVVMBusDevice` 给子类提供的 VSC API（同文件 L149-228）相当完整：

| 类别 | 方法 |
|---|---|
| 通道 | `openVMBusChannel(txSize, rxSize, maxAutoTransId)`、`closeVMBusChannel()`、`getChannelId()`、`getInstanceId()`、`getTypeIdString()` |
| **DMA** | **`createGPADLBuffer(HyperVDMABuffer*, UInt32 *gpadlHandle)`**、`freeGPADLBuffer()` ← GPU 命令提交所需的共享内存 |
| 包动作 | `installPacketActions(target, packetReadyAction, wakePacketAction, initialResponseBufferLength, registerInterrupt, flushPackets)`、`uninstallPacketActions()`、`triggerPacketAction()` |
| 收包 | `nextPacketAvailable()`、`nextInbandPacketAvailable()`、`readRawPacket()`、`readInbandCompletionPacket()` |
| 发包 | `writeRawPacket()`、`writeInbandPacket()`、`writeInbandPacketWithTransactionId()`、`writeGPADirectSinglePagePacket()`、`writeGPADirectMultiPagePacket()`、`writeCompletionPacketWithTransactionId()` |
| 环形缓冲 | tx/rx `read/write index`、`getAvailableTxSpace()` / `getAvailableRxSpace()` |

**→ 客体侧没有任何障碍**。卡点确实只在宿主侧（Hyper-V 不会 offer 我们编出来的 class GUID）。

---

## 7. 两个 kext 的分工与加载方式（B1 地基选择的一手证据）⭐

`MacHyperVFramebuffer/HyperVGraphicsFramebuffer.cpp` L85-88 的注释是直接答案：

```cpp
// Get instance of graphics service.
// This cannot link against the main kext due to macOS requirements, as this kext
// must be in /L/E on newer macOS versions, but the main one will be injected.
```

配套的运行时粘合（L89-111）：

```cpp
OSDictionary *gfxProvMatching = IOService::serviceMatching("HyperVGraphics");
_hvGfxProvider = waitForMatchingService(gfxProvMatching);
```

以及四个平台函数（`HyperVGraphicsPlatformFunctions.hpp` L14-17）：

```
HyperVGraphicsPlatformFunctionInit
HyperVGraphicsPlatformFunctionSetResolution
HyperVGraphicsPlatformFunctionSetCursorShape
HyperVGraphicsPlatformFunctionSetCursorPosition
```

**→ 一个已经跑在生产里的先例：**

| 部分 | 加载方式 | 备注 |
|---|---|---|
| 主体（`MacHyperVSupport.kext`） | **注入** | 承载 VMBus、各 VSC、伪造 PCI 桥 |
| 图形前端（`MacHyperVFramebuffer.kext`） | **装到 `/Library/Extensions`** | 因为要链到 `IOGraphicsFamily` |
| 两者通信 | `serviceMatching` + `callPlatformFunction` | 不能硬链接 |

这条直接支撑我们轨道 B 的分层方案（上层/下层解耦 + 不同加载机制）。

---

## 8. `Kernel→Force` 的确切依据 ⭐

`MacHyperVFramebuffer/Info.plist` L82-98：

```xml
<key>OSBundleLibraries</key>
<dict>
  <key>com.apple.iokit.IOGraphicsFamily</key> <string>1.0.0b1</string>   ← 关键
  <key>com.apple.iokit.IOPCIFamily</key>      <string>1.0.0b1</string>
  <key>com.apple.kpi.bsd</key>      <string>8.0.0</string>
  <key>com.apple.kpi.iokit</key>    <string>8.0.0</string>
  <key>com.apple.kpi.libkern</key>  <string>8.0.0</string>
  <key>com.apple.kpi.mach</key>     <string>8.0.0</string>
  <key>com.apple.kpi.unsupported</key> <string>8.0.0</string>
</dict>
<key>OSBundleRequired</key> <string>Safe Boot</string>
```

`README.md` L54-58：

> - Force
>   - On older versions of macOS, the following kernel extensions may need to be Force injected.
>   - IONetworkingFamily (`com.apple.iokit.IONetworkingFamily`)
>   - IOSCSIParallelFamily (`com.apple.iokit.IOSCSIParallelFamily`)
>   - **If injecting MacHyperVFramebuffer on supported versions, IOGraphicsFamily (`com.apple.iokit.IOGraphicsFamily`)
>     must also be injected with `Force`**

**因果链完整闭合**：kext 声明依赖 `IOGraphicsFamily` → macOS 11 起该类在 **System KC**、不在 Boot KC →
注入路径解析不到 → 报 `library kext ... not found` → 用 `Kernel→Force` 把整个 family 强行注入。

**两种解法的对照**：

| 解法 | 做法 | 来源 | 代价 |
|---|---|---|---|
| **Force 注入整个 family** | `Kernel→Force` 指定 `com.apple.iokit.IOGraphicsFamily` | MacHyperVSupport（**已在生产使用**） | 需要把 system kext 一并带上；版本要匹配 |
| **解析 System KC 符号** | 加载 `SystemKernelExtensions.kc`（366 MB），重算 KASLR slide、翻译绑定到实际 VA | OpenCore PR #600（**open，未合并**） | 精巧、无需复制 kext；但版本锁定、arm64e 未实机验证 |

同一个 kext 的同一个依赖，两种走法。**B1 应该按这个对照表做实测。**

---

## 9. 合成显卡协议里**没有 3D**（源码级确证）

`Graphics/HyperVGraphicsRegs.hpp` L73-83，全部消息类型仅 11 条：

```
0x0 Error
0x1 VersionRequest        0x2 VersionResponse
0x3 VRAMLocation          0x4 VRAMAck
0x5 ResolutionUpdate      0x6 ResolutionUpdateAck
0x7 CursorPosition        0x8 CursorShape
0x9 FeatureChange
0xA ImageUpdate           ← 脏矩形帧推送
```

限制常量（同文件 L24-41）：

| 项 | 值 |
|---|---|
| 协议版本 | VMBus 3.0（Win2008/2008R2）、3.2（Win8–Win10/2016）、3.5（Win10 1809+） |
| v3.0 最大分辨率 | 1600×1200 @ **16 bpp** |
| 通用位深 | 32 bpp |
| 光标上限 | 32×32 |

**没有任何 draw call、shader、命令缓冲、提交环。** 后端 `HyperVGraphics` 的能力清单也只有：
`negotiateVersion` / `allocateGraphicsMemory` / `setGraphicsMemory` / `screenResolution` / `cursorShape` / `cursorPosition` / `refreshFramebufferImage`（`Graphics/HyperVGraphics.hpp` L48-59）。

**→ 结论**：Hyper-V 对 macOS 客体只提供"帧缓冲 + 光标 + 脏矩形推送"。3D 加速在协议层面不存在，
与驱动是否完善无关。这从源码上关闭了"在 Hyper-V 上做 3D"的讨论。

---

## 10. 对本项目的可复用结论

1. **分层可照抄**：VSC（VMBus/传输层）→ 桥/设备呈现 → framebuffer/加速器前端，三层职责清晰。
2. **2D 显示用 `IOFramebuffer`（公开 API，15 个方法）就能做**，这是 B0 最现实的起点。
3. **`IOMatchCategory` + `IOProbeScore` 是接管的钥匙**，对我们接任何设备都一样。
4. **伪造 PCI 设备的手法是现成的**（§4），代码 BSD 风格许可可直接参考。
5. **`Kernel→Force` vs PR #600 是两条并行可行解**，B1 实测二选一（§8）。
6. **主 kext 注入 + 前端 kext 装 /L/E** 的分工已被验证，可降低对引导期注入的依赖。
7. **VSC 扩展点开放**，且 `HyperVVMBusDevice` 已提供 GPADL/DMA 与完整包收发 API——若将来有 VMBus 形态的加速设备，客体侧几乎零成本接入。
8. `Docs/modules.md` 的**逐模块 boot arg 开关**（`-hvgfxoff`、`-hvgfxboff`、`-hvgfxfboff` …）是很好的可测试性设计，我们的驱动应照做。

---

## 11. 新假设 H1：「客体自造 Apple paravirt 设备」⚠️

**这是本次精读引出的、可能改变全项目形态的假设。请当作待验证假设而非结论。**

**假设内容**

§4 证明"客体内核可以凭空伪造一个 PCI 设备，并让 macOS 自己去枚举它"。那么：

> 用同样的手法伪造一个 PCI 设备，填上 **Apple paravirt GPU 的 vendor/device ID**，
> 让**系统自带的 `AppleParavirtGPU.kext` 自己挂上去**——于是"最难的 Metal 驱动层"由 Apple 提供，
> 我们只需要实现 paravirt 协议的对端（**reims 已经逆好了这一层**）+ 一个设备后端
> （VMware 的 SVGA3D / VMBus / reims 的 QEMU 设备）。

**如果成立，它意味着什么**

| 项 | 原方案（路线 B） | 假设 H1 |
|---|---|---|
| Metal 驱动 / 私有 MTLDriver ABI | **必须自己写（最贵）** | **由 Apple 的 `AppleParavirtGPU.kext` 提供** |
| paravirt 协议解码 | 不涉及 | 复用 `reims-vgpu-wire` + `runtime::decode`（已有） |
| 每设备后端 | 需要 | 仍需要（SVGA3D / VMBus / virtio-gpu） |
| 与 VMM 的关系 | 需要 VMM 能加设备 | **不需要**（设备在客体内部伪造） |

也就是说：**H1 把"路线 B 的上层"替换成"路线 A 的 decode 层"，而后者已经有开源实现。**
它同时绕开了 B0 风险闸门里最难的那部分，并让方案在 VMware / Hyper-V / VirtualBox 上一视同仁。

**必须先回答的 5 个问题（按可验证性排序）**

1. **匹配条件**：`AppleParavirtGPU.kext` 的 `IOPCIMatch` 到底是什么？是否还要求 ACPI/_DSM 属性、
   特定 BAR 布局，或者 `PGCopyOptionROMURL()` 提供的 option ROM？→ 直接看 kext 的 Info.plist 即可，最快。
2. **有没有**这个 kext：确认 x86_64 与 arm64 macOS 各版本上它是否存在、在不在 System KC。
3. **时机与竞态**：若真设备存在（例如已在 QEMU 里），伪造设备能否抢到（`IOProbeScore`）；
   若真设备不存在（VMware），伪造设备能否独立启动到被 WindowServer 使用。
4. **共享内存契约**：paravirt 协议里的 `PGPhysicalMemoryRange` / trace range 监控，是否隐含假设
   这些物理页由 VMM 映射？若假设成立，纯客体内部实现能否等价替代。
5. **正确性门槛**：Apple 的驱动对协议合规性要求远高于"能点亮"；reims 的 decode 目前是 Alpha、
   覆盖不全。H1 一旦成立，"能不能跑"会变成"要补多少个 opcode"。

**判定方式**：H1 的第 1、2 问只需读 `AppleParavirtGPU.kext` 的 Info.plist，
成本极低、信息量极大，**建议作为 B0 的第一件事**。

---

## 12. 待办（本报告产生的）

- [ ] 读 `AppleParavirtGPU.kext` 的 Info.plist，确认 `IOPCIMatch` / 依赖 / 是否在 System KC（假设 H1 第 1、2 问）
- [ ] 用 `HyperVGraphicsBridge` 的手法做一次最小实验：在客体里伪造一个 PCI 设备，验证能否被枚举、被自家 driver 接管
- [ ] 汇总 `IOMatchCategory` / `IOProbeScore` 在我们目标设备上的取值
- [ ] 把 `Kernel→Force` 与 PR #600 两条路线写成 B1 的实测对照表
- [ ] 梳理 `IOFramebuffer` 的 15 个方法，产出 B0 最小 2D 加速器的实现清单
