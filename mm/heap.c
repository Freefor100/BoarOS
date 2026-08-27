#include <kernel/heap.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_HEAP_INITIALIZED UINT32_C(0x48454150)
#define KERNEL_SLAB_MAGIC UINT64_C(0x424f4152534c4142)
#define KERNEL_SLAB_BITMAP_WORDS 4U
#define KERNEL_SLAB_INDEX_NONE UINT32_MAX
#define KERNEL_HEAP_BITS_PER_BYTE 8U

struct kernel_slab {
    uint64_t magic;
    struct kernel_heap *heap;
    struct kernel_slab *next;
    struct kernel_slab *previous;
    uint64_t allocated[KERNEL_SLAB_BITMAP_WORDS];
    uint32_t free_head;
    uint16_t class_index;
    uint16_t slot_count;
    uint16_t free_count;
    uint16_t slot_offset;
    uint32_t reserved;
};

_Static_assert(KERNEL_HEAP_ALIGNMENT != 0U &&
                   (KERNEL_HEAP_ALIGNMENT & (KERNEL_HEAP_ALIGNMENT - 1U)) ==
                       0U,
               "kernel heap alignment must be a power of two");
_Static_assert(KERNEL_HEAP_MIN_CLASS_SHIFT >= 4U,
               "kernel heap classes must provide 16-byte alignment");
_Static_assert(KERNEL_HEAP_SIZE_CLASS_COUNT > 0U,
               "a page must contain at least one small size class");
_Static_assert(sizeof(struct kernel_slab) % KERNEL_HEAP_ALIGNMENT == 0U,
               "slab header must preserve slot alignment");

static int heap_initialized(const struct kernel_heap *heap)
{
    return heap != 0 && heap->initialized == KERNEL_HEAP_INITIALIZED &&
           heap->page_allocator != 0 && heap->physical_address != 0;
}

static enum kernel_heap_status page_status_to_heap(
    enum physical_page_status status)
{
    switch (status) {
    case PHYSICAL_PAGE_STATUS_OK:
        return KERNEL_HEAP_STATUS_OK;
    case PHYSICAL_PAGE_STATUS_EMPTY:
        return KERNEL_HEAP_STATUS_EMPTY;
    case PHYSICAL_PAGE_STATUS_DOUBLE_FREE:
        return KERNEL_HEAP_STATUS_DOUBLE_FREE;
    case PHYSICAL_PAGE_STATUS_STATE:
        return KERNEL_HEAP_STATUS_STATE;
    case PHYSICAL_PAGE_STATUS_INVALID:
    default:
        return KERNEL_HEAP_STATUS_INVALID;
    }
}

static void bytes_zero(void *pointer, size_t size)
{
    unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < size; index++) {
        bytes[index] = 0U;
    }
}

static void bytes_copy(void *destination,
                       const void *source,
                       size_t size)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    size_t index;

    for (index = 0U; index < size; index++) {
        output[index] = input[index];
    }
}

static size_t class_size(uint32_t class_index)
{
    return (size_t)1U << (KERNEL_HEAP_MIN_CLASS_SHIFT + class_index);
}

static int small_class_for_size(size_t size, uint32_t *class_index)
{
    size_t capacity = (size_t)1U << KERNEL_HEAP_MIN_CLASS_SHIFT;
    uint32_t index = 0U;

    while (capacity < size && index + 1U < KERNEL_HEAP_SIZE_CLASS_COUNT) {
        capacity <<= 1U;
        index++;
    }
    if (size > capacity) {
        return 0;
    }

    *class_index = index;
    return 1;
}

static void statistics_add_pages(struct kernel_heap *heap, uint32_t order)
{
    uint64_t pages = UINT64_C(1) << order;

    heap->statistics.current_pages += pages;
    if (heap->statistics.current_pages > heap->statistics.peak_pages) {
        heap->statistics.peak_pages = heap->statistics.current_pages;
    }
}

static void statistics_remove_pages(struct kernel_heap *heap, uint32_t order)
{
    uint64_t pages = UINT64_C(1) << order;

    if (pages <= heap->statistics.current_pages) {
        heap->statistics.current_pages -= pages;
    } else {
        heap->statistics.current_pages = 0U;
    }
}

static void partial_insert(struct kernel_heap *heap,
                           struct kernel_slab *slab)
{
    uint32_t class_index = slab->class_index;
    struct kernel_slab *head = heap->partial_slabs[class_index];

