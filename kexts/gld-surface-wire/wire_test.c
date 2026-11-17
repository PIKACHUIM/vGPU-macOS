#include "vgpu_surface_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned int checks;
static const unsigned char real_region[20] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0
};

static struct vgpu_surface_wire_message create_message(const uint64_t *scalar)
{
    struct vgpu_surface_wire_message m = {0};
    m.selector = 7;
    m.scalar_input = scalar;
    m.scalar_input_count = 2;
    return m;
}

static struct vgpu_surface_wire_message shape_message(const uint64_t *scalar,
                                                       const void *region)
{
    struct vgpu_surface_wire_message m = create_message(scalar);
    m.selector = 9;
    m.structure_input = region;
    m.structure_input_size = 20;
    return m;
}

static void rejected(const struct vgpu_surface_wire_message *m)
{
    struct vgpu_surface_wire_request output;
    unsigned char before[sizeof(output)];
    memset(&output, 0xa5, sizeof(output));
    memcpy(before, &output, sizeof(output));
    assert(vgpu_surface_wire_parse(m, &output) != VGPU_SURFACE_WIRE_PARSED);
    assert(memcmp(before, &output, sizeof(output)) == 0);
    ++checks;
}

static void accepted(const struct vgpu_surface_wire_message *m,
                     const struct vgpu_surface_wire_request *expected)
{
    struct vgpu_surface_wire_request output;
    memset(&output, 0xa5, sizeof(output));
    assert(vgpu_surface_wire_parse(m, &output) == VGPU_SURFACE_WIRE_PARSED);
    assert(memcmp(&output, expected, sizeof(output)) == 0);
    ++checks;
}

static void aliases(void)
{
    union storage {
        struct vgpu_surface_wire_message message;
        struct vgpu_surface_wire_request request;
        uint64_t scalar[32];
        unsigned char bytes[256];
    } u;
    unsigned char before[sizeof(u)];
    uint64_t scalar[2] = {1, UINT64_C(0x00100024)};
    struct vgpu_surface_wire_message m;
    size_t offsets[] = {0, 4, 8, 12};
    size_t i;

    memset(&u, 0, sizeof(u));
    u.message = create_message(scalar);
    memcpy(before, &u, sizeof(u));
    assert(vgpu_surface_wire_parse(&u.message, &u.request) == VGPU_SURFACE_WIRE_ALIAS);
    assert(memcmp(before, &u, sizeof(u)) == 0);
    ++checks;
    /* scalar 和输出从两侧发生部分重叠，不能只检查指针相等。 */
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        struct vgpu_surface_wire_request *out;
        memset(&u, 0, sizeof(u));
        u.scalar[0] = 1;
        u.scalar[1] = UINT64_C(0x00100024);
        m = create_message(u.scalar);
        out = (struct vgpu_surface_wire_request *)(void *)(u.bytes + offsets[i]);
        memcpy(before, &u, sizeof(u));
        assert(vgpu_surface_wire_parse(&m, out) == VGPU_SURFACE_WIRE_ALIAS);
        assert(memcmp(before, &u, sizeof(u)) == 0);
        ++checks;
    }
    m = create_message(u.scalar + 4);
    memcpy(before, &u, sizeof(u));
    assert(vgpu_surface_wire_parse(&m, &u.request) == VGPU_SURFACE_WIRE_ALIAS);
    assert(memcmp(before, &u, sizeof(u)) == 0);
    ++checks;
    scalar[0] = 0x81;
    scalar[1] = 0;
    memcpy(u.bytes + 8, real_region, sizeof(real_region));
    m = shape_message(scalar, u.bytes + 8);
    memcpy(before, &u, sizeof(u));
    assert(vgpu_surface_wire_parse(&m, &u.request) == VGPU_SURFACE_WIRE_ALIAS);
    assert(memcmp(before, &u, sizeof(u)) == 0);
    ++checks;
}

