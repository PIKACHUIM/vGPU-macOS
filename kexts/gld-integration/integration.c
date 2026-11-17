#include "integration.h"
#include "context_exports.h"
#include "render_exports.h"
#include "tcl_exports.h"
#include "dispatch_exports.h"
#include "../gld-state/vgpu_triangle_state.h"
#include <IOKit/IOReturn.h>
#include <OpenGL/CGLTypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 只记录入口、阶段和状态码，不读取或打印外部地址。 */
#if defined(GI_CONTROLLED_PUBLIC_EXPERIMENT)
#define GI_TRACE(entry, stage, result) \
    ((void)fprintf(stderr, "[VGPU-GLD-CONTROLLED] %s %s status=%d\n", \
                   (entry), (stage), (int)(result)))
#else
#define GI_TRACE(entry, stage, result) ((void)0)
#endif

/* 构建时只重命名原模块的编译单元；原源码不修改。内部符号不导出。 */
void gi_device_initialize(const uint32_t *, const uint8_t *, uint32_t,
                          uintptr_t, uintptr_t, uint32_t);
void gi_device_terminate(void);
int32_t gi_device_create(void **, uint32_t, void *);
int32_t gi_device_destroy(void *);
int32_t gi_device_populate(void *, void *);
int32_t gi_pixel_choose(void **, const uint32_t *);
int32_t gi_pixel_destroy(void *);

struct pixel_owner { void *head; struct pixel_owner *next; };
static pthread_mutex_t integration_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pixel_owner *owners;
static void *active_device;
static struct gi_status state;
static vgc_handle native_owner;
struct share_owner {
    void *share;
    int render_open, published, closing;
    struct share_owner *next;
};
struct context_owner {
    void *context, *render, *share;
    const unsigned char *h, *b;
    unsigned char *a;
    gi_tcl *tcl;
    unsigned tcl_refs;
    int lease_snapshot;
    gi_default_pipeline_snapshot pipeline;
    gi_render_triangle_target_info target;
    vgpu_triangle_state raster;
    void *draw_binding;
    uint32_t pending_gl_error, draw_state_dirty;
    int32_t update_fault;
    int32_t last_render_failure;
    int published, closing;
    struct context_owner *next;
};
static struct share_owner *share_owners;
static struct context_owner *context_owners;
static int controlled_draw_enabled;

static struct share_owner *find_share(void *share)
{
    for (struct share_owner *s = share_owners; s; s = s->next)
        if (s->share == share && s->published) return s;
    return NULL;
}
static struct context_owner *find_context(void *context)
{
    for (struct context_owner *c = context_owners; c; c = c->next)
        if (c->context == context && c->published) return c;
    return NULL;
}
/* 错误消费与 render 的 submit_failed/资源故障完全独立；仅锁存首错。 */
static void latch_render_error(struct context_owner *c, int32_t result)
{
    if (!c || !result) return;
    c->last_render_failure = result;
    if (!c->pending_gl_error)
        c->pending_gl_error = result == kIOReturnNoMemory ? UINT32_C(0x505) : UINT32_C(0x502);
}

uint32_t gldGetError(void *context)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    uint32_t error = c ? c->pending_gl_error : 0;
    if (c) c->pending_gl_error = 0;
    pthread_mutex_unlock(&integration_lock);
    return error;
}

static int share_ready(void *share)
{
    struct share_owner *s = find_share(share);
    return s && s->render_open && !s->closing;
}

/* TCL 持有复合 owner 的强引用；render/GI/A/H/B 只能在其归零后解除。
 * A/H/B 的外部分配仍遵守 GLD 借用到 DestroyContext 成功的契约。 */
