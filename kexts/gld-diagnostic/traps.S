#if !defined(__APPLE__) || !defined(__x86_64__)
#error 此汇编仅支持 Mach-O x86_64
#endif

/* 无设备负向路径不会调用这些入口；误调用必须立即失败。
 * 不声明未知函数签名，也不伪造任何返回值或能力。 */
.text
#define GLD_IMPLEMENTED(name)
#define GLD_TRAP(name) .globl _##name ; .p2align 2 ; _##name: ; ud2
#include "required_entries.inc"
#undef GLD_TRAP
#undef GLD_IMPLEMENTED
