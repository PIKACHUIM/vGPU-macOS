// 使用系统 OpenGL framework 验证上下文和实际栅格化。
// 默认和软件上下文仅作诊断；成功必须同时满足加速属性与像素回读校验。
// 构建与运行必须在 macOS 客体中实际进行。
// clang -framework OpenGL -framework Foundation -o /tmp/glprobe gl_probe.c
#include <stdio.h>
#include <stdlib.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLRenderers.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

static int check_gl(const char *label, const char *stage) {
    int ok = 1;
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        printf("%s: %s GL error=0x%04x\n", label, stage, (unsigned)error);
        ok = 0;
    }
    return ok;
}

static int report(const char *label, CGLPixelFormatAttribute *attrs, int require_accelerated) {
    CGLPixelFormatObj pix = NULL;
    CGLContextObj ctx = NULL;
    CGLContextObj previous = CGLGetCurrentContext();
    GLuint fbo = 0, tex = 0;
    GLint npix = 0, screen = 0, accelerated = 0;
    int current = 0, ok = 0;
    CGLError e = CGLChoosePixelFormat(attrs, &pix, &npix);
    if (e != kCGLNoError || pix == NULL || npix <= 0) {
        printf("%s: CGLChoosePixelFormat failed (%s), npix=%d\n", label, CGLErrorString(e), npix);
        goto cleanup;
    }
    e = CGLCreateContext(pix, NULL, &ctx);
    if (e != kCGLNoError || ctx == NULL) {
        printf("%s: CGLCreateContext failed (%s)\n", label, CGLErrorString(e));
        goto cleanup;
    }
    e = CGLSetCurrentContext(ctx);
    if (e != kCGLNoError) {
        printf("%s: CGLSetCurrentContext failed (%s)\n", label, CGLErrorString(e));
        goto cleanup;
    }
    current = 1;
    e = CGLGetVirtualScreen(ctx, &screen);
    if (e != kCGLNoError) {
        printf("%s: CGLGetVirtualScreen failed (%s)\n", label, CGLErrorString(e));
        goto cleanup;
    }
    e = CGLDescribePixelFormat(pix, screen, kCGLPFAAccelerated, &accelerated);
    if (e != kCGLNoError) {
        printf("%s: CGLDescribePixelFormat failed (%s)\n", label, CGLErrorString(e));
        goto cleanup;
    }
    printf("%s: virtualScreen=%d kCGLPFAAccelerated=%d\n", label, screen, accelerated);
    if (require_accelerated && !accelerated) {
        printf("%s: FAIL: accelerated context required\n", label);
        goto cleanup;
    }
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *version = glGetString(GL_VERSION);
    const GLubyte *glsl = glGetString(GL_SHADING_LANGUAGE_VERSION);
    if (!check_gl(label, "renderer query") || !renderer || !vendor || !version || !glsl) {
        printf("%s: renderer query failed\n", label);
        goto cleanup;
    }
    printf("%s: RENDERER=%s\n", label, renderer);
    printf("%s: VENDOR=%s VERSION=%s GLSL=%s\n", label, vendor, version, glsl);

    // 红色三角形中心和三角形外的黑色背景必须同时正确。
    glGenFramebuffersEXT(1, &fbo);
    glGenTextures(1, &tex);
    if (!check_gl(label, "object creation") || !fbo || !tex) {
        printf("%s: FBO/texture creation failed\n", label);
        goto cleanup;
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, tex, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    int setup_ok = check_gl(label, "FBO setup");
    if (!setup_ok || status != GL_FRAMEBUFFER_COMPLETE_EXT) {
        printf("%s: FBO failed, status=0x%04x\n", label, (unsigned)status);
        goto cleanup;
    }
    glViewport(0, 0, 64, 64);
    glDisable(GL_DITHER);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex2f(-1, -1);
    glVertex2f(1, -1);
    glVertex2f(0, 1);
    glEnd();
    glFinish();
    if (!check_gl(label, "render")) goto cleanup;

    unsigned char centre[4] = {0x5a, 0x5a, 0x5a, 0x5a};
    unsigned char background[4] = {0x5a, 0x5a, 0x5a, 0x5a};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centre);
    glReadPixels(4, 60, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, background);
    if (!check_gl(label, "readback")) goto cleanup;
    ok = centre[0] == 255 && centre[1] == 0 && centre[2] == 0 && centre[3] == 255 &&
         background[0] == 0 && background[1] == 0 && background[2] == 0 && background[3] == 255;
    printf("%s: centre(32,32)=%u,%u,%u,%u background(4,60)=%u,%u,%u,%u -> %s\n",
           label, (unsigned)centre[0], (unsigned)centre[1], (unsigned)centre[2], (unsigned)centre[3],
           (unsigned)background[0], (unsigned)background[1], (unsigned)background[2], (unsigned)background[3],
           ok ? "OPENGL RASTERISES" : "FAIL: pixel mismatch");

cleanup:
    if (current) {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (fbo) glDeleteFramebuffersEXT(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        if (!check_gl(label, "cleanup")) ok = 0;
        e = CGLSetCurrentContext(previous);
        if (e != kCGLNoError) {
            printf("%s: restoring current context failed (%s)\n", label, CGLErrorString(e));
            ok = 0;
        }
    }
    if (ctx) {
        e = CGLDestroyContext(ctx);
        if (e != kCGLNoError) {
            printf("%s: CGLDestroyContext failed (%s)\n", label, CGLErrorString(e));
            ok = 0;
        }
    }
    if (pix) {
        e = CGLDestroyPixelFormat(pix);
        if (e != kCGLNoError) {
            printf("%s: CGLDestroyPixelFormat failed (%s)\n", label, CGLErrorString(e));
            ok = 0;
        }
    }
    printf("%s: %s%s\n", label, ok ? "PASS" : "FAIL",
           require_accelerated ? " (accelerated acceptance)" : " (diagnostic only)");
    return ok;
}

int main(void) {
    // 默认和软件结果不参与硬件验收判定。
    CGLPixelFormatAttribute any[] = {
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFADoubleBuffer, (CGLPixelFormatAttribute)0
    };
    (void)report("default", any, 0);

    CGLPixelFormatAttribute soft[] = {
        kCGLPFARendererID, (CGLPixelFormatAttribute)kCGLRendererGenericFloatID,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    (void)report("software", soft, 0);

    // 禁止软件回退，并核对实际虚拟屏幕的加速属性。
    CGLPixelFormatAttribute accel[] = {
        kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    int ok = report("accelerated", accel, 1);
    if (!ok) {
        // 会话条件与硬件枚举需独立取证，不能从 CGL 错误推断驱动不支持。
        printf("FAIL: accelerated OpenGL acceptance did not pass in this session.\n");
        printf("CGL failure alone does not prove driver incompatibility or absent hardware.\n");
        printf("Record Aqua/login-session availability and IOAccelerator enumeration separately.\n");
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
