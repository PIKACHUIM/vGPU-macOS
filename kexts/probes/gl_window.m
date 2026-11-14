// A window that renders with OpenGL, so "OpenGL 可用" can be shown and not just asserted.
//
// The point of a window (rather than an offscreen FBO) is that it proves the whole chain on
// this guest: a software GL renderer producing pixels, into a surface the window server
// composites, on a framebuffer this project's kext is driving. A renderer string alone
// proves none of that.
//
// First version never appeared on screen: it pumped NSRunLoop by hand instead of letting
// NSApplication run, so the window was created but never ordered onto the display. It now
// goes through -[NSApplication run] and logs each step, so "no window" can be told apart
// from "no window server connection".
//
// Build: clang -fobjc-arc -DGL_SILENCE_DEPRECATION -framework Cocoa -framework OpenGL \
//            -o /tmp/glwin gl_window.m
// Run inside the GUI session: sudo launchctl asuser $(id -u pika) sudo -u pika /tmp/glwin
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

@interface GLView : NSOpenGLView
@end

@implementation GLView
- (void)prepareOpenGL {
    [super prepareOpenGL];
    note("prepareOpenGL: RENDERER=%s VERSION=%s GLSL=%s",
         glGetString(GL_RENDERER), glGetString(GL_VERSION),
         glGetString(GL_SHADING_LANGUAGE_VERSION));
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[self openGLContext] makeCurrentContext];
    NSRect b = [self bounds];
    glViewport(0, 0, (GLsizei)b.size.width, (GLsizei)b.size.height);
    glClearColor(0.05f, 0.06f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBegin(GL_TRIANGLES);
    glColor3f(0.10f, 0.95f, 0.35f);
    glVertex2f(-0.85f, -0.85f);
    glVertex2f(0.85f, -0.85f);
    glVertex2f(0.0f, 0.85f);
    glEnd();

    glBegin(GL_QUADS);
    for (int i = 0; i < 32; i++) {
        float t = (float)i / 31.0f;
        float x0 = -1.0f + 2.0f * t, x1 = -1.0f + 2.0f * (t + 1.0f / 32.0f);
        glColor3f(t, 0.2f, 1.0f - t);
        glVertex2f(x0, -1.0f);
        glVertex2f(x1, -1.0f);
        glVertex2f(x1, -0.90f);
        glVertex2f(x0, -0.90f);
    }
    glEnd();

    glFinish();
    [[self openGLContext] flushBuffer];
    note("drawRect done (%.0fx%.0f)", b.size.width, b.size.height);
}
@end

int main(void) {
    @autoreleasepool {
        note("main: start, pid=%d", (int)getpid());
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(60, 60, 760, 560);
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [win setTitle:@"OpenGL on VMware SVGA (software renderer)"];
        [win setBackgroundColor:[NSColor magentaColor]];   // visible even if GL never draws

        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFADoubleBuffer,
            0
        };
        NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (pf == nil) {
            note("main: NSOpenGLPixelFormat is nil -- no renderer for these attributes");
        }
        GLView *view = [[GLView alloc] initWithFrame:frame pixelFormat:pf];
        [win setContentView:view];

        [app finishLaunching];
        [win makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        note("main: window ordered front (visible=%d)", (int)[win isVisible]);

        // Quit on its own so a capture cannot be blocked by a stuck app.
        [NSTimer scheduledTimerWithTimeInterval:45.0 repeats:NO block:^(NSTimer *t) {
            (void)t;
            note("main: 45 s elapsed, terminating");
            [NSApp stop:nil];
            [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                location:NSZeroPoint
                                           modifierFlags:0
                                               timestamp:0
                                            windowNumber:0
                                                 context:nil
                                                 subtype:0
                                                   data1:0
                                                   data2:0]
                     atStart:YES];
        }];
        [app run];
        note("main: run loop returned");
    }
    return 0;
}
