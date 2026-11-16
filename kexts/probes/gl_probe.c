// 使用系统 OpenGL framework 验证上下文和实际栅格化。
// 默认和软件上下文仅作诊断；成功必须同时满足加速属性与像素回读校验。
// 构建与运行必须在 macOS 客体中实际进行。
// clang -Wall -Wextra -Werror -framework OpenGL -framework IOKit \
//   -framework CoreFoundation -o /tmp/glprobe gl_probe.c
#define GL_SILENCE_DEPRECATION
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <mach-o/dyld.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLRenderers.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsInterface.h>

/* Exercise the same public framebuffer association used by libGFXShared. */
static void report_accelerator_links(void) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("IOFramebuffer"), &iterator);
    if (kr != KERN_SUCCESS) {
        printf("accelerator links: matching failed=0x%08x\n", (unsigned)kr);
        return;
    }
    io_service_t fb;
    unsigned count = 0;
    while ((fb = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        io_name_t name = {0};
        io_service_t accelerator = IO_OBJECT_NULL;
        UInt32 index = 0;
        IORegistryEntryGetName(fb, name);
        ++count;
        CFTypeRef path = IORegistryEntryCreateCFProperty(fb, CFSTR("IOAccelTypes"),
            kCFAllocatorDefault, 0);
        CFTypeRef number = IORegistryEntryCreateCFProperty(fb, CFSTR("IOAccelIndex"),
            kCFAllocatorDefault, 0);
        int valid = (!path || CFGetTypeID(path) == CFStringGetTypeID()) &&
                    (!number || CFGetTypeID(number) == CFNumberGetTypeID());
        printf("accelerator links: framebuffer=%s IOAccelTypes=%s IOAccelIndex=%s\n",
            name, path ? "present" : "absent", number ? "present" : "absent");
        /* The OS implementation does not validate these CF types itself. */
        if (valid) {
            kr = IOAccelFindAccelerator(fb, &accelerator, &index);
            printf("accelerator links: IOAccelFindAccelerator=0x%08x index=%u found=%d\n",
                (unsigned)kr, (unsigned)index, accelerator != IO_OBJECT_NULL);
            if (kr == KERN_SUCCESS && accelerator != IO_OBJECT_NULL) {
                io_string_t accelerator_path = {0};
                if (IORegistryEntryGetPath(accelerator, kIOServicePlane,
                                          accelerator_path) == KERN_SUCCESS)
                    printf("accelerator links: path=%s\n", accelerator_path);
            }
        } else {
            puts("accelerator links: skipped malformed registry property types");
        }
        if (accelerator != IO_OBJECT_NULL) IOObjectRelease(accelerator);
        if (path) CFRelease(path);
        if (number) CFRelease(number);
        IOObjectRelease(fb);
    }
    IOObjectRelease(iterator);
    printf("accelerator links: framebuffers=%u\n", count);
}

static void report_renderers(void) {
    CGLRendererInfoObj info = NULL;
    GLint count = 0;
    CGLError e = CGLQueryRendererInfo(0xffffffffu, &info, &count);
    if (e != kCGLNoError) {
        printf("renderers: CGLQueryRendererInfo(displaymask=0xffffffff) failed: error=%d (%s)\n",
               (int)e, CGLErrorString(e));
        goto cleanup;
    }
    if (info == NULL || count < 0) {
        printf("renderers: CGLQueryRendererInfo returned invalid result: info=%s count=%d\n",
               info == NULL ? "NULL" : "non-NULL", count);
        goto cleanup;
    }
    printf("renderers: displaymask=0xffffffff count=%d\n", count);
    const struct {
        CGLRendererProperty property;
        const char *name;
        int hex;
    } properties[] = {
        {kCGLRPRendererID, "kCGLRPRendererID", 1},
        {kCGLRPAccelerated, "kCGLRPAccelerated", 0},
        {kCGLRPOnline, "kCGLRPOnline", 0},
        {kCGLRPDisplayMask, "kCGLRPDisplayMask", 1}
    };
    for (GLint renderer = 0; renderer < count; ++renderer) {
        for (size_t p = 0; p < sizeof(properties) / sizeof(properties[0]); ++p) {
            GLint value = 0;
            e = CGLDescribeRenderer(info, renderer, properties[p].property, &value);
            if (e != kCGLNoError) {
                printf("renderers[%d]: CGLDescribeRenderer(%s) failed: error=%d (%s)\n",
                       renderer, properties[p].name, (int)e, CGLErrorString(e));
            } else if (properties[p].hex) {
                printf("renderers[%d]: %s=0x%08x\n",
                       renderer, properties[p].name, (unsigned)value);
            } else {
                printf("renderers[%d]: %s=%d\n", renderer, properties[p].name, value);
            }
        }
    }
cleanup:
    if (info != NULL) {
        e = CGLDestroyRendererInfo(info);
        if (e != kCGLNoError) {
            printf("renderers: CGLDestroyRendererInfo failed: error=%d (%s)\n",
                   (int)e, CGLErrorString(e));
        }
    }
}

