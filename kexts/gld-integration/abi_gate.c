#include "abi_gate.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#endif

/* Mach-O 固定宽度小端字段；不用宿主结构体对齐或有符号整数转换解析。 */
enum { HEADER_SIZE = 32, COMMAND_SIZE = 8, UUID_SIZE = 24 };

static uint32_t read_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const unsigned char expected_uuid[GLD_ABI_TARGET_COUNT][16] = {
    {0xfb,0x99,0x4f,0x78,0x34,0xc6,0x39,0xf3,0x85,0x7d,0x99,0xf9,0xb3,0x01,0x3e,0x8f},
    {0xce,0xb3,0xff,0x13,0x17,0x9a,0x3a,0xfd,0xab,0x07,0x31,0xbe,0x4c,0x20,0x73,0xb9}
};

const char *gld_abi_target_path(enum gld_abi_target target)
{
    switch (target) {
    case GLD_ABI_GLENGINE: return GLD_ABI_GLENGINE_PATH;
    case GLD_ABI_GFXSHARED: return GLD_ABI_GFXSHARED_PATH;
    default: return NULL;
    }
}

const char *gld_abi_status_string(enum gld_abi_status status)
{
    switch (status) {
    case GLD_ABI_OK: return "ok";
    case GLD_ABI_INVALID_ARGUMENT: return "invalid_argument";
    case GLD_ABI_UNSUPPORTED_PLATFORM: return "unsupported_platform";
    case GLD_ABI_ENVIRONMENT_NOT_QUIESCENT: return "environment_not_quiescent";
    case GLD_ABI_MISSING_IMAGE: return "missing_image";
    case GLD_ABI_DUPLICATE_IMAGE: return "duplicate_image";
    case GLD_ABI_UNREADABLE_IMAGE: return "unreadable_or_writable_image";
    case GLD_ABI_MALFORMED_MACHO: return "malformed_macho";
    case GLD_ABI_MISSING_UUID: return "missing_uuid";
    case GLD_ABI_DUPLICATE_UUID: return "duplicate_uuid";
    case GLD_ABI_CPU_MISMATCH: return "cpu_mismatch";
    case GLD_ABI_UUID_MISMATCH: return "uuid_mismatch";
    case GLD_ABI_IMAGES_CHANGED: return "images_changed";
    case GLD_ABI_NO_MEMORY: return "no_memory";
    default: return "unknown_status";
    }
}

enum gld_abi_status gld_abi_parse_header(const void *bytes, size_t length,
                                        struct gld_abi_identity *identity)
{
    if (!identity) return GLD_ABI_INVALID_ARGUMENT;
    memset(identity, 0, sizeof(*identity));
    if (!bytes) return GLD_ABI_INVALID_ARGUMENT;
    const unsigned char *p = bytes;
    if (length < HEADER_SIZE || read_u32(p) != UINT32_C(0xfeedfacf))
        return GLD_ABI_MALFORMED_MACHO;
    uint32_t ncmds = read_u32(p + 16);
    size_t remaining = read_u32(p + 20);
    if (remaining > length - HEADER_SIZE || remaining % 8 != 0 ||
        ncmds > remaining / COMMAND_SIZE)
        return GLD_ABI_MALFORMED_MACHO;
    struct gld_abi_identity parsed = {0};
    parsed.cpu_type = read_u32(p + 4);
    parsed.cpu_subtype = read_u32(p + 8);
    p += HEADER_SIZE;
    unsigned uuid_count = 0;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (remaining < COMMAND_SIZE) return GLD_ABI_MALFORMED_MACHO;
        uint32_t command = read_u32(p);
        uint32_t size = read_u32(p + 4);
        if (size < COMMAND_SIZE || size > remaining || size % 8 != 0)
            return GLD_ABI_MALFORMED_MACHO;
        if (command == UINT32_C(0x1b)) {
            if (size != UUID_SIZE) return GLD_ABI_MALFORMED_MACHO;
            memcpy(parsed.uuid, p + COMMAND_SIZE, sizeof(parsed.uuid));
            /* 只需区分零、一、多，避免不必要的计数溢出。 */
            if (uuid_count < 2) ++uuid_count;
        }
        p += size;
        remaining -= size;
    }
    if (remaining != 0) return GLD_ABI_MALFORMED_MACHO;
    if (uuid_count == 0) return GLD_ABI_MISSING_UUID;
    if (uuid_count != 1) return GLD_ABI_DUPLICATE_UUID;
    *identity = parsed;
    return GLD_ABI_OK;
}

enum gld_abi_status gld_abi_match_identity(enum gld_abi_target target,
                                          const struct gld_abi_identity *identity)
{
    if (!identity || !gld_abi_target_path(target)) return GLD_ABI_INVALID_ARGUMENT;
    if (identity->cpu_type != GLD_ABI_CPU_X86_64) return GLD_ABI_CPU_MISMATCH;
    if (memcmp(identity->uuid, expected_uuid[target], 16) != 0)
        return GLD_ABI_UUID_MISMATCH;
    return GLD_ABI_OK;
}

