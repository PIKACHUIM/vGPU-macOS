/* 本地故障测试：替换 transport 边界，不访问设备、不占 GPU 槽位。 */
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, allocations, mutexes, services, connections;
static unsigned allocation_calls, fail_allocation, open_calls, close_calls, registry_calls, class_calls;
static unsigned mode;
static bool fail_mutex, fail_close;
static const uint64_t target_id = UINT64_C(0x123456789abcdef0);
static uint64_t requested_id;
static int dictionary;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #expr); } } while (0)

static void *test_calloc(size_t count, size_t size)
{
    if (++allocation_calls == fail_allocation) return NULL;
    void *p = calloc(count, size);
    if (p) ++allocations;
    return p;
}
static void test_free(void *p)
{
    if (p) { CHECK(allocations != 0); --allocations; }
    free(p);
}
static int test_mutex_init(pthread_mutex_t *lock, const pthread_mutexattr_t *attr)
{
    if (fail_mutex) return 1;
    int rc = pthread_mutex_init(lock, attr);
    if (!rc) ++mutexes;
    return rc;
}
static int test_mutex_destroy(pthread_mutex_t *lock)
{
    CHECK(mutexes != 0);
    --mutexes;
    return pthread_mutex_destroy(lock);
}
static CFMutableDictionaryRef test_registry_matching(uint64_t id)
{
    ++registry_calls;
    requested_id = id;
    return mode == 1 ? NULL : (CFMutableDictionaryRef)(void *)&dictionary;
}
static CFMutableDictionaryRef test_class_matching(const char *name)
{
    ++class_calls;
    CHECK(strcmp(name, "vgpuAccelerator") == 0);
    return (CFMutableDictionaryRef)(void *)&dictionary;
}
static io_service_t test_matching_service(mach_port_t port, CFDictionaryRef matching)
{
    CHECK(port == MACH_PORT_NULL && matching == (CFDictionaryRef)(void *)&dictionary);
    if (mode == 2) return IO_OBJECT_NULL;
    ++services;
    return (io_service_t)17;
}
static boolean_t test_conforms(io_object_t service, const io_name_t name)
{
    CHECK(service == 17 && strcmp(name, "vgpuAccelerator") == 0);
    return mode != 3;
}
static kern_return_t test_get_id(io_registry_entry_t service, uint64_t *id)
{
    CHECK(service == 17);
    if (mode == 4) return kIOReturnIOError;
    *id = mode == 5 ? target_id + 1 : target_id;
    return kIOReturnSuccess;
}
static kern_return_t test_object_release(io_object_t service)
{
    CHECK(service == 17 && services == 1);
    --services;
    return kIOReturnSuccess;
}
static kern_return_t test_open(io_service_t service, task_port_t task, uint32_t type, io_connect_t *out)
{
    CHECK(service == 17 && task == mach_task_self() && type == UINT32_C(0x56475055));
    ++open_calls;
    if (mode == 6) return kIOReturnNotPrivileged;
    if (mode == 7) return kIOReturnSuccess;
    ++connections;
    *out = (io_connect_t)23;
    return mode == 8 ? kIOReturnIOError : kIOReturnSuccess;
}
static kern_return_t test_close(io_connect_t connection)
{
    CHECK(connection == 23 && connections == 1);
    ++close_calls;
    if (fail_close) return kIOReturnBusy;
    --connections;
    return kIOReturnSuccess;
}
#define calloc test_calloc
#define free test_free
#define pthread_mutex_init test_mutex_init
#define pthread_mutex_destroy test_mutex_destroy
#define IORegistryEntryIDMatching test_registry_matching
#define IOServiceMatching test_class_matching
#define IOServiceGetMatchingService test_matching_service
#define IOObjectConformsTo test_conforms
#define IORegistryEntryGetRegistryEntryID test_get_id
#define IOObjectRelease test_object_release
#define IOServiceOpen test_open
#define IOServiceClose test_close
#include "vgpu_surface.c"

static void clean(void)
{
    CHECK(!allocations && !mutexes && !services && !connections);
}
int main(void)
{
    vgpu_surface_owner *owner = NULL;
    CHECK(vgpu_surface_owner_open_registry(target_id, NULL) == kIOReturnBadArgument);
    CHECK(vgpu_surface_owner_open_registry(0, &owner) == kIOReturnBadArgument && !owner);
    CHECK(!registry_calls && !class_calls && !open_calls);
    for (unsigned n = 1; n <= 2; ++n) {
        allocation_calls = 0; fail_allocation = n;
        CHECK(vgpu_surface_owner_open_registry(target_id, &owner) == kIOReturnNoMemory && !owner);
        clean();
    }
    fail_allocation = 0; fail_mutex = true;
    CHECK(vgpu_surface_owner_open_registry(target_id, &owner) == kIOReturnNoResources && !owner);
    clean(); fail_mutex = false;
    const IOReturn expected[] = { kIOReturnSuccess, kIOReturnNoMemory, kIOReturnNotFound,
        kIOReturnBadArgument, kIOReturnIOError, kIOReturnNotFound, kIOReturnNotPrivileged,
        kIOReturnIOError, kIOReturnIOError };
    for (mode = 1; mode <= 8; ++mode) {
        unsigned before = open_calls;
        owner = (vgpu_surface_owner *)(void *)&dictionary;
        CHECK(vgpu_surface_owner_open_registry(target_id, &owner) == expected[mode] && !owner);
        CHECK(requested_id == target_id && class_calls == 0);
        CHECK(open_calls == before + (mode >= 6 ? 1U : 0U));
        clean();
    }
    mode = 0;
    CHECK(vgpu_surface_owner_open_registry(target_id, &owner) == kIOReturnSuccess && owner);
    CHECK(vgpu_surface_owner_retain(owner) == kIOReturnSuccess);
    unsigned closes = close_calls;
    CHECK(vgpu_surface_owner_release(owner) == kIOReturnSuccess && close_calls == closes);
    fail_close = true;
    CHECK(vgpu_surface_owner_release(owner) == kIOReturnBusy);
    CHECK(owner->transport->references == 1 && connections == 1 && allocations == 2);
    fail_close = false;
    CHECK(vgpu_surface_owner_release(owner) == kIOReturnSuccess);
    clean();
    unsigned registry_before = registry_calls;
    CHECK(vgpu_surface_owner_open(&owner) == kIOReturnSuccess);
    CHECK(class_calls == 1 && registry_calls == registry_before);
    CHECK(vgpu_surface_owner_release(owner) == kIOReturnSuccess);
    clean();
    vgpu_surface *surface = NULL;
    CHECK(vgpu_surface_open(256, 256, VGPU_SURFACE_BGRX8, &surface) == kIOReturnSuccess);
    CHECK(class_calls == 2 && registry_calls == registry_before);
    vgpu_surface_release(surface);
    clean();
    printf("RESULT checks=%u failures=%u allocations=%u mutexes=%u services=%u connections=%u\n",
           checks, failures, allocations, mutexes, services, connections);
    return failures ? 1 : 0;
}
