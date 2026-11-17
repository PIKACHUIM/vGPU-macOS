/* 独立 dlopen 探针：只调用指定 integration 映像的导出，不链接驱动或 fixture。 */
#include "../gld-integration/integration.h"
#include "../gld-context/vgpu_context.h"
#include "../gld-texture/vgpu_texture.h"
#include "../gld-framebuffer/vgpu_framebuffer.h"
#include "../gld-state/vgpu_clear_state.h"
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); exit(1); } } while (0)

struct api {
    void (*initialize)(const uint32_t *, const uint8_t *, uint32_t, uintptr_t, uintptr_t, uint32_t);
    void (*terminate)(void);
    int32_t (*device_create)(void **, uint32_t, void *);
    int32_t (*device_destroy)(void *);
    int32_t (*enable)(void *);
    int32_t (*publish)(void *);
    int32_t (*pixel_choose)(void **, const uint32_t *);
    int32_t (*pixel_destroy)(void *);
    int32_t (*share_create)(void *, void **, uintptr_t);
    int32_t (*share_destroy)(void *);
    int32_t (*context_create)(void *, void **, void *, void *, void *, void *);
    int32_t (*context_destroy)(void *);
    int32_t (*texture_create)(void *, void **, void *, void *);
    int32_t (*texture_destroy)(void *, void *);
    int32_t (*fb_create)(void *, void **, void *, void *);
    int32_t (*fb_destroy)(void *, void *);
    int32_t (*fb_load)(void *, void *);
    uint32_t (*update_draw)(void *, uint32_t);
    uint32_t (*update_read)(void *, uint32_t);
    void (*clear)(void *, uint32_t, const void *);
    void (*flush)(void *);
    uint32_t (*error)(void *);
    int32_t (*read)(void *, int32_t, int32_t, int32_t, int32_t,
                    uint32_t, uint32_t, void *, uint32_t, void *);
    int32_t (*bounded_read)(void *, uint64_t, int32_t, int32_t, int32_t, int32_t,
                            uint32_t, uint32_t, void *, size_t, uint32_t, void *);
    void (*status)(struct gi_status *);
};

static void symbol(void *lib, void *base, const char *name, void *destination, size_t size)
{
    void *address = dlsym(lib, name);
    Dl_info info;
    CHECK(address && size == sizeof(address));
    CHECK(dladdr(address, &info) && info.dli_fbase == base);
    CHECK(info.dli_saddr == address && info.dli_sname && !strcmp(info.dli_sname, name));
    memcpy(destination, &address, size);
}

static void load_api(void *lib, struct api *a)
{
    Dl_info info;
    void *anchor = dlsym(lib, "vgpuIntegrationStatus");
    CHECK(anchor && dladdr(anchor, &info));
#define LOAD(field, name) symbol(lib, info.dli_fbase, #name, &a->field, sizeof(a->field))
    LOAD(initialize, gldInitializeLibrary); LOAD(terminate, gldTerminateLibrary);
    LOAD(device_create, gldCreateDevice); LOAD(device_destroy, gldDestroyDevice);
    LOAD(enable, vgpuIntegrationEnableMetadata); LOAD(publish, vgpuIntegrationPublishCandidate);
    LOAD(pixel_choose, gldChoosePixelFormat); LOAD(pixel_destroy, gldDestroyPixelFormat);
    LOAD(share_create, gldCreateShareGroup); LOAD(share_destroy, gldDestroyShareGroup);
    LOAD(context_create, gldCreateContext); LOAD(context_destroy, gldDestroyContext);
    LOAD(texture_create, gldCreateTexture); LOAD(texture_destroy, gldDestroyTexture);
    LOAD(fb_create, gldCreateFramebuffer); LOAD(fb_destroy, gldDestroyFramebuffer);
    LOAD(fb_load, gldLoadFramebuffer); LOAD(clear, gldClearFramebufferData);
    LOAD(update_draw, gldUpdateDrawFramebuffer); LOAD(update_read, gldUpdateReadFramebuffer);
    LOAD(read, gldReadFramebufferData); LOAD(error, gldGetError); LOAD(flush, gldFlushContext);
    LOAD(bounded_read, vgpuIntegrationRead); LOAD(status, vgpuIntegrationStatus);
#undef LOAD
    CHECK(!dlsym(lib, "gi_create_texture_views"));
    CHECK(!dlsym(lib, "vgpu_surface_image_clear"));
    printf("BUNDLE %s\n", info.dli_fname);
}

