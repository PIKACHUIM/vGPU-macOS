// 仅供目标 macOS 15.7.3 x86_64 的负向 loader 诊断候选；本轮只编译，不加载。
// 由运行时元数据核验 superclass、方法编码，禁止访问或复制基类私有 ivar。
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <objc/runtime.h>
#include <stdio.h>

#if __has_feature(objc_arc)
#error 此候选必须以MRC构建，避免ARC隐式释放未初始化基类
#endif

@interface _MTLDevice : NSObject
- (instancetype)initWithAcceleratorPort:(io_service_t)service;
@end

@interface VGPUMetalFailClosedDevice : _MTLDevice
@end

@implementation VGPUMetalFailClosedDevice
- (instancetype)initWithAcceleratorPort:(io_service_t)service {
    // 未调用基类 init，未打开或保留 service，绝不把 self 交给 Metal。
    fprintf(stderr, "VGPU_METAL_DIAGNOSTIC_INIT service=0x%x result=nil GPU_NOT_EXECUTED\n", service);
    // 仅适用于本机已核验的零状态 .cxx_construct / 无 .cxx_destruct 基类。
    // object_dispose 跳过 -dealloc，避免其对尚未建立的 dispatch queue 做释放。
    object_dispose(self);
    return nil;
}
@end
