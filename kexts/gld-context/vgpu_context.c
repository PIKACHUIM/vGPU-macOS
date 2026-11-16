#include "vgpu_context.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == 8, "需要 64 位宿主");
enum kind { SHARE, CONTEXT, IMAGE, FRAMEBUFFER };
struct object {
    struct object *next;
    vgc_handle id;
    size_t refs;
    enum kind kind;
    struct object *share;
    union {
        struct { vgpu_surface *surface; } s;
        struct { vgc_view h, b; struct object *draw, *read; vgc_state state; } c;
        struct { vgc_image_desc desc; uint64_t generation; } i;
        struct { struct object *image; vgc_state state; } f;
    } u;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct object *objects, *leased_image;
static uint64_t next_id = 1;

static struct object *lookup(vgc_handle id, enum kind kind)
{
    for (struct object *o = objects; o; o = o->next)
        if (o->id == id && o->kind == kind) return o;
    return NULL;
}
static struct object *lookup_any(vgc_handle id)
{
    for (struct object *o = objects; o; o = o->next)
        if (o->id == id) return o;
    return NULL;
}
static bool hold(struct object *o)
{
    if (o->refs == SIZE_MAX) return false;
    ++o->refs;
    return true;
}
static void drop(struct object *o)
{
    if (!o || --o->refs) return;
    struct object **p = &objects;
    while (*p != o) p = &(*p)->next;
    *p = o->next;
    if (o->kind == CONTEXT) {
        drop(o->u.c.draw);
        drop(o->u.c.read);
    } else if (o->kind == FRAMEBUFFER) {
        drop(o->u.f.image);
    } else if (o->kind == IMAGE) {
        if (leased_image == o) leased_image = NULL;
    } else {
        vgpu_surface_release(o->u.s.surface);
    }
    drop(o->share);
    free(o);
}
static struct object *allocate(enum kind kind, struct object *share)
{
    if (!next_id || (share && share->refs == SIZE_MAX)) return NULL;
    struct object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->id = next_id++;
    o->refs = 1;
    o->kind = kind;
    o->share = share;
    if (share) (void)hold(share);
    o->next = objects;
    objects = o;
    return o;
}
static bool view_ok(vgc_view v, size_t length)
{
    return v.data && v.length >= length && v.length <= UINTPTR_MAX - (uintptr_t)v.data;
}
static uint32_t u32(vgc_view v, size_t offset)
{
    uint32_t result;
    memcpy(&result, (const unsigned char *)v.data + offset, sizeof(result));
    return result;
}
static uint64_t u64(vgc_view v, size_t offset)
{
    uint64_t result;
    memcpy(&result, (const unsigned char *)v.data + offset, sizeof(result));
    return result;
}
IOReturn vgc_share_create(vgc_handle *out)
{
    if (!out) return kIOReturnBadArgument;
    *out = 0;
    const uint16_t endian = 1;
    if (*(const unsigned char *)&endian != 1) return kIOReturnUnsupported;
    pthread_mutex_lock(&lock);
    struct object *o = allocate(SHARE, NULL);
    IOReturn r = kIOReturnNoMemory;
    if (o) {
        r = vgpu_surface_open(256, 256, VGPU_SURFACE_BGRX8, &o->u.s.surface);
        if (r) drop(o); else *out = o->id;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_context_create(vgc_handle share, vgc_view h, vgc_view b, vgc_handle *out)
{
    if (!out) return kIOReturnBadArgument;
    *out = 0;
    if (!view_ok(h, VGC_H_LENGTH) || !view_ok(b, VGC_B_LENGTH)) return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    struct object *s = lookup(share, SHARE), *o = NULL;
    if (s) o = allocate(CONTEXT, s);
    if (o) { o->u.c.h = h; o->u.c.b = b; *out = o->id; }
    pthread_mutex_unlock(&lock);
    return o ? kIOReturnSuccess : s ? kIOReturnNoMemory : kIOReturnBadArgument;
}
IOReturn vgc_image_create(vgc_handle share, vgc_image_desc desc, vgc_handle *out)
{
    if (!out) return kIOReturnBadArgument;
    *out = 0;
    if (desc.width != 256 || desc.height != 256 ||
        desc.backing_format != VGPU_SURFACE_BGRX8 || desc.samples != 0)
        return kIOReturnUnsupported;
    pthread_mutex_lock(&lock);
    struct object *s = lookup(share, SHARE), *o = NULL;
    IOReturn r = !s ? kIOReturnBadArgument : leased_image ? kIOReturnExclusiveAccess : kIOReturnNoMemory;
    if (s && !leased_image) o = allocate(IMAGE, s);
    if (o) { o->u.i.desc = desc; leased_image = o; *out = o->id; r = kIOReturnSuccess; }
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_framebuffer_create(vgc_handle share, vgc_handle *out)
{
    if (!out) return kIOReturnBadArgument;
    *out = 0;
    pthread_mutex_lock(&lock);
    struct object *s = lookup(share, SHARE), *o = NULL;
    if (s) o = allocate(FRAMEBUFFER, s);
    if (o) { o->u.f.state.dirty = VGC_ATTACHMENT_DIRTY; *out = o->id; }
    pthread_mutex_unlock(&lock);
    return o ? kIOReturnSuccess : s ? kIOReturnNoMemory : kIOReturnBadArgument;
}
IOReturn vgc_retain(vgc_handle id)
{
    pthread_mutex_lock(&lock);
    struct object *o = lookup_any(id);
    IOReturn r = !o ? kIOReturnBadArgument : hold(o) ? kIOReturnSuccess : kIOReturnNoResources;
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_release(vgc_handle id)
{
    pthread_mutex_lock(&lock);
    struct object *o = lookup_any(id);
    IOReturn r = o ? kIOReturnSuccess : kIOReturnBadArgument;
    if (o) drop(o);
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_framebuffer_attach(vgc_handle id, uint32_t attachment, vgc_handle image)
{
    pthread_mutex_lock(&lock);
    struct object *f = lookup(id, FRAMEBUFFER), *i = lookup(image, IMAGE);
    IOReturn r = kIOReturnBadArgument;
    if (f) {
        f->u.f.state.dirty |= VGC_ATTACHMENT_DIRTY;
        f->u.f.state.failed = 1;
        if (attachment) r = kIOReturnUnsupported;
        else if (!image || (i && i->share == f->share)) {
            if (i && !hold(i)) r = kIOReturnNoResources;
            else {
                drop(f->u.f.image);
                f->u.f.image = i;
                r = kIOReturnSuccess;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_framebuffer_dirty(vgc_handle id, uint32_t dirty)
{
    pthread_mutex_lock(&lock);
    struct object *f = lookup(id, FRAMEBUFFER);
    if (f) f->u.f.state.dirty |= dirty;
    pthread_mutex_unlock(&lock);
    return f ? kIOReturnSuccess : kIOReturnBadArgument;
}
static IOReturn load(struct object *f)
{
    f->u.f.state.failed = 1;
    if (f->u.f.state.dirty & ~VGC_ATTACHMENT_DIRTY) return kIOReturnUnsupported;
    if (!f->u.f.image) return kIOReturnNotReady;
    if (f->u.f.image != leased_image) return kIOReturnNotReady;
    f->u.f.state.dirty &= ~VGC_ATTACHMENT_DIRTY;
    f->u.f.state.failed = 0;
    return kIOReturnSuccess;
}
IOReturn vgc_framebuffer_load(vgc_handle id)
{
    pthread_mutex_lock(&lock);
    struct object *f = lookup(id, FRAMEBUFFER);
    IOReturn r = f ? load(f) : kIOReturnBadArgument;
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_framebuffer_state(vgc_handle id, vgc_state *out)
{
    if (!out) return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    struct object *f = lookup(id, FRAMEBUFFER);
    if (f) {
        *out = f->u.f.state;
        out->completed_generation = f->u.f.image ? f->u.f.image->u.i.generation : 0;
    }
    pthread_mutex_unlock(&lock);
    return f ? kIOReturnSuccess : kIOReturnBadArgument;
}
IOReturn vgc_context_update(vgc_handle id, uint32_t *dirty)
{
    if (!dirty) return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    struct object *c = lookup(id, CONTEXT), *draw = NULL, *read = NULL;
    uint32_t flags = *dirty;
    IOReturn r = kIOReturnBadArgument;
    if (!c) goto done;
    if (flags & ~(VGC_DRAW_DIRTY | VGC_READ_DIRTY)) { r = kIOReturnUnsupported; goto done; }
    if (flags & VGC_DRAW_DIRTY) {
        uint64_t handle = u64(c->u.c.b, 0x4a0);
        draw = lookup(handle, FRAMEBUFFER);
        if (handle && (!draw || draw->share != c->share)) goto done;
    }
    if (flags & VGC_READ_DIRTY) {
        uint64_t handle = u64(c->u.c.b, 0x4a8);
        read = lookup(handle, FRAMEBUFFER);
        if (handle && (!read || read->share != c->share)) goto done;
    }
    if (draw && !hold(draw)) { r = kIOReturnNoResources; goto done; }
    if (read && !hold(read)) { drop(draw); r = kIOReturnNoResources; goto done; }
    if (flags & VGC_DRAW_DIRTY) { drop(c->u.c.draw); c->u.c.draw = draw; }
    if (flags & VGC_READ_DIRTY) { drop(c->u.c.read); c->u.c.read = read; }
    *dirty &= ~(VGC_DRAW_DIRTY | VGC_READ_DIRTY);
    r = kIOReturnSuccess;
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_clear(vgc_handle id, const vgc_clear_desc *desc, uint64_t *generation)
{
    if (!desc || !generation) return kIOReturnBadArgument;
    vgc_clear_desc command = *desc;
    pthread_mutex_lock(&lock);
    struct object *c = lookup(id, CONTEXT);
    IOReturn r = kIOReturnBadArgument;
    if (!c) goto done;
    c->u.c.state.dirty = 1;
    c->u.c.state.failed = 1;
    if (command.internal_mask != 1 || command.color_write_mask != 15 ||
        command.scissor_enabled || command.srgb_enabled) { r = kIOReturnUnsupported; goto done; }
    if (!c->u.c.draw) { r = kIOReturnNotReady; goto done; }
    r = load(c->u.c.draw);
    if (r) goto done;
    uint64_t completed = 0;
    r = vgpu_surface_clear(c->share->u.s.surface, command.rgba, &completed);
    if (!r) {
        c->u.c.draw->u.f.image->u.i.generation = completed;
        c->u.c.state.completed_generation = completed;
        c->u.c.state.dirty = 0;
        c->u.c.state.failed = 0;
        *generation = completed;
    }
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_read(vgc_handle id, uint64_t generation, vgpu_surface_rect rect,
                  void *destination, size_t capacity)
{
    pthread_mutex_lock(&lock);
    struct object *c = lookup(id, CONTEXT);
    IOReturn r = kIOReturnBadArgument;
    if (!c) goto done;
    if (!c->u.c.read) { r = kIOReturnNotReady; goto done; }
    r = load(c->u.c.read);
    if (r) goto done;
    if (!generation || generation != c->u.c.read->u.f.image->u.i.generation) {
        r = kIOReturnNotReady; goto done;
    }
    vgc_view h = c->u.c.h;
    const unsigned char *bytes = h.data;
    if (bytes[0x4ee8] || bytes[0x4ee9]) { r = kIOReturnUnsupported; goto done; }
    vgpu_surface_pack pack = { u32(h, 0x4ec8), u32(h, 0x4ecc),
        u32(h, 0x4ed8), u32(h, 0x4edc), u32(h, 0x4ee4) };
    r = vgpu_surface_read(c->share->u.s.surface, generation, rect,
                         VGPU_SURFACE_RGBA8_OUTPUT, pack, destination, capacity);
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_flush_completed(vgc_handle id, uint64_t *out)
{
    if (!out) return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    struct object *c = lookup(id, CONTEXT);
    IOReturn r = kIOReturnBadArgument;
    if (!c) goto done;
    uint64_t generation = c->u.c.state.completed_generation;
    if (!generation || c->u.c.state.dirty || c->u.c.state.failed) {
        r = kIOReturnNotReady; goto done;
    }
    unsigned char pixel[4];
    vgpu_surface_rect rect = { 0, 0, 1, 1 };
    vgpu_surface_pack pack = { 0, 0, 0, 0, 4 };
    r = vgpu_surface_read(c->share->u.s.surface, generation, rect,
                         VGPU_SURFACE_RGBA8_OUTPUT, pack, pixel, sizeof(pixel));
    if (!r) *out = generation;
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_context_state(vgc_handle id, vgc_state *out)
{
    if (!out) return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    struct object *c = lookup(id, CONTEXT);
    if (c) *out = c->u.c.state;
    pthread_mutex_unlock(&lock);
    return c ? kIOReturnSuccess : kIOReturnBadArgument;
}
