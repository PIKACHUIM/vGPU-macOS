// Probe the OpenGL stack: which renderer does a CGL context actually get?
//
// macOS keeps a software OpenGL renderer (kCGLRendererGenericFloatID, "Apple Software
// Renderer"), so "OpenGL works" can be true on a machine with no GPU driver at all --
// which is exactly the distinction that matters here. Asking for the renderer string is
// the only way to tell an accelerated context from a software one.
//
// Build: clang -framework OpenGL -framework Foundation -o /tmp/glprobe gl_probe.c
#include <stdio.h>
#include <string.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>

static void report(const char *label, CGLPixelFormatAttribute *attrs) {
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    CGLError e = CGLChoosePixelFormat(attrs, &pix, &npix);
    if (e != kCGLNoError || pix == NULL) {
        printf("%s: CGLChoosePixelFormat failed (%s)\n", label, CGLErrorString(e));
        return;
    }
    CGLContextObj ctx = NULL;
    e = CGLCreateContext(pix, NULL, &ctx);
    CGLDestroyPixelFormat(pix);
    if (e != kCGLNoError || ctx == NULL) {
        printf("%s: CGLCreateContext failed (%s)\n", label, CGLErrorString(e));
        return;
    }
    CGLSetCurrentContext(ctx);
    printf("%s: RENDERER=%s\n", label, glGetString(GL_RENDERER));
    printf("%s: VENDOR=%s VERSION=%s GLSL=%s\n", label, glGetString(GL_VENDOR),
           glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    // Draw one triangle into an FBO and read it back: a renderer string alone does not
    // prove the pipeline runs.
    GLuint fbo = 0, tex = 0;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
        printf("%s: FBO incomplete -> could not test rasterisation\n", label);
    } else {
        glViewport(0, 0, 64, 64);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glVertex2f(0, 1);
        glEnd();
        glFinish();
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(32, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("%s: triangle centre pixel = %d,%d,%d,%d -> %s\n", label,
               px[0], px[1], px[2], px[3],
               (px[0] > 200 && px[1] < 60) ? "OPENGL RASTERISES" : "no triangle");
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
}

int main(void) {
    // 1. whatever the system prefers (this is what an app gets by default)
    CGLPixelFormatAttribute any[] = {
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFADoubleBuffer, (CGLPixelFormatAttribute)0
    };
    report("default", any);

    // 2. explicitly the software renderer
    CGLPixelFormatAttribute soft[] = {
        kCGLPFARendererID, (CGLPixelFormatAttribute)kCGLRendererGenericFloatID,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    report("software", soft);

    // 3. explicitly accelerated -- expected to fail with no IOAccelerator
    CGLPixelFormatAttribute accel[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    report("accelerated", accel);
    return 0;
}
