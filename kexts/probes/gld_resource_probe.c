/*
 * macOS 构建（仓库根目录）：
 * clang -std=c11 -O2 -Wall -Wextra -Werror kexts/probes/gld_resource_probe.c \
 *   -framework IOKit -framework CoreFoundation -o /tmp/gld_resource_probe
 * 运行：sudo /tmp/gld_resource_probe
 * 前提：资源后端已 READY、两槽空闲，测试期间无其他客户端改写旧全局目标。
 * 仅 CPU oracle 自检：/tmp/gld_resource_probe --oracle-only
 *
 * 只调用自研 vgpuAccelerator 的同步 selector 0..8；无原始 GPU 命令。
 * 本文件按 framebuffer/vgpuFramebuffer.cpp 的资源 ABI v1 独立声明，
 * 不链接 surface/context 模块，避免被测转换代码充当像素 oracle。
 * 非预期结果立即停止测试，仅继续纯逻辑 RELEASE / IOServiceClose 清理。
 * 本地平台 shim 检查不等于完整 macOS SDK 编译，也不等于实机验收。
 */
#include <IOKit/IOKitLib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 256u
#define HEIGHT 256u
#define ROW_BYTES 1024u
#define OPEN_TYPE UINT32_C(0x56475055)
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
enum { CREATE = 4, RELEASE, SUBMIT, READ_ROW, GET_INFO };
enum { CLEAR = 1, FIXED_TRIANGLE, VERTEX_TRIANGLE };

struct create_request {
    uint32_t version, byteSize, width, height, format, flags, samples, reserved;
};
struct resource_info {
    uint32_t version, byteSize;
    uint64_t handle, generation;
    uint32_t width, height, format, rowBytes, backingBytes, flags;
    uint64_t reserved[2];
};
struct handle_request {
    uint32_t version, byteSize;
    uint64_t handle, reserved;
};
struct release_response {
    uint32_t version, byteSize;
    uint64_t handle;
    uint32_t flags, reserved;
};
struct submit_request {
    uint32_t version, byteSize, opcode, flags;
    uint64_t handle, expectedGeneration;
    uint32_t rgbaBits[4], verticesBits[12];
};
struct submit_response {
    uint32_t version, byteSize;
    uint64_t handle, generation;
    uint32_t width, height;
};
struct read_request {
    uint32_t version, byteSize;
    uint64_t handle, generation;
    uint32_t row, reserved;
};
struct read_response {
    uint32_t version, byteSize;
    uint64_t handle, generation;
    uint32_t row, rowBytes;
    unsigned char pixels[ROW_BYTES];
};
struct vertex_input { uint32_t version, reserved; float vertices[3][4]; };
struct clear_input { uint32_t version, byteSize; float rgba[4]; };

#define SIZE(t, n, a) _Static_assert(sizeof(struct t) == (n) && _Alignof(struct t) == (a), #t " ABI")
#define OFFSET(t, f, n) _Static_assert(offsetof(struct t, f) == (n), #t "." #f " ABI")
#define HEADER(t) OFFSET(t, version, 0); OFFSET(t, byteSize, 4)
SIZE(create_request, 32, 4); HEADER(create_request);
OFFSET(create_request, width, 8); OFFSET(create_request, height, 12);
OFFSET(create_request, format, 16); OFFSET(create_request, flags, 20);
OFFSET(create_request, samples, 24); OFFSET(create_request, reserved, 28);
SIZE(resource_info, 64, 8); HEADER(resource_info);
OFFSET(resource_info, handle, 8); OFFSET(resource_info, generation, 16);
OFFSET(resource_info, width, 24); OFFSET(resource_info, height, 28);
OFFSET(resource_info, format, 32); OFFSET(resource_info, rowBytes, 36);
OFFSET(resource_info, backingBytes, 40); OFFSET(resource_info, flags, 44);
OFFSET(resource_info, reserved, 48);
SIZE(handle_request, 24, 8); HEADER(handle_request);
OFFSET(handle_request, handle, 8); OFFSET(handle_request, reserved, 16);
SIZE(release_response, 24, 8); HEADER(release_response);
OFFSET(release_response, handle, 8); OFFSET(release_response, flags, 16);
OFFSET(release_response, reserved, 20);
SIZE(submit_request, 96, 8); HEADER(submit_request);
OFFSET(submit_request, opcode, 8); OFFSET(submit_request, flags, 12);
OFFSET(submit_request, handle, 16); OFFSET(submit_request, expectedGeneration, 24);
OFFSET(submit_request, rgbaBits, 32); OFFSET(submit_request, verticesBits, 48);
SIZE(submit_response, 32, 8); HEADER(submit_response);
OFFSET(submit_response, handle, 8); OFFSET(submit_response, generation, 16);
OFFSET(submit_response, width, 24); OFFSET(submit_response, height, 28);
SIZE(read_request, 32, 8); HEADER(read_request);
OFFSET(read_request, handle, 8); OFFSET(read_request, generation, 16);
OFFSET(read_request, row, 24); OFFSET(read_request, reserved, 28);
SIZE(read_response, 1056, 8); HEADER(read_response);
OFFSET(read_response, handle, 8); OFFSET(read_response, generation, 16);
OFFSET(read_response, row, 24); OFFSET(read_response, rowBytes, 28);
OFFSET(read_response, pixels, 32);
SIZE(vertex_input, 56, 4); SIZE(clear_input, 24, 4);
_Static_assert(sizeof(float) == 4, "IEEE754 float32 required");

