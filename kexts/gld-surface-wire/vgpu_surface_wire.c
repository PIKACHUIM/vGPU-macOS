#include "vgpu_surface_wire.h"

#ifdef __cplusplus
#define WIRE_ALIGNOF(T) alignof(T)
#else
#define WIRE_ALIGNOF(T) _Alignof(T)
#endif

static int range_valid(const void *pointer, size_t size)
{
    return pointer != NULL && size != 0 &&
        size - 1 <= UINTPTR_MAX - (uintptr_t)pointer;
}

/* 只对已经验证不会回绕的非空区间调用。 */
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t av = (uintptr_t)a;
    uintptr_t bv = (uintptr_t)b;
    return av <= bv + (bn - 1) && bv <= av + (an - 1);
}

static uint32_t le16(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

enum vgpu_surface_wire_status vgpu_surface_wire_parse(
    const struct vgpu_surface_wire_message *message,
    struct vgpu_surface_wire_request *request)
{
    struct vgpu_surface_wire_request parsed = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    const unsigned char *region;
    uint64_t a, b;
    uint32_t count;
    size_t required;

    if (message == NULL || request == NULL)
        return VGPU_SURFACE_WIRE_INVALID;
    if (!range_valid(message, sizeof(*message)) ||
        !range_valid(request, sizeof(*request)))
        return VGPU_SURFACE_WIRE_OVERFLOW;
    if ((uintptr_t)message % WIRE_ALIGNOF(struct vgpu_surface_wire_message) != 0 ||
        (uintptr_t)request % WIRE_ALIGNOF(struct vgpu_surface_wire_request) != 0)
        return VGPU_SURFACE_WIRE_INVALID;
    if (overlap(message, sizeof(*message), request, sizeof(*request)))
        return VGPU_SURFACE_WIRE_ALIAS;
    if (message->connection_type != 0 ||
        (message->selector != 7 && message->selector != 9))
        return VGPU_SURFACE_WIRE_UNSUPPORTED;
    if (message->scalar_input_count != 2 ||
        message->scalar_output_count != 0 || message->scalar_output != NULL ||
        message->structure_output_size != 0 || message->structure_output != NULL ||
        message->scalar_input == NULL)
        return VGPU_SURFACE_WIRE_INVALID;
    if (!range_valid(message->scalar_input, 2 * sizeof(uint64_t)))
        return VGPU_SURFACE_WIRE_OVERFLOW;
    if ((uintptr_t)message->scalar_input % WIRE_ALIGNOF(uint64_t) != 0)
        return VGPU_SURFACE_WIRE_INVALID;
    if (overlap(message->scalar_input, 2 * sizeof(uint64_t), request, sizeof(*request)))
        return VGPU_SURFACE_WIRE_ALIAS;
    if (message->selector == 7) {
        if (message->structure_input_size != 0 || message->structure_input != NULL)
            return VGPU_SURFACE_WIRE_INVALID;
    } else {
        if (message->structure_input_size != 20 || message->structure_input == NULL)
            return VGPU_SURFACE_WIRE_INVALID;
        if (!range_valid(message->structure_input, message->structure_input_size))
            return VGPU_SURFACE_WIRE_OVERFLOW;
        if (overlap(message->structure_input, message->structure_input_size,
                    request, sizeof(*request)))
            return VGPU_SURFACE_WIRE_ALIAS;
    }
    a = message->scalar_input[0];
    b = message->scalar_input[1];
    if (a > UINT32_MAX || b > UINT32_MAX)
        return VGPU_SURFACE_WIRE_INVALID;
    parsed.selector = message->selector;
    if (message->selector == 7) {
        if (a == 0)
            return VGPU_SURFACE_WIRE_INVALID;
        if (b == UINT32_C(0x00100024))
            parsed.mode_class = VGPU_SURFACE_MODE_CGL_00100024;
        else if (b == UINT32_C(0x0a))
            parsed.mode_class = VGPU_SURFACE_MODE_EXPLICIT_BGRA_0000000A;
        else
            return VGPU_SURFACE_WIRE_UNSUPPORTED;
        parsed.external_accel_id = (uint32_t)a;
        parsed.mode = (uint32_t)b;
    } else {
        if (a != UINT32_C(0x81) || b != 0)
            return VGPU_SURFACE_WIRE_UNSUPPORTED;
        region = (const unsigned char *)message->structure_input;
        count = le32(region);
        /* 在窄白名单判断前，以实际 size_t 宽度检查乘加。 */
        if ((uint64_t)count > (uint64_t)((SIZE_MAX - 12) / 8))
            return VGPU_SURFACE_WIRE_OVERFLOW;
        required = 12 + (size_t)count * 8;
        if (required != message->structure_input_size || count != 1)
            return VGPU_SURFACE_WIRE_INVALID;
        if (le16(region + 4) != 0 || le16(region + 6) != 0 ||
            le16(region + 8) != 256 || le16(region + 10) != 256 ||
            le32(region + 12) != 0 || le32(region + 16) != 0)
            return VGPU_SURFACE_WIRE_UNSUPPORTED;
        parsed.flags = (uint32_t)a;
        parsed.framebuffer_index = (uint32_t)b;
        parsed.rectangle_count = count;
        parsed.bounds_width = 256;
        parsed.bounds_height = 256;
    }
    *request = parsed;
    return VGPU_SURFACE_WIRE_PARSED;
}