static void put16(void *p, size_t offset, uint16_t value)
{ memcpy((unsigned char *)p + offset, &value, sizeof(value)); }
static void put32(void *p, size_t offset, uint32_t value)
{ memcpy((unsigned char *)p + offset, &value, sizeof(value)); }
static void token(void *p, size_t offset, void *value)
{ memcpy((unsigned char *)p + offset, &value, sizeof(value)); }

/* 完整 RB 视图匹配 context_exports.h 已核验的系统 producer 契约：
 * Create 前 flags=0x2002、TD>=VGT_TD_RB_LENGTH；仍须真实 Load 成功。 */
struct target {
    unsigned char td[VGT_TD_RB_LENGTH], tu[VGT_TU_LENGTH];
    unsigned char fq[VGF_FQ_LENGTH], fd[VGF_FD_LENGTH];
    void *texture, *fb;
};
struct context {
    unsigned char a[0x268], h[VGC_H_LENGTH], b[VGC_B_LENGTH];
    void *handle;
};
static void describe_target(struct target *t)
{
    memset(t, 0, sizeof(*t));
    put16(t->td, 0, 0x2002); put16(t->td, 0x68, 0x8058);
    put16(t->td, 0x6a, 256); put16(t->td, 0x6c, 256); put16(t->td, 0x6e, 1);
    t->td[0x79] = t->td[0x7b] = 1;
    put16(t->td, 0xb4, 256); put16(t->td, 0xb6, 256); put16(t->td, 0xb8, 1);
    t->td[0xbb] = 0x20;
    put16(t->td, 0xbc, 256); put16(t->td, 0xbe, 256);
    put16(t->td, 0xc0, 0x1908); put16(t->td, 0xc2, 0x1401);
    put16(t->tu, 8, 1); t->tu[0x14] = 1;
    put16(t->fq, 0, VGF_RENDERBUFFER_TYPE);
    put16(t->fq, 0xb2, 256); put16(t->fq, 0xb4, 256); put32(t->fq, 0xc0, 1);
}
static void pack(struct context *c, uint32_t row, uint32_t stride,
                 uint32_t rows, uint32_t pixels, uint32_t alignment)
{
    put32(c->h, 0x4ec8, row); put32(c->h, 0x4ecc, stride);
    put32(c->h, 0x4ed8, rows); put32(c->h, 0x4edc, pixels); put32(c->h, 0x4ee4, alignment);
}
static struct gi_status render_status(struct api *a, int32_t expected)
{
    struct gi_status s;
    a->status(&s);
    if (s.render_status != expected)
        fprintf(stderr, "render=%08" PRIx32 " expected=%08" PRIx32 "\n",
                (uint32_t)s.render_status, (uint32_t)expected);
    CHECK(s.render_status == expected && !s.context_supported);
    return s;
}
/* 系统 dirty 是按值 uint32_t；返回1仅为 ABI 载体，须另查诊断及 GL 错误。 */
static void update_binding(struct api *a, struct context *c, int draw, uint32_t dirty)
{
    uint32_t result = draw ? a->update_draw(c->handle, dirty) : a->update_read(c->handle, dirty);
    CHECK(result == 1);
    (void)render_status(a, 0);
    CHECK(a->error(c->handle) == 0);
}
static uint64_t clear_color(struct api *a, struct context *c, const float rgba[4])
{
    unsigned char values[VGPU_CLEAR_V_MIN_LENGTH] = {0};
    memcpy(values, rgba, 4 * sizeof(float));
    a->clear(c->handle, 1, values);
    struct gi_status s = render_status(a, 0);
    CHECK(s.render_generation != 0);
    CHECK(a->error(c->handle) == 0);
    return s.render_generation;
}

