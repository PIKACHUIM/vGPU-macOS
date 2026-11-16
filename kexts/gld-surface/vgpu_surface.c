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

static IOReturn surface_open_transport(uint64_t registry_id, bool exact,
                                       vgpu_surface **out)
{
    *out = NULL;
    vgpu_surface *s = calloc(1, sizeof(*s));
    if (!s) return kIOReturnNoMemory;
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        free(s);
        return kIOReturnNoResources;
    }
    CFMutableDictionaryRef matching = exact ? IORegistryEntryIDMatching(registry_id) :
                                             IOServiceMatching("vgpuAccelerator");
    IOReturn result = kIOReturnNoMemory;
    if (matching) {
        io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, matching);
        result = kIOReturnNotFound;
        if (service) {
            result = kIOReturnSuccess;
            if (exact) {
                uint64_t actual_id = 0;
                if (!IOObjectConformsTo(service, "vgpuAccelerator"))
                    result = kIOReturnBadArgument;
                else {
                    result = IORegistryEntryGetRegistryEntryID(service, &actual_id);
                    if (result == kIOReturnSuccess && actual_id != registry_id)
                        result = kIOReturnNotFound;
                }
            }
            if (result == kIOReturnSuccess)
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

IOReturn vgpu_surface_open(uint32_t width, uint32_t height, uint32_t backing,
                           vgpu_surface **out)
{
    if (!out) return kIOReturnBadArgument;
    *out = NULL;
    if (width != 256 || height != 256 || backing != VGPU_SURFACE_BGRX8)
        return kIOReturnUnsupported;
    return surface_open_transport(0, false, out);
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
        !mul_size(row_bytes, rect.height, &temporary_size) || !temporary_size || end > capacity ||
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

struct vgpu_surface_owner {
    vgpu_surface *transport;
};
struct vgpu_surface_image {
    pthread_mutex_t lock;
    size_t references;
    vgpu_surface_owner *owner;
    uint64_t handle;
    vgpu_surface_image_state state;
    IOReturn release_error;
};
struct image_create_request {
    uint32_t version, byte_size, width, height, format, flags, samples, reserved;
};
struct image_info_response {
    uint32_t version, byte_size;
    uint64_t handle, generation;
    uint32_t width, height, format, row_bytes, backing_bytes, flags;
    uint64_t reserved[2];
};
struct image_handle_request {
    uint32_t version, byte_size;
    uint64_t handle, reserved;
};
struct image_release_response {
    uint32_t version, byte_size;
    uint64_t handle;
    uint32_t flags, reserved;
};
struct image_clear_request {
    uint32_t version, byte_size, opcode, flags;
    uint64_t handle, expected_generation;
    uint32_t rgba_bits[4], vertices_bits[12];
};
struct image_clear_response {
    uint32_t version, byte_size;
    uint64_t handle, generation;
    uint32_t width, height;
};
struct image_read_request {
    uint32_t version, byte_size;
    uint64_t handle, generation;
    uint32_t row, reserved;
};
struct image_read_response {
    uint32_t version, byte_size;
    uint64_t handle, generation;
    uint32_t row, row_bytes;
    unsigned char pixels[1024];
};
_Static_assert(sizeof(struct image_create_request) == 32, "create ABI");
_Static_assert(sizeof(struct image_info_response) == 64 &&
               offsetof(struct image_info_response, handle) == 8 &&
               offsetof(struct image_info_response, reserved) == 48, "info ABI");
_Static_assert(sizeof(struct image_handle_request) == 24, "handle ABI");
_Static_assert(sizeof(struct image_release_response) == 24, "release ABI");
_Static_assert(sizeof(struct image_clear_request) == 96 &&
               offsetof(struct image_clear_request, rgba_bits) == 32 &&
               offsetof(struct image_clear_request, vertices_bits) == 48, "clear ABI");
_Static_assert(sizeof(struct image_clear_response) == 32, "clear response ABI");
_Static_assert(sizeof(struct image_read_request) == 32, "read ABI");
_Static_assert(sizeof(struct image_read_response) == 1056 &&
               offsetof(struct image_read_response, pixels) == 32, "read response ABI");

static bool memory_overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y) return true;
    return x < y + bn && y < x + an;
}
static bool owner_output_overlap(vgpu_surface_owner *o, const void *p, size_t n)
{
    return memory_overlap(p, n, o, sizeof(*o)) ||
           memory_overlap(p, n, o->transport, sizeof(*o->transport));
}
static bool image_output_overlap(vgpu_surface_image *s, const void *p, size_t n)
{
    return memory_overlap(p, n, s, sizeof(*s)) || owner_output_overlap(s->owner, p, n);
}
static IOReturn owner_open(uint64_t registry_id, bool exact, vgpu_surface_owner **out)
{
    if (!out) return kIOReturnBadArgument;
    *out = NULL;
    if (exact && !registry_id) return kIOReturnBadArgument;
    vgpu_surface_owner *o = calloc(1, sizeof(*o));
    if (!o) return kIOReturnNoMemory;
    IOReturn rc = surface_open_transport(registry_id, exact, &o->transport);
    if (rc) { free(o); return rc; }
    *out = o;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_owner_open(vgpu_surface_owner **out)
{
    return owner_open(0, false, out);
}
IOReturn vgpu_surface_owner_open_registry(uint64_t registry_id, vgpu_surface_owner **out)
{
    return owner_open(registry_id, true, out);
}
IOReturn vgpu_surface_owner_retain(vgpu_surface_owner *o)
{
    return o ? vgpu_surface_retain(o->transport) : kIOReturnBadArgument;
}
IOReturn vgpu_surface_owner_release(vgpu_surface_owner *o)
{
    if (!o) return kIOReturnSuccess;
    vgpu_surface *t = o->transport;
    pthread_mutex_lock(&t->lock);
    IOReturn rc = kIOReturnSuccess;
    if (t->references == 1 && t->connection) rc = IOServiceClose(t->connection);
    bool last = false;
    if (!rc) last = --t->references == 0;
    pthread_mutex_unlock(&t->lock);
    if (last) {
        pthread_mutex_destroy(&t->lock);
        free(t);
        free(o);
    }
    return rc;
}
/* 固定锁序：image -> owner transport；不从 owner 反向获取 image 锁。 */
static IOReturn image_call(vgpu_surface_image *s, uint32_t selector,
                           const void *in, size_t in_size, void *out, size_t out_size)
{
    vgpu_surface *t = s->owner->transport;
    size_t size = out_size;
    pthread_mutex_lock(&t->lock);
    IOReturn rc = IOConnectCallMethod(t->connection, selector, NULL, 0, in, in_size,
                                     NULL, NULL, out, &size);
    pthread_mutex_unlock(&t->lock);
    return rc ? rc : size == out_size ? kIOReturnSuccess : kIOReturnIOError;
}
static bool image_info_valid(const struct image_info_response *r, uint64_t handle)
{
    return r->version == 1 && r->byte_size == sizeof(*r) && r->handle &&
           (!handle || r->handle == handle) && r->generation && r->width == 256 &&
           r->height == 256 && r->format == VGPU_SURFACE_BGRA8 &&
           r->row_bytes == 1024 && r->backing_bytes == 262144 && r->flags == 7 &&
           !r->reserved[0] && !r->reserved[1];
}
static IOReturn image_close_locked(vgpu_surface_image *s)
{
    if (s->state == VGPU_SURFACE_IMAGE_CLOSED) return kIOReturnSuccess;
    struct image_handle_request in = {1, sizeof(in), s->handle, 0};
    struct image_release_response out = {0};
    IOReturn rc = image_call(s, 5, &in, sizeof(in), &out, sizeof(out));
    if (!rc && (out.version != 1 || out.byte_size != sizeof(out) ||
                out.handle != s->handle || out.flags != 1 || out.reserved)) rc = kIOReturnIOError;
    if (rc == kIOReturnNotFound) rc = kIOReturnSuccess;
    s->release_error = rc;
    s->state = rc ? VGPU_SURFACE_IMAGE_RELEASE_FAILED : VGPU_SURFACE_IMAGE_CLOSED;
    return rc;
}
IOReturn vgpu_surface_image_create(vgpu_surface_owner *o, uint32_t width,
                                   uint32_t height, uint32_t backing, vgpu_surface_image **out)
{
    if (!o || !out || owner_output_overlap(o, out, sizeof(*out))) return kIOReturnBadArgument;
    *out = NULL;
    if (width != 256 || height != 256 || backing != VGPU_SURFACE_BGRA8) return kIOReturnUnsupported;
    vgpu_surface_image *s = calloc(1, sizeof(*s));
    if (!s) return kIOReturnNoMemory;
    if (pthread_mutex_init(&s->lock, NULL)) { free(s); return kIOReturnNoResources; }
    IOReturn rc = vgpu_surface_owner_retain(o);
    if (rc) { pthread_mutex_destroy(&s->lock); free(s); return rc; }
    s->owner = o;
    s->references = 1;
    s->state = VGPU_SURFACE_IMAGE_LIVE;
    struct image_create_request in = {1, sizeof(in), width, height, backing, 0, 0, 0};
    struct image_info_response info = {0};
    rc = image_call(s, 4, &in, sizeof(in), &info, sizeof(info));
    s->handle = info.handle;
    if (!rc && !image_info_valid(&info, 0)) rc = kIOReturnIOError;
    if (rc) {
        /* 响应已给出 handle 时，不能把待释放资源或失败状态藏在析构中。 */
        if (s->handle && image_close_locked(s)) { *out = s; return rc; }
        (void)vgpu_surface_owner_release(o); /* 调用者仍持 owner 引用，不会关闭连接。 */
        pthread_mutex_destroy(&s->lock);
        free(s);
        return rc;
    }
    *out = s;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_retain(vgpu_surface_image *s)
{
    if (!s) return kIOReturnBadArgument;
    pthread_mutex_lock(&s->lock);
    IOReturn rc = kIOReturnNoResources;
    if (s->references != SIZE_MAX) { ++s->references; rc = kIOReturnSuccess; }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
IOReturn vgpu_surface_image_close(vgpu_surface_image *s)
{
    if (!s) return kIOReturnBadArgument;
    pthread_mutex_lock(&s->lock);
    IOReturn rc = image_close_locked(s);
    pthread_mutex_unlock(&s->lock);
    return rc;
}
IOReturn vgpu_surface_image_release(vgpu_surface_image *s)
{
    if (!s) return kIOReturnSuccess;
    pthread_mutex_lock(&s->lock);
    IOReturn rc = kIOReturnSuccess;
    if (s->references == 1) {
        rc = image_close_locked(s);
        if (!rc) rc = vgpu_surface_owner_release(s->owner);
        s->release_error = rc;
    }
    bool last = !rc && --s->references == 0;
    pthread_mutex_unlock(&s->lock);
    if (last) { pthread_mutex_destroy(&s->lock); free(s); }
    return rc;
}
IOReturn vgpu_surface_image_status(vgpu_surface_image *s, vgpu_surface_image_state *state,
                                   IOReturn *error)
{
    if (!s || !state || !error || image_output_overlap(s, state, sizeof(*state)) ||
        image_output_overlap(s, error, sizeof(*error)) ||
        memory_overlap(state, sizeof(*state), error, sizeof(*error))) return kIOReturnBadArgument;
    pthread_mutex_lock(&s->lock);
    *state = s->state;
    *error = s->release_error;
    pthread_mutex_unlock(&s->lock);
    return kIOReturnSuccess;
}
static IOReturn image_info_locked(vgpu_surface_image *s, struct image_info_response *info)
{
    if (s->state != VGPU_SURFACE_IMAGE_LIVE) return kIOReturnNotOpen;
    struct image_handle_request in = {1, sizeof(in), s->handle, 0};
    IOReturn rc = image_call(s, 8, &in, sizeof(in), info, sizeof(*info));
    return rc ? rc : image_info_valid(info, s->handle) ? kIOReturnSuccess : kIOReturnIOError;
}
IOReturn vgpu_surface_image_info(vgpu_surface_image *s, uint64_t *generation)
{
    if (!s || !generation || image_output_overlap(s, generation, sizeof(*generation)))
        return kIOReturnBadArgument;
    struct image_info_response info = {0};
    pthread_mutex_lock(&s->lock);
    IOReturn rc = image_info_locked(s, &info);
    if (!rc) *generation = info.generation;
    pthread_mutex_unlock(&s->lock);
    return rc;
}
IOReturn vgpu_surface_image_clear(vgpu_surface_image *s, const float rgba[4], uint64_t *generation)
{
    if (!s || !rgba || !generation || image_output_overlap(s, generation, sizeof(*generation)))
        return kIOReturnBadArgument;
    struct image_clear_request in = {0};
    in.version = 1; in.byte_size = sizeof(in); in.opcode = 1;
    memcpy(in.rgba_bits, rgba, sizeof(in.rgba_bits));
    for (size_t i = 0; i < 4; ++i) {
        uint32_t m = in.rgba_bits[i] & UINT32_C(0x7fffffff);
        if (m > UINT32_C(0x3f800000) || ((in.rgba_bits[i] & UINT32_C(0x80000000)) && m))
            return kIOReturnBadArgument;
    }
    struct image_info_response info = {0};
    struct image_clear_response out = {0};
    pthread_mutex_lock(&s->lock);
    IOReturn rc = image_info_locked(s, &info);
    if (!rc) {
        in.handle = s->handle; in.expected_generation = info.generation;
        rc = image_call(s, 6, &in, sizeof(in), &out, sizeof(out));
        if (!rc && (out.version != 1 || out.byte_size != sizeof(out) ||
                    out.handle != s->handle || info.generation == UINT64_MAX ||
                    out.generation != info.generation + 1 || out.width != 256 || out.height != 256))
            rc = kIOReturnIOError;
        if (!rc) *generation = out.generation;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
IOReturn vgpu_surface_image_draw_triangle(vgpu_surface_image *s, const float vertices[3][4],
                                          uint64_t *generation)
{
    if (!s || !vertices || !generation || image_output_overlap(s, generation, sizeof(*generation)))
        return kIOReturnBadArgument;
    struct image_clear_request in = {0};
    in.version = 1; in.byte_size = sizeof(in); in.opcode = 4;
    memcpy(in.vertices_bits, vertices, sizeof(in.vertices_bits));
    for (size_t v = 0; v < 3; ++v) {
        const uint32_t *p = in.vertices_bits + v * 4;
        if ((p[0] & UINT32_C(0x7fffffff)) > UINT32_C(0x3f800000) ||
            (p[1] & UINT32_C(0x7fffffff)) > UINT32_C(0x3f800000) ||
            (p[2] & UINT32_C(0x7fffffff)) > UINT32_C(0x3f800000) ||
            ((p[2] & UINT32_C(0x80000000)) && (p[2] & UINT32_C(0x7fffffff))) ||
            p[3] != UINT32_C(0x3f800000)) return kIOReturnBadArgument;
    }
    struct image_info_response info = {0};
    struct image_clear_response out = {0};
    pthread_mutex_lock(&s->lock);
    IOReturn rc = image_info_locked(s, &info);
    if (!rc) {
        in.handle = s->handle; in.expected_generation = info.generation;
        rc = image_call(s, 6, &in, sizeof(in), &out, sizeof(out));
        if (!rc && (out.version != 1 || out.byte_size != sizeof(out) ||
                    out.handle != s->handle || info.generation == UINT64_MAX ||
                    out.generation != info.generation + 1 || out.width != 256 || out.height != 256))
            rc = kIOReturnIOError;
        if (!rc) *generation = out.generation;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
static IOReturn image_read_row(vgpu_surface_image *s, uint64_t generation, uint32_t y,
                               struct image_read_response *row)
{
    struct image_read_request in = {1, sizeof(in), s->handle, generation, y, 0};
    IOReturn rc = image_call(s, 7, &in, sizeof(in), row, sizeof(*row));
    if (!rc && (row->version != 1 || row->byte_size != sizeof(*row) || row->handle != s->handle ||
                row->generation != generation || row->row != y || row->row_bytes != 1024))
        rc = kIOReturnIOError;
    return rc;
}
IOReturn vgpu_surface_image_read(vgpu_surface_image *s, uint64_t generation,
                                 vgpu_surface_rect rect, uint32_t format,
                                 vgpu_surface_pack pack, void *destination, size_t capacity)
{
    if (!s || !generation || !destination) return kIOReturnBadArgument;
    if (format != VGPU_SURFACE_RGBA8_OUTPUT) return kIOReturnUnsupported;
    if (!rect.width || !rect.height || rect.x >= 256 || rect.y >= 256 ||
        rect.width > 256 - rect.x || rect.height > 256 - rect.y) return kIOReturnBadArgument;
    if (pack.alignment != 1 && pack.alignment != 2 && pack.alignment != 4 && pack.alignment != 8)
        return kIOReturnBadArgument;
    size_t stride = pack.stride_bytes, row_bytes = (size_t)rect.width * 4;
    size_t offset, skip, last, end, temporary_size;
    if (!stride) {
        if (!mul_size(pack.row_length ? pack.row_length : rect.width, 4, &stride) ||
            !add_size(stride, pack.alignment - 1, &stride)) return kIOReturnBadArgument;
        stride &= ~(pack.alignment - 1);
    }
    if (!mul_size(pack.skip_rows, stride, &offset) || !mul_size(pack.skip_pixels, 4, &skip) ||
        !add_size(offset, skip, &offset) || !mul_size((size_t)rect.height - 1, stride, &last) ||
        !add_size(offset, last, &end) || !add_size(end, row_bytes, &end) ||
        !mul_size(row_bytes, rect.height, &temporary_size) || end > capacity ||
        image_output_overlap(s, destination, end)) return kIOReturnBadArgument;
    unsigned char *temporary = malloc(temporary_size);
    if (!temporary) return kIOReturnNoMemory;
    struct image_read_response row = {0};
    pthread_mutex_lock(&s->lock);
    IOReturn rc = kIOReturnNotOpen;
    if (s->state == VGPU_SURFACE_IMAGE_LIVE) {
        for (uint32_t j = 0; j < rect.height; ++j) {
            rc = image_read_row(s, generation, rect.y + j, &row);
            if (rc) break;
            unsigned char *p = temporary + (size_t)j * row_bytes;
            for (uint32_t i = 0; i < rect.width; ++i) {
                size_t at = (size_t)(rect.x + i) * 4;
                p[i * 4] = row.pixels[at + 2];
                p[i * 4 + 1] = row.pixels[at + 1];
                p[i * 4 + 2] = row.pixels[at];
                p[i * 4 + 3] = row.pixels[at + 3];
            }
        }
        if (!rc) rc = image_read_row(s, generation, rect.y, &row);
        if (!rc) {
            for (uint32_t j = 0; j < rect.height; ++j)
                memcpy((unsigned char *)destination + offset + (size_t)j * stride,
                       temporary + (size_t)j * row_bytes, row_bytes);
        }
    }
    pthread_mutex_unlock(&s->lock);
    free(temporary);
    return rc;
}
