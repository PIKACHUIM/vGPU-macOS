#ifndef VGPU_CONTEXT_H
#define VGPU_CONTEXT_H
#include "../gld-surface/vgpu_surface.h"

/* 独立 API，不导出 Apple GLD/CGL ABI。仅支持 64 位小端宿主。
 * handle 为本模块单调编号，不是指针；0 表示解绑。所有操作由模块锁串行化。
 * release 释放调用者的一份引用；对象链中的强引用允许调用者提前 release。
 * 借用 H/B 在 context 生命周期内有效，调用期间不得并发修改。
 * 所有输出缓冲区不得与任一存活 context 借用的 H/B 重叠，拒绝时不写输出。
 * 禁止 pthread cancellation 中断调用。进程内 lease 不隔离其他进程/模块。
 */
typedef uint64_t vgc_handle;
typedef struct { const void *data; size_t length; } vgc_view;
typedef struct { uint32_t width, height, backing_format, samples; } vgc_image_desc;
typedef struct {
    uint32_t internal_mask; /* bit0 为唯一颜色目标，不是通道 mask */
    uint32_t color_write_mask; /* 显式 RGBA 通道，必须为 15 */
    uint32_t scissor_enabled, srgb_enabled;
    float rgba[4];
} vgc_clear_desc;
typedef struct {
    uint32_t dirty, failed;
    uint64_t completed_generation;
} vgc_state;
#define VGC_H_LENGTH ((size_t)0x4eea)
#define VGC_B_LENGTH ((size_t)0x4b0)
#define VGC_DRAW_DIRTY UINT32_C(0x40000000)
#define VGC_READ_DIRTY UINT32_C(0x80000000)
#define VGC_ATTACHMENT_DIRTY UINT32_C(1)

IOReturn vgc_share_create(vgc_handle *out);
/* 显式双资源 BGRA8 路径；share 持 owner，每个 image 持独立后端资源。
 * 两槽容量由后端全局管理，旧 BGRX 单 lease 语义不变。 */
IOReturn vgc_share_create_bgra(vgc_handle *out);
/* 仅连接指定的 accelerator registry ID；零/无效 ID 拒绝，不回退自动发现。 */
IOReturn vgc_share_create_bgra_registry(uint64_t registry_id, vgc_handle *out);
/* 自动析构/创建回滚的后端释放失败会保留内部对象及 owner 引用。
 * 调用此函数重试，成功表示没有待释放对象；失败可继续重试。
 * 显式 vgc_release 自身后端释放失败不消费引用，原 handle 可重试。
 * 父对象 release 成功不保证其子对象后端释放成功；收尾必须 drain。
 * 进程退出前必须释放所有公开引用并 drain 成功，不能遗忘失败状态。 */
IOReturn vgc_drain_releases(void);
IOReturn vgc_context_create(vgc_handle share, vgc_view h, vgc_view b, vgc_handle *out);
IOReturn vgc_image_create(vgc_handle share, vgc_image_desc desc, vgc_handle *out);
IOReturn vgc_framebuffer_create(vgc_handle share, vgc_handle *out);
/* 在同一锁内严格验证 SHARE 类别并持有一份强引用；失败不消费/新增引用。
 * 非 share/失效句柄返回 BadArgument，引用计数溢出返回 NoResources。 */
IOReturn vgc_share_retain(vgc_handle share);
IOReturn vgc_retain(vgc_handle handle);
IOReturn vgc_release(vgc_handle handle);
/* 仅 attachment=0/color0，image=0 为解绑，未知 dirty 保留并拒绝 load。
 * 显式替换附件也不消除此前未知 dirty；没有忽略未知工作的 reset 接口。 */
IOReturn vgc_framebuffer_attach(vgc_handle fb, uint32_t attachment, vgc_handle image);
IOReturn vgc_framebuffer_dirty(vgc_handle fb, uint32_t dirty);
IOReturn vgc_framebuffer_load(vgc_handle fb);
IOReturn vgc_framebuffer_state(vgc_handle fb, vgc_state *out);
/* 更新读取 B+0x4a0/0x4a8 的本模块 uint64 句柄，事务性验证后持有强引用。
 * 未处理位或失败时 *dirty 保持原值；成功只清除已处理的两个位。 */
IOReturn vgc_context_update(vgc_handle context, uint32_t *dirty);
IOReturn vgc_clear(vgc_handle context, const vgc_clear_desc *desc, uint64_t *generation);
/* 受限内部接口，不表示支持 GL draw 或读取/验证完整 GL 状态。
 * 上层必须先验证：单个三角形、位置直通、固定红色 PS/alpha=1、全目标 viewport、
 * RGBA 全写；无 blend/depth/stencil/scissor/cull/sRGB/MSAA 及其他附加状态。
 * 仅 BGRA share；使用 context_update 已绑定的真实 draw FBO/color0，绝不 clear。
 * 三个 float4 顶点有限 x/y∈[-1,1]、z∈[0,1]、w=1，输入先快照。
 * 允许 vertices 与 generation 重叠；输出不得重叠借用 H/B。
 * 成功更新 image/last/completed_generation 并清 dirty/failed；失败保留 last/代次，
 * 有效 context 的提交失败置 dirty/failed，与 clear 相同。失败不写 generation。
 */
IOReturn vgc_draw_triangle(vgc_handle context, const float vertices[3][4], uint64_t *generation);
/* 输出格式固定 RGBA8，BGRX alpha=255、BGRA 保留 alpha；capacity 为真实可写容量。
 * generation 必须匹配该 image 的最后成功 clear/draw，后端再次验证外部 stale。
 * H pack 每次读取，swap/lsb 非零拒绝；保留 surface 的范围/溢出/事务语义。 */
IOReturn vgc_read(vgc_handle context, uint64_t generation, vgpu_surface_rect rect,
                  void *destination, size_t capacity);
/* 同步后端没有异步队列/fence。验证已完成 generation 当前仍可读取。
 * BGRA 对最后成功提交 image 持强引用，重绑/换 attachment 不改变 flush 目标；
 * 同 image 新提交使旧 context 的 flush stale。失败 clear/draw 保留 last 并置 dirty/failed。
 * BGRX 保留旧单 surface flush 与 lease 生命周期。
 * out 只在成功时写入；没有 clear 完成、pending 失败或 stale 返回失败。 */
IOReturn vgc_flush_completed(vgc_handle context, uint64_t *out);
IOReturn vgc_context_state(vgc_handle context, vgc_state *out);
#endif
