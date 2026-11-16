#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* 仅查询路径和现有名称，不加载 bundle，不创建候选，不修改属性。
 * 构建：clang -std=c11 -Wall -Wextra -Werror -Wpedantic -O0
 *       gld_trust_probe.c -o gld_trust_probe
 * 签名依据：24G419 x86_64 的实际调用方和 libsystem_sandbox 指令。
 */
typedef int (*path_trusted_fn)(const char *);
typedef int (*find_trusted_fn)(const char *, char *, size_t);

static void *resolve(const char *name)
{
    dlerror();
    void *symbol = dlsym(RTLD_DEFAULT, name);
    const char *error = dlerror();
    if (error != NULL || symbol == NULL) {
        fprintf(stderr, "RESOLVE_ERROR symbol=%s error=%s\n", name,
                error != NULL ? error : "NULL");
        return NULL;
    }
    Dl_info info;
    if (dladdr(symbol, &info) == 0 || info.dli_fname == NULL ||
        info.dli_sname == NULL) {
        fprintf(stderr, "RESOLVE_ERROR symbol=%s provenance=unknown\n", name);
        return NULL;
    }
    printf("SYMBOL requested=%s actual=%s image=%s\n", name,
           info.dli_sname, info.dli_fname);
    if (strcmp(info.dli_fname, "/usr/lib/system/libsystem_sandbox.dylib") != 0 ||
        strcmp(info.dli_sname, name) != 0) {
        fprintf(stderr, "RESOLVE_ERROR symbol=%s provenance=mismatch\n", name);
        return NULL;
    }
    return symbol;
}

static void query_path(path_trusted_fn trusted, const char *path)
{
    struct stat st;
    errno = 0;
    int status = stat(path, &st);
    int stat_errno = errno;
    errno = 0;
    int result = trusted(path);
    int trust_errno = errno;
    printf("PATH path=%s stat=%d stat_errno=%d directory=%d trust=%d trust_errno=%d\n",
           path, status, stat_errno, status == 0 && S_ISDIR(st.st_mode),
           result, trust_errno);
}

int main(void)
{
    struct utsname system;
    if (uname(&system) != 0) {
        perror("uname");
        return 1;
    }
    printf("SYSTEM sysname=%s release=%s machine=%s uid=%lu\n",
           system.sysname, system.release, system.machine, (unsigned long)getuid());
    void *path_symbol = resolve("gpu_bundle_is_path_trusted");
    void *find_symbol = resolve("gpu_bundle_find_trusted");
    if (path_symbol == NULL || find_symbol == NULL) return 1;
    path_trusted_fn trusted;
    find_trusted_fn find;
    _Static_assert(sizeof(trusted) == sizeof(path_symbol), "pointer size");
    _Static_assert(sizeof(find) == sizeof(find_symbol), "pointer size");
    memcpy(&trusted, &path_symbol, sizeof(trusted));
    memcpy(&find, &find_symbol, sizeof(find));

    const char *paths[] = {
        "/Library/GPUBundles",
        "/System/Library/Extensions",
        "/var/tmp",
        "/System/Library/Extensions/AppleIntelICLGraphicsGLDriver.bundle",
        "/System/Library/Extensions/AppleIntelICLGraphicsGLDriver.bundle/Contents/MacOS/libigdmd.dylib"
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        query_path(trusted, paths[i]);

    char missing[PATH_MAX];
    int length = snprintf(missing, sizeof(missing),
                          "/var/tmp/gld-trust-probe-absent-%ld", (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(missing)) return 1;
    struct stat st;
    errno = 0;
    if (lstat(missing, &st) != -1 || errno != ENOENT) {
        fprintf(stderr, "ABSENT_PATH_ERROR path=%s\n", missing);
        return 1;
    }
    query_path(trusted, missing);

    /* 先确认现有目录，再按三参数签名查询名称；不使用返回路径加载代码。 */
    const char *existing = "/System/Library/Extensions/AppleIntelICLGraphicsGLDriver.bundle";
    if (stat(existing, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "EXISTING_BUNDLE_ERROR path=%s\n", existing);
        return 1;
    }
    char output[PATH_MAX] = {0};
    errno = 0;
    int result = find("AppleIntelICLGraphicsGLDriver", output, sizeof(output));
    int find_errno = errno;
    printf("FIND name=AppleIntelICLGraphicsGLDriver result=%d errno=%d path=%.*s\n",
           result, find_errno, (int)sizeof(output), result == 0 ? output : "");
    puts("LIMIT errno is recorded after reset; nonzero errno alone does not imply failure.");
    puts("LIMIT No candidate bundle exists in /Library/GPUBundles; these queries do not prove a new bundle loads.");
    return 0;
}
