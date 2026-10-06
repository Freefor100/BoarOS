#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/memory_object.h>
#include <kernel/sync.h>
#include <arch/context.h>

#include <stddef.h>
#include <stdint.h>

#define SHARED_ANON_INITIAL_BUCKETS 16U

struct kernel_memory_object_page {
    struct kernel_memory_object_page *next;
    uint64_t index;
    uint64_t physical_address;
};

struct kernel_memory_object {
    struct kernel_mutex lock;
    struct kernel_memory_budget *budget;
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_memory_object_page **buckets;
    size_t bucket_count;
    size_t page_count;
    uint32_t references;
};

static void unaccount_page(struct kernel_memory_object *object)
{
    uintptr_t irq = arch_interrupt_save();
    if (!object->allocator->shared_anon_pages) __builtin_trap();
    object->allocator->shared_anon_pages--;
    if (object->budget) {
        if (!object->budget->used) __builtin_trap();
        object->budget->used--;
    }
    arch_interrupt_restore(irq);
}

static size_t bucket_for(uint64_t index, size_t bucket_count)
{
    index ^= index >> 33U;
    index *= UINT64_C(0xff51afd7ed558ccd);
    index ^= index >> 33U;
    return (size_t)index & (bucket_count - 1U);
}

static enum kernel_memory_object_status grow_buckets(
    struct kernel_memory_object *object)
{
    struct kernel_memory_object_page **buckets;
    size_t capacity;
    enum kernel_heap_status status;