static void report_graphics_images(void) {
    uint32_t count = _dyld_image_count();
    printf("graphics images: loaded OpenGL/GLDriver/Metal/IOAccelerator/GPUSupport images\n");
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (name != NULL &&
            (strstr(name, "OpenGL") || strstr(name, "GLDriver") ||
             strstr(name, "Metal") || strstr(name, "IOAccelerator") ||
             strstr(name, "GPUSupport"))) {
            printf("graphics images: %s\n", name);
        }
    }
}

static int check_gl(const char *label, const char *stage) {
    int ok = 1;
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        printf("%s: %s GL error=0x%04x\n", label, stage, (unsigned)error);
        ok = 0;
    }
    return ok;
}

/* Public-API resource tests: all images use ordinary CPU uploads, no IOSurface
 * import. A software pass is a reference result, never hardware acceptance. */
enum { RESOURCE_MAX_DIM = 17, RESOURCE_GUARD = 16 };

static void resource_pixel(unsigned char *p, int x, int y, unsigned seed) {
    p[0] = (unsigned char)(seed + 17u * (unsigned)x + 3u * (unsigned)y);
    p[1] = (unsigned char)(seed * 3u + 5u * (unsigned)x + 29u * (unsigned)y);
    p[2] = (unsigned char)(seed * 7u + 11u * (unsigned)x + 13u * (unsigned)y);
    p[3] = (unsigned char)(31u + (seed + 7u * (unsigned)x + 19u * (unsigned)y) % 211u);
}

static int resource_dimensions(const char *label, const char *stage, int w, int h) {
    GLint actual_w = 0, actual_h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actual_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actual_h);
    if (!check_gl(label, stage)) return 0;
    if (actual_w != w || actual_h != h) {
        printf("%s: resources %s dimensions=%dx%d expected=%dx%d -> FAIL\n",
               label, stage, actual_w, actual_h, w, h);
        return 0;
    }
    return 1;
}

static int resource_upload(const char *label, int w, int h, unsigned seed,
                           unsigned char *expected) {
    unsigned char source[(RESOURCE_MAX_DIM + 2) * (RESOURCE_MAX_DIM + 5) * 4];
    if (w <= 0 || h <= 0 || w > RESOURCE_MAX_DIM || h > RESOURCE_MAX_DIM) return 0;
    const size_t stride = ((size_t)(w + 3) * 4 + 7) & ~(size_t)7;
    memset(source, 0xa6, sizeof(source));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char *p = expected + ((size_t)y * (size_t)w + (size_t)x) * 4;
            resource_pixel(p, x, y, seed);
            memcpy(source + (size_t)(y + 1) * stride + (size_t)(x + 1) * 4, p, 4);
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w + 3);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    return check_gl(label, "resource upload/restore unpack") &&
           resource_dimensions(label, "upload dimensions", w, h);
}

