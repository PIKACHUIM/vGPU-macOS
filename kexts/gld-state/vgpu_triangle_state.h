#ifndef VGPU_TRIANGLE_STATE_H
#define VGPU_TRIANGLE_STATE_H

#include "../gld-context/vgpu_context.h"

#define VGPU_TRIANGLE_H_MIN_LENGTH ((size_t)0x4979)
/* 自有解码器的准入约束，不声称恢复了 Apple H 的完整 ABI 对齐。 */
#define VGPU_TRIANGLE_H_ALIGNMENT ((size_t)2)

typedef enum {
    VGPU_TRIANGLE_STATE_CORE_VERIFIED = 0,
    VGPU_TRIANGLE_STATE_BAD_ARGUMENT = 1,
    VGPU_TRIANGLE_STATE_UNSUPPORTED = 2
} vgpu_triangle_state_result;

enum {
    VGPU_TRIANGLE_UNVERIFIED_VERTEX_COLOR = 1u << 0,
    VGPU_TRIANGLE_UNVERIFIED_PROGRAM_TF = 1u << 1,
    VGPU_TRIANGLE_UNVERIFIED_POLYGON_MODE = 1u << 2,
    VGPU_TRIANGLE_UNVERIFIED_TARGET = 1u << 3,
    VGPU_TRIANGLE_UNVERIFIED_OTHER_STATE = 1u << 4
};
#define VGPU_TRIANGLE_UNVERIFIED_ALL UINT32_C(0x1b)

typedef struct {
    float modelview[16];
    float projection[16];
    float current_rgba[4];
    float viewport[4];
    double depth_range[2];
    uint32_t core_raster_state_verified;
    uint32_t all_state_verified; /* 永远为0；不能用核心检查直接授权提交。 */
    uint32_t unverified_state;
} vgpu_triangle_state;

/* 自有只读稀疏解码器，依据 out/gld-triangle-state-contract.txt 第1/2节
 * 及 out/gld-vertex-color-contract.txt 第5节（front/back u16 均须 FILL）。
 * 仅64位小端 IEEE binary32/binary64；不定义 Apple H 的整体 sizeof。
 * 调用者提供真实可读 length，保证调用期间输入稳定、输出可写且正确对齐。
 * H 须满足 VGPU_TRIANGLE_H_ALIGNMENT；未对齐视图为 BAD_ARGUMENT。
 * 逐字段 memcpy，不访问空洞，不持有或修改输入。
 * 拒绝输出与整个声明输入区间的任何重叠（含未读尾部），先检查地址溢出。
 * 只有 CORE_VERIFIED 写 out；失败保持 out 原样，短视图为 BAD_ARGUMENT。
 * blend/scissor/RGBA 位图只约束目标0；stencil/clip/纹理保守要求整值0。
 * 未知附加状态没有被核验：逐顶点颜色历史、program/TF、
 * 真实目标资源及其他状态必须另行闭合；当前红色不证明所有顶点均为红色。
 * 本返回值不是 GL 错误、UpdateDispatch 策略位或 GPU 完成证明。
 */
vgpu_triangle_state_result vgpu_triangle_state_decode(vgc_view h,
                                                       vgpu_triangle_state *out);

/* 自有错误槽，不复用 Apple 私有结构。GL error 是确定宽度的 u32，
 * 与内部失败原因、后端故障及 UpdateDispatch 策略返回类型分离。
 * 受限状态拒绝可由调用方选择 INVALID_OPERATION，不声称该状态违反GL规范。
 * 所有操作须由调用方在同一同步域串行化；指针必须指向有效自有对象。
 */
typedef uint32_t vgpu_gl_error;
#define VGPU_GL_NO_ERROR UINT32_C(0)
#define VGPU_GL_INVALID_OPERATION UINT32_C(0x0502)
#define VGPU_GL_OUT_OF_MEMORY UINT32_C(0x0505)

typedef enum {
    VGPU_TRIANGLE_REJECT_NONE = 0,
    VGPU_TRIANGLE_REJECT_STATE = 1,
    VGPU_TRIANGLE_REJECT_ARGUMENT = 2,
    VGPU_TRIANGLE_REJECT_BACKEND = 3
} vgpu_triangle_reject_reason;

typedef enum {
    VGPU_TRIANGLE_BACKEND_OK = 0,
    VGPU_TRIANGLE_BACKEND_DEVICE_LOST = 1,
    VGPU_TRIANGLE_BACKEND_COMPLETION_UNKNOWN = 2
} vgpu_triangle_backend_fault;

typedef struct {
    vgpu_gl_error pending_gl_error;
    vgpu_triangle_reject_reason reject_reason;
    vgpu_triangle_backend_fault backend_fault;
} vgpu_triangle_error_latch;

/* init 仅用于新对象，不是后端故障恢复接口。record 保留首个非零error、
 * 首个非NONE原因和首个非OK故障；NO_ERROR不清槽。get只取并清error。
 * get 的结果仅供错误查询接口使用，绝不能从 UpdateDispatch 返回。
 */
void vgpu_triangle_error_init(vgpu_triangle_error_latch *latch);
void vgpu_triangle_error_record(vgpu_triangle_error_latch *latch,
                                vgpu_gl_error error,
                                vgpu_triangle_reject_reason reason,
                                vgpu_triangle_backend_fault fault);
vgpu_gl_error vgpu_triangle_error_get(vgpu_triangle_error_latch *latch);

#endif
