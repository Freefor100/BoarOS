#include <kernel/heap.h>
#include <kernel/vma.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_VMA_INITIAL_CAPACITY 8U

struct kernel_vma_set {
    struct kernel_heap *heap;
    struct kernel_vma *entries;
    uint32_t count;
    uint32_t capacity;
};

static int set_valid(const struct kernel_vma_set *set)
{
    return set != 0 && set->heap != 0 &&
           ((set->entries == 0 && set->count == 0U &&
             set->capacity == 0U) ||
            (set->entries != 0 && set->capacity != 0U &&
             set->count <= set->capacity));
}

static int vma_valid(const struct kernel_vma *vma)
{
    if (vma == 0 || vma->start >= vma->end ||
        vma->kind < KERNEL_VMA_KIND_ANONYMOUS ||
        vma->kind > KERNEL_VMA_KIND_FILE_PRIVATE ||
        vma->role < KERNEL_VMA_ROLE_NONE ||
        vma->role > KERNEL_VMA_ROLE_MMAP ||
        vma->fault_policy < KERNEL_VMA_FAULT_RESIDENT_REQUIRED ||
        vma->fault_policy > KERNEL_VMA_FAULT_DEMAND_ZERO) {
        return 0;
    }
    if (vma->kind == KERNEL_VMA_KIND_ANONYMOUS &&
        (vma->file_offset != 0U || vma->backing != 0)) {
        return 0;
    }
    return 1;
}

static int can_merge(const struct kernel_vma *left,
                     const struct kernel_vma *right)
{
    uint64_t left_size;

    if (left->end != right->start ||
        left->permissions != right->permissions ||
        left->kind != right->kind || left->role != right->role ||
        left->fault_policy != right->fault_policy ||
        left->backing != right->backing) {
        return 0;
    }
    if (left->kind == KERNEL_VMA_KIND_ANONYMOUS) {
        return 1;
    }
    left_size = left->end - left->start;
    return left->file_offset <= UINT64_MAX - left_size &&
           left->file_offset + left_size == right->file_offset;
}

static uint32_t lower_bound(const struct kernel_vma_set *set,
                            uint64_t start)
{
    uint32_t low = 0U;
    uint32_t high = set->count;

    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;

        if (set->entries[middle].start < start) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

static enum kernel_vma_status reserve_entries(struct kernel_vma_set *set,
                                               uint32_t required)
{
    struct kernel_vma *resized;
    uint32_t capacity;
    enum kernel_heap_status status;

    if (required <= set->capacity) {
        return KERNEL_VMA_STATUS_OK;
    }
    capacity = set->capacity == 0U ? KERNEL_VMA_INITIAL_CAPACITY
                                   : set->capacity;
    while (capacity < required) {
        if (capacity > UINT32_MAX / 2U) {
            return KERNEL_VMA_STATUS_NO_MEMORY;
        }
        capacity *= 2U;
    }
    status = kernel_heap_resize(set->heap,
                                set->entries,
                                (size_t)capacity * sizeof(*resized),
                                (void **)&resized);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        return KERNEL_VMA_STATUS_NO_MEMORY;
    }
    if (status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_VMA_STATUS_STATE;
    }
    set->entries = resized;
    set->capacity = capacity;
    return KERNEL_VMA_STATUS_OK;
}

static void erase_entry(struct kernel_vma_set *set, uint32_t index)
{
    uint32_t current;

    for (current = index; current + 1U < set->count; current++) {
        set->entries[current] = set->entries[current + 1U];
    }
    set->count--;
}

enum kernel_vma_status kernel_vma_set_create(
    struct kernel_heap *heap,
    struct kernel_vma_set **set)
{
    struct kernel_vma_set *result;
    enum kernel_heap_status status;

    if (heap == 0 || set == 0 || *set != 0) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_heap_allocate_zeroed(heap,
                                         1U,
                                         sizeof(*result),
                                         (void **)&result);
    if (status == KERNEL_HEAP_STATUS_EMPTY) {
        return KERNEL_VMA_STATUS_NO_MEMORY;
    }
    if (status != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_VMA_STATUS_STATE;
    }
    result->heap = heap;
    *set = result;
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_clone(
    const struct kernel_vma_set *source,
    struct kernel_vma_set **destination)
{
    struct kernel_vma_set *working = 0;
    enum kernel_vma_status status;
    uint32_t index;

