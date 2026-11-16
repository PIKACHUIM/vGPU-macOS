#ifndef VGPU_TEXTURE_BGRA_H
#define VGPU_TEXTURE_BGRA_H
#include "vgpu_texture.h"
#include "../gld-context/vgpu_context.h"

/* 项目自有桥接；所有桥接/texture/FBO 操作须由集成层外部串行化，
 * 禁止 cancellation 和回调重入。沿用 texture 的有效输出/借用内存契约。
 * 复用调用者的 vgc_share_create_bgra share，不创建第二个 owner。
 * 持有一份 context_share 引用；其类型/BGRA 能力由首次 image_create 验证。
 * 当前只支持 256x256、单样本 RGBA8 renderbuffer，无 CPU 上传 ABI。
 * backend resource 编码 vgc image handle（非可解引用指针），供下面的
 * image_get 校验和提取。无 CPU 源成功只承诺分配及 alpha 存储能力，
 * 不承诺 clear、内容初始化、提交完成或 GPU 验收。
 */
vgt_status vgt_bgra_share_create(vgc_handle context_share, vgt_handle *out);
/* 借用当前 image；不 retain。须持有 texture，使用期间禁止重建/release。
 * framebuffer_attach 成功会自行持有 image；重建后旧 FBO 附件不会自动替换。
 * 调用者须通过 context_share 验证归属，并用新的 image 显式重绑 FBO。
 * 失败不写 out；必须先 ensure_backing 成功。 */
vgt_status vgt_bgra_texture_image_get(vgc_handle context_share, vgt_handle texture,
                                      vgc_handle *out);
/* void destroy/lease release 失败由桥接预分配记录保留公开 vgc 引用。
 * 本函数逐个重试，并调用 vgc_drain_releases 处理 context 的内部回滚。
 * 成功仅表示无待释放项，仍可有正常存活对象。收尾须释放所有公开引用，
 * 再调用本函数直到成功；失败不能视为已释放，不得卸载模块。 */
IOReturn vgt_bgra_drain_releases(void);
#endif
