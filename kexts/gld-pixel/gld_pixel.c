#include "gld_pixel.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8, "8-bit bytes required");
_Static_assert(sizeof(void *) == 8, "64-bit pointers required");
_Static_assert(sizeof(uint32_t) == 4, "32-bit tokens required");
_Static_assert(offsetof(gp_pixel_node, fields) == 8, "prefix fields offset");
_Static_assert(offsetof(gp_pixel_node, backend_format_index) == 0x38,
               "consumer prefix must be 0x38 bytes");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Only little-endian targets are supported"
#endif

/* 长度必须来自所有者；这些检查仅防算术溢出，不能证明指针所指映射可读。 */
static int extent_ok(const void *p, size_t n)
{
    return n <= (size_t)PTRDIFF_MAX &&
           (n == 0 || (p != NULL && (uintptr_t)p <= UINTPTR_MAX - n));
}

gp_status gp_parse_normalized(const void *tokens, size_t byte_length,
                              gp_request *out)
{
    gp_request request = {0};
    const unsigned char *bytes = tokens;
    size_t words = byte_length / sizeof(uint32_t);
    if (out == NULL || (tokens == NULL && byte_length != 0))
        return GP_BAD_ARGUMENT;
    if (!extent_ok(tokens, byte_length)) return GP_OVERFLOW;
    if (byte_length % sizeof(uint32_t) != 0) return GP_BAD_ARGUMENT;
    for (size_t i = 0; i < words;) {
        uint32_t token, value;
        uint32_t *minimum;
        memcpy(&token, bytes + i * sizeof(token), sizeof(token));
        ++i;
        switch (token) {
        case 0:
            request.consumed_words = i;
            *out = request;
            return GP_OK;
        case 4: /* GPUSupport 0x7ff918355df0 -> 0x7ff918355828，无值空操作。 */
            continue;
        case 5:
            request.double_buffer = 1;
            continue;
        case 8: minimum = &request.color_min; break;
        case 11: minimum = &request.alpha_min; break;
        case 12: minimum = &request.depth_min; break;
        case 13: minimum = &request.stencil_min; break;
        default:
            return GP_UNSUPPORTED_TOKEN; /* 不猜未知 token 的参数数量。 */
        }
        if (i == words) return GP_TRUNCATED_VALUE;
        memcpy(&value, bytes + i * sizeof(value), sizeof(value));
        ++i;
        if (value > INT32_MAX) return GP_BAD_VALUE;
        if (value > *minimum) *minimum = value;
    }
    return GP_UNTERMINATED;
}

static int valid_bits(uint32_t bits)
{
    return bits == 0 || bits == 8 || bits == 16 || bits == 24 || bits == 32;
}

static int valid_cap(const gp_format_cap *cap)
{
    const uint32_t known_flags = GP_FLAG_WINDOW | GP_FLAG_FULLSCREEN |
        GP_FLAG_OFFSCREEN | GP_FLAG_ACCELERATED | GP_FLAG_COMPLIANT |
        GP_FLAG_CORE32 | GP_FLAG_CORE41;
    return (cap->flags & ~known_flags) == 0 &&
        (cap->buffer_mode == GP_BUFFER_MONO_SINGLE ||
         cap->buffer_mode == GP_BUFFER_MONO_DOUBLE) &&
        (cap->color_mode == GP_COLOR_RGB888 || cap->color_mode == GP_COLOR_ARGB8888) &&
        valid_bits(cap->depth_bits) && valid_bits(cap->stencil_bits);
}

static int matches(const gp_request *request, const gp_format_cap *cap)
{
    const uint32_t alpha = cap->color_mode == GP_COLOR_ARGB8888 ? 8U : 0U;
    return (!request->double_buffer || cap->buffer_mode == GP_BUFFER_MONO_DOUBLE) &&
        request->color_min <= 32 && request->alpha_min <= alpha &&
        request->depth_min <= cap->depth_bits && request->stencil_min <= cap->stencil_bits;
}

static uint32_t bits_mode(uint32_t bits)
{
    switch (bits) {
    case 8: return 0x80;
    case 16: return 0x400;
    case 24: return 0x800;
    case 32: return 0x1000;
    default: return 1; /* kCGL0Bit，仅在能力已验证后调用。 */
    }
}

static void put32(gp_pixel_node *node, size_t offset, uint32_t value)
{
    memcpy(node->fields + (offset - 8), &value, sizeof(value));
}

static void *default_allocate(void *context, size_t bytes)
{
    (void)context;
    return malloc(bytes);
}

static void default_release(void *context, void *allocation)
{
    (void)context;
    free(allocation);
}

gp_status gp_choose_normalized(const void *tokens, size_t byte_length,
                               const gp_format_cap *formats, size_t format_count,
                               const gp_allocator *allocator, gp_pixel_list *out)
{
    gp_request request;
    gp_pixel_list result = {0};
    gp_status status;
    if (out == NULL || out->head != NULL || out->allocation != NULL || out->count != 0)
        return GP_BAD_ARGUMENT;
    if (format_count != 0 && formats == NULL) return GP_BAD_ARGUMENT;
    if (allocator != NULL && (allocator->allocate == NULL || allocator->release == NULL))
        return GP_BAD_ARGUMENT;
    if (format_count > (size_t)PTRDIFF_MAX / sizeof(*formats) ||
        format_count > (size_t)PTRDIFF_MAX / sizeof(gp_pixel_node)) return GP_OVERFLOW;
    if (!extent_ok(formats, format_count * sizeof(*formats))) return GP_OVERFLOW;
    status = gp_parse_normalized(tokens, byte_length, &request);
    if (status != GP_OK) return status;
    for (size_t i = 0; i < format_count; ++i) {
        if (!valid_cap(&formats[i])) return GP_UNSUPPORTED_FORMAT;
        if (matches(&request, &formats[i])) ++result.count;
    }
    if (result.count == 0) return GP_NO_MATCH;
    result.allocator = allocator != NULL ? *allocator :
        (gp_allocator){NULL, default_allocate, default_release};
    result.allocation = result.allocator.allocate(result.allocator.context,
                                                  result.count * sizeof(gp_pixel_node));
    if (result.allocation == NULL) return GP_NO_MEMORY;
    result.head = result.allocation;
    memset(result.head, 0, result.count * sizeof(gp_pixel_node));
    size_t index = 0;
    for (size_t i = 0; i < format_count; ++i) {
        const gp_format_cap *cap = &formats[i];
        gp_pixel_node *node;
        if (!matches(&request, cap)) continue;
        node = &result.head[index];
        ++index;
        node->next = index < result.count ? &result.head[index] : NULL;
        put32(node, 0x08, cap->renderer_id);
        put32(node, 0x0c, cap->flags);
        put32(node, 0x10, cap->buffer_mode);
        put32(node, 0x14, cap->color_mode);
        put32(node, 0x18, 0x80000000U); /* GPUSupport 无 accum 的模式位。 */
        put32(node, 0x1c, bits_mode(cap->depth_bits));
        put32(node, 0x20, bits_mode(cap->stencil_bits));
        put32(node, 0x34, cap->display_mask);
        node->backend_format_index = i;
    }
    *out = result;
    return GP_OK;
}

void gp_pixel_list_destroy(gp_pixel_list *list)
{
    if (list == NULL) return;
    if (list->allocation != NULL)
        list->allocator.release(list->allocator.context, list->allocation);
    *list = (gp_pixel_list){0};
}
