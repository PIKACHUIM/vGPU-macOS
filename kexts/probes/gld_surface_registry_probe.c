/* 独立真实设备测试：最多持有一个 BGRA image，结束立即释放。 */
#include "vgpu_surface.h"
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #expr); } } while (0)

int main(int argc, char **argv)
{
    bool denied = argc == 2 && strcmp(argv[1], "--expect-denied") == 0;
    if (argc != 1 && !denied) return 2;
    CHECK(denied ? geteuid() != 0 : geteuid() == 0);
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL,
                                                       IOServiceMatching("vgpuAccelerator"));
    if (!service) return 3;
    uint64_t id = 0;
    IOReturn rc = IORegistryEntryGetRegistryEntryID(service, &id);
    IOObjectRelease(service);
    if (rc || !id) return 4;
    printf("registry_id=0x%llx\n", (unsigned long long)id);
    vgpu_surface_owner *owner = NULL;
    CHECK(vgpu_surface_owner_open_registry(id, NULL) == kIOReturnBadArgument);
    CHECK(vgpu_surface_owner_open_registry(0, &owner) == kIOReturnBadArgument && !owner);
    CHECK(vgpu_surface_owner_open_registry(UINT64_MAX, &owner) == kIOReturnNotFound && !owner);
    service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOResources"));
    CHECK(service != IO_OBJECT_NULL);
    if (service) {
        uint64_t wrong_id = 0;
        CHECK(IORegistryEntryGetRegistryEntryID(service, &wrong_id) == kIOReturnSuccess);
        IOObjectRelease(service);
        CHECK(vgpu_surface_owner_open_registry(wrong_id, &owner) == kIOReturnBadArgument && !owner);
    }
    rc = vgpu_surface_owner_open_registry(id, &owner);
    if (denied) {
        CHECK(rc == kIOReturnNotPrivileged && !owner);
        if (owner) CHECK(vgpu_surface_owner_release(owner) == kIOReturnSuccess);
    } else {
        CHECK(rc == kIOReturnSuccess && owner);
        if (owner) {
            vgpu_surface_image *image = NULL;
            rc = vgpu_surface_image_create(owner, 256, 256, VGPU_SURFACE_BGRA8, &image);
            CHECK(rc == kIOReturnSuccess && image);
            if (!rc && image) {
                const float rgba[4] = {0.25f, 0.5f, 0.75f, 0.5f};
                uint64_t generation = 0;
                CHECK(vgpu_surface_image_clear(image, rgba, &generation) == kIOReturnSuccess);
                unsigned char pixels[256 * 256 * 4];
                memset(pixels, 0, sizeof(pixels));
                CHECK(vgpu_surface_image_read(image, generation,
                    (vgpu_surface_rect){0, 0, 256, 256}, VGPU_SURFACE_RGBA8_OUTPUT,
                    (vgpu_surface_pack){0, 0, 0, 0, 4}, pixels, sizeof(pixels)) == kIOReturnSuccess);
                bool match = true;
                for (size_t i = 0; i < sizeof(pixels); i += 4)
                    if (pixels[i] != 64 || pixels[i + 1] != 128 ||
                        pixels[i + 2] != 191 || pixels[i + 3] != 128) match = false;
                CHECK(match);
            }
            if (image) CHECK(vgpu_surface_image_release(image) == kIOReturnSuccess);
            CHECK(vgpu_surface_owner_release(owner) == kIOReturnSuccess);
            owner = NULL;
        }
    }
    rc = vgpu_surface_owner_open(&owner);
    CHECK(denied ? rc == kIOReturnNotPrivileged && !owner : rc == kIOReturnSuccess && owner);
    if (owner) CHECK(vgpu_surface_owner_release(owner) == kIOReturnSuccess);
    printf("RESULT checks=%u failures=%u mode=%s\n", checks, failures, denied ? "nonroot" : "root");
    return failures ? 1 : 0;
}
