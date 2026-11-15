/*
 * 编译：clang -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
 *           vgpu-user-draw.c -o vgpu-user-draw
 * 运行：sudo ./vgpu-user-draw [1..1000]
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
                         const char *label)
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
            int64_t a = edge(64, 448, 448, 448, px, py);
            int64_t b = edge(448, 448, 256, 64, px, py);
            int64_t c = edge(256, 64, 64, 448, px, py);
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
    if (expected_red != 18432 || expected_green != 47104 ||
        red != 18432 || green != 47104 || other != 0 || mismatch != 0) {
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
    kern_return_t result = IOConnectCallScalarMethod(connection, UINT32_MAX,
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
        if (!verify_image(first, generation, label))
            goto cleanup;
    }

    result = IOServiceOpen(service, mach_task_self(), OPEN_TYPE, &second);
    if (!expect_code("second IOServiceOpen", result, KERN_SUCCESS))
        goto cleanup;
    uint64_t previous = generation;
    if (!draw(second, &generation) ||
        !expect_row_code(first, previous, 0, "cross-connection old generation",
                         kIOReturnNotReady) ||
        !verify_image(second, generation, "cross-connection"))
        goto cleanup;

    if (!close_connection(&second))
        goto cleanup;
    if (!close_connection(&first))
        goto cleanup;
    result = IOServiceOpen(service, mach_task_self(), OPEN_TYPE, &first);
    if (!expect_code("reopen", result, KERN_SUCCESS) ||
        !draw(first, &generation) ||
        !verify_image(first, generation, "reopen"))
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
