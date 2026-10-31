#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/memory_object.h>
#include <kernel/sync.h>
#include <kernel/errno.h>
#include <kernel/irq.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHARED_ANON_INITIAL_BUCKETS 16U

enum page_publication { PAGE_DIRECT, PAGE_PREPARING, PAGE_ADOPTED };

struct kernel_memory_object_page {
    struct kernel_memory_object_page *next;
    uint64_t index;
    uint64_t physical_address;
    uint32_t preparations;
    uint32_t publication;
};

struct kernel_memory_object {
    struct kernel_mutex lock;
    struct kernel_mutex mutation;
    struct kernel_raw_lock mapping_metadata;
    struct kernel_wait_queue mapping_changed;
    struct kernel_memory_mapping *mappings;
    uint64_t epoch;
    uint32_t mapping_borrows;
    unsigned closing;
    struct kernel_memory_budget *budget;
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_memory_object_page **buckets;
    size_t bucket_count;
    size_t page_count;
    uint32_t references;
};

/* 只在实际后备页发布/撤销时收费；PTE及临时页引用不重复记账。 */
static int charge(uint64_t *counter, uint64_t limit)
{
    uint64_t value = __atomic_load_n(counter, __ATOMIC_RELAXED);
    for (;;) {
        if (value == UINT64_MAX) __builtin_trap();
        if (limit && value >= limit) return 0;
        if (__atomic_compare_exchange_n(counter, &value, value + 1, 1,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 1;
    }
}
static void uncharge(uint64_t *counter)
{
    uint64_t value = __atomic_load_n(counter, __ATOMIC_RELAXED);
    for (;;) {
        if (!value) __builtin_trap();
        if (__atomic_compare_exchange_n(counter, &value, value - 1, 1,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    }
}
static void unaccount_page(struct kernel_memory_object *object)
{
    uncharge(&object->allocator->shared_anon_pages);
    if (object->budget) uncharge(&object->budget->used);
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

    if (__atomic_load_n(&object->page_count, __ATOMIC_RELAXED) / 2U < object->bucket_count)
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
    kernel_heap_release(object->heap, object->buckets);
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
        kernel_heap_release(heap, result);
        return status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_MEMORY_OBJECT_NO_MEMORY
                   : KERNEL_MEMORY_OBJECT_STATE;
    }
    kernel_mutex_init(&result->lock, 35, (uintptr_t)result);
    kernel_mutex_init(&result->mutation, 20, (uintptr_t)result);
    kernel_raw_lock_init(&result->mapping_metadata, KERNEL_RAW_RANK_FILE_MAPPING);
    kernel_wait_queue_init(&result->mapping_changed);
    result->epoch = 1;
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
    if (!object) return KERNEL_MEMORY_OBJECT_STATE;
    uint32_t value = __atomic_load_n(&object->references, __ATOMIC_RELAXED);
    for (;;) {
        if (!value || value == UINT32_MAX) return KERNEL_MEMORY_OBJECT_STATE;
        if (__atomic_compare_exchange_n(&object->references, &value, value + 1, 1,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return KERNEL_MEMORY_OBJECT_OK;
    }
}

void kernel_memory_object_release(struct kernel_memory_object **object)
{
    struct kernel_memory_object *owner;

    if (object == 0 || *object == 0 || __atomic_load_n(&(*object)->references, __ATOMIC_ACQUIRE) == 0U)
        __builtin_trap();
    owner = *object;
    *object = 0;
    if (__atomic_fetch_sub(&owner->references, 1, __ATOMIC_ACQ_REL) != 1) return;
    if (owner->mappings || owner->mapping_borrows || owner->closing) __builtin_trap();
    /* 末次owner唯一负责销毁；所有操作的实际对象引用都已结束。 */
    for (size_t bucket = 0U; bucket < owner->bucket_count; bucket++) {
        struct kernel_memory_object_page *page = owner->buckets[bucket];

        while (page != 0) {
            struct kernel_memory_object_page *next = page->next;

            if (page->preparations) __builtin_trap();
            if (physical_page_release(owner->allocator,
                                      page->physical_address) !=
                PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
            kernel_heap_release(owner->heap, page);
            unaccount_page(owner);
            page = next;
        }
    }
    kernel_heap_release(owner->heap, owner->buckets);
    kernel_heap_release(owner->heap, owner);
}

static enum kernel_memory_object_status get_page(
    struct kernel_memory_object *object, uint64_t page_index,
    uint64_t *physical_address, int *created, int preparing)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_memory_object_page *page;
    enum kernel_heap_status heap_status;
    enum physical_page_status page_status;
    enum kernel_memory_object_status status;
    size_t bucket;
    void *bytes;

    if (object == 0 || physical_address == 0 || created == 0 ||
        __atomic_load_n(&object->references, __ATOMIC_ACQUIRE) == 0U) return KERNEL_MEMORY_OBJECT_STATE;
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    bucket = bucket_for(page_index, object->bucket_count);
    for (page = object->buckets[bucket]; page != 0; page = page->next) {
        if (page->index != page_index) continue;
        if (physical_page_acquire(object->allocator,
                                  page->physical_address) !=
            PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
        if (preparing) {
            if (page->preparations == UINT32_MAX) __builtin_trap();
            page->preparations++;
            /* 继承直接消费者保存的页；新协议不能撤销其既有内容。 */
            if (page->publication == PAGE_DIRECT) page->publication = PAGE_ADOPTED;
        } else if (page->publication == PAGE_PREPARING) {
            page->publication = PAGE_ADOPTED;
        }
        *physical_address = page->physical_address;
        *created = 0;
        return KERNEL_MEMORY_OBJECT_OK;
    }
    if (object->budget && object->budget->limit && __atomic_load_n(&object->budget->used, __ATOMIC_ACQUIRE) >= object->budget->limit)
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
        kernel_heap_release(object->heap, page);
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_MEMORY_OBJECT_NO_MEMORY
                   : KERNEL_MEMORY_OBJECT_STATE;
    }
    if (physical_page_resolve(object->allocator, page->physical_address,
                              &bytes) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    memset(bytes, 0, BOAROS_PAGE_SIZE);
    if (physical_page_acquire(object->allocator, page->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    /* 先完成私有的object/PTE页引用准备，再CAS竞争跨对象预算；失败归还两份。 */
    if (object->budget && !charge(&object->budget->used, object->budget->limit)) {
        physical_page_release(object->allocator, page->physical_address);
        physical_page_release(object->allocator, page->physical_address);
        kernel_heap_release(object->heap, page);
        return KERNEL_MEMORY_OBJECT_NO_SPACE;
    }
    if (!charge(&object->allocator->shared_anon_pages, 0)) __builtin_trap();
    page->index = page_index;
    page->preparations = preparing != 0;
    page->publication = preparing ? PAGE_PREPARING : PAGE_DIRECT;
    bucket = bucket_for(page_index, object->bucket_count);
    page->next = object->buckets[bucket];
    object->buckets[bucket] = page;
    __atomic_fetch_add(&object->page_count, 1, __ATOMIC_RELEASE);
    *physical_address = page->physical_address;
    *created = 1;
    return KERNEL_MEMORY_OBJECT_OK;
}

enum kernel_memory_object_status kernel_memory_object_get_page(
    struct kernel_memory_object *object, uint64_t index, uint64_t *address, int *created)
{ return get_page(object, index, address, created, 0); }

enum kernel_memory_object_status kernel_memory_object_prepare_page(
    struct kernel_memory_object *object, uint64_t index, uint64_t *address, int *created)
{ return get_page(object, index, address, created, 1); }

void kernel_memory_object_finish_page(struct kernel_memory_object *object,
    uint64_t index, uint64_t address, int accepted)
{
    if (!object || !__atomic_load_n(&object->references, __ATOMIC_ACQUIRE)) __builtin_trap();
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    struct kernel_memory_object_page **link = &object->buckets[bucket_for(index, object->bucket_count)];
    while (*link && (*link)->index != index) link = &(*link)->next;
    if (!*link || (*link)->physical_address != address) {
        /* 截断可摘除对象槽；失败准备仍独立拥有返回页，不能复活旧身份。 */
        if (accepted || physical_page_release(object->allocator, address) != PHYSICAL_PAGE_STATUS_OK)
            __builtin_trap();
        return;
    }
    struct kernel_memory_object_page *page = *link;
    if (!page->preparations || page->publication == PAGE_DIRECT) __builtin_trap();
    if (accepted) page->publication = PAGE_ADOPTED;
    else if (physical_page_release(object->allocator, address) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    page->preparations--;
    /* 只有最后一个失败准备者可撤销未接纳页；PTE结束不抹去接纳历史。 */
    if (page->preparations || page->publication == PAGE_ADOPTED) return;
    uint32_t references;
    if (physical_page_reference_count(object->allocator, address, &references) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    if (references != 1U) __builtin_trap();
    *link = page->next;
    __atomic_fetch_sub(&object->page_count, 1, __ATOMIC_RELEASE);
    unaccount_page(object);
    if (physical_page_release(object->allocator, address) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    kernel_heap_release(object->heap, page);
}

void kernel_memory_object_discard_new_page(
    struct kernel_memory_object *object, uint64_t page_index,
    uint64_t physical_address)
{
    struct kernel_memory_object_page **link;
    uint32_t references;

    if (object == 0 || __atomic_load_n(&object->references, __ATOMIC_ACQUIRE) == 0U) __builtin_trap();
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    link = &object->buckets[bucket_for(page_index, object->bucket_count)];
    while (*link != 0 && (*link)->index != page_index)
        link = &(*link)->next;
    /* 另一个映射已接管时保留页；truncate/replacement 也不能误删新身份。 */
    if (*link == 0 || (*link)->physical_address != physical_address ||
        (*link)->preparations || (*link)->publication == PAGE_ADOPTED) return;
    if (physical_page_reference_count(object->allocator, physical_address,
                                      &references) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    if (references != 1U) return;
    struct kernel_memory_object_page *page = *link;
    *link = page->next;
    __atomic_fetch_sub(&object->page_count, 1, __ATOMIC_RELEASE);
    unaccount_page(object);
    if (physical_page_release(object->allocator, physical_address) !=
            PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    kernel_heap_release(object->heap, page);
}

enum kernel_memory_object_status kernel_memory_object_find_page(
    struct kernel_memory_object *object, uint64_t index, uint64_t *address)
{
    if (!object || !__atomic_load_n(&object->references, __ATOMIC_ACQUIRE) || !address) return KERNEL_MEMORY_OBJECT_STATE;
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    for (struct kernel_memory_object_page *p = object->buckets[bucket_for(index, object->bucket_count)];
         p; p = p->next) {
        if (p->index != index) continue;
        if (physical_page_acquire(object->allocator, p->physical_address) != PHYSICAL_PAGE_STATUS_OK)
            __builtin_trap();
        if (p->publication == PAGE_PREPARING) p->publication = PAGE_ADOPTED;
        *address = p->physical_address;
        return KERNEL_MEMORY_OBJECT_OK;
    }
    return KERNEL_MEMORY_OBJECT_NOT_FOUND;
}

uint64_t kernel_memory_object_resident_pages(const struct kernel_memory_object *object)
{ if (!object || !__atomic_load_n(&object->references, __ATOMIC_ACQUIRE)) __builtin_trap(); return __atomic_load_n(&object->page_count, __ATOMIC_ACQUIRE); }

void kernel_memory_object_truncate(struct kernel_memory_object *object, uint64_t size)
{
    if (!object || !__atomic_load_n(&object->references, __ATOMIC_ACQUIRE)) __builtin_trap();
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
                __atomic_fetch_sub(&object->page_count, 1, __ATOMIC_RELEASE);
                unaccount_page(object);
                (void)physical_page_release(object->allocator, page->physical_address);
                kernel_heap_release(object->heap, page);
                continue;
            }
            if (tail && page->index == last) {
                void *bytes;
                if (physical_page_resolve(object->allocator, page->physical_address, &bytes) !=
                    PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
                /* 缩短后保留的末页不允许在再次增长时暴露旧内容。 */
                memset((unsigned char *)bytes + tail, 0, BOAROS_PAGE_SIZE - tail);
            }
            link = &page->next;
        }
    }
}

void kernel_memory_object_punch(struct kernel_memory_object *object, uint64_t start, uint64_t end)
{
    if (!object || start >= end || !__atomic_load_n(&object->references, __ATOMIC_ACQUIRE))
        __builtin_trap();
    KERNEL_LOCK_SCOPE(guard);
    kernel_mutex_lock(&object->lock, &guard);
    for (size_t i = 0; i < object->bucket_count; i++) {
        struct kernel_memory_object_page **link = &object->buckets[i];
        while (*link) {
            struct kernel_memory_object_page *page = *link;
            uint64_t offset = page->index << BOAROS_PAGE_SHIFT;
            if (offset >= end || (start >= offset && start - offset >= BOAROS_PAGE_SIZE)) {
                link = &page->next;
                continue;
            }
            size_t first = start > offset ? (size_t)(start - offset) : 0;
            size_t last = end - offset < BOAROS_PAGE_SIZE ? (size_t)(end - offset) : BOAROS_PAGE_SIZE;
            if (!first && last == BOAROS_PAGE_SIZE) {
                *link = page->next;
                __atomic_fetch_sub(&object->page_count, 1, __ATOMIC_RELEASE);
                unaccount_page(object);
                if (physical_page_release(object->allocator, page->physical_address)) __builtin_trap();
                kernel_heap_release(object->heap, page);
                continue;
            }
            void *bytes;
            if (physical_page_resolve(object->allocator, page->physical_address, &bytes)) __builtin_trap();
            memset((unsigned char *)bytes + first, 0, last - first);
            link = &page->next;
        }
    }
}

static void mapping_changed(struct kernel_memory_object *object)
{
    KERNEL_IRQ_SCOPE(irq);
    enum kernel_scheduler_status status=kernel_wait_queue_wake_all(&object->mapping_changed);
    if (status != KERNEL_SCHEDULER_STATUS_OK &&
        !(status == KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED &&
          !object->mapping_changed.registrations && !object->mapping_changed.borrows))
        __builtin_trap();
}

static void mapping_borrow(struct kernel_memory_object *object,struct kernel_memory_mapping *mapping)
{
    if (!mapping) return;
    if (mapping->object!=object || mapping->borrows==UINT32_MAX || object->mapping_borrows==UINT32_MAX)
        __builtin_trap();
    mapping->borrows++;object->mapping_borrows++;
}

static void mapping_put(struct kernel_memory_object *object,struct kernel_memory_mapping *mapping)
{
    do {
        { KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);
          for (unsigned count=0;mapping && count<16;count++) {
              if (mapping->object!=object || !mapping->borrows || !object->mapping_borrows) __builtin_trap();
              object->mapping_borrows--;
              if (--mapping->borrows || mapping->previous) {mapping=0;break;}
              struct kernel_memory_mapping *next=mapping->retired_next;
              mapping->next=mapping->retired_next=0;mapping=next;
          } }
        mapping_changed(object);
    } while (mapping);
}

int kernel_memory_object_mapping_register(struct kernel_memory_mapping *mapping,uint64_t expected_epoch)
{
    if (!mapping || !mapping->object || !mapping->owner || !mapping->invalidate ||
        mapping->previous || mapping->borrows || mapping->retired_next) __builtin_trap();
    struct kernel_memory_object *object=mapping->object;
    KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);
    if (expected_epoch && (object->closing || object->epoch!=expected_epoch)) return -KERNEL_EBUSY;
    mapping->next=object->mappings;mapping->previous=&object->mappings;
    if (mapping->next) mapping->next->previous=&mapping->next;
    object->mappings=mapping;return 0;
}

void kernel_memory_object_mapping_stop(struct kernel_memory_mapping *mapping)
{
    struct kernel_memory_object *object=mapping->object;
    KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);
    if (!mapping->previous) return;
    if (*mapping->previous!=mapping || mapping->retired_next) __builtin_trap();
    *mapping->previous=mapping->next;
    if (mapping->next) mapping->next->previous=mapping->previous;
    mapping->previous=0;
    if (mapping->borrows) {
        mapping->retired_next=mapping->next;mapping_borrow(object,mapping->retired_next);
    } else mapping->next=0;
}

void kernel_memory_object_mapping_drain(struct kernel_memory_mapping *mapping)
{
    struct kernel_memory_object *object=mapping->object;
    KERNEL_IRQ_SCOPE(irq);
    for (;;) {
        { KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);
          if (mapping->previous) __builtin_trap();
          if (!mapping->borrows) return; }
        enum kernel_wait_wake_reason reason;
        int busy;
        struct kernel_wait_token token;
        if (kernel_wait_prepare(&object->mapping_changed,0,0,&token)) __builtin_trap();
        { KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);busy=mapping->borrows!=0; }
        if (busy && kernel_wait_park(&token,&reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
    }
}

int kernel_memory_object_epoch(struct kernel_memory_object *object,uint64_t *epoch)
{
    KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);
    *epoch=object->epoch;return object->closing ? -KERNEL_EBUSY : 0;
}

void kernel_memory_object_wait(struct kernel_memory_object *object)
{
    KERNEL_IRQ_SCOPE(irq);
    uint64_t epoch;
    while (kernel_memory_object_epoch(object,&epoch)) {
        enum kernel_wait_wake_reason reason;
        if (KERNEL_WAIT_RECHECK(&object->mapping_changed,0,0,&reason,
                kernel_memory_object_epoch(object,&epoch))) __builtin_trap();
    }
}

void kernel_memory_object_remove(struct kernel_memory_object *object,uint64_t start,uint64_t end)
{
    KERNEL_LOCK_SCOPE(mutation);
    kernel_mutex_lock(&object->mutation,&mutation);
    struct kernel_memory_mapping *mapping;
    { KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);
      if (object->closing || object->epoch==UINT64_MAX) __builtin_trap();
      object->closing=1;object->epoch++;
      mapping=object->mappings;mapping_borrow(object,mapping); }
    while (mapping) {
        int active;
        { KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);active=mapping->previous!=0; }
        if (active) mapping->invalidate(mapping->owner,object,start,end);
        struct kernel_memory_mapping *next;
        { KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);
          next=mapping->previous?mapping->next:mapping->retired_next;mapping_borrow(object,next); }
        mapping_put(object,mapping);mapping=next;
    }
    kernel_memory_object_punch(object,start,end);
    { KERNEL_RAW_SCOPE(guard,&object->mapping_metadata);object->closing=0; }
    mapping_changed(object);
}

void kernel_memory_object_set_budget(struct kernel_memory_object *object,
                                      struct kernel_memory_budget *budget)
{
    if (!object || __atomic_load_n(&object->page_count, __ATOMIC_ACQUIRE) || object->budget) __builtin_trap();
    object->budget = budget;
}

enum kernel_memory_object_status kernel_memory_object_check_domain(
    const struct kernel_memory_object *object, const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator)
{
    if (!object || !heap || !allocator ||
        !__atomic_load_n(&object->references, __ATOMIC_ACQUIRE) ||
        object->heap != heap || object->allocator != allocator || object->budget)
        return KERNEL_MEMORY_OBJECT_STATE;
    return KERNEL_MEMORY_OBJECT_OK;
}

enum kernel_memory_object_status kernel_memory_object_check_budget_domain(
    const struct kernel_memory_object *object, const struct kernel_heap *heap,
    const struct physical_page_allocator *allocator,
    const struct kernel_memory_budget *budget)
{
    if (!object || !heap || !allocator || !budget ||
        !__atomic_load_n(&object->references, __ATOMIC_ACQUIRE) ||
        object->heap != heap || object->allocator != allocator || object->budget != budget)
        return KERNEL_MEMORY_OBJECT_STATE;
    return KERNEL_MEMORY_OBJECT_OK;
}
