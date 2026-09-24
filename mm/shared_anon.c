#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/shared_anon.h>

#include <stddef.h>
#include <stdint.h>

#define SHARED_ANON_INITIAL_BUCKETS 16U

struct kernel_shared_anon_page {
    struct kernel_shared_anon_page *next;
    uint64_t index;
    uint64_t physical_address;
};

struct kernel_shared_anon {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_shared_anon_page **buckets;
    size_t bucket_count;
    size_t page_count;
    uint32_t references;
};

static size_t bucket_for(uint64_t index, size_t bucket_count)
{
    index ^= index >> 33U;
    index *= UINT64_C(0xff51afd7ed558ccd);
    index ^= index >> 33U;
    return (size_t)index & (bucket_count - 1U);
}

static enum kernel_shared_anon_status grow_buckets(
    struct kernel_shared_anon *object)
{
    struct kernel_shared_anon_page **buckets;
    size_t capacity;
    enum kernel_heap_status status;

    if (object->page_count / 2U < object->bucket_count)
        return KERNEL_SHARED_ANON_OK;
    if (object->bucket_count > SIZE_MAX / 2U / sizeof(*buckets))
        return KERNEL_SHARED_ANON_NO_MEMORY;
    capacity = object->bucket_count * 2U;
    status = kernel_heap_allocate_zeroed(object->heap, capacity,
                                         sizeof(*buckets), (void **)&buckets);
    if (status == KERNEL_HEAP_STATUS_EMPTY)
        return KERNEL_SHARED_ANON_NO_MEMORY;
    if (status != KERNEL_HEAP_STATUS_OK)
        return KERNEL_SHARED_ANON_STATE;
    for (size_t bucket = 0U; bucket < object->bucket_count; bucket++) {
        struct kernel_shared_anon_page *page = object->buckets[bucket];

        while (page != 0) {
            struct kernel_shared_anon_page *next = page->next;
            size_t target = bucket_for(page->index, capacity);

            page->next = buckets[target];
            buckets[target] = page;
            page = next;
        }
    }
    if (kernel_heap_release(object->heap, object->buckets) !=
        KERNEL_HEAP_STATUS_OK) __builtin_trap();
    object->buckets = buckets;
    object->bucket_count = capacity;
    return KERNEL_SHARED_ANON_OK;
}

enum kernel_shared_anon_status kernel_shared_anon_create(
    struct kernel_heap *heap, struct physical_page_allocator *allocator,
    struct kernel_shared_anon **object)
{
    struct kernel_shared_anon *result;
    enum kernel_heap_status status;

