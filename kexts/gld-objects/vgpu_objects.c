#include "vgpu_objects.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(void *) == 8, "需要 64 位宿主");
typedef struct object object;
struct object {
    object *next, *share, *pipeline;
    vgo_handle id;
    vgc_handle native;
    vgo_kind kind;
    uint32_t refs;
    int attached, consumed, releasing;
    uint64_t generation;
    vgo_sampler_consumer consumer;
    void *consumer_context;
    vgo_views views;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static object *objects;
static uint64_t next_id = 1;

static object *lookup(vgo_handle id, vgo_kind kind) {
    for (object *o = objects; o; o = o->next)
        if (o->id == id && (!kind || o->kind == kind)) return o;
    return NULL;
}
static vgo_status hold(object *o) {
    if (o->releasing) return VGO_CONTEXT_FAILED;
    if (o->refs == UINT32_MAX) return VGO_OVERFLOW;
    ++o->refs;
    return VGO_OK;
}
static vgo_status drop(object *o) {
    if (!o) return VGO_OK;
    if (o->refs > 1) { --o->refs; return VGO_OK; }
    /* 最后一份引用保留到整条析构链成功；已释放的边清零，重试不重复释放。 */
    o->releasing = 1;
    if (o->native) {
        if (vgc_release(o->native) != kIOReturnSuccess) return VGO_CONTEXT_FAILED;
        o->native = 0;
    }
    vgo_status st = drop(o->pipeline);
    if (st != VGO_OK) return st;
    o->pipeline = NULL;
    st = drop(o->share);
    if (st != VGO_OK) return st;
    o->share = NULL;
    if (o->attached && o->views.lease.release)
        o->views.lease.release(o->views.lease.context);
    object **p = &objects;
    while (*p != o) p = &(*p)->next;
    *p = o->next;
    free(o);
    return VGO_OK;
}
static vgo_status allocate(vgo_kind kind, object *share, object **out) {
    if (next_id > UINT64_C(0x0000ffffffffffff)) return VGO_OVERFLOW;
    object *o = calloc(1, sizeof(*o));
    if (!o) return VGO_NO_MEMORY;
    if (share) {
        vgo_status st = hold(share);
        if (st != VGO_OK) { free(o); return st; }
    }
    o->kind = kind;
    o->share = share;
    o->refs = 1;
    /* 高位命名空间与低位类型标签；仍以完整注册表查找为准。 */
    o->id = UINT64_C(0xa700000000000000) | (next_id++ << 8) | (uint64_t)kind;
    *out = o;
    return VGO_OK;
}
static void publish(object *o, vgo_handle *out) {
    o->next = objects;
    objects = o;
    *out = o->id;
}
static vgo_status owned(vgo_handle share, vgo_handle id, vgo_kind kind, object **out) {
    object *s = lookup(share, VGO_SHARE), *o = lookup(id, kind);
    if (!s || !o || o->kind < VGO_SAMPLER) return VGO_BAD_HANDLE;
    if (o->share != s) return VGO_OWNER_MISMATCH;
    *out = o;
    return VGO_OK;
}
static int range_valid(const void *p, size_t n) {
    return p && n <= UINTPTR_MAX - (uintptr_t)p;
}
/* 仅用于已验证不回绕的半开区间，不探测调用者地址是否映射。 */
static int overlaps(const void *a, size_t an, const void *b, size_t bn) {
    return an && bn && (uintptr_t)a < (uintptr_t)b + bn &&
        (uintptr_t)b < (uintptr_t)a + an;
}
static vgo_status views_valid(vgo_kind kind, vgo_views v) {
    if (!range_valid(v.descriptor.data, v.descriptor.length) ||
        !range_valid(v.state.data, v.state.length) ||
        !!v.lease.retain != !!v.lease.release) return VGO_INVALID;
    size_t dn = kind == VGO_SAMPLER ? VGO_SAMPLER_DESCRIPTOR_MIN :
                kind == VGO_PIPELINE ? VGO_PIPELINE_DESCRIPTOR_MIN : 1;
    size_t sn = kind == VGO_PIPELINE ? VGO_PIPELINE_STATE_MIN : 1;
    if (v.descriptor.length < dn || v.state.length < sn) return VGO_BOUNDS;
    if ((uintptr_t)v.descriptor.data < (uintptr_t)v.state.data + v.state.length &&
        (uintptr_t)v.state.data < (uintptr_t)v.descriptor.data + v.descriptor.length)
        return VGO_INVALID;
    return VGO_OK;
}
vgo_status vgo_share_create(vgo_handle *out) {
    if (!range_valid(out, sizeof(*out))) return VGO_INVALID;
    *out = 0;
    pthread_mutex_lock(&lock);
    object *o;
    vgo_status st = allocate(VGO_SHARE, NULL, &o);
    if (st == VGO_OK) {
        if (vgc_share_create(&o->native) != kIOReturnSuccess) {
            free(o); st = VGO_CONTEXT_FAILED;
        } else publish(o, out);
    }
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_share_import_context(vgc_handle native_share, vgo_handle *out) {
    if (!range_valid(out, sizeof(*out))) return VGO_INVALID;
    *out = 0;
    pthread_mutex_lock(&lock);
    object *o;
    vgo_status st = allocate(VGO_SHARE, NULL, &o);
    if (st == VGO_OK) {
        IOReturn r = vgc_share_retain(native_share);
        if (r != kIOReturnSuccess) {
            free(o);
            st = r == kIOReturnBadArgument ? VGO_BAD_HANDLE :
                 r == kIOReturnNoResources ? VGO_OVERFLOW : VGO_CONTEXT_FAILED;
        } else {
            o->native = native_share;
            publish(o, out);
        }
    }
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_context_create(vgo_handle share, vgc_view h, vgc_view b, vgo_handle *out) {
    if (!range_valid(out, sizeof(*out))) return VGO_INVALID;
    if (!range_valid(h.data, h.length) || !range_valid(b.data, b.length))
        return VGO_CONTEXT_FAILED;
    if (overlaps(out, sizeof(*out), h.data, h.length) ||
        overlaps(out, sizeof(*out), b.data, b.length)) return VGO_INVALID;
    *out = 0;
    pthread_mutex_lock(&lock);
    object *s = lookup(share, VGO_SHARE), *o;
    vgo_status st = s ? allocate(VGO_CONTEXT, s, &o) : VGO_BAD_HANDLE;
    if (st == VGO_OK) {
        if (vgc_context_create(s->native, h, b, &o->native) != kIOReturnSuccess) {
            drop(s); free(o); st = VGO_CONTEXT_FAILED;
        } else publish(o, out);
    }
    pthread_mutex_unlock(&lock);
    return st;
}
static vgo_status create(vgo_handle share, vgo_kind kind, vgo_views views, vgo_handle *out) {
    if (!range_valid(out, sizeof(*out)) ||
        !range_valid(views.descriptor.data, views.descriptor.length) ||
        !range_valid(views.state.data, views.state.length)) return VGO_INVALID;
    if (overlaps(out, sizeof(*out), views.descriptor.data, views.descriptor.length) ||
        overlaps(out, sizeof(*out), views.state.data, views.state.length)) return VGO_INVALID;
    *out = 0;
    vgo_status st = views_valid(kind, views);
    if (st != VGO_OK) return st;
    pthread_mutex_lock(&lock);
    object *s = lookup(share, VGO_SHARE), *o;
    if (!s) { st = VGO_BAD_HANDLE; goto done; }
    if (kind == VGO_PIPELINE) {
        const unsigned char *p = views.descriptor.data;
        uint16_t tag;
        memcpy(&tag, p, sizeof(tag));
        if (!((tag == 0 && (p[2] == 0 || p[2] == 4)) ||
              (tag == 0x8620 && p[2] == 0) || (tag == 0x8804 && p[2] == 4))) {
            st = VGO_UNSUPPORTED; goto done;
        }
    }
    st = allocate(kind, s, &o);
    if (st != VGO_OK) goto done;
    if (views.lease.retain) {
        st = views.lease.retain(views.lease.context);
        if (st != VGO_OK) { drop(s); free(o); goto done; }
    }
    o->attached = 1;
    o->views = views;
    publish(o, out);
done:
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_sampler_create(vgo_handle s, vgo_views v, vgo_handle *out) {
    return create(s, VGO_SAMPLER, v, out);
}
vgo_status vgo_pipeline_create(vgo_handle s, vgo_views v, vgo_handle *out) {
    return create(s, VGO_PIPELINE, v, out);
}
vgo_status vgo_program_create(vgo_handle s, vgo_views v, vgo_handle *out) {
    return create(s, VGO_PROGRAM, v, out);
}
vgo_status vgo_retain(vgo_handle id) {
    pthread_mutex_lock(&lock);
    object *o = lookup(id, 0);
    vgo_status st = o ? hold(o) : VGO_BAD_HANDLE;
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_release(vgo_handle id) {
    pthread_mutex_lock(&lock);
    object *o = lookup(id, 0);
    vgo_status st = o ? drop(o) : VGO_BAD_HANDLE;
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_state_get(vgo_handle id, vgo_state *out) {
    if (!out) return VGO_INVALID;
    pthread_mutex_lock(&lock);
    object *o = lookup(id, 0);
    if (o) {
        *out = (vgo_state){o->kind, o->share ? o->share->id : 0,
            o->pipeline ? o->pipeline->id : 0, o->refs,
            (uint32_t)o->attached, (uint32_t)o->consumed, o->generation,
            o->views.descriptor, o->views.state};
    }
    pthread_mutex_unlock(&lock);
    return o ? VGO_OK : VGO_BAD_HANDLE;
}
vgo_status vgo_detach_views(vgo_handle share, vgo_handle id) {
    pthread_mutex_lock(&lock);
    object *o;
    vgo_status st = owned(share, id, 0, &o);
    if (st == VGO_OK) {
        if (o->releasing) st = VGO_CONTEXT_FAILED;
        else if (!o->attached) st = VGO_DETACHED;
        else {
            if (o->views.lease.release) o->views.lease.release(o->views.lease.context);
            memset(&o->views, 0, sizeof(o->views));
            o->attached = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return st;
}
static vgo_status destroy(vgo_handle share, vgo_handle id, vgo_kind kind) {
    pthread_mutex_lock(&lock);
    object *o;
    vgo_status st = owned(share, id, kind, &o);
    if (st == VGO_OK) st = drop(o);
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_sampler_destroy(vgo_handle s, vgo_handle id) { return destroy(s, id, VGO_SAMPLER); }
vgo_status vgo_pipeline_destroy(vgo_handle s, vgo_handle id) { return destroy(s, id, VGO_PIPELINE); }
vgo_status vgo_program_destroy(vgo_handle s, vgo_handle id) { return destroy(s, id, VGO_PROGRAM); }
vgo_status vgo_sampler_consume(vgo_handle share, vgo_handle id,
                               vgo_sampler_consumer consumer, void *context) {
    pthread_mutex_lock(&lock);
    object *o;
    vgo_status st = owned(share, id, VGO_SAMPLER, &o);
    if (st != VGO_OK) goto done;
    if (o->releasing) { st = VGO_CONTEXT_FAILED; goto done; }
    if (!o->attached) { st = VGO_DETACHED; goto done; }
    unsigned char *su = o->views.state.data;
    if (!su[0] && o->consumed && (!consumer ||
        (consumer == o->consumer && context == o->consumer_context))) goto done;
    if (!consumer) { st = VGO_NO_CONSUMER; goto done; }
    if (o->generation == UINT64_MAX) { st = VGO_OVERFLOW; goto done; }
    st = consumer(context, o->views.descriptor, su[0]);
    if (st == VGO_OK) {
        su[0] = 0;
        o->consumed = 1;
        o->consumer = consumer;
        o->consumer_context = context;
        ++o->generation;
    }
done:
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_pipeline_modify(vgo_handle share, vgo_handle id, uint32_t flags) {
    pthread_mutex_lock(&lock);
    object *o;
    vgo_status st = owned(share, id, VGO_PIPELINE, &o);
    if (st == VGO_OK) {
        if (o->releasing) st = VGO_CONTEXT_FAILED;
        else if (!o->attached) st = VGO_DETACHED;
        else if (flags & ~UINT32_C(5)) st = VGO_UNSUPPORTED;
        else if (flags) {
            if (o->generation == UINT64_MAX) st = VGO_OVERFLOW;
            else ++o->generation;
        }
    }
    pthread_mutex_unlock(&lock);
    return st;
}
static vgo_status binding(vgo_handle context, vgo_handle id, int bind) {
    pthread_mutex_lock(&lock);
    object *c = lookup(context, VGO_CONTEXT), *p = lookup(id, VGO_PIPELINE);
    vgo_status st = VGO_BAD_HANDLE;
    if (!c || !p) goto done;
    if (c->releasing || p->releasing) { st = VGO_CONTEXT_FAILED; goto done; }
    if (c->share != p->share) { st = VGO_OWNER_MISMATCH; goto done; }
    st = VGO_OK;
    if (bind) {
        if (!p->attached) { st = VGO_DETACHED; goto done; }
        if (c->pipeline == p) goto done;
        st = hold(p);
        if (st != VGO_OK) goto done;
        st = drop(c->pipeline);
        if (st != VGO_OK) { (void)drop(p); goto done; }
        c->pipeline = p;
    } else if (c->pipeline == p) {
        st = drop(p);
        if (st == VGO_OK) c->pipeline = NULL;
    }
done:
    pthread_mutex_unlock(&lock);
    return st;
}
vgo_status vgo_pipeline_bind(vgo_handle c, vgo_handle p) { return binding(c, p, 1); }
vgo_status vgo_pipeline_unbind(vgo_handle c, vgo_handle p) { return binding(c, p, 0); }
