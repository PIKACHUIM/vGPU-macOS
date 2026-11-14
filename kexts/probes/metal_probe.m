// Probe what Metal actually offers on this guest, without guessing.
//
// MTLCreateSystemDefaultDevice() enumerates IOAccelerator services that advertise a
// Metal plugin. A framebuffer-only display driver (what this project ships) provides
// none, so the expected answer is "nil" -- but "expected" is not a measurement.
//
// Build: clang -fobjc-arc -framework Metal -framework Foundation -o /tmp/metalprobe metal_probe.m
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(void) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        printf("MTLCopyAllDevices count = %lu\n", (unsigned long)all.count);
        for (id<MTLDevice> d in all) {
            printf("  device: %s (lowPower=%d, headless=%d, removable=%d)\n",
                   [[d name] UTF8String], (int)[d isLowPower], (int)[d isHeadless],
                   (int)[d isRemovable]);
            printf("  unifiedMemory=%d registryID=%llu\n", (int)[d hasUnifiedMemory],
                   (unsigned long long)[d registryID]);
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) {
            printf("MTLCreateSystemDefaultDevice = nil  -> Metal has NO device\n");
        } else {
            printf("MTLCreateSystemDefaultDevice = %s\n", [[dev name] UTF8String]);
            // Prove a real device by compiling and running the smallest command buffer.
            NSError *err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\n"
                                   "using namespace metal;\n"
                                   "kernel void k(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = 42.0; }\n"
                                                                        options:0
                                                                          error:&err];
            if (lib == nil) {
                printf("newLibraryWithSource failed: %s\n", [[err description] UTF8String]);
            } else {
                id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
                id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
                id<MTLBuffer> buf = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
                id<MTLCommandQueue> q = [dev newCommandQueue];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                [enc setBuffer:buf offset:0 atIndex:0];
                [enc dispatchThreads:MTLSizeMake(16,1,1) threadsPerThreadgroup:MTLSizeMake(16,1,1)];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                float *p = (float *)[buf contents];
                printf("compute kernel wrote o[0]=%f (want 42.0) -> %s\n", p[0],
                       (p[0] == 42.0f) ? "METAL EXECUTES" : "METAL DID NOT RUN");
            }
        }
    }
    return 0;
}
