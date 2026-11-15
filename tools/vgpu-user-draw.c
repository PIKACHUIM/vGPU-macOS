/*
 * 编译：clang -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
 *           vgpu-user-draw.c -o vgpu-user-draw
 * 运行：sudo ./vgpu-user-draw [1..1000]（默认覆盖 §69 和 §70）
 * 非管理员权限检查：./vgpu-user-draw --expect-denied
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPEN_TYPE UINT32_C(0x56475055)
#define WIDTH 256U
#define HEIGHT 256U
#define ROW_BYTES (WIDTH * sizeof(uint32_t))
#define RED UINT32_C(0xffff0000)
#define GREEN UINT32_C(0xff00ff00)

struct vertex_input {
    uint32_t version;
    uint32_t reserved;
    float vertices[3][4];
};

_Static_assert(sizeof(float) == 4, "float must be 32 bits");
_Static_assert(sizeof(struct vertex_input) == 56, "selector 2 input must be 56 bytes");

struct image_case {
    struct vertex_input input;
    int64_t screen[3][2];
    uint32_t red_pixels;
    const char *label;
};

/* 屏幕坐标乘二；坐标均可由二进制浮点精确表示，边不经过像素中心。 */
static const struct image_case legacy_image = {
    { 1, 0, { { -0.75f, -0.75f, 0.0f, 1.0f },
              { 0.75f, -0.75f, 0.0f, 1.0f },
              { 0.0f, 0.75f, 0.0f, 1.0f } } },
    { { 64, 448 }, { 448, 448 }, { 256, 64 } }, 18432, "selector 0"
};

static const struct image_case vertex_images[] = {
    {
        { 1, 0, { { -0.6875f, -0.5625f, 0.0f, 1.0f },
                  { 0.3125f, -0.5625f, 0.5f, 1.0f },
                  { -0.6875f, 0.21875f, 1.0f, 1.0f } } },
        { { 80, 400 }, { 336, 400 }, { 80, 200 } }, 6400, "vertices A"
    },
    {
        { 1, 0, { { -0.75f, 0.75f, 1.0f, 1.0f },
                  { -0.125f, -0.5f, 0.5f, 1.0f },
                  { 0.5f, 0.75f, 0.0f, 1.0f } } },
        { { 64, 64 }, { 224, 384 }, { 384, 64 } }, 12800, "vertices B"
    }
};

static kern_return_t call_vertices(io_connect_t connection,
                                    const void *input, size_t input_size,
                                    const uint64_t *scalar_input,
                                    uint32_t scalar_input_count,
                                    uint64_t *output, uint32_t *output_count)
{
    return IOConnectCallMethod(connection, 2, scalar_input, scalar_input_count,
                               input, input_size, output, output_count,
                               NULL, NULL);
}

static bool expect_code(const char *test, kern_return_t actual,
                        kern_return_t expected);

static bool draw_vertices(io_connect_t connection,
                          const struct image_case *image, uint64_t *generation)
{
    uint64_t output[3] = { 0, 0, 0 };
    uint32_t count = 3;
    kern_return_t result = call_vertices(connection, &image->input,
                                         sizeof(image->input), NULL, 0,
                                         output, &count);
    if (!expect_code("draw vertices", result, KERN_SUCCESS))
        return false;
    if (count != 3 || output[0] == 0 || output[1] != WIDTH ||
        output[2] != HEIGHT) {
        fprintf(stderr, "FAIL selector 2 ABI: count=%" PRIu32
                " generation=%" PRIu64 " dimensions=%" PRIu64
                "x%" PRIu64 "\n", count, output[0], output[1], output[2]);
        return false;
    }
    *generation = output[0];
    return true;
}

static bool expect_code(const char *test, kern_return_t actual,
                        kern_return_t expected)
{
    if (actual == expected)
        return true;
    fprintf(stderr, "FAIL %s: return=0x%08" PRIx32
            " expected=0x%08" PRIx32 "\n", test,
            (uint32_t)actual, (uint32_t)expected);
    return false;
}

