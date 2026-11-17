#include "vgpu_clear_state.h"

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

static unsigned char h[VGPU_CLEAR_H_MIN_LENGTH + 64];
static unsigned char v[VGPU_CLEAR_V_MIN_LENGTH + 64];
static vgc_view hv, vv;

static void put32(unsigned char *p, uint32_t n) { memcpy(p, &n, sizeof(n)); }
static void put16(unsigned char *p, uint16_t n) { memcpy(p, &n, sizeof(n)); }

static void reset(void)
{
    /* 非零邻字段能检出将u8/u16错误扩成u32的实现。 */
    memset(h, 0xa6, sizeof(h));
    memset(v, 0xd5, sizeof(v));
    hv = (vgc_view){h, VGPU_CLEAR_H_MIN_LENGTH};
    vv = (vgc_view){v, VGPU_CLEAR_V_MIN_LENGTH};
    h[0x3c1c] = h[0x3c38] = h[0x3d84] = h[0x3e8c] = 0;
    h[0x3ca8] = 1; h[0x3ca9] = 3; h[0x3caa] = 0x81; h[0x3cab] = 0xff;
    put16(h + 0x3cae, 1);
    put32(h + 0x3e88, 0);
    const float rgba[4] = {0, 0.25f, 0.75f, 1};
    memcpy(v, rgba, sizeof(rgba));
    put32(v + 0x18, 0);
}

static void expect(vgc_view hi, vgc_view vi, uint32_t mask,
                   vgpu_clear_state_result expected)
{
    vgc_clear_desc output;
    unsigned char before[sizeof(output)], hs[sizeof(h)], vs[sizeof(v)];
    memset(&output, 0x5b, sizeof(output));
    memcpy(before, &output, sizeof(output));
    memcpy(hs, h, sizeof(h)); memcpy(vs, v, sizeof(v));
    CHECK(vgpu_clear_state_decode(hi, vi, mask, &output) == expected);
    CHECK(memcmp(h, hs, sizeof(h)) == 0);
    CHECK(memcmp(v, vs, sizeof(v)) == 0);
    if (expected != VGPU_CLEAR_STATE_OK) {
        CHECK(memcmp(before, &output, sizeof(output)) == 0);
    } else {
        CHECK(output.internal_mask == 1 && output.color_write_mask == 15);
        CHECK(output.scissor_enabled == 0 && output.srgb_enabled == 0);
        CHECK(memcmp(output.rgba, vi.data, sizeof(output.rgba)) == 0);
    }
}

static void state_tests(void)
{
    reset(); expect(hv, vv, 1, VGPU_CLEAR_STATE_OK);
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned n = 0; n < 256; ++n) {
            reset(); h[0x3ca8 + c] = (unsigned char)n;
            expect(hv, vv, 1, (n & 1) ? VGPU_CLEAR_STATE_OK : VGPU_CLEAR_STATE_UNSUPPORTED);
        }
    }
    for (unsigned n = 0; n < 256; ++n) {
        reset();
        memset(h + 0x3ca8, (int)n, 4);
        expect(hv, vv, 1, (n & 1) ? VGPU_CLEAR_STATE_OK : VGPU_CLEAR_STATE_UNSUPPORTED);
    }
    const size_t bytes[] = {0x3c1c, 0x3c38, 0x3d84, 0x3e8c};
    for (size_t i = 0; i < sizeof(bytes) / sizeof(bytes[0]); ++i) {
        for (unsigned n = 1; n < 256; ++n) {
            reset(); h[bytes[i]] = (unsigned char)n;
            expect(hv, vv, 1, VGPU_CLEAR_STATE_UNSUPPORTED);
        }
    }
    /* 所有汇总mask组合；高byte也不能漏读。 */
    for (uint32_t n = 0; n <= UINT16_MAX; ++n) {
        reset(); put16(h + 0x3cae, (uint16_t)n);
        expect(hv, vv, 1, n == 1 ? VGPU_CLEAR_STATE_OK : VGPU_CLEAR_STATE_UNSUPPORTED);
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        uint32_t flag = UINT32_C(1) << bit;
        reset(); put32(h + 0x3e88, flag);
        expect(hv, vv, 1, VGPU_CLEAR_STATE_UNSUPPORTED);
        h[0x3d84] = 1; expect(hv, vv, 1, VGPU_CLEAR_STATE_UNSUPPORTED);
        reset(); put32(v + 0x18, flag);
        expect(hv, vv, 1, VGPU_CLEAR_STATE_UNSUPPORTED);
        reset(); expect(hv, vv, flag, bit == 0 ? VGPU_CLEAR_STATE_OK : VGPU_CLEAR_STATE_UNSUPPORTED);
        if (bit != 0) expect(hv, vv, flag | 1, VGPU_CLEAR_STATE_UNSUPPORTED);
    }
    reset(); put32(v + 0x18, UINT32_MAX);
    expect(hv, vv, 1, VGPU_CLEAR_STATE_UNSUPPORTED);
    reset(); expect(hv, vv, UINT32_MAX, VGPU_CLEAR_STATE_UNSUPPORTED);
    expect(hv, vv, UINT32_C(0x4000), VGPU_CLEAR_STATE_UNSUPPORTED);
    /* 未选择depth/stencil及关闭的scissor矩形不参与语义。 */
    reset(); put32(v + 0x10, UINT32_C(0x7f800001)); put32(v + 0x14, UINT32_MAX);
    expect(hv, vv, 1, VGPU_CLEAR_STATE_OK);
    memset(h, 0xff, sizeof(h)); memset(v, 0xff, sizeof(v));
    expect(hv, vv, 0, VGPU_CLEAR_STATE_NOOP);
}

