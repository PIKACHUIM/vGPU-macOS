#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 依据 out/opengl-loader.txt 实际指令及客体跳转表，非本地模拟实现自证。
 * 编译: clang -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations
 *       pixel_format_contract_probe.c -framework OpenGL -o pixel_format_contract_probe
 * 仅调用真实 CGLDescribePixelFormat；不创建、注册、retain 或销毁合成系统对象。
 * 地址均为 UUID EEBF4330-76CB-3E0E-AD13-BD031235E27F 的 unslid 地址。
 * 0x7ffb0616a5c5: movzbl (%rdi),%eax；a5c8 cmp %esi,%eax；a5ca jbe。
 * count 是 P[0] 的一个字节，不是与 P[3] 重叠的 uint32_t。
 * a5d1: s<<6；a5f9..a5fd: D=P+0x40*s+8；a5da: profile 始终取 P[3]。
 */
enum { STRIDE = 0x40, DATA_OFFSET = 8, RECORDS = 255 };
_Static_assert(sizeof(GLint) == 4, "GLint width");
_Static_assert(UINT_MAX == UINT32_MAX, "unsigned int width");
_Static_assert(STRIDE == DATA_OFFSET + 0x38, "wrapper layout");
static _Alignas(16) unsigned char storage[RECORDS * STRIDE];
static unsigned passed, failed;
static const GLint sentinel = INT32_C(0x5a17c0de);

/* 沿用 renderer_contract_probe 的只读映射/LC_UUID 门禁，不修改原文件。 */
static int readonly_range(uintptr_t p, size_t n)
{
    if (n == 0 || p > UINTPTR_MAX - n) return 0;
    uintptr_t end = p + n;
    while (p < end) {
        mach_vm_address_t base = p;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t result = mach_vm_region(mach_task_self(), &base, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || base > p || size == 0 ||
            !(info.protection & VM_PROT_READ) || (info.protection & VM_PROT_WRITE) ||
            base > UINTPTR_MAX - size) return 0;
        p = (uintptr_t)(base + size);
    }
    return 1;
}

static uintptr_t callback_slot;
static unsigned cgl_calls;

static void require_null_callback(void)
{
    uintptr_t table = 0;
    mach_vm_size_t copied = 0;
    if (!callback_slot || mach_vm_read_overwrite(mach_task_self(), callback_slot,
            sizeof(table), (mach_vm_address_t)(uintptr_t)&table, &copied) != KERN_SUCCESS ||
            copied != sizeof(table)) {
        printf("REFUSE callback slot unreadable; cgl_calls=%u\n", cgl_calls);
        exit(2);
    }
#ifdef PIXEL_PROBE_FORCE_CALLBACK_REFUSAL
    printf("REFUSE forced callback gate (no global write); cgl_calls=%u\n", cgl_calls);
    exit(2);
#endif
    if (table) {
        printf("REFUSE callback table non-NULL; cgl_calls=%u\n", cgl_calls);
        exit(2);
    }
    /* Single-threaded diagnostic; no protection against external injection races. */
}

