#include "abi_gate.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* 隔离编译：clang -std=c11 -Wall -Wextra -Werror abi_gate.c abi_gate_test.c
 * 可额外 -framework OpenGL 验证自然依赖；不调用任何 GL、CGL 或 GLD 函数。
 * 可加 -fsanitize=address,undefined 验证合成输入边界。 */
static unsigned passed, failed;
static const unsigned char engine_uuid[16] = {
    0xfb,0x99,0x4f,0x78,0x34,0xc6,0x39,0xf3,0x85,0x7d,0x99,0xf9,0xb3,0x01,0x3e,0x8f
};
static const unsigned char gfx_uuid[16] = {
    0xce,0xb3,0xff,0x13,0x17,0x9a,0x3a,0xfd,0xab,0x07,0x31,0xbe,0x4c,0x20,0x73,0xb9
};

static void check(const char *label, int condition)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (condition) ++passed;
    else ++failed;
}

static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

static void fixture(unsigned char *bytes)
{
    memset(bytes, 0, 96);
    put32(bytes, UINT32_C(0xfeedfacf));
    put32(bytes + 4, GLD_ABI_CPU_X86_64);
    put32(bytes + 8, UINT32_C(0x80000003));
    put32(bytes + 12, 6);
    put32(bytes + 16, 1);
    put32(bytes + 20, 24);
    put32(bytes + 32, 0x1b);
    put32(bytes + 36, 24);
    memcpy(bytes + 40, engine_uuid, 16);
}

static void expect_parse(const char *label, unsigned char *bytes, size_t length,
                         enum gld_abi_status expected)
{
    struct gld_abi_identity id;
    memset(&id, 0xff, sizeof(id));
    enum gld_abi_status actual = gld_abi_parse_header(bytes, length, &id);
    check(label, actual == expected);
    if (expected != GLD_ABI_OK) {
        struct gld_abi_identity zero = {0};
        check("解析失败清零输出", memcmp(&id, &zero, sizeof(id)) == 0);
    }
}

