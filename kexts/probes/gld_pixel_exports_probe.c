#include "gld_pixel_exports.h"
#include <stdlib.h>

#ifdef GPX_FAULT_ALLOCATOR
#include <stdatomic.h>
static _Atomic int allocation_countdown = -1;
GPX_EXPORT void gpx_test_fail_after(int count);
void *gpx_test_malloc(size_t bytes);
void *gpx_test_calloc(size_t count, size_t bytes);

void gpx_test_fail_after(int count)
{
    atomic_store(&allocation_countdown, count);
}

static int fail_allocation(void)
{
    int remaining = atomic_load(&allocation_countdown);
    if (remaining < 0) return 0;
    return atomic_fetch_sub(&allocation_countdown, 1) == 0;
}

void *gpx_test_malloc(size_t bytes)
{
    return fail_allocation() ? NULL : malloc(bytes);
}

void *gpx_test_calloc(size_t count, size_t bytes)
{
    return fail_allocation() ? NULL : calloc(count, bytes);
}
#else
#include "gld_pixel_adapter.h"
#include <OpenGL/CGLTypes.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int32_t (*choose_fn)(void **, const uint32_t *);
typedef int32_t (*destroy_fn)(void *);
typedef int32_t (*configure_fn)(const gp_format_cap *, size_t);
typedef void (*fail_fn)(int);
static choose_fn choose_pixel;
static destroy_fn destroy_pixel;
static configure_fn configure_pixel;
static size_t checks, failures;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); } } while (0)

/* 仅结构夹具，flags=0，不代表renderer或硬件能力。高字节及低字节不属driver段。 */
static const gp_format_cap fixture[] = {
    {0x01001200, 0, GP_BUFFER_MONO_SINGLE, GP_COLOR_RGB888, 0, 0, 1},
    {0x020012ff, 0, GP_BUFFER_MONO_DOUBLE, GP_COLOR_ARGB8888, 24, 8, 1}
};

static void expect(const void *tokens, int32_t wanted)
{
    void *head = (void *)(uintptr_t)1;
    CHECK(choose_pixel(&head, tokens) == wanted);
    if (wanted == kCGLNoError) {
        CHECK(destroy_pixel(head) == kCGLNoError);
        if (head != NULL) CHECK(destroy_pixel(head) == kCGLBadPixelFormat);
    } else CHECK(head == NULL);
}

static void expect_empty(const void *tokens)
{
    void *head = (void *)(uintptr_t)1;
    CHECK(choose_pixel(&head, tokens) == kCGLNoError);
    CHECK(head == NULL);
    CHECK(destroy_pixel(head) == kCGLNoError);
    CHECK(destroy_pixel(head) == kCGLNoError);
}