static gi_tcl_result tcl_retain(void *owner)
{
    struct context_owner *c = owner;
    if (!c->render || !c->a || !c->h || !c->b || c->closing || c->tcl_refs)
        return GI_TCL_INVALID;
    ++c->tcl_refs;
    return GI_TCL_OK;
}
static gi_tcl_result tcl_release(void *owner)
{
    struct context_owner *c = owner;
    if (c->tcl_refs != 1) return GI_TCL_INVALID;
    --c->tcl_refs;
    return GI_TCL_OK;
}
static void tcl_error(void *owner, uint32_t error, gi_tcl_detail detail)
{
    struct context_owner *c = owner;
    (void)detail;
    if (!c->pending_gl_error) c->pending_gl_error = error;
    c->last_render_failure = kIOReturnNotReady;
}
static gi_tcl_result tcl_validate(void *owner)
{
    struct context_owner *c = owner;
    vgpu_triangle_state raster;
    gi_default_pipeline_snapshot pipeline;
    gi_render_triangle_target_info target;
    uint16_t tf;
    void *descriptor, *alternate, *binding;
    if (c->update_fault) return GI_TCL_BACKEND_FAULT;
    if (c->closing || !c->context || !c->tcl_refs) return GI_TCL_REJECTED;
    memcpy(&descriptor, c->a + 0x208, sizeof(descriptor));
    memcpy(&alternate, c->a + 0x210, sizeof(alternate));
    memcpy(&tf, c->h + 0x497a, sizeof(tf));
    memcpy(&binding, c->b + 0x4a0, sizeof(binding));
    if (c->a[0x65] != 1 || descriptor != gi_tcl_descriptor(c->tcl) || alternate ||
        c->h[0x4978] || c->h[0x4979] || tf != UINT16_MAX ||
        vgpu_triangle_state_decode((vgc_view){c->h, VGC_H_LENGTH}, &raster) !=
            VGPU_TRIANGLE_STATE_CORE_VERIFIED ||
        gi_context_default_pipeline_snapshot(c->context, &pipeline) != GI_CONTEXT_OK)
        return GI_TCL_REJECTED;
    /* all_state_verified 恒0。其余域由显式受控历史、逐顶点颜色、默认
     * pipeline/TF 和真实 target 分别补齐，不能将 core 成功当作全状态证明。 */
    state.render_status = gi_render_triangle_target(c->render, &target);
    if (state.render_status) return GI_TCL_REJECTED;
    if (target.width != 256 || target.height != 256) return GI_TCL_REJECTED;
    if (c->lease_snapshot) {
        if (memcmp(&pipeline, &c->pipeline, sizeof(pipeline)) ||
            memcmp(raster.modelview, c->raster.modelview, sizeof(raster.modelview)) ||
            memcmp(raster.projection, c->raster.projection, sizeof(raster.projection)) ||
            memcmp(raster.current_rgba, c->raster.current_rgba, sizeof(raster.current_rgba)) ||
            memcmp(raster.viewport, c->raster.viewport, sizeof(raster.viewport)) ||
            memcmp(raster.depth_range, c->raster.depth_range, sizeof(raster.depth_range)) ||
            binding != c->draw_binding ||
            target.image != c->target.image ||
            target.completed_generation != c->target.completed_generation)
            return GI_TCL_REJECTED;
    } else {
        c->pipeline = pipeline; c->target = target; c->raster = raster;
        c->draw_binding = binding; c->lease_snapshot = 1;
    }
    return GI_TCL_OK;
}
static gi_tcl_result tcl_submit(void *owner, const float positions[3][4], uint64_t *generation)
{
    struct context_owner *c = owner;
    state.render_status = gi_render_draw_triangle(c->render, positions, generation);
    latch_render_error(c, state.render_status);
    if (state.render_status) return GI_TCL_BACKEND_FAULT;
    state.render_generation = *generation;
    c->draw_state_dirty = 0;
    return GI_TCL_OK;
}
static void invalidate_context(struct context_owner *c)
{
    gi_tcl_status status;
    if (c && c->tcl && gi_tcl_get_status(c->tcl, &status) == GI_TCL_OK && status.active)
        gi_tcl_invalidate(c->tcl, GI_TCL_DETAIL_INVALIDATED);
}
static void invalidate_share(void *share)
{
    for (struct context_owner *c = context_owners; c; c = c->next)
        if (c->share == share) invalidate_context(c);
}
static int tcl_completion_ready(struct context_owner *c)
{
    gi_tcl_status status;
    if (!c || c->closing || c->update_fault) return 0;
    if (!c->tcl) return 1;
    return gi_tcl_get_status(c->tcl, &status) == GI_TCL_OK &&
        !status.active && !status.pending_rejection && !status.backend_fault && !status.destroying;
}

/* 每次成功释放即清空对应字段，后续失败可从剩余阶段重试。 */
static int32_t close_context(struct context_owner *c)
{
    c->closing = 1;
    if (c->tcl) {
        gi_tcl_result r = gi_tcl_destroy(&c->tcl);
        if (r) return r == GI_TCL_BUSY ? kIOReturnBusy : kIOReturnNotReady;
    }
    if (c->tcl_refs) return kIOReturnBusy;
    if (c->render) {
        IOReturn r = gi_render_context_destroy(c->render);
        if (r) return r;
        c->render = NULL;
        --state.render_contexts;
    }
    if (c->context) {
        if (gi_destroy_context(c->context)) return kIOReturnNotReady;
        c->context = NULL;
    }
    return 0;
}
static int32_t close_share(struct share_owner *s)
{
    if (s->render_open) {
        IOReturn r = gi_render_share_close(s->share);
        if (r) return r;
        s->render_open = 0;
        --state.render_shares;
    }
    s->closing = 1;
    int32_t r = gi_destroy_share_group(s->share);
    return r == GI_CONTEXT_OK ? 0 : r == GI_CONTEXT_BUSY ? kIOReturnBusy : kIOReturnNotReady;
}
static int32_t drain_pending(void)
{
    struct context_owner **c = &context_owners;
    while (*c) {
        if ((*c)->published) { c = &(*c)->next; continue; }
        int32_t r = close_context(*c);
        if (r) return r;
        struct context_owner *old = *c;
        *c = old->next; free(old); --state.pending_cleanup;
    }
    struct share_owner **s = &share_owners;
    while (*s) {
        if ((*s)->published) { s = &(*s)->next; continue; }
        int32_t r = close_share(*s);
        if (r) return r;
        struct share_owner *old = *s;
        *s = old->next; free(old); --state.pending_cleanup; --state.share_groups;
    }
    return 0;
}

void gi_integration_lock(void) { pthread_mutex_lock(&integration_lock); }
void gi_integration_unlock(void) { pthread_mutex_unlock(&integration_lock); }

/* 仅用于已证 create/modify/destroy 的零成功入口。失败是本项目策略，
 * 不是恢复出的完整 Apple errno；不得用于 Load/Update 等布尔入口。 */