static int verify_image(void)
{
#if !defined(__x86_64__)
    puts("REFUSE process architecture is not x86_64");
    return 0;
#endif
    if (sizeof(void *) != 8 || sizeof(GLint) != 4) return 0;
    Dl_info symbol;
    if (!dladdr((const void *)CGLDescribePixelFormat, &symbol) ||
        !symbol.dli_fbase || !symbol.dli_fname || !symbol.dli_sname ||
        symbol.dli_saddr != (void *)CGLDescribePixelFormat ||
        strcmp(symbol.dli_sname, "CGLDescribePixelFormat") != 0) return 0;
    const unsigned char expected[16] = {
        0xee,0xbf,0x43,0x30,0x76,0xcb,0x3e,0x0e,
        0xad,0x13,0xbd,0x03,0x12,0x35,0xe2,0x7f
    };
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const struct mach_header *image = _dyld_get_image_header(i);
        if ((const void *)image != symbol.dli_fbase) continue;
        const char *name = _dyld_get_image_name(i);
        if (!name || !strstr(name, "/OpenGL.framework/Versions/A/OpenGL") ||
            strcmp(name, symbol.dli_fname) != 0 ||
            !readonly_range((uintptr_t)image, sizeof(struct mach_header_64))) break;
        struct mach_header_64 header;
        memcpy(&header, image, sizeof(header));
        if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_X86_64) break;
        const unsigned char *cursor = (const unsigned char *)image + sizeof(header);
        size_t remaining = header.sizeofcmds;
        if (!readonly_range((uintptr_t)cursor, remaining)) break;
        unsigned uuid_count = 0;
        unsigned char actual[16] = {0};
        for (uint32_t j = 0; j < header.ncmds; ++j) {
            struct load_command command;
            if (remaining < sizeof(command)) return 0;
            memcpy(&command, cursor, sizeof(command));
            if (command.cmdsize < sizeof(command) || command.cmdsize > remaining)
                return 0;
            if (command.cmd == LC_UUID) {
                struct uuid_command uuid;
                if (command.cmdsize != sizeof(uuid)) return 0;
                memcpy(&uuid, cursor, sizeof(uuid));
                memcpy(actual, uuid.uuid, sizeof(actual));
                ++uuid_count;
            }
            remaining -= command.cmdsize;
            cursor += command.cmdsize;
        }
        printf("IMAGE %s arch=x86_64 pointer_bits=%zu UUID=", name, sizeof(void *) * 8);
        for (size_t j = 0; j < sizeof(actual); ++j) {
            printf("%02X", actual[j]);
            if (j == 3 || j == 5 || j == 7 || j == 9) putchar('-');
        }
        putchar('\n');
        if (remaining != 0 || uuid_count != 1 ||
            memcmp(actual, expected, sizeof(actual)) != 0) return 0;
        /* 再核对已读取的真实边界指令，拒绝在未知计数语义下调用。 */
        uintptr_t entry = (uintptr_t)symbol.dli_saddr;
        const unsigned char bounds[] = {0x85,0xf6,0x78,0x53,0x0f,0xb6,0x07,
                                       0x39,0xf0,0x76,0x4c};
        if (entry < 0x39f || !readonly_range(entry - 0x39f, sizeof(bounds))) return 0;
        if (memcmp((const void *)(entry - 0x39f), bounds, sizeof(bounds)) != 0) return 0;
        /* a989 + 0x3d2c4fef = 0x7ffb4342f978: wrapper hook table. */
        const uintptr_t unslid = UINT64_C(0x7ffb4342f978);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        if (slide >= 0) {
            if (unslid > UINTPTR_MAX - (uintptr_t)slide) return 0;
            callback_slot = unslid + (uintptr_t)slide;
        } else {
            uintptr_t magnitude = (uintptr_t)(-(slide + 1)) + 1;
            if (unslid < magnitude) return 0;
            callback_slot = unslid - magnitude;
        }
        require_null_callback();
        return 1;
    }
    return 0;
}

static void reset_records(unsigned char count, unsigned char profile)
{
    memset(storage, 0, sizeof(storage));
    storage[0] = count;
    storage[3] = profile;
}

static void field(unsigned index, size_t offset, uint32_t value, size_t width)
{
    memcpy(storage + index * STRIDE + DATA_OFFSET + offset, &value, width);
}

static void check(const char *name, const char *scenario, int index,
                  CGLPixelFormatAttribute property, CGLError expected_error,
                  GLint expected_value)
{
    struct { uint32_t before; GLint value; uint32_t after; } output = {
        UINT32_C(0x11223344), sentinel, UINT32_C(0x55667788)
    };
    unsigned char snapshot[sizeof(storage)];
    memcpy(snapshot, storage, sizeof(snapshot));
    require_null_callback();
    ++cgl_calls;
    CGLError error = CGLDescribePixelFormat((CGLPixelFormatObj)(void *)storage,
                                            index, property, &output.value);
    int intact = output.before == UINT32_C(0x11223344) &&
                 output.after == UINT32_C(0x55667788) &&
                 memcmp(snapshot, storage, sizeof(storage)) == 0;
    int ok = error == expected_error && output.value == expected_value && intact;
    printf("%s %s/%s index=%d error=%d value=0x%08" PRIx32,
           ok ? "PASS" : "FAIL", name, scenario, index, (int)error,
           (uint32_t)output.value);
    if (ok) ++passed;
    else {
        ++failed;
        printf(" expected_error=%d expected_value=0x%08" PRIx32 " intact=%d",
               (int)expected_error, (uint32_t)expected_value, intact);
    }
    putchar('\n');
}

