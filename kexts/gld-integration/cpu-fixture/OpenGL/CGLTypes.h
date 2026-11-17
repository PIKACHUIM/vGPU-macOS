#ifndef GI_CPU_CGL_TYPES_H
#define GI_CPU_CGL_TYPES_H
/* 仅 CPU 编译夹具，不替代 macOS SDK 验证。 */
enum {
    kCGLNoError = 0, kCGLBadAttribute = 10000, kCGLBadPixelFormat = 10002,
    kCGLBadContext = 10004, kCGLBadState = 10007, kCGLBadValue = 10008,
    kCGLBadMatch = 10009, kCGLBadAlloc = 10016
};
#endif
