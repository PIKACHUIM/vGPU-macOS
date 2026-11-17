#include "integration.h"
#include "context_exports.h"
#include <IOKit/IOKitLib.h>
#include <OpenGL/CGLTypes.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define LOAD(name, type) type name; do { void *p = dlsym(lib, #name); \
    CHECK(p != NULL); memcpy(&name, &p, sizeof(name)); } while (0)

typedef void (*init_fn)(const uint32_t *, const uint8_t *, uint32_t,
                        uintptr_t, uintptr_t, uint32_t);
typedef void (*term_fn)(void);
typedef int32_t (*version_fn)(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
typedef int32_t (*create_fn)(void **, uint32_t, void *);
typedef int32_t (*destroy_fn)(void *);
typedef int32_t (*populate_fn)(void *, void *);
typedef int32_t (*choose_fn)(void **, const uint32_t *);
typedef void (*status_fn)(struct gi_status *);
typedef int32_t (*share_fn)(void *, void **, uintptr_t);
typedef int32_t (*context_fn)(void *, void **, void *, void *, void *, void *);
typedef int32_t (*object_create_fn)(void *, void **, void *, void *);
typedef int32_t (*object_destroy_fn)(void *, void *);
typedef int32_t (*object_modify_fn)(void *, void *, uint32_t);
typedef void (*dispatch_fn)(void *, void *);
typedef void (*clear_fn)(void *, uint32_t, const void *);
typedef void (*sync_fn)(void *);
typedef uint32_t (*error_fn)(void *);
typedef int32_t (*system_read_fn)(void *, int32_t, int32_t, int32_t, int32_t,
                                 uint32_t, uint32_t, void *, uint32_t, void *);
typedef int32_t (*read_fn)(void *, uint64_t, int32_t, int32_t, int32_t, int32_t,
                           uint32_t, uint32_t, void *, size_t, uint32_t, void *);

static uint32_t get32(const void *p, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)p + offset, 4);
    return value;
}
static int bytes_are(const void *p, size_t size, unsigned char expected)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < size; ++i) if (b[i] != expected) return 0;
    return 1;
}
static void check_symbol(void *lib, const char *name, int trap, void *base)
{
    void *p = dlsym(lib, name);
    CHECK(p != NULL);
    Dl_info info;
    CHECK(dladdr(p, &info) != 0 && info.dli_fbase == base);
    CHECK(info.dli_saddr == p && strcmp(info.dli_sname, name) == 0);
    if (trap) CHECK(memcmp(p, "\x0f\x0b", 2) == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3 && (strcmp(argv[2], "--device") == 0 ||
                        strcmp(argv[2], "--empty") == 0 ||
                        strcmp(argv[2], "--metadata") == 0));
    int test_metadata = strcmp(argv[2], "--metadata") == 0;
    int test_device = strcmp(argv[2], "--empty") != 0;
    io_service_t service = IO_OBJECT_NULL;
    if (test_device) {
        CHECK(geteuid() == 0);
        service = IOServiceGetMatchingService(MACH_PORT_NULL,
                                             IOServiceMatching("vgpuAccelerator"));
        CHECK(service != IO_OBJECT_NULL);
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        void *lib = dlopen(argv[1], (mode ? RTLD_LAZY : RTLD_NOW) | RTLD_LOCAL);
        if (!lib) fprintf(stderr, "%s\n", dlerror());
        CHECK(lib != NULL);
        LOAD(gldInitializeLibrary, init_fn);
        LOAD(gldTerminateLibrary, term_fn);
        LOAD(gldGetVersion, version_fn);
        LOAD(gldCreateDevice, create_fn);
        LOAD(gldDestroyDevice, destroy_fn);
        LOAD(gldPopulateRendererInfo, populate_fn);
        LOAD(gldChoosePixelFormat, choose_fn);
        LOAD(gldDestroyPixelFormat, destroy_fn);
        LOAD(vgpuIntegrationStatus, status_fn);
        LOAD(vgpuIntegrationPublishCandidate, destroy_fn);
        LOAD(gldCreateShareGroup, share_fn);
        LOAD(gldCreateContext, context_fn);
        LOAD(gldDestroyContext, destroy_fn);
        LOAD(gldDestroyShareGroup, destroy_fn);
        LOAD(vgpuIntegrationEnableMetadata, destroy_fn);
        LOAD(gldPopulateContextDispatch, dispatch_fn);
        LOAD(gldUnbindPipelineProgram, dispatch_fn);
        LOAD(gldCreateTexture, object_create_fn);
        LOAD(gldDestroyTexture, object_destroy_fn);
        LOAD(gldModifyTexture, object_modify_fn);
        LOAD(gldCreateSampler, object_create_fn);
        LOAD(gldDestroySampler, object_destroy_fn);
        LOAD(gldCreatePipelineProgram, object_create_fn);
        LOAD(gldModifyPipelineProgram, object_modify_fn);
        LOAD(gldDestroyPipelineProgram, object_destroy_fn);
        LOAD(gldCreateProgram, object_create_fn);
        LOAD(gldDestroyProgram, object_destroy_fn);
        LOAD(gldCreateFramebuffer, object_create_fn);
        LOAD(gldDestroyFramebuffer, object_destroy_fn);
        LOAD(gldLoadFramebuffer, object_destroy_fn);
        LOAD(gldClearFramebufferData, clear_fn);
        LOAD(gldFlushContext, sync_fn);
        LOAD(gldWaitForContext, sync_fn);
        LOAD(gldGetError, error_fn);
        LOAD(gldReadFramebufferData, system_read_fn);
        CHECK(gldGetError(NULL) == 0);
        CHECK(gldReadFramebufferData(NULL, 0, 0, 1, 1, 0x1908, 0x1401, NULL, 0, NULL) == 0);
        LOAD(vgpuIntegrationRead, read_fn);
        Dl_info image;
        CHECK(dladdr(dlsym(lib, "gldGetVersion"), &image) != 0);
#define GLD_IMPLEMENTED(name) check_symbol(lib, #name, 0, image.dli_fbase);
#define GLD_TRAP(name) check_symbol(lib, #name, 1, image.dli_fbase);
#include <integration_entries.inc>
#undef GLD_IMPLEMENTED
#undef GLD_TRAP
        check_symbol(lib, "gldInitializeLibrary", 0, image.dli_fbase);
        check_symbol(lib, "gldTerminateLibrary", 0, image.dli_fbase);
        check_symbol(lib, "vgpuIntegrationStatus", 0, image.dli_fbase);
        CHECK(dlsym(lib, "gpx_configure") == NULL);
        CHECK(dlsym(lib, "gi_device_create") == NULL);
        uint32_t version[4] = {0};
        CHECK(gldGetVersion(&version[0], &version[1], &version[2], &version[3]) == 1);
        CHECK(version[0] == 4 && version[1] == 1 && version[2] == 0x11);
        CHECK((version[3] | 0x20000U) == GLD_DEVICE_DRIVER_ID);
        const uint32_t empty[] = {0};
        void *pixel = (void *)(uintptr_t)1;
        CHECK(gldChoosePixelFormat(&pixel, empty) == 0 && pixel == NULL);
        gldInitializeLibrary(NULL, NULL, 0, 0, 0, 0);
        struct gi_status state;
        vgpuIntegrationStatus(&state);
        CHECK(state.backend.initialized_mask == 0 && state.candidate_count == 0);
        struct { unsigned char pre[16], value[0x190], post[16]; } config;
        memset(&config, 0xa5, sizeof(config));
        void *device = (void *)(uintptr_t)1;
        CHECK(gldCreateDevice(&device, 1, config.value) != 0 && device == NULL);
        CHECK(bytes_are(&config, sizeof(config), 0xa5));
        void *unsupported = (void *)(uintptr_t)1;
        CHECK(gldCreateShareGroup(NULL, &unsupported, 4) == kCGLBadMatch);
        CHECK(unsupported == NULL);
        unsigned char a[32], h[32], b[32];
        memset(a, 0xa1, sizeof(a)); memset(h, 0xb2, sizeof(h)); memset(b, 0xc3, sizeof(b));
        unsupported = (void *)(uintptr_t)1;
        CHECK(gldCreateContext(NULL, &unsupported, NULL, a, h, b) == kCGLBadMatch);
        CHECK(unsupported == NULL && bytes_are(a, sizeof(a), 0xa1) &&
              bytes_are(h, sizeof(h), 0xb2) && bytes_are(b, sizeof(b), 0xc3));
        CHECK(gldDestroyContext(NULL) == kCGLBadContext);
        CHECK(gldDestroyShareGroup(NULL) == kCGLBadMatch);
        unsupported = (void *)(uintptr_t)1;
        CHECK(gldCreateFramebuffer(NULL, &unsupported, NULL, NULL) != 0 && unsupported == NULL);
        CHECK(gldDestroyFramebuffer(NULL, NULL) != 0);
        CHECK(gldLoadFramebuffer(NULL, NULL) == 0);
        gldClearFramebufferData(NULL, 0, NULL);
        vgpuIntegrationStatus(&state); CHECK(state.render_status == kIOReturnBadArgument);
        gldFlushContext(NULL);
        vgpuIntegrationStatus(&state); CHECK(state.render_status == kIOReturnBadArgument);
        gldWaitForContext(NULL);
        vgpuIntegrationStatus(&state); CHECK(state.render_status == kIOReturnBadArgument);
        CHECK(vgpuIntegrationRead(NULL, 0, 0, 0, 1, 1, 0x1908, 0x1401,
                                  NULL, 0, 0, NULL) == kIOReturnBadArgument);
        CHECK(state.render_shares == 0 && state.render_contexts == 0 && state.pending_cleanup == 0);
        CHECK(dlsym(lib, "gi_render_share_open") == NULL);
        if (test_device) {
            uint32_t services[32] = {0};
            uint8_t bytes[32] = {0};
            const uint32_t mask = UINT32_C(0x80000081);
            services[0] = services[7] = services[31] = service;
            gldInitializeLibrary(services, bytes, mask, 0, 0, 0);
            for (unsigned round = 0; round < 16; ++round) {
                const uint32_t masks[] = {1, 0x80, UINT32_C(0x80000000), mask};
                memset(&config, 0xa5, sizeof(config));
                int32_t result = gldCreateDevice(&device, masks[round % 4], config.value);
                if (result) fprintf(stderr, "device status=0x%08" PRIx32 "\n", (uint32_t)result);
                /* 拒绝即退出，不提权重试或构造虚假设备。 */
                CHECK(result == 0 && device != NULL);
                CHECK(bytes_are(config.pre, 16, 0xa5) && bytes_are(config.post, 16, 0xa5));
                CHECK(bytes_are(config.value, 0x180, 0));
                vgpuIntegrationStatus(&state);
                CHECK(state.devices == 1 && state.candidate_count == 0);
                CHECK(gldChoosePixelFormat(&pixel, empty) == 0 && pixel == NULL);
                CHECK(vgpuIntegrationPublishCandidate(device) == 0);
                vgpuIntegrationStatus(&state);
                CHECK(state.devices == 1 && state.candidate_count == 1);
                CHECK(state.width == 256 && state.height == 256 && state.samples == 1);
                CHECK(state.context_supported == 0 && state.backend.connections == 1);
                CHECK(state.renderer_id == UINT32_C(0x01027e00) && state.display_mask == mask);
                struct { unsigned char pre[16], value[0x88], post[16]; } renderer;
                memset(&renderer, 0x5a, sizeof(renderer));
                CHECK(gldPopulateRendererInfo(device, renderer.value) == 0);
                CHECK(bytes_are(renderer.pre, 16, 0x5a) && bytes_are(renderer.post, 16, 0x5a));
                CHECK(get32(renderer.value, 8) == state.renderer_id);
                CHECK(get32(renderer.value, 0x0c) == GP_FLAG_OFFSCREEN);
                CHECK(get32(renderer.value, 0x10) == GP_BUFFER_MONO_SINGLE);
                CHECK(get32(renderer.value, 0x14) == GP_COLOR_ARGB8888);
                CHECK(get32(renderer.value, 0x1c) == 1 && get32(renderer.value, 0x20) == 1);
                CHECK(get32(renderer.value, 0x24) == mask);
                CHECK(memcmp(renderer.value + 0x34, config.value + 0x180, 8) == 0);
                CHECK(bytes_are(renderer.value + 0x28, 12, 0));
                CHECK(bytes_are(renderer.value + 0x3c, 0x4c, 0));
                const uint32_t rgba[] = {8, 32, 11, 8, 0};
                CHECK(gldChoosePixelFormat(&pixel, rgba) == 0 && pixel != NULL);
                CHECK(((gp_pixel_node *)pixel)->next == NULL);
                CHECK(get32(pixel, 8) == get32(renderer.value, 8));
                CHECK(get32(pixel, 0x0c) == GP_FLAG_OFFSCREEN);
                CHECK(get32(pixel, 0x14) == GP_COLOR_ARGB8888);
                CHECK(get32(pixel, 0x34) == mask);
                CHECK(bytes_are((unsigned char *)pixel + 0x24, 16, 0));
                void *second = NULL;
                CHECK(gldCreateDevice(&second, 1, config.value) == kIOReturnBusy && second == NULL);
                CHECK(gldDestroyDevice(device) == kIOReturnBusy);
                gldInitializeLibrary(NULL, NULL, 0, 0, 0, 0);
                vgpuIntegrationStatus(&state);
                CHECK(state.lifecycle_status == kIOReturnBusy && state.pixel_lists == 1);
                CHECK(gldPopulateRendererInfo(device, renderer.value) == 0);
                const uint32_t rejected[][3] = {{5, 0, 0}, {12, 1, 0}, {13, 1, 0}, {11, 9, 0}};
                for (size_t i = 0; i < 4; ++i) {
                    void *none = (void *)(uintptr_t)1;
                    CHECK(gldChoosePixelFormat(&none, rejected[i]) == 0 && none == NULL);
                }
                const uint32_t unknown[] = {99, 0};
                CHECK(gldChoosePixelFormat(&second, unknown) == kCGLBadAttribute && second == NULL);
                if (test_metadata) {
                    void *share = NULL, *context = NULL;
                    unsigned char ca[0x268], ch[VGC_H_LENGTH], cb[VGC_B_LENGTH];
                    unsigned char td[11][VGT_TD_METADATA_LENGTH] = {{0}};
                    unsigned char tu[11][VGT_TU_LENGTH] = {{0}}, sd[11][0x30] = {{0}}, su[11][4] = {{0}};
                    unsigned char pd[3][4] = {{0}}, pu[3][4] = {{0}};
                    void *textures[11] = {0}, *samplers[11] = {0}, *pipelines[3] = {0};
                    memset(ca, 0xa7, sizeof(ca)); memset(ch, 0x39, sizeof(ch)); memset(cb, 0x82, sizeof(cb));
                    int32_t enable_result = vgpuIntegrationEnableMetadata(device);
                    if (enable_result) {
                        vgpuIntegrationStatus(&state);
                        fprintf(stderr, "metadata enable=%08x registered=%u shares=%u pending=%u\n",
                            (unsigned)enable_result, (unsigned)state.native_registered,
                            (unsigned)state.share_groups, (unsigned)state.pending_cleanup);
                    }
                    CHECK(enable_result == 0);
                    CHECK(gldCreateShareGroup(device, &share, 4) == 0 && share);
                    CHECK(gldCreateContext(share, &context, pixel, ca, ch, cb) == 0 && context);
                    vgpuIntegrationStatus(&state);
                    CHECK(state.render_shares == 1 && state.render_contexts == 1 && state.pending_cleanup == 0);
                    CHECK(ca[0x6e] == 1 && ca[0x6f] == 0xa7 && ca[0x258] == 0xa7);
                    CHECK(bytes_are(ch, sizeof(ch), 0x39) && bytes_are(cb, sizeof(cb), 0x82));
                    unsigned char render[25 * sizeof(void *)]; memset(render, 0x91, sizeof(render));
                    gldPopulateContextDispatch(context, render);
                    vgpuIntegrationStatus(&state);
                    CHECK(state.dispatch_status == kCGLBadMatch && !state.context_supported);
                    CHECK(bytes_are(render, sizeof(render), 0x91));
                    for (unsigned i = 0; i < 11; ++i) {
                        uint16_t flags = (uint16_t)(0x1000 | i); memcpy(td[i], &flags, 2); su[i][0] = 1;
                        CHECK(gldCreateTexture(share, &textures[i], td[i], tu[i]) == 0 && textures[i]);
                        CHECK(gldCreateSampler(share, &samplers[i], sd[i], su[i]) == 0 && samplers[i]);
                        CHECK(gldModifyTexture(share, textures[i], 0) == 0 && tu[i][0x14] != 0);
                        CHECK(su[i][0] == 1);
                    }
                    for (unsigned i = 0; i < 3; ++i) {
                        pd[i][2] = i ? 4 : 0; pu[i][0] = 3;
                        if (i == 2) { pd[i][0] = 4; pd[i][1] = 0x88; pd[i][3] = 1; }
                        CHECK(gldCreatePipelineProgram(share, &pipelines[i], pd[i], pu[i]) == 0 && pipelines[i]);
                        CHECK(gldModifyPipelineProgram(share, pipelines[i], 5) == 0 && pu[i][0] == 3);
                        gldUnbindPipelineProgram(context, pipelines[i]);
                        vgpuIntegrationStatus(&state); CHECK(state.object_status == 0);
                    }
                    unsigned char gd = 0x41, gu = 0x42; void *program = NULL;
                    CHECK(gldCreateProgram(share, &program, &gd, &gu) == 0 && program);
                    CHECK(gldDestroyProgram(share, program) == 0 && gd == 0x41 && gu == 0x42);
                    CHECK(gldDestroyShareGroup(share) == kCGLBadState);
                    for (unsigned i = 0; i < 11; ++i) {
                        CHECK(gldDestroySampler(share, samplers[i]) == 0);
                        CHECK(gldDestroyTexture(share, textures[i]) == 0);
                    }
                    for (unsigned i = 0; i < 3; ++i) CHECK(gldDestroyPipelineProgram(share, pipelines[i]) == 0);
                    CHECK(gldDestroyContext(context) == 0);
                    CHECK(gldDestroyShareGroup(share) == 0);
                    vgpuIntegrationStatus(&state);
                    CHECK(state.render_shares == 0 && state.render_contexts == 0 && state.pending_cleanup == 0);
                }
                CHECK(gldDestroyPixelFormat((unsigned char *)pixel + 8) == kCGLBadPixelFormat);
                CHECK(gldDestroyPixelFormat(pixel) == 0);
                CHECK(gldDestroyPixelFormat(pixel) == kCGLBadPixelFormat);
                pixel = NULL;
                CHECK(gldDestroyDevice(device) == 0);
                device = NULL;
                CHECK(gldChoosePixelFormat(&pixel, empty) == 0 && pixel == NULL);
            }
            CHECK(gldCreateDevice(&device, 1, config.value) == 0);
            CHECK(vgpuIntegrationPublishCandidate(device) == 0);
            CHECK(gldChoosePixelFormat(&pixel, empty) == 0 && pixel != NULL);
            void *second = NULL;
            CHECK(gldChoosePixelFormat(&second, empty) == 0 && second != NULL);
            gldTerminateLibrary();
            CHECK(gldDestroyPixelFormat(pixel) == kCGLBadPixelFormat);
            CHECK(gldDestroyPixelFormat(second) == kCGLBadPixelFormat);
            CHECK(gldDestroyDevice(device) != 0);
            pixel = device = NULL;
            CHECK(gldChoosePixelFormat(&pixel, empty) == 0 && pixel == NULL);
            gldInitializeLibrary(services, bytes, mask, 0, 0, 0);
            CHECK(gldCreateDevice(&device, 1, config.value) == 0);
            CHECK(gldDestroyDevice(device) == 0);
        }
        gldTerminateLibrary();
        gldTerminateLibrary();
        vgpuIntegrationStatus(&state);
        CHECK(state.devices == 0 && state.pixel_lists == 0 && state.candidate_count == 0);
        CHECK(state.lifecycle_status == 0 && state.backend.service_refs == 0);
        CHECK(state.backend.devices == 0 && state.backend.connections == 0);
        CHECK(state.backend.retains == state.backend.releases);
        CHECK(state.backend.opens == state.backend.closes);
        CHECK(dlclose(lib) == 0);
    }
    if (service != IO_OBJECT_NULL) CHECK(IOObjectRelease(service) == 0);
    printf("PASS integration checks=%u mode=%s; 129 GLD slots, dispatch rejected, no CGL/install/draw\n",
           checks, argv[2]);
    return 0;
}
