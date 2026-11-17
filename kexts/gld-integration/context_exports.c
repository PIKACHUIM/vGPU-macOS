#include "context_exports.h"
#include "../gld-device/gld_device.h"
#include "../gld-pixel/gld_pixel.h"
#include <stdlib.h>
#include <string.h>

/* 外部串行化同时覆盖 vgt BGRA 桥，不另设无法覆盖外部 FBO 调用的局部锁。 */
typedef struct binding binding;
typedef struct entry entry;
enum kind { SHARE, CONTEXT, TEXTURE, SAMPLER, PIPELINE, PROGRAM };
struct binding {
    binding *next;
    void *device;
    uint64_t registry;
    vgc_handle native;
    size_t shares;
};
struct entry {
    entry *next, *share;
    enum kind kind;
    binding *device;
    size_t children;
    vgc_handle native;
    vgo_handle objects;
    unsigned char pipeline_origin[VGO_PIPELINE_DESCRIPTOR_MIN];
    vgt_handle textures;
    void *a, *h, *b;
    unsigned char native_b[VGC_B_LENGTH];
    gi_framebuffer_resolver resolver;
    void *resolver_cookie;
};
static binding *bindings;
static entry *entries;
static enum gi_context_policy policy;
static const char *diagnostic = "实验 gate 默认关闭；完整 A/render/clear/read 集成尚未验收";
static int32_t fail(int32_t code, const char *message) { diagnostic = message; return code; }
const char *gi_context_diagnostic(void) { return diagnostic; }
static entry *find(void *handle, enum kind kind) {
    entry *p;
    for (p = entries; p; p = p->next) if (p == handle && p->kind == kind) return p;
    return NULL;
}
static binding *device_find(void *device) {
    binding *p;
    for (p = bindings; p; p = p->next) if (p->device == device) return p;
    return NULL;
}
static void insert(entry *p, void **out) {
    p->next = entries; entries = p;
    if (p->share) ++p->share->children;
    *out = p;
}
static void remove_entry(entry *p) {
    entry **cursor = &entries;
    while (*cursor != p) cursor = &(*cursor)->next;
    *cursor = p->next;
    if (p->share) --p->share->children;
    free(p);
}
static uint32_t get32(const void *p, size_t offset) {
    uint32_t value; memcpy(&value, (const unsigned char *)p + offset, 4); return value;
}
static void put32(void *p, size_t offset, uint32_t value) {
    memcpy((unsigned char *)p + offset, &value, 4);
}
static int32_t live(binding *b, struct gld_device_status *s) {
    if (vgpuGLDDeviceGetStatus(b->device, s) || !s->connected || s->liveness_status != kIOReturnNotReady ||
        s->registry_id != b->registry)
        return fail(GI_CONTEXT_BACKEND, "显式设备注册已失效或 registry identity 不匹配");
    return GI_CONTEXT_OK;
}
int32_t gi_context_set_policy(enum gi_context_policy value) {
    if (value != GI_CONTEXT_CLOSED && value != GI_CONTEXT_METADATA_EXPERIMENT)
        return GI_CONTEXT_INVALID;
    if (entries) return GI_CONTEXT_BUSY;
    policy = value;
    diagnostic = value ? "仅启用元数据实验；不是完整 GL 初始化或 GPU 验收" : "实验 gate 已关闭";
    return GI_CONTEXT_OK;
}
int32_t gi_register_native_share(void *device, uint64_t registry, vgc_handle native) {
    struct gld_device_status s = {0};
    if (!device || !registry || device_find(device)) return GI_CONTEXT_INVALID;
    if (vgpuGLDDeviceGetStatus(device, &s) || !s.connected || s.liveness_status != kIOReturnNotReady ||
        s.registry_id != registry) return GI_CONTEXT_BACKEND;
    binding *b = calloc(1, sizeof(*b));
    if (!b) return GI_CONTEXT_NO_MEMORY;
    if (vgc_share_retain(native)) { free(b); return GI_CONTEXT_BACKEND; }
    b->device = device; b->registry = registry; b->native = native;
    b->next = bindings; bindings = b;
    return GI_CONTEXT_OK;
}
int32_t gi_unregister_native_share(void *device) {
    binding **cursor = &bindings;
    while (*cursor && (*cursor)->device != device) cursor = &(*cursor)->next;
    if (!*cursor) return GI_CONTEXT_INVALID;
    binding *b = *cursor;
    if (b->shares) return GI_CONTEXT_BUSY;
    if (vgt_bgra_drain_releases() || vgc_release(b->native)) return GI_CONTEXT_BACKEND;
    *cursor = b->next; free(b);
    return GI_CONTEXT_OK;
}
int32_t gi_create_share_group(void *device, void **out, uintptr_t opaque) {
    if (!out) return GI_CONTEXT_INVALID;
    *out = NULL;
    if (!policy) return fail(GI_CONTEXT_GATE_CLOSED, "share 创建被默认实验 gate 拒绝");
    binding *b = device_find(device);
    struct gld_device_status status = {0};
    if (!b || opaque != 4) return GI_CONTEXT_INVALID;
    if (live(b, &status)) return GI_CONTEXT_BACKEND;
    entry *s = calloc(1, sizeof(*s));
    if (!s) return GI_CONTEXT_NO_MEMORY;
    s->kind = SHARE; s->device = b; s->native = b->native;
    if (vgo_share_import_context(s->native, &s->objects) != VGO_OK) {
        free(s); return GI_CONTEXT_BACKEND;
    }
    if (vgt_bgra_share_create(s->native, &s->textures) != VGT_OK) {
        /* binding 仍持 native，故释放 vgo share 不会成为最终 owner 释放。 */
        (void)vgo_release(s->objects); free(s); return GI_CONTEXT_BACKEND;
    }
    ++b->shares; insert(s, out);
    return GI_CONTEXT_OK;
}
int32_t gi_destroy_share_group(void *handle) {
    entry *s = find(handle, SHARE);
    if (!s) return GI_CONTEXT_INVALID;
    if (s->children || s->resolver) return GI_CONTEXT_BUSY;
    if (s->objects) {
        if (vgo_release(s->objects) != VGO_OK) return GI_CONTEXT_BACKEND;
        s->objects = 0;
    }
    if (s->textures) {
        if (vgt_release(s->textures) != VGT_OK) return GI_CONTEXT_BACKEND;
        s->textures = 0;
    }
    if (vgt_bgra_drain_releases()) return GI_CONTEXT_BACKEND;
    --s->device->shares; remove_entry(s);
    return GI_CONTEXT_OK;
}
static int pixel_valid(const void *pf, const struct gld_device_status *s) {
    const unsigned char *p = pf;
    if (get32(p, 8) != s->renderer_id || get32(p, 0xc) != GI_PIXEL_FLAGS ||
        get32(p, 0x10) != GP_BUFFER_MONO_SINGLE || get32(p, 0x14) != GP_COLOR_ARGB8888 ||
        get32(p, 0x18) != UINT32_C(0x80000000) || get32(p, 0x1c) != 1 ||
        get32(p, 0x20) != 1 || get32(p, 0x34) != s->display_mask) return 0;
    /* 当前唯一受支持的实际 gp chooser 组合不含 aux/MSAA/额外模式。 */
    for (size_t i = 0x24; i < 0x34; ++i) if (p[i]) return 0;
    return 1;
}
static void config(void *view) {
    unsigned char *a = view;
    uint16_t zero16 = 0;
    uintptr_t null_descriptor = 0;
    float zero_limit = 0.0f;
    put32(a, 4, 256); put32(a, 8, 256);
    memcpy(a + 0x1a, &zero16, 2);
    a[0x28] = 1; a[0x29] = 0; a[0x2a] = 0; a[0x2b] = 0; a[0x2c] = 0;
    a[0x2e] = 32; a[0x2f] = 0;
    a[0x30] = 8; a[0x31] = 8; a[0x32] = 8; a[0x33] = 8;
    a[0x34] = 0; a[0x35] = 0; a[0x36] = 0; a[0x37] = 0;
    a[0x38] = 0; a[0x39] = 0; a[0x3a] = 0; a[0x3b] = 0;
    /* 明确实验策略：不声明未知能力或 primitive 实现；零值不被称为充分配置。 */
    memcpy(a + 0x40, &zero_limit, 4); memcpy(a + 0x48, &zero_limit, 4);
    a[0x65] = 0; a[0x6e] = 1; a[0xaa] = 0;
    memcpy(a + 0x208, &null_descriptor, 8); memcpy(a + 0x210, &null_descriptor, 8);
    /* 未触碰未知字段、+0x6f、能力字及 caller 的 +0x258 pagesize。 */
}
int32_t gi_create_context(void *share, void **out, void *pixel, void *a, void *h, void *b) {
    if (!out) return GI_CONTEXT_INVALID;
    *out = NULL;
    if (!policy) return GI_CONTEXT_GATE_CLOSED;
    entry *s = find(share, SHARE);
    if (!s || !s->objects || !s->textures || !pixel || !a || !h || !b) return GI_CONTEXT_INVALID;
    struct gld_device_status status = {0};
    if (live(s->device, &status)) return GI_CONTEXT_BACKEND;
    if (!pixel_valid(pixel, &status)) return GI_CONTEXT_BAD_PIXEL;
    entry *c = calloc(1, sizeof(*c));
    if (!c) return GI_CONTEXT_NO_MEMORY;
    c->kind = CONTEXT; c->share = s; c->a = a; c->h = h; c->b = b;
    /* B 是外部指针句柄视图。后端只看到独立 native handle 存储，绝不直接混用。 */
    if (vgc_context_create(s->native, (vgc_view){h, VGC_H_LENGTH},
        (vgc_view){c->native_b, sizeof(c->native_b)}, &c->native)) {
        free(c); return GI_CONTEXT_BACKEND;
    }
    config(a); insert(c, out);
    diagnostic = "context 元数据已创建；render dispatch 未实现，不能宣称系统 GL 可用";
    return GI_CONTEXT_OK;
}
int32_t gi_destroy_context(void *handle) {
    entry *c = find(handle, CONTEXT);
    if (!c) return GI_CONTEXT_INVALID;
    if (vgc_release(c->native)) return GI_CONTEXT_BACKEND;
    remove_entry(c); return GI_CONTEXT_OK;
}
int32_t gi_populate_context_dispatch(void *handle, void *storage) {
    if (!find(handle, CONTEXT) || !storage) return GI_CONTEXT_INVALID;
    return fail(GI_CONTEXT_UNIMPLEMENTED, "25 槽 render ABI/能力分支未闭合；未写 dispatch、未执行 draw");
}
int32_t gi_create_texture_views(void *share, void **out, vgt_views views) {
    if (!out) return GI_CONTEXT_INVALID;
    *out = NULL;
    entry *s = find(share, SHARE);
    if (!s || !s->textures) return GI_CONTEXT_INVALID;
    entry *o = calloc(1, sizeof(*o));
    if (!o) return GI_CONTEXT_NO_MEMORY;
    o->kind = TEXTURE; o->share = s;
    if (vgt_texture_create(s->textures, views, &o->textures) != VGT_OK) {
        free(o); return GI_CONTEXT_BACKEND;
    }
    insert(o, out); return GI_CONTEXT_OK;
}
int32_t gi_create_texture(void *s, void **out, void *td, void *tu) {
    if (!out) return GI_CONTEXT_INVALID;
    *out = NULL;
    if (!td || !tu || !find(s, SHARE)) return GI_CONTEXT_INVALID;
    uint16_t flags;
    memcpy(&flags, td, sizeof(flags));
    /* 已核验的 24G419 x86_64 GLEngine producer：
     * gleBindRenderbuffer 454617 传 0x2002；gleCreateTextureObject
     * 37e0e0..37e0fb 分配 0x3c0+1*1*0x20，TD=object+0x310。
     * gfxInitializeGLTexture 174c53 在 Create 前保存 flags；可借用尾部恰为 0xd0。
     * 默认 kind2 的 0x1002 仍是 metadata；标志本身不是任意指针的容量证明。
     * 仅使用头文件限定的已核验 producer，未知来源必须走有长度入口。 */
    size_t length = flags == UINT16_C(0x2002) ? VGT_TD_RB_LENGTH : VGT_TD_METADATA_LENGTH;
    vgt_views views = {{td, length}, {tu, VGT_TU_LENGTH}, {0}};
    return gi_create_texture_views(s, out, views);
}
static entry *owned(void *share, void *object, enum kind kind) {
    entry *s = find(share, SHARE), *o = find(object, kind);
    return s && o && o->share == s ? o : NULL;
}
int32_t gi_destroy_texture(void *s, void *handle) {
    entry *o = owned(s, handle, TEXTURE);
    if (!o) return GI_CONTEXT_INVALID;
    if (vgt_texture_destroy(o->share->textures, o->textures) != VGT_OK) return GI_CONTEXT_BACKEND;
    remove_entry(o); return GI_CONTEXT_OK;
}
int32_t gi_modify_texture(void *s, void *handle, uint32_t flags) {
    entry *o = owned(s, handle, TEXTURE);
    if (!o) return GI_CONTEXT_INVALID;
    return vgt_texture_modify(o->share->textures, o->textures, flags) == VGT_OK ?
        GI_CONTEXT_OK : GI_CONTEXT_BACKEND;
}
static int32_t metadata(void *share, void **out, void *d, void *u, enum kind kind) {
    if (!out) return GI_CONTEXT_INVALID;
    *out = NULL;
    entry *s = find(share, SHARE);
    if (!s || !s->objects || !d || !u) return GI_CONTEXT_INVALID;
    entry *o = calloc(1, sizeof(*o));
    if (!o) return GI_CONTEXT_NO_MEMORY;
    o->kind = kind; o->share = s;
    vgo_views views = {{d, kind == SAMPLER ? VGO_SAMPLER_DESCRIPTOR_MIN :
        kind == PIPELINE ? VGO_PIPELINE_DESCRIPTOR_MIN : 1},
        {u, kind == PIPELINE ? VGO_PIPELINE_STATE_MIN : 1}, {0}};
    vgo_status result = kind == SAMPLER ? vgo_sampler_create(s->objects, views, &o->objects) :
        kind == PIPELINE ? vgo_pipeline_create(s->objects, views, &o->objects) :
        vgo_program_create(s->objects, views, &o->objects);
    if (result != VGO_OK) { free(o); return GI_CONTEXT_BACKEND; }
    if (kind == PIPELINE) memcpy(o->pipeline_origin, d, sizeof(o->pipeline_origin));
    insert(o, out); return GI_CONTEXT_OK;
}
static int32_t metadata_destroy(void *s, void *handle, enum kind kind) {
    entry *o = owned(s, handle, kind);
    if (!o) return GI_CONTEXT_INVALID;
    vgo_status result = kind == SAMPLER ? vgo_sampler_destroy(o->share->objects, o->objects) :
        kind == PIPELINE ? vgo_pipeline_destroy(o->share->objects, o->objects) :
        vgo_program_destroy(o->share->objects, o->objects);
    if (result != VGO_OK) return GI_CONTEXT_BACKEND;
    remove_entry(o); return GI_CONTEXT_OK;
}
int32_t gi_create_sampler(void *s, void **out, void *d, void *u) { return metadata(s,out,d,u,SAMPLER); }
int32_t gi_create_pipeline_program(void *s, void **out, void *d, void *u) { return metadata(s,out,d,u,PIPELINE); }
int32_t gi_create_program(void *s, void **out, void *d, void *u) { return metadata(s,out,d,u,PROGRAM); }
int32_t gi_destroy_sampler(void *s, void *o) { return metadata_destroy(s,o,SAMPLER); }
int32_t gi_destroy_pipeline_program(void *s, void *o) { return metadata_destroy(s,o,PIPELINE); }
int32_t gi_destroy_program(void *s, void *o) { return metadata_destroy(s,o,PROGRAM); }
int32_t gi_modify_pipeline_program(void *s, void *handle, uint32_t flags) {
    entry *o = owned(s, handle, PIPELINE);
    if (!o) return GI_CONTEXT_INVALID;
    return vgo_pipeline_modify(o->share->objects, o->objects, flags) == VGO_OK ?
        GI_CONTEXT_OK : GI_CONTEXT_BACKEND;
}
int32_t gi_unbind_pipeline_program(void *context, void *pipeline) {
    entry *c = find(context, CONTEXT), *p = find(pipeline, PIPELINE);
    if (!c || !p || c->share != p->share) return GI_CONTEXT_INVALID;
    /* 本层没有 bind/编译入口，也没有 context 对 pipeline 的引用可解除。 */
    return GI_CONTEXT_OK;
}
static int default_prefix(const unsigned char *pd, unsigned stage) {
    uint16_t tag;
    memcpy(&tag, pd, sizeof(tag));
    return (stage == 0 || stage == 4) && pd[2] == stage && (pd[3] & 1) &&
        tag == (stage == 0 ? 0x8620 : 0x8804);
}
int32_t gi_context_default_pipeline_snapshot(void *handle, gi_default_pipeline_snapshot *out) {
    entry *c = find(handle, CONTEXT);
    if (!c || !out || !c->share->objects) return GI_CONTEXT_INVALID;
    void *pipelines[GI_PIPELINE_STAGE_COUNT], *programs[GI_PIPELINE_STAGE_COUNT];
    gi_default_pipeline_snapshot result = {0};
    result.share = c->share->objects;
    /* B 的外部指针必须先经 gi 注册表解析，不能作为 vgo 数值句柄使用。 */
    for (unsigned stage = 0; stage < GI_PIPELINE_STAGE_COUNT; ++stage) {
        memcpy(&pipelines[stage], (const unsigned char *)c->b + 0x400 + 8 * stage, 8);
        memcpy(&programs[stage], (const unsigned char *)c->b + 0x448 + 8 * stage, 8);
    }
    for (unsigned stage = 0; stage < GI_PIPELINE_STAGE_COUNT; ++stage) {
        if (programs[stage]) return GI_CONTEXT_INVALID;
        if (stage != 0 && stage != 4) {
            if (pipelines[stage]) return GI_CONTEXT_INVALID;
            continue;
        }
        entry *p = owned(c->share, pipelines[stage], PIPELINE);
        if (!p || !default_prefix(p->pipeline_origin, stage)) return GI_CONTEXT_INVALID;
        vgo_state state;
        if (vgo_state_get(p->objects, &state) != VGO_OK || state.kind != VGO_PIPELINE ||
            state.share != result.share || !state.attached || !state.descriptor.data ||
            state.descriptor.length < VGO_PIPELINE_DESCRIPTOR_MIN)
            return GI_CONTEXT_INVALID;
        unsigned char pd[VGO_PIPELINE_DESCRIPTOR_MIN];
        memcpy(pd, state.descriptor.data, sizeof(pd));
        if (!default_prefix(pd, stage)) return GI_CONTEXT_INVALID;
        result.pipeline[stage] = (gi_pipeline_slot){p->objects, state.generation};
    }
    *out = result;
    return GI_CONTEXT_OK;
}
int32_t gi_share_native(void *handle, vgc_handle *native, vgt_handle *textures, vgo_handle *objects) {
    entry *s = find(handle, SHARE);
    if (!s || !s->objects || !s->textures || !native || !textures || !objects) return GI_CONTEXT_INVALID;
    *native = s->native; *textures = s->textures; *objects = s->objects;
    return GI_CONTEXT_OK;
}
int32_t gi_texture_native(void *s, void *handle, vgt_handle *native) {
    entry *o = owned(s, handle, TEXTURE);
    if (!o || !native) return GI_CONTEXT_INVALID;
    *native = o->textures; return GI_CONTEXT_OK;
}
int32_t gi_context_native(void *handle, vgc_handle *native, void **a, void **h, void **b) {
    entry *c = find(handle, CONTEXT);
    if (!c || !native || !a || !h || !b) return GI_CONTEXT_INVALID;
    *native = c->native; *a = c->a; *h = c->h; *b = c->b;
    return GI_CONTEXT_OK;
}
int32_t gi_register_framebuffer_resolver(void *handle, gi_framebuffer_resolver resolver, void *cookie) {
    entry *s = find(handle, SHARE);
    if (!s || (!resolver && cookie)) return GI_CONTEXT_INVALID;
    if (resolver && s->resolver) return GI_CONTEXT_BUSY;
    s->resolver = resolver; s->resolver_cookie = cookie;
    return GI_CONTEXT_OK;
}
int32_t gi_resolve_framebuffer(void *handle, void *external, vgc_handle *native) {
    entry *s = find(handle, SHARE);
    if (!s || !native) return GI_CONTEXT_INVALID;
    *native = 0;
    if (!s->resolver) return fail(GI_CONTEXT_UNIMPLEMENTED, "尚未注册 FBO 映射消费者");
    vgc_handle result = 0;
    int32_t status = s->resolver(s->resolver_cookie, s->native, external, &result);
    if (status) return status;
    if (!result) return GI_CONTEXT_INVALID;
    *native = result; return GI_CONTEXT_OK;
}
