#include "../gld-objects/vgpu_objects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "失败 %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == VGO_OK)
#define NATIVE(x) CHECK((x) == kIOReturnSuccess)
extern unsigned vgo_fixture_surfaces, vgo_fixture_owners, vgo_fixture_images;
extern unsigned vgo_fixture_owner_release_calls, vgo_fixture_context_releases;
extern int vgo_fixture_bgra, vgo_fixture_fail_owner_release, vgo_fixture_fail_retain;
extern vgc_handle vgo_fixture_fail_release_handle;
extern vgc_handle vgo_fixture_last_context_share, vgo_fixture_last_context;
size_t vgo_fixture_native_refs(vgc_handle id);
void vgo_fixture_native_set_refs(vgc_handle id, size_t refs);

static vgo_views views(void *d, size_t dn, void *s, size_t sn) {
    return (vgo_views){{d, dn}, {s, sn}, {0}};
}
static void empty(void) {
    NATIVE(vgc_drain_releases());
    CHECK(!vgo_fixture_surfaces && !vgo_fixture_owners && !vgo_fixture_images);
}
static void strict_import(void) {
    vgc_handle native, image, fb, context;
    vgo_handle imported = 123;
    unsigned char h[VGC_H_LENGTH] = {0}, b[VGC_B_LENGTH] = {0};
    vgo_fixture_bgra = 1;
    NATIVE(vgc_share_create_bgra(&native));
    NATIVE(vgc_image_create(native, (vgc_image_desc){256, 256, VGPU_SURFACE_BGRA8, 0}, &image));
    NATIVE(vgc_framebuffer_create(native, &fb));
    NATIVE(vgc_context_create(native, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &context));
    vgc_handle invalid[] = {0, UINT64_MAX, image, fb, context};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        size_t refs = vgo_fixture_native_refs(invalid[i]);
        imported = 123;
        CHECK(vgc_share_retain(invalid[i]) == kIOReturnBadArgument);
        CHECK(vgo_share_import_context(invalid[i], &imported) == VGO_BAD_HANDLE && !imported);
        CHECK(vgo_fixture_native_refs(invalid[i]) == refs);
    }
    CHECK(vgo_share_import_context(native, NULL) == VGO_INVALID);
    CHECK(vgo_share_import_context(native, (vgo_handle *)(uintptr_t)(UINTPTR_MAX - 7)) == VGO_INVALID);
    size_t refs = vgo_fixture_native_refs(native);
    vgo_fixture_fail_retain = 1;
    imported = 123;
    CHECK(vgo_share_import_context(native, &imported) == VGO_CONTEXT_FAILED && !imported);
    CHECK(vgo_fixture_native_refs(native) == refs);
    vgo_fixture_fail_retain = 0;
    vgo_fixture_native_set_refs(native, SIZE_MAX);
    imported = 123;
    CHECK(vgc_share_retain(native) == kIOReturnNoResources);
    CHECK(vgo_share_import_context(native, &imported) == VGO_OVERFLOW && !imported);
    CHECK(vgo_fixture_native_refs(native) == SIZE_MAX);
    vgo_fixture_native_set_refs(native, refs);
    OK(vgo_share_import_context(native, &imported));
    CHECK(vgo_fixture_native_refs(native) == refs + 1);
    vgo_handle invalid_vgo = 123;
    CHECK(vgo_share_import_context(imported, &invalid_vgo) == VGO_BAD_HANDLE && !invalid_vgo);
    OK(vgo_release(imported));
    CHECK(vgo_fixture_native_refs(native) == refs);
    NATIVE(vgc_release(context)); NATIVE(vgc_release(fb)); NATIVE(vgc_release(image));
    NATIVE(vgc_release(native));
    imported = 123;
    CHECK(vgo_share_import_context(native, &imported) == VGO_BAD_HANDLE && !imported);
    empty();
}
static void shared_lifetime(void) {
    vgc_handle native, image;
    vgo_handle share, second, sampler, pipeline, program, context;
    vgo_state state;
    unsigned char d[0x30] = {0}, u[4] = {1};
    unsigned char h[VGC_H_LENGTH] = {0}, b[VGC_B_LENGTH] = {0};
    NATIVE(vgc_share_create_bgra(&native));
    OK(vgo_share_import_context(native, &share));
    OK(vgo_share_import_context(native, &second));
    CHECK(share != second && share != native && vgo_fixture_native_refs(native) == 3);
    OK(vgo_release(second));
    NATIVE(vgc_release(native));
    CHECK(vgo_fixture_native_refs(native) == 1 && vgo_fixture_owners == 1);
    OK(vgo_sampler_create(share, views(d, sizeof(d), u, 1), &sampler));
    OK(vgo_pipeline_create(share, views(d, 4, u, 4), &pipeline));
    OK(vgo_program_create(share, views(d, 1, u, 1), &program));
    OK(vgo_context_create(share, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &context));
    CHECK(vgo_fixture_last_context_share == native && vgo_fixture_native_refs(native) == 2);
    OK(vgo_state_get(share, &state)); CHECK(state.references == 5);
    OK(vgo_pipeline_bind(context, pipeline));
    NATIVE(vgc_image_create(native, (vgc_image_desc){256, 256, VGPU_SURFACE_BGRA8, 0}, &image));
    CHECK(vgo_fixture_owners == 1 && vgo_fixture_images == 1 && !vgo_fixture_surfaces);
    OK(vgo_release(share)); OK(vgo_release(sampler)); OK(vgo_release(program));
    OK(vgo_release(pipeline)); OK(vgo_release(context));
    CHECK(vgo_state_get(share, &state) == VGO_BAD_HANDLE);
    CHECK(vgo_fixture_native_refs(native) == 1 && vgo_fixture_owners == 1);
    NATIVE(vgc_release(image));
    CHECK(!vgo_fixture_native_refs(native));
    empty();
}
static void direct_release_failure(void) {
    vgc_handle native;
    vgo_handle share, other;
    vgo_state state;
    NATIVE(vgc_share_create_bgra(&native));
    OK(vgo_share_import_context(native, &share));
    NATIVE(vgc_release(native));
    vgo_fixture_fail_owner_release = 1;
    unsigned calls = vgo_fixture_owner_release_calls;
    CHECK(vgo_release(share) == VGO_CONTEXT_FAILED);
    CHECK(vgo_release(share) == VGO_CONTEXT_FAILED);
    CHECK(vgo_fixture_owner_release_calls == calls + 2);
    OK(vgo_state_get(share, &state)); CHECK(state.references == 1);
    CHECK(vgo_fixture_native_refs(native) == 1 && vgo_fixture_owners == 1);
    CHECK(vgo_retain(share) == VGO_CONTEXT_FAILED);
    unsigned char d = 0, u = 0;
    other = 123;
    CHECK(vgo_program_create(share, views(&d, 1, &u, 1), &other) == VGO_CONTEXT_FAILED && !other);
    vgo_fixture_fail_owner_release = 0;
    OK(vgo_release(share));
    CHECK(vgo_release(share) == VGO_BAD_HANDLE && !vgo_fixture_native_refs(native));
    empty();
}
static vgo_status lease_retain(void *p) { ++*(unsigned *)p; return VGO_OK; }
static void lease_release(void *p) { CHECK(*(unsigned *)p == 1); --*(unsigned *)p; }
static void child_release_failure(void) {
    vgo_status (*const creators[])(vgo_handle, vgo_views, vgo_handle *) = {
        vgo_sampler_create, vgo_pipeline_create, vgo_program_create};
    vgo_status (*const destroyers[])(vgo_handle, vgo_handle) = {
        vgo_sampler_destroy, vgo_pipeline_destroy, vgo_program_destroy};
    for (size_t i = 0; i < 3; ++i) {
        vgc_handle native;
        vgo_handle share, child;
        vgo_state state;
        unsigned lease_refs = 0;
        unsigned char d[0x30] = {0}, u[4] = {0};
        vgo_views v = views(d, sizeof(d), u, sizeof(u));
        v.lease = (vgo_lease){&lease_refs, lease_retain, lease_release};
        NATIVE(vgc_share_create_bgra(&native));
        OK(vgo_share_import_context(native, &share)); NATIVE(vgc_release(native));
        OK(creators[i](share, v, &child)); OK(vgo_release(share));
        vgo_fixture_fail_owner_release = 1;
        CHECK(destroyers[i](share, child) == VGO_CONTEXT_FAILED);
        CHECK(vgo_release(child) == VGO_CONTEXT_FAILED);
        OK(vgo_state_get(child, &state));
        CHECK(state.references == 1 && state.share == share && state.attached && lease_refs == 1);
        OK(vgo_state_get(share, &state)); CHECK(state.references == 1);
        CHECK(vgo_retain(child) == VGO_CONTEXT_FAILED);
        CHECK(vgo_detach_views(share, child) == VGO_CONTEXT_FAILED);
        vgo_fixture_fail_owner_release = 0;
        OK(destroyers[i](share, child));
        CHECK(!lease_refs && vgo_state_get(child, &state) == VGO_BAD_HANDLE);
        CHECK(vgo_state_get(share, &state) == VGO_BAD_HANDLE);
        empty();
    }
}
static void context_release_failure(void) {
    vgc_handle native;
    vgo_handle share, context, pipeline;
    vgo_state state;
    unsigned lease_refs = 0;
    unsigned char d[4] = {0}, u[4] = {0};
    unsigned char h[VGC_H_LENGTH] = {0}, b[VGC_B_LENGTH] = {0};
    vgo_views v = views(d, sizeof(d), u, sizeof(u));
    v.lease = (vgo_lease){&lease_refs, lease_retain, lease_release};
    NATIVE(vgc_share_create_bgra(&native));
    OK(vgo_share_import_context(native, &share)); NATIVE(vgc_release(native));
    OK(vgo_context_create(share, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &context));
    vgc_handle native_context = vgo_fixture_last_context;
    OK(vgo_pipeline_create(share, v, &pipeline)); OK(vgo_pipeline_bind(context, pipeline));
    OK(vgo_release(pipeline)); OK(vgo_release(share));
    vgo_fixture_fail_release_handle = native_context;
    CHECK(vgo_release(context) == VGO_CONTEXT_FAILED);
    OK(vgo_state_get(context, &state)); CHECK(state.references == 1 && state.pipeline == pipeline);
    CHECK(lease_refs == 1 && vgo_fixture_native_refs(native_context) == 1);
    CHECK(vgo_pipeline_unbind(context, pipeline) == VGO_CONTEXT_FAILED);
    CHECK(vgo_pipeline_bind(context, pipeline) == VGO_CONTEXT_FAILED);
    vgo_fixture_fail_release_handle = 0;
    vgo_fixture_fail_owner_release = 1;
    CHECK(vgo_release(context) == VGO_CONTEXT_FAILED);
    CHECK(!vgo_fixture_native_refs(native_context) && !lease_refs);
    CHECK(vgo_state_get(pipeline, &state) == VGO_BAD_HANDLE);
    OK(vgo_state_get(context, &state)); CHECK(!state.pipeline && state.share == share && state.references == 1);
    unsigned calls = vgo_fixture_context_releases;
    CHECK(vgo_release(context) == VGO_CONTEXT_FAILED);
    CHECK(vgo_fixture_context_releases == calls);
    vgo_fixture_fail_owner_release = 0;
    OK(vgo_release(context)); CHECK(vgo_fixture_context_releases == calls);
    CHECK(vgo_state_get(share, &state) == VGO_BAD_HANDLE);
    empty();
}
int main(void) {
    strict_import(); shared_lifetime(); direct_release_failure();
    child_release_failure(); context_release_failure();
    printf("PASS: %u 检查；BGRA 导入/严格类别/强引用/故障重试，真实 context/objects 加 CPU fixture，非 GPU 验证。\n", checks);
    return 0;
}
