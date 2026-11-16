/* 独立 CPU/fault probe；IOKit 提交及关闭被替换，绝不访问 GPU。 */
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static kern_return_t fake_call(mach_port_t, uint32_t, const uint64_t *, uint32_t,
    const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
static kern_return_t fake_close(io_connect_t connection) { (void)connection; return 0; }
#define IOConnectCallMethod fake_call
#define IOServiceClose fake_close
#include "vgpu_surface.c"
#include "../gld-context/vgpu_context.c"

static unsigned checks, failures, info_calls, draw_calls, clear_calls;
static unsigned fault_info, fault_draw;
static IOReturn info_error, draw_error;
static uint64_t backend_generation[3] = {0, 10, 20}, next_handle = 1, last_draw, last_read;
static const float triangle[3][4] = {{-1, -1, 0, 1}, {1, -1, 0.5f, 1}, {0, 1, 1, 1}};
static float expected_vertices[3][4];
static float *mutate_input;
static vgpu_surface_image *locked_image;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); } } while (0)

static kern_return_t fake_call(mach_port_t connection, uint32_t selector,
    const uint64_t *scalar_in, uint32_t scalar_count, const void *input, size_t input_size,
    uint64_t *scalar_out, uint32_t *out_count, void *output, size_t *output_size)
{
    CHECK(connection == 23 && !scalar_in && !scalar_count && !scalar_out && !out_count);
    if (locked_image && (selector == 8 || selector == 6)) {
        int rc = pthread_mutex_trylock(&locked_image->lock);
        CHECK(rc == EBUSY);
        if (!rc) pthread_mutex_unlock(&locked_image->lock);
        rc = pthread_mutex_trylock(&locked_image->owner->transport->lock);
        CHECK(rc == EBUSY);
        if (!rc) pthread_mutex_unlock(&locked_image->owner->transport->lock);
    }
    if (selector == 4 || selector == 8) {
        uint64_t handle;
        if (selector == 4) {
            CHECK(input_size == sizeof(struct image_create_request));
            handle = next_handle++;
        } else {
            ++info_calls;
            CHECK(input_size == sizeof(struct image_handle_request));
            const struct image_handle_request *in = input;
            CHECK(in->version == 1 && in->byte_size == sizeof(*in) && !in->reserved);
            handle = in->handle;
            if (mutate_input) { mutate_input[0] = 42; mutate_input = NULL; }
            if (info_error) return info_error;
        }
        CHECK(handle > 0 && handle < 3 && *output_size == sizeof(struct image_info_response));
        struct image_info_response out = {1, sizeof(out), handle, backend_generation[handle],
            256, 256, VGPU_SURFACE_BGRA8, 1024, 262144, 7, {0, 0}};
        if (selector == 8) switch (fault_info) {
            case 1: --*output_size; break;
            case 2: out.version++; break;
            case 3: out.byte_size--; break;
            case 4: out.handle++; break;
            case 5: out.generation = 0; break;
            case 6: out.width--; break;
            case 7: out.height--; break;
            case 8: out.format--; break;
            case 9: out.row_bytes--; break;
            case 10: out.backing_bytes--; break;
            case 11: out.flags ^= 1; break;
            case 12: out.reserved[0] = 1; break;
            case 13: out.reserved[1] = 1; break;
        }
        memcpy(output, &out, sizeof(out));
        return 0;
    }
    if (selector == 6) {
        CHECK(input_size == 96 && *output_size == sizeof(struct image_clear_response));
        const struct image_clear_request *in = input;
        CHECK(in->version == 1 && in->byte_size == 96 && !in->flags);
        CHECK(in->handle > 0 && in->handle < 3);
        CHECK(in->expected_generation == backend_generation[in->handle]);
        if (in->opcode == 1) { ++clear_calls; return kIOReturnUnsupported; }
        ++draw_calls; last_draw = in->handle;
        CHECK(in->opcode == 4);
        const uint32_t zeros[4] = {0};
        CHECK(!memcmp(in->rgba_bits, zeros, sizeof(zeros)));
        CHECK(!memcmp(in->vertices_bits, expected_vertices, sizeof(expected_vertices)));
        if (draw_error) return draw_error;
        uint64_t generation = ++backend_generation[in->handle];
        struct image_clear_response out = {1, sizeof(out), in->handle, generation, 256, 256};
        switch (fault_draw) {
            case 1: --*output_size; break;
            case 2: out.version++; break;
            case 3: out.byte_size--; break;
            case 4: out.handle++; break;
            case 5: out.generation = 0; break;
            case 6: out.generation--; break;
            case 7: out.generation++; break;
            case 8: out.width--; break;
            case 9: out.height--; break;
        }
        memcpy(output, &out, sizeof(out));
        return 0;
    }
    if (selector == 7) {
        const struct image_read_request *in = input;
        CHECK(input_size == sizeof(*in) && *output_size == sizeof(struct image_read_response));
        last_read = in->handle;
        if (in->generation != backend_generation[in->handle]) return kIOReturnNotReady;
        struct image_read_response out = {1, sizeof(out), in->handle, in->generation,
            in->row, 1024, {0}};
        memcpy(output, &out, sizeof(out));
        return 0;
    }
    if (selector == 5) {
        const struct image_handle_request *in = input;
        struct image_release_response out = {1, sizeof(out), in->handle, 1, 0};
        memcpy(output, &out, sizeof(out));
        return 0;
    }
    CHECK(false);
    return kIOReturnUnsupported;
}

