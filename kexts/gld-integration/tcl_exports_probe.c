#include "tcl_exports.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
static unsigned allocations, frees;
static int fail_allocate;
void *gi_tcl_test_calloc(size_t n, size_t size);
void gi_tcl_test_free(void *pointer);
void *gi_tcl_test_calloc(size_t n, size_t size)
{
    ++allocations;
    return fail_allocate ? NULL : calloc(n, size);
}
void gi_tcl_test_free(void *pointer)
{
    ++frees;
    free(pointer);
}

typedef struct {
    unsigned refs, retain_calls, release_calls, validate_calls, submits, errors;
    gi_tcl_result retain_result, release_result, validate_result, submit_result;
    uint32_t gl_error;
    gi_tcl_detail first_detail;
    uint64_t generation;
    float positions[3][4];
    unsigned char *sink;
    int mutate_sink;
} owner;
static gi_tcl_result retain_owner(void *value)
{
    owner *o = value;
    ++o->retain_calls;
    if (o->retain_result) return o->retain_result;
    ++o->refs;
    return GI_TCL_OK;
}
static gi_tcl_result release_owner(void *value)
{
    owner *o = value;
    ++o->release_calls;
    if (o->release_result) return o->release_result;
    CHECK(o->refs > 0);
    --o->refs;
    return GI_TCL_OK;
}
static gi_tcl_result validate_owner(void *value)
{
    owner *o = value;
    CHECK(o->refs > 0);
    ++o->validate_calls;
    return o->validate_result;
}
static gi_tcl_result submit_owner(void *value, const float positions[3][4], uint64_t *generation)
{
    owner *o = value;
    CHECK(o->refs > 0);
    ++o->submits;
    if (o->mutate_sink) memset(o->sink, 0, 3U * GI_TCL_STRIDE);
    memcpy(o->positions, positions, sizeof(o->positions));
    if (o->submit_result) {
        *generation = UINT64_MAX;
        return o->submit_result;
    }
    *generation = ++o->generation;
    return GI_TCL_OK;
}
static void error_owner(void *value, uint32_t gl_error, gi_tcl_detail detail)
{
    owner *o = value;
    CHECK(o->refs > 0);
    ++o->errors;
    if (!o->gl_error) {
        o->gl_error = gl_error;
        o->first_detail = detail;
    }
}
static gi_tcl_backend backend(owner *o)
{
    gi_tcl_backend b = {o, retain_owner, release_owner, validate_owner, submit_owner, error_owner};
    return b;
}
static gi_tcl *create(owner *o)
{
    gi_tcl *c = NULL;
    gi_tcl_backend b = backend(o);
    CHECK(gi_tcl_create(&b, GI_TCL_CONTROLLED_LEGACY, &c) == GI_TCL_OK);
    CHECK(c && o->refs == 1);
    return c;
}
static gi_tcl_status status(gi_tcl *c)
{
    gi_tcl_status s;
    CHECK(gi_tcl_get_status(c, &s) == GI_TCL_OK);
    return s;
}
static unsigned char *begin(gi_tcl *c, uint32_t primitive, int32_t input)
{
    unsigned before = allocations;
    void *p = gi_tcl_begin(c, primitive, &input);
    CHECK(allocations == before && p != NULL && input == GI_TCL_CAPACITY);
    CHECK(((uintptr_t)p & 15U) == 0);
    return p;
}
static void fill(unsigned char *p, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i) {
        gi_tcl_vertex v = {{-0.5f, 0.5f, (float)(i % 3U) - 1.0f, 1}, {1, 0, 0, 1}};
        memcpy(p + (size_t)i * GI_TCL_STRIDE, &v, sizeof(v));
    }
}
static void draw(gi_tcl *c)
{
    fill(begin(c, 4, 0), 3);
    gi_tcl_end(c, 0x8002, 4, 3);
}
static void destroy(gi_tcl **c, owner *o)
{
    CHECK(gi_tcl_destroy(c) == GI_TCL_OK);
    CHECK(*c == NULL && o->refs == 0);
}

