#ifndef VGPU_SURFACE_REGISTRY_H
#define VGPU_SURFACE_REGISTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 项目自有、设备级、纯 CPU 生命周期核心；不是 selector/GLD ABI。
 * 尚无内核 wrapper；整数 ID/handle 本身不构成授权。
 *
 * 可信边界：仅内核适配层可调用本接口、访问 registry 内存。scope 是内核
 * 分配且永不复用的设备实例标识；identity 是内核从调用者验证所得的主体
 * 实例标识，不是 PID/task_t/用户字段，不得在旧引用存活时复用。owner 是
 * open 返回并由内核绑定到实际连接的 token；不得从用户输入构造 actor。
 * 同 identity 的不同 draw 连接可导入；跨主体共享不支持。适配层必须自行
 * 执行权限、调用者身份验证、长度验证、连接关闭同步和设备生命期保活。
 *
 * 所有操作由同一设备锁串行化。不得复制/搬迁已发布的 registry，不支持
 * 并发、重入、直接改内部字段。输入输出必须有效、正确对齐且不与 registry
 * 或其他参数重叠。输出仅成功时写入，失败后禁止消费原输出值。
 * 完成分配/清理是可信内核确认，不是用户可调用的承诺。无内存/GPU/IO 操作。
 *
 * reserve 只预留槽位并返回 ALLOCATE，complete(ready=1) 仅在后端真实分配、
 * 初始化及安全可用后调用。ready=0 表示失败，进入待清理状态。关闭/撤销
 * 进行中的分配仅取消发布；必须等分配返回后调用 complete，才允许清理。
 * 每次 reserve 成功必须且只能 complete 一次，即使 creator 已关闭。
 *
 * revoke/close/release 撤销逻辑引用；最后引用消失后 next_cleanup 返回
 * RELEASE。适配层完成真实清理（包括部分分配回滚、GPU fence 等）后调用
 * cleanup_complete，之前槽位不会复用。清理失败保留 action 重试；不可假
 * 确认。next_cleanup 为可重复窥视，内核必须保证一个槽最多一个清理执行者。
 * ready 返回成功只证明适配层确认了 backing，不代表 GPU 工作完成。
 *
 * surface_id 由可信适配层按设备生命周期严格递增提供，0 无效；成功预留
 * 即消耗 ID（失败回滚也不复用），不允许用户选择 ID 驱动计数器。此规则
 * 使固定容量无需无限 tombstone 即可拒绝过期 ID。现有 selector ID 宽度/
 * 分配方式尚未接线，不能直接假定兼容。所有 token 达 UINT64_MAX 后拒绝
 * 新分配但仍可清理；不回绕、不 ABA。scope 在设备重建后也必须更换。
 */
#define VSR_MAX_SLOTS 8u
#define VSR_MAX_OWNERS 16u
#define VSR_MAX_LEASES 32u

typedef enum {
    VSR_OK = 0, VSR_INVALID = -1, VSR_SCOPE = -2, VSR_DENIED = -3,
    VSR_CLOSED = -4, VSR_STALE = -5, VSR_DUPLICATE = -6,
    VSR_CAPACITY = -7, VSR_OVERFLOW = -8, VSR_NOT_READY = -9,
    VSR_BACKEND_FAILED = -10, VSR_EMPTY = -11
} vsr_status;

typedef struct { uint64_t scope, identity, owner; } vsr_actor;
typedef struct { uint64_t scope, generation; uint32_t slot; } vsr_backing;
typedef enum { VSR_ALLOCATE = 1, VSR_RELEASE = 2 } vsr_action_kind;
typedef struct {
    vsr_action_kind kind;
    uint64_t surface_id;
    vsr_backing backing;
} vsr_action;
typedef struct {
    uint64_t scope, handle;
} vsr_lease;

/* 结构公开仅用于内核嵌入式固定存储，不属于稳定 ABI。调用方不得修改。 */
typedef enum {
    VSR_SLOT_FREE = 0, VSR_SLOT_RESERVED, VSR_SLOT_READY,
    VSR_SLOT_CANCELLED, VSR_SLOT_RETIRED, VSR_SLOT_DRAINING
} vsr_slot_state;
typedef struct {
    uint64_t token, identity;
} vsr_owner_entry;
typedef struct {
    uint64_t surface_id, generation, creator_owner, creator_identity;
    uint32_t creator_ref, lease_refs;
    vsr_slot_state state;
} vsr_slot_entry;
typedef struct {
    uint64_t token, owner, generation;
    uint32_t slot;
} vsr_lease_entry;
typedef struct {
    uint64_t scope, next_token, high_surface_id;
    uint32_t slot_capacity, owner_capacity, lease_capacity;
    vsr_owner_entry owners[VSR_MAX_OWNERS];
    vsr_slot_entry slots[VSR_MAX_SLOTS];
    vsr_lease_entry leases[VSR_MAX_LEASES];
} vsr_registry;

/* r 必须先零初始化；每个存储实例仅 init 一次，scope 全生命周期不复用。
 * 容量运行时选取，1..MAX，边界可测试。模块不持有 task_t 或动态内存。 */
vsr_status vsr_init(vsr_registry *r, uint64_t scope, uint32_t slots,
                    uint32_t owners, uint32_t leases);
vsr_status vsr_open(vsr_registry *r, uint64_t scope, uint64_t identity,
                    vsr_actor *out);
/* 先移除连接，再撤销其 creator 引用及其全部 lease；其他 owner 的 lease 保活。 */
vsr_status vsr_close(vsr_registry *r, vsr_actor actor);
vsr_status vsr_reserve(vsr_registry *r, vsr_actor actor, uint64_t surface_id,
                       vsr_action *out);
/* 独立事务凭据，不依赖 creator 仍 open。ready 必须为 0 或 1。
 * 失败/取消返回 BACKEND_FAILED/CLOSED，同时产生待清理工作。 */
vsr_status vsr_complete(vsr_registry *r, vsr_action allocation, int ready);
/* 撤销 ID 并释放 creator 引用；已导入 lease 仍能 resolve backing。 */
vsr_status vsr_revoke(vsr_registry *r, vsr_actor actor, uint64_t surface_id);
vsr_status vsr_import(vsr_registry *r, vsr_actor actor, uint64_t surface_id,
                      vsr_lease *out);
vsr_status vsr_resolve(vsr_registry *r, vsr_actor actor, vsr_lease lease,
                       vsr_backing *out);
vsr_status vsr_release(vsr_registry *r, vsr_actor actor, vsr_lease lease);
vsr_status vsr_next_cleanup(vsr_registry *r, uint64_t scope, vsr_action *out);
vsr_status vsr_cleanup_complete(vsr_registry *r, vsr_action release);
/* EMPTY 表示仍有连接、事务、backing 或 lease，必须继续收尾；不自动遗弃。 */
vsr_status vsr_quiescent(vsr_registry *r, uint64_t scope);

#ifdef __cplusplus
}
#endif
#endif