static void check_surface(vgpu_surface_image *image)
{
    uint64_t generation = 1234;
    unsigned before = info_calls;
    CHECK(vgpu_surface_image_draw_triangle(NULL, triangle, &generation) == kIOReturnBadArgument);
    CHECK(vgpu_surface_image_draw_triangle(image, NULL, &generation) == kIOReturnBadArgument);
    CHECK(vgpu_surface_image_draw_triangle(image, triangle, NULL) == kIOReturnBadArgument);
    CHECK(vgpu_surface_image_draw_triangle(image, triangle, &image->handle) == kIOReturnBadArgument);
    CHECK(vgpu_surface_image_draw_triangle(image, triangle,
        (uint64_t *)(void *)image->owner) == kIOReturnBadArgument);
    CHECK(vgpu_surface_image_draw_triangle(image, triangle,
        (uint64_t *)(void *)image->owner->transport) == kIOReturnBadArgument);
    const uint32_t bad[] = {0x7fc00000, 0x7f800000, 0xff800000, 0x3f800001, 0xbf800001};
    for (unsigned v = 0; v < 3; ++v) for (unsigned c = 0; c < 4; ++c)
        for (unsigned b = 0; b < sizeof(bad) / sizeof(bad[0]); ++b) {
            float input[3][4]; memcpy(input, triangle, sizeof(input));
            memcpy(&input[v][c], &bad[b], 4);
            CHECK(vgpu_surface_image_draw_triangle(image, input, &generation) == kIOReturnBadArgument);
        }
    float invalid[3][4]; memcpy(invalid, triangle, sizeof(invalid));
    invalid[0][2] = -0.5f;
    CHECK(vgpu_surface_image_draw_triangle(image, invalid, &generation) == kIOReturnBadArgument);
    invalid[0][2] = 0; invalid[0][3] = 0;
    CHECK(vgpu_surface_image_draw_triangle(image, invalid, &generation) == kIOReturnBadArgument);
    CHECK(generation == 1234 && info_calls == before);
    image->state = VGPU_SURFACE_IMAGE_RELEASE_FAILED;
    CHECK(vgpu_surface_image_draw_triangle(image, triangle, &generation) == kIOReturnNotOpen);
    image->state = VGPU_SURFACE_IMAGE_CLOSED;
    CHECK(vgpu_surface_image_draw_triangle(image, triangle, &generation) == kIOReturnNotOpen);
    image->state = VGPU_SURFACE_IMAGE_LIVE;
    unsigned submits = draw_calls;
    for (fault_info = 1; fault_info <= 13; ++fault_info)
        CHECK(vgpu_surface_image_draw_triangle(image, triangle, &generation) == kIOReturnIOError && generation == 1234);
    fault_info = 0; info_error = kIOReturnNotReady;
    CHECK(vgpu_surface_image_draw_triangle(image, triangle, &generation) == info_error);
    info_error = 0;
    CHECK(draw_calls == submits);
    for (fault_draw = 1; fault_draw <= 9; ++fault_draw)
        CHECK(vgpu_surface_image_draw_triangle(image, triangle, &generation) == kIOReturnIOError && generation == 1234);
    fault_draw = 0;
    const IOReturn errors[] = {kIOReturnUnsupported, kIOReturnNotReady, kIOReturnIOError, kIOReturnNoResources};
    for (unsigned n = 0; n < sizeof(errors) / sizeof(errors[0]); ++n) {
        draw_error = errors[n];
        CHECK(vgpu_surface_image_draw_triangle(image, triangle, &generation) == draw_error && generation == 1234);
    }
    draw_error = 0;
    backend_generation[1] = UINT64_MAX;
    CHECK(vgpu_surface_image_draw_triangle(image, triangle, &generation) == kIOReturnIOError && generation == 1234);
    backend_generation[1] = 100;
    union { float vertices[3][4]; uint64_t generation; } alias;
    memcpy(alias.vertices, triangle, sizeof(alias.vertices));
    mutate_input = &alias.vertices[0][0];
    CHECK(!vgpu_surface_image_draw_triangle(image, alias.vertices, &alias.generation) && alias.generation == 101);
    float negative_zero[3][4]; memcpy(negative_zero, triangle, sizeof(negative_zero));
    negative_zero[0][2] = -0.0f;
    memcpy(expected_vertices, negative_zero, sizeof(expected_vertices));
    CHECK(!vgpu_surface_image_draw_triangle(image, negative_zero, &generation) && generation == 102);
    memcpy(expected_vertices, triangle, sizeof(expected_vertices));
}