static void test_inputs(void)
{
    uint32_t zero = 0;
    CHECK(destroy_pixel(NULL) == 0);
    expect_empty(&zero);
    CHECK(configure_pixel(NULL, 1) == kCGLBadAttribute);
    CHECK(configure_pixel(fixture, SIZE_MAX) == kCGLBadAttribute);
    CHECK(configure_pixel(fixture, 2) == 0);
    gp_format_cap mixed[2] = {fixture[0], fixture[1]};
    mixed[1].renderer_id ^= 0x100;
    CHECK(configure_pixel(mixed, 2) == kCGLBadMatch);
    mixed[1].renderer_id = fixture[1].renderer_id ^ 0x10000;
    CHECK(configure_pixel(mixed, 2) == kCGLBadMatch);
    expect(&zero, 0); /* 拒绝跨段不改变旧表。 */
    CHECK(choose_pixel(NULL, &zero) == kCGLBadAttribute);
    expect(NULL, kCGLBadAttribute);
    expect((void *)(uintptr_t)1, kCGLBadAttribute);
    expect((void *)(UINTPTR_MAX - 2), kCGLBadAttribute);
    const uint32_t valid[] = {4, 5, 8, 24, 11, 8, 12, 24, 13, 8, 0};
    const uint32_t values_zero[] = {8, 0, 11, 0, 12, 0, 13, 0, 0};
    const uint32_t no_match[] = {8, 33, 0};
    expect(valid, 0);
    expect(values_zero, 0);
    const uint32_t value_tokens[] = {8, 11, 12, 13};
    for (size_t i = 0; i < sizeof(value_tokens) / sizeof(value_tokens[0]); ++i) {
        const uint32_t bad[] = {value_tokens[i], UINT32_MAX};
        const uint32_t minimum[] = {value_tokens[i], UINT32_C(0x80000000)};
        const uint32_t maximum[] = {value_tokens[i], INT32_MAX, 0};
        expect(bad, kCGLBadValue);
        expect(minimum, kCGLBadValue);
        expect_empty(maximum);
    }
    expect_empty(no_match);
    CHECK(configure_pixel(fixture, 2) == kCGLNoError); /* 空结果不注册owner。 */
    void *continued = NULL;
    CHECK(choose_pixel(&continued, valid) == kCGLNoError);
    CHECK(continued != NULL); /* 空结果后仍可继续选择真实夹具候选。 */
    CHECK(destroy_pixel(continued) == kCGLNoError);
    for (uint32_t token = 1; token < 104; ++token) {
        if (token == 4 || token == 5 || token == 8 || token == 11 ||
            token == 12 || token == 13) continue;
        expect(&token, kCGLBadAttribute);
    }
    uint32_t *short_stream = malloc(sizeof(*short_stream));
    CHECK(short_stream != NULL);
    if (short_stream != NULL) {
        *short_stream = 0;
        expect(short_stream, 0);
        *short_stream = 73;
        expect(short_stream, kCGLBadAttribute);
        free(short_stream);
    }
    unsigned char *unaligned = malloc(sizeof(valid) + 1);
    CHECK(unaligned != NULL);
    if (unaligned != NULL) {
        memcpy(unaligned + 1, valid, sizeof(valid));
        expect(unaligned + 1, 0);
        free(unaligned);
    }
    uint32_t limit[GPA_MAX_WORDS];
    for (size_t i = 0; i < GPA_MAX_WORDS; ++i) limit[i] = 4;
    expect(limit, kCGLBadAttribute);
    limit[GPA_MAX_WORDS - 1] = 0;
    expect(limit, 0);
    limit[GPA_MAX_WORDS - 2] = 8;
    expect(limit, kCGLBadAttribute); /* 值0不是终止。 */
    gp_format_cap unsupported = fixture[0];
    unsupported.flags = UINT32_MAX;
    CHECK(configure_pixel(&unsupported, 1) == 0);
    expect(&zero, kCGLBadPixelFormat);
    CHECK(configure_pixel(fixture, 2) == 0);
}

static void test_guards(void)
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
    expect(end - 4, 0);
    expect(end, kCGLBadAttribute);
    CHECK(destroy_pixel(end) == kCGLBadPixelFormat);
    for (size_t tail = 1; tail < 4; ++tail) expect(end - tail, kCGLBadAttribute);
    word = 4;
    memcpy(end - 4, &word, 4);
    expect(end - 4, kCGLBadAttribute);
    word = 8;
    memcpy(end - 4, &word, 4);
    expect(end - 4, kCGLBadAttribute);
    const uint32_t zero_value[] = {8, 0};
    memcpy(end - sizeof(zero_value), zero_value, sizeof(zero_value));
    expect(end - sizeof(zero_value), kCGLBadAttribute);
    word = 73;
    memcpy(end - 4, &word, 4);
    expect(end - 4, kCGLBadAttribute);
    CHECK(munmap(mapping, 2 * page) == 0);
}

typedef struct destroy_arg { void *head; int32_t status; } destroy_arg;
static void *destroy_worker(void *opaque)
{
    destroy_arg *arg = opaque;
    arg->status = destroy_pixel(arg->head);
    return NULL;
}

static void *lifecycle_worker(void *opaque)
{
    int *failed = opaque;
    const uint32_t zero = 0;
    for (size_t i = 0; i < 500; ++i) {
        void *head = NULL;
        if (choose_pixel(&head, &zero) != 0 || head == NULL ||
            destroy_pixel(head) != 0) ++*failed;
        /* 并发重分配时不重试旧指针：系统ABI没有generation。 */
    }
    return NULL;
}