    if (heap == 0 || allocator == 0 || object == 0 || *object != 0)
        return KERNEL_SHARED_ANON_STATE;
    status = kernel_heap_allocate_zeroed(heap, 1U, sizeof(*result),
                                         (void **)&result);
    if (status == KERNEL_HEAP_STATUS_EMPTY)
        return KERNEL_SHARED_ANON_NO_MEMORY;
    if (status != KERNEL_HEAP_STATUS_OK)
        return KERNEL_SHARED_ANON_STATE;
    status = kernel_heap_allocate_zeroed(heap, SHARED_ANON_INITIAL_BUCKETS,
                                         sizeof(*result->buckets),
                                         (void **)&result->buckets);
    if (status != KERNEL_HEAP_STATUS_OK) {
        if (kernel_heap_release(heap, result) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_SHARED_ANON_NO_MEMORY
                   : KERNEL_SHARED_ANON_STATE;
    }
    result->heap = heap;
    result->allocator = allocator;
    result->bucket_count = SHARED_ANON_INITIAL_BUCKETS;
    result->references = 1U;
    *object = result;
    return KERNEL_SHARED_ANON_OK;
}

enum kernel_shared_anon_status kernel_shared_anon_acquire(
    struct kernel_shared_anon *object)
{
    if (object == 0 || object->references == 0U ||
        object->references == UINT32_MAX)
        return KERNEL_SHARED_ANON_STATE;
    object->references++;
    return KERNEL_SHARED_ANON_OK;
}

void kernel_shared_anon_release(struct kernel_shared_anon **object)
{
    struct kernel_shared_anon *owner;

    if (object == 0 || *object == 0 || (*object)->references == 0U)
        __builtin_trap();
    owner = *object;
    *object = 0;
    if (--owner->references != 0U) return;
    for (size_t bucket = 0U; bucket < owner->bucket_count; bucket++) {
        struct kernel_shared_anon_page *page = owner->buckets[bucket];

        while (page != 0) {
            struct kernel_shared_anon_page *next = page->next;

            if (physical_page_release(owner->allocator,
                                      page->physical_address) !=
                PHYSICAL_PAGE_STATUS_OK ||
                kernel_heap_release(owner->heap, page) !=
                KERNEL_HEAP_STATUS_OK) __builtin_trap();
            page = next;
        }
    }
    if (kernel_heap_release(owner->heap, owner->buckets) !=
            KERNEL_HEAP_STATUS_OK ||
        kernel_heap_release(owner->heap, owner) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

enum kernel_shared_anon_status kernel_shared_anon_get_page(
    struct kernel_shared_anon *object, uint64_t page_index,
    uint64_t *physical_address, int *created)
{
    struct kernel_shared_anon_page *page;
    enum kernel_heap_status heap_status;
    enum physical_page_status page_status;
    enum kernel_shared_anon_status status;
    size_t bucket;
    void *bytes;

    if (object == 0 || physical_address == 0 || created == 0 ||
        object->references == 0U) return KERNEL_SHARED_ANON_STATE;
    bucket = bucket_for(page_index, object->bucket_count);
    for (page = object->buckets[bucket]; page != 0; page = page->next) {
        if (page->index != page_index) continue;
        if (physical_page_acquire(object->allocator,
                                  page->physical_address) !=
            PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        *physical_address = page->physical_address;
        *created = 0;
        return KERNEL_SHARED_ANON_OK;
    }
    status = grow_buckets(object);
    if (status != KERNEL_SHARED_ANON_OK) return status;
    heap_status = kernel_heap_allocate_zeroed(object->heap, 1U,
                                               sizeof(*page), (void **)&page);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY)
        return KERNEL_SHARED_ANON_NO_MEMORY;
    if (heap_status != KERNEL_HEAP_STATUS_OK)
        return KERNEL_SHARED_ANON_STATE;
    page_status = physical_page_allocate(object->allocator,
                                         &page->physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        if (kernel_heap_release(object->heap, page) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_SHARED_ANON_NO_MEMORY
                   : KERNEL_SHARED_ANON_STATE;
    }
    if (physical_page_resolve(object->allocator, page->physical_address,
                              &bytes) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    for (size_t i = 0U; i < BOAROS_PAGE_SIZE; i++)
        ((unsigned char *)bytes)[i] = 0U;
    if (physical_page_acquire(object->allocator, page->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    page->index = page_index;
    bucket = bucket_for(page_index, object->bucket_count);
    page->next = object->buckets[bucket];
    object->buckets[bucket] = page;
    object->page_count++;
    *physical_address = page->physical_address;
    *created = 1;
    return KERNEL_SHARED_ANON_OK;
}

void kernel_shared_anon_discard_new_page(
    struct kernel_shared_anon *object, uint64_t page_index,
    uint64_t physical_address)
{
    struct kernel_shared_anon_page **link;
    uint32_t references;

    if (object == 0 || object->references == 0U) __builtin_trap();
    link = &object->buckets[bucket_for(page_index, object->bucket_count)];
    while (*link != 0 && (*link)->index != page_index)
        link = &(*link)->next;
    if (*link == 0 || (*link)->physical_address != physical_address ||
        physical_page_reference_count(object->allocator, physical_address,
                                      &references) != PHYSICAL_PAGE_STATUS_OK ||
        references != 1U) __builtin_trap();
    struct kernel_shared_anon_page *page = *link;
    *link = page->next;
    object->page_count--;
    if (physical_page_release(object->allocator, physical_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_heap_release(object->heap, page) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}
