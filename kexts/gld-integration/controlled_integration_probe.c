#include "integration.h"
#include "dispatch_exports.h"
#include "tcl_exports.h"
#include <OpenGL/CGLTypes.h>

/* 仅复用已有 CPU surface 模型，不改旧探针，不链接真实 GPU surface。 */
int controlled_render_baseline(void);
int32_t controlled_device_status(void *, struct gld_device_status *);
#define main controlled_render_baseline
#define vgpuGLDDeviceGetStatus controlled_device_status
#include "render_exports_probe.c"
#undef vgpuGLDDeviceGetStatus
#undef main

/* 只为此探针加入状态故障；复用的 render fixture 与 metadata 测试不修改。 */
static unsigned device_status_fault, device_status_calls;
int32_t vgpuGLDDeviceGetStatus(void *token, struct gld_device_status *out)
{
    ++device_status_calls;
    int32_t result = controlled_device_status(token, out);
    if (result) return result;
    switch (device_status_fault) {
    case 1: return kIOReturnNotReady;
    case 2: out->connected = 0; break;
    case 3: out->liveness_status = kIOReturnSuccess; break;
    case 4: out->registry_id = 0; break;
    default: break;
    }
    return 0;
}

static void expect_device_strings(void *token, const char *vendor, const char *renderer)
{
    struct gi_status before = {0}, after = {0};
    const unsigned counts[] = {owners, images, clears, reads, triangles, device};
    vgpuIntegrationStatus(&before);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(gldGetDeviceString(token, UINT32_C(0x1f00)) == vendor);
        CHECK(gldGetDeviceString(token, UINT32_C(0x1f01)) == renderer);
        const uint32_t rejected[] = {0, UINT32_C(0x1f02), UINT32_C(0x1f03),
            UINT32_C(0x1f04), UINT32_MAX};
        for (size_t j = 0; j < sizeof(rejected) / sizeof(rejected[0]); ++j)
            CHECK(gldGetDeviceString(token, rejected[j]) == NULL);
    }
    vgpuIntegrationStatus(&after);
    CHECK(!memcmp(&before, &after, sizeof(before)));
    const unsigned current[] = {owners, images, clears, reads, triangles, device};
    CHECK(!memcmp(counts, current, sizeof(counts)));
}

static void expect_attach(void *context, uint32_t kind, void *drawable,
    uint32_t options, int32_t expected, const unsigned char *a,
    const unsigned char *h, const unsigned char *b)
{
    struct gi_status before = {0}, after = {0};
    unsigned char saved_a[0x268], saved_h[VGC_H_LENGTH], saved_b[VGC_B_LENGTH];
    vgc_state native_before = {0}, native_after = {0};
    vgc_handle native = 0;
    void *native_a, *native_h, *native_b;
    if (a) {
        memcpy(saved_a, a, sizeof(saved_a)); memcpy(saved_h, h, sizeof(saved_h));
        memcpy(saved_b, b, sizeof(saved_b));
        CHECK(gi_context_native(context, &native, &native_a, &native_h, &native_b) == GI_CONTEXT_OK);
        OK(vgc_context_state(native, &native_before));
    }
    const unsigned counts[] = {owners, images, clears, reads, triangles, device, device_status_calls};
    const char *diagnostic_before = gi_context_diagnostic();
    vgpuIntegrationStatus(&before);
    uint32_t output[4] = {UINT32_C(0x12345678), UINT32_C(0x89abcdef),
        UINT32_C(0xfedcba98), UINT32_C(0x76543210)};
    uint32_t saved_output[4]; memcpy(saved_output, output, sizeof(output));
    CHECK(gldAttachDrawable(context, kind, drawable, options, &output[1]) == expected);
    if (expected == 1) saved_output[1] = saved_output[2] = 0;
    CHECK(!memcmp(output, saved_output, sizeof(output)));
    CHECK(gldAttachDrawable(context, kind, drawable, options, NULL) == expected);
    if (expected != 1)
        CHECK(gldAttachDrawable(context, kind, drawable, options,
            (uint32_t *)(uintptr_t)1) == expected);
    vgpuIntegrationStatus(&after);
    CHECK(!memcmp(&before, &after, sizeof(before)));
    CHECK(gi_context_diagnostic() == diagnostic_before);
    const unsigned current[] = {owners, images, clears, reads, triangles, device, device_status_calls};
    CHECK(!memcmp(counts, current, sizeof(counts)));
    if (a) {
        CHECK(!memcmp(a, saved_a, sizeof(saved_a)) && !memcmp(h, saved_h, sizeof(saved_h)) &&
            !memcmp(b, saved_b, sizeof(saved_b)));
        OK(vgc_context_state(native, &native_after));
        CHECK(!memcmp(&native_before, &native_after, sizeof(native_before)));
    }
}