static void test_create_destroy(void)
{
    owner o = {0};
    gi_tcl_backend b = backend(&o);
    gi_tcl *c = NULL, *unchanged;
    gi_tcl_status s, saved;
    CHECK(gi_tcl_create(&b, 0, &c) == GI_TCL_INVALID && c == NULL);
    fail_allocate = 1;
    CHECK(gi_tcl_create(&b, 1, &c) == GI_TCL_NO_MEMORY && c == NULL);
    CHECK(o.retain_calls == 0);
    fail_allocate = 0;
    o.retain_result = GI_TCL_REJECTED;
    CHECK(gi_tcl_create(&b, 1, &c) == GI_TCL_REJECTED && c == NULL && o.refs == 0);
    o.retain_result = GI_TCL_OK;
    c = create(&o);
    unchanged = c;
    fail_allocate = 1;
    CHECK(gi_tcl_create(&b, 1, &c) == GI_TCL_NO_MEMORY && c == unchanged);
    fail_allocate = 0;
    memset(&s, 0x5a, sizeof(s));
    memcpy(&saved, &s, sizeof(s));
    CHECK(gi_tcl_get_status(NULL, &s) == GI_TCL_INVALID);
    CHECK(memcmp(&s, &saved, sizeof(s)) == 0);
    CHECK(gi_tcl_descriptor(c)[2] == 8 && gi_tcl_alternate_descriptor(c) == NULL);
    begin(c, 4, 0);
    CHECK(gi_tcl_destroy(&c) == GI_TCL_BUSY && c == unchanged && o.refs == 1);
    gi_tcl_end(c, 0x8002, 4, 0);
    o.release_result = GI_TCL_REJECTED;
    CHECK(gi_tcl_destroy(&c) == GI_TCL_REJECTED && c == unchanged && o.refs == 1);
    CHECK(status(c).destroying && status(c).backend_fault);
    draw(c);
    CHECK(o.submits == 0);
    CHECK(gi_tcl_destroy(&c) == GI_TCL_REJECTED && c == unchanged && o.refs == 1);
    o.release_result = GI_TCL_OK;
    destroy(&c, &o);
    CHECK(o.release_calls == 3);
}

static void test_capacity_and_snapshot(void)
{
    owner o = {0};
    gi_tcl *c = create(&o);
    unsigned char *p = begin(c, 4, 0);
    void *(*typed_begin)(gi_tcl *, uint32_t, int32_t *) = gi_tcl_begin;
    void (*typed_end)(gi_tcl *, uint32_t, uint32_t, uint32_t) = gi_tcl_end;
    int32_t n = 0;
    CHECK(status(c).active && o.validate_calls == 1);
    gi_tcl_end(c, 0x8002, 4, 0);
    CHECK(o.submits == 0 && o.validate_calls == 2);
    CHECK(typed_begin(c, 4, &n) == p && n == 48);
    fill(p, 48);
    typed_end(c, 0x8002, 4, 48);
    CHECK(o.submits == 0 && status(c).pending_rejection && !status(c).backend_fault);
    o.sink = p;
    o.mutate_sink = 1;
    draw(c);
    CHECK(o.submits == 1 && !status(c).pending_rejection);
    CHECK(o.positions[0][0] == -0.5f && o.positions[0][2] == 0);
    CHECK(o.positions[0][1] == -0.5f && o.positions[1][1] == -0.5f &&
          o.positions[2][1] == -0.5f);
    CHECK(o.positions[1][2] == 0.5f && o.positions[2][2] == 1);
    CHECK(status(c).completed_generation == 1);
    CHECK(o.gl_error == GI_TCL_GL_INVALID_OPERATION);
    destroy(&c, &o);
}

static int coverage_edge(const int a[2], const int b[2], int x, int y)
{
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
}

static void dx_coverage(const float positions[3][4], unsigned char rows[8])
{
    int window[3][2];
    unsigned i, x, y;
    int area;
    /* 独立执行 DX 视口映射；四分之一像素坐标使边函数完全使用整数。 */
    for (i = 0; i < 3; ++i) {
        float wx = (positions[i][0] / positions[i][3] + 1.0f) * 16.0f;
        float wy = (1.0f - positions[i][1] / positions[i][3]) * 16.0f;
        CHECK(isfinite(wx) && wx >= 0.0f && wx <= 32.0f);
        CHECK(isfinite(wy) && wy >= 0.0f && wy <= 32.0f);
        window[i][0] = (int)wx;
        window[i][1] = (int)wy;
        CHECK((float)window[i][0] == wx && (float)window[i][1] == wy);
    }
    area = coverage_edge(window[0], window[1], window[2][0], window[2][1]);
    CHECK(area != 0);
    memset(rows, 0, 8);
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            int inside = 1;
            for (i = 0; i < 3; ++i) {
                int edge = coverage_edge(window[i], window[(i + 1U) % 3U],
                                         (int)(4U * x + 2U), (int)(4U * y + 2U));
                /* 此样本无像素中心落在边线，覆盖不依赖边界归属规则。 */
                CHECK(edge != 0);
                if ((edge > 0) != (area > 0)) inside = 0;
            }
            if (inside) rows[y] |= (unsigned char)(1U << x);
        }
    }
}

static unsigned coverage_mismatches(const unsigned char a[8], const unsigned char b[8])
{
    unsigned x, y, mismatches = 0;
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x)
            mismatches += ((unsigned)(a[y] ^ b[y]) >> x) & 1U;
    return mismatches;
}