struct vector { uint32_t input; GLint expected; };
static void vectors(const char *name, CGLPixelFormatAttribute attr, size_t offset,
                    size_t width, const struct vector *v, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        char label[48];
        snprintf(label, sizeof(label), "D+0x%02zx_input_0x%08" PRIx32, offset, v[i].input);
        reset_records(2, 0);
        /* 两条记录用不同值验证步长；第二条头部不参与全局属性计算。 */
        field(0, offset, 0, width);
        field(1, offset, v[i].input, width);
        check(name, label, 1, attr, v[i].expected == -1 ? kCGLBadAttribute : kCGLNoError,
              v[i].expected == -1 ? sentinel : v[i].expected);
    }
}
#define VECTORS(name, offset, width, v) \
    vectors(#name, kCGLPFA##name, offset, width, v, sizeof(v) / sizeof((v)[0]))

static void test_bitmaps(void)
{
    /* 固定期望来自 0x7ffb06162e91..62ee3 的分支和立即数。 */
    static const struct vector color[] = {
        {0,0}, {1,0}, {2,0}, {4,8}, {0x20,8}, {0x40,16}, {0x2000,16},
        {0x4000,32}, {0x8000,32}, {0x10000,32}, {0x80000,32},
        {0x100000,64}, {0x2000000,64}, {0x4000000,128}, {0x8000000,128},
        {0x10000000,256}, {0x20000000,256}, {0xc0000000,0},
        {0xffffffff,8}, {0x8040,16}, {0x108000,32}, {0x4100000,64},
        {0x14000000,128}, {24,8}
    };
    /* 0x62f13..62f70：1/2/4/8/12/16/32/64 的优先级。
     * ARGB8888(0x8000) 由 test $0x9a928 路径得到 alpha=8。 */
    static const struct vector alpha[] = {
        {0,0}, {0x4000,0}, {0x8000,8}, {0x10000,8}, {0x400,1},
        {0x40000,2}, {0x80,4}, {0x200000,12}, {0x800000,16},
        {0x2000000,16}, {0x8000000,32}, {0x20000000,64},
        {0x10000000,0}, {0xffffffff,1}, {0x48000,2}, {0x8080,4},
        {0x208000,8}, {0x2800000,16}, {0x28000000,32}
    };
    /* 0x62ee4..62f12：仅 bit12/11/10/7 转为 32/24/16/8。 */
    static const struct vector depth[] = {
        {0,0}, {1,0}, {8,0}, {16,0}, {24,0}, {32,0},
        {0x80,8}, {0x400,16}, {0x800,24}, {0x1000,32},
        {0x480,16}, {0xc80,24}, {0x1c80,32}, {0xffffffff,32},
        {0xffffe37f,0}
    };
    VECTORS(ColorSize, 0x14, 4, color);
    VECTORS(AccumSize, 0x18, 4, color);
    VECTORS(AlphaSize, 0x14, 4, alpha);
    VECTORS(DepthSize, 0x1c, 4, depth);
    VECTORS(StencilSize, 0x20, 4, depth);
}

static void test_samples(void)
{
    /* a687/a6dd/a6ef movswl 后统一经 a772 cmp $-1 判无效属性。 */
    static const struct vector narrow[] = {
        {0,0}, {1,1}, {4,4}, {8,8}, {0x7fff,32767},
        {0x8000,-32768}, {0xfffe,-2}, {0xffff,-1}, {0x12340004,4}
    };
    static const struct vector byte[] = {
        {0,0}, {1,1}, {0x7f,127}, {0x80,128}, {0xff,255}, {0x123401,1}
    };
    VECTORS(AuxBuffers, 0x26, 2, narrow);
    VECTORS(SampleBuffers, 0x28, 2, narrow);
    VECTORS(Samples, 0x2a, 2, narrow);
    VECTORS(SampleAlpha, 0x30, 1, byte);
    reset_records(2, 0);
    field(1, 0x26, 3, 2);
    field(1, 0x28, 1, 2);
    field(1, 0x2a, 8, 2);
    field(1, 0x2c, 3, 4);
    field(1, 0x30, 255, 1);
    check("AuxBuffers", "adjacent_fields", 1, kCGLPFAAuxBuffers, kCGLNoError, 3);
    check("SampleBuffers", "adjacent_fields", 1, kCGLPFASampleBuffers, kCGLNoError, 1);
    check("Samples", "adjacent_fields", 1, kCGLPFASamples, kCGLNoError, 8);
    check("SampleAlpha", "adjacent_fields", 1, kCGLPFASampleAlpha, kCGLNoError, 255);
}

static void test_flags(void)
{
    /* 每个掩码均对应 a6xx..a7cc 的移位/&1；不调用 RemotePBuffer 的 CGS 路径。 */
    static const struct { const char *name; CGLPixelFormatAttribute attr;
                         size_t offset; unsigned bit; } flags[] = {
        {"Accelerated", kCGLPFAAccelerated, 0x0c, 8},
        {"AcceleratedCompute", kCGLPFAAcceleratedCompute, 0x0c, 16},
        {"DoubleBuffer", kCGLPFADoubleBuffer, 0x10, 3},
        {"TripleBuffer", kCGLPFATripleBuffer, 0x10, 4},
        {"Stereo", kCGLPFAStereo, 0x10, 1},
        {"Multisample", kCGLPFAMultisample, 0x2c, 1},
        {"Supersample", kCGLPFASupersample, 0x2c, 0},
        {"Window", kCGLPFAWindow, 0x0c, 0},
        {"FullScreen", kCGLPFAFullScreen, 0x0c, 1},
        {"OffScreen", kCGLPFAOffScreen, 0x0c, 2},
        {"BackingStore", kCGLPFABackingStore, 0x0c, 3},
        {"BackingVolatile", kCGLPFABackingVolatile, 0x0c, 4},
        {"Robust", kCGLPFARobust, 0x0c, 6},
        {"MultiScreen", kCGLPFAMultiScreen, 0x0c, 9},
        {"Compliant", kCGLPFACompliant, 0x0c, 10},
        {"AuxDepthStencil", kCGLPFAAuxDepthStencil, 0x0c, 11},
        {"PBuffer", kCGLPFAPBuffer, 0x0c, 13}
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            char label[40];
            snprintf(label, sizeof(label), "isolated_bit_%u", bit);
            reset_records(2, 0);
            field(1, flags[i].offset, UINT32_C(1) << bit, 4);
            check(flags[i].name, label, 1, flags[i].attr, kCGLNoError,
                  bit == flags[i].bit);
        }
        reset_records(1, 0);
        check(flags[i].name, "zero", 0, flags[i].attr, kCGLNoError, 0);
        field(0, flags[i].offset, UINT32_MAX, 4);
        check(flags[i].name, "all_bits", 0, flags[i].attr, kCGLNoError, 1);
    }
}