static int32_t metadata_result(int32_t result, int32_t invalid)
{
    switch (result) {
    case GI_CONTEXT_OK: return kCGLNoError;
    case GI_CONTEXT_INVALID: return invalid;
    case GI_CONTEXT_BAD_PIXEL: return kCGLBadPixelFormat;
    case GI_CONTEXT_NO_MEMORY: return kCGLBadAlloc;
    case GI_CONTEXT_BACKEND: return kCGLBadState;
    case GI_CONTEXT_BUSY: return kCGLBadState;
    case GI_CONTEXT_GATE_CLOSED: return kCGLBadMatch;
    case GI_CONTEXT_UNIMPLEMENTED: return kCGLBadMatch;
    default: return kCGLBadState;
    }
}

/* 保留每个未成功释放的 owner/注册状态，允许再次关闭或 terminate。 */
static int32_t release_native(void)
{
    int32_t pending = drain_pending();
    if (pending) return pending;
    if (state.share_groups) return kIOReturnBusy;
    if (gi_context_set_policy(GI_CONTEXT_CLOSED) != GI_CONTEXT_OK) return kIOReturnBusy;
    state.metadata_enabled = 0;
    controlled_draw_enabled = 0;
    if (state.native_registered) {
        int32_t result = gi_unregister_native_share(active_device);
        if (result != GI_CONTEXT_OK)
            return result == GI_CONTEXT_BUSY ? kIOReturnBusy : kIOReturnNotReady;
        state.native_registered = 0;
    }
    if (native_owner) {
        int32_t result = vgc_release(native_owner);
        if (result != 0) return result;
        native_owner = 0;
    }
    return vgt_bgra_drain_releases();
}

int32_t vgpuIntegrationEnableMetadata(void *device)
{
    pthread_mutex_lock(&integration_lock);
    int32_t result = kIOReturnBadArgument;
    struct gld_device_status status = {0};
    if (!device || device != active_device) goto done;
    result = vgpuGLDDeviceGetStatus(device, &status);
    if (result != 0) goto done;
    if (!status.connected || status.liveness_status != kIOReturnNotReady || !status.registry_id) {
        result = kIOReturnNotReady; goto done;
    }
    if (state.metadata_enabled) goto done;
    result = release_native();
    if (result != 0) goto done;
    result = vgc_share_create_bgra_registry(status.registry_id, &native_owner);
    if (result != 0) goto done;
    result = gi_register_native_share(device, status.registry_id, native_owner);
    if (result != GI_CONTEXT_OK) {
        result = metadata_result(result, kCGLBadMatch);
        /* 注册失败仍保留创建引用，下一次 enable/destroy/terminate 重试释放。 */
        goto done;
    }
    state.native_registered = 1;
    result = gi_context_set_policy(GI_CONTEXT_METADATA_EXPERIMENT);
    if (result == GI_CONTEXT_OK) state.metadata_enabled = 1;
    result = metadata_result(result, kCGLBadMatch);
done:
    state.lifecycle_status = result;
    pthread_mutex_unlock(&integration_lock);
    return result;
}

int32_t vgpuIntegrationEnableControlledDraw(void *device)
{
    /* 已有入口自行持锁；不得在 integration_lock 内重入。 */
    int32_t result = vgpuIntegrationEnableMetadata(device);
    if (result) return result;
    pthread_mutex_lock(&integration_lock);
    if (!device || device != active_device || !state.metadata_enabled)
        result = kIOReturnNotReady;
    else if (context_owners || share_owners) result = kIOReturnBusy;
    else controlled_draw_enabled = 1;
    state.lifecycle_status = result;
    pthread_mutex_unlock(&integration_lock);
    return result;
}

static void put32(void *output, size_t offset, uint32_t value)
{
    memcpy((unsigned char *)output + offset, &value, sizeof(value));
}

void vgpuIntegrationStatus(struct gi_status *out)
{
    if (out == NULL) return;
    pthread_mutex_lock(&integration_lock);
    *out = state;
    vgpuGLDDeviceGetCounts(&out->backend);
    pthread_mutex_unlock(&integration_lock);
}

const char *gldGetDeviceString(void *device, uint32_t pname)
{
    static const char vendor[] = "vGPU-macOS";
    static const char renderer[] = "vGPU restricted SVGA3D";
    const char *result = NULL;
    struct gld_device_status status = {0};
    pthread_mutex_lock(&integration_lock);
    if (!device || device != active_device) goto done;
    if (vgpuGLDDeviceGetStatus(device, &status) != 0 || !status.connected ||
        status.liveness_status != kIOReturnNotReady || !status.registry_id) goto done;
    /* 只报告身份。NULL 是本项目拒绝策略，不虚构 GL 版本或扩展支持。 */
    switch (pname) {
    case UINT32_C(0x1f00): result = vendor; break;
    case UINT32_C(0x1f01): result = renderer; break;
    default: break;
    }
done:
    pthread_mutex_unlock(&integration_lock);
    return result;
}

int32_t gldGetVersion(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    if (!a || !b || !c || !d) return 0;
    /* loader 协议版本；不是 GL 版本。driver ID 与 device 模块一致。 */
    *a = 4; *b = 1; *c = 0x11;
    *d = GLD_DEVICE_DRIVER_ID & UINT32_C(0xff00);
    return 1;
}

