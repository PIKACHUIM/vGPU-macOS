#include "dispatch_exports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CPU check failed at line %d: %s\n", __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

struct fixture {
    uint32_t slots[GI_DISPATCH_SLOT_COUNT];
    uint32_t errors;
    uint32_t pending_error;
    uint32_t primitive;
    uint32_t flags;
    uint32_t count;
    int32_t input_count;
    int32_t *count_pointer;
    _Alignas(16) unsigned char sink[48u * 32u];
};

void gi_dispatch_reject(void *context, uint32_t slot)
{
    struct fixture *f = context;
    CHECK(slot < GI_DISPATCH_SLOT_COUNT);
    ++f->slots[slot];
    ++f->errors;
    if (f->pending_error == 0) {
        f->pending_error = UINT32_C(0x502);
    }
}

void *gi_dispatch_begin(void *context, uint32_t primitive, int32_t *count_inout)
{
    struct fixture *f = context;
    ++f->slots[20];
    f->primitive = primitive;
    f->input_count = *count_inout;
    f->count_pointer = count_inout;
    *count_inout = 48;
    return f->sink;
}

void gi_dispatch_end(void *context, uint32_t flags, uint32_t primitive, uint32_t count)
{
    struct fixture *f = context;
    ++f->slots[21];
    f->flags = flags;
    f->primitive = primitive;
    f->count = count;
}

static void check_canaries(const unsigned char *bytes, unsigned char expected)
{
    size_t i;
    for (i = 0; i < 17u; ++i) {
        CHECK(bytes[i] == expected);
        CHECK(bytes[17u + GI_DISPATCH_TABLE_BYTES + i] == expected);
    }
}

static void exercise(const unsigned char *table, struct fixture *f)
{
    /* 不可访问的载体：所有未知 P 必须被忽略，包括指针数组和栈上传参。 */
    void *p = (void *)(uintptr_t)1;
    void *q = (void *)(uintptr_t)3;
    const uint32_t u = UINT32_C(0xfedcba98);
    const uint64_t x = UINT64_C(0x9876543210fedcba);
    static const int32_t inputs[] = {0, 1, 48, 97};
    uint32_t before_errors;
    size_t i;
#define LOAD(N) \
    gi_dispatch_r##N##_fn call##N; \
    memcpy(&call##N, table + (N) * 8u, sizeof(call##N)); \
    CHECK(call##N != NULL)
    LOAD(0); LOAD(1); LOAD(2); LOAD(3); LOAD(4); LOAD(5); LOAD(6);
    LOAD(7); LOAD(8); LOAD(9); LOAD(10); LOAD(11); LOAD(12);
    LOAD(17); LOAD(18); LOAD(19); LOAD(20); LOAD(21); LOAD(22);
    LOAD(23); LOAD(24);
#undef LOAD

    call0(f, u);
    CHECK(call1(f, p, u, 2, 3, 4, q, 5, x) == 0);
    call2(f, p, u, 2, 3, 4, 5);
    call3(f, p, u, 3);
    call4(f, p, u, 4);
    call5(f, p, u, 5);
    call6(f, p, u, 6);
    call7(f, p, u, 7);
    call8(f, p, u, 8);
    call9(f, p, q, u, 9);
    call10(f, p, u, 10);
    call11(f, p, u, 11);
    call12(f, p, u, 12);
    call17(f, p, u);
    call18(f, p, u, 18);
    call19(f, p, u, 19);
    before_errors = f->errors;
    CHECK(call22(f, u, 1, 2, 3, 4) == NULL);
    CHECK(f->errors == before_errors + 1u && f->slots[22] == 1u);
    call23(f, u, 1, 2, 3, 4, p);
    before_errors = f->errors;
    CHECK(call24(f, 1, u, 2, 3, 4, p, 5, x) == 1u);
    CHECK(f->errors == before_errors + 1u && f->slots[24] == 1u);
    CHECK(f->errors == 19u);
    CHECK(f->pending_error == UINT32_C(0x502));

    for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        int32_t count = inputs[i];
        uint32_t primitive = u - (uint32_t)i;
        CHECK(call20(f, primitive, &count) == f->sink);
        CHECK(((uintptr_t)f->sink & (uintptr_t)15) == 0);
        CHECK(f->input_count == inputs[i]);
        CHECK(f->count_pointer == &count);
        CHECK(f->primitive == primitive);
        CHECK(count == 48);
        call21(f, UINT32_C(0x8002), primitive, (uint32_t)i);
        CHECK(f->flags == UINT32_C(0x8002));
        CHECK(f->primitive == primitive);
        CHECK(f->count == (uint32_t)i);
    }
    call21(f, UINT32_C(0xf1234567), UINT32_C(0x89abcdef), UINT32_MAX);
    CHECK(f->flags == UINT32_C(0xf1234567));
    CHECK(f->primitive == UINT32_C(0x89abcdef));
    CHECK(f->count == UINT32_MAX);
    CHECK(f->errors == 19u);
    CHECK(f->pending_error == UINT32_C(0x502));
    for (i = 0; i < GI_DISPATCH_SLOT_COUNT; ++i) {
        uint32_t expected = 1;
        if (i >= 13u && i <= 16u) {
            expected = 0;
        } else if (i == 20u) {
            expected = 4;
        } else if (i == 21u) {
            expected = 5;
        }
        CHECK(f->slots[i] == expected);
    }
    /* 后续拒绝不覆盖尚未消费的错误。 */
    f->pending_error = UINT32_C(0x505);
    call0(f, 0);
    CHECK(f->pending_error == UINT32_C(0x505));
    CHECK(f->errors == 20u && f->slots[0] == 2u);
}

int main(void)
{
    unsigned char first[GI_DISPATCH_TABLE_BYTES + 34u];
    unsigned char second[GI_DISPATCH_TABLE_BYTES + 34u];
    struct fixture a = {0};
    struct fixture b = {0};
    size_t i;
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    gi_dispatch_init(first + 17u);
    gi_dispatch_init(second + 17u);
    CHECK(memcmp(first + 17u, second + 17u, GI_DISPATCH_TABLE_BYTES) == 0);
    check_canaries(first, 0xa5);
    check_canaries(second, 0x5a);
    for (i = 13u * 8u; i < 17u * 8u; ++i) {
        CHECK(first[17u + i] == 0);
    }
    exercise(first + 17u, &a);
    CHECK(b.errors == 0);
    exercise(second + 17u, &b);
    CHECK(a.errors == 20u);
    CHECK(a.slots[20] == 4u && a.slots[21] == 5u);
    CHECK(memcmp(first + 17u, second + 17u, GI_DISPATCH_TABLE_BYTES) == 0);
    check_canaries(first, 0xa5);
    check_canaries(second, 0x5a);
    puts("typed dispatch CPU probe: PASS (21 reachable slots, 4 NULL boundaries)");
    return EXIT_SUCCESS;
}
