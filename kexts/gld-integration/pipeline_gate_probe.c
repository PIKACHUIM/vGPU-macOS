#include "context_exports.h"
#include "../gld-device/gld_device.h"
#include "../gld-pixel/gld_pixel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 所有检查在 NDEBUG 下仍执行；仅替代 device/surface 平台边界。 */
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static unsigned device_token;
static IOReturn liveness = kIOReturnNotReady;
int32_t vgpuGLDDeviceGetStatus(void *device, struct gld_device_status *out) {
    if (device != &device_token || !out) return -1;
    *out = (struct gld_device_status){.registry_id = 73, .connected = 1,
        .liveness_status = liveness, .renderer_id = GLD_DEVICE_DRIVER_ID, .display_mask = 1};
    return 0;
}
extern int vgo_fixture_bgra;
extern unsigned vgo_fixture_owners, vgo_fixture_images;
static void slot(unsigned char *b, size_t base, unsigned stage, void *value) {
    memcpy(b + base + 8 * stage, &value, sizeof(value));
}
static void reject(void *context) {
    gi_default_pipeline_snapshot out, before;
    memset(&out, 0x9b, sizeof(out)); memcpy(&before, &out, sizeof(out));
    CHECK(gi_context_default_pipeline_snapshot(context, &out) == GI_CONTEXT_INVALID);
    CHECK(memcmp(&before, &out, sizeof(out)) == 0);
}
int main(void) {
    vgc_handle native = 0, sn = 0;
    vgt_handle ts = 0;
    vgo_handle os = 0;
    void *share = NULL, *other = NULL, *context = NULL;
    unsigned char a[0x268], h[VGC_H_LENGTH], b[VGC_B_LENGTH];
    unsigned char a_before[sizeof(a)], h_before[sizeof(h)], b_before[sizeof(b)];
    unsigned char pd[6][4] = {{0x20, 0x86, 0, 1}, {4, 0x88, 4, 1},
        {0x20, 0x86, 0, 1}, {0, 0, 0, 1}, {0x20, 0x86, 0, 0}, {0x20, 0x86, 0, 1}};
    unsigned char pu[6][4], pu_before[sizeof(pu)];
    void *p[6] = {0}, *program = NULL, *sampler = NULL;
    unsigned char gd = 0x20, gu = 0xa3, sd[0x30] = {0}, su = 1;
    memset(a, 0xa7, sizeof(a)); memset(h, 0x39, sizeof(h)); memset(b, 0, sizeof(b));
    memset(pu, 0xd7, sizeof(pu)); memcpy(pu_before, pu, sizeof(pu));
    vgo_fixture_bgra = 1;
    CHECK(vgc_share_create_bgra(&native) == kIOReturnSuccess);
    /* 使用真实 SDK NotReady；Success 不能冒充该 metadata 设备状态。 */
    CHECK(kIOReturnNotReady != kIOReturnSuccess);
    liveness = kIOReturnSuccess;
    CHECK(gi_register_native_share(&device_token, 73, native) == GI_CONTEXT_BACKEND);
    liveness = kIOReturnNotReady;
    CHECK(gi_register_native_share(&device_token, 73, native) == GI_CONTEXT_OK);
    CHECK(vgc_release(native) == kIOReturnSuccess);
    CHECK(gi_context_set_policy(GI_CONTEXT_METADATA_EXPERIMENT) == GI_CONTEXT_OK);
    CHECK(gi_create_share_group(&device_token, &share, 4) == GI_CONTEXT_OK);
    CHECK(gi_create_share_group(&device_token, &other, 4) == GI_CONTEXT_OK);
    const uint32_t tokens[] = {8, 32, 11, 8, 0};
    const gp_format_cap cap = {GLD_DEVICE_DRIVER_ID, GP_FLAG_OFFSCREEN,
        GP_BUFFER_MONO_SINGLE, GP_COLOR_ARGB8888, 0, 0, 1};
    gp_pixel_list pixels = {0};
    CHECK(gp_choose_normalized(tokens, sizeof(tokens), &cap, 1, NULL, &pixels) == GP_OK);
    CHECK(gi_create_context(share, &context, pixels.head, a, h, b) == GI_CONTEXT_OK);
    CHECK(a[0x65] == 0);
    memcpy(a_before, a, sizeof(a)); memcpy(h_before, h, sizeof(h));
    CHECK(gi_share_native(share, &sn, &ts, &os) == GI_CONTEXT_OK);
    /* FP 先于 VP，且夹入普通 pipeline，身份绝不能依赖创建次序。 */
    CHECK(gi_create_pipeline_program(share, &p[1], pd[1], pu[1]) == GI_CONTEXT_OK);
    CHECK(gi_create_pipeline_program(share, &p[3], pd[3], pu[3]) == GI_CONTEXT_OK);
    CHECK(gi_create_pipeline_program(share, &p[0], pd[0], pu[0]) == GI_CONTEXT_OK);
    CHECK(gi_create_pipeline_program(other, &p[2], pd[2], pu[2]) == GI_CONTEXT_OK);
    CHECK(gi_create_pipeline_program(share, &p[4], pd[4], pu[4]) == GI_CONTEXT_OK);
    CHECK(gi_create_pipeline_program(share, &p[5], pd[5], pu[5]) == GI_CONTEXT_OK);
    CHECK(gi_create_program(share, &program, &gd, &gu) == GI_CONTEXT_OK);
    CHECK(gi_create_sampler(share, &sampler, sd, &su) == GI_CONTEXT_OK);
    reject(NULL); reject(share); reject(context);
    CHECK(gi_context_default_pipeline_snapshot(context, NULL) == GI_CONTEXT_INVALID);
    slot(b, 0x400, 0, p[0]); slot(b, 0x400, 4, p[1]);
    gi_default_pipeline_snapshot first, next;
    CHECK(gi_context_default_pipeline_snapshot(context, &first) == GI_CONTEXT_OK);
    CHECK(first.share == os && first.pipeline[0].handle && first.pipeline[4].handle);
    CHECK(first.pipeline[0].handle != first.pipeline[4].handle);
    CHECK(first.pipeline[0].generation == 0 && first.pipeline[4].generation == 0);
    for (unsigned stage = 0; stage < 5; ++stage) {
        CHECK(first.program[stage].handle == 0 && first.program[stage].generation == 0);
        if (stage != 0 && stage != 4)
            CHECK(first.pipeline[stage].handle == 0 && first.pipeline[stage].generation == 0);
        slot(b, 0x448, stage, program); reject(context); slot(b, 0x448, stage, NULL);
        if (stage != 0 && stage != 4) {
            slot(b, 0x400, stage, p[0]); reject(context); slot(b, 0x400, stage, NULL);
        }
    }
    void *bad[] = {p[2], program, sampler, p[1], &device_token, NULL};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        slot(b, 0x400, 0, bad[i]); reject(context);
    }
    /* 普通 pipeline 和 Create 时无 bit0 的对象不能通过事后伪装成为默认来源。 */
    pd[3][0] = 0x20; pd[3][1] = 0x86; pd[4][3] = 1;
    slot(b, 0x400, 0, p[3]); reject(context);
    slot(b, 0x400, 0, p[4]); reject(context);
    slot(b, 0x400, 0, p[0]);
    for (unsigned byte = 0; byte < 4; ++byte) {
        unsigned char saved = pd[0][byte];
        pd[0][byte] ^= 1; reject(context); pd[0][byte] = saved;
    }
    for (unsigned byte = 0; byte < 4; ++byte) {
        unsigned char saved = pd[1][byte];
        pd[1][byte] ^= 1; reject(context); pd[1][byte] = saved;
    }
    pd[0][2] = 9; reject(context); pd[0][2] = 0;
    pd[0][3] = 0x81;
    CHECK(gi_context_default_pipeline_snapshot(context, &next) == GI_CONTEXT_OK);
    pd[0][3] = 1;
    CHECK(gi_modify_pipeline_program(share, p[0], 1) == GI_CONTEXT_OK);
    CHECK(gi_modify_pipeline_program(share, p[1], 4) == GI_CONTEXT_OK);
    CHECK(gi_context_default_pipeline_snapshot(context, &next) == GI_CONTEXT_OK);
    CHECK(next.pipeline[0].handle == first.pipeline[0].handle && next.pipeline[0].generation == 1);
    CHECK(next.pipeline[4].handle == first.pipeline[4].handle && next.pipeline[4].generation == 1);
    CHECK(gi_modify_pipeline_program(share, p[0], 2) == GI_CONTEXT_BACKEND);
    CHECK(gi_modify_pipeline_program(share, p[0], 0) == GI_CONTEXT_OK);
    CHECK(gi_context_default_pipeline_snapshot(context, &next) == GI_CONTEXT_OK);
    CHECK(next.pipeline[0].generation == 1);
    /* 换绑相同阶段也必须返回新自有身份；detach 后有 owner 仍必须拒绝。 */
    slot(b, 0x400, 0, p[5]);
    CHECK(gi_context_default_pipeline_snapshot(context, &next) == GI_CONTEXT_OK);
    CHECK(next.pipeline[0].handle != first.pipeline[0].handle);
    CHECK(vgo_detach_views(os, next.pipeline[0].handle) == VGO_OK);
    reject(context);
    /* 销毁后不创建新 entry，遵守外部指针无 ABA 契约，验证 stale 注册表拒绝。 */
    slot(b, 0x400, 0, p[0]);
    CHECK(gi_destroy_pipeline_program(share, p[0]) == GI_CONTEXT_OK);
    p[0] = NULL;
    reject(context);
    vgo_state dead;
    CHECK(vgo_state_get(first.pipeline[0].handle, &dead) == VGO_BAD_HANDLE);
    memcpy(b_before, b, sizeof(b)); reject(context);
    CHECK(memcmp(b_before, b, sizeof(b)) == 0);
    CHECK(memcmp(a_before, a, sizeof(a)) == 0 && memcmp(h_before, h, sizeof(h)) == 0);
    CHECK(memcmp(pu_before, pu, sizeof(pu)) == 0 && gd == 0x20 && gu == 0xa3 && su == 1);
    unsigned char render[25 * sizeof(void *)], saved_render[sizeof(render)];
    memset(render, 0x91, sizeof(render)); memcpy(saved_render, render, sizeof(render));
    CHECK(gi_populate_context_dispatch(context, render) == GI_CONTEXT_UNIMPLEMENTED);
    CHECK(memcmp(render, saved_render, sizeof(render)) == 0);
    CHECK(gi_destroy_program(share, program) == GI_CONTEXT_OK);
    CHECK(gi_destroy_sampler(share, sampler) == GI_CONTEXT_OK);
    for (unsigned i = 1; i < 6; ++i)
        CHECK(gi_destroy_pipeline_program(i == 2 ? other : share, p[i]) == GI_CONTEXT_OK);
    CHECK(gi_destroy_context(context) == GI_CONTEXT_OK);
    CHECK(gi_destroy_share_group(other) == GI_CONTEXT_OK);
    CHECK(gi_destroy_share_group(share) == GI_CONTEXT_OK);
    CHECK(gi_unregister_native_share(&device_token) == GI_CONTEXT_OK);
    CHECK(vgt_bgra_drain_releases() == kIOReturnSuccess);
    CHECK(vgo_fixture_owners == 0 && vgo_fixture_images == 0);
    gp_pixel_list_destroy(&pixels);
    CHECK(gi_context_set_policy(GI_CONTEXT_CLOSED) == GI_CONTEXT_OK);
    puts("PASS CPU pipeline gate: NotReady/来源/当前PD/五阶段/五PROGRAM/share/type/stale/detach/Modify/失败输出不变");
    puts("未调用系统 GL/GPU；policy、A、dispatch 与 PU 行为保持不变");
    return 0;
}
