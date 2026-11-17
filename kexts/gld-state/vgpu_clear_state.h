#ifndef VGPU_CLEAR_STATE_H
#define VGPU_CLEAR_STATE_H

#include "../gld-context/vgpu_context.h"

#define VGPU_CLEAR_H_MIN_LENGTH ((size_t)0x3e8d)
#define VGPU_CLEAR_V_MIN_LENGTH ((size_t)0x1c)

typedef enum {
    VGPU_CLEAR_STATE_OK = 0,
    VGPU_CLEAR_STATE_NOOP = 1,
    VGPU_CLEAR_STATE_BAD_ARGUMENT = 2,
    VGPU_CLEAR_STATE_UNSUPPORTED = 3
} vgpu_clear_state_result;

/* 独立、同步、无分配的纯解码器，不是 Apple GLD 导出或错误码映射。
 * 证据：out/gld-context-r1-r4-state-evidence.txt 第3/7节，以及
 * out/gld-clear-readback-research.txt 第4.2节（V+0x18 是 u32）。
 * 仅支持64位小端 IEEE binary32 宿主；H/V 可未对齐且互相重叠。
 * 调用者保证整个声明区间真实可读、调用期间不变，out 指向真实可写
 * sizeof(vgc_clear_desc) 字节。长度检查不能验证虚拟地址是否已映射。
 * out 不得与任一完整声明输入区间重叠，包括本模块不读取的尾部。
 * 所有区间先检查地址加长度溢出；仅 OK 写 out，其余结果不改任何字节。
 *
 * 参数检查始终先做：非NULL、最短长度、区间溢出、输出别名。
 * internal_mask==0 返回 NOOP，不读取状态字段、不写 out，不调用 vgc_clear；
 * 它不表示其余状态受支持。其余仅接受 mask==1（内部 attachment0，非GL mask）。
 * 非空操作只接受：H+3cae u16==1；四个 H+3ca8..3cab u8 的 bit0 均为1；
 * H+3d84 u8==0，H+3e88 u32==0（所有位），H+3e8c u8==0；
 * H+3c1c / H+3c38 u8==0；V+18 u32==0，V+0..0f 四个有限float在[0,1]。
 * multi-viewport 非零保守拒绝。所有上述 enable/type/汇总字段均精确检查，
 * 不掩掉未知位；通道位图的 bit1..7 已知对应其他目标，允许任意组合。
 * 关闭scissor时矩形值不影响本路径；V的depth/stencil值未被mask选择，不读取。
 * 不复制整个H、不持有输入、不写输入；成功只复制四分量及构造desc。
 *
 * 调用方还须验证命名FBO、唯一attachment0、RGBA8非整数/非sRGB/非多采样、
 * 实际资源与同步条件。成功不证明这些资源条件、GPU完成或GLD错误映射闭合。
 * 仅 OK 时可以把 out 交给 vgc_clear；NOOP 不应伪造generation或清除dirty。
 * 调用方必须保持状态解码与实际提交的同步，不得在二者之间并发更改状态。
 */
vgpu_clear_state_result vgpu_clear_state_decode(vgc_view h, vgc_view v,
                                                 uint32_t internal_mask,
                                                 vgc_clear_desc *out);
#endif
