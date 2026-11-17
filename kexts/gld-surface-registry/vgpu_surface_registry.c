#include "vgpu_surface_registry.h"

static vsr_status scope_check(const vsr_registry *r, uint64_t scope)
{
    if (!r || !r->scope || !scope) return VSR_INVALID;
    return r->scope == scope ? VSR_OK : VSR_SCOPE;
}

static vsr_status actor_check(const vsr_registry *r, vsr_actor a)
{
    uint32_t i;
    vsr_status rc = scope_check(r, a.scope);
    if (rc != VSR_OK) return rc;
    if (!a.owner || !a.identity) return VSR_INVALID;
    for (i = 0; i < r->owner_capacity; ++i) {
        if (r->owners[i].token == a.owner)
            return r->owners[i].identity == a.identity ? VSR_OK : VSR_DENIED;
    }
    return VSR_CLOSED;
}

static vsr_slot_entry *surface_find(vsr_registry *r, uint64_t id)
{
    uint32_t i;
    for (i = 0; i < r->slot_capacity; ++i)
        if (r->slots[i].state != VSR_SLOT_FREE && r->slots[i].surface_id == id)
            return &r->slots[i];
    return 0;
}

static vsr_action action_make(const vsr_registry *r, uint32_t slot,
                              vsr_action_kind kind)
{
    vsr_action a;
    a.kind = kind;
    a.surface_id = r->slots[slot].surface_id;
    a.backing.scope = r->scope;
    a.backing.generation = r->slots[slot].generation;
    a.backing.slot = slot;
    return a;
}

static vsr_status action_check(vsr_registry *r, vsr_action a,
                               vsr_action_kind kind)
{
    vsr_slot_entry *s;
    vsr_status rc = scope_check(r, a.backing.scope);
    if (rc != VSR_OK) return rc;
    if (a.kind != kind || a.backing.slot >= r->slot_capacity ||
        !a.backing.generation || !a.surface_id) return VSR_INVALID;
    s = &r->slots[a.backing.slot];
    if (s->state == VSR_SLOT_FREE || s->generation != a.backing.generation ||
        s->surface_id != a.surface_id) return VSR_STALE;
    return VSR_OK;
}

static void creator_drop(vsr_slot_entry *s)
{
    s->creator_ref = 0;
    if (s->state == VSR_SLOT_RESERVED) s->state = VSR_SLOT_CANCELLED;
    else if (s->state == VSR_SLOT_READY)
        s->state = s->lease_refs ? VSR_SLOT_RETIRED : VSR_SLOT_DRAINING;
}

static void lease_drop(vsr_registry *r, vsr_lease_entry *l)
{
    vsr_slot_entry *s = &r->slots[l->slot];
    --s->lease_refs;
    l->token = 0;
    if (!s->lease_refs && !s->creator_ref) s->state = VSR_SLOT_DRAINING;
}

static vsr_status lease_find(vsr_registry *r, vsr_actor actor,
                             vsr_lease lease, vsr_lease_entry **out)
{
    uint32_t i;
    vsr_status rc = actor_check(r, actor);
    if (rc != VSR_OK) return rc;
    if (lease.scope != r->scope) return VSR_SCOPE;
    if (!lease.handle) return VSR_INVALID;
    for (i = 0; i < r->lease_capacity; ++i) {
        vsr_lease_entry *l = &r->leases[i];
        if (l->token == lease.handle) {
            if (l->owner != actor.owner) return VSR_DENIED;
            *out = l;
            return VSR_OK;
        }
    }
    return VSR_STALE;
}

