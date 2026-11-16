#include "../gld-context/vgpu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
static int failure;
#define EXPECT(expr, wanted) do { IOReturn actual_ = (expr); ++checks; \
    if (actual_ != (wanted)) { \
        fprintf(stderr, "FAIL line=%d %s actual=0x%08x expected=0x%08x\n", \
                __LINE__, #expr, (unsigned)actual_, (unsigned)(wanted)); \
        failure = actual_ == kIOReturnNotReady && (wanted) == kIOReturnSuccess ? 75 : 1; \
        goto cleanup; } } while (0)
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #expr); failure = 1; goto cleanup; } } while (0)
static void put32(unsigned char *p, size_t offset, uint32_t value)
{ memcpy(p + offset, &value, sizeof(value)); }
static void put64(unsigned char *p, size_t offset, uint64_t value)
{ memcpy(p + offset, &value, sizeof(value)); }
static int pixels(const unsigned char *actual, size_t size, size_t offset,
                  size_t stride, unsigned width, unsigned height, const unsigned char color[4])
{
    unsigned char *expected = malloc(size);
    if (!expected) return 0;
    memset(expected, 0xa5, size);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            memcpy(expected + offset + y * stride + x * 4, color, 4);
    int equal = memcmp(actual, expected, size) == 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= actual[i];
        hash *= UINT64_C(1099511628211);
    }
    printf("PIXELS width=%u height=%u pixels=%u canary_bytes=%zu offset=%zu stride=%zu fnv1a64=%016llx status=%s\n",
           width, height, width * height, size - (size_t)width * height * 4,
           offset, stride, (unsigned long long)hash, equal ? "PASS" : "FAIL");
    free(expected);
    return equal;
}
int main(int argc, char **argv)
{
    vgc_handle share = 0, other = 0, context = 0, image = 0, fb = 0;
    vgc_handle foreign = 0, poison = 0, temporary = 0, second = 0;
    vgpu_surface *external = NULL;
    int framebuffer_owned = 0;
    unsigned char h[VGC_H_LENGTH] = {0}, b[VGC_B_LENGTH] = {0};
    unsigned char dst[512], before[512];
    unsigned char *full = NULL;
    vgc_view hv = { h, sizeof(h) }, bv = { b, sizeof(b) };
    vgc_image_desc desc = { 256, 256, VGPU_SURFACE_BGRX8, 0 };
    vgc_clear_desc clear = { 1, 15, 0, 0, { 0.2f, 0.4f, 0.8f, 0.3f } };
    const unsigned char color[4] = { 51, 102, 204, 255 };
    vgpu_surface_rect rect = { 13, 17, 3, 2 };
    vgc_state state;
    uint64_t generation = 0, old = 0, completed = 0;
    uint32_t dirty;
    printf("权限 uid=%u euid=%u\n", (unsigned)getuid(), (unsigned)geteuid());
    if (argc == 2 && strcmp(argv[1], "--expect-denied") == 0) {
        CHECK(geteuid() != 0);
        EXPECT(vgc_share_create(&share), kIOReturnNotPrivileged);
        CHECK(share == 0);
        printf("权限拒绝已验证 kIOReturnNotPrivileged=0x%08x\n", (unsigned)kIOReturnNotPrivileged);
        goto cleanup;
    }
    CHECK(argc == 1 && geteuid() == 0);
    puts("独立 context GPU probe：合成 H/B 仅用于契约输入，clear/read 使用真实连接");
    EXPECT(vgc_share_create(&share), kIOReturnSuccess);
    EXPECT(vgc_share_create(&other), kIOReturnSuccess);
    EXPECT(vgc_context_create(share, (vgc_view){h, sizeof(h)-1}, bv, &temporary), kIOReturnBadArgument);
    CHECK(temporary == 0);
    EXPECT(vgc_context_create(share, hv, (vgc_view){b, sizeof(b)-1}, &temporary), kIOReturnBadArgument);
    EXPECT(vgc_context_create(share, hv, bv, &context), kIOReturnSuccess);
    EXPECT(vgc_flush_completed(context, &completed), kIOReturnNotReady);
    vgc_image_desc invalid = desc;
    invalid.backing_format = VGPU_SURFACE_RGBA8_OUTPUT;
    EXPECT(vgc_image_create(share, invalid, &temporary), kIOReturnUnsupported);
    invalid = desc; invalid.width = 128;
    EXPECT(vgc_image_create(share, invalid, &temporary), kIOReturnUnsupported);
    invalid = desc; invalid.samples = 1;
    EXPECT(vgc_image_create(share, invalid, &temporary), kIOReturnUnsupported);
    EXPECT(vgc_image_create(share, desc, &image), kIOReturnSuccess);
    EXPECT(vgc_image_create(other, desc, &temporary), kIOReturnExclusiveAccess);
    EXPECT(vgc_framebuffer_create(share, &fb), kIOReturnSuccess); framebuffer_owned = 1;
    EXPECT(vgc_framebuffer_create(other, &foreign), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_create(share, &poison), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_load(fb), kIOReturnNotReady);
    EXPECT(vgc_framebuffer_state(fb, &state), kIOReturnSuccess);
    CHECK(state.dirty == 1 && state.failed == 1);
    EXPECT(vgc_framebuffer_attach(foreign, 0, image), kIOReturnBadArgument);
    EXPECT(vgc_framebuffer_attach(fb, 1, image), kIOReturnUnsupported);
    EXPECT(vgc_framebuffer_attach(fb, 0, UINT64_MAX), kIOReturnBadArgument);
    EXPECT(vgc_framebuffer_state(fb, &state), kIOReturnSuccess);
    CHECK(state.dirty == 1 && state.failed == 1);
    EXPECT(vgc_framebuffer_attach(fb, 0, image), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_attach(poison, 0, image), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_dirty(poison, 0x80), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_load(poison), kIOReturnUnsupported);
    EXPECT(vgc_framebuffer_state(poison, &state), kIOReturnSuccess);
    CHECK(state.dirty == 0x81 && state.failed == 1);
    EXPECT(vgc_framebuffer_attach(poison, 0, image), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_load(poison), kIOReturnUnsupported);
    EXPECT(vgc_framebuffer_state(poison, &state), kIOReturnSuccess);
    CHECK(state.dirty == 0x81 && state.failed == 1);
    EXPECT(vgc_framebuffer_load(fb), kIOReturnSuccess);
    put64(b, 0x4a0, fb); put64(b, 0x4a8, foreign);
    dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    EXPECT(vgc_context_update(context, &dirty), kIOReturnBadArgument);
    CHECK(dirty == (VGC_DRAW_DIRTY | VGC_READ_DIRTY));
    EXPECT(vgc_clear(context, &clear, &generation), kIOReturnNotReady);
    put64(b, 0x4a8, UINT64_MAX);
    EXPECT(vgc_context_update(context, &dirty), kIOReturnBadArgument);
    put64(b, 0x4a8, fb);
    dirty |= 0x40;
    EXPECT(vgc_context_update(context, &dirty), kIOReturnUnsupported);
    CHECK(dirty == (VGC_DRAW_DIRTY | VGC_READ_DIRTY | 0x40));
    dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    EXPECT(vgc_context_update(context, &dirty), kIOReturnSuccess);
    CHECK(!dirty);
    EXPECT(vgc_release(poison), kIOReturnSuccess); poison = 0;
    EXPECT(vgc_retain(image), kIOReturnSuccess);
    EXPECT(vgc_release(image), kIOReturnSuccess);
    EXPECT(vgc_release(image), kIOReturnSuccess); image = 0;
    EXPECT(vgc_release(share), kIOReturnSuccess); share = 0;
    EXPECT(vgc_release(fb), kIOReturnSuccess); framebuffer_owned = 0;
    EXPECT(vgc_clear(context, &clear, &generation), kIOReturnSuccess);
    CHECK(generation != 0);
    EXPECT(vgc_flush_completed(context, &completed), kIOReturnSuccess);
    CHECK(completed == generation);
    printf("GENERATION first=%llu completed=%llu\n", (unsigned long long)generation, (unsigned long long)completed);
    const size_t full_size = 16 + 1032 * 256 + 16;
    full = malloc(full_size);
    CHECK(full != NULL);
    put32(h, 0x4ecc, 1032); put32(h, 0x4edc, 4); put32(h, 0x4ee4, 8);
    memset(full, 0xa5, full_size);
    EXPECT(vgc_read(context, generation, (vgpu_surface_rect){0, 0, 256, 256}, full, full_size), kIOReturnSuccess);
    CHECK(pixels(full, full_size, 16, 1032, 256, 256, color));
    free(full); full = NULL;
    put32(h, 0x4ecc, 0); put32(h, 0x4edc, 0); put32(h, 0x4ee4, 4);
    memset(dst, 0xa5, sizeof(dst));
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnSuccess);
    CHECK(pixels(dst, sizeof(dst), 0, 12, 3, 2, color));
    put32(h, 0x4ec8, 7); put32(h, 0x4ed8, 1); put32(h, 0x4edc, 2); put32(h, 0x4ee4, 8);
    memset(dst, 0xa5, sizeof(dst));
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnSuccess);
    CHECK(pixels(dst, sizeof(dst), 40, 32, 3, 2, color));
    put32(h, 0x4ecc, 48);
    memset(dst, 0xa5, sizeof(dst));
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnSuccess);
    CHECK(pixels(dst, sizeof(dst), 56, 48, 3, 2, color));
    memcpy(before, dst, sizeof(dst));
    EXPECT(vgc_read(context, generation, rect, dst, 115), kIOReturnBadArgument);
    CHECK(!memcmp(before, dst, sizeof(dst)));
    put32(h, 0x4ee4, 3);
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnBadArgument);
    put32(h, 0x4ee4, 4); h[0x4ee8] = 1;
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnUnsupported);
    h[0x4ee8] = 0; h[0x4ee9] = 1;
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnUnsupported);
    h[0x4ee9] = 0;
    put32(h, 0x4ecc, UINT32_MAX); put32(h, 0x4ed8, UINT32_MAX);
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnBadArgument);
    CHECK(!memcmp(before, dst, sizeof(dst)));
    put32(h, 0x4ecc, 0); put32(h, 0x4ed8, 0); put32(h, 0x4edc, 0); put32(h, 0x4ec8, 0);
    vgc_clear_desc rejected = clear;
    rejected.internal_mask = 0x101;
    EXPECT(vgc_clear(context, &rejected, &generation), kIOReturnUnsupported);
    rejected.internal_mask = 0x201;
    EXPECT(vgc_clear(context, &rejected, &generation), kIOReturnUnsupported);
    rejected.internal_mask = 2;
    EXPECT(vgc_clear(context, &rejected, &generation), kIOReturnUnsupported);
    rejected = clear; rejected.color_write_mask = 7;
    EXPECT(vgc_clear(context, &rejected, &generation), kIOReturnUnsupported);
    rejected = clear; rejected.scissor_enabled = 1;
    EXPECT(vgc_clear(context, &rejected, &generation), kIOReturnUnsupported);
    rejected = clear; rejected.srgb_enabled = 1;
    EXPECT(vgc_clear(context, &rejected, &generation), kIOReturnUnsupported);
    rejected = clear; rejected.rgba[0] = -1.0f;
    EXPECT(vgc_clear(context, &rejected, &generation), kIOReturnBadArgument);
    EXPECT(vgc_context_state(context, &state), kIOReturnSuccess);
    CHECK(state.dirty && state.failed && state.completed_generation == generation);
    EXPECT(vgc_flush_completed(context, &completed), kIOReturnNotReady);
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnSuccess);
    old = generation;
    EXPECT(vgc_clear(context, &clear, &generation), kIOReturnSuccess);
    CHECK(old != generation);
    printf("GENERATION prior=%llu next=%llu\n", (unsigned long long)old, (unsigned long long)generation);
    memset(dst, 0xa5, sizeof(dst)); memcpy(before, dst, sizeof(dst));
    EXPECT(vgc_read(context, old, rect, dst, sizeof(dst)), kIOReturnNotReady);
    CHECK(!memcmp(before, dst, sizeof(dst)));
    EXPECT(vgpu_surface_open(256, 256, VGPU_SURFACE_BGRX8, &external), kIOReturnSuccess);
    EXPECT(vgpu_surface_clear(external, clear.rgba, &old), kIOReturnSuccess);
    printf("GENERATION external=%llu stale=%llu\n", (unsigned long long)old, (unsigned long long)generation);
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnNotReady);
    EXPECT(vgc_flush_completed(context, &completed), kIOReturnNotReady);
    CHECK(!memcmp(before, dst, sizeof(dst)));
    EXPECT(vgc_clear(context, &clear, &generation), kIOReturnSuccess);
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnSuccess);
    CHECK(pixels(dst, sizeof(dst), 0, 12, 3, 2, color));
    printf("GPU clear/read 已完成 generation=%llu，实时 pack/外部 stale 通过\n", (unsigned long long)generation);
    EXPECT(vgc_framebuffer_attach(fb, 0, 0), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_load(fb), kIOReturnNotReady);
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnNotReady);
    EXPECT(vgc_image_create(other, desc, &second), kIOReturnSuccess);
    put64(b, 0x4a0, 0); put64(b, 0x4a8, 0);
    dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    EXPECT(vgc_context_update(context, &dirty), kIOReturnSuccess);
    EXPECT(vgc_framebuffer_state(fb, &state), kIOReturnBadArgument); fb = 0;
    EXPECT(vgc_clear(context, &clear, &generation), kIOReturnNotReady);
    EXPECT(vgc_read(context, generation, rect, dst, sizeof(dst)), kIOReturnNotReady);
    EXPECT(vgc_release(context), kIOReturnSuccess);
    EXPECT(vgc_release(context), kIOReturnBadArgument); context = 0;
    EXPECT(vgc_retain(UINT64_MAX), kIOReturnBadArgument);
    puts("引用生命周期、lease 释放、解绑和无效句柄通过");
cleanup:
    free(full);
    if (external) vgpu_surface_release(external);
    if (context) (void)vgc_release(context);
    if (framebuffer_owned) (void)vgc_release(fb);
    if (image) (void)vgc_release(image);
    if (poison) (void)vgc_release(poison);
    if (foreign) (void)vgc_release(foreign);
    if (second) (void)vgc_release(second);
    if (share) (void)vgc_release(share);
    if (other) (void)vgc_release(other);
    printf("RESULT checks=%u status=%s exit=%d\n", checks, failure ? "FAIL" : "PASS", failure);
    return failure;
}
