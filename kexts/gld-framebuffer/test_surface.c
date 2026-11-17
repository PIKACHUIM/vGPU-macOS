#include "../gld-surface/vgpu_surface.h"
#include <stdlib.h>

/* 仅供本目录测试链接：双槽 CPU 生命周期替身，绝不接生产或模拟 GPU clear/read。 */
struct vgpu_surface_owner { unsigned refs; };
struct vgpu_surface_image { vgpu_surface_owner *owner; };
unsigned vgf_test_images, vgf_test_owners;
int vgf_test_release_fail;
IOReturn vgpu_surface_owner_open(vgpu_surface_owner **out)
{
    *out = calloc(1, sizeof(**out));
    if (!*out) return kIOReturnNoMemory;
    (*out)->refs = 1;
    ++vgf_test_owners;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_owner_release(vgpu_surface_owner *s)
{
    if (s && !--s->refs) { --vgf_test_owners; free(s); }
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_create(vgpu_surface_owner *s, uint32_t w, uint32_t h,
                                  uint32_t format, vgpu_surface_image **out)
{
    *out = NULL;
    if (w != 256 || h != 256 || format != VGPU_SURFACE_BGRA8) return kIOReturnUnsupported;
    if (vgf_test_images == 2) return kIOReturnNoResources;
    *out = calloc(1, sizeof(**out));
    if (!*out) return kIOReturnNoMemory;
    (*out)->owner = s;
    ++s->refs;
    ++vgf_test_images;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_release(vgpu_surface_image *i)
{
    if (!i) return kIOReturnSuccess;
    if (vgf_test_release_fail) return kIOReturnNotReady;
    (void)vgpu_surface_owner_release(i->owner);
    --vgf_test_images;
    free(i);
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_clear(vgpu_surface_image *i, const float rgba[4], uint64_t *g)
{
    (void)i; (void)rgba; (void)g;
    return kIOReturnUnsupported;
}
IOReturn vgpu_surface_image_read(vgpu_surface_image *i, uint64_t g, vgpu_surface_rect r,
                                uint32_t fmt, vgpu_surface_pack p, void *d, size_t n)
{
    (void)i; (void)g; (void)r; (void)fmt; (void)p; (void)d; (void)n;
    return kIOReturnUnsupported;
}
IOReturn vgpu_surface_open(uint32_t w, uint32_t h, uint32_t f, vgpu_surface **out)
{
    (void)w; (void)h; (void)f;
    *out = NULL;
    return kIOReturnUnsupported;
}
void vgpu_surface_release(vgpu_surface *s) { (void)s; }
IOReturn vgpu_surface_clear(vgpu_surface *s, const float rgba[4], uint64_t *g)
{
    (void)s; (void)rgba; (void)g;
    return kIOReturnUnsupported;
}
IOReturn vgpu_surface_read(vgpu_surface *s, uint64_t g, vgpu_surface_rect r,
                          uint32_t fmt, vgpu_surface_pack p, void *d, size_t n)
{
    (void)s; (void)g; (void)r; (void)fmt; (void)p; (void)d; (void)n;
    return kIOReturnUnsupported;
}
