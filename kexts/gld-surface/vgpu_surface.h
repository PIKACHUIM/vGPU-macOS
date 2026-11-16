#ifndef VGPU_SURFACE_H
#define VGPU_SURFACE_H

#include <IOKit/IOReturn.h>
#include <stddef.h>
#include <stdint.h>

/* 本模块自有 API，不是 Apple GLD ABI。所有连接共享内核的同一个目标。 */
#define VGPU_SURFACE_WIDTH 256U
#define VGPU_SURFACE_HEIGHT 256U
#define VGPU_SURFACE_BGRX8 1U
#define VGPU_SURFACE_RGBA8_OUTPUT 2U

typedef struct vgpu_surface vgpu_surface;
typedef struct {
    uint32_t x, y, width, height;
} vgpu_surface_rect;
typedef struct {
    size_t row_length;
    size_t stride_bytes;
    size_t skip_rows;
    size_t skip_pixels;
    size_t alignment;
} vgpu_surface_pack;

/* 仅接受 256x256 / BGRX8 backing；创建连接不创建独立 GPU 资源。
 * 每个并发调用者须持有一份引用；retain 仅可用于仍持有引用的对象。
 * close 可与操作并发，等待本连接正在进行的 IO 完成；release 后不可再使用。
 * 输入/输出内存由调用者保持有效且不得在调用期间并发修改。
 * 禁止通过 pthread cancellation 中断本模块同步操作。
 */
IOReturn vgpu_surface_open(uint32_t width, uint32_t height, uint32_t backing,
                           vgpu_surface **out);
IOReturn vgpu_surface_retain(vgpu_surface *surface);
void vgpu_surface_release(vgpu_surface *surface);
IOReturn vgpu_surface_close(vgpu_surface *surface);
IOReturn vgpu_surface_clear(vgpu_surface *surface, const float rgba[4],
                            uint64_t *generation);
/* 仅接受非空、完全在目标内的矩形，坐标采用 selector 1 的行顺序，不翻转。
 * stride!=0 优先；否则 align_up((row_length ?: width)*4, alignment)。
 * alignment 仅 1/2/4/8。允许行重叠，按递增行号复制；skip 不改变源坐标。
 * capacity 是从 destination 开始实际可写的字节数，padding 保持原值。
 * 失败不写 destination；kIOReturnNotReady 包括 stale generation/设备未就绪。
 * 成功表示最终 selector 1 校验时的完整快照；返回后其他连接仍可提交。
 * 输出 RGBA8，BGRX 的 X 不当作 alpha，alpha 明确写 255。
 */
IOReturn vgpu_surface_read(vgpu_surface *surface, uint64_t generation,
                           vgpu_surface_rect rect, uint32_t output_format,
                           vgpu_surface_pack pack, void *destination,
                           size_t capacity);
#endif
