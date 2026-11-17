#include "vgpu_framebuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned checks;
extern unsigned vgf_test_images, vgf_test_owners;
extern int vgf_test_release_fail;
struct texture { uint64_t token; vgc_handle share, image; int rebuild; uint32_t format; };
struct backend { struct texture textures[3]; unsigned refs, ensures; int fail_release; };
static IOReturn hold(void *p) { ++((struct backend *)p)->refs; return kIOReturnSuccess; }
static IOReturn drop(void *p)
{
    struct backend *b = p;
    if (b->fail_release) return kIOReturnNotReady;
    --b->refs;
    return kIOReturnSuccess;
}
static IOReturn resolve(void *p, uint64_t token, vgf_texture *out)
{
    struct backend *b = p;
    for (unsigned i = 0; i < 3; ++i) if (b->textures[i].token == token) {
        out->share = b->textures[i].share;
        out->texture = &b->textures[i];
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
static IOReturn ensure(void *p, const vgf_texture *texture, vgf_backing *out)
{
    struct backend *b = p;
    struct texture *t = texture->texture;
    ++b->ensures;
    if (!t->image || t->rebuild) {
        vgc_handle candidate = 0;
        IOReturn r = vgc_image_create(t->share,
            (vgc_image_desc){256, 256, VGPU_SURFACE_BGRA8, 0}, &candidate);
        if (r) return r;
        if (t->image) CHECK(vgc_release(t->image) == 0);
        t->image = candidate;
        t->rebuild = 0;
    }
    *out = (vgf_backing){t->image, 256, 256, 1, t->format, 0};
    return kIOReturnSuccess;
}
static void put16(unsigned char *p, uint16_t v) { memcpy(p, &v, sizeof(v)); }
static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }
static void setup(unsigned char *q, unsigned char *d, uint64_t token)
{
    memset(q, 0xa5, VGF_FQ_LENGTH);
    memset(d, 0x5a, VGF_FD_LENGTH);
    put16(q, VGF_RENDERBUFFER_TYPE);
    put32(q + 4, 0); put32(q + 8, 0); put32(q + 12, 0);
    put16(q + 0xb2, 256); put16(q + 0xb4, 256); put32(q + 0xc0, 1);
    put64(d, token); d[0x50] = 1; d[0x51] = 0x80;
}
static void unchanged_except_failed(const unsigned char *before, const unsigned char *d)
{
    CHECK(memcmp(before, d, 0x51) == 0);
    CHECK(d[0x51] == (unsigned char)(before[0x51] | 1));
}
int main(void)
{
    vgc_handle share = 0, foreign = 0;
    CHECK(vgc_share_create_bgra(&share) == 0);
    CHECK(vgc_share_create_bgra(&foreign) == 0);
    struct backend b = {0};
    b.textures[0] = (struct texture){UINT64_C(0xfeed0000aabbccdd), share, 0, 0, VGF_RGBA8};
    b.textures[1] = (struct texture){UINT64_C(0xfeed1111aabbccdd), share, 0, 0, VGF_RGBA8};
    b.textures[2] = (struct texture){UINT64_C(0xfeed2222aabbccdd), foreign, 0, 0, VGF_RGBA8};
    vgf_backend ops = {&b, hold, drop, resolve, ensure};
    unsigned char q[VGF_FQ_LENGTH], d[VGF_FD_LENGTH], saved[VGF_FD_LENGTH], qs[VGF_FQ_LENGTH];
    setup(q, d, b.textures[0].token);
    memcpy(qs, q, sizeof(q));
    vgf_handle f = 123;
    CHECK(vgf_create(share, (vgc_view){q, sizeof(q) - 1}, (vgf_mut_view){d, sizeof(d)}, &ops, &f) != 0);
    CHECK(f == 123);
    CHECK(vgf_create(share, (vgc_view){q, sizeof(q)}, (vgf_mut_view){d, sizeof(d) - 1}, &ops, &f) != 0);
    CHECK(vgf_create(share, (vgc_view){q, sizeof(q)}, (vgf_mut_view){d, sizeof(d)}, &ops, (vgf_handle *)(void *)(q + 8)) != 0);
    CHECK(memcmp(q, qs, sizeof(q)) == 0);
    CHECK(vgf_create(share, (vgc_view){q, sizeof(q)}, (vgf_mut_view){d, sizeof(d)}, &ops, &f) == 0);
    CHECK(b.refs == 1);
    vgc_handle target = 777;
    CHECK(vgf_target_get(f, &target) == kIOReturnNotReady && target == 777);
    memcpy(saved, d, sizeof(d));
    CHECK(vgf_load(f) == 0);
    CHECK(memcmp(saved, d, 0x50) == 0 && d[0x50] == 0 && d[0x51] == 0x80);
    CHECK(memcmp(q, qs, sizeof(q)) == 0);
    CHECK(vgf_target_get(f, &target) == 0 && target != 0);
    CHECK(vgf_target_get(f, (vgc_handle *)(void *)(d + 8)) != 0);
    CHECK(memcmp(saved, d, 0x50) == 0);
    vgc_state state;
    CHECK(vgc_framebuffer_state(target, &state) == 0 && !state.dirty && !state.failed);

    /* 双槽满时，第三个候选失败；旧 FBO 仍持 image，FD 不提前提交。 */
    vgf_texture tx = {share, &b.textures[1]};
    vgf_backing bk;
    CHECK(ensure(&b, &tx, &bk) == 0 && vgf_test_images == 2);
    b.textures[0].rebuild = 1;
    d[0x50] = 1; memcpy(saved, d, sizeof(d));
    CHECK(vgf_load(f) == kIOReturnNoResources);
    unchanged_except_failed(saved, d);
    vgc_handle current = 0;
    CHECK(vgf_target_get(f, &current) == 0 && current == target);
    CHECK(vgc_framebuffer_state(target, &state) == 0 && !state.failed);
    CHECK(vgc_release(b.textures[1].image) == 0); b.textures[1].image = 0;
    CHECK(vgf_load(f) == 0 && vgf_test_images == 1);
    CHECK(vgf_target_get(f, &target) == 0);

    unsigned n = b.ensures;
    put64(d, b.textures[2].token); d[0x50] = 1; memcpy(saved, d, sizeof(d));
    CHECK(vgf_load(f) == kIOReturnBadArgument && b.ensures == n);
    unchanged_except_failed(saved, d);
    put64(d, 42); CHECK(vgf_load(f) == kIOReturnBadArgument && b.ensures == n);
    put64(d, b.textures[0].token);
    d[0x50] = 0x81; memcpy(saved, d, sizeof(d));
    CHECK(vgf_load(f) == kIOReturnUnsupported && b.ensures == n);
    unchanged_except_failed(saved, d); d[0x50] = 1;
    for (unsigned offset = 4; offset <= 12; offset += 4) {
        put32(q + offset, 1); CHECK(vgf_load(f) == kIOReturnUnsupported); put32(q + offset, 0);
    }
    put16(q, 3); CHECK(vgf_load(f) == kIOReturnUnsupported); put16(q, VGF_RENDERBUFFER_TYPE);
    put32(q + 0xc0, 3); CHECK(vgf_load(f) == kIOReturnUnsupported); put32(q + 0xc0, 1);
    put16(q + 0xb2, 0); CHECK(vgf_load(f) == kIOReturnUnsupported); put16(q + 0xb2, 256);
    b.textures[0].format = 123; CHECK(vgf_load(f) == kIOReturnUnsupported); b.textures[0].format = VGF_RGBA8;
    CHECK(vgf_target_get(f, &current) == 0 && current == target);

    /* 回调即使错误报告同 share，真实 attach 也拒绝跨 share image。 */
    CHECK(vgc_image_create(foreign, (vgc_image_desc){256, 256, VGPU_SURFACE_BGRA8, 0}, &b.textures[2].image) == 0);
    b.textures[2].share = share; put64(d, b.textures[2].token); memcpy(saved, d, sizeof(d));
    CHECK(vgf_load(f) == kIOReturnBadArgument); unchanged_except_failed(saved, d);
    CHECK(vgf_target_get(f, &current) == 0 && current == target);
    CHECK(vgc_release(b.textures[2].image) == 0); b.textures[2].image = 0;
    put64(d, b.textures[0].token); CHECK(vgf_load(f) == 0);
    CHECK(vgf_target_get(f, &target) == 0);

    /* context 持 FBO 强引用，包装销毁不使已绑定目标悬空。 */
    unsigned char h[VGC_H_LENGTH] = {0}, bindings[VGC_B_LENGTH] = {0};
    vgc_handle context = 0;
    CHECK(vgc_context_create(share, (vgc_view){h, sizeof(h)}, (vgc_view){bindings, sizeof(bindings)}, &context) == 0);
    put64(bindings + 0x4a0, target); put64(bindings + 0x4a8, target);
    uint32_t dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    CHECK(vgc_context_update(context, &dirty) == 0 && !dirty);
    CHECK(vgc_release(b.textures[0].image) == 0); b.textures[0].image = 0;
    b.fail_release = 1;
    CHECK(vgf_destroy(f) == kIOReturnNotReady && b.refs == 1);
    CHECK(vgf_load(f) == kIOReturnBadArgument);
    CHECK(vgf_target_get(f, &current) == kIOReturnBadArgument);
    b.fail_release = 0;
    CHECK(vgf_destroy(f) == 0 && !b.refs);
    CHECK(vgc_framebuffer_state(target, &state) == 0 && vgf_test_images == 1);
    CHECK(vgc_release(context) == 0 && vgf_test_images == 0);

    /* 无 context 持有时，image 最后释放失败保留待回收节点。 */
    setup(q, d, b.textures[0].token);
    CHECK(vgf_create(share, (vgc_view){q, sizeof(q)}, (vgf_mut_view){d, sizeof(d)}, &ops, &f) == 0);
    CHECK(vgf_load(f) == 0);
    CHECK(vgc_release(b.textures[0].image) == 0); b.textures[0].image = 0;
    vgf_test_release_fail = 1;
    CHECK(vgf_destroy(f) == kIOReturnNotReady && vgf_test_images == 1 && b.refs == 1);
    CHECK(vgf_destroy(f) == kIOReturnNotReady && vgf_test_images == 1);
    vgf_test_release_fail = 0;
    CHECK(vgf_destroy(f) == 0 && !vgf_test_images && !b.refs);
    CHECK(vgf_destroy(f) == kIOReturnBadArgument);

    /* 替换已提交，旧资源释放失败只影响回收，不回滚已成功目标。 */
    setup(q, d, b.textures[0].token);
    CHECK(vgf_create(share, (vgc_view){q, sizeof(q)}, (vgf_mut_view){d, sizeof(d)}, &ops, &f) == 0);
    CHECK(vgf_load(f) == 0);
    CHECK(vgf_target_get(f, &target) == 0);
    CHECK(vgc_release(b.textures[0].image) == 0); b.textures[0].image = 0;
    put64(d, b.textures[1].token); d[0x50] = 1;
    vgf_test_release_fail = 1;
    CHECK(vgf_load(f) == 0 && d[0x50] == 0 && d[0x51] == 0x80);
    CHECK(vgf_target_get(f, &current) == 0 && current != target && vgf_test_images == 2);
    d[0x50] = 1; memcpy(saved, d, sizeof(d));
    CHECK(vgf_load(f) == kIOReturnNotReady); unchanged_except_failed(saved, d);
    CHECK(vgf_target_get(f, &target) == 0 && target == current);
    vgf_test_release_fail = 0;
    CHECK(vgf_load(f) == 0 && vgf_test_images == 1);
    CHECK(vgc_release(b.textures[1].image) == 0); b.textures[1].image = 0;

    unsigned char q2[VGF_FQ_LENGTH], d2[VGF_FD_LENGTH];
    setup(q2, d2, b.textures[0].token);
    memcpy(qs, q, sizeof(q)); memcpy(saved, d, sizeof(d));
    CHECK(vgf_create(share, (vgc_view){q2, sizeof(q2)}, (vgf_mut_view){d2, sizeof(d2)}, &ops,
                     (vgf_handle *)(void *)(q + 8)) == kIOReturnBadArgument);
    CHECK(vgf_create(share, (vgc_view){q2, sizeof(q2)}, (vgf_mut_view){d, sizeof(d)}, &ops,
                     &current) == kIOReturnBadArgument);
    CHECK(vgf_create(share, (vgc_view){q, sizeof(q)}, (vgf_mut_view){q + 1, sizeof(d)}, &ops,
                     &current) == kIOReturnBadArgument);
    CHECK(vgf_create(share, (vgc_view){q2, SIZE_MAX}, (vgf_mut_view){d2, sizeof(d2)}, &ops,
                     &current) == kIOReturnBadArgument);
    CHECK(vgf_create(share, (vgc_view){q2, sizeof(q2)}, (vgf_mut_view){d2, SIZE_MAX}, &ops,
                     &current) == kIOReturnBadArgument);
    CHECK(memcmp(q, qs, sizeof(q)) == 0 && memcmp(d, saved, sizeof(d)) == 0);
    CHECK(vgf_create(target, (vgc_view){q2, sizeof(q2)}, (vgf_mut_view){d2, sizeof(d2)}, &ops,
                     &current) == kIOReturnBadArgument);
    CHECK(vgf_destroy(f) == 0 && !vgf_test_images);
    CHECK(vgc_release(share) == 0 && vgc_release(foreign) == 0);
    CHECK(vgc_drain_releases() == 0 && !vgf_test_images && !vgf_test_owners);
    printf("PASS %u checks; actual context + isolated two-slot CPU surface; no GPU\n", checks);
    return 0;
}
