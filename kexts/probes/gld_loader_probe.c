#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Build: clang -std=c11 -Wall -Wextra -Werror -O0 -framework OpenGL
 * gld_loader_probe.c -o gld_loader_probe
 * Version-pinned read-only diagnostic for macOS 15.7.3 / 24G419 x86_64h.
 * Never invokes private functions or loads third-party bundles. */
static size_t entry_count;
static size_t gpu_path_count;
static int gfx_uuid_matches;

/* 只读取本进程正常链接映像中的常量，不解析或调用私有函数。 */
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
        kern_return_t kr = mach_vm_region(mach_task_self(), &base, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || base > p || size == 0 ||
            !(info.protection & VM_PROT_READ) || (info.protection & VM_PROT_WRITE) ||
            base > UINTPTR_MAX - size) return 0;
        p = (uintptr_t)(base + size);
    }
    return 1;
}

static int image_uuid_matches(const struct mach_header_64 *h,
                              const unsigned char expected[16])
{
    if (!readonly_range((uintptr_t)h, sizeof(*h)) || h->magic != MH_MAGIC_64 ||
        !readonly_range((uintptr_t)(h + 1), h->sizeofcmds)) return 0;
    const unsigned char *cmd = (const unsigned char *)(h + 1);
    size_t remaining = h->sizeofcmds;
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        if (remaining < sizeof(struct load_command)) return 0;
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmdsize < sizeof(*lc) || lc->cmdsize > remaining) return 0;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            const struct uuid_command *u = (const struct uuid_command *)cmd;
            return memcmp(u->uuid, expected, 16) == 0;
        }
        remaining -= lc->cmdsize;
        cmd += lc->cmdsize;
    }
    return 0;
}

static void quoted(const unsigned char *p, size_t n)
{
    putchar('"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c >= 32 && c <= 126) putchar(c);
        else printf("\\x%02x", c);
    }
    putchar('"');
}

/* macOS 15.7.3 / 24G419 x86_64h: RIP target from gpu_bundle_find_trusted.
 * Diagnostic only: the caller logs UUID/base; every pointer is range checked. */
static void dump_gpu_paths(intptr_t slide)
{
    const uintptr_t unslid = UINT64_C(0x7ff8412bbfe0);
    uintptr_t mapped = unslid + (uintptr_t)slide;
    if (!readonly_range(mapped, 2 * sizeof(uintptr_t))) {
        puts("SKIP GPU path table is not mapped read-only");
        return;
    }
    for (size_t i = 0; i < 2; ++i) {
        uintptr_t text;
        memcpy(&text, (const void *)(mapped + i * sizeof(text)), sizeof(text));
        size_t n = 0;
        while (n < 1024 && text <= UINTPTR_MAX - n &&
               readonly_range(text + n, 1)) {
            if (*(const unsigned char *)(text + n) == 0) break;
            ++n;
        }
        if (n == 1024 || text > UINTPTR_MAX - n ||
            !readonly_range(text + n, 1)) {
            puts("SKIP GPU path string is not bounded read-only data");
            continue;
        }
        printf("GPU_PATH index=%zu table_unslid=0x%" PRIxPTR " ", i, unslid);
        quoted((const unsigned char *)text, n);
        putchar('\n');
        ++gpu_path_count;
    }
}

static void dump_section(const struct section_64 *s, intptr_t slide,
                         uint64_t cstring_addr, uint64_t cstring_size)
{
    uintptr_t p = (uintptr_t)s->addr + (uintptr_t)slide;
    printf("SECTION %.16s,%.16s unslid=0x%016" PRIx64 " size=0x%" PRIx64
           " mapped=0x%016" PRIxPTR "\n", s->segname, s->sectname, s->addr, s->size, p);
    if (s->size > SIZE_MAX || !readonly_range(p, (size_t)s->size)) {
        puts("SKIP section is not wholly mapped read-only");
        return;
    }
    const unsigned char *data = (const unsigned char *)p;
    size_t size = (size_t)s->size;
    if (strncmp(s->sectname, "__cfstring", 16) == 0) {
        for (size_t off = 0; off + 32 <= size; off += 32) {
            uint64_t words[4];
            memcpy(words, data + off, sizeof(words));
            printf("CF unslid=0x%016" PRIx64 " flags=0x%" PRIx64
                   " pointer_unslid=0x%016" PRIx64 " length=%" PRIu64,
                   s->addr + off, words[1], words[2] - (uint64_t)slide, words[3]);
            /* 仅对常见的八位常量字符串记录读取有界只读字符区。 */
            if ((words[1] & 0xfff) == 0x7c8 && words[3] <= 4096 &&
                readonly_range((uintptr_t)words[2], (size_t)words[3] + 1)) {
                putchar(' ');
                quoted((const unsigned char *)(uintptr_t)words[2], (size_t)words[3]);
            }
            putchar('\n');
        }
        return;
    }
    /* 此构建的相对入口表地址和长度来自加载循环的指令证据。 */
    if (gfx_uuid_matches && s->addr == UINT64_C(0x7ffb0617ae40) && size >= 0x204) {
        for (size_t i = 0; i < 129; ++i) {
            int32_t relative;
            memcpy(&relative, data + i * 4, sizeof(relative));
            uint64_t target = (uint64_t)((int64_t)s->addr + relative);
            if (target < cstring_addr || target - cstring_addr >= cstring_size) return;
            uintptr_t mapped = (uintptr_t)target + (uintptr_t)slide;
            size_t available = (size_t)(cstring_size - (target - cstring_addr));
            if (!readonly_range(mapped, available)) return;
            const unsigned char *text = (const unsigned char *)mapped;
            const unsigned char *nul = memchr(text, 0, available);
            if (!nul) return;
            printf("ENTRY index=%zu slot=0x%zx rel_unslid=0x%016" PRIx64
                   " name_unslid=0x%016" PRIx64 " ",
                   i, 0x420 + i * 8, s->addr + i * 4, target);
            quoted(text, (size_t)(nul - text));
            putchar('\n');
            ++entry_count;
        }
    }
    /* __const 包含固定宽度入口名称表；保留每项实际起始地址。 */
    for (size_t off = 0; off < size;) {
        size_t start = off;
        while (off < size && data[off] >= 32 && data[off] <= 126) ++off;
        if (off > start && off < size && data[off] == 0 && off - start >= 3) {
            printf("STR unslid=0x%016" PRIx64 " ", s->addr + start);
            quoted(data + start, off - start);
            putchar('\n');
        }
        if (off < size) ++off;
    }
}