void gldInitializeLibrary(const uint32_t *services, const uint8_t *bytes,
                          uint32_t mask, uintptr_t flush, uintptr_t bind,
                          uint32_t flags)
{
    pthread_mutex_lock(&integration_lock);
    if (active_device || owners) {
        state.lifecycle_status = kIOReturnBusy;
        goto done;
    }
    state.lifecycle_status = gpx_configure(NULL, 0);
    if (state.lifecycle_status != 0) goto done;
    memset(&state, 0, sizeof(state));
    gi_device_initialize(services, bytes, mask, flush, bind, flags);
    vgpuGLDDeviceGetCounts(&state.backend);
    state.lifecycle_status = state.backend.initialize_status;
done:
    pthread_mutex_unlock(&integration_lock);
}

int32_t gldCreateDevice(void **out, uint32_t mask, void *config)
{
    GI_TRACE("gldCreateDevice", "enter", 0);
    if (!out) { GI_TRACE("gldCreateDevice", "out-null", kIOReturnBadArgument); return kIOReturnBadArgument; }
    *out = NULL;
    pthread_mutex_lock(&integration_lock);
    int32_t result = kIOReturnBusy;
    if (active_device || owners) goto done;
    void *device = NULL;
    result = gi_device_create(&device, mask, config);
    if (result != 0) goto done;
    struct gld_device_status status = {0};
    result = vgpuGLDDeviceGetStatus(device, &status);
    if (result != 0) {
        int32_t cleanup = gi_device_destroy(device);
        if (cleanup != 0) state.lifecycle_status = cleanup;
        goto done;
    }
    active_device = device;
    state.devices = 1;
    state.renderer_id = status.renderer_id;
    state.display_mask = status.display_mask;
    *out = device;
done:
    pthread_mutex_unlock(&integration_lock);
#if defined(GI_CONTROLLED_PUBLIC_EXPERIMENT)
    if (!result) {
        int32_t enable = vgpuIntegrationPublishCandidate(*out);
        GI_TRACE("gldCreateDevice", "publish", enable);
        if (!enable) {
            enable = vgpuIntegrationEnableControlledDraw(*out);
            GI_TRACE("gldCreateDevice", "enable", enable);
        }
        if (enable) {
            /* 保留可清理的 device；后续 share/context 沿真实错误通道拒绝。 */
            pthread_mutex_lock(&integration_lock);
            state.lifecycle_status = enable;
            pthread_mutex_unlock(&integration_lock);
        }
    }
#endif
    GI_TRACE("gldCreateDevice", "return", result);
    return result;
}

int32_t vgpuIntegrationPublishCandidate(void *device)
{
    pthread_mutex_lock(&integration_lock);
    int32_t result = kIOReturnBadArgument;
    if (!device || device != active_device) goto done;
    if (owners) { result = kIOReturnBusy; goto done; }
    struct gld_device_status status = {0};
    result = vgpuGLDDeviceGetStatus(device, &status);
    if (result != 0) goto done;
    /* 默认由核对后端证据后的 harness 显式调用；受控公开实验构建
     * 可在 CreateDevice 后自动启用，不代表通用驱动能力。 */
    const gp_format_cap cap = {status.renderer_id, GI_PIXEL_FLAGS,
        GP_BUFFER_MONO_SINGLE, GP_COLOR_ARGB8888, 0, 0, status.display_mask};
    result = gpx_configure(&cap, 1);
    if (result == 0) {
        state.candidate_count = 1;
        state.width = state.height = 256;
        state.samples = 1;
    }
done:
    pthread_mutex_unlock(&integration_lock);
    return result;
}

int32_t gldDestroyDevice(void *device)
{
    pthread_mutex_lock(&integration_lock);
    int32_t result = kIOReturnBadArgument;
    if (!device || device != active_device) goto done;
    if (owners) { result = kIOReturnBusy; goto done; }
    result = release_native();
    if (result != 0) { state.lifecycle_status = result; goto done; }
    result = gpx_configure(NULL, 0);
    if (result != 0) goto done;
    /* 当前 device 模块即使 close 报错也消费句柄；不能保留悬空指针。 */
    result = gi_device_destroy(device);
    active_device = NULL;
    state.devices = state.candidate_count = 0;
    state.renderer_id = state.display_mask = 0;
    state.width = state.height = state.samples = 0;
    state.lifecycle_status = result;
done:
    pthread_mutex_unlock(&integration_lock);
    return result;
}

int32_t gldPopulateRendererInfo(void *device, void *out)
{
    GI_TRACE("gldPopulateRendererInfo", "enter", 0);
    pthread_mutex_lock(&integration_lock);
    int32_t result = kIOReturnBadArgument;
    if (!device || device != active_device || !out) goto done;
    result = gi_device_populate(device, out);
    if (result == 0 && state.candidate_count != 0) {
        /* renderer-properties-research 的已证偏移。未知字段保持原模块的零。
         * 仅受控公开实验增加 accelerated；不置 compliant/core/vertex/fragment/compute。
         * CGL MajorGLVersion 对零 core 位默认返回2：这是系统解释的限制，
         * 不是本实验的 GL2 或通用驱动支持声明。 */
        put32(out, 0x0c, GI_PIXEL_FLAGS);
        put32(out, 0x10, GP_BUFFER_MONO_SINGLE);
        put32(out, 0x14, GP_COLOR_ARGB8888);
        put32(out, 0x1c, 1); /* kCGL0Bit */
        put32(out, 0x20, 1);
    }
done:
    GI_TRACE("gldPopulateRendererInfo", "return", result);
    pthread_mutex_unlock(&integration_lock);
    return result;
}

