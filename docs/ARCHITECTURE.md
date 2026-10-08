# 方向与架构回顾（2026-10-08）

> 目标：macOS 客户机内"完整可用的 GPU 加速"——桌面合成 + 应用级 3D（Metal / OpenGL /
> Vulkan / D3D）。本文回顾当前方向、给出全栈架构，并映射四个参考项目的复用点。

## 1. 关键架构模板：nullmoth/nvidia-macos-driver

NullMoth 已在 macOS 15 Sequoia 上为 NVIDIA RTX 实现了**完整 Metal 驱动**（源码开放），
其分层正是本项目的路线图：

```
Metal app ─► NVMTLDriver.bundle ─► translator (Apple AIR → SPIR-V) ─► NVK (Mesa Vulkan) ─► kexts ─► GPU
```

| 组件 | 路径 | 对我们的对应物 |
|---|---|---|
| Metal 驱动插件 | `/Library/GPUBundles/NVMTLDriver.bundle` | `SVGMTLDriver.bundle`（plugin/ 目录有源码可研读） |
| 着色器翻译器 | AIR→SPIR-V，基于 metal2vulkan（LGPL） | **直接复用** metal2vulkan（用户早前已指出） |
| Vulkan 后端 | NVK（Mesa 补丁） | Vulkan-on-SVGA3D（需自研，见 §3） |
| 内核扩展 | NVRM/NVAccel/NVRMFB/NVRMAGDC，装 `/Library/Extensions`、Auxiliary KC 加载 | 我们已有 vgpuFramebuffer（同样的 auxKC 加载路径！）+ 需补 NVAccel 等价物 |
| WindowServer 接入 | `nvaccel=1` boot-arg + AMFI 参数放行驱动 bundle | 同样机制 |

**重要红利**：Metal 驱动一旦工作——
- **OpenGL 免费获得**：Apple 的 GL-on-Metal（nullmoth 实测 GL 跑在 Metal 之上）
- **OpenCL / Core Image / Core ML / MetalFX** 免费
- **Vulkan** 通过 MoltenVK（Vulkan→Metal）
- **D3D10/11** 通过 DXMT（3Shain/dxmt，LGPL，活跃，Wine 集成）——**这就是"同时支持
  GL/Vulkan/DX"的答案：不需要分别实现，全都汇聚到同一个 Metal 驱动层**

## 2. 当前栈状态（2026-10-08）

```
┌─ 应用层（未来）  Metal apps / GL / Vulkan(MoltenVK) / D3D(DXMT) ─ 未开始
├─ 驱动插件层      MTLDriver.bundle（翻译器 + 后端）────────────── 未开始（模板已有）
├─ 内核加速层      IOAccelerator 等价物（窗口合成/命令提交）────── 未开始
├─ 传输层          vgpuFramebuffer.kext ────────────────────────── 显示 ✓ GB/DX 栈 ✓
│                  FIFO 所有权 + OTable + CB 提交机制 + STDU 呈现
│                  卡点：DX 命令链最后一环（SET_COTABLE 被 MKS 拒，见 STATUS.md）
└─ 设备层          MKS SVGA3D（level 9 vGPU10 代际，SM41+SM5+GL43 devcap）
```

- 已确认：本 MKS 上 Windows 客户机 + vm3dmp 12.4.5（== 13.1.5，md5 相同）正常跑 3D——
  设备/协议/驱动三方都验证可用，我们的 CB 流与参考驱动同构，剩余为初始化序列细节。
- qemu-vmvga 价值：一个**设备侧参考实现**（SVGA3D 寄存器/FIFO/GB/DX 命令解析全开源），
  可作为第二 oracle 交叉验证我们的命令流与设备期望。

## 3. 后端路线决策：Vulkan 作为内部 IR

用户问题："DX 主要支持 Windows，GL 和 Vulkan 支持更多平台，是否同时支持？"

**答案：实现一次 Metal 驱动层，四种 API 全部覆盖**（§1 红利）。真正要做的后端选择：

| 方案 | 工作量 | 说明 |
|---|---|---|
| A. Vulkan-on-SVGA3D 自研后端 | 大 | SPIR-V → SVGA DX 字节码（SPIRV-Cross→HLSL→fxc 或直译）+ 对象模型映射；SM41 表面小，SVGA3D DX 命令面窄 |
| B. 复用 Mesa svga Gallium（GL-only） | 中 | Mesa 已有 OpenGL-over-SVGA3D（Linux 客户机在用），但 Apple GL 不挂外部 GL 驱动，GL-on-Metal 才是 macOS 路径 → 仍需 Metal |
| C. 直接 Metal AIR → SVGA DX 字节码 | 中大 | 少一层 IR，但放弃 metal2vulkan 复用 |

**选 A**（对齐 nullmoth 模板）：翻译器免费复用（metal2vulkan，LGPL），NVK 源码作为
"Vulkan 驱动如何对接 kext"的完整教材，SM41 表面让后端范围可控。

## 4. 分阶段路线

- **Phase A（当前）— DX 传输层判决**：CB 字节级 diff（我们 vs vm3dmp 构造），攻破
  SET_COTABLE 被拒问题 → 拿到 `ROUND TRIP VERIFIED`。里程碑：3D 引擎写 guest 可见内存。
- **Phase B — 用户态冒烟**：kext 暴露用户客户端（IOUserClient），无 Metal 的裸
  Vulkan-on-SVGA 原型（离线 SPIR-V 测试着色器跑通 CLEAR→读回→三角形）。
- **Phase C — Metal 插件**：SVGMTLDriver.bundle + IOAccelerator kext（研读 nullmoth
  plugin/ 与 NVAccel），WindowServer 合成走我们的设备。
- **Phase D — 生态**：GL-on-Metal / MoltenVK / DXMT 自动覆盖；Mesa svga 可作设备行为
  第二参照。

## 5. 参考项目映射

| 项目 | 在本项目的角色 |
|---|---|
| nullmoth/nvidia-macos-driver | **总架构模板**：MTLDriver plugin 结构、AIR→SPIR-V 翻译器、auxKC 加载、WindowServer 接入、boot-args/AMFI |
| 3Shain/dxmt | D3D10/11→Metal 翻译层（Phase D 直接用）——证明"Metal 就位后 DX 免费获得" |
| qemus/qemu-vmvga | 设备侧 SVGA3D 参考实现（寄存器/FIFO/GB/DX 解析），第二协议 oracle |
| templarsco/limiar | PV-GPU 协议设计模式参考（命令类型/载荷/错误码），非直接复用 |
| metal2vulkan（steelbrain） | AIR→SPIR-V 翻译器，Phase C 直接复用（LGPL） |
| Mesa (NVK / svga gallium) | Vulkan 驱动↔kext 对接教材 / SVGA3D 设备行为参照 |

## 6. 近期执行顺序

1. CB 字节级 diff → DX 读回判决（Phase A 收口，`kDxTest=1` 开关已就绪）。
2. 判决通过后：SVGA3D DX 命令集完备性清单（SM41 表面逐项 vs DXMT/Vulkan 需求）。
3. Phase B 用户态冒烟原型（不依赖 WindowServer）。
4. 并行研读 nullmoth `plugin/` + `kexts/`（NVAccel）确定 IOAccelerator 接入面。
