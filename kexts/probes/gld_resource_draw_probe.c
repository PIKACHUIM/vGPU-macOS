/* 独立 selector6 opcode4 实机探针；不链接 surface/context/texture。
 * clang -std=c11 -O2 -Wall -Wextra -Werror gld_resource_draw_probe.c \
 *   -framework IOKit -framework CoreFoundation -o resource_draw_probe
 * --oracle-only 仅 CPU；--gpu 才访问自研服务，需要管理员权限和空闲资源池。
 * 复用既有资源探针的 ABI/独立整数像素 oracle；原 main 不作为新测试执行。
 */
#define main resource_baseline_main
#include "gld_resource_probe.c"
#undef main

static const struct color asymmetric_a={{.25f,.75f,.125f,.5f},{32,191,64,128}};
static const struct color asymmetric_b={{.875f,.125f,.625f,.25f},{159,32,223,64}};

static struct submit_request draw_input(const struct resource *r, const struct triangle *t)
{
    struct submit_request q={0};
    q.version=1; q.byteSize=sizeof(q); q.opcode=4;
    q.handle=r->handle; q.expectedGeneration=r->generation;
    memcpy(q.verticesBits,t->input.vertices,sizeof(q.verticesBits));
    return q;
}
static bool draw_only(struct resource *r, const struct triangle *t)
{
    struct submit_request q=draw_input(r,t);
    struct submit_response o;
    if (!success(r->connection,SUBMIT,&q,sizeof(q),&o,sizeof(o),"DRAW_ONLY opcode4")) return false;
    if (!check(o.version==1 && o.byteSize==sizeof(o) && o.handle==r->handle &&
               o.generation==r->generation+1 && o.width==WIDTH && o.height==HEIGHT,
               "draw 响应和代次")) return false;
    r->generation=o.generation;
    return true;
}
static bool overlay_image(const struct resource *r, unsigned mask, const char *label)
{
    struct read_response o;
    unsigned red=0;
    for (uint32_t y=0;y<HEIGHT;++y) {
        struct read_request q={1,sizeof(q),r->handle,r->generation,y,0};
        if (!success(r->connection,READ_ROW,&q,sizeof(q),&o,sizeof(o),"draw READ_ROW") ||
            !check(o.version==1 && o.byteSize==sizeof(o) && o.handle==r->handle &&
                   o.generation==r->generation && o.row==y && o.rowBytes==ROW_BYTES,
                   "draw READ_ROW 身份/代次/行号")) return false;
        for (uint32_t x=0;x<WIDTH;++x) {
            bool edge=false,hit=false;
            for (unsigned t=0;t<ARRAY_COUNT(triangles);++t)
                if (mask&(1u<<t)) hit |= inside(&triangles[t],x,y,&edge);
            const unsigned char *expected=hit?opaque_red:r->background->bgra;
            if (!check(!edge && !memcmp(o.pixels+x*4,expected,4),label)) {
                fprintf(stderr,"像素 (%u,%u) BGRA=%02x %02x %02x %02x expected=%02x %02x %02x %02x\n",
                    x,y,o.pixels[x*4],o.pixels[x*4+1],o.pixels[x*4+2],o.pixels[x*4+3],
                    expected[0],expected[1],expected[2],expected[3]);
                return false;
            }
            red+=hit;
        }
    }
    if (!info_matches(r)) return false;
    printf("PASS %s handle=%" PRIu64 " generation=%" PRIu64 " red=%u mask=%u\n",
           label,r->handle,r->generation,red,mask);
    return true;
}
static bool reject_draw(const struct resource *r, io_connect_t stranger)
{
    struct submit_request q=draw_input(r,&triangles[0]);
    if (!reject_call(stranger,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),
                     kIOReturnNotPermitted,"draw 拒绝其他 owner")) return false;
    q.expectedGeneration=r->generation-1;
    if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),
                     kIOReturnNotReady,"draw 拒绝过期 generation")) return false;
    q=draw_input(r,&triangles[0]); q.rgbaBits[0]=0x80000000;
    if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),
                     kIOReturnBadArgument,"draw 未使用颜色必须全零")) return false;
    for (unsigned i=0;i<12;++i) {
        q=draw_input(r,&triangles[0]); q.verticesBits[i]=0x7fc00000;
        if (!reject_call(r->connection,SUBMIT,&q,sizeof(q),sizeof(struct submit_response),
                         kIOReturnBadArgument,"draw 拒绝 NaN 顶点")) return false;
    }
    return info_matches(r);
}
int main(int argc, char **argv)
{
    if (!oracle_selftest()) return 1;
    if (argc==2 && !strcmp(argv[1],"--oracle-only")) return 0;
    if (argc!=2 || strcmp(argv[1],"--gpu")) {
        fprintf(stderr,"用法：%s --oracle-only | --gpu\n",argv[0]);
        return 2;
    }
    io_connect_t a=IO_OBJECT_NULL,b=IO_OBJECT_NULL;
    struct resource ra={0},rb={0};
    bool ok=false;
    uint64_t legacy=0,old=0;
    if (!open_connection(&a) || !open_connection(&b) ||
        !create_resource(a,&ra) || !create_resource(b,&rb) ||
        !submit(&ra,&asymmetric_a,NULL) || !submit(&rb,&asymmetric_b,NULL) ||
        !overlay_image(&ra,0,"A 非对称底色") || !overlay_image(&rb,0,"B 非对称底色")) goto done;
    /* 旧 RTV 最后绑定：draw 必须重新选择资源，旧快照始终不变。 */
    if (!legacy_submit(a,3,NULL,&blue_quarter,&legacy)) goto done;
    old=ra.generation;
    if (!draw_only(&ra,&triangles[1]) || !stale(&ra,old) ||
        !overlay_image(&ra,2,"A 用户三角形、底色及 alpha 保留") ||
        !overlay_image(&rb,0,"A draw 不改变 B") ||
        !legacy_image(a,legacy,NULL,&blue_quarter)) goto done;
    if (!draw_only(&rb,&triangles[2]) || !overlay_image(&rb,4,"B 不同顶点覆盖") ||
        !overlay_image(&ra,2,"B draw 不改变 A 或 A 代次") ||
        !legacy_image(a,legacy,NULL,&blue_quarter)) goto done;
    if (!draw_only(&ra,&triangles[2]) || !overlay_image(&ra,6,"A 第二次 draw 累积覆盖、无隐式 clear") ||
        !overlay_image(&rb,4,"第二次 A draw 不改变 B") || !reject_draw(&ra,b) ||
        !overlay_image(&ra,6,"拒绝请求不改变目标") ||
        !overlay_image(&rb,4,"拒绝请求不改变另一目标") ||
        !legacy_image(a,legacy,NULL,&blue_quarter)) goto done;
    /* selector6 的 4→2→4→3→4：核对 VS/layout/VB 切换与旧 clear 语义。 */
    if (!submit(&ra,&asymmetric_a,&triangles[0]) ||
        !overlay_image(&ra,1,"opcode2 固定三角形回归") ||
        !draw_only(&ra,&triangles[1]) || !overlay_image(&ra,3,"opcode2 后 draw-only") ||
        !submit(&ra,&asymmetric_b,&triangles[2]) ||
        !overlay_image(&ra,4,"opcode3 用户顶点及 clear 回归") ||
        !draw_only(&ra,&triangles[1]) || !overlay_image(&ra,6,"opcode3 后 draw-only") ||
        !overlay_image(&rb,4,"混合 opcode 不改变 B") ||
        !legacy_submit(a,0,NULL,&green1,&legacy) ||
        !legacy_image(a,legacy,&triangles[0],&green1) ||
        !overlay_image(&ra,6,"旧 selector draw 不改变 A") ||
        !overlay_image(&rb,4,"旧 selector draw 不改变 B")) goto done;
    if (!submit(&ra,&asymmetric_b,NULL) || !overlay_image(&ra,0,"旧 clear ABI 回归") ||
        !overlay_image(&rb,4,"clear A 不改变 B") || !draw_only(&ra,&triangles[1]) ||
        !overlay_image(&ra,2,"clear 后继续 draw")) goto done;
    ok=true;
done:
    if (!release_resource(&ra)) ok=false;
    if (!release_resource(&rb)) ok=false;
    if (!close_connection(&a)) ok=false;
    if (!close_connection(&b)) ok=false;
    printf("%s 显式资源 draw GPU 验收；拒绝测试=%u\n",ok?"PASS":"FAIL",rejected);
    return ok?0:1;
}