int32_t gldChoosePixelFormat(void **out, const uint32_t *tokens)
{
    GI_TRACE("gldChoosePixelFormat", "enter", 0);
    if (!out) { GI_TRACE("gldChoosePixelFormat", "out-null", kCGLBadAttribute); return kCGLBadAttribute; }
    *out = NULL;
    pthread_mutex_lock(&integration_lock);
    struct pixel_owner *owner = malloc(sizeof(*owner));
    int32_t result = kCGLBadAlloc;
    if (!owner) goto done;
    result = gi_pixel_choose(out, tokens);
    if (result == 0 && *out != NULL) {
        owner->head = *out;
        owner->next = owners;
        owners = owner;
        ++state.pixel_lists;
    } else free(owner);
done:
    GI_TRACE("gldChoosePixelFormat", "return", result);
    GI_TRACE("gldChoosePixelFormat", "has-pixel", *out != NULL);
    pthread_mutex_unlock(&integration_lock);
    return result;
}

int32_t gldDestroyPixelFormat(void *head)
{
    pthread_mutex_lock(&integration_lock);
    int32_t result = kCGLNoError;
    if (!head) goto done;
    struct pixel_owner **cursor = &owners;
    while (*cursor && (*cursor)->head != head) cursor = &(*cursor)->next;
    if (!*cursor) { result = kCGLBadPixelFormat; goto done; }
    result = gi_pixel_destroy(head);
    if (result == 0) {
        struct pixel_owner *old = *cursor;
        *cursor = old->next;
        free(old);
        --state.pixel_lists;
    }
done:
    pthread_mutex_unlock(&integration_lock);
    return result;
}

void gldTerminateLibrary(void)
{
    pthread_mutex_lock(&integration_lock);
    state.lifecycle_status = release_native();
    if (state.lifecycle_status != 0) goto done;
    while (owners) {
        int32_t result = gi_pixel_destroy(owners->head);
        if (result != 0) { state.lifecycle_status = result; goto done; }
        struct pixel_owner *old = owners;
        owners = old->next;
        free(old);
        --state.pixel_lists;
    }
    state.lifecycle_status = gpx_configure(NULL, 0);
    if (state.lifecycle_status != 0) goto done;
    gi_device_terminate();
    active_device = NULL;
    memset(&state, 0, sizeof(state));
    vgpuGLDDeviceGetCounts(&state.backend);
    state.lifecycle_status = state.backend.cleanup_status;
done:
    pthread_mutex_unlock(&integration_lock);
}

