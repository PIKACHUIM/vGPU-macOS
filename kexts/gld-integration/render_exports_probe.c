#include "render_exports.h"
#include "../gld-device/gld_device.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 只在本 probe 中替代 device/surface 边界，其余链接仓库实际实现。
 * CPU 像素模型和故障注入不是 GPU/系统 OpenGL/Metal 验收。 */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(2); } } while (0)
#define OK(x) CHECK((x) == kIOReturnSuccess)
static unsigned device;
static unsigned owners, images, clears, reads, triangles;
static int fail_create, fail_clear, fail_read, fail_release, fail_owner, fail_triangle;
static float submitted_vertices[3][4];
struct vgpu_surface_owner { unsigned references; };
struct vgpu_surface_image { vgpu_surface_owner *owner; uint64_t generation; unsigned char rgba[4]; };
int32_t vgpuGLDDeviceGetStatus(void *token, struct gld_device_status *out) {
    if (token != &device) return -1;
    *out = (struct gld_device_status){.registry_id=73,.connected=1,.liveness_status=kIOReturnNotReady,
        .renderer_id=GLD_DEVICE_DRIVER_ID,.display_mask=1};
    return 0;
}
IOReturn vgpu_surface_owner_open(vgpu_surface_owner **out) {
    *out = calloc(1,sizeof(**out));
    if (!*out) return kIOReturnNoMemory;
    (*out)->references = 1; ++owners; return kIOReturnSuccess;
}
IOReturn vgpu_surface_owner_open_registry(uint64_t registry, vgpu_surface_owner **out) {
    if (registry != 73) return kIOReturnNotFound;
    return vgpu_surface_owner_open(out);
}
IOReturn vgpu_surface_owner_release(vgpu_surface_owner *owner) {
    if (owner->references == 1 && fail_owner) return kIOReturnNotReady;
    if (!--owner->references) { --owners; free(owner); }
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_create(vgpu_surface_owner *owner, uint32_t w, uint32_t h,
                                  uint32_t format, vgpu_surface_image **out) {
    *out = NULL;
    if (fail_create) return kIOReturnNotReady;
    if (w != 256 || h != 256 || format != VGPU_SURFACE_BGRA8) return kIOReturnUnsupported;
    *out = calloc(1,sizeof(**out));
    if (!*out) return kIOReturnNoMemory;
    (*out)->owner = owner; ++owner->references; ++images;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_release(vgpu_surface_image *image) {
    if (fail_release) return kIOReturnNotReady;
    IOReturn r = vgpu_surface_owner_release(image->owner);
    if (r) return r;
    --images; free(image); return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_clear(vgpu_surface_image *image, const float rgba[4], uint64_t *gen) {
    ++clears;
    if (fail_clear) return kIOReturnNotReady;
    for (unsigned i=0;i<4;++i) image->rgba[i]=(unsigned char)(rgba[i]*255.0f+0.5f);
    *gen = ++image->generation; return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_draw_triangle(vgpu_surface_image *image, const float vertices[3][4], uint64_t *gen) {
    ++triangles;
    if (fail_triangle) return kIOReturnNotReady;
    memcpy(submitted_vertices,vertices,sizeof(submitted_vertices));
    /* 只模拟提交与代次；不声称验证光栅化或 GPU 像素。 */
    *gen = ++image->generation;
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_image_read(vgpu_surface_image *image, uint64_t gen, vgpu_surface_rect rect,
                                uint32_t format, vgpu_surface_pack pack, void *out, size_t capacity) {
    ++reads;
    if (fail_read || !gen || gen != image->generation) return kIOReturnNotReady;
    if (format != VGPU_SURFACE_RGBA8_OUTPUT || !rect.width || !rect.height ||
        rect.x >= 256 || rect.y >= 256 || rect.width > 256-rect.x || rect.height > 256-rect.y ||
        (pack.alignment != 1 && pack.alignment != 2 && pack.alignment != 4 && pack.alignment != 8))
        return kIOReturnBadArgument;
    size_t row = pack.row_length ? pack.row_length : rect.width;
    if (row > (SIZE_MAX-7)/4) return kIOReturnBadArgument;
    size_t stride = pack.stride_bytes ? pack.stride_bytes : (row*4+pack.alignment-1)&~(pack.alignment-1);
    if (pack.skip_rows && stride > SIZE_MAX/pack.skip_rows) return kIOReturnBadArgument;
    size_t offset = pack.skip_rows*stride;
    if (pack.skip_pixels > (SIZE_MAX-offset)/4) return kIOReturnBadArgument;
    offset += pack.skip_pixels*4;
    if (rect.height > 1 && stride > (SIZE_MAX-offset)/(rect.height-1)) return kIOReturnBadArgument;
    size_t end = offset + (rect.height-1)*stride;
    if (rect.width > (SIZE_MAX-end)/4 || end+rect.width*4 > capacity) return kIOReturnBadArgument;
    for (uint32_t y=0;y<rect.height;++y)
        for (uint32_t x=0;x<rect.width;++x)
            memcpy((unsigned char *)out+offset+y*stride+x*4,image->rgba,4);
    return kIOReturnSuccess;
}
IOReturn vgpu_surface_open(uint32_t w,uint32_t h,uint32_t f,vgpu_surface **out) {
    (void)w;(void)h;(void)f;*out=NULL;return kIOReturnUnsupported;
}
void vgpu_surface_release(vgpu_surface *surface) { (void)surface; }
IOReturn vgpu_surface_clear(vgpu_surface *s,const float rgba[4],uint64_t *gen) {
    (void)s;(void)rgba;(void)gen;return kIOReturnUnsupported;
}
IOReturn vgpu_surface_read(vgpu_surface *s,uint64_t gen,vgpu_surface_rect rect,uint32_t f,
                          vgpu_surface_pack pack,void *out,size_t capacity) {
    (void)s;(void)gen;(void)rect;(void)f;(void)pack;(void)out;(void)capacity;return kIOReturnUnsupported;
}
static void put16(void *p,size_t off,uint16_t v) { memcpy((unsigned char *)p+off,&v,2); }
static void put32(void *p,size_t off,uint32_t v) { memcpy((unsigned char *)p+off,&v,4); }
static void token(void *p,size_t off,void *v) { memcpy((unsigned char *)p+off,&v,sizeof(v)); }
static void rb(unsigned char *td,unsigned char *tu) {
    memset(td,0,VGT_TD_RB_LENGTH);memset(tu,0,VGT_TU_LENGTH);
    put16(td,0,0x2002);put16(td,0x68,0x8058);
    put16(td,0x6a,256);put16(td,0x6c,256);put16(td,0x6e,1);
    td[0x79]=td[0x7b]=1;
    put16(td,0xb4,256);put16(td,0xb6,256);put16(td,0xb8,1);td[0xbb]=0x20;
    put16(td,0xbc,256);put16(td,0xbe,256);put16(td,0xc0,0x1908);put16(td,0xc2,0x1401);
    put16(tu,8,1);tu[0x14]=1;
}
static IOReturn read_pixels(void *c,uint64_t gen,void *out,size_t n) {
    return gi_render_read(c,gen,0,0,2,2,0x1908,0x1401,out,n,0,NULL);
}
int main(void) {
    vgc_handle native;
    void *share=NULL,*other=NULL,*texture=NULL,*fb=NULL,*c=NULL,*c2=NULL;
    unsigned char td[VGT_TD_RB_LENGTH],tu[VGT_TU_LENGTH];
    unsigned char fq[VGF_FQ_LENGTH]={0},fd[VGF_FD_LENGTH]={0};
    unsigned char h[VGC_H_LENGTH]={0},b[VGC_B_LENGTH]={0},b2[VGC_B_LENGTH]={0};
    unsigned char values[VGPU_CLEAR_V_MIN_LENGTH]={0};
    const float rgba[4]={0.2f,0.4f,0.6f,0.8f}; memcpy(values,rgba,sizeof(rgba));
    uint64_t gen=999,flushed=999;
    OK(vgc_share_create_bgra_registry(73,&native));
    CHECK(gi_register_native_share(&device,73,native)==GI_CONTEXT_OK);
    OK(vgc_release(native));
    CHECK(gi_context_set_policy(GI_CONTEXT_METADATA_EXPERIMENT)==GI_CONTEXT_OK);
    CHECK(gi_create_share_group(&device,&share,4)==GI_CONTEXT_OK);
    CHECK(gi_create_share_group(&device,&other,4)==GI_CONTEXT_OK);
    OK(gi_render_share_open(share));OK(gi_render_share_open(other));
    CHECK(gi_render_share_open(share)==kIOReturnExclusiveAccess);
    CHECK(gi_destroy_share_group(share)==GI_CONTEXT_BUSY);
    rb(td,tu);
    CHECK(gi_create_texture_views(share,&texture,(vgt_views){{td,sizeof(td)},{tu,sizeof(tu)},{0}})==GI_CONTEXT_OK);
    CHECK(gi_render_texture_load(other,texture)==kIOReturnBadArgument);
    put16(fq,0,0x2000);put16(fq,0xb2,256);put16(fq,0xb4,256);put32(fq,0xc0,1);
    token(fd,0,texture);fd[0x50]=1;
    CHECK(gi_render_framebuffer_create(share,(vgc_view){fq,sizeof(fq)-1},(vgf_mut_view){fd,sizeof(fd)},&fb)==kIOReturnBadArgument);
    OK(gi_render_framebuffer_create(share,(vgc_view){fq,sizeof(fq)},(vgf_mut_view){fd,sizeof(fd)},&fb));
    CHECK(gi_render_framebuffer_load(other,fb)==kIOReturnBadArgument);
    fail_create=1;
    CHECK(gi_render_framebuffer_load(share,fb)!=0 && fd[0x50]==1 && (fd[0x51]&1) && tu[0x14]==1);
    fail_create=0;OK(gi_render_framebuffer_load(share,fb));CHECK(!fd[0x50]&&!fd[0x51]&&images==1);
    vgc_handle target=0;
    CHECK(gi_resolve_framebuffer(share,fb,&target)==GI_CONTEXT_OK && target);
    CHECK(gi_resolve_framebuffer(other,fb,&target)!=GI_CONTEXT_OK);
    OK(gi_render_context_create(share,(vgc_view){h,sizeof(h)},(vgc_view){b,sizeof(b)},&c));
    OK(gi_render_context_create(share,(vgc_view){h,sizeof(h)},(vgc_view){b2,sizeof(b2)},&c2));
    CHECK(gi_render_flush(c,&flushed)!=0 && flushed==999);
    token(b,0x4a0,fb);token(b,0x4a8,fb);token(b2,0x4a0,fb);token(b2,0x4a8,fb);
    uint32_t dirty=VGC_DRAW_DIRTY|VGC_READ_DIRTY|1;
    CHECK(gi_render_context_update(c,&dirty)==kIOReturnUnsupported && (dirty&1));
    dirty=VGC_DRAW_DIRTY|VGC_READ_DIRTY;
    OK(gi_render_context_update(c,&dirty));CHECK(dirty==0);
    CHECK(gi_render_framebuffer_destroy(share,fb)==kIOReturnExclusiveAccess);
    CHECK(gi_render_share_close(share)==kIOReturnExclusiveAccess);
    token(b,0x4a8,(void *)(uintptr_t)17);dirty=VGC_DRAW_DIRTY|VGC_READ_DIRTY;
    CHECK(gi_render_context_update(c,&dirty)==kIOReturnBadArgument && dirty==(VGC_DRAW_DIRTY|VGC_READ_DIRTY));
    token(b,0x4a8,fb);
    gi_render_triangle_target_info queried, original;
    OK(gi_render_triangle_target(c,&queried));
    CHECK(queried.width==256&&queried.height==256&&queried.image&&queried.completed_generation==0);
    original=queried;
    OK(gi_render_triangle_target(c,&queried));
    CHECK(queried.image==original.image&&queried.completed_generation==0);
    CHECK(!clears&&!triangles&&!reads);
    CHECK(gi_render_flush(c,&flushed)==kIOReturnNotReady&&flushed==999);
    put16(h,0x3cae,1);memset(h+0x3ca8,1,4);put32(h,0x4ee4,4);
    OK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen));CHECK(gen==1&&clears==1);
    OK(gi_render_flush(c,&flushed));CHECK(flushed==gen);
    unsigned before=clears;
    h[0x3d84]=1;
    CHECK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen)==kIOReturnUnsupported&&clears==before);
    CHECK(gi_render_flush(c,&flushed)==kIOReturnNotReady);
    OK(gi_render_clear(c,0,(vgc_view){values,sizeof(values)},&gen));
    CHECK(gi_render_flush(c,&flushed)==kIOReturnNotReady);
    h[0x3d84]=0;fail_clear=1;
    CHECK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen)!=0&&gen==1);
    fail_clear=0;OK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen));CHECK(gen==2);
    unsigned char pixels[96],saved[96];memset(pixels,0xa7,sizeof(pixels));memcpy(saved,pixels,sizeof(saved));
    put32(h,0x4ec8,4);put32(h,0x4ed8,1);put32(h,0x4edc,1);
    CHECK(read_pixels(c,gen,pixels,43)!=0&&!memcmp(pixels,saved,sizeof(pixels)));
    OK(read_pixels(c,gen,pixels,sizeof(pixels)));
    const unsigned char expected[4]={51,102,153,204};
    for (size_t i=0;i<sizeof(pixels);++i) {
        int written=(i>=20&&i<28)||(i>=36&&i<44);
        CHECK(pixels[i]==(written?expected[i%4]:0xa7));
    }
    memset(pixels,0xa7,sizeof(pixels));put32(h,0x4ecc,24);
    OK(read_pixels(c,gen,pixels,sizeof(pixels)));CHECK(!memcmp(pixels+28,expected,4)&&!memcmp(pixels+52,expected,4));
    h[0x4ee8]=1;memcpy(saved,pixels,sizeof(saved));
    CHECK(read_pixels(c,gen,pixels,sizeof(pixels))==kIOReturnUnsupported&&!memcmp(pixels,saved,sizeof(pixels)));
    h[0x4ee8]=0;fail_read=1;
    CHECK(gi_render_wait(c,&flushed)!=0);
    CHECK(read_pixels(c,gen,pixels,sizeof(pixels))!=0&&!memcmp(pixels,saved,sizeof(pixels)));
    fail_read=0;
    CHECK(gi_render_read(c,gen,0,0,1,1,0x1908,0x1401,pixels,sizeof(pixels),2,NULL)==kIOReturnUnsupported);
    CHECK(gi_render_read(c,gen,0,0,1,1,0x1908,0x1401,pixels,sizeof(pixels),0,texture)==kIOReturnUnsupported);
    CHECK(read_pixels(c,gen,h,sizeof(h))==kIOReturnBadArgument);
    uint64_t second;
    OK(gi_render_clear(c2,1,(vgc_view){values,sizeof(values)},&second));CHECK(second==3);
    CHECK(gi_render_flush(c,&flushed)!=0);
    CHECK(read_pixels(c,gen,pixels,sizeof(pixels))!=0);
    OK(read_pixels(c,second,pixels,sizeof(pixels)));
    uint64_t current=777;
    OK(gi_render_read_generation(c,&current));CHECK(current==second);
    CHECK(gi_render_flush(c,&flushed)!=0);
    CHECK(gi_render_read_generation(c,(uint64_t *)(void *)h)==kIOReturnBadArgument);
    CHECK(gi_render_read_generation(c,NULL)==kIOReturnBadArgument);
    CHECK(gi_render_read_generation((void *)(uintptr_t)17,&current)==kIOReturnBadArgument);
    /* 独立 image 与不同代次，防止误用 draw/last；也覆盖 READ token 未显式 update。 */
    unsigned char td2[VGT_TD_RB_LENGTH],tu2[VGT_TU_LENGTH];
    unsigned char fq2[VGF_FQ_LENGTH],fd2[VGF_FD_LENGTH]={0};
    void *texture2=NULL,*fb2=NULL;
    rb(td2,tu2);memcpy(fq2,fq,sizeof(fq2));
    CHECK(gi_create_texture_views(share,&texture2,(vgt_views){{td2,sizeof(td2)},{tu2,sizeof(tu2)},{0}})==GI_CONTEXT_OK);
    token(fd2,0,texture2);fd2[0x50]=1;
    OK(gi_render_framebuffer_create(share,(vgc_view){fq2,sizeof(fq2)},(vgf_mut_view){fd2,sizeof(fd2)},&fb2));
    token(b,0x4a8,fb2);current=777;
    CHECK(gi_render_read_generation(c,&current)==kIOReturnNotReady&&current==777);
    OK(gi_render_framebuffer_load(share,fb2));
    CHECK(gi_render_read_generation(c,&current)==kIOReturnNotReady&&current==777);
    token(b2,0x4a0,fb2);
    OK(gi_render_clear(c2,1,(vgc_view){values,sizeof(values)},&second));CHECK(second==1);
    OK(gi_render_read_generation(c,&current));CHECK(current==1);
    OK(read_pixels(c,current,pixels,sizeof(pixels)));
    token(b,0x4a8,NULL);current=777;
    CHECK(gi_render_read_generation(c,&current)==kIOReturnNotReady&&current==777);
    token(b,0x4a8,(void *)(uintptr_t)17);
    CHECK(gi_render_read_generation(c,&current)==kIOReturnBadArgument&&current==777);
    token(b,0x4a8,fb);
    OK(gi_render_read_generation(c,&current));CHECK(current==3);
    /* 同一外部 FBO 更换附件，必须查询本次 load 后的新 target。 */
    token(fd,0,texture2);fd[0x50]=1;
    OK(gi_render_read_generation(c,&current));CHECK(current==1);
    token(fd,0,texture);fd[0x50]=1;
    OK(gi_render_read_generation(c,&current));CHECK(current==3);
    put32(fq,4,1);current=777;
    CHECK(gi_render_read_generation(c,&current)==kIOReturnUnsupported&&current==777);
    put32(fq,4,0);
    CHECK(gi_render_read_generation(c,&current)==kIOReturnNotReady&&current==777);
    OK(gi_render_framebuffer_load(share,fb));
    const float vertices[3][4]={{-0.5f,-0.5f,0,1},{0.5f,-0.5f,0,1},{0,0.5f,0,1}};
    token(b,0x4a8,fb2);before=clears;
    OK(gi_render_draw_triangle(c,vertices,&gen));CHECK(gen==4&&triangles==1&&clears==before);
    CHECK(!memcmp(submitted_vertices,vertices,sizeof(vertices)));
    OK(gi_render_flush(c,&flushed));CHECK(flushed==4);
    OK(gi_render_read_generation(c,&current));CHECK(current==1);
    fail_triangle=1;
    CHECK(gi_render_draw_triangle(c,vertices,&gen)==kIOReturnNotReady&&gen==4);
    CHECK(gi_render_wait(c,&flushed)==kIOReturnNotReady);
    OK(gi_render_read_generation(c,&current));CHECK(current==1);
    OK(gi_render_clear(c,0,(vgc_view){values,sizeof(values)},&gen));
    CHECK(gi_render_flush(c,&flushed)==kIOReturnNotReady);
    fail_triangle=0;
    OK(gi_render_draw_triangle(c,vertices,&gen));CHECK(gen==5);
    OK(gi_render_wait(c,&flushed));CHECK(flushed==5);
    before=triangles;
    CHECK(gi_render_draw_triangle(c,NULL,&gen)==kIOReturnBadArgument&&gen==5);
    CHECK(gi_render_flush(c,&flushed)==kIOReturnNotReady);
    CHECK(gi_render_draw_triangle(c,vertices,NULL)==kIOReturnBadArgument);
    CHECK(gi_render_draw_triangle(c,vertices,(uint64_t *)(void *)h)==kIOReturnBadArgument);
    _Alignas(uint64_t) float invalid[3][4];memcpy(invalid,vertices,sizeof(invalid));
    CHECK(gi_render_draw_triangle(c,(const float (*)[4])invalid,(uint64_t *)(void *)invalid)==kIOReturnBadArgument);
    invalid[0][0]=NAN;
    CHECK(gi_render_draw_triangle(c,(const float (*)[4])invalid,&gen)==kIOReturnBadArgument);
    invalid[0][0]=2;
    CHECK(gi_render_draw_triangle(c,(const float (*)[4])invalid,&gen)==kIOReturnUnsupported);
    CHECK(triangles==before&&gen==5);
    token(b,0x4a0,NULL);
    CHECK(gi_render_draw_triangle(c,vertices,&gen)==kIOReturnNotReady);
    token(b,0x4a0,(void *)(uintptr_t)17);
    CHECK(gi_render_draw_triangle(c,vertices,&gen)==kIOReturnBadArgument);
    token(b,0x4a0,fb);
    put32(fq,4,1);
    CHECK(gi_render_draw_triangle(c,vertices,&gen)==kIOReturnUnsupported&&triangles==before);
    put32(fq,4,0);
    CHECK(gi_render_draw_triangle(c,vertices,&gen)==kIOReturnNotReady);
    OK(gi_render_framebuffer_load(share,fb));
    OK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen));CHECK(gen==6);
    OK(gi_render_flush(c,&flushed));CHECK(flushed==6);
    /* 查询始终重新解析 DRAW；READ 故意保留另一 image。 */
    unsigned query_clears=clears,query_triangles=triangles,query_reads=reads;
    OK(gi_render_triangle_target(c,&queried));
    CHECK(queried.image==original.image&&queried.completed_generation==6);
    CHECK(clears==query_clears&&triangles==query_triangles&&reads==query_reads);
    OK(gi_render_flush(c,&flushed));CHECK(flushed==6);
    query_reads=reads; /* flush 本身会通过 read 验证完成状态。 */
    token(b,0x4a0,fb2);
    OK(gi_render_triangle_target(c,&queried));
    CHECK(queried.image!=original.image&&queried.completed_generation==1);
    vgc_handle image2=queried.image;
    token(b,0x4a0,fb);token(fd,0,texture2);fd[0x50]=1;
    OK(gi_render_triangle_target(c,&queried));
    CHECK(queried.image==image2&&queried.completed_generation==1);
    token(fd,0,texture);fd[0x50]=1;
    OK(gi_render_triangle_target(c,&queried));
    CHECK(queried.image==original.image&&queried.completed_generation==6);
    memset(&original,0xa7,sizeof(original));queried=original;
