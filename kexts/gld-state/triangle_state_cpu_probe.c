#include "vgpu_triangle_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static const size_t zeros[] = {
    0x3e8c, 0x39ba, 0x39bb, 0x3c72, 0x3b5e, 0x3c24, 0x3c25,
    0x3c26, 0x3ecc, 0x3d6e, 0x3d6d, 0x3d6c, 0x3d6f, 0x3d70,
    0x3d71, 0x3c97, 0x3c96, 0x3d84, 0x3c1c, 0x3c38,
    0x4840, 0x4841, 0x4842, 0x4843, 0x4845, 0x4847, 0x4978
};
_Alignas(8) static unsigned char h[VGPU_TRIANGLE_H_MIN_LENGTH + 512];
static void put16(unsigned char *p, uint16_t n) { memcpy(p, &n, sizeof(n)); }
static void put32(unsigned char *p, uint32_t n) { memcpy(p, &n, sizeof(n)); }
static void put64(unsigned char *p, uint64_t n) { memcpy(p, &n, sizeof(n)); }

static void reset(void)
{
    /* 空洞、未知状态与相邻字节非零，检出偏移混淆与错误加宽读取。 */
    memset(h, 0xa6, sizeof(h));
    for (size_t i = 0; i < 16; ++i) {
        put32(h + 0x2520 + i * 4, i % 5 == 0 ? UINT32_C(0x3f800000) : 0);
        put32(h + 0x24e0 + i * 4, i % 5 == 0 ? UINT32_C(0x3f800000) : 0);
    }
    const float red[] = {1, 0, 0, 1};
    const float viewport[] = {0, 0, 256, 256};
    const double depth[] = {0, 1};
    memcpy(h + 0x1830, red, sizeof(red));
    memcpy(h + 0x1b80, viewport, sizeof(viewport));
    memcpy(h + 0x1b60, depth, sizeof(depth));
    for (size_t i = 0; i < sizeof(zeros) / sizeof(zeros[0]); ++i)
        h[zeros[i]] = 0;
    h[0x3bd0] = 0;
    put16(h + 0x3d64, UINT16_C(0x1b02));
    put16(h + 0x3d66, UINT16_C(0x1b02));
    memset(h + 0x3ca8, 1, 4);
    put32(h + 0x3e88, 0);
    put32(h + 0x47d4, 0);
    for (size_t i = 0; i < 8; ++i)
        put32(h + 0x3ef4 + 0x4c * i, 0);
}

static void expect(vgc_view view, vgpu_triangle_state_result result)
{
    vgpu_triangle_state out;
    unsigned char before[sizeof(out)], input[sizeof(h)];
    memset(&out, 0x5b, sizeof(out));
    memcpy(before, &out, sizeof(out));
    memcpy(input, h, sizeof(h));
    CHECK(vgpu_triangle_state_decode(view, &out) == result);
    CHECK(memcmp(input, h, sizeof(h)) == 0);
    if (result != VGPU_TRIANGLE_STATE_CORE_VERIFIED) {
        CHECK(memcmp(before, &out, sizeof(out)) == 0);
    } else {
        CHECK(out.core_raster_state_verified == 1);
        CHECK(out.all_state_verified == 0);
        CHECK(out.unverified_state == VGPU_TRIANGLE_UNVERIFIED_ALL);
        CHECK(out.unverified_state == UINT32_C(0x1b));
        CHECK((out.unverified_state & VGPU_TRIANGLE_UNVERIFIED_POLYGON_MODE) == 0);
        const unsigned char *data = view.data;
        CHECK(memcmp(out.modelview, data + 0x2520, sizeof(out.modelview)) == 0);
        CHECK(memcmp(out.projection, data + 0x24e0, sizeof(out.projection)) == 0);
        CHECK(memcmp(out.current_rgba, data + 0x1830, sizeof(out.current_rgba)) == 0);
        CHECK(memcmp(out.viewport, data + 0x1b80, sizeof(out.viewport)) == 0);
        CHECK(memcmp(out.depth_range, data + 0x1b60, sizeof(out.depth_range)) == 0);
    }
}

