#include "../gld-texture/vgpu_texture.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <pthread.h>

/* 仅 CPU 结构 fixture，无系统 GLD 调用、无设备、无 GPU 提交。 */
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "失败: %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == VGT_OK)
static void put16(unsigned char *p, size_t off, uint16_t v) { memcpy(p + off, &v, 2); }
static void putptr(unsigned char *p, size_t off, const void *v) { memcpy(p + off, &v, 8); }
static vgt_view none;

typedef struct {
    unsigned refs, creates, destroys, live;
    int fail_retain, fail_create, empty, bad_format, bad_pitch, bad_size, bad_samples;
    void *alias;
} fixture;
static vgt_status lease_retain(void *p) {
    fixture *f = p;
    if (f->fail_retain) return VGT_NO_MEMORY;
    ++f->refs;
    return VGT_OK;
}
static void lease_release(void *p) {
    fixture *f = p;
    CHECK(f->refs > 0);
    --f->refs;
}
static vgt_status create_image(void *p, const vgt_image_desc *d, vgt_backing *out) {
    fixture *f = p;
    unsigned char *bytes;
    size_t row = (size_t)d->width * 4, n = row * d->height;
    ++f->creates;
    if (f->empty) return VGT_OK;
    if (f->alias) { out->resource = f->alias; return VGT_OK; }
    bytes = malloc(n);
    if (!bytes) return VGT_NO_MEMORY;
    ++f->live;
    if (d->cpu.data) {
        uint32_t y;
        for (y = 0; y < d->height; ++y)
            memcpy(bytes + row * y, (const unsigned char *)d->cpu.data + d->cpu_row_bytes * y, row);
    } else memset(bytes, 0xcd, n);
    *out = (vgt_backing){bytes, d->width, d->height,
        f->bad_format ? 99 : d->format, f->bad_samples ? 2 : d->samples,
        f->bad_pitch ? SIZE_MAX : row, f->bad_size ? n - 1 : n};
    return f->fail_create ? VGT_BACKEND_FAILED : VGT_OK;
}
static void destroy_image(void *p, vgt_backing *b) {
    fixture *f = p;
    CHECK(b->resource && f->live);
    free(b->resource);
    --f->live;
    ++f->destroys;
    memset(b, 0, sizeof(*b));
}
static vgt_backend backend(fixture *f) {
    vgt_backend b = {VGT_BACKEND_VERSION, 4096, 4096, 1, f,
        {f, lease_retain, lease_release}, create_image, destroy_image};
    return b;
}
static void rb(unsigned char *td, unsigned char *tu) {
    memset(td, 0, VGT_TD_RB_LENGTH);
    memset(tu, 0, VGT_TU_LENGTH);
    put16(td, 0, 0x2002); put16(td, 0x68, 0x8058);
    put16(td, 0x6a, 3); put16(td, 0x6c, 2); put16(td, 0x6e, 1);
    td[0x79] = td[0x7b] = 1;
    put16(td, 0xb4, 3); put16(td, 0xb6, 2); put16(td, 0xb8, 1);
    td[0xbb] = 0x20;
    put16(td, 0xbc, 3); put16(td, 0xbe, 2);
    put16(td, 0xc0, 0x1908); put16(td, 0xc2, 0x1401);
    put16(tu, 8, 1); tu[0x14] = 1;
}
static vgt_views views(unsigned char *td, size_t n, unsigned char *tu, size_t m) {
    vgt_views v = {{td, n}, {tu, m}, {0}};
    return v;
}
static void create_output_overlap(void) {
    union { vgt_handle slots[40]; unsigned char bytes[320]; } td, tu;
    unsigned char saved_td[320], saved_tu[320];
    fixture f = {0}; vgt_backend b = backend(&f); vgt_handle s, t;
    OK(vgt_share_create(&b, &s));
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned edge = 0; edge < 3; ++edge) {
            memset(&td, 0, sizeof(td)); memset(&tu, 0xa5, sizeof(tu));
            memcpy(saved_td, td.bytes, sizeof(td)); memcpy(saved_tu, tu.bytes, sizeof(tu));
            vgt_views v = views(td.bytes + 4, 0xd0, tu.bytes + 4, 0x15);
            v.lease = (vgt_lease){&f, lease_retain, lease_release};
            size_t slot = edge == 0 ? 0 : edge == 1 ? 2 : side ? 3 : 26;
            vgt_handle *out = side ? &tu.slots[slot] : &td.slots[slot];
            CHECK(vgt_texture_create(s, v, out) == VGT_INVALID);
            CHECK(!memcmp(saved_td, td.bytes, sizeof(td)) && !memcmp(saved_tu, tu.bytes, sizeof(tu)));
            CHECK(f.refs == 1 && !f.creates && !f.live);
        }
    }
    memset(&td, 0, sizeof(td)); memset(&tu, 0, sizeof(tu));
    OK(vgt_texture_create(s, views(td.bytes + 8, 0xd0, tu.bytes + 8, 0x18), &td.slots[0]));
    t = td.slots[0]; OK(vgt_release(t));
    OK(vgt_texture_create(s, views(td.bytes + 8, 0xd0, tu.bytes + 8, 0x18), &tu.slots[4]));
    t = tu.slots[4]; OK(vgt_release(t));
    CHECK(vgt_texture_create(s, views(td.bytes, 0xd0, tu.bytes, 0x15),
          (vgt_handle *)(uintptr_t)(UINTPTR_MAX - 7)) == VGT_INVALID);
    OK(vgt_release(s)); CHECK(!f.refs);
}
static void share_output_overlap(void) {
    union { vgt_backend b; vgt_handle slots[sizeof(vgt_backend) / sizeof(vgt_handle)]; } storage;
    unsigned char saved[sizeof(storage)]; fixture f = {0};
    storage.b = backend(&f); memcpy(saved, &storage, sizeof(storage));
    CHECK(vgt_share_create(&storage.b, &storage.slots[0]) == VGT_INVALID);
    CHECK(!memcmp(saved, &storage, sizeof(storage)) && !f.refs);
}
static void cpu_dirty_overlap(void) {
    unsigned char td[0xd0], storage[128], saved[128], old_pixels[24];
    unsigned char *tu = storage + 32;
    fixture f = {0}; vgt_backend b = backend(&f); vgt_handle s, t;
    vgt_texture_state before, after; vgt_image_desc d, marker;
    OK(vgt_share_create(&b, &s));
    memset(storage, 0x5a, sizeof(storage)); rb(td, tu);
    OK(vgt_texture_create(s, views(td, sizeof(td), tu, 0x15), &t));
    OK(vgt_texture_load(s, t, none)); OK(vgt_texture_state_get(t, &before));
    memcpy(old_pixels, before.backing.resource, sizeof(old_pixels));
    /* 覆盖 source 从写入区之前进入、首字节、末字节，以及超额容量相交。 */
    const size_t starts[] = {17, 40, 52, 16};
    const size_t lengths[] = {24, 24, 24, 25};
    for (size_t n = 0; n < sizeof(starts) / sizeof(starts[0]); ++n) {
        rb(td, tu); putptr(td, 0xc8, storage + starts[n]);
        memcpy(saved, storage, sizeof(storage)); memset(&marker, 0xa5, sizeof(marker)); d = marker;
        unsigned creates = f.creates, destroys = f.destroys;
        vgt_view cpu = {storage + starts[n], lengths[n]};
        CHECK(vgt_texture_decode_renderbuffer(s, t, cpu, &d) == VGT_INVALID);
        CHECK(!memcmp(&d, &marker, sizeof(d)));
        CHECK(vgt_texture_ensure_backing(s, t, cpu) == VGT_INVALID);
        CHECK(vgt_texture_load(s, t, cpu) == VGT_INVALID);
        CHECK(!memcmp(saved, storage, sizeof(storage)));
        CHECK(f.creates == creates && f.destroys == destroys && f.live == 1);
        OK(vgt_texture_state_get(t, &after));
        CHECK(after.generation == before.generation && after.backing.resource == before.backing.resource);
        CHECK(!memcmp(old_pixels, after.backing.resource, sizeof(old_pixels)));
    }
    /* 半开区间恰好相邻不误拒绝；允许源覆盖 TU 中不会写的前缀。 */
    const size_t adjacent[] = {16, 53};
    for (size_t n = 0; n < sizeof(adjacent) / sizeof(adjacent[0]); ++n) {
        rb(td, tu); putptr(td, 0xc8, storage + adjacent[n]);
        unsigned char source[24]; memcpy(source, storage + adjacent[n], sizeof(source));
        vgt_view cpu = {storage + adjacent[n], sizeof(source)};
        OK(vgt_texture_decode_renderbuffer(s, t, cpu, &d));
        OK(vgt_texture_load(s, t, cpu));
        CHECK(!memcmp(source, storage + adjacent[n], sizeof(source)));
        OK(vgt_texture_state_get(t, &after)); CHECK(!memcmp(source, after.backing.resource, sizeof(source)));
    }
    OK(vgt_release(t)); OK(vgt_release(s)); CHECK(!f.live && !f.refs);
}
static void defaults_and_bounds(void) {
    unsigned char td[0xd0], tu[0x18], before[0x18];
    vgt_handle s, t, old = 0;
    unsigned k;
    OK(vgt_share_create(NULL, &s));
    for (k = 0; k <= 10; ++k) {
        memset(td, 0, sizeof(td)); memset(tu, 0xa5, sizeof(tu));
        put16(td, 0, (uint16_t)(0x1000 | k)); tu[0x14] = 0x81;
        memcpy(before, tu, sizeof(tu));
        OK(vgt_texture_create(s, views(td, 0x80, tu, 0x15), &t));
        CHECK(t > old); old = t;
        CHECK(vgt_texture_load(s, t, none) == VGT_UNSUPPORTED);
        CHECK(!memcmp(tu, before, sizeof(tu)));
        OK(vgt_release(t)); CHECK(vgt_release(t) == VGT_BAD_HANDLE);
    }
    rb(td, tu);
    for (k = 0; k < 0x80; ++k) {
        t = 123;
        CHECK(vgt_texture_create(s, views(td, k, tu, 0x15), &t) == VGT_BOUNDS);
        CHECK(!t);
    }
    t = 123;
    memcpy(before, tu, sizeof(tu));
    CHECK(vgt_texture_create(s, views(td, sizeof(td), td + 1, 0x15), &t) == VGT_INVALID);
    CHECK(!t && !memcmp(before, tu, sizeof(tu)));
    CHECK(vgt_texture_create(s, views(td + 1, 0x80, td, 0x15), &t) == VGT_INVALID);
    CHECK(!t);
    CHECK(vgt_texture_create(s, views((void *)(uintptr_t)(UINTPTR_MAX - 7), 0x80, tu, 0x15), &t) == VGT_INVALID);
    CHECK(!t);
    for (k = 0; k < 0x15; ++k)
        CHECK(vgt_texture_create(s, views(td, sizeof(td), tu, k), &t) == VGT_BOUNDS);
    for (k = 0x80; k < 0xd0; ++k) {
        vgt_image_desc d;
        OK(vgt_texture_create(s, views(td, k, tu, 0x15), &t));
        CHECK(vgt_texture_decode_renderbuffer(s, t, none, &d) == VGT_BOUNDS);
        OK(vgt_release(t));
    }
    OK(vgt_texture_create(s, views(td, sizeof(td), tu, 0x15), &t));
    memcpy(before, tu, sizeof(tu));
    CHECK(vgt_texture_load(s, t, none) == VGT_NO_BACKEND);
    CHECK(!memcmp(tu, before, sizeof(tu)));
    tu[0x14] = 0; put16(tu, 8, 0);
    CHECK(vgt_texture_load(s, t, none) == VGT_NO_BACKEND);
    OK(vgt_release(t)); OK(vgt_release(s));
}
static void transactions(void) {
    unsigned char td[0xd0], tu[0x18], before[0x18], pixels[40];
    fixture f = {0}; vgt_backend b = backend(&f);
    vgt_handle s, other, t;
    vgt_texture_state a, z;
    unsigned n;
    rb(td, tu); memset(tu + 0x15, 0xa9, 3);
    OK(vgt_share_create(&b, &s)); OK(vgt_share_create(NULL, &other));
    OK(vgt_texture_create(s, views(td, sizeof(td), tu, 0x15), &t));
    CHECK(vgt_texture_load(other, t, none) == VGT_OWNER_MISMATCH);
    CHECK(vgt_texture_destroy(other, t) == VGT_OWNER_MISMATCH);
    CHECK(vgt_texture_modify(other, t, 1) == VGT_OWNER_MISMATCH);
    CHECK(vgt_texture_detach_views(other, t) == VGT_OWNER_MISMATCH);
    OK(vgt_texture_load(s, t, none));
    OK(vgt_texture_state_get(t, &a));
    CHECK(a.has_backing && a.generation == 1 && f.live == 1);
    CHECK(tu[0x14] == 0 && tu[8] == 0 && tu[0x15] == 0xa9 && tu[0x17] == 0xa9);
    OK(vgt_texture_ensure_backing(s, t, none)); CHECK(f.creates == 1);
    for (n = 0; n < 7; ++n) {
        OK(vgt_texture_modify(s, t, 1));
        put16(tu, 8, 1); memcpy(before, tu, sizeof(tu));
        f.fail_create = n == 0; f.empty = n == 1; f.bad_format = n == 2;
        f.bad_pitch = n == 3; f.bad_size = n == 4; f.bad_samples = n == 5;
        f.alias = n == 6 ? a.backing.resource : NULL;
        CHECK(vgt_texture_load(s, t, none) == VGT_BACKEND_FAILED);
        CHECK(!memcmp(tu, before, sizeof(tu)));
        OK(vgt_texture_state_get(t, &z));
        CHECK(z.generation == a.generation && z.backing.resource == a.backing.resource && f.live == 1);
    }
    f.fail_create = f.empty = f.bad_format = f.bad_pitch = f.bad_size = f.bad_samples = 0;
    f.alias = NULL;
    tu[0x14] = 0x80; memcpy(before, tu, sizeof(tu));
    CHECK(vgt_texture_load(s, t, none) == VGT_UNSUPPORTED);
    CHECK(vgt_texture_modify(s, t, 1) == VGT_UNSUPPORTED);
    CHECK(!memcmp(tu, before, sizeof(tu)));
    tu[0x14] = 1;
    CHECK(vgt_texture_modify(s, t, UINT32_C(0x100)) == VGT_UNSUPPORTED);
    put16(tu, 8, 2); CHECK(vgt_texture_load(s, t, none) == VGT_UNSUPPORTED);
    put16(tu, 8, 1); put16(tu, 10, 1);
    CHECK(vgt_texture_load(s, t, none) == VGT_UNSUPPORTED);
    memset(tu + 8, 0xff, 12);
    tu[0x14] = 2; CHECK(vgt_texture_load(s, t, none) == VGT_UNSUPPORTED);
    tu[0x14] = 1; OK(vgt_texture_load(s, t, none));
    CHECK(f.live == 1 && tu[0x14] == 0);
    /* 无 dirty 的几何变化也不能继续报告旧尺寸资源为当前资源。 */
    put16(td, 0x6a, 4); put16(td, 0xb4, 4); put16(td, 0xbc, 4);
    OK(vgt_texture_load(s, t, none)); OK(vgt_texture_state_get(t, &z));
    CHECK(z.generation == 3 && z.backing.width == 4);
    /* 有界 CPU 源、带 padding 行和非255 alpha，确认 fixture 没有强制 alpha。 */
    put16(td, 0xbc, 5); putptr(td, 0xc8, pixels);
    for (n = 0; n < sizeof(pixels); ++n) pixels[n] = (unsigned char)(n + 3);
    tu[0x14] = 4;
    CHECK(vgt_texture_load(s, t, none) == VGT_INVALID);
    CHECK(vgt_texture_load(s, t, (vgt_view){pixels, 39}) == VGT_BOUNDS);
    OK(vgt_texture_load(s, t, (vgt_view){pixels, sizeof(pixels)}));
    OK(vgt_texture_state_get(t, &z));
    CHECK(!memcmp(z.backing.resource, pixels, 16));
    CHECK(!memcmp((unsigned char *)z.backing.resource + 16, pixels + 20, 16));
    CHECK(((unsigned char *)z.backing.resource)[3] == 6);
    /* 模拟 FBO retain；释放 caller share/texture 后仍可访问。 */
    OK(vgt_retain(t)); OK(vgt_release(s)); OK(vgt_release(t));
    OK(vgt_texture_state_get(t, &z)); CHECK(z.references == 1);
    OK(vgt_texture_load(s, t, (vgt_view){pixels, sizeof(pixels)}));
    OK(vgt_release(t)); CHECK(!f.live && !f.refs);
    CHECK(vgt_retain(s) == VGT_BAD_HANDLE); OK(vgt_release(other));
}
static void invalid_descriptors(void) {
    unsigned char td[0xd0], tu[0x15], saved[0xd0];
    fixture f = {0}; vgt_backend b = backend(&f);
    vgt_handle s, t; vgt_image_desc d, marker;
    const size_t offsets[] = {0, 1, 0x68, 0x6a, 0x6c, 0x6e, 0x78, 0x79, 0x7a,
        0x7b, 0x7c, 0x7e, 0xb4, 0xb6, 0xb8, 0xba, 0xbb, 0xbc, 0xbe, 0xc0, 0xc2};
    size_t n;
    rb(td, tu); memcpy(saved, td, sizeof(td));
    memset(&marker, 0x5a, sizeof(marker));
    OK(vgt_share_create(&b, &s));
    OK(vgt_texture_create(s, views(td, sizeof(td), tu, sizeof(tu)), &t));
    for (n = 0; n < sizeof(offsets) / sizeof(offsets[0]); ++n) {
        memcpy(td, saved, sizeof(td)); td[offsets[n]] ^= 0x40;
        if (offsets[n] == 0xbc || offsets[n] == 0xbe) td[offsets[n]] = 0;
        d = marker;
        CHECK(vgt_texture_decode_renderbuffer(s, t, none, &d) != VGT_OK);
        CHECK(!memcmp(&d, &marker, sizeof(d)));
    }
    memcpy(td, saved, sizeof(td));
    put16(td, 0xb4, 0); CHECK(vgt_texture_load(s, t, none) == VGT_INVALID);
    memcpy(td, saved, sizeof(td)); putptr(tu, 0, td);
    CHECK(vgt_texture_load(s, t, none) == VGT_UNSUPPORTED);
    memset(tu, 0, 8); memcpy(td, saved, sizeof(td));
    put16(td, 0x6a, 4097); put16(td, 0xb4, 4097); put16(td, 0xbc, 4097);
    CHECK(vgt_texture_load(s, t, none) == VGT_BOUNDS); CHECK(!f.creates);
    OK(vgt_release(t)); OK(vgt_release(s));
    b.rgba8_alpha = 0; rb(td, tu);
    OK(vgt_share_create(&b, &s));
    OK(vgt_texture_create(s, views(td, sizeof(td), tu, sizeof(tu)), &t));
    CHECK(vgt_texture_load(s, t, none) == VGT_UNSUPPORTED); CHECK(!f.creates);
    OK(vgt_release(t)); OK(vgt_release(s));
}
typedef struct { fixture refs; unsigned char td[0xd0], tu[0x15]; } owner;
static void owner_release(void *p) {
    owner *o = p;
    CHECK(o->refs.refs);
    if (!--o->refs.refs) free(o);
}
static void leases_and_detach(void) {
    fixture f = {0}; vgt_backend b = backend(&f);
    vgt_handle s, t;
    owner *o = calloc(1, sizeof(*o)); vgt_views v;
    CHECK(o != NULL); rb(o->td, o->tu); o->refs.refs = 1;
    v = views(o->td, sizeof(o->td), o->tu, sizeof(o->tu));
    v.lease = (vgt_lease){o, lease_retain, owner_release};
    f.fail_retain = 1; s = 99;
    CHECK(vgt_share_create(&b, &s) == VGT_NO_MEMORY && !s && !f.refs);
    CHECK(vgt_share_create(&b, &s) == VGT_NO_MEMORY && !s && !f.refs);
    f.fail_retain = 0; OK(vgt_share_create(&b, &s));
    o->refs.fail_retain = 1;
    CHECK(vgt_texture_create(s, v, &t) == VGT_NO_MEMORY && !t && o->refs.refs == 1);
    o->refs.fail_retain = 0;
    OK(vgt_texture_create(s, v, &t)); CHECK(o->refs.refs == 2);
    owner_release(o); /* caller 提前释放 owner 引用，由 texture lease 保活 */
    OK(vgt_release(s)); OK(vgt_texture_load(s, t, none));
    OK(vgt_texture_detach_views(s, t)); /* 最后 owner 引用在此释放 */
    CHECK(vgt_texture_load(s, t, none) == VGT_DETACHED);
    CHECK(vgt_texture_modify(s, t, 1) == VGT_DETACHED);
    OK(vgt_release(t)); CHECK(!f.refs && !f.live);
}
static void guard_pages(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size_t page = info.dwPageSize;
    unsigned char *map = VirtualAlloc(NULL, page * 6, MEM_RESERVE, PAGE_NOACCESS);
#else
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *map = mmap(NULL, page * 6, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    unsigned char *td, *tu;
    fixture f = {0}; vgt_backend b = backend(&f);
    vgt_handle s, t;
#ifdef _WIN32
    CHECK(map != NULL);
    CHECK(VirtualAlloc(map + page, page, MEM_COMMIT, PAGE_READWRITE) != NULL);
    CHECK(VirtualAlloc(map + page * 4, page, MEM_COMMIT, PAGE_READWRITE) != NULL);
#else
    CHECK(map != MAP_FAILED);
    CHECK(!mprotect(map + page, page, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(map + page * 4, page, PROT_READ | PROT_WRITE));
#endif
    td = map + page * 2 - 0xd0; tu = map + page * 5 - 0x15;
    rb(td, tu);
    OK(vgt_share_create(&b, &s));
    OK(vgt_texture_create(s, views(td, 0xd0, tu, 0x15), &t));
    OK(vgt_texture_load(s, t, none));
    OK(vgt_texture_detach_views(s, t));
#ifdef _WIN32
    CHECK(VirtualFree(map, 0, MEM_RELEASE));
#else
    CHECK(!munmap(map, page * 6));
#endif
    CHECK(vgt_texture_load(s, t, none) == VGT_DETACHED);
    OK(vgt_release(t)); OK(vgt_release(s)); CHECK(!f.live);
}
static void *retain_worker(void *p) {
    vgt_handle id = *(vgt_handle *)p;
    unsigned n;
    for (n = 0; n < 2000; ++n) {
        /* CHECK 全局计数不在线程中使用。 */
        if (vgt_retain(id) != VGT_OK || vgt_release(id) != VGT_OK) abort();
    }
    return NULL;
}
static void threaded_refs(void) {
    pthread_t threads[4]; vgt_handle s;
    unsigned n;
    OK(vgt_share_create(NULL, &s));
    for (n = 0; n < 4; ++n) CHECK(!pthread_create(&threads[n], NULL, retain_worker, &s));
    for (n = 0; n < 4; ++n) CHECK(!pthread_join(threads[n], NULL));
    OK(vgt_release(s)); CHECK(vgt_release(s) == VGT_BAD_HANDLE);
}
int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "create-output")) create_output_overlap();
        else if (!strcmp(argv[1], "share-output")) share_output_overlap();
        else if (!strcmp(argv[1], "cpu-dirty-overlap")) cpu_dirty_overlap();
        else return 2;
        printf("PASS: %s %u 检查\n", argv[1], checks);
        return 0;
    }
    create_output_overlap(); share_output_overlap(); cpu_dirty_overlap();
    defaults_and_bounds(); transactions(); invalid_descriptors();
    leases_and_detach(); guard_pages(); threaded_refs();
    printf("PASS: %u 检查；CPU backend fixture，非系统 GLD、非 GPU 验证。\n", checks);
    return 0;
}