    slab->previous = 0;
    slab->next = head;
    if (head != 0) {
        head->previous = slab;
    }
    heap->partial_slabs[class_index] = slab;
}

static void partial_remove(struct kernel_heap *heap,
                           struct kernel_slab *slab)
{
    uint32_t class_index = slab->class_index;

    if (slab->previous != 0) {
        slab->previous->next = slab->next;
    } else {
        heap->partial_slabs[class_index] = slab->next;
    }
    if (slab->next != 0) {
        slab->next->previous = slab->previous;
    }
    slab->next = 0;
    slab->previous = 0;
}

static unsigned char *slab_slot(struct kernel_slab *slab,
                                uint32_t slot_index)
{
    return (unsigned char *)slab + slab->slot_offset +
           class_size(slab->class_index) * slot_index;
}

static uint32_t slot_next(const struct kernel_slab *slab,
                          uint32_t slot_index)
{
    const unsigned char *slot =
        (const unsigned char *)slab + slab->slot_offset +
        class_size(slab->class_index) * slot_index;
    uint32_t result = 0U;
    uint32_t index;

    for (index = 0U; index < sizeof(result); index++) {
        result |= (uint32_t)slot[index] <<
                  (index * KERNEL_HEAP_BITS_PER_BYTE);
    }
    return result;
}

static void slot_set_next(struct kernel_slab *slab,
                          uint32_t slot_index,
                          uint32_t next)
{
    unsigned char *slot = slab_slot(slab, slot_index);
    uint32_t index;

    for (index = 0U; index < sizeof(next); index++) {
        slot[index] = (unsigned char)(
            next >> (index * KERNEL_HEAP_BITS_PER_BYTE));
    }
}

static int bitmap_test(const struct kernel_slab *slab,
                       uint32_t slot_index)
{
    uint32_t word = slot_index / 64U;
    uint32_t bit = slot_index % 64U;

    return (slab->allocated[word] & (UINT64_C(1) << bit)) != 0U;
}

static void bitmap_set(struct kernel_slab *slab, uint32_t slot_index)
{
    uint32_t word = slot_index / 64U;
    uint32_t bit = slot_index % 64U;

    slab->allocated[word] |= UINT64_C(1) << bit;
}

static void bitmap_clear(struct kernel_slab *slab, uint32_t slot_index)
{
    uint32_t word = slot_index / 64U;
    uint32_t bit = slot_index % 64U;

    slab->allocated[word] &= ~(UINT64_C(1) << bit);
}

static enum kernel_heap_status slab_create(struct kernel_heap *heap,
                                           uint32_t class_index,
                                           struct kernel_slab **slab_out)
{
    uint64_t physical_address;
    void *pointer;
    struct kernel_slab *slab;
    uint32_t slot_count;
    uint32_t index;
    enum physical_page_status page_status;

    page_status = physical_page_allocate_order(heap->page_allocator,
                                                0U,
                                                &physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status_to_heap(page_status);
    }
    page_status = physical_page_resolve(heap->page_allocator,
                                        physical_address,
                                        &pointer);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        (void)physical_page_release_order(heap->page_allocator,
                                          physical_address,
                                          0U);
        return page_status_to_heap(page_status);
    }

    slab = pointer;
    slot_count = (uint32_t)((BOAROS_PAGE_SIZE - sizeof(*slab)) /
                            class_size(class_index));
    if (slot_count == 0U ||
        slot_count > KERNEL_SLAB_BITMAP_WORDS * 64U) {
        (void)physical_page_release_order(heap->page_allocator,
                                          physical_address,
                                          0U);
        return KERNEL_HEAP_STATUS_STATE;
    }

    slab->magic = KERNEL_SLAB_MAGIC;
    slab->heap = heap;
    slab->next = 0;
    slab->previous = 0;
    for (index = 0U; index < KERNEL_SLAB_BITMAP_WORDS; index++) {
        slab->allocated[index] = 0U;
    }
    slab->free_head = 0U;
    slab->class_index = (uint16_t)class_index;
    slab->slot_count = (uint16_t)slot_count;
    slab->free_count = (uint16_t)slot_count;
    slab->slot_offset = (uint16_t)sizeof(*slab);
    slab->reserved = 0U;
    for (index = 0U; index < slot_count; index++) {
        uint32_t next = index + 1U < slot_count ?
                            index + 1U : KERNEL_SLAB_INDEX_NONE;

        slot_set_next(slab, index, next);
    }
    partial_insert(heap, slab);
    statistics_add_pages(heap, 0U);
    *slab_out = slab;
    return KERNEL_HEAP_STATUS_OK;
}