/* 已证 ABI：minimal 第2节、init-required 第5节、default-object 第3节。 */
int32_t gldCreateShareGroup(void *device, void **out, uintptr_t opaque)
{
    GI_TRACE("gldCreateShareGroup", "enter", 0);
    if (!out) { GI_TRACE("gldCreateShareGroup", "out-null", kCGLBadMatch); return kCGLBadMatch; }
    *out = NULL;
    pthread_mutex_lock(&integration_lock);
    int32_t result = kCGLBadState;
    if (drain_pending()) goto done;
    struct share_owner *s = calloc(1, sizeof(*s));
    if (!s) { result = kCGLBadAlloc; goto done; }
    result = metadata_result(gi_create_share_group(device, &s->share, opaque), kCGLBadMatch);
    if (result) { free(s); goto done; }
    ++state.share_groups;
    s->next = share_owners; share_owners = s;
    state.render_status = gi_render_share_open(s->share);
    if (state.render_status) {
        ++state.pending_cleanup;
        state.lifecycle_status = drain_pending();
        result = kCGLBadState;
        goto done;
    }
    s->render_open = s->published = 1;
    ++state.render_shares;
    *out = s->share;
done:
    GI_TRACE("gldCreateShareGroup", "return", result);
    pthread_mutex_unlock(&integration_lock);
    return result;
}
int32_t gldCreateContext(void *share, void **out, void *pixel,
                         void *a, void *h, void *b)
{
    GI_TRACE("gldCreateContext", "enter", 0);
    if (!out) { GI_TRACE("gldCreateContext", "out-null", kCGLBadMatch); return kCGLBadMatch; }
    *out = NULL;
    pthread_mutex_lock(&integration_lock);
    int32_t result = kCGLBadMatch;
    if (!share_ready(share)) goto done;
    if (drain_pending()) { result = kCGLBadState; goto done; }
    struct context_owner *c = calloc(1, sizeof(*c));
    if (!c) { result = kCGLBadAlloc; goto done; }
    c->share = share;
    c->a = a; c->h = h; c->b = b;
    if (controlled_draw_enabled) {
        for (struct context_owner *other = context_owners; other; other = other->next)
            if (other->share == share) { free(c); goto done; }
    }
    /* 先创建 render/TCL，GI 成功写 A 后不再有可失败的创建步骤。 */
    state.render_status = gi_render_context_create(share,
        (vgc_view){h, VGC_H_LENGTH}, (vgc_view){b, VGC_B_LENGTH}, &c->render);
    if (state.render_status) { free(c); goto done; }
    ++state.render_contexts;
    c->next = context_owners; context_owners = c;
    if (controlled_draw_enabled) {
        const gi_tcl_backend backend = {c, tcl_retain, tcl_release, tcl_validate, tcl_submit, tcl_error};
        gi_tcl_result created = gi_tcl_create(&backend, GI_TCL_CONTROLLED_LEGACY, &c->tcl);
        if (created) {
            result = created == GI_TCL_NO_MEMORY ? kCGLBadAlloc : kCGLBadMatch;
            ++state.pending_cleanup;
            state.lifecycle_status = drain_pending();
            goto done;
        }
    }
    result = metadata_result(gi_create_context(share, &c->context, pixel, a, h, b), kCGLBadMatch);
    if (result) {
        ++state.pending_cleanup;
        state.lifecycle_status = drain_pending();
        goto done;
    }
    if (c->tcl) {
        /* 仅显式实验借用 A[0..0x258)，不覆盖 caller 的 pagesize/未知字段。 */
        const uint32_t groups[8] = {0x10, 0x7fef, 0, 0, 0, 0, 0, 0};
        const unsigned char inputs[4] = {1, 2, 4, 0xf8};
        const float point = 1.0f;
        void *descriptor = gi_tcl_descriptor(c->tcl);
        void *alternate = gi_tcl_alternate_descriptor(c->tcl);
        c->a[0x15] = 1; c->a[0x65] = 1;
        for (size_t offset = 0x3c; offset <= 0x48; offset += 4)
            memcpy(c->a + offset, &point, sizeof(point));
        memcpy(c->a + 0x208, &descriptor, sizeof(descriptor));
        memcpy(c->a + 0x210, &alternate, sizeof(alternate));
        memcpy(c->a + 0x230, groups, sizeof(groups));
        memcpy(c->a + 0x250, inputs, sizeof(inputs));
        memcpy(c->a + 0x254, inputs, sizeof(inputs));
    }
    c->published = 1;
    *out = c->context;
done:
    GI_TRACE("gldCreateContext", "return", result);
    pthread_mutex_unlock(&integration_lock);
    return result;
}
int32_t gldDestroyShareGroup(void *share)
{
    pthread_mutex_lock(&integration_lock);
    int32_t result = kCGLBadMatch;
    struct share_owner *s = find_share(share);
    if (!s) goto done;
    if (drain_pending()) { result = kCGLBadState; goto done; }
    for (struct context_owner *c = context_owners; c; c = c->next)
        if (c->share == share) { result = kCGLBadState; goto done; }
    state.render_status = close_share(s);
    if (state.render_status) { result = kCGLBadState; goto done; }
    struct share_owner **cursor = &share_owners;
    while (*cursor != s) cursor = &(*cursor)->next;
    *cursor = s->next; free(s); --state.share_groups;
    result = 0;
done:
    pthread_mutex_unlock(&integration_lock);
    return result;
}
int32_t gldDestroyContext(void *context)
{
    pthread_mutex_lock(&integration_lock);
    int32_t result = kCGLBadContext;
    struct context_owner *c = find_context(context);
    if (!c) goto done;
    state.render_status = close_context(c);
    if (state.render_status) { result = kCGLBadState; goto done; }
    struct context_owner **cursor = &context_owners;
    while (*cursor != c) cursor = &(*cursor)->next;
    *cursor = c->next; free(c);
    result = 0;
done:
    pthread_mutex_unlock(&integration_lock);
    return result;
}
void gldPopulateContextDispatch(void *context, void *storage)
{
    GI_TRACE("gldPopulateContextDispatch", "enter", 0);
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    if (c && c->tcl && storage) {
        gi_dispatch_init(storage);
        state.dispatch_status = 0;
    } else {
        state.dispatch_status = metadata_result(gi_populate_context_dispatch(context, storage), kCGLBadContext);
    }
    GI_TRACE("gldPopulateContextDispatch", "return", state.dispatch_status);
    pthread_mutex_unlock(&integration_lock);
}
uint32_t gldUpdateDispatch(void *context, void *storage, void *dirty)
{
    (void)dirty;
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    if (c && c->tcl && storage) {
        gi_dispatch_init(storage);
        state.dispatch_status = 0;
    } else {
        state.dispatch_status = kCGLBadMatch;
        latch_render_error(c, kIOReturnUnsupported);
    }
    pthread_mutex_unlock(&integration_lock);
    /* 固定布局从 Create 发布；Q 为未知活视图，不清零。5 是策略而非错误。 */
    return 5;
}
static void update_failure(struct context_owner *c, int32_t result)
{
    latch_render_error(c, result);
    if (c && result) {
        c->update_fault = result;
        if (c->tcl) gi_tcl_fault(c->tcl, GI_TCL_DETAIL_STATE);
    }
}
static uint32_t update_framebuffer(void *context, uint32_t dirty, int draw)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    uint32_t allowed = draw ? UINT32_C(0x443c0478) : VGC_READ_DIRTY;
    int32_t result = kIOReturnBadArgument;
    if (c && !c->closing) {
        invalidate_context(c);
        if (dirty & ~allowed) result = kIOReturnUnsupported;
        else {
            uint32_t binding = dirty & (draw ? VGC_DRAW_DIRTY : VGC_READ_DIRTY);
            if (draw) c->draw_state_dirty |= dirty & UINT32_C(0x043c0478);
            result = gi_render_context_update(c->render, &binding);
        }
    }
    state.render_status = result;
    update_failure(c, result);
    pthread_mutex_unlock(&integration_lock);
    return 1;
}
uint32_t gldUpdateDrawFramebuffer(void *context, uint32_t dirty)
{
    return update_framebuffer(context, dirty, 1);
}
uint32_t gldUpdateReadFramebuffer(void *context, uint32_t dirty)
{
    return update_framebuffer(context, dirty, 0);
}
void gldUpdateTransformFeedbackState(void *context, uint32_t argument)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    int32_t result = kIOReturnUnsupported;
    if (c && c->tcl && !c->closing) {
        uint16_t primitive;
        invalidate_context(c);
        memcpy(&primitive, c->h + 0x497a, sizeof(primitive));
        if (!argument && c->a[0x65] == 1 && !c->h[0x4978] && !c->h[0x4979] &&
            primitive == UINT16_MAX) result = 0;
    }
    state.render_status = result;
    update_failure(c, result);
    pthread_mutex_unlock(&integration_lock);
}
void gi_dispatch_reject(void *context, uint32_t slot)
{
    (void)slot;
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    if (c && c->tcl) gi_tcl_invalidate(c->tcl, GI_TCL_DETAIL_DOMAIN);
    latch_render_error(c, kIOReturnUnsupported);
    pthread_mutex_unlock(&integration_lock);
}
void *gi_dispatch_begin(void *context, uint32_t primitive, int32_t *count)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    void *sink = NULL;
    if (c && c->tcl) {
        c->lease_snapshot = 0;
        sink = gi_tcl_begin(c->tcl, primitive, count);
    }
    pthread_mutex_unlock(&integration_lock);
    return sink;
}
void gi_dispatch_end(void *context, uint32_t flags, uint32_t primitive, uint32_t count)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    if (c && c->tcl) {
        gi_tcl_end(c->tcl, flags, primitive, count);
        c->lease_snapshot = 0;
    }
    pthread_mutex_unlock(&integration_lock);
}
void gldUnbindPipelineProgram(void *context, void *pipeline)
{
    pthread_mutex_lock(&integration_lock);
    invalidate_context(find_context(context));
    state.object_status = metadata_result(gi_unbind_pipeline_program(context, pipeline), kCGLBadContext);
    pthread_mutex_unlock(&integration_lock);
}

