#include "integration.h"
#include "context_exports.h"
#include <OpenGL/CGLTypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
/* 只替代 device/pixel 平台边界。元数据、context、objects、texture 为真实源码。 */
void gi_device_initialize(const uint32_t *, const uint8_t *, uint32_t, uintptr_t, uintptr_t, uint32_t);
void gi_device_terminate(void);
int32_t gi_device_create(void **, uint32_t, void *);
int32_t gi_device_destroy(void *);
int32_t gi_device_populate(void *, void *);
int32_t gi_pixel_choose(void **, const uint32_t *);
int32_t gi_pixel_destroy(void *);
extern int vgo_fixture_bgra, vgo_fixture_fail_owner_release;
extern unsigned vgo_fixture_owners, vgo_fixture_images;
extern unsigned vgo_fixture_default_open_calls, vgo_fixture_registry_open_calls;
extern uint64_t vgo_fixture_last_registry_id;
static int device_token, device_live, fail_status_after_open;
static uint64_t registry = 1;
static unsigned destroys;
static gp_format_cap configured;
static size_t configured_count;
static gp_pixel_list pixel_list;
void gi_device_initialize(const uint32_t *s, const uint8_t *b, uint32_t m,
                          uintptr_t f, uintptr_t d, uint32_t flags) {
    (void)s; (void)b; (void)m; (void)f; (void)d; (void)flags;
}
void gi_device_terminate(void) { device_live = 0; }
int32_t gi_device_create(void **out, uint32_t mask, void *config) {
    (void)mask; (void)config; device_live = 1; *out = &device_token; return 0;
}
int32_t gi_device_destroy(void *device) {
    CHECK(device == &device_token && device_live); device_live = 0; ++destroys; return 0;
}
int32_t gi_device_populate(void *d, void *out) { (void)d; memset(out, 0, 0x88); return 0; }
int32_t vgpuGLDDeviceGetStatus(void *device, struct gld_device_status *out) {
    if (!device_live || device != &device_token ||
        (fail_status_after_open && vgo_fixture_owners)) return kIOReturnNotReady;
    *out = (struct gld_device_status){.registry_id = registry, .connected = 1, .liveness_status = kIOReturnNotReady,
        .renderer_id = UINT32_C(0x01027e00), .display_mask = 1};
    return 0;
}
void vgpuGLDDeviceGetCounts(struct gld_device_counts *out) {
    memset(out, 0, sizeof(*out)); out->devices = (uint32_t)device_live;
}
int32_t gpx_configure(const gp_format_cap *formats, size_t count) {
    configured_count = count;
    if (count) configured = formats[0];
    return 0;
}
int32_t gi_pixel_choose(void **out, const uint32_t *tokens) {
    size_t n = 0;
    while (tokens[n]) ++n;
    gp_status result = gp_choose_normalized(tokens, (n + 1) * 4, &configured,
                                         configured_count, NULL, &pixel_list);
    *out = pixel_list.head;
    return result == GP_OK || result == GP_NO_MATCH ? 0 : kCGLBadAttribute;
}
int32_t gi_pixel_destroy(void *head) {
    CHECK(head == pixel_list.head); gp_pixel_list_destroy(&pixel_list); return 0;
}
static uint32_t get32(const void *p, size_t offset) {
    uint32_t value; memcpy(&value, (const unsigned char *)p + offset, 4); return value;
}
int main(void) {
    void *device = NULL, *share = NULL, *other = NULL, *context = NULL, *pixel = NULL;
    unsigned char a[0x268], h[VGC_H_LENGTH], b[VGC_B_LENGTH];
    unsigned char td[11][VGT_TD_METADATA_LENGTH] = {{0}}, tu[11][VGT_TU_LENGTH] = {{0}};
    unsigned char sd[11][0x30] = {{0}}, su[11][4] = {{0}};
    unsigned char pd[3][4] = {{0}}, pu[3][4] = {{0}};
    void *textures[11] = {0}, *samplers[11] = {0}, *pipelines[3] = {0};
    struct gi_status state;
    CHECK(gldCreateShareGroup(NULL, &share, 4) == kCGLBadMatch && !share);
    CHECK(gldCreateContext(NULL, &context, NULL, NULL, NULL, NULL) == kCGLBadMatch && !context);
    CHECK(gldDestroyContext(NULL) == kCGLBadContext);
    gldInitializeLibrary(NULL, NULL, 0, 0, 0, 0);
    CHECK(gldCreateDevice(&device, 1, NULL) == 0);
    CHECK(gldCreateShareGroup(device, &share, 4) == kCGLBadMatch && !share);
    vgo_fixture_bgra = 1;
    registry = 73;
    CHECK(vgpuIntegrationEnableMetadata(device) == kIOReturnNotFound);
    CHECK(vgo_fixture_last_registry_id == 73 && vgo_fixture_default_open_calls == 0);
    registry = 1;
    /* native 创建后 device liveness 验证失败，回滚引用也必须可重试。 */
    fail_status_after_open = 1;
    CHECK(vgpuIntegrationEnableMetadata(device) == kCGLBadState);
    vgpuIntegrationStatus(&state);
    CHECK(!state.metadata_enabled && !state.native_registered && vgo_fixture_owners == 1);
    vgo_fixture_fail_owner_release = 1;
    CHECK(gldDestroyDevice(device) == kIOReturnNotReady && destroys == 0);
    vgo_fixture_fail_owner_release = 0; fail_status_after_open = 0;
    CHECK(vgpuIntegrationEnableMetadata(device) == 0);
    unsigned opens = vgo_fixture_registry_open_calls;
    CHECK(vgpuIntegrationEnableMetadata(device) == 0 && opens == vgo_fixture_registry_open_calls);
    vgpuIntegrationStatus(&state);
    CHECK(state.metadata_enabled == 1 && state.native_registered == 1 && state.context_supported == 0);
    CHECK(state.candidate_count == 0 && vgo_fixture_default_open_calls == 0);
    CHECK(vgpuIntegrationPublishCandidate(device) == 0);
    const uint32_t tokens[] = {8, 32, 11, 8, 0};
    CHECK(gldChoosePixelFormat(&pixel, tokens) == 0 && pixel);
    CHECK(configured.buffer_mode == 5 && get32(pixel, 0x10) == 0);
    CHECK((get32(pixel, 0x0c) & GP_FLAG_WINDOW) == 0);
    unsigned char renderer[0x88];
    CHECK(gldPopulateRendererInfo(device, renderer) == 0);
    CHECK(get32(renderer, 0x10) == 5 && configured.buffer_mode == 5);
    CHECK((get32(renderer, 0x0c) & GP_FLAG_WINDOW) == 0);
    CHECK(gldCreateShareGroup(device, &share, 5) == kCGLBadMatch && !share);
    CHECK(gldCreateShareGroup(device, &share, 4) == 0 && share);
    CHECK(gldCreateShareGroup(device, &other, 4) == 0 && other != share);
    memset(a, 0xa7, sizeof(a)); memset(h, 0x39, sizeof(h)); memset(b, 0x82, sizeof(b));
    unsigned char bad_pixel[0x38]; memcpy(bad_pixel, pixel, sizeof(bad_pixel)); bad_pixel[0x1c] = 0x80;
    CHECK(gldCreateContext(share, &context, bad_pixel, a, h, b) == kCGLBadPixelFormat && !context);
    CHECK(a[0x65] == 0xa7 && h[0] == 0x39 && b[0] == 0x82);
    const uint32_t rejected_buffers[] = {5, 9, 8};
    for (size_t i = 0; i < sizeof(rejected_buffers) / sizeof(rejected_buffers[0]); ++i) {
        memcpy(bad_pixel, pixel, sizeof(bad_pixel));
        memcpy(bad_pixel + 0x10, &rejected_buffers[i], sizeof(rejected_buffers[i]));
        CHECK(gldCreateContext(share, &context, bad_pixel, a, h, b) == kCGLBadPixelFormat && !context);
        CHECK(a[0x65] == 0xa7 && h[0] == 0x39 && b[0] == 0x82);
    }
    CHECK(gldCreateContext(share, &context, pixel, a, h, b) == 0 && context);
    CHECK(a[0x65] == 0 && a[0x6e] == 1 && a[0x6f] == 0xa7 && a[0x258] == 0xa7);
    CHECK(get32(a, 4) == 256 && get32(a, 8) == 256 && h[0] == 0x39 && b[0] == 0x82);
    CHECK(gldGetError(context) == 0);
    CHECK(gldReadFramebufferData(context, 0, 0, 1, 1, 0x1908, 0x1401, NULL, 2, NULL) == 0);
    CHECK(gldGetError(context) == 0x502 && gldGetError(context) == 0);
    gldFlushContext(context);
    CHECK(gldGetError(context) == 0x502);
    gldWaitForContext(context);
    CHECK(gldGetError(context) == 0x502 && gldGetError(context) == 0);
    unsigned char render[25 * sizeof(void *)]; memset(render, 0x91, sizeof(render));
    gldPopulateContextDispatch(context, render);
    vgpuIntegrationStatus(&state);
    CHECK(state.dispatch_status == kCGLBadMatch && state.context_supported == 0);
    for (size_t i = 0; i < sizeof(render); ++i) CHECK(render[i] == 0x91);
    for (unsigned i = 0; i < 11; ++i) {
        uint16_t flags = (uint16_t)(0x1000 | i); memcpy(td[i], &flags, 2); su[i][0] = 1;
        CHECK(gldCreateTexture(share, &textures[i], td[i], tu[i]) == 0);
        CHECK(gldCreateSampler(share, &samplers[i], sd[i], su[i]) == 0);
        CHECK(textures[i] && samplers[i] && textures[i] != samplers[i]);
        CHECK(gldDestroyTexture(other, textures[i]) == kCGLBadValue);
        CHECK(gldDestroySampler(share, textures[i]) == kCGLBadValue);
        CHECK(su[i][0] == 1);
        vgt_handle native = 0; vgt_texture_state ts;
        CHECK(gi_texture_native(share, textures[i], &native) == 0);
        CHECK(vgt_texture_state_get(native, &ts) == VGT_OK && ts.kind == i && !ts.has_backing);
        CHECK(gldModifyTexture(share, textures[i], 0) == 0 && tu[i][0x14] != 0);
    }
    for (unsigned i = 0; i < 3; ++i) {
        pd[i][2] = i ? 4 : 0;
        if (i == 2) { pd[i][0] = 4; pd[i][1] = 0x88; pd[i][3] = 1; }
        pu[i][0] = 3;
        CHECK(gldCreatePipelineProgram(share, &pipelines[i], pd[i], pu[i]) == 0);
        CHECK(gldModifyPipelineProgram(share, pipelines[i], 5) == 0 && pu[i][0] == 3);
        CHECK(gldModifyPipelineProgram(share, pipelines[i], 2) == kCGLBadState);
        gldUnbindPipelineProgram(other, pipelines[i]);
        vgpuIntegrationStatus(&state); CHECK(state.object_status == kCGLBadContext);
        gldUnbindPipelineProgram(context, pipelines[i]);
        vgpuIntegrationStatus(&state); CHECK(state.object_status == 0);
    }
    unsigned char gd = 0x41, gu = 0x42; void *program = NULL;
    CHECK(gldCreateProgram(share, &program, &gd, &gu) == 0 && program);
    CHECK(gldDestroyProgram(other, program) == kCGLBadValue);
    CHECK(gldDestroyProgram(share, program) == 0 && gd == 0x41 && gu == 0x42);
    CHECK(gldDestroyShareGroup(share) == kCGLBadState);
    gldTerminateLibrary(); vgpuIntegrationStatus(&state);
    CHECK(state.lifecycle_status == kIOReturnBusy && state.share_groups == 2 && state.pixel_lists == 1);
    CHECK(gldDestroyPixelFormat(pixel) == 0);
    CHECK(gldDestroyDevice(device) == kIOReturnBusy && destroys == 0);
    for (unsigned i = 0; i < 11; ++i) {
        CHECK(gldDestroySampler(share, samplers[i]) == 0);
        CHECK(gldDestroyTexture(share, textures[i]) == 0);
    }
    for (unsigned i = 0; i < 3; ++i) CHECK(gldDestroyPipelineProgram(share, pipelines[i]) == 0);
    CHECK(gldDestroyContext(context) == 0);
    CHECK(gldDestroyContext(context) == kCGLBadContext);
    CHECK(gldDestroyShareGroup(other) == 0 && gldDestroyShareGroup(share) == 0);
    vgo_fixture_fail_owner_release = 1;
    CHECK(gldDestroyDevice(device) == kIOReturnNotReady && destroys == 0);
    gldTerminateLibrary(); vgpuIntegrationStatus(&state);
    CHECK(state.lifecycle_status == kIOReturnNotReady && state.devices == 1 && vgo_fixture_owners == 1);
    vgo_fixture_fail_owner_release = 0;
    CHECK(gldDestroyDevice(device) == 0 && destroys == 1);
    CHECK(vgo_fixture_owners == 0 && vgo_fixture_images == 0 && vgo_fixture_default_open_calls == 0);
    gldTerminateLibrary(); gldTerminateLibrary(); vgpuIntegrationStatus(&state);
    CHECK(!state.devices && !state.share_groups && !state.metadata_enabled && !state.native_registered);
    CHECK(state.lifecycle_status == 0);
    printf("PASS integration metadata checks=%u; 真实元数据模块，平台 CPU fixture；非系统 GL/GPU 验收\n", checks);
    return 0;
}
