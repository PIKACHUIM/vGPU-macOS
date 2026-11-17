#include "tcl_exports.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define STAGING_BYTES ((size_t)GI_TCL_CAPACITY * GI_TCL_STRIDE)
#define GUARD_BYTES 16U
static const unsigned char descriptor_bytes[GI_TCL_DESCRIPTOR_SIZE] = {
    2, 0, 8, 0, 0, 0x18, 0, 0, 4, 0x98, 0, 0
};
struct gi_tcl {
    gi_tcl_backend backend;
    gi_tcl_status status;
    uint32_t lease_rejected;
    unsigned char descriptor[GI_TCL_DESCRIPTOR_SIZE];
    unsigned char storage[48U * 32U + 2U * GUARD_BYTES + 15U];
    unsigned char *sink;
};
_Static_assert(sizeof(float) == 4, "TCL requires float32");
_Static_assert(sizeof(gi_tcl_vertex) == GI_TCL_STRIDE, "TCL stride mismatch");

static void reject(gi_tcl *c, gi_tcl_detail detail, int fault)
{
    c->status.pending_rejection = 1;
    c->lease_rejected = 1;
    c->status.last_detail = detail;
    if (fault) c->status.backend_fault = 1;
    c->backend.error(c->backend.owner, GI_TCL_GL_INVALID_OPERATION, detail);
}

static int check_storage(gi_tcl *c)
{
    size_t i;
    for (i = 0; i < GUARD_BYTES; ++i) {
        if (c->sink[-(ptrdiff_t)GUARD_BYTES + (ptrdiff_t)i] != 0xa5 ||
            c->sink[STAGING_BYTES + i] != 0x5a) {
            reject(c, GI_TCL_DETAIL_CANARY, 1);
            return 0;
        }
    }
    if (memcmp(c->descriptor, descriptor_bytes, sizeof(descriptor_bytes)) != 0) {
        reject(c, GI_TCL_DETAIL_DESCRIPTOR, 1);
        return 0;
    }
    return 1;
}

static void validate(gi_tcl *c)
{
    gi_tcl_result result = c->backend.validate(c->backend.owner);
    if (result != GI_TCL_OK)
        reject(c, GI_TCL_DETAIL_STATE, result == GI_TCL_BACKEND_FAULT);
}

gi_tcl_result gi_tcl_create(const gi_tcl_backend *backend, uint32_t domain, gi_tcl **out)
{
    gi_tcl *c;
    gi_tcl_result result;
    uintptr_t base;
    if (!backend || !out || !backend->owner || !backend->retain || !backend->release ||
        !backend->validate || !backend->submit || !backend->error ||
        domain != GI_TCL_CONTROLLED_LEGACY) return GI_TCL_INVALID;
    c = calloc(1, sizeof(*c));
    if (!c) return GI_TCL_NO_MEMORY;
    c->backend = *backend;
    base = (uintptr_t)(c->storage + GUARD_BYTES);
    c->sink = (unsigned char *)((base + 15U) & ~(uintptr_t)15U);
    memset(c->sink - GUARD_BYTES, 0xa5, GUARD_BYTES);
    memset(c->sink + STAGING_BYTES, 0x5a, GUARD_BYTES);
    memcpy(c->descriptor, descriptor_bytes, sizeof(descriptor_bytes));
    result = backend->retain(backend->owner);
    if (result != GI_TCL_OK) {
        free(c);
        return result;
    }
    *out = c;
    return GI_TCL_OK;
}

unsigned char *gi_tcl_descriptor(gi_tcl *context)
{
    return context ? context->descriptor : NULL;
}

void *gi_tcl_alternate_descriptor(gi_tcl *context)
{
    (void)context;
    return NULL;
}

gi_tcl_result gi_tcl_destroy(gi_tcl **context)
{
    gi_tcl *c;
    gi_tcl_result result;
    if (!context || !*context) return GI_TCL_INVALID;
    c = *context;
    if (c->status.active) return GI_TCL_BUSY;
    c->status.destroying = 1;
    result = c->backend.release(c->backend.owner);
    if (result != GI_TCL_OK) {
        reject(c, GI_TCL_DETAIL_RELEASE, 1);
        return result;
    }
    free(c);
    *context = NULL;
    return GI_TCL_OK;
}

