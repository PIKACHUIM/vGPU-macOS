#include "../../gld-surface/vgpu_surface.h"
#include <stdlib.h>

/* 仅替代平台 surface 边界；context 与 objects 都链接仓库实际源文件。
 * 不执行或模拟成功 clear/read，避免把 CPU 生命周期测试当作 GPU 支持。 */
struct vgpu_surface { unsigned refs; };
unsigned vgo_fixture_surfaces;
int vgo_fixture_fail_open;
IOReturn vgpu_surface_open(uint32_t w, uint32_t h, uint32_t f, vgpu_surface **out) {
    if (!out) return kIOReturnBadArgument;
    *out = NULL;
    if (vgo_fixture_fail_open) return kIOReturnNotReady;
    if (w != 256 || h != 256 || f != VGPU_SURFACE_BGRX8) return kIOReturnUnsupported;
    vgpu_surface *s = calloc(1, sizeof(*s));
    if (!s) return kIOReturnNoMemory;
    s->refs = 1; ++vgo_fixture_surfaces; *out = s;
    return kIOReturnSuccess;
}
void vgpu_surface_release(vgpu_surface *s) {
    if (s && !--s->refs) { --vgo_fixture_surfaces; free(s); }
}
IOReturn vgpu_surface_clear(vgpu_surface *s, const float rgba[4], uint64_t *generation) {
    (void)s; (void)rgba; (void)generation;
    return kIOReturnUnsupported;
}
IOReturn vgpu_surface_read(vgpu_surface *s, uint64_t generation, vgpu_surface_rect rect,
                          uint32_t format, vgpu_surface_pack pack, void *out, size_t n) {
    (void)s; (void)generation; (void)rect; (void)format; (void)pack; (void)out; (void)n;
    return kIOReturnUnsupported;
}
