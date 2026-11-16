#include "../gld-context/vgpu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>

/* 仅在显式运行时接触真实 GPU；CPU fixture 必须拒绝 BGRA owner_open。 */
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); failed = 1; goto cleanup; } } while (0)
#define EXPECT(x, e) do { IOReturn r_ = (x); ++checks; if (r_ != (e)) { fprintf(stderr, "FAIL line=%d %s got=%08x expected=%08x\n", __LINE__, #x, (unsigned)r_, (unsigned)(e)); failed = 1; goto cleanup; } } while (0)
static void put32(unsigned char *p, size_t n, uint32_t v) { memcpy(p + n, &v, sizeof(v)); }
static void put64(unsigned char *p, size_t n, uint64_t v) { memcpy(p + n, &v, sizeof(v)); }
static IOReturn bind(vgc_handle c, unsigned char *b, vgc_handle f)
{
    put64(b, 0x4a0, f); put64(b, 0x4a8, f);
    uint32_t dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    return vgc_context_update(c, &dirty);
}
static int packed(const unsigned char *p, size_t n, const unsigned char rgba[4])
{
    for (size_t k = 0; k < n; ++k) {
        unsigned char wanted = 0xa5;
        if (k >= 1040) {
            size_t relative = k - 1040;
            if (relative / 1032 < 256 && relative % 1032 < 1024)
                wanted = rgba[(relative % 1032) % 4];
        }
        if (p[k] != wanted) return 0;
    }
    return 1;
}
enum { CLEANUP_ATTEMPTS = 3 };
static void release_cleanup(vgc_handle *handle, const char *name, int *failed)
{
    for (unsigned attempt = 1; *handle && attempt <= CLEANUP_ATTEMPTS; ++attempt) {
        IOReturn r = vgc_release(*handle);
        if (!r) { *handle = 0; return; }
        *failed = 1;
        fprintf(stderr, "cleanup release %s handle=%llu attempt=%u/%u error=%08x\n",
            name, (unsigned long long)*handle, attempt, (unsigned)CLEANUP_ATTEMPTS, (unsigned)r);
    }
    if (*handle)
        fprintf(stderr, "cleanup release %s 未完成，保留 handle=%llu\n",
            name, (unsigned long long)*handle);
}
static void drain_cleanup(int *failed)
{
    for (unsigned attempt = 1; attempt <= CLEANUP_ATTEMPTS; ++attempt) {
        IOReturn r = vgc_drain_releases();
        if (!r) return;
        *failed = 1;
        fprintf(stderr, "cleanup drain attempt=%u/%u error=%08x\n",
            attempt, (unsigned)CLEANUP_ATTEMPTS, (unsigned)r);
    }
    fprintf(stderr, "cleanup drain 未完成，待释放资源仍需重试\n");
}
int main(int argc, char **argv)
{
    int failed = 0;
    vgc_handle share = 0, other = 0, c[2] = {0}, f[2] = {0}, image[2] = {0}, extra = 0;
    unsigned char h[2][VGC_H_LENGTH] = {{0}}, b[2][VGC_B_LENGTH] = {{0}};
    const size_t size = 1040 + 256 * 1032 + 16;
    unsigned char *dst = NULL, pixel[4] = {0}, saved[8];
    uint64_t gen[2] = {0}, completed = 0;
    vgc_state state;
    vgc_image_desc desc = {256, 256, VGPU_SURFACE_BGRA8, 0};
    vgc_clear_desc color[2] = {{1, 15, 0, 0, {1, 0, 0, 0.2f}}, {1, 15, 0, 0, {0, 1, 1, 0.8f}}};
    const unsigned char rgba[2][4] = {{255, 0, 0, 51}, {0, 255, 255, 204}};
    int registry = 0, denied = 0;
    uint64_t registry_id = 0;
    for (int arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--registry")) registry = 1;
        else if (!strcmp(argv[arg], "--expect-denied")) denied = 1;
        else { failed = 1; goto cleanup; }
    }
    CHECK(denied ? geteuid() != 0 : geteuid() == 0);
    if (registry) {
        io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("vgpuAccelerator"));
        CHECK(service != IO_OBJECT_NULL);
        IOReturn result = IORegistryEntryGetRegistryEntryID(service, &registry_id);
        IOObjectRelease(service);
        CHECK(result == kIOReturnSuccess && registry_id);
        printf("registry_id=0x%llx\n", (unsigned long long)registry_id);
        EXPECT(vgc_share_create_bgra_registry(registry_id, NULL), kIOReturnBadArgument);
        share = UINT64_MAX;
        EXPECT(vgc_share_create_bgra_registry(0, &share), kIOReturnBadArgument); CHECK(!share);
        share = UINT64_MAX;
        EXPECT(vgc_share_create_bgra_registry(UINT64_MAX, &share), kIOReturnNotFound); CHECK(!share);
        service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOResources"));
        CHECK(service != IO_OBJECT_NULL);
        uint64_t wrong_id = 0;
        result = IORegistryEntryGetRegistryEntryID(service, &wrong_id);
        IOObjectRelease(service);
        CHECK(result == kIOReturnSuccess && wrong_id);
        share = UINT64_MAX;
        EXPECT(vgc_share_create_bgra_registry(wrong_id, &share), kIOReturnBadArgument); CHECK(!share);
    }
    if (denied) {
        EXPECT(registry ? vgc_share_create_bgra_registry(registry_id, &share) :
            vgc_share_create_bgra(&share), kIOReturnNotPrivileged);
        CHECK(!share);
        goto cleanup;
    }
    EXPECT(registry ? vgc_share_create_bgra_registry(registry_id, &share) :
        vgc_share_create_bgra(&share), kIOReturnSuccess);
    EXPECT(registry ? vgc_share_create_bgra_registry(registry_id, &other) :
        vgc_share_create_bgra(&other), kIOReturnSuccess);
    EXPECT(vgc_context_create(share, (vgc_view){h[0], sizeof(h[0])},
        (vgc_view){b[0], sizeof(b[0])}, (vgc_handle *)(void *)h[0]), kIOReturnBadArgument);
    CHECK(h[0][0] == 0);
    vgc_image_desc wrong = desc; wrong.backing_format = VGPU_SURFACE_BGRX8;
    EXPECT(vgc_image_create(share, wrong, &extra), kIOReturnUnsupported);
    CHECK(!extra);
    for (unsigned j = 0; j < 2; ++j) {
        put32(h[j], 0x4ee4, 4);
        EXPECT(vgc_context_create(share, (vgc_view){h[j], sizeof(h[j])},
            (vgc_view){b[j], sizeof(b[j])}, &c[j]), kIOReturnSuccess);
        EXPECT(vgc_image_create(share, desc, &image[j]), kIOReturnSuccess);
        EXPECT(vgc_framebuffer_create(share, &f[j]), kIOReturnSuccess);
        EXPECT(vgc_framebuffer_attach(f[j], 0, image[j]), kIOReturnSuccess);
        EXPECT(bind(c[j], b[j], f[j]), kIOReturnSuccess);
        EXPECT(vgc_clear(c[j], &color[j], &gen[j]), kIOReturnSuccess);
        if (j == 1) EXPECT(vgc_clear(c[j], &color[j], &gen[j]), kIOReturnSuccess);
        CHECK(gen[j] != 0);
    }
    /* 第三个资源必须失败，不能发布句柄；不同 owner 共享同一两槽容量。 */
    CHECK(vgc_image_create(other, desc, &extra) != kIOReturnSuccess);
    CHECK(extra == 0);
    EXPECT(vgc_framebuffer_create(other, &extra), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_attach(extra, 0, image[0]), kIOReturnBadArgument);
    EXPECT(vgc_release(extra), kIOReturnSuccess); extra = 0;
    dst = malloc(size); CHECK(dst != NULL);
    for (unsigned j = 0; j < 2; ++j) {
        put32(h[j], 0x4ecc, 1032); put32(h[j], 0x4ed8, 1);
        put32(h[j], 0x4edc, 2); put32(h[j], 0x4ee4, 8);
        memset(dst, 0xa5, size);
        EXPECT(vgc_read(c[j], gen[j], (vgpu_surface_rect){0, 0, 256, 256}, dst, size), kIOReturnSuccess);
        CHECK(packed(dst, size, rgba[j]));
        EXPECT(vgc_flush_completed(c[j], &completed), kIOReturnSuccess);
        CHECK(completed == gen[j]);
        memset(dst, 0xa5, size);
        EXPECT(vgc_read(c[j], gen[j], (vgpu_surface_rect){0, 0, 256, 256}, dst, 1040 + 255 * 1032 + 1023), kIOReturnBadArgument);
        for (size_t k = 0; k < size; ++k) CHECK(dst[k] == 0xa5);
        put32(h[j], 0x4ecc, 0); put32(h[j], 0x4ed8, 0);
        put32(h[j], 0x4edc, 0); put32(h[j], 0x4ee4, 4);
    }
    /* H/B 输出别名必须在写入前拒绝，也检查另一个 context 的借用区。 */
    memcpy(saved, b[1] + 0x4a0, sizeof(saved));
    EXPECT(vgc_clear(c[0], &color[0], (uint64_t *)(void *)(b[1] + 0x4a0)), kIOReturnBadArgument);
    EXPECT(vgc_share_create_bgra((vgc_handle *)(void *)(b[1] + 0x4a0)), kIOReturnBadArgument);
    EXPECT(vgc_flush_completed(c[0], (uint64_t *)(void *)(b[1] + 0x4a0)), kIOReturnBadArgument);
    EXPECT(vgc_context_update(c[0], (uint32_t *)(void *)(b[1] + 0x4a0)), kIOReturnBadArgument);
    EXPECT(vgc_read(c[0], gen[0], (vgpu_surface_rect){0, 0, 1, 1}, b[1] + 0x4a0, 4), kIOReturnBadArgument);
    CHECK(!memcmp(saved, b[1] + 0x4a0, sizeof(saved)));
    /* 重绑到 B，再改 B 附件为 A；flush 仍是各 context 最后提交。 */
    EXPECT(bind(c[0], b[0], f[1]), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_attach(f[1], 0, image[0]), kIOReturnSuccess);
    EXPECT(vgc_flush_completed(c[0], &completed), kIOReturnSuccess);
    CHECK(completed == gen[0]);
    EXPECT(vgc_flush_completed(c[1], &completed), kIOReturnSuccess);
    CHECK(completed == gen[1]);
    EXPECT(vgc_clear(c[1], &color[1], &gen[1]), kIOReturnSuccess);
    completed = UINT64_MAX;
    EXPECT(vgc_flush_completed(c[0], &completed), kIOReturnNotReady);
    CHECK(completed == UINT64_MAX);
    EXPECT(vgc_read(c[1], gen[1], (vgpu_surface_rect){0, 0, 1, 1}, pixel, sizeof(pixel)), kIOReturnSuccess);
    CHECK(!memcmp(pixel, rgba[1], sizeof(pixel)));
    EXPECT(vgc_framebuffer_attach(f[1], 0, image[1]), kIOReturnSuccess);
    EXPECT(vgc_clear(c[0], &color[0], &gen[0]), kIOReturnSuccess);
    vgc_clear_desc bad = color[0]; bad.rgba[0] = -1;
    completed = UINT64_MAX;
    EXPECT(vgc_clear(c[0], &bad, &completed), kIOReturnBadArgument);
    CHECK(completed == UINT64_MAX);
    EXPECT(vgc_context_state(c[0], &state), kIOReturnSuccess);
    CHECK(state.dirty && state.failed && state.completed_generation == gen[0]);
    EXPECT(vgc_flush_completed(c[0], &completed), kIOReturnNotReady);
    EXPECT(vgc_clear(c[0], &color[0], &gen[0]), kIOReturnSuccess);
    /* 两个 last 各持一个 image；拆掉全部附件/调用者引用仍占两槽。 */
    for (unsigned j = 0; j < 2; ++j) {
        EXPECT(vgc_framebuffer_attach(f[j], 0, 0), kIOReturnSuccess);
        EXPECT(bind(c[j], b[j], 0), kIOReturnSuccess);
        EXPECT(vgc_release(f[j]), kIOReturnSuccess); f[j] = 0;
        EXPECT(vgc_release(image[j]), kIOReturnSuccess); image[j] = 0;
        EXPECT(vgc_flush_completed(c[j], &completed), kIOReturnSuccess);
    }
    CHECK(vgc_image_create(other, desc, &extra) != kIOReturnSuccess); CHECK(!extra);
    EXPECT(vgc_release(share), kIOReturnSuccess); share = 0;
    EXPECT(vgc_release(c[0]), kIOReturnSuccess); c[0] = 0;
    EXPECT(vgc_drain_releases(), kIOReturnSuccess);
    EXPECT(vgc_image_create(other, desc, &image[0]), kIOReturnSuccess);
    CHECK(vgc_image_create(other, desc, &extra) != kIOReturnSuccess); CHECK(!extra);
    EXPECT(vgc_release(c[1]), kIOReturnSuccess); c[1] = 0;
    EXPECT(vgc_drain_releases(), kIOReturnSuccess);
    EXPECT(vgc_image_create(other, desc, &image[1]), kIOReturnSuccess);
    puts("双 image 全图 alpha/pack、隔离、容量、last flush 与强引用重租通过");
cleanup:
    free(dst);
    for (unsigned j = 0; j < 2; ++j) {
        release_cleanup(&c[j], "context", &failed);
        release_cleanup(&f[j], "framebuffer", &failed);
        release_cleanup(&image[j], "image", &failed);
    }
    release_cleanup(&extra, "extra", &failed);
    release_cleanup(&share, "share", &failed);
    release_cleanup(&other, "other", &failed);
    drain_cleanup(&failed);
    printf("RESULT checks=%u status=%s\n", checks, failed ? "FAIL" : "PASS");
    return failed;
}
