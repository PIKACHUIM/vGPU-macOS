#ifndef VGPU_FRAMEBUFFER_H
#define VGPU_FRAMEBUFFER_H
#include "../gld-context/vgpu_context.h"

/* 项目自有有界 API，不是无长度的系统 GLD 导出。仅 64 位小端。
 * 所有本模块/回调/texture/context 操作由集成层外部串行化，禁止重入和 cancellation。
 * FQ/FD 借用至 destroy 成功，调用期间不得修改；视图不得互相重叠。
 * 所有输出不得覆盖任一存活对象的借用视图；拒绝时不写输出。
 * FD 中的 64 位值只作为系统不透明 token 交给 resolve，绝不直接转成 vgc/vgt handle。
 */
typedef uint64_t vgf_handle;
typedef struct { void *data; size_t length; } vgf_mut_view;
#define VGF_FQ_LENGTH ((size_t)0xc4)
#define VGF_FD_LENGTH ((size_t)0x52)
#define VGF_RENDERBUFFER_TYPE UINT16_C(0x2000)
#define VGF_RGBA8 UINT32_C(0x8058)

typedef struct {
    vgc_handle share;
    void *texture; /* 仅回调可解释；resolve 不转移引用 */
} vgf_texture;
typedef struct {
    vgc_handle image; /* 已有真实 backing 的借用 vgc image */
    uint32_t width, height, depth, internal_format, samples;
} vgf_backing;
typedef struct {
    void *context;
    IOReturn (*retain)(void *context);
    /* 失败不消费引用；适配层保留对象并重试。 */
    IOReturn (*release)(void *context);
    IOReturn (*resolve)(void *context, uint64_t system_token, vgf_texture *out);
    /* 与 LoadTexture 共用的真实 backing 操作；不得修改 FQ/FD。
     * 失败可以保留 texture 自身 dirty，但不能替换已发布 FBO 的 image。
     * 成功的 image 在本次调用结束前保持有效；适配层自行 retain。
     * 描述必须准确反映 image，samples=0 表示非 MSAA。
     */
    IOReturn (*ensure_backing)(void *context, const vgf_texture *texture,
                              vgf_backing *out);
} vgf_backend;

IOReturn vgf_create(vgc_handle share, vgc_view fq, vgf_mut_view fd,
                    const vgf_backend *backend, vgf_handle *out);
/* 当前仅 color0、RGBA8 renderbuffer，记录+0=0x2000，+4/+8/+c=0。
 * +2/+3 未证实为语义字段，忽略而不要求清零。其他附件/层/格式拒绝。
 * load 先置 failed bit0；成功才清 pending bit0 和 failed bit0。
 * 未知 pending 位保留并拒绝；失败保持旧 target 和 pending。
 * 独立候选 vgc FBO attach/load 全部成功才发布。texture 自身的成功 backing
 * 可缓存，但 FD 附件句柄和 Q 不会被改写。旧 target 的引用延迟回收可重试。
 */
IOReturn vgf_load(vgf_handle framebuffer);
/* 借用已提交 target；需长期使用时调用 vgc_retain，context 绑定自行持强引用。
 * load 失败仍可取得上次成功 target，未成功 load 返回 NotReady。
 */
IOReturn vgf_target_get(vgf_handle framebuffer, vgc_handle *out);
/* 首次 destroy 开始后拒绝 load/get；失败保留 handle 和未释放引用，可重试。
 * 只有成功才销毁包装，绝不释放调用者 FQ/FD。 */
IOReturn vgf_destroy(vgf_handle framebuffer);
#endif
