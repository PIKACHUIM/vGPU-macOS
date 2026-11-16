#include "vgpu_surface.h"
#include <IOKit/IOKitLib.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const float colors[2][4] = {
    { .25f, .75f, .125f, .375f }, { .875f, .125f, .625f, .75f }
};
static const unsigned char pixels[2][4] = {
    { 64, 191, 32, 255 }, { 223, 32, 159, 255 }
};
static int failures;
static bool check(bool ok, const char *label)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
    return ok;
}
static bool code(IOReturn actual, IOReturn expected, const char *label)
{
    if (actual != expected)
        printf("return=0x%08" PRIx32 " expected=0x%08" PRIx32 "\n",
               (uint32_t)actual, (uint32_t)expected);
    return check(actual == expected, label);
}
static bool image(vgpu_surface *s, uint64_t generation, unsigned color,
                   vgpu_surface_rect rect, vgpu_surface_pack pack, const char *label)
{
    size_t stride = pack.stride_bytes;
    if (!stride) stride = (((pack.row_length ? pack.row_length : rect.width) * 4 +
                             pack.alignment - 1) / pack.alignment) * pack.alignment;
    size_t offset = pack.skip_rows * stride + pack.skip_pixels * 4;
    size_t size = offset + (rect.height - 1) * stride + rect.width * 4;
    unsigned char *actual = malloc(size + 64), *expected = malloc(size + 64);
    if (!actual || !expected) { free(actual); free(expected); return check(false, "malloc"); }
    memset(actual, 0xa5, size + 64);
    memset(expected, 0xa5, size + 64);
    for (uint32_t j = 0; j < rect.height; ++j)
        for (uint32_t i = 0; i < rect.width; ++i)
            memcpy(expected + 32 + offset + j * stride + i * 4, pixels[color], 4);
    IOReturn result = vgpu_surface_read(s, generation, rect, VGPU_SURFACE_RGBA8_OUTPUT,
                                        pack, actual + 32, size);
    bool ok = result == kIOReturnSuccess && !memcmp(actual, expected, size + 64);
    printf("generation=%" PRIu64 " rect=%u,%u,%u,%u stride=%zu bytes=%zu pixels=%u\n",
           generation, rect.x, rect.y, rect.width, rect.height, stride, size,
           rect.width * rect.height);
    if (result) printf("read return=0x%08" PRIx32 "\n", (uint32_t)result);
    free(actual); free(expected);
    return check(ok, label);
}
static bool reject(vgpu_surface *s, uint64_t generation, vgpu_surface_rect r,
                    uint32_t format, vgpu_surface_pack p, size_t capacity,
                    IOReturn expected, const char *label)
{
    unsigned char actual[4096], copy[4096];
    memset(actual, 0xa5, sizeof(actual));
    memcpy(copy, actual, sizeof(copy));
    IOReturn result = vgpu_surface_read(s, generation, r, format, p, actual, capacity);
    return code(result, expected, label) && check(!memcmp(actual, copy, sizeof(copy)),
                                                 "failure leaves entire destination unchanged");
}
/* 只在 probe 直接提交 selector 2，产品 surface API 不增加绘图入口。 */
struct vertex_input {
    uint32_t version, reserved;
    float vertices[3][4];
};
_Static_assert(sizeof(float) == 4, "selector 2 requires float32");
_Static_assert(sizeof(struct vertex_input) == 56, "selector 2 requires 56 bytes");
struct triangle_case {
    struct vertex_input input;
    int64_t screen[3][2];
    uint32_t red_pixels;
    const char *label;
};
/* 来自 tools/vgpu-user-draw.c；屏幕坐标乘二，三条边均不经过像素中心。 */
static const struct triangle_case triangles[] = {
    {
        {1, 0, {{-.6875f,-.5625f,0.f,1.f}, {.3125f,-.5625f,.5f,1.f},
                {-.6875f,.21875f,1.f,1.f}}},
        {{80,400},{336,400},{80,200}}, 6400, "vertices A"
    },
    {
        {1, 0, {{-.75f,.75f,1.f,1.f}, {-.125f,-.5f,.5f,1.f},
                {.5f,.75f,0.f,1.f}}},
        {{64,64},{224,384},{384,64}}, 12800, "vertices B"
    }
};
static int64_t cross(const int64_t a[2], const int64_t b[2], int64_t x, int64_t y)
{
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
}
static bool triangle_image(vgpu_surface *s, uint64_t generation,
                           const struct triangle_case *triangle,
                           vgpu_surface_rect rect, vgpu_surface_pack pack)
{
    size_t stride = pack.stride_bytes;
    if (!stride) stride = (((pack.row_length ? pack.row_length : rect.width) * 4 +
                             pack.alignment - 1) / pack.alignment) * pack.alignment;
    size_t offset = pack.skip_rows * stride + pack.skip_pixels * 4;
    size_t size = offset + (rect.height - 1) * stride + rect.width * 4;
    unsigned char *actual = malloc(size + 64), *expected = malloc(size + 64);
    if (!actual || !expected) { free(actual); free(expected); return check(false, "triangle malloc"); }
    memset(actual, 0xa5, size + 64);
    memset(expected, 0xa5, size + 64);
    uint32_t red = 0, edges = 0, mismatch = 0, alpha = 0;
    for (uint32_t j = 0; j < rect.height; ++j) {
        for (uint32_t i = 0; i < rect.width; ++i) {
            int64_t x = 2 * (int64_t)(rect.x + i) + 1;
            int64_t y = 2 * (int64_t)(rect.y + j) + 1;
            int64_t a = cross(triangle->screen[0], triangle->screen[1], x, y);
            int64_t b = cross(triangle->screen[1], triangle->screen[2], x, y);
            int64_t c = cross(triangle->screen[2], triangle->screen[0], x, y);
            if (!a || !b || !c) ++edges;
            bool inside = (a < 0 && b < 0 && c < 0) || (a > 0 && b > 0 && c > 0);
            if (inside) ++red;
            unsigned char pixel[4] = {inside ? 255 : 0, inside ? 0 : 255, 0, 255};
            memcpy(expected + 32 + offset + j * stride + i * 4, pixel, 4);
        }
    }
    IOReturn result = vgpu_surface_read(s, generation, rect, VGPU_SURFACE_RGBA8_OUTPUT,
                                        pack, actual + 32, size);
    for (uint32_t j = 0; j < rect.height; ++j) {
        for (uint32_t i = 0; i < rect.width; ++i) {
            size_t at = 32 + offset + j * stride + i * 4;
            if (memcmp(actual + at, expected + at, 3)) ++mismatch;
            if (actual[at + 3] != 255) ++alpha;
        }
    }
    size_t byte_mismatch = 0;
    for (size_t i = 0; i < size + 64; ++i)
        if (actual[i] != expected[i]) ++byte_mismatch;
    bool full = rect.x == 0 && rect.y == 0 && rect.width == 256 && rect.height == 256;
    bool ok = result == kIOReturnSuccess && !edges && !byte_mismatch &&
              red > 0 && red < rect.width * rect.height &&
              (!full || red == triangle->red_pixels);
    printf("%s generation=%" PRIu64 " rect=%u,%u,%u,%u stride=%zu bytes=%zu "
           "expected_red=%u expected_green=%u edge_samples=%u rgb_mismatch=%u "
           "alpha_mismatch=%u byte_mismatch=%zu return=0x%08" PRIx32 "\n",
           triangle->label, generation, rect.x, rect.y, rect.width, rect.height,
           stride, size, red, rect.width * rect.height - red, edges, mismatch,
           alpha, byte_mismatch, (uint32_t)result);
    free(actual); free(expected);
    return check(ok, "triangle cross-product RGBA pixels, skip/padding and guards");
}
static void triangle_tests(vgpu_surface *s)
{
    io_connect_t connection = IO_OBJECT_NULL;
    CFMutableDictionaryRef matching = IOServiceMatching("vgpuAccelerator");
    if (!check(matching != NULL, "triangle service matching")) return;
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, matching);
    if (!check(service != IO_OBJECT_NULL, "triangle accelerator service")) return;
    IOReturn result = IOServiceOpen(service, mach_task_self(), UINT32_C(0x56475055), &connection);
    IOObjectRelease(service);
    if (code(result, kIOReturnSuccess, "probe-only selector 2 connection")) {
        for (size_t t = 0; t < sizeof(triangles) / sizeof(triangles[0]); ++t) {
            uint64_t output[3] = {0,0,0};
            uint32_t count = 3;
            result = IOConnectCallMethod(connection, 2, NULL, 0, &triangles[t].input,
                                          sizeof(triangles[t].input), output, &count, NULL, NULL);
            if (!code(result, kIOReturnSuccess, "actual selector 2 user vertices")) break;
            if (!check(count == 3 && output[0] && output[1] == 256 && output[2] == 256,
                       "selector 2 generation and dimensions")) break;
            triangle_image(s, output[0], &triangles[t], (vgpu_surface_rect){0,0,256,256},
                           (vgpu_surface_pack){0,0,0,0,4});
            vgpu_surface_rect rect = {37,83,101,119};
            for (size_t alignment = 1; alignment <= 8; alignment *= 2)
                triangle_image(s, output[0], &triangles[t], rect,
                               (vgpu_surface_pack){109,0,3,2,alignment});
            triangle_image(s, output[0], &triangles[t], rect,
                           (vgpu_surface_pack){SIZE_MAX,453,2,5,8});
        }
    }
    if (connection) code(IOServiceClose(connection), kIOReturnSuccess, "close selector 2 connection");
}
struct worker { vgpu_surface *s; bool ok; };
static void *clear_worker(void *arg)
{
    struct worker *w = arg;
    w->ok = true;
    for (unsigned i = 0; i < 12; ++i) {
        uint64_t generation = 0;
        if (vgpu_surface_clear(w->s, colors[i % 2], &generation) != kIOReturnSuccess)
            w->ok = false;
    }
    vgpu_surface_release(w->s);
    return NULL;
}
int main(int argc, char **argv)
{
    bool denied = argc == 2 && !strcmp(argv[1], "--expect-denied");
    if (argc != 1 && !denied) return 2;
    vgpu_surface *s = NULL, *second = NULL;
    IOReturn result = vgpu_surface_open(256, 256, VGPU_SURFACE_BGRX8, &s);
    if (denied) {
        check(geteuid() != 0, "non-root process");
        code(result, kIOReturnNotPrivileged, "real IOServiceOpen denied");
        check(s == NULL, "failed open returns NULL");
        vgpu_surface_release(s);
        return failures ? 1 : 0;
    }
    if (!code(result, kIOReturnSuccess, "IOServiceOpen vgpuAccelerator type 0x56475055")) return 1;
    triangle_tests(s);
    vgpu_surface_rect full = { 0, 0, 256, 256 }, small = { 17, 29, 13, 9 };
    vgpu_surface_pack tight = { 0, 0, 0, 0, 4 };
    uint64_t generation = 0, old = 0;
    for (unsigned c = 0; c < 2; ++c) {
        old = generation;
        if (!code(vgpu_surface_clear(s, colors[c], &generation), kIOReturnSuccess,
                  "GPU nontrivial clear")) goto done;
        check(generation != 0 && generation != old, "nonzero changed generation");
        image(s, generation, c, full, tight, "all 65536 RGBA pixels including alpha=255 and guards");
        for (size_t a = 1; a <= 8; a *= 2) {
            vgpu_surface_pack p = { 19, 0, 3, 2, a };
            image(s, generation, c, small, p, "subrect row_length/skip/alignment all pixels+padding");
        }
        vgpu_surface_pack explicit_stride = { SIZE_MAX, 93, 2, 5, 8 };
        image(s, generation, c, small, explicit_stride, "explicit unaligned stride overrides row_length");
        vgpu_surface_pack overlap = { 1, 0, 1, 1, 1 };
        image(s, generation, c, small, overlap, "overlapping rows follow increasing row order");
        image(s, generation, c, (vgpu_surface_rect){255,255,1,1}, tight, "last source pixel");
        if (old) reject(s, old, small, VGPU_SURFACE_RGBA8_OUTPUT, tight, 4096,
                        kIOReturnNotReady, "stale generation rejected");
    }
    code(vgpu_surface_open(255,256,VGPU_SURFACE_BGRX8,&second), kIOReturnUnsupported, "reject width");
    code(vgpu_surface_open(256,255,VGPU_SURFACE_BGRX8,&second), kIOReturnUnsupported, "reject height");
    code(vgpu_surface_open(256,256,VGPU_SURFACE_RGBA8_OUTPUT,&second), kIOReturnUnsupported, "reject RGBA8 backing");
    reject(s,generation,small,99,tight,4096,kIOReturnUnsupported,"reject output format");
    reject(s,0,small,VGPU_SURFACE_RGBA8_OUTPUT,tight,4096,kIOReturnBadArgument,"zero generation");
    const vgpu_surface_rect invalid_rects[] = {
        {256,0,1,1},{0,256,1,1},{250,0,7,1},{0,250,1,7},
        {0,0,0,1},{0,0,1,0},{UINT32_MAX,0,1,1},{0,0,UINT32_MAX,1}
    };
    for (size_t i=0;i<sizeof(invalid_rects)/sizeof(invalid_rects[0]);++i)
        reject(s,generation,invalid_rects[i],VGPU_SURFACE_RGBA8_OUTPUT,tight,4096,
               kIOReturnBadArgument,"reject invalid rectangle");
    const vgpu_surface_pack invalid_packs[] = {
        {0,0,0,0,0},{0,0,0,0,3},{0,0,0,0,SIZE_MAX},
        {SIZE_MAX,0,0,0,4},{SIZE_MAX/4,0,0,0,8},
        {0,0,SIZE_MAX,0,4},{0,0,0,SIZE_MAX,4},
        {0,SIZE_MAX,2,0,4},{0,SIZE_MAX/2+1,0,0,4},
        {0,SIZE_MAX-15,1,4,4}
    };
    for (size_t i=0;i<sizeof(invalid_packs)/sizeof(invalid_packs[0]);++i)
        reject(s,generation,small,VGPU_SURFACE_RGBA8_OUTPUT,invalid_packs[i],4096,
               kIOReturnBadArgument,"reject invalid/overflowing pack");
    reject(s,generation,small,VGPU_SURFACE_RGBA8_OUTPUT,tight,13*9*4-1,
           kIOReturnBadArgument,"capacity one byte short");
    const uint32_t bad_bits[] = {0x7fc00000,0x7f800000,0xff800000,0x3f800001,0x80000001};
    for (size_t i=0;i<sizeof(bad_bits)/sizeof(bad_bits[0]);++i) {
        float invalid[4]; memcpy(invalid,colors[0],sizeof(invalid));
        memcpy(&invalid[2],&bad_bits[i],4);
        uint64_t untouched = UINT64_MAX;
        code(vgpu_surface_clear(s,invalid,&untouched),kIOReturnBadArgument,"invalid clear rejected");
        check(untouched == UINT64_MAX,"failed clear preserves generation output");
    }
    image(s,generation,1,full,tight,"invalid requests preserve current GPU frame");
    if (!code(vgpu_surface_open(256,256,VGPU_SURFACE_BGRX8,&second),kIOReturnSuccess,"second connection shares target")) goto done;
    old = generation;
    if (!code(vgpu_surface_clear(second,colors[0],&generation),kIOReturnSuccess,"other connection clear")) goto done;
    reject(s,old,small,VGPU_SURFACE_RGBA8_OUTPUT,tight,4096,kIOReturnNotReady,"cross-connection stale generation");
    image(s,generation,0,full,tight,"cross-connection current generation");
    struct worker worker = {second,false};
    pthread_t thread;
    if (code(vgpu_surface_retain(second),kIOReturnSuccess,"worker retains connection")) {
        int created = pthread_create(&thread,NULL,clear_worker,&worker);
        if (check(created == 0,"start concurrent submitter")) {
            unsigned stale = 0, snapshots = 0;
            unsigned char *buffer = malloc(256*256*4);
            if (buffer) {
                for (unsigned i=0;i<12;++i) {
                    memset(buffer,0xa5,256*256*4);
                    result=vgpu_surface_read(s,generation,full,VGPU_SURFACE_RGBA8_OUTPUT,tight,buffer,256*256*4);
                    if (result==kIOReturnNotReady) {
                        ++stale;
                        bool intact=true;
                        for(size_t j=0;j<256*256*4;++j) if(buffer[j]!=0xa5) intact=false;
                        check(intact,"concurrent stale read writes no destination bytes");
                    } else if (result==kIOReturnSuccess) {
                        ++snapshots;
                        bool valid=true;
                        for(size_t j=0;j<256*256;++j) if(memcmp(buffer+j*4,pixels[0],4)) valid=false;
                        check(valid,"concurrent successful read is a complete requested frame");
                    } else code(result,kIOReturnSuccess,"unexpected concurrent return");
                }
                free(buffer);
            } else check(false,"concurrent buffer allocation");
            check(pthread_join(thread,NULL)==0 && worker.ok,"concurrent worker completes");
            printf("concurrency stale=%u complete_snapshots=%u\n",stale,snapshots);
            reject(s,generation,small,VGPU_SURFACE_RGBA8_OUTPUT,tight,4096,kIOReturnNotReady,"old generation after concurrent submissions");
        } else vgpu_surface_release(second);
    }
    code(vgpu_surface_close(s),kIOReturnSuccess,"close");
    code(vgpu_surface_close(s),kIOReturnSuccess,"idempotent close");
    reject(s,generation,small,VGPU_SURFACE_RGBA8_OUTPUT,tight,4096,kIOReturnNotOpen,"closed read rejected");
    code(vgpu_surface_clear(s,colors[0],&generation),kIOReturnNotOpen,"closed clear rejected");
    vgpu_surface_release(s); s=NULL;
    if (code(vgpu_surface_open(256,256,VGPU_SURFACE_BGRX8,&s),kIOReturnSuccess,"reopen")) {
        if (code(vgpu_surface_clear(s,colors[1],&generation),kIOReturnSuccess,"clear after reopen"))
            image(s,generation,1,full,tight,"reopen full frame");
    }
done:
    vgpu_surface_release(second);
    vgpu_surface_release(s);
    printf("surface probe failures=%d\n",failures);
    return failures ? 1 : 0;
}
