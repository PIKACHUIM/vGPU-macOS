#include "vgpu_surface_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    exit(1); } } while (0)
#define OK(x) CHECK((x) == VSR_OK)

/* 仅模拟内核实际资源动作；所有生命周期决策调用生产源，不复制状态机。 */
typedef struct {
    uint64_t generation[VSR_MAX_SLOTS];
    uint32_t pixels[VSR_MAX_SLOTS];
    unsigned allocations, releases;
} cpu_backend;

static vsr_actor open_owner(vsr_registry *r, uint64_t identity)
{
    vsr_actor a;
    OK(vsr_open(r, r->scope, identity, &a));
    return a;
}

static void allocate(cpu_backend *b, vsr_action a)
{
    CHECK(a.kind == VSR_ALLOCATE);
    CHECK(a.backing.slot < VSR_MAX_SLOTS);
    CHECK(!b->generation[a.backing.slot]);
    b->generation[a.backing.slot] = a.backing.generation;
    b->pixels[a.backing.slot] = 0;
    ++b->allocations;
}

static void clean(vsr_registry *r, cpu_backend *b)
{
    vsr_action a;
    vsr_status rc;
    while ((rc = vsr_next_cleanup(r, r->scope, &a)) == VSR_OK) {
        CHECK(a.kind == VSR_RELEASE);
        CHECK(b->generation[a.backing.slot] == a.backing.generation);
        b->generation[a.backing.slot] = 0;
        ++b->releases;
        OK(vsr_cleanup_complete(r, a));
        CHECK(vsr_cleanup_complete(r, a) == VSR_STALE);
    }
    CHECK(rc == VSR_EMPTY);
}

static vsr_action create(vsr_registry *r, cpu_backend *b, vsr_actor a, uint64_t id)
{
    vsr_action action;
    OK(vsr_reserve(r, a, id, &action));
    allocate(b, action);
    OK(vsr_complete(r, action, 1));
    return action;
}

static void invariants(const vsr_registry *r)
{
    uint32_t i, j;
    for (i = 0; i < r->slot_capacity; ++i) {
        const vsr_slot_entry *s = &r->slots[i];
        uint32_t refs = 0;
        for (j = 0; j < r->lease_capacity; ++j) {
            if (r->leases[j].token && r->leases[j].slot == i) {
                ++refs;
                CHECK(r->leases[j].generation == s->generation);
            }
        }
        CHECK(refs == s->lease_refs);
        if (refs) CHECK(s->state == VSR_SLOT_READY || s->state == VSR_SLOT_RETIRED);
        if (s->state == VSR_SLOT_FREE || s->state == VSR_SLOT_DRAINING ||
            s->state == VSR_SLOT_CANCELLED) CHECK(!refs && !s->creator_ref);
        if (s->state == VSR_SLOT_RESERVED || s->state == VSR_SLOT_READY)
            CHECK(s->creator_ref == 1);
        if (s->state == VSR_SLOT_RETIRED) CHECK(refs && !s->creator_ref);
        if (s->creator_ref) {
            unsigned found = 0;
            for (j = 0; j < r->owner_capacity; ++j)
                if (r->owners[j].token == s->creator_owner &&
                    r->owners[j].identity == s->creator_identity) ++found;
            CHECK(found == 1);
        }
    }
    for (i = 0; i < r->lease_capacity; ++i) {
        if (r->leases[i].token) {
            unsigned found = 0;
            CHECK(r->leases[i].slot < r->slot_capacity);
            for (j = 0; j < r->owner_capacity; ++j)
                if (r->owners[j].token == r->leases[i].owner) ++found;
            CHECK(found == 1);
            for (j = i + 1; j < r->lease_capacity; ++j)
                CHECK(r->leases[i].token != r->leases[j].token);
        }
    }
}