int main(void)
{
    /* 引用公开符号保证正常链接 OpenGL，不触发渲染器初始化。 */
    CGLContextObj (*volatile public_symbol)(void) = CGLGetCurrentContext;
    printf("PUBLIC CGLGetCurrentContext=%p (not called)\n", (void *)public_symbol);
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (!name || (!strstr(name, "/OpenGL.framework/Versions/A/OpenGL") &&
                      !strstr(name, "/libGFXShared.dylib") &&
                      !strstr(name, "/libsystem_sandbox.dylib"))) continue;
        const struct mach_header_64 *h =
            (const struct mach_header_64 *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        if (!readonly_range((uintptr_t)h, sizeof(*h)) || h->magic != MH_MAGIC_64 ||
            !readonly_range((uintptr_t)(h + 1), h->sizeofcmds)) return 2;
        const unsigned char gfx_uuid[16] = {
            0xce,0xb3,0xff,0x13,0x17,0x9a,0x3a,0xfd,
            0xab,0x07,0x31,0xbe,0x4c,0x20,0x73,0xb9};
        const unsigned char sandbox_uuid[16] = {
            0x23,0x23,0x8a,0x76,0x35,0x54,0x3e,0xf9,
            0x93,0xa5,0xd2,0xde,0x56,0x6c,0x6c,0xac};
        gfx_uuid_matches = strstr(name, "/libGFXShared.dylib") &&
            image_uuid_matches(h, gfx_uuid);
        printf("IMAGE %s header=%p slide=0x%" PRIxPTR " unslid=0x%" PRIxPTR "\n",
               name, (const void *)h, (uintptr_t)slide, (uintptr_t)h - (uintptr_t)slide);
        const unsigned char *cmd = (const unsigned char *)(h + 1);
        const unsigned char *end = cmd + h->sizeofcmds;
        for (uint32_t j = 0; j < h->ncmds; ++j) {
            if ((size_t)(end - cmd) < sizeof(struct load_command)) return 2;
            const struct load_command *lc = (const struct load_command *)cmd;
            if (lc->cmdsize < sizeof(*lc) || lc->cmdsize > (size_t)(end - cmd)) return 2;
            if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
                const struct uuid_command *u = (const struct uuid_command *)cmd;
                printf("UUID ");
                for (size_t k = 0; k < sizeof(u->uuid); ++k) printf("%02x", u->uuid[k]);
                putchar('\n');
            }
            if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
                const struct segment_command_64 *seg = (const struct segment_command_64 *)cmd;
                uint64_t cstring_addr = 0, cstring_size = 0;
                const struct section_64 *all_sections = (const struct section_64 *)(seg + 1);
                if (seg->nsects > (lc->cmdsize - sizeof(*seg)) / sizeof(struct section_64)) return 2;
                for (uint32_t m = 0; m < seg->nsects; ++m) {
                    if (strncmp(all_sections[m].sectname, "__cstring", 16) == 0) {
                        cstring_addr = all_sections[m].addr;
                        cstring_size = all_sections[m].size;
                    }
                }
                if (seg->nsects > (lc->cmdsize - sizeof(*seg)) / sizeof(struct section_64)) return 2;
                const struct section_64 *sections = (const struct section_64 *)(seg + 1);
                for (uint32_t k = 0; k < seg->nsects; ++k) {
                    const struct section_64 *s = &sections[k];
                    if (strncmp(s->sectname, "__cstring", 16) == 0 ||
                        strncmp(s->sectname, "__cfstring", 16) == 0 ||
                        strncmp(s->sectname, "__const", 16) == 0)
                        dump_section(s, slide, cstring_addr, cstring_size);
                }
            }
            cmd += lc->cmdsize;
        }
        if (strstr(name, "/libsystem_sandbox.dylib") &&
            (uintptr_t)h - (uintptr_t)slide == UINT64_C(0x7ff80e5a6000) &&
            image_uuid_matches(h, sandbox_uuid))
            dump_gpu_paths(slide);
    }
    printf("CONTRACT entries=%zu/129 gpu_paths=%zu/2\n", entry_count, gpu_path_count);
    return entry_count == 129 && gpu_path_count == 2 ? 0 : 2;
}