static bool expect_rejected(const char *test, kern_return_t actual)
{
    if (actual != KERN_SUCCESS)
        return true;
    fprintf(stderr, "FAIL %s: invalid call succeeded\n", test);
    return false;
}

static kern_return_t read_row(io_connect_t connection, uint64_t generation,
                              uint64_t row, uint32_t *pixels, size_t *size)
{
    uint64_t input[2] = { generation, row };
    return IOConnectCallMethod(connection, 1, input, 2, NULL, 0,
                               NULL, NULL, pixels, size);
}

static bool expect_row_code(io_connect_t connection, uint64_t generation,
                            uint64_t row, const char *test,
                            kern_return_t expected)
{
    uint32_t pixels[WIDTH];
    size_t size = sizeof(pixels);
    return expect_code(test, read_row(connection, generation, row, pixels,
                                     &size), expected);
}

static bool draw(io_connect_t connection, uint64_t *generation)
{
    uint64_t output[3] = { 0, 0, 0 };
    uint32_t count = 3;
    kern_return_t result = IOConnectCallScalarMethod(connection, 0, NULL, 0,
                                                    output, &count);
    if (!expect_code("draw", result, KERN_SUCCESS))
        return false;
    if (count != 3 || output[0] == 0 || output[1] != WIDTH ||
        output[2] != HEIGHT) {
        fprintf(stderr, "FAIL draw ABI: count=%" PRIu32
                " generation=%" PRIu64 " dimensions=%" PRIu64
                "x%" PRIu64 "\n", count, output[0], output[1], output[2]);
        return false;
    }
    *generation = output[0];
    return true;
}

static int64_t edge(int64_t ax, int64_t ay, int64_t bx, int64_t by,
                    int64_t px, int64_t py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static bool verify_image(io_connect_t connection, uint64_t generation,
                         const struct image_case *image, const char *label)
{
    uint32_t red = 0, green = 0, other = 0, mismatch = 0;
    uint32_t expected_red = 0, expected_green = 0;
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        uint32_t pixels[WIDTH];
        size_t size = sizeof(pixels);
        memset(pixels, 0, sizeof(pixels));
        kern_return_t result = read_row(connection, generation, y, pixels,
                                        &size);
        if (!expect_code("read row", result, KERN_SUCCESS)) {
            fprintf(stderr, "  %s generation=%" PRIu64 " row=%" PRIu32
                    "\n", label, generation, y);
            return false;
        }
        if (size != ROW_BYTES) {
            fprintf(stderr, "FAIL row length: row=%" PRIu32
                    " size=%zu expected=%zu\n", y, size, ROW_BYTES);
            return false;
        }
        for (uint32_t x = 0; x < WIDTH; ++x) {
            /* 坐标乘二，用整数精确表示像素中心，独立计算三条有向边。 */
            int64_t px = 2 * (int64_t)x + 1;
            int64_t py = 2 * (int64_t)y + 1;
            int64_t a = edge(image->screen[0][0], image->screen[0][1],
                             image->screen[1][0], image->screen[1][1], px, py);
            int64_t b = edge(image->screen[1][0], image->screen[1][1],
                             image->screen[2][0], image->screen[2][1], px, py);
            int64_t c = edge(image->screen[2][0], image->screen[2][1],
                             image->screen[0][0], image->screen[0][1], px, py);
            if (a == 0 || b == 0 || c == 0) {
                fprintf(stderr, "FAIL CPU reference: sample on edge\n");
                return false;
            }
            bool inside = (a < 0 && b < 0 && c < 0) ||
                          (a > 0 && b > 0 && c > 0);
            uint32_t expected = inside ? RED : GREEN;
            if (inside)
                ++expected_red;
            else
                ++expected_green;
            if (pixels[x] == RED)
                ++red;
            else if (pixels[x] == GREEN)
                ++green;
            else
                ++other;
            if (pixels[x] != expected) {
                if (mismatch == 0)
                    fprintf(stderr, "FAIL pixel: %s (%" PRIu32 ",%" PRIu32
                            ") actual=0x%08" PRIx32 " expected=0x%08"
                            PRIx32 "\n", label, x, y, pixels[x], expected);
                ++mismatch;
            }
        }
    }
    printf("%s generation=%" PRIu64 " red=%" PRIu32 " green=%" PRIu32
           " other=%" PRIu32 " mismatch=%" PRIu32 "\n",
           label, generation, red, green, other, mismatch);
    if (expected_red != image->red_pixels ||
        expected_green != WIDTH * HEIGHT - image->red_pixels ||
        red != expected_red || green != expected_green ||
        other != 0 || mismatch != 0) {
        fprintf(stderr, "FAIL image totals: CPU red=%" PRIu32
                " green=%" PRIu32 "\n", expected_red, expected_green);
        return false;
    }
    return true;
}

