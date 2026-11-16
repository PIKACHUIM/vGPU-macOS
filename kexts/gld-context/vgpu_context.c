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
        struct { vgpu_surface *surface; vgpu_surface_owner *owner; } s;
        struct { vgc_view h, b; struct object *draw, *read, *last; vgc_state state; } c;
        struct { vgc_image_desc desc; uint64_t generation; vgpu_surface_image *image; } i;
        struct { struct object *image; vgc_state state; } f;
    } u;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct object *objects, *leased_image;
static uint64_t next_id = 1;

static struct object *lookup(vgc_handle id, enum kind kind)
{
    for (struct object *o = objects; o; o = o->next)
        if (o->refs && o->id == id && o->kind == kind) return o;
    return NULL;
}
static struct object *lookup_any(vgc_handle id)
{
    for (struct object *o = objects; o; o = o->next)
        if (o->refs && o->id == id) return o;
    return NULL;
}
static bool hold(struct object *o)
{
    if (o->refs == SIZE_MAX) return false;
    ++o->refs;
    return true;
}
static IOReturn drop(struct object *o);
/* refs=0 的对象仍留在 objects 中：自动析构失败交给显式 drain 重试。 */
static IOReturn destroy(struct object *o)
{
    IOReturn r = kIOReturnSuccess;
    if (o->kind == IMAGE && o->u.i.image)
        r = vgpu_surface_image_release(o->u.i.image);
    else if (o->kind == SHARE && o->u.s.owner)
        r = vgpu_surface_owner_release(o->u.s.owner);
    if (r) return r;
    struct object **p = &objects;
    while (*p != o) p = &(*p)->next;
    *p = o->next;
    if (o->kind == CONTEXT) {
        (void)drop(o->u.c.draw);
        (void)drop(o->u.c.read);
        (void)drop(o->u.c.last);
    } else if (o->kind == FRAMEBUFFER) {
        (void)drop(o->u.f.image);
    } else if (o->kind == IMAGE) {
        if (leased_image == o) leased_image = NULL;
    } else if (o->u.s.surface) {
        vgpu_surface_release(o->u.s.surface);
    }
    (void)drop(o->share);
    free(o);
    return kIOReturnSuccess;
}
static IOReturn drop(struct object *o)
{
    if (!o || --o->refs) return kIOReturnSuccess;
    return destroy(o);
}
IOReturn vgc_drain_releases(void)
{
    pthread_mutex_lock(&lock);
    IOReturn result = kIOReturnSuccess;
    /* destroy 可递归移除节点，成功后从表头重新遍历。每个失败对象本次只试一次。 */
    uint64_t cursor = 0;
    for (;;) {
        struct object *candidate = NULL;
        for (struct object *o = objects; o; o = o->next)
            if (!o->refs && o->id > cursor && (!candidate || o->id < candidate->id)) candidate = o;
        if (!candidate) break;
        cursor = candidate->id;
        IOReturn r = destroy(candidate);
        if (r && !result) result = r;
    }
    if (!result) {
        for (struct object *o = objects; o; o = o->next)
            if (!o->refs) { result = kIOReturnNotReady; break; }
    }
    pthread_mutex_unlock(&lock);
    return result;
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
static bool overlaps(const void *p, size_t n, vgc_view v)
{
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)v.data;
    return n > UINTPTR_MAX - a || v.length > UINTPTR_MAX - b ||
        (a < b + v.length && b < a + n);
}
static bool output_ok(const void *p, size_t n)
{
    if (!p || n > UINTPTR_MAX - (uintptr_t)p) return false;
    for (struct object *o = objects; o; o = o->next)
        if (o->kind == CONTEXT && (overlaps(p, n, o->u.c.h) || overlaps(p, n, o->u.c.b))) return false;
    return true;
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
static IOReturn share_create(vgc_handle *out, bool bgra,
                            bool registry_specific, uint64_t registry_id)
{
    const uint16_t endian = 1;
    if (*(const unsigned char *)&endian != 1) return kIOReturnUnsupported;
    pthread_mutex_lock(&lock);
    if (!output_ok(out, sizeof(*out))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
    *out = 0;
    struct object *o = allocate(SHARE, NULL);
    IOReturn r = kIOReturnNoMemory;
    if (o) {
        if (registry_specific)
            r = vgpu_surface_owner_open_registry(registry_id, &o->u.s.owner);
        else if (bgra)
            r = vgpu_surface_owner_open(&o->u.s.owner);
        else
            r = vgpu_surface_open(256, 256, VGPU_SURFACE_BGRX8, &o->u.s.surface);
        if (r) (void)drop(o); else *out = o->id;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_share_create(vgc_handle *out) { return share_create(out, false, false, 0); }
IOReturn vgc_share_create_bgra(vgc_handle *out) { return share_create(out, true, false, 0); }
IOReturn vgc_share_create_bgra_registry(uint64_t registry_id, vgc_handle *out)
{
    return share_create(out, true, true, registry_id);
}
IOReturn vgc_context_create(vgc_handle share, vgc_view h, vgc_view b, vgc_handle *out)
{
    if (!out) return kIOReturnBadArgument;
    bool valid = view_ok(h, VGC_H_LENGTH) && view_ok(b, VGC_B_LENGTH);
    if ((h.data && overlaps(out, sizeof(*out), h)) ||
        (b.data && overlaps(out, sizeof(*out), b))) return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    if (!output_ok(out, sizeof(*out))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
    *out = 0;
    if (!valid) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
    struct object *s = lookup(share, SHARE), *o = NULL;
    if (s) o = allocate(CONTEXT, s);
    if (o) { o->u.c.h = h; o->u.c.b = b; *out = o->id; }
    pthread_mutex_unlock(&lock);
    return o ? kIOReturnSuccess : s ? kIOReturnNoMemory : kIOReturnBadArgument;
}
IOReturn vgc_image_create(vgc_handle share, vgc_image_desc desc, vgc_handle *out)
{
    pthread_mutex_lock(&lock);
    if (!output_ok(out, sizeof(*out))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
    *out = 0;
    struct object *s = lookup(share, SHARE), *o = NULL;
    IOReturn r = kIOReturnBadArgument;
    if (!s) goto done;
    if (desc.width != 256 || desc.height != 256 || desc.samples ||
        desc.backing_format != (s->u.s.owner ? VGPU_SURFACE_BGRA8 : VGPU_SURFACE_BGRX8)) {
        r = kIOReturnUnsupported; goto done;
    }
    if (!s->u.s.owner && leased_image) { r = kIOReturnExclusiveAccess; goto done; }
    o = allocate(IMAGE, s);
    if (!o) { r = kIOReturnNoMemory; goto done; }
    o->u.i.desc = desc;
    r = s->u.s.owner ? vgpu_surface_image_create(s->u.s.owner, desc.width,
        desc.height, desc.backing_format, &o->u.i.image) : kIOReturnSuccess;
    if (r) { (void)drop(o); goto done; }
    if (!s->u.s.owner) leased_image = o;
    *out = o->id;
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_framebuffer_create(vgc_handle share, vgc_handle *out)
{
    pthread_mutex_lock(&lock);
    if (!output_ok(out, sizeof(*out))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
    *out = 0;
    struct object *s = lookup(share, SHARE), *o = NULL;
    if (s) o = allocate(FRAMEBUFFER, s);
    if (o) { o->u.f.state.dirty = VGC_ATTACHMENT_DIRTY; *out = o->id; }
    pthread_mutex_unlock(&lock);
    return o ? kIOReturnSuccess : s ? kIOReturnNoMemory : kIOReturnBadArgument;
}
IOReturn vgc_share_retain(vgc_handle id)
{
    pthread_mutex_lock(&lock);
    struct object *o = lookup(id, SHARE);
    IOReturn r = !o ? kIOReturnBadArgument : hold(o) ? kIOReturnSuccess : kIOReturnNoResources;
    pthread_mutex_unlock(&lock);
    return r;
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
    if (o) {
        r = drop(o);
        /* 显式 release 失败不消费调用者引用，保留原 handle 供重试。 */
        if (r) o->refs = 1;
    }
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
    if (!f->share->u.s.owner && f->u.f.image != leased_image) return kIOReturnNotReady;
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
    pthread_mutex_lock(&lock);
    if (!output_ok(out, sizeof(*out))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
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
    pthread_mutex_lock(&lock);
    if (!output_ok(dirty, sizeof(*dirty))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
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
    if (!output_ok(generation, sizeof(*generation))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
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
    struct object *image = c->u.c.draw->u.f.image;
    bool bgra = c->share->u.s.owner != NULL;
    if (bgra && !hold(image)) { r = kIOReturnNoResources; goto done; }
    uint64_t completed = 0;
    r = bgra ? vgpu_surface_image_clear(image->u.i.image, command.rgba, &completed) :
        vgpu_surface_clear(c->share->u.s.surface, command.rgba, &completed);
    if (!r) {
        image->u.i.generation = completed;
        if (bgra) { (void)drop(c->u.c.last); c->u.c.last = image; }
        c->u.c.state.completed_generation = completed;
        c->u.c.state.dirty = 0;
        c->u.c.state.failed = 0;
        *generation = completed;
    } else if (bgra) (void)drop(image);
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_draw_triangle(vgc_handle id, const float vertices[3][4], uint64_t *generation)
{
    if (!vertices || !generation) return kIOReturnBadArgument;
    float command[3][4];
    memcpy(command, vertices, sizeof(command));
    pthread_mutex_lock(&lock);
    if (!output_ok(generation, sizeof(*generation))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
    struct object *c = lookup(id, CONTEXT);
    IOReturn r = kIOReturnBadArgument;
    if (!c) goto done;
    c->u.c.state.dirty = 1;
    c->u.c.state.failed = 1;
    if (!c->share->u.s.owner) { r = kIOReturnUnsupported; goto done; }
    if (!c->u.c.draw) { r = kIOReturnNotReady; goto done; }
    r = load(c->u.c.draw);
    if (r) goto done;
    struct object *image = c->u.c.draw->u.f.image;
    if (!hold(image)) { r = kIOReturnNoResources; goto done; }
    uint64_t completed = 0;
    r = vgpu_surface_image_draw_triangle(image->u.i.image, command, &completed);
    if (!r) {
        image->u.i.generation = completed;
        (void)drop(c->u.c.last);
        c->u.c.last = image;
        c->u.c.state.completed_generation = completed;
        c->u.c.state.dirty = 0;
        c->u.c.state.failed = 0;
        *generation = completed;
    } else (void)drop(image);
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
    if (!c || !output_ok(destination, capacity)) goto done;
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
    r = c->share->u.s.owner ?
        vgpu_surface_image_read(c->u.c.read->u.f.image->u.i.image, generation, rect,
            VGPU_SURFACE_RGBA8_OUTPUT, pack, destination, capacity) :
        vgpu_surface_read(c->share->u.s.surface, generation, rect,
            VGPU_SURFACE_RGBA8_OUTPUT, pack, destination, capacity);
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_flush_completed(vgc_handle id, uint64_t *out)
{
    pthread_mutex_lock(&lock);
    if (!output_ok(out, sizeof(*out))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
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
    if (c->share->u.s.owner) {
        if (!c->u.c.last || c->u.c.last->u.i.generation != generation) {
            r = kIOReturnNotReady; goto done;
        }
        r = vgpu_surface_image_read(c->u.c.last->u.i.image, generation, rect,
            VGPU_SURFACE_RGBA8_OUTPUT, pack, pixel, sizeof(pixel));
    } else r = vgpu_surface_read(c->share->u.s.surface, generation, rect,
        VGPU_SURFACE_RGBA8_OUTPUT, pack, pixel, sizeof(pixel));
    if (!r) *out = generation;
done:
    pthread_mutex_unlock(&lock);
    return r;
}
IOReturn vgc_context_state(vgc_handle id, vgc_state *out)
{
    pthread_mutex_lock(&lock);
    if (!output_ok(out, sizeof(*out))) { pthread_mutex_unlock(&lock); return kIOReturnBadArgument; }
    struct object *c = lookup(id, CONTEXT);
    if (c) *out = c->u.c.state;
    pthread_mutex_unlock(&lock);
    return c ? kIOReturnSuccess : kIOReturnBadArgument;
}
