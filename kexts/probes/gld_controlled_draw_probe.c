/* 独立翻译单元：真实 bundle 的系统25槽受控 Begin/End；不链接实现或 fixture。 */
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); exit(1); } } while (0)
enum { W = 256, H = 256, IMAGE_BYTES = W * H * 4, GUARD = 16, BYTES = IMAGE_BYTES + 32 };
struct triangle { float p[3][4]; int64_t window2[3][2]; };
/* 独立 GL 左下原点整数 oracle；边不穿过像素中心，z 覆盖负半区。 */
static const struct triangle shapes[2] = {
    {{{-.6875f,-.5625f,-1,1},{.3125f,-.5625f,-.5f,1},{-.6875f,.21875f,.5f,1}},
     {{80,112},{336,112},{80,312}}},
    {{{-.75f,.75f,-.75f,1},{-.125f,-.5f,0,1},{.5f,.75f,1,1}},
     {{64,448},{224,128},{384,448}}}
};
static const unsigned char backgrounds[2][4] = {{0,0,255,64},{255,0,255,128}};
static const unsigned char red[4] = {255,0,0,255};
static int inside(unsigned shape, unsigned x, unsigned y, int *boundary)
{
    int positive = 1, negative = 1;
    for (unsigned i = 0; i < 3; ++i) {
        const int64_t *a = shapes[shape].window2[i], *b = shapes[shape].window2[(i+1)%3];
        int64_t e = (b[0]-a[0])*(2*(int64_t)y+1-a[1]) -
                    (b[1]-a[1])*(2*(int64_t)x+1-a[0]);
        if (!e) *boundary = 1;
        positive &= e > 0; negative &= e < 0;
    }
    return positive || negative;
}
static unsigned char expected(size_t offset, unsigned target, unsigned mask)
{
    if (offset < GUARD || offset >= GUARD + IMAGE_BYTES) return 0xa7;
    size_t k = offset - GUARD;
    unsigned x = (unsigned)(k / 4 % W), y = (unsigned)(k / (W * 4));
    int boundary = 0;
    int covered = ((mask & 1u) && inside(0,x,y,&boundary)) ||
                  ((mask & 2u) && inside(1,x,y,&boundary));
    return (covered ? red : backgrounds[target])[k % 4];
}
static int verify(const unsigned char *image, unsigned target, unsigned mask, int report)
{
    for (size_t i = 0; i < BYTES; ++i) {
        unsigned char want = expected(i,target,mask);
        if (image[i] != want) {
            if (report) {
                unsigned red_pixels = 0, flipped_mismatches = 0;
                for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
                    size_t at = GUARD + ((size_t)y * W + x) * 4;
                    red_pixels += (unsigned)(memcmp(image + at, red, 4) == 0);
                    for (unsigned channel = 0; channel < 4; ++channel) {
                        size_t flip = GUARD + ((size_t)(H - 1 - y) * W + x) * 4 + channel;
                        flipped_mismatches += (unsigned)(image[at + channel] != expected(flip,target,mask));
                    }
                }
                fprintf(stderr, "FAIL image target=%u mask=%u byte=%zu got=%u expected=%u red_pixels=%u flipped_mismatches=%u\n",
                        target,mask,i,image[i],want,red_pixels,flipped_mismatches);
            }
            return 0;
        }
    }
    return 1;
}
static void cpu_oracle(void)
{
    unsigned counts[2] = {0}, overlap = 0, exclusive[2] = {0}, flipped = 0;
    int boundary = 0;
    for (unsigned t = 0; t < 2; ++t) for (unsigned v = 0; v < 3; ++v)
        for (unsigned axis = 0; axis < 2; ++axis)
            CHECK(256.0*((double)shapes[t].p[v][axis]+1.0) == (double)shapes[t].window2[v][axis]);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        int a = inside(0,x,y,&boundary), b = inside(1,x,y,&boundary);
        counts[0] += (unsigned)a; counts[1] += (unsigned)b;
        overlap += (unsigned)(a && b);
        exclusive[0] += (unsigned)(a && !b); exclusive[1] += (unsigned)(b && !a);
        flipped += (unsigned)(a != inside(0,x,H-1-y,&boundary));
    }
    CHECK(!boundary && counts[0] == 6400 && counts[1] == 12800);
    CHECK(overlap && exclusive[0] && exclusive[1] && flipped);
    unsigned char *p = malloc(BYTES); CHECK(p);
    for (unsigned t = 0; t < 2; ++t) for (unsigned mask = 0; mask < 4; ++mask) {
        for (size_t i = 0; i < BYTES; ++i) p[i] = expected(i,t,mask);
        CHECK(verify(p,t,mask,1));
        const size_t mutations[] = {0,GUARD-1,GUARD,GUARD+3,GUARD+57*W*4+40*4,BYTES-1};
        for (size_t j = 0; j < sizeof(mutations)/sizeof(mutations[0]); ++j) {
            p[mutations[j]] ^= 1;
            CHECK(!verify(p,t,mask,0));
            p[mutations[j]] ^= 1;
        }
        CHECK(!verify(p,1-t,mask,0));
    }
    free(p);
    printf("PASS CPU oracle triangle=%u,%u overlap=%u；未打开 bundle、未访问 GPU\n",
           counts[0],counts[1],overlap);
}