static void float_tests(void)
{
    const uint32_t accepted[] = {0, UINT32_C(0x80000000), 1, UINT32_C(0x007fffff),
        UINT32_C(0x00800000), UINT32_C(0x3f000000), UINT32_C(0x3f7fffff), UINT32_C(0x3f800000)};
    const uint32_t rejected[] = {UINT32_C(0x80000001), UINT32_C(0xbf800000),
        UINT32_C(0x3f800001), UINT32_C(0x7f7fffff), UINT32_C(0xff7fffff),
        UINT32_C(0x7f800000), UINT32_C(0xff800000), UINT32_C(0x7fc00000),
        UINT32_C(0xffc00000), UINT32_C(0x7f800001), UINT32_C(0xff800001), UINT32_MAX};
    for (size_t c = 0; c < 4; ++c) {
        for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
            reset(); put32(v + c * 4, accepted[i]); expect(hv, vv, 1, VGPU_CLEAR_STATE_OK);
        }
        for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
            reset(); put32(v + c * 4, rejected[i]); expect(hv, vv, 1, VGPU_CLEAR_STATE_UNSUPPORTED);
        }
    }
}

static void argument_tests(void)
{
    reset();
    for (size_t n = 0; n < VGPU_CLEAR_H_MIN_LENGTH; ++n)
        expect((vgc_view){h, n}, vv, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    for (size_t n = 0; n < VGPU_CLEAR_V_MIN_LENGTH; ++n)
        expect(hv, (vgc_view){v, n}, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    expect((vgc_view){NULL, hv.length}, vv, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    expect(hv, (vgc_view){NULL, vv.length}, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    expect((vgc_view){NULL, 0}, vv, 0, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    CHECK(vgpu_clear_state_decode(hv, vv, 1, NULL) == VGPU_CLEAR_STATE_BAD_ARGUMENT);
    expect((vgc_view){h, SIZE_MAX}, vv, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    expect(hv, (vgc_view){v, SIZE_MAX}, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    const void *near_end = (const void *)(UINTPTR_MAX - 15);
    expect((vgc_view){near_end, hv.length}, vv, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    expect(hv, (vgc_view){near_end, vv.length}, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    CHECK(vgpu_clear_state_decode(hv, vv, 1, (vgc_clear_desc *)(UINTPTR_MAX - 15)) == VGPU_CLEAR_STATE_BAD_ARGUMENT);
    CHECK(vgpu_clear_state_decode(hv, vv, 0, (vgc_clear_desc *)(UINTPTR_MAX - 15)) == VGPU_CLEAR_STATE_BAD_ARGUMENT);
    /* 两个输入可重叠；H/V每次重新读取，没有缓存。 */
    memcpy(h, v, VGPU_CLEAR_V_MIN_LENGTH);
    expect(hv, (vgc_view){h, VGPU_CLEAR_V_MIN_LENGTH}, 1, VGPU_CLEAR_STATE_OK);
    h[0x3c38] = 1;
    expect(hv, (vgc_view){h, VGPU_CLEAR_V_MIN_LENGTH}, 1, VGPU_CLEAR_STATE_UNSUPPORTED);
}

static void alias_tests(void)
{
    unsigned char arena[VGPU_CLEAR_H_MIN_LENGTH + 128], before[sizeof(arena)];
    for (unsigned which = 0; which < 2; ++which) {
        reset(); memset(arena, 0x91, sizeof(arena));
        size_t length = which ? sizeof(v) : sizeof(h);
        memcpy(arena + 32, which ? v : h, length);
        vgc_view input = {arena + 32, length};
        memcpy(before, arena, sizeof(arena));
        /* 包括输出跨输入起点/终点和只落在未读取尾部的重叠。 */
        for (size_t offset = 1; offset < 32 + length; ++offset) {
            vgc_clear_desc *out = (vgc_clear_desc *)(void *)(arena + offset);
            CHECK(vgpu_clear_state_decode(which ? hv : input, which ? input : vv, 1, out) == VGPU_CLEAR_STATE_BAD_ARGUMENT);
            CHECK(memcmp(arena, before, sizeof(arena)) == 0);
        }
        CHECK(vgpu_clear_state_decode(which ? hv : input, which ? input : vv, 0,
              (vgc_clear_desc *)(void *)(arena + 32)) == VGPU_CLEAR_STATE_BAD_ARGUMENT);
        /* 半开区间紧邻不算别名；也覆盖未对齐输出。 */
        CHECK(vgpu_clear_state_decode(which ? hv : input, which ? input : vv, 1,
              (vgc_clear_desc *)(void *)arena) == VGPU_CLEAR_STATE_OK);
        CHECK(vgpu_clear_state_decode(which ? hv : input, which ? input : vv, 1,
              (vgc_clear_desc *)(void *)(arena + 32 + length)) == VGPU_CLEAR_STATE_OK);
        CHECK(memcmp(arena + 32, before + 32, length) == 0);
    }
}

static void guard_tests(void)
{
    size_t page;
    unsigned char *memory;
#if defined(_WIN32)
    SYSTEM_INFO info; GetSystemInfo(&info); page = (size_t)info.dwPageSize;
    size_t readable = ((VGPU_CLEAR_H_MIN_LENGTH + page - 1) / page) * page;
    memory = VirtualAlloc(NULL, readable + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory != NULL);
    DWORD previous;
    CHECK(VirtualProtect(memory + readable, page, PAGE_NOACCESS, &previous) != 0);
#else
    long page_size = sysconf(_SC_PAGESIZE); CHECK(page_size > 0); page = (size_t)page_size;
    size_t readable = ((VGPU_CLEAR_H_MIN_LENGTH + page - 1) / page) * page;
    memory = mmap(NULL, readable + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    CHECK(mprotect(memory + readable, page, PROT_NONE) == 0);
#endif
    /* 最后一字节紧贴保护页，真实不可读尾部；H起点故意未对齐。 */
    reset();
    unsigned char *end = memory + readable;
    memcpy(end - VGPU_CLEAR_H_MIN_LENGTH, h, VGPU_CLEAR_H_MIN_LENGTH);
    expect((vgc_view){end - VGPU_CLEAR_H_MIN_LENGTH, VGPU_CLEAR_H_MIN_LENGTH}, vv, 1, VGPU_CLEAR_STATE_OK);
    expect((vgc_view){end - 1, VGPU_CLEAR_H_MIN_LENGTH - 1}, vv, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    memcpy(end - VGPU_CLEAR_V_MIN_LENGTH, v, VGPU_CLEAR_V_MIN_LENGTH);
    expect(hv, (vgc_view){end - VGPU_CLEAR_V_MIN_LENGTH, VGPU_CLEAR_V_MIN_LENGTH}, 1, VGPU_CLEAR_STATE_OK);
    expect(hv, (vgc_view){end - 1, VGPU_CLEAR_V_MIN_LENGTH - 1}, 1, VGPU_CLEAR_STATE_BAD_ARGUMENT);
    /* 只读输入页验证实现不尝试写回。 */
    memcpy(memory + 1, h, VGPU_CLEAR_H_MIN_LENGTH);
    memcpy(memory + 1, v, VGPU_CLEAR_V_MIN_LENGTH);
#if defined(_WIN32)
    CHECK(VirtualProtect(memory, readable, PAGE_READONLY, &previous) != 0);
#else
    CHECK(mprotect(memory, readable, PROT_READ) == 0);
#endif
    expect((vgc_view){memory + 1, VGPU_CLEAR_H_MIN_LENGTH},
           (vgc_view){memory + 1, VGPU_CLEAR_V_MIN_LENGTH}, 1, VGPU_CLEAR_STATE_OK);
#if defined(_WIN32)
    CHECK(VirtualFree(memory, 0, MEM_RELEASE) != 0);
#else
    CHECK(munmap(memory, readable + page) == 0);
#endif
    /* 未对齐的V也必须成功。 */
    reset(); unsigned char unaligned[VGPU_CLEAR_V_MIN_LENGTH + 1];
    memcpy(unaligned + 1, v, VGPU_CLEAR_V_MIN_LENGTH);
    expect(hv, (vgc_view){unaligned + 1, VGPU_CLEAR_V_MIN_LENGTH}, 1, VGPU_CLEAR_STATE_OK);
}

int main(void)
{
    state_tests(); float_tests(); argument_tests(); alias_tests(); guard_tests();
    printf("PASS clear_state CPU checks=%u\n", checks);
    return 0;
}
