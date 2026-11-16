#ifndef VGPU_OBJECTS_H
#define VGPU_OBJECTS_H
#include "../gld-context/vgpu_context.h"

/* 项目自有元数据 API，不是系统 GLD ABI，不实现编译、采样或 GPU 提交。
 * 句柄带类别标签且不复用；每个 API 还验证注册表类别及 share 归属。
 * share/context 包装并持有实际 vgc 对象，禁止将 vgc 数值当作 vgo 句柄。
 * 模块锁串行化；回调不得重入，调用期间禁止 cancellation 及借用内存并发修改。
 * descriptor/state 必须真实有效且互不重叠；不释放借用内存。无 lease 时由调用者
 * 保活到 detach/最后 release；有 lease 时 retain 成功后才发布对象。
 * create 的 out 必须指向真实有效、正确对齐的可写句柄，且不得与完整借用
 * descriptor/state 或 context H/B 视图区间重叠；重叠/区间无效时不写 out。
 * 区间检查仅检查数值边界，不证明地址映射或实际容量；其他 create 失败清零 out。
 */
typedef uint64_t vgo_handle;
typedef enum {
    VGO_OK = 0, VGO_INVALID = -1, VGO_BOUNDS = -2, VGO_BAD_HANDLE = -3,
    VGO_OWNER_MISMATCH = -4, VGO_NO_MEMORY = -5, VGO_OVERFLOW = -6,
    VGO_DETACHED = -7, VGO_UNSUPPORTED = -8, VGO_NO_CONSUMER = -9,
    VGO_CONSUMER_FAILED = -10, VGO_CONTEXT_FAILED = -11
} vgo_status;
typedef enum {
    VGO_SHARE = 1, VGO_CONTEXT = 2, VGO_SAMPLER = 3,
    VGO_PIPELINE = 4, VGO_PROGRAM = 5
} vgo_kind;
typedef struct { const void *data; size_t length; } vgo_view;
typedef struct { void *data; size_t length; } vgo_mut_view;
typedef struct {
    void *context;
    vgo_status (*retain)(void *context);
    void (*release)(void *context);
} vgo_lease;
typedef struct { vgo_view descriptor; vgo_mut_view state; vgo_lease lease; } vgo_views;
#define VGO_SAMPLER_DESCRIPTOR_MIN ((size_t)0x30)
#define VGO_SAMPLER_STATE_MIN ((size_t)1)
/* 只解释 PD 的四字节前缀；其余字节按调用者提供的真实长度借用。
 * 不把生产者的 0x88/0x8c 或 GU 步长误作完整结构 sizeof。 */
#define VGO_PIPELINE_DESCRIPTOR_MIN ((size_t)4)
#define VGO_PIPELINE_STATE_MIN ((size_t)4)
typedef struct {
    vgo_kind kind;
    vgo_handle share, pipeline;
    uint32_t references, attached, consumed;
    uint64_t generation;
    vgo_view descriptor;
    vgo_mut_view state;
} vgo_state;
/* 同步消费完整 SD；成功表示调用方提供的消费者确实完成自己的状态转换。
 * 回调不能保留借用地址或修改 SD/SU；失败必须保持自己的旧状态。
 * 本模块不将该成功解释为 GPU 工作完成；没有消费者时不清 dirty。
 * clean 且已消费时，NULL consumer 仅查询并返回成功；同一成功函数/context
 * 可跳过，任一身份变化必须重新消费（dirty 可为0）。失败保留身份、代次及 dirty。
 * 身份按函数/context 地址比较，调用者不得将地址复用当作新的目标身份。 */
typedef vgo_status (*vgo_sampler_consumer)(void *context, vgo_view descriptor, uint8_t dirty);

vgo_status vgo_share_create(vgo_handle *out);
/* 显式导入已有 vgc SHARE（含 BGRA），成功持有一份真实强引用；不转移调用者引用。
 * 严格拒绝 context/image/framebuffer/失效句柄（BAD_HANDLE）；retain 溢出为 OVERFLOW，
 * 其他底层错误为 CONTEXT_FAILED；失败清零有效 out，不发布半成品。
 * 每次导入生成独立 vgo 所有权域；同一组 objects 应复用一个导入句柄。 */
vgo_status vgo_share_import_context(vgc_handle native_share, vgo_handle *out);
vgo_status vgo_context_create(vgo_handle share, vgc_view h, vgc_view b, vgo_handle *out);
vgo_status vgo_sampler_create(vgo_handle share, vgo_views views, vgo_handle *out);
vgo_status vgo_pipeline_create(vgo_handle share, vgo_views views, vgo_handle *out);
/* GD/GU 仅检查非空有界视图，不读取任何未恢复字段。 */
vgo_status vgo_program_create(vgo_handle share, vgo_views views, vgo_handle *out);
vgo_status vgo_retain(vgo_handle handle);
/* 最后 release/destroy 的底层失败返回 CONTEXT_FAILED，保留一份引用供原句柄重试。
 * 析构可能已部分完成；失败后仅允许 state_get 和 release/destroy 重试，其他操作拒绝。
 * 内部 share 引用同样保留；不得另行消费仅由子对象持有的引用。
 * vgc 自动析构的待释放对象仍遵循 vgc_drain_releases 收尾契约。 */
vgo_status vgo_release(vgo_handle handle);
vgo_status vgo_state_get(vgo_handle handle, vgo_state *out);
vgo_status vgo_detach_views(vgo_handle share, vgo_handle object);
vgo_status vgo_sampler_destroy(vgo_handle share, vgo_handle sampler);
vgo_status vgo_pipeline_destroy(vgo_handle share, vgo_handle pipeline);
vgo_status vgo_program_destroy(vgo_handle share, vgo_handle program);
vgo_status vgo_sampler_consume(vgo_handle share, vgo_handle sampler,
                               vgo_sampler_consumer consumer, void *context);
/* flags 只接受已证 bit0/bit2；只更新自有失效代次，不清 PU。 */
vgo_status vgo_pipeline_modify(vgo_handle share, vgo_handle pipeline, uint32_t flags);
/* 每个 context 一个本模块绑定槽，不冒充完整系统绑定表。绑定持有强引用。
 * unbind 首参只能是 context，destroy 首参只能是 share；不匹配绑定为无操作。
 * destroy 释放 caller 的一份引用，已有 context 引用保持对象存活。 */
vgo_status vgo_pipeline_bind(vgo_handle context, vgo_handle pipeline);
vgo_status vgo_pipeline_unbind(vgo_handle context, vgo_handle pipeline);
#endif
