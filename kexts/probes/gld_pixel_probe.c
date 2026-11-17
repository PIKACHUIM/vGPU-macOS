#define _DARWIN_C_SOURCE
#include "gld_pixel.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

/* 仅结构/解析验证。能力表是测试夹具，不代表设备能力；不调用任何 CGL/GLD ABI。 */
static unsigned checks, failures;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        fprintf(stderr, "FAIL line=%d expression=%s\n", __LINE__, #expression); \
    } \
} while (0)
#define WORDS(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t get32(const gp_pixel_node *node, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)node + offset, sizeof(value));
    return value;
}

static void parse_status(const uint32_t *words, size_t bytes, gp_status expected)
{
    gp_request request, before;
    memset(&request, 0xa5, sizeof(request));
    memcpy(&before, &request, sizeof(before));
    gp_status status = gp_parse_normalized(words, bytes, &request);
    CHECK(status == expected);
    if (status != GP_OK) CHECK(memcmp(&request, &before, sizeof(request)) == 0);
}

static void parser_tests(void)
{
    const uint32_t valid[] = {5, 8, 24, 11, 8, 12, 24, 13, 8, 4, 0};
    const uint32_t empty[] = {0};
    const uint32_t token4[] = {4, 5, 4, 4, 0, 999};
    const uint32_t duplicate[] = {8, 32, 8, 16, 11, 8, 11, 0, 12, 16, 12, 24,
                                  13, 8, 13, 0, 5, 5, 0};
    gp_request request;
    CHECK(gp_parse_normalized(valid, sizeof(valid), &request) == GP_OK);
    CHECK(request.color_min == 24 && request.alpha_min == 8 &&
          request.depth_min == 24 && request.stencil_min == 8 &&
          request.double_buffer == 1 && request.consumed_words == WORDS(valid));
    CHECK(gp_parse_normalized(token4, sizeof(token4), &request) == GP_OK);
    CHECK(request.double_buffer == 1 && request.consumed_words == 5);
    CHECK(gp_parse_normalized(duplicate, sizeof(duplicate), &request) == GP_OK);
    CHECK(request.color_min == 32 && request.alpha_min == 8 && request.depth_min == 24 &&
          request.stencil_min == 8 && request.double_buffer == 1);
    const uint32_t offscreen[] = {53, 53, 4, 5, 8, 24, 11, 8, 12, 16, 13, 8, 0, 999};
    CHECK(gp_parse_normalized(offscreen, sizeof(offscreen), &request) == GP_OK);
    CHECK(request.offscreen == 1 && request.double_buffer == 1 &&
          request.color_min == 24 && request.alpha_min == 8 &&
          request.depth_min == 16 && request.stencil_min == 8 &&
          request.consumed_words == WORDS(offscreen) - 1);
    const uint32_t offscreen_only[] = {53, 0};
    CHECK(gp_parse_normalized(offscreen_only, sizeof(offscreen_only), &request) == GP_OK);
    CHECK(request.offscreen == 1 && request.double_buffer == 0 && request.consumed_words == 2);
    parse_status(offscreen_only, sizeof(uint32_t), GP_UNTERMINATED);
    const uint32_t offscreen_truncated[] = {53, 12};
    parse_status(offscreen_truncated, sizeof(offscreen_truncated), GP_TRUNCATED_VALUE);
    const uint32_t no_recovery[] = {53, 72, 0};
    parse_status(no_recovery, sizeof(no_recovery), GP_UNSUPPORTED_TOKEN);
    CHECK(gp_parse_normalized(empty, sizeof(empty), &request) == GP_OK);
    CHECK(request.offscreen == 0);
    parse_status(empty, sizeof(empty), GP_OK);
    parse_status(NULL, 0, GP_UNTERMINATED);
    parse_status(NULL, 4, GP_BAD_ARGUMENT);
    CHECK(gp_parse_normalized(empty, sizeof(empty), NULL) == GP_BAD_ARGUMENT);
    parse_status(empty, SIZE_MAX, GP_OVERFLOW);
    parse_status(empty, (size_t)PTRDIFF_MAX + 1, GP_OVERFLOW);
    const gp_status prefix[] = {GP_UNTERMINATED, GP_UNTERMINATED, GP_TRUNCATED_VALUE,
        GP_UNTERMINATED, GP_TRUNCATED_VALUE, GP_UNTERMINATED, GP_TRUNCATED_VALUE,
        GP_UNTERMINATED, GP_TRUNCATED_VALUE, GP_UNTERMINATED, GP_UNTERMINATED, GP_OK};
    for (size_t n = 0; n <= sizeof(valid); ++n) {
        parse_status(valid, n, n % 4 ? GP_BAD_ARGUMENT : prefix[n / 4]);
    }
    const uint32_t valued[] = {8, 11, 12, 13};
    for (size_t i = 0; i < WORDS(valued); ++i) {
        uint32_t pair[] = {valued[i], 0, 0};
        parse_status(pair, 4, GP_TRUNCATED_VALUE);
        parse_status(pair, 8, GP_UNTERMINATED); /* value=0 不能冒充 terminator。 */
        parse_status(pair, 12, GP_OK);
        pair[1] = INT32_MAX;
        parse_status(pair, 12, GP_OK);
        pair[1] = UINT32_C(0x80000000);
        parse_status(pair, 12, GP_BAD_VALUE);
        pair[1] = UINT32_MAX;
        parse_status(pair, 12, GP_BAD_VALUE);
    }
    for (uint32_t token = 1; token <= 102; ++token) {
        if (token == 4 || token == 5 || token == 8 || token == 11 || token == 12 || token == 13 || token == 53)
            continue;
        parse_status(&token, sizeof(token), GP_UNSUPPORTED_TOKEN);
    }
    const uint32_t unknowns[] = {999, UINT32_MAX, UINT32_C(0x80000000)};
    for (size_t i = 0; i < WORDS(unknowns); ++i)
        parse_status(&unknowns[i], 4, GP_UNSUPPORTED_TOKEN);
    unsigned char unaligned[sizeof(valid) + 1];
    memcpy(unaligned + 1, valid, sizeof(valid));
    CHECK(gp_parse_normalized(unaligned + 1, sizeof(valid), &request) == GP_OK);
    puts("GROUP parser: valid, duplicate-max, token4-no-value, every-byte-truncation, missing-terminator, unknown, overflow");
}

