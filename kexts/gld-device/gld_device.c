#include "gld_device.h"

#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct device {
    struct device *next;
    io_service_t service;
    io_connect_t connection;
    void *config; /* 调用者借用区；destroy/terminate 不写入、不释放。 */
    uint64_t registry_id;
    uint32_t mask;
    uint32_t renderer_id;
};
struct slot {
    io_service_t service;
    uint8_t byte; /* 仅复制；此字节的正式语义尚未闭合。 */
    uint32_t group_mask;
    uint32_t renderer_id;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct slot slots[32];
static struct device *devices;
static struct gld_device_counts counts;

static void cleanup_result(kern_return_t result)
{
    if (result != KERN_SUCCESS && counts.cleanup_status == KERN_SUCCESS)
        counts.cleanup_status = result;
}

static kern_return_t retain_service(io_service_t service)
{
    kern_return_t result = IOObjectRetain(service);
    if (result == KERN_SUCCESS) {
        ++counts.service_refs;
        ++counts.retains;
    }
    return result;
}

static void release_service(io_service_t service)
{
    kern_return_t result = IOObjectRelease(service);
    cleanup_result(result);
    if (result == KERN_SUCCESS) {
        --counts.service_refs;
        ++counts.releases;
    }
}

static void release_slots(void)
{
    for (uint32_t i = 0; i < 32; ++i)
        if (slots[i].service != IO_OBJECT_NULL)
            release_service(slots[i].service);
    memset(slots, 0, sizeof(slots));
    counts.initialized_mask = 0;
}

void gldInitializeLibrary(const uint32_t *services, const uint8_t *bytes,
                          uint32_t mask, uintptr_t flush_address,
                          uintptr_t bind_address, uint32_t flags)
{
    /* 未闭合的回调签名不保存、不调用；不伪造计算/渲染初始化。 */
    (void)flush_address;
    (void)bind_address;
    (void)flags;
    pthread_mutex_lock(&lock);
    if (devices != NULL) {
        counts.initialize_status = kIOReturnBusy;
        pthread_mutex_unlock(&lock);
        return;
    }
    release_slots();
    counts.initialize_status = kIOReturnBadArgument;
    if (services == NULL || bytes == NULL || mask == 0)
        goto done;
    uint32_t ordinal = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        if ((mask & (UINT32_C(1) << i)) == 0)
            continue;
        io_service_t service = services[i];
        if (service == IO_OBJECT_NULL || !IOObjectConformsTo(service, "vgpuAccelerator") ||
            !IOObjectConformsTo(service, "IOAccelerator")) {
            counts.initialize_status = kIOReturnNoDevice;
            goto failed;
        }
        counts.initialize_status = retain_service(service);
        if (counts.initialize_status != KERN_SUCCESS)
            goto failed;
        slots[i].service = service;
        slots[i].byte = bytes[i];
        uint32_t prior = 0;
        while (prior < i && (slots[prior].service == IO_OBJECT_NULL ||
               !IOObjectIsEqualTo(slots[prior].service, service)))
            ++prior;
        slots[i].renderer_id = prior < i ? slots[prior].renderer_id :
            (++ordinal << 24) | GLD_DEVICE_DRIVER_ID;
        for (uint32_t j = 0; j <= i; ++j) {
            if (slots[j].renderer_id == slots[i].renderer_id) {
                slots[j].group_mask |= UINT32_C(1) << i;
                slots[i].group_mask |= UINT32_C(1) << j;
            }
        }
    }
    counts.initialized_mask = mask;
    counts.initialize_status = KERN_SUCCESS;
    goto done;
failed:
    release_slots();
done:
    pthread_mutex_unlock(&lock);
}

/* 已验证 selector 1：generation=0 恒返回 NotReady，不读取像素、不提交 GPU。
 * 这只证明已打开的私有连接可响应，不表示 GPU 渲染资源就绪。 */
static kern_return_t check_connection(const struct device *device,
                                      int32_t *liveness_status)
{
    uint64_t input[2] = {0, 0};
    uint32_t pixels[256] = {0};
    size_t size = sizeof(pixels);
    kern_return_t result = IOConnectCallMethod(device->connection, 1, input, 2,
        NULL, 0, NULL, NULL, pixels, &size);
    *liveness_status = result;
    if (result == kIOReturnNotReady)
        return KERN_SUCCESS;
    return result == KERN_SUCCESS ? kIOReturnError : result;
}

static void close_connection(io_connect_t connection)
{
    kern_return_t result = IOServiceClose(connection);
    cleanup_result(result);
    if (result == KERN_SUCCESS) {
        --counts.connections;
        ++counts.closes;
    }
}

