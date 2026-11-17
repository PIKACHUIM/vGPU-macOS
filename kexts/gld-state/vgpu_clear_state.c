#include "vgpu_clear_state.h"

#include <float.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == 8 && sizeof(size_t) == 8,
               "需要64位地址和长度");
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128 && FLT_MIN_EXP == -125, "需要IEEE binary32");
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

static uint16_t load_u16(const unsigned char *data)
{
    uint16_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static uint32_t load_u32(const unsigned char *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

vgpu_clear_state_result vgpu_clear_state_decode(vgc_view h, vgc_view v,
                                                 uint32_t internal_mask,
                                                 vgc_clear_desc *out)
{
    if (h.length < VGPU_CLEAR_H_MIN_LENGTH || v.length < VGPU_CLEAR_V_MIN_LENGTH ||
        !range_ok(h.data, h.length) || !range_ok(v.data, v.length) ||
        !range_ok(out, sizeof(*out)))
        return VGPU_CLEAR_STATE_BAD_ARGUMENT;
    if (overlaps(out, sizeof(*out), h.data, h.length) ||
        overlaps(out, sizeof(*out), v.data, v.length))
        return VGPU_CLEAR_STATE_BAD_ARGUMENT;
    if (internal_mask == 0)
        return VGPU_CLEAR_STATE_NOOP;
    if (internal_mask != 1)
        return VGPU_CLEAR_STATE_UNSUPPORTED;

    const unsigned char *hb = h.data, *vb = v.data;
    if (load_u16(hb + 0x3cae) != 1 ||
        ((hb[0x3ca8] & hb[0x3ca9] & hb[0x3caa] & hb[0x3cab]) & 1) == 0 ||
        hb[0x3d84] != 0 || load_u32(hb + 0x3e88) != 0 || hb[0x3e8c] != 0 ||
        hb[0x3c1c] != 0 || hb[0x3c38] != 0 || load_u32(vb + 0x18) != 0)
        return VGPU_CLEAR_STATE_UNSUPPORTED;

    vgc_clear_desc decoded = {1, 15, 0, 0, {0, 0, 0, 0}};
    for (size_t i = 0; i < 4; ++i) {
        uint32_t bits = load_u32(vb + i * 4);
        /* 先按位拒绝NaN/Inf，避免对信号NaN执行浮点比较。 */
        if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000))
            return VGPU_CLEAR_STATE_UNSUPPORTED;
        memcpy(&decoded.rgba[i], &bits, sizeof(bits));
        if (decoded.rgba[i] < 0.0f || decoded.rgba[i] > 1.0f)
            return VGPU_CLEAR_STATE_UNSUPPORTED;
    }
    memcpy(out, &decoded, sizeof(decoded));
    return VGPU_CLEAR_STATE_OK;
}
