// 使用系统 Metal framework 验证设备枚举与 compute 执行。
// compute 成功只是阶段性结果，不代表 render pipeline 已验收。
// 构建与运行必须在 macOS 客体中实际进行。
// clang -fobjc-arc -framework Metal -framework Foundation -o /tmp/metalprobe metal_probe.m
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

static const char *diagnostic_text(NSString *text) {
    const char *utf8 = [text UTF8String];
    return utf8 != NULL ? utf8 : "(unavailable)";
}

int main(void) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        printf("MTLCopyAllDevices count = %lu\n", (unsigned long)all.count);
        for (id<MTLDevice> d in all) {
            printf("  device: %s (lowPower=%d, headless=%d, removable=%d)\n",
                   diagnostic_text([d name]), (int)[d isLowPower], (int)[d isHeadless],
                   (int)[d isRemovable]);
            printf("  unifiedMemory=%d registryID=%llu\n", (int)[d hasUnifiedMemory],
                   (unsigned long long)[d registryID]);
        }
        if (all == nil || all.count == 0) {
            printf("FAIL: Metal device enumeration returned no devices\n");
            return 1;
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) {
            printf("FAIL: MTLCreateSystemDefaultDevice = nil\n");
            return 1;
        }
        printf("MTLCreateSystemDefaultDevice = %s\n", diagnostic_text([dev name]));
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\n"
                               "using namespace metal;\n"
                               "kernel void k(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = 42.0; }\n"
                                                                    options:nil error:&err];
        if (lib == nil) {
            printf("FAIL: newLibraryWithSource: %s\n", diagnostic_text([err description]));
            return 1;
        }
        id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
        if (fn == nil) {
            printf("FAIL: newFunctionWithName returned nil\n");
            return 1;
        }
        err = nil;
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (ps == nil) {
            printf("FAIL: newComputePipelineStateWithFunction: %s\n", diagnostic_text([err description]));
            return 1;
        }
        id<MTLBuffer> buf = [dev newBufferWithLength:16 * sizeof(float) options:MTLResourceStorageModeShared];
        if (buf == nil) {
            printf("FAIL: newBufferWithLength returned nil\n");
            return 1;
        }
        float *p = (float *)[buf contents];
        if (p == NULL) {
            printf("FAIL: MTLBuffer contents returned NULL\n");
            return 1;
        }
        for (unsigned i = 0; i < 16; ++i) p[i] = 0.0f;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (q == nil) {
            printf("FAIL: newCommandQueue returned nil\n");
            return 1;
        }
        id<MTLCommandBuffer> cb = [q commandBuffer];
        if (cb == nil) {
            printf("FAIL: commandBuffer returned nil\n");
            return 1;
        }
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        if (enc == nil) {
            printf("FAIL: computeCommandEncoder returned nil\n");
            return 1;
        }
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if ([cb status] != MTLCommandBufferStatusCompleted || [cb error] != nil) {
            printf("FAIL: command buffer status=%ld error=%s\n", (long)[cb status],
                   diagnostic_text([[cb error] description]));
            return 1;
        }
        for (unsigned i = 0; i < 16; ++i) {
            if (p[i] != 42.0f) {
                printf("FAIL: compute output[%u]=%f (want 42.0)\n", i, p[i]);
                return 1;
            }
        }
        printf("PASS: Metal compute wrote 42.0 to all 16 floats (render pipeline not tested)\n");
        return 0;
    }
}
