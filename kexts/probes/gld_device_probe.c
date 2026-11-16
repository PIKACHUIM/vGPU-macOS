#include "gld_device.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsInterface.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MASK UINT32_C(0x80000081)
#define ROUNDS 64u
struct guarded_config {
    uint8_t before[16];
    uint8_t value[GLD_DEVICE_CONFIG_BYTES];
    uint8_t after[16];
};
struct guarded_renderer {
    uint8_t before[16];
    uint8_t value[GLD_DEVICE_RENDERER_BYTES];
    uint8_t after[16];
};
struct guarded_handle {
    uint64_t before;
    void *value;
    uint64_t after;
};
_Static_assert(offsetof(struct guarded_config, after) == 16 + 0x190, "C边界");
_Static_assert(offsetof(struct guarded_renderer, after) == 16 + 0x88, "R边界");
_Static_assert(sizeof(void *) == 8, "输出槽宽度");

#define REQUIRE(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #expression); \
        goto cleanup; \
    } \
} while (0)

static bool bytes_are(const void *memory, size_t size, uint8_t value)
{
    const uint8_t *bytes = memory;
    for (size_t i = 0; i < size; ++i)
        if (bytes[i] != value)
            return false;
    return true;
}
static bool config_guard(const struct guarded_config *c)
{
    return bytes_are(c->before, 16, 0xa5) && bytes_are(c->after, 16, 0xa5);
}
static bool renderer_guard(const struct guarded_renderer *r)
{
    return bytes_are(r->before, 16, 0x5a) && bytes_are(r->after, 16, 0x5a);
}
static bool handle_guard(const struct guarded_handle *d)
{
    return d->before == UINT64_C(0x123456789abcdef0) &&
           d->after == UINT64_C(0xfedcba9876543210);
}
static struct guarded_handle new_handle(void)
{
    return (struct guarded_handle){UINT64_C(0x123456789abcdef0),
        (void *)(uintptr_t)1, UINT64_C(0xfedcba9876543210)};
}
static void initialize(io_service_t service)
{
    uint32_t services[32] = {0};
    uint8_t bytes[32] = {0};
    services[0] = services[7] = services[31] = service;
    bytes[0] = 0;
    bytes[7] = 7;
    bytes[31] = 31;
    gldInitializeLibrary(services, bytes, MASK, 0, 0, 0);
    /* 初始化必须复制数据；后续不能依赖栈数组或把字节数组当 renderer ID。 */
    memset(services, 0xcc, sizeof(services));
    memset(bytes, 0xdd, sizeof(bytes));
}
static bool balanced(uint32_t expected_refs)
{
    struct gld_device_counts c = {0};
    vgpuGLDDeviceGetCounts(&c);
    return c.service_refs == expected_refs && c.devices == 0 && c.connections == 0 &&
        c.retains == c.releases + expected_refs && c.opens == c.closes &&
        c.cleanup_status == KERN_SUCCESS;
}

