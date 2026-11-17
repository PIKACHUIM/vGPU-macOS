#include "vgpu_framebuffer.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == 8, "需要 64 位宿主");
struct target {
    struct target *next;
    vgc_handle framebuffer, image;
};
struct framebuffer {
    struct framebuffer *next;
    vgf_handle id;
    vgc_handle share, anchor;
    vgc_view fq;
    vgf_mut_view fd;
    vgf_backend backend;
    bool backend_held, destroying;
    struct target *active, *retired;
};
static struct framebuffer *objects;
static vgf_handle next_id = 1;

static bool range_ok(const void *p, size_t n)
{
    return p && n <= UINTPTR_MAX - (uintptr_t)p;
}
static bool overlap(const void *a, size_t n, const void *b, size_t m)
{
    if (!a || !b) return false;
    if (!range_ok(a, n) || !range_ok(b, m)) return true;
    return (uintptr_t)a < (uintptr_t)b + m && (uintptr_t)b < (uintptr_t)a + n;
}
static bool output_ok(const void *p, size_t n)
{
    if (!range_ok(p, n)) return false;
    for (struct framebuffer *f = objects; f; f = f->next)
        if (overlap(p, n, f->fq.data, f->fq.length) ||
            overlap(p, n, f->fd.data, f->fd.length)) return false;
    return true;
}
static struct framebuffer *lookup(vgf_handle id)
{
    for (struct framebuffer *f = objects; f; f = f->next)
        if (f->id == id) return f;
    return NULL;
}
static uint16_t u16(const unsigned char *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static uint32_t u32(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static uint64_t u64(const unsigned char *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
/* 每个节点预先持有 image 引用，失败清理不分配内存。vgc 父析构内部失败
 * 由其 drain 保管；显式 image release 失败则由本节点保管。 */
static IOReturn reap(struct framebuffer *f)
{
    struct target **p = &f->retired;
    while (*p) {
        struct target *t = *p;
        IOReturn r;
        if (t->framebuffer) {
            r = vgc_release(t->framebuffer);
            if (r) return r;
            t->framebuffer = 0;
        }
        if (t->image) {
            r = vgc_release(t->image);
            if (r) return r;
            t->image = 0;
        }
        *p = t->next;
        free(t);
    }
    return vgc_drain_releases();
}
IOReturn vgf_create(vgc_handle share, vgc_view fq, vgf_mut_view fd,
                    const vgf_backend *backend, vgf_handle *out)
{
    if (!output_ok(out, sizeof(*out)) ||
        overlap(out, sizeof(*out), fq.data, fq.length) ||
        overlap(out, sizeof(*out), fd.data, fd.length)) return kIOReturnBadArgument;
    /* backend 可能与 out 重叠，先快照再写输出。 */
    vgf_backend ops = {0};
    if (backend) ops = *backend;
    if (!range_ok(fq.data, fq.length) || fq.length < VGF_FQ_LENGTH ||
        !range_ok(fd.data, fd.length) || fd.length < VGF_FD_LENGTH ||
        overlap(fq.data, fq.length, fd.data, fd.length) ||
        !ops.retain || !ops.release || !ops.resolve || !ops.ensure_backing)
        return kIOReturnBadArgument;
    /* FD 写入不能污染另一个存活对象的 Q/D；只读 Q 允许共享。 */
    for (struct framebuffer *f = objects; f; f = f->next)
        if (overlap(fd.data, fd.length, f->fq.data, f->fq.length) ||
            overlap(fd.data, fd.length, f->fd.data, f->fd.length) ||
            overlap(fq.data, fq.length, f->fd.data, f->fd.length)) return kIOReturnBadArgument;
    const uint16_t endian = 1;
    if (*(const unsigned char *)&endian != 1) return kIOReturnUnsupported;
    if (!next_id) return kIOReturnNoResources;
    struct framebuffer *f = calloc(1, sizeof(*f));
    if (!f) return kIOReturnNoMemory;
    /* 空 FBO 验证 share 类型并持强引用；retain 失败时没有 image/backing
     * 要释放，调用者仍持 share，回滚不会触发 owner 最后释放。 */
    IOReturn r = vgc_framebuffer_create(share, &f->anchor);
    if (r) { free(f); return r; }
    r = ops.retain(ops.context);
    if (r) { (void)vgc_release(f->anchor); free(f); return r; }
    f->id = next_id++;
    f->share = share;
    f->fq = fq;
    f->fd = fd;
    f->backend = ops;
    f->backend_held = true;
    f->next = objects;
    objects = f;
    *out = f->id;
    return kIOReturnSuccess;
}
IOReturn vgf_load(vgf_handle id)
{
    struct framebuffer *f = lookup(id);
    if (!f || f->destroying) return kIOReturnBadArgument;
    unsigned char *fd = f->fd.data;
    const unsigned char *q = f->fq.data;
    fd[0x51] |= 1;
    if (fd[0x50] & (unsigned char)~1u) return kIOReturnUnsupported;
    /* E6/E7/E8：只读取已证字段，不把 Q+2/+3 当 padding 约束。
     * renderbuffer 调用者 0x7FFB063CBCC9..CBCF6 明确 type=0x2000、
     * face/level/layer 三个实际写入的 u32 都为0。非零语义尚未全部闭合。 */
    if (u32(q + 0xc0) != 1 || u16(q) != VGF_RENDERBUFFER_TYPE ||
        u32(q + 4) || u32(q + 8) || u32(q + 12)) return kIOReturnUnsupported;
    uint32_t width = u16(q + 0xb2), height = u16(q + 0xb4);
    if (!width || !height) return kIOReturnUnsupported;
    uint64_t token = u64(fd);
    if (!token) return kIOReturnBadArgument;
    IOReturn r = reap(f);
    if (r) return r;
    vgf_texture texture = {0};
    r = f->backend.resolve(f->backend.context, token, &texture);
    if (r) return r;
    if (!texture.texture || texture.share != f->share) return kIOReturnBadArgument;
    struct target *t = calloc(1, sizeof(*t));
    if (!t) return kIOReturnNoMemory;
    vgf_backing backing = {0};
    r = f->backend.ensure_backing(f->backend.context, &texture, &backing);
    if (r) { free(t); return r; }
    if (!backing.image || backing.width != width || backing.height != height ||
        backing.depth != 1 || backing.internal_format != VGF_RGBA8 || backing.samples) {
        free(t); return kIOReturnUnsupported;
    }
    r = vgc_retain(backing.image);
    if (r) { free(t); return r; }
    t->image = backing.image;
    r = vgc_framebuffer_create(f->share, &t->framebuffer);
    if (!r) r = vgc_framebuffer_attach(t->framebuffer, 0, t->image);
    if (!r) r = vgc_framebuffer_load(t->framebuffer);
    if (r) {
        t->next = f->retired;
        f->retired = t;
        (void)reap(f);
        return r;
    }
    if (f->active) {
        f->active->next = f->retired;
        f->retired = f->active;
    }
    f->active = t;
    fd[0x50] &= (unsigned char)~1u;
    fd[0x51] &= (unsigned char)~1u;
    /* 提交后不把旧对象清理错误报告成 load 失败；下次 load/destroy 重试。 */
    (void)reap(f);
    return kIOReturnSuccess;
}
IOReturn vgf_target_get(vgf_handle id, vgc_handle *out)
{
    if (!output_ok(out, sizeof(*out))) return kIOReturnBadArgument;
    struct framebuffer *f = lookup(id);
    if (!f || f->destroying) return kIOReturnBadArgument;
    if (!f->active) return kIOReturnNotReady;
    *out = f->active->framebuffer;
    return kIOReturnSuccess;
}
IOReturn vgf_destroy(vgf_handle id)
{
    struct framebuffer *f = lookup(id);
    if (!f) return kIOReturnBadArgument;
    f->destroying = true;
    if (f->active) {
        f->active->next = f->retired;
        f->retired = f->active;
        f->active = NULL;
    }
    IOReturn r = reap(f);
    if (r) return r;
    if (f->backend_held) {
        r = f->backend.release(f->backend.context);
        if (r) return r;
        f->backend_held = false;
    }
    if (f->anchor) {
        r = vgc_release(f->anchor);
        if (r) return r;
        f->anchor = 0;
    }
    r = vgc_drain_releases();
    if (r) return r;
    struct framebuffer **p = &objects;
    while (*p != f) p = &(*p)->next;
    *p = f->next;
    free(f);
    return kIOReturnSuccess;
}
