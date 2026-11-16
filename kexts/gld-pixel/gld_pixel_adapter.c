#include "gld_pixel_adapter.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == sizeof(mach_vm_address_t), "64-bit Mach addresses required");

typedef struct gpa_owner {
    gp_pixel_list list;
    uint64_t generation;
    struct gpa_owner *next;
} gpa_owner;

typedef struct gpa_registry {
    gpa_owner *owners;
    gp_format_cap *capabilities;
    size_t capability_count;
    uint64_t last_generation;
} gpa_registry;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static gpa_registry independent_registry;
static gpa_registry system_registry;

static int32_t configure(gpa_registry *registry, const gp_format_cap *formats,
                         size_t count, int single_driver)
{
    gp_format_cap *copy = NULL;
    if (count != 0 && formats == NULL) return GP_BAD_ARGUMENT;
    if (count > (size_t)PTRDIFF_MAX / sizeof(*formats)) return GP_OVERFLOW;
    if (count != 0 && (uintptr_t)formats > UINTPTR_MAX - count * sizeof(*formats))
        return GP_OVERFLOW;
    if (pthread_mutex_lock(&registry_lock) != 0) return GPA_LOCK_FAILED;
    if (registry->owners != NULL) {
        (void)pthread_mutex_unlock(&registry_lock);
        return GPA_BUSY;
    }
    if (count != 0) {
        copy = malloc(count * sizeof(*copy));
        if (copy == NULL) {
            (void)pthread_mutex_unlock(&registry_lock);
            return GP_NO_MEMORY;
        }
        memcpy(copy, formats, count * sizeof(*copy));
        if (single_driver) {
            uint32_t driver = copy[0].renderer_id & UINT32_C(0x00ffff00);
            for (size_t i = 1; i < count; ++i) {
                if ((copy[i].renderer_id & UINT32_C(0x00ffff00)) != driver) {
                    free(copy);
                    (void)pthread_mutex_unlock(&registry_lock);
                    return GPA_DRIVER_MISMATCH;
                }
            }
        }
    }
    free(registry->capabilities);
    registry->capabilities = copy;
    registry->capability_count = count;
    (void)pthread_mutex_unlock(&registry_lock);
    return GP_OK;
}

int32_t gpa_configure(const gp_format_cap *formats, size_t count)
{
    return configure(&independent_registry, formats, count, 0);
}

int32_t gpa_system_configure(const gp_format_cap *formats, size_t count)
{
    return configure(&system_registry, formats, count, 1);
}

static int32_t copy_tokens(const uint32_t *source, uint32_t *local, size_t *bytes)
{
    uintptr_t address = (uintptr_t)source;
    int need_value = 0;
    for (size_t i = 0; i < GPA_MAX_WORDS; ++i) {
        mach_vm_size_t copied = 0;
        if (address > UINTPTR_MAX - sizeof(uint32_t)) return GP_OVERFLOW;
        kern_return_t kr = mach_vm_read_overwrite(mach_task_self(),
            (mach_vm_address_t)address, sizeof(uint32_t),
            (mach_vm_address_t)(uintptr_t)&local[i], &copied);
        if (kr != KERN_SUCCESS || copied != sizeof(uint32_t)) return GPA_READ_FAILED;
        *bytes = (i + 1) * sizeof(uint32_t);
        if (need_value) {
            need_value = 0;
            if (local[i] > INT32_MAX) break;
        } else {
            switch (local[i]) {
            case 0: return GP_OK;
            case 4: case 5: break;
            case 8: case 11: case 12: case 13: need_value = 1; break;
            default: return GP_OK; /* 仅已复制前缀交给parser报告未知项。 */
            }
        }
        if (local[i] > INT32_MAX) break;
        if (i + 1 == GPA_MAX_WORDS) return GPA_WORD_LIMIT;
        address += sizeof(uint32_t);
    }
    return GP_OK; /* 已复制的负值交给parser报告。 */
}

static int32_t choose(gpa_registry *registry, void **out_head,
                      const uint32_t *tokens, uint64_t *out_generation)
{
    uint32_t local[GPA_MAX_WORDS];
    size_t bytes = 0;
    gp_request request;
    int32_t status;
    gpa_owner *owner;
    if (out_head != NULL) *out_head = NULL;
    if (out_generation != NULL) *out_generation = 0;
    if (out_head == NULL || out_generation == NULL || tokens == NULL)
        return GP_BAD_ARGUMENT;
    status = copy_tokens(tokens, local, &bytes);
    if (status != GP_OK) return status;
    status = (int32_t)gp_parse_normalized(local, bytes, &request);
    if (status != GP_OK) return status;
    if (pthread_mutex_lock(&registry_lock) != 0) return GPA_LOCK_FAILED;
    if (registry->last_generation == UINT64_MAX) {
        status = GPA_GENERATION_EXHAUSTED;
        goto done;
    }
    owner = calloc(1, sizeof(*owner));
    if (owner == NULL) {
        status = GP_NO_MEMORY;
        goto done;
    }
    status = (int32_t)gp_choose_normalized(local, bytes, registry->capabilities,
                                         registry->capability_count, NULL, &owner->list);
    if (status != GP_OK) {
        free(owner);
        goto done;
    }
    owner->generation = ++registry->last_generation;
    owner->next = registry->owners;
    registry->owners = owner;
    *out_head = owner->list.head;
    *out_generation = owner->generation;
done:
    (void)pthread_mutex_unlock(&registry_lock);
    return status;
}

int32_t gpa_choose(void **out_head, const uint32_t *tokens,
                   uint64_t *out_generation)
{
    return choose(&independent_registry, out_head, tokens, out_generation);
}

int32_t gpa_system_choose(void **out_head, const uint32_t *tokens)
{
    uint64_t generation;
    return choose(&system_registry, out_head, tokens, &generation);
}

static int32_t destroy(gpa_registry *registry, void *head, uint64_t generation,
                       int current)
{
    gpa_owner **link;
    gpa_owner *owner;
    if (head == NULL || (!current && generation == 0)) return GPA_INVALID_HANDLE;
    if (pthread_mutex_lock(&registry_lock) != 0) return GPA_LOCK_FAILED;
    for (link = &registry->owners; *link != NULL; link = &(*link)->next) {
        if ((*link)->list.head == head &&
            (current || (*link)->generation == generation)) break;
    }
    owner = *link;
    if (owner == NULL) {
        (void)pthread_mutex_unlock(&registry_lock);
        return GPA_INVALID_HANDLE;
    }
    /* current 查到的就是此锁范围内仍注册的代次；不解锁后再按head/generation销毁。 */
    *link = owner->next;
    gp_pixel_list_destroy(&owner->list);
    free(owner);
    (void)pthread_mutex_unlock(&registry_lock);
    return GP_OK;
}

int32_t gpa_destroy(void *head, uint64_t generation)
{
    return destroy(&independent_registry, head, generation, 0);
}

int32_t gpa_system_destroy_current(void *head)
{
    if (head == NULL) return GP_OK;
    return destroy(&system_registry, head, 0, 1);
}