#ifndef CONTROLLED_ORACLE_ONLY
#include "../gld-integration/integration.h"
#include "../gld-integration/dispatch_exports.h"
#include "../gld-integration/tcl_exports.h"
#include "../gld-integration/context_exports.h"
#include "../gld-context/vgpu_context.h"
#include "../gld-texture/vgpu_texture.h"
#include "../gld-framebuffer/vgpu_framebuffer.h"
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned device_string_checks, detach_checks;
#define STRING_CHECK(x) do { ++device_string_checks; CHECK(x); } while (0)
#define DETACH_CHECK(x) do { ++detach_checks; CHECK(x); } while (0)
struct api {
    void *base;
    const char *(*device_string)(void *, uint32_t);
    const char *vendor, *renderer;
    void (*initialize)(const uint32_t *, const uint8_t *, uint32_t, uintptr_t, uintptr_t, uint32_t);
    void (*terminate)(void);
    int32_t (*device_create)(void **, uint32_t, void *);
    int32_t (*device_destroy)(void *);
    int32_t (*enable)(void *), (*publish)(void *);
    int32_t (*pixel_choose)(void **, const uint32_t *), (*pixel_destroy)(void *);
    int32_t (*share_create)(void *, void **, uintptr_t), (*share_destroy)(void *);
    int32_t (*context_create)(void *, void **, void *, void *, void *, void *);
    int32_t (*context_destroy)(void *);
    int32_t (*attach)(void *, uint32_t, void *, uint32_t, uint32_t *);
    int32_t (*texture_create)(void *, void **, void *, void *), (*texture_destroy)(void *, void *);
    int32_t (*fb_create)(void *, void **, void *, void *), (*fb_destroy)(void *, void *);
    int32_t (*fb_load)(void *, void *);
    int32_t (*pipeline_create)(void *, void **, void *, void *), (*pipeline_destroy)(void *, void *);
    void (*populate)(void *, void *);
    uint32_t (*update_draw)(void *, uint32_t), (*update_read)(void *, uint32_t);
    void (*clear)(void *, uint32_t, const void *), (*flush)(void *), (*finish)(void *);
    uint32_t (*error)(void *);
    int32_t (*read)(void *, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *, uint32_t, void *);
    int32_t (*bounded_read)(void *, uint64_t, int32_t, int32_t, int32_t, int32_t,
                            uint32_t, uint32_t, void *, size_t, uint32_t, void *);
    void (*status)(struct gi_status *);
};
static int symbol(void *lib, void *base, const char *name, void *out, size_t size)
{
    void *p = dlsym(lib,name); Dl_info info;
    if (!p || !dladdr(p,&info) || info.dli_fbase != base || info.dli_saddr != p ||
        !info.dli_sname || strcmp(info.dli_sname,name)) {
        fprintf(stderr,"BLOCKED 指定 bundle 缺少或错误来源的符号 %s\n",name); return 0;
    }
    CHECK(size == sizeof(p)); memcpy(out,&p,size); return 1;
}
static int load_api(void *lib, const char *path, struct api *a)
{
    Dl_info info; struct stat requested, loaded;
    void *anchor = dlsym(lib,"vgpuIntegrationStatus");
    if (!anchor || !dladdr(anchor,&info) || stat(path,&requested) || stat(info.dli_fname,&loaded) ||
        requested.st_dev != loaded.st_dev || requested.st_ino != loaded.st_ino) {
        fputs("BLOCKED 无法核实指定 bundle 映像身份\n",stderr); return 0;
    }
    a->base = info.dli_fbase;
#define LOAD(field, name) do { if (!symbol(lib,a->base,#name,&a->field,sizeof(a->field))) return 0; } while (0)
    LOAD(initialize,gldInitializeLibrary); LOAD(terminate,gldTerminateLibrary);
    LOAD(device_create,gldCreateDevice); LOAD(device_destroy,gldDestroyDevice);
    LOAD(device_string,gldGetDeviceString); ++device_string_checks;
    LOAD(enable,vgpuIntegrationEnableControlledDraw); LOAD(publish,vgpuIntegrationPublishCandidate);
    LOAD(pixel_choose,gldChoosePixelFormat); LOAD(pixel_destroy,gldDestroyPixelFormat);
    LOAD(share_create,gldCreateShareGroup); LOAD(share_destroy,gldDestroyShareGroup);
    LOAD(context_create,gldCreateContext); LOAD(context_destroy,gldDestroyContext);
    LOAD(attach,gldAttachDrawable); ++detach_checks;
    LOAD(texture_create,gldCreateTexture); LOAD(texture_destroy,gldDestroyTexture);
    LOAD(fb_create,gldCreateFramebuffer); LOAD(fb_destroy,gldDestroyFramebuffer); LOAD(fb_load,gldLoadFramebuffer);
    LOAD(pipeline_create,gldCreatePipelineProgram); LOAD(pipeline_destroy,gldDestroyPipelineProgram);
    LOAD(populate,gldPopulateContextDispatch); LOAD(update_draw,gldUpdateDrawFramebuffer);
    LOAD(update_read,gldUpdateReadFramebuffer); LOAD(clear,gldClearFramebufferData);
    LOAD(flush,gldFlushContext); LOAD(finish,gldWaitForContext); LOAD(error,gldGetError);
    LOAD(read,gldReadFramebufferData); LOAD(bounded_read,vgpuIntegrationRead); LOAD(status,vgpuIntegrationStatus);
#undef LOAD
    printf("BUNDLE %s；全部导出归属同一映像\n",info.dli_fname); return 1;
}
static void device_strings(struct api *a, void *device, int valid, const char *phase)
{
    struct gi_status before = {0}, after = {0};
    a->status(&before);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        const char *vendor = a->device_string(device,UINT32_C(0x1f00));
        const char *renderer = a->device_string(device,UINT32_C(0x1f01));
        if (valid) {
            STRING_CHECK(vendor && !strcmp(vendor,"vGPU-macOS"));
            STRING_CHECK(renderer && !strcmp(renderer,"vGPU restricted SVGA3D"));
            if (!a->vendor) { a->vendor = vendor; a->renderer = renderer; }
            STRING_CHECK(vendor == a->vendor && renderer == a->renderer);
        } else {
            /* 无效 token 只透传到待测 API；探针不解引用 token 或返回指针。 */
            STRING_CHECK(vendor == NULL && renderer == NULL);
        }
        const uint32_t rejected[] = {0, UINT32_C(0x1f02), UINT32_C(0x1f03),
                                     UINT32_C(0x1f04), UINT32_MAX};
        for (size_t i = 0; i < sizeof(rejected)/sizeof(rejected[0]); ++i)
            STRING_CHECK(a->device_string(device,rejected[i]) == NULL);
    }
    a->status(&after);
    STRING_CHECK(!memcmp(&before,&after,sizeof(before)));
    printf("PASS device-string phase=%s valid=%d 重复值/指针稳定、拒绝selector、状态不变\n",phase,valid);
}
static void invalid_device_strings(struct api *a)
{
    unsigned char forged = 0;
    device_strings(a,NULL,0,"null");
    device_strings(a,(void *)(uintptr_t)1,0,"forged-address");
    device_strings(a,&forged,0,"foreign-object");
}
static void put16(void *p, size_t offset, uint16_t v) { memcpy((unsigned char *)p+offset,&v,sizeof(v)); }
static void put32(void *p, size_t offset, uint32_t v) { memcpy((unsigned char *)p+offset,&v,sizeof(v)); }
static void putf(void *p, size_t offset, float v) { memcpy((unsigned char *)p+offset,&v,sizeof(v)); }
static void token(void *p, size_t offset, void *v) { memcpy((unsigned char *)p+offset,&v,sizeof(v)); }
struct context {
    _Alignas(8) unsigned char a[0x268], h[VGC_H_LENGTH], b[VGC_B_LENGTH];
    void *handle;
    gi_dispatch_r20_fn begin;
    gi_dispatch_r21_fn end;
};
struct target {
    _Alignas(8) unsigned char td[VGT_TD_RB_LENGTH], tu[VGT_TU_LENGTH], fq[VGF_FQ_LENGTH], fd[VGF_FD_LENGTH];
    void *texture, *fb;
};
static void valid_state(struct context *c)
{
    for (size_t i = 0; i < 16; ++i) {
        putf(c->h,0x24e0+i*4,i%5 == 0 ? 1.0f : 0.0f);
        putf(c->h,0x2520+i*4,i%5 == 0 ? 1.0f : 0.0f);
    }
    putf(c->h,0x1830,1); putf(c->h,0x183c,1);
    putf(c->h,0x1b88,256); putf(c->h,0x1b8c,256);
    const double far_depth = 1; memcpy(c->h+0x1b68,&far_depth,sizeof(far_depth));
    put16(c->h,0x3d64,0x1b02); put16(c->h,0x3d66,0x1b02);
    memset(c->h+0x3ca8,1,4); put16(c->h,0x3cae,1);
    put16(c->h,0x497a,UINT16_MAX); put32(c->h,0x4ee4,4);
}
static void describe_target(struct target *t)
{
    put16(t->td,0,0x2002); put16(t->td,0x68,0x8058);
    put16(t->td,0x6a,256); put16(t->td,0x6c,256); put16(t->td,0x6e,1);
    t->td[0x79] = t->td[0x7b] = 1;
    put16(t->td,0xb4,256); put16(t->td,0xb6,256); put16(t->td,0xb8,1); t->td[0xbb] = 0x20;
    put16(t->td,0xbc,256); put16(t->td,0xbe,256); put16(t->td,0xc0,0x1908); put16(t->td,0xc2,0x1401);
    put16(t->tu,8,1); t->tu[0x14] = 1;
    put16(t->fq,0,VGF_RENDERBUFFER_TYPE); put16(t->fq,0xb2,256); put16(t->fq,0xb4,256); put32(t->fq,0xc0,1);
}
static struct gi_status status(struct api *a, int32_t want)
{
    struct gi_status s; a->status(&s);
    if (s.render_status != want) fprintf(stderr,"render=%08" PRIx32 " expected=%08" PRIx32 "\n",
                                        (uint32_t)s.render_status,(uint32_t)want);
    CHECK(s.render_status == want && !s.context_supported); return s;
}
static void bind_target(struct api *a, struct context *c, struct target *t)
{
    token(c->b,0x4a0,t->fb); token(c->b,0x4a8,t->fb);
    CHECK(a->update_draw(c->handle,VGC_DRAW_DIRTY) == 1); (void)status(a,0);
    CHECK(a->update_read(c->handle,VGC_READ_DIRTY) == 1); (void)status(a,0);
    CHECK(!a->error(c->handle));
}
static void dispatch(struct api *a, struct context *c)
{
    unsigned char storage[GI_DISPATCH_TABLE_BYTES+32], second[GI_DISPATCH_TABLE_BYTES];
    memset(storage,0xa7,sizeof(storage)); a->populate(c->handle,storage+16);
    a->populate(c->handle,second); CHECK(!memcmp(storage+16,second,sizeof(second)));
    for (size_t i = 0; i < 16; ++i) CHECK(storage[i] == 0xa7 && storage[sizeof(storage)-1-i] == 0xa7);
    for (size_t i = 0; i < GI_DISPATCH_SLOT_COUNT; ++i) {
        void *p; Dl_info info; memcpy(&p,second+i*sizeof(p),sizeof(p));
        if (i >= 13 && i <= 16) CHECK(!p);
        else CHECK(p && dladdr(p,&info) && info.dli_fbase == a->base);
    }
    memcpy(&c->begin,second+20*sizeof(void *),sizeof(c->begin));
    memcpy(&c->end,second+21*sizeof(void *),sizeof(c->end));
    CHECK(c->begin && c->end);
}
static uint64_t draw(struct api *a, struct context *c, unsigned shape, int reject, uint64_t previous)
{
    struct gi_status before; a->status(&before);
    int32_t capacity = 0;
    gi_tcl_vertex *sink = c->begin(c->handle,4,&capacity);
    CHECK(sink && capacity == 48 && (uintptr_t)sink%16 == 0);
    gi_tcl_vertex vertices[3];
    for (size_t i = 0; i < 3; ++i) {
        memcpy(vertices[i].position,shapes[shape].p[i],sizeof(vertices[i].position));
        vertices[i].rgba[0] = reject && i == 1 ? 0.0f : 1.0f;
        vertices[i].rgba[1] = reject && i == 1 ? 1.0f : 0.0f;
        vertices[i].rgba[2] = 0; vertices[i].rgba[3] = 1;
    }
    memcpy(sink,vertices,sizeof(vertices)); c->end(c->handle,0x8002,4,3);
    struct gi_status after; a->status(&after);
    if (reject) {
        CHECK(a->error(c->handle) == 0x502 && !a->error(c->handle));
        CHECK(after.render_generation == before.render_generation);
    } else {
        CHECK(!a->error(c->handle)); (void)status(a,0);
        CHECK(previous != UINT64_MAX && after.render_generation == previous + 1);
        a->finish(c->handle); CHECK(status(a,0).render_generation == after.render_generation);
        a->flush(c->handle); CHECK(status(a,0).render_generation == after.render_generation);
        CHECK(!a->error(c->handle));
    }
    return after.render_generation;
}
static void image(struct api *a, struct context *c, unsigned target, unsigned mask,
                  uint64_t generation, unsigned char *out)
{
    memset(out,0xa7,BYTES);
    CHECK(a->read(c->handle,0,0,W,H,0x1908,0x1401,out+GUARD,0,NULL) == 0);
    (void)status(a,0); CHECK(!a->error(c->handle)); CHECK(verify(out,target,mask,1));
    /* 以先前成功代次再读，拒绝后若曾提交并改变 image generation 必须失败。 */
    memset(out,0xa7,BYTES);
    CHECK(a->bounded_read(c->handle,generation,0,0,W,H,0x1908,0x1401,out+GUARD,
                          IMAGE_BYTES,0,NULL) == 0);
    CHECK(verify(out,target,mask,1));
}
static void detach_case(struct api *a, struct context *c, void *handle, uint32_t kind,
                        void *drawable, uint32_t options, int32_t want)
{
    struct gi_status before = {0}, after = {0};
    struct context saved = {0};
    if (c) saved = *c;
    uint32_t size[4] = {UINT32_C(0x12345678), UINT32_C(0x89abcdef),
                        UINT32_C(0xfedcba98), UINT32_C(0x76543210)};
    uint32_t expected_size[4]; memcpy(expected_size,size,sizeof(size));
    if (want == 1) expected_size[1] = expected_size[2] = 0;
    a->status(&before);
    DETACH_CHECK(a->attach(handle,kind,drawable,options,size+1) == want);
    DETACH_CHECK(!memcmp(size,expected_size,sizeof(size)));
    DETACH_CHECK(a->attach(handle,kind,drawable,options,NULL) == want);
    a->status(&after);
    DETACH_CHECK(!memcmp(&before,&after,sizeof(before)));
    if (c) DETACH_CHECK(!memcmp(c->a,saved.a,sizeof(c->a)) &&
                       !memcmp(c->h,saved.h,sizeof(c->h)) &&
                       !memcmp(c->b,saved.b,sizeof(c->b)));
}
static void detach_matrix(struct api *a, struct context *c, const char *phase)
{
    unsigned char foreign = 0;
    void *invalid[] = {NULL, (void *)(uintptr_t)1, &foreign};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        detach_case(a,NULL,invalid[i],0,NULL,0,0x2714);
        detach_case(a,NULL,invalid[i],UINT32_MAX,&foreign,UINT32_MAX,0x2714);
    }
    detach_case(a,c,c->handle,0,NULL,0,1);
    detach_case(a,c,c->handle,0,NULL,1,0x2718);
    detach_case(a,c,c->handle,0,NULL,UINT32_MAX,0x2718);
    detach_case(a,c,c->handle,1,NULL,0,0x2715);
    detach_case(a,c,c->handle,UINT32_MAX,NULL,0,0x2715);
    detach_case(a,c,c->handle,0,&foreign,0,0x2715);
    detach_case(a,c,c->handle,1,&foreign,0,0x2715);
    detach_case(a,c,c->handle,1,&foreign,1,0x2718);
    printf("PASS detach phase=%s size2/canary、NULL输出、2714/2718/2715拒绝、状态及A/H/B保持\n",phase);
}
static void detach_image(struct api *a, struct context *c, unsigned target, unsigned mask,
                         uint64_t generation, unsigned char *out, const char *phase)
{
    unsigned char saved[BYTES]; memcpy(saved,out,sizeof(saved));
    detach_matrix(a,c,phase);
    unsigned before = checks;
    image(a,c,target,mask,generation,out);
    detach_checks += checks - before;
    DETACH_CHECK(!memcmp(saved,out,sizeof(saved)));
    printf("PASS detach-image phase=%s target=%u mask=%u generation=%" PRIu64 " 全图及边界保持\n",
           phase,target,mask,generation);
}
static void cleanup(struct api *a, void *device, void *pixel, void *share,
                    struct context *c, struct target t[2], void *pipeline[2])
{
    if (c->handle) {
        uintptr_t destroyed_context = (uintptr_t)c->handle;
        CHECK(a->context_destroy(c->handle) == 0);
        detach_case(a,NULL,(void *)destroyed_context,0,NULL,0,0x2714);
        puts("PASS detach phase=destroyed-context 拒绝且输出保持");
    }
    for (size_t i = 0; i < 2; ++i) {
        if (t[i].fb) CHECK(a->fb_destroy(share,t[i].fb) == 0);
        if (t[i].texture) CHECK(a->texture_destroy(share,t[i].texture) == 0);
        if (pipeline[i]) CHECK(a->pipeline_destroy(share,pipeline[i]) == 0);
    }
    if (share) CHECK(a->share_destroy(share) == 0);
    if (pixel) CHECK(a->pixel_destroy(pixel) == 0);
    uintptr_t destroyed_token = (uintptr_t)device;
    if (device) {
        CHECK(a->device_destroy(device) == 0);
        device_strings(a,(void *)destroyed_token,0,"destroyed-before-terminate");
    }
    a->terminate(); struct gi_status s; a->status(&s);
    device_strings(a,(void *)destroyed_token,0,"after-terminate");
    invalid_device_strings(a);
    CHECK(!s.devices && !s.pixel_lists && !s.share_groups && !s.render_shares && !s.render_contexts);
    CHECK(!s.pending_cleanup && !s.native_registered && !s.lifecycle_status);
    CHECK(!s.backend.connections && !s.backend.service_refs && !s.backend.devices);
    CHECK(s.backend.opens == s.backend.closes && s.backend.retains == s.backend.releases);
}
static int gpu_round(struct api *a, io_service_t service)
{
    uint32_t services[32] = {0}; uint8_t flags[32] = {0}; services[0] = service;
    a->initialize(services,flags,1,0,0,0);
    void *device = NULL, *pixel = NULL, *share = NULL, *pipeline[2] = {0};
    struct context c = {0}; struct target t[2] = {0};
    unsigned char config[0x190] = {0}, pd[2][4] = {{0x20,0x86,0,1},{4,0x88,4,1}}, pu[2][4] = {{0}};
    unsigned char *out = NULL, *saved = NULL;
    const uint32_t attrs[] = {8,32,11,8,0};
    int result = 3;
#define NEED(x) do { if (!(x)) { struct gi_status s; a->status(&s); \
    fprintf(stderr,"BLOCKED 前提 line=%d %s lifecycle=%08" PRIx32 " render=%08" PRIx32 "；非PASS\n", \
            __LINE__,#x,(uint32_t)s.lifecycle_status,(uint32_t)s.render_status); goto done; } } while (0)
    invalid_device_strings(a);
    NEED(a->device_create(&device,1,config) == 0 && device);
    device_strings(a,device,1,"created");
    invalid_device_strings(a);
    NEED(a->publish(device) == 0); NEED(a->enable(device) == 0);
    device_strings(a,device,1,"enabled");
    NEED(a->pixel_choose(&pixel,attrs) == 0 && pixel);
    NEED(a->share_create(device,&share,4) == 0 && share);
    valid_state(&c);
    NEED(a->context_create(share,&c.handle,pixel,c.a,c.h,c.b) == 0 && c.handle);
    CHECK(!a->error(c.handle));
    detach_matrix(a,&c,"created-unbound");
    for (size_t i = 0; i < 2; ++i) {
        NEED(a->pipeline_create(share,&pipeline[i],pd[i],pu[i]) == 0 && pipeline[i]);
        describe_target(&t[i]);
        NEED(a->texture_create(share,&t[i].texture,t[i].td,t[i].tu) == 0 && t[i].texture);
        token(t[i].fd,0,t[i].texture); t[i].fd[0x50] = 1;
        NEED(a->fb_create(share,&t[i].fb,t[i].fq,t[i].fd) == 0 && t[i].fb);
        NEED(a->fb_load(share,t[i].fb) == 1);
    }
    token(c.b,0x400,pipeline[0]); token(c.b,0x420,pipeline[1]); dispatch(a,&c);
    out = malloc(BYTES); saved = malloc(BYTES); CHECK(out && saved);
    uint64_t generation[2];
    for (unsigned i = 0; i < 2; ++i) {
        bind_target(a,&c,&t[i]);
        unsigned char values[28] = {0}; float rgba[4];
        for (unsigned channel = 0; channel < 4; ++channel) rgba[channel] = (float)backgrounds[i][channel]/255.0f;
        memcpy(values,rgba,sizeof(rgba)); a->clear(c.handle,1,values);
        generation[i] = status(a,0).render_generation; CHECK(generation[i] && !a->error(c.handle));
        image(a,&c,i,0,generation[i],out);
        detach_image(a,&c,i,0,generation[i],out,"before-draw");
    }
    bind_target(a,&c,&t[0]); generation[0] = draw(a,&c,0,0,generation[0]);
    image(a,&c,0,1,generation[0],out); memcpy(saved,out,BYTES);
    detach_image(a,&c,0,1,generation[0],out,"after-draw");
    CHECK(draw(a,&c,1,1,generation[0]) == generation[0]);
    a->finish(c.handle); CHECK(status(a,kIOReturnNotReady).render_generation == generation[0]);
    detach_matrix(a,&c,"pending-first-error");
    CHECK(a->error(c.handle) == 0x502 && !a->error(c.handle));
    a->flush(c.handle); CHECK(status(a,kIOReturnNotReady).render_generation == generation[0]);
    CHECK(a->error(c.handle) == 0x502 && !a->error(c.handle));
    image(a,&c,0,1,generation[0],out); CHECK(!memcmp(saved,out,BYTES));
    generation[0] = draw(a,&c,1,0,generation[0]); image(a,&c,0,3,generation[0],out);
    bind_target(a,&c,&t[1]); image(a,&c,1,0,generation[1],out);
    generation[1] = draw(a,&c,1,0,generation[1]); image(a,&c,1,2,generation[1],out);
    detach_image(a,&c,1,2,generation[1],out,"after-draw");
    bind_target(a,&c,&t[0]); image(a,&c,0,3,generation[0],out);
    detach_image(a,&c,0,3,generation[0],out,"before-release");
    device_strings(a,device,1,"after-draw");
    puts("PASS GPU typed R20/R21、双目标、累积、红绿红不变、GetError/Finish/Flush拒绝及健康恢复");
    result = 0;
done:
    free(out); free(saved); cleanup(a,device,pixel,share,&c,t,pipeline); return result;
#undef NEED
}
static void empty(struct api *a)
{
    a->initialize(NULL,NULL,0,0,0,0);
    invalid_device_strings(a);
    unsigned char storage[GI_DISPATCH_TABLE_BYTES+32]; memset(storage,0xa7,sizeof(storage));
    a->populate(NULL,storage+16);
    for (size_t i = 0; i < sizeof(storage); ++i) CHECK(storage[i] == 0xa7);
    struct gi_status rejected; a->status(&rejected); CHECK(rejected.dispatch_status != 0);
    CHECK(a->enable(NULL) != 0 && a->error(NULL) == 0);
    a->finish(NULL); (void)status(a,kIOReturnBadArgument);
    a->flush(NULL); (void)status(a,kIOReturnBadArgument);
    a->terminate(); struct gi_status s; a->status(&s);
    CHECK(!s.devices && !s.render_contexts && !s.share_groups && !s.backend.opens && !s.backend.connections);
    CHECK(!s.backend.retains && !s.backend.service_refs);
    puts("PASS CPU empty：空服务表、空context dispatch、来源核验；未访问 GPU");
}
#endif
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1],"--cpu-only")) { cpu_oracle(); return 0; }
#ifndef CONTROLLED_ORACLE_ONLY
    if (argc != 4 || (strcmp(argv[1],"--gpu") && strcmp(argv[1],"--empty")) ||
        strcmp(argv[2],"--bundle") || argv[3][0] != '/') {
        fprintf(stderr,"用法：%s --cpu-only | (--empty|--gpu) --bundle /绝对路径/映像\n",argv[0]); return 2;
    }
    int gpu = !strcmp(argv[1],"--gpu");
    if (gpu && geteuid() != 0) { fputs("BLOCKED GPU需主代理独占串行提权执行\n",stderr); return 3; }
    void *lib = dlopen(argv[3],RTLD_NOW|RTLD_LOCAL|RTLD_FIRST);
    if (!lib) { fprintf(stderr,"BLOCKED dlopen: %s\n",dlerror()); return 3; }
    struct api a = {0}; int result = 3;
    if (load_api(lib,argv[3],&a)) {
        if (!gpu) { empty(&a); result = 0; }
        else {
            io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("vgpuAccelerator"));
            if (!service) fputs("BLOCKED 缺少 vgpuAccelerator 服务\n",stderr);
            else {
                for (unsigned round = 0; round < 2; ++round) {
                    result = gpu_round(&a,service); if (result) break;
                    printf("PASS GPU round=%u 完整销毁释放；下一轮重新租用\n",round+1);
                }
                CHECK(IOObjectRelease(service) == 0);
            }
        }
    }
    CHECK(dlclose(lib) == 0);
    if (gpu && !result && (checks - device_string_checks - detach_checks != 379 ||
                           device_string_checks != 569 || detach_checks != 975)) {
        fputs("FAIL 原有379项、身份569项或detach975项检查计数不符\n",stderr); result = 1;
    }
    printf("%s controlled-draw checks=%u mode=%s baseline_checks=%u device_string_checks=%u detach_checks=%u；不代表公开CGL/JIT验收\n",
           result ? "BLOCKED" : "PASS",checks,argv[1],checks-device_string_checks-detach_checks,
           device_string_checks,detach_checks);
    return result;
#else
    fputs("用法：--cpu-only；本构建无GPU入口\n",stderr); return 2;
#endif
}
