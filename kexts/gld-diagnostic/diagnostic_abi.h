#ifndef VGPU_GLD_DIAGNOSTIC_ABI_H
#define VGPU_GLD_DIAGNOSTIC_ABI_H

#include <stdint.h>

#if !defined(__APPLE__) || !defined(__x86_64__)
#error 此诊断仅支持已核验的 macOS x86_64 调用点
#endif

#define GLD_DIAGNOSTIC_DRIVER_FIELD UINT32_C(0x7e00)
#define GLD_DIAGNOSTIC_DEVICE_REJECT INT32_C(-1)
/* 仅对应 24G419 gfxAddDevice 的 0x1b0 分配及第三参数 +0x20。
 * 这是负向路径可清零的调用者尾区，不是已恢复的公开结构体。 */
#define GLD_DIAGNOSTIC_DEVICE_OUTPUT_BYTES 0x190u
#define GLD_EXPORT __attribute__((visibility("default")))

/* 两个回调只接收地址位，不声明、调用或保存未知签名的函数。
 * 初始化/终止的 void 表示本实现不提供返回值；调用者均忽略返回。
 * 不据此宣称已恢复原始私有 C 声明。 */
typedef void (*gld_diag_initialize_fn)(const uint32_t *, const uint8_t *,
                                       uint32_t, uintptr_t, uintptr_t, uint32_t);
typedef int32_t (*gld_diag_version_fn)(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
typedef int32_t (*gld_diag_device_fn)(void **, uint32_t, void *);
typedef void (*gld_diag_terminate_fn)(void);

GLD_EXPORT void gldInitializeLibrary(const uint32_t *services, const uint8_t *bytes,
                                    uint32_t mask, uintptr_t flush_address,
                                    uintptr_t bind_address, uint32_t flags);
GLD_EXPORT int32_t gldGetVersion(uint32_t *v0, uint32_t *v1, uint32_t *v2, uint32_t *v3);
GLD_EXPORT int32_t gldCreateDevice(void **device_out, uint32_t mask, void *caller_tail_out);
GLD_EXPORT void gldTerminateLibrary(void);

#endif
