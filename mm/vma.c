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
    uint64_t generation;
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
        vma->kind > KERNEL_VMA_KIND_ELF_PRIVATE ||
        vma->role < KERNEL_VMA_ROLE_NONE ||
        vma->role > KERNEL_VMA_ROLE_MMAP ||
        vma->fault_policy < KERNEL_VMA_FAULT_RESIDENT_REQUIRED ||
        vma->fault_policy > KERNEL_VMA_FAULT_ELF) {
        return 0;
    }
    if (vma->kind == KERNEL_VMA_KIND_ANONYMOUS &&
        (vma->backing_offset != 0U || vma->backing != 0 ||
         vma->fault_policy == KERNEL_VMA_FAULT_FILE_PRIVATE ||
         vma->fault_policy == KERNEL_VMA_FAULT_ELF)) {
        return 0;
    }
    if ((vma->kind == KERNEL_VMA_KIND_FILE_PRIVATE &&
        (vma->backing == 0 ||
         vma->fault_policy != KERNEL_VMA_FAULT_FILE_PRIVATE ||
         vma->backing_offset > UINT64_MAX - (vma->end - vma->start))) ||
        (vma->kind == KERNEL_VMA_KIND_ELF_PRIVATE &&
         (vma->backing == 0 ||
          vma->fault_policy != KERNEL_VMA_FAULT_ELF ||
          vma->backing_offset > UINT64_MAX - (vma->end - vma->start)))) {
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
    return left->backing_offset <= UINT64_MAX - left_size &&
           left->backing_offset + left_size == right->backing_offset;
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

static void move_entries(struct kernel_vma_set *set,
                         uint32_t destination,
                         uint32_t source,
                         uint32_t count)
{
    uint32_t index;

    if (count == 0U || destination == source) {
        return;
    }
    if (destination < source) {
        for (index = 0U; index < count; index++) {
            set->entries[destination + index] =
                set->entries[source + index];
        }
        return;
    }
    for (index = count; index > 0U; index--) {
        set->entries[destination + index - 1U] =
            set->entries[source + index - 1U];
    }
}

static enum kernel_vma_status insert_entry(struct kernel_vma_set *set,
                                            const struct kernel_vma *vma)
{
    struct kernel_vma merged;
    uint32_t index;
    enum kernel_vma_status status;

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
    move_entries(set, index + 1U, index, set->count - index);
    set->entries[index] = merged;
    set->count++;
    return KERNEL_VMA_STATUS_OK;
}

static enum kernel_vma_status split_at(struct kernel_vma_set *set,
                                        uint64_t address)
{
    struct kernel_vma right;
    uint64_t delta;
    uint32_t index;

    index = lower_bound(set, address + (address != UINT64_MAX ? 1U : 0U));
    if (index == 0U || address <= set->entries[index - 1U].start ||
        address >= set->entries[index - 1U].end) {
        return KERNEL_VMA_STATUS_OK;
    }
    if (set->count >= set->capacity) {
        return KERNEL_VMA_STATUS_STATE;
    }
    index--;
    right = set->entries[index];
    delta = address - right.start;
    if (right.kind != KERNEL_VMA_KIND_ANONYMOUS) {
        if (right.backing_offset > UINT64_MAX - delta) {
            return KERNEL_VMA_STATUS_STATE;
        }
        right.backing_offset += delta;
    }
    right.start = address;
    set->entries[index].end = address;
    move_entries(set,
                 index + 2U,
                 index + 1U,
                 set->count - index - 1U);
    set->entries[index + 1U] = right;
    set->count++;
    return KERNEL_VMA_STATUS_OK;
}

static void merge_all(struct kernel_vma_set *set)
{
    uint32_t index = 1U;

    while (index < set->count) {
        if (can_merge(&set->entries[index - 1U],
                      &set->entries[index])) {
            set->entries[index - 1U].end = set->entries[index].end;
            erase_entry(set, index);
        } else {
            index++;
        }
    }
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
    (void)kernel_vma_set_destroy(&working);
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
        (void)kernel_heap_release(working->heap, working->entries);
        working->entries = 0;
        working->count = 0U;
        working->capacity = 0U;
    }
    (void)kernel_heap_release(working->heap, working);
    *set = 0;
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_insert(
    struct kernel_vma_set *set,
    const struct kernel_vma *vma)
{
    enum kernel_vma_status status;

    if (!set_valid(set) || !vma_valid(vma)) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    if (set->generation == UINT64_MAX) {
        return KERNEL_VMA_STATUS_STATE;
    }
    status = insert_entry(set, vma);
    if (status == KERNEL_VMA_STATUS_OK) {
        set->generation++;
    }
    return status;
}

enum kernel_vma_status kernel_vma_set_overlaps(
    const struct kernel_vma_set *set,
    uint64_t start,
    uint64_t end,
    int *overlaps)
{
    uint32_t index;

    if (!set_valid(set) || start >= end || overlaps == 0) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    index = lower_bound(set, start);
    *overlaps = (index > 0U && set->entries[index - 1U].end > start) ||
                (index < set->count && set->entries[index].start < end);
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_backing_in_use(
    const struct kernel_vma_set *set,
    const void *backing,
    int *in_use)
{
    uint32_t index;

    if (!set_valid(set) || backing == 0 || in_use == 0) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    *in_use = 0;
    for (index = 0U; index < set->count; index++) {
        if (set->entries[index].backing == backing) {
            *in_use = 1;
            break;
        }
    }
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_find_topdown_gap(
    const struct kernel_vma_set *set,
    uint64_t hint,
    uint64_t lower,
    uint64_t upper,
    uint64_t length,
    uint64_t *address)
{
    uint64_t cursor;
    uint32_t index;
    int conflict;

    if (!set_valid(set) || lower >= upper || length == 0U ||
        length > upper - lower || address == 0) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    if (hint >= lower && hint <= upper - length &&
        kernel_vma_set_overlaps(set,
                                hint,
                                hint + length,
                                &conflict) == KERNEL_VMA_STATUS_OK &&
        conflict == 0) {
        *address = hint;
        return KERNEL_VMA_STATUS_OK;
    }

    cursor = upper;
    index = set->count;
    while (index > 0U) {
        const struct kernel_vma *entry = &set->entries[index - 1U];
        uint64_t gap_start;

        index--;
        if (entry->start >= upper) {
            continue;
        }
        if (entry->end <= lower) {
            break;
        }
        gap_start = entry->end > lower ? entry->end : lower;
        if (cursor > gap_start && cursor - gap_start >= length) {
            *address = cursor - length;
            return KERNEL_VMA_STATUS_OK;
        }
        if (entry->start < cursor) {
            cursor = entry->start;
        }
    }
    if (cursor >= lower && cursor - lower >= length) {
        *address = cursor - length;
        return KERNEL_VMA_STATUS_OK;
    }
    return KERNEL_VMA_STATUS_NOT_FOUND;
}

static int split_needed(const struct kernel_vma_set *set,
                        uint64_t address)
{
    uint32_t index = lower_bound(
        set,
        address + (address != UINT64_MAX ? 1U : 0U));

    return index > 0U && address > set->entries[index - 1U].start &&
           address < set->entries[index - 1U].end;
}

static enum kernel_vma_status reserve_edit_capacity(
    struct kernel_vma_set *set,
    uint64_t start,
    uint64_t end,
    int replacement)
{
    uint32_t required = set->count;
    uint32_t extra = (uint32_t)split_needed(set, start) +
                     (uint32_t)split_needed(set, end);
    int overlaps;

    if (required > UINT32_MAX - extra) {
        return KERNEL_VMA_STATUS_NO_MEMORY;
    }
    required += extra;
    if (replacement != 0) {
        if (kernel_vma_set_overlaps(set, start, end, &overlaps) !=
            KERNEL_VMA_STATUS_OK) {
            return KERNEL_VMA_STATUS_STATE;
        }
        if (overlaps == 0) {
            if (set->count == UINT32_MAX) {
                return KERNEL_VMA_STATUS_NO_MEMORY;
            }
            if (required < set->count + 1U) {
                required = set->count + 1U;
            }
        }
    }
    return reserve_entries(set, required);
}

enum kernel_vma_status kernel_vma_set_prepare_replace(
    struct kernel_vma_set *set,
    uint64_t start,
    uint64_t end,
    const struct kernel_vma *replacement,
    struct kernel_vma_edit *edit)
{
    enum kernel_vma_status status;

    if (!set_valid(set) || start >= end || edit == 0 ||
        (replacement != 0 &&
         (!vma_valid(replacement) || replacement->start != start ||
          replacement->end != end))) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    if (set->generation == UINT64_MAX) {
        return KERNEL_VMA_STATUS_STATE;
    }
    status = reserve_edit_capacity(set,
                                   start,
                                   end,
                                   replacement != 0);
    if (status != KERNEL_VMA_STATUS_OK) {
        return status;
    }
    *edit = (struct kernel_vma_edit){
        .set = set,
        .generation = set->generation,
        .start = start,
        .end = end,
        .kind = replacement == 0 ? KERNEL_VMA_EDIT_REMOVE
                                 : KERNEL_VMA_EDIT_REPLACE,
    };
    if (replacement != 0) {
        edit->replacement = *replacement;
    }
    return KERNEL_VMA_STATUS_OK;
}

static int range_is_covered(const struct kernel_vma_set *set,
                            uint64_t start,
                            uint64_t end)
{
    uint64_t cursor = start;
    uint32_t index = lower_bound(set, start);

    if (index > 0U && set->entries[index - 1U].end > start) {
        index--;
    }
    while (index < set->count && cursor < end) {
        if (set->entries[index].start > cursor ||
            set->entries[index].end <= cursor) {
            return 0;
        }
        cursor = set->entries[index].end;
        index++;
    }
    return cursor >= end;
}

enum kernel_vma_status kernel_vma_set_prepare_protect(
    struct kernel_vma_set *set,
    uint64_t start,
    uint64_t end,
    uint32_t permissions,
    struct kernel_vma_edit *edit)
{
    enum kernel_vma_status status;

    if (!set_valid(set) || start >= end || edit == 0) {
        return KERNEL_VMA_STATUS_INVALID_ARGUMENT;
    }
    if (!range_is_covered(set, start, end)) {
        return KERNEL_VMA_STATUS_NOT_FOUND;
    }
    if (set->generation == UINT64_MAX) {
        return KERNEL_VMA_STATUS_STATE;
    }
    status = reserve_edit_capacity(set, start, end, 0);
    if (status != KERNEL_VMA_STATUS_OK) {
        return status;
    }
    *edit = (struct kernel_vma_edit){
        .set = set,
        .generation = set->generation,
        .start = start,
        .end = end,
        .permissions = permissions,
        .kind = KERNEL_VMA_EDIT_PROTECT,
    };
    return KERNEL_VMA_STATUS_OK;
}

enum kernel_vma_status kernel_vma_set_commit_edit(
    struct kernel_vma_set *set,
    const struct kernel_vma_edit *edit)
{
    uint32_t first;
    uint32_t last;
    uint32_t index;
    enum kernel_vma_status status;

    if (!set_valid(set) || edit == 0 || edit->set != set ||
        edit->generation != set->generation ||
        edit->start >= edit->end ||
        edit->kind < KERNEL_VMA_EDIT_REMOVE ||
        edit->kind > KERNEL_VMA_EDIT_PROTECT) {
        return KERNEL_VMA_STATUS_STATE;
    }
    if (edit->kind == KERNEL_VMA_EDIT_REPLACE &&
        (!vma_valid(&edit->replacement) ||
         edit->replacement.start != edit->start ||
         edit->replacement.end != edit->end)) {
        return KERNEL_VMA_STATUS_STATE;
    }
    status = split_at(set, edit->start);
    if (status != KERNEL_VMA_STATUS_OK) {
        return KERNEL_VMA_STATUS_STATE;
    }
    status = split_at(set, edit->end);
    if (status != KERNEL_VMA_STATUS_OK) {
        return KERNEL_VMA_STATUS_STATE;
    }
    first = lower_bound(set, edit->start);
    last = lower_bound(set, edit->end);
    if (edit->kind == KERNEL_VMA_EDIT_PROTECT) {
        for (index = first; index < last; index++) {
            set->entries[index].permissions = edit->permissions;
        }
    } else {
        move_entries(set,
                     first,
                     last,
                     set->count - last);
        set->count -= last - first;
        if (edit->kind == KERNEL_VMA_EDIT_REPLACE) {
            status = insert_entry(set, &edit->replacement);
            if (status != KERNEL_VMA_STATUS_OK) {
                return KERNEL_VMA_STATUS_STATE;
            }
        }
    }
    merge_all(set);
    set->generation++;
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
    if (set->generation == UINT64_MAX) {
        return KERNEL_VMA_STATUS_STATE;
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
    set->generation++;
    return KERNEL_VMA_STATUS_OK;
}
