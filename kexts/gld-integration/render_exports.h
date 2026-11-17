#ifndef VGPU_GI_RENDER_EXPORTS_H
#define VGPU_GI_RENDER_EXPORTS_H
#include "context_exports.h"
#include "../gld-framebuffer/vgpu_framebuffer.h"
#include "../gld-state/vgpu_clear_state.h"

/* 项目自有有界接口，返回 IOReturn，不冒充系统 GLD 的返回协议。
 * 调用者统一串行化 gi/vgt/vgf/vgc，禁止回调重入和 cancellation。
 * 仅 64 位小端；所有声明区间必须真实有效；输出不得覆盖任何借用视图。
 * share_open 独占该 gi share 的 resolver，close 前不得自行替换/注销它。
 * H/B、FQ/FD 借用至对应 destroy 成功；TD/TU 遵守 context_exports 契约。
 * 所有外部指针只查注册表；销毁后的 token 禁止复用（无 ABA 保证）。
 * 本层 context 是独立 render 句柄，并非 gi_create_context 的返回值。
 * 集成者在创建 gi context 时用同一 share/H/B 创建本层句柄并保存映射；
 * 不把 gi_context_native 的私有 native_b 当作可更新存储。
 */
IOReturn gi_render_share_open(void *share);
IOReturn gi_render_share_close(void *share);
IOReturn gi_render_framebuffer_create(void *share, vgc_view fq, vgf_mut_view fd, void **out);
IOReturn gi_render_framebuffer_load(void *share, void *framebuffer);
/* 绑定中的 FBO 返回 ExclusiveAccess；开始销毁后只允许重试 destroy。 */
IOReturn gi_render_framebuffer_destroy(void *share, void *framebuffer);
IOReturn gi_render_texture_load(void *share, void *texture);
IOReturn gi_render_context_create(void *share, vgc_view h, vgc_view b, void **out);
IOReturn gi_render_context_destroy(void *context);
/* 只接受 DRAW/READ dirty，失败不改 dirty/已绑定对象；0 token 表示解绑。
 * B+4a0/+4a8 是外部 FBO 指针，不是 native handle。 */
IOReturn gi_render_context_update(void *context, uint32_t *dirty);
/* mask=0 是明确无操作，不改 generation，也不消除先前失败状态。
 * 非零仅支持命名 RGBA8 renderbuffer、全颜色写入、无 scissor/sRGB。
 * clear/read 每次重新解析对应 B token 和已成功 load 的 target。
 * clear 失败阻止 flush 把较早的完成状态误报为此次成功。 */
IOReturn gi_render_clear(void *context, uint32_t mask, vgc_view values, uint64_t *generation);
/* 受限内部提交：调用者必须已验证完整 GL 状态满足 vgc_draw_triangle 契约；
 * 本接口不解码/验证未知 H 状态。三个 float4 顶点必须有限，x/y∈[-1,1]、
 * z∈[0,1]、w=1；输入快照，generation 不得覆盖 vertices 或借用视图。
 * 每次重新加载并绑定当前 DRAW token；失败保留 generation 并阻止 flush/wait，
 * 后续成功 clear/triangle 才清除该失败状态，clear(mask=0) 不清除。 */
IOReturn gi_render_draw_triangle(void *context, const float vertices[3][4], uint64_t *generation);
typedef struct {
    uint32_t width, height;
    vgc_handle image;
    uint64_t completed_generation;
} gi_render_triangle_target_info;
/* 为 TCL validate 查询当前 B+4a0 DRAW：沿用 triangle 的 load/bind 规则，
 * FBO 须已成功初次 load；失败失效后须显式 framebuffer_load 重试。
 * 仅命名、线性 256x256 RGBA8 renderbuffer、单 color0，无 depth/stencil/MSAA。
 * image 是实际 backing 的借用身份，不是每次 load 都会更换的 FBO target；
 * completed_generation 来自实际 image，可以为0（尚无提交），不证明内容已初始化。
 * 不 clear、不提交、不清除先前 submit_failed；失败不写 out 并阻止 flush/wait。
 * out 不得覆盖借用视图；调用者须保持串行化，使用 image 期间不得重建/释放目标。
 * 成功只验证目标，不代表 triangle_state 的其他未验证状态已闭合。 */
IOReturn gi_render_triangle_target(void *context, gi_render_triangle_target_info *out);
/* 加载当前 B+4a8 READ token 并查询实际 image 的 completed_generation，
 * 支持 draw/read 分离和其他 context 更新；不使用 context last/flush。
 * 无完成代次、未绑定或 load 失败均不写输出；查询成功不消除提交失败。
 * 调用者须在本接口与 read 之间保持统一串行化；read 仍重新解析并校验代次。 */
IOReturn gi_render_read_generation(void *context, uint64_t *generation);
/* generation 必须来自目标 image 的真实成功 clear/draw；显式容量，不猜 pixels 长度。
 * 仅 GL_RGBA/GL_UNSIGNED_BYTE、mode=0、无 PBO。每次读取当前 H pack。 */
IOReturn gi_render_read(void *context, uint64_t generation,
                       int32_t x, int32_t y, int32_t width, int32_t height,
                       uint32_t format, uint32_t type, void *pixels, size_t capacity,
                       uint32_t mode, void *buffer);
IOReturn gi_render_flush(void *context, uint64_t *generation);
/* 同步后端的 wait 与 flush 均验证真实完成代次，不创建虚构 fence。 */
IOReturn gi_render_wait(void *context, uint64_t *generation);
#endif