static enum kernel_heap_status allocate_small(struct kernel_heap *heap,
                                              uint32_t class_index,
                                              void **pointer)
{
    struct kernel_slab *slab = heap->partial_slabs[class_index];
    uint32_t slot_index;
    enum kernel_heap_status status;

    if (slab == 0) {
        status = slab_create(heap, class_index, &slab);
        if (status != KERNEL_HEAP_STATUS_OK) {
            return status;
        }
    }
    if (slab->magic != KERNEL_SLAB_MAGIC || slab->heap != heap ||
        slab->class_index != class_index || slab->free_count == 0U ||
        slab->free_head >= slab->slot_count) {
        return KERNEL_HEAP_STATUS_STATE;
    }

    slot_index = slab->free_head;
    slab->free_head = slot_next(slab, slot_index);
    if (bitmap_test(slab, slot_index)) {
        return KERNEL_HEAP_STATUS_STATE;
    }
    bitmap_set(slab, slot_index);
    slab->free_count--;
    if (slab->free_count == 0U) {
        partial_remove(heap, slab);
    }

    *pointer = slab_slot(slab, slot_index);
    return KERNEL_HEAP_STATUS_OK;
}

static int allocation_order_for_size(size_t size, uint32_t *order)
{
    uint64_t pages;
    uint64_t capacity = 1U;
    uint32_t result = 0U;

    if (size == 0U || size > UINT64_MAX - BOAROS_PAGE_MASK) {
        return 0;
    }
    pages = ((uint64_t)size + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
    while (capacity < pages && result < PHYSICAL_PAGE_MAX_ORDER) {
        capacity <<= 1U;
        result++;
    }
    if (capacity < pages) {
        return 0;
    }

    *order = result;
    return 1;
}

static enum kernel_heap_status allocate_large(struct kernel_heap *heap,
                                              size_t size,
                                              void **pointer)
{
    uint32_t order;
    uint64_t physical_address;
    void *result = 0;
    enum physical_page_status page_status;

    if (!allocation_order_for_size(size, &order)) {
        return KERNEL_HEAP_STATUS_OVERFLOW;
    }
    page_status = physical_page_allocate_order(heap->page_allocator,
                                                order,
                                                &physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status_to_heap(page_status);
    }
    page_status = physical_page_resolve(heap->page_allocator,
                                        physical_address,
                                        &result);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        (void)physical_page_release_order(heap->page_allocator,
                                          physical_address,
                                          order);
        return page_status_to_heap(page_status);
    }

    statistics_add_pages(heap, order);
    *pointer = result;
    return KERNEL_HEAP_STATUS_OK;
}

enum kernel_heap_status kernel_heap_init(
    struct kernel_heap *heap,
    struct physical_page_allocator *page_allocator,
    kernel_heap_physical_address_fn physical_address)
{
    struct kernel_heap result;
    uint32_t index;

    if (heap == 0 || page_allocator == 0 || physical_address == 0) {
        return KERNEL_HEAP_STATUS_INVALID;
    }
    if (!physical_page_allocator_is_finalized(page_allocator)) {
        return KERNEL_HEAP_STATUS_STATE;
    }

    result.page_allocator = page_allocator;
    result.physical_address = physical_address;
    for (index = 0U; index < KERNEL_HEAP_SIZE_CLASS_COUNT; index++) {
        result.partial_slabs[index] = 0;
    }
    result.statistics.allocation_calls = 0U;
    result.statistics.allocation_failures = 0U;
    result.statistics.live_allocations = 0U;
    result.statistics.current_pages = 0U;
    result.statistics.peak_pages = 0U;
    result.initialized = KERNEL_HEAP_INITIALIZED;
    *heap = result;
    return KERNEL_HEAP_STATUS_OK;
}