static void shared_backing_and_orders(void)
{
    unsigned order;
    for (order = 0; order < 2; ++order) {
        vsr_registry r = {0};
        cpu_backend b = {0};
        vsr_actor creator, a, c;
        vsr_action first, next, pending;
        vsr_lease la, lc, stale, extra = {99, 88};
        vsr_backing ba, bc;
        OK(vsr_init(&r, 100 + order, 1, 3, 4));
        creator = open_owner(&r, 7);
        a = open_owner(&r, 7);
        c = open_owner(&r, 7);
        first = create(&r, &b, creator, 1);
        OK(vsr_import(&r, a, 1, &la));
        OK(vsr_import(&r, c, 1, &lc));
        CHECK(la.handle != lc.handle);
        OK(vsr_resolve(&r, a, la, &ba));
        OK(vsr_resolve(&r, c, lc, &bc));
        CHECK(ba.slot == bc.slot && ba.generation == bc.generation);
        b.pixels[ba.slot] = 0x1234u;
        CHECK(b.pixels[bc.slot] == 0x1234u);
        CHECK(vsr_revoke(&r, a, 1) == VSR_DENIED);
        CHECK(vsr_release(&r, c, la) == VSR_DENIED);
        OK(vsr_revoke(&r, creator, 1));
        CHECK(vsr_import(&r, c, 1, &extra) == VSR_STALE);
        CHECK(extra.scope == 99 && extra.handle == 88);
        CHECK(vsr_reserve(&r, creator, 2, &next) == VSR_CAPACITY);
        CHECK(vsr_next_cleanup(&r, r.scope, &pending) == VSR_EMPTY);
        stale = la;
        OK(vsr_close(&r, order ? c : a));
        OK(vsr_resolve(&r, order ? a : c, order ? la : lc, &ba));
        CHECK(b.pixels[ba.slot] == 0x1234u);
        CHECK(vsr_next_cleanup(&r, r.scope, &pending) == VSR_EMPTY);
        invariants(&r);
        OK(vsr_close(&r, order ? a : c));
        CHECK(vsr_reserve(&r, creator, 2, &next) == VSR_CAPACITY);
        clean(&r, &b);
        next = create(&r, &b, creator, 2);
        CHECK(next.backing.slot == first.backing.slot);
        CHECK(next.backing.generation != first.backing.generation);
        CHECK(vsr_complete(&r, first, 1) == VSR_STALE);
        CHECK(vsr_reserve(&r, creator, 1, &pending) == VSR_STALE);
        CHECK(vsr_import(&r, creator, 1, &extra) == VSR_STALE);
        a = open_owner(&r, 7);
        CHECK(vsr_resolve(&r, a, stale, &ba) == VSR_STALE);
        OK(vsr_close(&r, a));
        OK(vsr_close(&r, creator));
        clean(&r, &b);
        invariants(&r);
        OK(vsr_quiescent(&r, r.scope));
        CHECK(b.allocations == 2 && b.releases == 2);
    }
    puts("PASS two connections share backing, both close orders, revoke, final reuse, stale");
}

static void creator_disconnect(void)
{
    vsr_registry r = {0};
    cpu_backend b = {0};
    vsr_actor creator, a, c;
    vsr_lease l0, l1, l2;
    vsr_backing backing;
    vsr_action pending;
    OK(vsr_init(&r, 200, 1, 3, 4));
    creator = open_owner(&r, 8);
    a = open_owner(&r, 8);
    c = open_owner(&r, 8);
    (void)create(&r, &b, creator, 1);
    OK(vsr_import(&r, creator, 1, &l0));
    OK(vsr_import(&r, a, 1, &l1));
    OK(vsr_import(&r, c, 1, &l2));
    OK(vsr_close(&r, creator));
    CHECK(vsr_close(&r, creator) == VSR_CLOSED);
    CHECK(vsr_import(&r, a, 1, &l0) == VSR_STALE);
    OK(vsr_resolve(&r, a, l1, &backing));
    OK(vsr_resolve(&r, c, l2, &backing));
    OK(vsr_close(&r, a));
    OK(vsr_resolve(&r, c, l2, &backing));
    CHECK(vsr_next_cleanup(&r, r.scope, &pending) == VSR_EMPTY);
    OK(vsr_release(&r, c, l2));
    CHECK(vsr_release(&r, c, l2) == VSR_STALE);
    invariants(&r);
    clean(&r, &b);
    OK(vsr_close(&r, c));
    OK(vsr_quiescent(&r, r.scope));
    CHECK(b.releases == 1);
    puts("PASS disconnect creator or one draw owner preserves other leases");
}

