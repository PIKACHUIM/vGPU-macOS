#include "../../gld-surface/vgpu_surface.h"
#include <stdlib.h>

/* 仅替代平台 surface 边界；context 与 objects 都链接仓库实际源文件。
 * 不执行或模拟成功 clear/read，避免把 CPU 生命周期测试当作 GPU 支持。 */
/* 显式启用的 BGRA CPU 生命周期模型；clear/read 始终不支持，不证明 GPU 能力。 */
struct vgpu_surface_owner { unsigned images; };
struct vgpu_surface_image { vgpu_surface_owner *owner; };
unsigned vgo_fixture_owners, vgo_fixture_images, vgo_fixture_owner_release_calls;
int vgo_fixture_bgra, vgo_fixture_fail_owner_release;
unsigned vgo_fixture_default_open_calls, vgo_fixture_registry_open_calls;
uint64_t vgo_fixture_last_registry_id;
static IOReturn owner_allocate(vgpu_surface_owner **out) {
    if (!out) return kIOReturnBadArgument;
    *out = NULL;
    if (!vgo_fixture_bgra) return kIOReturnUnsupported;
    *out = calloc(1, sizeof(**out));
    if (!*out) return kIOReturnNoMemory;
    ++vgo_fixture_owners;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_owner_open(vgpu_surface_owner **out) {
    ++vgo_fixture_default_open_calls;
    return owner_allocate(out);
}
IOReturn vgpu_surface_owner_open_registry(uint64_t registry_id, vgpu_surface_owner **out) {
    ++vgo_fixture_registry_open_calls;
    vgo_fixture_last_registry_id = registry_id;
    if (!out) return kIOReturnBadArgument;
    *out = NULL;
    if (!registry_id || registry_id == 2) return kIOReturnBadArgument;
    if (registry_id != 1) return kIOReturnNotFound;
    return owner_allocate(out);
}
IOReturn vgpu_surface_owner_release(vgpu_surface_owner *owner) {
    if (!owner) return kIOReturnSuccess;
    ++vgo_fixture_owner_release_calls;
    if (vgo_fixture_fail_owner_release || owner->images) return kIOReturnNotReady;
    --vgo_fixture_owners;
    free(owner);
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_create(vgpu_surface_owner *owner, uint32_t w,
                                  uint32_t h, uint32_t f, vgpu_surface_image **out) {
    if (!out) return kIOReturnBadArgument;
    *out = NULL;
    if (!vgo_fixture_bgra || !owner || w != 256 || h != 256 || f != VGPU_SURFACE_BGRA8)
        return kIOReturnUnsupported;
    *out = calloc(1, sizeof(**out));
    if (!*out) return kIOReturnNoMemory;
    (*out)->owner = owner;
    ++owner->images; ++vgo_fixture_images;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_release(vgpu_surface_image *image) {
    if (image) {
        --image->owner->images; --vgo_fixture_images;
        free(image);
    }
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_clear(vgpu_surface_image *image, const float rgba[4], uint64_t *generation) {
    (void)image; (void)rgba; (void)generation;
    return kIOReturnUnsupported;
}
IOReturn vgpu_surface_image_read(vgpu_surface_image *image, uint64_t generation,
                                vgpu_surface_rect rect, uint32_t format,
                                vgpu_surface_pack pack, void *out, size_t n) {
    (void)image; (void)generation; (void)rect; (void)format; (void)pack; (void)out; (void)n;
    return kIOReturnUnsupported;
}
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
/* 元数据 fixture 不执行绘制；明确拒绝，不能充当正式 GPU 后端。 */
IOReturn vgpu_surface_image_draw_triangle(vgpu_surface_image *image,
                                         const float vertices[3][4], uint64_t *generation) {
    (void)image; (void)vertices; (void)generation;
    return kIOReturnUnsupported;
}
IOReturn vgpu_surface_read(vgpu_surface *s, uint64_t generation, vgpu_surface_rect rect,
                          uint32_t format, vgpu_surface_pack pack, void *out, size_t n) {
    (void)s; (void)generation; (void)rect; (void)format; (void)pack; (void)out; (void)n;
    return kIOReturnUnsupported;
}
