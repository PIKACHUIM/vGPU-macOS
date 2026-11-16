/* 受限内部 primitive -> native context -> surface opcode4 探针；不证明公开 OpenGL。
 * CPU 模式仅执行 staging/状态机与独立像素 oracle，绝不发现服务或访问 GPU。
 * 复用资源探针整数几何 oracle，避免修改其他代理拥有的 context 探针。
 */
#define main primitive_unused_resource_main
#include "gld_resource_probe.c"
#undef main
#undef REQUIRE
#include "../gld-context/vgpu_context.h"
#include "../gld-integration/primitive_exports.h"
#include <math.h>
#include <unistd.h>

enum { P_OFFSET = 1040, P_STRIDE = 1032, P_SIZE = P_OFFSET + HEIGHT * P_STRIDE + 16 };
static const struct color background = {{.25f,.75f,.125f,.5f}, {32,191,64,128}};
static unsigned assertions;
#define REQUIRE(x) do { ++assertions; if (!(x)) { fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x); goto cleanup; } } while (0)
#define GOOD(x) REQUIRE(code((x),kIOReturnSuccess,#x))
#define BAD(x,e) REQUIRE(code((x),(e),#x))
struct counters { unsigned retains, releases, submits, flushes, freed; };
struct primitive_owner {
    bool cpu;
    unsigned refs, expected_triangle;
    vgc_handle native, framebuffer;
    unsigned char h[VGC_H_LENGTH], b[VGC_B_LENGTH];
    struct counters *counts;
    uint64_t cpu_generation;
};
static void p_store32(unsigned char *p, size_t offset, uint32_t value)
{ memcpy(p + offset,&value,sizeof(value)); }
static void p_store64(unsigned char *p, size_t offset, uint64_t value)
{ memcpy(p + offset,&value,sizeof(value)); }
static IOReturn p_bind(struct primitive_owner *owner, vgc_handle fb)
{
    p_store64(owner->b,0x4a0,fb); p_store64(owner->b,0x4a8,fb);
    uint32_t dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    IOReturn r = vgc_context_update(owner->native,&dirty);
    return !r && dirty ? kIOReturnError : r;
}
static IOReturn p_retain(void *opaque)
{
    struct primitive_owner *owner = opaque;
    IOReturn r = owner->cpu ? kIOReturnSuccess : vgc_retain(owner->native);
    if (!r) { ++owner->refs; ++owner->counts->retains; }
    return r;
}
static IOReturn p_release(void *opaque)
{
    struct primitive_owner *owner = opaque;
    IOReturn r = owner->cpu ? kIOReturnSuccess : vgc_release(owner->native);
    if (r) return r;
    ++owner->counts->releases;
    if (!--owner->refs) { ++owner->counts->freed; free(owner); }
    return kIOReturnSuccess;
}
static IOReturn p_submit(void *opaque, const float vertices[3][4], uint64_t *generation)
{
    struct primitive_owner *owner = opaque;
    ++owner->counts->submits;
    /* 独立预期值：输入 z=-1,-0.5,+1 必须成为 0,0.25,1，不能只看无深度的红图。 */
    static const float mapped_z[3] = {0,.25f,1};
    for (unsigned i = 0; i < 3; ++i)
        if (vertices[i][0] != triangles[owner->expected_triangle].input.vertices[i][0] ||
            vertices[i][1] != triangles[owner->expected_triangle].input.vertices[i][1] ||
            vertices[i][2] != mapped_z[i] || vertices[i][3] != 1) return kIOReturnBadArgument;
    if (owner->cpu) { *generation = ++owner->cpu_generation; return kIOReturnSuccess; }
    IOReturn r = p_bind(owner,owner->framebuffer);
    return r ? r : vgc_draw_triangle(owner->native,vertices,generation);
}
static IOReturn p_flush(void *opaque, uint64_t *generation)
{
    struct primitive_owner *owner = opaque;
    ++owner->counts->flushes;
    if (owner->cpu) { *generation = owner->cpu_generation; return kIOReturnSuccess; }
    return vgc_flush_completed(owner->native,generation);
}
static gi_primitive_state_desc p_state(void)
{
    gi_primitive_state_desc state = {0};
    static const unsigned char position[8] = {1,0,4,0,0,0x18,0,0};
    state.version = GI_PRIMITIVE_STATE_VERSION; state.size = sizeof(state);
    state.all_state_verified = 1; state.full_target_viewport = 1;
    state.color_write_mask = 15; state.samples = 1;
    for (unsigned i = 0; i < 16; ++i) state.modelview[i] = state.projection[i] = i % 5 == 0 ? 1 : 0;
    state.rgba[0] = state.rgba[3] = 1;
    memcpy(state.position_descriptor,position,sizeof(position));
    return state;
}
static void p_vertices(float vertices[3][4], unsigned triangle)
{
    static const float gl_z[3] = {-1,-.5f,1};
    memcpy(vertices,triangles[triangle].input.vertices,sizeof(float) * 12);
    for (unsigned i = 0; i < 3; ++i) vertices[i][2] = gl_z[i];
}
static bool p_expected(unsigned x, unsigned y, unsigned mask, const struct color *bg, unsigned char rgba[4])
{
    bool edge = false, hit = false;
    for (unsigned t = 0; t < ARRAY_COUNT(triangles); ++t)
        if (mask & (1u << t)) hit |= inside(&triangles[t],x,y,&edge);
    const unsigned char *bgra = hit ? opaque_red : bg->bgra;
    rgba[0] = bgra[2]; rgba[1] = bgra[1]; rgba[2] = bgra[0]; rgba[3] = bgra[3];
    return !edge;
}
static bool p_verify(const unsigned char *pixels, unsigned mask, const struct color *bg, bool report)
{
    for (size_t k = 0; k < P_SIZE; ++k) {
        unsigned char expected = 0xa5;
        if (k >= P_OFFSET) {
            size_t relative = k - P_OFFSET;
            unsigned y = (unsigned)(relative / P_STRIDE), byte = (unsigned)(relative % P_STRIDE);
            if (y < HEIGHT && byte < ROW_BYTES) {
                unsigned char rgba[4];
                if (!p_expected(byte / 4,y,mask,bg,rgba)) return false;
                expected = rgba[byte % 4];
            }
        }
        if (pixels[k] != expected) {
            if (report) fprintf(stderr,"FAIL pixel offset=%zu actual=%02x expected=%02x mask=%u\n",k,pixels[k],expected,mask);
            return false;
        }
    }
    return true;
}
static bool p_read(struct primitive_owner *owner, uint64_t generation, unsigned mask,
                   const struct color *bg, unsigned char *pixels, const char *label)
{
    if (owner->cpu) return true;
    memset(pixels,0xa5,P_SIZE);
    if (!code(vgc_read(owner->native,generation,(vgpu_surface_rect){0,0,WIDTH,HEIGHT},pixels,P_SIZE),
              kIOReturnSuccess,label) || !p_verify(pixels,mask,bg,true)) return false;
    printf("PASS %s generation=%" PRIu64 " mask=%u pixels=65536 RGBA+pack\n",label,generation,mask);
    return true;
}
static bool p_cpu_oracle(void)
{
    if (!oracle_selftest()) return false;
    unsigned a_only = 0, b_only = 0, overlap = 0;
    bool edge = false, ok = true;
    unsigned char *pixels = malloc(P_SIZE);
    if (!pixels) return false;
    memset(pixels,0xa5,P_SIZE);
    for (unsigned y = 0; y < HEIGHT; ++y) for (unsigned x = 0; x < WIDTH; ++x) {
        bool a = inside(&triangles[1],x,y,&edge), b = inside(&triangles[2],x,y,&edge);
        a_only += a && !b; b_only += b && !a; overlap += a && b;
        ok = p_expected(x,y,6,&background,pixels + P_OFFSET + y * P_STRIDE + x * 4) && ok;
    }
    ok = ok && !edge && a_only && b_only && overlap && a_only + overlap == 6400 && b_only + overlap == 12800;
    ok = ok && p_verify(pixels,6,&background,false);
    const size_t mutations[] = {0,P_OFFSET-1,P_OFFSET,P_OFFSET+1,P_OFFSET+2,P_OFFSET+3,P_OFFSET+ROW_BYTES,P_SIZE-1};
    for (unsigned i = 0; i < ARRAY_COUNT(mutations); ++i) {
        pixels[mutations[i]] ^= 1;
        ok = ok && !p_verify(pixels,6,&background,false);
        pixels[mutations[i]] ^= 1;
    }
    free(pixels);
    printf("CPU oracle onlyA=%u onlyB=%u overlap=%u RGBA/pack mutations=8 %s\n",a_only,b_only,overlap,ok ? "PASS" : "FAIL");
    return ok;
}
static bool p_draw(gi_primitive_handle primitive, struct primitive_owner *owner,
                   unsigned triangle, uint64_t *generation)
{
    int32_t capacity = 100;
    float (*vertices)[4] = NULL;
    uint64_t next = UINT64_MAX, completed = UINT64_MAX;
    owner->expected_triangle = triangle;
    if (!code(gi_primitive_begin(primitive,GI_PRIMITIVE_TRIANGLES,&capacity,&vertices),kIOReturnSuccess,"Begin") ||
        !check(capacity == 64 && vertices && (uintptr_t)vertices % 16 == 0,"staging 容量与对齐")) return false;
    p_vertices(vertices,triangle);
    if (!code(gi_primitive_end(primitive,GI_PRIMITIVE_END_FLAGS,GI_PRIMITIVE_TRIANGLES,3,&next),kIOReturnSuccess,"End") ||
        !check(next == *generation + 1,"End 精确 generation+1") ||
        !code(gi_primitive_flush(primitive,&completed),kIOReturnSuccess,"primitive flush") ||
        !check(completed == next,"primitive flush 精确代次")) return false;
    *generation = next;
    return true;
}
static void p_cleanup_handle(vgc_handle *handle, bool *ok)
{
    for (unsigned i = 0; *handle && i < 3; ++i) {
        IOReturn r = vgc_release(*handle);
        if (!r) { *handle = 0; return; }
        *ok = false; fprintf(stderr,"FAIL cleanup handle=%" PRIu64 " code=%08x\n",*handle,(unsigned)r);
    }
}
static int p_run(bool cpu, bool colored_test)
{
    bool ok = false, external_owner = false;
    struct counters counts = {0};
    struct primitive_owner *owner = calloc(1,sizeof(*owner));
    gi_primitive_handle primitive = 0, colored = 0;
    vgc_handle share = 0, spare = 0, image = 0, fb = 0, blocker = 0, extra = 0;
    vgc_handle retired_native = 0, retired_image = 0;
    unsigned char *pixels = NULL;
    uint64_t generation = 2, out = UINT64_MAX;
    int32_t capacity = 3;
    float (*vertices)[4] = NULL;
    gi_primitive_status status;
    gi_primitive_state_desc state = p_state();
    vgc_image_desc image_desc = {WIDTH,HEIGHT,VGPU_SURFACE_BGRA8,0};
    REQUIRE(owner != NULL);
    owner->cpu = cpu; owner->refs = 1; owner->counts = &counts; owner->cpu_generation = generation;
    external_owner = true;
    if (!cpu) {
        REQUIRE(geteuid() == 0);
        uint64_t registry_id = 0;
        io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("vgpuAccelerator"));
        REQUIRE(service != IO_OBJECT_NULL);
        IOReturn found = IORegistryEntryGetRegistryEntryID(service,&registry_id);
        IOReturn released = IOObjectRelease(service);
        REQUIRE(!found && !released && registry_id);
        printf("MODE GPU_INTERNAL_PRIMITIVE registry_id=0x%" PRIx64 "；需要两槽空闲、串行执行\n",registry_id);
        GOOD(vgc_share_create_bgra_registry(registry_id,&share));
        GOOD(vgc_share_create_bgra_registry(registry_id,&spare));
        p_store32(owner->h,0x4ecc,P_STRIDE); p_store32(owner->h,0x4ed8,1);
        p_store32(owner->h,0x4edc,2); p_store32(owner->h,0x4ee4,8);
        GOOD(vgc_context_create(share,(vgc_view){owner->h,sizeof(owner->h)},(vgc_view){owner->b,sizeof(owner->b)},&owner->native));
        GOOD(vgc_image_create(share,image_desc,&image));
        GOOD(vgc_image_create(spare,image_desc,&blocker));
        GOOD(vgc_framebuffer_create(share,&fb));
        GOOD(vgc_framebuffer_attach(fb,0,image));
        owner->framebuffer = fb;
        GOOD(p_bind(owner,fb));
        vgc_clear_desc clear = {1,15,0,0,{0}};
        memcpy(clear.rgba,background.rgba,sizeof(clear.rgba));
        GOOD(vgc_clear(owner->native,&clear,&generation)); REQUIRE(generation == 2);
        pixels = malloc(P_SIZE); REQUIRE(pixels != NULL);
        REQUIRE(p_read(owner,generation,0,&background,pixels,"初始非对称底色"));
        retired_native = owner->native; retired_image = image;
    }
    gi_primitive_backend backend = {owner,p_retain,p_release,p_submit,p_flush};
    GOOD(gi_primitive_create(&backend,&primitive)); REQUIRE(counts.retains == 1 && owner->refs == 2);
    GOOD(p_release(owner)); external_owner = false;
    if (!cpu) {
        GOOD(vgc_release(fb)); fb = 0;
        GOOD(vgc_release(image)); image = 0;
        GOOD(vgc_release(share)); share = 0;
    }
    REQUIRE(owner->refs == 1 && counts.freed == 0);
    GOOD(gi_primitive_set_state(primitive,&state));
    BAD(gi_primitive_flush(primitive,&out),kIOReturnNotReady); REQUIRE(out == UINT64_MAX && !counts.flushes);
    REQUIRE(p_draw(primitive,owner,1,&generation));
    REQUIRE(p_read(owner,generation,2,&background,pixels,"Begin/End 负z映射首图"));
    REQUIRE(p_draw(primitive,owner,2,&generation));
    REQUIRE(p_read(owner,generation,6,&background,pixels,"两次 draw 无 clear 累积"));
    /* 每项拒绝都复验全图、代次、回调计数，成功配置或空 End 不能清 pending。 */
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        unsigned submitted = counts.submits, flushed = counts.flushes;
        uint64_t previous = generation;
        out = UINT64_MAX; capacity = 3; vertices = NULL;
        if (scenario == 5) {
            gi_primitive_state_desc unsupported = state; unsupported.rgba[1] = 1;
            BAD(gi_primitive_set_state(primitive,&unsupported),kIOReturnUnsupported);
        } else {
            GOOD(gi_primitive_begin(primitive,GI_PRIMITIVE_TRIANGLES,&capacity,&vertices));
            p_vertices(vertices,1);
            IOReturn expected = kIOReturnBadArgument;
            uint32_t flags = GI_PRIMITIVE_END_FLAGS;
            if (scenario == 0) vertices[0][3] = 0;
            if (scenario == 1) vertices[0][2] = -1.25f;
            if (scenario == 2) vertices[1][0] = NAN;
            if (scenario == 3) { flags = 0; expected = kIOReturnUnsupported; }
            if (scenario == 4) {
                float (*untouched)[4] = NULL; int32_t requested = 3;
                BAD(gi_primitive_begin(primitive,GI_PRIMITIVE_TRIANGLES,&requested,&untouched),kIOReturnExclusiveAccess);
                REQUIRE(untouched == NULL && requested == 3); expected = kIOReturnExclusiveAccess;
            }
            if (scenario == 6) {
                BAD(gi_primitive_flush(primitive,&out),kIOReturnExclusiveAccess);
                REQUIRE(out == UINT64_MAX); expected = kIOReturnExclusiveAccess;
            }
            BAD(gi_primitive_end(primitive,flags,GI_PRIMITIVE_TRIANGLES,3,&out),expected);
        }
        REQUIRE(out == UINT64_MAX && counts.submits == submitted && counts.flushes == flushed);
        GOOD(gi_primitive_get_status(primitive,&status));
        REQUIRE(!status.active && status.pending_failure && status.completed_generation == previous);
        IOReturn pending = status.pending_failure;
        BAD(gi_primitive_flush(primitive,&out),pending);
        GOOD(gi_primitive_set_state(primitive,&state));
        capacity = 3;
        GOOD(gi_primitive_begin(primitive,GI_PRIMITIVE_TRIANGLES,&capacity,&vertices));
        GOOD(gi_primitive_end(primitive,GI_PRIMITIVE_END_FLAGS,GI_PRIMITIVE_TRIANGLES,0,&out));
        BAD(gi_primitive_flush(primitive,&out),pending);
        REQUIRE(out == UINT64_MAX && counts.submits == submitted && counts.flushes == flushed);
        REQUIRE(p_read(owner,previous,6,&background,pixels,"拒绝与 pending 保持全部像素"));
        REQUIRE(p_draw(primitive,owner,1,&generation));
        REQUIRE(generation == previous + 1 && counts.submits == submitted + 1 && counts.flushes == flushed + 1);
        GOOD(gi_primitive_get_status(primitive,&status));
        REQUIRE(!status.pending_failure && status.last_failure && status.completed_generation == generation);
        REQUIRE(p_read(owner,generation,6,&background,pixels,"有效 draw 恢复且不 clear"));
    }
    if (colored_test) {
        GOOD(gi_primitive_create_colored_probe(&backend,&colored));
        REQUIRE(owner->refs == 2 && counts.retains == 2);
        for (unsigned scenario = 0; scenario < 3; ++scenario) {
            gi_primitive_colored_vertex *records = NULL;
            float positions[3][4];
            capacity = 64;
            owner->expected_triangle = 1;
            GOOD(gi_primitive_begin_colored(colored,GI_PRIMITIVE_TRIANGLES,&capacity,&records));
            REQUIRE(records && capacity == 64 && (uintptr_t)records % 16 == 0 && sizeof(*records) == 32);
            p_vertices(positions,1);
            for (unsigned i = 0; i < 3; ++i) {
                memcpy(records[i].position,positions[i],sizeof(positions[i]));
                records[i].rgba[0] = records[i].rgba[3] = 1;
                records[i].rgba[1] = records[i].rgba[2] = 0;
            }
            unsigned submitted = counts.submits, flushed = counts.flushes;
            uint64_t previous = generation;
            out = UINT64_MAX;
            if (scenario == 1) {
                /* Begin/End 当前色都红，但中间顶点为绿；必须在提交前拒绝。 */
                records[1].rgba[0] = 0; records[1].rgba[1] = 1;
                BAD(gi_primitive_end(colored,GI_PRIMITIVE_END_FLAGS,GI_PRIMITIVE_TRIANGLES,3,&out),kIOReturnUnsupported);
                REQUIRE(out == UINT64_MAX && counts.submits == submitted);
                BAD(gi_primitive_flush(colored,&out),kIOReturnUnsupported);
                REQUIRE(out == UINT64_MAX && counts.flushes == flushed);
                GOOD(gi_primitive_get_status(colored,&status));
                REQUIRE(status.pending_failure && status.completed_generation == previous);
                REQUIRE(p_read(owner,previous,6,&background,pixels,"逐顶点红绿红拒绝保持像素与代次"));
            } else {
                GOOD(gi_primitive_end(colored,GI_PRIMITIVE_END_FLAGS,GI_PRIMITIVE_TRIANGLES,3,&out));
                REQUIRE(out == previous + 1 && counts.submits == submitted + 1);
                generation = out;
                GOOD(gi_primitive_flush(colored,&out));
                REQUIRE(out == generation && counts.flushes == flushed + 1);
                GOOD(gi_primitive_get_status(colored,&status));
                REQUIRE(!status.pending_failure && status.completed_generation == generation);
                REQUIRE(p_read(owner,generation,6,&background,pixels,"32字节 position+RGBA 提交及失败恢复"));
            }
        }
        GOOD(gi_primitive_destroy(colored)); colored = 0;
        REQUIRE(owner->refs == 1 && counts.freed == 0);
        /* 两个 staging owner 共享 native：再次提交同步旧实例的完成代次。 */
        REQUIRE(p_draw(primitive,owner,1,&generation));
    }
    if (!cpu) {
        GOOD(p_bind(owner,0)); owner->framebuffer = 0;
        GOOD(gi_primitive_flush(primitive,&out)); REQUIRE(out == generation);
        BAD(vgc_image_create(spare,image_desc,&extra),kIOReturnNoResources); REQUIRE(!extra);
        /* 外部 image/FBO/share 已释放，解绑后只剩 native.last 持有图像。 */
    }
    GOOD(gi_primitive_destroy(primitive)); primitive = 0; owner = NULL;
    REQUIRE(counts.retains == (colored_test ? 2u : 1u) &&
            counts.releases == (colored_test ? 3u : 2u) && counts.freed == 1);
    if (!cpu) {
        GOOD(vgc_drain_releases());
        BAD(vgc_retain(retired_native),kIOReturnBadArgument);
        BAD(vgc_retain(retired_image),kIOReturnBadArgument);
        GOOD(vgc_image_create(spare,image_desc,&extra)); REQUIRE(extra != retired_image);
        printf("PASS 最后 primitive 引用释放 native/H/B/last image，资源槽可重租\n");
    }
    ok = true;
