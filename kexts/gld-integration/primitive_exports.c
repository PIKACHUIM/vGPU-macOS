#include "primitive_exports.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float) == 4, "需要 32 位 float");
_Static_assert(sizeof(uintptr_t) == 8, "需要 64 位宿主");
_Static_assert(sizeof(gi_primitive_colored_vertex) == 32, "彩色顶点必须为 32B");
_Static_assert(offsetof(gi_primitive_colored_vertex,rgba) == 16, "颜色偏移必须为 16B");
const unsigned char gi_primitive_colored_descriptor[GI_PRIMITIVE_COLORED_DESCRIPTOR_SIZE] =
    {2,0,8,0,0,0x18,0,0,4,0x98,0,0};
typedef struct primitive_context primitive_context;
struct primitive_context {
    primitive_context *next;
    gi_primitive_handle handle;
    gi_primitive_backend backend;
    gi_primitive_status status;
    IOReturn batch_failure;
    void *storage, *vertices;
    size_t storage_size, stride;
    int colored;
};
static primitive_context *contexts;
static gi_primitive_handle next_handle = 1;
static int range_ok(const void *p, size_t size) {
    return p && size <= UINTPTR_MAX - (uintptr_t)p;
}
static int overlaps(const void *a, size_t n, const void *b, size_t m) {
    if (!range_ok(a,n) || !range_ok(b,m)) return 1;
    return (uintptr_t)a < (uintptr_t)b + m && (uintptr_t)b < (uintptr_t)a + n;
}
static int output_ok(const void *p, size_t size, size_t alignment) {
    if (!range_ok(p,size) || (uintptr_t)p % alignment) return 0;
    for (primitive_context *c = contexts; c; c = c->next)
        if (overlaps(p,size,c,sizeof(*c)) || overlaps(p,size,c->storage,c->storage_size)) return 0;
    return 1;
}
static primitive_context *lookup(gi_primitive_handle handle) {
    for (primitive_context *c = contexts; c; c = c->next)
        if (c->handle == handle) return c;
    return NULL;
}
static IOReturn fail(primitive_context *c, IOReturn result) {
    if (!c->status.pending_failure) c->status.pending_failure = result;
    c->status.last_failure = result;
    if (c->status.active && !c->batch_failure) c->batch_failure = result;
    return result;
}
static int valid_state(const gi_primitive_state_desc *s) {
    static const unsigned char position[8] = {1,0,4,0,0,0x18,0,0};
    if (s->version != GI_PRIMITIVE_STATE_VERSION || s->size != sizeof(*s) ||
        s->all_state_verified != 1 || s->full_target_viewport != 1 ||
        s->color_write_mask != 15 || s->samples != 1 || s->additional_state ||
        memcmp(s->position_descriptor,position,sizeof(position))) return 0;
    for (size_t i = 0; i < 16; ++i) {
        float expected = i % 5 == 0 ? 1.0f : 0.0f;
        if (s->modelview[i] != expected || s->projection[i] != expected) return 0;
    }
    return s->rgba[0] == 1.0f && s->rgba[1] == 0.0f &&
           s->rgba[2] == 0.0f && s->rgba[3] == 1.0f;
}
static IOReturn create(const gi_primitive_backend *backend, gi_primitive_handle *out, int colored) {
    if (!range_ok(backend,sizeof(*backend)) ||
        !output_ok(out,sizeof(*out),_Alignof(gi_primitive_handle)) ||
        overlaps(out,sizeof(*out),backend,sizeof(*backend))) return kIOReturnBadArgument;
    gi_primitive_backend copy;
    memcpy(&copy,backend,sizeof(copy));
    if (!copy.owner || !copy.retain || !copy.release || !copy.submit || !copy.flush)
        return kIOReturnBadArgument;
    if (!next_handle) return kIOReturnNoResources;
    primitive_context *c = calloc(1,sizeof(*c));
    if (!c) return kIOReturnNoMemory;
    c->colored = colored;
    c->stride = colored ? sizeof(gi_primitive_colored_vertex) : sizeof(float[4]);
    if ((size_t)GI_PRIMITIVE_CAPACITY > (SIZE_MAX - 15u) / c->stride) {
        free(c); return kIOReturnNoResources;
    }
    c->storage_size = (size_t)GI_PRIMITIVE_CAPACITY * c->stride + 15u;
    c->storage = malloc(c->storage_size);
    if (!c->storage) { free(c); return kIOReturnNoMemory; }
    c->vertices = (void *)(((uintptr_t)c->storage + 15u) & ~(uintptr_t)15u);
    c->status.configured = colored ? 1u : 0u;
    IOReturn r = copy.retain(copy.owner);
    if (r) { free(c->storage); free(c); return r; }
    c->backend = copy;
    c->handle = next_handle++;
    c->next = contexts; contexts = c;
    *out = c->handle;
    return kIOReturnSuccess;
}
IOReturn gi_primitive_create(const gi_primitive_backend *backend, gi_primitive_handle *out) {
    return create(backend,out,0);
}
IOReturn gi_primitive_create_colored_probe(const gi_primitive_backend *backend, gi_primitive_handle *out) {
    return create(backend,out,1);
}
IOReturn gi_primitive_destroy(gi_primitive_handle handle) {
    primitive_context *c = lookup(handle);
    if (!c) return kIOReturnBadArgument;
    if (c->status.active) return fail(c,kIOReturnExclusiveAccess);
    c->status.destroying = 1;
    IOReturn r = c->backend.release(c->backend.owner);
    if (r) return fail(c,r);
    primitive_context **p = &contexts;
    while (*p != c) p = &(*p)->next;
    *p = c->next;
    free(c->storage);
    free(c);
    return kIOReturnSuccess;
}
IOReturn gi_primitive_set_state(gi_primitive_handle handle, const gi_primitive_state_desc *state) {
    primitive_context *c = lookup(handle);
    if (!c) return kIOReturnBadArgument;
    if (c->status.destroying) return fail(c,kIOReturnNotReady);
    if (c->colored) return fail(c,kIOReturnUnsupported);
    c->status.configured = 0;
    if (c->status.active) return fail(c,kIOReturnExclusiveAccess);
    if (!range_ok(state,sizeof(*state))) return fail(c,kIOReturnBadArgument);
    gi_primitive_state_desc copy;
    memcpy(&copy,state,sizeof(copy));
    if (!valid_state(&copy)) return fail(c,kIOReturnUnsupported);
    c->status.configured = 1;
    return kIOReturnSuccess;
}
static IOReturn begin(gi_primitive_handle handle, uint32_t primitive,
                      int32_t *capacity_inout, void *out, int colored) {
    primitive_context *c = lookup(handle);
    if (!c) return kIOReturnBadArgument;
    if (c->status.destroying) return fail(c,kIOReturnNotReady);
    if (c->status.active) return fail(c,kIOReturnExclusiveAccess);
    if (c->colored != colored) return fail(c,kIOReturnUnsupported);
    size_t out_size = colored ? sizeof(gi_primitive_colored_vertex *) : sizeof(float (*)[4]);
    size_t out_alignment = colored ? _Alignof(gi_primitive_colored_vertex *) : _Alignof(float (*)[4]);
    if (!output_ok(capacity_inout,sizeof(*capacity_inout),_Alignof(int32_t)) ||
        !output_ok(out,out_size,out_alignment) ||
        overlaps(out,out_size,capacity_inout,sizeof(*capacity_inout)))
        return fail(c,kIOReturnBadArgument);
    int32_t capacity = *capacity_inout;
    if (capacity < 0) return fail(c,kIOReturnBadArgument);
    if (!c->status.configured) return fail(c,kIOReturnNotReady);
    if (primitive != GI_PRIMITIVE_TRIANGLES) return fail(c,kIOReturnUnsupported);
    if (!capacity) {
        if (colored) *(gi_primitive_colored_vertex **)out = NULL;
        else *(float (**)[4])out = NULL;
        return kIOReturnSuccess;
    }
    if (capacity > GI_PRIMITIVE_CAPACITY) capacity = GI_PRIMITIVE_CAPACITY;
    const float unwritten = NAN;
    for (size_t offset = 0; offset < (size_t)GI_PRIMITIVE_CAPACITY * c->stride; offset += sizeof(float))
        memcpy((unsigned char *)c->vertices + offset,&unwritten,sizeof(unwritten));
    c->status.capacity = capacity;
    c->status.active = 1;
    c->batch_failure = kIOReturnSuccess;
    *capacity_inout = capacity;
    if (colored) *(gi_primitive_colored_vertex **)out = c->vertices;
    else *(float (**)[4])out = c->vertices;
    return kIOReturnSuccess;
}
IOReturn gi_primitive_begin(gi_primitive_handle handle, uint32_t primitive,
                           int32_t *capacity_inout, float (**out)[4]) {
    return begin(handle,primitive,capacity_inout,out,0);
}
IOReturn gi_primitive_begin_colored(gi_primitive_handle handle, uint32_t primitive,
                                   int32_t *capacity_inout, gi_primitive_colored_vertex **out) {
    return begin(handle,primitive,capacity_inout,out,1);
}
IOReturn gi_primitive_end(gi_primitive_handle handle, uint32_t flags, uint32_t primitive,
                         int32_t count, uint64_t *generation) {
    primitive_context *c = lookup(handle);
    if (!c) return kIOReturnBadArgument;
    if (c->status.destroying) return fail(c,kIOReturnNotReady);
    if (!c->status.active) return fail(c,kIOReturnNotReady);
    int32_t capacity = c->status.capacity;
    c->status.active = 0; c->status.capacity = 0;
    if (c->batch_failure) return fail(c,c->batch_failure);
    if (!output_ok(generation,sizeof(*generation),_Alignof(uint64_t)))
        return fail(c,kIOReturnBadArgument);
    if (flags != GI_PRIMITIVE_END_FLAGS || primitive != GI_PRIMITIVE_TRIANGLES)
        return fail(c,kIOReturnUnsupported);
    if (count < 0 || count > capacity) return fail(c,kIOReturnBadArgument);
    if (!count) return kIOReturnSuccess;
    if (count != 3) return fail(c,kIOReturnUnsupported);
    float vertices[3][4];
    if (c->colored) {
        gi_primitive_colored_vertex snapshot[3];
        memcpy(snapshot,c->vertices,sizeof(snapshot));
        for (size_t i = 0; i < 3; ++i) {
            for (size_t j = 0; j < 4; ++j)
                if (!isfinite(snapshot[i].rgba[j])) return fail(c,kIOReturnBadArgument);
            if (snapshot[i].rgba[0] != 1.0f || snapshot[i].rgba[1] != 0.0f ||
                snapshot[i].rgba[2] != 0.0f || snapshot[i].rgba[3] != 1.0f)
                return fail(c,kIOReturnUnsupported);
            memcpy(vertices[i],snapshot[i].position,sizeof(vertices[i]));
        }
    } else memcpy(vertices,c->vertices,sizeof(vertices));
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j)
            if (!isfinite(vertices[i][j]) || vertices[i][j] < -1.0f || vertices[i][j] > 1.0f)
                return fail(c,kIOReturnBadArgument);
        if (vertices[i][3] != 1.0f) return fail(c,kIOReturnBadArgument);
        vertices[i][2] = (vertices[i][2] + 1.0f) * 0.5f;
    }
    uint64_t completed = 0;
    IOReturn r = c->backend.submit(c->backend.owner,(const float (*)[4])vertices,&completed);
    if (r) return fail(c,r);
    if (!completed) return fail(c,kIOReturnNotReady);
    c->status.pending_failure = kIOReturnSuccess;
    c->status.completed_generation = completed;
    *generation = completed;
    return kIOReturnSuccess;
}
IOReturn gi_primitive_flush(gi_primitive_handle handle, uint64_t *generation) {
    primitive_context *c = lookup(handle);
    if (!c) return kIOReturnBadArgument;
    if (c->status.destroying) return fail(c,kIOReturnNotReady);
    if (!output_ok(generation,sizeof(*generation),_Alignof(uint64_t)))
        return fail(c,kIOReturnBadArgument);
    if (c->status.active) return fail(c,kIOReturnExclusiveAccess);
    if (c->status.pending_failure) return c->status.pending_failure;
    if (!c->status.completed_generation) return kIOReturnNotReady;
    uint64_t completed = 0;
    IOReturn r = c->backend.flush(c->backend.owner,&completed);
    if (r) return fail(c,r);
    if (completed != c->status.completed_generation) return fail(c,kIOReturnNotReady);
    *generation = completed;
    return kIOReturnSuccess;
}
IOReturn gi_primitive_get_status(gi_primitive_handle handle, gi_primitive_status *out) {
    primitive_context *c = lookup(handle);
    if (!c || !output_ok(out,sizeof(*out),_Alignof(gi_primitive_status))) return kIOReturnBadArgument;
    *out = c->status;
    return kIOReturnSuccess;
}