#if defined(__APPLE__)
/* 延续 renderer_contract_probe.c 的逐 VM 区间检查；拒绝空区间、洞、
 * 地址溢出和可写 header。拷贝失败也拒绝，不直接解引用未验证映像。 */
static int readable_readonly_range(uintptr_t p, size_t n)
{
    if (!n || p > UINTPTR_MAX - n) return 0;
    uintptr_t end = p + n;
    while (p < end) {
        mach_vm_address_t base = p;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &base, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || count != VM_REGION_BASIC_INFO_COUNT_64 ||
            base > p || size == 0 || !(info.protection & VM_PROT_READ) ||
            (info.protection & VM_PROT_WRITE) || base > UINTPTR_MAX ||
            size > UINTPTR_MAX - (uintptr_t)base)
            return 0;
        p = (uintptr_t)(base + size);
    }
    return 1;
}

static int copy_loaded_bytes(const void *source, void *destination, size_t length)
{
    if (!readable_readonly_range((uintptr_t)source, length)) return 0;
    mach_vm_size_t copied = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(),
        (mach_vm_address_t)(uintptr_t)source, length,
        (mach_vm_address_t)(uintptr_t)destination, &copied);
    return kr == KERN_SUCCESS && copied == length;
}
#endif

enum gld_abi_status gld_abi_read_loaded_header(const void *header,
                                               struct gld_abi_identity *identity)
{
    if (!identity) return GLD_ABI_INVALID_ARGUMENT;
    memset(identity, 0, sizeof(*identity));
    if (!header) return GLD_ABI_INVALID_ARGUMENT;
#if defined(__APPLE__)
    unsigned char prefix[HEADER_SIZE];
    if (!copy_loaded_bytes(header, prefix, sizeof(prefix)))
        return GLD_ABI_UNREADABLE_IMAGE;
    size_t commands = read_u32(prefix + 20);
    if (read_u32(prefix) != UINT32_C(0xfeedfacf) || commands % 8 != 0 ||
        read_u32(prefix + 16) > commands / COMMAND_SIZE ||
        commands > SIZE_MAX - HEADER_SIZE)
        return GLD_ABI_MALFORMED_MACHO;
    size_t length = HEADER_SIZE + commands;
    if (!readable_readonly_range((uintptr_t)header, length))
        return GLD_ABI_UNREADABLE_IMAGE;
    unsigned char *snapshot = malloc(length);
    if (!snapshot) return GLD_ABI_NO_MEMORY;
    enum gld_abi_status status;
    if (!copy_loaded_bytes(header, snapshot, length))
        status = GLD_ABI_UNREADABLE_IMAGE;
    else if (memcmp(prefix, snapshot, HEADER_SIZE) != 0)
        status = GLD_ABI_IMAGES_CHANGED;
    else
        status = gld_abi_parse_header(snapshot, length, identity);
    free(snapshot);
    return status;
#else
    return GLD_ABI_UNSUPPORTED_PLATFORM;
#endif
}

enum gld_abi_status gld_abi_check_loaded_images(int caller_guarantees_quiescence,
                                               struct gld_abi_report *report)
{
    if (!report) return GLD_ABI_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    for (unsigned t = 0; t < GLD_ABI_TARGET_COUNT; ++t)
        report->images[t].status = GLD_ABI_MISSING_IMAGE;
    if (caller_guarantees_quiescence != 1)
        return report->status = GLD_ABI_ENVIRONMENT_NOT_QUIESCENT;
#if defined(__APPLE__) && defined(__x86_64__)
    if (sizeof(void *) != 8) return report->status = GLD_ABI_CPU_MISMATCH;
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (!name) return report->status = GLD_ABI_IMAGES_CHANGED;
        for (unsigned t = 0; t < GLD_ABI_TARGET_COUNT; ++t) {
            if (strcmp(name, gld_abi_target_path((enum gld_abi_target)t)) != 0)
                continue;
            struct gld_abi_image_result *result = &report->images[t];
            if (++result->occurrences != 1) {
                result->status = GLD_ABI_DUPLICATE_IMAGE;
                continue;
            }
            const struct mach_header *header = _dyld_get_image_header(i);
            result->header_address = (uintptr_t)header;
            result->status = header ? gld_abi_read_loaded_header(header, &result->identity)
                                    : GLD_ABI_IMAGES_CHANGED;
            if (result->status == GLD_ABI_OK)
                result->status = gld_abi_match_identity((enum gld_abi_target)t,
                                                       &result->identity);
        }
    }
    if (_dyld_image_count() != count) return report->status = GLD_ABI_IMAGES_CHANGED;
    for (unsigned t = 0; t < GLD_ABI_TARGET_COUNT; ++t)
        if (report->images[t].status != GLD_ABI_OK)
            return report->status = report->images[t].status;
    return report->status = GLD_ABI_OK;
#elif defined(__APPLE__)
    return report->status = GLD_ABI_CPU_MISMATCH;
#else
    return report->status = GLD_ABI_UNSUPPORTED_PLATFORM;
#endif
}