static void expect_attach_cases(void *context, int valid, const unsigned char *a,
    const unsigned char *h, const unsigned char *b)
{
    const uint32_t kinds[] = {0, 1, 2, 3, 4, UINT32_C(0x100), UINT32_MAX};
    const uint32_t options[] = {0, 1, UINT32_C(0x80000000), UINT32_MAX};
    unsigned char payload[32]; memset(payload, 0xa5, sizeof(payload));
    void *drawables[] = {NULL, (void *)(uintptr_t)1, payload};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i)
        for (size_t j = 0; j < sizeof(drawables) / sizeof(drawables[0]); ++j)
            for (size_t k = 0; k < sizeof(options) / sizeof(options[0]); ++k) {
                int32_t expected = !valid ? INT32_C(0x2714) : options[k] ? INT32_C(0x2718) :
                    kinds[i] || drawables[j] ? INT32_C(0x2715) : 1;
                expect_attach(context, kinds[i], drawables[j], options[k], expected, a, h, b);
            }
    for (size_t i = 0; i < sizeof(payload); ++i) CHECK(payload[i] == 0xa5);
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
static void set_float(void *p, size_t offset, float value)
{ memcpy((unsigned char *)p + offset, &value, sizeof(value)); }
static void valid_state(unsigned char *h)
{
    memset(h, 0, VGC_H_LENGTH);
    for (size_t i = 0; i < 16; ++i) {
        set_float(h, 0x24e0 + i * 4, i % 5 == 0 ? 1.0f : 0.0f);
        set_float(h, 0x2520 + i * 4, i % 5 == 0 ? 1.0f : 0.0f);
    }
    set_float(h, 0x1830, 1); set_float(h, 0x183c, 1);
    set_float(h, 0x1b88, 256); set_float(h, 0x1b8c, 256);
    const double far_depth = 1;
    memcpy(h + 0x1b68, &far_depth, sizeof(far_depth));
    put16(h, 0x3d64, 0x1b02); put16(h, 0x3d66, 0x1b02);
    memset(h + 0x3ca8, 1, 4); put16(h, 0x3cae, 1);
    put16(h, 0x497a, UINT16_MAX); put32(h, 0x4ee4, 4);
}
static gi_dispatch_r20_fn begin_fn;
static gi_dispatch_r21_fn end_fn;
static void *begin_triangle(void *context)
{
    int32_t count = 0;
    gi_tcl_vertex *sink = begin_fn(context, 4, &count);
    CHECK(sink && count == 48 && (uintptr_t)sink % 16 == 0);
    const gi_tcl_vertex vertices[3] = {
        {{-0.5f,-0.5f,-1,1},{1,0,0,1}},
        {{0.5f,-0.5f,0,1},{1,0,0,1}},
        {{0,0.5f,1,1},{1,0,0,1}}
    };
    memcpy(sink, vertices, sizeof(vertices));
    return sink;
}
static void end_triangle(void *context) { end_fn(context, 0x8002, 4, 3); }
static void expect_render(int32_t result)
{ struct gi_status status; vgpuIntegrationStatus(&status); CHECK(status.render_status == result); }
int main(void)
{
    void *dev = NULL, *share = NULL, *pixel = NULL, *context = NULL, *second = NULL;
    void *texture = NULL, *fb = NULL, *pipeline[2] = {0};
    _Alignas(8) unsigned char a[0x268], h[VGC_H_LENGTH], b[VGC_B_LENGTH] = {0};
    unsigned char before_a[sizeof(a)], table[GI_DISPATCH_TABLE_BYTES], table2[sizeof(table)];
    unsigned char q[32], td[VGT_TD_RB_LENGTH], tu[VGT_TU_LENGTH];
    unsigned char fq[VGF_FQ_LENGTH] = {0}, fd[VGF_FD_LENGTH] = {0};
    unsigned char pd[2][4] = {{0x20,0x86,0,1},{4,0x88,4,1}}, pu[2][4] = {{0}};
    unsigned char values[28] = {0};
    const uint32_t tokens[] = {8,32,11,8,0};
    struct gi_status status;
    gldInitializeLibrary(NULL, NULL, 0, 0, 0, 0);
    expect_attach_cases(NULL, 0, NULL, NULL, NULL);
    expect_attach_cases((void *)(uintptr_t)1, 0, NULL, NULL, NULL);
    expect_attach_cases(a, 0, NULL, NULL, NULL);
    unsigned calls = device_status_calls;
    expect_device_strings(NULL, NULL, NULL);
    expect_device_strings((void *)(uintptr_t)1, NULL, NULL);
    expect_device_strings(&device, NULL, NULL);
    CHECK(device_status_calls == calls);
    OK(gldCreateDevice(&dev, 1, NULL));
    const char *vendor = gldGetDeviceString(dev, UINT32_C(0x1f00));
    const char *renderer_string = gldGetDeviceString(dev, UINT32_C(0x1f01));
    CHECK(vendor && !strcmp(vendor, "vGPU-macOS"));
    CHECK(renderer_string && !strcmp(renderer_string, "vGPU restricted SVGA3D"));
    expect_device_strings(dev, vendor, renderer_string);
    calls = device_status_calls;
    expect_device_strings(NULL, NULL, NULL);
    expect_device_strings((void *)(uintptr_t)1, NULL, NULL);
    CHECK(device_status_calls == calls);
    for (device_status_fault = 1; device_status_fault <= 4; ++device_status_fault) {
        calls = device_status_calls;
        expect_device_strings(dev, NULL, NULL);
        CHECK(device_status_calls > calls);
    }
    device_status_fault = 0;
    expect_device_strings(dev, vendor, renderer_string);
    vgpuIntegrationStatus(&status);
#if defined(GI_CONTROLLED_PUBLIC_EXPERIMENT)
    CHECK(status.metadata_enabled && status.candidate_count == 1);
#else
    CHECK(!status.metadata_enabled && !status.candidate_count);
    OK(vgpuIntegrationPublishCandidate(dev));
    OK(vgpuIntegrationEnableControlledDraw(dev));
#endif
    const uint32_t expected_flags =
#if defined(GI_CONTROLLED_PUBLIC_EXPERIMENT)
        UINT32_C(0x104);
#else
        UINT32_C(4);
#endif
    CHECK(cap.flags == expected_flags);
    unsigned char renderer[0x88];
    uint32_t observed_flags;
    OK(gldPopulateRendererInfo(dev, renderer));
    memcpy(&observed_flags, renderer + 0xc, sizeof(observed_flags));
    CHECK(observed_flags == expected_flags);
    /* Exercise normalized OFFSCREEN through the integration wrapper as well as
     * the pixel unit tests; this does not bypass public CGL SDK policy. */
    const uint32_t offscreen_tokens[] = {53,8,32,11,8,0};
    OK(gldChoosePixelFormat(&pixel, offscreen_tokens)); CHECK(pixel != NULL);
    memcpy(&observed_flags, (unsigned char *)pixel + 0xc, sizeof(observed_flags));
    CHECK(observed_flags == expected_flags);
    OK(gldDestroyPixelFormat(pixel)); pixel = NULL;
    OK(gldChoosePixelFormat(&pixel, tokens)); CHECK(pixel != NULL);
    memcpy(&observed_flags, (unsigned char *)pixel + 0xc, sizeof(observed_flags));
    CHECK(observed_flags == expected_flags);
    OK(gldCreateShareGroup(dev, &share, 4));
    CHECK(vgpuIntegrationEnableControlledDraw(dev) == kIOReturnBusy);
    memset(a, 0xa7, sizeof(a)); valid_state(h);
    put32(a, 0x230, 0x0800ffff); a[0x250] = a[0x254] = 0x8f;
    const uint64_t pagesize = 4096;
    memcpy(a + 0x258, &pagesize, sizeof(pagesize)); memcpy(before_a, a, sizeof(a));
    unsigned char bad_pixel[0x38]; memcpy(bad_pixel, pixel, sizeof(bad_pixel)); bad_pixel[0x1c] = 0x80;
    CHECK(gldCreateContext(share, &context, bad_pixel, a, h, b) == kCGLBadPixelFormat && !context);
    CHECK(!memcmp(a, before_a, sizeof(a)));
    vgpuIntegrationStatus(&status); CHECK(!status.render_contexts && !status.pending_cleanup);
    /* 默认必须拒绝 0x104；候选必须拒绝 0x4、缺 OFFSCREEN 或任何额外位。 */
    const uint32_t rejected_flags[] = {expected_flags ^ UINT32_C(0x100),
        UINT32_C(0x100), expected_flags | UINT32_C(0x400),
        expected_flags | UINT32_C(0x20000), expected_flags | UINT32_C(0x40000),
        expected_flags | UINT32_C(0x80000000)};
    for (size_t i = 0; i < sizeof(rejected_flags) / sizeof(rejected_flags[0]); ++i) {
        memcpy(bad_pixel, pixel, sizeof(bad_pixel));
        put32(bad_pixel, 0xc, rejected_flags[i]);
        CHECK(gldCreateContext(share, &context, bad_pixel, a, h, b) == kCGLBadPixelFormat && !context);
        CHECK(!memcmp(a, before_a, sizeof(a)));
        vgpuIntegrationStatus(&status); CHECK(!status.render_contexts && !status.pending_cleanup);
    }
    OK(gldCreateContext(share, &context, pixel, a, h, b));
    expect_attach_cases(context, 1, a, h, b);
    expect_attach_cases(dev, 0, NULL, NULL, NULL);
    expect_attach_cases(share, 0, NULL, NULL, NULL);
    CHECK(gldGetError(context) == 0);
    CHECK(gldCreateContext(share, &second, pixel, a, h, b) == kCGLBadMatch && !second);
    CHECK(a[0x15] == 1 && a[0x65] == 1 && a[0x6f] == 0xa7);
    CHECK(!memcmp(a + 0x258, before_a + 0x258, 16));
    CHECK(!memcmp(a + 0x218, before_a + 0x218, 0x18));
    const uint32_t groups[8] = {0x10,0x7fef,0,0,0,0,0,0};
    const unsigned char inputs[4] = {1,2,4,0xf8};
    CHECK(!memcmp(a + 0x230, groups, sizeof(groups)));
    CHECK(!memcmp(a + 0x250, inputs, 4) && !memcmp(a + 0x254, inputs, 4));
    for (size_t offset = 0x3c; offset <= 0x48; offset += 4) {
        float point; memcpy(&point, a + offset, 4); CHECK(point == 1);
    }
    void *descriptor, *alternate;
    memcpy(&descriptor, a + 0x208, sizeof(descriptor)); memcpy(&alternate, a + 0x210, sizeof(alternate));
    const unsigned char expected_descriptor[12] = {2,0,8,0,0,0x18,0,0,4,0x98,0,0};
    CHECK(descriptor && !alternate && !memcmp(descriptor, expected_descriptor, 12));
    gldPopulateContextDispatch(context, table); gldPopulateContextDispatch(context, table2);
    CHECK(!memcmp(table, table2, sizeof(table)));
    memset(q, 0x6b, sizeof(q)); CHECK(gldUpdateDispatch(context, table2, q) == 5);
    CHECK(!memcmp(table, table2, sizeof(table)));
    for (size_t i = 0; i < sizeof(q); ++i) CHECK(q[i] == 0x6b);
    memcpy(&begin_fn, table + 20 * 8, sizeof(begin_fn)); memcpy(&end_fn, table + 21 * 8, sizeof(end_fn));
    CHECK(begin_fn && end_fn);
    CHECK(gldUpdateDrawFramebuffer(context, VGC_DRAW_DIRTY | UINT32_C(0x043c0478)) == 1);
    expect_render(0);
    CHECK(gldUpdateReadFramebuffer(context, VGC_READ_DIRTY) == 1); expect_render(0);
    gldUpdateTransformFeedbackState(context, 0); expect_render(0);
    CHECK(!images && !triangles && gldGetError(context) == 0);
    /* 新 context 没有完成代次，沿用真实 render 的 NotReady，不伪造 fence。 */
    gldWaitForContext(context); expect_render(kIOReturnNotReady);
    expect_attach_cases(context, 1, a, h, b);
    expect_device_strings(dev, vendor, renderer_string);
    calls = device_status_calls;
    expect_device_strings(context, NULL, NULL);
    expect_device_strings(share, NULL, NULL);
    CHECK(device_status_calls == calls);
    CHECK(gldGetError(context) == 0x502 && gldGetError(context) == 0);
    for (size_t i = 0; i < 2; ++i) OK(gldCreatePipelineProgram(share, &pipeline[i], pd[i], pu[i]));
    token(b, 0x400, pipeline[0]); token(b, 0x420, pipeline[1]);
    rb(td, tu);
    OK(gi_create_texture_views(share, &texture, (vgt_views){{td,sizeof(td)},{tu,sizeof(tu)},{0}}));
    put16(fq, 0, 0x2000); put16(fq, 0xb2, 256); put16(fq, 0xb4, 256); put32(fq, 0xc0, 1);
    token(fd, 0, texture); fd[0x50] = 1;
    OK(gldCreateFramebuffer(share, &fb, fq, fd)); CHECK(gldLoadFramebuffer(share, fb) == 1);
    token(b, 0x4a0, fb); token(b, 0x4a8, fb);
    CHECK(gldUpdateDrawFramebuffer(context, VGC_DRAW_DIRTY) == 1); expect_render(0);
    CHECK(gldUpdateReadFramebuffer(context, VGC_READ_DIRTY) == 1); expect_render(0);
    token(b, 0x4a0, NULL);
    CHECK(gldUpdateDrawFramebuffer(context, VGC_DRAW_DIRTY) == 1); expect_render(0);
    token(b, 0x4a0, fb);
    CHECK(gldUpdateDrawFramebuffer(context, VGC_DRAW_DIRTY) == 1); expect_render(0);
    /* 命名 FBO 和活跃 lease 都不属于 drawable detach 的修改范围。 */
    expect_attach_cases(context, 1, a, h, b);
    expect_attach_cases(texture, 0, NULL, NULL, NULL);
    expect_attach_cases(fb, 0, NULL, NULL, NULL);
    expect_attach_cases(pipeline[0], 0, NULL, NULL, NULL);
    begin_triangle(context);
    expect_attach_cases(context, 1, a, h, b);
    end_triangle(context);
    CHECK(triangles == 1 && gldGetError(context) == 0);
    CHECK(submitted_vertices[0][2] == 0 && submitted_vertices[1][2] == 0.5f && submitted_vertices[2][2] == 1);
    gldFlushContext(context); expect_render(0);
    vgpuIntegrationStatus(&status);
    const uint64_t completed_generation = status.render_generation;
    CHECK(completed_generation != 0);
    expect_attach_cases(context, 1, a, h, b);
    gldWaitForContext(context); expect_render(0);
    vgpuIntegrationStatus(&status); CHECK(status.render_generation == completed_generation);
    unsigned char pixels[4];
    OK(vgpuIntegrationRead(context, completed_generation, 0, 0, 1, 1,
        UINT32_C(0x1908), UINT32_C(0x1401), pixels, sizeof(pixels), 0, NULL));
    CHECK(gldGetError(context) == 0);
    begin_triangle(context); gldFlushContext(context); expect_render(kIOReturnNotReady);
    expect_attach_cases(context, 1, a, h, b);
    CHECK(gldGetError(context) == 0x502); end_triangle(context); CHECK(triangles == 2);
    begin_triangle(context); OK(gldModifyPipelineProgram(share, pipeline[0], 5));
    expect_attach_cases(context, 1, a, h, b);
    end_triangle(context);
    CHECK(triangles == 2 && gldGetError(context) == 0x502);
    /* 消费首错之后 pending 仍保留，detach 不能使 wait 误报成功。 */
    expect_attach_cases(context, 1, a, h, b);
    CHECK(gldGetError(context) == 0);
    gldWaitForContext(context); expect_render(kIOReturnNotReady); CHECK(gldGetError(context) == 0x502);
    begin_triangle(context); end_triangle(context); CHECK(triangles == 3 && gldGetError(context) == 0);
    begin_triangle(context); CHECK(gldLoadFramebuffer(share, fb) == 1); end_triangle(context);
    CHECK(triangles == 3 && gldGetError(context) == 0x502);
    begin_triangle(context); token(b, 0x400, NULL); end_triangle(context); token(b, 0x400, pipeline[0]);
    CHECK(triangles == 3 && gldGetError(context) == 0x502);
    h[0x4979] = 1; begin_triangle(context); end_triangle(context); h[0x4979] = 0;
    CHECK(triangles == 3 && gldGetError(context) == 0x502);
    gi_tcl_vertex *sink = begin_triangle(context); sink[1].rgba[0] = 0; end_triangle(context);
    CHECK(triangles == 3 && gldGetError(context) == 0x502);
    int32_t count = 1; CHECK(begin_fn(context, 1, &count) && count == 48); end_fn(context, 0x8002, 1, 3);
    CHECK(triangles == 3 && gldGetError(context) == 0x502);
    count = 0; CHECK(begin_fn(context, 4, &count)); end_fn(context, 0x8002, 4, 0);
    gldFlushContext(context); expect_render(kIOReturnNotReady); CHECK(gldGetError(context) == 0x502);
    begin_triangle(context); end_triangle(context); CHECK(triangles == 4);
    gi_dispatch_r24_fn reject;
    memcpy(&reject, table + 24 * 8, sizeof(reject));
    CHECK(reject(context, 0, 4, 0, 3, 0, NULL, 1, 0) == 1 && gldGetError(context) == 0x502);
    begin_triangle(context); end_triangle(context); CHECK(triangles == 5);
    fail_triangle = 1; begin_triangle(context); end_triangle(context); fail_triangle = 0;
    expect_attach_cases(context, 1, a, h, b);
    CHECK(triangles == 6 && gldGetError(context) == 0x502 && gldGetError(context) == 0);
    gldClearFramebufferData(context, 1, values); expect_render(0);
    begin_triangle(context); end_triangle(context); CHECK(triangles == 6);
    gldWaitForContext(context); expect_render(kIOReturnNotReady); CHECK(gldGetError(context) == 0x502);
    gldUpdateTransformFeedbackState(context, 1); expect_render(kIOReturnUnsupported);
    CHECK(gldGetError(context) == 0x502);
    gldClearFramebufferData(context, 1, values); CHECK(gldGetError(context) == 0x502);
    gldUpdateTransformFeedbackState(context, 0);
    CHECK(gldUpdateDrawFramebuffer(context, VGC_DRAW_DIRTY) == 1);
    begin_triangle(context); end_triangle(context); CHECK(triangles == 6);
    CHECK(gldGetError(context) == 0x502);
    begin_triangle(context); CHECK(gldDestroyContext(context) == kCGLBadState);
    end_fn(context, 0x8002, 4, 0); OK(gldDestroyContext(context));
    /* 销毁后不再分配 context，避免把无 ABA 保证误当成可复用 token。 */
    expect_attach_cases(context, 0, NULL, NULL, NULL);
    OK(gldDestroyFramebuffer(share, fb));
    OK(gldDestroyTexture(share, texture));
    for (size_t i = 0; i < 2; ++i) OK(gldDestroyPipelineProgram(share, pipeline[i]));
    OK(gldDestroyShareGroup(share)); OK(gldDestroyPixelFormat(pixel));
    fail_owner = 1; CHECK(gldDestroyDevice(dev) != 0); fail_owner = 0;
    expect_device_strings(dev, vendor, renderer_string);
    OK(gldDestroyDevice(dev));
    calls = device_status_calls;
    expect_device_strings(dev, NULL, NULL);
    CHECK(device_status_calls == calls);
    CHECK(!strcmp(vendor, "vGPU-macOS") && !strcmp(renderer_string, "vGPU restricted SVGA3D"));
    gldTerminateLibrary(); CHECK(!owners && !images);
    expect_device_strings(dev, NULL, NULL);
    CHECK(device_status_calls == calls);
    CHECK(!strcmp(vendor, "vGPU-macOS") && !strcmp(renderer_string, "vGPU restricted SVGA3D"));
    puts("PASS controlled integration: unbound detach/无效与销毁token/参数拒绝/NULL与canary/命名FBO与首错与pending与代次保留/device身份/状态故障/稳定字符串/selector拒绝/状态不变/稀疏A/回滚/typed dispatch/真实门控/失效/完成与永久故障/销毁重试；仅CPU模型");
    return 0;
}
