/* 公开 CGL 受限红三角形验收；只调用公开 OpenGL，不链接驱动内部实现。
 * --context-only 只验初始化/销毁；--clear-read 验双FBO背景和pack，不绘制。
 * --gpu 为完整正向用例，--reject-color 为独立受限实现负向用例。
 * --pbuffer-choose-only / --pbuffer-context-only / --pbuffer-clear-read
 * 独立验 PBuffer 格式、无 drawable context、默认单缓冲 clear/read；不创建 FBO。
 * --oracle-only 不创建 CGL context；TRIANGLE_ORACLE_ONLY 编译完全不引用 OpenGL。
 * 整数边判据源自 gld_resource_probe.c，但这里独立使用 GL 左下原点坐标。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef TRIANGLE_ORACLE_ONLY
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#endif

enum { WIDTH = 256, HEIGHT = 256, STRIDE = 1040, START = 1048,
       BYTES = START + HEIGHT * STRIDE + 32 };
struct triangle { float v[3][4]; int64_t window2[3][2]; unsigned count; };
/* x/y/w 不经被测代码计算；z 特意跨越 GL [-1,1] 的负半区。
 * GL window2 = 256 * (NDC + 1)，不是资源 ABI 的自上而下行方向。
 * 两组边均不经过像素中心，避免边覆盖归属规则歧义。 */
static const struct triangle triangles[2] = {
    {{{-.6875f,-.5625f,-1.f,1.f},{.3125f,-.5625f,-.5f,1.f},
      {-.6875f,.21875f,.5f,1.f}}, {{80,112},{336,112},{80,312}}, 6400},
    {{{-.75f,.75f,-.75f,1.f},{-.125f,-.5f,0.f,1.f},
      {.5f,.75f,1.f,1.f}}, {{64,448},{224,128},{384,448}}, 12800}
};
static const unsigned char backgrounds[4][4] = {
    {0,0,255,64},{255,0,255,128},
    {17,83,149,211},{231,157,79,23}
};
static const unsigned char red[4] = {255,0,0,255};