/* 逐字节检查全部 RGBA 和所有未写区域；oracle 不执行任何驱动函数。 */
static int pixels_match(const unsigned char *p, size_t size, size_t first,
                        size_t stride, size_t width, size_t height,
                        const unsigned char rgba[4])
{
    for (size_t i = 0; i < size; ++i) {
        unsigned char expected = 0xa7;
        if (i >= first) {
            size_t delta = i - first;
            size_t y = delta / stride;
            if (y >= height) y = height - 1;
            size_t column = delta - y * stride;
            if (column < width * 4) expected = rgba[column % 4];
        }
        if (p[i] != expected) return 0;
    }
    return 1;
}
static void cpu_oracle(void)
{
    unsigned char pixels[96];
    const unsigned char rgba[4] = {51, 102, 153, 204};
    memset(pixels, 0xa7, sizeof(pixels));
    for (size_t y = 0; y < 2; ++y)
        for (size_t x = 0; x < 2; ++x) memcpy(pixels + 35 + y * 24 + x * 4, rgba, 4);
    CHECK(pixels_match(pixels, sizeof(pixels), 35, 24, 2, 2, rgba));
    /* 逐一破坏像素、alpha、前后及行间哨兵，验证检查器能检出每个字节。 */
    for (size_t i = 0; i < sizeof(pixels); ++i) {
        pixels[i] ^= 1;
        CHECK(!pixels_match(pixels, sizeof(pixels), 35, 24, 2, 2, rgba));
        pixels[i] ^= 1;
    }
    puts("PASS CPU 像素/pack 哨兵检查器；未打开 bundle、未访问设备，不代表渲染验收");
}
static void read_pixels(struct api *a, struct context *c, int32_t width, int32_t height, void *out)
{
    CHECK(a->read(c->handle, 0, 0, width, height, 0x1908, 0x1401, out, 0, NULL) == 0);
    (void)render_status(a, 0);
    CHECK(a->error(c->handle) == 0);
}
static void full_image(struct api *a, struct context *c, const unsigned char rgba[4])
{
    const size_t bytes = 256 * 256 * 4;
    unsigned char *out = malloc(bytes + 32);
    CHECK(out);
    memset(out, 0xa7, bytes + 32);
    pack(c, 0, 0, 0, 0, 4);
    read_pixels(a, c, 256, 256, out + 16);
    CHECK(pixels_match(out, bytes + 32, 16, 1024, 256, 256, rgba));
    free(out);
}
static void packed_image(struct api *a, struct context *c, const unsigned char rgba[4],
                         uint32_t stride, uint32_t alignment, size_t effective)
{
    unsigned char out[128];
    memset(out, 0xa7, sizeof(out));
    pack(c, 4, stride, 1, 1, alignment);
    read_pixels(a, c, 2, 2, out + 7);
    CHECK(pixels_match(out, sizeof(out), 7 + effective + 4, effective, 2, 2, rgba));
}
static void rejected_reads(struct api *a, struct context c[2], struct target t[2])
{
    unsigned char out[128], saved[128];
    memset(out, 0xa7, sizeof(out)); memcpy(saved, out, sizeof(out));
    pack(&c[0], 0, 0, 0, 0, 4);
    CHECK(a->error(c[0].handle) == 0 && a->error(c[1].handle) == 0);
    const uint32_t formats[] = {0x80e1, 0x1908, 0x1908, 0x1908};
    const uint32_t types[] = {0x1401, 0x1406, 0x1401, 0x1401};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(a->read(c[0].handle, 0, 0, 2, 2, formats[i], types[i], out,
                      i == 2 ? 2u : 0u, i == 3 ? t[0].texture : NULL) == 0);
        (void)render_status(a, kIOReturnUnsupported);
        CHECK(!memcmp(out, saved, sizeof(out)));
    }
    token(c[0].b, 0x4a8, t[0].texture);
    CHECK(a->read(c[0].handle, 0, 0, 2, 2, 0x1908, 0x1401, out, 0, NULL) == 0);
    (void)render_status(a, kIOReturnBadArgument);
    CHECK(!memcmp(out, saved, sizeof(out)));
    token(c[0].b, 0x4a8, NULL);
    CHECK(a->read(c[0].handle, 0, 0, 2, 2, 0x1908, 0x1401, out, 0, NULL) == 0);
    (void)render_status(a, kIOReturnNotReady);
    CHECK(!memcmp(out, saved, sizeof(out)));
    CHECK(a->error(c[1].handle) == 0);
    CHECK(a->error(c[0].handle) == 0x502 && a->error(c[0].handle) == 0);
    /* GetError 只消费 GL 错误，不清诊断；多个同类失败只消费一次。 */
    (void)render_status(a, kIOReturnNotReady);
    token(c[0].b, 0x4a8, t[0].fb);
}
static void cleanup(struct api *a, void *device, void *pixel, void *share,
                    struct context c[2], struct target t[2])
{
    for (size_t i = 0; i < 2; ++i) if (c[i].handle) CHECK(a->context_destroy(c[i].handle) == 0);
    for (size_t i = 0; i < 2; ++i) {
        if (t[i].fb) CHECK(a->fb_destroy(share, t[i].fb) == 0);
        if (t[i].texture) CHECK(a->texture_destroy(share, t[i].texture) == 0);
    }
    if (share) CHECK(a->share_destroy(share) == 0);
    if (pixel) CHECK(a->pixel_destroy(pixel) == 0);
    if (device) CHECK(a->device_destroy(device) == 0);
    a->terminate();
    struct gi_status s; a->status(&s);
    CHECK(!s.devices && !s.pixel_lists && !s.share_groups && !s.render_shares &&
          !s.render_contexts && !s.pending_cleanup && !s.native_registered && !s.lifecycle_status);
    CHECK(!s.backend.connections && !s.backend.service_refs);
    CHECK(s.backend.opens == s.backend.closes && s.backend.retains == s.backend.releases);
}
static int gpu_round(struct api *a, io_service_t service)
{
    uint32_t services[32] = {0}; uint8_t bytes[32] = {0};
    services[0] = service;
    a->initialize(services, bytes, 1, 0, 0, 0);
    void *device = NULL, *pixel = NULL, *share = NULL;
    struct context c[2] = {0}; struct target t[2] = {0};
    unsigned char config[0x190] = {0};
    const uint32_t attributes[] = {8, 32, 11, 8, 0};
    CHECK(a->device_create(&device, 1, config) == 0 && device);
    CHECK(a->enable(device) == 0);
    CHECK(a->publish(device) == 0);
    CHECK(a->pixel_choose(&pixel, attributes) == 0 && pixel);
    CHECK(a->share_create(device, &share, 4) == 0 && share);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(a->context_create(share, &c[i].handle, pixel, c[i].a, c[i].h, c[i].b) == 0);
        CHECK(c[i].handle && !a->error(c[i].handle));
        put16(c[i].h, 0x3cae, 1); memset(c[i].h + 0x3ca8, 1, 4);
        pack(&c[i], 0, 0, 0, 0, 4);
        describe_target(&t[i]);
        CHECK(a->texture_create(share, &t[i].texture, t[i].td, t[i].tu) == 0 && t[i].texture);
        token(t[i].fd, 0, t[i].texture); t[i].fd[0x50] = 1;
        CHECK(a->fb_create(share, &t[i].fb, t[i].fq, t[i].fd) == 0 && t[i].fb);
        if (a->fb_load(share, t[i].fb) != 1) {
            struct gi_status s; a->status(&s);
            fprintf(stderr, "BLOCKED 真实 FBO load 失败 render=%08" PRIx32
                    "；系统 CreateTexture/FBO 路径未能加载已描述的完整 RB。"
                    "未执行 clear/read 验收，BLOCKED 不算通过。\n",
                    (uint32_t)s.render_status);
            cleanup(a, device, pixel, share, c, t);
            return 3;
        }
        token(c[i].b, 0x4a0, t[i].fb); token(c[i].b, 0x4a8, t[i].fb);
        update_binding(a, &c[i], 1, VGC_DRAW_DIRTY);
        update_binding(a, &c[i], 0, VGC_READ_DIRTY);
        update_binding(a, &c[i], 1, 0);
        update_binding(a, &c[i], 0, 0);
    }
    const float colors[2][4] = {{0.2f, 0.4f, 0.6f, 0.8f}, {0.8f, 0.6f, 0.4f, 0.2f}};
    const unsigned char rgba[2][4] = {{51, 102, 153, 204}, {204, 153, 102, 51}};
    uint64_t generation[2];
    generation[0] = clear_color(a, &c[0], colors[0]);
    uint64_t previous = generation[0];
    generation[0] = clear_color(a, &c[0], colors[0]);
    CHECK(generation[0] > previous);
    generation[1] = clear_color(a, &c[1], colors[1]);
    /* 使用实际返回代次，不假定设备的初始代次为零。 */
    if (generation[0] == generation[1]) generation[0] = clear_color(a, &c[0], colors[0]);
    CHECK(generation[0] != generation[1]);
    printf("GEN image0=%" PRIu64 " image1=%" PRIu64 "\n", generation[0], generation[1]);
    for (size_t i = 0; i < 2; ++i) full_image(a, &c[i], rgba[i]);
    token(c[0].b, 0x4a8, t[1].fb);
    update_binding(a, &c[0], 0, VGC_READ_DIRTY);
    full_image(a, &c[0], rgba[1]);
    unsigned char out[128], saved[128];
    memset(out, 0xa7, sizeof(out)); memcpy(saved, out, sizeof(out));
    CHECK(a->bounded_read(c[0].handle, generation[0], 0, 0, 2, 2, 0x1908, 0x1401,
                          out, sizeof(out), 0, NULL) != 0);
    CHECK(!memcmp(out, saved, sizeof(out)) && !a->error(c[0].handle));
    CHECK(a->bounded_read(c[0].handle, generation[1], 0, 0, 2, 2, 0x1908, 0x1401,
                          out, sizeof(out), 0, NULL) == 0);
    CHECK(pixels_match(out, sizeof(out), 0, 8, 2, 2, rgba[1]));
    packed_image(a, &c[0], rgba[1], 24, 4, 24);
    packed_image(a, &c[0], rgba[1], 0, 8, 16);
    packed_image(a, &c[0], rgba[1], 4, 4, 4);
    /* 同一 context 改 draw 到 image1，同时 read 回 image0。 */
    token(c[0].b, 0x4a0, t[1].fb); token(c[0].b, 0x4a8, t[0].fb);
    update_binding(a, &c[0], 1, VGC_DRAW_DIRTY);
    update_binding(a, &c[0], 0, VGC_READ_DIRTY);
    generation[1] = clear_color(a, &c[0], colors[0]);
    full_image(a, &c[0], rgba[0]);
    full_image(a, &c[1], rgba[0]);
    generation[1] = clear_color(a, &c[1], colors[1]);
    full_image(a, &c[1], rgba[1]);
    token(c[0].b, 0x4a0, t[0].fb);
    update_binding(a, &c[0], 1, VGC_DRAW_DIRTY);
    puts("PASS GPU 按值 dirty：draw/read 初始绑定、零 dirty、重绑与分离像素验证");
    rejected_reads(a, c, t);
    for (size_t i = 0; i < 2; ++i) full_image(a, &c[i], rgba[i]);
    /* 合法状态拒绝制造 submit_failed，无故障注入、无危险 OOM 分配。 */
    unsigned char values[VGPU_CLEAR_V_MIN_LENGTH] = {0};
    memcpy(values, colors[0], sizeof(colors[0]));
    c[0].h[0x3d84] = 1;
    a->clear(c[0].handle, 1, values); (void)render_status(a, kIOReturnUnsupported);
    c[0].h[0x3d84] = 0;
    a->flush(c[0].handle); (void)render_status(a, kIOReturnNotReady);
    CHECK(a->error(c[1].handle) == 0);
    CHECK(a->error(c[0].handle) == 0x502 && a->error(c[0].handle) == 0);
    (void)render_status(a, kIOReturnNotReady);
    full_image(a, &c[0], rgba[0]);
    a->clear(c[0].handle, 0, values); (void)render_status(a, 0);
    a->flush(c[0].handle); (void)render_status(a, kIOReturnNotReady);
    CHECK(a->error(c[0].handle) == 0x502 && a->error(c[0].handle) == 0);
    generation[0] = clear_color(a, &c[0], colors[0]);
    a->flush(c[0].handle);
    CHECK(render_status(a, 0).render_generation == generation[0]);
    CHECK(!a->error(c[0].handle));
    /* 同一个 read FBO 换附件，系统 read 必须使用新 image 代次。 */
    token(c[0].b, 0x4a8, t[1].fb);
    token(t[1].fd, 0, t[0].texture); t[1].fd[0x50] = 1;
    full_image(a, &c[0], rgba[0]);
    token(t[1].fd, 0, t[1].texture); t[1].fd[0x50] = 1;
    full_image(a, &c[0], rgba[1]);
    cleanup(a, device, pixel, share, c, t);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--cpu-only")) { cpu_oracle(); return 0; }
    if (argc != 4 || (strcmp(argv[1], "--gpu") && strcmp(argv[1], "--empty")) ||
        strcmp(argv[2], "--bundle") || argv[3][0] != '/') {
        fprintf(stderr, "用法：%s --cpu-only | (--empty|--gpu) --bundle /绝对路径/映像\n", argv[0]);
        return 2;
    }
    const int gpu = !strcmp(argv[1], "--gpu");
    if (gpu && geteuid() != 0) { fputs("--gpu 需要主代理显式提权执行\n", stderr); return 2; }
    void *lib = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    struct api a; load_api(lib, &a);
    int result = 0;
    if (gpu) {
        io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("vgpuAccelerator"));
        CHECK(service != IO_OBJECT_NULL);
        for (unsigned round = 0; round < 2; ++round) {
            result = gpu_round(&a, service);
            if (result) break;
            printf("PASS GPU round=%u；完整释放后重租\n", round + 1);
        }
        CHECK(IOObjectRelease(service) == 0);
    } else {
        a.initialize(NULL, NULL, 0, 0, 0, 0);
        CHECK(a.error(NULL) == 0);
        CHECK(a.update_draw(NULL, VGC_DRAW_DIRTY) == 1);
        (void)render_status(&a, kIOReturnBadArgument);
        CHECK(a.update_read(NULL, VGC_READ_DIRTY) == 1);
        (void)render_status(&a, kIOReturnBadArgument);
        CHECK(a.error(NULL) == 0);
        CHECK(a.read(NULL, 0, 0, 1, 1, 0x1908, 0x1401, NULL, 0, NULL) == 0);
        (void)render_status(&a, kIOReturnBadArgument);
        a.flush(NULL); (void)render_status(&a, kIOReturnBadArgument);
        a.terminate();
        struct gi_status s; a.status(&s);
        CHECK(!s.devices && !s.render_contexts && !s.backend.connections && !s.backend.opens);
        puts("PASS CPU empty：独立映像导出来源及无设备按值 update/read/error/flush；未执行 GPU");
    }
    CHECK(dlclose(lib) == 0);
    printf("%s integration render checks=%u mode=%s；无系统私有调用、无安装\n",
           result ? "BLOCKED" : "PASS", checks, argv[1]);
    return result;
}
