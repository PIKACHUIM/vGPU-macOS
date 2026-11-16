#ifndef VGPU_GLD_PIXEL_ADAPTER_H
#define VGPU_GLD_PIXEL_ADAPTER_H

#include "gld_pixel.h"

/* 独立 macOS 用户态适配 API，不导出系统 GLD 符号，不使用 CGLError 编码。
 * 0..GP_NO_MEMORY 原样保留 gp_status；额外错误见下表。返回值固定32位。
 * 256 是本实现的工作量限额，包括 token、值及终止词，不是源对象容量。
 */
#define GPA_MAX_WORDS 256U
#define GPA_READ_FAILED INT32_C(100)
#define GPA_WORD_LIMIT INT32_C(101)
#define GPA_INVALID_HANDLE INT32_C(102)
#define GPA_BUSY INT32_C(103)
#define GPA_LOCK_FAILED INT32_C(104)
#define GPA_GENERATION_EXHAUSTED INT32_C(105)
#define GPA_DRIVER_MISMATCH INT32_C(106)

/* 显式复制完整能力组合，默认空表；NULL,0 清空。存在活跃owner时拒绝变更。
 * 输入表须实际可读且在调用期间不变；格式合法性由 choose 中的原parser验证。
 * 不添加任何能力位。失败保留旧表。进程退出前可用 NULL,0 释放表。
 */
int32_t gpa_configure(const gp_format_cap *formats, size_t count);

/* tokens 可未对齐：逐个32位词经 mach_vm_read_overwrite 复制；只把成功
 * 且 outsize==4 的词交给原parser。token位置的0立即结束，value位置0不结束。
 * 未知token/负值立即由parser拒绝。读失败、溢出或达到限额均失败。
 * Mach保证映射读取，不证明malloc对象边界，也不是整个流的原子快照；调用者
 * 仍须保证输入生命期与内容稳定。无终止短堆对象不能检测其分配边界。
 * out_head/out_generation 须为有效、不重叠且与输入不重叠的独占可写槽。
 * 对非NULL输出槽先置0，任何失败不发布owner；NULL输入不表示空流。
 * 成功返回借用链与非零代次号；上层仅可改链，不可释放节点或私有owner。
 */
int32_t gpa_choose(void **out_head, const uint32_t *tokens,
                   uint64_t *out_generation);

/* 用 head->owner 注册表查找，持互斥锁移除且释放一次，不读取传入head内存，
 * 不遍历next。NULL、内部节点、外来/旧/重复句柄拒绝；generation防地址重用。
 * 同一owner的读取与destroy须由调用者同步；并发destroy仅一个成功。
 * 不支持信号处理器或多线程fork后的未exec子进程调用。
 * 此generation保证仅适用于独立API，不适用于无代次的系统GLD ABI。
 */
int32_t gpa_destroy(void *head, uint64_t generation);

/* 系统包装专用注册表，与上述独立API的配置和owner隔离；返回适配层状态。
 * 配置复制后要求全部(renderer_id & 0x00ffff00)相同，否则GPA_DRIVER_MISMATCH，
 * 失败保持旧表。默认空表，不提供默认renderer或能力；存在本表owner时GPA_BUSY。
 * choose沿用上述输入/输出槽契约；销毁仅接受本表当前注册head，NULL成功。
 * current查找、注册代次确认、移除与释放在同一锁范围，不读取传入head内存。
 * 无代次销毁按free式生命周期：销毁后不得再用旧指针，包括地址重用后的旧指针。
 * 只能拒绝当前未注册/内部/外来head，不能承诺ABA拒绝。调用者同步读取与销毁。
 */
int32_t gpa_system_configure(const gp_format_cap *formats, size_t count);
int32_t gpa_system_choose(void **out_head, const uint32_t *tokens);
int32_t gpa_system_destroy_current(void *head);

#endif