static bool check_invalid_calls(io_connect_t connection, uint64_t generation)
{
    uint64_t scalar[4] = { 0, 0, 0, 0 };
    uint64_t input[3] = { generation, 0, 0 };
    uint32_t pixels[WIDTH + 1];
    uint32_t count = 0;
    kern_return_t result = IOConnectCallScalarMethod(connection, 99,
                                                    NULL, 0, scalar, &count);
    if (!expect_code("unknown selector", result, kIOReturnUnsupported))
        return false;

    count = 3;
    result = IOConnectCallScalarMethod(connection, 0, input, 1, scalar, &count);
    if (!expect_rejected("draw scalar input count=1", result))
        return false;
    const uint32_t bad_draw_counts[] = { 0, 2, 4 };
    for (size_t i = 0; i < sizeof(bad_draw_counts) / sizeof(bad_draw_counts[0]);
         ++i) {
        count = bad_draw_counts[i];
        result = IOConnectCallScalarMethod(connection, 0, NULL, 0, scalar,
                                           &count);
        if (!expect_rejected("draw scalar output count", result))
            return false;
    }
    const uint32_t bad_input_counts[] = { 0, 1, 3 };
    for (size_t i = 0; i < sizeof(bad_input_counts) / sizeof(bad_input_counts[0]);
         ++i) {
        size_t size = ROW_BYTES;
        result = IOConnectCallMethod(connection, 1, input, bad_input_counts[i],
                                     NULL, 0, NULL, NULL, pixels, &size);
        if (!expect_rejected("read scalar input count", result))
            return false;
    }
    const size_t bad_sizes[] = { 0, ROW_BYTES - 1, ROW_BYTES + 1 };
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i) {
        size_t size = bad_sizes[i];
        result = read_row(connection, generation, 0, pixels, &size);
        if (!expect_rejected("read structure output size", result))
            return false;
    }
    return expect_row_code(connection, generation, HEIGHT, "row=256",
                           kIOReturnBadArgument) &&
           expect_row_code(connection, generation, UINT64_MAX,
                           "row=UINT64_MAX", kIOReturnBadArgument);
}

static bool rejected_vertices_unchanged(io_connect_t connection,
                                         kern_return_t result,
                                         uint64_t generation,
                                         const struct image_case *image,
                                         const char *label)
{
    return expect_rejected(label, result) &&
           verify_image(connection, generation, image, label);
}