vsr_status vsr_init(vsr_registry *r, uint64_t scope, uint32_t slots,
                    uint32_t owners, uint32_t leases)
{
    uint32_t i;
    if (!r || r->scope || !scope || !slots || slots > VSR_MAX_SLOTS ||
        !owners || owners > VSR_MAX_OWNERS || !leases || leases > VSR_MAX_LEASES)
        return VSR_INVALID;
    r->scope = scope;
    r->next_token = 0;
    r->high_surface_id = 0;
    r->slot_capacity = slots;
    r->owner_capacity = owners;
    r->lease_capacity = leases;
    for (i = 0; i < VSR_MAX_OWNERS; ++i) {
        r->owners[i].token = 0;
        r->owners[i].identity = 0;
    }
    for (i = 0; i < VSR_MAX_SLOTS; ++i) {
        r->slots[i].surface_id = 0;
        r->slots[i].generation = 0;
        r->slots[i].creator_owner = 0;
        r->slots[i].creator_identity = 0;
        r->slots[i].creator_ref = 0;
        r->slots[i].lease_refs = 0;
        r->slots[i].state = VSR_SLOT_FREE;
    }
    for (i = 0; i < VSR_MAX_LEASES; ++i) {
        r->leases[i].token = 0;
        r->leases[i].owner = 0;
        r->leases[i].generation = 0;
        r->leases[i].slot = 0;
    }
    return VSR_OK;
}

vsr_status vsr_open(vsr_registry *r, uint64_t scope, uint64_t identity,
                    vsr_actor *out)
{
    uint32_t i;
    vsr_status rc = scope_check(r, scope);
    if (rc != VSR_OK) return rc;
    if (!identity || !out) return VSR_INVALID;
    if (r->next_token == UINT64_MAX) return VSR_OVERFLOW;
    for (i = 0; i < r->owner_capacity; ++i) {
        if (!r->owners[i].token) {
            r->owners[i].token = ++r->next_token;
            r->owners[i].identity = identity;
            out->scope = scope;
            out->identity = identity;
            out->owner = r->owners[i].token;
            return VSR_OK;
        }
    }
    return VSR_CAPACITY;
}

vsr_status vsr_close(vsr_registry *r, vsr_actor actor)
{
    uint32_t i;
    vsr_status rc = actor_check(r, actor);
    if (rc != VSR_OK) return rc;
    for (i = 0; i < r->owner_capacity; ++i)
        if (r->owners[i].token == actor.owner) r->owners[i].token = 0;
    for (i = 0; i < r->slot_capacity; ++i)
        if (r->slots[i].creator_ref && r->slots[i].creator_owner == actor.owner)
            creator_drop(&r->slots[i]);
    for (i = 0; i < r->lease_capacity; ++i)
        if (r->leases[i].token && r->leases[i].owner == actor.owner)
            lease_drop(r, &r->leases[i]);
    return VSR_OK;
}

vsr_status vsr_reserve(vsr_registry *r, vsr_actor actor, uint64_t surface_id,
                       vsr_action *out)
{
    uint32_t i;
    vsr_slot_entry *s;
    vsr_status rc = actor_check(r, actor);
    if (rc != VSR_OK) return rc;
    if (!surface_id || !out) return VSR_INVALID;
    if (surface_find(r, surface_id)) return VSR_DUPLICATE;
    if (surface_id <= r->high_surface_id) return VSR_STALE;
    if (r->next_token == UINT64_MAX) return VSR_OVERFLOW;
    for (i = 0; i < r->slot_capacity; ++i) {
        s = &r->slots[i];
        if (s->state == VSR_SLOT_FREE) {
            s->surface_id = surface_id;
            s->generation = ++r->next_token;
            s->creator_owner = actor.owner;
            s->creator_identity = actor.identity;
            s->creator_ref = 1;
            s->lease_refs = 0;
            s->state = VSR_SLOT_RESERVED;
            r->high_surface_id = surface_id;
            *out = action_make(r, i, VSR_ALLOCATE);
            return VSR_OK;
        }
    }
    return VSR_CAPACITY;
}

vsr_status vsr_complete(vsr_registry *r, vsr_action allocation, int ready)
{
    vsr_slot_entry *s;
    vsr_status rc = action_check(r, allocation, VSR_ALLOCATE);
    if (rc != VSR_OK) return rc;
    if (ready != 0 && ready != 1) return VSR_INVALID;
    s = &r->slots[allocation.backing.slot];
    if (s->state == VSR_SLOT_CANCELLED) {
        s->state = VSR_SLOT_DRAINING;
        return VSR_CLOSED;
    }
    if (s->state != VSR_SLOT_RESERVED) return VSR_STALE;
    if (!ready) {
        s->creator_ref = 0;
        s->state = VSR_SLOT_DRAINING;
        return VSR_BACKEND_FAILED;
    }
    s->state = VSR_SLOT_READY;
    return VSR_OK;
}

