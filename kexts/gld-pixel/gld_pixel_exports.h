#ifndef VGPU_GLD_PIXEL_EXPORTS_H
#define VGPU_GLD_PIXEL_EXPORTS_H

#include "gld_pixel.h"

#define GPX_EXPORT __attribute__((visibility("default")))

/* 已证系统形态；成功0，错误采用本机SDK CGLError数值。
 * 输入契约见adapter：逐词Mach复制不是C对象边界证明，调用期间流须稳定存活。
 * out_head须独占可写且不与tokens重叠；失败清NULL。
 * 默认空能力表；合法但无候选时成功并返回NULL，不臆造renderer能力。
 * 已支持数值token的负值返回SDK kCGLBadValue。
 */
GPX_EXPORT int32_t gldChoosePixelFormat(void **out_head, const uint32_t *tokens);

/* NULL成功；仅销毁本包装当前注册的整条链head，不遍历next。
 * 销毁后不得再使用旧指针。无generation的free式ABI不提供ABA防护。
 * 内部节点/外来非NULL/当前未注册head返回BadPixelFormat。
 * 读链与销毁由调用者同步；卸载动态库前须销毁所有链并清空能力表。
 */
GPX_EXPORT int32_t gldDestroyPixelFormat(void *head);

/* 自研集成配置入口，不是Apple GLD槽。复制后限同一0x00ffff00 driver ID段。
 * 表由backend提供已证完整组合；NULL,0清空。活跃系统owner时拒绝修改。
 * 配置参数须真实可读且稳定；返回SDK CGLError，配置冲突映射BadMatch。
 * 配置错误映射是本层策略，不是两个系统GLD导出的硬件语义。
 */
GPX_EXPORT int32_t gpx_configure(const gp_format_cap *formats, size_t count);

#endif