static bool check(bool ok, const char *label)
{
    if (!ok) fprintf(stderr,"FAIL %s\n",label);
    return ok;
}
static int64_t edge(const int64_t a[2], const int64_t b[2], int64_t x, int64_t y)
{
    return (b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]);
}
static bool inside(unsigned t, unsigned x, unsigned y, bool *on_edge)
{
    const struct triangle *p = &triangles[t];
    int64_t px = 2*(int64_t)x+1, py = 2*(int64_t)y+1;
    int64_t a=edge(p->window2[0],p->window2[1],px,py);
    int64_t b=edge(p->window2[1],p->window2[2],px,py);
    int64_t c=edge(p->window2[2],p->window2[0],px,py);
    if (!a || !b || !c) *on_edge=true;
    return (a>0 && b>0 && c>0) || (a<0 && b<0 && c<0);
}
static bool covered(unsigned mask, unsigned x, unsigned y)
{
    bool boundary=false;
    return ((mask & 1u) && inside(0,x,y,&boundary)) ||
           ((mask & 2u) && inside(1,x,y,&boundary));
}
static unsigned char expected_byte(size_t k, unsigned target, unsigned mask)
{
    if (k < START) return 0xa5;
    size_t q=k-START, y=q/STRIDE, offset=q%STRIDE;
    if (y >= HEIGHT || offset >= WIDTH*4) return 0xa5;
    const unsigned char *rgba=covered(mask,(unsigned)(offset/4),(unsigned)y)
                              ? red : backgrounds[target];
    return rgba[offset%4];
}
static bool verify(const unsigned char *data, unsigned target, unsigned mask, bool report)
{
    for (size_t k=0;k<BYTES;++k) {
        unsigned char want=expected_byte(k,target,mask);
        if (data[k] != want) {
            if (report) fprintf(stderr,"FAIL target=%u mask=%u byte=%zu got=%u expected=%u\n",
                                target,mask,k,data[k],want);
            return false;
        }
    }
    return true;
}
static bool oracle_selftest(void)
{
    unsigned counts[2]={0}, overlap=0, only[2]={0}, flipped=0;
    bool boundary=false, negative_z=false;
    for (unsigned t=0;t<2;++t) for (unsigned v=0;v<3;++v) {
        const float *p=triangles[t].v[v];
        if (!check(p[3]==1.f && p[2]>=-1.f && p[2]<=1.f,"合法 GL clip 顶点")) return false;
        negative_z |= p[2]<0.f;
        for (unsigned axis=0;axis<2;++axis)
            if (!check(256.0*((double)p[axis]+1.0)==(double)triangles[t].window2[v][axis],
                       "独立 GL viewport 坐标常量")) return false;
    }
    for (unsigned y=0;y<HEIGHT;++y) for (unsigned x=0;x<WIDTH;++x) {
        bool a=inside(0,x,y,&boundary), b=inside(1,x,y,&boundary);
        counts[0]+=a; counts[1]+=b; overlap+=a && b;
        only[0]+=a && !b; only[1]+=b && !a;
        flipped+=a != inside(0,x,HEIGHT-1-y,&boundary);
    }
    if (!check(!boundary && negative_z && counts[0]==6400 && counts[1]==12800 &&
               overlap>0 && only[0]>0 && only[1]>0 && flipped>0,
               "全图覆盖、累积独有区域及行翻转可检测")) return false;
    unsigned char *data=malloc(BYTES);
    if (!check(data!=NULL,"CPU oracle 分配")) return false;
    for (unsigned target=0;target<2;++target) for (unsigned mask=0;mask<4;++mask) {
        for (size_t k=0;k<BYTES;++k) data[k]=expected_byte(k,target,mask);
        if (!verify(data,target,mask,true)) { free(data); return false; }
        const size_t mutations[]={0,START-1,START,START+3,START+WIDTH*4,
                                  START+STRIDE*57+40*4,BYTES-1};
        for (size_t j=0;j<sizeof(mutations)/sizeof(mutations[0]);++j) {
            data[mutations[j]]^=1;
            bool rejected=!verify(data,target,mask,false);
            data[mutations[j]]^=1;
            if (!check(rejected,"整图/alpha/pack/padding 变异检测")) { free(data); return false; }
        }
    }
    for (unsigned target=2;target<4;++target) {
        memset(data,0xa5,BYTES);
        for (unsigned y=0;y<HEIGHT;++y) for (unsigned x=0;x<WIDTH;++x)
            memcpy(data + (y+1)*1040 + (x+2)*4,backgrounds[target],4);
        if (!verify(data,target,0,true)) { free(data); return false; }
        const size_t mutations[]={0,START-1,START,START+1,START+2,START+3,
                                  START+WIDTH*4,START+(HEIGHT-1)*STRIDE+(WIDTH-1)*4+3,
                                  START+HEIGHT*STRIDE,BYTES-1};
        for (size_t j=0;j<sizeof(mutations)/sizeof(mutations[0]);++j) {
            data[mutations[j]]^=1;
            bool rejected=!verify(data,target,0,false);
            data[mutations[j]]^=1;
            if (!check(rejected,"PBuffer RGBA/首尾像素/pack 哨兵变异检测")) { free(data); return false; }
        }
        unsigned char saved=data[START]; data[START]=data[START+2]; data[START+2]=saved;
        bool rejected=!verify(data,target,0,false);
        if (!check(rejected,"PBuffer R/B 互换检测")) { free(data); return false; }
    }
    puts("CPU_PBUFFER_ORACLE PASS colors=17,83,149,211;231,157,79,23 pack=1040/1048");
    free(data);
    printf("CPU_ORACLE PASS triangles=%u,%u overlap=%u union=%u exclusive=%u,%u flipped=%u\n",
           counts[0],counts[1],overlap,counts[0]+counts[1]-overlap,only[0],only[1],flipped);
    puts("仅 CPU oracle 自检，不代表公开 CGL/GPU 验收通过。");
    return true;
}