#define QUERY_FAIL(status) do { \
    CHECK(gi_render_triangle_target(c,&queried)==(status)); \
    CHECK(!memcmp(&queried,&original,sizeof(queried))); \
    flushed=777;CHECK(gi_render_flush(c,&flushed)==kIOReturnNotReady&&flushed==777); \
} while (0)
    token(b,0x4a0,NULL);QUERY_FAIL(kIOReturnNotReady);
    token(b,0x4a0,(void *)(uintptr_t)17);QUERY_FAIL(kIOReturnBadArgument);
    unsigned char foreign_fq[VGF_FQ_LENGTH],foreign_fd[VGF_FD_LENGTH]={0};
    memcpy(foreign_fq,fq,sizeof(foreign_fq));
    void *foreign_fb=NULL;
    OK(gi_render_framebuffer_create(other,(vgc_view){foreign_fq,sizeof(foreign_fq)},
                                   (vgf_mut_view){foreign_fd,sizeof(foreign_fd)},&foreign_fb));
    token(b,0x4a0,foreign_fb);QUERY_FAIL(kIOReturnBadArgument);
    OK(gi_render_framebuffer_destroy(other,foreign_fb));
    QUERY_FAIL(kIOReturnBadArgument);
    token(b,0x4a0,fb);
    put16(fq,0xb2,128);QUERY_FAIL(kIOReturnUnsupported);
    put16(fq,0xb2,256);QUERY_FAIL(kIOReturnNotReady);
    OK(gi_render_framebuffer_load(share,fb));
    put16(fq,0xb4,128);QUERY_FAIL(kIOReturnUnsupported);
    put16(fq,0xb4,256);OK(gi_render_framebuffer_load(share,fb));
    put32(fq,0xc0,3);QUERY_FAIL(kIOReturnUnsupported);
    put32(fq,0xc0,1);OK(gi_render_framebuffer_load(share,fb));
    td[0x7c]=1;QUERY_FAIL(kIOReturnUnsupported);
    td[0x7c]=0;OK(gi_render_framebuffer_load(share,fb));
    put16(td,0x68,0x8c43);QUERY_FAIL(kIOReturnUnsupported);
    put16(td,0x68,0x8058);OK(gi_render_framebuffer_load(share,fb));
    token(fd,0,(void *)(uintptr_t)17);QUERY_FAIL(kIOReturnBadArgument);
    token(fd,0,texture);OK(gi_render_framebuffer_load(share,fb));
    CHECK(gi_render_triangle_target(c,NULL)==kIOReturnBadArgument);
    CHECK(gi_render_triangle_target(c,(gi_render_triangle_target_info *)(void *)h)==kIOReturnBadArgument);
    CHECK(gi_render_triangle_target(c,(gi_render_triangle_target_info *)(void *)b)==kIOReturnBadArgument);
    CHECK(gi_render_triangle_target(c,(gi_render_triangle_target_info *)(void *)fq)==kIOReturnBadArgument);
    CHECK(gi_render_triangle_target(c,(gi_render_triangle_target_info *)(void *)fd)==kIOReturnBadArgument);
    _Alignas(gi_render_triangle_target_info) unsigned char unaligned[sizeof(queried)+1];
    memset(unaligned,0xa7,sizeof(unaligned));
    CHECK(gi_render_triangle_target(c,(gi_render_triangle_target_info *)(void *)(unaligned+1))==kIOReturnBadArgument);
    for (size_t i=0;i<sizeof(unaligned);++i) CHECK(unaligned[i]==0xa7);
    CHECK(gi_render_triangle_target((void *)(uintptr_t)17,&queried)==kIOReturnBadArgument);
    CHECK(!memcmp(&queried,&original,sizeof(queried)));
    OK(gi_render_triangle_target(c,&queried));CHECK(queried.completed_generation==6);
    CHECK(gi_render_wait(c,&flushed)==kIOReturnNotReady&&flushed==777);
    CHECK(clears==query_clears&&triangles==query_triangles&&reads==query_reads);
    OK(gi_render_draw_triangle(c,vertices,&gen));
    OK(gi_render_flush(c,&flushed));CHECK(flushed==gen);
    fail_triangle=1;
    CHECK(gi_render_draw_triangle(c,vertices,&gen)==kIOReturnNotReady);
    fail_triangle=0;
    OK(gi_render_triangle_target(c,&queried));CHECK(queried.completed_generation==gen);
    CHECK(gi_render_flush(c,&flushed)==kIOReturnNotReady);
    OK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen));
    OK(gi_render_flush(c,&flushed));CHECK(flushed==gen);
