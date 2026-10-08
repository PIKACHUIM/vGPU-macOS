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

## 当前开放问题

### P1：context-0 CB 里的 SVGA3D 命令解析失败（CB → 3D 执行的最后一关）
- 现象：DEFINE_GB_MOB64（context-0 CB 第一条）`COMMAND_ERROR at offset 0`。
- 已试：字节流与 legacy FIFO 相同格式（SVGA3dCmdHeader+body）。
- 待查：header 附加 flags/字段（8 字节对齐？）、context-0 CB 前导命令、payload 构造差异。

### P2：DX 激活之外的验证
- CLEAR → SURFACE_DMA(READ_HOST_VRAM) → 客体校验品红。P1 解决后即可跑通。

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