#ifndef TRIANGLE_ORACLE_ONLY
static bool gl_ok(const char *label)
{
    GLenum e=glGetError();
    if (e==GL_NO_ERROR) return true;
    fprintf(stderr,"FAIL %s GL error=0x%x\n",label,(unsigned)e);
    return false;
}
static bool cgl_ok(CGLError e, const char *label)
{
    if (e==kCGLNoError) return true;
    fprintf(stderr,"FAIL %s CGL error=%d (%s)\n",label,(int)e,CGLErrorString(e));
    return false;
}
static bool image(GLuint fb, unsigned target, unsigned mask, unsigned char *data)
{
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fb);
    if (!gl_ok("绑定读 FBO")) return false;
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glPixelStorei(GL_PACK_ALIGNMENT,8);
    glPixelStorei(GL_PACK_ROW_LENGTH,259);
    glPixelStorei(GL_PACK_SKIP_ROWS,1);
    glPixelStorei(GL_PACK_SKIP_PIXELS,2);
    if (!gl_ok("pack 设置")) return false;
    memset(data,0xa5,BYTES);
    glReadPixels(0,0,WIDTH,HEIGHT,GL_RGBA,GL_UNSIGNED_BYTE,data);
    if (!gl_ok("公开 glReadPixels") || !verify(data,target,mask,true)) return false;
    printf("PASS FBO=%u target=%u mask=%u 全图 RGBA=65536 pack/padding 精确\n",fb,target,mask);
    return true;
}
static bool draw(GLuint fb, unsigned shape, bool reject_color)
{
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fb);
    if (!gl_ok("绑定绘图 FBO")) return false;
    glColor4f(1.f,0.f,0.f,1.f);
    if (!gl_ok("初始红色")) return false;
    glBegin(GL_TRIANGLES);
    for (unsigned v=0;v<3;++v) {
        /* glBegin/glEnd 内不可调用 glGetError；保留红绿红历史，不能只看最终颜色。 */
        if (reject_color) glColor4f(v==1 ? 0.f : 1.f,v==1 ? 1.f : 0.f,0.f,1.f);
        if (shape == 0)
            glVertex3f(triangles[shape].v[v][0],triangles[shape].v[v][1],triangles[shape].v[v][2]);
        else
            glVertex4f(triangles[shape].v[v][0],triangles[shape].v[v][1],
                       triangles[shape].v[v][2],triangles[shape].v[v][3]);
    }
    glEnd();
    if (reject_color) {
        GLenum e=glGetError();
        if (!check(e==GL_INVALID_OPERATION,"受限颜色历史必须 GL_INVALID_OPERATION")) {
            fprintf(stderr,"actual GL error=0x%x\n",(unsigned)e); return false;
        }
        if (!gl_ok("颜色拒绝后无额外错误")) return false;
        /* GetError 不解除提交拒绝；finish 必须仍报告该拒绝，不能冒报旧完成。 */
        glFinish();
        e=glGetError();
        if (!check(e==GL_INVALID_OPERATION,"拒绝后的 finish 保留失败门禁")) return false;
        return gl_ok("finish 错误已消费");
    } else if (!gl_ok("公开 glBegin/glVertex/glEnd")) return false;
    glFinish();
    return gl_ok("绘图 finish");
}
#define REQUIRE(x) do { if (!(x)) goto cleanup; } while (0)
enum public_stage { STAGE_CHOOSE, STAGE_CONTEXT, STAGE_CLEAR_READ, STAGE_TRIANGLE };
static int pbuffer_probe(enum public_stage stage)
{
    CGLPixelFormatObj pf=NULL; CGLContextObj ctx=NULL; CGLPBufferObj pb=NULL;
    GLint count=0, pbuffer=0, accelerated=0, recovery=0, renderer=0, current_renderer=0;
    GLint double_buffer=0, triple_buffer=0, profile=0, screen=0;
    unsigned char *data=NULL;
    bool ok=false, attach_attempted=false, detached=true, unbound=true;
    const CGLPixelFormatAttribute attrs[]={
        kCGLPFAPBuffer,kCGLPFAAccelerated,kCGLPFANoRecovery,
        kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
        kCGLPFARendererID,(CGLPixelFormatAttribute)0x00027e00,
        kCGLPFAColorSize,(CGLPixelFormatAttribute)32,
        kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,(CGLPixelFormatAttribute)0
    };
    REQUIRE(cgl_ok(CGLChoosePixelFormat(attrs,&pf,&count),"PBuffer 选择格式"));
    REQUIRE(check(pf && count>0,"PBuffer 格式非空"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFAPBuffer,&pbuffer),"PBuffer 属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFAAccelerated,&accelerated),"PBuffer 加速属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFANoRecovery,&recovery),"PBuffer 禁止恢复属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFARendererID,&renderer),"PBuffer renderer"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFADoubleBuffer,&double_buffer),"PBuffer 双缓冲属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFATripleBuffer,&triple_buffer),"PBuffer 三缓冲属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFAOpenGLProfile,&profile),"PBuffer profile"));
    REQUIRE(check(pbuffer && accelerated && recovery && !double_buffer && !triple_buffer &&
                  profile==kCGLOGLPVersion_Legacy &&
                  ((unsigned)renderer & 0x00ffff00u)==0x00027e00u,
                  "必须自有 PBuffer/accelerated/noRecovery/legacy/单缓冲格式"));
    printf("PBUFFER_FORMAT renderer=0x%08x pbuffer=%d accelerated=%d noRecovery=%d profile=0x%x double=%d triple=%d\n",
           (unsigned)renderer,pbuffer,accelerated,recovery,(unsigned)profile,double_buffer,triple_buffer);
    fflush(stdout);
    if (stage==STAGE_CHOOSE) { ok=true; goto cleanup; }
    REQUIRE(cgl_ok(CGLCreateContext(pf,NULL,&ctx),"PBuffer 创建 context"));
    REQUIRE(check(ctx!=NULL,"PBuffer context 非空"));
    REQUIRE(cgl_ok(CGLGetParameter(ctx,kCGLCPCurrentRendererID,&current_renderer),"PBuffer 当前 renderer"));
    REQUIRE(check(((unsigned)current_renderer & 0x00ffff00u)==0x00027e00u,"PBuffer 当前自有 renderer"));
    /* context-only 只验创建/销毁，不创建或附加 drawable，也不调用 GL。 */
    if (stage==STAGE_CONTEXT) { ok=true; goto cleanup; }
    REQUIRE(cgl_ok(CGLSetCurrentContext(ctx),"PBuffer 绑定 context"));
    REQUIRE(cgl_ok(CGLGetVirtualScreen(ctx,&screen),"PBuffer 当前虚拟屏幕"));
    REQUIRE(check(screen==0,"PBuffer 使用已验证格式的虚拟屏幕 0"));
    /* 2D 的 face 必须为 0；无 mipmap 的 max_level 和 level 均为 0。
     * CGLCreatePBuffer 的 internalFormat 接受 GL_RGBA，不是 GL_RGBA8。 */
    REQUIRE(cgl_ok(CGLCreatePBuffer(WIDTH,HEIGHT,GL_TEXTURE_2D,GL_RGBA,0,&pb),"创建 256x256 PBuffer"));
    REQUIRE(check(pb!=NULL,"PBuffer 对象非空"));
    GLsizei width=0,height=0; GLenum target=0,format=0; GLint max_level=-1;
    REQUIRE(cgl_ok(CGLDescribePBuffer(pb,&width,&height,&target,&format,&max_level),"PBuffer 描述"));
    REQUIRE(check(width==WIDTH && height==HEIGHT && target==GL_TEXTURE_2D &&
                  format==GL_RGBA && max_level==0,"PBuffer 尺寸/目标/格式/层级"));
    attach_attempted=true; detached=false;
    REQUIRE(cgl_ok(CGLSetPBuffer(ctx,pb,0,0,screen),"附加 PBuffer face=0 level=0"));
    CGLPBufferObj attached=NULL; GLenum face=1; GLint level=-1, attached_screen=-1;
    REQUIRE(cgl_ok(CGLGetPBuffer(ctx,&attached,&face,&level,&attached_screen),"查询 PBuffer 附加"));
    REQUIRE(check(attached==pb && face==0 && level==0 && attached_screen==screen,"PBuffer 附加必须精确匹配"));
    REQUIRE(cgl_ok(CGLGetParameter(ctx,kCGLCPCurrentRendererID,&current_renderer),"附加后 renderer"));
    REQUIRE(check(((unsigned)current_renderer & 0x00ffff00u)==0x00027e00u,"附加后仍为自有 renderer"));
    data=malloc(BYTES); REQUIRE(check(data!=NULL,"PBuffer 读缓冲分配"));
    /* 新 context 的默认 framebuffer 为 0；不分配 FBO/RBO 或额外纹理。 */
    GLint framebuffer=-1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT,&framebuffer);
    REQUIRE(gl_ok("PBuffer 默认 framebuffer 查询"));
    REQUIRE(check(framebuffer==0,"PBuffer 必须使用默认目标"));
    glDrawBuffer(GL_FRONT); glReadBuffer(GL_FRONT);
    glViewport(0,0,WIDTH,HEIGHT);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DITHER);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glPixelStorei(GL_PACK_ALIGNMENT,8); glPixelStorei(GL_PACK_ROW_LENGTH,259);
    glPixelStorei(GL_PACK_SKIP_ROWS,1); glPixelStorei(GL_PACK_SKIP_PIXELS,2);
    REQUIRE(gl_ok("PBuffer 单缓冲默认目标及 pack 设置"));
    for (unsigned color=2;color<4;++color) {
        glClearColor(backgrounds[color][0]/255.f,backgrounds[color][1]/255.f,
                     backgrounds[color][2]/255.f,backgrounds[color][3]/255.f);
        REQUIRE(gl_ok("PBuffer 非对称 RGBA"));
        glClear(GL_COLOR_BUFFER_BIT); REQUIRE(gl_ok("PBuffer 默认目标 clear"));
        glFinish(); REQUIRE(gl_ok("PBuffer clear finish"));
        memset(data,0xa5,BYTES);
        glReadPixels(0,0,WIDTH,HEIGHT,GL_RGBA,GL_UNSIGNED_BYTE,data);
        REQUIRE(gl_ok("PBuffer 默认目标 read"));
        REQUIRE(verify(data,color,0,true));
        printf("PBUFFER_READ PASS color=%u RGBA=%u,%u,%u,%u pixels=65536 pack/padding 精确\n",
               color-2,backgrounds[color][0],backgrounds[color][1],backgrounds[color][2],backgrounds[color][3]);
    }
    ok=true;
