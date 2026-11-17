#include "context_exports.h"
#include "../gld-device/gld_device.h"
#include "../gld-pixel/gld_pixel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 只在独立 CPU probe 链接，正式实现必须链接真实 gld-device。 */
static unsigned device_token, other_token;
static int device_dead;
int32_t vgpuGLDDeviceGetStatus(void *device, struct gld_device_status *out) {
    if (device != &device_token || !out || device_dead) return -1;
    *out = (struct gld_device_status){.registry_id = 73, .connected = 1, .liveness_status = kIOReturnNotReady,
        .renderer_id = GLD_DEVICE_DRIVER_ID, .display_mask = 1};
    return 0;
}
extern int vgo_fixture_bgra, vgo_fixture_fail_owner_release;
extern unsigned vgo_fixture_owners, vgo_fixture_images;
/* registry 入口由共享 surface_fixture.c 提供；本 probe 使用可信注册路径。 */
static uint32_t u32(const void *p, size_t offset) {
    uint32_t value; memcpy(&value, (const unsigned char *)p + offset, 4); return value;
}
static int32_t reject_fb(void *cookie, vgc_handle share, void *external, vgc_handle *out) {
    assert(cookie && share && external && out);
    return GI_CONTEXT_UNIMPLEMENTED;
}
int main(void) {
    vgc_handle native = 0;
    void *share = NULL, *other_share = NULL, *context = NULL;
    unsigned char a[0x268], h[VGC_H_LENGTH], b[VGC_B_LENGTH];
    unsigned char td[11][VGT_TD_METADATA_LENGTH], tu[11][VGT_TU_LENGTH];
    unsigned char sd[11][0x30], su[11][4], pd[3][4], pu[3][4];
    void *textures[11] = {0}, *samplers[11] = {0}, *pipelines[3] = {0};
    memset(a, 0xa7, sizeof(a)); memset(h, 0x39, sizeof(h)); memset(b, 0x82, sizeof(b));
    memset(td, 0, sizeof(td)); memset(tu, 0, sizeof(tu)); memset(sd, 0, sizeof(sd));
    memset(su, 0x6c, sizeof(su)); memset(pd, 0, sizeof(pd)); memset(pu, 0, sizeof(pu));
    assert(gi_create_share_group(&device_token, &share, 4) == GI_CONTEXT_GATE_CLOSED && !share);
    vgo_fixture_bgra = 1;
    assert(vgc_share_create_bgra(&native) == kIOReturnSuccess);
    assert(gi_register_native_share(&device_token, 74, native) == GI_CONTEXT_BACKEND);
    assert(gi_register_native_share(&other_token, 73, native) == GI_CONTEXT_BACKEND);
    assert(gi_register_native_share(&device_token, 73, native) == GI_CONTEXT_OK);
    assert(vgc_release(native) == kIOReturnSuccess);
    assert(gi_context_set_policy(GI_CONTEXT_METADATA_EXPERIMENT) == GI_CONTEXT_OK);
    assert(gi_create_share_group(&other_token, &share, 4) == GI_CONTEXT_INVALID);
    assert(gi_create_share_group(&device_token, &share, 5) == GI_CONTEXT_INVALID);
    assert(gi_create_share_group(&device_token, &share, 4) == GI_CONTEXT_OK && share);
    assert(gi_create_share_group(&device_token, &other_share, 4) == GI_CONTEXT_OK && other_share != share);
    assert(gi_unregister_native_share(&device_token) == GI_CONTEXT_BUSY);
    assert(gi_context_set_policy(GI_CONTEXT_CLOSED) == GI_CONTEXT_BUSY);
    const uint32_t tokens[] = {8, 32, 11, 8, 0};
    const gp_format_cap cap = {GLD_DEVICE_DRIVER_ID, GP_FLAG_OFFSCREEN,
        GP_BUFFER_MONO_SINGLE, GP_COLOR_ARGB8888, 0, 0, 1};
    gp_pixel_list pixels = {0};
    assert(gp_choose_normalized(tokens, sizeof(tokens), &cap, 1, NULL, &pixels) == GP_OK);
    unsigned char bad_pf[0x38]; memcpy(bad_pf, pixels.head, sizeof(bad_pf));
    bad_pf[0x1c] = 0x80;
    assert(gi_create_context(share, &context, bad_pf, a, h, b) == GI_CONTEXT_BAD_PIXEL && !context);
    assert(a[0x65] == 0xa7 && h[0] == 0x39 && b[0] == 0x82);
    device_dead = 1;
    assert(gi_create_context(share, &context, pixels.head, a, h, b) == GI_CONTEXT_BACKEND);
    device_dead = 0;
    assert(gi_create_context(share, &context, pixels.head, a, h, b) == GI_CONTEXT_OK && context);
    assert(a[0x65] == 0 && a[0x6e] == 1 && a[0x6f] == 0xa7);
    assert(u32(a, 4) == 256 && u32(a, 8) == 256 && a[0x38] == 0 && a[0x39] == 0);
    assert(a[0x258] == 0xa7 && a[0x220] == 0xa7 && h[0] == 0x39 && b[0] == 0x82);
    vgc_handle cn = 0, sn = 0;
    vgt_handle ts = 0;
    vgo_handle os = 0;
    void *av = NULL, *hv = NULL, *bv = NULL;
    assert(gi_context_native(context, &cn, &av, &hv, &bv) == GI_CONTEXT_OK);
    assert(av == a && hv == h && bv == b);
    assert(gi_share_native(share, &sn, &ts, &os) == GI_CONTEXT_OK && sn == native);
    unsigned char render[25 * sizeof(void *)]; memset(render, 0x91, sizeof(render));
    assert(gi_populate_context_dispatch(context, render) == GI_CONTEXT_UNIMPLEMENTED);
    for (size_t i = 0; i < sizeof(render); ++i) assert(render[i] == 0x91);
    for (unsigned i = 0; i < 11; ++i) {
        uint16_t flags = (uint16_t)(0x1000 | i);
        memcpy(td[i], &flags, 2);
        su[i][0] = 1;
        assert(gi_create_texture(share, &textures[i], td[i], tu[i]) == GI_CONTEXT_OK);
        assert(gi_create_sampler(share, &samplers[i], sd[i], su[i]) == GI_CONTEXT_OK);
        assert(textures[i] != samplers[i]);
        assert(gi_destroy_texture(other_share, textures[i]) == GI_CONTEXT_INVALID);
        assert(gi_destroy_sampler(share, textures[i]) == GI_CONTEXT_INVALID);
        vgt_handle t = 0; vgt_texture_state state;
        assert(gi_texture_native(share, textures[i], &t) == GI_CONTEXT_OK);
        assert(vgt_texture_state_get(t, &state) == VGT_OK && state.kind == i && !state.has_backing);
        assert(su[i][0] == 1 && su[i][1] == 0x6c);
        for (unsigned j = 0; j < i; ++j) assert(textures[i] != textures[j] && samplers[i] != samplers[j]);
    }
    for (unsigned i = 0; i < 3; ++i) {
        pd[i][2] = i ? 4 : 0;
        if (i == 2) { pd[i][0] = 4; pd[i][1] = 0x88; pd[i][3] = 1; }
        pu[i][0] = 3;
        assert(gi_create_pipeline_program(share, &pipelines[i], pd[i], pu[i]) == GI_CONTEXT_OK);
        assert(gi_modify_pipeline_program(share, pipelines[i], 5) == GI_CONTEXT_OK && pu[i][0] == 3);
        assert(gi_modify_pipeline_program(share, pipelines[i], 2) == GI_CONTEXT_BACKEND);
        assert(gi_unbind_pipeline_program(context, pipelines[i]) == GI_CONTEXT_OK);
    }
    unsigned char gd = 0x41, gu = 0x42;
    void *program = NULL;
    assert(gi_create_program(share, &program, &gd, &gu) == GI_CONTEXT_OK);
    assert(gi_destroy_program(other_share, program) == GI_CONTEXT_INVALID);
    assert(gi_destroy_program(share, program) == GI_CONTEXT_OK && gd == 0x41 && gu == 0x42);
    vgc_handle fb = 0;
    assert(gi_resolve_framebuffer(share, &device_token, &fb) == GI_CONTEXT_UNIMPLEMENTED && !fb);
    assert(gi_register_framebuffer_resolver(share, reject_fb, &device_token) == GI_CONTEXT_OK);
    assert(gi_resolve_framebuffer(share, &device_token, &fb) == GI_CONTEXT_UNIMPLEMENTED && !fb);
    assert(gi_destroy_share_group(share) == GI_CONTEXT_BUSY);
    assert(gi_register_framebuffer_resolver(share, NULL, NULL) == GI_CONTEXT_OK);
    for (unsigned i = 0; i < 3; ++i) assert(gi_destroy_pipeline_program(share, pipelines[i]) == GI_CONTEXT_OK);
    for (unsigned i = 0; i < 11; ++i) {
        assert(gi_destroy_sampler(share, samplers[i]) == GI_CONTEXT_OK);
        assert(gi_destroy_texture(share, textures[i]) == GI_CONTEXT_OK);
    }
    assert(gi_destroy_context(context) == GI_CONTEXT_OK);
    assert(gi_destroy_share_group(other_share) == GI_CONTEXT_OK);
    assert(gi_destroy_share_group(share) == GI_CONTEXT_OK);
    vgo_fixture_fail_owner_release = 1;
    assert(gi_unregister_native_share(&device_token) == GI_CONTEXT_BACKEND && vgo_fixture_owners == 1);
    vgo_fixture_fail_owner_release = 0;
    assert(gi_unregister_native_share(&device_token) == GI_CONTEXT_OK);
    assert(vgt_bgra_drain_releases() == kIOReturnSuccess);
    assert(vgo_fixture_owners == 0 && vgo_fixture_images == 0);
    gp_pixel_list_destroy(&pixels);
    assert(gi_context_set_policy(GI_CONTEXT_CLOSED) == GI_CONTEXT_OK);
    puts("PASS CPU: gate/pixel/AHB/同一 native share/11 texture/11 sampler/3 pipeline/program/归属/释放失败重试");
    puts("未执行 GPU、系统 GLEngine 或私有函数；render dispatch 保持显式失败");
    return 0;
}