static void creator_reference(void)
{
    vsr_registry r = {0};
    cpu_backend b = {0};
    vsr_actor a;
    vsr_lease l, old;
    vsr_action pending;
    OK(vsr_init(&r, 201, 1, 1, 1));
    a = open_owner(&r, 1);
    (void)create(&r, &b, a, 1);
    OK(vsr_import(&r, a, 1, &l));
    old = l;
    OK(vsr_release(&r, a, l));
    CHECK(vsr_next_cleanup(&r, r.scope, &pending) == VSR_EMPTY);
    OK(vsr_import(&r, a, 1, &l));
    CHECK(l.handle != old.handle);
    CHECK(vsr_release(&r, a, old) == VSR_STALE);
    OK(vsr_release(&r, a, l));
    OK(vsr_revoke(&r, a, 1));
    clean(&r, &b);
    OK(vsr_close(&r, a));
    puts("PASS creator reference independently retains backing, lease tokens do not ABA");
}

static void isolation(void)
{
    vsr_registry r = {0}, other = {0}, before;
    cpu_backend b = {0}, b2 = {0};
    vsr_actor a, foreign, same, wrong, remote;
    vsr_action allocation, remote_allocation, tampered;
    vsr_lease l, remote_lease;
    vsr_backing out;
    OK(vsr_init(&r, 300, 2, 3, 3));
    OK(vsr_init(&other, 301, 2, 3, 3));
    a = open_owner(&r, 10);
    remote = open_owner(&other, 10);
    CHECK(remote.owner == a.owner);
    allocation = create(&r, &b, a, 1);
    remote_allocation = create(&other, &b2, remote, 1);
    foreign = open_owner(&r, 11);
    same = open_owner(&r, 10);
    OK(vsr_import(&r, same, 1, &l));
    OK(vsr_import(&other, remote, 1, &remote_lease));
    memcpy(&before, &r, sizeof(r));
    CHECK(vsr_import(&r, foreign, 1, &remote_lease) == VSR_DENIED);
    wrong = same;
    wrong.identity = foreign.identity;
    CHECK(vsr_import(&r, wrong, 1, &remote_lease) == VSR_DENIED);
    CHECK(vsr_import(&r, remote, 1, &remote_lease) == VSR_SCOPE);
    CHECK(vsr_complete(&r, remote_allocation, 1) == VSR_SCOPE);
    CHECK(vsr_resolve(&r, same, remote_lease, &out) == VSR_SCOPE);
    CHECK(vsr_resolve(&r, foreign, l, &out) == VSR_DENIED);
    CHECK(vsr_resolve(&r, a, l, &out) == VSR_DENIED);
    CHECK(vsr_revoke(&r, same, 1) == VSR_DENIED);
    CHECK(vsr_close(&r, wrong) == VSR_DENIED);
    CHECK(vsr_open(&r, other.scope, 10, &wrong) == VSR_SCOPE);
    CHECK(vsr_reserve(&r, a, 1, &tampered) == VSR_DUPLICATE);
    tampered = allocation;
    tampered.kind = VSR_RELEASE;
    CHECK(vsr_cleanup_complete(&r, tampered) == VSR_NOT_READY);
    CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    OK(vsr_close(&r, a));
    OK(vsr_close(&r, same));
    OK(vsr_close(&r, foreign));
    clean(&r, &b);
    OK(vsr_close(&other, remote));
    clean(&other, &b2);
    puts("PASS scope, identity, owner, action-kind isolation and unchanged failure state");
}