struct triangle {
    struct vertex_input input;
    int64_t screen[3][2];
    uint32_t red;
};
/* 与 gld_surface_probe.c / tools/vgpu-user-draw.c 相同的独立整数边判据。
 * 屏幕坐标乘二，全部边避开像素中心；不可用 GPU 结果反推这些期望值。 */
static const struct triangle triangles[] = {
    {{1, 0, {{-.75f,-.75f,0.f,1.f}, {.75f,-.75f,0.f,1.f}, {0.f,.75f,0.f,1.f}}},
     {{64,448},{448,448},{256,64}}, 18432},
    {{1, 0, {{-.6875f,-.5625f,0.f,1.f}, {.3125f,-.5625f,.5f,1.f}, {-.6875f,.21875f,1.f,1.f}}},
     {{80,400},{336,400},{80,200}}, 6400},
    {{1, 0, {{-.75f,.75f,1.f,1.f}, {-.125f,-.5f,.5f,1.f}, {.5f,.75f,0.f,1.f}}},
     {{64,64},{224,384},{384,64}}, 12800}
};
struct color { float rgba[4]; unsigned char bgra[4]; };
static const struct color zero = {{0,0,0,0}, {0,0,0,0}};
static const struct color red0 = {{1,0,0,0}, {0,0,255,0}};
static const struct color green1 = {{0,1,0,1}, {0,255,0,255}};
static const struct color green0 = {{0,1,0,0}, {0,255,0,0}};
static const struct color green_half = {{0,1,0,.5f}, {0,255,0,128}};
static const struct color blue_quarter = {{0,0,1,.25f}, {255,0,0,64}};
static const unsigned char opaque_red[4] = {0,0,255,255};
static const struct create_request create_input = {1,32,256,256,141,0,0,0};
struct resource {
    io_connect_t connection;
    uint64_t handle, generation;
    const struct color *background;
    const struct triangle *triangle;
};
static unsigned rejected;

