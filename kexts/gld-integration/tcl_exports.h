#ifndef VGPU_GI_TCL_EXPORTS_H
#define VGPU_GI_TCL_EXPORTS_H
#include <stdint.h>

/* 独立系统参数形状适配；不导出 gld，不依赖旧 primitive ABI。
 * 仅受控 legacy、新建无共享、无用户 program/TF 的调用域。不能据此发布未知
 * R13..16，不能证明真实 JIT/公开 CGL 已通过。调用方串行化调用和 staging 写入，
 * 回调不可重入。对象须存活；destroy 成功后任何旧指针均不得使用。
 */
#define GI_TCL_CAPACITY INT32_C(48)
#define GI_TCL_STRIDE 32
#define GI_TCL_DESCRIPTOR_SIZE 12
#define GI_TCL_GL_INVALID_OPERATION UINT32_C(0x502)
#define GI_TCL_CONTROLLED_LEGACY UINT32_C(1)

typedef struct gi_tcl gi_tcl;
typedef int32_t gi_tcl_result;
enum {
    GI_TCL_OK = 0, GI_TCL_INVALID = -1, GI_TCL_NO_MEMORY = -2,
    GI_TCL_BUSY = -3, GI_TCL_REJECTED = -4, GI_TCL_BACKEND_FAULT = -5
};
typedef enum {
    GI_TCL_DETAIL_NONE = 0, GI_TCL_DETAIL_DOMAIN, GI_TCL_DETAIL_MODE,
    GI_TCL_DETAIL_PRIMITIVE, GI_TCL_DETAIL_LEASE, GI_TCL_DETAIL_COUNT,
    GI_TCL_DETAIL_SEGMENT, GI_TCL_DETAIL_VERTEX, GI_TCL_DETAIL_STATE,
    GI_TCL_DETAIL_INVALIDATED, GI_TCL_DETAIL_CANARY, GI_TCL_DETAIL_DESCRIPTOR,
    GI_TCL_DETAIL_SUBMIT, GI_TCL_DETAIL_RELEASE, GI_TCL_DETAIL_DESTROYING
} gi_tcl_detail;

typedef struct {
    float position[4];
    float rgba[4];
} gi_tcl_vertex;

typedef struct {
    void *owner;
    /* 成功 retain 必须真正强持有实际 render owner、H/B 及依赖对象。
     * retain 失败不增引用；release 失败不消费引用，允许 destroy 重试。
     * validate 每次读取当前真实 H/B、默认 pipeline、目标及代次；不能只读
     * create 时的快照。返回 BACKEND_FAULT 永久封锁，其他失败为临时拒绝。
     * 跨 Begin/End 的状态/绑定/资源失效须调用 gi_tcl_invalidate。
     * submit 同步消费映射后的 position：x/w保持，y取反，z=(z+1)/2。
     * 资源ABI采用自上而下的行序，映射使直接读回行号符合GL窗口Y。
     * 不保存指针；成功才发布 generation。
     * error 由集成实现 GL 首错锁存；成功批次绝不调用 error(...,0,...)。
     */
    gi_tcl_result (*retain)(void *owner);
    gi_tcl_result (*release)(void *owner);
    gi_tcl_result (*validate)(void *owner);
    gi_tcl_result (*submit)(void *owner, const float positions[3][4], uint64_t *generation);
    void (*error)(void *owner, uint32_t gl_error, gi_tcl_detail detail);
} gi_tcl_backend;

typedef struct {
    uint32_t active, pending_rejection, backend_fault, destroying;
    gi_tcl_detail last_detail;
    uint64_t completed_generation;
} gi_tcl_status;

/* create 失败保持 *out 不变；创建时一次性预分配 staging 和自有可写 D。
 * Dalt 始终 NULL，D 地址稳定。只允许系统对 D 作保持既定布局的写入。
 */
gi_tcl_result gi_tcl_create(const gi_tcl_backend *backend, uint32_t domain, gi_tcl **out);
unsigned char *gi_tcl_descriptor(gi_tcl *context);
void *gi_tcl_alternate_descriptor(gi_tcl *context);
/* 活跃 lease 拒绝销毁。release 失败进入仅可 sink/重试销毁状态，不释放内存。
 * 只有成功才写 *context=NULL，失败保留指针和强引用。
 */
gi_tcl_result gi_tcl_destroy(gi_tcl **context);
/* R20/R21 的参数形状，C 由集成 typed wrapper 从自有 owner 映射到 gi_tcl。
 * 对有效对象 Begin 永远返回预分配 sink；有效 count 指针输出始终为48。
 * 输入非零拒绝；Begin 不分配、不清跨 lease pending。每次填 NaN 防旧顶点。
 * End 消耗 lease；只提交首次 flags8002/primitive4/count3。count0 不提交、
 * 不清 pending；flags2/大批拒绝。未知 primitive 不读取顶点。
 * 只能保证单段提交，R21 不提供完整 glEnd 事务边界。有效生命周期是前置条件。
 */
void *gi_tcl_begin(gi_tcl *context, uint32_t primitive, int32_t *count_inout);
void gi_tcl_end(gi_tcl *context, uint32_t flags, uint32_t primitive, uint32_t count);
/* 失效会污染当前 lease；后续健康首段成功才清临时 pending，不清 GL 首错。
 * fault 永久锁存，无 reset/GetError 通道。flush 必须检查 active/pending/fault。
 */
void gi_tcl_invalidate(gi_tcl *context, gi_tcl_detail detail);
void gi_tcl_fault(gi_tcl *context, gi_tcl_detail detail);
gi_tcl_result gi_tcl_get_status(const gi_tcl *context, gi_tcl_status *out);
#endif