#undef QUERY_FAIL
    token(b,0x4a8,fb);token(b2,0x4a0,fb);
    dirty=VGC_DRAW_DIRTY|VGC_READ_DIRTY;OK(gi_render_context_update(c,&dirty));
    dirty=VGC_DRAW_DIRTY|VGC_READ_DIRTY;OK(gi_render_context_update(c2,&dirty));
    OK(gi_render_clear(c2,1,(vgc_view){values,sizeof(values)},&second));
    OK(gi_render_framebuffer_destroy(share,fb2));
    CHECK(gi_destroy_texture(share,texture2)==GI_CONTEXT_OK);
    OK(vgt_bgra_drain_releases());CHECK(images==1);
    /* 借用 Q/D 改变后不能偷偷使用旧 target；失败必须显式 load 重试。 */
    put32(fq,4,1);before=clears;
    CHECK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen)!=0&&clears==before);
    put32(fq,4,0);
    CHECK(gi_render_clear(c,1,(vgc_view){values,sizeof(values)},&gen)!=0);
    OK(gi_render_framebuffer_load(share,fb));
    fd[0x50]=0x80;
    CHECK(gi_render_framebuffer_load(share,fb)==kIOReturnUnsupported&&fd[0x50]==0x80);
    fd[0x50]=1;fd[0x51]=0x80;
    CHECK(gi_render_framebuffer_load(share,fb)==kIOReturnUnsupported&&fd[0x51]==0x81);
    fd[0x51]=0;OK(gi_render_framebuffer_load(share,fb));
    token(b,0x4a0,NULL);token(b,0x4a8,NULL);dirty=VGC_DRAW_DIRTY|VGC_READ_DIRTY;
    OK(gi_render_context_update(c,&dirty));
    OK(gi_render_context_destroy(c));OK(gi_render_context_destroy(c2));
    CHECK(gi_destroy_texture(share,texture)==GI_CONTEXT_OK);
    fail_release=1;
    CHECK(gi_render_framebuffer_destroy(share,fb)!=0 && images==1);
    CHECK(gi_render_framebuffer_load(share,fb)==kIOReturnBadArgument);
    CHECK(gi_resolve_framebuffer(share,fb,&target)!=GI_CONTEXT_OK);
    fail_release=0;OK(gi_render_framebuffer_destroy(share,fb));CHECK(images==0);
    OK(gi_render_share_close(share));OK(gi_render_share_close(other));
    CHECK(gi_destroy_share_group(share)==GI_CONTEXT_OK);
    CHECK(gi_destroy_share_group(other)==GI_CONTEXT_OK);
    fail_owner=1;CHECK(gi_unregister_native_share(&device)==GI_CONTEXT_BACKEND&&owners==1);
    fail_owner=0;CHECK(gi_unregister_native_share(&device)==GI_CONTEXT_OK);
    OK(vgt_bgra_drain_releases());CHECK(!owners&&!images);
    puts("PASS CPU render: token/同share/有界FBO/跨context与draw-read分离代次/附件替换/triangle目标只查询与失败pending/triangle提交/动态pack/释放重试");
    printf("CPU模型边界调用: clear=%u read=%u triangle=%u；不验证未知H或GPU光栅化/系统OpenGL/Metal\n",clears,reads,triangles);
    return 0;
}