static void synthetic(void)
{
    unsigned char bytes[96];
    struct gld_abi_identity id;
    fixture(bytes);
    check("有效 GLEngine header", gld_abi_parse_header(bytes, 56, &id) == GLD_ABI_OK);
    check("GLEngine UUID 与 CPU 精确匹配", gld_abi_match_identity(GLD_ABI_GLENGINE, &id) == GLD_ABI_OK);
    check("记录 subtype 全部位", id.cpu_subtype == UINT32_C(0x80000003));
    id.cpu_subtype = UINT32_C(0xffffffff);
    check("不推测 subtype 白名单", gld_abi_match_identity(GLD_ABI_GLENGINE, &id) == GLD_ABI_OK);
    id.cpu_type = 7;
    check("错误 CPU 类型拒绝", gld_abi_match_identity(GLD_ABI_GLENGINE, &id) == GLD_ABI_CPU_MISMATCH);
    id.cpu_type = GLD_ABI_CPU_X86_64;
    id.uuid[15] ^= 1;
    check("错误 UUID 拒绝", gld_abi_match_identity(GLD_ABI_GLENGINE, &id) == GLD_ABI_UUID_MISMATCH);
    memcpy(bytes + 40, gfx_uuid, 16);
    check("有效 libGFXShared header", gld_abi_parse_header(bytes, 56, &id) == GLD_ABI_OK);
    check("libGFXShared UUID 与 CPU 精确匹配", gld_abi_match_identity(GLD_ABI_GFXSHARED, &id) == GLD_ABI_OK);
    check("目标身份不可互换", gld_abi_match_identity(GLD_ABI_GLENGINE, &id) == GLD_ABI_UUID_MISMATCH);
    check("非法目标拒绝", gld_abi_match_identity((enum gld_abi_target)-1, &id) == GLD_ABI_INVALID_ARGUMENT);
    check("GLEngine 精确路径", strcmp(gld_abi_target_path(GLD_ABI_GLENGINE), GLD_ABI_GLENGINE_PATH) == 0);
    check("libGFXShared 精确路径", strcmp(gld_abi_target_path(GLD_ABI_GFXSHARED), GLD_ABI_GFXSHARED_PATH) == 0);

    fixture(bytes);
    put32(bytes + 16, 2); put32(bytes + 20, 48);
    memcpy(bytes + 56, bytes + 32, 24);
    expect_parse("重复相同 LC_UUID 拒绝", bytes, 80, GLD_ABI_DUPLICATE_UUID);
    bytes[64] ^= 1;
    expect_parse("重复不同 LC_UUID 拒绝", bytes, 80, GLD_ABI_DUPLICATE_UUID);
    fixture(bytes); put32(bytes + 36, 32);
    expect_parse("越界 cmdsize 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    put32(bytes + 36, UINT32_MAX);
    expect_parse("最大 cmdsize 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    put32(bytes + 36, 0);
    expect_parse("零 cmdsize 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    put32(bytes + 36, 4);
    expect_parse("过短 cmdsize 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    put32(bytes + 36, 20);
    expect_parse("非八字节对齐 cmdsize 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    put32(bytes + 36, 16);
    expect_parse("错误 UUID 命令长度拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    fixture(bytes); put32(bytes + 32, 2);
    expect_parse("无 UUID 拒绝", bytes, 56, GLD_ABI_MISSING_UUID);
    put32(bytes + 16, 0); put32(bytes + 20, 0);
    expect_parse("无命令无 UUID 拒绝", bytes, 32, GLD_ABI_MISSING_UUID);
    fixture(bytes); put32(bytes + 20, UINT32_MAX);
    expect_parse("超长 sizeofcmds 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    fixture(bytes); put32(bytes + 16, UINT32_MAX);
    expect_parse("超大 ncmds 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    fixture(bytes); put32(bytes + 16, 2);
    expect_parse("命令不足拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    fixture(bytes); put32(bytes + 20, 32);
    expect_parse("命令区剩余字节拒绝", bytes, 64, GLD_ABI_MALFORMED_MACHO);
    fixture(bytes); put32(bytes, UINT32_C(0xcffaedfe));
    expect_parse("反字节序拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    fixture(bytes); put32(bytes, UINT32_C(0xfeedface));
    expect_parse("32 位 header 拒绝", bytes, 56, GLD_ABI_MALFORMED_MACHO);
    fixture(bytes);
    int all_truncated = 1;
    for (size_t n = 0; n < 56; ++n)
        if (gld_abi_parse_header(bytes, n, &id) != GLD_ABI_MALFORMED_MACHO)
            all_truncated = 0;
    check("所有截断长度拒绝", all_truncated);
    unsigned char unaligned[97];
    memcpy(unaligned + 1, bytes, 96);
    check("非对齐缓冲区可解析", gld_abi_parse_header(unaligned + 1, 56, &id) == GLD_ABI_OK);
    check("空输出拒绝", gld_abi_parse_header(bytes, 56, NULL) == GLD_ABI_INVALID_ARGUMENT);
    expect_parse("空输入拒绝", NULL, 56, GLD_ABI_INVALID_ARGUMENT);
    struct gld_abi_report report;
    check("环境未确认拒绝", gld_abi_check_loaded_images(0, &report) == GLD_ABI_ENVIRONMENT_NOT_QUIESCENT);
    check("环境参数必须为一", gld_abi_check_loaded_images(2, &report) == GLD_ABI_ENVIRONMENT_NOT_QUIESCENT);
    check("空报告拒绝", gld_abi_check_loaded_images(1, NULL) == GLD_ABI_INVALID_ARGUMENT);
#if !defined(__APPLE__)
    check("非 Apple 运行时明确拒绝", gld_abi_check_loaded_images(1, &report) == GLD_ABI_UNSUPPORTED_PLATFORM);
    check("非 Apple 映像读取明确拒绝", gld_abi_read_loaded_header(bytes, &id) == GLD_ABI_UNSUPPORTED_PLATFORM);
#endif
}

#if defined(__APPLE__)
static void runtime(void)
{
    struct gld_abi_identity id;
    check("普通进程自身已加载 header 可读取",
          gld_abi_read_loaded_header(_dyld_get_image_header(0), &id) == GLD_ABI_OK);
    printf("SELF cpu=0x%08" PRIx32 " subtype=0x%08" PRIx32 "\n", id.cpu_type, id.cpu_subtype);
    check("无效映像地址安全拒绝", gld_abi_read_loaded_header((const void *)(uintptr_t)1, &id) == GLD_ABI_UNREADABLE_IMAGE);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) { check("页大小可用", 0); return; }
    size_t page = (size_t)page_size;
    unsigned char *region = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                                 MAP_ANON | MAP_PRIVATE, -1, 0);
    if (region == MAP_FAILED) { check("测试映射分配", 0); return; }
    fixture(region);
    check("可写 header 拒绝", gld_abi_read_loaded_header(region, &id) == GLD_ABI_UNREADABLE_IMAGE);
    check("测试映射改只读", mprotect(region, page, PROT_READ) == 0);
    check("只读合成 header 可读取", gld_abi_read_loaded_header(region, &id) == GLD_ABI_OK);
    fixture(region + page);
    check("测试映射改不可读", mprotect(region + page, page, PROT_NONE) == 0);
    check("不可读 header 拒绝", gld_abi_read_loaded_header(region + page, &id) == GLD_ABI_UNREADABLE_IMAGE);
    check("跨不可读页 header 拒绝", gld_abi_read_loaded_header(region + page - 16, &id) == GLD_ABI_UNREADABLE_IMAGE);
    check("测试映射释放", munmap(region, page * 2) == 0);

    struct gld_abi_report report;
    enum gld_abi_status status = gld_abi_check_loaded_images(1, &report);
    printf("GATE %s\n", gld_abi_status_string(status));
    int both_ok = 1;
    for (unsigned t = 0; t < GLD_ABI_TARGET_COUNT; ++t) {
        const struct gld_abi_image_result *r = &report.images[t];
        printf("IMAGE %s status=%s count=%" PRIu32 " cpu=0x%08" PRIx32
               " subtype=0x%08" PRIx32 " UUID=",
               gld_abi_target_path((enum gld_abi_target)t), gld_abi_status_string(r->status),
               r->occurrences, r->identity.cpu_type, r->identity.cpu_subtype);
        for (unsigned j = 0; j < 16; ++j) printf("%02X", r->identity.uuid[j]);
        putchar('\n');
        if (r->status != GLD_ABI_OK) both_ok = 0;
        if (r->occurrences == 0) check("缺失映像明确拒绝", r->status == GLD_ABI_MISSING_IMAGE);
    }
    check("总状态必须同时满足两张映像", (status == GLD_ABI_OK) == both_ok);
    puts("SCOPE 仅单线程普通进程读取；未强制加载 GLEngine；未调用 GPU/GL/GLD");
}
#endif

int main(void)
{
    synthetic();
#if defined(__APPLE__)
    runtime();
#endif
    printf("SUMMARY passed=%u failed=%u\n", passed, failed);
    return failed ? 1 : 0;
}