cleanup:
    /* 先解除线程绑定和 drawable，再销毁 context、PBuffer、格式。
     * SetPBuffer 即使失败也尝试 ClearDrawable；绝不以 NULL 调用 nonnull SetPBuffer。 */
    if (ctx && CGLGetCurrentContext()==ctx) {
        unbound=cgl_ok(CGLSetCurrentContext(NULL),"PBuffer 解绑 context");
        if (!unbound) ok=false;
    }
    if (ctx && attach_attempted) {
        detached=cgl_ok(CGLClearDrawable(ctx),"PBuffer 解绑 drawable");
        if (!detached) ok=false;
    }
    if (ctx && unbound) {
        if (cgl_ok(CGLDestroyContext(ctx),"PBuffer 销毁 context")) detached=true;
        else ok=false;
    }
    /* 若 drawable 和 context 都未成功释放，保留 PBuffer 引用并明确失败。 */
    if (pb && detached && !cgl_ok(CGLDestroyPBuffer(pb),"销毁 PBuffer")) ok=false;
    if (pf && !cgl_ok(CGLDestroyPixelFormat(pf),"PBuffer 销毁格式")) ok=false;
    free(data);
    const char *name=stage==STAGE_CHOOSE ? "CHOOSE" : stage==STAGE_CONTEXT ? "CONTEXT" : "CLEAR_READ";
    printf("PUBLIC_CGL_PBUFFER_%s %s mode=no-draw\n",name,ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
static int public_probe(enum public_stage stage, bool reject_color)
{
    CGLPixelFormatObj pf=NULL; CGLContextObj ctx=NULL;
    GLuint fb[2]={0}, rb[2]={0};
    GLint count=0, accelerated=0, recovery=0, offscreen=0, renderer=0, current_renderer=0;
    unsigned char *data=NULL; bool ok=false;
    const CGLPixelFormatAttribute attrs[]={
        kCGLPFAOffScreen,kCGLPFAAccelerated,kCGLPFANoRecovery,
        kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
        kCGLPFARendererID,(CGLPixelFormatAttribute)0x00027e00,
        kCGLPFAColorSize,(CGLPixelFormatAttribute)32,
        kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,(CGLPixelFormatAttribute)0
    };
    /* Modern SDKs reject legacy OffScreen before driver selection. The
     * choose-only diagnostic omits it but never claims Window support or
     * creates a context; the other acceptance stages retain their contract. */
    REQUIRE(cgl_ok(CGLChoosePixelFormat(stage == STAGE_CHOOSE ? attrs + 1 : attrs,
                                       &pf,&count),"选择格式"));
    REQUIRE(check(pf && count>0,"选择格式非空"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFAOffScreen,&offscreen),"离屏属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFAAccelerated,&accelerated),"加速属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFANoRecovery,&recovery),"禁止恢复属性"));
    REQUIRE(cgl_ok(CGLDescribePixelFormat(pf,0,kCGLPFARendererID,&renderer),"renderer ID"));
    if (stage == STAGE_CHOOSE) {
        REQUIRE(check(accelerated && recovery &&
                      ((unsigned)renderer & 0x00ffff00u)==0x00027e00u,
                      "格式必须属于自有 accelerated/no-recovery renderer"));
        printf("CHOOSE_ONLY renderer=0x%08x offscreen=%d accelerated=%d noRecovery=%d; no context created\n",
               (unsigned)renderer,offscreen,accelerated,recovery);
        ok=true; goto cleanup;
    }
    REQUIRE(check(offscreen && accelerated && recovery && ((unsigned)renderer & 0x00ffff00u)==0x00027e00u,
                  "必须自有 offscreen/accelerated/no-recovery renderer"));
    REQUIRE(cgl_ok(CGLCreateContext(pf,NULL,&ctx),"创建 context"));
    REQUIRE(check(ctx!=NULL,"context 非空"));
    REQUIRE(cgl_ok(CGLSetCurrentContext(ctx),"绑定 context"));
    REQUIRE(cgl_ok(CGLGetParameter(ctx,kCGLCPCurrentRendererID,&current_renderer),"当前 renderer"));
    REQUIRE(check(((unsigned)current_renderer & 0x00ffff00u)==0x00027e00u,"当前自有 renderer"));
    printf("CGL renderer=0x%08x current=0x%08x offscreen=%d accelerated=%d noRecovery=%d\n",
           (unsigned)renderer,(unsigned)current_renderer,offscreen,accelerated,recovery);
    const GLenum names[]={GL_VENDOR,GL_RENDERER,GL_VERSION};
    const char *labels[]={"GL_VENDOR","GL_RENDERER","GL_VERSION"};
    for (unsigned i=0;i<3;++i) {
        const GLubyte *value=glGetString(names[i]);
        REQUIRE(gl_ok(labels[i])); REQUIRE(check(value!=NULL,"GL 字符串非空"));
        printf("%s=%s\n",labels[i],(const char *)value);
    }
    fflush(stdout);
    if (stage == STAGE_CONTEXT) { ok=true; goto cleanup; }
    data=malloc(BYTES); REQUIRE(check(data!=NULL,"分配读缓冲"));
    glViewport(0,0,WIDTH,HEIGHT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST); glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB_EXT); glDisable(GL_MULTISAMPLE);
    glDisable(GL_FOG); glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_POLYGON_OFFSET_LINE);
    glDisable(GL_POLYGON_OFFSET_POINT); glDisable(GL_POLYGON_SMOOTH);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable(GL_SAMPLE_ALPHA_TO_ONE);
    glDisable(GL_SAMPLE_COVERAGE);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    glDepthRange(0.0,1.0);
    for (unsigned unit=0;unit<8;++unit) {
        glActiveTexture(GL_TEXTURE0+unit);
        glDisable(GL_TEXTURE_1D); glDisable(GL_TEXTURE_2D); glDisable(GL_TEXTURE_3D);
        glDisable(GL_TEXTURE_CUBE_MAP); glDisable(GL_TEXTURE_RECTANGLE_ARB);
    }
    glActiveTexture(GL_TEXTURE0);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    REQUIRE(gl_ok("固定函数初始状态"));
    glGenFramebuffersEXT(2,fb); glGenRenderbuffersEXT(2,rb);
    REQUIRE(gl_ok("分配 FBO/RBO 名称"));
    REQUIRE(check(fb[0] && fb[1] && fb[0]!=fb[1] && rb[0] && rb[1] && rb[0]!=rb[1],"两个独立目标"));
    for (unsigned j=0;j<2;++j) {
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,rb[j]);
        REQUIRE(gl_ok("绑定 renderbuffer"));
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_RGBA8,WIDTH,HEIGHT);
        REQUIRE(gl_ok("分配 RGBA8"));
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fb[j]);
        REQUIRE(gl_ok("绑定 framebuffer"));
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_RENDERBUFFER_EXT,rb[j]);
        REQUIRE(gl_ok("附加 renderbuffer"));
        GLenum status=glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        REQUIRE(gl_ok("FBO 状态")); REQUIRE(check(status==GL_FRAMEBUFFER_COMPLETE_EXT,"FBO 完整"));
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT); glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
        REQUIRE(gl_ok("选择颜色附件"));
        /* q/255 canonical 值避免 .25/.5 的半整数舍入歧义。 */
        glClearColor(backgrounds[j][0]/255.f,backgrounds[j][1]/255.f,
                     backgrounds[j][2]/255.f,backgrounds[j][3]/255.f);
        REQUIRE(gl_ok("背景颜色"));
        glClear(GL_COLOR_BUFFER_BIT); REQUIRE(gl_ok("唯一初始 clear"));
        glFinish(); REQUIRE(gl_ok("clear finish"));
    }
    REQUIRE(image(fb[0],0,0,data)); REQUIRE(image(fb[1],1,0,data));
    if (stage == STAGE_CLEAR_READ) { ok=true; goto cleanup; }
    REQUIRE(draw(fb[0],0,false));
    REQUIRE(image(fb[0],0,1,data)); REQUIRE(image(fb[1],1,0,data));
    REQUIRE(draw(fb[1],1,false));
    REQUIRE(image(fb[0],0,1,data)); REQUIRE(image(fb[1],1,2,data));
    if (reject_color) {
        REQUIRE(draw(fb[0],1,true));
        REQUIRE(image(fb[0],0,1,data)); REQUIRE(image(fb[1],1,2,data));
        REQUIRE(draw(fb[1],0,true));
        REQUIRE(image(fb[1],1,2,data)); REQUIRE(image(fb[0],0,1,data));
    }
    /* 不 clear：先前三角形独有区域必须保留。负向模式同时检验拒绝后健康提交。 */
    REQUIRE(draw(fb[0],1,false));
    REQUIRE(image(fb[0],0,3,data)); REQUIRE(image(fb[1],1,2,data));
    REQUIRE(draw(fb[1],0,false));
    REQUIRE(image(fb[1],1,3,data)); REQUIRE(image(fb[0],0,3,data));
    ok=true;