enum kernel_heap_status kernel_heap_allocate(
    struct kernel_heap *heap,
    size_t size,
    void **pointer)
{
    uint32_t class_index;
    void *result = 0;
    enum kernel_heap_status status;

    if (!heap_initialized(heap) || pointer == 0) {
        return KERNEL_HEAP_STATUS_INVALID;
    }
    heap->statistics.allocation_calls++;
    if (size == 0U) {
        *pointer = 0;
        return KERNEL_HEAP_STATUS_OK;
    }

    if (small_class_for_size(size, &class_index)) {
        status = allocate_small(heap, class_index, &result);
    } else {
        status = allocate_large(heap, size, &result);
    }
    if (status != KERNEL_HEAP_STATUS_OK) {
        heap->statistics.allocation_failures++;
        return status;
    }

    heap->statistics.live_allocations++;
    *pointer = result;
    return KERNEL_HEAP_STATUS_OK;
}

enum kernel_heap_status kernel_heap_allocate_zeroed(
    struct kernel_heap *heap,
    size_t count,
    size_t size,
    void **pointer)
{
    size_t total;
    void *result;
    enum kernel_heap_status status;

    if (!heap_initialized(heap) || pointer == 0) {
        return KERNEL_HEAP_STATUS_INVALID;
    }
    if (size != 0U && count > SIZE_MAX / size) {
        heap->statistics.allocation_calls++;
        heap->statistics.allocation_failures++;
        return KERNEL_HEAP_STATUS_OVERFLOW;
    }
    total = count * size;
    status = kernel_heap_allocate(heap, total, &result);
    if (status != KERNEL_HEAP_STATUS_OK) {
        return status;
    }
    bytes_zero(result, total);
    *pointer = result;
    return KERNEL_HEAP_STATUS_OK;
}

static enum kernel_heap_status allocation_information(
    struct kernel_heap *heap,
    void *pointer,
    uint64_t *physical_address,
    uint32_t *order,
    size_t *capacity,
    struct kernel_slab **slab_out,
    uint32_t *slot_index_out)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t page_value = value & ~(uintptr_t)BOAROS_PAGE_MASK;
    uint64_t page_address;
    uint32_t page_order;
    enum physical_page_status page_status;

    if (!heap->physical_address((const void *)page_value, &page_address) ||
        (page_address & BOAROS_PAGE_MASK) != 0U) {
        return KERNEL_HEAP_STATUS_INVALID;
    }
    page_status = physical_page_allocation_order(heap->page_allocator,
                                                  page_address,
                                                  &page_order);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status_to_heap(page_status);
    }

    if ((value & BOAROS_PAGE_MASK) == 0U) {
        if (page_order > PHYSICAL_PAGE_MAX_ORDER ||
            page_order > (sizeof(size_t) * KERNEL_HEAP_BITS_PER_BYTE -
                          BOAROS_PAGE_SHIFT - 1U)) {
            return KERNEL_HEAP_STATUS_STATE;
        }
        *physical_address = page_address;
        *order = page_order;
        *capacity = (size_t)BOAROS_PAGE_SIZE << page_order;
        *slab_out = 0;
        *slot_index_out = 0U;
        return KERNEL_HEAP_STATUS_OK;
    }

    {
        struct kernel_slab *slab = (struct kernel_slab *)page_value;
        size_t slot_size;
        uintptr_t slot_start;
        uintptr_t slot_offset;
        uint32_t slot_index;

        if (page_order != 0U || slab->magic != KERNEL_SLAB_MAGIC ||
            slab->heap != heap ||
            slab->class_index >= KERNEL_HEAP_SIZE_CLASS_COUNT ||
            slab->slot_offset != sizeof(*slab) || slab->slot_count == 0U ||
            slab->slot_count > KERNEL_SLAB_BITMAP_WORDS * 64U) {
            return KERNEL_HEAP_STATUS_INVALID;
        }
        slot_size = class_size(slab->class_index);
        slot_start = page_value + slab->slot_offset;
        if (value < slot_start) {
            return KERNEL_HEAP_STATUS_INVALID;
        }
        slot_offset = value - slot_start;
        if (slot_offset % slot_size != 0U) {
            return KERNEL_HEAP_STATUS_INVALID;
        }
        slot_index = (uint32_t)(slot_offset / slot_size);
        if (slot_index >= slab->slot_count) {
            return KERNEL_HEAP_STATUS_INVALID;
        }
        if (!bitmap_test(slab, slot_index)) {
            return KERNEL_HEAP_STATUS_DOUBLE_FREE;
        }

        *physical_address = page_address;
        *order = 0U;
        *capacity = slot_size;
        *slab_out = slab;
        *slot_index_out = slot_index;
        return KERNEL_HEAP_STATUS_OK;
    }
}