static void test_clip_coordinates_and_coverage(void)
{
    static const gi_tcl_vertex triangle[3] = {
        {{-0.6875f, -0.75f, -1.0f, 1.0f}, {1, 0, 0, 1}},
        {{ 0.5f, -0.5625f, 0.25f, 1.0f}, {1, 0, 0, 1}},
        {{-0.5625f, 0.5625f, 1.0f, 1.0f}, {1, 0, 0, 1}}
    };
    static const float expected[3][4] = {
        {-0.6875f, 0.75f, 0.0f, 1.0f},
        { 0.5f, 0.5625f, 0.625f, 1.0f},
        {-0.5625f, -0.5625f, 1.0f, 1.0f}
    };
    /* GL 窗口点为 (1.25,1)、(6,1.75)、(1.75,6.25)。
     * 按 GL 行号从下到上，像素中心覆盖的 x 为：空、1..3、1..4、
     * 1..3、2、空、空、空。常量 oracle 不调用 DX 映射或边函数。 */
    static const unsigned char gl_oracle[8] = {0x00, 0x0e, 0x1e, 0x0e, 0x04, 0, 0, 0};
    static const unsigned char empty[8] = {0};
    owner o = {0};
    gi_tcl *c = create(&o);
    unsigned char actual[8], missing_rows[8], double_rows[8];
    float missing_flip[3][4], double_flip[3][4];
    unsigned i, j;
    unsigned char *p = begin(c, 4, 0);
    memcpy(p, triangle, sizeof(triangle));
    o.sink = p;
    o.mutate_sink = 1;
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 1 && o.errors == 0 && status(c).completed_generation == 1);
    CHECK(!status(c).pending_rejection && !status(c).backend_fault);
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            CHECK(o.positions[i][j] == expected[i][j]);
    dx_coverage((const float (*)[4])o.positions, actual);
    CHECK(coverage_mismatches(actual, gl_oracle) == 0);
    CHECK(coverage_mismatches(actual, empty) == 11);
    for (i = 0; i < 3; ++i) {
        memcpy(missing_flip[i], expected[i], sizeof(missing_flip[i]));
        missing_flip[i][1] = triangle[i].position[1];
        memcpy(double_flip[i], expected[i], sizeof(double_flip[i]));
        double_flip[i][1] = -double_flip[i][1];
        CHECK(missing_flip[i][1] != o.positions[i][1]);
        CHECK(double_flip[i][1] != o.positions[i][1]);
    }
    dx_coverage((const float (*)[4])missing_flip, missing_rows);
    dx_coverage((const float (*)[4])double_flip, double_rows);
    CHECK(coverage_mismatches(missing_rows, gl_oracle) == 18);
    CHECK(coverage_mismatches(double_rows, gl_oracle) == 18);
    destroy(&c, &o);
    puts("TCL clip PASS: 12 submitted components, 11 GL pixels, 0 mismatches; missing/double flip: 18 mismatches each");
}

static void test_colors_and_recovery(void)
{
    owner o = {0};
    gi_tcl *c = create(&o);
    unsigned char *p = begin(c, 4, 0);
    gi_tcl_vertex v;
    fill(p, 3);
    memcpy(&v, p + GI_TCL_STRIDE, sizeof(v));
    v.rgba[0] = 0; v.rgba[1] = 1;
    memcpy(p + GI_TCL_STRIDE, &v, sizeof(v));
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 0 && o.first_detail == GI_TCL_DETAIL_VERTEX);
    fill(begin(c, 4, 0), 3);
    gi_tcl_end(c, 2, 4, 3);
    CHECK(o.submits == 0 && status(c).pending_rejection);
    draw(c);
    CHECK(o.submits == 1 && !status(c).pending_rejection);
    CHECK(o.first_detail == GI_TCL_DETAIL_VERTEX && o.gl_error != 0);
    gi_tcl_invalidate(c, GI_TCL_DETAIL_STATE);
    begin(c, 4, 0);
    gi_tcl_end(c, 0x8002, 4, 0);
    CHECK(status(c).pending_rejection);
    draw(c);
    CHECK(o.submits == 2 && !status(c).pending_rejection);
    destroy(&c, &o);
}