vsr_status vsr_revoke(vsr_registry *r, vsr_actor actor, uint64_t surface_id)
{
    vsr_slot_entry *s;
    vsr_status rc = actor_check(r, actor);
    if (rc != VSR_OK) return rc;
    if (!surface_id) return VSR_INVALID;
    s = surface_find(r, surface_id);
    if (!s || !s->creator_ref) return VSR_STALE;
    if (s->creator_owner != actor.owner || s->creator_identity != actor.identity)
        return VSR_DENIED;
    creator_drop(s);
    return VSR_OK;
}

vsr_status vsr_import(vsr_registry *r, vsr_actor actor, uint64_t surface_id,
                      vsr_lease *out)
{
    uint32_t i, slot;
    vsr_slot_entry *s;
    vsr_status rc = actor_check(r, actor);
    if (rc != VSR_OK) return rc;
    if (!surface_id || !out) return VSR_INVALID;
    s = surface_find(r, surface_id);
    if (!s || !s->creator_ref) return VSR_STALE;
    if (s->creator_identity != actor.identity) return VSR_DENIED;
    if (s->state != VSR_SLOT_READY) return VSR_NOT_READY;
    if (r->next_token == UINT64_MAX || s->lease_refs == UINT32_MAX)
        return VSR_OVERFLOW;
    for (slot = 0; slot < r->slot_capacity; ++slot)
        if (&r->slots[slot] == s) break;
    for (i = 0; i < r->lease_capacity; ++i) {
        vsr_lease_entry *l = &r->leases[i];
        if (!l->token) {
            l->token = ++r->next_token;
            l->owner = actor.owner;
            l->slot = slot;
            l->generation = s->generation;
            ++s->lease_refs;
            out->scope = r->scope;
            out->handle = l->token;
            return VSR_OK;
        }
    }
    return VSR_CAPACITY;
}

vsr_status vsr_resolve(vsr_registry *r, vsr_actor actor, vsr_lease lease,
                       vsr_backing *out)
{
    vsr_lease_entry *l;
    vsr_status rc;
    if (!out) return VSR_INVALID;
    rc = lease_find(r, actor, lease, &l);
    if (rc != VSR_OK) return rc;
    out->scope = r->scope;
    out->slot = l->slot;
    out->generation = l->generation;
    return VSR_OK;
}

vsr_status vsr_release(vsr_registry *r, vsr_actor actor, vsr_lease lease)
{
    vsr_lease_entry *l;
    vsr_status rc = lease_find(r, actor, lease, &l);
    if (rc != VSR_OK) return rc;
    lease_drop(r, l);
    return VSR_OK;
}

vsr_status vsr_next_cleanup(vsr_registry *r, uint64_t scope, vsr_action *out)
{
    uint32_t i;
    vsr_status rc = scope_check(r, scope);
    if (rc != VSR_OK) return rc;
    if (!out) return VSR_INVALID;
    for (i = 0; i < r->slot_capacity; ++i) {
        if (r->slots[i].state == VSR_SLOT_DRAINING) {
            *out = action_make(r, i, VSR_RELEASE);
            return VSR_OK;
        }
    }
    return VSR_EMPTY;
}

vsr_status vsr_cleanup_complete(vsr_registry *r, vsr_action release)
{
    vsr_status rc = action_check(r, release, VSR_RELEASE);
    if (rc != VSR_OK) return rc;
    if (r->slots[release.backing.slot].state != VSR_SLOT_DRAINING)
        return VSR_NOT_READY;
    r->slots[release.backing.slot].state = VSR_SLOT_FREE;
    return VSR_OK;
}

vsr_status vsr_quiescent(vsr_registry *r, uint64_t scope)
{
    uint32_t i;
    vsr_status rc = scope_check(r, scope);
    if (rc != VSR_OK) return rc;
    for (i = 0; i < r->owner_capacity; ++i)
        if (r->owners[i].token) return VSR_EMPTY;
    for (i = 0; i < r->slot_capacity; ++i)
        if (r->slots[i].state != VSR_SLOT_FREE) return VSR_EMPTY;
    for (i = 0; i < r->lease_capacity; ++i)
        if (r->leases[i].token) return VSR_EMPTY;
    return VSR_OK;
}
