#include "gld_pixel_adapter.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t checks, failures;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); } } while (0)

/* 仅为结构测试夹具，不证明任何GPU支持。flags特意为0。 */
static const gp_format_cap fixture[] = {
    {0x1200, 0, GP_BUFFER_MONO_SINGLE, GP_COLOR_RGB888, 0, 0, 1},
    {0x1200, 0, GP_BUFFER_MONO_DOUBLE, GP_COLOR_ARGB8888, 24, 8, 1}
};

static void expect(const void *input, int32_t wanted)
{
    void *head = (void *)(uintptr_t)1;
    uint64_t generation = UINT64_MAX;
    CHECK(gpa_choose(&head, input, &generation) == wanted);
    if (wanted == GP_OK) {
        CHECK(head != NULL && generation != 0);
        CHECK(gpa_destroy(head, generation) == GP_OK);
        CHECK(gpa_destroy(head, generation) == GPA_INVALID_HANDLE);
    } else {
        CHECK(head == NULL && generation == 0);
    }
}

static void test_inputs(void)
{
    uint32_t zero = 0;
    void *head = (void *)(uintptr_t)1;
    uint64_t generation = 99;
    expect(&zero, GP_NO_MATCH);
    CHECK(gpa_configure(fixture, sizeof(fixture) / sizeof(fixture[0])) == GP_OK);
    expect(NULL, GP_BAD_ARGUMENT);
    CHECK(gpa_choose(NULL, &zero, &generation) == GP_BAD_ARGUMENT);
    CHECK(generation == 0);
    CHECK(gpa_choose(&head, &zero, NULL) == GP_BAD_ARGUMENT);
    CHECK(head == NULL);
    expect((const void *)(uintptr_t)1, GPA_READ_FAILED);
    expect((const void *)(UINTPTR_MAX - 2), GP_OVERFLOW);

    const uint32_t valid[] = {4, 5, 8, 24, 11, 8, 12, 24, 13, 8, 0};
    expect(valid, GP_OK);
    const uint32_t zeros[] = {8, 0, 11, 0, 12, 0, 13, 0, 0};
    expect(zeros, GP_OK);
    const uint32_t no_match[] = {8, 33, 0};
    expect(no_match, GP_NO_MATCH);
    const uint32_t negative[] = {8, UINT32_MAX};
    expect(negative, GP_BAD_VALUE);
    for (uint32_t token = 1; token < 104; ++token) {
        if (token == 4 || token == 5 || token == 8 || token == 11 ||
            token == 12 || token == 13 || token == 53) continue;
        expect(&token, GP_UNSUPPORTED_TOKEN);
    }
    uint32_t *short_allocation = malloc(sizeof(uint32_t));
    CHECK(short_allocation != NULL);
    if (short_allocation != NULL) {
        *short_allocation = 0;
        expect(short_allocation, GP_OK);
        *short_allocation = 73;
        expect(short_allocation, GP_UNSUPPORTED_TOKEN);
        free(short_allocation);
    }
    unsigned char *unaligned = malloc(sizeof(valid) + 1);
    CHECK(unaligned != NULL);
    if (unaligned != NULL) {
        memcpy(unaligned + 1, valid, sizeof(valid));
        expect(unaligned + 1, GP_OK);
        free(unaligned);
    }
    uint32_t limit[GPA_MAX_WORDS + 1];
    for (size_t i = 0; i < GPA_MAX_WORDS + 1; ++i) limit[i] = 4;
    limit[GPA_MAX_WORDS] = 0;
    expect(limit, GPA_WORD_LIMIT);
    limit[GPA_MAX_WORDS - 1] = 0;
    expect(limit, GP_OK);
    limit[GPA_MAX_WORDS - 1] = 8;
    expect(limit, GPA_WORD_LIMIT);
    limit[GPA_MAX_WORDS - 2] = 8;
    limit[GPA_MAX_WORDS - 1] = 0;
    expect(limit, GPA_WORD_LIMIT); /* 值0未终止。 */
    limit[GPA_MAX_WORDS - 1] = UINT32_MAX;
    expect(limit, GP_BAD_VALUE);
}

