#ifndef VGPU_GI_PRIMITIVE_EXPORTS_H
#define VGPU_GI_PRIMITIVE_EXPORTS_H
#include <IOKit/IOReturn.h>
#include <stddef.h>
#include <stdint.h>

/* 项目自有 CPU staging API；不导出系统 GLD，不将 IOReturn 当作 GL error。
 * 调用者串行化全部调用和 staging 写入，禁止回调重入/cancellation。
 * 所有输入/输出区间须真实有效；输出独占且不能覆盖后端借用的 H/B 等存储。
 * 不解析未知 H 偏移，也不证明系统 JIT 已采用该布局。
 */
typedef uint64_t gi_primitive_handle;
#define GI_PRIMITIVE_CAPACITY INT32_C(64)
#define GI_PRIMITIVE_TRIANGLES UINT32_C(4)
#define GI_PRIMITIVE_END_FLAGS UINT32_C(0x8002)
#define GI_PRIMITIVE_STATE_VERSION UINT32_C(1)

typedef struct {
    uint32_t version, size;
    /* 仅为旧受控 probe 保留的 ABI 声明，不是已实现的系统状态门控。
     * 调用者自行置 1 不能据此启用系统 GL 路径。 */
    uint32_t all_state_verified;
    uint32_t full_target_viewport, color_write_mask, samples;
    /* blend/depth/stencil/scissor/cull/sRGB/纹理/程序等附加状态必须全部为零。 */
    uint64_t additional_state;
    float modelview[16], projection[16], rgba[4];
    unsigned char position_descriptor[8];
} gi_primitive_state_desc;

typedef struct {
    void *owner;
    /* retain 必须同时保活实际 render context、native H/B 和依赖对象。
     * retain 失败不增引用；release 失败不消费引用，可以重试 destroy。
     * submit 同步消费快照，不可保存 vertices 指针；实现应验证/更新真实 draw
     * 绑定后调用 vgc_draw_triangle(native, vertices, generation)，绝不隐式 clear。
     * flush 应验证同一个 native 的真实完成状态；失败可以改其临时 out。
     */
    IOReturn (*retain)(void *owner);
    IOReturn (*release)(void *owner);
    IOReturn (*submit)(void *owner, const float vertices[3][4], uint64_t *generation);
    IOReturn (*flush)(void *owner, uint64_t *generation);
} gi_primitive_backend;

typedef struct {
    uint32_t configured, active, destroying;
    int32_t capacity;
    IOReturn pending_failure, last_failure;
    uint64_t completed_generation;
} gi_primitive_status;

/* 创建时分配至少 64*16 字节、16 字节对齐的 staging，成功才持有后端强引用。
 * create 后仍未配置支持状态；失败保持 out 不变。 */
IOReturn gi_primitive_create(const gi_primitive_backend *backend, gi_primitive_handle *out);

typedef struct {
    float position[4];
    float rgba[4];
} gi_primitive_colored_vertex;
#define GI_PRIMITIVE_COLORED_DESCRIPTOR_SIZE 12
extern const unsigned char gi_primitive_colored_descriptor[GI_PRIMITIVE_COLORED_DESCRIPTOR_SIZE];
/* 仅受控自有 probe：创建固定 position+primary color 模式，直接配置该 CPU 布局。
 * 不接受 all_state_verified，也不宣称实现系统 GL 状态门控；实际状态读取、
 * polygon FILL、JIT/解释器缺口及系统启用条件尚未闭合，不能接到系统 GL 导出。
 * 分配 GI_PRIMITIVE_CAPACITY * 32 字节并额外预留 15 字节以保证 16B 对齐。
 * begin 返回容量以顶点计，每个记录 position float4 后接 rgba float4。
 * End 先复制全部三个记录，逐顶点验证有限 position 和红色 (1,0,0,1)，
 * 容许负零；任何非红均锁存失败并阻止 flush，绝不调用 submit。
 * 后端仍只收到映射后的 position 快照。此模式不能使用旧 set_state/begin，
 * 错用使批次失败；end/destroy/flush/status 与旧模式共用生命周期。
 */
IOReturn gi_primitive_create_colored_probe(const gi_primitive_backend *backend,
                                         gi_primitive_handle *out);
IOReturn gi_primitive_begin_colored(gi_primitive_handle context, uint32_t primitive,
                                   int32_t *capacity_inout, gi_primitive_colored_vertex **out);
/* 活跃 Begin 返回 ExclusiveAccess；release 失败进入仅允许重试 destroy 的状态。 */
IOReturn gi_primitive_destroy(gi_primitive_handle context);
/* 仅旧 position-only 受控 probe：复制并严格验证描述符；失败撤销配置，
 * 成功不消除 pending failure。此层不侦测 H/B 变化，描述符不能证明实际系统
 * GL 状态；受控调用者须维持所声明条件至 End，变化时重新核验配置。
 * 核验失败应传 NULL 撤销旧模式配置，并保留失败阻止 flush。
 * colored probe 拒绝此接口并锁存失败，不使用此声明作为系统状态门控。
 */
IOReturn gi_primitive_set_state(gi_primitive_handle context, const gi_primitive_state_desc *state);
/* capacity_inout 是有符号顶点数；正数裁剪到 64，零为无操作（out=NULL）。
 * 负数失败；重复 Begin 使当前批次失效，必须 End 消耗后才能重新 Begin。
 * 每批 staging 填 NaN，未写顶点不能误提交旧数据；指针仅活跃批次期间可写。
 * 失败不改输出。不适合作为 caller 不检查 NULL 的 R20 直接导出函数。
 */
IOReturn gi_primitive_begin(gi_primitive_handle context, uint32_t primitive,
                           int32_t *capacity_inout, float (**out)[4]);
/* 每次活跃 End 都消耗批次，包括错误参数；无 Begin/重复 End 失败。
 * 仅 flags=0x8002、primitive=4、count=3 提交；count=0 无操作，不清失败。
 * count 采用有符号值，必须 <= Begin 返回容量；其它非零 count 拒绝。
 * 先复制三个顶点（彩色模式包括 RGBA），再验证有限 xyz∈[-1,1], w=1，
 * z 映射到 (z+1)/2；彩色模式同时逐顶点验证红色。
 * 错误不调用 submit；仅成功非零提交清 pending，last_failure 保留诊断。
 * generation 仅非零成功提交时更新。
 */
IOReturn gi_primitive_end(gi_primitive_handle context, uint32_t flags, uint32_t primitive,
                         int32_t count, uint64_t *generation);
/* active/pending failure/尚无提交时不调用后端 flush，不报告早先的成功。
 * 后端代次必须等于本模块最近成功提交的代次；失败不写 out。
 * 集成层所有 flush/wait 必须检查本层 active/pending_failure，不能直接绕过
 * 本层调用 gi_render_flush/vgc_flush_completed，否则会丢失 CPU 验证失败。
 */
IOReturn gi_primitive_flush(gi_primitive_handle context, uint64_t *generation);
IOReturn gi_primitive_get_status(gi_primitive_handle context, gi_primitive_status *out);
#endif