    if (!set_valid(source) || destination == 0 || *destination != 0) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_vma_set_create(source->heap, &working);
    if (status != KERNEL_VMA_STATUS_OK) {
        return status;
    }
    status = reserve_entries(working, source->count);
    if (status == KERNEL_VMA_STATUS_OK) {
        for (index = 0U; index < source->count; index++) {
            working->entries[index] = source->entries[index];
        }
        working->count = source->count;
        *destination = working;
        return KERNEL_VMA_STATUS_OK;
    }
    if (kernel_vma_set_destroy(&working) != KERNEL_VMA_STATUS_OK) {
        *destination = working;
        return KERNEL_VMA_STATUS_CLEANUP_REQUIRED;
    }
    return status;
}

enum kernel_vma_status kernel_vma_set_destroy(struct kernel_vma_set **set)
{
    struct kernel_vma_set *working;

    if (set == 0 || !set_valid(*set)) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    working = *set;
    if (working->entries != 0) {
        if (kernel_heap_release(working->heap, working->entries) !=
            KERNEL_HEAP_STATUS_OK) {
            return KERNEL_VMA_STATUS_CLEANUP_REQUIRED;
        }
        working->entries = 0;
        working->count = 0U;
        working->capacity = 0U;
    }
    if (kernel_heap_release(working->heap, working) !=
        KERNEL_HEAP_STATUS_OK) {
        return KERNEL_VMA_STATUS_CLEANUP_REQUIRED;
    }
    *set = 0;
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_insert(
    struct kernel_vma_set *set,
    const struct kernel_vma *vma)
{
    struct kernel_vma merged;
    uint32_t index;
    enum kernel_vma_status status;

    if (!set_valid(set) || !vma_valid(vma)) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    index = lower_bound(set, vma->start);
    if ((index > 0U && set->entries[index - 1U].end > vma->start) ||
        (index < set->count && set->entries[index].start < vma->end)) {
        return KERNEL_VMA_STATUS_CONFLICT;
    }
    merged = *vma;
    if (index > 0U && can_merge(&set->entries[index - 1U], &merged)) {
        set->entries[index - 1U].end = merged.end;
        if (index < set->count &&
            can_merge(&set->entries[index - 1U], &set->entries[index])) {
            set->entries[index - 1U].end = set->entries[index].end;
            erase_entry(set, index);
        }
        return KERNEL_VMA_STATUS_OK;
    }
    if (index < set->count && can_merge(&merged, &set->entries[index])) {
        merged.end = set->entries[index].end;
        set->entries[index] = merged;
        return KERNEL_VMA_STATUS_OK;
    }
    if (set->count == UINT32_MAX) {
        return KERNEL_VMA_STATUS_NO_MEMORY;
    }
    status = reserve_entries(set, set->count + 1U);
    if (status != KERNEL_VMA_STATUS_OK) {
        return status;
    }
    {
        uint32_t current;

        for (current = set->count; current > index; current--) {
            set->entries[current] = set->entries[current - 1U];
        }
    }
    set->entries[index] = merged;
    set->count++;
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_lookup(
    const struct kernel_vma_set *set,
    uint64_t virtual_address,
    struct kernel_vma *vma)
{
    uint32_t index;

    if (!set_valid(set) || vma == 0) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    index = lower_bound(set, virtual_address +
                        (virtual_address != UINT64_MAX ? 1U : 0U));
    if (index == 0U ||
        virtual_address >= set->entries[index - 1U].end) {
        return KERNEL_VMA_STATUS_NOT_FOUND;
    }
    *vma = set->entries[index - 1U];
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_trim_end(
    struct kernel_vma_set *set,
    uint64_t start,
    uint64_t old_end,
    uint64_t new_end)
{
    uint32_t index;

    if (!set_valid(set) || start >= old_end || new_end < start ||
        new_end >= old_end) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    index = lower_bound(set, start);
    if (index >= set->count || set->entries[index].start != start ||
        set->entries[index].end != old_end) {
        return KERNEL_VMA_STATUS_NOT_FOUND;
    }
    if (new_end == start) {
        erase_entry(set, index);
    } else {
        set->entries[index].end = new_end;
    }
    return KERNEL_VMA_STATUS_OK;
}
