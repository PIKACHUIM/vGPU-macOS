#ifndef VGPU_GI_CONTEXT_EXPORTS_H
#define VGPU_GI_CONTEXT_EXPORTS_H
#include "../gld-context/vgpu_context.h"
#include "../gld-objects/vgpu_objects.h"
#include "../gld-texture/vgpu_texture_bgra.h"
#include "../gld-pixel/gld_pixel.h"

/* cap、renderer 与 context PF 必须使用同一精确标志值。
 * accelerated 仅用于受控公开实验，不声明 COMPLIANT 或 core 支持。 */
#if defined(GI_CONTROLLED_PUBLIC_EXPERIMENT)
#define GI_PIXEL_FLAGS (GP_FLAG_OFFSCREEN | GP_FLAG_ACCELERATED)
#else
#define GI_PIXEL_FLAGS GP_FLAG_OFFSCREEN
#endif

/* 项目内部适配，不导出 gld*，不构成完整系统 GLD 支持。
 * 所有调用及借用视图访问须由集成层统一串行化，禁止 cancellation/回调重入。
 * 指针句柄只在本注册表查找，不解引用外来句柄；销毁后禁止复用（无 ABA 保证）。
 * out 必须独占、对齐且不与任何存活借用视图重叠。所有输入内存须真实有效。
 * 无长度的四/六参入口仅适用已核验消费者：PF>=0x38，A>=0x218，
 * H>=VGC_H_LENGTH，B>=VGC_B_LENGTH；这不是未知结构的 sizeof 声明。
 * 无长度 texture Create 仅适用已核验的 macOS 15.7.3/24G419 x86_64
 * GLEngine/libGFXShared producer（UUID/调用链见 texture_system_views_probe.py）。
 * Create 时 TD.flags 精确为 0x2002 的 renderbuffer 借用 VGT_TD_RB_LENGTH；
 * 其真实分配 0x3e0、TD=object+0x310，包含 level0 到 +0xcf。
 * 其余 TD（包括全部 11 个默认对象的 0x1000|kind）仍仅借用 0x80。
 * flags 不是任意内存容量证明；其他版本/来源必须使用有长度 views 入口。
 * SD>=0x30、SU>=1、PD>=4、PU>=4；GD/GU 只借用非空地址的1字节，不读字段。
 * 调用者负责保活 device、A/H/B、descriptor/state，直到相关句柄成功销毁。
 */
enum gi_context_policy { GI_CONTEXT_CLOSED = 0, GI_CONTEXT_METADATA_EXPERIMENT = 1 };
enum gi_context_result {
    GI_CONTEXT_OK = 0, GI_CONTEXT_INVALID = -2001, GI_CONTEXT_GATE_CLOSED = -2002,
    GI_CONTEXT_BUSY = -2003, GI_CONTEXT_BACKEND = -2004, GI_CONTEXT_NO_MEMORY = -2005,
    GI_CONTEXT_BAD_PIXEL = -2006, GI_CONTEXT_UNIMPLEMENTED = -2007
};
/* 自有错误码不能直接当系统 GLD errno；最终导出层需显式映射。 */
int32_t gi_context_set_policy(enum gi_context_policy policy);
const char *gi_context_diagnostic(void);
/* 临时显式导入接口：调用方必须已证明 native_share 属于 registry_id 且为 BGRA share。
 * 本层比对实际 device 状态/registry_id 并严格 retain SHARE；现有 vgc API 尚不能
 * 反查 native 的 registry identity，因此这个绑定是可信集成者的声明，不是自动探测。
 * 绝不打开首个匹配服务。成功持有引用；失败不消费调用者引用。 */
