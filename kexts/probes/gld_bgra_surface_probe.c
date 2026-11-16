/* 实际 macOS 验收：
 * clang -std=c11 -Wall -Wextra -Werror -pthread -Ikexts/gld-surface
 *   kexts/gld-surface/vgpu_surface.c kexts/probes/gld_bgra_surface_probe.c
 *   -framework IOKit -framework CoreFoundation -o /tmp/gld_bgra_surface_probe
 * sudo /tmp/gld_bgra_surface_probe
 * 需要已加载支持 selector 4..8 的 framebuffer；此源文件本身不是 GPU 通过证据。
 */
#include "vgpu_surface.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static const float colors[2][4] = {{.25f,.75f,.125f,.5f},{.875f,.125f,.625f,.25f}};
static const unsigned char pixels[2][4] = {{64,191,32,128},{223,32,159,64}};
static const vgpu_surface_rect full = {0,0,256,256};
static const vgpu_surface_pack tight = {0,0,0,0,4};
static bool check(bool ok, const char *label)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
    return ok;
}
static bool code(IOReturn rc, IOReturn expected, const char *label)
{
    if (rc != expected) printf("实际=0x%08" PRIx32 " 预期=0x%08" PRIx32 "\n",
                               (uint32_t)rc, (uint32_t)expected);
    return check(rc == expected, label);
}
static bool image(vgpu_surface_image *s, uint64_t gen, unsigned color,
                   vgpu_surface_rect r, vgpu_surface_pack p)
{
    size_t stride = p.stride_bytes;
    if (!stride) stride = (((p.row_length ? p.row_length : r.width)*4 + p.alignment-1)
                           / p.alignment)*p.alignment;
    size_t offset = p.skip_rows*stride + p.skip_pixels*4;
    size_t size = offset + (r.height-1)*stride + r.width*4;
    unsigned char *actual = malloc(size+64), *expected = malloc(size+64);
    if (!actual || !expected) { free(actual); free(expected); return check(false,"分配缓冲区"); }
    memset(actual,0xa5,size+64); memset(expected,0xa5,size+64);
    for (uint32_t y=0;y<r.height;++y)
        for (uint32_t x=0;x<r.width;++x)
            memcpy(expected+32+offset+y*stride+x*4,pixels[color],4);
    IOReturn rc = vgpu_surface_image_read(s,gen,r,VGPU_SURFACE_RGBA8_OUTPUT,p,actual+32,size);
    bool ok = code(rc,kIOReturnSuccess,"BGRA 资源 readpack") &&
              check(!memcmp(actual,expected,size+64),"完整 RGBA/真实 alpha/skip/padding/guard");
    if (!ok) printf("generation=%" PRIu64 " 首像素=%u,%u,%u,%u\n",gen,
                    actual[32+offset],actual[33+offset],actual[34+offset],actual[35+offset]);
    free(actual); free(expected);
    return ok;
}
static void reject(vgpu_surface_image *s, uint64_t gen, vgpu_surface_rect r,
                    vgpu_surface_pack p, size_t capacity, IOReturn expected)
{
    unsigned char actual[4096], before[4096];
    memset(actual,0xa5,sizeof(actual)); memcpy(before,actual,sizeof(before));
    code(vgpu_surface_image_read(s,gen,r,VGPU_SURFACE_RGBA8_OUTPUT,p,actual,capacity),
         expected,"拒绝 stale/非法读取");
    check(!memcmp(actual,before,sizeof(actual)),"失败不改变任何目标字节");
}
struct worker { vgpu_surface_image *s; IOReturn result; };
static void *clear_worker(void *arg)
{
    struct worker *w = arg;
    w->result = kIOReturnSuccess;
    for (unsigned n=0;n<8;++n) {
        uint64_t gen;
        IOReturn rc = vgpu_surface_image_clear(w->s,colors[n%2],&gen);
        if (rc) { w->result=rc; break; }
    }
    IOReturn rc = vgpu_surface_image_release(w->s);
    if (rc) w->result=rc;
    return NULL;
}
static void concurrent(vgpu_surface_image *reader, vgpu_surface_image *writer,
                        uint64_t gen, unsigned color, bool same)
{
    if (!code(vgpu_surface_image_retain(writer),kIOReturnSuccess,"线程持 image 引用")) return;
    struct worker w = {writer,kIOReturnSuccess};
    pthread_t thread;
    if (!check(pthread_create(&thread,NULL,clear_worker,&w)==0,"启动并发 clear")) {
        code(vgpu_surface_image_release(writer),kIOReturnSuccess,"回收线程引用"); return;
    }
    unsigned char *buffer = malloc(262144);
    unsigned stale=0, snapshots=0;
    if (check(buffer!=NULL,"并发读取缓冲区")) {
        for (unsigned n=0;n<8;++n) {
            memset(buffer,0xa5,262144);
            IOReturn rc = vgpu_surface_image_read(reader,gen,full,VGPU_SURFACE_RGBA8_OUTPUT,
                                                 tight,buffer,262144);
            bool ok=true;
            if (same && rc==kIOReturnNotReady) {
                ++stale;
                for (size_t j=0;j<262144;++j) if (buffer[j]!=0xa5) ok=false;
                check(ok,"同资源 stale 事务保留全部目标");
            } else if (code(rc,kIOReturnSuccess,"读取请求代次完整快照")) {
                ++snapshots;
                for (size_t j=0;j<65536;++j) if (memcmp(buffer+j*4,pixels[color],4)) ok=false;
                check(ok,"并发成功快照每像素一致");
            }
        }
        free(buffer);
    }
    check(pthread_join(thread,NULL)==0,"等待 clear 线程");
    code(w.result,kIOReturnSuccess,"线程 clear/release 完成");
    printf("同资源=%d stale=%u 完整快照=%u\n",same,stale,snapshots);
    if (same) reject(reader,gen,(vgpu_surface_rect){17,29,13,9},tight,4096,kIOReturnNotReady);
    else image(reader,gen,color,full,tight);
}
int main(int argc, char **argv)
{
    bool denied = argc==2 && !strcmp(argv[1],"--expect-denied");
    if (argc!=1 && !denied) return 2;
    vgpu_surface_owner *owner=NULL;
    vgpu_surface_image *a=NULL,*b=NULL,*third=NULL;
    IOReturn rc=vgpu_surface_owner_open(&owner);
    if (denied) {
        check(geteuid()!=0,"非 root 进程");
        code(rc,kIOReturnNotPrivileged,"实际 owner 打开权限拒绝");
        check(owner==NULL,"失败输出为空");
        code(vgpu_surface_owner_release(owner),kIOReturnSuccess,"清理 owner");
        return failures ? 1 : 0;
    }
    if (!code(rc,kIOReturnSuccess,"打开资源 owner")) return 1;
    if (!code(vgpu_surface_image_create(owner,256,256,VGPU_SURFACE_BGRA8,&a),kIOReturnSuccess,"创建资源 A")) goto done;
    if (!code(vgpu_surface_image_create(owner,256,256,VGPU_SURFACE_BGRA8,&b),kIOReturnSuccess,"创建资源 B")) goto done;
    code(vgpu_surface_image_create(owner,256,256,VGPU_SURFACE_BGRA8,&third),kIOReturnNoResources,"第三资源失败");
    check(third==NULL,"第三资源输出为空");
    uint64_t ga=0,gb=0,old=0,again=0;
    code(vgpu_surface_image_info(a,&ga),kIOReturnSuccess,"selector 8 查询 A");
    code(vgpu_surface_image_info(b,&gb),kIOReturnSuccess,"selector 8 查询 B");
    check(ga==1 && gb==1,"两个资源独立初始代次");
    if (!code(vgpu_surface_image_clear(a,colors[0],&ga),kIOReturnSuccess,"A 非平凡 clear alpha .5")) goto done;
    if (!code(vgpu_surface_image_clear(b,colors[1],&gb),kIOReturnSuccess,"B 非平凡 clear alpha .25")) goto done;
    image(a,ga,0,full,tight); image(b,gb,1,full,tight);
    vgpu_surface_rect small={17,29,13,9};
    for (size_t align=1;align<=8;align*=2) {
        image(a,ga,0,small,(vgpu_surface_pack){19,0,3,2,align});
        image(b,gb,1,small,(vgpu_surface_pack){19,0,3,2,align});
    }
    image(a,ga,0,small,(vgpu_surface_pack){SIZE_MAX,93,2,5,8});
    image(b,gb,1,small,(vgpu_surface_pack){1,0,1,1,1});
    image(a,ga,0,(vgpu_surface_rect){255,255,1,1},tight);
    reject(a,ga,(vgpu_surface_rect){250,0,7,1},tight,4096,kIOReturnBadArgument);
    reject(a,ga,small,(vgpu_surface_pack){SIZE_MAX,0,0,0,4},4096,kIOReturnBadArgument);
    reject(a,ga,small,(vgpu_surface_pack){0,SIZE_MAX,2,0,4},4096,kIOReturnBadArgument);
    reject(a,ga,small,tight,13*9*4-1,kIOReturnBadArgument);
    const uint32_t bad[]={0x7fc00000,0x7f800000,0xff800000,0x3f800001,0x80000001};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        float c[4]; memcpy(c,colors[0],sizeof(c)); memcpy(c+2,bad+i,4);
        uint64_t untouched=UINT64_MAX;
        code(vgpu_surface_image_clear(a,c,&untouched),kIOReturnBadArgument,"拒绝非法浮点输入");
        check(untouched==UINT64_MAX,"失败保留 generation 输出");
    }
    union { float rgba[4]; uint64_t generation; } aliased;
    memcpy(aliased.rgba,colors[0],sizeof(aliased.rgba));
    if (code(vgpu_surface_image_clear(a,aliased.rgba,&aliased.generation),kIOReturnSuccess,
             "输入颜色与代次输出重叠时先快照")) {
        ga=aliased.generation;
        image(a,ga,0,small,tight);
    }
    code(vgpu_surface_image_read(a,ga,small,VGPU_SURFACE_RGBA8_OUTPUT,tight,a,4096),
         kIOReturnBadArgument,"拒绝目标覆盖 opaque image 内存");
    uint64_t untouched=UINT64_MAX;
    code(vgpu_surface_image_read(a,ga,small,VGPU_SURFACE_RGBA8_OUTPUT,
                                 (vgpu_surface_pack){0,0,0,0,3},&untouched,sizeof(untouched)),
         kIOReturnBadArgument,"拒绝非法 alignment");
    check(untouched==UINT64_MAX,"非法 alignment 不写目标");
    old=ga;
    code(vgpu_surface_image_clear(a,colors[1],&ga),kIOReturnSuccess,"A 更新自己的代次");
    code(vgpu_surface_image_info(b,&again),kIOReturnSuccess,"查询未提交的 B");
    check(gb==again,"A 提交不修改 B 代次");
    reject(a,old,small,tight,4096,kIOReturnNotReady);
    image(b,gb,1,full,tight);
    concurrent(b,a,gb,1,false);
    concurrent(b,b,gb,1,true);
    code(vgpu_surface_image_close(a),kIOReturnSuccess,"仅关闭 A 资源");
    code(vgpu_surface_image_close(a),kIOReturnSuccess,"A close 幂等");
    vgpu_surface_image_state state=VGPU_SURFACE_IMAGE_LIVE;
    IOReturn release_error=kIOReturnError;
    code(vgpu_surface_image_status(a,&state,&release_error),kIOReturnSuccess,"查询释放状态");
    check(state==VGPU_SURFACE_IMAGE_CLOSED && release_error==kIOReturnSuccess,"已确认 A 释放");
    reject(a,ga,small,tight,4096,kIOReturnNotOpen);
    if (!code(vgpu_surface_image_create(owner,256,256,VGPU_SURFACE_BGRA8,&third),kIOReturnSuccess,"image close 归还单独槽位")) goto done;
    code(vgpu_surface_image_info(third,&again),kIOReturnSuccess,"新 image 初始代次");
    check(again==1,"复用槽位不复用旧代次");
    old=ga;
    reject(third,old,small,tight,4096,kIOReturnNotReady);
    if (code(vgpu_surface_owner_release(owner),kIOReturnSuccess,"share 放下 owner 引用")) owner=NULL;
    if (code(vgpu_surface_image_release(a),kIOReturnSuccess,"回收已关闭 A")) a=NULL;
    if (code(vgpu_surface_image_clear(b,colors[0],&gb),kIOReturnSuccess,"image 强引用保活 owner"))
        image(b,gb,0,full,tight);
    if (code(vgpu_surface_image_release(third),kIOReturnSuccess,"释放另一个 image 不关闭 B owner")) third=NULL;
    if (code(vgpu_surface_image_clear(b,colors[1],&gb),kIOReturnSuccess,"兄弟 image 释放后 B 仍可用"))
        image(b,gb,1,full,tight);
done:
    code(vgpu_surface_image_release(third),kIOReturnSuccess,"清理 third");
    code(vgpu_surface_image_release(a),kIOReturnSuccess,"清理 A");
    code(vgpu_surface_image_release(b),kIOReturnSuccess,"清理 B/最后 owner 引用");
    code(vgpu_surface_owner_release(owner),kIOReturnSuccess,"清理 share owner");
    printf("独立 BGRA macOS probe failures=%d\n",failures);
    return failures ? 1 : 0;
}
