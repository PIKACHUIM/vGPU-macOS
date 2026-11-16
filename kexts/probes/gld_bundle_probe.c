#define _DARWIN_C_SOURCE 1
#include "../gld-diagnostic/diagnostic_abi.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct {
    const char *name;
    int is_trap;
} entries[] = {
#define GLD_IMPLEMENTED(name) { #name, 0 },
#define GLD_TRAP(name) { #name, 1 },
#include "../gld-diagnostic/required_entries.inc"
#undef GLD_TRAP
#undef GLD_IMPLEMENTED
};
_Static_assert(sizeof(entries) / sizeof(entries[0]) == 129, "必须有129个入口");
_Static_assert(sizeof(void *) == 8, "仅适用64位目标");

struct guarded_device {
    unsigned char prefix[0x18];
    void *device;
    unsigned char tail[GLD_DIAGNOSTIC_DEVICE_OUTPUT_BYTES];
    unsigned char guard[16];
};
_Static_assert(offsetof(struct guarded_device, device) == 0x18, "设备槽偏移");
_Static_assert(offsetof(struct guarded_device, tail) == 0x20, "第三参数偏移");
_Static_assert(offsetof(struct guarded_device, guard) == 0x1b0, "调用者分配边界");

static void callback_must_not_run(void)
{
    fprintf(stderr, "FAIL pid=%ld unexpected callback\n", (long)getpid());
    _Exit(90);
}

static int uniform(const unsigned char *p, size_t n, unsigned char value)
{
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != value) {
            return 0;
        }
    }
    return 1;
}

static void *checked_symbol(void *handle, const char *name, const char *expected,
                            void **image_base)
{
    dlerror();
    void *symbol = dlsym(handle, name);
    const char *error = dlerror();
    if (error != NULL || symbol == NULL) {
        fprintf(stderr, "FAIL symbol=%s dlsym=%s\n", name, error ? error : "NULL");
        return NULL;
    }
    Dl_info info = {0};
    char resolved[PATH_MAX];
    if (!dladdr(symbol, &info) || info.dli_fname == NULL || info.dli_sname == NULL ||
        realpath(info.dli_fname, resolved) == NULL || strcmp(resolved, expected) != 0 ||
        strcmp(info.dli_sname, name) != 0 || info.dli_saddr != symbol ||
        (*image_base != NULL && info.dli_fbase != *image_base)) {
        fprintf(stderr, "FAIL symbol=%s source-or-name-mismatch\n", name);
        return NULL;
    }
    *image_base = info.dli_fbase;
    printf("SYMBOL name=%s address=%p image=%s\n", name, symbol, resolved);
    return symbol;
}

