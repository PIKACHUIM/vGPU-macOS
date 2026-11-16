#ifndef VGPU_GLD_PIXEL_H
#define VGPU_GLD_PIXEL_H

#include <stddef.h>
#include <stdint.h>

/* 独立用户态 C11 API；不是系统 GLD ABI。仅支持 64 位小端目标。 */
typedef enum gp_status {
    GP_OK = 0,
    GP_BAD_ARGUMENT,
    GP_TRUNCATED_VALUE,
    GP_UNTERMINATED,
    GP_UNSUPPORTED_TOKEN,
    GP_BAD_VALUE,
    GP_OVERFLOW,
    GP_UNSUPPORTED_FORMAT,
    GP_NO_MATCH,
    GP_NO_MEMORY
} gp_status;

typedef struct gp_request {
    uint32_t color_min, alpha_min, depth_min, stencil_min;
    uint32_t double_buffer;
    size_t consumed_words; /* 包括首个终止符；其后内容不读取。 */
} gp_request;

/* byte_length 必须是调用方确认可读的实际长度；不探测映射、不无界扫描。
 * 非整词长度拒绝。失败不修改 out。tokens 可未对齐，out 不得与输入重叠。
 * 仅接受 0、4、5、8/value、11/value、12/value、13/value。
 * 4 无值且无副作用；重复尺寸取最大值；负数位模式拒绝。
 */
gp_status gp_parse_normalized(const void *tokens, size_t byte_length,
                              gp_request *out);

enum {
    GP_COLOR_RGB888 = 0x4000,
    GP_COLOR_ARGB8888 = 0x8000,
    GP_FLAG_WINDOW = 0x1,
    GP_FLAG_FULLSCREEN = 0x2,
    GP_FLAG_OFFSCREEN = 0x4,
    GP_FLAG_ACCELERATED = 0x100,
    GP_FLAG_COMPLIANT = 0x400,
    GP_FLAG_CORE32 = 0x20000,
    GP_FLAG_CORE41 = 0x40000,
    GP_BUFFER_MONO_SINGLE = 0x5,
    GP_BUFFER_MONO_DOUBLE = 0x9
};

/* 每条记录是一种已由 backend 证实可用的完整组合，不能传 capability 笛卡尔积。
 * color_mode 仅限上面两种；depth_bits/stencil_bits 仅限 0/8/16/24/32。
 * flags 只接受上面已知位，完全由调用方声明，无默认 accelerated/compliant/core。
 * renderer_id/display_mask 原样写入，不臆造 GLEngine 的 ID 修饰。
 * 表及其记录在 choose 期间必须保持不变且实际可读；空表不产生格式。
 */
typedef struct gp_format_cap {
    uint32_t renderer_id, flags, buffer_mode, color_mode;
    uint32_t depth_bits, stencil_bits, display_mask;
} gp_format_cap;

typedef struct gp_pixel_node {
    struct gp_pixel_node *next;            /* 0x00，供上层复制的前缀 */
    unsigned char fields[0x30];            /* 0x08..0x37，未知字段保持零 */
    size_t backend_format_index;           /* 0x38，独立模块私有尾部 */
} gp_pixel_node;

typedef struct gp_allocator {
    void *context;
    void *(*allocate)(void *context, size_t bytes);
    void (*release)(void *context, void *allocation);
} gp_allocator;

/* 必须以 {0} 初始化并保持唯一所有权。不得复制 owner 后重复销毁。
 * 一次分配容纳整条链；head 是借用视图。上层可复制 0x38 前缀，不能 free 节点。
 * 自定义 allocator 须成对提供，返回符合 C 分配对齐规则的独占存储。
 */
typedef struct gp_pixel_list {
    gp_pixel_node *head;
    size_t count;
    void *allocation;
    gp_allocator allocator;
} gp_pixel_list;

/* 成功按 capability 表顺序枚举匹配项，无评分/恢复/离线策略。
 * 5 要求 double；未出现5时不额外限制 single/double。
 * 尺寸是最小值，ColorSize 的已证消费者定义为32（包括 RGB888）。
 * out 必须为空 owner，失败不改变它；allocator==NULL 使用 malloc/free。
 */
gp_status gp_choose_normalized(const void *tokens, size_t byte_length,
                               const gp_format_cap *formats, size_t format_count,
                               const gp_allocator *allocator, gp_pixel_list *out);

/* 只接收本模块 owner；按 allocation 释放一次，不遍历可能被消费者改写的 next。
 * 释放后清零，可重复销毁同一个已清零 owner，NULL 也可。
 */
void gp_pixel_list_destroy(gp_pixel_list *list);

#endif
