#include "vgpu_texture.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

_Static_assert(sizeof(void *) == 8, "需要 64 位指针");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error 需要小端宿主
#endif

typedef struct object object;
struct object {
    object *next;
    vgt_handle id;
    uint32_t refs, type;
    union {
        struct { vgt_backend backend; int configured; } share;
        struct {
            object *share;
            vgt_views views;
            uint32_t kind;
            int attached;
            uint64_t generation;
            vgt_backing backing;
            vgt_image_desc desc;
        } texture;
    } u;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static object *objects;
static uint64_t next_id = 1;

static uint16_t u16(const void *p, size_t offset) {
    uint16_t v;
    memcpy(&v, (const unsigned char *)p + offset, sizeof(v));
    return v;
}
static uint64_t u64(const void *p, size_t offset) {
    uint64_t v;
    memcpy(&v, (const unsigned char *)p + offset, sizeof(v));
    return v;
}
static int lease_valid(vgt_lease l) { return !!l.retain == !!l.release; }
static int view_range_valid(const void *data, size_t length) {
    return data && length <= UINTPTR_MAX - (uintptr_t)data;
}
/* 调用前须验证两个半开区间均不回绕。 */
static int overlaps(const void *a, size_t an, const void *b, size_t bn) {
    return an && bn && (uintptr_t)a < (uintptr_t)b + bn &&
        (uintptr_t)b < (uintptr_t)a + an;
}
static object *lookup(vgt_handle id) {
    object *o;
    for (o = objects; o; o = o->next) if (o->id == id) return o;
    return NULL;
}
static vgt_status hold(object *o) {
    if (o->refs == UINT32_MAX) return VGT_OVERFLOW;
    ++o->refs;
    return VGT_OK;
}
static vgt_status insert(object *o, vgt_handle *out) {
    if (!next_id) return VGT_OVERFLOW;
    o->id = next_id++;
    o->refs = 1;
    o->next = objects;
    objects = o;
    *out = o->id;
    return VGT_OK;
}
static void drop(object *o) {
    object **p;
    if (--o->refs) return;
    for (p = &objects; *p != o; p = &(*p)->next) {}
    *p = o->next;
    if (o->type == 2) {
        object *s = o->u.texture.share;
        vgt_backend *b = &s->u.share.backend;
        if (o->u.texture.backing.resource)
            b->destroy_image(b->context, &o->u.texture.backing);
        if (o->u.texture.attached && o->u.texture.views.lease.release)
            o->u.texture.views.lease.release(o->u.texture.views.lease.context);
        drop(s);
    } else if (o->u.share.backend.lease.release) {
        o->u.share.backend.lease.release(o->u.share.backend.lease.context);
    }
    free(o);
}
static vgt_status texture(vgt_handle share, vgt_handle id, object **out) {
    object *s = lookup(share), *o = lookup(id);
    if (!s || s->type != 1 || !o || o->type != 2) return VGT_BAD_HANDLE;
    if (o->u.texture.share != s) return VGT_OWNER_MISMATCH;
    if (!o->u.texture.attached) return VGT_DETACHED;
    *out = o;
    return VGT_OK;
}
vgt_status vgt_share_create(const vgt_backend *backend, vgt_handle *out) {
    object *s;
    vgt_status st = VGT_OK;
    int backend_held = 0;
    if (!view_range_valid(out, sizeof(*out))) return VGT_INVALID;
    if (backend && (!view_range_valid(backend, sizeof(*backend)) ||
        overlaps(out, sizeof(*out), backend, sizeof(*backend)))) return VGT_INVALID;
    *out = 0;
    if (backend && (backend->version != VGT_BACKEND_VERSION ||
        !backend->create_image || !backend->destroy_image ||
        !backend->max_width || !backend->max_height ||
        !lease_valid(backend->lease))) return VGT_INVALID;
    pthread_mutex_lock(&lock);
    s = calloc(1, sizeof(*s));
    if (!s) { st = VGT_NO_MEMORY; goto done; }
    s->type = 1;
    if (backend) {
        s->u.share.backend = *backend;
        s->u.share.configured = 1;
        if (backend->lease.retain) {
            st = backend->lease.retain(backend->lease.context);
            if (st != VGT_OK) { free(s); goto done; }
            backend_held = 1;
        }
    }
    st = insert(s, out);
    if (st != VGT_OK) {
        if (backend_held) backend->lease.release(backend->lease.context);
        free(s);
    }
done:
    pthread_mutex_unlock(&lock);
    return st;
}
vgt_status vgt_texture_create(vgt_handle share, vgt_views views, vgt_handle *out) {
    object *s, *o;
    vgt_status st;
    uint32_t kind;
    if (!view_range_valid(out, sizeof(*out)) ||
        !view_range_valid(views.td.data, views.td.length) ||
        !view_range_valid(views.tu.data, views.tu.length)) return VGT_INVALID;
    if (overlaps(out, sizeof(*out), views.td.data, views.td.length) ||
        overlaps(out, sizeof(*out), views.tu.data, views.tu.length)) return VGT_INVALID;
    *out = 0;
    if (!lease_valid(views.lease)) return VGT_INVALID;
    if (views.td.length < VGT_TD_METADATA_LENGTH || views.tu.length < VGT_TU_LENGTH)
        return VGT_BOUNDS;
    /* TU 的成功写入不能覆盖长期借用的只读 TD。 */
    if ((uintptr_t)views.td.data < (uintptr_t)views.tu.data + views.tu.length &&
        (uintptr_t)views.tu.data < (uintptr_t)views.td.data + views.td.length)
        return VGT_INVALID;
    pthread_mutex_lock(&lock);
    s = lookup(share);
    if (!s || s->type != 1) { st = VGT_BAD_HANDLE; goto done; }
    /* 元数据创建不承诺任何格式/图像支持，默认对象允许尚无 image。 */
    kind = u16(views.td.data, 0) & 15;
    if (kind > 10) { st = VGT_UNSUPPORTED; goto done; }
    o = calloc(1, sizeof(*o));
    if (!o) { st = VGT_NO_MEMORY; goto done; }
    st = hold(s);
    if (st != VGT_OK) { free(o); goto done; }
    if (views.lease.retain) {
        st = views.lease.retain(views.lease.context);
        if (st != VGT_OK) { drop(s); free(o); goto done; }
    }
    o->type = 2;
    o->u.texture.share = s;
    o->u.texture.views = views;
    o->u.texture.kind = kind;
    o->u.texture.attached = 1;
    st = insert(o, out);
    if (st != VGT_OK) {
        if (views.lease.release) views.lease.release(views.lease.context);
        drop(s);
        free(o);
    }
done:
    pthread_mutex_unlock(&lock);
    return st;
}
vgt_status vgt_texture_destroy(vgt_handle share, vgt_handle id) {
    object *s, *o;
    vgt_status st;
    pthread_mutex_lock(&lock);
    s = lookup(share);
    o = lookup(id);
    if (!s || s->type != 1 || !o || o->type != 2) st = VGT_BAD_HANDLE;
    else if (o->u.texture.share != s) st = VGT_OWNER_MISMATCH;
    else { drop(o); st = VGT_OK; }
    pthread_mutex_unlock(&lock);
    return st;
}
vgt_status vgt_retain(vgt_handle id) {
    object *o;
    vgt_status st;
    pthread_mutex_lock(&lock);
    o = lookup(id);
    st = o ? hold(o) : VGT_BAD_HANDLE;
    pthread_mutex_unlock(&lock);
    return st;
}
vgt_status vgt_release(vgt_handle id) {
    object *o;
    vgt_status st;
    pthread_mutex_lock(&lock);
    o = lookup(id);
    st = o ? VGT_OK : VGT_BAD_HANDLE;
    if (o) drop(o);
    pthread_mutex_unlock(&lock);
    return st;
}
vgt_status vgt_texture_state_get(vgt_handle id, vgt_texture_state *out) {
    object *o;
    if (!out) return VGT_INVALID;
    pthread_mutex_lock(&lock);
    o = lookup(id);
    if (!o || o->type != 2) { pthread_mutex_unlock(&lock); return VGT_BAD_HANDLE; }
    memset(out, 0, sizeof(*out));
    out->share = o->u.texture.share->id;
    out->generation = o->u.texture.generation;
    out->references = o->refs;
    out->kind = o->u.texture.kind;
    out->attached = (uint32_t)o->u.texture.attached;
    out->has_backing = o->u.texture.backing.resource != NULL;
    out->backing = o->u.texture.backing;
    pthread_mutex_unlock(&lock);
    return VGT_OK;
}
vgt_status vgt_texture_detach_views(vgt_handle share, vgt_handle id) {
    object *o;
    vgt_status st;
    pthread_mutex_lock(&lock);
    st = texture(share, id, &o);
    if (st == VGT_OK) {
        if (o->u.texture.views.lease.release)
            o->u.texture.views.lease.release(o->u.texture.views.lease.context);
        memset(&o->u.texture.views, 0, sizeof(o->u.texture.views));
        o->u.texture.attached = 0;
    }
    pthread_mutex_unlock(&lock);
    return st;
}
vgt_status vgt_texture_modify(vgt_handle share, vgt_handle id, uint32_t flags) {
    object *o;
    vgt_status st;
    pthread_mutex_lock(&lock);
    st = texture(share, id, &o);
    if (st == VGT_OK) {
        unsigned char *tu = o->u.texture.views.tu.data;
        if ((flags & ~UINT32_C(7)) || (tu[0x14] & ~VGT_DIRTY_SUPPORTED)) st = VGT_UNSUPPORTED;
        else tu[0x14] |= 2;
    }
    pthread_mutex_unlock(&lock);
    return st;
}
static vgt_status decode(object *o, vgt_view cpu, vgt_image_desc *out) {
    const unsigned char *td = o->u.texture.views.td.data;
    const unsigned char *tu = o->u.texture.views.tu.data;
    const unsigned char *l;
    vgt_image_desc d = {0};
    uint16_t flags;
    size_t rows, texels;
    if (o->u.texture.views.td.length < VGT_TD_RB_LENGTH) return VGT_BOUNDS;
    flags = u16(td, 0);
    if (o->u.texture.kind != 2 || (flags & 15) != 2 || (flags & ~UINT16_C(0x300f)) ||
        u64(tu, 0) || td[0x78] || td[0x79] != 1 || td[0x7a] || td[0x7b] != 1 ||
        td[0x7c] || u16(td, 0x7e)) return VGT_UNSUPPORTED;
    l = td + 0xb0;
    if (u16(td, 0x68) != 0x8058 || u16(l, 0x10) != 0x1908 ||
        u16(l, 0x12) != 0x1401 || l[0xa] ||
        (l[0xb] != 0x20 && l[0xb] != 0x21 && l[0xb] != 0x28)) return VGT_UNSUPPORTED;
    d.width = u16(l, 4); d.height = u16(l, 6); d.depth = u16(l, 8);
    if (!d.width || !d.height || d.depth != 1 ||
        d.width != u16(td, 0x6a) || d.height != u16(td, 0x6c) ||
        d.depth != u16(td, 0x6e)) return VGT_INVALID;
    texels = u16(l, 0xc); rows = u16(l, 0xe);
    if (texels < d.width || rows < d.height) return VGT_INVALID;
    if (texels > SIZE_MAX / 4 || rows > SIZE_MAX / (texels * 4)) return VGT_OVERFLOW;
    d.cpu_row_bytes = texels * 4;
    d.cpu_slice_bytes = d.cpu_row_bytes * rows;
    if (u64(l, 0x18) != (uint64_t)(uintptr_t)cpu.data) return VGT_INVALID;
    if (cpu.data) {
        if (cpu.length < d.cpu_slice_bytes) return VGT_BOUNDS;
        if (cpu.length > UINTPTR_MAX - (uintptr_t)cpu.data) return VGT_OVERFLOW;
        if (overlaps(cpu.data, cpu.length, tu + 8, 0x15 - 8)) return VGT_INVALID;
        d.cpu = (vgt_view){cpu.data, d.cpu_slice_bytes};
    } else if (cpu.length) return VGT_INVALID;
    d.format = VGT_FORMAT_RGBA8_UNORM;
    d.samples = 1; /* TD sample 选择码0对应本接口的一个样本。 */
    *out = d;
    return VGT_OK;
}
vgt_status vgt_texture_decode_renderbuffer(vgt_handle share, vgt_handle id,
                                          vgt_view cpu, vgt_image_desc *out) {
    object *o;
    vgt_status st;
    if (!out) return VGT_INVALID;
    pthread_mutex_lock(&lock);
    st = texture(share, id, &o);
    if (st == VGT_OK) st = decode(o, cpu, out);
    pthread_mutex_unlock(&lock);
    return st;
}
static int desc_equal(const vgt_image_desc *a, const vgt_image_desc *b) {
    return a->width == b->width && a->height == b->height && a->depth == b->depth &&
        a->format == b->format && a->samples == b->samples &&
        a->cpu_row_bytes == b->cpu_row_bytes && a->cpu_slice_bytes == b->cpu_slice_bytes &&
        a->cpu.data == b->cpu.data && a->cpu.length == b->cpu.length;
}
vgt_status vgt_texture_ensure_backing(vgt_handle share, vgt_handle id, vgt_view cpu) {
    object *o;
    vgt_backend *b;
    vgt_image_desc d;
    vgt_backing candidate = {0}, old;
    unsigned char *tu;
    unsigned int i;
    int broadcast = 1, masks = 0;
    vgt_status st;
    pthread_mutex_lock(&lock);
    st = texture(share, id, &o);
    if (st != VGT_OK) goto done;
    tu = o->u.texture.views.tu.data;
    if (tu[0x14] & ~VGT_DIRTY_SUPPORTED) { st = VGT_UNSUPPORTED; goto done; }
    st = decode(o, cpu, &d);
    if (st != VGT_OK) goto done;
    for (i = 0; i < 6; ++i) {
        uint16_t m = u16(tu, 8 + 2 * i);
        broadcast &= m == UINT16_MAX;
        masks |= m != 0;
    }
    /* GFX 几何变更明确广播六个 ffff。对已验证单组单level描述符，
     * 完整重建处理该广播；其他超出 face0/level0 的掩码拒绝。 */
    if (!(broadcast && (tu[0x14] & 1))) {
        if (u16(tu, 8) & ~UINT16_C(1)) { st = VGT_UNSUPPORTED; goto done; }
        for (i = 1; i < 6; ++i)
            if (u16(tu, 8 + i * 2)) { st = VGT_UNSUPPORTED; goto done; }
    }
    b = &o->u.texture.share->u.share.backend;
    if (!o->u.texture.share->u.share.configured) { st = VGT_NO_BACKEND; goto done; }
    if (b->rgba8_alpha != 1) { st = VGT_UNSUPPORTED; goto done; }
    if (d.width > b->max_width || d.height > b->max_height) { st = VGT_BOUNDS; goto done; }
    if (!tu[0x14] && !masks && o->u.texture.backing.resource &&
        desc_equal(&d, &o->u.texture.desc)) { st = VGT_OK; goto done; }
    if (o->u.texture.generation == UINT64_MAX) { st = VGT_OVERFLOW; goto done; }
    st = b->create_image(b->context, &d, &candidate);
    if (st == VGT_OK && (!candidate.resource || candidate.width != d.width ||
        candidate.height != d.height || candidate.format != d.format ||
        candidate.samples != d.samples || candidate.row_bytes < (size_t)d.width * 4 ||
        candidate.row_bytes > SIZE_MAX / d.height ||
        candidate.byte_size < candidate.row_bytes * d.height)) st = VGT_BACKEND_FAILED;
    if (candidate.resource && candidate.resource == o->u.texture.backing.resource) {
        /* 违反独立候选契约；不销毁仍被本对象拥有的旧资源。 */
        st = VGT_BACKEND_FAILED;
        candidate.resource = NULL;
    }
    if (st != VGT_OK) {
        if (candidate.resource) b->destroy_image(b->context, &candidate);
        goto done;
    }
    old = o->u.texture.backing;
    o->u.texture.backing = candidate;
    o->u.texture.desc = d;
    ++o->u.texture.generation;
    memset(tu + 8, 0, 12);
    tu[0x14] = 0;
    if (old.resource) b->destroy_image(b->context, &old);
done:
    pthread_mutex_unlock(&lock);
    return st;
}
vgt_status vgt_texture_load(vgt_handle share, vgt_handle texture_id, vgt_view cpu) {
    return vgt_texture_ensure_backing(share, texture_id, cpu);
}
