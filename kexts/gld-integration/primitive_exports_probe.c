#include "primitive_exports.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 此 probe 只链接 primitive 模块，以同步 CPU 回调代替 GPU。 */
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(2); } } while (0)
#define OK(x) CHECK((x) == kIOReturnSuccess)
static unsigned checks;
static unsigned allocations, fail_allocation, live_allocations;
static size_t last_malloc_size;
void *gi_primitive_test_malloc(size_t size);
void *gi_primitive_test_calloc(size_t count, size_t size);
void gi_primitive_test_free(void *pointer);
void *gi_primitive_test_malloc(size_t size) {
    last_malloc_size = size;
    ++allocations;
    if (allocations == fail_allocation) return NULL;
    void *p = malloc(size); if (p) ++live_allocations; return p;
}
void *gi_primitive_test_calloc(size_t count, size_t size) {
    ++allocations;
    if (allocations == fail_allocation) return NULL;
    void *p = calloc(count,size); if (p) ++live_allocations; return p;
}
void gi_primitive_test_free(void *p) {
    if (p) { CHECK(live_allocations != 0); --live_allocations; }
    free(p);
}
typedef struct {
    unsigned refs, retains, releases, submissions, flushes;
    IOReturn retain_error, release_error, submit_error, flush_error;
    uint64_t generation;
    int zero_generation, stale;
    float captured[3][4];
    float (*staging)[4];
    gi_primitive_colored_vertex *colored_staging;
    int mutate_colored_staging;
} cpu_backend;
static IOReturn retain(void *owner) {
    cpu_backend *b = owner; ++b->retains;
    if (b->retain_error) return b->retain_error;
    ++b->refs; return kIOReturnSuccess;
}
static IOReturn release(void *owner) {
    cpu_backend *b = owner; ++b->releases;
    if (b->release_error) return b->release_error;
    CHECK(b->refs != 0); --b->refs; return kIOReturnSuccess;
}
static IOReturn submit(void *owner, const float vertices[3][4], uint64_t *out) {
    cpu_backend *b = owner; ++b->submissions;
    CHECK(b->refs != 0);
    CHECK((const void *)vertices != (const void *)b->staging);
    if (b->colored_staging) {
        CHECK((const void *)vertices != (const void *)b->colored_staging);
        /* 故障注入：改变源缓冲，验证回调实际消费独立快照。 */
        if (b->mutate_colored_staging)
            memset(b->colored_staging,0,3 * sizeof(*b->colored_staging));
    }
    memcpy(b->captured,vertices,sizeof(b->captured));
    *out = UINT64_C(0xdead);
    if (b->submit_error) return b->submit_error;
    ++b->generation; *out = b->zero_generation ? 0 : b->generation;
    return kIOReturnSuccess;
}
static IOReturn flush(void *owner, uint64_t *out) {
    cpu_backend *b = owner; ++b->flushes;
    CHECK(b->refs != 0);
    *out = UINT64_C(0xdead);
    if (b->flush_error) return b->flush_error;
    *out = b->generation + (b->stale ? 1u : 0u);
    return kIOReturnSuccess;
}
static gi_primitive_state_desc state(void) {
    gi_primitive_state_desc s = {0};
    s.version = 1; s.size = (uint32_t)sizeof(s); s.all_state_verified = 1;
    s.full_target_viewport = 1; s.color_write_mask = 15; s.samples = 1;
    for (size_t i = 0; i < 16; ++i) s.modelview[i] = s.projection[i] = i % 5 == 0 ? 1.0f : 0.0f;
    s.rgba[0] = s.rgba[3] = 1.0f;
    const unsigned char d[8] = {1,0,4,0,0,0x18,0,0};
    memcpy(s.position_descriptor,d,sizeof(d)); return s;
}
static float (*begin(gi_primitive_handle c, cpu_backend *b, int32_t requested))[4] {
    float (*p)[4] = NULL;
    int32_t capacity = requested;
    OK(gi_primitive_begin(c,4,&capacity,&p));
    CHECK(capacity == (requested > 64 ? 64 : requested));
    CHECK(p && (uintptr_t)p % 16 == 0);
    b->staging = p; return p;
}
static void fill(float p[3][4]) {
    const float v[3][4] = {{-1,-1,-1,1},{1,-1,0,1},{0,1,1,1}};
    memcpy(p,v,sizeof(v));
}
static void draw(gi_primitive_handle c, cpu_backend *b) {
    float (*p)[4] = begin(c,b,64); fill(p);
    uint64_t generation = UINT64_MAX;
    OK(gi_primitive_end(c,0x8002,4,3,&generation));
    CHECK(generation == b->generation);
    CHECK(b->captured[0][2] == 0 && b->captured[1][2] == 0.5f && b->captured[2][2] == 1);
    CHECK(p[0][2] == -1 && p[1][2] == 0 && p[2][2] == 1);
    for (size_t i=0;i<3;++i) {
        CHECK(b->captured[i][0] == p[i][0] && b->captured[i][1] == p[i][1]);
        CHECK(b->captured[i][3] == 1);
    }
    OK(gi_primitive_flush(c,&generation));
    CHECK(generation == b->generation);
}
static void blocked(gi_primitive_handle c, cpu_backend *b) {
    unsigned before = b->flushes;
    uint64_t out = UINT64_MAX;
    CHECK(gi_primitive_flush(c,&out) != kIOReturnSuccess);
    CHECK(out == UINT64_MAX && before == b->flushes);
    gi_primitive_status s;
    OK(gi_primitive_get_status(c,&s)); CHECK(s.pending_failure != 0);
}
static gi_primitive_colored_vertex *begin_colored(gi_primitive_handle c, cpu_backend *b,
                                                  int32_t requested) {
    gi_primitive_colored_vertex *p = NULL;
    int32_t capacity = requested;
    OK(gi_primitive_begin_colored(c,4,&capacity,&p));
    CHECK(capacity == (requested > 64 ? 64 : requested));
    CHECK(p && (uintptr_t)p % 16 == 0);
    b->colored_staging = p;
    return p;
}
static void fill_colored(gi_primitive_colored_vertex *p) {
    float positions[3][4]; fill(positions);
    for (size_t i=0;i<3;++i) {
        memcpy(p[i].position,positions[i],sizeof(positions[i]));
        p[i].rgba[0] = p[i].rgba[3] = 1;
        p[i].rgba[1] = p[i].rgba[2] = -0.0f;
    }
}
static void draw_colored(gi_primitive_handle c, cpu_backend *b) {
    gi_primitive_colored_vertex *p = begin_colored(c,b,64);
    fill_colored(p);
    gi_primitive_colored_vertex saved[3]; memcpy(saved,p,sizeof(saved));
    uint64_t generation = UINT64_MAX;
    OK(gi_primitive_end(c,0x8002,4,3,&generation));
    CHECK(generation == b->generation);
    CHECK(b->captured[0][2] == 0 && b->captured[1][2] == 0.5f && b->captured[2][2] == 1);
    for (size_t i=0;i<3;++i) {
        CHECK(b->captured[i][0] == saved[i].position[0]);
        CHECK(b->captured[i][1] == saved[i].position[1] && b->captured[i][3] == 1);
    }
    if (!b->mutate_colored_staging) CHECK(memcmp(saved,p,sizeof(saved)) == 0);
    OK(gi_primitive_flush(c,&generation)); CHECK(generation == b->generation);
}
static void colored_tests(void) {
    const unsigned char descriptor[12] = {2,0,8,0,0,0x18,0,0,4,0x98,0,0};
    CHECK(sizeof(gi_primitive_colored_vertex) == 32);
    CHECK(offsetof(gi_primitive_colored_vertex,rgba) == 16);
    CHECK(sizeof(gi_primitive_colored_descriptor) == sizeof(descriptor));
    CHECK(memcmp(gi_primitive_colored_descriptor,descriptor,sizeof(descriptor)) == 0);
    cpu_backend b = {.refs=1};
    gi_primitive_backend backend = {&b,retain,release,submit,flush};
    gi_primitive_handle c = UINT64_MAX;
    CHECK(gi_primitive_create_colored_probe(NULL,&c) == kIOReturnBadArgument);
    CHECK(gi_primitive_create_colored_probe(&backend,NULL) == kIOReturnBadArgument);
    CHECK(gi_primitive_create_colored_probe(&backend,(gi_primitive_handle *)&backend) == kIOReturnBadArgument);
    for (unsigned i=1;i<=2;++i) {
        allocations = 0; fail_allocation = i;
        CHECK(gi_primitive_create_colored_probe(&backend,&c) == kIOReturnNoMemory);
        CHECK(c == UINT64_MAX && b.refs == 1 && b.retains == 0 && live_allocations == 0);
    }
    fail_allocation = 0; b.retain_error = kIOReturnNoResources;
    CHECK(gi_primitive_create_colored_probe(&backend,&c) == kIOReturnNoResources);
    CHECK(c == UINT64_MAX && b.refs == 1 && live_allocations == 0);
    b.retain_error = 0;
    OK(gi_primitive_create_colored_probe(&backend,&c));
    CHECK(last_malloc_size == (size_t)GI_PRIMITIVE_CAPACITY * 32 + 15);
    CHECK(b.refs == 2 && live_allocations == 2);
    gi_primitive_status status;
    OK(gi_primitive_get_status(c,&status)); CHECK(status.configured && !status.active);
    uint64_t out = UINT64_MAX;
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNotReady && out == UINT64_MAX && !b.flushes);
    int32_t capacity = 3;
    float (*old)[4] = (float (*)[4])(uintptr_t)16;
    CHECK(gi_primitive_begin(c,4,&capacity,&old) == kIOReturnUnsupported);
    CHECK(capacity == 3 && (uintptr_t)old == 16); blocked(c,&b);
    gi_primitive_state_desc good = state();
    CHECK(gi_primitive_set_state(c,&good) == kIOReturnUnsupported); blocked(c,&b);
    draw_colored(c,&b);
    b.mutate_colored_staging = 1; draw_colored(c,&b); b.mutate_colored_staging = 0;
    gi_primitive_colored_vertex *p = begin_colored(c,&b,INT32_MAX);
    for (size_t i=0;i<64;++i) {
        CHECK((uintptr_t)&p[i] % 16 == 0 && (uintptr_t)p[i].rgba % 16 == 0);
        for (size_t j=0;j<4;++j) CHECK(isnan(p[i].position[j]) && isnan(p[i].rgba[j]));
    }
    p[63].position[3] = p[63].rgba[3] = 1;
    fill_colored(p); OK(gi_primitive_end(c,0x8002,4,3,&out));
    /* 三次顶点写入分别捕获红、绿、红，首尾均红不能掩盖中间绿。 */
    p = begin_colored(c,&b,64); fill_colored(p);
    p[1].rgba[0] = 0; p[1].rgba[1] = 1;
    unsigned before = b.submissions;
    uint64_t completed = b.generation; out = UINT64_MAX;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnUnsupported);
    CHECK(before == b.submissions && out == UINT64_MAX);
    OK(gi_primitive_get_status(c,&status));
    CHECK(!status.active && !status.capacity && status.completed_generation == completed);
    blocked(c,&b);
    p = begin_colored(c,&b,64); out = UINT64_MAX;
    OK(gi_primitive_end(c,0x8002,4,0,&out)); CHECK(out == UINT64_MAX); blocked(c,&b);
    draw_colored(c,&b);
    const float bad_colors[] = {NAN,INFINITY,-INFINITY,0.25f};
    for (size_t vertex=0;vertex<3;++vertex)
        for (size_t channel=0;channel<4;++channel)
            for (size_t value=0;value<4;++value) {
                p = begin_colored(c,&b,64); fill_colored(p);
                p[vertex].rgba[channel] = bad_colors[value];
                before = b.submissions; out = UINT64_MAX;
                IOReturn expected = value == 3 ? kIOReturnUnsupported : kIOReturnBadArgument;
                CHECK(gi_primitive_end(c,0x8002,4,3,&out) == expected);
                CHECK(before == b.submissions && out == UINT64_MAX); blocked(c,&b);
            }
    const float invalid[] = {NAN,INFINITY,-INFINITY,1.001f,-1.001f};
    for (size_t vertex=0;vertex<3;++vertex)
        for (size_t axis=0;axis<4;++axis)
            for (size_t value=0;value<5;++value) {
                p = begin_colored(c,&b,64); fill_colored(p);
                p[vertex].position[axis] = invalid[value];
                before = b.submissions; out = UINT64_MAX;
                CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
                CHECK(before == b.submissions && out == UINT64_MAX); blocked(c,&b);
            }
    p = begin_colored(c,&b,64); before = b.submissions;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
    CHECK(before == b.submissions); blocked(c,&b); draw_colored(c,&b);
    const int32_t counts[] = {INT32_MIN,-1,1,2,4,63,64,65,INT32_MAX};
    for (size_t i=0;i<sizeof(counts)/sizeof(counts[0]);++i) {
        p = begin_colored(c,&b,64); fill_colored(p); before = b.submissions; out = UINT64_MAX;
        CHECK(gi_primitive_end(c,0x8002,4,counts[i],&out) != kIOReturnSuccess);
        CHECK(before == b.submissions && out == UINT64_MAX);
        OK(gi_primitive_get_status(c,&status)); CHECK(!status.active); blocked(c,&b);
    }
    p = begin_colored(c,&b,2); fill_colored(p); before = b.submissions;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
    CHECK(before == b.submissions); blocked(c,&b);
    capacity = INT32_MIN; p = (gi_primitive_colored_vertex *)(uintptr_t)16;
    CHECK(gi_primitive_begin_colored(c,4,&capacity,&p) == kIOReturnBadArgument);
    CHECK(capacity == INT32_MIN && (uintptr_t)p == 16);
    capacity = 0; OK(gi_primitive_begin_colored(c,4,&capacity,&p)); CHECK(!p); blocked(c,&b);
    union { uint64_t alignment; unsigned char bytes[80]; } alias = {0};
    capacity = 3;
    CHECK(gi_primitive_begin_colored(c,4,(int32_t *)alias.bytes,
                                   (gi_primitive_colored_vertex **)alias.bytes) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,(int32_t *)(alias.bytes+1),&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,&capacity,
                                   (gi_primitive_colored_vertex **)(alias.bytes+1)) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,(int32_t *)(uintptr_t)(UINTPTR_MAX-1),&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,&capacity,
                                   (gi_primitive_colored_vertex **)(uintptr_t)(UINTPTR_MAX-7)) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,NULL,&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,&capacity,NULL) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,5,&capacity,&p) == kIOReturnUnsupported);
    draw_colored(c,&b);
    for (unsigned mode=0;mode<11;++mode) {
        p = begin_colored(c,&b,64); fill_colored(p); before = b.submissions; out = UINT64_MAX;
        if (mode == 0) CHECK(gi_primitive_end(c,0,4,3,&out) == kIOReturnUnsupported);
        if (mode == 1) CHECK(gi_primitive_end(c,0x8002,5,3,&out) == kIOReturnUnsupported);
        if (mode == 2) CHECK(gi_primitive_end(c,0x8002,4,3,NULL) == kIOReturnBadArgument);
        if (mode == 3) CHECK(gi_primitive_end(c,0x8002,4,3,(uint64_t *)p[63].rgba) == kIOReturnBadArgument);
        if (mode == 4) CHECK(gi_primitive_end(c,0x8002,4,3,(uint64_t *)(alias.bytes+1)) == kIOReturnBadArgument);
        if (mode == 5) CHECK(gi_primitive_end(c,0x8002,4,3,(uint64_t *)(uintptr_t)(UINTPTR_MAX-7)) == kIOReturnBadArgument);
        if (mode >= 6) {
            if (mode == 6) CHECK(gi_primitive_flush(c,&out) == kIOReturnExclusiveAccess);
            if (mode == 7) CHECK(gi_primitive_destroy(c) == kIOReturnExclusiveAccess);
            if (mode == 8) CHECK(gi_primitive_begin_colored(c,4,&capacity,&p) == kIOReturnExclusiveAccess);
            if (mode == 9) CHECK(gi_primitive_begin(c,4,&capacity,&old) == kIOReturnExclusiveAccess);
            if (mode == 10) CHECK(gi_primitive_set_state(c,&good) == kIOReturnUnsupported);
            CHECK(gi_primitive_end(c,0x8002,4,3,&out) != kIOReturnSuccess);
        }
        CHECK(before == b.submissions && out == UINT64_MAX); blocked(c,&b); draw_colored(c,&b);
    }
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnNotReady);
    draw_colored(c,&b);
    p = begin_colored(c,&b,64); fill_colored(p); b.submit_error = kIOReturnNoResources; out = UINT64_MAX;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnNoResources && out == UINT64_MAX);
    blocked(c,&b); b.submit_error = 0; draw_colored(c,&b);
    p = begin_colored(c,&b,64); fill_colored(p); b.zero_generation = 1; out = UINT64_MAX;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnNotReady && out == UINT64_MAX);
    blocked(c,&b); b.zero_generation = 0; draw_colored(c,&b);
    b.flush_error = kIOReturnNoResources; out = UINT64_MAX;
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNoResources && out == UINT64_MAX);
    b.flush_error = 0; blocked(c,&b); draw_colored(c,&b);
    b.stale = 1; out = UINT64_MAX;
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNotReady && out == UINT64_MAX);
    b.stale = 0; blocked(c,&b); draw_colored(c,&b);
    gi_primitive_handle second;
    OK(gi_primitive_create(&backend,&second));
    CHECK(last_malloc_size == 64 * 16 + 15);
    OK(gi_primitive_set_state(second,&good));
    capacity = 3; p = (gi_primitive_colored_vertex *)(uintptr_t)16;
    CHECK(gi_primitive_begin_colored(second,4,&capacity,&p) == kIOReturnUnsupported);
    CHECK(capacity == 3 && (uintptr_t)p == 16); blocked(second,&b);
    p = begin_colored(c,&b,64); fill_colored(p);
    CHECK(gi_primitive_begin(second,4,(int32_t *)p[63].rgba,&old) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin(second,4,&capacity,(float (**)[4])p[63].rgba) == kIOReturnBadArgument);
    CHECK(gi_primitive_get_status(second,(gi_primitive_status *)&p[62]) == kIOReturnBadArgument);
    CHECK(gi_primitive_create_colored_probe(&backend,(gi_primitive_handle *)p[63].rgba) == kIOReturnBadArgument);
    OK(gi_primitive_end(c,0x8002,4,3,&out));
    b.colored_staging = NULL; draw(second,&b);
    old = begin(second,&b,64); fill(old);
    CHECK(gi_primitive_begin_colored(c,4,(int32_t *)old,&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,&capacity,(gi_primitive_colored_vertex **)old) == kIOReturnBadArgument);
    OK(gi_primitive_end(second,0x8002,4,3,&out));
    OK(gi_primitive_destroy(second)); b.staging = NULL; draw_colored(c,&b);
    p = begin_colored(c,&b,64); before = b.submissions; out = UINT64_MAX;
    OK(gi_primitive_end(c,0x8002,4,0,&out));
    CHECK(before == b.submissions && out == UINT64_MAX);
    OK(gi_primitive_flush(c,&out)); CHECK(out == b.generation);
    --b.refs;
    b.release_error = kIOReturnNotReady;
    CHECK(gi_primitive_destroy(c) == kIOReturnNotReady && b.refs == 1 && live_allocations == 2);
    OK(gi_primitive_get_status(c,&status)); CHECK(status.destroying);
    capacity = 3;
    CHECK(gi_primitive_begin_colored(c,4,&capacity,&p) == kIOReturnNotReady);
    CHECK(gi_primitive_set_state(c,&good) == kIOReturnNotReady);
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnNotReady);
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNotReady);
    b.release_error = 0; OK(gi_primitive_destroy(c));
    CHECK(b.refs == 0 && live_allocations == 0);
    CHECK(gi_primitive_destroy(c) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin_colored(c,4,&capacity,&p) == kIOReturnBadArgument);
    printf("PASS primitive position+color CPU: %u submissions, %u flushes; no GPU\n",b.submissions,b.flushes);
}
int main(void) {
    cpu_backend b = {.refs=1};
    gi_primitive_backend backend = {&b,retain,release,submit,flush};
    gi_primitive_handle c = UINT64_MAX;
    gi_primitive_state_desc good = state();
    CHECK(gi_primitive_create(NULL,&c) == kIOReturnBadArgument && c == UINT64_MAX);
    CHECK(gi_primitive_create(&backend,NULL) == kIOReturnBadArgument);
    gi_primitive_backend bad_backend = backend;
    bad_backend.submit = NULL;
    CHECK(gi_primitive_create(&bad_backend,&c) == kIOReturnBadArgument);
    for (unsigned i=1;i<=2;++i) {
        allocations = 0; fail_allocation = i;
        CHECK(gi_primitive_create(&backend,&c) == kIOReturnNoMemory);
        CHECK(c == UINT64_MAX && b.refs == 1 && b.retains == 0 && live_allocations == 0);
    }
    fail_allocation = 0; b.retain_error = kIOReturnNoResources;
    CHECK(gi_primitive_create(&backend,&c) == kIOReturnNoResources);
    CHECK(c == UINT64_MAX && b.refs == 1 && live_allocations == 0);
    b.retain_error = 0;
    OK(gi_primitive_create(&backend,&c)); CHECK(b.refs == 2 && live_allocations == 2);
    uint64_t out = UINT64_MAX;
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNotReady && b.flushes == 0 && out == UINT64_MAX);
    int32_t capacity = 64; float (*p)[4] = (float (*)[4])(uintptr_t)16;
    CHECK(gi_primitive_begin(c,4,&capacity,&p) == kIOReturnNotReady);
    CHECK(capacity == 64 && (uintptr_t)p == 16); blocked(c,&b);
    OK(gi_primitive_set_state(c,&good));
    good.rgba[0] = 0; /* 描述符不是借用，不允许后续修改影响已接受配置。 */
    draw(c,&b); good = state();
    unsigned before;
    for (unsigned field=0;field<13;++field) {
        gi_primitive_state_desc bad = good;
        switch (field) {
        case 0: bad.version = 2; break;
        case 1: --bad.size; break;
        case 2: bad.all_state_verified = 0; break;
        case 3: bad.full_target_viewport = 0; break;
        case 4: bad.color_write_mask = 7; break;
        case 5: bad.samples = 4; break;
        case 6: bad.additional_state = UINT64_C(1) << 63; break;
        case 7: bad.modelview[1] = NAN; break;
        case 8: bad.projection[0] = INFINITY; break;
        case 9: bad.rgba[1] = 0.5f; break;
        case 10: bad.position_descriptor[5] = 0; break;
        case 11: bad.all_state_verified = 2; break;
        default: bad.modelview[15] = 2; break;
        }
        before = b.submissions;
        CHECK(gi_primitive_set_state(c,&bad) == kIOReturnUnsupported);
        capacity = 3;
        CHECK(gi_primitive_begin(c,4,&capacity,&p) == kIOReturnNotReady);
        CHECK(b.submissions == before); blocked(c,&b);
        OK(gi_primitive_set_state(c,&good)); blocked(c,&b); draw(c,&b);
    }
    CHECK(gi_primitive_set_state(c,NULL) == kIOReturnBadArgument);
    OK(gi_primitive_set_state(c,&good)); draw(c,&b);
    capacity = INT32_MIN; p = NULL;
    CHECK(gi_primitive_begin(c,4,&capacity,&p) == kIOReturnBadArgument);
    CHECK(capacity == INT32_MIN && !p); blocked(c,&b);
    capacity = 0; p = (float (*)[4])(uintptr_t)16;
    OK(gi_primitive_begin(c,4,&capacity,&p)); CHECK(!p); blocked(c,&b);
    capacity = 3;
    CHECK(gi_primitive_begin(c,5,&capacity,&p) == kIOReturnUnsupported);
    CHECK(gi_primitive_begin(c,4,NULL,&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin(c,4,&capacity,NULL) == kIOReturnBadArgument);
    draw(c,&b);
    p = begin(c,&b,INT32_MAX);
    p[63][3] = 1; /* 验证裁剪后确实可写满 64 顶点。 */
    fill(p); before = b.submissions;
    capacity = 3;
    CHECK(gi_primitive_begin(c,4,&capacity,&p) == kIOReturnExclusiveAccess);
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnExclusiveAccess);
    CHECK(before == b.submissions); blocked(c,&b); draw(c,&b);
    const int32_t counts[] = {INT32_MIN,-1,1,2,4,63,64,65,INT32_MAX};
    for (size_t i=0;i<sizeof(counts)/sizeof(counts[0]);++i) {
        p = begin(c,&b,64); fill(p); before = b.submissions; out = UINT64_MAX;
        CHECK(gi_primitive_end(c,0x8002,4,counts[i],&out) != kIOReturnSuccess);
        CHECK(out == UINT64_MAX && b.submissions == before);
        gi_primitive_status s; OK(gi_primitive_get_status(c,&s)); CHECK(!s.active);
        blocked(c,&b); draw(c,&b);
    }
    p = begin(c,&b,2); fill(p); before = b.submissions;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
    CHECK(before == b.submissions); draw(c,&b);
    const float invalid[] = {NAN,INFINITY,-INFINITY,1.001f,-1.001f};
    for (size_t vertex=0;vertex<3;++vertex)
        for (size_t axis=0;axis<4;++axis)
            for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
                p = begin(c,&b,64); fill(p); p[vertex][axis] = invalid[i];
                before = b.submissions; out = UINT64_MAX;
                CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
                CHECK(before == b.submissions && out == UINT64_MAX); blocked(c,&b);
            }
    p = begin(c,&b,64); fill(p); p[1][3] = 0.5f;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
    p = begin(c,&b,64); fill(p); p[1][3] = -1;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
    before = b.submissions; p = begin(c,&b,64);
    CHECK(isnan(p[0][0]) && isnan(p[63][3]));
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnBadArgument);
    CHECK(before == b.submissions); draw(c,&b);
    for (unsigned mode=0;mode<7;++mode) {
        p = begin(c,&b,64); fill(p); before = b.submissions;
        if (mode == 0) CHECK(gi_primitive_end(c,0,4,3,&out) == kIOReturnUnsupported);
        if (mode == 1) CHECK(gi_primitive_end(c,0x8002,5,3,&out) == kIOReturnUnsupported);
        if (mode == 2) CHECK(gi_primitive_end(c,0x8002,4,3,NULL) == kIOReturnBadArgument);
        if (mode == 3) CHECK(gi_primitive_end(c,0x8002,4,3,(uint64_t *)p) == kIOReturnBadArgument);
        if (mode == 4) {
            CHECK(gi_primitive_flush(c,&out) == kIOReturnExclusiveAccess);
            CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnExclusiveAccess);
        }
        if (mode == 5) {
            CHECK(gi_primitive_destroy(c) == kIOReturnExclusiveAccess);
            CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnExclusiveAccess);
        }
        if (mode == 6) {
            CHECK(gi_primitive_set_state(c,&good) == kIOReturnExclusiveAccess);
            CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnExclusiveAccess);
            OK(gi_primitive_set_state(c,&good));
        }
        CHECK(before == b.submissions); blocked(c,&b); draw(c,&b);
    }
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnNotReady);
    p = begin(c,&b,64); before = b.submissions; out = UINT64_MAX;
    OK(gi_primitive_end(c,0x8002,4,0,&out));
    CHECK(out == UINT64_MAX && before == b.submissions); blocked(c,&b); draw(c,&b);
    p = begin(c,&b,64); fill(p); b.submit_error = kIOReturnNoResources; out = UINT64_MAX;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnNoResources);
    CHECK(out == UINT64_MAX); blocked(c,&b);
    b.submit_error = 0; draw(c,&b);
    b.zero_generation = 1; p = begin(c,&b,64); fill(p); out = UINT64_MAX;
    CHECK(gi_primitive_end(c,0x8002,4,3,&out) == kIOReturnNotReady);
    CHECK(out == UINT64_MAX); blocked(c,&b); b.zero_generation = 0; draw(c,&b);
    b.flush_error = kIOReturnNoResources; out = UINT64_MAX;
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNoResources && out == UINT64_MAX);
    b.flush_error = 0; blocked(c,&b); draw(c,&b);
    b.stale = 1; out = UINT64_MAX;
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNotReady && out == UINT64_MAX);
    b.stale = 0; blocked(c,&b); draw(c,&b);
    gi_primitive_status s; OK(gi_primitive_get_status(c,&s));
    CHECK(s.last_failure != 0 && s.pending_failure == 0 && s.completed_generation == b.generation);
    /* 零 count 不提交，并保留此前确实成功的完成代次。 */
    p = begin(c,&b,64); before = b.submissions; out = UINT64_MAX;
    OK(gi_primitive_end(c,0x8002,4,0,&out));
    CHECK(before == b.submissions && out == UINT64_MAX);
    OK(gi_primitive_flush(c,&out)); CHECK(out == b.generation);
    union { uint64_t alignment; unsigned char bytes[80]; } alias;
    memset(&alias,0,sizeof(alias));
    CHECK(gi_primitive_begin(c,4,(int32_t *)alias.bytes,(float (**)[4])alias.bytes) == kIOReturnBadArgument);
    capacity = 3;
    CHECK(gi_primitive_begin(c,4,(int32_t *)(alias.bytes+1),&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin(c,4,&capacity,(float (**)[4])(alias.bytes+1)) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin(c,4,(int32_t *)(uintptr_t)(UINTPTR_MAX-1),&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_get_status(c,(gi_primitive_status *)(alias.bytes+1)) == kIOReturnBadArgument);
    CHECK(gi_primitive_create(&backend,(gi_primitive_handle *)&backend) == kIOReturnBadArgument);
    CHECK(gi_primitive_create(&backend,(gi_primitive_handle *)(alias.bytes+1)) == kIOReturnBadArgument);
    draw(c,&b);
    p = begin(c,&b,64); fill(p); before = b.submissions;
    CHECK(gi_primitive_end(c,0x8002,4,3,(uint64_t *)(alias.bytes+1)) == kIOReturnBadArgument);
    CHECK(before == b.submissions); blocked(c,&b); draw(c,&b);
    /* 两个实例分别持有引用；销毁一个不能破坏另一个的 staging。 */
    gi_primitive_handle second;
    OK(gi_primitive_create(&backend,&second)); CHECK(b.refs == 3);
    OK(gi_primitive_set_state(second,&good)); draw(second,&b);
    p = begin(c,&b,64); fill(p);
    capacity = 3;
    CHECK(gi_primitive_begin(second,4,(int32_t *)p,&b.staging) == kIOReturnBadArgument);
    CHECK(gi_primitive_get_status(second,(gi_primitive_status *)p) == kIOReturnBadArgument);
    OK(gi_primitive_end(c,0x8002,4,3,&out));
    --b.refs; /* 模拟调用者释放原始 owner 引用，模块引用仍然有效。 */
    OK(gi_primitive_destroy(second)); CHECK(b.refs == 1);
    CHECK(gi_primitive_destroy(second) == kIOReturnBadArgument);
    draw(c,&b);
    b.release_error = kIOReturnNotReady;
    CHECK(gi_primitive_destroy(c) == kIOReturnNotReady && b.refs == 1);
    OK(gi_primitive_get_status(c,&s)); CHECK(s.destroying);
    capacity = 3;
    CHECK(gi_primitive_begin(c,4,&capacity,&p) == kIOReturnNotReady);
    CHECK(gi_primitive_set_state(c,&good) == kIOReturnNotReady);
    CHECK(gi_primitive_flush(c,&out) == kIOReturnNotReady);
    b.release_error = 0; OK(gi_primitive_destroy(c)); CHECK(b.refs == 0 && live_allocations == 0);
    CHECK(gi_primitive_get_status(c,&s) == kIOReturnBadArgument);
    CHECK(gi_primitive_destroy(c) == kIOReturnBadArgument);
    CHECK(gi_primitive_begin(UINT64_MAX,4,&capacity,&p) == kIOReturnBadArgument);
    CHECK(gi_primitive_end(0,0x8002,4,3,&out) == kIOReturnBadArgument);
    printf("PASS primitive position-only CPU: %u checks, %u submissions, %u flushes; no GPU\n",checks,b.submissions,b.flushes);
    colored_tests();
    printf("PASS primitive CPU combined: %u checks; no GPU\n",checks);
    return 0;
}
