#include "../gld-texture/vgpu_texture_bgra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "失败 %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define T(x) CHECK((x) == VGT_OK)
#define C(x) CHECK((x) == kIOReturnSuccess)
static const vgt_view none = {NULL, 0};
static void put16(unsigned char *p, size_t off, uint16_t n) { memcpy(p + off, &n, 2); }
static void put32(unsigned char *p, size_t off, uint32_t n) { memcpy(p + off, &n, 4); }
static void put64(unsigned char *p, size_t off, uint64_t n) { memcpy(p + off, &n, 8); }
static void rb(unsigned char *td, unsigned char *tu)
{
    memset(td, 0, VGT_TD_RB_LENGTH);
    memset(tu, 0, VGT_TU_LENGTH);
    put16(td, 0, 0x2002); put16(td, 0x68, 0x8058);
    put16(td, 0x6a, 256); put16(td, 0x6c, 256); put16(td, 0x6e, 1);
    td[0x79] = td[0x7b] = 1;
    put16(td, 0xb4, 256); put16(td, 0xb6, 256); put16(td, 0xb8, 1);
    td[0xbb] = 0x20;
    put16(td, 0xbc, 256); put16(td, 0xbe, 256);
    put16(td, 0xc0, 0x1908); put16(td, 0xc2, 0x1401);
    put16(tu, 8, 1); tu[0x14] = 1;
}
static vgt_handle texture(vgt_handle share, unsigned char *td, unsigned char *tu)
{
    rb(td, tu);
    vgt_views views = {{td, VGT_TD_RB_LENGTH}, {tu, VGT_TU_LENGTH}, {0}};
    vgt_handle t = 0;
    T(vgt_texture_create(share, views, &t));
    return t;
}
static void unchanged(vgt_handle t, const vgt_texture_state *old, const unsigned char *tu,
                      const unsigned char *saved)
{
    vgt_texture_state now;
    T(vgt_texture_state_get(t, &now));
    CHECK(now.generation == old->generation && now.backing.resource == old->backing.resource);
    CHECK(now.has_backing == old->has_backing);
    CHECK(memcmp(tu, saved, VGT_TU_LENGTH) == 0);
}
static void shared_fbo(void)
{
    vgc_handle s = 0, f = 0, c = 0, image1 = 0, image2 = 0, image3 = 0;
    vgt_handle ts = 0;
    unsigned char td[VGT_TD_RB_LENGTH], tu[VGT_TU_LENGTH], saved[VGT_TU_LENGTH];
    unsigned char h[VGC_H_LENGTH] = {0}, b[VGC_B_LENGTH] = {0};
    unsigned char *cpu = calloc(256 * 256, 4);
    CHECK(cpu != NULL);
    C(vgc_share_create_bgra(&s));
    T(vgt_bgra_share_create(s, &ts));
    vgt_handle t = texture(ts, td, tu);
    vgc_handle sentinel = UINT64_MAX;
    CHECK(vgt_bgra_texture_image_get(s, t, &sentinel) == VGT_NO_BACKEND);
    CHECK(sentinel == UINT64_MAX);
    put64(td, 0xc8, (uint64_t)(uintptr_t)cpu);
    vgt_texture_state old;
    T(vgt_texture_state_get(t, &old)); memcpy(saved, tu, sizeof(tu));
    CHECK(vgt_texture_ensure_backing(ts, t, (vgt_view){cpu, 256 * 256 * 4}) == VGT_UNSUPPORTED);
    unchanged(t, &old, tu, saved);
    put64(td, 0xc8, 0);
    T(vgt_texture_ensure_backing(ts, t, none));
    T(vgt_bgra_texture_image_get(s, t, &image1));
    CHECK(vgt_bgra_texture_image_get(UINT64_MAX, t, &sentinel) == VGT_OWNER_MISMATCH);
    CHECK(sentinel == UINT64_MAX);
    C(vgc_framebuffer_create(s, &f));
    C(vgc_framebuffer_attach(f, 0, image1));
    C(vgc_framebuffer_load(f));
    put32(h, 0x4ee4, 4); put64(b, 0x4a0, f); put64(b, 0x4a8, f);
    C(vgc_context_create(s, (vgc_view){h, sizeof(h)}, (vgc_view){b, sizeof(b)}, &c));
    uint32_t dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    C(vgc_context_update(c, &dirty)); CHECK(dirty == 0);
    uint64_t gen = UINT64_MAX;
    CHECK(vgc_flush_completed(c, &gen) == kIOReturnNotReady && gen == UINT64_MAX);
    unsigned char px[4] = {9, 9, 9, 9};
    CHECK(vgc_read(c, 0, (vgpu_surface_rect){0, 0, 1, 1}, px, 4) == kIOReturnNotReady);
    CHECK(px[0] == 9 && px[3] == 9);
    vgc_clear_desc clear = {1, 15, 0, 0, {1, 0, 0, 0.25f}};
    C(vgc_clear(c, &clear, &gen));
    C(vgc_read(c, gen, (vgpu_surface_rect){0, 0, 1, 1}, px, 4));
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 64);
    T(vgt_texture_modify(ts, t, 0));
    T(vgt_texture_ensure_backing(ts, t, none));
    T(vgt_bgra_texture_image_get(s, t, &image2)); CHECK(image2 != image1);
    T(vgt_texture_modify(ts, t, 0));
    T(vgt_texture_state_get(t, &old)); memcpy(saved, tu, sizeof(tu));
    CHECK(vgt_texture_ensure_backing(ts, t, none) == VGT_BACKEND_FAILED);
    unchanged(t, &old, tu, saved);
    /* 两槽满时旧 FBO 仍可读取，失败未破坏旧资源。 */
    C(vgc_read(c, gen, (vgpu_surface_rect){0, 0, 1, 1}, px, 4)); CHECK(px[3] == 64);
    C(vgc_framebuffer_attach(f, 0, image2));
    clear.rgba[3] = 0.5f;
    C(vgc_clear(c, &clear, &gen));
    C(vgc_read(c, gen, (vgpu_surface_rect){0, 0, 1, 1}, px, 4)); CHECK(px[3] == 128);
    T(vgt_texture_ensure_backing(ts, t, none));
    T(vgt_bgra_texture_image_get(s, t, &image3)); CHECK(image3 != image2);
    T(vgt_texture_state_get(t, &old)); CHECK(old.generation == 3);
    T(vgt_texture_load(ts, t, none));
    vgt_texture_state same; T(vgt_texture_state_get(t, &same)); CHECK(same.generation == 3);
    T(vgt_texture_modify(ts, t, 0));
    put64(td, 0xc8, (uint64_t)(uintptr_t)cpu); memcpy(saved, tu, sizeof(tu));
    CHECK(vgt_texture_load(ts, t, (vgt_view){cpu, 256 * 256 * 4}) == VGT_UNSUPPORTED);
    unchanged(t, &old, tu, saved);
    put64(td, 0xc8, 0);
    /* FBO retain 的 image 在 texture/share 引用提前释放后仍有效。 */
    C(vgc_release(s)); T(vgt_release(ts)); T(vgt_release(t));
    C(vgc_read(c, gen, (vgpu_surface_rect){0, 0, 1, 1}, px, 4)); CHECK(px[3] == 128);
    C(vgc_release(c)); C(vgc_release(f)); C(vgt_bgra_drain_releases());
    free(cpu);
}
#ifdef VGT_BGRA_CPU_SIMULATION
extern unsigned sim_owners, sim_images, sim_creates;
extern int sim_fail_create, sim_fail_partial, sim_fail_image_release, sim_fail_owner_release;
static void failures(void)
{
    vgc_handle s = 0, f = 0, image = 0;
    vgt_handle ts = 0;
    unsigned char td[VGT_TD_RB_LENGTH], tu[VGT_TU_LENGTH], saved[VGT_TU_LENGTH];
    C(vgc_share_create_bgra(&s)); T(vgt_bgra_share_create(s, &ts));
    vgt_handle t = texture(ts, td, tu);
    vgt_texture_state old; T(vgt_texture_state_get(t, &old)); memcpy(saved, tu, sizeof(tu));
    unsigned char *cpu = calloc(256 * 256, 4);
    CHECK(cpu != NULL);
    put64(td, 0xc8, (uint64_t)(uintptr_t)cpu);
    unsigned before_creates = sim_creates;
    CHECK(vgt_texture_load(ts, t, (vgt_view){cpu, 256 * 256 * 4}) == VGT_UNSUPPORTED);
    CHECK(sim_creates == before_creates);
    unchanged(t, &old, tu, saved);
    put64(td, 0xc8, 0); free(cpu);
    put16(td, 0x6a, 128); put16(td, 0xb4, 128);
    CHECK(vgt_texture_load(ts, t, none) == VGT_UNSUPPORTED);
    CHECK(sim_creates == before_creates);
    unchanged(t, &old, tu, saved);
    put16(td, 0x6a, 256); put16(td, 0xb4, 256);
    sim_fail_create = 1;
    CHECK(vgt_texture_load(ts, t, none) == VGT_BACKEND_FAILED);
    unchanged(t, &old, tu, saved); CHECK(sim_images == 0);
    sim_fail_create = 0; sim_fail_partial = 1; sim_fail_image_release = 1;
    CHECK(vgt_texture_load(ts, t, none) == VGT_BACKEND_FAILED);
    unchanged(t, &old, tu, saved); CHECK(sim_images == 1);
    CHECK(vgt_bgra_drain_releases() != kIOReturnSuccess);
    sim_fail_partial = 0; sim_fail_image_release = 0;
    C(vgt_bgra_drain_releases()); CHECK(sim_images == 0);
    T(vgt_texture_load(ts, t, none));
    T(vgt_texture_modify(ts, t, 0)); sim_fail_image_release = 1;
    T(vgt_texture_load(ts, t, none)); CHECK(sim_images == 2);
    T(vgt_texture_modify(ts, t, 0));
    T(vgt_texture_state_get(t, &old)); memcpy(saved, tu, sizeof(tu));
    CHECK(vgt_texture_load(ts, t, none) == VGT_BACKEND_FAILED);
    unchanged(t, &old, tu, saved);
    CHECK(vgt_bgra_drain_releases() != kIOReturnSuccess);
    sim_fail_image_release = 0; C(vgt_bgra_drain_releases()); CHECK(sim_images == 1);
    T(vgt_texture_load(ts, t, none)); CHECK(sim_images == 1);
    /* 父 FBO drop 的失败进入 context drain，不再由桥接持有。 */
    T(vgt_bgra_texture_image_get(s, t, &image));
    C(vgc_framebuffer_create(s, &f)); C(vgc_framebuffer_attach(f, 0, image));
    T(vgt_release(t)); sim_fail_image_release = 1; C(vgc_release(f));
    CHECK(vgt_bgra_drain_releases() != kIOReturnSuccess && sim_images == 1);
    sim_fail_image_release = 0; C(vgt_bgra_drain_releases());
    /* 最后 lease release 的 owner 关闭失败仍有桥接节点拥有引用。 */
    C(vgc_release(s)); sim_fail_owner_release = 1; T(vgt_release(ts));
    CHECK(sim_owners == 1 && vgt_bgra_drain_releases() != kIOReturnSuccess);
    sim_fail_owner_release = 0; C(vgt_bgra_drain_releases()); CHECK(sim_owners == 0);
    /* void destroy 失败后 texture/share 都消失，队列必须继续保活 image+owner。 */
    C(vgc_share_create_bgra(&s)); T(vgt_bgra_share_create(s, &ts));
    t = texture(ts, td, tu); T(vgt_texture_load(ts, t, none));
    C(vgc_release(s)); T(vgt_release(ts)); sim_fail_image_release = 1; T(vgt_release(t));
    CHECK(sim_images == 1 && sim_owners == 1);
    CHECK(vgt_bgra_drain_releases() != kIOReturnSuccess);
    sim_fail_image_release = 0; C(vgt_bgra_drain_releases());
    CHECK(sim_images == 0 && sim_owners == 0);
    /* BGRX share 不能因为声明 alpha 能力而获得假 BGRA backing。 */
    C(vgc_share_create(&s)); T(vgt_bgra_share_create(s, &ts));
    t = texture(ts, td, tu); unsigned creates = sim_creates;
    CHECK(vgt_texture_load(ts, t, none) == VGT_UNSUPPORTED);
    CHECK(sim_creates == creates);
    T(vgt_release(t)); T(vgt_release(ts)); C(vgc_release(s)); C(vgt_bgra_drain_releases());
}
#endif
int main(void)
{
    shared_fbo();
#ifdef VGT_BGRA_CPU_SIMULATION
    failures(); CHECK(sim_images == 0 && sim_owners == 0);
    printf("PASS %u checks; CPU surface 模拟，真实 texture/bridge/context；不是 GPU 验收\n", checks);
#else
    printf("PASS 首轮 %u checks；完整释放并 drain 后在同进程重新租用 owner/双槽\n", checks);
    shared_fbo();
    printf("PASS %u checks; texture/bridge/context BGRA 设备探针；不是系统 GLD/FBO ABI 验收\n", checks);
#endif
    return 0;
}
