#include "../gld-objects/vgpu_objects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "失败 %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == VGO_OK)
extern unsigned vgo_fixture_surfaces;
extern int vgo_fixture_fail_open;
static vgo_views views(void *d, size_t dn, void *s, size_t sn) {
    return (vgo_views){{d, dn}, {s, sn}, {0}};
}
static void put32(void *p, uint32_t x) { memcpy(p, &x, 4); }
static void pd(unsigned char *p, uint16_t tag, unsigned char kind) {
    memset(p, 0, 0x8c); memcpy(p, &tag, 2); p[2] = kind;
    memset(p + 0x18, 0xff, 0x40);
}
typedef struct {
    unsigned refs, calls;
    int fail;
    uint8_t dirty;
    unsigned char snapshot[0x30];
} fixture;
static vgo_status retain(void *p) {
    fixture *f = p;
    if (f->fail) return VGO_NO_MEMORY;
    ++f->refs; return VGO_OK;
}
static void release(void *p) {
    fixture *f = p;
    CHECK(f->refs > 0); --f->refs;
}
static vgo_status consume(void *p, vgo_view d, uint8_t dirty) {
    fixture *f = p;
    ++f->calls; f->dirty = dirty;
    CHECK(d.length == 0x30);
    if (f->fail) return VGO_CONSUMER_FAILED;
    memcpy(f->snapshot, d.data, sizeof(f->snapshot));
    return VGO_OK;
}
static vgo_status consume_other(void *p, vgo_view d, uint8_t dirty) {
    return consume(p, d, dirty);
}
static void sampler_targets(void) {
    unsigned char sd[0x30] = {0x5a}, su = 1;
    fixture a = {0}, b = {0}; vgo_handle s, t; vgo_state state;
    OK(vgo_share_create(&s));
    OK(vgo_sampler_create(s, views(sd, sizeof(sd), &su, 1), &t));
    OK(vgo_sampler_consume(s, t, consume, &a));
    OK(vgo_sampler_consume(s, t, consume, &a)); CHECK(a.calls == 1);
    OK(vgo_sampler_consume(s, t, NULL, &b)); CHECK(!b.calls);
    b.fail = 1;
    CHECK(vgo_sampler_consume(s, t, consume, &b) == VGO_CONSUMER_FAILED);
    CHECK(b.calls == 1 && b.dirty == 0 && !su);
    OK(vgo_state_get(t, &state)); CHECK(state.generation == 1 && state.consumed);
    OK(vgo_sampler_consume(s, t, consume, &a)); CHECK(a.calls == 1);
    b.fail = 0;
    OK(vgo_sampler_consume(s, t, consume, &b));
    CHECK(b.calls == 2 && !memcmp(b.snapshot, sd, sizeof(sd)));
    OK(vgo_sampler_consume(s, t, consume, &b)); CHECK(b.calls == 2);
    b.fail = 1;
    CHECK(vgo_sampler_consume(s, t, consume_other, &b) == VGO_CONSUMER_FAILED);
    OK(vgo_state_get(t, &state)); CHECK(state.generation == 2 && !su);
    OK(vgo_sampler_consume(s, t, consume, &b)); CHECK(b.calls == 3);
    b.fail = 0;
    OK(vgo_sampler_consume(s, t, consume_other, &b)); CHECK(b.calls == 4);
    OK(vgo_sampler_consume(s, t, consume_other, &b)); CHECK(b.calls == 4);
    OK(vgo_sampler_consume(s, t, consume, &a)); CHECK(a.calls == 2);
    OK(vgo_state_get(t, &state)); CHECK(state.generation == 4);
    su = 0x80; b.fail = 1;
    CHECK(vgo_sampler_consume(s, t, consume, &b) == VGO_CONSUMER_FAILED);
    CHECK(su == 0x80 && b.dirty == 0x80);
    OK(vgo_state_get(t, &state)); CHECK(state.generation == 4);
    CHECK(vgo_sampler_consume(s, t, NULL, NULL) == VGO_NO_CONSUMER);
    OK(vgo_sampler_consume(s, t, consume, &a)); CHECK(a.calls == 3 && !su);
    OK(vgo_state_get(t, &state)); CHECK(state.generation == 5);
    OK(vgo_release(t)); OK(vgo_release(s));
}
static void create_output_overlap(void) {
    union { vgo_handle slots[20]; unsigned char bytes[160]; } d, u;
    unsigned char saved_d[160], saved_u[160];
    vgo_status (*const creators[])(vgo_handle, vgo_views, vgo_handle *) = {
        vgo_sampler_create, vgo_pipeline_create, vgo_program_create};
    vgo_handle s, t; fixture f = {0}; vgo_state state;
    OK(vgo_share_create(&s));
    for (size_t k = 0; k < sizeof(creators) / sizeof(creators[0]); ++k) {
        for (unsigned side = 0; side < 2; ++side) {
            for (unsigned edge = 0; edge < 3; ++edge) {
                memset(&d, 0, sizeof(d)); memset(&u, 0xa5, sizeof(u));
                memcpy(saved_d, d.bytes, sizeof(d)); memcpy(saved_u, u.bytes, sizeof(u));
                vgo_views v = views(d.bytes + 4, 0x31, u.bytes + 4, 0x31);
                v.lease = (vgo_lease){&f, retain, release};
                size_t slot = edge == 0 ? 0 : edge == 1 ? 2 : 6;
                vgo_handle *out = side ? &u.slots[slot] : &d.slots[slot];
                CHECK(creators[k](s, v, out) == VGO_INVALID);
                CHECK(!memcmp(saved_d, d.bytes, sizeof(d)) && !memcmp(saved_u, u.bytes, sizeof(u)));
                CHECK(!f.refs); OK(vgo_state_get(s, &state)); CHECK(state.references == 1);
            }
        }
        /* 输出紧邻视图边界允许；句柄本身有真实可写存储。 */
        memset(&d, 0, sizeof(d)); memset(&u, 0, sizeof(u));
        OK(creators[k](s, views(d.bytes + 8, 0x30, u.bytes + 8, 0x30), &d.slots[0]));
        t = d.slots[0]; OK(vgo_release(t));
        OK(creators[k](s, views(d.bytes + 8, 0x30, u.bytes + 8, 0x30), &u.slots[7]));
        t = u.slots[7]; OK(vgo_release(t));
        CHECK(creators[k](s, views(d.bytes, 0x30, u.bytes, 8),
              (vgo_handle *)(uintptr_t)(UINTPTR_MAX - 7)) == VGO_INVALID);
    }
    OK(vgo_release(s));
}
static void context_output_overlap(void) {
    union { vgo_handle slots[2600]; unsigned char bytes[20800]; } h, b;
    unsigned char saved_h[20800], saved_b[20800];
    vgo_handle s, t; vgo_state state;
    OK(vgo_share_create(&s));
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned edge = 0; edge < 3; ++edge) {
            memset(&h, 0x5a, sizeof(h)); memset(&b, 0xa5, sizeof(b));
            memcpy(saved_h, h.bytes, sizeof(h)); memcpy(saved_b, b.bytes, sizeof(b));
            size_t length = side ? VGC_B_LENGTH : VGC_H_LENGTH;
            size_t slot = edge == 0 ? 0 : edge == 1 ? 2 : (length + 3) / 8;
            vgo_handle *out = side ? &b.slots[slot] : &h.slots[slot];
            CHECK(vgo_context_create(s, (vgc_view){h.bytes + 4, VGC_H_LENGTH},
                  (vgc_view){b.bytes + 4, VGC_B_LENGTH}, out) == VGO_INVALID);
            CHECK(!memcmp(saved_h, h.bytes, sizeof(h)) && !memcmp(saved_b, b.bytes, sizeof(b)));
            OK(vgo_state_get(s, &state)); CHECK(state.references == 1);
        }
    }
    OK(vgo_context_create(s, (vgc_view){h.bytes + 8, VGC_H_LENGTH},
       (vgc_view){b.bytes + 8, VGC_B_LENGTH}, &h.slots[0]));
    t = h.slots[0]; OK(vgo_release(t)); OK(vgo_release(s));
}
static void sampler_transactions(void) {
    unsigned char sd[0x30], su[4] = {1, 0xa1, 0xb2, 0xc3}, before[4];
    memset(sd, 0x5a, sizeof(sd));
    vgo_handle s, other, t;
    fixture f = {0}; vgo_state state;
    OK(vgo_share_create(&s)); OK(vgo_share_create(&other));
    vgo_views v = views(sd, sizeof(sd), su, 1);
    v.lease = (vgo_lease){&f, retain, release};
    f.fail = 1; t = 123;
    CHECK(vgo_sampler_create(s, v, &t) == VGO_NO_MEMORY && !t && !f.refs);
    f.fail = 0; OK(vgo_sampler_create(s, v, &t));
    CHECK(f.refs == 1 && su[0] == 1);
    OK(vgo_state_get(t, &state));
    CHECK(state.kind == VGO_SAMPLER && state.descriptor.data == sd && state.state.data == su);
    CHECK(state.descriptor.length == sizeof(sd) && state.state.length == 1 && !state.consumed);
    memcpy(before, su, 4);
    CHECK(vgo_sampler_consume(other, t, consume, &f) == VGO_OWNER_MISMATCH);
    CHECK(vgo_sampler_consume(s, t, NULL, NULL) == VGO_NO_CONSUMER);
    CHECK(!memcmp(before, su, 4));
    f.fail = 1;
    CHECK(vgo_sampler_consume(s, t, consume, &f) == VGO_CONSUMER_FAILED);
    CHECK(!memcmp(before, su, 4));
    OK(vgo_state_get(t, &state)); CHECK(!state.generation && !state.consumed);
    f.fail = 0; OK(vgo_sampler_consume(s, t, consume, &f));
    CHECK(su[0] == 0 && !memcmp(su + 1, before + 1, 3));
    CHECK(f.calls == 2 && f.dirty == 1 && !memcmp(sd, f.snapshot, sizeof(sd)));
    OK(vgo_sampler_consume(s, t, NULL, NULL)); CHECK(f.calls == 2);
    sd[0] = 0x19; su[0] = 0x80;
    OK(vgo_sampler_consume(s, t, consume, &f));
    CHECK(f.snapshot[0] == 0x19 && f.dirty == 0x80);
    OK(vgo_state_get(t, &state)); CHECK(state.generation == 2 && state.consumed);
    CHECK(vgo_sampler_destroy(other, t) == VGO_OWNER_MISMATCH);
    CHECK(vgo_pipeline_destroy(s, t) == VGO_BAD_HANDLE);
    CHECK(vgo_program_destroy(s, t) == VGO_BAD_HANDLE);
    CHECK(vgo_detach_views(other, t) == VGO_OWNER_MISMATCH);
    OK(vgo_release(s));
    OK(vgo_detach_views(s, t)); CHECK(!f.refs);
    CHECK(vgo_detach_views(s, t) == VGO_DETACHED);
    CHECK(vgo_sampler_consume(s, t, consume, &f) == VGO_DETACHED);
    OK(vgo_state_get(t, &state)); CHECK(!state.descriptor.data && !state.state.data && !state.attached);
    OK(vgo_sampler_destroy(s, t)); CHECK(vgo_release(t) == VGO_BAD_HANDLE);
    CHECK(vgo_release(s) == VGO_BAD_HANDLE); OK(vgo_release(other));
}
static void bounds_and_defaults(void) {
    unsigned char d[0x90], u[8]; vgo_handle s, t, previous = 0;
    OK(vgo_share_create(&s)); memset(d, 0, sizeof(d)); memset(u, 0xa5, sizeof(u));
    for (size_t n = 0; n < 0x30; ++n) {
        t = 123; CHECK(vgo_sampler_create(s, views(d, n, u, 1), &t) == VGO_BOUNDS); CHECK(!t);
    }
    t = 123; CHECK(vgo_sampler_create(s, views(d, sizeof(d), u, 0), &t) == VGO_BOUNDS && !t);
    CHECK(vgo_sampler_create(s, views(d, sizeof(d), d + 4, 1), &t) == VGO_INVALID);
    CHECK(vgo_program_create(s, views(d, 1, d, 1), &t) == VGO_INVALID);
    CHECK(vgo_program_create(s, views(NULL, 0, u, 1), &t) == VGO_INVALID);
    CHECK(vgo_program_create(s, views(d, 0, u, 1), &t) == VGO_BOUNDS);
    CHECK(vgo_program_create(s, views(d, 1, u, 0), &t) == VGO_BOUNDS);
    CHECK(vgo_program_create(s, views((void *)(uintptr_t)(UINTPTR_MAX - 3), 8, u, 1), &t) == VGO_INVALID);
    vgo_views bad = views(d, 0x30, u, 1); bad.lease.retain = retain;
    CHECK(vgo_sampler_create(s, bad, &t) == VGO_INVALID);
    for (unsigned i = 0; i < 11; ++i) {
        u[0] = 1; OK(vgo_sampler_create(s, views(d, 0x30, u, 1), &t));
        CHECK(t > previous && (t & 255) == VGO_SAMPLER); previous = t;
        CHECK(u[0] == 1); OK(vgo_sampler_destroy(s, t));
    }
    for (unsigned i = 0; i < 4; ++i) {
        pd(d, i == 2 ? 0x8804 : i == 3 ? 0x8620 : 0, (i == 1 || i == 2) ? 4 : 0);
        put32(u, 3);
        size_t dn = i < 2 ? 0x88 : 0x8c;
        OK(vgo_pipeline_create(s, views(d, dn, u, 4), &t));
        vgo_state state; OK(vgo_state_get(t, &state));
        CHECK(state.kind == VGO_PIPELINE && state.descriptor.length == dn && state.state.length == 4);
        CHECK(u[0] == 3 && !state.consumed && !state.generation);
        OK(vgo_pipeline_destroy(s, t));
    }
    pd(d, 0, 0);
    for (size_t n = 0; n < 4; ++n) {
        CHECK(vgo_pipeline_create(s, views(d, n, u, 4), &t) == VGO_BOUNDS);
        CHECK(vgo_pipeline_create(s, views(d, 4, u, n), &t) == VGO_BOUNDS);
    }
    d[2] = 3; CHECK(vgo_pipeline_create(s, views(d, 4, u, 4), &t) == VGO_UNSUPPORTED);
    /* program 的最小元数据借用不读取未知结构字段。 */
    OK(vgo_program_create(s, views(d, 1, u, 1), &t));
    CHECK(vgo_sampler_consume(s, t, NULL, NULL) == VGO_BAD_HANDLE);
    CHECK(vgo_pipeline_modify(s, t, 1) == VGO_BAD_HANDLE);
    vgo_state state, marker; memset(&marker, 0x5a, sizeof(marker)); state = marker;
    CHECK(vgo_state_get(UINT64_MAX, &state) == VGO_BAD_HANDLE && !memcmp(&state, &marker, sizeof(state)));
    OK(vgo_state_get(t, &state)); CHECK(state.kind == VGO_PROGRAM && state.descriptor.data == d);
    OK(vgo_program_destroy(s, t)); OK(vgo_release(s));
}
static void pipeline_lifetime(void) {
    unsigned char d[0x8c], u[8], saved[8];
    unsigned char h[VGC_H_LENGTH] = {0}, b[VGC_B_LENGTH] = {0};
    vgo_handle s, other, c1, c2, foreign, p, q, pg;
    vgo_state state;
    pd(d, 0, 4); memset(u, 0xa6, sizeof(u)); put32(u, 0x80000003); memcpy(saved, u, 8);
    OK(vgo_share_create(&s)); OK(vgo_share_create(&other));
    CHECK(vgo_context_create(s, (vgc_view){h, 1}, (vgc_view){b, sizeof(b)}, &c1) == VGO_CONTEXT_FAILED && !c1);
    OK(vgo_context_create(s, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &c1));
    OK(vgo_context_create(s, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &c2));
    OK(vgo_context_create(other, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &foreign));
    CHECK(vgo_pipeline_create(c1, views(d, sizeof(d), u, 4), &p) == VGO_BAD_HANDLE);
    OK(vgo_pipeline_create(s, views(d, sizeof(d), u, 4), &p));
    OK(vgo_pipeline_create(s, views(d, sizeof(d), u, 4), &q));
    OK(vgo_program_create(s, views(d, 1, u, 1), &pg));
    CHECK(vgo_pipeline_bind(s, p) == VGO_BAD_HANDLE);
    CHECK(vgo_pipeline_bind(c1, pg) == VGO_BAD_HANDLE);
    CHECK(vgo_pipeline_bind(foreign, p) == VGO_OWNER_MISMATCH);
    CHECK(vgo_pipeline_unbind(s, p) == VGO_BAD_HANDLE);
    CHECK(vgo_pipeline_destroy(c1, p) == VGO_BAD_HANDLE);
    CHECK(vgo_pipeline_modify(other, p, 1) == VGO_OWNER_MISMATCH);
    CHECK(vgo_pipeline_modify(s, p, 2) == VGO_UNSUPPORTED);
    OK(vgo_pipeline_modify(s, p, 0));
    OK(vgo_pipeline_modify(s, p, 5));
    CHECK(!memcmp(u, saved, 8));
    OK(vgo_state_get(p, &state)); CHECK(state.generation == 1 && !state.consumed);
    OK(vgo_pipeline_bind(c1, p)); OK(vgo_pipeline_bind(c1, p)); OK(vgo_pipeline_bind(c2, p));
    OK(vgo_state_get(p, &state)); CHECK(state.references == 3);
    OK(vgo_pipeline_unbind(c1, q));
    OK(vgo_state_get(c1, &state)); CHECK(state.pipeline == p);
    CHECK(vgo_pipeline_unbind(foreign, p) == VGO_OWNER_MISMATCH);
    OK(vgo_pipeline_bind(c1, q));
    OK(vgo_state_get(p, &state)); CHECK(state.references == 2);
    OK(vgo_pipeline_destroy(s, p));
    OK(vgo_state_get(p, &state)); CHECK(state.references == 1);
    OK(vgo_pipeline_unbind(c2, p)); CHECK(vgo_state_get(p, &state) == VGO_BAD_HANDLE);
    OK(vgo_pipeline_destroy(s, q));
    OK(vgo_detach_views(s, q));
    CHECK(vgo_pipeline_modify(s, q, 1) == VGO_DETACHED);
    CHECK(vgo_pipeline_bind(c2, q) == VGO_DETACHED);
    OK(vgo_release(s)); OK(vgo_program_destroy(s, pg));
    OK(vgo_release(c1)); CHECK(vgo_state_get(q, &state) == VGO_BAD_HANDLE);
    OK(vgo_release(c2)); CHECK(vgo_release(s) == VGO_BAD_HANDLE);
    OK(vgo_release(other)); OK(vgo_release(foreign));
    CHECK(!vgo_fixture_surfaces && !memcmp(u, saved, 8));
}
static void guarded_views(void) {
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si); size_t page = si.dwPageSize;
    unsigned char *m = VirtualAlloc(NULL, page * 4, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(m != NULL);
    CHECK(VirtualAlloc(m, page, MEM_COMMIT, PAGE_READWRITE) != NULL);
    CHECK(VirtualAlloc(m + page * 2, page, MEM_COMMIT, PAGE_READWRITE) != NULL);
#else
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *m = mmap(NULL, page * 4, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(m != MAP_FAILED);
    CHECK(!mprotect(m, page, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(m + page * 2, page, PROT_READ | PROT_WRITE));
#endif
    unsigned char *d = m + page - 0x30, *u = m + page * 3 - 1;
    memset(d, 0, 0x30); *u = 0;
    vgo_handle s, t; fixture f = {0};
    OK(vgo_share_create(&s)); OK(vgo_sampler_create(s, views(d, 0x30, u, 1), &t));
    CHECK(vgo_sampler_consume(s, t, NULL, NULL) == VGO_NO_CONSUMER);
    OK(vgo_sampler_consume(s, t, consume, &f)); CHECK(f.calls == 1 && f.dirty == 0);
    OK(vgo_detach_views(s, t));
    vgo_handle pipeline, program;
    unsigned char *pd4 = m + page - 4, *pu4 = m + page * 3 - 4;
    memset(pd4, 0, 4); put32(pu4, 3);
    OK(vgo_pipeline_create(s, views(pd4, 4, pu4, 4), &pipeline));
    OK(vgo_pipeline_modify(s, pipeline, 5)); CHECK(pu4[0] == 3);
    OK(vgo_detach_views(s, pipeline));
    OK(vgo_program_create(s, views(m + page - 1, 1, u, 1), &program));
    OK(vgo_detach_views(s, program));
#ifdef _WIN32
    CHECK(VirtualFree(m, 0, MEM_RELEASE));
#else
    CHECK(!munmap(m, page * 4));
#endif
    CHECK(vgo_sampler_consume(s, t, consume, &f) == VGO_DETACHED);
    CHECK(vgo_pipeline_modify(s, pipeline, 1) == VGO_DETACHED);
    OK(vgo_pipeline_destroy(s, pipeline)); OK(vgo_program_destroy(s, program));
    OK(vgo_sampler_destroy(s, t)); OK(vgo_release(s));
}
static void *worker(void *p) {
    vgo_handle t = *(vgo_handle *)p;
    for (unsigned i = 0; i < 2000; ++i)
        if (vgo_retain(t) != VGO_OK || vgo_release(t) != VGO_OK) abort();
    return NULL;
}
static void threaded_refs(void) {
    vgo_handle s, t; unsigned char d = 1, u = 2; pthread_t threads[4];
    OK(vgo_share_create(&s)); OK(vgo_program_create(s, views(&d, 1, &u, 1), &t));
    for (unsigned i = 0; i < 4; ++i) CHECK(!pthread_create(&threads[i], NULL, worker, &t));
    for (unsigned i = 0; i < 4; ++i) CHECK(!pthread_join(threads[i], NULL));
    vgo_state state; OK(vgo_state_get(t, &state)); CHECK(state.references == 1);
    OK(vgo_program_destroy(s, t)); OK(vgo_release(s));
}
int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "sampler-targets")) sampler_targets();
        else if (!strcmp(argv[1], "create-output")) create_output_overlap();
        else if (!strcmp(argv[1], "context-output")) context_output_overlap();
        else return 2;
        printf("PASS: %s %u 检查\n", argv[1], checks);
        return 0;
    }
    sampler_targets(); create_output_overlap(); context_output_overlap();
    vgo_handle s = 123;
    vgo_fixture_fail_open = 1;
    CHECK(vgo_share_create(&s) == VGO_CONTEXT_FAILED && !s && !vgo_fixture_surfaces);
    vgo_fixture_fail_open = 0;
    sampler_transactions(); bounds_and_defaults(); pipeline_lifetime(); guarded_views(); threaded_refs();
    CHECK(!vgo_fixture_surfaces);
    printf("PASS: %u 检查；实际 objects/context 源模块，CPU surface fixture，非系统 GLD/GPU 验证。\n", checks);
    return 0;
}