static void test_profile(void)
{
    static const struct { unsigned char header; GLint profile; GLint window; } v[] = {
        {0,0x1000,1}, {0x10,0x3200,0}, {0x20,0x4100,0}, {0x30,0x4100,0},
        {0xcf,0x1000,1}, {0xdf,0x3200,0}, {0xef,0x4100,0}, {0xff,0x4100,0}
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
        reset_records(2, v[i].header);
        storage[1] = 0xff;
        storage[2] = 0xff;
        storage[STRIDE + 3] = (unsigned char)(v[i].header ^ 0x30);
        for (int s = 0; s < 2; ++s) {
            char label[48];
            snprintf(label, sizeof(label), "P3_0x%02x_Dflags_profile_disagree", v[i].header);
            /* 与头部独立的能力位不能改变公开 profile。 */
            field((unsigned)s, 0x0c, 0x60101, 4);
            check("OpenGLProfile", label, s, kCGLPFAOpenGLProfile, kCGLNoError, v[i].profile);
            check("Accelerated", "core_filter_preserves_bit8", s, kCGLPFAAccelerated,
                  kCGLNoError, 1);
            check("Window", "core_filter_clears_bit0", s, kCGLPFAWindow,
                  kCGLNoError, v[i].window);
            check("VirtualScreenCount", "only_P0_byte", s, kCGLPFAVirtualScreenCount,
                  kCGLNoError, 2);
        }
        check("OpenGLProfile", "index_2_not_expanded_by_header", 2,
              kCGLPFAOpenGLProfile, kCGLBadValue, sentinel);
        reset_records(1, v[i].header);
        check("OpenGLProfile", "no_node_capability_bits", 0, kCGLPFAOpenGLProfile,
              kCGLNoError, v[i].profile);
    }
}

