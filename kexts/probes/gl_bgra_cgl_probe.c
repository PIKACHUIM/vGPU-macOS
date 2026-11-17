/* Public CGL acceptance for the deliberately restricted BGRA renderbuffer path.
 * clang -std=c11 -Wall -Wextra -Werror -framework OpenGL gl_bgra_cgl_probe.c -o gl_bgra_cgl_probe
 * No software fallback. This tests clear/read, not triangle or full GL support. */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gl_ok(const char *where)
{
    GLenum e; int ok = 1;
    while ((e = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "%s GL error=0x%x\n", where, (unsigned)e); ok = 0;
    }
    return ok;
}
static int read_check(GLuint fb, const unsigned char rgba[4], unsigned char *data,
                      size_t size)
{
    const size_t stride = 1032, start = 1040;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    glPixelStorei(GL_PACK_ROW_LENGTH, 258);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 2);
    memset(data, 0xa5, size);
    glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, data);
    if (!gl_ok("packed read")) return 0;
    for (size_t k = 0; k < size; ++k) {
        unsigned char want = 0xa5;
        if (k >= start) {
            size_t q = k - start;
            if (q / stride < 256 && q % stride < 1024) want = rgba[q % 4];
        }
        if (data[k] != want) {
            fprintf(stderr, "fb=%u byte=%zu got=%u expected=%u\n",
                    fb, k, data[k], want); return 0;
        }
    }
    printf("PASS fb=%u pixels=65536 RGBA=%u,%u,%u,%u pack/canary exact\n",
           fb, rgba[0], rgba[1], rgba[2], rgba[3]);
    return 1;
}
int main(void)
{
    CGLPixelFormatObj pf = NULL; CGLContextObj ctx = NULL;
    GLuint fb[2] = {0}, rb[2] = {0}; GLint count = 0, accelerated = 0, renderer = 0;
    unsigned char *data = NULL; int ok = 0;
    const size_t bytes = 1040 + 256 * 1032 + 16;
    const CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)32,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        (CGLPixelFormatAttribute)0
    };
    const GLfloat colors[2][4] = {{1.0f, 0.0f, 0.0f, 0.5f}, {0.0f, 1.0f, 1.0f, 0.8f}};
    const unsigned char expected[2][4] = {{255, 0, 0, 128}, {0, 255, 255, 204}};
    CGLError e = CGLChoosePixelFormat(attrs, &pf, &count);
    if (e || !pf || count <= 0) {
        fprintf(stderr, "CGLChoosePixelFormat error=%d (%s) count=%d\n", e, CGLErrorString(e), count);
        goto cleanup;
    }
    e = CGLDescribePixelFormat(pf, 0, kCGLPFAAccelerated, &accelerated);
    if (e || !accelerated) { fprintf(stderr, "not accelerated error=%d\n", e); goto cleanup; }
    e = CGLDescribePixelFormat(pf, 0, kCGLPFARendererID, &renderer);
    if (e) { fprintf(stderr, "renderer query error=%d\n", e); goto cleanup; }
    printf("CGL renderer=0x%08x accelerated=%d\n", (unsigned)renderer, accelerated);
    if (((unsigned)renderer & 0x00ffff00u) != 0x00027e00u) {
        fprintf(stderr, "selected renderer is not the vGPU driver\n"); goto cleanup;
    }
    e = CGLCreateContext(pf, NULL, &ctx);
    if (e || !ctx) { fprintf(stderr, "CGLCreateContext error=%d (%s)\n", e, CGLErrorString(e)); goto cleanup; }
    e = CGLSetCurrentContext(ctx);
    if (e) { fprintf(stderr, "CGLSetCurrentContext error=%d\n", e); goto cleanup; }
    data = malloc(bytes); if (!data) goto cleanup;
    glGenFramebuffersEXT(2, fb); glGenRenderbuffersEXT(2, rb);
    if (!gl_ok("allocate names")) goto cleanup;
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (unsigned j = 0; j < 2; ++j) {
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[j]);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 256, 256);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb[j]);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                      GL_RENDERBUFFER_EXT, rb[j]);
        GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        if (!gl_ok("FBO storage") || status != GL_FRAMEBUFFER_COMPLETE_EXT) {
            fprintf(stderr, "fb=%u incomplete=0x%x\n", fb[j], (unsigned)status); goto cleanup;
        }
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT); glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
        glClearColor(colors[j][0], colors[j][1], colors[j][2], colors[j][3]);
        glClear(GL_COLOR_BUFFER_BIT); glFinish();
        if (!gl_ok("clear/finish")) goto cleanup;
    }
    /* Read A after B was submitted, then B, then A again. */
    if (!read_check(fb[0], expected[0], data, bytes) ||
        !read_check(fb[1], expected[1], data, bytes) ||
        !read_check(fb[0], expected[0], data, bytes)) goto cleanup;
    ok = 1;
cleanup:
    if (ctx && CGLGetCurrentContext() == ctx) {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
        glDeleteFramebuffersEXT(2, fb); glDeleteRenderbuffersEXT(2, rb);
        if (!gl_ok("delete resources")) ok = 0;
        e = CGLSetCurrentContext(NULL); if (e) ok = 0;
    }
    if (ctx && CGLDestroyContext(ctx)) ok = 0;
    if (pf && CGLDestroyPixelFormat(pf)) ok = 0;
    free(data);
    printf("PUBLIC_CGL_BGRA %s (clear/read only; triangle not tested)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
