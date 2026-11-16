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

/* 依据 out/renderer-properties-research.txt 和 out/opengl-loader.txt。
 * 编译: clang -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations
 *       renderer_contract_probe.c -framework OpenGL -o renderer_contract_probe
 * 仅测试自有合成包装记录；不注册 renderer，不销毁合成 CGL 对象。 */
enum { WRAPPER_SIZE = 0x90, DATA_OFFSET = 8, DATA_SIZE = 0x88 };
_Static_assert(WRAPPER_SIZE == DATA_OFFSET + DATA_SIZE, "wrapper layout");
_Static_assert(sizeof(GLint) == 4, "GLint width");
_Static_assert(UINT_MAX == UINT32_MAX, "unsigned int width");
static _Alignas(16) unsigned char storage[2 * WRAPPER_SIZE];
static unsigned passed, failed;
static const GLint sentinel = INT32_C(0x5a17c0de);

enum kind { RAW, FLAGS, BUFFER, SIGNED16, BYTE, MEMORY, MAJOR, CONSTANT, COUNT };
struct property {
    const char *name;
    CGLRendererProperty id;
    enum kind kind;
    size_t offset;
    uint32_t mask;
};
#define PROP(name, kind, offset, mask) { #name, kCGLRP##name, kind, offset, mask }
static const struct property properties[] = {
    PROP(FullScreen, FLAGS, 0x0c, 0x2),
    PROP(RendererID, RAW, 0x08, 0),
    PROP(Accelerated, FLAGS, 0x0c, 0x100),
    PROP(Robust, FLAGS, 0x0c, 0x40),
    PROP(BackingStore, FLAGS, 0x0c, 0x8),
    PROP(MPSafe, CONSTANT, 0, 0),
    PROP(Window, FLAGS, 0x0c, 0x1),
    PROP(MultiScreen, FLAGS, 0x0c, 0x200),
    PROP(Compliant, FLAGS, 0x0c, 0x400),
    PROP(BufferModes, BUFFER, 0x10, 0),
    PROP(ColorModes, RAW, 0x14, 0),
    PROP(AccumModes, RAW, 0x18, 0),
    PROP(DepthModes, RAW, 0x1c, 0),
    PROP(StencilModes, RAW, 0x20, 0),
    PROP(MaxAuxBuffers, SIGNED16, 0x28, 0),
    PROP(MaxSampleBuffers, SIGNED16, 0x2a, 0),
    PROP(MaxSamples, SIGNED16, 0x2c, 0),
    PROP(SampleModes, RAW, 0x30, 0),
    PROP(SampleAlpha, BYTE, 0x2e, 0),
    PROP(VideoMemory, MEMORY, 0x4c, 0),
    PROP(TextureMemory, MEMORY, 0x54, 0),
    PROP(GPUVertProcCapable, FLAGS, 0x0c, 0x4000),
    PROP(GPUFragProcCapable, FLAGS, 0x0c, 0x8000),
    PROP(RendererCount, COUNT, 0, 0),
    PROP(AcceleratedCompute, FLAGS, 0x0c, 0x10000),
    PROP(VideoMemoryMegabytes, RAW, 0x4c, 0),
    PROP(TextureMemoryMegabytes, RAW, 0x54, 0),
    PROP(MajorGLVersion, MAJOR, 0x0c, 0),
    PROP(RegistryIDLow, RAW, 0x34, 0),
    PROP(RegistryIDHigh, RAW, 0x38, 0),
    PROP(Removable, FLAGS, 0x0c, 0x80000)
};
#undef PROP

/* LC_UUID 要求只读；回调表指针位于可写数据，仅读取时不拒绝写权限。 */
static int readable_range(uintptr_t p, size_t n, int require_readonly)
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
            !(info.protection & VM_PROT_READ) ||
            (require_readonly && (info.protection & VM_PROT_WRITE)) ||
            base > UINTPTR_MAX - size) return 0;
        p = (uintptr_t)(base + size);
    }
    return 1;
}

static uintptr_t callback_slot;
static unsigned callback_gate_checks;
static unsigned cgl_calls;

/* Python: 0x7FFB0616BCBF + 0x3D2C3CB9 = 0x7FFB4342F978。
 * 该槽非空时，CGLDescribeRenderer 在 index 检查前调用 table+0x28。
 * 仅在 UUID 匹配后定位；不写槽或其指向对象。 */
static int locate_callback_slot(intptr_t slide)
{
    const uintptr_t unslid = UINT64_C(0x7ffb4342f978);
    if (slide >= 0) {
        if (unslid > UINTPTR_MAX - (uintptr_t)slide) return 0;
        callback_slot = unslid + (uintptr_t)slide;
    } else {
        /* 避免直接对 INTPTR_MIN 求负。 */
        uintptr_t magnitude = (uintptr_t)(-(slide + 1)) + 1;
        if (unslid < magnitude) return 0;
        callback_slot = unslid - magnitude;
    }
    printf("CALLBACK_SLOT unslid=0x%016" PRIxPTR " slide=%" PRIdPTR
           " runtime=0x%016" PRIxPTR "\n", unslid, slide, callback_slot);
    return 1;
}

