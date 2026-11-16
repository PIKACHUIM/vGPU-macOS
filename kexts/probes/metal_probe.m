// 使用系统 Metal framework 验证设备枚举、compute 和 render pipeline。
// 只有设备存在且计算、真实绘制与像素回读均通过才返回成功。
// 构建与运行必须在 macOS 客体中实际进行；同时支持 ARC 和 MRC。
// clang -Wall -Wextra -Werror -framework Foundation -framework Metal -o /tmp/metalprobe metal_probe.m
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>

#if __has_feature(objc_arc)
#define PROBE_RELEASE(object) ((void)(object))
#else
#define PROBE_RELEASE(object) [(object) release]
#endif

static const char *diagnostic_text(NSString *text) {
    const char *utf8 = [text UTF8String];
    return utf8 != NULL ? utf8 : "(unavailable)";
}

static BOOL check_compute(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    BOOL passed = NO;
    NSError *err = nil;
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    id<MTLComputePipelineState> ps = nil;
    id<MTLBuffer> buf = nil;
    id<MTLCommandBuffer> cb = nil;
    id<MTLComputeCommandEncoder> enc = nil;

    lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\n"
                                    "using namespace metal;\n"
                                    "kernel void k(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = 42.0; }\n"
                            options:nil error:&err];
    if (lib == nil) {
        printf("FAIL: compute newLibraryWithSource: %s\n", diagnostic_text([err description]));
        goto cleanup;
    }
    fn = [lib newFunctionWithName:@"k"];
    if (fn == nil) {
        printf("FAIL: compute newFunctionWithName returned nil\n");
        goto cleanup;
    }
    err = nil;
    ps = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (ps == nil) {
        printf("FAIL: newComputePipelineStateWithFunction: %s\n", diagnostic_text([err description]));
        goto cleanup;
    }
    buf = [dev newBufferWithLength:16 * sizeof(float) options:MTLResourceStorageModeShared];
    if (buf == nil) {
        printf("FAIL: compute newBufferWithLength returned nil\n");
        goto cleanup;
    }
    float *p = (float *)[buf contents];
    if (p == NULL) {
        printf("FAIL: compute MTLBuffer contents returned NULL\n");
        goto cleanup;
    }
    for (unsigned i = 0; i < 16; ++i) p[i] = 0.0f;
    cb = [q commandBuffer];
    if (cb == nil) {
        printf("FAIL: compute commandBuffer returned nil\n");
        goto cleanup;
    }
    enc = [cb computeCommandEncoder];
    if (enc == nil) {
        printf("FAIL: compute computeCommandEncoder returned nil\n");
        goto cleanup;
    }
    [enc setComputePipelineState:ps];
    [enc setBuffer:buf offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if ([cb status] != MTLCommandBufferStatusCompleted || [cb error] != nil) {
        printf("FAIL: compute command buffer status=%ld error=%s\n", (long)[cb status],
               diagnostic_text([[cb error] description]));
        goto cleanup;
    }
    for (unsigned i = 0; i < 16; ++i) {
        if (p[i] != 42.0f) {
            printf("FAIL: compute output[%u]=%f (want 42.0)\n", i, p[i]);
            goto cleanup;
        }
    }
    printf("PASS: Metal compute wrote 42.0 to all 16 floats\n");
    passed = YES;

cleanup:
    PROBE_RELEASE(buf);
    PROBE_RELEASE(ps);
    PROBE_RELEASE(fn);
    PROBE_RELEASE(lib);
    return passed;
}