int32_t gldCreateDevice(void **device_out, uint32_t mask, void *config_out)
{
    if (device_out == NULL)
        return kIOReturnBadArgument;
    *device_out = NULL;
    if (config_out == NULL)
        return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    kern_return_t result = kIOReturnNoDevice;
    if (mask == 0 || (mask & counts.initialized_mask) != mask)
        goto done;
    uint32_t i = 0;
    while ((mask & (UINT32_C(1) << i)) == 0)
        ++i;
    if ((mask & slots[i].group_mask) != mask)
        goto done;
    struct device pending = {0};
    pending.service = slots[i].service;
    pending.mask = slots[i].group_mask;
    pending.renderer_id = slots[i].renderer_id;
    pending.config = config_out;
    result = retain_service(pending.service);
    if (result != KERN_SUCCESS)
        goto done;
    result = IOServiceOpen(pending.service, mach_task_self(),
                           GLD_DEVICE_OPEN_TYPE, &pending.connection);
    counts.backend_status = result;
    if (pending.connection != IO_OBJECT_NULL) {
        ++counts.connections;
        ++counts.opens;
    }
    if (result != KERN_SUCCESS || pending.connection == IO_OBJECT_NULL) {
        if (result == KERN_SUCCESS)
            result = kIOReturnNotOpen;
        goto rollback;
    }
    int32_t liveness_status = KERN_SUCCESS;
    result = check_connection(&pending, &liveness_status);
    if (result != KERN_SUCCESS)
        goto rollback;
    result = IORegistryEntryGetRegistryEntryID(pending.service, &pending.registry_id);
    if (result != KERN_SUCCESS)
        goto rollback;
    int64_t page_size = getpagesize();
    if (page_size <= 0) {
        result = kIOReturnError;
        goto rollback;
    }
    struct device *device = malloc(sizeof(*device));
    if (device == NULL) {
        result = kIOReturnNoMemory;
        goto rollback;
    }
    *device = pending;
    device->next = devices;
    devices = device;
    ++counts.devices;
    /* 只发布已知字段；未知配置/能力清零，包括 C+6/C+7。 */
    memset(config_out, 0, GLD_DEVICE_CONFIG_BYTES);
    memcpy((uint8_t *)config_out + 0x180, &pending.registry_id, 8);
    memcpy((uint8_t *)config_out + 0x188, &page_size, 8);
    *device_out = device;
    result = KERN_SUCCESS;
    goto done;
rollback:
    if (pending.connection != IO_OBJECT_NULL)
        close_connection(pending.connection);
    release_service(pending.service);
done:
    pthread_mutex_unlock(&lock);
    return result;
}

static struct device **find_device(void *opaque)
{
    struct device **cursor = &devices;
    while (*cursor != NULL && *cursor != opaque)
        cursor = &(*cursor)->next;
    return cursor;
}

static void destroy_device(struct device **cursor)
{
    struct device *device = *cursor;
    *cursor = device->next;
    close_connection(device->connection);
    release_service(device->service);
    --counts.devices;
    free(device);
}

int32_t gldDestroyDevice(void *opaque)
{
    pthread_mutex_lock(&lock);
    struct device **cursor = find_device(opaque);
    kern_return_t result = kIOReturnBadArgument;
    if (*cursor != NULL) {
        destroy_device(cursor);
        result = counts.cleanup_status;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

int32_t gldPopulateRendererInfo(void *opaque, void *renderer_out)
{
    if (renderer_out == NULL)
        return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    struct device *device = *find_device(opaque);
    kern_return_t result = kIOReturnBadArgument;
    if (device != NULL) {
        int32_t liveness_status = KERN_SUCCESS;
        result = check_connection(device, &liveness_status);
        if (result == KERN_SUCCESS) {
            uint8_t renderer[GLD_DEVICE_RENDERER_BYTES] = {0};
            memcpy(renderer + 8, &device->renderer_id, 4);
            memcpy(renderer + 0x24, &device->mask, 4);
            memcpy(renderer + 0x34, &device->registry_id, 8);
            memcpy(renderer_out, renderer, sizeof(renderer));
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

int32_t vgpuGLDDeviceGetStatus(void *opaque, struct gld_device_status *out)
{
    if (out == NULL)
        return kIOReturnBadArgument;
    pthread_mutex_lock(&lock);
    struct device *device = *find_device(opaque);
    kern_return_t result = kIOReturnBadArgument;
    if (device != NULL) {
        struct gld_device_status status = {0};
        result = check_connection(device, &status.liveness_status);
        if (result == KERN_SUCCESS) {
            status.registry_id = device->registry_id;
            status.renderer_id = device->renderer_id;
            status.display_mask = device->mask;
            status.connection_type = GLD_DEVICE_OPEN_TYPE;
            status.connected = 1;
            *out = status;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

void vgpuGLDDeviceGetCounts(struct gld_device_counts *out)
{
    if (out != NULL) {
        pthread_mutex_lock(&lock);
        *out = counts;
        pthread_mutex_unlock(&lock);
    }
}

void gldTerminateLibrary(void)
{
    pthread_mutex_lock(&lock);
    while (devices != NULL)
        destroy_device(&devices);
    release_slots();
    pthread_mutex_unlock(&lock);
}
