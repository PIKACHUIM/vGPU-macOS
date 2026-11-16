#include "vgpu_texture_bgra.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == sizeof(vgc_handle), "需要 64 位句柄载体");

typedef struct image_ref image_ref;
typedef struct bridge bridge;
struct image_ref {
    image_ref *next;
    vgc_handle image;
    int pending;
};
struct bridge {
    bridge *next;
    vgc_handle share;
    vgt_handle texture_share;
    size_t refs;
    image_ref *images;
};
static bridge *bridges;

static vgt_status status(IOReturn r)
{
    if (r == kIOReturnSuccess) return VGT_OK;
    if (r == kIOReturnUnsupported) return VGT_UNSUPPORTED;
    if (r == kIOReturnNoMemory) return VGT_NO_MEMORY;
    if (r == kIOReturnBadArgument) return VGT_BAD_HANDLE;
    return VGT_BACKEND_FAILED;
}
/* 释放失败时节点本身就是拥有引用的待释放记录，不在失败路径分配内存。 */
static IOReturn finish(bridge *b)
{
    if (b->refs || b->images) return kIOReturnSuccess;
    IOReturn r = vgc_release(b->share);
    if (r) return r;
    bridge **p = &bridges;
    while (*p != b) p = &(*p)->next;
    *p = b->next;
    free(b);
    return kIOReturnSuccess;
}
static vgt_status lease_retain(void *context)
{
    bridge *b = context;
    if (b->refs == SIZE_MAX) return VGT_OVERFLOW;
    ++b->refs;
    return VGT_OK;
}
static void lease_release(void *context)
{
    bridge *b = context;
    --b->refs;
    (void)finish(b);
}
static vgt_status create_image(void *context, const vgt_image_desc *desc,
                               vgt_backing *out)
{
    bridge *b = context;
    memset(out, 0, sizeof(*out));
    /* 当前 surface/context 没有 upload，任何 CPU 源都不能丢弃后成功。 */
    if (desc->cpu.data || desc->cpu.length) return VGT_UNSUPPORTED;
    if (desc->width != VGPU_SURFACE_WIDTH || desc->height != VGPU_SURFACE_HEIGHT ||
        desc->depth != 1 || desc->format != VGT_FORMAT_RGBA8_UNORM || desc->samples != 1)
        return VGT_UNSUPPORTED;
    image_ref *ref = calloc(1, sizeof(*ref));
    if (!ref) return VGT_NO_MEMORY;
    /* vgc samples=0 表示非 MSAA；vgt samples=1 表示一个样本。 */
    vgc_image_desc d = {desc->width, desc->height, VGPU_SURFACE_BGRA8, 0};
    IOReturn r = vgc_image_create(b->share, d, &ref->image);
    if (r) {
        /* vgc 失败不导出 image，内部部分创建回滚由 vgc drain 持有。 */
        free(ref);
        return status(r);
    }
    ref->next = b->images;
    b->images = ref;
    out->resource = (void *)(uintptr_t)ref->image;
    out->width = desc->width;
    out->height = desc->height;
    out->format = desc->format;
    out->samples = 1;
    /* 固定 BGRA8 的线性读回布局，不是可 CPU 映射的 GPU 指针。 */
    out->row_bytes = (size_t)desc->width * 4;
    out->byte_size = out->row_bytes * desc->height;
    return VGT_OK;
}
static void destroy_image(void *context, vgt_backing *image)
{
    bridge *b = context;
    vgc_handle id = (vgc_handle)(uintptr_t)image->resource;
    image_ref **p = &b->images;
    while (*p && (*p)->image != id) p = &(*p)->next;
    if (*p) {
        image_ref *ref = *p;
        if (vgc_release(ref->image)) ref->pending = 1;
        else { *p = ref->next; free(ref); }
    }
    memset(image, 0, sizeof(*image));
}
vgt_status vgt_bgra_share_create(vgc_handle context_share, vgt_handle *out)
{
    if (!out) return VGT_INVALID;
    *out = 0;
    bridge *b = calloc(1, sizeof(*b));
    if (!b) return VGT_NO_MEMORY;
    IOReturn r = vgc_retain(context_share);
    if (r) { free(b); return status(r); }
    b->share = context_share;
    b->refs = 1;
    b->next = bridges;
    bridges = b;
    vgt_backend backend = {0};
    backend.version = VGT_BACKEND_VERSION;
    backend.max_width = VGPU_SURFACE_WIDTH;
    backend.max_height = VGPU_SURFACE_HEIGHT;
    backend.rgba8_alpha = 1;
    backend.context = b;
    backend.lease = (vgt_lease){b, lease_retain, lease_release};
    backend.create_image = create_image;
    backend.destroy_image = destroy_image;
    vgt_status st = vgt_share_create(&backend, out);
    if (st == VGT_OK) b->texture_share = *out;
    lease_release(b);
    return st;
}
vgt_status vgt_bgra_texture_image_get(vgc_handle context_share, vgt_handle texture,
                                      vgc_handle *out)
{
    if (!out) return VGT_INVALID;
    vgt_texture_state state;
    vgt_status st = vgt_texture_state_get(texture, &state);
    if (st != VGT_OK) return st;
    for (bridge *b = bridges; b; b = b->next) {
        if (!b->refs || b->texture_share != state.share) continue;
        if (b->share != context_share) return VGT_OWNER_MISMATCH;
        if (!state.has_backing) return VGT_NO_BACKEND;
        vgc_handle id = (vgc_handle)(uintptr_t)state.backing.resource;
        for (image_ref *ref = b->images; ref; ref = ref->next) {
            if (ref->image == id && !ref->pending) { *out = id; return VGT_OK; }
        }
        return VGT_BACKEND_FAILED;
    }
    return VGT_OWNER_MISMATCH;
}
IOReturn vgt_bgra_drain_releases(void)
{
    IOReturn result = kIOReturnSuccess;
    for (bridge *b = bridges, *next; b; b = next) {
        next = b->next;
        image_ref **p = &b->images;
        while (*p) {
            image_ref *ref = *p;
            if (!ref->pending) { p = &ref->next; continue; }
            IOReturn r = vgc_release(ref->image);
            if (r) { if (!result) result = r; p = &ref->next; }
            else { *p = ref->next; free(ref); }
        }
        IOReturn r = finish(b);
        if (r && !result) result = r;
    }
    IOReturn r = vgc_drain_releases();
    return result ? result : r;
}