static int run_cycle(const char *path, unsigned cycle)
{
    /* 第二轮使用目标 loader 的 5 = RTLD_LAZY | RTLD_LOCAL。
     * 第一轮 RTLD_NOW 额外验证所有外部引用可立即解析。 */
    int mode = (cycle == 1 ? RTLD_NOW : RTLD_LAZY) | RTLD_LOCAL;
    void *handle = dlopen(path, mode);
    if (handle == NULL) {
        fprintf(stderr, "FAIL dlopen=%s\n", dlerror());
        return 1;
    }
    printf("OPEN cycle=%u mode=0x%x pid=%ld path=%s\n", cycle, mode, (long)getpid(), path);
    void *base = NULL;
    void *init_address = checked_symbol(handle, "gldInitializeLibrary", path, &base);
    void *term_address = checked_symbol(handle, "gldTerminateLibrary", path, &base);
    void *version_address = NULL;
    void *device_address = NULL;
    int result = 1;
    int initialized = 0;
    gld_diag_initialize_fn initialize = NULL;
    gld_diag_terminate_fn terminate = NULL;
    gld_diag_version_fn version = NULL;
    gld_diag_device_fn create_device = NULL;
    _Static_assert(sizeof(initialize) == sizeof(init_address), "函数地址宽度");
    _Static_assert(sizeof(terminate) == sizeof(term_address), "函数地址宽度");
    _Static_assert(sizeof(version) == sizeof(version_address), "函数地址宽度");
    _Static_assert(sizeof(create_device) == sizeof(device_address), "函数地址宽度");
    if (init_address == NULL || term_address == NULL) {
        goto cleanup;
    }
    unsigned traps = 0;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        void *p = checked_symbol(handle, entries[i].name, path, &base);
        if (p == NULL) {
            goto cleanup;
        }
        if (i == 0) version_address = p;
        if (i == 1) device_address = p;
        if (entries[i].is_trap) {
            /* 核验所有 UD2 字节，不用臆测的 C 签名去调用未知入口。 */
            const unsigned char *code = p;
            if (code[0] != 0x0f || code[1] != 0x0b) {
                fprintf(stderr, "FAIL symbol=%s expected-ud2\n", entries[i].name);
                goto cleanup;
            }
            ++traps;
        }
    }
    if (traps != 127) goto cleanup;
    printf("EXPORTS cycle=%u required=129/129 additional=2 traps=127/127 provenance=PASS\n", cycle);
    memcpy(&initialize, &init_address, sizeof(initialize));
    memcpy(&terminate, &term_address, sizeof(terminate));
    memcpy(&version, &version_address, sizeof(version));
    memcpy(&create_device, &device_address, sizeof(create_device));

    uint32_t services[32] = {0};
    uint8_t bytes[32] = {0};
    void (*callback)(void) = callback_must_not_run;
    uintptr_t callback_bits = 0;
    _Static_assert(sizeof(callback_bits) == sizeof(callback), "回调地址宽度");
    memcpy(&callback_bits, &callback, sizeof(callback_bits));
    initialize(services, bytes, UINT32_C(0x80000001), callback_bits, callback_bits,
               UINT32_C(0x12345678));
    initialized = 1;
    if (!uniform((const unsigned char *)services, sizeof(services), 0) ||
        !uniform(bytes, sizeof(bytes), 0)) {
        fprintf(stderr, "FAIL initialize modified input\n");
        goto cleanup;
    }

    struct { uint32_t before, value, after; } v[4];
    for (unsigned i = 0; i < 4; ++i) {
        v[i].before = UINT32_C(0xa1b2c3d4);
        v[i].value = UINT32_MAX;
        v[i].after = UINT32_C(0xd4c3b2a1);
    }
    int32_t version_rc = version(&v[0].value, &v[1].value, &v[2].value, &v[3].value);
    if (version_rc == 0 || v[0].value != 4 || v[1].value != 1 || v[2].value != 0x11 ||
        v[3].value != GLD_DIAGNOSTIC_DRIVER_FIELD ||
        (v[3].value & UINT32_C(0xffff00ff)) != 0 ||
        (v[3].value & UINT32_C(0x7f00)) == 0x400) {
        fprintf(stderr, "FAIL version contract\n");
        goto cleanup;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (v[i].before != UINT32_C(0xa1b2c3d4) || v[i].after != UINT32_C(0xd4c3b2a1)) {
            fprintf(stderr, "FAIL version output boundary\n");
            goto cleanup;
        }
    }
    printf("VERSION cycle=%u rc=%" PRId32 " fields=4/1/0x11/0x%04" PRIx32
           " driver-id=0x%08" PRIx32 " guards=PASS\n", cycle, version_rc,
           v[3].value, v[3].value | UINT32_C(0x20000));

    const uint32_t masks[] = {0, 1, UINT32_C(0x80000000), UINT32_MAX};
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
        struct guarded_device record;
        memset(&record, 0xa5, sizeof(record));
        record.device = &record;
        int32_t rc = create_device(&record.device, masks[i], record.tail);
        if (rc != GLD_DIAGNOSTIC_DEVICE_REJECT || rc == 0 || record.device != NULL ||
            !uniform(record.tail, sizeof(record.tail), 0) ||
            !uniform(record.prefix, sizeof(record.prefix), 0xa5) ||
            !uniform(record.guard, sizeof(record.guard), 0xa5)) {
            fprintf(stderr, "FAIL device mask=0x%08" PRIx32 " rc=%" PRId32 "\n", masks[i], rc);
            goto cleanup;
        }
        printf("DEVICE cycle=%u mask=0x%08" PRIx32 " rc=%" PRId32
               " handle=NULL zero-bytes=400 guards=PASS\n", cycle, masks[i], rc);
    }
    result = 0;
cleanup:
    if (initialized) {
        terminate();
        printf("TERMINATE cycle=%u PASS\n", cycle);
    }
    if (dlclose(handle) != 0) {
        fprintf(stderr, "FAIL dlclose=%s\n", dlerror());
        result = 1;
    } else {
        printf("DLCLOSE cycle=%u PASS\n", cycle);
    }
    return result;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) {
        fprintf(stderr, "用法: %s bundle/Contents/MacOS/VGPUDriverDiagnostic\n", argv[0]);
        return 2;
    }
    char path[PATH_MAX];
    struct stat st;
    if (realpath(argv[1], path) == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "FAIL bundle executable path\n");
        return 2;
    }
    if (run_cycle(path, 1) || run_cycle(path, 2)) return 1;
    printf("PASS required=129 traps=127 cycles=2 device-rejections=8 cleanup=2 diagnostic-only\n");
    return 0;
}
