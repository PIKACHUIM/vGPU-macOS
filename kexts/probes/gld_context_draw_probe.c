/* 真实 context -> surface -> selector6/opcode4 链路；不使用 CPU fixture。
 * --oracle-only 不发现服务、不创建 context、不访问 GPU；--gpu 显式执行实机验收。
 * 复用独立整数几何 oracle，原资源探针 main 不参与本探针执行。
 */
#define main context_draw_unused_resource_main
#include "gld_resource_probe.c"
#undef main
#include "../gld-context/vgpu_context.h"
#include <unistd.h>

static const struct color backgrounds[2] = {
    {{.25f, .75f, .125f, .5f}, {32, 191, 64, 128}},
    {{.875f, .125f, .625f, .25f}, {159, 32, 223, 64}}
};
enum { PACK_OFFSET = 1040, PACK_STRIDE = 1032,
       PACK_SIZE = PACK_OFFSET + HEIGHT * PACK_STRIDE + 16 };
static unsigned assertions;
#define MUST(x) do { ++assertions; if (!(x)) { fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); goto cleanup; } } while (0)
#define OK(x) MUST(code((x), kIOReturnSuccess, #x))
#define ERROR(x, e) MUST(code((x), (e), #x))

static void store32(unsigned char *p, size_t offset, uint32_t value)
{ memcpy(p + offset, &value, sizeof(value)); }
static void store64(unsigned char *p, size_t offset, uint64_t value)
{ memcpy(p + offset, &value, sizeof(value)); }
static IOReturn bind_pair(vgc_handle c, unsigned char *b, vgc_handle draw, vgc_handle read)
{
    store64(b, 0x4a0, draw); store64(b, 0x4a8, read);
    uint32_t dirty = VGC_DRAW_DIRTY | VGC_READ_DIRTY;
    IOReturn result = vgc_context_update(c, &dirty);
    return !result && dirty ? kIOReturnError : result;
}
static void set_pack(unsigned char *h)
{
    store32(h, 0x4ecc, PACK_STRIDE);
    store32(h, 0x4ed8, 1); store32(h, 0x4edc, 2);
    store32(h, 0x4ee4, 8);
}
static bool unchanged(const unsigned char *p)
{
    for (size_t k = 0; k < PACK_SIZE; ++k) if (p[k] != 0xa5) return false;
    return true;
}
static bool expected_rgba(unsigned x, unsigned y, unsigned mask,
                          const struct color *background, unsigned char out[4])
{
    bool edge = false, hit = false;
    for (unsigned t = 0; t < ARRAY_COUNT(triangles); ++t)
        if (mask & (1u << t)) hit |= inside(&triangles[t], x, y, &edge);
    const unsigned char *bgra = hit ? opaque_red : background->bgra;
    out[0] = bgra[2]; out[1] = bgra[1]; out[2] = bgra[0]; out[3] = bgra[3];
    return !edge;
}
static bool verify_packed(const unsigned char *p, unsigned mask,
                          const struct color *background, const char *label, bool report)
{
    /* 整个分配区逐字节检查，包括 RGBA 全通道、skip、行间 padding 和尾哨兵。 */
    for (size_t k = 0; k < PACK_SIZE; ++k) {
        unsigned char expected = 0xa5;
        if (k >= PACK_OFFSET) {
            size_t relative = k - PACK_OFFSET;
            unsigned y = (unsigned)(relative / PACK_STRIDE);
            unsigned byte = (unsigned)(relative % PACK_STRIDE);
            if (y < HEIGHT && byte < ROW_BYTES) {
                unsigned char rgba[4];
                if (!expected_rgba(byte / 4, y, mask, background, rgba)) return false;
                expected = rgba[byte % 4];
            }
        }
        if (p[k] != expected) {
            if (report) fprintf(stderr, "FAIL %s offset=%zu actual=%02x expected=%02x mask=%u\n",
                                label, k, p[k], expected, mask);
            return false;
        }
    }
    return true;
}
static bool read_image(vgc_handle c, uint64_t generation, unsigned mask,
                       const struct color *background, unsigned char *dst, const char *label)
{
    memset(dst, 0xa5, PACK_SIZE);
    if (!code(vgc_read(c, generation, (vgpu_surface_rect){0, 0, WIDTH, HEIGHT},
                       dst, PACK_SIZE), kIOReturnSuccess, label) ||
        !verify_packed(dst, mask, background, label, true)) return false;
    printf("PASS %s generation=%" PRIu64 " mask=%u pixels=65536 RGBA+pack\n", label, generation, mask);
    return true;
}
static bool flush_is(vgc_handle c, uint64_t generation)
{
    uint64_t completed = UINT64_MAX;
    vgc_state state;
    return code(vgc_flush_completed(c, &completed), kIOReturnSuccess, "flush last image") &&
           check(completed == generation, "flush 精确 generation") &&
           code(vgc_context_state(c, &state), kIOReturnSuccess, "context state") &&
           check(!state.dirty && !state.failed && state.completed_generation == generation,
                 "成功提交后 state");
}
static bool draw(vgc_handle c, unsigned triangle, uint64_t *generation)
{
    uint64_t previous = *generation, result = UINT64_MAX;
    if (!code(vgc_draw_triangle(c, triangles[triangle].input.vertices, &result),
              kIOReturnSuccess, "vgc_draw_triangle") ||
        !check(result == previous + 1, "draw 精确 generation+1")) return false;
    *generation = result;
    return flush_is(c, result);
}
static bool cpu_oracle(void)
{
    if (!oracle_selftest()) return false;
    unsigned only_a = 0, only_b = 0, overlap = 0;
    bool edge = false;
    for (unsigned y = 0; y < HEIGHT; ++y) for (unsigned x = 0; x < WIDTH; ++x) {
        bool a = inside(&triangles[1], x, y, &edge);
        bool b = inside(&triangles[2], x, y, &edge);
        only_a += a && !b; only_b += b && !a; overlap += a && b;
    }
    if (!check(!edge && only_a && only_b && overlap && only_a + overlap == 6400 &&
               only_b + overlap == 12800, "累积 oracle 必须区分两个独占区与重叠区")) return false;
    unsigned char *p = malloc(PACK_SIZE);
    if (!p) return false;
    memset(p, 0xa5, PACK_SIZE);
    for (unsigned y = 0; y < HEIGHT; ++y) for (unsigned x = 0; x < WIDTH; ++x)
        if (!expected_rgba(x, y, 6, &backgrounds[0], p + PACK_OFFSET + y * PACK_STRIDE + x * 4)) {
            free(p); return false;
        }
    bool ok = verify_packed(p, 6, &backgrounds[0], "CPU 合成图", false);
    const size_t mutations[] = {0, PACK_OFFSET - 1, PACK_OFFSET, PACK_OFFSET + 1,
        PACK_OFFSET + 2, PACK_OFFSET + 3, PACK_OFFSET + ROW_BYTES, PACK_SIZE - 1};
    for (unsigned j = 0; j < ARRAY_COUNT(mutations); ++j) {
        p[mutations[j]] ^= 1;
        ok = ok && !verify_packed(p, 6, &backgrounds[0], "CPU 单字节故障", false);
        p[mutations[j]] ^= 1;
    }
    free(p);
    if (!check(ok, "RGBA 四通道及 pack 哨兵比较器拒绝单字节故障")) return false;
    printf("PASS CPU 累积 oracle onlyA=%u onlyB=%u overlap=%u；未验证 GPU/context 执行\n",
           only_a, only_b, overlap);
    return true;
}
static void cleanup_release(vgc_handle *handle, bool *ok)
{
    for (unsigned attempt = 0; *handle && attempt < 3; ++attempt) {
        IOReturn result = vgc_release(*handle);
        if (!result) { *handle = 0; return; }
        *ok = false;
        fprintf(stderr, "FAIL cleanup handle=%" PRIu64 " attempt=%u result=%08x\n",
                *handle, attempt + 1, (unsigned)result);
    }
}
int main(int argc, char **argv)
{
    bool gpu = argc == 2 && !strcmp(argv[1], "--gpu");
    bool cpu = argc == 2 && !strcmp(argv[1], "--oracle-only");
    if (!gpu && !cpu) {
        fprintf(stderr, "用法：%s --oracle-only | --gpu\n", argv[0]); return 2;
    }
    if (!cpu_oracle()) return 1;
    if (cpu) { puts("RESULT CPU_ORACLE PASS；未打开 IOKit、未运行 GPU"); return 0; }
    bool ok = false;
    vgc_handle share = 0, other = 0, c[2] = {0}, f[2] = {0}, images[2] = {0}, extra = 0;
    vgc_handle retired[2] = {0};
    unsigned char h[2][VGC_H_LENGTH] = {{0}}, b[2][VGC_B_LENGTH] = {{0}};
    unsigned char *dst = NULL;
    uint64_t gen[2] = {0}, old = 0, completed = 0, registry_id = 0;
    vgc_state state;
    vgc_image_desc desc = {WIDTH, HEIGHT, VGPU_SURFACE_BGRA8, 0};
    MUST(geteuid() == 0);
    puts("MODE GPU：真实 registry/context/surface/opcode4；需要两槽空闲及串行执行");
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("vgpuAccelerator"));
    MUST(service != IO_OBJECT_NULL);
    IOReturn result = IORegistryEntryGetRegistryEntryID(service, &registry_id);
    IOReturn released = IOObjectRelease(service);
    MUST(!result && !released && registry_id);
    printf("registry_id=0x%" PRIx64 "\n", registry_id);
    OK(vgc_share_create_bgra_registry(registry_id, &share));
    OK(vgc_share_create_bgra_registry(registry_id, &other));
    dst = malloc(PACK_SIZE); MUST(dst != NULL);
    for (unsigned j = 0; j < 2; ++j) {
        set_pack(h[j]);
        OK(vgc_context_create(share, (vgc_view){h[j], sizeof(h[j])}, (vgc_view){b[j], sizeof(b[j])}, &c[j]));
        OK(vgc_image_create(share, desc, &images[j]));
        OK(vgc_framebuffer_create(share, &f[j]));
        OK(vgc_framebuffer_attach(f[j], 0, images[j]));
        OK(bind_pair(c[j], b[j], f[j], f[j]));
        vgc_clear_desc clear = {1, 15, 0, 0, {0}};
        memcpy(clear.rgba, backgrounds[j].rgba, sizeof(clear.rgba));
        OK(vgc_clear(c[j], &clear, &gen[j])); MUST(gen[j] == 2);
        MUST(read_image(c[j], gen[j], 0, &backgrounds[j], dst, "初始非对称 alpha 底色"));
    }
    MUST(gen[0] == gen[1] && images[0] != images[1] && c[0] != c[1] && f[0] != f[1]);
    ERROR(vgc_image_create(other, desc, &extra), kIOReturnNoResources); MUST(!extra);
    /* 同 generation、不同底色：读错 draw FBO 也必须被全图 oracle 发现。 */
    OK(bind_pair(c[0], b[0], f[0], f[1]));
    OK(bind_pair(c[1], b[1], f[1], f[0]));
    MUST(read_image(c[0], gen[1], 0, &backgrounds[1], dst, "C0 draw=A/read=B"));
    MUST(read_image(c[1], gen[0], 0, &backgrounds[0], dst, "C1 draw=B/read=A"));
    old = gen[0];
    MUST(draw(c[0], 1, &gen[0]));
    memset(dst, 0xa5, PACK_SIZE);
    ERROR(vgc_read(c[1], old, (vgpu_surface_rect){0, 0, WIDTH, HEIGHT}, dst, PACK_SIZE), kIOReturnNotReady);
    MUST(unchanged(dst));
    MUST(read_image(c[1], gen[0], 2, &backgrounds[0], dst, "A 首个用户三角形"));
    MUST(read_image(c[0], gen[1], 0, &backgrounds[1], dst, "A draw 后 B 保持"));
    MUST(draw(c[1], 2, &gen[1]));
    MUST(read_image(c[0], gen[1], 4, &backgrounds[1], dst, "B 不同用户三角形"));
    MUST(read_image(c[1], gen[0], 2, &backgrounds[0], dst, "B draw 后 A 保持"));
    MUST(draw(c[0], 2, &gen[0]));
    MUST(read_image(c[1], gen[0], 6, &backgrounds[0], dst, "A 无 clear 累积两个三角形"));
    MUST(read_image(c[0], gen[1], 4, &backgrounds[1], dst, "累积 A 后 B 保持"));
    MUST(flush_is(c[1], gen[1]));
    /* 重绑不改变 last；C0 随后提交 B，使 C1 的旧 B flush stale。 */
    OK(bind_pair(c[0], b[0], f[1], f[0]));
    MUST(flush_is(c[0], gen[0]));
    MUST(draw(c[0], 1, &gen[1]));
    completed = UINT64_MAX;
    ERROR(vgc_flush_completed(c[1], &completed), kIOReturnNotReady); MUST(completed == UINT64_MAX);
    MUST(read_image(c[0], gen[0], 6, &backgrounds[0], dst, "重绑 draw B 保留 read A"));
    OK(bind_pair(c[1], b[1], f[0], f[1]));
    MUST(read_image(c[1], gen[1], 6, &backgrounds[1], dst, "B 也累计两个三角形"));
    MUST(draw(c[1], 0, &gen[0]));
    MUST(read_image(c[0], gen[0], 7, &backgrounds[0], dst, "A 加入第三个三角形"));
    MUST(flush_is(c[0], gen[1]));
    /* 输入拒绝保留 generation/last/所有像素，后续有效 draw 恢复健康状态。 */
    float bad[3][4]; memcpy(bad, triangles[1].input.vertices, sizeof(bad)); bad[0][3] = 0;
    completed = UINT64_MAX;
    ERROR(vgc_draw_triangle(c[0], bad, &completed), kIOReturnBadArgument); MUST(completed == UINT64_MAX);
    OK(vgc_context_state(c[0], &state));
    MUST(state.dirty && state.failed && state.completed_generation == gen[1]);
    ERROR(vgc_flush_completed(c[0], &completed), kIOReturnNotReady); MUST(completed == UINT64_MAX);
    MUST(read_image(c[0], gen[0], 7, &backgrounds[0], dst, "拒绝 draw 后 A 保持"));
    MUST(read_image(c[1], gen[1], 6, &backgrounds[1], dst, "拒绝 draw 后 B 保持"));
    MUST(draw(c[0], 2, &gen[1]));
    MUST(read_image(c[1], gen[1], 6, &backgrounds[1], dst, "失败后 draw 恢复且未 clear"));
    /* 换 attachment 也不能改变 flush 的 last image。 */
    OK(vgc_framebuffer_attach(f[1], 0, images[0]));
    MUST(flush_is(c[0], gen[1]));
    MUST(draw(c[0], 2, &gen[0]));
    completed = UINT64_MAX;
    ERROR(vgc_flush_completed(c[1], &completed), kIOReturnNotReady); MUST(completed == UINT64_MAX);
    MUST(read_image(c[0], gen[0], 7, &backgrounds[0], dst, "换附件后 draw 实际落在 A"));
    OK(vgc_framebuffer_attach(f[1], 0, images[1]));
    MUST(read_image(c[1], gen[1], 6, &backgrounds[1], dst, "恢复附件后 B 保持"));
    MUST(draw(c[1], 1, &gen[0]));
    MUST(draw(c[0], 2, &gen[1]));
    MUST(read_image(c[0], gen[0], 7, &backgrounds[0], dst, "释放前 A 最终全图"));
    MUST(read_image(c[1], gen[1], 6, &backgrounds[1], dst, "释放前 B 最终全图"));
    memset(dst, 0xa5, PACK_SIZE);
    ERROR(vgc_read(c[1], gen[1], (vgpu_surface_rect){0, 0, WIDTH, HEIGHT}, dst,
                   PACK_OFFSET + (HEIGHT - 1) * PACK_STRIDE + ROW_BYTES - 1), kIOReturnBadArgument);
    MUST(unchanged(dst));
    for (unsigned j = 0; j < 2; ++j) {
        retired[j] = images[j];
        OK(vgc_framebuffer_attach(f[j], 0, 0));
        OK(bind_pair(c[j], b[j], 0, 0));
        OK(vgc_release(f[j])); f[j] = 0;
        OK(vgc_release(images[j])); images[j] = 0;
        MUST(flush_is(c[j], gen[1 - j]));
    }
    OK(vgc_release(share)); share = 0;
    ERROR(vgc_image_create(other, desc, &extra), kIOReturnNoResources); MUST(!extra);
    /* 每释放一个 last context 只归还一个槽；重租后直接 draw，无 clear，检验零底色。 */
    for (unsigned j = 0; j < 2; ++j) {
        OK(vgc_release(c[j])); c[j] = 0;
        OK(vgc_drain_releases());
        ERROR(vgc_retain(retired[1 - j]), kIOReturnBadArgument);
        OK(vgc_image_create(other, desc, &images[j]));
        MUST(images[j] != retired[0] && images[j] != retired[1]);
        memset(h[j], 0, sizeof(h[j])); memset(b[j], 0, sizeof(b[j])); set_pack(h[j]);
        OK(vgc_context_create(other, (vgc_view){h[j], sizeof(h[j])}, (vgc_view){b[j], sizeof(b[j])}, &c[j]));
        OK(vgc_framebuffer_create(other, &f[j]));
        OK(vgc_framebuffer_attach(f[j], 0, images[j]));
        OK(bind_pair(c[j], b[j], f[j], f[j]));
        gen[j] = 1;
        MUST(draw(c[j], j + 1, &gen[j]));
        MUST(read_image(c[j], gen[j], 1u << (j + 1), &zero, dst, "重租直接 draw：全图透明黑底，无旧内容"));
        ERROR(vgc_image_create(other, desc, &extra), kIOReturnNoResources); MUST(!extra);
    }
    MUST(read_image(c[0], gen[0], 2, &zero, dst, "第二槽重租不改变第一槽"));
    ok = true;
cleanup:
    free(dst);
    for (unsigned j = 0; j < 2; ++j) {
        cleanup_release(&c[j], &ok); cleanup_release(&f[j], &ok); cleanup_release(&images[j], &ok);
    }
    cleanup_release(&extra, &ok); cleanup_release(&share, &ok); cleanup_release(&other, &ok);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        IOReturn result = vgc_drain_releases();
        if (!result) break;
        ok = false;
        fprintf(stderr, "FAIL cleanup drain attempt=%u result=%08x\n", attempt + 1, (unsigned)result);
    }
    printf("RESULT GPU_CONTEXT_DRAW %s assertions=%u；真实 context/surface/opcode4、双图全像素\n",
           ok ? "PASS" : "FAIL", assertions);
    return ok ? 0 : 1;
}