    if (object->page_count / 2U < object->bucket_count)
        return KERNEL_MEMORY_OBJECT_OK;
    if (object->bucket_count > SIZE_MAX / 2U / sizeof(*buckets))
        return KERNEL_MEMORY_OBJECT_NO_MEMORY;
    capacity = object->bucket_count * 2U;
    status = kernel_heap_allocate_zeroed(object->heap, capacity,
                                         sizeof(*buckets), (void **)&buckets);
    if (status == KERNEL_HEAP_STATUS_EMPTY)
        return KERNEL_MEMORY_OBJECT_NO_MEMORY;
    if (status != KERNEL_HEAP_STATUS_OK)
        return KERNEL_MEMORY_OBJECT_STATE;
    for (size_t bucket = 0U; bucket < object->bucket_count; bucket++) {
        struct kernel_memory_object_page *page = object->buckets[bucket];

        while (page != 0) {
            struct kernel_memory_object_page *next = page->next;
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
    return KERNEL_MEMORY_OBJECT_OK;
}

enum kernel_memory_object_status kernel_memory_object_create(
    struct kernel_heap *heap, struct physical_page_allocator *allocator,
    struct kernel_memory_object **object)
{
    struct kernel_memory_object *result;
    enum kernel_heap_status status;

    if (heap == 0 || allocator == 0 || object == 0 || *object != 0)
        return KERNEL_MEMORY_OBJECT_STATE;
    status = kernel_heap_allocate_zeroed(heap, 1U, sizeof(*result),
                                         (void **)&result);
    if (status == KERNEL_HEAP_STATUS_EMPTY)
        return KERNEL_MEMORY_OBJECT_NO_MEMORY;
    if (status != KERNEL_HEAP_STATUS_OK)
        return KERNEL_MEMORY_OBJECT_STATE;
    status = kernel_heap_allocate_zeroed(heap, SHARED_ANON_INITIAL_BUCKETS,
                                         sizeof(*result->buckets),
                                         (void **)&result->buckets);
    if (status != KERNEL_HEAP_STATUS_OK) {
        if (kernel_heap_release(heap, result) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_MEMORY_OBJECT_NO_MEMORY
                   : KERNEL_MEMORY_OBJECT_STATE;
    }
    kernel_mutex_init(&result->lock, 35, (uintptr_t)result);
    result->heap = heap;
    result->allocator = allocator;
    result->bucket_count = SHARED_ANON_INITIAL_BUCKETS;
    result->references = 1U;
    *object = result;
    return KERNEL_MEMORY_OBJECT_OK;
}

enum kernel_memory_object_status kernel_memory_object_acquire(
    struct kernel_memory_object *object)
{
    if (object == 0 || object->references == 0U ||
        object->references == UINT32_MAX)
        return KERNEL_MEMORY_OBJECT_STATE;
    object->references++;
    return KERNEL_MEMORY_OBJECT_OK;
}

void kernel_memory_object_release(struct kernel_memory_object **object)
{
    struct kernel_memory_object *owner;

    if (object == 0 || *object == 0 || (*object)->references == 0U)
        __builtin_trap();
    owner = *object;
    *object = 0;
    if (--owner->references != 0U) return;
    for (size_t bucket = 0U; bucket < owner->bucket_count; bucket++) {
        struct kernel_memory_object_page *page = owner->buckets[bucket];

        while (page != 0) {
            struct kernel_memory_object_page *next = page->next;

            if (physical_page_release(owner->allocator,
                                      page->physical_address) !=
                PHYSICAL_PAGE_STATUS_OK ||
                kernel_heap_release(owner->heap, page) !=
                KERNEL_HEAP_STATUS_OK) __builtin_trap();
            unaccount_page(owner);
            page = next;
        }
    }
    if (kernel_heap_release(owner->heap, owner->buckets) !=
            KERNEL_HEAP_STATUS_OK ||
        kernel_heap_release(owner->heap, owner) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

enum kernel_memory_object_status kernel_memory_object_get_page(
    struct kernel_memory_object *object, uint64_t page_index,
    uint64_t *physical_address, int *created)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_memory_object_page *page;
    enum kernel_heap_status heap_status;
    enum physical_page_status page_status;
    enum kernel_memory_object_status status;
    size_t bucket;
    void *bytes;

    if (object == 0 || physical_address == 0 || created == 0 ||
        object->references == 0U) return KERNEL_MEMORY_OBJECT_STATE;
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    bucket = bucket_for(page_index, object->bucket_count);
    for (page = object->buckets[bucket]; page != 0; page = page->next) {
        if (page->index != page_index) continue;
        if (physical_page_acquire(object->allocator,
                                  page->physical_address) !=
            PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        *physical_address = page->physical_address;
        *created = 0;
        return KERNEL_MEMORY_OBJECT_OK;
    }
    if (object->budget && object->budget->limit && object->budget->used >= object->budget->limit)
        return KERNEL_MEMORY_OBJECT_NO_SPACE;
    status = grow_buckets(object);
    if (status != KERNEL_MEMORY_OBJECT_OK) return status;
    heap_status = kernel_heap_allocate_zeroed(object->heap, 1U,
                                               sizeof(*page), (void **)&page);
    if (heap_status == KERNEL_HEAP_STATUS_EMPTY)
        return KERNEL_MEMORY_OBJECT_NO_MEMORY;
    if (heap_status != KERNEL_HEAP_STATUS_OK)
        return KERNEL_MEMORY_OBJECT_STATE;
    page_status = physical_page_allocate(object->allocator,
                                         &page->physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        if (kernel_heap_release(object->heap, page) != KERNEL_HEAP_STATUS_OK)
            __builtin_trap();
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_MEMORY_OBJECT_NO_MEMORY
                   : KERNEL_MEMORY_OBJECT_STATE;
    }
    if (physical_page_resolve(object->allocator, page->physical_address,
                              &bytes) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    for (size_t i = 0U; i < BOAROS_PAGE_SIZE; i++)
        ((unsigned char *)bytes)[i] = 0U;
    uintptr_t irq = arch_interrupt_save();
    /* 多个 inode 共用预算；分配后再次核对并与发布原子记账。 */
    if (object->budget && object->budget->limit &&
        object->budget->used >= object->budget->limit) {
        arch_interrupt_restore(irq);
        if (physical_page_release(object->allocator, page->physical_address) != PHYSICAL_PAGE_STATUS_OK ||
            kernel_heap_release(object->heap, page) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
        return KERNEL_MEMORY_OBJECT_NO_SPACE;
    }
    if (physical_page_acquire(object->allocator, page->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    page->index = page_index;
    bucket = bucket_for(page_index, object->bucket_count);
    page->next = object->buckets[bucket];
    object->buckets[bucket] = page;
    object->page_count++;
    if (object->budget) object->budget->used++;
    object->allocator->shared_anon_pages++;
    *physical_address = page->physical_address;
    *created = 1;
    arch_interrupt_restore(irq);
    return KERNEL_MEMORY_OBJECT_OK;
}

void kernel_memory_object_discard_new_page(
    struct kernel_memory_object *object, uint64_t page_index,
    uint64_t physical_address)
{
    struct kernel_memory_object_page **link;
    uint32_t references;

    if (object == 0 || object->references == 0U) __builtin_trap();
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    link = &object->buckets[bucket_for(page_index, object->bucket_count)];
    while (*link != 0 && (*link)->index != page_index)
        link = &(*link)->next;
    /* 另一个映射已接管时保留页；truncate/replacement 也不能误删新身份。 */
    if (*link == 0 || (*link)->physical_address != physical_address) return;
    if (physical_page_reference_count(object->allocator, physical_address,
                                      &references) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    if (references != 1U) return;
    struct kernel_memory_object_page *page = *link;
    *link = page->next;
    object->page_count--;
    unaccount_page(object);
    if (physical_page_release(object->allocator, physical_address) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_heap_release(object->heap, page) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

enum kernel_memory_object_status kernel_memory_object_find_page(
    struct kernel_memory_object *object, uint64_t index, uint64_t *address)
{
    if (!object || !object->references || !address) return KERNEL_MEMORY_OBJECT_STATE;
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    for (struct kernel_memory_object_page *p = object->buckets[bucket_for(index, object->bucket_count)];
         p; p = p->next) {
        if (p->index != index) continue;
        if (physical_page_acquire(object->allocator, p->physical_address) != PHYSICAL_PAGE_STATUS_OK)
            __builtin_trap();
        *address = p->physical_address;
        return KERNEL_MEMORY_OBJECT_OK;
    }
    return KERNEL_MEMORY_OBJECT_NOT_FOUND;
}

uint64_t kernel_memory_object_resident_pages(const struct kernel_memory_object *object)
{ if (!object || !object->references) __builtin_trap(); return object->page_count; }

void kernel_memory_object_truncate(struct kernel_memory_object *object, uint64_t size)
{
    if (!object || !object->references) __builtin_trap();
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    uint64_t last = size >> BOAROS_PAGE_SHIFT;
    size_t tail = size & BOAROS_PAGE_MASK;
    for (size_t i = 0; i < object->bucket_count; i++) {
        struct kernel_memory_object_page **link = &object->buckets[i];
        while (*link) {
            struct kernel_memory_object_page *page = *link;
            if (page->index > last || (page->index == last && !tail)) {
                *link = page->next;
                object->page_count--;
                unaccount_page(object);
                (void)physical_page_release(object->allocator, page->physical_address);
                if (kernel_heap_release(object->heap, page) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
                continue;
            }
            if (tail && page->index == last) {
                void *bytes;
                if (physical_page_resolve(object->allocator, page->physical_address, &bytes) !=
                    PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
                /* 缩短后保留的末页不允许在再次增长时暴露旧内容。 */
                for (size_t j = tail; j < BOAROS_PAGE_SIZE; j++) ((unsigned char *)bytes)[j] = 0;
            }
            link = &page->next;
        }
    }
}

void kernel_memory_object_set_budget(struct kernel_memory_object *object,
                                      struct kernel_memory_budget *budget)
{
    if (!object || object->page_count || object->budget) __builtin_trap();
    object->budget = budget;
}