static void field_tests(void)
{
    vgc_view view = {h, VGPU_TRIANGLE_H_MIN_LENGTH};
    reset(); expect(view, VGPU_TRIANGLE_STATE_CORE_VERIFIED);
    for (size_t i = 0; i < sizeof(zeros) / sizeof(zeros[0]); ++i) {
        for (unsigned value = 1; value < 256; ++value) {
            reset(); h[zeros[i]] = (unsigned char)value;
            expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
        }
    }
    for (unsigned value = 0; value < 256; ++value) {
        reset(); h[0x3bd0] = (unsigned char)value;
        expect(view, (value & 1u) ? VGPU_TRIANGLE_STATE_UNSUPPORTED : VGPU_TRIANGLE_STATE_CORE_VERIFIED);
        for (size_t channel = 0; channel < 4; ++channel) {
            reset(); h[0x3ca8 + channel] = (unsigned char)value;
            expect(view, (value & 1u) ? VGPU_TRIANGLE_STATE_CORE_VERIFIED : VGPU_TRIANGLE_STATE_UNSUPPORTED);
        }
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        uint32_t flag = UINT32_C(1) << bit;
        reset(); put32(h + 0x3e88, flag);
        expect(view, bit == 0 ? VGPU_TRIANGLE_STATE_UNSUPPORTED : VGPU_TRIANGLE_STATE_CORE_VERIFIED);
        reset(); put32(h + 0x47d4, flag);
        expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
        for (size_t unit = 0; unit < 8; ++unit) {
            reset(); put32(h + 0x3ef4 + unit * 0x4c, flag);
            expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
        }
    }
    const size_t bases[] = {0x2520, 0x24e0, 0x1830, 0x1b80};
    const size_t counts[] = {16, 16, 4, 4};
    const uint32_t invalid[] = {UINT32_C(0x7f800000), UINT32_C(0xff800000),
        UINT32_C(0x7fc00001), UINT32_C(0x7f800001), UINT32_C(0xffc00001), UINT32_C(0x40000000)};
    for (size_t group = 0; group < 4; ++group) {
        for (size_t i = 0; i < counts[group]; ++i) {
            for (size_t n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
                reset(); put32(h + bases[group] + i * 4, invalid[n]);
                expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
            }
            reset();
            uint32_t bits;
            memcpy(&bits, h + bases[group] + i * 4, sizeof(bits));
            if (bits == 0) {
                put32(h + bases[group] + i * 4, UINT32_C(0x80000000));
                expect(view, VGPU_TRIANGLE_STATE_CORE_VERIFIED);
                put32(h + bases[group] + i * 4, 1);
                expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
            }
        }
    }
    /* 两矩阵可相互抵消，仍必须分别拒绝。 */
    reset(); put32(h + 0x2520, UINT32_C(0x40000000));
    put32(h + 0x24e0, UINT32_C(0x3f000000));
    expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
    const uint64_t bad_double[] = {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff0000000000001),
        UINT64_C(0x4000000000000000), UINT64_C(1), UINT64_C(0x3ff0000000000001)};
    for (size_t i = 0; i < 2; ++i) {
        for (size_t n = 0; n < sizeof(bad_double) / sizeof(bad_double[0]); ++n) {
            reset(); put64(h + 0x1b60 + i * 8, bad_double[n]);
            expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
        }
    }
    reset(); put64(h + 0x1b60, UINT64_C(0x8000000000000000));
    expect(view, VGPU_TRIANGLE_STATE_CORE_VERIFIED);
    reset(); put32(h + 0x1b60, 0); put32(h + 0x1b64, UINT32_C(0x3f800000));
    expect(view, VGPU_TRIANGLE_STATE_UNSUPPORTED);
}

static void polygon_tests(void)
{
    vgc_view view = {h, VGPU_TRIANGLE_H_MIN_LENGTH};
    /* 穷举每面所有 u16，包括 POINT、LINE、未知值及单字节混淆。 */
    for (size_t face = 0; face < 2; ++face) {
        reset();
        for (uint32_t value = 0; value <= UINT16_MAX; ++value) {
            put16(h + 0x3d64 + face * 2, (uint16_t)value);
            expect(view, value == UINT16_C(0x1b02) ?
                   VGPU_TRIANGLE_STATE_CORE_VERIFIED : VGPU_TRIANGLE_STATE_UNSUPPORTED);
        }
    }
    const uint16_t modes[] = {0x1b00, 0x1b01, 0x1b02, 0, 0xffff};
    for (size_t front = 0; front < sizeof(modes) / sizeof(modes[0]); ++front) {
        for (size_t back = 0; back < sizeof(modes) / sizeof(modes[0]); ++back) {
            reset();
            put16(h + 0x3d64, modes[front]);
            put16(h + 0x3d66, modes[back]);
            h[0x3d72] = 1; /* 派生标志不能让非法模式通过。 */
            expect(view, front == 2 && back == 2 ?
                   VGPU_TRIANGLE_STATE_CORE_VERIFIED : VGPU_TRIANGLE_STATE_UNSUPPORTED);
        }
    }
    for (unsigned value = 0; value < 256; ++value) {
        reset();
        h[0x3d63] = (unsigned char)value;
        h[0x3d68] = (unsigned char)value;
        h[0x3d72] = (unsigned char)value;
        expect(view, VGPU_TRIANGLE_STATE_CORE_VERIFIED);
    }
}