cleanup:
    if (colored) {
        gi_primitive_status cleanup_status;
        if (!gi_primitive_get_status(colored,&cleanup_status) && cleanup_status.active) {
            uint64_t ignored = 0;
            (void)gi_primitive_end(colored,GI_PRIMITIVE_END_FLAGS,GI_PRIMITIVE_TRIANGLES,0,&ignored);
        }
        for (unsigned i = 0; colored && i < 3; ++i) {
            IOReturn r = gi_primitive_destroy(colored);
            if (!r) colored = 0;
            else { ok = false; fprintf(stderr,"FAIL cleanup colored=%08x\n",(unsigned)r); }
        }
    }
    if (primitive) {
        gi_primitive_status cleanup_status;
        if (!gi_primitive_get_status(primitive,&cleanup_status) && cleanup_status.active) {
            uint64_t ignored = 0;
            (void)gi_primitive_end(primitive,GI_PRIMITIVE_END_FLAGS,GI_PRIMITIVE_TRIANGLES,0,&ignored);
        }
        for (unsigned i = 0; primitive && i < 3; ++i) {
            IOReturn r = gi_primitive_destroy(primitive);
            if (!r) { primitive = 0; if (!external_owner) owner = NULL; }
            else { ok = false; fprintf(stderr,"FAIL cleanup primitive=%08x\n",(unsigned)r); }
        }
    }
    if (external_owner && owner) {
        /* 创建 native 之前的失败也需释放 owner，不能调用 release(0)。 */
        if (!owner->cpu && !owner->native) { free(owner); owner = NULL; }
        else for (unsigned i = 0; owner && i < 3; ++i) {
            if (!p_release(owner)) owner = NULL; else ok = false;
        }
    }
    if (!cpu) {
        p_cleanup_handle(&fb,&ok); p_cleanup_handle(&image,&ok); p_cleanup_handle(&blocker,&ok);
        p_cleanup_handle(&extra,&ok); p_cleanup_handle(&share,&ok); p_cleanup_handle(&spare,&ok);
        for (unsigned i = 0; i < 3; ++i) {
            IOReturn r = vgc_drain_releases();
            if (!r) break;
            ok = false; fprintf(stderr,"FAIL cleanup drain=%08x\n",(unsigned)r);
        }
    }
    free(pixels);
    printf("RESULT %s %s assertions=%u submit=%u flush=%u retain=%u release=%u freed=%u；非公开 OpenGL\n",
           cpu ? "CPU_PRIMITIVE" : "GPU_INTERNAL_PRIMITIVE",ok ? "PASS" : "FAIL",assertions,
           counts.submits,counts.flushes,counts.retains,counts.releases,counts.freed);
    return ok ? 0 : 1;
}
int main(int argc, char **argv)
{
    bool colored = argc == 2 && (!strcmp(argv[1],"--oracle-colored") || !strcmp(argv[1],"--gpu-colored"));
    bool cpu = argc == 2 && (!strcmp(argv[1],"--oracle-only") || !strcmp(argv[1],"--oracle-colored"));
    bool gpu = argc == 2 && (!strcmp(argv[1],"--gpu") || !strcmp(argv[1],"--gpu-colored"));
    if (!cpu && !gpu) { fprintf(stderr,"用法：%s --oracle-only | --gpu | --oracle-colored | --gpu-colored\n",argv[0]); return 2; }
    if (!p_cpu_oracle()) return 1;
    if (cpu) puts("MODE CPU：未打开 IOKit、未运行 GPU；像素合成只用于校验 oracle");
    printf("COLORED_STAGING %s\n",colored ? "enabled" : "disabled");
    return p_run(cpu,colored);
}