static bool check(bool ok, const char *label)
{
    if (!ok) fprintf(stderr, "FAIL %s\n", label);
    return ok;
}
static bool code(IOReturn actual, IOReturn expected, const char *label)
{
    if (actual == expected) return true;
    fprintf(stderr, "FAIL %s: return=0x%08" PRIx32 " expected=0x%08" PRIx32 "\n",
            label, (uint32_t)actual, (uint32_t)expected);
    return false;
}
static IOReturn call(io_connect_t c, uint32_t selector, const void *in, size_t in_size,
                     void *out, size_t *out_size)
{
    return IOConnectCallMethod(c, selector, NULL, 0, in, in_size, NULL, NULL, out, out_size);
}
static bool success(io_connect_t c, uint32_t selector, const void *in, size_t in_size,
                    void *out, size_t out_size, const char *label)
{
    size_t size = out_size;
    memset(out, 0xa5, out_size);
    return code(call(c, selector, in, in_size, out, &size), kIOReturnSuccess, label) &&
           check(size == out_size, "返回结构长度");
}
static bool reject_call(io_connect_t c, uint32_t selector, const void *in, size_t in_size,
                        size_t out_size, IOReturn expected, const char *label)
{
    /* 正常用户栈；失败输出不作任何有效性承诺。 */
    unsigned char output[sizeof(struct read_response) + 8];
    if (!check(out_size <= sizeof(output), "拒绝测试输出容量")) return false;
    size_t size = out_size;
    if (!code(call(c, selector, in, in_size, output, &size), expected, label)) return false;
    ++rejected;
    return true;
}
static bool info_valid(const struct resource_info *i)
{
    return check(i->version == 1 && i->byteSize == sizeof(*i) && i->handle && i->generation &&
                 i->width == WIDTH && i->height == HEIGHT && i->format == 141 &&
                 i->rowBytes == ROW_BYTES && i->backingBytes == WIDTH * HEIGHT * 4 &&
                 i->flags == 7 && !i->reserved[0] && !i->reserved[1], "ResourceInfo 所有字段");
}
static bool info_matches(const struct resource *r)
{
    struct handle_request q = {1,sizeof(q),r->handle,0};
    struct resource_info i;
    return success(r->connection,GET_INFO,&q,sizeof(q),&i,sizeof(i),"GET_INFO") &&
           info_valid(&i) && check(i.handle == r->handle && i.generation == r->generation,
                                 "handle/generation 保持且最终快照复验成功");
}
static bool create_resource(io_connect_t c, struct resource *r)
{
    struct resource_info i;
    if (!success(c,CREATE,&create_input,sizeof(create_input),&i,sizeof(i),"CREATE")) return false;
    *r = (struct resource){c,i.handle,i.generation,&zero,NULL};
    return info_valid(&i) && check(i.generation == 1, "CREATE generation=1");
}
static struct submit_request submit_input(const struct resource *r, const struct color *color,
                                          const struct triangle *triangle)
{
    struct submit_request q = {0};
    q.version = 1; q.byteSize = sizeof(q); q.handle = r->handle;
    q.expectedGeneration = r->generation;
    q.opcode = !triangle ? CLEAR : triangle == &triangles[0] ? FIXED_TRIANGLE : VERTEX_TRIANGLE;
    memcpy(q.rgbaBits,color->rgba,sizeof(q.rgbaBits));
    if (q.opcode == VERTEX_TRIANGLE) memcpy(q.verticesBits,triangle->input.vertices,sizeof(q.verticesBits));
    return q;
}
static bool submit(struct resource *r, const struct color *color, const struct triangle *triangle)
{
    struct submit_request q = submit_input(r,color,triangle);
    struct submit_response o;
    if (!success(r->connection,SUBMIT,&q,sizeof(q),&o,sizeof(o),"SUBMIT")) return false;
    if (!check(o.version == 1 && o.byteSize == sizeof(o) && o.handle == r->handle &&
               r->generation != UINT64_MAX && o.generation == r->generation + 1 &&
               o.width == WIDTH && o.height == HEIGHT, "SUBMIT 精确 generation+1 和响应 ABI")) return false;
    r->generation = o.generation; r->background = color; r->triangle = triangle;
    return true;
}
static int64_t cross(const int64_t a[2], const int64_t b[2], int64_t x, int64_t y)
{
    return (b[0]-a[0])*(y-a[1]) - (b[1]-a[1])*(x-a[0]);
}
static bool inside(const struct triangle *t, uint32_t x, uint32_t y, bool *edge)
{
    if (!t) return false;
    int64_t px = 2*(int64_t)x+1, py = 2*(int64_t)y+1;
    int64_t a = cross(t->screen[0],t->screen[1],px,py);
    int64_t b = cross(t->screen[1],t->screen[2],px,py);
    int64_t c = cross(t->screen[2],t->screen[0],px,py);
    if (!a || !b || !c) *edge = true;
    return (a<0 && b<0 && c<0) || (a>0 && b>0 && c>0);
}
static bool oracle_selftest(void)
{
    for (size_t t=0;t<ARRAY_COUNT(triangles);++t) {
        uint32_t count=0; bool edge=false;
        for (uint32_t y=0;y<HEIGHT;++y)
            for (uint32_t x=0;x<WIDTH;++x) count += inside(&triangles[t],x,y,&edge);
        if (!check(!edge && count == triangles[t].red, "独立三角形 oracle 自检")) return false;
        printf("PASS CPU oracle %zu: red=%u background=%u\n",t,count,WIDTH*HEIGHT-count);
    }
    return true;
}
static bool verify_row(const unsigned char *pixels, uint32_t y, const struct color *background,
                       const struct triangle *t, bool legacy, uint32_t *red)
{
    for (uint32_t x=0;x<WIDTH;++x) {
        bool edge=false, hit=inside(t,x,y,&edge);
        if (!check(!edge,"CPU oracle 像素中心不落边")) return false;
        const unsigned char *expected=hit ? opaque_red : background->bgra;
        const unsigned char *p=pixels+x*4;
        *red += hit;
        /* 旧 BGRX 的 X 不承诺 alpha；新 BGRA 必须逐字节精确一致。 */
        if (memcmp(p,expected,legacy ? 3 : 4)) {
            fprintf(stderr,"FAIL pixel (%u,%u) actual BGRA=%02x %02x %02x %02x "
                    "expected=%02x %02x %02x %02x legacy=%d\n",x,y,
                    p[0],p[1],p[2],p[3],expected[0],expected[1],expected[2],expected[3],legacy);
            return false;
        }
    }
    return true;
}
/* 逐行读 A 时在固定行之间同步更新 B，确定性覆盖另一资源提交不使 A 失效。 */
static bool image(const struct resource *r, struct resource *interleave, const char *label)
{
    struct read_response *o=malloc(sizeof(*o));
    if (!check(o != NULL,"分配 READ_ROW 用户堆响应")) return false;
    bool ok=false; uint32_t red=0;
    for (uint32_t y=0;y<HEIGHT;++y) {
        if (interleave && y && y%64 == 0 &&
            !submit(interleave,y%128 ? &green_half : &blue_quarter,NULL)) goto done;
        struct read_request q={1,sizeof(q),r->handle,r->generation,y,0};
        if (!success(r->connection,READ_ROW,&q,sizeof(q),o,sizeof(*o),"READ_ROW") ||
            !check(o->version == 1 && o->byteSize == sizeof(*o) && o->handle == r->handle &&
                   o->generation == r->generation && o->row == y && o->rowBytes == ROW_BYTES,
                   "READ_ROW 响应头、身份和行号") ||
            !verify_row(o->pixels,y,r->background,r->triangle,false,&red)) goto done;
    }
    if (!check(red == (r->triangle ? r->triangle->red : 0),"整图 oracle 三角形像素数") ||
        !info_matches(r)) goto done;
    printf("PASS %s: handle=%" PRIu64 " generation=%" PRIu64 " pixels=65536 red=%u\n",
           label,r->handle,r->generation,red);
    ok=true;
done:
    free(o);
    return ok;
}
static bool legacy_submit(io_connect_t c, uint32_t selector, const struct triangle *t,
                          const struct color *color, uint64_t *generation)
{
    struct clear_input clear={1,sizeof(clear),{0,0,0,0}};
    const void *in=NULL; size_t bytes=0;
    if (selector == 2) { in=&t->input; bytes=sizeof(t->input); }
    if (selector == 3) { memcpy(clear.rgba,color->rgba,sizeof(clear.rgba)); in=&clear; bytes=sizeof(clear); }
    uint64_t o[3]={0}; uint32_t count=3;
    if (!code(IOConnectCallMethod(c,selector,NULL,0,in,bytes,o,&count,NULL,NULL),
              kIOReturnSuccess,"旧 selector 提交")) return false;
    if (!check(count == 3 && o[0] && (!*generation || o[0] == *generation+1) &&
               o[1] == WIDTH && o[2] == HEIGHT,"旧 selector generation 和尺寸")) return false;
    *generation=o[0];
    return true;
}
static bool legacy_image(io_connect_t c, uint64_t generation, const struct triangle *t,
                         const struct color *background)
{
    unsigned char pixels[ROW_BYTES]; uint32_t red=0;
    for (uint32_t y=0;y<HEIGHT;++y) {
        uint64_t q[2]={generation,y}; size_t size=sizeof(pixels);
        if (!code(IOConnectCallMethod(c,1,q,2,NULL,0,NULL,NULL,pixels,&size),
                  kIOReturnSuccess,"旧 selector1 读行") || !check(size == ROW_BYTES,"旧行尺寸") ||
            !verify_row(pixels,y,background,t,true,&red)) return false;
    }
    /* 复读最后一行也检验这一旧 generation 仍有效。 */
    uint64_t q[2]={generation,HEIGHT-1}; size_t size=sizeof(pixels);
    if (!code(IOConnectCallMethod(c,1,q,2,NULL,0,NULL,NULL,pixels,&size),kIOReturnSuccess,"旧快照最终复验") ||
        !check(size == ROW_BYTES && red == (t ? t->red : 0),"旧整图像素数")) return false;
    printf("PASS legacy RGB: generation=%" PRIu64 " pixels=65536 red=%u (X不作为alpha)\n",generation,red);
    return true;
}
static bool denied_handle(io_connect_t c, const struct resource *r, IOReturn expected)
{
    struct handle_request h={1,sizeof(h),r->handle,0};
    struct read_request q={1,sizeof(q),r->handle,r->generation,0,0};
    struct submit_request s=submit_input(r,&red0,NULL);
    return reject_call(c,GET_INFO,&h,sizeof(h),sizeof(struct resource_info),expected,"拒绝 handle GET_INFO") &&
           reject_call(c,READ_ROW,&q,sizeof(q),sizeof(struct read_response),expected,"拒绝 handle READ_ROW") &&
           reject_call(c,SUBMIT,&s,sizeof(s),sizeof(struct submit_response),expected,"拒绝 handle SUBMIT") &&
           reject_call(c,RELEASE,&h,sizeof(h),sizeof(struct release_response),expected,"拒绝 handle RELEASE");
}
static bool stale(const struct resource *r, uint64_t generation)
{
    struct read_request q={1,sizeof(q),r->handle,generation,0,0};
    struct submit_request s=submit_input(r,&red0,NULL); s.expectedGeneration=generation;
    return reject_call(r->connection,READ_ROW,&q,sizeof(q),sizeof(struct read_response),kIOReturnNotReady,"过期 generation 读拒绝") &&
           reject_call(r->connection,SUBMIT,&s,sizeof(s),sizeof(struct submit_response),kIOReturnNotReady,"过期 generation 提交拒绝");
}
static bool release_resource(struct resource *r)
{
    if (!r->handle) return true;
    struct handle_request q={1,sizeof(q),r->handle,0};
    struct release_response o;
    if (!success(r->connection,RELEASE,&q,sizeof(q),&o,sizeof(o),"RELEASE 逻辑撤销")) return false;
    uint64_t handle=r->handle; r->handle=0;
    return check(o.version == 1 && o.byteSize == sizeof(o) && o.handle == handle &&
                 o.flags == 1 && !o.reserved,"RELEASE 响应；只表示逻辑回收");
}