static void argument_tests(void)
{
    reset();
    for (size_t length = 0; length < VGPU_TRIANGLE_H_MIN_LENGTH; ++length)
        expect((vgc_view){h, length}, VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    expect((vgc_view){NULL, VGPU_TRIANGLE_H_MIN_LENGTH}, VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    expect((vgc_view){h, SIZE_MAX}, VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    expect((vgc_view){(const void *)(UINTPTR_MAX - 8), VGPU_TRIANGLE_H_MIN_LENGTH}, VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    CHECK(vgpu_triangle_state_decode((vgc_view){h, sizeof(h)}, NULL) == VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    CHECK(vgpu_triangle_state_decode((vgc_view){h, sizeof(h)},
          (vgpu_triangle_state *)(UINTPTR_MAX - 7)) == VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    _Alignas(8) unsigned char shifted[sizeof(h) + 8];
    for (size_t offset = 0; offset < 8; ++offset) {
        memcpy(shifted + offset, h, sizeof(h));
        expect((vgc_view){shifted + offset, VGPU_TRIANGLE_H_MIN_LENGTH},
               offset % VGPU_TRIANGLE_H_ALIGNMENT == 0 ?
               VGPU_TRIANGLE_STATE_CORE_VERIFIED : VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
        put16(shifted + offset + 0x3d64, UINT16_C(0x1b00));
        expect((vgc_view){shifted + offset, VGPU_TRIANGLE_H_MIN_LENGTH},
               offset % VGPU_TRIANGLE_H_ALIGNMENT == 0 ?
               VGPU_TRIANGLE_STATE_UNSUPPORTED : VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    }
    /* 对齐输出覆盖输入开头、尾端、未读取尾部以及仅1字节的交集。 */
    union { vgpu_triangle_state align; unsigned char bytes[VGPU_TRIANGLE_H_MIN_LENGTH + 1024]; } arena;
    unsigned char snapshot[sizeof(arena)];
    const size_t offsets[] = {0, 8, 0x4978, 0x4980};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        memset(&arena, 0xa5, sizeof(arena));
        memcpy(arena.bytes, h, sizeof(h));
        memcpy(snapshot, &arena, sizeof(arena));
        CHECK(vgpu_triangle_state_decode((vgc_view){arena.bytes, sizeof(h)},
              (vgpu_triangle_state *)(void *)(arena.bytes + offsets[i])) == VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
        CHECK(memcmp(snapshot, &arena, sizeof(arena)) == 0);
    }
    size_t prefix = sizeof(vgpu_triangle_state) - 1;
    memcpy(arena.bytes + prefix, h, VGPU_TRIANGLE_H_MIN_LENGTH);
    memcpy(snapshot, &arena, sizeof(arena));
    CHECK(vgpu_triangle_state_decode((vgc_view){arena.bytes + prefix, VGPU_TRIANGLE_H_MIN_LENGTH},
          &arena.align) == VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
    CHECK(memcmp(snapshot, &arena, sizeof(arena)) == 0);
    /* 紧邻但不重叠的输出可用。 */
    memcpy(arena.bytes + sizeof(vgpu_triangle_state), h, VGPU_TRIANGLE_H_MIN_LENGTH);
    CHECK(vgpu_triangle_state_decode((vgc_view){arena.bytes + sizeof(vgpu_triangle_state),
          VGPU_TRIANGLE_H_MIN_LENGTH}, &arena.align) == VGPU_TRIANGLE_STATE_CORE_VERIFIED);
}

static void protect(unsigned char *data, size_t length, int readable)
{
#if defined(_WIN32)
    DWORD previous;
    CHECK(VirtualProtect(data, length, readable ? PAGE_READONLY : PAGE_NOACCESS, &previous) != 0);
#else
    CHECK(mprotect(data, length, readable ? PROT_READ : PROT_NONE) == 0);
#endif
}

static void guard_tests(void)
{
    size_t page;
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info); page = info.dwPageSize;
#else
    long page_value = sysconf(_SC_PAGESIZE);
    CHECK(page_value > 0); page = (size_t)page_value;
#endif
    size_t span = ((VGPU_TRIANGLE_H_MIN_LENGTH + page - 1) / page) * page;
    size_t total = span + page;
    for (unsigned mode = 0; mode < 2; ++mode) {
#if defined(_WIN32)
        unsigned char *memory = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(memory != NULL);
#else
        unsigned char *memory = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(memory != MAP_FAILED);
#endif
        reset();
        /* 奇数端点向下对齐，末字段后仅一个可读填充字节。 */
        size_t padded = (VGPU_TRIANGLE_H_MIN_LENGTH + VGPU_TRIANGLE_H_ALIGNMENT - 1) /
                        VGPU_TRIANGLE_H_ALIGNMENT * VGPU_TRIANGLE_H_ALIGNMENT;
        unsigned char *input = mode == 0 ? memory + span - padded : memory;
        memcpy(input, h, VGPU_TRIANGLE_H_MIN_LENGTH);
        protect(memory, total, 1);
        protect(memory + span, page, 0);
        if (mode == 1 && page <= 0x1830)
            protect(memory, page, 0); /* H首部空洞完全不可读。 */
        expect((vgc_view){input, VGPU_TRIANGLE_H_MIN_LENGTH}, VGPU_TRIANGLE_STATE_CORE_VERIFIED);
        expect((vgc_view){input, VGPU_TRIANGLE_H_MIN_LENGTH - 1}, VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
        /* 未对齐且指向保护页：必须在读取任何字段前拒绝。 */
        expect((vgc_view){memory + span + 1, VGPU_TRIANGLE_H_MIN_LENGTH},
               VGPU_TRIANGLE_STATE_BAD_ARGUMENT);
#if defined(_WIN32)
        CHECK(VirtualFree(memory, 0, MEM_RELEASE) != 0);
#else
        CHECK(munmap(memory, total) == 0);
#endif
    }
}

static void error_tests(void)
{
    vgpu_triangle_error_latch latch;
    vgpu_triangle_error_init(&latch);
    CHECK(vgpu_triangle_error_get(&latch) == VGPU_GL_NO_ERROR);
    vgpu_triangle_error_record(&latch, VGPU_GL_INVALID_OPERATION,
        VGPU_TRIANGLE_REJECT_STATE, VGPU_TRIANGLE_BACKEND_OK);
    vgpu_triangle_error_record(&latch, VGPU_GL_OUT_OF_MEMORY,
        VGPU_TRIANGLE_REJECT_BACKEND, VGPU_TRIANGLE_BACKEND_DEVICE_LOST);
    vgpu_triangle_error_record(&latch, VGPU_GL_NO_ERROR,
        VGPU_TRIANGLE_REJECT_NONE, VGPU_TRIANGLE_BACKEND_COMPLETION_UNKNOWN);
    CHECK(latch.reject_reason == VGPU_TRIANGLE_REJECT_STATE);
    CHECK(latch.backend_fault == VGPU_TRIANGLE_BACKEND_DEVICE_LOST);
    CHECK(vgpu_triangle_error_get(&latch) == VGPU_GL_INVALID_OPERATION);
    CHECK(vgpu_triangle_error_get(&latch) == VGPU_GL_NO_ERROR);
    CHECK(latch.reject_reason == VGPU_TRIANGLE_REJECT_STATE);
    CHECK(latch.backend_fault == VGPU_TRIANGLE_BACKEND_DEVICE_LOST);
    vgpu_triangle_error_record(&latch, VGPU_GL_OUT_OF_MEMORY,
        VGPU_TRIANGLE_REJECT_BACKEND, VGPU_TRIANGLE_BACKEND_OK);
    CHECK(vgpu_triangle_error_get(&latch) == VGPU_GL_OUT_OF_MEMORY);
    CHECK(latch.backend_fault == VGPU_TRIANGLE_BACKEND_DEVICE_LOST);
    vgpu_triangle_error_init(&latch);
    vgpu_triangle_error_record(&latch, VGPU_GL_NO_ERROR,
        VGPU_TRIANGLE_REJECT_BACKEND, VGPU_TRIANGLE_BACKEND_COMPLETION_UNKNOWN);
    CHECK(vgpu_triangle_error_get(&latch) == VGPU_GL_NO_ERROR);
    CHECK(latch.backend_fault == VGPU_TRIANGLE_BACKEND_COMPLETION_UNKNOWN);
}

int main(void)
{
    field_tests();
    polygon_tests();
    argument_tests();
    guard_tests();
    error_tests();
    printf("PASS triangle-state CPU checks=%u core-only all_state_verified=0\n", checks);
    return 0;
}