static int resource_read(const char *label, const char *stage, int w, int h,
                         const unsigned char *expected, int via_texture) {
    unsigned char result[RESOURCE_MAX_DIM * RESOURCE_MAX_DIM * 4 + 2 * RESOURCE_GUARD];
    if (w <= 0 || h <= 0 || w > RESOURCE_MAX_DIM || h > RESOURCE_MAX_DIM) return 0;
    const size_t bytes = (size_t)w * (size_t)h * 4;
    memset(result, 0x5a, sizeof(result));
    if (!via_texture) {
        GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        if (!check_gl(label, stage) || status != GL_FRAMEBUFFER_COMPLETE_EXT) {
            printf("%s: resources %s incomplete FBO=0x%04x\n", label, stage, (unsigned)status);
            return 0;
        }
    }
    if (via_texture && !resource_dimensions(label, stage, w, h)) return 0;
    glFinish();
    if (via_texture)
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, result + RESOURCE_GUARD);
    else
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, result + RESOURCE_GUARD);
    if (!check_gl(label, stage)) return 0;
    size_t mismatch = 0, guard_mismatch = 0;
    for (size_t i = 0; i < bytes; ++i)
        if (result[RESOURCE_GUARD + i] != expected[i]) ++mismatch;
    for (size_t i = 0; i < sizeof(result); ++i)
        if ((i < RESOURCE_GUARD || i >= RESOURCE_GUARD + bytes) && result[i] != 0x5a)
            ++guard_mismatch;
    printf("%s: resources %s %dx%d bytes=%zu mismatch=%zu guard=%zu -> %s\n",
           label, stage, w, h, bytes, mismatch, guard_mismatch,
           (mismatch || guard_mismatch) ? "FAIL" : "PASS");
    return mismatch == 0 && guard_mismatch == 0;
}

static void resource_attach(GLuint texture) {
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                             GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
}

static int check_texture_resources(const char *label) {
    GLuint fbo = 0, textures[2] = {0, 0};
    unsigned char a[RESOURCE_MAX_DIM * RESOURCE_MAX_DIM * 4];
    unsigned char b[RESOURCE_MAX_DIM * RESOURCE_MAX_DIM * 4];
    int ok = 0;
    glGenFramebuffersEXT(1, &fbo);
    glGenTextures(2, textures);
    if (!check_gl(label, "resource object creation") || !fbo || !textures[0] || !textures[1])
        goto cleanup;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    for (unsigned i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    if (!resource_upload(label, 13, 7, 23, a)) goto cleanup;
    resource_attach(textures[0]);
    if (!resource_read(label, "upload-A", 13, 7, a, 0)) goto cleanup;

    /* 3x2 patch from a padded/offset source: bytes [28,40), [52,64).
     * The reference image is updated by destination coordinates separately. */
    unsigned char patch[96];
    memset(patch, 0xc7, sizeof(patch));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            unsigned char p[4];
            resource_pixel(p, x, y, 181);
            memcpy(patch + (size_t)(y + 1) * 24 + (size_t)(x + 1) * 4, p, 4);
            memcpy(a + ((size_t)(y + 2) * 13 + (size_t)(x + 4)) * 4, p, 4);
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 5);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 4, 2, 3, 2, GL_RGBA, GL_UNSIGNED_BYTE, patch);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    if (!check_gl(label, "resource subimage") ||
        !resource_read(label, "subimage-A", 13, 7, a, 0)) goto cleanup;

    glBindTexture(GL_TEXTURE_2D, textures[1]);
    if (!resource_upload(label, 9, 11, 79, b)) goto cleanup;
    resource_attach(textures[1]);
    if (!resource_read(label, "switch-B", 9, 11, b, 0)) goto cleanup;
    resource_attach(textures[0]);
    if (!resource_read(label, "preserved-A", 13, 7, a, 0)) goto cleanup;

    /* Redefine storage while the image remains attached to the FBO. */
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    if (!resource_upload(label, 5, 17, 113, a) ||
        !resource_read(label, "resize-attached-A", 5, 17, a, 0)) goto cleanup;
    glDeleteTextures(1, &textures[0]);
    textures[0] = 0;
    GLint attachment_type = -1;
    glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                             GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT,
                                             &attachment_type);
    /* Isolate the missing-attachment rule from draw/read-buffer completeness. */
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum detached_status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (!check_gl(label, "delete attached texture") || attachment_type != GL_NONE ||
        detached_status != GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_EXT) {
        printf("%s: resources delete-attachment type=0x%x status=0x%x -> FAIL\n",
               label, (unsigned)attachment_type, (unsigned)detached_status);
        goto cleanup;
    }
    printf("%s: resources delete-attachment -> PASS\n", label);
    resource_attach(textures[1]);
    if (!resource_read(label, "B-survives-A-resize-delete", 9, 11, b, 0)) goto cleanup;
    glGenTextures(1, &textures[0]);
    if (!check_gl(label, "recreate texture") || !textures[0]) goto cleanup;
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (!resource_upload(label, 7, 5, 151, a)) goto cleanup;
    resource_attach(textures[0]);
    if (!resource_read(label, "reattach-new-A", 7, 5, a, 0)) goto cleanup;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glDeleteFramebuffersEXT(1, &fbo);
    fbo = 0;
    if (!check_gl(label, "destroy FBO") ||
        !resource_read(label, "texture-survives-FBO", 7, 5, a, 1)) goto cleanup;
    ok = 1;