int32_t gi_register_native_share(void *device, uint64_t registry_id, vgc_handle native_share);
int32_t gi_unregister_native_share(void *device);
int32_t gi_create_share_group(void *device, void **out, uintptr_t opaque);
int32_t gi_destroy_share_group(void *share);
int32_t gi_create_context(void *share, void **out, void *pixel, void *a, void *h, void *b);
int32_t gi_destroy_context(void *context);
/* 失败不改 render 存储；未恢复完整表时绝不填成功桩或任意函数。 */
int32_t gi_populate_context_dispatch(void *context, void *render_storage);
int32_t gi_create_texture(void *share, void **out, void *td, void *tu);
int32_t gi_destroy_texture(void *share, void *texture);
int32_t gi_modify_texture(void *share, void *texture, uint32_t flags);
/* 未知 producer 必须在 Create 时提供真实容量/lease，不推断 sizeof。
 * 长度在创建时固定，Modify 不自动扩读；不替换 native handle 或已持有的引用。
 * 已核验 renderbuffer 在 Create 前已有 kind2/0x2002，不需要升级契约。
 * 调用方若只持有短 metadata，事后改 flags 不会授权扩读或 materialization。 */
int32_t gi_create_texture_views(void *share, void **out, vgt_views views);
int32_t gi_create_sampler(void *share, void **out, void *descriptor, void *state);
int32_t gi_destroy_sampler(void *share, void *sampler);
int32_t gi_create_pipeline_program(void *share, void **out, void *descriptor, void *state);
int32_t gi_modify_pipeline_program(void *share, void *pipeline, uint32_t flags);
int32_t gi_unbind_pipeline_program(void *context, void *pipeline);
int32_t gi_destroy_pipeline_program(void *share, void *pipeline);
int32_t gi_create_program(void *share, void **out, void *descriptor, void *state);
int32_t gi_destroy_program(void *share, void *program);
#define GI_PIPELINE_STAGE_COUNT 5
/* 自有句柄及 Modify 失效代次；空槽的两个字段均为零。 */
typedef struct { vgo_handle handle; uint64_t generation; } gi_pipeline_slot;
typedef struct {
    vgo_handle share;
    gi_pipeline_slot pipeline[GI_PIPELINE_STAGE_COUNT];
    gi_pipeline_slot program[GI_PIPELINE_STAGE_COUNT];
} gi_default_pipeline_snapshot;
/* 仅供新建、无共享、无用户 program API 历史的受控实验使用。调用者负责证明
 * 该历史及默认来源；Create 前缀/tag 只能分类，不能证明一般程序语义或 H 支持域。
 * 从正式借用 B 读取全部五个 PIPELINE/PROGRAM 槽；仅允许 VP/FP 默认 state-program，
 * 核对 Create 保存的类型/PD 前缀及当前 descriptor、类别/share/attached。
 * 不读 PD 四字节以外、不读写 PU，不改变 policy、A 或 dispatch；失败不修改 out。
 * 返回借用快照，不新增强引用。gi 注册对象的既有 owner 保活 vgo 对象/share；
 * 所有 gi/vgo 生命周期调用、Modify、B/PD 写入及快照消费必须处于同一串行域。
 * 有效期仅到退出该串行域或任何绑定/descriptor/代次/生命周期变化之前；提交前重取。
 * 不得释放返回句柄的 owner 引用。外部指针句柄仍遵守本头的无 ABA 保证契约。 */
int32_t gi_context_default_pipeline_snapshot(void *context, gi_default_pipeline_snapshot *out);
/* 借用返回值，调用方须保活 gi 句柄；不得 release 原生句柄的这份引用。 */
int32_t gi_share_native(void *share, vgc_handle *native, vgt_handle *textures, vgo_handle *objects);
int32_t gi_texture_native(void *share, void *texture, vgt_handle *native);
int32_t gi_context_native(void *context, vgc_handle *native, void **a, void **h, void **b);
/* 供 FBO 代理显式登记的映射协议；不是虚构 vgf API。resolve 必须验证同 share，
 * 返回仍由 FBO 所有者保活的 native FB；注册期间 callback/cookie 必须有效。
 * 此处只提供映射，不将 B 的外部指针直接解释为 vgc handle。 */
typedef int32_t (*gi_framebuffer_resolver)(void *cookie, vgc_handle share,
                                         void *external_fb, vgc_handle *native_fb);
int32_t gi_register_framebuffer_resolver(void *share, gi_framebuffer_resolver resolve, void *cookie);
int32_t gi_resolve_framebuffer(void *share, void *external_fb, vgc_handle *native_fb);
#endif