static void test_offscreen(void)
{
    const uint32_t offscreen[] = {53, 0};
    const uint32_t following[] = {53, 53, 4, 5, 8, 24, 11, 8, 12, 24, 13, 8, 0};
    const uint32_t no_recovery[] = {53, 72, 0};
    const uint32_t too_deep[] = {53, 12, 32, 0};
    const uint32_t empty[] = {0};
    gp_format_cap caps[] = {fixture[0], fixture[1]};
    caps[0].flags = GP_FLAG_WINDOW;
    caps[1].flags = GP_FLAG_OFFSCREEN | GP_FLAG_WINDOW;
    CHECK(gpa_configure(caps, 1) == GP_OK);
    expect(offscreen, GP_NO_MATCH);
    expect(empty, GP_OK);
    CHECK(gpa_configure(caps, 2) == GP_OK);
    expect(no_recovery, GP_UNSUPPORTED_TOKEN);
    expect(too_deep, GP_NO_MATCH);
    for (size_t i = 0; i < 2; ++i) {
        void *head = NULL;
        uint64_t generation = 0;
        CHECK(gpa_choose(&head, i == 0 ? offscreen : following, &generation) == GP_OK);
        CHECK(head != NULL && generation != 0);
        if (head != NULL) {
            const gp_pixel_node *node = head;
            uint32_t flags = 0;
            memcpy(&flags, node->fields + 4, sizeof(flags));
            CHECK(node->next == NULL && node->backend_format_index == 1);
            CHECK(flags == caps[1].flags);
            uint32_t buffer;
            memcpy(&buffer, node->fields + 8, sizeof(buffer));
            CHECK(buffer == 8 && caps[1].buffer_mode == 9);
            CHECK(gpa_destroy(head, generation) == GP_OK);
        }
    }
    void *head = NULL;
    uint64_t generation = 0;
    CHECK(gpa_choose(&head, empty, &generation) == GP_OK);
    CHECK(head != NULL);
    if (head != NULL) {
        const gp_pixel_node *node = head;
        uint32_t flags = 0;
        memcpy(&flags, node->fields + 4, sizeof(flags));
        CHECK(node->backend_format_index == 0 && node->next != NULL);
        CHECK(flags == GP_FLAG_WINDOW);
        CHECK(gpa_destroy(head, generation) == GP_OK);
    }
    CHECK(gpa_configure(fixture, sizeof(fixture) / sizeof(fixture[0])) == GP_OK);
}

static void test_guard_pages(void)
{
    long page_long = sysconf(_SC_PAGESIZE);
    CHECK(page_long > 0);
    if (page_long <= 0) return;
    size_t page = (size_t)page_long;
    unsigned char *mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mapping != MAP_FAILED);
    if (mapping == MAP_FAILED) return;
    CHECK(mprotect(mapping + page, page, PROT_NONE) == 0);
    unsigned char *end = mapping + page;
    uint32_t word = 0;
    memcpy(end - 4, &word, 4);
    expect(end - 4, GP_OK); /* 终止后不触碰保护页。 */
    expect(end, GPA_READ_FAILED);
    for (size_t tail = 1; tail < 4; ++tail) expect(end - tail, GPA_READ_FAILED);
    word = 4;
    memcpy(end - 4, &word, 4);
    expect(end - 4, GPA_READ_FAILED); /* 缺终止。 */
    word = 53;
    memcpy(end - 4, &word, 4);
    expect(end - 4, GPA_READ_FAILED); /* 无值53仍须读取后继终止符。 */
    const uint32_t offscreen[] = {53, 0};
    memcpy(end - sizeof(offscreen), offscreen, sizeof(offscreen));
    expect(end - sizeof(offscreen), GP_NO_MATCH); /* 完整复制且不越过终止符。 */
    word = 8;
    memcpy(end - 4, &word, 4);
    expect(end - 4, GPA_READ_FAILED); /* 值不可读。 */
    const uint32_t value_zero[] = {8, 0};
    memcpy(end - sizeof(value_zero), value_zero, sizeof(value_zero));
    expect(end - sizeof(value_zero), GPA_READ_FAILED);
    word = 73;
    memcpy(end - 4, &word, 4);
    expect(end - 4, GP_UNSUPPORTED_TOKEN); /* 未知token后不再读取。 */
    const uint32_t bad_value[] = {12, UINT32_MAX};
    memcpy(end - sizeof(bad_value), bad_value, sizeof(bad_value));
    expect(end - sizeof(bad_value), GP_BAD_VALUE);
    const uint32_t unaligned[] = {4, 8, 0, 0};
    memcpy(end - sizeof(unaligned) - 1, unaligned, sizeof(unaligned));
    expect(end - sizeof(unaligned) - 1, GP_OK);
    for (size_t i = 0; i < GPA_MAX_WORDS; ++i) {
        word = 4;
        memcpy(end - (i + 1) * 4, &word, 4);
    }
    expect(end - GPA_MAX_WORDS * 4, GPA_WORD_LIMIT);
    CHECK(munmap(mapping, 2 * page) == 0);
}

typedef struct destroy_arg {
    void *head;
    uint64_t generation;
    int32_t status;
} destroy_arg;

static void *destroy_worker(void *opaque)
{
    destroy_arg *arg = opaque;
    arg->status = gpa_destroy(arg->head, arg->generation);
    return NULL;
}

