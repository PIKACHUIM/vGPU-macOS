// 使用系统 Metal framework 验证设备枚举、compute 和 render pipeline。
// 只有设备存在且计算、真实绘制与像素回读均通过才返回成功。
// 构建与运行必须在 macOS 客体中实际进行；同时支持 ARC 和 MRC。
// clang -Wall -Wextra -Werror -framework Foundation -framework Metal -framework IOKit -o /tmp/metalprobe metal_probe.m
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_feature(objc_arc)
#define PROBE_RELEASE(object) ((void)(object))
#else
#define PROBE_RELEASE(object) [(object) release]
#endif

static const char *diagnostic_text(NSString *text) {
    const char *utf8 = [text UTF8String];
    return utf8 != NULL ? utf8 : "(unavailable)";
}

static void print_cf(CFTypeRef value) {
    if (value == NULL) { printf("absent"); return; }
    CFStringRef description = CFCopyDescription(value);
    if (description == NULL) { printf("(description unavailable)"); return; }
    CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(description), kCFStringEncodingUTF8) + 1;
    char *text = malloc((size_t)capacity);
    if (text != NULL && CFStringGetCString(description, text, capacity, kCFStringEncodingUTF8))
        printf("%s", text);
    else printf("(conversion unavailable)");
    free(text);
    CFRelease(description);
}

static void print_property(const char *key, CFTypeRef value) {
    printf("  %s present=%s", key, value != NULL ? "true" : "false");
    if (value != NULL) {
        CFStringRef type = CFCopyTypeIDDescription(CFGetTypeID(value));
        printf(" type="); print_cf(type);
        printf(" value="); print_cf(value);
        if (type != NULL) CFRelease(type);
    }
    printf("\n");
}

// 本诊断只处理短 ASCII 文件名，不模拟 loader 对异常字符串的行为。
static const char *discovery_name_gate(CFTypeRef value, char name[255]) {
    if (value == NULL) return "G1_NO_METAL_PLUGIN_NAME";
    if (CFGetTypeID(value) != CFStringGetTypeID()) return "G1_NAME_NOT_STRING";
    if (!CFStringGetCString((CFStringRef)value, name, 255, kCFStringEncodingUTF8))
        return "DIAGNOSTIC_NAME_UNSUPPORTED";
    size_t length = strlen(name);
    if (length == 0 || (CFIndex)length != CFStringGetLength((CFStringRef)value) ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return "DIAGNOSTIC_NAME_UNSUPPORTED";
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return "DIAGNOSTIC_NAME_UNSUPPORTED";
    }
    return NULL;
}

static BOOL describe_path(const char *path) {
    struct stat entry, target;
    int entryResult = lstat(path, &entry);
    int entryError = entryResult == 0 ? 0 : errno;
    int targetResult = stat(path, &target);
    int targetError = targetResult == 0 ? 0 : errno;
    printf("  path=%s lstat=%d errno=%d symlink=%s stat=%d errno=%d directory=%s\n",
           path, entryResult, entryError,
           entryResult == 0 && S_ISLNK(entry.st_mode) ? "true" : "false",
           targetResult, targetError,
           targetResult == 0 && S_ISDIR(target.st_mode) ? "true" : "false");
    return targetResult == 0;
}

static void inspect_bundle_files(const char *root, const char *name) {
    char bundle[1024], plistPath[1200];
    snprintf(bundle, sizeof(bundle), "%s/%s.bundle", root, name);
    if (!describe_path(bundle)) return;
    snprintf(plistPath, sizeof(plistPath), "%s/Contents/Info.plist", bundle);
    if (!describe_path(plistPath)) return;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)plistPath,
                                                          (CFIndex)strlen(plistPath), false);
    CFReadStreamRef stream = url != NULL ? CFReadStreamCreateWithFile(NULL, url) : NULL;
    CFPropertyListRef plist = NULL;
    CFErrorRef error = NULL;
    if (stream != NULL && CFReadStreamOpen(stream))
        plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, &error);
    if (stream != NULL) { CFReadStreamClose(stream); CFRelease(stream); }
    if (url != NULL) CFRelease(url);
    if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        printf("  plist=UNREADABLE_OR_NOT_DICTIONARY error="); print_cf(error); printf("\n");
    } else {
        CFDictionaryRef dict = (CFDictionaryRef)plist;
        CFTypeRef executable = CFDictionaryGetValue(dict, CFSTR("CFBundleExecutable"));
        print_property("CFBundleExecutable", executable);
        print_property("NSPrincipalClass", CFDictionaryGetValue(dict, CFSTR("NSPrincipalClass")));
        char leaf[255];
        if (discovery_name_gate(executable, leaf) == NULL) {
            char executablePath[1500];
            snprintf(executablePath, sizeof(executablePath), "%s/Contents/MacOS/%s", bundle, leaf);
            if (describe_path(executablePath)) {
                CFURLRef executableURL = CFURLCreateFromFileSystemRepresentation(NULL,
                    (const UInt8 *)executablePath, (CFIndex)strlen(executablePath), false);
                CFArrayRef architectures = executableURL != NULL ?
                    CFBundleCopyExecutableArchitecturesForURL(executableURL) : NULL;
                printf("  executable Mach-O CPU types (16777223=x86_64, 16777228=arm64)=");
                print_cf(architectures); printf("\n");
                if (architectures != NULL) CFRelease(architectures);
                if (executableURL != NULL) CFRelease(executableURL);
            }
        } else printf("  executable inspection=DIAGNOSTIC_NAME_UNSUPPORTED\n");
    }
    if (error != NULL) CFRelease(error);
    if (plist != NULL) CFRelease(plist);
}