static void faults(void)
{
    unsigned mode;
    for (mode = 0; mode < 5; ++mode) {
        vsr_registry r = {0};
        cpu_backend b = {0};
        vsr_actor a, survivor;
        vsr_action allocation, cleanup, repeated, next;
        vsr_lease l;
        OK(vsr_init(&r, 400 + mode, 1, 2, 2));
        a = open_owner(&r, 1);
        survivor = open_owner(&r, 1);
        OK(vsr_reserve(&r, a, 1, &allocation));
        CHECK(vsr_import(&r, survivor, 1, &l) == VSR_NOT_READY);
        CHECK(vsr_complete(&r, allocation, 2) == VSR_INVALID);
        if (mode != 0) allocate(&b, allocation);
        if (mode >= 2) {
            if (mode == 4) OK(vsr_revoke(&r, a, 1));
            else OK(vsr_close(&r, a));
            CHECK(vsr_next_cleanup(&r, r.scope, &cleanup) == VSR_EMPTY);
            CHECK(vsr_import(&r, survivor, 1, &l) == VSR_STALE);
            CHECK(vsr_reserve(&r, survivor, 2, &next) == VSR_CAPACITY);
        }
        CHECK(vsr_complete(&r, allocation, mode == 3 ? 1 : 0) ==
              (mode >= 2 ? VSR_CLOSED : VSR_BACKEND_FAILED));
        CHECK(vsr_complete(&r, allocation, 1) == VSR_STALE);
        CHECK(vsr_import(&r, survivor, 1, &l) == VSR_STALE);
        OK(vsr_next_cleanup(&r, r.scope, &cleanup));
        /* 注入释放失败：不确认，重复取出同一 action，容量保持占用。 */
        OK(vsr_next_cleanup(&r, r.scope, &repeated));
        CHECK(cleanup.backing.generation == repeated.backing.generation);
        CHECK(vsr_reserve(&r, survivor, 2, &next) == VSR_CAPACITY);
        CHECK(vsr_quiescent(&r, r.scope) == VSR_EMPTY);
        invariants(&r);
        if (mode == 0) {
            CHECK(!b.generation[cleanup.backing.slot]);
            OK(vsr_cleanup_complete(&r, cleanup));
        } else clean(&r, &b);
        CHECK(vsr_reserve(&r, survivor, 1, &next) == VSR_STALE);
        next = create(&r, &b, survivor, 2);
        CHECK(next.backing.generation != allocation.backing.generation);
        CHECK(vsr_cleanup_complete(&r, cleanup) == VSR_STALE);
        if (mode < 2 || mode == 4) OK(vsr_close(&r, a));
        OK(vsr_close(&r, survivor));
        clean(&r, &b);
        CHECK(b.allocations == b.releases);
        OK(vsr_quiescent(&r, r.scope));
    }
    puts("PASS allocation/partial allocation failures, cancel-before-completion, release retry");
}

static void capacity_and_overflow(void)
{
    vsr_registry r = {0}, invalid = {0}, before;
    cpu_backend b = {0};
    vsr_actor owners[VSR_MAX_OWNERS], a;
    vsr_lease leases[VSR_MAX_LEASES], l;
    vsr_action allocation;
    uint32_t i;
    CHECK(vsr_init(&invalid, 1, 0, 1, 1) == VSR_INVALID);
    CHECK(vsr_init(&invalid, 1, VSR_MAX_SLOTS + 1, 1, 1) == VSR_INVALID);
    CHECK(vsr_init(&invalid, 1, 1, VSR_MAX_OWNERS + 1, 1) == VSR_INVALID);
    CHECK(vsr_init(&invalid, 1, 1, 1, VSR_MAX_LEASES + 1) == VSR_INVALID);
    CHECK(vsr_init(&invalid, 0, 1, 1, 1) == VSR_INVALID);
    OK(vsr_init(&r, 500, VSR_MAX_SLOTS, VSR_MAX_OWNERS, VSR_MAX_LEASES));
    CHECK(vsr_init(&r, 501, 1, 1, 1) == VSR_INVALID);
    for (i = 0; i < VSR_MAX_OWNERS; ++i) owners[i] = open_owner(&r, 1);
    CHECK(vsr_open(&r, r.scope, 1, &a) == VSR_CAPACITY);
    for (i = 0; i < VSR_MAX_SLOTS; ++i)
        (void)create(&r, &b, owners[0], (uint64_t)i + 1);
    memcpy(&before, &r, sizeof(r));
    CHECK(vsr_reserve(&r, owners[0], VSR_MAX_SLOTS + 1, &allocation) == VSR_CAPACITY);
    CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    for (i = 0; i < VSR_MAX_LEASES; ++i)
        OK(vsr_import(&r, owners[1], 1, &leases[i]));
    memcpy(&before, &r, sizeof(r));
    CHECK(vsr_import(&r, owners[1], 1, &l) == VSR_CAPACITY);
    CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    OK(vsr_release(&r, owners[1], leases[0]));
    OK(vsr_import(&r, owners[1], 1, &l));
    CHECK(l.handle != leases[0].handle);
    for (i = 0; i < VSR_MAX_OWNERS; ++i) OK(vsr_close(&r, owners[i]));
    invariants(&r);
    clean(&r, &b);
    OK(vsr_quiescent(&r, r.scope));
    a = open_owner(&r, 1);
    CHECK(a.owner != owners[0].owner);
    CHECK(vsr_close(&r, owners[0]) == VSR_CLOSED);
    (void)create(&r, &b, a, UINT64_MAX);
    CHECK(vsr_reserve(&r, a, 0, &allocation) == VSR_INVALID);
    /* 白盒故障注入仅在测试侧；生产代码无测试替换/条件路径。 */
    r.slots[0].lease_refs = UINT32_MAX;
    CHECK(vsr_import(&r, a, UINT64_MAX, &l) == VSR_OVERFLOW);
    r.slots[0].lease_refs = 0;
    r.next_token = UINT64_MAX - 1;
    OK(vsr_import(&r, a, UINT64_MAX, &l));
    CHECK(l.handle == UINT64_MAX);
    CHECK(vsr_import(&r, a, UINT64_MAX, &leases[0]) == VSR_OVERFLOW);
    CHECK(vsr_open(&r, r.scope, 2, &owners[0]) == VSR_OVERFLOW);
    OK(vsr_revoke(&r, a, UINT64_MAX));
    OK(vsr_release(&r, a, l));
    clean(&r, &b);
    CHECK(vsr_reserve(&r, a, UINT64_MAX, &allocation) == VSR_STALE);
    OK(vsr_close(&r, a));
    OK(vsr_quiescent(&r, r.scope));
    {
        vsr_registry exhausted = {0};
        OK(vsr_init(&exhausted, 501, 1, 1, 1));
        a = open_owner(&exhausted, 1);
        exhausted.next_token = UINT64_MAX;
        memcpy(&before, &exhausted, sizeof(exhausted));
        CHECK(vsr_reserve(&exhausted, a, 1, &allocation) == VSR_OVERFLOW);
        CHECK(memcmp(&before, &exhausted, sizeof(exhausted)) == 0);
        OK(vsr_close(&exhausted, a));
    }
    puts("PASS exact capacity, output/state rollback, token/refcount overflow fail-closed");
}

