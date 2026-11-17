#ifndef VGPU_GI_DISPATCH_EXPORTS_H
#define VGPU_GI_DISPATCH_EXPORTS_H

#include <stdint.h>

/* 已证目标仅为 x86_64 SysV；Windows CPU fixture 也必须使用同一调用约定。 */
#if !defined(__x86_64__) && !defined(_M_X64)
#error "dispatch requires x86_64"
#endif
#if defined(_WIN32)
#if defined(__clang__) || defined(__GNUC__)
#define GI_DISPATCH_ABI __attribute__((sysv_abi))
#else
#error "Windows CPU probe requires a compiler supporting sysv_abi"
#endif
#else
#define GI_DISPATCH_ABI
#endif

#define GI_DISPATCH_SLOT_COUNT 25u
#define GI_DISPATCH_TABLE_BYTES 200u

/* 机器载体类型，不宣称恢复 Apple 源语言原型；未知 P 仅透传、不读取。
 * ignore-return 槽采用 void。R13..16 无已证签名，保持空槽，禁止调用。
 */
typedef void (GI_DISPATCH_ABI *gi_dispatch_r0_fn)(void *, uint32_t);
typedef uint32_t (GI_DISPATCH_ABI *gi_dispatch_r1_fn)(
    void *, void *, uint32_t, uint32_t, uint32_t, uint32_t,
    void *, uint32_t, uint64_t);
typedef void (GI_DISPATCH_ABI *gi_dispatch_r2_fn)(
    void *, void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (GI_DISPATCH_ABI *gi_dispatch_r3_fn)(void *, void *, uint32_t, uint32_t);
typedef gi_dispatch_r3_fn gi_dispatch_r4_fn;
typedef gi_dispatch_r3_fn gi_dispatch_r5_fn;
typedef gi_dispatch_r3_fn gi_dispatch_r6_fn;
typedef gi_dispatch_r3_fn gi_dispatch_r7_fn;
typedef gi_dispatch_r3_fn gi_dispatch_r8_fn;
typedef void (GI_DISPATCH_ABI *gi_dispatch_r9_fn)(
    void *, void *, void *, uint32_t, uint32_t);
typedef gi_dispatch_r3_fn gi_dispatch_r10_fn;
typedef gi_dispatch_r3_fn gi_dispatch_r11_fn;
typedef gi_dispatch_r3_fn gi_dispatch_r12_fn;
typedef void (GI_DISPATCH_ABI *gi_dispatch_r17_fn)(void *, void *, uint32_t);
typedef gi_dispatch_r3_fn gi_dispatch_r18_fn;
typedef gi_dispatch_r3_fn gi_dispatch_r19_fn;
typedef void *(GI_DISPATCH_ABI *gi_dispatch_r20_fn)(void *, uint32_t, int32_t *);
typedef void (GI_DISPATCH_ABI *gi_dispatch_r21_fn)(void *, uint32_t, uint32_t, uint32_t);
typedef void *(GI_DISPATCH_ABI *gi_dispatch_r22_fn)(
    void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (GI_DISPATCH_ABI *gi_dispatch_r23_fn)(
    void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void *);
typedef uint32_t (GI_DISPATCH_ABI *gi_dispatch_r24_fn)(
    void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    void *, uint32_t, uint64_t);

/* 集成层提供这三个固定链接符号；C 是系统传入的 context，不是自有 owner。
 * 适配实现须解析 C 到受保活的自有状态，并串行化生命周期和错误锁存。
 * reject 锁存 GL 错误并记录 slot，不解引用未知 P、不提交 GPU。
 * begin 必须原样接收输入 count（包括 0），覆盖为有效正容量并返回预分配
 * 长期存活的对齐 sink；拒绝/故障也必须履行该协议。end 结束本段 lease，
 * flags 不是整个 glBegin/glEnd 的最终边界。表工厂不实现这些状态策略。
 * 这些适配符号使用宿主 C ABI，槽 wrapper 负责转换为目标 SysV ABI。
 */
void gi_dispatch_reject(void *context, uint32_t slot);
void *gi_dispatch_begin(void *context, uint32_t primitive, int32_t *count_inout);
void gi_dispatch_end(void *context, uint32_t flags, uint32_t primitive, uint32_t count);

/* storage 指向至少 200 字节独占可写存储，可不对齐；完整构造后调用者再发布。
 * 不分配、不保留 storage；所有回调具有静态生命周期，没有可变全局 hooks。
 * R22 返回 NULL 会触发 fallback，必须已有安全下游；R24 锁存后返回 1，
 * 只表示消费拒绝，绝非绘制成功。R13..16 为 NULL，仅限已证不触达的受控域。
 * 依据 out/gld-reject-dispatch-contract.txt 第 5 节，不是系统 GL/JIT 验收。
 */
void gi_dispatch_init(void *storage);

#endif
