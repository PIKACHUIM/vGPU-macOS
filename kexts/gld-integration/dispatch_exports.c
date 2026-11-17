#include "dispatch_exports.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(void *) == 8, "64-bit object pointers required");
_Static_assert(sizeof(uint64_t) == 8, "64-bit opaque carrier required");
_Static_assert(GI_DISPATCH_SLOT_COUNT * 8u == GI_DISPATCH_TABLE_BYTES,
               "dispatch table must be 200 bytes");

static void GI_DISPATCH_ABI r0(void *c, uint32_t mask)
{
    (void)mask;
    gi_dispatch_reject(c, 0);
}

static uint32_t GI_DISPATCH_ABI r1(void *c, void *xy, uint32_t w, uint32_t h,
                                  uint32_t format, uint32_t type, void *pixels,
                                  uint32_t mode, uint64_t arg9)
{
    (void)xy; (void)w; (void)h; (void)format; (void)type;
    (void)pixels; (void)mode; (void)arg9;
    gi_dispatch_reject(c, 1);
    return 0;
}

static void GI_DISPATCH_ABI r2(void *c, void *xy, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, uint32_t kind)
{
    (void)xy; (void)x; (void)y; (void)w; (void)h; (void)kind;
    gi_dispatch_reject(c, 2);
}

/* 相同的已证形状生成独立函数，各自保留准确槽号；没有函数指针强转。 */
#define REJECT_VERTICES(N) \
    static void GI_DISPATCH_ABI r##N(void *c, void *vertices, \
                                    uint32_t count, uint32_t flags) \
    { \
        (void)vertices; (void)count; (void)flags; \
        gi_dispatch_reject(c, N); \
    }
REJECT_VERTICES(3)
REJECT_VERTICES(4)
REJECT_VERTICES(5)
REJECT_VERTICES(6)
REJECT_VERTICES(7)
REJECT_VERTICES(8)
REJECT_VERTICES(10)
REJECT_VERTICES(11)
REJECT_VERTICES(12)
REJECT_VERTICES(18)
REJECT_VERTICES(19)
#undef REJECT_VERTICES

static void GI_DISPATCH_ABI r9(void *c, void *first, void *following,
                              uint32_t count, uint32_t flags)
{
    (void)first; (void)following; (void)count; (void)flags;
    gi_dispatch_reject(c, 9);
}

static void GI_DISPATCH_ABI r17(void *c, void *vertices_or_array, uint32_t count)
{
    (void)vertices_or_array; (void)count;
    gi_dispatch_reject(c, 17);
}

static void *GI_DISPATCH_ABI r20(void *c, uint32_t primitive, int32_t *count)
{
    return gi_dispatch_begin(c, primitive, count);
}

static void GI_DISPATCH_ABI r21(void *c, uint32_t flags, uint32_t primitive,
                               uint32_t count)
{
    gi_dispatch_end(c, flags, primitive, count);
}

static void *GI_DISPATCH_ABI r22(void *c, uint32_t primitive, uint32_t min,
                                uint32_t max, uint32_t type, uint32_t count)
{
    (void)primitive; (void)min; (void)max; (void)type; (void)count;
    gi_dispatch_reject(c, 22);
    return NULL;
}

static void GI_DISPATCH_ABI r23(void *c, uint32_t primitive, uint32_t min,
                               uint32_t max, uint32_t type, uint32_t count,
                               void *indices)
{
    (void)primitive; (void)min; (void)max; (void)type; (void)count; (void)indices;
    gi_dispatch_reject(c, 23);
}

static uint32_t GI_DISPATCH_ABI r24(void *c, uint32_t mode, uint32_t primitive,
                                   uint32_t first, uint32_t count, uint32_t type,
                                   void *indices, uint32_t instances, uint64_t arg9)
{
    (void)mode; (void)primitive; (void)first; (void)count; (void)type;
    (void)indices; (void)instances; (void)arg9;
    gi_dispatch_reject(c, 24);
    return 1;
}

void gi_dispatch_init(void *storage)
{
    unsigned char *bytes = storage;
    /* 受限 x86_64 目标的空槽编码为全零；不为未知 R13..16 发明调用类型。 */
    memset(bytes, 0, GI_DISPATCH_TABLE_BYTES);
#define STORE(N) do { \
        gi_dispatch_r##N##_fn callback = r##N; \
        _Static_assert(sizeof(callback) == 8, "64-bit function pointers required"); \
        memcpy(bytes + (N) * 8u, &callback, sizeof(callback)); \
    } while (0)
    STORE(0); STORE(1); STORE(2); STORE(3); STORE(4); STORE(5); STORE(6);
    STORE(7); STORE(8); STORE(9); STORE(10); STORE(11); STORE(12);
    STORE(17); STORE(18); STORE(19); STORE(20); STORE(21); STORE(22);
    STORE(23); STORE(24);
#undef STORE
}
