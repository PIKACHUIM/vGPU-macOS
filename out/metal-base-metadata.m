#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    @autoreleasepool {
        // 只查运行时元数据；不发类消息、不分配设备、不枚举 Metal 设备。
        const char *names[] = {"_MTLDevice", "MTLIOAccelDevice"};
        for (unsigned n = 0; n < 2; ++n) {
            Class cls = objc_getClass(names[n]);
            if (cls == Nil) { printf("missing %s\n", names[n]); return 1; }
            printf("CLASS %s SUPER %s SIZE %zu IMAGE %s\n", class_getName(cls),
                   class_getName(class_getSuperclass(cls)), class_getInstanceSize(cls), class_getImageName(cls));
            unsigned count = 0;
            Ivar *ivars = class_copyIvarList(cls, &count);
            for (unsigned i = 0; i < count; ++i)
                printf("IVAR %s OFFSET %td TYPE %s\n", ivar_getName(ivars[i]), ivar_getOffset(ivars[i]), ivar_getTypeEncoding(ivars[i]));
            free(ivars);
            for (unsigned meta = 0; meta < 2; ++meta) {
                Class target = meta ? object_getClass(cls) : cls;
                Method *methods = class_copyMethodList(target, &count);
                for (unsigned i = 0; i < count; ++i)
                    printf("METHOD %c %s TYPE %s\n", meta ? '+' : '-',
                           sel_getName(method_getName(methods[i])), method_getTypeEncoding(methods[i]));
                free(methods);
            }
        }
        return 0;
    }
}