int main(void)
{
    uint64_t scalar[2] = {1, UINT64_C(0x00100024)};
    struct vgpu_surface_wire_message m = create_message(scalar);
    struct vgpu_surface_wire_message bad;
    struct vgpu_surface_wire_request expected = {0};
    unsigned char region[21];
    size_t i;
    uint32_t bit;
    const size_t counts[] = {0, 1, 3, SIZE_MAX};
    const size_t sizes[] = {0, 1, 11, 12, 19, 21, 28, SIZE_MAX};

    expected.selector = 7;
    expected.external_accel_id = 1;
    expected.mode = 0x00100024;
    expected.mode_class = VGPU_SURFACE_MODE_CGL_00100024;
    accepted(&m, &expected);
    scalar[0] = UINT32_MAX;
    expected.external_accel_id = UINT32_MAX;
    accepted(&m, &expected);
    scalar[1] = 0x0a;
    expected.mode = 0x0a;
    expected.mode_class = VGPU_SURFACE_MODE_EXPLICIT_BGRA_0000000A;
    accepted(&m, &expected);
    scalar[0] = 0;
    rejected(&m);
    scalar[0] = 1;
    for (bit = 0; bit < 32; ++bit) {
        scalar[1] = UINT64_C(0x00100024) ^ (UINT64_C(1) << bit);
        rejected(&m);
        scalar[1] = UINT64_C(0x0a) ^ (UINT64_C(1) << bit);
        rejected(&m);
    }
    scalar[1] = 0x00100024;
    for (bit = 32; bit < 64; ++bit) {
        scalar[0] = UINT64_C(1) << bit;
        rejected(&m);
        scalar[0] = 1;
        scalar[1] = UINT64_C(0x00100024) | (UINT64_C(1) << bit);
        rejected(&m);
    }
    scalar[1] = 0x00100024;
    bad = m;
    bad.structure_input = real_region;
    rejected(&bad);
    bad.structure_input_size = 20;
    rejected(&bad);
    bad.structure_input = NULL;
    rejected(&bad);

    scalar[0] = 0x81;
    scalar[1] = 0;
    m = shape_message(scalar, real_region);
    memset(&expected, 0, sizeof(expected));
    expected.selector = 9;
    expected.flags = 0x81;
    expected.rectangle_count = 1;
    expected.bounds_width = 256;
    expected.bounds_height = 256;
    accepted(&m, &expected);
    memcpy(region + 1, real_region, 20);
    m.structure_input = region + 1;
    accepted(&m, &expected);
    /* 每个 wire 字节的每一位都不可被静默忽略，含负 SInt16。 */
    for (i = 0; i < 20; ++i) {
        for (bit = 0; bit < 8; ++bit) {
            memcpy(region, real_region, 20);
            region[i] ^= (unsigned char)(1U << bit);
            m.structure_input = region;
            rejected(&m);
        }
    }
    memcpy(region, real_region, 20);
    memset(region, 0xff, 4);
    rejected(&m);
    m.structure_input = real_region;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        bad = m;
        bad.structure_input_size = sizes[i];
        rejected(&bad);
    }
    for (bit = 0; bit < 64; ++bit) {
        scalar[0] = UINT64_C(0x81) ^ (UINT64_C(1) << bit);
        rejected(&m);
        scalar[0] = 0x81;
        scalar[1] = UINT64_C(1) << bit;
        rejected(&m);
    }
    scalar[1] = 0;
    bad = m;
    bad.structure_input = NULL;
    rejected(&bad);
    bad.structure_input = (const void *)(UINTPTR_MAX - 8);
    rejected(&bad);

    /* 对两种 selector 检查相同的 dispatch/count/pointer 合约。 */
    for (i = 0; i < 2; ++i) {
        size_t j;
        scalar[0] = i == 0 ? 1 : 0x81;
        scalar[1] = i == 0 ? 0x00100024 : 0;
        m = i == 0 ? create_message(scalar) : shape_message(scalar, real_region);
        for (j = 0; j < sizeof(counts) / sizeof(counts[0]); ++j) {
            bad = m;
            bad.scalar_input_count = counts[j];
            rejected(&bad);
        }
        bad = m;
        bad.scalar_input = NULL;
        rejected(&bad);
        bad.scalar_input = (const uint64_t *)(UINTPTR_MAX - 7);
        rejected(&bad);
        bad.scalar_input = (const uint64_t *)(const void *)((const unsigned char *)scalar + 1);
        rejected(&bad);
        bad = m;
        bad.scalar_output_count = 1;
        rejected(&bad);
        bad = m;
        bad.scalar_output = &expected;
        rejected(&bad);
        bad = m;
        bad.structure_output_size = 1;
        rejected(&bad);
        bad = m;
        bad.structure_output = &expected;
        rejected(&bad);
        bad = m;
        bad.connection_type = 0x20;
        rejected(&bad);
        bad.connection_type = UINT32_MAX;
        rejected(&bad);
    }
    for (bit = 0; bit < 32; ++bit) {
        if (bit == 7 || bit == 9)
            continue;
        bad = m;
        bad.selector = bit;
        rejected(&bad);
    }
    bad = m;
    bad.selector = UINT32_MAX;
    rejected(&bad);
    rejected(NULL);
    rejected((const struct vgpu_surface_wire_message *)(UINTPTR_MAX - 7));
    assert(vgpu_surface_wire_parse(&m, NULL) == VGPU_SURFACE_WIRE_INVALID);
    assert(vgpu_surface_wire_parse(&m,
        (struct vgpu_surface_wire_request *)(UINTPTR_MAX - 3)) == VGPU_SURFACE_WIRE_OVERFLOW);
    aliases();
    printf("PASS: %u CPU checks; exact wire, strict rejection, unchanged outputs\n", checks);
    return 0;
}