static int discovery_contract_only(void) {
    char build[128] = "unknown";
    size_t buildSize = sizeof(build);
    if (sysctlbyname("kern.osversion", build, &buildSize, NULL, 0) != 0)
        snprintf(build, sizeof(build), "unknown");
    struct utsname machine;
    int unameResult = uname(&machine);
    printf("DISCOVERY_CONTRACT_ONLY OS=%s build=%s arch=%s\n",
           diagnostic_text([[NSProcessInfo processInfo] operatingSystemVersionString]), build,
           unameResult == 0 ? machine.machine : "unknown");
    printf("trust=NOT_EXECUTED/UNKNOWN class_resolution=NOT_EXECUTED/UNKNOWN "
           "inheritance=NOT_EXECUTED/UNKNOWN init=NOT_EXECUTED/UNKNOWN "
           "type5=NOT_EXECUTED/UNKNOWN GPU=NOT_EXECUTED/UNKNOWN\n");
    CFMutableDictionaryRef matching = IOServiceMatching("IOAccelerator");
    if (matching == NULL) { printf("ERROR: matching dictionary unavailable\n"); return 1; }
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t status = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (status != KERN_SUCCESS) {
        printf("ERROR: IOServiceGetMatchingServices=0x%x\n", (unsigned)status);
        if (iterator != IO_OBJECT_NULL) IOObjectRelease(iterator);
        return 1;
    }
    unsigned count = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        ++count;
        io_name_t className = "unknown";
        io_string_t path = "unknown";
        uint64_t registryID = 0;
        kern_return_t classStatus = IOObjectGetClass(service, className);
        kern_return_t idStatus = IORegistryEntryGetRegistryEntryID(service, &registryID);
        kern_return_t pathStatus = IORegistryEntryGetPath(service, kIOServicePlane, path);
        printf("candidate=%u class=%s class_status=0x%x registryID=0x%llx id_status=0x%x path=%s path_status=0x%x\n",
               count, className, (unsigned)classStatus, (unsigned long long)registryID,
               (unsigned)idStatus, path, (unsigned)pathStatus);
        printf("  IOServiceCompatibility=%s\n",
               IOObjectConformsTo(service, "IOServiceCompatibility") ? "true" : "false");
        const CFStringRef keys[] = {CFSTR("MetalPluginName"), CFSTR("MetalPluginClassName"),
                                   CFSTR("Removable"), CFSTR("SafeEjectRequested")};
        const char *labels[] = {"MetalPluginName", "MetalPluginClassName", "Removable", "SafeEjectRequested"};
        CFTypeRef properties[4] = {NULL, NULL, NULL, NULL};
        for (size_t i = 0; i < 4; ++i) {
            properties[i] = IORegistryEntryCreateCFProperty(service, keys[i], NULL, 0);
            print_property(labels[i], properties[i]);
        }
        char name[255];
        const char *gate = discovery_name_gate(properties[0], name);
        if (gate != NULL) printf("  stage=%s\n", gate);
        else {
            inspect_bundle_files("/Library/GPUBundles", name);
            inspect_bundle_files("/System/Library/Extensions", name);
            printf("  stage=FILES_INSPECTED_LOADER_NOT_EXECUTED\n");
        }
        for (size_t i = 0; i < 4; ++i) if (properties[i] != NULL) CFRelease(properties[i]);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    printf("DISCOVERY_COMPLETE candidates=%u; diagnostic completion is not Metal availability\n", count);
    return 0;
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

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--discovery-contract-only") == 0)
            return discovery_contract_only();
        if (argc != 1) {
            fprintf(stderr, "usage: %s [--discovery-contract-only]\n", argv[0]);
            return 64;
        }
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