static void require_null_callback(void)
{
    ++callback_gate_checks;
    if (!callback_slot ||
        !readable_range(callback_slot, sizeof(uintptr_t), 0)) {
        printf("REFUSE callback slot unreadable; cgl_calls=%u\n", cgl_calls);
        exit(2);
    }
    /* volatile 保证每次检查重新读取；工具单线程，不提供抗外部注入竞态保障。 */
    uintptr_t table = *(const volatile uintptr_t *)callback_slot;
#ifdef RENDERER_PROBE_FORCE_CALLBACK_REFUSAL
    printf("REFUSE forced callback gate (no global write); cgl_calls=%u\n", cgl_calls);
    (void)table;
    exit(2);
#endif
    if (table != 0) {
        printf("REFUSE callback table is non-NULL (0x%016" PRIxPTR
               "); cgl_calls=%u\n", table, cgl_calls);
        exit(2);
    }
}

static int verify_image(void)
{
#if !defined(__x86_64__)
    puts("REFUSE process architecture is not x86_64");
    return 0;
#endif
    if (sizeof(void *) != 8 || sizeof(GLint) != 4) {
        puts("REFUSE process pointer/GLint width mismatch");
        return 0;
    }
    Dl_info symbol;
    if (!dladdr((const void *)CGLDescribeRenderer, &symbol) ||
        !symbol.dli_fbase || !symbol.dli_fname || !symbol.dli_sname ||
        symbol.dli_saddr != (void *)CGLDescribeRenderer ||
        strcmp(symbol.dli_sname, "CGLDescribeRenderer") != 0) {
        puts("REFUSE cannot identify actual CGLDescribeRenderer symbol");
        return 0;
    }
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
            !readable_range((uintptr_t)image, sizeof(struct mach_header_64), 1)) break;
        struct mach_header_64 header;
        memcpy(&header, image, sizeof(header));
        if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_X86_64) break;
        const unsigned char *cursor = (const unsigned char *)image + sizeof(header);
        size_t remaining = header.sizeofcmds;
        if (!readable_range((uintptr_t)cursor, remaining, 1)) break;
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
        if (!locate_callback_slot(_dyld_get_image_vmaddr_slide(i))) {
            puts("REFUSE callback slot address overflows");
            return 0;
        }
        return 1;
    }
    return 0;
}