static void test_boundaries(void)
{
    reset_records(255, 0x30);
    storage[1] = 0xff;
    storage[2] = 0xff;
    field(0, 0x08, 0x12345678, 4);
    field(1, 0x08, 0x23456789, 4);
    field(254, 0x08, 0x3456789a, 4);
    check("RendererID", "D_equals_P_plus_8", 0, kCGLPFARendererID,
          kCGLNoError, 0x12345678);
    check("RendererID", "stride_0x40", 1, kCGLPFARendererID,
          kCGLNoError, 0x23456789);
    check("RendererID", "last_allocated_record_254", 254, kCGLPFARendererID,
          kCGLNoError, 0x3456789a);
    check("VirtualScreenCount", "unsigned_byte_255", 254, kCGLPFAVirtualScreenCount,
          kCGLNoError, 255);
    const int invalid[] = {INT_MIN, -1, 255, 256, INT_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        check("RendererID", "invalid_index", invalid[i], kCGLPFARendererID,
              kCGLBadValue, sentinel);
        check("VirtualScreenCount", "invalid_index", invalid[i], kCGLPFAVirtualScreenCount,
              kCGLBadValue, sentinel);
        check("UnknownAttribute", "invalid_index_precedes_attribute", invalid[i],
              (CGLPixelFormatAttribute)999, kCGLBadValue, sentinel);
    }
    reset_records(0, 0xff);
    storage[1] = 1;
    storage[2] = 0xff;
    check("VirtualScreenCount", "zero_low_byte_upper_bytes_nonzero", 0,
          kCGLPFAVirtualScreenCount, kCGLBadValue, sentinel);
    check("OpenGLProfile", "zero_count_core_bits", 0,
          kCGLPFAOpenGLProfile, kCGLBadValue, sentinel);
    reset_records(2, 0);
    check("RendererID", "exact_count_index_2", 2, kCGLPFARendererID,
          kCGLBadValue, sentinel);
    /* 客体跳转表中明确指向 a777 的所有洞，加表外属性。 */
    const int holes[] = {INT_MIN,-1,0,2,4,9,10,15,16,17,18,19,20,21,22,23,24,
        25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,
        47,48,49,50,62,63,64,65,66,67,68,69,79,82,85,86,87,88,89,92,93,
        94,95,98,100,102,127,129,999,INT_MAX};
    for (size_t i = 0; i < sizeof(holes) / sizeof(holes[0]); ++i)
        check("UnknownAttribute", "table_hole_or_outside", 1,
              (CGLPixelFormatAttribute)holes[i], kCGLBadAttribute, sentinel);
    static const struct vector raw[] = {
        {0,0}, {0x12345678,0x12345678}, {0x80000000,INT32_MIN},
        {0xfffffffe,-2}, {0xffffffff,-1}
    };
    VECTORS(RendererID, 0x08, 4, raw);
    VECTORS(DisplayMask, 0x34, 4, raw);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("MODE synthetic ABI tests not hardware acceptance");
    if (!verify_image()) {
        puts("REFUSE UUID/architecture/64-bit/bounds-instruction gate failed; no CGL tests called");
        return 2;
    }
    puts("GATE PASS pinned OpenGL EEBF4330-76CB-3E0E-AD13-BD031235E27F x86_64 64-bit");
    puts("EVIDENCE 0x7ffb0616a5c1 bytes=85f678530fb60739f0764c: negative reject, movzbl P[0], unsigned count<=index reject");
    puts("CORRECTION count is uint8 P[0]; P[1..3] do not widen count; P[3] profile is separate");
    puts("EVIDENCE a5d1 stride=0x40; a5fd D=B+8; a772 -1 rejects with kCGLBadAttribute=10000");
    test_bitmaps();
    test_samples();
    test_flags();
    test_profile();
    test_boundaries();
    puts("SKIP RemotePBuffer: dynamic CGSMainConnectionID branch; attribute not called");
    puts("SKIP selection/recovery/offline policy attributes: outside requested static output contract; not called");
    printf("SUMMARY passed=%u failed=%u skipped_groups=2\n", passed, failed);
    puts("RESULT synthetic ABI tests not hardware acceptance; no synthetic system object created/registered/destroyed");
    return failed ? 1 : 0;
}