/* 每次只修改一个字段；全为现有入口已验证会在 GPU 提交前拒绝的输入。 */
struct mutation { size_t offset; uint32_t value; IOReturn expected; const char *label; };
#define MUT(t,f,v,e) {offsetof(struct t,f),(v),(e),#t "." #f}
static bool mutations(io_connect_t c, uint32_t selector, const void *input, size_t size,
                      size_t output_size, const struct mutation *tests, size_t count)
{
    unsigned char bytes[sizeof(struct submit_request)];
    if (!check(size <= sizeof(bytes),"变异请求容量")) return false;
    for (size_t i=0;i<count;++i) {
        memcpy(bytes,input,size);
        memcpy(bytes+tests[i].offset,&tests[i].value,4);
        if (!reject_call(c,selector,bytes,size,output_size,tests[i].expected,tests[i].label)) return false;
    }
    return true;
}
static bool malformed(const struct resource *r)
{
    const IOReturn bad=kIOReturnBadArgument, unsupported=kIOReturnUnsupported;
    const struct mutation create_tests[]={
        MUT(create_request,version,0,bad), MUT(create_request,version,2,bad),
        MUT(create_request,byteSize,31,bad), MUT(create_request,byteSize,33,bad),
        MUT(create_request,width,255,unsupported), MUT(create_request,height,255,unsupported),
        MUT(create_request,format,142,unsupported), MUT(create_request,samples,1,unsupported),
        MUT(create_request,flags,1,bad), MUT(create_request,reserved,1,bad)
    };
    if (!mutations(r->connection,CREATE,&create_input,sizeof(create_input),sizeof(struct resource_info),
                   create_tests,ARRAY_COUNT(create_tests))) return false;
    struct submit_request s=submit_input(r,&red0,NULL);
    const struct mutation submit_tests[]={
        MUT(submit_request,version,2,bad), MUT(submit_request,byteSize,95,bad),
        MUT(submit_request,flags,1,bad), MUT(submit_request,opcode,0,unsupported),
        MUT(submit_request,opcode,4,unsupported), MUT(submit_request,verticesBits,1,bad)
    };
    if (!mutations(r->connection,SUBMIT,&s,sizeof(s),sizeof(struct submit_response),
                   submit_tests,ARRAY_COUNT(submit_tests))) return false;
    const uint32_t invalid[]={0x7fc00000,0x7f800000,0xff800000,0x3f800001,0x80000001,0xbf800000};
    for (size_t i=0;i<ARRAY_COUNT(invalid);++i) for (size_t channel=0;channel<4;++channel) {
        struct submit_request q=s; q.rgbaBits[channel]=invalid[i];
        if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),bad,"NaN/Inf/越界 RGBA")) return false;
    }
    struct submit_request q=s; q.opcode=FIXED_TRIANGLE; q.verticesBits[11]=1;
    if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),bad,"固定三角形未使用字段非零")) return false;
    const uint32_t bad_vertex[]={0x7fc00000,0x3f800001,0xbf000000,0};
    for (unsigned v=0;v<3;++v) for (unsigned field=0;field<4;++field) {
        q=submit_input(r,&green0,&triangles[1]); q.verticesBits[v*4+field]=bad_vertex[field];
        if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),bad,"非法 vertex x/y/z/w")) return false;
    }
    q=s; q.handle=0;
    if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),bad,"SUBMIT handle0")) return false;
    q=s; q.expectedGeneration=0;
    if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),bad,"SUBMIT generation0")) return false;
    struct read_request read={1,sizeof(read),r->handle,r->generation,0,0};
    const struct mutation read_tests[]={
        MUT(read_request,version,2,bad), MUT(read_request,byteSize,31,bad),
        MUT(read_request,row,256,bad), MUT(read_request,row,UINT32_MAX,bad), MUT(read_request,reserved,1,bad)
    };
    if (!mutations(r->connection,READ_ROW,&read,sizeof(read),sizeof(struct read_response),read_tests,ARRAY_COUNT(read_tests))) return false;
    struct read_request rr=read; rr.handle=0;
    if (!reject_call(r->connection,READ_ROW,&rr,sizeof(rr),sizeof(struct read_response),bad,"READ handle0")) return false;
    rr=read; rr.generation=0;
    if (!reject_call(r->connection,READ_ROW,&rr,sizeof(rr),sizeof(struct read_response),bad,"READ generation0")) return false;
    struct handle_request h={1,sizeof(h),r->handle,0};
    const struct mutation handle_tests[]={
        MUT(handle_request,version,2,bad), MUT(handle_request,byteSize,23,bad),
        MUT(handle_request,reserved,1,bad)
    };
    for (unsigned i=0;i<2;++i) {
        uint32_t sel=i ? GET_INFO : RELEASE;
        size_t output=i ? sizeof(struct resource_info) : sizeof(struct release_response);
        if (!mutations(r->connection,sel,&h,sizeof(h),output,handle_tests,ARRAY_COUNT(handle_tests))) return false;
        struct handle_request empty=h; empty.handle=0;
        if (!reject_call(r->connection,sel,&empty,sizeof(empty),output,bad,"handle0")) return false;
    }
    const void *inputs[]={&create_input,&h,&s,&read,&h};
    const size_t input_sizes[]={sizeof(create_input),sizeof(h),sizeof(s),sizeof(read),sizeof(h)};
    const size_t output_sizes[]={sizeof(struct resource_info),sizeof(struct release_response),
        sizeof(struct submit_response),sizeof(struct read_response),sizeof(struct resource_info)};
    for (unsigned i=0;i<5;++i) {
        unsigned char extended[sizeof(struct submit_request)+8]={0};
        memcpy(extended,inputs[i],input_sizes[i]);
        if (!reject_call(r->connection,CREATE+i,extended,input_sizes[i]-1,output_sizes[i],bad,"dispatch 短请求") ||
            !reject_call(r->connection,CREATE+i,extended,input_sizes[i]+1,output_sizes[i],bad,"dispatch 长请求") ||
            !reject_call(r->connection,CREATE+i,inputs[i],input_sizes[i],output_sizes[i]-1,bad,"dispatch 短响应") ||
            !reject_call(r->connection,CREATE+i,inputs[i],input_sizes[i],output_sizes[i]+1,bad,"dispatch 长响应")) return false;
        unsigned char out[sizeof(struct read_response)]; size_t out_size=output_sizes[i];
        uint64_t scalar=0;
        if (!code(IOConnectCallMethod(r->connection,CREATE+i,&scalar,1,inputs[i],input_sizes[i],
                  NULL,NULL,out,&out_size),bad,"dispatch 非零 scalar 输入")) return false;
        ++rejected;
    }
    printf("PASS 输入拒绝，累计=%u；未假设失败输出内容\n",rejected);
    return true;
}
static bool open_connection(io_connect_t *c)
{
    CFMutableDictionaryRef matching=IOServiceMatching("vgpuAccelerator");
    if (!check(matching != NULL,"IOServiceMatching")) return false;
    io_service_t service=IOServiceGetMatchingService(MACH_PORT_NULL,matching);
    if (!check(service != IO_OBJECT_NULL,"找不到自研 vgpuAccelerator；不回退到系统服务")) return false;
    IOReturn result=IOServiceOpen(service,mach_task_self(),OPEN_TYPE,c);
    IOReturn released=IOObjectRelease(service);
    return code(result,kIOReturnSuccess,"IOServiceOpen（需要对应驱动及权限）") &&
           code(released,kIOReturnSuccess,"释放 service 引用");
}
static bool close_connection(io_connect_t *c)
{
    if (*c == IO_OBJECT_NULL) return true;
    IOReturn result=IOServiceClose(*c);
    *c=IO_OBJECT_NULL;
    return code(result,kIOReturnSuccess,"IOServiceClose");
}
#define REQUIRE(x) do { if (!(x)) goto done; } while (0)
int main(int argc, char **argv)
{
    bool cpu_only=argc == 2 && !strcmp(argv[1],"--oracle-only");
    if (argc != 1 && !cpu_only) {
        fprintf(stderr,"用法：%s [--oracle-only]\n",argv[0]); return 2;
    }
    if (!oracle_selftest()) return 1;
    if (cpu_only) {
        puts("仅 CPU oracle/ABI 布局自检；没有打开 IOKit 连接，未验证 GPU。"); return 0;
    }
    io_connect_t owner=IO_OBJECT_NULL, other=IO_OBJECT_NULL;
    struct resource a={0}, b={0}; bool ok=false;
    uint64_t legacy_generation=0;
    REQUIRE(open_connection(&owner));
    REQUIRE(open_connection(&other));
    REQUIRE(create_resource(owner,&a));
    REQUIRE(image(&a,NULL,"CREATE A 全透明黑"));
    REQUIRE(create_resource(owner,&b));
    REQUIRE(check(a.handle != b.handle && a.generation == b.generation,"同连接两资源，不同 handle、相同 generation1"));
    REQUIRE(image(&b,NULL,"CREATE B 全透明黑"));
    REQUIRE(reject_call(owner,CREATE,&create_input,sizeof(create_input),sizeof(struct resource_info),kIOReturnNoResources,"第三 CREATE 拒绝"));
    REQUIRE(reject_call(other,CREATE,&create_input,sizeof(create_input),sizeof(struct resource_info),kIOReturnNoResources,"跨连接池容量共享"));
    REQUIRE(denied_handle(other,&a,kIOReturnNotPermitted));
    REQUIRE(denied_handle(other,&b,kIOReturnNotPermitted));

    uint64_t old_a=a.generation;
    REQUIRE(submit(&a,&red0,NULL));
    REQUIRE(stale(&a,old_a));
    REQUIRE(image(&a,NULL,"A 红透明，精确 alpha0"));
    REQUIRE(image(&b,NULL,"A 更新后 B generation1 及全图保持"));
    REQUIRE(submit(&b,&green1,NULL));
    REQUIRE(check(a.generation == b.generation,"两资源独立到达同一 generation"));
    REQUIRE(image(&a,NULL,"同 generation 的 A 红透明"));
    REQUIRE(image(&b,NULL,"同 generation 的 B 绿不透明"));
    REQUIRE(submit(&b,&green_half,NULL));
    REQUIRE(image(&a,NULL,"B 半透明提交不影响 A"));
    REQUIRE(image(&b,NULL,"B 半透明精确 BGRA 00 ff 00 80"));
    REQUIRE(stale(&a,b.generation));
    REQUIRE(stale(&b,a.generation));
    REQUIRE(submit(&b,&blue_quarter,NULL));
    REQUIRE(image(&a,NULL,"B 四分之一提交不影响 A"));
    REQUIRE(image(&b,NULL,"B 四分之一精确 BGRA ff 00 00 40"));
    REQUIRE(submit(&b,&blue_quarter,NULL));
    REQUIRE(image(&b,NULL,"重复相同颜色仍 generation+1"));
    REQUIRE(image(&a,&b,"逐行读 A 期间交错 B 提交，A 全图与 generation 保持"));
    REQUIRE(image(&b,NULL,"行间提交后的 B 最终全图"));
    puts("PASS 141 alpha 及 A/B 全图隔离阶段");

    old_a=a.generation;
    REQUIRE(submit(&a,&green0,&triangles[0]));
    REQUIRE(stale(&a,old_a));
    REQUIRE(image(&a,NULL,"新 A 固定三角形：18432红opaque/47104绿transparent"));
    REQUIRE(image(&b,NULL,"A 绘图后 B 保持"));
    REQUIRE(legacy_submit(owner,0,NULL,NULL,&legacy_generation));
    REQUIRE(legacy_image(owner,legacy_generation,&triangles[0],&green1));
    REQUIRE(image(&a,NULL,"旧固定 draw 后 A 保持"));
    REQUIRE(image(&b,NULL,"旧固定 draw 后 B 保持"));
    REQUIRE(submit(&b,&blue_quarter,&triangles[1]));
    REQUIRE(image(&b,NULL,"新 B vertex A：6400红opaque/四分之一蓝背景"));
    REQUIRE(image(&a,NULL,"B vertex 后 A 保持"));
    REQUIRE(legacy_image(owner,legacy_generation,&triangles[0],&green1));
    REQUIRE(legacy_submit(owner,2,&triangles[2],NULL,&legacy_generation));
    REQUIRE(legacy_image(owner,legacy_generation,&triangles[2],&green1));
    REQUIRE(image(&a,NULL,"旧 vertex B 后 A 保持"));
    REQUIRE(image(&b,NULL,"旧 vertex B 后 B 保持"));
    REQUIRE(submit(&a,&green_half,&triangles[2]));
    REQUIRE(image(&a,NULL,"新 A vertex B：12800红opaque/半透明绿背景"));
    REQUIRE(image(&b,NULL,"新 A vertex 后 B 保持"));
    REQUIRE(legacy_image(owner,legacy_generation,&triangles[2],&green1));
    REQUIRE(legacy_submit(owner,3,NULL,&blue_quarter,&legacy_generation));
    REQUIRE(legacy_image(owner,legacy_generation,NULL,&blue_quarter));
    REQUIRE(image(&a,NULL,"旧 clear 后 A 保持"));
    REQUIRE(image(&b,NULL,"旧 clear 后 B 保持"));
    REQUIRE(submit(&a,&green0,&triangles[0]));
    REQUIRE(image(&a,NULL,"旧 clear 后重新路由新 A 固定 draw"));
    REQUIRE(image(&b,NULL,"重新路由后 B 保持"));
    REQUIRE(legacy_image(owner,legacy_generation,NULL,&blue_quarter));
    puts("PASS 新旧 selector 0/1/2/3/6/7 三目标交错路由阶段");

    REQUIRE(malformed(&b));
    REQUIRE(image(&a,NULL,"非法输入后 A 整图保持"));
    REQUIRE(image(&b,NULL,"非法输入后 B 整图保持"));
    REQUIRE(submit(&b,&blue_quarter,NULL));
    REQUIRE(image(&b,NULL,"拒绝后合法提交仍成功"));
    REQUIRE(image(&a,NULL,"拒绝后合法 B 提交不影响 A"));
    puts("PASS generation、owner、输入拒绝及后续健康提交阶段");

    struct resource released_a=a;
    REQUIRE(release_resource(&a));
    REQUIRE(denied_handle(owner,&released_a,kIOReturnNotFound));
    REQUIRE(image(&b,NULL,"释放 A 后 B 保持"));
    REQUIRE(create_resource(owner,&a));
    REQUIRE(check(a.handle != released_a.handle && a.handle != b.handle,"RECREATE 新 handle"));
    REQUIRE(image(&a,NULL,"RECREATE generation1 全透明黑，无旧 A 数据"));
    REQUIRE(denied_handle(owner,&released_a,kIOReturnNotFound));
    REQUIRE(image(&b,NULL,"RECREATE A 后 B 保持"));
    REQUIRE(submit(&a,&red0,NULL));
    REQUIRE(image(&a,NULL,"关闭前 A 非零数据"));
    struct resource closed_a=a, closed_b=b;
    REQUIRE(close_connection(&owner));
    a.handle=0; b.handle=0;
    REQUIRE(denied_handle(other,&closed_a,kIOReturnNotFound));
    REQUIRE(denied_handle(other,&closed_b,kIOReturnNotFound));
    REQUIRE(create_resource(other,&a));
    REQUIRE(create_resource(other,&b));
    REQUIRE(check(a.handle != b.handle && a.handle != released_a.handle && b.handle != released_a.handle &&
                  a.handle != closed_a.handle && a.handle != closed_b.handle &&
                  b.handle != closed_a.handle && b.handle != closed_b.handle,"close 后全部 handle 永不复用"));
    REQUIRE(image(&a,NULL,"另一连接重租第一槽，全图透明黑"));
    REQUIRE(image(&b,NULL,"另一连接重租第二槽，全图透明黑"));
    REQUIRE(denied_handle(other,&closed_a,kIOReturnNotFound));
    REQUIRE(denied_handle(other,&closed_b,kIOReturnNotFound));
    REQUIRE(open_connection(&owner));
    REQUIRE(denied_handle(owner,&a,kIOReturnNotPermitted));
    REQUIRE(denied_handle(owner,&b,kIOReturnNotPermitted));
    REQUIRE(submit(&b,&green_half,NULL));
    REQUIRE(image(&b,NULL,"重开连接拒绝越权后健康提交"));
    REQUIRE(image(&a,NULL,"重租两槽仍隔离"));
    puts("PASS RELEASE、重复释放、close 自动撤销两个资源、跨连接重租清零阶段");
    ok=true;
done:
    /* 无论失败发生于何处，禁止继续 CREATE/SUBMIT/探活；RELEASE 为纯 CPU 撤销。
     * close 自动撤销成功 CREATE 但响应校验失败、未能记录的 handle。 */
    if (a.handle && a.connection != IO_OBJECT_NULL && (a.connection == owner || a.connection == other))
        if (!release_resource(&a)) ok=false;
    if (b.handle && b.connection != IO_OBJECT_NULL && (b.connection == owner || b.connection == other))
        if (!release_resource(&b)) ok=false;
    if (!close_connection(&other)) ok=false;
    if (!close_connection(&owner)) ok=false;
    printf("%s resource probe；预期拒绝=%u\n",ok ? "PASS" : "FAIL",rejected);
    puts("范围：同步私有资源 ABI。未覆盖 close/submit 真并发、进程崩溃、"
         "异步/descriptor 内核参数、计数耗尽、GPU 故障注入或 surface/context pack/flush。");
    return ok ? 0 : 1;
}
