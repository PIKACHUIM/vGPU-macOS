#if !defined(__APPLE__) || !defined(__x86_64__)
#error 此汇编仅支持 Mach-O x86_64
#endif
/* 保留独立符号地址，崩溃现场可定位到未知入口；不猜签名或返回值。 */
.text
#define GLD_IMPLEMENTED(name)
#define GLD_TRAP(name) .globl _##name ; .p2align 2 ; _##name: ; ud2
#include <integration_entries.inc>
