#ifndef VGPU_SURFACE_H
#define VGPU_SURFACE_H

#include <IOKit/IOReturn.h>
#include <stddef.h>
#include <stdint.h>

/* 本模块自有 API，不是 Apple GLD ABI。旧 vgpu_surface API 的连接共享同一目标。 */
#define VGPU_SURFACE_WIDTH 256U
#define VGPU_SURFACE_HEIGHT 256U
#define VGPU_SURFACE_BGRX8 1U
#define VGPU_SURFACE_RGBA8_OUTPUT 2U

typedef struct vgpu_surface vgpu_surface;
typedef struct {
    uint32_t x, y, width, height;
} vgpu_surface_rect;
typedef struct {
    size_t row_length;
    size_t stride_bytes;
    size_t skip_rows;
    size_t skip_pixels;
    size_t alignment;
} vgpu_surface_pack;

/* 仅接受 256x256 / BGRX8 backing；创建连接不创建独立 GPU 资源。
 * 每个并发调用者须持有一份引用；retain 仅可用于仍持有引用的对象。
 * close 可与操作并发，等待本连接正在进行的 IO 完成；release 后不可再使用。
 * 输入/输出内存由调用者保持有效且不得在调用期间并发修改。
 * 禁止通过 pthread cancellation 中断本模块同步操作。
 */
IOReturn vgpu_surface_open(uint32_t width, uint32_t height, uint32_t backing,
                           vgpu_surface **out);
IOReturn vgpu_surface_retain(vgpu_surface *surface);
void vgpu_surface_release(vgpu_surface *surface);
IOReturn vgpu_surface_close(vgpu_surface *surface);
IOReturn vgpu_surface_clear(vgpu_surface *surface, const float rgba[4],
                            uint64_t *generation);
/* 仅接受非空、完全在目标内的矩形，坐标采用 selector 1 的行顺序，不翻转。
 * stride!=0 优先；否则 align_up((row_length ?: width)*4, alignment)。
 * alignment 仅 1/2/4/8。允许行重叠，按递增行号复制；skip 不改变源坐标。
 * capacity 是从 destination 开始实际可写的字节数，padding 保持原值。
 * 失败不写 destination；kIOReturnNotReady 包括 stale generation/设备未就绪。
 * 成功表示最终 selector 1 校验时的完整快照；返回后其他连接仍可提交。
 * 输出 RGBA8，BGRX 的 X 不当作 alpha，alpha 明确写 255。
 */
IOReturn vgpu_surface_read(vgpu_surface *surface, uint64_t generation,
                           vgpu_surface_rect rect, uint32_t output_format,
                           vgpu_surface_pack pack, void *destination,
                           size_t capacity);
/* 独立 BGRA8 资源：显式 opt-in，不接入旧 BGRX/context 路径。 */
#define VGPU_SURFACE_BGRA8 141U
typedef struct vgpu_surface_owner vgpu_surface_owner;
typedef struct vgpu_surface_image vgpu_surface_image;
typedef enum {
    VGPU_SURFACE_IMAGE_LIVE = 1,
    VGPU_SURFACE_IMAGE_RELEASE_FAILED = 2,
    VGPU_SURFACE_IMAGE_CLOSED = 3
} vgpu_surface_image_state;

/* share 可持 owner 引用；每个 image 另持强引用。owner 无提前 close 入口。
 * retain/并发调用/内存有效期/cancellation 契约与旧 API 相同。
 * release(NULL) 成功；release 失败不消费引用，调用者必须保留对象并重试。
 * owner 仅在最后引用释放时关闭连接，关闭失败保留连接及引用。
 */
/* 旧入口仍选择首个匹配的 vgpuAccelerator。 */
IOReturn vgpu_surface_owner_open(vgpu_surface_owner **out);
/* 精确绑定 IORegistry entry ID（不是 io_service_t），自建并拥有独立连接。
 * registry_id=0 / out=NULL / 非 vgpuAccelerator 返回 BadArgument；未找到或
 * ID 校验不一致返回 NotFound；查询和打开失败原样返回，不回退到其他设备。
 * out 有效时失败置 *out=NULL；引用及 release 重试规则同上。
 */
IOReturn vgpu_surface_owner_open_registry(uint64_t registry_id, vgpu_surface_owner **out);
IOReturn vgpu_surface_owner_retain(vgpu_surface_owner *owner);
IOReturn vgpu_surface_owner_release(vgpu_surface_owner *owner);
/* 仅支持 256x256/BGRA8。通常失败置 *out=NULL；若内核创建成功但响应
 * 异常且回滚释放失败，*out 返回 RELEASE_FAILED 对象，必须重试 release。
 * out 不得与 owner/image 对象内存重叠。
 */
IOReturn vgpu_surface_image_create(vgpu_surface_owner *owner, uint32_t width,
                                   uint32_t height, uint32_t backing,
                                   vgpu_surface_image **out);
IOReturn vgpu_surface_image_retain(vgpu_surface_image *image);
IOReturn vgpu_surface_image_release(vgpu_surface_image *image);
/* close 仅释放该 image 的资源；成功后幂等。失败进入 RELEASE_FAILED，
 * 禁止 clear/read/info，保留 handle、owner 和引用以供 close/release 重试。
 * selector 5 返回 NotFound 表示已无此资源（例如前次响应损坏），视为已关闭。
 * 若资源已关闭、最后 owner 关闭失败，状态为 CLOSED、release_error 非零，
 * image 引用仍未消费；再次 release 重试 owner 关闭。
 */
IOReturn vgpu_surface_image_close(vgpu_surface_image *image);
IOReturn vgpu_surface_image_status(vgpu_surface_image *image,
                                   vgpu_surface_image_state *state,
                                   IOReturn *release_error);
/* info 使用 selector 8；generation 是该 image 独立代次。失败不改输出。
 * clear 先查当前代次再提交 selector 6；rgba 与 generation 允许重叠，
 * 输入会先快照。所有输出均不得与 opaque 对象内存重叠。
 */
IOReturn vgpu_surface_image_info(vgpu_surface_image *image, uint64_t *generation);
IOReturn vgpu_surface_image_clear(vgpu_surface_image *image, const float rgba[4],
                                  uint64_t *generation);
/* 受限独立绘制：selector 6 opcode 4，固定红色 PS、alpha=1，绝不 clear。
 * 三个 float4 顶点：有限 x/y∈[-1,1]、z∈[0,1]、w=1；不支持其他绘制状态。
 * 输入先快照，允许 vertices 与 generation 重叠；输出不得与 opaque 对象重叠。
 * 与 clear 一样持 image 锁先 info，再提交 expected_generation 并完整校验响应。
 * 失败不写 generation，不回退到 clear+draw；旧内核 Unsupported 原样返回。
 */
IOReturn vgpu_surface_image_draw_triangle(vgpu_surface_image *image, const float vertices[3][4],
                                          uint64_t *generation);
/* pack、越界、行重叠、事务快照契约同旧 read；BGRA 的 alpha 原样保留。
 * selector 7 逐行读临时区，最终再次校验才提交；不同 image 的提交不 stale。
 * 同一 image 操作串行化；失败不写 destination，包括 stale generation。
 */
IOReturn vgpu_surface_image_read(vgpu_surface_image *image, uint64_t generation,
                                 vgpu_surface_rect rect, uint32_t output_format,
                                 vgpu_surface_pack pack, void *destination,
                                 size_t capacity);
#endif
