#ifndef VGPU_SURFACE_WIRE_H
#define VGPU_SURFACE_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vgpu_surface_wire_status {
    VGPU_SURFACE_WIRE_PARSED = 0,
    VGPU_SURFACE_WIRE_INVALID = 1,
    VGPU_SURFACE_WIRE_UNSUPPORTED = 2,
    VGPU_SURFACE_WIRE_OVERFLOW = 3,
    VGPU_SURFACE_WIRE_ALIAS = 4
};

enum vgpu_surface_wire_mode_class {
    VGPU_SURFACE_MODE_NONE = 0,
    VGPU_SURFACE_MODE_CGL_00100024 = 1,
    VGPU_SURFACE_MODE_EXPLICIT_BGRA_0000000A = 2
};

struct vgpu_surface_wire_message {
    uint32_t connection_type;
    uint32_t selector;
    const uint64_t *scalar_input;
    size_t scalar_input_count;
    const void *structure_input;
    size_t structure_input_size;
    void *scalar_output;
    size_t scalar_output_count;
    void *structure_output;
    size_t structure_output_size;
};

/* 全部是规范化请求字段，不包含 backing、授权、内部 registry ID 或格式映射。 */
struct vgpu_surface_wire_request {
    uint32_t selector;
    uint32_t external_accel_id;
    uint32_t mode;
    uint32_t mode_class;
    uint32_t flags;
    uint32_t framebuffer_index;
    uint32_t rectangle_count;
    uint32_t bounds_x;
    uint32_t bounds_y;
    uint32_t bounds_width;
    uint32_t bounds_height;
    uint32_t rectangle_x;
    uint32_t rectangle_y;
    uint32_t rectangle_width;
    uint32_t rectangle_height;
};

/*
 * 仅接受 type0、selector7/9 的研究白名单。PARSED 不是设备操作成功。
 * scalar 为本机 uint64_t 元素；region 是 x86_64 小端 wire 字节，可未对齐。
 * 零长度输入/输出要求 NULL；任何失败均不改写 request。
 * request 不得与 message、scalar 或 region 有任何重叠。
 * 调用方必须提供实际可访问、调用期间稳定的内核缓冲区；本函数不验证
 * 虚拟内存映射、不复制用户地址，也不负责并发、生命周期或连接状态机。
 * external_accel_id 仅为非零 uint32 标签，不能作为授权凭证或内部单调 ID。
 */
enum vgpu_surface_wire_status vgpu_surface_wire_parse(
    const struct vgpu_surface_wire_message *message,
    struct vgpu_surface_wire_request *request);

#ifdef __cplusplus
}
#endif
#endif
