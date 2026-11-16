#ifndef VGPU_TEXTURE_H
#define VGPU_TEXTURE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 项目自有、有长度 API；不导出或冒充系统 GLD ABI。仅 64 位小端宿主。
 * 单调编号句柄，非指针；所有入口串行化。后端/生命周期回调不得重入，
 * 调用期间禁止 pthread cancellation，调用者不得并发修改借用视图。
 * retain/release 管理自有对象引用。FBO 应 retain texture，texture 强持有 share。
 * TD/TU 借用而不释放，两个视图不得重叠；可选 lease 将其实际所有者保活到 detach 或最后 release。
 * 无 lease 时，调用者必须保证视图有效到 detach 或最后 release。
 * create 的 out 必须是真实有效、正确对齐的可写句柄，且不与完整 TD/TU
 * 视图或 share_create 的 backend 结构重叠；重叠/区间无效时不写 out。
 * 数值区间检查不证明地址映射或实际容量；其他 create 失败清零 out。
 * detach 后可释放 TD/TU，但对象只能检查状态或释放，不可再次 materialize。
 */
typedef uint64_t vgt_handle;
typedef enum {
    VGT_OK = 0, VGT_INVALID = -1, VGT_BOUNDS = -2, VGT_UNSUPPORTED = -3,
    VGT_NO_MEMORY = -4, VGT_NO_BACKEND = -5, VGT_BACKEND_FAILED = -6,
    VGT_BAD_HANDLE = -7, VGT_OWNER_MISMATCH = -8, VGT_DETACHED = -9,
    VGT_OVERFLOW = -10
} vgt_status;
typedef struct { const void *data; size_t length; } vgt_view;
typedef struct { void *data; size_t length; } vgt_mut_view;
typedef struct {
    void *context;
    vgt_status (*retain)(void *context);
    void (*release)(void *context);
} vgt_lease;
typedef struct { vgt_view td; vgt_mut_view tu; vgt_lease lease; } vgt_views;
#define VGT_TD_METADATA_LENGTH ((size_t)0x80)
#define VGT_TD_RB_LENGTH ((size_t)0xd0)
#define VGT_TU_LENGTH ((size_t)0x15)
#define VGT_DIRTY_SUPPORTED UINT8_C(0x07)
#define VGT_FORMAT_RGBA8_UNORM UINT32_C(1)
#define VGT_BACKEND_VERSION UINT32_C(1)

typedef struct {
    uint32_t width, height, depth, format, samples;
    size_t cpu_row_bytes, cpu_slice_bytes;
    vgt_view cpu; /* 同步调用内借用；后端返回后不得保留地址 */
} vgt_image_desc;
typedef struct {
    void *resource; /* 后端自有独立资源；不得返回旧 resource */
    uint32_t width, height, format, samples;
    size_t row_bytes, byte_size;
} vgt_backing;
typedef struct {
    uint32_t version, max_width, max_height;
    /* 必须真的保留 alpha；BGRX/固定 alpha 后端不满足此能力。 */
    uint32_t rgba8_alpha;
    void *context;
    vgt_lease lease; /* 使 context 在 share 存活期间有效；无回调时由调用者保活 */
    /* 完整重建+同步导入 CPU 源；成功必须给出真实资源。失败时返回的部分
     * resource 也移交本模块 destroy。不得修改现有资源或借用 TD/TU。
     * 不承诺 clear 已完成/内容已初始化；无 CPU 源时内容未定义。
     * row_bytes/byte_size 是后端导出的线性存储契约；非线性后端需适配。
     */
    vgt_status (*create_image)(void *context, const vgt_image_desc *desc,
                               vgt_backing *out);
    void (*destroy_image)(void *context, vgt_backing *image);
} vgt_backend;
typedef struct {
    vgt_handle share;
    uint64_t generation; /* backing 代次，不是 GPU clear/fence 完成代次 */
    uint32_t references, kind, attached, has_backing;
    vgt_backing backing; /* resource 仅观察；使用者须持有 texture 并外部串行化 */
} vgt_texture_state;

/* backend=NULL 是正式默认：允许元数据创建，ensure 必须返回 NO_BACKEND。 */
vgt_status vgt_share_create(const vgt_backend *backend, vgt_handle *out);
vgt_status vgt_texture_create(vgt_handle share, vgt_views views, vgt_handle *out);
/* 验证归属并原子释放一份 texture 引用；允许 views 已 detach。 */
vgt_status vgt_texture_destroy(vgt_handle share, vgt_handle texture);
/* 通用 release 不需要 owner，texture_destroy 则精确映射证实的 destroy(SG,X)。 */
vgt_status vgt_retain(vgt_handle handle);
vgt_status vgt_release(vgt_handle handle);
vgt_status vgt_texture_state_get(vgt_handle texture, vgt_texture_state *out);
vgt_status vgt_texture_detach_views(vgt_handle share, vgt_handle texture);
/* 不自行写调用方的 flags/masks；modify 在成功时按已证 Mercury 行为 OR dirty 2。
 * 未支持 flags 或已有未知 dirty 均拒绝且完全不写 TU。 */
vgt_status vgt_texture_modify(vgt_handle share, vgt_handle texture, uint32_t flags);
/* 仅 kind2 单组 level0；cpu 必须匹配 L+0x18 并提供真实可读容量。
 * cpu 完整容量区间不得与 TU 将写入的 [8,0x15) 重叠；decode 即拒绝，
 * ensure/load 不调用后端、不改源/TU 或旧 backing。无 CPU 源时传 {NULL,0}。
 * 输出只在成功写入。 */
vgt_status vgt_texture_decode_renderbuffer(vgt_handle share, vgt_handle texture,
                                          vgt_view cpu, vgt_image_desc *out);
/* Load 和未来 FBO 必须共用此函数。未知 dirty/masks 拒绝；失败保留 TU、
 * 旧 backing 和 generation；完整同步成功才提交新 backing 并清已处理 TU。
 * 即使 dirty=0，没有资源也不能空成功。 */
vgt_status vgt_texture_ensure_backing(vgt_handle share, vgt_handle texture, vgt_view cpu);
vgt_status vgt_texture_load(vgt_handle share, vgt_handle texture, vgt_view cpu);

/* 已证系统入口的未来映射（尚不提供无长度 wrapper）：
 * gldCreateTexture(SG,&X,TD,TU) -> 映射 SG，先取得受信任长度/lease，再 create。
 * X 需集成层独立句柄映射，不能直接当成本模块对象指针。
 * gldModifyTexture(SG,X,u32 flags) -> modify；保留第三参。
 * gldDestroyTexture(SG,X) -> 验证 share 后释放调用者持有的 X 引用；不是 &X。
 * 三入口已证成功返回 0，失败码须由集成层独立映射，不能猜系统 errno。
 * Mercury Load 是成功 1/失败 0；本 API 始终使用 vgt_status，不混用。
 * 资料：out/gld-context-init-required-verified.txt 第5节；
 * out/gld-texture-verified-snippets.txt [B][D][H][J]。
 */
#ifdef __cplusplus
}
#endif
#endif
