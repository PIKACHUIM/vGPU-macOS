#include "vgpu_surface.h"
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct vgpu_surface {
    pthread_mutex_t lock;
    io_connect_t connection;
    size_t references;
};
struct clear_input {
    uint32_t version, byte_size;
    float rgba[4];
};
_Static_assert(sizeof(float) == 4, "selector requires float32");
_Static_assert(sizeof(struct clear_input) == 24, "selector requires 24 bytes");

static bool add_size(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}
static bool mul_size(size_t a, size_t b, size_t *out)
{
    if (a && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

IOReturn vgpu_surface_open(uint32_t width, uint32_t height, uint32_t backing,
                           vgpu_surface **out)
{
    if (!out) return kIOReturnBadArgument;
    *out = NULL;
    if (width != 256 || height != 256 || backing != VGPU_SURFACE_BGRX8)
        return kIOReturnUnsupported;
    vgpu_surface *s = calloc(1, sizeof(*s));
    if (!s) return kIOReturnNoMemory;
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        free(s);
        return kIOReturnNoResources;
    }
    CFMutableDictionaryRef matching = IOServiceMatching("vgpuAccelerator");
    IOReturn result = kIOReturnNoMemory;
    if (matching) {
        io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, matching);
        result = kIOReturnNotFound;
        if (service) {
            result = IOServiceOpen(service, mach_task_self(), UINT32_C(0x56475055),
                                   &s->connection);
            IOObjectRelease(service);
            if (result == kIOReturnSuccess && !s->connection)
                result = kIOReturnIOError;
        }
    }
    if (result != kIOReturnSuccess) {
        if (s->connection) IOServiceClose(s->connection);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return result;
    }
    s->references = 1;
    *out = s;
    return kIOReturnSuccess;
}

IOReturn vgpu_surface_retain(vgpu_surface *s)
{
    if (!s) return kIOReturnBadArgument;
    pthread_mutex_lock(&s->lock);
    IOReturn result = kIOReturnNoResources;
    if (s->references != SIZE_MAX) {
        ++s->references;
        result = kIOReturnSuccess;
    }
    pthread_mutex_unlock(&s->lock);
    return result;
}
IOReturn vgpu_surface_close(vgpu_surface *s)
{
    if (!s) return kIOReturnBadArgument;
    pthread_mutex_lock(&s->lock);
    IOReturn result = kIOReturnSuccess;
    if (s->connection) {
        result = IOServiceClose(s->connection);
        s->connection = IO_OBJECT_NULL;
    }
    pthread_mutex_unlock(&s->lock);
    return result;
}
void vgpu_surface_release(vgpu_surface *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    bool last = --s->references == 0;
    pthread_mutex_unlock(&s->lock);
    if (last) {
        (void)vgpu_surface_close(s);
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
}
IOReturn vgpu_surface_clear(vgpu_surface *s, const float rgba[4], uint64_t *generation)
{
    if (!s || !rgba || !generation) return kIOReturnBadArgument;
    struct clear_input input = { 1, 24, { 0, 0, 0, 0 } };
    memcpy(input.rgba, rgba, sizeof(input.rgba));
    for (size_t i = 0; i < 4; ++i) {
        uint32_t bits;
        memcpy(&bits, &input.rgba[i], sizeof(bits));
        uint32_t magnitude = bits & UINT32_C(0x7fffffff);
        if (magnitude > UINT32_C(0x3f800000) ||
            ((bits & UINT32_C(0x80000000)) && magnitude))
            return kIOReturnBadArgument;
    }
    uint64_t output[3] = { 0, 0, 0 };
    uint32_t count = 3;
    pthread_mutex_lock(&s->lock);
    IOReturn result = kIOReturnNotOpen;
    if (s->connection)
        result = IOConnectCallMethod(s->connection, 3, NULL, 0, &input,
                                      sizeof(input), output, &count, NULL, NULL);
    pthread_mutex_unlock(&s->lock);
    if (result != kIOReturnSuccess) return result;
    if (count != 3 || !output[0] || output[1] != 256 || output[2] != 256)
        return kIOReturnIOError;
    *generation = output[0];
    return kIOReturnSuccess;
}

static IOReturn read_row(io_connect_t c, uint64_t generation, uint32_t y,
                          unsigned char row[1024])
{
    uint64_t input[2] = { generation, y };
    size_t size = 1024;
    IOReturn result = IOConnectCallMethod(c, 1, input, 2, NULL, 0,
                                          NULL, NULL, row, &size);
    if (result != kIOReturnSuccess) return result;
    return size == 1024 ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn vgpu_surface_read(vgpu_surface *s, uint64_t generation,
                           vgpu_surface_rect rect, uint32_t format,
                           vgpu_surface_pack pack, void *destination, size_t capacity)
{
    if (!s || !generation || !destination) return kIOReturnBadArgument;
    if (format != VGPU_SURFACE_RGBA8_OUTPUT) return kIOReturnUnsupported;
    if (!rect.width || !rect.height || rect.x >= 256 || rect.y >= 256 ||
        rect.width > 256 - rect.x || rect.height > 256 - rect.y)
        return kIOReturnBadArgument;
    if (pack.alignment != 1 && pack.alignment != 2 &&
        pack.alignment != 4 && pack.alignment != 8) return kIOReturnBadArgument;
    size_t stride = pack.stride_bytes, row_bytes = (size_t)rect.width * 4;
    size_t offset, skip, last, end, temporary_size;
    if (!stride) {
        if (!mul_size(pack.row_length ? pack.row_length : rect.width, 4, &stride) ||
            !add_size(stride, pack.alignment - 1, &stride)) return kIOReturnBadArgument;
        stride &= ~(pack.alignment - 1);
    }
    if (!mul_size(pack.skip_rows, stride, &offset) ||
        !mul_size(pack.skip_pixels, 4, &skip) || !add_size(offset, skip, &offset) ||
        !mul_size((size_t)rect.height - 1, stride, &last) ||
        !add_size(offset, last, &end) || !add_size(end, row_bytes, &end) ||
        !mul_size(row_bytes, rect.height, &temporary_size) || end > capacity ||
        end > UINTPTR_MAX - (uintptr_t)destination) return kIOReturnBadArgument;
    unsigned char *temporary = malloc(temporary_size);
    if (!temporary) return kIOReturnNoMemory;
    unsigned char row[1024];
    IOReturn result = kIOReturnNotOpen;
    /* 仅串行化本连接的 IO；其他连接提交仍由 generation 校验检测。 */
    pthread_mutex_lock(&s->lock);
    if (s->connection) {
        for (uint32_t j = 0; j < rect.height; ++j) {
            result = read_row(s->connection, generation, rect.y + j, row);
            if (result != kIOReturnSuccess) break;
            unsigned char *out = temporary + (size_t)j * row_bytes;
            for (uint32_t i = 0; i < rect.width; ++i) {
                size_t source = (size_t)(rect.x + i) * 4;
                out[i * 4] = row[source + 2];
                out[i * 4 + 1] = row[source + 1];
                out[i * 4 + 2] = row[source];
                out[i * 4 + 3] = 255;
            }
        }
        /* 提交点前再验证，避免最后一次转换期间发生的提交被遗漏。 */
        if (result == kIOReturnSuccess)
            result = read_row(s->connection, generation, rect.y, row);
    }
    pthread_mutex_unlock(&s->lock);
    if (result == kIOReturnSuccess) {
        for (uint32_t j = 0; j < rect.height; ++j)
            memcpy((unsigned char *)destination + offset + (size_t)j * stride,
                   temporary + (size_t)j * row_bytes, row_bytes);
    }
    free(temporary);
    return result;
}