enum kernel_heap_status kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer)
{
    uint64_t physical_address;
    uint32_t order;
    size_t capacity;
    struct kernel_slab *slab;
    uint32_t slot_index;
    enum kernel_heap_status status;
    enum physical_page_status page_status;

    if (!heap_initialized(heap)) {
        return KERNEL_HEAP_STATUS_INVALID;
    }
    if (pointer == 0) {
        return KERNEL_HEAP_STATUS_OK;
    }

    status = allocation_information(heap,
                                    pointer,
                                    &physical_address,
                                    &order,
                                    &capacity,
                                    &slab,
                                    &slot_index);
    (void)capacity;
    if (status != KERNEL_HEAP_STATUS_OK) {
        return status;
    }
    if (heap->statistics.live_allocations == 0U) {
        return KERNEL_HEAP_STATUS_STATE;
    }

    if (slab == 0) {
        page_status = physical_page_release_order(heap->page_allocator,
                                                  physical_address,
                                                  order);
        if (page_status != PHYSICAL_PAGE_STATUS_OK) {
            return page_status_to_heap(page_status);
        }
        statistics_remove_pages(heap, order);
    } else {
        int was_full = slab->free_count == 0U;
        uint32_t old_free_head = slab->free_head;
        uint16_t old_free_count = slab->free_count;

        bitmap_clear(slab, slot_index);
        slot_set_next(slab, slot_index, slab->free_head);
        slab->free_head = slot_index;
        slab->free_count++;
        if (was_full) {
            partial_insert(heap, slab);
        }
        if (slab->free_count == slab->slot_count) {
            partial_remove(heap, slab);
            slab->magic = 0U;
            page_status = physical_page_release_order(heap->page_allocator,
                                                      physical_address,
                                                      0U);
            if (page_status != PHYSICAL_PAGE_STATUS_OK) {
                slab->magic = KERNEL_SLAB_MAGIC;
                bitmap_set(slab, slot_index);
                slab->free_head = old_free_head;
                slab->free_count = old_free_count;
                if (!was_full) {
                    partial_insert(heap, slab);
                }
                return page_status_to_heap(page_status);
            }
            statistics_remove_pages(heap, 0U);
        }
    }

    heap->statistics.live_allocations--;
    return KERNEL_HEAP_STATUS_OK;
}

enum kernel_heap_status kernel_heap_resize(
    struct kernel_heap *heap,
    void *old_pointer,
    size_t new_size,
    void **new_pointer)
{
    uint64_t physical_address;
    uint32_t order;
    size_t capacity;
    struct kernel_slab *slab;
    uint32_t slot_index;
    void *result;
    enum kernel_heap_status status;

    if (!heap_initialized(heap) || new_pointer == 0) {
        return KERNEL_HEAP_STATUS_INVALID;
    }
    if (old_pointer == 0) {
        return kernel_heap_allocate(heap, new_size, new_pointer);
    }
    if (new_size == 0U) {
        status = kernel_heap_release(heap, old_pointer);
        if (status == KERNEL_HEAP_STATUS_OK) {
            *new_pointer = 0;
        }
        return status;
    }
    if (!allocation_order_for_size(new_size, &order)) {
        return KERNEL_HEAP_STATUS_OVERFLOW;
    }

    status = allocation_information(heap,
                                    old_pointer,
                                    &physical_address,
                                    &order,
                                    &capacity,
                                    &slab,
                                    &slot_index);
    (void)physical_address;
    (void)slab;
    (void)slot_index;
    if (status != KERNEL_HEAP_STATUS_OK) {
        return status;
    }
    if (new_size <= capacity) {
        *new_pointer = old_pointer;
        return KERNEL_HEAP_STATUS_OK;
    }

    status = kernel_heap_allocate(heap, new_size, &result);
    if (status != KERNEL_HEAP_STATUS_OK) {
        return status;
    }
    bytes_copy(result, old_pointer, capacity);
    status = kernel_heap_release(heap, old_pointer);
    if (status != KERNEL_HEAP_STATUS_OK) {
        (void)kernel_heap_release(heap, result);
        return status;
    }

    *new_pointer = result;
    return KERNEL_HEAP_STATUS_OK;
}

void kernel_heap_get_statistics(
    const struct kernel_heap *heap,
    struct kernel_heap_statistics *statistics)
{
    if (!heap_initialized(heap) || statistics == 0) {
        return;
    }

    *statistics = heap->statistics;
}