static void out_of_order_and_replay(void)
{
    static const uint32_t orders[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    unsigned order;
    for (order = 0; order < 6; ++order) {
        vsr_registry r = {0}, before;
        cpu_backend b = {0};
        vsr_actor a, reader;
        vsr_action allocations[3], pending;
        vsr_lease leases[3], unavailable;
        vsr_backing backing;
        uint32_t i, j;
        OK(vsr_init(&r, 700 + order, 3, 2, 3));
        a = open_owner(&r, 1);
        reader = open_owner(&r, 1);
        for (i = 0; i < 3; ++i) {
            OK(vsr_reserve(&r, a, (uint64_t)i + 1, &allocations[i]));
            allocate(&b, allocations[i]);
        }
        for (i = 0; i < 3; ++i) {
            uint32_t index = orders[order][i];
            OK(vsr_complete(&r, allocations[index], 1));
            OK(vsr_import(&r, reader, allocations[index].surface_id, &leases[index]));
            b.pixels[allocations[index].backing.slot] = 100u + index;
            memcpy(&before, &r, sizeof(r));
            CHECK(vsr_complete(&r, allocations[index], 0) == VSR_STALE);
            CHECK(memcmp(&before, &r, sizeof(r)) == 0);
            CHECK(vsr_complete(&r, allocations[index], 1) == VSR_STALE);
            CHECK(memcmp(&before, &r, sizeof(r)) == 0);
            for (j = 0; j <= i; ++j) {
                uint32_t done = orders[order][j];
                OK(vsr_resolve(&r, reader, leases[done], &backing));
                CHECK(backing.slot == allocations[done].backing.slot);
                CHECK(backing.generation == allocations[done].backing.generation);
                CHECK(b.pixels[backing.slot] == 100u + done);
            }
            for (j = i + 1; j < 3; ++j)
                CHECK(vsr_import(&r, reader, allocations[orders[order][j]].surface_id,
                                 &unavailable) == VSR_NOT_READY);
            CHECK(vsr_next_cleanup(&r, r.scope, &pending) == VSR_EMPTY);
            invariants(&r);
        }
        OK(vsr_close(&r, a));
        memcpy(&before, &r, sizeof(r));
        for (i = 0; i < 3; ++i) {
            CHECK(vsr_complete(&r, allocations[i], 0) == VSR_STALE);
            CHECK(vsr_complete(&r, allocations[i], 1) == VSR_STALE);
            CHECK(memcmp(&before, &r, sizeof(r)) == 0);
            OK(vsr_resolve(&r, reader, leases[i], &backing));
            CHECK(backing.generation == allocations[i].backing.generation);
        }
        OK(vsr_close(&r, reader));
        clean(&r, &b);
        OK(vsr_quiescent(&r, r.scope));
        CHECK(b.allocations == 3 && b.releases == 3);
    }
    puts("PASS all six completion orders, ready=0/1 replay preserves READY/RETIRED leases");
}

static void mixed_close_and_blocked_cleanup(void)
{
    vsr_registry r = {0}, before;
    cpu_backend b = {0};
    vsr_actor owner, survivor;
    vsr_action pending, own_ready, shared, foreign, blocked, retry, next;
    vsr_lease own_lease, shared_lease, foreign_lease, live_lease, next_lease;
    vsr_backing backing;
    OK(vsr_init(&r, 710, 5, 2, 6));
    owner = open_owner(&r, 1);
    survivor = open_owner(&r, 1);
    OK(vsr_reserve(&r, owner, 1, &pending));
    allocate(&b, pending);
    own_ready = create(&r, &b, owner, 2);
    shared = create(&r, &b, owner, 3);
    foreign = create(&r, &b, survivor, 4);
    OK(vsr_import(&r, owner, 2, &own_lease));
    OK(vsr_import(&r, owner, 3, &shared_lease));
    OK(vsr_import(&r, owner, 4, &foreign_lease));
    OK(vsr_import(&r, survivor, 3, &live_lease));
    OK(vsr_close(&r, owner));
    CHECK(r.slots[pending.backing.slot].state == VSR_SLOT_CANCELLED);
    CHECK(r.slots[own_ready.backing.slot].state == VSR_SLOT_DRAINING);
    CHECK(r.slots[shared.backing.slot].state == VSR_SLOT_RETIRED);
    CHECK(r.slots[shared.backing.slot].lease_refs == 1);
    CHECK(r.slots[foreign.backing.slot].state == VSR_SLOT_READY);
    CHECK(r.slots[foreign.backing.slot].lease_refs == 0);
    CHECK(vsr_release(&r, owner, own_lease) == VSR_CLOSED);
    CHECK(vsr_release(&r, survivor, shared_lease) == VSR_STALE);
    CHECK(vsr_release(&r, survivor, foreign_lease) == VSR_STALE);
    OK(vsr_resolve(&r, survivor, live_lease, &backing));
    CHECK(backing.generation == shared.backing.generation);
    invariants(&r);
    OK(vsr_next_cleanup(&r, r.scope, &blocked));
    CHECK(blocked.backing.slot == own_ready.backing.slot);
    /* 不确认一个槽的清理，其他槽仍可分配、完成、导入、释放和撤销。 */
    next = create(&r, &b, survivor, 5);
    CHECK(next.backing.slot != blocked.backing.slot);
    OK(vsr_import(&r, survivor, 5, &next_lease));
    OK(vsr_resolve(&r, survivor, next_lease, &backing));
    CHECK(backing.generation == next.backing.generation);
    OK(vsr_revoke(&r, survivor, 5));
    OK(vsr_release(&r, survivor, next_lease));
    memcpy(&before, &r, sizeof(r));
    OK(vsr_next_cleanup(&r, r.scope, &retry));
    CHECK(retry.kind == blocked.kind && retry.surface_id == blocked.surface_id);
    CHECK(retry.backing.scope == blocked.backing.scope);
    CHECK(retry.backing.slot == blocked.backing.slot);
    CHECK(retry.backing.generation == blocked.backing.generation);
    CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    CHECK(b.generation[blocked.backing.slot] == blocked.backing.generation);
    CHECK(b.releases == 0);
    CHECK(vsr_complete(&r, pending, 1) == VSR_CLOSED);
    clean(&r, &b);
    CHECK(b.releases == 3);
    OK(vsr_resolve(&r, survivor, live_lease, &backing));
    CHECK(backing.generation == shared.backing.generation);
    next = create(&r, &b, survivor, 6);
    CHECK(next.backing.slot == pending.backing.slot);
    invariants(&r);
    OK(vsr_close(&r, survivor));
    clean(&r, &b);
    OK(vsr_quiescent(&r, r.scope));
    CHECK(b.allocations == 6 && b.releases == 6);
    puts("PASS mixed RESERVED/READY/self/foreign leases close, blocked cleanup isolation");
}

static void action_tampering_and_early_cleanup(void)
{
    vsr_registry r = {0}, before;
    cpu_backend b = {0};
    vsr_actor a;
    vsr_action allocation, other, cleanup, changed, next;
    unsigned phase, variant;
    OK(vsr_init(&r, 720, 2, 1, 1));
    a = open_owner(&r, 1);
    OK(vsr_reserve(&r, a, 1, &allocation));
    allocate(&b, allocation);
    OK(vsr_reserve(&r, a, 2, &other));
    allocate(&b, other);
    cleanup = allocation;
    cleanup.kind = VSR_RELEASE;
    /* RESERVED 与 CANCELLED 都不能提前确认清理。 */
    for (phase = 0; phase < 3; ++phase) {
        memcpy(&before, &r, sizeof(r));
        if (phase < 2) {
            CHECK(vsr_cleanup_complete(&r, cleanup) == VSR_NOT_READY);
            CHECK(memcmp(&before, &r, sizeof(r)) == 0);
        }
        for (variant = 0; variant < 7; ++variant) {
            vsr_status expected = VSR_STALE;
            changed = allocation;
            switch (variant) {
            case 0: changed.backing.generation = other.backing.generation; break;
            case 1: changed.surface_id = other.surface_id; break;
            case 2: changed.backing.slot = other.backing.slot; break;
            case 3: changed.backing.generation = 0; expected = VSR_INVALID; break;
            case 4: changed.surface_id = 0; expected = VSR_INVALID; break;
            case 5: changed.backing.slot = r.slot_capacity; expected = VSR_INVALID; break;
            default: ++changed.backing.scope; expected = VSR_SCOPE; break;
            }
            CHECK(vsr_complete(&r, changed, 0) == expected);
            CHECK(memcmp(&before, &r, sizeof(r)) == 0);
            CHECK(vsr_complete(&r, changed, 1) == expected);
            CHECK(memcmp(&before, &r, sizeof(r)) == 0);
            changed.kind = VSR_RELEASE;
            CHECK(vsr_cleanup_complete(&r, changed) == expected);
            CHECK(memcmp(&before, &r, sizeof(r)) == 0);
        }
        if (phase == 0) OK(vsr_revoke(&r, a, 1));
        if (phase == 1) CHECK(vsr_complete(&r, allocation, 0) == VSR_CLOSED);
        invariants(&r);
    }
    clean(&r, &b);
    next = create(&r, &b, a, 3);
    CHECK(next.backing.slot == allocation.backing.slot);
    CHECK(next.backing.generation != allocation.backing.generation);
    memcpy(&before, &r, sizeof(r));
    CHECK(vsr_cleanup_complete(&r, cleanup) == VSR_STALE);
    CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    OK(vsr_revoke(&r, a, 3));
    memcpy(&before, &r, sizeof(r));
    CHECK(vsr_cleanup_complete(&r, cleanup) == VSR_STALE);
    CHECK(memcmp(&before, &r, sizeof(r)) == 0);
    CHECK(b.generation[next.backing.slot] == next.backing.generation);
    OK(vsr_complete(&r, other, 1));
    OK(vsr_close(&r, a));
    clean(&r, &b);
    OK(vsr_quiescent(&r, r.scope));
    CHECK(b.allocations == 3 && b.releases == 3);
    puts("PASS early cleanup refusal, tampered action fields, old cleanup after reuse");
}

static void failed_output_canaries(void)
{
    vsr_registry r = {0}, before;
    cpu_backend b = {0};
    vsr_actor a, reader, wrong, actor_out;
    vsr_action allocation, action_out;
    vsr_backing backing_out;
    vsr_lease lease, bad_lease;
    /* 同时验证整个输出对象（含填充字节）与 registry 在失败时保持原样。 */
#define CANARY(call, expected, output) do { \
    unsigned char saved[sizeof(output)]; \
    memset(&(output), 0xa5, sizeof(output)); \
    memcpy(saved, &(output), sizeof(output)); \
    memcpy(&before, &r, sizeof(r)); \
    CHECK((call) == (expected)); \
    CHECK(memcmp(saved, &(output), sizeof(output)) == 0); \
    CHECK(memcmp(&before, &r, sizeof(r)) == 0); \
} while (0)
    OK(vsr_init(&r, 730, 1, 2, 1));
    a = open_owner(&r, 1);
    reader = open_owner(&r, 1);
    CANARY(vsr_open(&r, 0, 1, &actor_out), VSR_INVALID, actor_out);
    CANARY(vsr_open(&r, r.scope + 1, 1, &actor_out), VSR_SCOPE, actor_out);
    CANARY(vsr_open(&r, r.scope, 0, &actor_out), VSR_INVALID, actor_out);
    CANARY(vsr_open(&r, r.scope, 1, &actor_out), VSR_CAPACITY, actor_out);
    CANARY(vsr_reserve(&r, a, 0, &action_out), VSR_INVALID, action_out);
    wrong = a;
    ++wrong.scope;
    CANARY(vsr_reserve(&r, wrong, 1, &action_out), VSR_SCOPE, action_out);
    wrong = a;
    ++wrong.identity;
    CANARY(vsr_reserve(&r, wrong, 1, &action_out), VSR_DENIED, action_out);
    CANARY(vsr_next_cleanup(&r, r.scope, &action_out), VSR_EMPTY, action_out);
    CANARY(vsr_next_cleanup(&r, 0, &action_out), VSR_INVALID, action_out);
    CANARY(vsr_next_cleanup(&r, r.scope + 1, &action_out), VSR_SCOPE, action_out);
    allocation = create(&r, &b, a, 2);
    CANARY(vsr_reserve(&r, a, 2, &action_out), VSR_DUPLICATE, action_out);
    CANARY(vsr_reserve(&r, a, 1, &action_out), VSR_STALE, action_out);
    CANARY(vsr_reserve(&r, a, 3, &action_out), VSR_CAPACITY, action_out);
    OK(vsr_import(&r, reader, 2, &lease));
    CANARY(vsr_resolve(&r, a, lease, &backing_out), VSR_DENIED, backing_out);
    wrong = reader;
    ++wrong.scope;
    CANARY(vsr_resolve(&r, wrong, lease, &backing_out), VSR_SCOPE, backing_out);
    wrong = reader;
    ++wrong.identity;
    CANARY(vsr_resolve(&r, wrong, lease, &backing_out), VSR_DENIED, backing_out);
    bad_lease = lease;
    ++bad_lease.scope;
    CANARY(vsr_resolve(&r, reader, bad_lease, &backing_out), VSR_SCOPE, backing_out);
    bad_lease = lease;
    bad_lease.handle = 0;
    CANARY(vsr_resolve(&r, reader, bad_lease, &backing_out), VSR_INVALID, backing_out);
    OK(vsr_release(&r, reader, lease));
    CANARY(vsr_resolve(&r, reader, lease, &backing_out), VSR_STALE, backing_out);
    OK(vsr_close(&r, reader));
    CANARY(vsr_resolve(&r, reader, lease, &backing_out), VSR_CLOSED, backing_out);
    CANARY(vsr_reserve(&r, reader, 3, &action_out), VSR_CLOSED, action_out);
    OK(vsr_revoke(&r, a, allocation.surface_id));
    clean(&r, &b);
    r.next_token = UINT64_MAX;
    CANARY(vsr_open(&r, r.scope, 1, &actor_out), VSR_OVERFLOW, actor_out);
    CANARY(vsr_reserve(&r, a, 3, &action_out), VSR_OVERFLOW, action_out);
    OK(vsr_close(&r, a));
    OK(vsr_quiescent(&r, r.scope));
#undef CANARY
    puts("PASS open/reserve/resolve/next_cleanup failure output and registry canaries");
}