static void test_owners(void)
{
    uint32_t zero = 0;
    void *head = NULL;
    CHECK(choose_pixel(&head, &zero) == 0);
    if (head == NULL) return;
    gp_pixel_node *node = head;
    CHECK(node->next != NULL);
    if (node->next != NULL) CHECK(destroy_pixel(node->next) == kCGLBadPixelFormat);
    CHECK(destroy_pixel((void *)(uintptr_t)1) == kCGLBadPixelFormat);
    CHECK(configure_pixel(NULL, 0) == kCGLBadMatch);
    uint32_t flags, id;
    memcpy(&flags, node->fields + 4, sizeof(flags));
    memcpy(&id, node->fields, sizeof(id));
    CHECK(flags == 0 && id == fixture[0].renderer_id);
    node->next = node;
    CHECK(destroy_pixel(head) == 0);
    CHECK(destroy_pixel(head) == kCGLBadPixelFormat);
    CHECK(choose_pixel(&head, &zero) == 0);
    destroy_arg args[2] = {{head, -1}, {head, -1}};
    pthread_t threads[2];
    int started[2];
    for (size_t i = 0; i < 2; ++i) {
        started[i] = pthread_create(&threads[i], NULL, destroy_worker, &args[i]);
        CHECK(started[i] == 0);
    }
    for (size_t i = 0; i < 2; ++i)
        if (started[i] == 0) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK((args[0].status == 0 && args[1].status == kCGLBadPixelFormat) ||
          (args[1].status == 0 && args[0].status == kCGLBadPixelFormat));
    if (started[0] != 0 && started[1] != 0) (void)destroy_pixel(head);
    pthread_t workers[4];
    int failed[4] = {0}, created[4];
    for (size_t i = 0; i < 4; ++i) {
        created[i] = pthread_create(&workers[i], NULL, lifecycle_worker, &failed[i]);
        CHECK(created[i] == 0);
    }
    for (size_t i = 0; i < 4; ++i) {
        if (created[i] == 0) CHECK(pthread_join(workers[i], NULL) == 0);
        CHECK(failed[i] == 0);
    }
}

/* POSIX dlsym地址复制成已知原型；不调用系统私有入口。 */
#define LOAD(target, name) do { \
    void *address = dlsym(image, name); \
    _Static_assert(sizeof(target) == sizeof(address), "function pointer size"); \
    memcpy(&(target), &address, sizeof(target)); \
    CHECK((target) != NULL); \
    if ((target) == NULL) return EXIT_FAILURE; \
} while (0)

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return EXIT_FAILURE;
    void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(image != NULL);
    if (image == NULL) { fprintf(stderr, "%s\n", dlerror()); return EXIT_FAILURE; }
    LOAD(choose_pixel, "gldChoosePixelFormat");
    LOAD(destroy_pixel, "gldDestroyPixelFormat");
    LOAD(configure_pixel, "gpx_configure");
    test_inputs();
    test_guards();
    test_owners();
    if (argc == 3) {
        fail_fn fail_after;
        LOAD(fail_after, "gpx_test_fail_after");
        const uint32_t zero = 0;
        fail_after(0);
        expect(&zero, kCGLBadAlloc); /* owner分配失败。 */
        fail_after(1);
        expect(&zero, kCGLBadAlloc); /* 节点分配失败，owner清理。 */
        fail_after(0);
        CHECK(configure_pixel(fixture, 2) == kCGLBadAlloc);
        fail_after(-1);
        expect(&zero, 0); /* 失败后旧配置保持且无owner泄漏。 */
    }
    CHECK(configure_pixel(NULL, 0) == 0);
    const uint32_t zero = 0;
    expect_empty(&zero);
    CHECK(destroy_pixel(NULL) == 0);
    CHECK(dlclose(image) == 0);
    printf("gld_pixel_exports: %zu checks, %zu failures; 4x500 lifecycles; fault=%d\n",
           checks, failures, argc == 3);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
