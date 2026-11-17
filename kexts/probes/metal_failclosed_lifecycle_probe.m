// 仅验证固定系统上未初始化对象的负向生命周期；不调用 Metal 设备 API。
#import <Foundation/Foundation.h>
#include <malloc/malloc.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

// runner 上传已固定 SHA256 的原候选快照，不改其释放实现。
#include "metal-failclosed-candidate.m"

static int own_method(Class cls, const char *name) {
    unsigned int count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    int found = 0;
    for (unsigned int i = 0; i < count; ++i)
        if (strcmp(sel_getName(method_getName(methods[i])), name) == 0)
            found = 1;
    free(methods);
    return found;
}

static int encoding_is(Class cls, const char *selector, const char *expected) {
    Method method = class_getInstanceMethod(cls, sel_registerName(selector));
    const char *encoding = method ? method_getTypeEncoding(method) : NULL;
    printf("METHOD class=%s selector=%s encoding=%s\n", class_getName(cls), selector,
           encoding ? encoding : "absent");
    return encoding && strcmp(encoding, expected) == 0;
}

static int contract(void) {
#if !defined(__x86_64__)
    return 0;
#endif
    char build[128] = {0};
    size_t length = sizeof(build);
    if (sysctlbyname("kern.osversion", build, &length, NULL, 0) != 0 ||
        strcmp(build, "24G419") != 0)
        return 0;
    Class base = objc_getClass("_MTLDevice");
    Class candidate = objc_getClass("VGPUMetalFailClosedDevice");
    Class root = objc_getClass("NSObject");
    if (!base || !candidate || !root || class_getSuperclass(base) != root ||
        class_getSuperclass(candidate) != base)
        return 0;
    printf("CONTRACT build=%s base=%s superclass=%s base_size=%zu candidate=%s candidate_size=%zu\n",
           build, class_getName(base), class_getName(root), class_getInstanceSize(base),
           class_getName(candidate), class_getInstanceSize(candidate));
    if (class_getInstanceSize(base) != 656 || class_getInstanceSize(candidate) != 656)
        return 0;
    unsigned int ivarCount = 0;
    Ivar *ivars = class_copyIvarList(candidate, &ivarCount);
    free(ivars);
    if (ivarCount != 0 || !own_method(base, ".cxx_construct") ||
        own_method(candidate, ".cxx_construct"))
        return 0;
    for (Class cls = candidate; cls != Nil; cls = class_getSuperclass(cls)) {
        if (own_method(cls, ".cxx_destruct"))
            return 0;
        if (cls == root)
            break;
        Class meta = object_getClass(cls);
        const char *forbidden[] = {"load", "initialize", "alloc", "allocWithZone:"};
        for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i)
            if (own_method(meta, forbidden[i]))
                return 0;
    }
    return encoding_is(base, "initWithAcceleratorPort:", "@20@0:8I16") &&
           encoding_is(candidate, "initWithAcceleratorPort:", "@20@0:8I16") &&
           encoding_is(base, ".cxx_construct", "@16@0:8") &&
           encoding_is(base, "dealloc", "v16@0:8");
}

int main(int argc, const char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    if (argc != 2 || strcmp(argv[1], "--negative-lifecycle-only") != 0)
        return 64;
    if (!contract()) {
        fprintf(stderr, "CONTRACT_REJECTED before_alloc=1\n");
        return 65;
    }
    @autoreleasepool {
        for (unsigned int i = 1; i <= 4; ++i) {
            VGPUMetalFailClosedDevice *object = [VGPUMetalFailClosedDevice alloc];
            if (!object || object_getClass(object) != objc_getClass("VGPUMetalFailClosedDevice"))
                _exit(66);
            size_t bytes = malloc_size(object);
            printf("ALLOC iteration=%u size=%zu pid=%d\n", i, bytes, getpid());
            // 异常布局只终止隔离进程，不尝试未知 Apple 清理路径。
            if (bytes < 656)
                _exit(67);
            id result = [object initWithAcceleratorPort:IO_OBJECT_NULL];
            // init 已销毁原对象；此后不得读取或 release object。
            if (result != nil)
                _exit(68);
            printf("RETURN iteration=%u nil=1\n", i);
        }
    }
    printf("LIFECYCLE_PASS iterations=4 nil_returns=4 GPU_NOT_EXECUTED\n");
    return 0;
}