static GLint bits(uint32_t value)
{
    GLint result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static void reset_records(uint32_t count)
{
    memset(storage, 0, sizeof(storage));
    memcpy(storage, &count, sizeof(count));
}

static void field(unsigned index, size_t offset, uint32_t value, size_t width)
{
    /* 固定 x86_64 小端；窄字段也以 memcpy 写入，避免非对齐类型解引用。 */
    memcpy(storage + index * WRAPPER_SIZE + DATA_OFFSET + offset, &value, width);
}

static void check(const char *name, const char *scenario, int index,
                  CGLRendererProperty property, CGLError expected_error,
                  GLint expected_value)
{
    struct { uint32_t before; GLint value; uint32_t after; } output = {
        UINT32_C(0x11223344), sentinel, UINT32_C(0x55667788)
    };
    unsigned char snapshot[sizeof(storage)];
    memcpy(snapshot, storage, sizeof(snapshot));
    require_null_callback();
    ++cgl_calls;
    CGLError error = CGLDescribeRenderer((CGLRendererInfoObj)(void *)storage,
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

static void run_case(const struct property *p, const char *scenario, uint32_t input)
{
    reset_records(1);
    CGLError error = kCGLNoError;
    GLint expected = 0;
    size_t width = p->kind == SIGNED16 ? 2 : (p->kind == BYTE ? 1 : 4);
    if (p->kind != CONSTANT && p->kind != COUNT) field(0, p->offset, input, width);
    switch (p->kind) {
        case RAW: expected = bits(input); break;
        case FLAGS: expected = (input & p->mask) != 0; break;
        case BUFFER: expected = bits(input & ~UINT32_C(2)); break;
        case SIGNED16:
            expected = (input & 0x8000) ? (GLint)(input & 0xffff) - 65536 :
                                        (GLint)(input & 0xffff);
            if (expected == -1) { error = kCGLBadProperty; expected = sentinel; }
            break;
        case BYTE: expected = (GLint)(input & 0xff); break;
        case MEMORY: expected = input <= 2047 ? (GLint)(input << 20) : INT32_MAX; break;
        case MAJOR: expected = (input & 0x40000) ? 4 : 2 + ((input >> 17) & 1); break;
        case CONSTANT: expected = 1; break;
        case COUNT: expected = 1; break;
    }
    check(p->name, scenario, 0, p->id, error, expected);
}

static void test_static_properties(void)
{
    for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); ++i) {
        const struct property *p = &properties[i];
        uint32_t normal = p->kind == FLAGS ? p->mask :
                          p->kind == MEMORY ? 64 :
                          p->kind == SIGNED16 ? 7 : UINT32_C(0x1234abcd);
        run_case(p, "normal", normal);
        run_case(p, "zero", 0);
        run_case(p, "all_bits", UINT32_MAX);
        if (p->kind != CONSTANT && p->kind != COUNT) {
            unsigned width = p->kind == SIGNED16 ? 16 : (p->kind == BYTE ? 8 : 32);
            for (unsigned bit = 0; bit < width; ++bit) {
                char label[40];
                snprintf(label, sizeof(label), "isolated_bit_%u", bit);
                run_case(p, label, UINT32_C(1) << bit);
            }
        }
        if (p->kind == FLAGS) run_case(p, "all_except_own_bit", ~p->mask);
        if (p->kind == SIGNED16) {
            run_case(p, "minus_1_sentinel", UINT16_MAX);
            run_case(p, "minus_2_returned", UINT16_C(0xfffe));
            run_case(p, "max_positive", INT16_MAX);
        }
        if (p->kind == MEMORY) {
            run_case(p, "2047_MiB", 2047);
            run_case(p, "2048_MiB_saturated", 2048);
            run_case(p, "UINT_MAX_saturated", UINT_MAX);
        }
        if (p->kind == MAJOR) {
            run_case(p, "both_version_bits_4_wins", UINT32_C(0x60000));
            run_case(p, "unrelated_bits", ~UINT32_C(0x60000));
        }
    }
}

static void test_boundaries(void)
{
    reset_records(2);
    field(0, 0x08, UINT32_C(0x12345678), 4);
    field(1, 0x08, UINT32_C(0x87654321), 4);
    check("RendererID", "first_record", 0, kCGLRPRendererID, kCGLNoError,
          bits(UINT32_C(0x12345678)));
    check("RendererID", "last_record_stride_0x90", 1, kCGLRPRendererID, kCGLNoError,
          bits(UINT32_C(0x87654321)));
    check("RendererCount", "first_index", 0, kCGLRPRendererCount, kCGLNoError, 2);
    check("RendererCount", "last_index_uses_O_count", 1, kCGLRPRendererCount,
          kCGLNoError, 2);
    const int invalid[] = { INT_MIN, -1, 2, 3, INT_MAX };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        check("RendererID", "invalid_index_sentinel", invalid[i], kCGLRPRendererID,
              kCGLBadValue, sentinel);
        check("RendererCount", "invalid_index_sentinel", invalid[i], kCGLRPRendererCount,
              kCGLBadValue, sentinel);
    }
    /* 覆盖跳转表中的全部空洞、表外及 53/54 附近的未知值。 */
    const int outside[] = { INT_MIN, -1, 0, 52, 55, 69, 143, 999, INT_MAX };
    for (size_t i = 0; i < sizeof(outside) / sizeof(outside[0]); ++i)
        check("UnknownProperty", "outside_table_sentinel", 0,
              (CGLRendererProperty)outside[i], kCGLBadProperty, sentinel);
    for (int id = 70; id <= 142; ++id) {
        int known = id == kCGLRPDisplayMask || id == kCGLRPOnline;
        for (size_t j = 0; j < sizeof(properties) / sizeof(properties[0]); ++j)
            if (id == (int)properties[j].id) known = 1;
        if (!known) check("UnknownProperty", "table_hole_sentinel", 0,
                          (CGLRendererProperty)id, kCGLBadProperty, sentinel);
    }
    reset_records(0);
    check("RendererCount", "empty_index_0_sentinel", 0, kCGLRPRendererCount,
          kCGLBadValue, sentinel);
    check("RendererID", "empty_index_0_sentinel", 0, kCGLRPRendererID,
          kCGLBadValue, sentinel);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("MODE synthetic ABI tests not hardware acceptance");
    puts("SCOPE single-threaded probe; callback slot read-only access; rechecked before every CGL call; no protection against external injection races");
    if (!verify_image()) {
        puts("REFUSE image UUID/architecture/64-bit validation failed; no CGL tests called");
        return 2;
    }
    require_null_callback();
    puts("GATE PASS pinned OpenGL EEBF4330-76CB-3E0E-AD13-BD031235E27F x86_64 64-bit callback=NULL");
    test_static_properties();
    test_boundaries();
    puts("SKIP OffScreen: global gate not assumed; property not called");
    puts("SKIP DisplayMask: removable-display policy not assumed; property not called");
    puts("SKIP Online: dynamic display intersection/policy not assumed; property not called");
    printf("SUMMARY passed=%u failed=%u skipped=3 static_properties=%zu/31 callback_gate_checks=%u cgl_calls=%u\n",
           passed, failed, sizeof(properties) / sizeof(properties[0]),
           callback_gate_checks, cgl_calls);
    puts("RESULT synthetic ABI tests not hardware acceptance; no renderer registered");
    return failed ? 1 : 0;
}
