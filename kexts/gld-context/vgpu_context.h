#ifndef VGPU_CONTEXT_H
#define VGPU_CONTEXT_H
#include "../gld-surface/vgpu_surface.h"

/* 独立 API，不导出 Apple GLD/CGL ABI。仅支持 64 位小端宿主。
 * handle 为本模块单调编号，不是指针；0 表示解绑。所有操作由模块锁串行化。
 * release 释放调用者的一份引用；对象链中的强引用允许调用者提前 release。
 * 借用 H/B 在 context 生命周期内有效，调用期间不得并发修改。
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
IOReturn vgc_context_create(vgc_handle share, vgc_view h, vgc_view b, vgc_handle *out);
IOReturn vgc_image_create(vgc_handle share, vgc_image_desc desc, vgc_handle *out);
IOReturn vgc_framebuffer_create(vgc_handle share, vgc_handle *out);
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
/* 输出格式固定 RGBA8，alpha=255；capacity 为 destination 的真实可写容量。
 * generation 必须匹配该 image 的最后成功 clear，后端再次验证外部 stale。
 * H pack 每次读取，swap/lsb 非零拒绝；保留 surface 的范围/溢出/事务语义。 */
IOReturn vgc_read(vgc_handle context, uint64_t generation, vgpu_surface_rect rect,
                  void *destination, size_t capacity);
/* 同步后端没有异步队列/fence。验证已完成 generation 当前仍可读取。
 * out 只在成功时写入；没有 clear 完成、pending 失败或 stale 返回失败。 */
IOReturn vgc_flush_completed(vgc_handle context, uint64_t *out);
IOReturn vgc_context_state(vgc_handle context, vgc_state *out);
#endif