static bool check_invalid_vertices(io_connect_t connection, uint64_t generation,
                                    const struct image_case *image)
{
    /* 使用位模式构造非法浮点数，不依赖 NaN 运算或编译器浮点折叠。 */
    static const struct {
        uint32_t component;
        uint32_t bits;
        const char *label;
    } bad_values[] = {
        { 0, UINT32_C(0x7fc00000), "x NaN" },
        { 0, UINT32_C(0x7f800000), "x +Inf" },
        { 0, UINT32_C(0xff800000), "x -Inf" },
        { 0, UINT32_C(0x3f800001), "x > 1" },
        { 0, UINT32_C(0xbf800001), "x < -1" },
        { 1, UINT32_C(0x7fc00000), "y NaN" },
        { 1, UINT32_C(0x7f800000), "y +Inf" },
        { 1, UINT32_C(0xff800000), "y -Inf" },
        { 1, UINT32_C(0x3f800001), "y > 1" },
        { 1, UINT32_C(0xbf800001), "y < -1" },
        { 2, UINT32_C(0x7fc00000), "z NaN" },
        { 2, UINT32_C(0x7f800000), "z +Inf" },
        { 2, UINT32_C(0xff800000), "z -Inf" },
        { 2, UINT32_C(0x3f800001), "z > 1" },
        { 2, UINT32_C(0x80000001), "z < 0 (subnormal)" },
        { 3, UINT32_C(0x7fc00000), "w NaN" },
        { 3, UINT32_C(0x7f800000), "w +Inf" },
        { 3, UINT32_C(0xff800000), "w -Inf" },
        { 3, UINT32_C(0x3f7fffff), "w < 1" },
        { 3, UINT32_C(0x3f800001), "w > 1" },
        { 3, UINT32_C(0xbf800000), "w = -1" },
        { 3, UINT32_C(0x00000000), "w = 0" }
    };
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); ++i) {
            struct vertex_input input = image->input;
            memcpy(&input.vertices[vertex][bad_values[i].component],
                   &bad_values[i].bits, sizeof(bad_values[i].bits));
            uint64_t output[3] = { 0, 0, 0 };
            uint32_t count = 3;
            char label[80];
            (void)snprintf(label, sizeof(label), "reject vertex %" PRIu32 " %s",
                           vertex, bad_values[i].label);
            kern_return_t result = call_vertices(connection, &input,
                                                 sizeof(input), NULL, 0,
                                                 output, &count);
            if (!rejected_vertices_unchanged(connection, result, generation,
                                             image, label))
                return false;
        }
    }
    const uint32_t bad_versions[] = { 0, 2, UINT32_MAX };
    const uint32_t bad_reserved[] = { 1, UINT32_MAX };
    for (size_t field = 0; field < 2; ++field) {
        const uint32_t *values = field == 0 ? bad_versions : bad_reserved;
        size_t length = field == 0 ? sizeof(bad_versions) / sizeof(uint32_t) :
                                    sizeof(bad_reserved) / sizeof(uint32_t);
        for (size_t i = 0; i < length; ++i) {
            struct vertex_input input = image->input;
            if (field == 0)
                input.version = values[i];
            else
                input.reserved = values[i];
            uint64_t output[3] = { 0, 0, 0 };
            uint32_t count = 3;
            const char *label = field == 0 ? "reject version" : "reject reserved";
            kern_return_t result = call_vertices(connection, &input,
                                                 sizeof(input), NULL, 0,
                                                 output, &count);
            if (!rejected_vertices_unchanged(connection, result, generation,
                                             image, label))
                return false;
        }
    }
    unsigned char bytes[sizeof(struct vertex_input) + 1] = { 0 };
    memcpy(bytes, &image->input, sizeof(image->input));
    const size_t bad_sizes[] = { 55, 57 };
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i) {
        uint64_t output[3] = { 0, 0, 0 };
        uint32_t count = 3;
        kern_return_t result = call_vertices(connection, bytes, bad_sizes[i],
                                             NULL, 0, output, &count);
        if (!rejected_vertices_unchanged(connection, result, generation, image,
                                         "reject vertex structure size"))
            return false;
    }
    uint64_t scalar[4] = { 0, 0, 0, 0 };
    for (uint32_t input_count = 1; input_count <= 2; ++input_count) {
        uint64_t output[3] = { 0, 0, 0 };
        uint32_t count = 3;
        kern_return_t result = call_vertices(connection, &image->input,
                                             sizeof(image->input), scalar,
                                             input_count, output, &count);
        if (!rejected_vertices_unchanged(connection, result, generation, image,
                                         "reject vertex scalar input count"))
            return false;
    }
    const uint32_t bad_counts[] = { 0, 1, 2, 4 };
    for (size_t i = 0; i < sizeof(bad_counts) / sizeof(bad_counts[0]); ++i) {
        uint32_t count = bad_counts[i];
        kern_return_t result = call_vertices(connection, &image->input,
                                             sizeof(image->input), NULL, 0,
                                             scalar, &count);
        if (!rejected_vertices_unchanged(connection, result, generation, image,
                                         "reject vertex scalar output count"))
            return false;
    }
    uint32_t output_structure = 0;
    size_t output_size = sizeof(output_structure);
    uint32_t count = 3;
    kern_return_t result = IOConnectCallMethod(connection, 2, NULL, 0,
                                               &image->input,
                                               sizeof(image->input), scalar,
                                               &count, &output_structure,
                                               &output_size);
    return rejected_vertices_unchanged(connection, result, generation, image,
                                       "reject vertex structure output");
}