static void test_leases_and_rejections(void)
{
    owner o = {0};
    gi_tcl *c = create(&o);
    unsigned i;
    int32_t inputs[] = {1, 48, -1, INT32_MAX};
    for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        fill(begin(c, 4, inputs[i]), 3);
        gi_tcl_end(c, 0x8002, 4, 3);
    }
    CHECK(o.submits == 0);
    for (i = 0; i < 17; ++i) {
        if (i == 4) continue;
        begin(c, i, 0);
        gi_tcl_end(c, 0x8002, i == 2 ? 3 : i, 3);
        CHECK(status(c).last_detail == GI_TCL_DETAIL_PRIMITIVE);
    }
    fill(begin(c, 4, 0), 3);
    begin(c, 4, 0);
    fill(begin(c, 4, 0), 3);
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 0);
    fill(begin(c, 4, 0), 3);
    gi_tcl_end(c, 2, 4, 3);
    begin(c, 4, 0);
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 0 && status(c).last_detail == GI_TCL_DETAIL_VERTEX);
    fill(begin(c, 4, 0), 3);
    gi_tcl_invalidate(c, GI_TCL_DETAIL_NONE);
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 0);
    fill(begin(c, 4, 0), 3);
    gi_tcl_end(c, 0x8002, 4, UINT32_MAX);
    CHECK(o.submits == 0 && status(c).last_detail == GI_TCL_DETAIL_COUNT);
    draw(c);
    CHECK(o.submits == 1);
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 1 && status(c).pending_rejection);
    destroy(&c, &o);
}

static void test_state_and_fault(void)
{
    owner o = {0};
    gi_tcl *c = create(&o);
    o.validate_result = GI_TCL_REJECTED;
    fill(begin(c, 4, 0), 3);
    o.validate_result = GI_TCL_OK;
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 0);
    fill(begin(c, 4, 0), 3);
    o.validate_result = GI_TCL_REJECTED;
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 0);
    o.validate_result = GI_TCL_OK;
    draw(c);
    CHECK(o.submits == 1 && status(c).completed_generation == 1);
    o.submit_result = GI_TCL_REJECTED;
    draw(c);
    CHECK(o.submits == 2 && status(c).backend_fault && status(c).completed_generation == 1);
    o.submit_result = GI_TCL_OK;
    o.gl_error = 0;
    draw(c);
    CHECK(o.submits == 2 && status(c).backend_fault && status(c).pending_rejection);
    CHECK(status(c).completed_generation == 1);
    destroy(&c, &o);
    c = create(&o);
    o.validate_result = GI_TCL_BACKEND_FAULT;
    draw(c);
    o.validate_result = GI_TCL_OK;
    draw(c);
    CHECK(o.submits == 2 && status(c).backend_fault);
    destroy(&c, &o);
}

static void test_bad_vertices(void)
{
    owner o = {0};
    gi_tcl *c = create(&o);
    unsigned i;
    for (i = 0; i < 6; ++i) {
        unsigned char *p = begin(c, 4, 0);
        gi_tcl_vertex v;
        fill(p, 3);
        memcpy(&v, p, sizeof(v));
        if (i == 0) v.position[0] = NAN;
        if (i == 1) v.position[1] = INFINITY;
        if (i == 2) v.position[2] = -1.001f;
        if (i == 3) v.position[3] = 0;
        if (i == 4) v.rgba[3] = NAN;
        if (i == 5) v.rgba[0] = 0.5f;
        memcpy(p, &v, sizeof(v));
        gi_tcl_end(c, 0x8002, 4, 3);
        CHECK(o.submits == 0 && !status(c).backend_fault);
    }
    draw(c);
    CHECK(o.submits == 1);
    destroy(&c, &o);
}

static void test_guards(void)
{
    owner o = {0};
    gi_tcl *c = create(&o);
    unsigned char *p = begin(c, 4, 0);
    fill(p, 3);
    p[48U * GI_TCL_STRIDE] = 0;
    gi_tcl_end(c, 0x8002, 4, 3);
    CHECK(o.submits == 0 && status(c).backend_fault && status(c).last_detail == GI_TCL_DETAIL_CANARY);
    p[48U * GI_TCL_STRIDE] = 0x5a;
    draw(c);
    CHECK(o.submits == 0 && status(c).backend_fault);
    destroy(&c, &o);
    c = create(&o);
    p = begin(c, 4, 0);
    p[-1] = 0;
    gi_tcl_end(c, 0x8002, 4, 0);
    CHECK(status(c).backend_fault);
    destroy(&c, &o);
    c = create(&o);
    gi_tcl_descriptor(c)[2] = 4;
    draw(c);
    CHECK(o.submits == 0 && status(c).backend_fault && status(c).last_detail == GI_TCL_DETAIL_DESCRIPTOR);
    destroy(&c, &o);
    c = create(&o);
    gi_tcl_fault(c, GI_TCL_DETAIL_STATE);
    o.gl_error = 0;
    draw(c);
    CHECK(o.submits == 0 && status(c).backend_fault);
    destroy(&c, &o);
}

int main(void)
{
    test_clip_coordinates_and_coverage();
    test_create_destroy();
    test_capacity_and_snapshot();
    test_colors_and_recovery();
    test_leases_and_rejections();
    test_state_and_fault();
    test_bad_vertices();
    test_guards();
    CHECK(allocations == frees + 2U);
    puts("TCL CPU PASS: ABI, 48-capacity, snapshots, leases, recovery, persistent faults, lifecycle");
    return 0;
}