cleanup:
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (fbo) glDeleteFramebuffersEXT(1, &fbo);
    glDeleteTextures(2, textures);
    if (!check_gl(label, "resource cleanup")) ok = 0;
    printf("%s: ordinary texture/FBO resource lifecycle -> %s\n", label, ok ? "PASS" : "FAIL");
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
    /* Separate clear/readback from drawing so a broken clear path cannot be
     * hidden by the triangle. Nonzero asymmetric channels also catch swizzles. */
    glClearColor(17.0f / 255.0f, 83.0f / 255.0f, 149.0f / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    unsigned char cleared[64 * 64 * 4];
    memset(cleared, 0x5a, sizeof(cleared));
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, cleared);
    if (!check_gl(label, "clear/readback")) goto cleanup;
    size_t clear_mismatch = 0;
    for (size_t i = 0; i < 64 * 64; ++i) {
        const unsigned char *p = cleared + i * 4;
        if (p[0] != 17 || p[1] != 83 || p[2] != 149 || p[3] != 255)
            ++clear_mismatch;
    }
    printf("%s: clear/readback pixels=4096 mismatch=%zu -> %s\n",
           label, clear_mismatch, clear_mismatch ? "FAIL" : "PASS");
    if (clear_mismatch) goto cleanup;
    /* Exercise shared pack state: five RGBA pixels per row round to 24 bytes
     * at alignment eight; skip one row and one pixel before a 3x2 read. */
    unsigned char packed[96];
    memset(packed, 0x5a, sizeof(packed));
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    glPixelStorei(GL_PACK_ROW_LENGTH, 5);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
    glReadPixels(2, 3, 3, 2, GL_RGBA, GL_UNSIGNED_BYTE, packed);
    int pack_ok = check_gl(label, "strided readback");
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    if (!check_gl(label, "restore pack state") || !pack_ok) goto cleanup;
    size_t pack_mismatch = 0;
    const unsigned char clear_rgba[4] = {17, 83, 149, 255};
    for (size_t i = 0; i < sizeof(packed); ++i) {
        int selected = (i >= 28 && i < 40) || (i >= 52 && i < 64);
        unsigned char expected = selected ? clear_rgba[i % 4] : 0x5a;
        if (packed[i] != expected) ++pack_mismatch;
    }
    printf("%s: strided readback bytes=96 mismatch=%zu -> %s\n",
           label, pack_mismatch, pack_mismatch ? "FAIL" : "PASS");
    if (pack_mismatch) goto cleanup;
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
    if (ok) ok = check_texture_resources(label);

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

int main(int argc, char **argv) {
    int software_only = argc == 2 && strcmp(argv[1], "--software-only") == 0;
    if (argc != 1 && !software_only) {
        fprintf(stderr, "Usage: %s [--software-only]\n", argv[0]);
        return 2;
    }
    report_accelerator_links();
    // 默认和软件结果不参与硬件验收判定。
    if (!software_only) {
        CGLPixelFormatAttribute any[] = {
            kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
            kCGLPFADoubleBuffer, (CGLPixelFormatAttribute)0
        };
        (void)report("default", any, 0);
    }

    CGLPixelFormatAttribute soft[] = {
        kCGLPFARendererID, (CGLPixelFormatAttribute)kCGLRendererGenericFloatID,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    int software_ok = report("software", soft, 0);
    if (software_only) {
        puts("SOFTWARE DIAGNOSTIC ONLY: hardware acceleration was not tested.");
        return software_ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // 禁止软件回退，并核对实际虚拟屏幕的加速属性。
    CGLPixelFormatAttribute accel[] = {
        kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    int ok = report("accelerated", accel, 1);
    report_renderers();
    report_graphics_images();
    if (!ok) {
        // 会话条件与硬件枚举需独立取证，不能从 CGL 错误推断驱动不支持。
        printf("FAIL: accelerated OpenGL acceptance did not pass in this session.\n");
        printf("CGL failure alone does not prove driver incompatibility or absent hardware.\n");
        printf("Record Aqua/login-session availability and IOAccelerator enumeration separately.\n");
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