static void repeated_lifetimes(void)
{
    vsr_registry r = {0};
    cpu_backend b = {0};
    uint64_t id;
    vsr_lease previous = {0, 0};
    OK(vsr_init(&r, 600, 2, 2, 2));
    for (id = 1; id <= 10000; ++id) {
        vsr_actor a = open_owner(&r, 1), c = open_owner(&r, 1);
        vsr_lease l;
        vsr_backing out;
        (void)create(&r, &b, a, id);
        OK(vsr_import(&r, c, id, &l));
        if (previous.handle) CHECK(vsr_resolve(&r, c, previous, &out) == VSR_STALE);
        previous = l;
        invariants(&r);
        if (id & 1u) { OK(vsr_close(&r, a)); OK(vsr_close(&r, c)); }
        else { OK(vsr_close(&r, c)); OK(vsr_close(&r, a)); }
        invariants(&r);
        clean(&r, &b);
        OK(vsr_quiescent(&r, r.scope));
    }
    CHECK(b.allocations == 10000 && b.releases == 10000);
    puts("PASS 10000 production-core lifetimes with reference/owner invariants");
}

int main(void)
{
    shared_backing_and_orders();
    creator_disconnect();
    creator_reference();
    isolation();
    faults();
    capacity_and_overflow();
    out_of_order_and_replay();
    mixed_close_and_blocked_cleanup();
    action_tampering_and_early_cleanup();
    failed_output_canaries();
    repeated_lifetimes();
    printf("ALL PASS: %u checks\n", checks);
    return 0;
}
