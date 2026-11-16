/* 仅 CPU 定向测试：真实 context 源码，公开调用边界注入失败及计数溢出。
 * 不编入产品，不改变产品源码，不执行 GPU 工作。 */
#define vgc_release vgo_fixture_real_release
#define vgc_share_retain vgo_fixture_real_share_retain
#define vgc_context_create vgo_fixture_real_context_create
#include "../../gld-context/vgpu_context.c"
#undef vgc_release
#undef vgc_share_retain
#undef vgc_context_create

int vgo_fixture_fail_retain;
vgc_handle vgo_fixture_fail_release_handle;
vgc_handle vgo_fixture_last_context_share, vgo_fixture_last_context;
unsigned vgo_fixture_context_releases;
IOReturn vgc_share_retain(vgc_handle id) {
    if (vgo_fixture_fail_retain) return kIOReturnNotReady;
    return vgo_fixture_real_share_retain(id);
}
IOReturn vgc_release(vgc_handle id) {
    if (id == vgo_fixture_last_context) ++vgo_fixture_context_releases;
    if (id == vgo_fixture_fail_release_handle) return kIOReturnNotReady;
    return vgo_fixture_real_release(id);
}
IOReturn vgc_context_create(vgc_handle share, vgc_view h, vgc_view b, vgc_handle *out) {
    IOReturn r = vgo_fixture_real_context_create(share, h, b, out);
    if (!r) { vgo_fixture_last_context_share = share; vgo_fixture_last_context = *out; }
    return r;
}
size_t vgo_fixture_native_refs(vgc_handle id) {
    pthread_mutex_lock(&lock);
    struct object *o = lookup_any(id);
    size_t refs = o ? o->refs : 0;
    pthread_mutex_unlock(&lock);
    return refs;
}
void vgo_fixture_native_set_refs(vgc_handle id, size_t refs) {
    pthread_mutex_lock(&lock);
    struct object *o = lookup_any(id);
    if (!o || !refs) abort();
    o->refs = refs;
    pthread_mutex_unlock(&lock);
}
