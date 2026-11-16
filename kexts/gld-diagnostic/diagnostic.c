#include "diagnostic_abi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void gldInitializeLibrary(const uint32_t *services, const uint8_t *bytes,
                          uint32_t mask, uintptr_t flush_address,
                          uintptr_t bind_address, uint32_t flags)
{
    /* 不解引用服务数组、不获取 IOKit 连接、不保存或调用回调。 */
    fprintf(stderr, "[VGPU-GLD-DIAG pid=%ld] initialize services=%p bytes=%p "
            "mask=0x%08" PRIx32 " flush=0x%" PRIxPTR " bind=0x%" PRIxPTR
            " flags=0x%08" PRIx32 " mode=reject-all-devices\n",
            (long)getpid(), (const void *)services, (const void *)bytes,
            mask, flush_address, bind_address, flags);
}

int32_t gldGetVersion(uint32_t *v0, uint32_t *v1, uint32_t *v2, uint32_t *v3)
{
    *v0 = 4;
    *v1 = 1;
    *v2 = 0x11;
    *v3 = GLD_DIAGNOSTIC_DRIVER_FIELD;
    fprintf(stderr, "[VGPU-GLD-DIAG pid=%ld] version 4/1/0x11/0x%04" PRIx32
            " result=1 diagnostic-only\n", (long)getpid(), *v3);
    return 1;
}

int32_t gldCreateDevice(void **device_out, uint32_t mask, void *caller_tail_out)
{
    *device_out = NULL;
    memset(caller_tail_out, 0, GLD_DIAGNOSTIC_DEVICE_OUTPUT_BYTES);
    fprintf(stderr, "[VGPU-GLD-DIAG pid=%ld] device mask=0x%08" PRIx32
            " result=%" PRId32 " device=NULL caller-tail-zero=0x190\n",
            (long)getpid(), mask, GLD_DIAGNOSTIC_DEVICE_REJECT);
    return GLD_DIAGNOSTIC_DEVICE_REJECT;
}

void gldTerminateLibrary(void)
{
    /* gfxPluginDisconnect 0x7ffb06178048：无参数，返回值未使用。
     * 本实现没有分配资源或持有外部引用，清理仅记录调用。 */
    fprintf(stderr, "[VGPU-GLD-DIAG pid=%ld] terminate resources=0\n", (long)getpid());
}