static void *choose_worker(void *opaque)
{
    int *failed = opaque;
    const uint32_t input[] = {4, 5, 8, 0, 0};
    for (size_t i = 0; i < 500; ++i) {
        void *head = NULL;
        uint64_t generation = 0;
        if (gpa_choose(&head, input, &generation) != GP_OK ||
            gpa_destroy(head, generation) != GP_OK ||
            gpa_destroy(head, generation) != GPA_INVALID_HANDLE) ++*failed;
    }
    return NULL;
}

static void test_owners(void)
{
    uint32_t zero = 0;
    void *head = NULL, *other = NULL;
    uint64_t generation = 0, other_generation = 0;
    CHECK(gpa_choose(&head, &zero, &generation) == GP_OK);
    if (head == NULL) return;
    gp_pixel_node *node = head;
    uint32_t flags = UINT32_MAX;
    memcpy(&flags, node->fields + 4, sizeof(flags));
    CHECK(flags == 0);
    uint32_t buffer;
    memcpy(&buffer, node->fields + 8, sizeof(buffer));
    CHECK(buffer == 0 && fixture[0].buffer_mode == 5);
    CHECK(node->next != NULL);
    if (node->next != NULL) {
        memcpy(&buffer, node->next->fields + 8, sizeof(buffer));
        CHECK(buffer == 8 && fixture[1].buffer_mode == 9);
    }
    CHECK(gpa_destroy(node->next, generation) == GPA_INVALID_HANDLE);
    CHECK(gpa_destroy((void *)(uintptr_t)1, generation) == GPA_INVALID_HANDLE);
    CHECK(gpa_destroy(NULL, generation) == GPA_INVALID_HANDLE);
    CHECK(gpa_destroy(head, 0) == GPA_INVALID_HANDLE);
    CHECK(gpa_destroy(head, generation + 1) == GPA_INVALID_HANDLE);
    CHECK(gpa_configure(NULL, 0) == GPA_BUSY);
    CHECK(gpa_choose(&other, &zero, &other_generation) == GP_OK);
    CHECK(gpa_destroy(head, other_generation) == GPA_INVALID_HANDLE);
    node->next = node; /* 所有权释放不依赖消费者可改写的next。 */
    CHECK(gpa_destroy(head, generation) == GP_OK);
    CHECK(gpa_destroy(head, generation) == GPA_INVALID_HANDLE);
    CHECK(gpa_destroy(other, generation) == GPA_INVALID_HANDLE);
    CHECK(gpa_destroy(other, other_generation) == GP_OK);
    CHECK(gpa_choose(&head, &zero, &other_generation) == GP_OK);
    CHECK(other_generation != generation);
    CHECK(gpa_destroy(head, generation) == GPA_INVALID_HANDLE); /* 模拟同地址旧代次。 */
    destroy_arg args[2] = {{head, other_generation, -1}, {head, other_generation, -1}};
    pthread_t threads[2];
    int created[2];
    for (size_t i = 0; i < 2; ++i) {
        created[i] = pthread_create(&threads[i], NULL, destroy_worker, &args[i]);
        CHECK(created[i] == 0);
    }
    for (size_t i = 0; i < 2; ++i)
        if (created[i] == 0) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK((args[0].status == GP_OK && args[1].status == GPA_INVALID_HANDLE) ||
          (args[1].status == GP_OK && args[0].status == GPA_INVALID_HANDLE));
    if (created[0] != 0 && created[1] != 0) (void)gpa_destroy(head, other_generation);
    pthread_t workers[4];
    int failed[4] = {0}, started[4];
    for (size_t i = 0; i < 4; ++i) {
        started[i] = pthread_create(&workers[i], NULL, choose_worker, &failed[i]);
        CHECK(started[i] == 0);
    }
    for (size_t i = 0; i < 4; ++i) {
        if (started[i] == 0) CHECK(pthread_join(workers[i], NULL) == 0);
        CHECK(failed[i] == 0);
    }
}

static void test_configuration(void)
{
    uint32_t zero = 0;
    CHECK(gpa_configure(NULL, 1) == GP_BAD_ARGUMENT);
    CHECK(gpa_configure(fixture, SIZE_MAX) == GP_OVERFLOW);
    gp_format_cap cap = fixture[0];
    cap.flags = UINT32_MAX;
    CHECK(gpa_configure(&cap, 1) == GP_OK);
    expect(&zero, GP_UNSUPPORTED_FORMAT);
    cap = fixture[0];
    CHECK(gpa_configure(&cap, 1) == GP_OK);
    cap.flags = UINT32_MAX; /* 配置是拷贝，不保留借用指针。 */
    expect(&zero, GP_OK);
    CHECK(gpa_configure(NULL, 0) == GP_OK);
    expect(&zero, GP_NO_MATCH);
}

int main(void)
{
    test_inputs();
    test_offscreen();
    test_guard_pages();
    test_owners();
    test_configuration();
    printf("gld_pixel_adapter: %zu checks, %zu failures; 4x500 concurrent lifecycles\n",
           checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
