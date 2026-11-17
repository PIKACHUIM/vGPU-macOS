#ifndef VGPU_GLD_ABI_GATE_H
#define VGPU_GLD_ABI_GATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLD_ABI_CPU_X86_64 UINT32_C(0x01000007)
#define GLD_ABI_GLENGINE_PATH "/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLEngine.bundle/GLEngine"
#define GLD_ABI_GFXSHARED_PATH "/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGFXShared.dylib"

enum gld_abi_status {
    GLD_ABI_OK = 0,
    GLD_ABI_INVALID_ARGUMENT,
    GLD_ABI_UNSUPPORTED_PLATFORM,
    GLD_ABI_ENVIRONMENT_NOT_QUIESCENT,
    GLD_ABI_MISSING_IMAGE,
    GLD_ABI_DUPLICATE_IMAGE,
    GLD_ABI_UNREADABLE_IMAGE,
    GLD_ABI_MALFORMED_MACHO,
    GLD_ABI_MISSING_UUID,
    GLD_ABI_DUPLICATE_UUID,
    GLD_ABI_CPU_MISMATCH,
    GLD_ABI_UUID_MISMATCH,
    GLD_ABI_IMAGES_CHANGED,
    GLD_ABI_NO_MEMORY
};

enum gld_abi_target {
    GLD_ABI_GLENGINE = 0,
    GLD_ABI_GFXSHARED,
    GLD_ABI_TARGET_COUNT
};

struct gld_abi_identity {
    uint32_t cpu_type;
    /* 保留完整原始值（含 capability bits）；未经验证，不按 subtype 白名单放行。 */
    uint32_t cpu_subtype;
    unsigned char uuid[16];
};

struct gld_abi_image_result {
    enum gld_abi_status status;
    struct gld_abi_identity identity;
    uintptr_t header_address;
    uint32_t occurrences;
};

struct gld_abi_report {
    enum gld_abi_status status;
    struct gld_abi_image_result images[GLD_ABI_TARGET_COUNT];
};

/* 纯解析器：bytes 指向调用方持有的、至少 length 字节的稳定可读缓冲区。
 * 仅接受小端 MH_MAGIC_64；严格验证命令边界、8 字节对齐、命令总长度，
 * 且必须恰有一个长度正确的 LC_UUID。不解释其他命令的语义。
 * identity 在结构错误时清零；CPU/UUID 策略由 match_identity 单独执行。
 * 输出结构不得与输入缓冲区重叠。此接口不读取磁盘或访问 dyld。 */
enum gld_abi_status gld_abi_parse_header(const void *bytes, size_t length,
                                        struct gld_abi_identity *identity);
enum gld_abi_status gld_abi_match_identity(enum gld_abi_target target,
                                          const struct gld_abi_identity *identity);
const char *gld_abi_target_path(enum gld_abi_target target);
const char *gld_abi_status_string(enum gld_abi_status status);

/* 读取本进程已加载的 header；逐区间要求可读且不可写，再用公开 Mach API
 * 拷贝稳定解析快照。调用方必须保证映像在整个调用期间不卸载/重映射。
 * 此函数也可对普通进程自身的 dyld header 做无 GPU 的隔离验证。 */
enum gld_abi_status gld_abi_read_loaded_header(const void *header,
                                               struct gld_abi_identity *identity);

/* caller_guarantees_quiescence 必须等于 1，否则拒绝。
 * 调用环境：正常线程上下文（非信号处理器、非 dyld 回调/构造函数重入）；
 * 调用方已串行化所有映像加载/卸载及相关内存映射变更，目标已自然加载。
 * 公开 dyld 枚举不提供原子快照；计数复核只能发现部分变化，不能排除 ABA，
 * 也不能替代上述环境保证。这里不注册回调、不 dlopen、不调用 GL/GLD。
 * 返回结果仅描述本次检查；不得在可能卸载/替换后作为长期授权缓存。
 * OK 只表示路径+UUID+CPU 类型匹配，不代表全部私有 ABI 或硬件验收闭合。
 * 非 Apple 平台明确返回 UNSUPPORTED_PLATFORM。 */
enum gld_abi_status gld_abi_check_loaded_images(int caller_guarantees_quiescence,
                                               struct gld_abi_report *report);

#ifdef __cplusplus
}
#endif
#endif
