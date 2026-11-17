#include "integration.h"
#include "render_exports.h"
#include <OpenGL/CGLTypes.h>

/* 复用 CPU 像素模型，正式构建不链接本文件。 */
int render_baseline_main(void);
IOReturn model_clear(vgpu_surface_image *, const float[4], uint64_t *);
IOReturn model_read(vgpu_surface_image *, uint64_t, vgpu_surface_rect, uint32_t,
                    vgpu_surface_pack, void *, size_t);
#define main render_baseline_main
#define vgpu_surface_image_clear model_clear
#define vgpu_surface_image_read model_read
#include "render_exports_probe.c"
#undef main
#undef vgpu_surface_image_clear
#undef vgpu_surface_image_read

static IOReturn injected_clear;
static size_t observed_span;
static uint64_t observed_generation;
IOReturn vgpu_surface_image_clear(vgpu_surface_image *image, const float rgba[4], uint64_t *gen)
{
    if (injected_clear) return injected_clear;
    return model_clear(image, rgba, gen);
}
IOReturn vgpu_surface_image_read(vgpu_surface_image *image, uint64_t gen,
    vgpu_surface_rect rect, uint32_t format, vgpu_surface_pack pack, void *out, size_t capacity)
{
    observed_span = capacity; observed_generation = gen;
    return model_read(image, gen, rect, format, pack, out, capacity);
}