static const gp_format_cap fixture[] = {
    {0x1234, 0, GP_BUFFER_MONO_SINGLE, GP_COLOR_RGB888, 0, 0, 1},
    {0x2345, GP_FLAG_OFFSCREEN, GP_BUFFER_MONO_DOUBLE, GP_COLOR_ARGB8888, 24, 8, 2},
    {0x3456, GP_FLAG_WINDOW, GP_BUFFER_MONO_SINGLE, GP_COLOR_ARGB8888, 16, 0, 4}
};

struct allocation_state { size_t allocations, releases, bytes; int fail; };
static void *count_allocate(void *context, size_t bytes)
{
    struct allocation_state *state = context;
    ++state->allocations;
    state->bytes = bytes;
    return state->fail ? NULL : malloc(bytes);
}
static void count_release(void *context, void *allocation)
{
    struct allocation_state *state = context;
    ++state->releases;
    free(allocation);
}

static void selection_tests(void)
{
    const uint32_t empty[] = {4, 0};
    const uint32_t request[] = {5, 8, 24, 11, 8, 12, 24, 13, 8, 4, 0};
    gp_pixel_list list = {0};
    struct allocation_state state = {0};
    const gp_allocator allocator = {&state, count_allocate, count_release};
    CHECK(gp_choose_normalized(empty, sizeof(empty), NULL, 0, &allocator, &list) == GP_NO_MATCH);
    CHECK(state.allocations == 0 && list.head == NULL && list.count == 0);
    CHECK(gp_choose_normalized(request, sizeof(request), fixture, WORDS(fixture), &allocator, &list) == GP_OK);
    CHECK(list.count == 1 && list.head != NULL);
    if (list.head != NULL) {
        CHECK(list.head->next == NULL && list.head->backend_format_index == 1);
        CHECK(get32(list.head, 0x08) == 0x2345 && get32(list.head, 0x0c) == GP_FLAG_OFFSCREEN);
        CHECK(get32(list.head, 0x10) == 8 && get32(list.head, 0x14) == 0x8000);
        CHECK(get32(list.head, 0x18) == UINT32_C(0x80000000));
        CHECK(get32(list.head, 0x1c) == 0x800 && get32(list.head, 0x20) == 0x80);
        CHECK(get32(list.head, 0x34) == 2);
        for (size_t i = 0x24; i < 0x34; ++i)
            CHECK(((const unsigned char *)list.head)[i] == 0);
        unsigned char prefix[0x38];
        memcpy(prefix, list.head, sizeof(prefix));
        CHECK(memcmp(prefix, list.head, sizeof(prefix)) == 0);
    }
    CHECK(state.allocations == 1 && state.bytes == sizeof(gp_pixel_node));
    gp_pixel_list before = list;
    CHECK(gp_choose_normalized(empty, sizeof(empty), fixture, 3, NULL, &list) == GP_BAD_ARGUMENT);
    CHECK(list.head == before.head && list.allocation == before.allocation && list.count == before.count);
    gp_pixel_list_destroy(&list);
    CHECK(state.releases == 1 && list.head == NULL && list.allocation == NULL && list.count == 0);
    gp_pixel_list_destroy(&list);
    gp_pixel_list_destroy(NULL);
    CHECK(state.releases == 1);

    CHECK(gp_choose_normalized(empty, sizeof(empty), fixture, 3, &allocator, &list) == GP_OK);
    if (list.head != NULL) {
        const gp_pixel_node *node = list.head;
        for (size_t i = 0; i < 3; ++i) {
            CHECK(node != NULL);
            if (node == NULL) break;
            CHECK(node->backend_format_index == i);
            CHECK(get32(node, 0x10) == (i == 1 ? 8U : 0U));
            CHECK(fixture[i].buffer_mode == (i == 1 ? 9U : 5U));
            CHECK((get32(node, 0x0c) & (GP_FLAG_ACCELERATED | GP_FLAG_COMPLIANT |
                   GP_FLAG_CORE32 | GP_FLAG_CORE41)) == 0);
            node = node->next;
        }
        CHECK(node == NULL && list.count == 3);
        CHECK(get32(list.head, 0x1c) == 1 && get32(list.head, 0x20) == 1);
        /* owner 释放不依赖消费者能改写的 next：环也不能使释放循环或重复 free。 */
        list.head->next = list.head;
    }
    gp_pixel_list_destroy(&list);
    CHECK(state.releases == 2);
    state.fail = 1;
    CHECK(gp_choose_normalized(empty, sizeof(empty), fixture, 3, &allocator, &list) == GP_NO_MEMORY);
    CHECK(list.head == NULL && list.count == 0 && list.allocation == NULL && state.releases == 2);
    state.fail = 0;

    uint32_t impossible[] = {8, 33, 0};
    CHECK(gp_choose_normalized(impossible, sizeof(impossible), fixture, 3, NULL, &list) == GP_NO_MATCH);
    const uint32_t ids[] = {8, 11, 12, 13};
    const uint32_t sizes[] = {33, 9, 25, 9};
    for (size_t i = 0; i < WORDS(ids); ++i) {
        impossible[0] = ids[i]; impossible[1] = sizes[i];
        CHECK(gp_choose_normalized(impossible, sizeof(impossible), fixture, 3, NULL, &list) == GP_NO_MATCH);
        impossible[1] = INT32_MAX;
        CHECK(gp_choose_normalized(impossible, sizeof(impossible), fixture, 3, NULL, &list) == GP_NO_MATCH);
    }
    const uint32_t offscreen[] = {53, 0};
    gp_format_cap surface_caps[] = {fixture[2], fixture[1]};
    CHECK(gp_choose_normalized(offscreen, sizeof(offscreen), surface_caps, 1, NULL, &list) == GP_NO_MATCH);
    CHECK(list.head == NULL && list.allocation == NULL && list.count == 0);
    CHECK(gp_choose_normalized(offscreen, sizeof(offscreen), surface_caps, 2, NULL, &list) == GP_OK);
    CHECK(list.count == 1);
    if (list.head != NULL) {
        CHECK(list.head->backend_format_index == 1 && list.head->next == NULL);
        CHECK(get32(list.head, 0x0c) == surface_caps[1].flags);
    }
    gp_pixel_list_destroy(&list);
    CHECK(gp_choose_normalized(empty, sizeof(empty), surface_caps, 2, NULL, &list) == GP_OK);
    CHECK(list.count == 2);
    if (list.head != NULL) {
        CHECK(list.head->backend_format_index == 0 && get32(list.head, 0x0c) == GP_FLAG_WINDOW);
        CHECK(list.head->next != NULL);
        if (list.head->next != NULL)
            CHECK(get32(list.head->next, 0x0c) == GP_FLAG_OFFSCREEN);
    }
    gp_pixel_list_destroy(&list);
    const uint32_t doubles[] = {5, 0};
    CHECK(gp_choose_normalized(doubles, sizeof(doubles), fixture, 1, NULL, &list) == GP_NO_MATCH);
    const uint32_t color24[] = {8, 24, 0};
    CHECK(gp_choose_normalized(color24, sizeof(color24), fixture, 1, NULL, &list) == GP_OK);
    gp_pixel_list_destroy(&list);

    gp_format_cap cap = fixture[0];
    cap.flags = GP_FLAG_ACCELERATED | GP_FLAG_COMPLIANT | GP_FLAG_CORE32 | GP_FLAG_CORE41;
    CHECK(gp_choose_normalized(empty, sizeof(empty), &cap, 1, NULL, &list) == GP_OK);
    if (list.head != NULL) CHECK(get32(list.head, 0x0c) == cap.flags);
    gp_pixel_list_destroy(&list);
    const uint32_t bits[] = {0, 8, 16, 24, 32};
    const uint32_t modes[] = {1, 0x80, 0x400, 0x800, 0x1000};
    for (size_t i = 0; i < WORDS(bits); ++i) {
        cap = fixture[0]; cap.depth_bits = bits[i]; cap.stencil_bits = bits[i];
        CHECK(gp_choose_normalized(empty, sizeof(empty), &cap, 1, NULL, &list) == GP_OK);
        if (list.head != NULL)
            CHECK(get32(list.head, 0x1c) == modes[i] && get32(list.head, 0x20) == modes[i]);
        gp_pixel_list_destroy(&list);
    }
    gp_format_cap invalid[2] = {fixture[0], fixture[1]};
    for (unsigned i = 0; i < 5; ++i) {
        invalid[1] = fixture[1];
        switch (i) {
        case 0: invalid[1].color_mode = 0xc000; break;
        case 1: invalid[1].depth_bits = 25; break;
        case 2: invalid[1].stencil_bits = UINT32_MAX; break;
        case 3: invalid[1].buffer_mode = 0x19; break;
        default: invalid[1].flags = 0x80000000U; break;
        }
        size_t count = state.allocations;
        CHECK(gp_choose_normalized(empty, sizeof(empty), invalid, 2, &allocator, &list) == GP_UNSUPPORTED_FORMAT);
        CHECK(state.allocations == count && list.head == NULL);
    }
    const uint32_t invalid_buffers[] = {0, 1, 4, 8, 0xd, 0x15, 0x19, UINT32_MAX};
    for (size_t i = 0; i < WORDS(invalid_buffers); ++i) {
        invalid[1] = fixture[1];
        invalid[1].buffer_mode = invalid_buffers[i];
        size_t count = state.allocations;
        CHECK(gp_choose_normalized(empty, sizeof(empty), invalid, 2, &allocator, &list) == GP_UNSUPPORTED_FORMAT);
        CHECK(state.allocations == count && list.head == NULL &&
              list.allocation == NULL && list.count == 0);
        CHECK(gp_choose_normalized(doubles, sizeof(doubles), invalid, 2, &allocator, &list) == GP_UNSUPPORTED_FORMAT);
        CHECK(state.allocations == count && list.head == NULL &&
              list.allocation == NULL && list.count == 0);
    }
    CHECK(gp_choose_normalized(empty, sizeof(empty), fixture, SIZE_MAX, NULL, &list) == GP_OVERFLOW);
    CHECK(gp_choose_normalized(empty, sizeof(empty), fixture,
        (size_t)PTRDIFF_MAX / sizeof(gp_pixel_node) + 1, NULL, &list) == GP_OVERFLOW);
    CHECK(gp_choose_normalized(empty, sizeof(empty), NULL, 1, NULL, &list) == GP_BAD_ARGUMENT);
    CHECK(gp_choose_normalized(empty, sizeof(empty), fixture, 1, NULL, NULL) == GP_BAD_ARGUMENT);
    const gp_allocator bad_allocator = {NULL, count_allocate, NULL};
    CHECK(gp_choose_normalized(empty, sizeof(empty), fixture, 1, &bad_allocator, &list) == GP_BAD_ARGUMENT);
    const uint32_t unknown[] = {73};
    size_t count = state.allocations;
    CHECK(gp_choose_normalized(unknown, sizeof(unknown), fixture, 3, &allocator, &list) == GP_UNSUPPORTED_TOKEN);
    CHECK(state.allocations == count);
    puts("GROUP selection: explicit-capabilities, no-default-flags, bitmaps, no-match, invalid-capability, allocation-failure, list-release");
}