int main(void)
{
    memcpy(expected_vertices, triangle, sizeof(expected_vertices));
    /* 手动建立仅 CPU transport，所有后续对象操作走生产 API。 */
    vgpu_surface_owner *owner = calloc(1, sizeof(*owner));
    owner->transport = calloc(1, sizeof(*owner->transport));
    pthread_mutex_init(&owner->transport->lock, NULL);
    owner->transport->connection = 23; owner->transport->references = 1;
    struct object *share = allocate(SHARE, NULL);
    share->u.s.owner = owner;
    vgc_handle s = share->id, images[2], fb[2], context;
    vgc_image_desc desc = {256, 256, VGPU_SURFACE_BGRA8, 0};
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(!vgc_image_create(s, desc, &images[n]));
        CHECK(!vgc_framebuffer_create(s, &fb[n]));
        CHECK(!vgc_framebuffer_attach(fb[n], 0, images[n]));
    }
    vgpu_surface_image *image = lookup(images[0], IMAGE)->u.i.image;
    locked_image = image;
    check_surface(image);
    locked_image = NULL;
    unsigned char h[VGC_H_LENGTH] = {0}, b[VGC_B_LENGTH] = {0};
    uint32_t alignment = 4; memcpy(h + 0x4ee4, &alignment, 4);
    CHECK(!vgc_context_create(s, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &context));
    uint64_t generation = 999, flushed = 888;
    CHECK(vgc_draw_triangle(context, triangle, &generation) == kIOReturnNotReady && generation == 999);
    memcpy(b + 0x4a0, &fb[0], 8); memcpy(b + 0x4a8, &fb[1], 8);
    uint32_t dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    CHECK(!vgc_context_update(context, &dirty) && !dirty);
    CHECK(!vgc_draw_triangle(context, triangle, &generation) && last_draw == 1);
    vgc_state state;
    CHECK(!vgc_context_state(context, &state) && !state.failed && !state.dirty && state.completed_generation == generation);
    CHECK(lookup(images[0], IMAGE)->u.i.generation == generation && lookup(images[1], IMAGE)->u.i.generation == 0);
    CHECK(!vgc_flush_completed(context, &flushed) && flushed == generation && last_read == 1);
    /* 重绑 draw/read 不改变 last；公开引用释放后 last 仍保持目标有效。 */
    memcpy(b + 0x4a0, &fb[1], 8); dirty = VGC_DRAW_DIRTY;
    CHECK(!vgc_context_update(context, &dirty));
    CHECK(!vgc_framebuffer_attach(fb[0], 0, 0));
    CHECK(!vgc_release(images[0]));
    CHECK(!vgc_flush_completed(context, &flushed) && last_read == 1);
    draw_error = kIOReturnUnsupported;
    uint64_t saved = generation;
    CHECK(vgc_draw_triangle(context, triangle, &generation) == draw_error && generation == saved && last_draw == 2);
    CHECK(!vgc_context_state(context, &state) && state.failed && state.dirty && state.completed_generation == saved);
    CHECK(lookup(context, CONTEXT)->u.c.last->u.i.image == image);
    CHECK(vgc_flush_completed(context, &flushed) == kIOReturnNotReady && flushed == saved);
    draw_error = 0;
    union { float vertices[3][4]; uint64_t generation; } alias;
    memcpy(alias.vertices, triangle, sizeof(alias.vertices));
    mutate_input = &alias.vertices[0][0];
    CHECK(!vgc_draw_triangle(context, alias.vertices, &alias.generation) && last_draw == 2);
    generation = alias.generation;
    CHECK(!vgc_context_state(context, &state) && !state.failed && !state.dirty && state.completed_generation == generation);
    CHECK(!vgc_flush_completed(context, &flushed) && flushed == generation && last_read == 2);
    unsigned char pixel[4];
    CHECK(!vgc_read(context, generation, (vgpu_surface_rect){0, 0, 1, 1}, pixel, sizeof(pixel)));
    ++backend_generation[2];
    CHECK(vgc_flush_completed(context, &flushed) == kIOReturnNotReady && flushed == generation);
    CHECK(!vgc_draw_triangle(context, triangle, &generation));
    saved = generation;
    fault_draw = 4;
    CHECK(vgc_draw_triangle(context, triangle, &generation) == kIOReturnIOError && generation == saved);
    CHECK(!vgc_context_state(context, &state) && state.failed && state.dirty && state.completed_generation == saved);
    CHECK(lookup(images[1], IMAGE)->u.i.generation == saved);
    fault_draw = 0;
    CHECK(!vgc_draw_triangle(context, triangle, &generation));
    float invalid[3][4]; memcpy(invalid, triangle, sizeof(invalid)); invalid[2][3] = 2;
    saved = generation;
    unsigned submits = draw_calls;
    CHECK(vgc_draw_triangle(context, invalid, &generation) == kIOReturnBadArgument && generation == saved);
    CHECK(!vgc_context_state(context, &state) && state.failed && state.dirty && state.completed_generation == saved);
    CHECK(vgc_draw_triangle(context, triangle, (uint64_t *)(void *)h) == kIOReturnBadArgument);
    CHECK(vgc_draw_triangle(context, triangle, (uint64_t *)(void *)b) == kIOReturnBadArgument);
    CHECK(vgc_draw_triangle(0, triangle, &generation) == kIOReturnBadArgument);
    CHECK(vgc_draw_triangle(context, NULL, &generation) == kIOReturnBadArgument);
    CHECK(vgc_draw_triangle(context, triangle, NULL) == kIOReturnBadArgument);
    CHECK(!vgc_framebuffer_dirty(fb[1], 2));
    CHECK(vgc_draw_triangle(context, triangle, &generation) == kIOReturnUnsupported);
    CHECK(draw_calls == submits);
    CHECK(!vgc_context_state(context, &state) && state.failed && state.dirty);
    struct object *legacy = allocate(SHARE, NULL);
    vgc_handle legacy_id = legacy->id, legacy_context;
    CHECK(!vgc_context_create(legacy_id, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &legacy_context));
    CHECK(vgc_draw_triangle(legacy_context, triangle, &generation) == kIOReturnUnsupported);
    CHECK(!vgc_release(legacy_context) && !vgc_release(legacy_id));
    CHECK(!vgc_release(context));
    for (unsigned n = 0; n < 2; ++n) CHECK(!vgc_release(fb[n]));
    CHECK(!vgc_release(images[1]));
    CHECK(!vgc_release(s) && !vgc_drain_releases());
    CHECK(!objects && !clear_calls);
    printf("RESULT checks=%u failures=%u info=%u draw=%u clear=%u\n", checks, failures, info_calls, draw_calls, clear_calls);
    return failures ? 1 : 0;
}