void gi_device_initialize(const uint32_t *, const uint8_t *, uint32_t, uintptr_t, uintptr_t, uint32_t);
void gi_device_terminate(void);
int32_t gi_device_create(void **, uint32_t, void *);
int32_t gi_device_destroy(void *);
int32_t gi_device_populate(void *, void *);
int32_t gi_pixel_choose(void **, const uint32_t *);
int32_t gi_pixel_destroy(void *);
static gp_format_cap cap;
static size_t cap_count;
static gp_pixel_list pixel_list;
void gi_device_initialize(const uint32_t *s, const uint8_t *b, uint32_t m,
    uintptr_t f, uintptr_t d, uint32_t flags)
{ (void)s; (void)b; (void)m; (void)f; (void)d; (void)flags; }
void gi_device_terminate(void) {}
int32_t gi_device_create(void **out, uint32_t mask, void *config)
{ (void)mask; (void)config; *out = &device; return 0; }
int32_t gi_device_destroy(void *p) { CHECK(p == &device); return 0; }
int32_t gi_device_populate(void *p, void *out) { (void)p; memset(out, 0, 0x88); return 0; }
void vgpuGLDDeviceGetCounts(struct gld_device_counts *out) { memset(out, 0, sizeof(*out)); }
int32_t gpx_configure(const gp_format_cap *formats, size_t count)
{ cap_count = count; if (count) cap = formats[0]; return 0; }
int32_t gi_pixel_choose(void **out, const uint32_t *tokens)
{
    size_t n = 0; while (tokens[n]) ++n;
    gp_status r = gp_choose_normalized(tokens, (n + 1) * 4, &cap, cap_count, NULL, &pixel_list);
    *out = pixel_list.head;
    return r == GP_OK || r == GP_NO_MATCH ? 0 : kCGLBadAttribute;
}
int32_t gi_pixel_destroy(void *head)
{ CHECK(head == pixel_list.head); gp_pixel_list_destroy(&pixel_list); return 0; }
static void system_read(void *context, void *out)
{
    CHECK(gldReadFramebufferData(context, 0, 0, 2, 2, 0x1908, 0x1401, out, 0, NULL) == 0);
}
static void read_status(int32_t expected)
{
    struct gi_status s; vgpuIntegrationStatus(&s); CHECK(s.render_status == expected);
    CHECK(!s.context_supported && s.metadata_enabled);
}
int main(void)
{
    CHECK(render_baseline_main() == 0);
    void *dev = NULL, *share = NULL, *pixel = NULL, *c[2] = {0}, *tex[2] = {0}, *fb[2] = {0};
    unsigned char a[2][0x268], h[2][VGC_H_LENGTH] = {{0}}, b[2][VGC_B_LENGTH] = {{0}};
    unsigned char td[2][VGT_TD_RB_LENGTH], tu[2][VGT_TU_LENGTH];
    unsigned char fq[2][VGF_FQ_LENGTH] = {{0}}, fd[2][VGF_FD_LENGTH] = {{0}};
    unsigned char values[28] = {0}, pixels[128], saved[128];
    const float rgba[4] = {0.2f, 0.4f, 0.6f, 0.8f}; memcpy(values, rgba, sizeof(rgba));
    const unsigned char expected[4] = {51, 102, 153, 204};
    const uint32_t tokens[] = {8, 32, 11, 8, 0};
    gldInitializeLibrary(NULL, NULL, 0, 0, 0, 0);
    OK(gldCreateDevice(&dev, 1, NULL));
    CHECK(gldCreateShareGroup(dev, &share, 4) != 0 && !share);
    OK(vgpuIntegrationEnableMetadata(dev)); OK(vgpuIntegrationPublishCandidate(dev));
    OK(gldChoosePixelFormat(&pixel, tokens)); OK(gldCreateShareGroup(dev, &share, 4));
    for (size_t i = 0; i < 2; ++i) {
        OK(gldCreateContext(share, &c[i], pixel, a[i], h[i], b[i]));
        rb(td[i], tu[i]);
        /* 系统 CreateTexture 仍仅借用元数据 span；完整 RB 由有界测试入口提供。 */
        OK(gi_create_texture_views(share, &tex[i], (vgt_views){{td[i], sizeof(td[i])}, {tu[i], sizeof(tu[i])}, {0}}));
        put16(fq[i], 0, 0x2000); put16(fq[i], 0xb2, 256); put16(fq[i], 0xb4, 256); put32(fq[i], 0xc0, 1);
        token(fd[i], 0, tex[i]); fd[i][0x50] = 1;
        OK(gldCreateFramebuffer(share, &fb[i], fq[i], fd[i])); CHECK(gldLoadFramebuffer(share, fb[i]) == 1);
        token(b[i], 0x4a0, fb[i]); token(b[i], 0x4a8, fb[i]);
        put16(h[i], 0x3cae, 1); memset(h[i] + 0x3ca8, 1, 4); put32(h[i], 0x4ee4, 4);
        CHECK(gldGetError(c[i]) == 0);
    }
    memset(pixels, 0xa7, sizeof(pixels)); memcpy(saved, pixels, sizeof(saved));
    system_read(c[0], pixels); read_status(kIOReturnNotReady); CHECK(!memcmp(pixels, saved, sizeof(pixels)));
    CHECK(gldGetError(c[1]) == 0 && gldGetError(c[0]) == 0x502 && gldGetError(c[0]) == 0);
    gldClearFramebufferData(c[0], 1, values); read_status(0);
    gldClearFramebufferData(c[0], 1, values); read_status(0);
    gldClearFramebufferData(c[1], 1, values); read_status(0);
    /* 全局 last=1，而 read image0=2；然后 draw0/read1 分离。 */
    system_read(c[0], pixels); read_status(0); CHECK(observed_generation == 2 && observed_span == 16);
    token(b[0], 0x4a8, fb[1]); system_read(c[0], pixels); read_status(0); CHECK(observed_generation == 1);
    gldClearFramebufferData(c[1], 1, values); system_read(c[0], pixels); read_status(0); CHECK(observed_generation == 2);
    /* GLEngine 已移动 pixels；当前 stride 优先，不再次应用裁剪。 */
    put32(h[0], 0x4ec8, 4); put32(h[0], 0x4ecc, 24); put32(h[0], 0x4ed8, 1); put32(h[0], 0x4edc, 1);
    memset(pixels, 0xa7, sizeof(pixels)); system_read(c[0], pixels + 7); read_status(0); CHECK(observed_span == 60);
    for (size_t i = 0; i < sizeof(pixels); ++i) {
        int written = (i >= 35 && i < 43) || (i >= 59 && i < 67);
        CHECK(pixels[i] == (written ? expected[(i - 35) % 4] : 0xa7));
    }
    put32(h[0], 0x4ecc, 0); put32(h[0], 0x4ee4, 8);
    system_read(c[0], pixels); read_status(0); CHECK(observed_span == 44);
    /* 行重叠仍按最大写终点计算，不强制 rowLength>=width。 */
    put32(h[0], 0x4ecc, 4); system_read(c[0], pixels); read_status(0); CHECK(observed_span == 20);
    memcpy(saved, pixels, sizeof(saved));
    unsigned before = reads;
    CHECK(gldReadFramebufferData(c[0], 0, 0, 2, 2, 0x1908, 0x1401, pixels, 2, NULL) == 0);
    CHECK(gldReadFramebufferData(c[0], 0, 0, 2, 2, 0x1908, 0x1401, pixels, 0, tex[0]) == 0);
    CHECK(gldReadFramebufferData(c[0], 0, 0, 2, 2, 0x80e1, 0x1401, pixels, 0, NULL) == 0);
    CHECK(gldReadFramebufferData(c[0], 0, 0, 2, 2, 0x1908, 0x1406, pixels, 0, NULL) == 0);
    h[0][0x4ee8] = 1; system_read(c[0], pixels); h[0][0x4ee8] = 0;
    h[0][0x4ee9] = 1; system_read(c[0], pixels); h[0][0x4ee9] = 0;
    put32(h[0], 0x4ee4, 3); system_read(c[0], pixels); put32(h[0], 0x4ee4, 4);
    CHECK(gldReadFramebufferData(c[0], -1, 0, 1, 1, 0x1908, 0x1401, pixels, 0, NULL) == 0);
    CHECK(gldReadFramebufferData(c[0], 0, 0, 0, 1, 0x1908, 0x1401, pixels, 0, NULL) == 0);
    system_read(c[0], (void *)(uintptr_t)(UINTPTR_MAX - 3)); system_read(c[0], NULL);
    put32(h[0], 0x4ecc, UINT32_MAX); put32(h[0], 0x4ed8, UINT32_MAX); put32(h[0], 0x4edc, UINT32_MAX);
    system_read(c[0], pixels); read_status(kIOReturnBadArgument);
    CHECK(reads == before && !memcmp(pixels, saved, sizeof(pixels)));
    CHECK(gldGetError(c[0]) == 0x502 && gldGetError(c[0]) == 0 && gldGetError(c[1]) == 0);
    put32(h[0], 0x4ecc, 0); put32(h[0], 0x4ed8, 0); put32(h[0], 0x4edc, 0);
    /* OOM 首错保留，查询清错不消除 submit_failed；成功 read 也不恢复提交。 */
    injected_clear = kIOReturnNoMemory; gldClearFramebufferData(c[0], 1, values);
    injected_clear = 0; gldFlushContext(c[0]); read_status(kIOReturnNotReady);
    CHECK(gldGetError(c[0]) == 0x505 && gldGetError(c[0]) == 0);
    system_read(c[0], pixels); read_status(0); CHECK(gldGetError(c[0]) == 0);
    gldClearFramebufferData(c[0], 0, values); gldWaitForContext(c[0]); read_status(kIOReturnNotReady);
    CHECK(gldGetError(c[0]) == 0x502 && gldGetError(c[1]) == 0);
    gldClearFramebufferData(c[0], 1, values); gldFlushContext(c[0]); read_status(0);
    fail_read = 1; system_read(c[0], pixels); read_status(kIOReturnNotReady);
    CHECK(gldGetError(c[0]) == 0x502); fail_read = 0;
    /* 更换附件之后代次必须来自新 image。 */
    token(fd[1], 0, tex[0]); fd[1][0x50] = 1;
    system_read(c[0], pixels); read_status(0); CHECK(observed_generation == 3);
    for (size_t i = 0; i < 2; ++i) OK(gldDestroyContext(c[i]));
    CHECK(gldGetError(c[0]) == 0);
    for (size_t i = 0; i < 2; ++i) { OK(gldDestroyFramebuffer(share, fb[i])); OK(gldDestroyTexture(share, tex[i])); }
    OK(gldDestroyShareGroup(share)); OK(gldDestroyPixelFormat(pixel)); OK(gldDestroyDevice(dev));
    gldTerminateLibrary(); CHECK(!owners && !images);
    puts("PASS integration system read/error: 当前READ代次/跨context/附件替换/pack与canary/溢出/PBO拒绝/独立首错/OOM/清错保留故障；CPU only");
    return 0;
}