int main(int argc, char **argv)
{
    bool denied = argc == 2 && strcmp(argv[1], "--expect-denied") == 0;
    bool bad_type = argc == 2 && strcmp(argv[1], "--expect-open-failure") == 0;
    if ((argc != 1 && !denied && !bad_type) || (denied && geteuid() == 0) ||
        (!denied && geteuid() != 0)) {
        fprintf(stderr, "用法：root执行无参数/--expect-open-failure，非root执行--expect-denied\n");
        return EXIT_FAILURE;
    }
    int exit_status = EXIT_FAILURE;
    io_service_t framebuffer = IO_OBJECT_NULL;
    io_service_t accelerator = IO_OBJECT_NULL;
    struct guarded_config config;
    struct guarded_renderer renderer;
    struct guarded_handle handle = new_handle();
    struct gld_device_counts counts = {0};
    framebuffer = IOServiceGetMatchingService(MACH_PORT_NULL,
                                               IOServiceMatching("vgpuFramebuffer"));
    REQUIRE(framebuffer != IO_OBJECT_NULL);
    UInt32 index = UINT32_MAX;
    REQUIRE(IOAccelFindAccelerator(framebuffer, &accelerator, &index) == KERN_SUCCESS);
    REQUIRE(accelerator != IO_OBJECT_NULL && index == 0);
    REQUIRE(IOObjectConformsTo(accelerator, "vgpuAccelerator"));
    uint64_t registry_id = 0;
    REQUIRE(IORegistryEntryGetRegistryEntryID(accelerator, &registry_id) == KERN_SUCCESS);
    mach_port_urefs_t baseline_refs = 0;
    REQUIRE(mach_port_get_refs(mach_task_self(), accelerator, MACH_PORT_RIGHT_SEND,
                               &baseline_refs) == KERN_SUCCESS);
    printf("uid=%lu type=0x%08" PRIx32 " registry=0x%016" PRIx64
           " association-index=%u source-send-refs=%u\n", (unsigned long)geteuid(),
           GLD_DEVICE_OPEN_TYPE, registry_id, index, baseline_refs);

    initialize(accelerator);
    vgpuGLDDeviceGetCounts(&counts);
    REQUIRE(counts.initialize_status == KERN_SUCCESS && counts.initialized_mask == MASK);
    REQUIRE(balanced(3));
    const uint32_t invalid_masks[] = {0, 2, MASK | 2};
    for (size_t i = 0; i < sizeof(invalid_masks) / sizeof(invalid_masks[0]); ++i) {
        memset(&config, 0xa5, sizeof(config));
        handle = new_handle();
        REQUIRE(gldCreateDevice(&handle.value, invalid_masks[i], config.value) != 0);
        REQUIRE(handle.value == NULL && handle_guard(&handle));
        REQUIRE(bytes_are(&config, sizeof(config), 0xa5) && balanced(3));
    }
    puts("PASS zero/disjoint/partially-outside mask, NULL output, untouched C and canaries");

    if (denied || bad_type) {
        int32_t expected = denied ? kIOReturnNotPrivileged : kIOReturnUnsupported;
        for (uint32_t round = 0; round < ROUNDS; ++round) {
            memset(&config, 0xa5, sizeof(config));
            handle = new_handle();
            int32_t actual = gldCreateDevice(&handle.value, MASK, config.value);
            if (round == 0)
                printf("backend-open status=0x%08" PRIx32 " expected=0x%08" PRIx32 "\n",
                       (uint32_t)actual, (uint32_t)expected);
            REQUIRE(actual == expected && handle.value == NULL && handle_guard(&handle));
            REQUIRE(bytes_are(&config, sizeof(config), 0xa5) && balanced(3));
            vgpuGLDDeviceGetCounts(&counts);
            REQUIRE(counts.backend_status == expected);
            REQUIRE(counts.retains == counts.releases + 3 && counts.opens == counts.closes);
        }
        puts("PASS 64 actual backend-open failures with full rollback; no dummy device");
    } else {
        uint8_t expected_config[GLD_DEVICE_CONFIG_BYTES] = {0};
        uint8_t expected_renderer[GLD_DEVICE_RENDERER_BYTES] = {0};
        int64_t page_size = getpagesize();
        const uint32_t renderer_id = UINT32_C(0x01027e00);
        const uint32_t full_mask = MASK;
        memcpy(expected_config + 0x180, &registry_id, 8);
        memcpy(expected_config + 0x188, &page_size, 8);
        memcpy(expected_renderer + 8, &renderer_id, 4);
        memcpy(expected_renderer + 0x24, &full_mask, 4);
        memcpy(expected_renderer + 0x34, &registry_id, 8);
        const uint32_t valid_masks[] = {1, 0x80, UINT32_C(0x80000000), MASK};
        for (uint32_t round = 0; round < ROUNDS; ++round) {
            memset(&config, 0xa5, sizeof(config));
            handle = new_handle();
            int32_t create_result = gldCreateDevice(&handle.value, valid_masks[round % 4], config.value);
            if (create_result != 0) {
                vgpuGLDDeviceGetCounts(&counts);
                fprintf(stderr, "create=0x%08" PRIx32 " open=0x%08" PRIx32
                        " cleanup=0x%08" PRIx32 "\n", (uint32_t)create_result,
                        (uint32_t)counts.backend_status, (uint32_t)counts.cleanup_status);
            }
            REQUIRE(create_result == 0);
            REQUIRE(handle.value != NULL && handle_guard(&handle) && config_guard(&config));
            REQUIRE(memcmp(config.value, expected_config, sizeof(expected_config)) == 0);
            vgpuGLDDeviceGetCounts(&counts);
            REQUIRE(counts.devices == 1 && counts.connections == 1 && counts.service_refs == 4);
            struct gld_device_status status = {0};
            REQUIRE(vgpuGLDDeviceGetStatus(handle.value, &status) == 0);
            REQUIRE(status.connected == 1 && status.registry_id == registry_id &&
                    status.liveness_status == kIOReturnNotReady && status.renderer_id == renderer_id &&
                    status.display_mask == MASK && status.connection_type == GLD_DEVICE_OPEN_TYPE);
            if (round == 0)
                printf("backend connected=1 liveness=0x%08" PRIx32
                       " renderer=0x%08" PRIx32 " mask=0x%08" PRIx32
                       " page-size=%" PRId64 " flags=0\n",
                       (uint32_t)status.liveness_status, status.renderer_id,
                       status.display_mask, page_size);
            for (uint32_t query = 0; query < 16; ++query) {
                memset(&renderer, 0x5a, sizeof(renderer));
                REQUIRE(gldPopulateRendererInfo(handle.value, renderer.value) == 0);
                REQUIRE(renderer_guard(&renderer) && config_guard(&config));
                REQUIRE(memcmp(renderer.value, expected_renderer, sizeof(expected_renderer)) == 0);
                REQUIRE(memcmp(config.value, expected_config, sizeof(expected_config)) == 0);
            }
            uintptr_t old_handle = (uintptr_t)handle.value;
            REQUIRE(gldDestroyDevice(handle.value) == 0);
            handle.value = NULL;
            REQUIRE(balanced(3) && config_guard(&config) && renderer_guard(&renderer));
            REQUIRE(memcmp(config.value, expected_config, sizeof(expected_config)) == 0);
            REQUIRE(memcmp(renderer.value, expected_renderer, sizeof(expected_renderer)) == 0);
            memset(&renderer, 0x5a, sizeof(renderer));
            REQUIRE(gldPopulateRendererInfo((void *)old_handle, renderer.value) != 0);
            REQUIRE(gldDestroyDevice((void *)old_handle) != 0);
            REQUIRE(bytes_are(&renderer, sizeof(renderer), 0x5a));
        }
        puts("PASS 64 real create/destroy cycles, 1024 identical queries, C/R/handle canaries");
        puts("PASS duplicate service slots 0/7/31 share ordinal 1, renderer ID and merged mask");
        memset(&config, 0xa5, sizeof(config));
        REQUIRE(gldCreateDevice(&handle.value, MASK, config.value) == 0);
        initialize(accelerator);
        vgpuGLDDeviceGetCounts(&counts);
        REQUIRE(counts.initialize_status == kIOReturnBusy && counts.devices == 1);
        REQUIRE(gldPopulateRendererInfo(handle.value, renderer.value) == 0);
        gldTerminateLibrary();
        handle.value = NULL;
        REQUIRE(balanced(0) && config_guard(&config));
        REQUIRE(memcmp(config.value, expected_config, sizeof(expected_config)) == 0);
        puts("PASS reinitialize with live device refused; terminate closes remaining device");
    }
    gldTerminateLibrary();
    gldTerminateLibrary();
    REQUIRE(balanced(0));
    mach_port_urefs_t final_refs = 0;
    REQUIRE(mach_port_get_refs(mach_task_self(), accelerator, MACH_PORT_RIGHT_SEND,
                               &final_refs) == KERN_SUCCESS);
    REQUIRE(final_refs == baseline_refs);
    printf("PASS service send refs baseline=%u final=%u\n", baseline_refs, final_refs);

    /* 错误服务初始化必须全回滚，包括较早已 retain 的正确槽。 */
    uint32_t invalid_services[32] = {0};
    uint8_t bytes[32] = {0};
    invalid_services[0] = accelerator;
    invalid_services[7] = framebuffer;
    gldInitializeLibrary(invalid_services, bytes, 0x81, 0, 0, 0);
    vgpuGLDDeviceGetCounts(&counts);
    REQUIRE(counts.initialize_status != 0 && counts.initialized_mask == 0 && balanced(0));
    memset(&config, 0xa5, sizeof(config));
    handle = new_handle();
    REQUIRE(gldCreateDevice(&handle.value, 1, config.value) != 0 && handle.value == NULL);
    REQUIRE(bytes_are(&config, sizeof(config), 0xa5));
    puts("PASS non-vgpuAccelerator rejected, partial initialization rolled back");

    if (!denied && !bad_type) {
        initialize(accelerator);
        REQUIRE(IOObjectRelease(accelerator) == KERN_SUCCESS);
        accelerator = IO_OBJECT_NULL;
        memset(&config, 0xa5, sizeof(config));
        REQUIRE(gldCreateDevice(&handle.value, MASK, config.value) == 0);
        REQUIRE(config_guard(&config));
        REQUIRE(gldDestroyDevice(handle.value) == 0);
        handle.value = NULL;
        gldTerminateLibrary();
        REQUIRE(balanced(0));
        puts("PASS caller service released before create; module retains remain usable");
    }
    REQUIRE(gldDestroyDevice(NULL) != 0);
    memset(&renderer, 0x5a, sizeof(renderer));
    REQUIRE(gldPopulateRendererInfo(NULL, renderer.value) != 0);
    REQUIRE(bytes_are(&renderer, sizeof(renderer), 0x5a));
    vgpuGLDDeviceGetCounts(&counts);
    printf("FINAL refs=%u devices=%u connections=%u retains=%" PRIu64
           " releases=%" PRIu64 " opens=%" PRIu64 " closes=%" PRIu64
           " cleanup=0x%08" PRIx32 "\n", counts.service_refs, counts.devices,
           counts.connections, counts.retains, counts.releases, counts.opens,
           counts.closes, (uint32_t)counts.cleanup_status);
    exit_status = EXIT_SUCCESS;
cleanup:
    gldTerminateLibrary();
    if (accelerator != IO_OBJECT_NULL && IOObjectRelease(accelerator) != KERN_SUCCESS)
        exit_status = EXIT_FAILURE;
    if (framebuffer != IO_OBJECT_NULL && IOObjectRelease(framebuffer) != KERN_SUCCESS)
        exit_status = EXIT_FAILURE;
    if (exit_status == EXIT_SUCCESS)
        puts("PASS all checks; standalone lifecycle only, no draw, no system GLD installation");
    return exit_status;
}
