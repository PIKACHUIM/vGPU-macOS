#include "render_exports.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == 8, "需要 64 位指针 token");
typedef struct render_share render_share;
typedef struct render_fb render_fb;
typedef struct render_context render_context;
struct render_share {
    render_share *next;
    void *external;
    vgc_handle native;
    vgt_handle textures;
    size_t children, backend_refs;
};
struct render_fb {
    render_fb *next;
    render_share *share;
    vgf_handle native;
    vgc_view fq;
    vgf_mut_view fd;
    size_t bindings;
    int destroying, ready;
};
struct render_context {
    render_context *next;
    render_share *share;
    vgc_handle native;
    vgc_view h, b;
    unsigned char native_h[VGC_H_LENGTH], native_b[VGC_B_LENGTH];
    render_fb *draw, *read;
    int destroying, submit_failed;
};
static render_share *shares;
static render_fb *framebuffers;
static render_context *contexts;
static int range_ok(const void *p, size_t n) {
    return p && n <= UINTPTR_MAX - (uintptr_t)p;
}
static int overlaps(const void *p, size_t n, const void *q, size_t m) {
    if (!p || !q) return 0;
    if (!range_ok(p, n) || !range_ok(q, m)) return 1;
    return (uintptr_t)p < (uintptr_t)q + m && (uintptr_t)q < (uintptr_t)p + n;
}
static int output_ok(const void *p, size_t n) {
    if (!range_ok(p, n)) return 0;
    for (render_fb *f = framebuffers; f; f = f->next)
        if (overlaps(p,n,f->fq.data,f->fq.length) || overlaps(p,n,f->fd.data,f->fd.length)) return 0;
    for (render_context *c = contexts; c; c = c->next)
        if (overlaps(p,n,c->h.data,c->h.length) || overlaps(p,n,c->b.data,c->b.length)) return 0;
    return 1;
}
static render_share *share_find(void *p) {
    for (render_share *s = shares; s; s = s->next) if (s->external == p) return s;
    return NULL;
}
static render_fb *fb_find(void *p) {
    for (render_fb *f = framebuffers; f; f = f->next) if (f == p) return f;
    return NULL;
}
static render_context *context_find(void *p) {
    for (render_context *c = contexts; c; c = c->next) if (c == p) return c;
    return NULL;
}
static IOReturn texture_status(vgt_status r) {
    if (r == VGT_OK) return kIOReturnSuccess;
    if (r == VGT_UNSUPPORTED || r == VGT_NO_BACKEND) return kIOReturnUnsupported;
    if (r == VGT_NO_MEMORY) return kIOReturnNoMemory;
    if (r == VGT_BACKEND_FAILED) return kIOReturnNotReady;
    return kIOReturnBadArgument;
}
static IOReturn backend_retain(void *cookie) {
    render_share *s = cookie;
    if (s->backend_refs == SIZE_MAX) return kIOReturnNoResources;
    ++s->backend_refs;
    return kIOReturnSuccess;
}
static IOReturn backend_release(void *cookie) {
    render_share *s = cookie;
    if (!s->backend_refs) return kIOReturnBadArgument;
    --s->backend_refs;
    return kIOReturnSuccess;
}
static IOReturn resolve_texture(void *cookie, uint64_t token, vgf_texture *out) {
    render_share *s = cookie;
    void *external = (void *)(uintptr_t)token;
    vgt_handle texture;
    if (gi_texture_native(s->external, external, &texture)) return kIOReturnBadArgument;
    *out = (vgf_texture){s->native, external};
    return kIOReturnSuccess;
}
static IOReturn ensure_texture(void *cookie, const vgf_texture *input, vgf_backing *out) {
    render_share *s = cookie;
    vgt_handle texture;
    if (input->share != s->native || gi_texture_native(s->external,input->texture,&texture))
        return kIOReturnBadArgument;
    vgt_image_desc desc;
    vgt_status st = vgt_texture_decode_renderbuffer(s->textures,texture,(vgt_view){0},&desc);
    if (st) return texture_status(st);
    if (desc.format != VGT_FORMAT_RGBA8_UNORM || desc.depth != 1 || desc.samples != 1)
        return kIOReturnUnsupported;
    st = vgt_texture_ensure_backing(s->textures,texture,(vgt_view){0});
    if (st) return texture_status(st);
    vgc_handle image;
    st = vgt_bgra_texture_image_get(s->native,texture,&image);
    if (st) return texture_status(st);
    *out = (vgf_backing){image,desc.width,desc.height,desc.depth,VGF_RGBA8,0};
    return kIOReturnSuccess;
}
static int32_t resolve_fb(void *cookie, vgc_handle share, void *token, vgc_handle *out) {
    render_share *s = cookie;
    render_fb *f = fb_find(token);
    if (share != s->native || !f || f->share != s || f->destroying)
        return GI_CONTEXT_INVALID;
    if (!f->ready || ((unsigned char *)f->fd.data)[0x50] || ((unsigned char *)f->fd.data)[0x51])
        return GI_CONTEXT_BACKEND;
    return vgf_target_get(f->native,out) ? GI_CONTEXT_BACKEND : GI_CONTEXT_OK;
}
IOReturn gi_render_share_open(void *external) {
    if (share_find(external)) return kIOReturnExclusiveAccess;
    render_share *s = calloc(1,sizeof(*s));
    if (!s) return kIOReturnNoMemory;
    vgo_handle objects;
    if (gi_share_native(external,&s->native,&s->textures,&objects)) {
        free(s); return kIOReturnBadArgument;
    }
    s->external = external;
    int32_t r = gi_register_framebuffer_resolver(external,resolve_fb,s);
    if (r) { free(s); return r == GI_CONTEXT_BUSY ? kIOReturnExclusiveAccess : kIOReturnBadArgument; }
    s->next = shares; shares = s;
    return kIOReturnSuccess;
}
IOReturn gi_render_share_close(void *external) {
    render_share *s = share_find(external);
    if (!s) return kIOReturnBadArgument;
    if (s->children || s->backend_refs) return kIOReturnExclusiveAccess;
    IOReturn r = vgt_bgra_drain_releases();
    if (r) return r;
    if (gi_register_framebuffer_resolver(external,NULL,NULL)) return kIOReturnBadArgument;
    render_share **p = &shares;
    while (*p != s) p = &(*p)->next;
    *p = s->next; free(s);
    return kIOReturnSuccess;
}
IOReturn gi_render_texture_load(void *external, void *texture) {
    render_share *s = share_find(external);
    vgf_backing backing;
    if (!s) return kIOReturnBadArgument;
    vgf_texture input = {s->native,texture};
    return ensure_texture(s,&input,&backing);
}
IOReturn gi_render_framebuffer_create(void *external, vgc_view fq, vgf_mut_view fd, void **out) {
    render_share *s = share_find(external);
    if (!s || !output_ok(out,sizeof(*out)) || (uintptr_t)out % _Alignof(void *) ||
        !range_ok(fq.data,fq.length) || fq.length < VGF_FQ_LENGTH ||
        !range_ok(fd.data,fd.length) || fd.length < VGF_FD_LENGTH ||
        overlaps(out,sizeof(*out),fq.data,fq.length) || overlaps(out,sizeof(*out),fd.data,fd.length) ||
        !output_ok(fd.data,fd.length)) return kIOReturnBadArgument;
    /* FQ/FD 不能覆盖 context 的借用状态；vgf 另行校验 FQ/FD 的相互别名。 */
    for (render_context *c = contexts; c; c = c->next)
        if (overlaps(fq.data,fq.length,c->h.data,c->h.length) ||
            overlaps(fq.data,fq.length,c->b.data,c->b.length)) return kIOReturnBadArgument;
    if (s->children == SIZE_MAX) return kIOReturnNoResources;
    render_fb *f = calloc(1,sizeof(*f));
    if (!f) return kIOReturnNoMemory;
    vgf_backend backend = {s,backend_retain,backend_release,resolve_texture,ensure_texture};
    IOReturn r = vgf_create(s->native,fq,fd,&backend,&f->native);
    if (r) { free(f); return r; }
    f->share = s; f->fq = fq; f->fd = fd;
    f->next = framebuffers; framebuffers = f; ++s->children; *out = f;
    return kIOReturnSuccess;
}
IOReturn gi_render_framebuffer_load(void *external, void *token) {
    render_fb *f = fb_find(token);
    if (!f || f->share->external != external || f->destroying) return kIOReturnBadArgument;
    f->ready = 0;
    unsigned char *fd = f->fd.data;
    if (fd[0x51] & (unsigned char)~1u) {
        fd[0x51] |= 1;
        return kIOReturnUnsupported;
    }
    IOReturn r = vgf_load(f->native);
    if (!r) f->ready = 1;
    return r;
}
IOReturn gi_render_framebuffer_destroy(void *external, void *token) {
    render_fb *f = fb_find(token);
    if (!f || f->share->external != external) return kIOReturnBadArgument;
    if (f->bindings) return kIOReturnExclusiveAccess;
    f->destroying = 1;
    IOReturn r = vgf_destroy(f->native);
    if (r) return r;
    render_fb **p = &framebuffers;
    while (*p != f) p = &(*p)->next;
    *p = f->next; --f->share->children; free(f);
    return kIOReturnSuccess;
}
IOReturn gi_render_context_create(void *external, vgc_view h, vgc_view b, void **out) {
    render_share *s = share_find(external);
    if (!s || !output_ok(out,sizeof(*out)) || (uintptr_t)out % _Alignof(void *) ||
        !range_ok(h.data,h.length) || h.length < VGC_H_LENGTH ||
        !range_ok(b.data,b.length) || b.length < VGC_B_LENGTH ||
        overlaps(h.data,h.length,b.data,b.length) ||
        overlaps(out,sizeof(*out),h.data,h.length) || overlaps(out,sizeof(*out),b.data,b.length))
        return kIOReturnBadArgument;
    for (render_fb *f = framebuffers; f; f = f->next)
        if (overlaps(h.data,h.length,f->fd.data,f->fd.length) ||
            overlaps(b.data,b.length,f->fd.data,f->fd.length)) return kIOReturnBadArgument;
    if (s->children == SIZE_MAX) return kIOReturnNoResources;
    const uint16_t endian = 1;
    if (*(const unsigned char *)&endian != 1) return kIOReturnUnsupported;
    render_context *c = calloc(1,sizeof(*c));
    if (!c) return kIOReturnNoMemory;
    IOReturn r = vgc_context_create(s->native,(vgc_view){c->native_h,sizeof(c->native_h)},
                                   (vgc_view){c->native_b,sizeof(c->native_b)},&c->native);
    if (r) { free(c); return r; }
    c->share = s; c->h = h; c->b = b;
    c->next = contexts; contexts = c; ++s->children; *out = c;
    return kIOReturnSuccess;
}
IOReturn gi_render_context_destroy(void *token) {
    render_context *c = context_find(token);
    if (!c) return kIOReturnBadArgument;
    c->destroying = 1;
    if (c->native) {
        IOReturn r = vgc_release(c->native);
        if (r) return r;
        c->native = 0;
    }
    IOReturn r = vgt_bgra_drain_releases();
    if (r) return r;
    if (c->draw) --c->draw->bindings;
    if (c->read) --c->read->bindings;
    render_context **p = &contexts;
    while (*p != c) p = &(*p)->next;
    *p = c->next; --c->share->children; free(c);
    return kIOReturnSuccess;
}
static IOReturn binding(render_context *c, size_t offset, render_fb **fb, vgc_handle *native) {
    void *token;
    memcpy(&token,(const unsigned char *)c->b.data + offset,sizeof(token));
    *fb = NULL; *native = 0;
    if (!token) return kIOReturnSuccess;
    render_fb *f = fb_find(token);
    if (!f || f->share != c->share || f->destroying) return kIOReturnBadArgument;
    if (!f->ready) return kIOReturnNotReady;
    vgc_handle resolved = 0, expected = 0;
    if (gi_resolve_framebuffer(c->share->external,token,&resolved) ||
        vgf_target_get(f->native,&expected) || resolved != expected) return kIOReturnNotReady;
    if (f->bindings > SIZE_MAX - 2) return kIOReturnNoResources;
    *fb = f; *native = resolved;
    return kIOReturnSuccess;
}
static IOReturn update(render_context *c, uint32_t *dirty) {
    uint32_t flags = *dirty;
    if (flags & ~(VGC_DRAW_DIRTY | VGC_READ_DIRTY)) return kIOReturnUnsupported;
    render_fb *draw = c->draw, *read = c->read;
    vgc_handle dn = 0, rn = 0;
    IOReturn r;
    /* 先验证两个 token，再对唯一 FBO 刷新一次；同一 FBO 的第二次 load
     * 会替换并释放第一次的 target，不能把那个失效句柄交给 context。 */
    render_fb *refresh[2] = {NULL,NULL};
    const uint32_t bits[2] = {VGC_DRAW_DIRTY,VGC_READ_DIRTY};
    for (size_t i = 0; i < 2; ++i) {
        if (!(flags & bits[i])) continue;
        void *external;
        memcpy(&external,(const unsigned char *)c->b.data + 0x4a0 + i*8,8);
        if (!external) continue;
        refresh[i] = fb_find(external);
        if (!refresh[i] || refresh[i]->share != c->share || refresh[i]->destroying)
            return kIOReturnBadArgument;
        if (!refresh[i]->ready) return kIOReturnNotReady;
    }
    for (size_t i = 0; i < 2; ++i) {
        if (!refresh[i] || (i && refresh[i] == refresh[0])) continue;
        r = gi_render_framebuffer_load(c->share->external,refresh[i]);
        if (r) return r;
    }
    if (flags & VGC_DRAW_DIRTY) { r = binding(c,0x4a0,&draw,&dn); if (r) return r; }
    if (flags & VGC_READ_DIRTY) { r = binding(c,0x4a8,&read,&rn); if (r) return r; }
    unsigned char previous[VGC_B_LENGTH];
    memcpy(previous,c->native_b,sizeof(previous));
    if (flags & VGC_DRAW_DIRTY) memcpy(c->native_b + 0x4a0,&dn,sizeof(dn));
    if (flags & VGC_READ_DIRTY) memcpy(c->native_b + 0x4a8,&rn,sizeof(rn));
    r = vgc_context_update(c->native,&flags);
    if (r) { memcpy(c->native_b,previous,sizeof(previous)); return r; }
    if (*dirty & VGC_DRAW_DIRTY) {
        if (c->draw) --c->draw->bindings;
        c->draw = draw; if (draw) ++draw->bindings;
    }
    if (*dirty & VGC_READ_DIRTY) {
        if (c->read) --c->read->bindings;
        c->read = read; if (read) ++read->bindings;
    }
    *dirty = flags;
    return kIOReturnSuccess;
}
IOReturn gi_render_context_update(void *token, uint32_t *dirty) {
    render_context *c = context_find(token);
    if (!c || c->destroying || !output_ok(dirty,sizeof(*dirty)) || (uintptr_t)dirty % _Alignof(uint32_t))
        return kIOReturnBadArgument;
    return update(c,dirty);
}
IOReturn gi_render_clear(void *token, uint32_t mask, vgc_view values, uint64_t *generation) {
    render_context *c = context_find(token);
    if (!c || c->destroying) return kIOReturnBadArgument;
    if (!output_ok(generation,sizeof(*generation)) || (uintptr_t)generation % _Alignof(uint64_t) ||
        overlaps(generation,sizeof(*generation),values.data,values.length)) {
        c->submit_failed = 1; return kIOReturnBadArgument;
    }
    vgc_clear_desc desc;
    vgpu_clear_state_result decoded = vgpu_clear_state_decode(c->h,values,mask,&desc);
    if (decoded == VGPU_CLEAR_STATE_NOOP) return kIOReturnSuccess;
    c->submit_failed = 1;
    if (decoded != VGPU_CLEAR_STATE_OK)
        return decoded == VGPU_CLEAR_STATE_BAD_ARGUMENT ? kIOReturnBadArgument : kIOReturnUnsupported;
    uint32_t dirty = VGC_DRAW_DIRTY;
    IOReturn r = update(c,&dirty);
    if (r) return r;
    uint64_t completed;
    r = vgc_clear(c->native,&desc,&completed);
    if (!r) { c->submit_failed = 0; *generation = completed; }
    return r;
}
IOReturn gi_render_draw_triangle(void *token, const float vertices[3][4], uint64_t *generation) {
    render_context *c = context_find(token);
    if (!c || c->destroying) return kIOReturnBadArgument;
    c->submit_failed = 1;
    if (!range_ok(vertices,3 * 4 * sizeof(float)) ||
        !output_ok(generation,sizeof(*generation)) || (uintptr_t)generation % _Alignof(uint64_t) ||
        overlaps(generation,sizeof(*generation),vertices,3 * 4 * sizeof(float)))
        return kIOReturnBadArgument;
    float command[3][4];
    memcpy(command,vertices,sizeof(command));
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 4; ++j)
            if (!isfinite(command[i][j])) return kIOReturnBadArgument;
        if (command[i][0] < -1 || command[i][0] > 1 || command[i][1] < -1 || command[i][1] > 1 ||
            command[i][2] < 0 || command[i][2] > 1 || command[i][3] != 1)
            return kIOReturnUnsupported;
    }
    uint32_t dirty = VGC_DRAW_DIRTY;
    IOReturn r = update(c,&dirty);
    if (r) return r;
    uint64_t completed;
    r = vgc_draw_triangle(c->native,(const float (*)[4])command,&completed);
    if (!r) { c->submit_failed = 0; *generation = completed; }
    return r;
}
IOReturn gi_render_triangle_target(void *token, gi_render_triangle_target_info *out) {
    render_context *c = context_find(token);
    if (!c || c->destroying) return kIOReturnBadArgument;
    IOReturn r = kIOReturnBadArgument;
    if (!output_ok(out,sizeof(*out)) || (uintptr_t)out % _Alignof(gi_render_triangle_target_info))
        goto failed;
    uint32_t dirty = VGC_DRAW_DIRTY;
    r = update(c,&dirty);
    if (r) goto failed;
    if (!c->draw) { r = kIOReturnNotReady; goto failed; }
    vgc_handle target;
    r = vgf_target_get(c->draw->native,&target);
    if (r) goto failed;
    vgc_state state;
    r = vgc_framebuffer_state(target,&state);
    if (r) goto failed;
    if (state.dirty || state.failed) { r = kIOReturnNotReady; goto failed; }
    /* load 已验证唯一 color0 及 FQ/实际 backing 一致；同一串行域内提取
     * 该附件的 image 身份，不能将每次 load 新建的 target 当成 image。 */
    void *external;
    memcpy(&external,c->draw->fd.data,sizeof(external));
    vgt_handle texture;
    if (gi_texture_native(c->share->external,external,&texture)) {
        r = kIOReturnBadArgument; goto failed;
    }
    vgt_image_desc desc;
    r = texture_status(vgt_texture_decode_renderbuffer(c->share->textures,texture,(vgt_view){0},&desc));
    if (r) goto failed;
    if (desc.width != 256 || desc.height != 256 || desc.depth != 1 ||
        desc.format != VGT_FORMAT_RGBA8_UNORM || desc.samples != 1) {
        r = kIOReturnUnsupported; goto failed;
    }
    vgc_handle image;
    r = texture_status(vgt_bgra_texture_image_get(c->share->native,texture,&image));
    if (r) goto failed;
    *out = (gi_render_triangle_target_info){desc.width,desc.height,image,state.completed_generation};
    return kIOReturnSuccess;
failed:
    c->submit_failed = 1;
    return r;
}
IOReturn gi_render_read_generation(void *token, uint64_t *generation) {
    render_context *c = context_find(token);
    if (!c || c->destroying || !output_ok(generation,sizeof(*generation)) ||
        (uintptr_t)generation % _Alignof(uint64_t)) return kIOReturnBadArgument;
    uint32_t dirty = VGC_READ_DIRTY;
    IOReturn r = update(c,&dirty);
    if (r) return r;
    if (!c->read) return kIOReturnNotReady;
    vgc_handle target;
    r = vgf_target_get(c->read->native,&target);
    if (r) return r;
    vgc_state state;
    r = vgc_framebuffer_state(target,&state);
    if (r) return r;
    if (state.dirty || state.failed || !state.completed_generation) return kIOReturnNotReady;
    *generation = state.completed_generation;
    return kIOReturnSuccess;
}
IOReturn gi_render_read(void *token, uint64_t generation,
                       int32_t x, int32_t y, int32_t width, int32_t height,
                       uint32_t format, uint32_t type, void *pixels, size_t capacity,
                       uint32_t mode, void *buffer) {
    render_context *c = context_find(token);
    if (!c || c->destroying || !output_ok(pixels,capacity) || x < 0 || y < 0 || width <= 0 || height <= 0)
        return kIOReturnBadArgument;
    if (format != UINT32_C(0x1908) || type != UINT32_C(0x1401) || mode || buffer)
        return kIOReturnUnsupported;
    uint32_t dirty = VGC_READ_DIRTY;
    IOReturn r = update(c,&dirty);
    if (r) return r;
    /* 只复制已证 pack 稀疏区域，不能把未知 H 当成固定结构快照。 */
    const unsigned char *h = c->h.data;
    const size_t offsets[] = {0x4ec8,0x4ecc,0x4ed8,0x4edc,0x4ee4};
    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i)
        memcpy(c->native_h + offsets[i],h + offsets[i],4);
    c->native_h[0x4ee8] = h[0x4ee8]; c->native_h[0x4ee9] = h[0x4ee9];
    return vgc_read(c->native,generation,(vgpu_surface_rect){(uint32_t)x,(uint32_t)y,
                    (uint32_t)width,(uint32_t)height},pixels,capacity);
}
IOReturn gi_render_flush(void *token, uint64_t *generation) {
    render_context *c = context_find(token);
    if (!c || c->destroying || !output_ok(generation,sizeof(*generation)) ||
        (uintptr_t)generation % _Alignof(uint64_t)) return kIOReturnBadArgument;
    if (c->submit_failed) return kIOReturnNotReady;
    return vgc_flush_completed(c->native,generation);
}
IOReturn gi_render_wait(void *context, uint64_t *generation) {
    return gi_render_flush(context,generation);
}