static BOOL check_render(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    const NSUInteger width = 64, height = 64;
    const NSUInteger bytesPerRow = width * 4;
    const NSUInteger imageBytes = bytesPerRow * height;
    BOOL passed = NO;
    NSError *err = nil;
    id<MTLLibrary> lib = nil;
    id<MTLFunction> vertex = nil, fragment = nil;
    MTLRenderPipelineDescriptor *pipelineDesc = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    MTLTextureDescriptor *textureDesc = nil;
    id<MTLTexture> target = nil;
    id<MTLBuffer> readback = nil;
    MTLRenderPassDescriptor *pass = nil;
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    id<MTLBlitCommandEncoder> blit = nil;

    lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\n"
                                    "using namespace metal;\n"
                                    "struct RasterData { float4 position [[position]]; };\n"
                                    "vertex RasterData triangle_vertex(uint i [[vertex_id]]) {\n"
                                    "    const float2 positions[3] = { float2(-0.75, -0.75), float2(0.75, -0.75), float2(0.0, 0.75) };\n"
                                    "    RasterData out; out.position = float4(positions[i], 0.0, 1.0); return out;\n"
                                    "}\n"
                                    "fragment float4 triangle_fragment() { return float4(1.0, 0.0, 0.0, 1.0); }\n"
                            options:nil error:&err];
    if (lib == nil) {
        printf("FAIL: render newLibraryWithSource: %s\n", diagnostic_text([err description]));
        goto cleanup;
    }
    vertex = [lib newFunctionWithName:@"triangle_vertex"];
    fragment = [lib newFunctionWithName:@"triangle_fragment"];
    if (vertex == nil || fragment == nil) {
        printf("FAIL: render vertex or fragment function returned nil\n");
        goto cleanup;
    }
    pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    if (pipelineDesc == nil) {
        printf("FAIL: MTLRenderPipelineDescriptor returned nil\n");
        goto cleanup;
    }
    pipelineDesc.vertexFunction = vertex;
    pipelineDesc.fragmentFunction = fragment;
    pipelineDesc.rasterSampleCount = 1;
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    err = nil;
    pipeline = [dev newRenderPipelineStateWithDescriptor:pipelineDesc error:&err];
    if (pipeline == nil) {
        printf("FAIL: newRenderPipelineStateWithDescriptor: %s\n", diagnostic_text([err description]));
        goto cleanup;
    }
    textureDesc = [[MTLTextureDescriptor alloc] init];
    if (textureDesc == nil) {
        printf("FAIL: MTLTextureDescriptor returned nil\n");
        goto cleanup;
    }
    textureDesc.textureType = MTLTextureType2D;
    textureDesc.pixelFormat = MTLPixelFormatRGBA8Unorm;
    textureDesc.width = width;
    textureDesc.height = height;
    textureDesc.mipmapLevelCount = 1;
    textureDesc.sampleCount = 1;
    textureDesc.storageMode = MTLStorageModePrivate;
    textureDesc.usage = MTLTextureUsageRenderTarget;
    target = [dev newTextureWithDescriptor:textureDesc];
    if (target == nil) {
        printf("FAIL: render newTextureWithDescriptor returned nil\n");
        goto cleanup;
    }
    readback = [dev newBufferWithLength:imageBytes options:MTLResourceStorageModeShared];
    if (readback == nil) {
        printf("FAIL: render readback buffer returned nil\n");
        goto cleanup;
    }
    pass = [[MTLRenderPassDescriptor alloc] init];
    if (pass == nil) {
        printf("FAIL: MTLRenderPassDescriptor returned nil\n");
        goto cleanup;
    }
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    cb = [q commandBuffer];
    if (cb == nil) {
        printf("FAIL: render commandBuffer returned nil\n");
        goto cleanup;
    }
    enc = [cb renderCommandEncoderWithDescriptor:pass];
    if (enc == nil) {
        printf("FAIL: renderCommandEncoderWithDescriptor returned nil\n");
        goto cleanup;
    }
    [enc setRenderPipelineState:pipeline];
    [enc setViewport:(MTLViewport){0.0, 0.0, (double)width, (double)height, 0.0, 1.0}];
    [enc setCullMode:MTLCullModeNone];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];

    // 私有纹理由 GPU blit 到共享缓冲区；CPU 不生成或写入任何像素。
    blit = [cb blitCommandEncoder];
    if (blit == nil) {
        printf("FAIL: render blitCommandEncoder returned nil\n");
        goto cleanup;
    }
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:readback destinationOffset:0 destinationBytesPerRow:bytesPerRow
        destinationBytesPerImage:imageBytes];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if ([cb status] != MTLCommandBufferStatusCompleted || [cb error] != nil) {
        printf("FAIL: render command buffer status=%ld error=%s\n", (long)[cb status],
               diagnostic_text([[cb error] description]));
        goto cleanup;
    }
    const uint8_t *pixels = (const uint8_t *)[readback contents];
    if (pixels == NULL) {
        printf("FAIL: render readback contents returned NULL\n");
        goto cleanup;
    }
    // 中部五点严格在三角形内部，四角在包围盒外，均避开光栅化边缘。
    const struct { NSUInteger x, y; uint8_t red; } samples[] = {
        {32, 32, 255}, {28, 28, 255}, {35, 28, 255}, {28, 35, 255}, {35, 35, 255},
        {4, 4, 0}, {59, 4, 0}, {4, 59, 0}, {59, 59, 0}
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const uint8_t *pixel = pixels + samples[i].y * bytesPerRow + samples[i].x * 4;
        if (pixel[0] != samples[i].red || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255) {
            printf("FAIL: render pixel(%lu,%lu)=(%u,%u,%u,%u), want (%u,0,0,255)\n",
                   (unsigned long)samples[i].x, (unsigned long)samples[i].y,
                   (unsigned)pixel[0], (unsigned)pixel[1], (unsigned)pixel[2], (unsigned)pixel[3],
                   (unsigned)samples[i].red);
            goto cleanup;
        }
    }
    printf("PASS: Metal render drew a red triangle; 5 interior red and 4 exterior black RGBA8 pixels verified\n");
    passed = YES;

cleanup:
    PROBE_RELEASE(pass);
    PROBE_RELEASE(readback);
    PROBE_RELEASE(target);
    PROBE_RELEASE(textureDesc);
    PROBE_RELEASE(pipeline);
    PROBE_RELEASE(pipelineDesc);
    PROBE_RELEASE(fragment);
    PROBE_RELEASE(vertex);
    PROBE_RELEASE(lib);
    return passed;
}

int main(void) {
    @autoreleasepool {
        int result = 1;
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        id<MTLDevice> dev = nil;
        id<MTLCommandQueue> q = nil;
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
            goto cleanup;
        }
        dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) {
            printf("FAIL: MTLCreateSystemDefaultDevice = nil\n");
            goto cleanup;
        }
        printf("MTLCreateSystemDefaultDevice = %s\n", diagnostic_text([dev name]));
        q = [dev newCommandQueue];
        if (q == nil) {
            printf("FAIL: newCommandQueue returned nil\n");
            goto cleanup;
        }
        if (!check_compute(dev, q) || !check_render(dev, q)) goto cleanup;
        printf("PASS: Metal device, compute and render pipeline acceptance passed\n");
        result = 0;

cleanup:
        // command buffer 和 encoder 由 autoreleasepool 管理，已提交命令均已等待完成。
        PROBE_RELEASE(q);
        PROBE_RELEASE(dev);
        PROBE_RELEASE(all);
        return result;
    }
}