static bool close_connection(io_connect_t *connection)
{
    if (*connection == IO_OBJECT_NULL)
        return true;
    kern_return_t result = IOServiceClose(*connection);
    *connection = IO_OBJECT_NULL;
    return expect_code("IOServiceClose", result, KERN_SUCCESS);
}

int main(int argc, char **argv)
{
    unsigned long rounds = 3;
    bool denied = false;
    if (argc == 2 && strcmp(argv[1], "--expect-denied") == 0) {
        denied = true;
    } else if (argc == 2) {
        char *end = NULL;
        errno = 0;
        rounds = strtoul(argv[1], &end, 10);
        if (argv[1][0] < '0' || argv[1][0] > '9' || errno != 0 ||
            end == argv[1] || *end != '\0' || rounds < 1 || rounds > 1000) {
            fprintf(stderr, "Usage: %s [1..1000 | --expect-denied]\n", argv[0]);
            return EXIT_FAILURE;
        }
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [1..1000 | --expect-denied]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (denied && geteuid() == 0) {
        fprintf(stderr, "FAIL --expect-denied requires a non-root process\n");
        return EXIT_FAILURE;
    }

    int status = EXIT_FAILURE;
    io_connect_t first = IO_OBJECT_NULL;
    io_connect_t second = IO_OBJECT_NULL;
    CFMutableDictionaryRef matching = IOServiceMatching("vgpuFramebuffer");
    if (matching == NULL) {
        fprintf(stderr, "FAIL IOServiceMatching\n");
        return EXIT_FAILURE;
    }
    /* IOServiceGetMatchingService 消耗 matching 的引用。 */
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, matching);
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "FAIL vgpuFramebuffer service not found\n");
        return EXIT_FAILURE;
    }
    kern_return_t result = IOServiceOpen(service, mach_task_self(), OPEN_TYPE,
                                         &first);
    if (denied) {
        if (expect_code("non-root open", result, kIOReturnNotPrivileged))
            status = EXIT_SUCCESS;
        goto cleanup;
    }
    if (!expect_code("IOServiceOpen (administrator required)", result,
                     KERN_SUCCESS))
        goto cleanup;

    uint32_t initial_pixels[WIDTH];
    size_t initial_size = sizeof(initial_pixels);
    result = read_row(first, 0, 0, initial_pixels, &initial_size);
    if (!expect_rejected("generation=0 before draw", result))
        goto cleanup;

    uint64_t generation = 0;
    for (unsigned long round = 1; round <= rounds; ++round) {
        uint64_t previous = generation;
        if (!draw(first, &generation))
            goto cleanup;
        if (round > 1 &&
            !expect_row_code(first, previous, 0, "previous generation",
                             kIOReturnNotReady))
            goto cleanup;
        if (round == 1 && !check_invalid_calls(first, generation))
            goto cleanup;
        char label[48];
        (void)snprintf(label, sizeof(label), "round %lu/%lu", round, rounds);
        if (!verify_image(first, generation, &legacy_image, label))
            goto cleanup;
    }

    for (unsigned long round = 1; round <= rounds; ++round) {
        for (size_t i = 0; i < sizeof(vertex_images) / sizeof(vertex_images[0]);
             ++i) {
            uint64_t previous = generation;
            const struct image_case *image = &vertex_images[i];
            if (!draw_vertices(first, image, &generation) ||
                !expect_row_code(first, previous, 0, "selector 2 old generation",
                                 kIOReturnNotReady) ||
                !verify_image(first, generation, image, image->label))
                goto cleanup;
            if (round == 1 && i == 0 &&
                !check_invalid_vertices(first, generation, image))
                goto cleanup;
            previous = generation;
            if (!draw(first, &generation) ||
                !expect_row_code(first, previous, 0, "selector 0 old generation",
                                 kIOReturnNotReady) ||
                !verify_image(first, generation, &legacy_image,
                              "selector 0 after vertices"))
                goto cleanup;
        }
    }

    for (size_t i = 0; i < 3; ++i) {
        uint64_t previous = generation;
        const struct image_case *image = &vertex_images[i % 2];
        if (!draw_vertices(first, image, &generation) ||
            !expect_row_code(first, previous, 0,
                             "consecutive selector 2 old generation",
                             kIOReturnNotReady) ||
            !verify_image(first, generation, image,
                          i == 1 ? "consecutive vertices B" :
                                   "consecutive vertices A"))
            goto cleanup;
    }

    result = IOServiceOpen(service, mach_task_self(), OPEN_TYPE, &second);
    if (!expect_code("second IOServiceOpen", result, KERN_SUCCESS))
        goto cleanup;
    uint64_t previous = generation;
    if (!draw(second, &generation) ||
        !expect_row_code(first, previous, 0, "cross-connection old generation",
                         kIOReturnNotReady) ||
        !verify_image(second, generation, &legacy_image, "cross-connection"))
        goto cleanup;

    previous = generation;
    if (!draw_vertices(second, &vertex_images[1], &generation) ||
        !expect_row_code(first, previous, 0,
                         "cross-connection vertices old generation",
                         kIOReturnNotReady) ||
        !verify_image(first, generation, &vertex_images[1],
                      "cross-connection vertices B read on first"))
        goto cleanup;

    if (!close_connection(&second))
        goto cleanup;
    if (!close_connection(&first))
        goto cleanup;
    result = IOServiceOpen(service, mach_task_self(), OPEN_TYPE, &first);
    if (!expect_code("reopen", result, KERN_SUCCESS) ||
        !draw(first, &generation) ||
        !verify_image(first, generation, &legacy_image, "reopen"))
        goto cleanup;
    previous = generation;
    if (!draw_vertices(first, &vertex_images[0], &generation) ||
        !expect_row_code(first, previous, 0, "reopen vertices old generation",
                         kIOReturnNotReady) ||
        !verify_image(first, generation, &vertex_images[0], "reopen vertices A"))
        goto cleanup;
    status = EXIT_SUCCESS;

cleanup:
    if (!close_connection(&second))
        status = EXIT_FAILURE;
    if (!close_connection(&first))
        status = EXIT_FAILURE;
    if (!expect_code("IOObjectRelease(service)", IOObjectRelease(service),
                     KERN_SUCCESS))
        status = EXIT_FAILURE;
    if (status == EXIT_SUCCESS)
        printf("PASS %s\n", denied ? "non-root open denied" : "all checks");
    return status;
}
