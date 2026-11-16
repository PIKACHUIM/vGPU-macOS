#ifndef VGPU_GLD_DEVICE_H
#define VGPU_GLD_DEVICE_H

#include <stdint.h>

#if !defined(__APPLE__) || !defined(__x86_64__)
#error 此模块仅支持已核验的 macOS x86_64 ABI
#endif

#define GLD_DEVICE_CONFIG_BYTES 0x190u
#define GLD_DEVICE_RENDERER_BYTES 0x88u
/* 只允许独立失败构建覆盖此类型，正式构建使用已验证的管理员私有连接。 */
#ifndef GLD_DEVICE_OPEN_TYPE
#define GLD_DEVICE_OPEN_TYPE UINT32_C(0x56475055)
#endif
#define GLD_DEVICE_DRIVER_ID UINT32_C(0x00027e00)
#define GLD_DEVICE_EXPORT __attribute__((visibility("default")))

GLD_DEVICE_EXPORT void gldInitializeLibrary(const uint32_t *services,
    const uint8_t *bytes, uint32_t mask, uintptr_t flush_address,
    uintptr_t bind_address, uint32_t flags);
GLD_DEVICE_EXPORT int32_t gldCreateDevice(void **device_out, uint32_t mask,
                                         void *config_out);
GLD_DEVICE_EXPORT int32_t gldDestroyDevice(void *device);
GLD_DEVICE_EXPORT int32_t gldPopulateRendererInfo(void *device, void *renderer_out);
GLD_DEVICE_EXPORT void gldTerminateLibrary(void);

/* 本模块专用接口，不是恢复出的苹果 ABI，不返回可关闭的借用连接。 */
struct gld_device_status {
    uint64_t registry_id;
    int32_t liveness_status;
    uint32_t reserved;
    uint32_t renderer_id;
    uint32_t display_mask;
    uint32_t connection_type;
    uint32_t connected;
};
struct gld_device_counts {
    uint32_t service_refs;
    uint32_t devices;
    uint32_t connections;
    uint32_t initialized_mask;
    uint64_t retains;
    uint64_t releases;
    uint64_t opens;
    uint64_t closes;
    int32_t initialize_status;
    int32_t backend_status;
    int32_t cleanup_status;
};
GLD_DEVICE_EXPORT int32_t vgpuGLDDeviceGetStatus(void *device,
                                               struct gld_device_status *out);
GLD_DEVICE_EXPORT void vgpuGLDDeviceGetCounts(struct gld_device_counts *out);
#endif
