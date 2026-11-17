#ifndef VGPU_GLD_INTEGRATION_H
#define VGPU_GLD_INTEGRATION_H
#include "../gld-device/gld_device.h"
#include "../gld-pixel/gld_pixel_exports.h"
#define GI_EXPORT __attribute__((visibility("default")))
/* 仅供隔离探针，不是 Apple GLD ABI。固定尺寸不能通过已证 pixel 前缀表达。 */
struct gi_status {
    uint32_t devices, pixel_lists, renderer_id, display_mask;
    uint32_t width, height, samples, candidate_count;
    int32_t lifecycle_status;
    uint32_t context_supported;
    struct gld_device_counts backend;
    uint32_t metadata_enabled, native_registered, share_groups;
    int32_t dispatch_status, object_status;
    uint32_t render_shares, render_contexts, pending_cleanup;
    int32_t render_status;
    uint64_t render_generation;
};
GI_EXPORT void vgpuIntegrationStatus(struct gi_status *out);
/* 默认由隔离 harness 在核对后端映像/启动证据后调用；不是系统 GLD 槽。
 * GI_CONTROLLED_PUBLIC_EXPERIMENT 允许 CreateDevice 自动发布受控候选；
 * 仍由外部核验后端，不将连接成功或 accelerated 标志作为通用驱动能力证明。 */
GI_EXPORT int32_t vgpuIntegrationPublishCandidate(void *device);
GI_EXPORT int32_t gldGetVersion(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
GI_EXPORT int32_t gldCreateShareGroup(void *, void **, uintptr_t);
GI_EXPORT int32_t gldDestroyShareGroup(void *);
GI_EXPORT int32_t gldCreateContext(void *, void **, void *, void *, void *, void *);
GI_EXPORT int32_t gldDestroyContext(void *);
/* 只供隔离实验显式启用，不表示系统 context/render 可用。 */
GI_EXPORT int32_t vgpuIntegrationEnableMetadata(void *device);
/* 外部 harness 核验版本后、任何 share/context 创建前调用。仅受控 legacy、
 * 新建无共享、无用户 program/TF 历史；A 的真实可写借用扩至0x258。
 * 本入口不发布 candidate；普通构建不自动调用。受控公开实验构建在
 * CreateDevice 发布候选后自动调用，不声明通用 OpenGL 支持。 */
GI_EXPORT int32_t vgpuIntegrationEnableControlledDraw(void *device);
GI_EXPORT uint32_t gldUpdateDispatch(void *context, void *storage, void *dirty);
/* 已证按值 dirty；返回1是已忽略/非2的载体，错误仍由自有状态锁存。 */
GI_EXPORT uint32_t gldUpdateDrawFramebuffer(void *context, uint32_t dirty);
GI_EXPORT uint32_t gldUpdateReadFramebuffer(void *context, uint32_t dirty);
GI_EXPORT void gldUpdateTransformFeedbackState(void *context, uint32_t argument);
/* render 适配器必须与本层使用同一把锁，覆盖 gi_* 与借用视图访问。 */
void gi_integration_lock(void);
void gi_integration_unlock(void);
/* Populate/Unbind 返回类型未恢复，采用 void 适配；状态由专用诊断读取。 */
GI_EXPORT void gldPopulateContextDispatch(void *, void *);
GI_EXPORT void gldUnbindPipelineProgram(void *, void *);
GI_EXPORT int32_t gldCreateTexture(void *, void **, void *, void *);
GI_EXPORT int32_t gldModifyTexture(void *, void *, uint32_t);
GI_EXPORT int32_t gldDestroyTexture(void *, void *);
GI_EXPORT int32_t gldCreateSampler(void *, void **, void *, void *);
GI_EXPORT int32_t gldDestroySampler(void *, void *);
GI_EXPORT int32_t gldCreatePipelineProgram(void *, void **, void *, void *);
GI_EXPORT int32_t gldModifyPipelineProgram(void *, void *, uint32_t);
GI_EXPORT int32_t gldDestroyPipelineProgram(void *, void *);
GI_EXPORT int32_t gldCreateProgram(void *, void **, void *, void *);
GI_EXPORT int32_t gldDestroyProgram(void *, void *);
GI_EXPORT int32_t gldCreateFramebuffer(void *, void **, void *, void *);
GI_EXPORT int32_t gldDestroyFramebuffer(void *, void *);
GI_EXPORT int32_t gldLoadFramebuffer(void *, void *);
/* 已证参数形状，返回协议未闭合；使用 void，诊断读 render_status。
 * 失败进入每 context 的 GL 错误槽；仍不声明完整系统 GLD 支持。 */
GI_EXPORT void gldClearFramebufferData(void *, uint32_t, const void *);
GI_EXPORT void gldFlushContext(void *);
GI_EXPORT void gldWaitForContext(void *);
/* 系统 CPU read 的十参载体；仅 mode=0、无 PBO、RGBA8。
 * 调用者保证 pixels 覆盖当前 H pack 所需写跨度；无法从裸指针证明分配容量。
 * CPU 分支忽略返回值，固定返回0；错误由每 context 的 GetError 消费。 */
GI_EXPORT int32_t gldReadFramebufferData(void *, int32_t, int32_t, int32_t, int32_t,
                                        uint32_t, uint32_t, void *, uint32_t, void *);
GI_EXPORT uint32_t gldGetError(void *);
/* 显式容量/代次的项目接口，返回 IOReturn，不消费系统 GL 错误槽。 */
GI_EXPORT int32_t vgpuIntegrationRead(void *, uint64_t, int32_t, int32_t,
                                    int32_t, int32_t, uint32_t, uint32_t,
                                    void *, size_t, uint32_t, void *);
#endif