static void long_list_tests(void)
{
    const uint32_t tokens[] = {4, 0};
    const size_t count = 4096;
    gp_format_cap *formats = malloc(count * sizeof(*formats));
    CHECK(formats != NULL);
    if (formats == NULL) return;
    for (size_t i = 0; i < count; ++i) formats[i] = fixture[0];
    gp_pixel_list list = {0};
    CHECK(gp_choose_normalized(tokens, sizeof(tokens), formats, count, NULL, &list) == GP_OK);
    const gp_pixel_node *node = list.head;
    size_t visited = 0;
    while (node != NULL && visited < count) {
        CHECK(node->backend_format_index == visited);
        ++visited;
        node = node->next;
    }
    CHECK(visited == count && node == NULL);
    gp_pixel_list_destroy(&list);
    free(formats);
    puts("GROUP lifetime: 4096-node chain and one-owner destruction");
}

static void guarded_tests(void)
{
#if defined(__APPLE__)
    long size = sysconf(_SC_PAGESIZE);
    CHECK(size > 0);
    if (size <= 0) return;
    size_t page = (size_t)size;
    unsigned char *mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mapping != MAP_FAILED);
    if (mapping == MAP_FAILED) return;
    CHECK(mprotect(mapping + page, page, PROT_NONE) == 0);
    gp_request output;
    CHECK(gp_parse_normalized(mapping + page, 0, &output) == GP_UNTERMINATED);
    const uint32_t endings[] = {0, 4, 5, 8, 11, 12, 13, 53, 99};
    for (size_t i = 0; i < WORDS(endings); ++i) {
        memcpy(mapping + page - 4, &endings[i], 4);
        gp_status expected = endings[i] == 0 ? GP_OK : endings[i] == 99 ? GP_UNSUPPORTED_TOKEN :
            (endings[i] <= 5 || endings[i] == 53) ? GP_UNTERMINATED : GP_TRUNCATED_VALUE;
        CHECK(gp_parse_normalized(mapping + page - 4, 4, &output) == expected);
    }
    const uint32_t zero_value[] = {8, 0};
    memcpy(mapping + page - sizeof(zero_value), zero_value, sizeof(zero_value));
    CHECK(gp_parse_normalized(mapping + page - 8, 8, &output) == GP_UNTERMINATED);
    const uint32_t done[] = {5, 4, 0};
    memcpy(mapping + page - sizeof(done), done, sizeof(done));
    CHECK(gp_parse_normalized(mapping + page - sizeof(done), sizeof(done), &output) == GP_OK);
    uint32_t seed = 0x51a7f00d;
    const uint32_t pool[] = {0, 4, 5, 8, 11, 12, 13, 53, 73, 99, 24, UINT32_MAX, INT32_MAX};
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        seed = seed * 1664525U + 1013904223U;
        size_t bytes = (seed % 32) * 4;
        unsigned char *start = mapping + page - bytes;
        for (size_t n = 0; n < bytes; n += 4) {
            seed = seed * 1664525U + 1013904223U;
            uint32_t token = pool[seed % WORDS(pool)];
            memcpy(start + n, &token, 4);
        }
        gp_status status = gp_parse_normalized(start, bytes, &output);
        CHECK(status >= GP_OK && status <= GP_OVERFLOW);
        if (status == GP_OK) CHECK(output.consumed_words <= bytes / 4);
    }
    CHECK(munmap(mapping, page * 2) == 0);
    puts("GROUP bounds: PROT_NONE next-page guard, zero-length inaccessible pointer, 20000 deterministic streams");
#else
    puts("SKIP guard-page tests: requires macOS mmap in this probe");
#endif
}

int main(void)
{
    puts("MODE structural/parser tests only; fixture capabilities are not device claims; no CGL or private ABI calls");
    parser_tests();
    selection_tests();
    long_list_tests();
    guarded_tests();
    printf("SUMMARY checks=%u failures=%u\n", checks, failures);
    return failures != 0;
}