cleanup:
    /* 失败后不再绘制、读图或探活，只释放公开 GL/CGL 对象。 */
    if (ctx && CGLGetCurrentContext()==ctx) {
        if (fb[0] || fb[1]) glDeleteFramebuffersEXT(2,fb);
        if (rb[0] || rb[1]) glDeleteRenderbuffersEXT(2,rb);
        if ((fb[0] || fb[1] || rb[0] || rb[1]) && !gl_ok("资源释放")) ok=false;
        if (!cgl_ok(CGLSetCurrentContext(NULL),"解绑 context")) ok=false;
    }
    if (ctx && !cgl_ok(CGLDestroyContext(ctx),"销毁 context")) ok=false;
    if (pf && !cgl_ok(CGLDestroyPixelFormat(pf),"销毁格式")) ok=false;
    free(data);
    const char *stage_name = stage == STAGE_CHOOSE ? "CHOOSE" :
                             stage == STAGE_CONTEXT ? "CONTEXT" :
                             stage == STAGE_CLEAR_READ ? "CLEAR_READ" : "TRIANGLE";
    printf("PUBLIC_CGL_%s %s mode=%s\n",stage_name,ok ? "PASS" : "FAIL",
           stage != STAGE_TRIANGLE ? "no-draw" :
           reject_color ? "restricted-reject-color" : "restricted-red");
    return ok ? 0 : 1;
}
#endif
int main(int argc, char **argv)
{
    bool cpu=argc==2 && !strcmp(argv[1],"--oracle-only");
#ifndef TRIANGLE_ORACLE_ONLY
    bool gpu=argc==2 && !strcmp(argv[1],"--gpu");
    bool reject=argc==2 && !strcmp(argv[1],"--reject-color");
    bool context=argc==2 && !strcmp(argv[1],"--context-only");
    bool clear=argc==2 && !strcmp(argv[1],"--clear-read");
    bool choose=argc==2 && !strcmp(argv[1],"--choose-only");
    bool pb_choose=argc==2 && !strcmp(argv[1],"--pbuffer-choose-only");
    bool pb_context=argc==2 && !strcmp(argv[1],"--pbuffer-context-only");
    bool pb_clear=argc==2 && !strcmp(argv[1],"--pbuffer-clear-read");
    if (!cpu && !gpu && !reject && !context && !clear && !choose && !pb_choose && !pb_context && !pb_clear) {
        fprintf(stderr,"用法：%s --oracle-only | --choose-only | --context-only | --clear-read | --gpu | --reject-color | --pbuffer-choose-only | --pbuffer-context-only | --pbuffer-clear-read\n",argv[0]); return 2;
    }
    if (!oracle_selftest()) return 1;
    if (pb_choose || pb_context || pb_clear)
        return pbuffer_probe(pb_choose ? STAGE_CHOOSE : pb_context ? STAGE_CONTEXT : STAGE_CLEAR_READ);
    return cpu ? 0 : public_probe(choose ? STAGE_CHOOSE : context ? STAGE_CONTEXT :
                                  clear ? STAGE_CLEAR_READ : STAGE_TRIANGLE,reject);
#else
    if (!cpu) { fprintf(stderr,"CPU 构建只接受 --oracle-only\n"); return 2; }
    return oracle_selftest() ? 0 : 1;
#endif
}
