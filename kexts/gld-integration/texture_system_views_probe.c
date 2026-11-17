#include "context_exports.h"
#include "../gld-device/gld_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* 仅 CPU fixture；不链接正式 surface，不调用系统 GLD 或 GPU。 */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned device_token;
extern int vgo_fixture_bgra;
extern unsigned vgo_fixture_owners, vgo_fixture_images;
int32_t vgpuGLDDeviceGetStatus(void *device, struct gld_device_status *out) {
    if (device != &device_token || !out) return -1;
    *out = (struct gld_device_status){.registry_id = 73, .connected = 1,
        .liveness_status = kIOReturnNotReady};
    return 0;
}
static void put16(void *p, size_t off, uint16_t value) {
    memcpy((unsigned char *)p + off, &value, sizeof(value));
}
typedef struct { unsigned char *base, *data; size_t page, length; } guarded;
static guarded guard_create(size_t length) {
    guarded g = {0};
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info); g.page = info.dwPageSize;
    g.base = VirtualAlloc(NULL, 2 * g.page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(g.base);
    DWORD old;
    CHECK(VirtualProtect(g.base + g.page, g.page, PAGE_NOACCESS, &old));
#else
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0); g.page = (size_t)page;
    g.base = mmap(NULL, 2 * g.page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(g.base != MAP_FAILED);
    CHECK(mprotect(g.base + g.page, g.page, PROT_NONE) == 0);
#endif
    CHECK(length + 32 < g.page);
    memset(g.base, 0xa5, g.page);
    g.length = length; g.data = g.base + g.page - length;
    memset(g.data, 0, length);
    return g;
}
static void guard_destroy(guarded g) {
    for (size_t i = 0; i < g.page - g.length; ++i) CHECK(g.base[i] == 0xa5);
#ifdef _WIN32
    CHECK(VirtualFree(g.base, 0, MEM_RELEASE));
#else
    CHECK(munmap(g.base, 2 * g.page) == 0);
#endif
}
static void geometry(unsigned char *td) {
    put16(td, 0x68, 0x8058);
    put16(td, 0x6a, 256); put16(td, 0x6c, 256); put16(td, 0x6e, 1);
    td[0x79] = 1; td[0x7b] = 1;
    put16(td, 0xb4, 256); put16(td, 0xb6, 256); put16(td, 0xb8, 1);
    td[0xbb] = 0x20;
    put16(td, 0xbc, 256); put16(td, 0xbe, 256);
    put16(td, 0xc0, 0x1908); put16(td, 0xc2, 0x1401);
}
static void metadata_cases(void *share, vgt_handle ts) {
    /* 包括默认 kind2、裸 kind2、混合/未知高位，均不得从 0x80 偷升到 0xd0。 */
    const uint16_t extra[] = {2, 0x3002, 0x2012, 0x2003};
    for (unsigned i = 0; i < 15; ++i) {
        guarded td = guard_create(VGT_TD_METADATA_LENGTH), tu = guard_create(VGT_TU_LENGTH);
        uint16_t flags = i < 11 ? (uint16_t)(0x1000 | i) : extra[i - 11];
        put16(td.data, 0, flags);
        unsigned char before[VGT_TD_METADATA_LENGTH]; memcpy(before, td.data, sizeof(before));
        void *texture = NULL;
        vgt_handle t = 0; vgt_texture_state state;
        CHECK(gi_create_texture(share, &texture, td.data, tu.data) == GI_CONTEXT_OK);
        CHECK(gi_texture_native(share, texture, &t) == GI_CONTEXT_OK);
        CHECK(vgt_texture_state_get(t, &state) == VGT_OK && state.kind == (flags & 15));
        CHECK(vgt_texture_ensure_backing(ts, t, (vgt_view){0}) == VGT_BOUNDS);
        CHECK(!memcmp(before, td.data, sizeof(before)) && tu.data[0x14] == 0);
        /* 已持有引用时修改 flags 也不扩读、不替换 handle、不分配 backing。 */
        CHECK(vgt_retain(t) == VGT_OK);
        put16(td.data, 0, 0x2002);
        CHECK(gi_modify_texture(share, texture, 1) == GI_CONTEXT_OK);
        vgt_handle same = 0;
        CHECK(gi_texture_native(share, texture, &same) == GI_CONTEXT_OK && same == t);
        unsigned char dirty[VGT_TU_LENGTH]; memcpy(dirty, tu.data, sizeof(dirty));
        CHECK(vgt_texture_ensure_backing(ts, t, (vgt_view){0}) == VGT_BOUNDS);
        CHECK(!memcmp(dirty, tu.data, sizeof(dirty)) && vgo_fixture_images == 0);
        CHECK(vgt_texture_state_get(t, &state) == VGT_OK && state.references == 2 && !state.has_backing);
        CHECK(gi_destroy_texture(share, texture) == GI_CONTEXT_OK);
        CHECK(vgt_texture_state_get(t, &state) == VGT_OK && state.references == 1);
        CHECK(vgt_release(t) == VGT_OK);
        guard_destroy(td); guard_destroy(tu);
    }
    /* 未知 producer 的 kind2 按显式容量处理，包括不足 metadata 与少一字节的 RB。 */
    const size_t lengths[] = {VGT_TD_METADATA_LENGTH - 1, VGT_TD_METADATA_LENGTH,
        VGT_TD_RB_LENGTH - 1};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        guarded td = guard_create(lengths[i]), tu = guard_create(VGT_TU_LENGTH);
        put16(td.data, 0, 0x2002);
        void *texture = &device_token; vgt_handle t;
        int32_t result = gi_create_texture_views(share, &texture,
            (vgt_views){{td.data, td.length}, {tu.data, tu.length}, {0}});
        if (td.length < VGT_TD_METADATA_LENGTH) {
            CHECK(result == GI_CONTEXT_BACKEND && !texture);
        } else {
            CHECK(result == GI_CONTEXT_OK);
            CHECK(gi_texture_native(share, texture, &t) == GI_CONTEXT_OK);
            CHECK(gi_modify_texture(share, texture, 1) == GI_CONTEXT_OK);
            unsigned char dirty[VGT_TU_LENGTH]; memcpy(dirty, tu.data, sizeof(dirty));
            CHECK(vgt_texture_ensure_backing(ts, t, (vgt_view){0}) == VGT_BOUNDS);
            CHECK(!memcmp(dirty, tu.data, sizeof(dirty)) && vgo_fixture_images == 0);
            CHECK(gi_destroy_texture(share, texture) == GI_CONTEXT_OK);
        }
        guard_destroy(td); guard_destroy(tu);
    }
}
static void renderbuffer_case(void *share, vgc_handle ns, vgt_handle ts) {
    guarded td = guard_create(VGT_TD_RB_LENGTH), tu = guard_create(VGT_TU_LENGTH);
    put16(td.data, 0, 0x2002);
    void *texture = NULL; vgt_handle t, same;
    CHECK(gi_create_texture(share, &texture, td.data, tu.data) == GI_CONTEXT_OK);
    CHECK(gi_texture_native(share, texture, &t) == GI_CONTEXT_OK);
    CHECK(vgo_fixture_images == 0);
    CHECK(vgt_texture_ensure_backing(ts, t, (vgt_view){0}) != VGT_OK);
    CHECK(vgo_fixture_images == 0);
    geometry(td.data);
    unsigned char before[VGT_TD_RB_LENGTH]; memcpy(before, td.data, sizeof(before));
    memset(tu.data + 8, 0xff, 12); tu.data[0x14] = 1;
    CHECK(gi_modify_texture(share, texture, 1) == GI_CONTEXT_OK);
    CHECK(vgt_texture_ensure_backing(ts, t, (vgt_view){0}) == VGT_OK);
    CHECK(!memcmp(before, td.data, sizeof(before)));
    for (size_t i = 8; i < VGT_TU_LENGTH; ++i) CHECK(tu.data[i] == 0);
    CHECK(vgo_fixture_images == 1);
    vgc_handle image, fb;
    CHECK(vgt_bgra_texture_image_get(ns, t, &image) == VGT_OK);
    CHECK(vgc_framebuffer_create(ns, &fb) == kIOReturnSuccess);
    CHECK(vgc_framebuffer_attach(fb, 0, image) == kIOReturnSuccess);
    CHECK(vgc_framebuffer_load(fb) == kIOReturnSuccess);
    CHECK(vgt_retain(t) == VGT_OK);
    /* 已有 texture/FBO 引用仍指向原对象；Modify 后资源重建，旧 FBO 显式重绑。 */
    CHECK(gi_modify_texture(share, texture, 1) == GI_CONTEXT_OK);
    CHECK(gi_texture_native(share, texture, &same) == GI_CONTEXT_OK && same == t);
    CHECK(vgt_texture_ensure_backing(ts, t, (vgt_view){0}) == VGT_OK);
    vgc_handle new_image;
    CHECK(vgt_bgra_texture_image_get(ns, t, &new_image) == VGT_OK && new_image != image);
    CHECK(vgo_fixture_images == 2);
    CHECK(vgc_framebuffer_load(fb) == kIOReturnSuccess);
    CHECK(vgc_framebuffer_attach(fb, 0, new_image) == kIOReturnSuccess);
    CHECK(vgc_framebuffer_load(fb) == kIOReturnSuccess);
    CHECK(vgo_fixture_images == 1);
    CHECK(gi_destroy_texture(share, texture) == GI_CONTEXT_OK);
    CHECK(vgt_release(t) == VGT_OK);
    CHECK(vgo_fixture_images == 1);
    CHECK(vgc_framebuffer_load(fb) == kIOReturnSuccess);
    CHECK(vgc_release(fb) == kIOReturnSuccess);
    CHECK(vgo_fixture_images == 0);
    CHECK(!memcmp(before, td.data, sizeof(before)));
    guard_destroy(td); guard_destroy(tu);
}
int main(void) {
    vgc_handle ns = 0, sn; vgt_handle ts; vgo_handle os;
    void *share = NULL;
    vgo_fixture_bgra = 1;
    CHECK(vgc_share_create_bgra(&ns) == kIOReturnSuccess);
    CHECK(gi_register_native_share(&device_token, 73, ns) == GI_CONTEXT_OK);
    CHECK(vgc_release(ns) == kIOReturnSuccess);
    CHECK(gi_context_set_policy(GI_CONTEXT_METADATA_EXPERIMENT) == GI_CONTEXT_OK);
    CHECK(gi_create_share_group(&device_token, &share, 4) == GI_CONTEXT_OK);
    CHECK(gi_share_native(share, &sn, &ts, &os) == GI_CONTEXT_OK && ns == sn);
    void *invalid = &device_token;
    CHECK(gi_create_texture(share, &invalid, NULL, NULL) == GI_CONTEXT_INVALID && !invalid);
    metadata_cases(share, ts);
    renderbuffer_case(share, ns, ts);
    CHECK(gi_destroy_share_group(share) == GI_CONTEXT_OK);
    CHECK(gi_unregister_native_share(&device_token) == GI_CONTEXT_OK);
    CHECK(vgt_bgra_drain_releases() == kIOReturnSuccess);
    CHECK(vgo_fixture_owners == 0 && vgo_fixture_images == 0);
    CHECK(gi_context_set_policy(GI_CONTEXT_CLOSED) == GI_CONTEXT_OK);
    puts("PASS CPU: 11 default metadata / exact producer flags / guard pages / canary / no implicit upgrade / backing / FBO / retained references");
    puts("CPU fixture only: no GLD invocation, installation, GPU, clear or readback");
    return 0;
}
