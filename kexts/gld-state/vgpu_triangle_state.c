#include "vgpu_triangle_state.h"

#include <float.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == 8 && sizeof(size_t) == 8,
               "需要64位地址和长度");
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128 && FLT_MIN_EXP == -125, "需要IEEE binary32");
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 &&
               DBL_MAX_EXP == 1024 && DBL_MIN_EXP == -1021, "需要IEEE binary64");
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "需要已确认的小端宿主"
#endif

static int range_ok(const void *data, size_t length)
{
    return data != NULL && length <= UINTPTR_MAX - (uintptr_t)data;
}

static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x < y + bn && y < x + an;
}

static int field(vgc_view h, size_t offset, size_t width, void *out)
{
    if (offset > h.length || width > h.length - offset)
        return 0;
    memcpy(out, (const unsigned char *)h.data + offset, width);
    return 1;
}

static int finite_float(uint32_t bits, float *out)
{
    if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000))
        return 0;
    memcpy(out, &bits, sizeof(bits));
    return 1;
}

vgpu_triangle_state_result vgpu_triangle_state_decode(vgc_view h,
                                                       vgpu_triangle_state *out)
{
    if (!range_ok(h.data, h.length) || !range_ok(out, sizeof(*out)) ||
        (uintptr_t)h.data % VGPU_TRIANGLE_H_ALIGNMENT != 0 ||
        overlaps(h.data, h.length, out, sizeof(*out)))
        return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;

    /* 短视图先拒绝；各字段读取仍独立验证 offset/width，尚未读输入。 */
    static const size_t float_offsets[] = {0x2520, 0x24e0, 0x1830, 0x1b80};
    static const size_t float_counts[] = {16, 16, 4, 4};
    static const size_t zero_bytes[] = {
        0x3e8c, 0x39ba, 0x39bb, 0x3c72, 0x3b5e,
        0x3c24, 0x3c25, 0x3c26, 0x3ecc, 0x3d6e,
        0x3d6d, 0x3d6c, 0x3d6f, 0x3d70, 0x3d71,
        0x3c97, 0x3c96, 0x3d84, 0x3c1c, 0x3c38,
        0x4840, 0x4841, 0x4842, 0x4843, 0x4845, 0x4847, 0x4978
    };
    /* 最高实际字段是一个byte，端点不是H的整体尺寸。 */
    if (h.length < VGPU_TRIANGLE_H_MIN_LENGTH)
        return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;

    vgpu_triangle_state decoded = {0};
    float *destinations[] = {decoded.modelview, decoded.projection,
                            decoded.current_rgba, decoded.viewport};
    for (size_t group = 0; group < 4; ++group) {
        for (size_t i = 0; i < float_counts[group]; ++i) {
            uint32_t bits;
            if (!field(h, float_offsets[group] + i * 4, sizeof(bits), &bits))
                return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;
            if (!finite_float(bits, &destinations[group][i]))
                return VGPU_TRIANGLE_STATE_UNSUPPORTED;
            float expected;
            if (group < 2)
                expected = i % 5 == 0 ? 1.0f : 0.0f;
            else if (group == 2)
                expected = i == 0 || i == 3 ? 1.0f : 0.0f;
            else
                expected = i < 2 ? 0.0f : 256.0f;
            if (destinations[group][i] != expected)
                return VGPU_TRIANGLE_STATE_UNSUPPORTED;
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        uint64_t bits;
        if (!field(h, 0x1b60 + i * 8, sizeof(bits), &bits))
            return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;
        if ((bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000))
            return VGPU_TRIANGLE_STATE_UNSUPPORTED;
        memcpy(&decoded.depth_range[i], &bits, sizeof(bits));
        if (decoded.depth_range[i] != (double)i)
            return VGPU_TRIANGLE_STATE_UNSUPPORTED;
    }
    for (size_t i = 0; i < sizeof(zero_bytes) / sizeof(zero_bytes[0]); ++i) {
        uint8_t value;
        if (!field(h, zero_bytes[i], sizeof(value), &value))
            return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;
        if (value != 0)
            return VGPU_TRIANGLE_STATE_UNSUPPORTED;
    }
    /* 两面独立的 u16 setter 字段；不依赖 H+3d72 派生状态。 */
    for (size_t face = 0; face < 2; ++face) {
        uint16_t polygon_mode;
        if (!field(h, 0x3d64 + face * 2, sizeof(polygon_mode), &polygon_mode))
            return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;
        if (polygon_mode != UINT16_C(0x1b02))
            return VGPU_TRIANGLE_STATE_UNSUPPORTED;
    }
    uint8_t blend;
    uint32_t scissor, clip;
    if (!field(h, 0x3bd0, sizeof(blend), &blend) ||
        !field(h, 0x3e88, sizeof(scissor), &scissor) ||
        !field(h, 0x47d4, sizeof(clip), &clip))
        return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;
    if ((blend & 1u) != 0 || (scissor & 1u) != 0 || clip != 0)
        return VGPU_TRIANGLE_STATE_UNSUPPORTED;
    for (size_t i = 0; i < 4; ++i) {
        uint8_t mask;
        if (!field(h, 0x3ca8 + i, sizeof(mask), &mask))
            return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;
        if ((mask & 1u) == 0)
            return VGPU_TRIANGLE_STATE_UNSUPPORTED;
    }
    for (size_t i = 0; i < 8; ++i) {
        uint32_t texture;
        if (!field(h, 0x3ef4 + 0x4c * i, sizeof(texture), &texture))
            return VGPU_TRIANGLE_STATE_BAD_ARGUMENT;
        if (texture != 0)
            return VGPU_TRIANGLE_STATE_UNSUPPORTED;
    }
    decoded.core_raster_state_verified = 1;
    decoded.all_state_verified = 0;
    decoded.unverified_state = VGPU_TRIANGLE_UNVERIFIED_ALL;
    memcpy(out, &decoded, sizeof(decoded));
    return VGPU_TRIANGLE_STATE_CORE_VERIFIED;
}

void vgpu_triangle_error_init(vgpu_triangle_error_latch *latch)
{
    latch->pending_gl_error = VGPU_GL_NO_ERROR;
    latch->reject_reason = VGPU_TRIANGLE_REJECT_NONE;
    latch->backend_fault = VGPU_TRIANGLE_BACKEND_OK;
}

void vgpu_triangle_error_record(vgpu_triangle_error_latch *latch,
                                vgpu_gl_error error,
                                vgpu_triangle_reject_reason reason,
                                vgpu_triangle_backend_fault fault)
{
    if (latch->pending_gl_error == VGPU_GL_NO_ERROR)
        latch->pending_gl_error = error;
    if (latch->reject_reason == VGPU_TRIANGLE_REJECT_NONE)
        latch->reject_reason = reason;
    if (latch->backend_fault == VGPU_TRIANGLE_BACKEND_OK)
        latch->backend_fault = fault;
}

vgpu_gl_error vgpu_triangle_error_get(vgpu_triangle_error_latch *latch)
{
    vgpu_gl_error error = latch->pending_gl_error;
    latch->pending_gl_error = VGPU_GL_NO_ERROR;
    return error;
}