void *gi_tcl_begin(gi_tcl *c, uint32_t primitive, int32_t *count_inout)
{
    int32_t input;
    size_t i;
    gi_tcl_vertex poison;
    uint32_t was_active;
    if (!c) return NULL;
    input = count_inout ? *count_inout : -1;
    was_active = c->status.active;
    c->lease_rejected = 0;
    c->status.active = 1;
    (void)check_storage(c);
    if (was_active) reject(c, GI_TCL_DETAIL_LEASE, 0);
    if (c->status.destroying) reject(c, GI_TCL_DETAIL_DESTROYING, 1);
    if (input != 0) reject(c, GI_TCL_DETAIL_MODE, 0);
    if (primitive != 4) reject(c, GI_TCL_DETAIL_PRIMITIVE, 0);
    validate(c);
    for (i = 0; i < 4; ++i) {
        poison.position[i] = NAN;
        poison.rgba[i] = NAN;
    }
    for (i = 0; i < (size_t)GI_TCL_CAPACITY; ++i)
        memcpy(c->sink + i * GI_TCL_STRIDE, &poison, sizeof(poison));
    if (count_inout) *count_inout = GI_TCL_CAPACITY;
    return c->sink;
}

void gi_tcl_end(gi_tcl *c, uint32_t flags, uint32_t primitive, uint32_t count)
{
    gi_tcl_vertex snapshot[3];
    float positions[3][4];
    uint64_t generation;
    gi_tcl_result result;
    size_t i, j;
    if (!c) return;
    if (!c->status.active) {
        reject(c, GI_TCL_DETAIL_LEASE, 0);
        return;
    }
    c->status.active = 0;
    (void)check_storage(c);
    validate(c);
    if (count > (uint32_t)GI_TCL_CAPACITY) {
        reject(c, GI_TCL_DETAIL_COUNT, 0);
        return;
    }
    /* 在任何解码之前消费未知拓扑及续段；不假定 Begin/End 拓扑总相同。 */
    if (primitive != 4) {
        reject(c, GI_TCL_DETAIL_PRIMITIVE, 0);
        return;
    }
    if (flags != UINT32_C(0x8002)) {
        reject(c, GI_TCL_DETAIL_SEGMENT, 0);
        return;
    }
    if (!count) return;
    if (count != 3) {
        reject(c, GI_TCL_DETAIL_COUNT, 0);
        return;
    }
    if (c->lease_rejected || c->status.backend_fault || c->status.destroying) return;
    memcpy(snapshot, c->sink, sizeof(snapshot));
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 4; ++j) {
            if (!isfinite(snapshot[i].position[j]) || !isfinite(snapshot[i].rgba[j])) {
                reject(c, GI_TCL_DETAIL_VERTEX, 0);
                return;
            }
        }
        for (j = 0; j < 3; ++j) {
            if (snapshot[i].position[j] < -1.0f || snapshot[i].position[j] > 1.0f) {
                reject(c, GI_TCL_DETAIL_VERTEX, 0);
                return;
            }
        }
        if (snapshot[i].position[3] != 1.0f || snapshot[i].rgba[0] != 1.0f ||
            snapshot[i].rgba[1] != 0.0f || snapshot[i].rgba[2] != 0.0f ||
            snapshot[i].rgba[3] != 1.0f) {
            reject(c, GI_TCL_DETAIL_VERTEX, 0);
            return;
        }
        memcpy(positions[i], snapshot[i].position, sizeof(positions[i]));
        /* Resource rows are top-down and readback preserves row indices.
         * Map GL's bottom-up window Y here, leaving the resource ABI unchanged. */
        positions[i][1] = -positions[i][1];
        positions[i][2] = (positions[i][2] + 1.0f) * 0.5f;
    }
    generation = c->status.completed_generation;
    result = c->backend.submit(c->backend.owner, (const float (*)[4])positions, &generation);
    if (result != GI_TCL_OK) {
        reject(c, GI_TCL_DETAIL_SUBMIT, 1);
        return;
    }
    c->status.completed_generation = generation;
    c->status.pending_rejection = 0;
}

void gi_tcl_invalidate(gi_tcl *context, gi_tcl_detail detail)
{
    if (context) reject(context, detail == GI_TCL_DETAIL_NONE ? GI_TCL_DETAIL_INVALIDATED : detail, 0);
}

void gi_tcl_fault(gi_tcl *context, gi_tcl_detail detail)
{
    if (context) reject(context, detail, 1);
}

gi_tcl_result gi_tcl_get_status(const gi_tcl *context, gi_tcl_status *out)
{
    if (!context || !out) return GI_TCL_INVALID;
    *out = context->status;
    return GI_TCL_OK;
}