#define GI_CREATE(export_name, internal_name) \
int32_t export_name(void *share, void **out, void *descriptor, void *view) { \
    GI_TRACE(#export_name, "enter", 0); \
    pthread_mutex_lock(&integration_lock); \
    int32_t result; \
    if (!share_ready(share)) { if (out) *out = NULL; result = kCGLBadValue; } \
    else result = metadata_result(internal_name(share, out, descriptor, view), kCGLBadValue); \
    GI_TRACE(#export_name, "return", result); \
    pthread_mutex_unlock(&integration_lock); \
    return result; \
}
#define GI_DESTROY(export_name, internal_name) \
int32_t export_name(void *share, void *object) { \
    pthread_mutex_lock(&integration_lock); \
    invalidate_share(share); \
    int32_t result = metadata_result(internal_name(share, object), kCGLBadValue); \
    pthread_mutex_unlock(&integration_lock); \
    return result; \
}
#define GI_MODIFY(export_name, internal_name) \
int32_t export_name(void *share, void *object, uint32_t flags) { \
    pthread_mutex_lock(&integration_lock); \
    invalidate_share(share); \
    int32_t result = metadata_result(internal_name(share, object, flags), kCGLBadValue); \
    pthread_mutex_unlock(&integration_lock); \
    return result; \
}
GI_CREATE(gldCreateTexture, gi_create_texture)
GI_CREATE(gldCreateSampler, gi_create_sampler)
GI_CREATE(gldCreatePipelineProgram, gi_create_pipeline_program)
GI_CREATE(gldCreateProgram, gi_create_program)
GI_DESTROY(gldDestroyTexture, gi_destroy_texture)
GI_DESTROY(gldDestroySampler, gi_destroy_sampler)
GI_DESTROY(gldDestroyPipelineProgram, gi_destroy_pipeline_program)
GI_DESTROY(gldDestroyProgram, gi_destroy_program)
GI_MODIFY(gldModifyTexture, gi_modify_texture)
GI_MODIFY(gldModifyPipelineProgram, gi_modify_pipeline_program)
#undef GI_CREATE
#undef GI_DESTROY
#undef GI_MODIFY

int32_t gldCreateFramebuffer(void *share, void **out, void *fq, void *fd)
{
    if (!out) return kCGLBadValue;
    *out = NULL;
    pthread_mutex_lock(&integration_lock);
    state.render_status = share_ready(share) ? gi_render_framebuffer_create(share,
        (vgc_view){fq, VGF_FQ_LENGTH}, (vgf_mut_view){fd, VGF_FD_LENGTH}, out) : kIOReturnBadArgument;
    int32_t result = state.render_status == 0 ? 0 :
        state.render_status == kIOReturnNoMemory ? kCGLBadAlloc : kCGLBadValue;
    pthread_mutex_unlock(&integration_lock);
    return result;
}
int32_t gldDestroyFramebuffer(void *share, void *framebuffer)
{
    pthread_mutex_lock(&integration_lock);
    invalidate_share(share);
    state.render_status = gi_render_framebuffer_destroy(share, framebuffer);
    int32_t result = state.render_status == 0 ? 0 : kCGLBadState;
    pthread_mutex_unlock(&integration_lock);
    return result;
}
int32_t gldLoadFramebuffer(void *share, void *framebuffer)
{
    pthread_mutex_lock(&integration_lock);
    invalidate_share(share);
    state.render_status = share_ready(share) ? gi_render_framebuffer_load(share, framebuffer) : kIOReturnBadArgument;
    int32_t result = state.render_status == 0 ? 1 : 0;
    pthread_mutex_unlock(&integration_lock);
    return result;
}
void gldClearFramebufferData(void *context, uint32_t mask, const void *values)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    invalidate_context(c);
    state.render_status = c && !c->closing && !c->update_fault ? gi_render_clear(c->render, mask,
        (vgc_view){values, 28}, &state.render_generation) : kIOReturnBadArgument;
    latch_render_error(c, state.render_status);
    pthread_mutex_unlock(&integration_lock);
}
void gldFlushContext(void *context)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    state.render_status = tcl_completion_ready(c) ? gi_render_flush(c->render,
        &state.render_generation) : c && !c->closing ? kIOReturnNotReady : kIOReturnBadArgument;
    latch_render_error(c, state.render_status);
    pthread_mutex_unlock(&integration_lock);
}
void gldWaitForContext(void *context)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    state.render_status = tcl_completion_ready(c) ? gi_render_wait(c->render,
        &state.render_generation) : c && !c->closing ? kIOReturnNotReady : kIOReturnBadArgument;
    latch_render_error(c, state.render_status);
    pthread_mutex_unlock(&integration_lock);
}
static int checked_add(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return 0;
    *out = a + b;
    return 1;
}
static int checked_multiply(size_t a, size_t b, size_t *out)
{
    if (b && a > SIZE_MAX / b) return 0;
    *out = a * b;
    return 1;
}
static int32_t read_span(const unsigned char *h, int32_t width, int32_t height,
                         void *pixels, size_t *required)
{
    uint32_t pack[5];
    const size_t offsets[] = {0x4ec8, 0x4ecc, 0x4ed8, 0x4edc, 0x4ee4};
    for (size_t i = 0; i < 5; ++i) memcpy(&pack[i], h + offsets[i], 4);
    if (h[0x4ee8] || h[0x4ee9]) return kIOReturnUnsupported;
    size_t alignment = pack[4], stride = pack[1], row, skip, tail, bytes;
    if (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)
        return kIOReturnBadArgument;
    if (!stride) {
        if (!checked_multiply(pack[0] ? pack[0] : (size_t)width, 4, &row) ||
            !checked_add(row, alignment - 1, &stride)) return kIOReturnBadArgument;
        stride &= ~(alignment - 1);
    }
    if (!checked_multiply(pack[2], stride, &skip) ||
        !checked_multiply(pack[3], 4, &bytes) || !checked_add(skip, bytes, &skip) ||
        !checked_multiply((size_t)height - 1, stride, &tail) ||
        !checked_add(skip, tail, &tail) || !checked_multiply((size_t)width, 4, &bytes) ||
        !checked_add(tail, bytes, required) || !pixels ||
        *required > UINTPTR_MAX - (uintptr_t)pixels) return kIOReturnBadArgument;
    return 0;
}

int32_t gldReadFramebufferData(void *context, int32_t x, int32_t y,
    int32_t width, int32_t height, uint32_t format, uint32_t type,
    void *pixels, uint32_t mode, void *buffer)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    int32_t result = kIOReturnBadArgument;
    size_t required = 0;
    uint64_t generation = 0;
    if (!c || c->closing) goto done;
    if (c->update_fault) { result = kIOReturnNotReady; goto done; }
    if (format != UINT32_C(0x1908) || type != UINT32_C(0x1401) || mode || buffer) {
        result = kIOReturnUnsupported; goto done;
    }
    if (x < 0 || y < 0 || width <= 0 || height <= 0) goto done;
    /* pixels 已由 GLEngine 裁剪偏移；只读取此刻 H pack，不再次裁剪。
     * required 是调用者承诺有效的最大写跨度，不是测得的真实分配容量。 */
    result = read_span(c->h, width, height, pixels, &required);
    if (result) goto done;
    result = gi_render_read_generation(c->render, &generation);
    if (!result) result = gi_render_read(c->render, generation, x, y, width, height,
                                        format, type, pixels, required, mode, buffer);
done:
    state.render_status = result;
    latch_render_error(c, result);
    pthread_mutex_unlock(&integration_lock);
    /* 已证 CPU 消费者忽略 EAX；固定 0 不进入 PBO 的非零所有权分支。 */
    return 0;
}

int32_t vgpuIntegrationRead(void *context, uint64_t generation,
    int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
    uint32_t type, void *pixels, size_t capacity, uint32_t mode, void *buffer)
{
    pthread_mutex_lock(&integration_lock);
    struct context_owner *c = find_context(context);
    state.render_status = c && !c->closing && !c->update_fault ? gi_render_read(c->render, generation,
        x, y, width, height, format, type, pixels, capacity, mode, buffer) : kIOReturnBadArgument;
    int32_t result = state.render_status;
    pthread_mutex_unlock(&integration_lock);
    return result;
}
