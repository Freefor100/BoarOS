#include <kernel/mm_backend.h>
#include <kernel/rlimit.h>
#include <kernel/cpu.h>
#include <kernel/elf64_source.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/futex.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/open_file.h>
#include <kernel/accounting.h>
#include <kernel/page.h>
#include <kernel/memory_object.h>
#include <kernel/task.h>
#include <kernel/sync.h>
#include <kernel/shm.h>
#include <kernel/scheduler.h>
#include <kernel/vma.h>
#include <kernel/vfs.h>
#include <kernel/tmpfs.h>
#include "../fs/vfs_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if BOAROS_COST_DIAGNOSTICS
#define MM_DIAGNOSTIC(statement) statement
#else
#define MM_DIAGNOSTIC(statement) ((void)0)
#endif

#if BOAROS_USER_SMP_COMPOSITION
void riscv_user_composition_fault_retry(uint64_t address, int resumed);
#endif


/* 创建仍在CPU0；原子预约也覆盖其抢占交错，零永久表示代次耗尽。 */
static uint64_t next_futex_mm_id = 1U;
static uint64_t reserve_mm_identity(void)
{
    uint64_t value = __atomic_load_n(&next_futex_mm_id, __ATOMIC_ACQUIRE);
    while (value) {
        if (__atomic_compare_exchange_n(&next_futex_mm_id, &value, value + 1U,
            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return value;
    }
    return 0;
}

enum kernel_mm_record_stage {
    KERNEL_MM_RECORD_LIVE = 0,
    KERNEL_MM_RECORD_VMAS_CLEANUP,
    KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP,
    KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP,
    KERNEL_MM_RECORD_ONLY_CLEANUP,
};

struct kernel_mm_file_source {
    struct kernel_mm_file_source *next;
    struct kernel_open_file_description *file;
    _Atomic uint32_t operations;
    int draining;
};

struct kernel_mm_shared_anon {
    struct kernel_mm_shared_anon *next;
    struct kernel_memory_object *object;
};

struct mm_file_mapping {
    struct mm_file_mapping *next;
    struct kernel_file_mapping registration;
};

/* PTE owns the physical reference. This metadata survives cache eviction and
 * PROT_NONE; fork copies provenance, not a guess based on the PTE COW bit. */
struct mm_file_resident {
    struct kernel_mm_record *record;
    struct mm_file_resident *next;
    struct mm_file_resident **previous;
    struct mm_file_resident *clone_source;
    struct mm_file_resident *hash_next;
    uint64_t address;
    uint64_t physical_address;
    unsigned private:1, cache_retry_registered:1;
    struct mm_file_resident *cache_retry_next;
    struct kernel_page_cache_alias alias;
};

struct kernel_mm_elf_source {
    struct kernel_mm_elf_source *next;
    struct kernel_elf64_source *source;
    _Atomic uint32_t faults;
    int draining;
};

struct kernel_mm_record {
    uint64_t futex_id;
    uint32_t references;
    uint32_t users;
    struct kernel_raw_lock metadata;
    uint32_t access_pins, write_pins;
    struct kernel_mutex operations;
    struct kernel_wait_queue pins_changed;
    uint32_t access_closed;
    uint32_t active_cpus, pending_retirement;
    uint64_t tlb_generation, instruction_generation;
    uint64_t pin_acquisitions, prepared_pages, fault_retries;
    uint64_t retirement_batches, retired_pages, pin_waits;
    enum kernel_mm_record_stage stage;
    uint64_t start_code, end_code, start_stack;
    uint64_t start_data, end_data, stack_top;
    uint64_t start_brk;
    uint64_t current_brk;
    uint64_t brk_limit;
    uint64_t mmap_base;
    uint64_t vdso_address;
    uint32_t brk_initialized;
    struct arch_mmu_user_space space;
    struct kernel_vma_set *vmas;
    struct kernel_heap *vma_heap;
    struct kernel_mm_file_source *file_sources;
    _Atomic(struct kernel_mm_file_source *) retired_file_sources;
    struct kernel_mm_shared_anon *shared_anon;
    struct mm_file_mapping *file_mappings;
    struct mm_file_mapping *retired_file_mappings;
    struct mm_file_resident *file_residents;
    struct mm_file_resident *retired_file_residents;
    uint64_t cache_gate_generation;
    struct mm_file_resident *cache_retry_head;
    uint64_t resident_probes;
    struct mm_file_resident **resident_buckets;
    size_t resident_capacity;
    size_t resident_count;
    struct kernel_mm_elf_source *elf_sources;
    _Atomic(struct kernel_mm_elf_source *) retired_elf_sources;
    struct kernel_open_file_description *executable_file;
    uint32_t future_lock_flags;
};

static uint32_t backend_permissions_from_mm(uint32_t permissions);
static enum kernel_mm_status unmap_space_range(struct kernel_mm_record *,uint64_t,uint64_t);
static uint32_t note_tlb_change_locked(struct kernel_mm_record *,
    const struct kernel_mm_retirement *, int);
static void finish_tlb_change(struct kernel_mm_record *,
    struct kernel_mm_retirement *, uint32_t, uint64_t, uint64_t, int);

_Static_assert(sizeof(struct kernel_mm_record) <= BOAROS_PAGE_SIZE,
               "RISC-V MM record must fit in one physical page");

static int empty_handle(const struct kernel_mm *mm)
{
    return mm->state == KERNEL_MM_EMPTY && mm->allocator == 0 &&
           mm->record == 0 && mm->record_page_address == 0U &&
           mm->cleanup_stage == KERNEL_MM_CLEANUP_NONE;
}

static int owner_handle(const struct kernel_mm *mm)
{
    return mm->record != 0 && mm->allocator != 0 &&
           (mm->record_page_address & BOAROS_PAGE_MASK) == 0U &&
           (mm->state == KERNEL_MM_LIVE ||
            mm->state == KERNEL_MM_CLEANUP);
}

static void finish_handle(struct kernel_mm *mm, enum kernel_mm_state state)
{
    mm->allocator = 0;
    mm->record = 0;
    mm->record_page_address = 0U;
    mm->state = state;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
}

static void clear_page(void *pointer)
{
    memset(pointer, 0, BOAROS_PAGE_SIZE);
}

static enum kernel_mm_status resolve_record_storage(
    const struct kernel_mm *mm,
    struct kernel_mm_record **record)
{
    if (!owner_handle(mm)) return KERNEL_MM_STATUS_STATE;
    *record = mm->record;
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status resolve_record(const struct kernel_mm *mm,
    struct kernel_mm_record **record)
{
    return resolve_record_storage(mm, record);
}

static enum kernel_mm_status release_record_only(struct kernel_mm *mm)
{
    (void)physical_page_release(mm->allocator,
                              mm->record_page_address);
    finish_handle(mm, KERNEL_MM_RELEASED);
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status abandon_unresolved_record(
    struct kernel_mm *mm,
    struct physical_page_allocator *allocator,
    uint64_t record_page_address,
    enum kernel_mm_status failure)
{
    (void)mm;
    (void)physical_page_release(allocator, record_page_address);
    return failure;
}

enum kernel_mm_status kernel_mm_create(
    struct kernel_mm *mm,
    struct arch_mmu_user_space *space)
{
    kernel_assert_service_context();
    KERNEL_NO_RECLAIM_IO;
    struct physical_page_allocator *allocator;
    struct kernel_mm_record *record;
    uint64_t record_page_address;
    void *pointer;
    enum physical_page_status page_status;

    if (mm == 0 || space == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(mm) ||
        space->state != ARCH_MMU_USER_SPACE_LIVE ||
        space->allocator == 0) {
        return KERNEL_MM_STATUS_STATE;
    }
    if (!__atomic_load_n(&next_futex_mm_id, __ATOMIC_ACQUIRE)) return KERNEL_MM_STATUS_NO_MEMORY;
    allocator = space->allocator;
    page_status = physical_page_allocate(allocator,
                                         &record_page_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_MM_STATUS_STATE;
    }
    if (physical_page_map_owned(allocator,
                              record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return abandon_unresolved_record(mm,
                                         allocator,
                                         record_page_address,
                                         KERNEL_MM_STATUS_PAGE_ACCESS);
    }

    clear_page(pointer);
    record = pointer;
    record->futex_id = reserve_mm_identity();
    if (!record->futex_id) return abandon_unresolved_record(mm, allocator,
        record_page_address, KERNEL_MM_STATUS_NO_MEMORY);
    record->references = 1U;
    kernel_raw_lock_init(&record->metadata, KERNEL_RAW_RANK_MM);
    kernel_mutex_init(&record->operations, 25, (uintptr_t)record);
    kernel_wait_queue_init(&record->pins_changed);
    record->stage = KERNEL_MM_RECORD_LIVE;
    if (arch_mmu_user_space_move(&record->space, space) !=
        ARCH_MMU_STATUS_OK) {
        return abandon_unresolved_record(
            mm,
            allocator,
            record_page_address,
            KERNEL_MM_STATUS_ADDRESS_SPACE);
    }
    mm->allocator = allocator;
    mm->record = record;
    mm->record_page_address = record_page_address;
    mm->state = KERNEL_MM_LIVE;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_acquire(
    struct kernel_mm *destination,
    const struct kernel_mm *source)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(destination) || source->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(source, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (record->stage != KERNEL_MM_RECORD_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    /* source的owner保证record存活；引用取得也可在任务身份锁内完成，
     * 避免为了借用MM反向取得其页表metadata锁。 */
    uint32_t references = __atomic_load_n(&record->references, __ATOMIC_RELAXED);
    do {
        if (!references || references == UINT32_MAX) return KERNEL_MM_STATUS_STATE;
    } while (!__atomic_compare_exchange_n(&record->references, &references,
        references + 1U, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    *destination = *source;
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status fork_status_from_backend(
    enum arch_mmu_status status)
{
    switch (status) {
    case ARCH_MMU_STATUS_NO_MEMORY:
        return KERNEL_MM_STATUS_NO_MEMORY;
    case ARCH_MMU_STATUS_INVALID:
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    case ARCH_MMU_STATUS_CONFLICT:
        return KERNEL_MM_STATUS_CONFLICT;
    case ARCH_MMU_STATUS_STATE:
        return KERNEL_MM_STATUS_STATE;
    case ARCH_MMU_STATUS_NOT_MAPPED:
    case ARCH_MMU_STATUS_OK:
    default:
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
}

static enum kernel_mm_status status_from_vma(enum kernel_vma_status status)
{
    switch (status) {
    case KERNEL_VMA_STATUS_OK:
        return KERNEL_MM_STATUS_OK;
    case KERNEL_VMA_STATUS_INVALID_ARGUMENT:
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    case KERNEL_VMA_STATUS_NO_MEMORY:
        return KERNEL_MM_STATUS_NO_MEMORY;
    case KERNEL_VMA_STATUS_CONFLICT:
        return KERNEL_MM_STATUS_CONFLICT;
    case KERNEL_VMA_STATUS_NOT_FOUND:
        return KERNEL_MM_STATUS_NOT_MAPPED;
    case KERNEL_VMA_STATUS_ACCESS:
        return KERNEL_MM_STATUS_ACCESS;
    case KERNEL_VMA_STATUS_STATE:
    default:
        return KERNEL_MM_STATUS_STATE;
    }
}

static struct kernel_mm_file_source *find_file_source(
    const struct kernel_mm_record *record,
    const struct kernel_open_file_description *file)
{
    struct kernel_mm_file_source *source;

    for (source = record->file_sources;
         source != 0;
         source = source->next) {
        if (!source->draining && source->file == file) {
            return source;
        }
    }
    return 0;
}

static size_t resident_bucket(uint64_t address, size_t capacity)
{
    uint64_t value = address >> BOAROS_PAGE_SHIFT;
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return (size_t)value & (capacity - 1);
}

/* Allocate before publishing PTEs/aliases. Rehash itself cannot sleep/fail. */
static enum kernel_mm_status reserve_file_residents(
    struct kernel_mm_record *record, size_t needed)
{
    if (needed <= record->resident_capacity) return KERNEL_MM_STATUS_OK;
    size_t capacity = record->resident_capacity ? record->resident_capacity : 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) return KERNEL_MM_STATUS_NO_MEMORY;
        capacity *= 2;
    }
    struct mm_file_resident **buckets;
    enum kernel_heap_status hs = kernel_heap_allocate_zeroed(record->vma_heap,
        capacity, sizeof(*buckets), (void **)&buckets);
    if (hs != KERNEL_HEAP_STATUS_OK)
        return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                               : KERNEL_MM_STATUS_STATE;
    struct mm_file_resident **old;
    {
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    for (struct mm_file_resident *page = record->file_residents;
         page != 0; page = page->next) {
        size_t index = resident_bucket(page->address, capacity);
        page->hash_next = buckets[index];
        buckets[index] = page;
    }
    old = record->resident_buckets;
    record->resident_buckets = buckets;
    record->resident_capacity = capacity;
    }
    if (old) kernel_heap_release(record->vma_heap, old);
    return KERNEL_MM_STATUS_OK;
}

static void publish_file_resident(struct kernel_mm_record *record,
                                   struct mm_file_resident *page)
{
    page->record = record;
    if (record->resident_count >= record->resident_capacity) __builtin_trap();
    size_t index = resident_bucket(page->address, record->resident_capacity);
    for (struct mm_file_resident *old = record->resident_buckets[index];
         old != 0; old = old->hash_next)
        if (old->address == page->address) __builtin_trap();
    page->hash_next = record->resident_buckets[index];
    record->resident_buckets[index] = page;
    record->resident_count++;
    page->next = record->file_residents;
    page->previous = &record->file_residents;
    if (page->next) page->next->previous = &page->next;
    record->file_residents = page;
}

/* 只遍历真实登记的重试借用，不扫描MM/来源；gate释放后通知缓存。 */
static void mm_gate_release(struct kernel_mm_record *record,struct kernel_lock_guard *guard)
{
    if (!guard->lock) return;
    int indexed = record->resident_capacity != 0;
    kernel_lock_release(guard);
    if (!indexed) return;
    struct mm_file_resident *pending;
    {
        KERNEL_RAW_SCOPE(metadata,&record->metadata);
        if (record->cache_gate_generation==UINT64_MAX) __builtin_trap();
        record->cache_gate_generation++;
        pending=record->cache_retry_head; record->cache_retry_head=0;
    }
    while (pending) {
        struct mm_file_resident *page=pending;
        {
            KERNEL_RAW_SCOPE(metadata,&record->metadata);
            pending=page->cache_retry_next; page->cache_retry_next=0;
            if (!page->cache_retry_registered) __builtin_trap();
            page->cache_retry_registered=0;
        }
        kernel_page_cache_retry_alias(&page->alias);
        kernel_page_cache_alias_put(&page->alias);
    }
}
struct mm_gate_scope { struct kernel_mm_record *record; struct kernel_lock_guard guard; };
static void mm_gate_scope_end(struct mm_gate_scope *scope)
{ mm_gate_release(scope->record,&scope->guard); }
static void defer_cache_rearm(struct mm_file_resident *page,uint64_t generation)
{
    struct kernel_mm_record *record=page->record;
    kernel_page_cache_alias_hold(&page->alias);
    int queued=0;
    {
        KERNEL_RAW_SCOPE(metadata,&record->metadata);
        if (record->stage==KERNEL_MM_RECORD_LIVE && kernel_page_cache_alias_active(&page->alias) &&
            generation==record->cache_gate_generation && !page->cache_retry_registered) {
            page->cache_retry_registered=1; page->cache_retry_next=record->cache_retry_head;
            record->cache_retry_head=page; queued=1;
        }
    }
    if (!queued) {
        /* release发生于try和登记之间：立即通知，不能等一个已过去的事件。 */
        kernel_page_cache_retry_alias(&page->alias);
        kernel_page_cache_alias_put(&page->alias);
    }
}

static void retire_file_resident(struct kernel_mm_record *record,struct mm_file_resident *page)
{
    if (kernel_page_cache_alias_active(&page->alias)) kernel_page_cache_alias_detach(&page->alias);
    if (page->alias.entry) {
        page->next=record->retired_file_residents; record->retired_file_residents=page;
    } else kernel_heap_release(record->vma_heap,page);
}
static void drain_retired_residents(struct kernel_mm_record *record,struct mm_file_resident *pages)
{
    while (pages) {
        struct mm_file_resident *page=pages; pages=page->next;
        kernel_page_cache_alias_drain(&page->alias);
        kernel_heap_release(record->vma_heap,page);
    }
}

static void unindex_file_resident(struct kernel_mm_record *record,
                                  struct mm_file_resident *page)
{
    if (record->resident_count == 0) __builtin_trap();
    size_t index = resident_bucket(page->address, record->resident_capacity);
    struct mm_file_resident **link = &record->resident_buckets[index];
    while (*link != page) {
        if (*link == 0) __builtin_trap();
        link = &(*link)->hash_next;
    }
    *link = page->hash_next;
    record->resident_count--;
    *page->previous = page->next;
    if (page->next) page->next->previous = page->previous;
    page->previous = 0;
}

static void forget_file_residents(struct kernel_mm_record *record)
{
    while (record->file_residents) {
        struct mm_file_resident *page = record->file_residents;
        { KERNEL_RAW_SCOPE(guard, &record->metadata);
          unindex_file_resident(record, page); }
        retire_file_resident(record, page);
    }
}

static int rearm_shared_file_alias(void *owner, uint64_t address)
{
    struct mm_file_resident *page=owner;
    struct kernel_mm_record *record = page->record;
    struct mm_gate_scope operation __attribute__((cleanup(mm_gate_scope_end)))={.record=record};
    if (!kernel_lock_held(&record->operations.lock,1)) {
        int background=kernel_io_context_current()->background_reclaim;
        uint64_t generation;
        { KERNEL_RAW_SCOPE(metadata,&record->metadata); generation=record->cache_gate_generation; }
        if (background || kernel_io_context_current()->user_pins) {
            if (!kernel_mutex_try_lock(&record->operations,&operation.guard)) {
                if (background) defer_cache_rearm(page,generation);
                return -KERNEL_EBUSY;
            }
        } else kernel_mutex_lock(&record->operations,&operation.guard);
    }
    /* 回调可能在mremap之前借出；gate内由resident owner读取当前地址。 */
    address = page->address;
    struct kernel_vma vma;
    uint32_t permissions;
    if (record->stage!=KERNEL_MM_RECORD_LIVE || !kernel_page_cache_alias_active(&page->alias) ||
        kernel_vma_set_lookup(record->vmas, address, &vma) !=
            KERNEL_VMA_STATUS_OK ||
        vma.kind != KERNEL_VMA_KIND_FILE_SHARED) return 0;
    permissions = vma.permissions & ~KERNEL_MM_WRITE;
    struct kernel_mm_retirement batch={0}; uint32_t targets=0;
    {
        KERNEL_RAW_SCOPE(guard,&record->metadata);
        struct arch_mmu_mapping mapping;
        if (arch_mmu_user_lookup(&record->space,address,&mapping)!=ARCH_MMU_STATUS_OK ||
            (mapping.physical_address&~BOAROS_PAGE_MASK)!=page->physical_address) return 0;
        if (!(mapping.permissions&ARCH_MMU_WRITE)) return 0;
        if (arch_mmu_user_protect_owned_page(&record->space,address,
                backend_permissions_from_mm(permissions))) __builtin_trap();
        targets=note_tlb_change_locked(record, &batch, 0);
    }
    finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,0);
    return 0;
}

static struct mm_file_resident *find_file_resident(
    struct kernel_mm_record *record, uint64_t address)
{
    if (record->resident_capacity == 0) return 0;
    size_t index = resident_bucket(address, record->resident_capacity);
    for (struct mm_file_resident *page = record->resident_buckets[index];
         page != 0; page = page->hash_next) {
#if BOAROS_COST_DIAGNOSTICS
        record->resident_probes++;
#endif
        if (page->address == address) return page;
    }
    return 0;
}

static void truncate_file_residents(void *owner, struct kernel_vfs_node *node,
                                    uint64_t size)
{
    struct kernel_mm_record *record = owner;
    struct mm_file_resident **link = &record->file_residents;
    uint64_t last_page = size & ~BOAROS_PAGE_MASK;
    int cleared = 0;
    KERNEL_LOCK_SCOPE(operation);
    kernel_mutex_lock(&record->operations,&operation);
    if (record->stage != KERNEL_MM_RECORD_LIVE) return;
    /* 先按VMA形成EOF外的连续区间；不能每个resident重新扫描同一叶表。 */
    uint64_t first=0,end=0;
    for (uint32_t index=0;index<kernel_vma_set_count(record->vmas);index++) {
        struct kernel_vma vma;
        if (kernel_vma_set_get_at(record->vmas,index,&vma)) __builtin_trap();
        if ((vma.kind!=KERNEL_VMA_KIND_FILE_PRIVATE && vma.kind!=KERNEL_VMA_KIND_FILE_SHARED) ||
            kernel_open_file_node(vma.backing)!=node) continue;
        uint64_t start=vma.start;
        if (vma.backing_offset<size) {
            uint64_t bytes=size-vma.backing_offset;
            if (bytes>=vma.end-vma.start) continue;
            start+=(bytes+BOAROS_PAGE_MASK)&~BOAROS_PAGE_MASK;
        }
        if (start==vma.end) continue;
        if (first && start!=end) {
            if (unmap_space_range(record,first,end)) __builtin_trap();
            first=0;
        }
        if (!first) first=start;
        end=vma.end;
    }
    if (first && unmap_space_range(record,first,end)) __builtin_trap();

    while (*link != 0) {
        struct mm_file_resident *page = *link;
        struct kernel_vma vma;
        uint64_t offset;
        if (kernel_vma_set_lookup(record->vmas, page->address, &vma) !=
                KERNEL_VMA_STATUS_OK ||
            (vma.kind != KERNEL_VMA_KIND_FILE_PRIVATE &&
             vma.kind != KERNEL_VMA_KIND_FILE_SHARED)) {
            __builtin_trap();
        }
        if (kernel_open_file_node(vma.backing) != node) {
            link = &page->next;
            continue;
        }
        offset = vma.backing_offset + page->address - vma.start;
        if (offset>=size) __builtin_trap();
        if (kernel_open_file_memory_backed(vma.backing) && offset == last_page && !page->private &&
            vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE) {
            unsigned char *bytes;
            if (physical_page_map_owned(record->space.allocator,
                    page->physical_address, (void **)&bytes) !=
                    PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
            size_t tail = (size_t)(size & BOAROS_PAGE_MASK);
            memset(bytes + tail, 0, BOAROS_PAGE_SIZE - tail);
            cleared = 1;
        }
        link = &page->next;
    }
    if (cleared) {
        uint32_t targets;
        { KERNEL_RAW_SCOPE(guard, &record->metadata);
          if (record->instruction_generation == UINT64_MAX) __builtin_trap();
          record->instruction_generation++;
          targets = record->active_cpus; }
        arch_mmu_sync_user(targets, 0, UINT64_MAX, 1);
    }
    struct mm_file_resident *retired=record->retired_file_residents;
    record->retired_file_residents=0;
    if (operation.lock) mm_gate_release(record,&operation);
    drain_retired_residents(record,retired);
}

static void invalidate_file_residents(void *owner, struct kernel_vfs_node *node,
    uint64_t start, uint64_t end)
{
    struct kernel_mm_record *record = owner;
    KERNEL_LOCK_SCOPE(operation);
    kernel_mutex_lock(&record->operations,&operation);
    if (record->stage != KERNEL_MM_RECORD_LIVE) return;
    struct mm_file_resident **link = &record->file_residents;
    while (*link) {
        struct mm_file_resident *page = *link;
        struct kernel_vma vma;
        if (kernel_vma_set_lookup(record->vmas,page->address,&vma)) __builtin_trap();
        uint64_t offset = vma.backing_offset + page->address - vma.start;
        /* Private COW owns its bytes independently of later file mutations. */
        if (kernel_open_file_node(vma.backing) != node || page->private ||
            offset >= end || offset + BOAROS_PAGE_SIZE <= start) {
            link = &page->next;
            continue;
        }
        if (unmap_space_range(record,page->address,page->address+BOAROS_PAGE_SIZE)) __builtin_trap();
    }
    struct mm_file_resident *retired = record->retired_file_residents;
    record->retired_file_residents = 0;
    if (operation.lock) mm_gate_release(record,&operation);
    drain_retired_residents(record,retired);
}

static enum kernel_mm_status prepare_file_registration(
    struct kernel_mm_record *record,
    struct kernel_open_file_description *file,
    struct mm_file_mapping **prepared)
{
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    enum kernel_heap_status status;
    *prepared = 0;
    if (node == 0) return KERNEL_MM_STATUS_STATE;
    int error = kernel_file_mapping_prepare(node);
    if (error) return error == -KERNEL_ENOMEM ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
    for (struct mm_file_mapping *entry = record->file_mappings;
         entry != 0; entry = entry->next) {
        if (entry->registration.node == node) return KERNEL_MM_STATUS_OK;
    }
    status = kernel_heap_allocate_zeroed(record->vma_heap, 1U,
                                         sizeof(**prepared), (void **)prepared);
    if (status != KERNEL_HEAP_STATUS_OK) {
        return status == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                                  : KERNEL_MM_STATUS_STATE;
    }
    (*prepared)->registration.node = node;
    (*prepared)->registration.owner = record;
    (*prepared)->registration.truncate = truncate_file_residents;
    (*prepared)->registration.invalidate_range = invalidate_file_residents;
    return KERNEL_MM_STATUS_OK;
}

static void drain_file_registrations(struct kernel_mm_record *record,
                                     int unused_only)
{
    struct mm_file_mapping **link = &record->file_mappings;
    while (*link != 0) {
        struct mm_file_mapping *entry = *link;
        int used = 0;
        for (struct kernel_mm_file_source *source = record->file_sources;
             unused_only && source != 0; source = source->next) {
            if (kernel_open_file_node(source->file) != entry->registration.node)
                continue;
            if (kernel_vma_set_backing_in_use(record->vmas, source->file,
                                               &used) != KERNEL_VMA_STATUS_OK)
                __builtin_trap();
            if (used) break;
        }
        if (used) {
            link = &entry->next;
        } else {
            *link = entry->next;
            if (record->stage == KERNEL_MM_RECORD_LIVE &&
                kernel_lock_held(&record->operations.lock,1)) {
                kernel_file_mapping_stop(&entry->registration);
                /* 控制者的独立OFD引用覆盖inode；当前MM句柄覆盖record。
                 * 摘登记时不等回调，operation尾部放gate后才drain。 */
                entry->next=record->retired_file_mappings;
                record->retired_file_mappings=entry;
            } else {
                kernel_file_mapping_unregister(&entry->registration);
                kernel_heap_release(record->vma_heap, entry);
            }
        }
    }
}

/* Detached nodes keep their real OFD owner through callback drain and close.
 * A failed close returns the same node to the MM for the existing retry path. */
static int release_file_sources(struct kernel_mm_record *record,
    struct kernel_mm_file_source *sources)
{
    struct kernel_mm_file_source *failed=NULL;
    while (sources) {
        struct kernel_mm_file_source *source=sources; sources=source->next;
        if (source->file && kernel_open_file_release(&source->file)!=KERNEL_OPEN_FILE_STATUS_OK) {
            source->draining=1; source->next=failed; failed=source;
        } else kernel_heap_release(record->vma_heap,source);
    }
    if (!failed) return 0;
    KERNEL_LOCK_SCOPE(guard);
    if (record->stage==KERNEL_MM_RECORD_LIVE)
        kernel_mutex_lock(&record->operations,&guard);
    while (failed) {
        struct kernel_mm_file_source *source=failed; failed=source->next;
        source->next=record->retired_file_sources; record->retired_file_sources=source;
    }
    return 1;
}

static enum kernel_mm_status drain_file_sources(
    struct kernel_mm_record *record,
    int unused_only)
{
    struct kernel_mm_file_source **link = &record->file_sources;
    int invalid=0,defer=kernel_lock_held(&record->operations.lock,1);
    struct kernel_mm_file_source *released=defer ? NULL : record->retired_file_sources;
    if (released) record->retired_file_sources=NULL;

    drain_file_registrations(record, unused_only);

    while (*link != 0) {
        struct kernel_mm_file_source *source = *link;
        int in_use = 0;

        if (source->operations || source->draining) {
            if (!unused_only) __builtin_trap();
            link = &source->next;
            continue;
        }
        if (source->file != 0) {
            if (unused_only != 0) {
                if (record->vmas == 0 ||
                    kernel_vma_set_backing_in_use(record->vmas,
                                                  source->file,
                                                  &in_use) !=
                        KERNEL_VMA_STATUS_OK) {
                    invalid=1; break;
                }
            }
            if (in_use != 0) {
                link = &source->next;
                continue;
            }
        }
        *link = source->next;
        source->draining=1;
        if (defer) {
            source->next=record->retired_file_sources; record->retired_file_sources=source;
        } else { source->next=released; released=source; }
    }
    int failed=release_file_sources(record,released);
    if (invalid) return KERNEL_MM_STATUS_STATE;
    return failed != 0 ? KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       : KERNEL_MM_STATUS_OK;
}

static struct kernel_mm_elf_source *find_elf_source(
    const struct kernel_mm_record *record,
    const struct kernel_elf64_source *source)
{
    struct kernel_mm_elf_source *entry;

    for (entry = record->elf_sources; entry != 0; entry = entry->next) {
        if (!entry->draining && entry->source == source) {
            return entry;
        }
    }
    return 0;
}

static int release_elf_sources(struct kernel_mm_record *record,
    struct kernel_mm_elf_source *sources)
{
    struct kernel_mm_elf_source *failed=NULL;
    while (sources) {
        struct kernel_mm_elf_source *source=sources; sources=source->next;
        if (source->source && kernel_elf64_source_release(&source->source)!=KERNEL_ELF64_SOURCE_STATUS_OK) {
            source->draining=1; source->next=failed; failed=source;
        } else kernel_heap_release(record->vma_heap,source);
    }
    if (!failed) return 0;
    KERNEL_LOCK_SCOPE(guard);
    if (record->stage==KERNEL_MM_RECORD_LIVE)
        kernel_mutex_lock(&record->operations,&guard);
    while (failed) {
        struct kernel_mm_elf_source *source=failed; failed=source->next;
        source->next=record->retired_elf_sources; record->retired_elf_sources=source;
    }
    return 1;
}

static enum kernel_mm_status drain_elf_sources(
    struct kernel_mm_record *record,
    int unused_only)
{
    struct kernel_mm_elf_source **link = &record->elf_sources;
    int invalid=0,defer=kernel_lock_held(&record->operations.lock,1);
    struct kernel_mm_elf_source *released=defer ? NULL : record->retired_elf_sources;
    if (released) record->retired_elf_sources=NULL;

    while (*link != 0) {
        struct kernel_mm_elf_source *entry = *link;
        int in_use = 0;

        if (entry->faults || entry->draining) {
            if (!unused_only) __builtin_trap();
            link = &entry->next;
            continue;
        }
        if (entry->source != 0) {
            if (unused_only != 0) {
                if (record->vmas == 0 ||
                    kernel_vma_set_backing_in_use(record->vmas,
                                                  entry->source,
                                                  &in_use) !=
                        KERNEL_VMA_STATUS_OK) {
                    invalid=1; break;
                }
            }
            if (in_use != 0) {
                link = &entry->next;
                continue;
            }
        }
        *link = entry->next;
        entry->draining=1;
        if (defer) {
            entry->next=record->retired_elf_sources; record->retired_elf_sources=entry;
        } else { entry->next=released; released=entry; }
    }
    int failed=release_elf_sources(record,released);
    if (invalid) return KERNEL_MM_STATUS_STATE;
    return failed != 0 ? KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       : KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status clone_elf_sources(
    const struct kernel_mm_record *source_record,
    struct kernel_mm_record *destination_record)
{
    struct kernel_mm_elf_source *source;

    for (source = source_record->elf_sources;
         source != 0;
         source = source->next) {
        struct kernel_mm_elf_source *copy;
        int in_use;
        enum kernel_heap_status heap_status;

        if (source->source == 0 ||
            kernel_vma_set_backing_in_use(source_record->vmas,
                                          source->source,
                                          &in_use) != KERNEL_VMA_STATUS_OK) {
            return KERNEL_MM_STATUS_STATE;
        }
        if (in_use == 0) {
            continue;
        }
        heap_status = kernel_heap_allocate_zeroed(destination_record->vma_heap,
                                                  1U,
                                                  sizeof(*copy),
                                                  (void **)&copy);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            return heap_status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        }
        if (kernel_elf64_source_acquire(source->source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
            kernel_heap_release(destination_record->vma_heap, copy);
            return KERNEL_MM_STATUS_STATE;
        }
        copy->source = source->source;
        copy->next = destination_record->elf_sources;
        destination_record->elf_sources = copy;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status clone_file_sources(
    const struct kernel_mm_record *source_record,
    struct kernel_mm_record *destination_record)
{
    struct kernel_mm_file_source *source;

    for (source = source_record->file_sources;
         source != 0;
         source = source->next) {
        struct kernel_mm_file_source *copy;
        int in_use;
        enum kernel_heap_status heap_status;

        if (source->file == 0) {
            continue;
        }
        if (kernel_vma_set_backing_in_use(source_record->vmas,
                                          source->file,
                                          &in_use) !=
            KERNEL_VMA_STATUS_OK) {
            return KERNEL_MM_STATUS_STATE;
        }
        if (in_use == 0) {
            continue;
        }
        heap_status = kernel_heap_allocate_zeroed(
            destination_record->vma_heap,
            1U,
            sizeof(*copy),
            (void **)&copy);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            return heap_status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        }
        if (kernel_open_file_acquire(source->file) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            kernel_heap_release(destination_record->vma_heap,
                                    copy);
            return KERNEL_MM_STATUS_STATE;
        }
        copy->file = source->file;
        copy->next = destination_record->file_sources;
        destination_record->file_sources = copy;
    }
    return KERNEL_MM_STATUS_OK;
}

static void drain_shared_anon(struct kernel_mm_record *record,
                              int unused_only)
{
    struct kernel_mm_shared_anon **link = &record->shared_anon;

    while (*link != 0) {
        struct kernel_mm_shared_anon *entry = *link;
        int in_use = 0;

        if (unused_only != 0 && record->vmas != 0 &&
            kernel_vma_set_backing_in_use(record->vmas, entry->object,
                                          &in_use) != KERNEL_VMA_STATUS_OK)
            __builtin_trap();
        if (in_use != 0) {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        kernel_memory_object_release(&entry->object);
        kernel_heap_release(record->vma_heap, entry);
    }
}

static enum kernel_mm_status clone_shared_anon(
    const struct kernel_mm_record *source_record,
    struct kernel_mm_record *destination_record)
{
    for (const struct kernel_mm_shared_anon *source =
             source_record->shared_anon;
         source != 0; source = source->next) {
        struct kernel_mm_shared_anon *copy;
        enum kernel_heap_status status;

        status = kernel_heap_allocate_zeroed(destination_record->vma_heap,
                                              1U, sizeof(*copy),
                                              (void **)&copy);
        if (status != KERNEL_HEAP_STATUS_OK)
            return status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        if (kernel_memory_object_acquire(source->object) !=
            KERNEL_MEMORY_OBJECT_OK) {
            kernel_heap_release(destination_record->vma_heap, copy);
            return KERNEL_MM_STATUS_STATE;
        }
        copy->object = source->object;
        copy->next = destination_record->shared_anon;
        destination_record->shared_anon = copy;
    }
    return KERNEL_MM_STATUS_OK;
}

static int valid_vma_range(uint64_t start,
                           uint64_t end,
                           uint32_t permissions)
{
    uint32_t known = KERNEL_MM_READ | KERNEL_MM_WRITE |
                     KERNEL_MM_EXECUTE;

    return start >= ARCH_MMU_PAGE_SIZE && start < end &&
           end <= ARCH_MMU_USER_LIMIT &&
           (start & BOAROS_PAGE_MASK) == 0U &&
           (end & BOAROS_PAGE_MASK) == 0U &&
           (permissions & ~known) == 0U &&
           (permissions & known) != 0U &&
           !((permissions & KERNEL_MM_WRITE) != 0U &&
             (permissions & KERNEL_MM_READ) == 0U);
}

static enum kernel_mm_status mutable_vma_record(
    struct kernel_mm *mm,
    struct kernel_mm_record **record);

static int shared_anon_leaf(void *context, uint64_t virtual_address)
{
    const struct kernel_vma_set *vmas = context;
    struct kernel_vma vma;
    enum kernel_vma_status status = kernel_vma_set_lookup(
        vmas, virtual_address, &vma);

    if (status == KERNEL_VMA_STATUS_NOT_FOUND) return 0;
    if (status != KERNEL_VMA_STATUS_OK) __builtin_trap();
    return vma.kind == KERNEL_VMA_KIND_ANON_SHARED ||
           vma.kind == KERNEL_VMA_KIND_FILE_SHARED ||
           vma.kind == KERNEL_VMA_KIND_SYSV_SHM;
}

struct fork_file_guards {
    struct kernel_vfs_node **nodes;
    struct kernel_lock_guard *guards;
    struct kernel_heap *heap;
    size_t count;
};
static void fork_file_guards_end(struct fork_file_guards *owner)
{
    if (!owner->nodes) return;
    for (size_t i=owner->count; i--;) if (owner->guards[i].lock)
        kernel_lock_release(&owner->guards[i]);
    kernel_heap_release(owner->heap,owner->nodes);
    *owner=(struct fork_file_guards){0};
}
static void sift_file_nodes(struct kernel_vfs_node **nodes,size_t root,size_t count)
{
    while (root*2+1 < count) {
        size_t child=root*2+1;
        if (child+1<count && (uintptr_t)nodes[child+1]>(uintptr_t)nodes[child]) child++;
        if ((uintptr_t)nodes[root]>=(uintptr_t)nodes[child]) break;
        struct kernel_vfs_node *node=nodes[root]; nodes[root]=nodes[child]; nodes[child]=node;
        root=child;
    }
}
static enum kernel_mm_status fork_file_guards_begin(struct kernel_mm_record *record,
    struct fork_file_guards *owner)
{
    if (!record->file_mappings) return KERNEL_MM_STATUS_OK;
    if (!kernel_lock_held(&record->operations.lock,1)) __builtin_trap();
    size_t count=0, bytes=sizeof(*owner->nodes)+sizeof(*owner->guards);
    for (struct mm_file_mapping *m=record->file_mappings; m; m=m->next) {
        if (++count > SIZE_MAX/bytes) return KERNEL_MM_STATUS_NO_MEMORY;
    }
    enum kernel_heap_status hs=kernel_heap_allocate_zeroed(record->vma_heap,count,bytes,(void **)&owner->nodes);
    if (hs != KERNEL_HEAP_STATUS_OK)
        return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
    owner->heap=record->vma_heap; owner->count=count;
    owner->guards=(void *)(owner->nodes+count);
    size_t index=0;
    for (struct mm_file_mapping *m=record->file_mappings; m; m=m->next)
        owner->nodes[index++]=m->registration.node;
    /* MM gate固定来源和inode寿命；只借用，不增加一层对象引用。
     * 地址锁序用O(F log F)私有排序，避免源文件数平方的准备成本。 */
    for (size_t i=count/2; i--;) sift_file_nodes(owner->nodes,i,count);
    for (size_t i=count; --i;) {
        struct kernel_vfs_node *node=owner->nodes[0];
        owner->nodes[0]=owner->nodes[i]; owner->nodes[i]=node;
        sift_file_nodes(owner->nodes,0,i);
    }
    for (size_t i=0; i<count; i++) {
        kernel_vfs_node_lock(owner->nodes[i],&owner->guards[i],0);
        uint64_t epoch;
        if (kernel_file_mapping_epoch(owner->nodes[i],&epoch)) {
            fork_file_guards_end(owner);
            return KERNEL_MM_STATUS_CONFLICT;
        }
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status mm_fork_impl(
    struct kernel_mm *destination,
    struct kernel_mm *source)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *source_record;
    struct kernel_mm_record *destination_record;
    uint64_t record_page_address;
    void *pointer;
    enum physical_page_status page_status;
    enum kernel_mm_status status;
    enum kernel_mm_status failure;
    enum arch_mmu_status backend_status;
    struct fork_file_guards files __attribute__((cleanup(fork_file_guards_end)))={0};

    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(destination) || source->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(source, &source_record);
    if (status != KERNEL_MM_STATUS_OK ||
        source_record->stage != KERNEL_MM_RECORD_LIVE) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    if (!__atomic_load_n(&next_futex_mm_id, __ATOMIC_ACQUIRE)) return KERNEL_MM_STATUS_NO_MEMORY;
    page_status = physical_page_allocate(source->allocator,
                                         &record_page_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_MM_STATUS_STATE;
    }
    if (physical_page_map_owned(source->allocator,
                              record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return abandon_unresolved_record(destination,
                                         source->allocator,
                                         record_page_address,
                                         KERNEL_MM_STATUS_PAGE_ACCESS);
    }
    clear_page(pointer);
    destination_record = pointer;
    destination_record->futex_id = reserve_mm_identity();
    if (!destination_record->futex_id) return abandon_unresolved_record(destination,
        source->allocator, record_page_address, KERNEL_MM_STATUS_NO_MEMORY);
    destination_record->references = 1U;
    kernel_raw_lock_init(&destination_record->metadata, KERNEL_RAW_RANK_MM);
    kernel_mutex_init(&destination_record->operations, 25, (uintptr_t)destination_record);
    kernel_wait_queue_init(&destination_record->pins_changed);
    destination_record->stage = KERNEL_MM_RECORD_LIVE;
    destination_record->vma_heap = source_record->vma_heap;

    /*
     * Clone allocation-bearing metadata before sharing leaves.  The Sv39
     * fork commit removes write permission from the parent and therefore is
     * intentionally the final fallible construction step.
     */
    if (source_record->vmas != 0) {
        status = status_from_vma(kernel_vma_set_clone(
            source_record->vmas,
            &destination_record->vmas));
        if (status != KERNEL_MM_STATUS_OK) {
            if (destination_record->vmas != 0) {
                destination_record->stage =
                    KERNEL_MM_RECORD_VMAS_CLEANUP;
                destination->allocator = source->allocator;
                destination->record = destination_record;
                destination->record_page_address = record_page_address;
                destination->state = KERNEL_MM_CLEANUP;
                destination->cleanup_stage = KERNEL_MM_CLEANUP_VMAS;
                if (kernel_mm_release(destination) != KERNEL_MM_STATUS_OK) {
                    return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
                }
                finish_handle(destination, KERNEL_MM_EMPTY);
            } else (void)physical_page_release(source->allocator,
                                             record_page_address);
            return status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_MM_STATUS_STATE
                       : status;
        }
    }
    if (destination_record->vmas) kernel_vma_set_clear_locks(destination_record->vmas);
    status = clone_shared_anon(source_record, destination_record);
    if (status == KERNEL_MM_STATUS_OK)
        status = clone_file_sources(source_record, destination_record);
    if (status != KERNEL_MM_STATUS_OK) {
        destination_record->stage = KERNEL_MM_RECORD_VMAS_CLEANUP;
        destination->allocator = source->allocator;
        destination->record = destination_record;
        destination->record_page_address = record_page_address;
        destination->state = KERNEL_MM_CLEANUP;
        destination->cleanup_stage = KERNEL_MM_CLEANUP_VMAS;
        if (kernel_mm_release(destination) != KERNEL_MM_STATUS_OK) {
            return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
        }
        finish_handle(destination, KERNEL_MM_EMPTY);
        return status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                   ? KERNEL_MM_STATUS_STATE
                   : status;
    }
    /* Reserve per-node registration and resident provenance before the final
     * PTE-sharing commit. Unpublished records are also handled by release. */
    for (struct kernel_mm_file_source *source = destination_record->file_sources;
         source != 0 && status == KERNEL_MM_STATUS_OK; source = source->next) {
        struct mm_file_mapping *prepared;
        status = prepare_file_registration(destination_record, source->file,
                                             &prepared);
        if (status == KERNEL_MM_STATUS_OK && prepared != 0) {
            prepared->next = destination_record->file_mappings;
            destination_record->file_mappings = prepared;
        }
    }
    if (status == KERNEL_MM_STATUS_OK)
        status = reserve_file_residents(destination_record, source_record->resident_count);
    for (struct mm_file_resident *page = source_record->file_residents;
         page != 0 && status == KERNEL_MM_STATUS_OK; page = page->next) {
        struct mm_file_resident *copy;
        enum kernel_heap_status hs = kernel_heap_allocate_zeroed(
            destination_record->vma_heap, 1U, sizeof(*copy), (void **)&copy);
        if (hs != KERNEL_HEAP_STATUS_OK) {
            status = hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                                     : KERNEL_MM_STATUS_STATE;
            break;
        }
        *copy = *page;
        copy->alias=(struct kernel_page_cache_alias){0};
        copy->clone_source = page;
        copy->cache_retry_registered=0; copy->cache_retry_next=0;
        { KERNEL_RAW_SCOPE(guard, &destination_record->metadata);
          publish_file_resident(destination_record, copy); }
    }
    if (status == KERNEL_MM_STATUS_OK)
        status = clone_elf_sources(source_record, destination_record);
    if (status == KERNEL_MM_STATUS_OK && source_record->executable_file) {
        if (kernel_open_file_acquire(source_record->executable_file) !=
            KERNEL_OPEN_FILE_STATUS_OK)
            status = KERNEL_MM_STATUS_STATE;
        else destination_record->executable_file =
                source_record->executable_file;
    }
    if (status != KERNEL_MM_STATUS_OK) {
        destination_record->stage = KERNEL_MM_RECORD_VMAS_CLEANUP;
        destination->allocator = source->allocator;
        destination->record = destination_record;
        destination->record_page_address = record_page_address;
        destination->state = KERNEL_MM_CLEANUP;
        destination->cleanup_stage = KERNEL_MM_CLEANUP_VMAS;
        if (kernel_mm_release(destination) != KERNEL_MM_STATUS_OK) {
            return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
        }
        finish_handle(destination, KERNEL_MM_EMPTY);
        return status;
    }

    status=fork_file_guards_begin(source_record,&files);
    if (status != KERNEL_MM_STATUS_OK) {
        destination_record->stage=KERNEL_MM_RECORD_VMAS_CLEANUP;
        destination->allocator=source->allocator;
        destination->record=destination_record;
        destination->record_page_address=record_page_address;
        destination->state=KERNEL_MM_CLEANUP;
        destination->cleanup_stage=KERNEL_MM_CLEANUP_VMAS;
        if (kernel_mm_release(destination)) __builtin_trap();
        finish_handle(destination,KERNEL_MM_EMPTY);
        return status;
    }
    backend_status = arch_mmu_user_space_fork_prepare(&destination_record->space,
        &source_record->space, source_record->vmas ? shared_anon_leaf : 0, source_record->vmas);
    if (backend_status == ARCH_MMU_STATUS_OK) {
        uint64_t cursor = 0, address;
        /* child尚未发布，遍历私有树不持MM raw；每次只提交一个父叶。 */
        while ((backend_status = arch_mmu_user_next_owned(&destination_record->space,
            &cursor, ARCH_MMU_USER_LIMIT, &address)) == ARCH_MMU_STATUS_OK) {
            /* 按VMA语义分类，不能把合法shared/protected页当作私有COW。 */
            if (source_record->vmas && shared_anon_leaf(source_record->vmas, address)) continue;
            KERNEL_RAW_SCOPE(guard, &source_record->metadata);
            if (arch_mmu_user_mark_cow_page(&source_record->space, address) !=
                ARCH_MMU_STATUS_OK) __builtin_trap();
        }
        if (backend_status != ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
        struct kernel_mm_retirement batch = {0};
        uint32_t targets;
        {
            KERNEL_RAW_SCOPE(guard, &source_record->metadata);
            targets = note_tlb_change_locked(source_record, &batch, 0);
        }
        finish_tlb_change(source_record, &batch, targets, 0, UINT64_MAX, 0);
        backend_status = ARCH_MMU_STATUS_OK;
    }
    if (backend_status == ARCH_MMU_STATUS_OK) {
        for (struct mm_file_resident *page = destination_record->file_residents;
             page != 0; page = page->next) {
            if (page->clone_source != 0 &&
                kernel_page_cache_alias_active(&page->clone_source->alias)) {
                /* 新别名不在已捕获的快照游标中，发布前必须保持只读。 */
                {
                    struct kernel_vma vma;
                    if (kernel_vma_set_lookup(destination_record->vmas,page->address,&vma) ||
                        arch_mmu_user_protect_owned_page(&destination_record->space,page->address,
                            backend_permissions_from_mm(vma.permissions & ~KERNEL_MM_WRITE))) __builtin_trap();
                }
                kernel_page_cache_alias_clone(&page->alias,
                    &page->clone_source->alias, page);
            }
            page->clone_source = 0;
        }
        destination_record->start_code = source_record->start_code;
        destination_record->end_code = source_record->end_code;
        destination_record->start_stack = source_record->start_stack;
        destination_record->start_data = source_record->start_data;
        destination_record->end_data = source_record->end_data;
        destination_record->stack_top = source_record->stack_top;
        destination_record->start_brk = source_record->start_brk;
        destination_record->current_brk = source_record->current_brk;
        destination_record->brk_limit = source_record->brk_limit;
        destination_record->mmap_base = source_record->mmap_base;
        destination_record->vdso_address = source_record->vdso_address;
        destination_record->brk_initialized =
            source_record->brk_initialized;
        /* 可失败准备和父失效已完成。inode读保护阻止关闭新登记，回调只会
         * 看到完整的child来源/VMA/页表/驻留记录，不存在半构造发布。 */
        for (struct mm_file_mapping *entry=destination_record->file_mappings; entry; entry=entry->next)
            if (kernel_file_mapping_register(&entry->registration)) __builtin_trap();
        destination->allocator = source->allocator;
        destination->record = destination_record;
        destination->record_page_address = record_page_address;
        destination->state = KERNEL_MM_LIVE;
        destination->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
        return KERNEL_MM_STATUS_OK;
    }

    fork_file_guards_end(&files);
    failure = fork_status_from_backend(backend_status);
    destination_record->stage = KERNEL_MM_RECORD_VMAS_CLEANUP;
    destination->allocator = source->allocator;
    destination->record = destination_record;
    destination->record_page_address = record_page_address;
    destination->state = KERNEL_MM_CLEANUP;
    destination->cleanup_stage = KERNEL_MM_CLEANUP_VMAS;
    if (kernel_mm_release(destination) != KERNEL_MM_STATUS_OK) {
        return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
    }
    finish_handle(destination, KERNEL_MM_EMPTY);
    return failure == KERNEL_MM_STATUS_CLEANUP_REQUIRED
               ? KERNEL_MM_STATUS_STATE
               : failure;
}

enum kernel_mm_status kernel_mm_move(
    struct kernel_mm *destination,
    struct kernel_mm *source)
{
    KERNEL_NO_RECLAIM_IO;
    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(destination) || !owner_handle(source)) {
        return KERNEL_MM_STATUS_STATE;
    }
    *destination = *source;
    finish_handle(source, KERNEL_MM_MOVED);
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status lookup_record(
    const struct kernel_mm_record *record,
    uint64_t virtual_address,
    struct kernel_mm_mapping *mapping)
{
    struct arch_mmu_mapping riscv_mapping;
    enum arch_mmu_status backend_status;
    backend_status = arch_mmu_user_lookup(&record->space,
                                         virtual_address,
                                         &riscv_mapping);
    if (backend_status == ARCH_MMU_STATUS_NOT_MAPPED) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    if (backend_status != ARCH_MMU_STATUS_OK) {
        return backend_status == ARCH_MMU_STATUS_INVALID
                   ? KERNEL_MM_STATUS_INVALID_ARGUMENT
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    mapping->physical_address = riscv_mapping.physical_address;
    mapping->permissions = 0U;
    if ((riscv_mapping.permissions & ARCH_MMU_READ) != 0U) {
        mapping->permissions |= KERNEL_MM_READ;
    }
    if ((riscv_mapping.permissions & ARCH_MMU_WRITE) != 0U) {
        mapping->permissions |= KERNEL_MM_WRITE;
    }
    if ((riscv_mapping.permissions & ARCH_MMU_EXECUTE) != 0U) {
        mapping->permissions |= KERNEL_MM_EXECUTE;
    }
    if ((riscv_mapping.permissions & ARCH_MMU_USER) != 0U) {
        mapping->permissions |= KERNEL_MM_USER;
    }
    return KERNEL_MM_STATUS_OK;
}

/* gate串行映射编辑；MM raw保护PTE、resident索引和pin许可。
 * 缺页候选在锁外准备，重新取得gate并核对映射后才发布。 */
struct mm_operation {
    struct kernel_mm_record *record;
    const struct kernel_mm *mm;
    struct kernel_lock_guard guard;
};
static uint32_t note_tlb_change_locked(struct kernel_mm_record *record,
    const struct kernel_mm_retirement *batch, int instructions)
{
    if (record->tlb_generation == UINT64_MAX ||
        (batch->count && record->pending_retirement == UINT32_MAX)) __builtin_trap();
    record->tlb_generation++;
    if (instructions) {
        if (record->instruction_generation == UINT64_MAX) __builtin_trap();
        record->instruction_generation++;
    }
    if (batch->count) {
        record->pending_retirement++;
#if BOAROS_COST_DIAGNOSTICS
        record->retirement_batches++;
        record->retired_pages += batch->count;
#endif
    }
    return record->active_cpus;
}
static void finish_tlb_change(struct kernel_mm_record *record,
    struct kernel_mm_retirement *batch, uint32_t targets, uint64_t address,
    uint64_t size, int instructions)
{
    arch_mmu_sync_user(targets, address, size, instructions);
    for (unsigned i = 0; i < batch->count; i++)
        if (physical_page_release(record->space.allocator, batch->pages[i]) !=
            PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (batch->count) {
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        if (!record->pending_retirement) __builtin_trap();
        record->pending_retirement--;
    }
    *batch = (struct kernel_mm_retirement){0};
}
static void wake_pin_waiters(struct kernel_mm_record *record)
{
    KERNEL_IRQ_SCOPE(irq);
    enum kernel_scheduler_status status=kernel_wait_queue_wake_all(&record->pins_changed);
    if (status != KERNEL_SCHEDULER_STATUS_OK && status != KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED)
        __builtin_trap();
}
static void mm_operation_end(struct mm_operation *operation)
{
    if (operation->guard.lock) {
        struct kernel_mm_record *record = operation->record;
        struct mm_file_mapping *retired=record->retired_file_mappings;
        record->retired_file_mappings=0;
        struct mm_file_resident *residents=record->retired_file_residents;
        record->retired_file_residents=0;
        struct kernel_mm_file_source *files=record->retired_file_sources;
        if (files) record->retired_file_sources=0;
        struct kernel_mm_elf_source *elfs=record->retired_elf_sources;
        if (elfs) record->retired_elf_sources=0;
        if (record->access_closed) {
            KERNEL_RAW_SCOPE(guard, &record->metadata);
            record->access_closed = 0;
            kernel_raw_lock_release(&guard);
            wake_pin_waiters(record);
        }
        mm_gate_release(operation->record,&operation->guard);
        drain_retired_residents(record,residents);
        while (retired) {
            struct mm_file_mapping *entry=retired;
            retired=entry->next;
            kernel_file_mapping_drain(&entry->registration);
            kernel_heap_release(record->vma_heap,entry);
        }
        /* Registration callbacks still borrow the node; their drain must end
         * before its final OFD/ELF owner can close the backend. */
        (void)release_file_sources(record,files);
        (void)release_elf_sources(record,elfs);
    }
}
static int current_pins(const struct kernel_mm *mm)
{
    for (const struct kernel_mm_user_pin *pin = kernel_io_context_current()->user_pins;
         pin; pin = pin->owner_next)
        if (pin->mm->allocator == mm->allocator &&
            pin->mm->record == mm->record) return 1;
    return 0;
}
static enum kernel_mm_status mm_operation_lock(struct mm_operation *operation)
{
    /* gate持有者可能正排空本owner的pin；不把仍有义务归还的owner排进等待环。 */
    if (current_pins(operation->mm)) {
        if (!kernel_mutex_try_lock(&operation->record->operations, &operation->guard))
            return KERNEL_MM_STATUS_CONFLICT;
    } else kernel_mutex_lock(&operation->record->operations, &operation->guard);
    return KERNEL_MM_STATUS_OK;
}
static enum kernel_mm_status mm_operation_begin(const struct kernel_mm *mm,
    struct mm_operation *operation)
{
    if (!mm) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    operation->mm = mm;
    enum kernel_mm_status status = resolve_record(mm, &operation->record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    return mm_operation_lock(operation);
}
/* 在MM raw内登记并复查，解锁后park；pin归还/开放许可都在锁外wake。
 * 注册owner由局部token持有，退出函数前finish，不携带裸队列游标。 */
static void wait_pin_condition(struct kernel_mm_record *record, int drain)
{
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            KERNEL_RAW_SCOPE(guard, &record->metadata);
            int blocked = drain ? (record->access_closed == 2 ? record->write_pins :
                record->access_pins) != 0 : record->access_closed != 0;
            if (!blocked) return;
#if BOAROS_COST_DIAGNOSTICS
            record->pin_waits++;
#endif
            if (kernel_wait_prepare(&record->pins_changed, 0, 0, &token) !=
                KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
}
static void close_pins(struct mm_operation *operation, int writers_only)
{
    if (!operation->guard.lock) return;
    /* 不等待自身尚未结束的片段；munmap不排空pin，仍允许同owner撤映射。 */
    for (struct kernel_mm_user_pin *pin = kernel_io_context_current()->user_pins;
         pin; pin = pin->owner_next)
        if (pin->mm->allocator == operation->mm->allocator &&
            pin->mm->record == operation->mm->record &&
            (!writers_only || (pin->access & KERNEL_MM_WRITE)))
            __builtin_trap();
    {
        KERNEL_RAW_SCOPE(guard, &operation->record->metadata);
        if (operation->record->access_closed) __builtin_trap();
        operation->record->access_closed = writers_only ? 2 : 1;
    }
    wait_pin_condition(operation->record, 1);
}

enum kernel_mm_status kernel_mm_lookup(const struct kernel_mm *mm,
    uint64_t address, struct kernel_mm_mapping *mapping)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    if (!mm || !mapping) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (mm->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    if (record->stage != KERNEL_MM_RECORD_LIVE) return KERNEL_MM_STATUS_STATE;
    return lookup_record(record, address, mapping);
}

/* PTE、resident索引和许可取得共用MM raw；复制不取得映射编辑gate。 */
static __attribute__((always_inline)) inline enum kernel_mm_status pin_mapping(const struct kernel_mm *mm,
    struct kernel_mm_record *record, uint64_t address, uint32_t access,
    struct kernel_mm_user_pin *pin, int cache_write)
{
    struct kernel_mm_mapping mapping;
    void *pointer;
    enum kernel_mm_status status = lookup_record(record, address, &mapping);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if ((mapping.permissions & (access | KERNEL_MM_USER)) !=
        (access | KERNEL_MM_USER)) return KERNEL_MM_STATUS_ACCESS;
    uint64_t physical = mapping.physical_address & ~BOAROS_PAGE_MASK;
    /* permission许可和取得实际页引用不能被撤映射插入。 */
    if (physical_page_map_owned(mm->allocator, physical, &pointer) !=
        PHYSICAL_PAGE_STATUS_OK) return KERNEL_MM_STATUS_PAGE_ACCESS;
    if (record->access_pins == UINT32_MAX || record->write_pins == UINT32_MAX)
        __builtin_trap();
    if (cache_write) {
        struct mm_file_resident *resident=find_file_resident(record,address&~BOAROS_PAGE_MASK);
        if (resident && kernel_page_cache_alias_active(&resident->alias)) {
            enum kernel_page_cache_status admitted=kernel_page_cache_alias_write_begin(&resident->alias,&pin->cache_write);
            if (admitted==KERNEL_PAGE_CACHE_STATUS_BUSY) return KERNEL_MM_STATUS_ACCESS;
            if (admitted) __builtin_trap();
        }
    }
    if (physical_page_acquire(mm->allocator, physical) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    record->access_pins++;
#if BOAROS_COST_DIAGNOSTICS
    record->pin_acquisitions++;
#endif
    if (access & KERNEL_MM_WRITE) record->write_pins++;
    struct kernel_io_context *owner = kernel_io_context_current();
    pin->mm=mm; pin->record=record; pin->owner=owner; pin->registration=pin;
    pin->owner_next=owner->user_pins; pin->owner_previous=&owner->user_pins;
    if (pin->owner_next) pin->owner_next->owner_previous=&pin->owner_next;
    pin->page=pointer; pin->physical_address=physical; pin->access=access;
    owner->user_pins = pin;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_pin_user_page(const struct kernel_mm *mm,
    uint64_t address, uint32_t access, struct kernel_mm_user_pin *pin)
{
    struct kernel_mm_record *record;
    if (!mm || !pin || !access ||
        (access & ~(KERNEL_MM_READ | KERNEL_MM_WRITE | KERNEL_MM_EXECUTE)))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (pin->registration) __builtin_trap();
    if (mm->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
retry:
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    if (record->access_closed == 1 ||
        (record->access_closed == 2 && (access & KERNEL_MM_WRITE))) {
        if (current_pins(mm)) return KERNEL_MM_STATUS_CONFLICT;
        kernel_raw_lock_release(&guard);
        wait_pin_condition(record, 0);
        goto retry;
    }
    return pin_mapping(mm,record,address,access,pin,(access & KERNEL_MM_WRITE)!=0);
}


void kernel_mm_unpin_user_page(struct kernel_mm_user_pin *pin)
{
    if (!pin || pin->registration != pin ||
        pin->owner != kernel_io_context_current() || !pin->mm ||
        pin->mm->state != KERNEL_MM_LIVE || !pin->record ||
        !pin->owner_previous || *pin->owner_previous != pin)
        __builtin_trap();
    /* 页借用已由存活MM持有；归还同一记录，不重新解析整个地址空间。 */
    struct kernel_mm_record *record=pin->record;
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    *pin->owner_previous = pin->owner_next;
    if (pin->owner_next) pin->owner_next->owner_previous=pin->owner_previous;
    if (!record->access_pins ||
        ((pin->access & KERNEL_MM_WRITE) && !record->write_pins)) __builtin_trap();
    record->access_pins--;
    if (pin->access & KERNEL_MM_WRITE) record->write_pins--;
    if (physical_page_release(pin->mm->allocator, pin->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) __builtin_trap();

    int wake = record->access_closed &&
        (record->access_closed == 2 ? !record->write_pins : !record->access_pins);
    kernel_raw_lock_release(&guard);
    if (pin->cache_write.entry) kernel_page_cache_write_end(&pin->cache_write);
    *pin=(struct kernel_mm_user_pin){0};
    if (wake) wake_pin_waiters(record);
}

enum kernel_mm_status kernel_mm_vma_enable(
    struct kernel_mm *mm,
    struct kernel_heap *heap)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || heap == 0 || heap->page_allocator != mm->allocator) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE ||
        __atomic_load_n(&record->references, __ATOMIC_ACQUIRE) != 1U || record->vmas != 0) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    status = status_from_vma(kernel_vma_set_create(heap, &record->vmas));
    if (status == KERNEL_MM_STATUS_OK) {
        record->vma_heap = heap;
    }
    return status;
}

enum kernel_mm_status kernel_mm_vma_insert_anon(
    struct kernel_mm *mm,
    uint64_t start,
    uint64_t end,
    uint32_t permissions,
    enum kernel_vma_role role,
    enum kernel_vma_fault_policy fault_policy)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_vma vma = {0};
    enum kernel_mm_status status;

    if (mm == 0 || !valid_vma_range(start, end, permissions) ||
        role < KERNEL_VMA_ROLE_NONE || role > KERNEL_VMA_ROLE_MMAP ||
        fault_policy < KERNEL_VMA_FAULT_RESIDENT_REQUIRED ||
        fault_policy > KERNEL_VMA_FAULT_DEMAND_ZERO) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE ||
        __atomic_load_n(&record->references, __ATOMIC_ACQUIRE) != 1U || record->vmas == 0) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    vma.start = start;
    vma.end = end;
    vma.backing_offset = 0U;
    vma.permissions = permissions;
    vma.maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS;
    vma.kind = KERNEL_VMA_KIND_ANONYMOUS;
    vma.role = role;
    vma.fault_policy = fault_policy;
    vma.backing = 0;
    status = status_from_vma(kernel_vma_set_insert(record->vmas, &vma));
    if (!status && role == KERNEL_VMA_ROLE_STACK) record->stack_top = end;
    return status;
}

static uint32_t mm_permissions_from_elf_flags(uint32_t flags)
{
    uint32_t permissions = 0U;

    if ((flags & KERNEL_ELF64_FLAG_READ) != 0U) {
        permissions |= KERNEL_MM_READ;
    }
    if ((flags & KERNEL_ELF64_FLAG_WRITE) != 0U) {
        permissions |= KERNEL_MM_WRITE | KERNEL_MM_READ;
    }
    if ((flags & KERNEL_ELF64_FLAG_EXECUTE) != 0U) {
        permissions |= KERNEL_MM_EXECUTE;
    }
    return permissions;
}

enum kernel_mm_status kernel_mm_map_elf_source(
    struct kernel_mm *mm,
    struct kernel_elf64_source *source,
    uint64_t load_bias)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_mm_elf_source *source_owner = 0;
    uint32_t run_count;
    uint32_t index;
    uint32_t inserted = 0U;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;
    enum kernel_heap_status heap_status;

    if (mm == 0 || source == 0 ||
        load_bias > ARCH_MMU_USER_LIMIT - ARCH_MMU_PAGE_SIZE) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (find_elf_source(record, source) == 0) {
        heap_status = kernel_heap_allocate_zeroed(record->vma_heap,
                                                  1U,
                                                  sizeof(*source_owner),
                                                  (void **)&source_owner);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            return heap_status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        }
        if (kernel_elf64_source_acquire(source) !=
            KERNEL_ELF64_SOURCE_STATUS_OK) {
            kernel_heap_release(record->vma_heap, source_owner);
            return KERNEL_MM_STATUS_STATE;
        }
        source_owner->source = source;
        source_owner->next = record->elf_sources;
        record->elf_sources = source_owner;
    }
    run_count = kernel_elf64_source_run_count(source);
    if (run_count == 0U) {
        if (source_owner != 0) {
            (void)drain_elf_sources(record, 1);
        }
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < run_count; index++) {
        const struct kernel_elf64_source_run *run =
            kernel_elf64_source_run_at(source, index);
        uint64_t start;
        uint64_t end;
        int overlaps;

        if (run == 0 || run->start >= run->end ||
            load_bias > UINT64_MAX - run->start ||
            load_bias + run->start < ARCH_MMU_PAGE_SIZE ||
            load_bias > UINT64_MAX - run->end ||
            load_bias + run->end > ARCH_MMU_USER_LIMIT ||
            (run->start & BOAROS_PAGE_MASK) != 0U ||
            (run->end & BOAROS_PAGE_MASK) != 0U ||
            mm_permissions_from_elf_flags(run->flags) == 0U) {
            status = KERNEL_MM_STATUS_ADDRESS_SPACE;
            goto rollback;
        }
        start = load_bias + run->start;
        end = load_bias + run->end;
        vma_status = kernel_vma_set_overlaps(record->vmas,
                                             start,
                                             end,
                                             &overlaps);
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            status = status_from_vma(vma_status);
            goto rollback;
        }
        if (overlaps != 0) {
            status = KERNEL_MM_STATUS_CONFLICT;
            goto rollback;
        }
    }
    for (index = 0U; index < run_count; index++) {
        const struct kernel_elf64_source_run *run =
            kernel_elf64_source_run_at(source, index);
        struct kernel_vma vma = {
            .start = load_bias + run->start,
            .end = load_bias + run->end,
            .backing_offset = run->start,
            .permissions = mm_permissions_from_elf_flags(run->flags),
            .maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS,
            .kind = KERNEL_VMA_KIND_ELF_PRIVATE,
            .role = KERNEL_VMA_ROLE_ELF,
            .fault_policy = KERNEL_VMA_FAULT_ELF,
            .backing = source,
        };

        vma_status = kernel_vma_set_insert(record->vmas, &vma);
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            status = status_from_vma(vma_status);
            goto rollback;
        }
        inserted++;
    }
    return KERNEL_MM_STATUS_OK;

rollback:
    while (inserted != 0U) {
        const struct kernel_elf64_source_run *run =
            kernel_elf64_source_run_at(source, inserted - 1U);
        struct kernel_vma_edit edit;

        if (run == 0 ||
            kernel_vma_set_prepare_replace(record->vmas,
                                            load_bias + run->start,
                                            load_bias + run->end,
                                            0,
                                            &edit) != KERNEL_VMA_STATUS_OK ||
            kernel_vma_set_commit_edit(record->vmas, &edit) !=
                KERNEL_VMA_STATUS_OK) {
            status = KERNEL_MM_STATUS_CLEANUP_REQUIRED;
            break;
        }
        inserted--;
    }
    if (source_owner != 0) {
        if (drain_elf_sources(record, 1) != KERNEL_MM_STATUS_OK) {
            status = KERNEL_MM_STATUS_CLEANUP_REQUIRED;
        }
    }
    return status;
}

enum kernel_mm_status kernel_mm_set_executable(
    struct kernel_mm *mm, struct kernel_open_file_description *file)
{
    struct kernel_mm_record *record;
    enum kernel_mm_status status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (!file || record->executable_file || !kernel_open_file_path(file) ||
        !record->elf_sources) return KERNEL_MM_STATUS_STATE;
    if (kernel_open_file_acquire(file) != KERNEL_OPEN_FILE_STATUS_OK)
        return KERNEL_MM_STATUS_STATE;
    record->executable_file = file;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_executable_path_acquire(
    const struct kernel_mm *mm, struct kernel_vfs_path **owner)
{
    struct kernel_mm_record *record;
    if (!owner || *owner) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (!record->executable_file) return KERNEL_MM_STATUS_NOT_MAPPED;
    struct kernel_vfs_path *path = kernel_open_file_path(record->executable_file);
    if (!path || kernel_vfs_path_acquire(path)) return KERNEL_MM_STATUS_STATE;
    *owner = path;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_set_exec_layout(
    struct kernel_mm *mm, uint64_t start_code, uint64_t end_code,
    uint64_t start_stack)
{
    struct kernel_mm_record *record;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (record->stage != KERNEL_MM_RECORD_LIVE)
        return KERNEL_MM_STATUS_STATE;
    if (start_code > end_code || end_code >= ARCH_MMU_USER_LIMIT ||
        start_stack >= ARCH_MMU_USER_LIMIT)
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    record->start_code = start_code;
    record->end_code = end_code;
    record->start_stack = start_stack;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_proc_memory_snapshot(
    const struct kernel_mm *mm, struct kernel_mm_proc_memory *snapshot)
{
    if (!snapshot) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    enum kernel_mm_status status = mm_operation_begin(mm, &operation);
    if (status != KERNEL_MM_STATUS_OK) return status;
    struct kernel_mm_record *record;
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (record->stage != KERNEL_MM_RECORD_LIVE || !record->vmas)
        return KERNEL_MM_STATUS_STATE;
    /* gate 固定 VMA 数组，raw 固定与 fault 并发更新的页表计数。 */
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    *snapshot = (struct kernel_mm_proc_memory){
        .start_code = record->start_code,
        .end_code = record->end_code,
        .start_stack = record->start_stack,
        .virtual_bytes = kernel_vma_set_total_bytes(record->vmas),
        .locked_bytes = kernel_vma_set_locked_bytes(record->vmas),
        .resident_pages = (uint64_t)record->space.leaf_pages +
                          record->space.protected_pages,
    };
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status mm_vma_lookup_impl(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_vma *vma)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || vma == 0 || virtual_address >= ARCH_MMU_USER_LIMIT) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE ||
        record->vmas == 0) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    return status_from_vma(kernel_vma_set_lookup(record->vmas,
                                                 virtual_address,
                                                 vma));
}

/* 任务使用计数不包含已退出任务留给清理器的 MM 引用。 */
void kernel_mm_add_user(struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK || mm->state != KERNEL_MM_LIVE)
        __builtin_trap();
    if (__atomic_fetch_add(&record->users, 1U, __ATOMIC_ACQ_REL) == UINT32_MAX)
        __builtin_trap();
}
uint32_t kernel_mm_user_count(const struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK || mm->state != KERNEL_MM_LIVE)
        __builtin_trap();
    uint32_t count = __atomic_load_n(&record->users, __ATOMIC_ACQUIRE);
    if (!count) __builtin_trap();
    return count;
}
uint32_t kernel_mm_remove_user(struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK || mm->state != KERNEL_MM_LIVE)
        __builtin_trap();
    uint32_t old = __atomic_fetch_sub(&record->users, 1U, __ATOMIC_ACQ_REL);
    if (!old) __builtin_trap();
    return old - 1U;
}

enum kernel_mm_status kernel_mm_futex_id(
    const struct kernel_mm *mm, uint64_t *identity)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || identity == 0) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (mm->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE)
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    *identity = record->futex_id;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_brk_initialize(
    struct kernel_mm *mm,
    uint64_t start,
    uint64_t limit)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || start < ARCH_MMU_PAGE_SIZE || start > limit ||
        limit > ARCH_MMU_USER_LIMIT ||
        (start & BOAROS_PAGE_MASK) != 0U ||
        (limit & BOAROS_PAGE_MASK) != 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE ||
        __atomic_load_n(&record->references, __ATOMIC_ACQUIRE) != 1U || record->vmas == 0 ||
        record->brk_initialized != 0U) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    record->start_brk = start;
    record->current_brk = start;
    record->brk_limit = limit;
    record->mmap_base = limit;
    record->vdso_address = 0U;
    record->brk_initialized = 1U;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_mmap_base_initialize(
    struct kernel_mm *mm,
    uint64_t base)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || base < ARCH_MMU_PAGE_SIZE ||
        base > ARCH_MMU_USER_LIMIT ||
        (base & BOAROS_PAGE_MASK) != 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (base < record->start_brk) {
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    record->mmap_base = base;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_vdso_set_address(
    struct kernel_mm *mm,
    uint64_t address)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || address < ARCH_MMU_PAGE_SIZE ||
        address > ARCH_MMU_USER_LIMIT - BOAROS_PAGE_SIZE ||
        (address & BOAROS_PAGE_MASK) != 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    record->vdso_address = address;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_vdso_address(
    const struct kernel_mm *mm,
    uint64_t *address)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || address == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE ||
        record->vmas == 0) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    if (record->vdso_address == 0U) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    *address = record->vdso_address;
    return KERNEL_MM_STATUS_OK;
}

static int align_brk_page(uint64_t address, uint64_t *aligned)
{
    if (address > ARCH_MMU_USER_LIMIT - BOAROS_PAGE_MASK) {
        return 0;
    }
    *aligned = (address + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    return 1;
}

static int align_mapping_length(uint64_t length, uint64_t *aligned)
{
    if (length == 0U ||
        length > ARCH_MMU_USER_LIMIT - BOAROS_PAGE_MASK) {
        return 0;
    }
    *aligned = (length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    return *aligned != 0U;
}

static uint32_t backend_permissions_from_mm(uint32_t permissions);

static int normalize_user_permissions(uint32_t permissions,
                                      uint32_t *normalized)
{
    uint32_t known = KERNEL_MM_READ | KERNEL_MM_WRITE |
                     KERNEL_MM_EXECUTE;

    if ((permissions & ~known) != 0U) {
        return 0;
    }
    if ((permissions & KERNEL_MM_WRITE) != 0U) {
        permissions |= KERNEL_MM_READ;
    }
    *normalized = permissions;
    return 1;
}

static enum kernel_mm_status mutable_vma_record(
    struct kernel_mm *mm,
    struct kernel_mm_record **record)
{
    enum kernel_mm_status status;

    if (mm == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, record);
    if (status != KERNEL_MM_STATUS_OK ||
        (*record)->stage != KERNEL_MM_RECORD_LIVE ||
        (*record)->vmas == 0 || (*record)->brk_initialized != 1U) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    return KERNEL_MM_STATUS_OK;
}

struct mm_unmap_residents {
    struct kernel_mm_record *record;
    struct mm_file_resident *removed;
};
static void unmap_resident_before_pte(void *owner,uint64_t address,uint64_t physical)
{
    struct mm_unmap_residents *residents=owner;
    struct mm_file_resident *page=find_file_resident(residents->record,address);
    if (!page) return;
    if (page->physical_address!=physical) __builtin_trap();
    unindex_file_resident(residents->record,page);
    page->next=residents->removed; residents->removed=page;
}
static enum kernel_mm_status unmap_space_range(
    struct kernel_mm_record *record,uint64_t start,uint64_t end)
{
    if (end<=ARCH_MMU_PAGE_SIZE) return KERNEL_MM_STATUS_OK;
    if (start<ARCH_MMU_PAGE_SIZE) start=ARCH_MMU_PAGE_SIZE;
    struct kernel_mm_detach_cursor cursor={0};
    enum arch_mmu_status status;
    do {
        struct kernel_mm_retirement batch={0};
        struct mm_unmap_residents residents={.record=record};
        uint32_t targets;
        {
            KERNEL_RAW_SCOPE(guard,&record->metadata);
            status=arch_mmu_user_detach_range(&record->space,start,end,&cursor,&batch,
                unmap_resident_before_pte,&residents);
            if (status!=ARCH_MMU_STATUS_OK && status!=ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
            targets=batch.count?note_tlb_change_locked(record,&batch,0):0;
        }
        if (batch.count) finish_tlb_change(record,&batch,targets,
            batch.nonleaf?0:start,batch.nonleaf?UINT64_MAX:end-start,0);
        while (residents.removed) {
            struct mm_file_resident *page=residents.removed;
            residents.removed=page->next;
            retire_file_resident(record,page);
        }
    } while (status==ARCH_MMU_STATUS_OK);
    return KERNEL_MM_STATUS_OK;
}

/* 调用者拥有资源限制；MM只拥有用量。检查和提交都在同一个操作锁内。 */
static int may_expand_usage(uint64_t total, uint64_t data,
    const struct kernel_vma *vma, uint64_t growth,
    const struct kernel_memory_limits *limits)
{
    if (total > limits->address_space || growth > limits->address_space - total)
        return 0;
    uint64_t data_limit = limits->data_soft ? limits->data_soft : limits->data_hard;
    if (kernel_vma_is_data(vma) &&
        (data > data_limit || growth > data_limit - data)) return 0;
    return 1;
}

static int may_expand_memory(const struct kernel_mm_record *record,
    const struct kernel_vma *vma, uint64_t growth,
    const struct kernel_memory_limits *limits)
{
    return may_expand_usage(kernel_vma_set_total_bytes(record->vmas),
        kernel_vma_set_data_bytes(record->vmas), vma, growth, limits);
}

static int may_expand_current(const struct kernel_mm_record *record,
    const struct kernel_vma *vma, uint64_t growth)
{
    struct kernel_memory_limits limits;
    kernel_task_current_memory_limits(&limits);
    return may_expand_memory(record, vma, growth, &limits);
}

/* MAP_FIXED按全部被覆盖字节抵扣扩张量，保留Linux的检查时点。 */
static int may_map_memory(const struct kernel_mm_record *record,
    uint64_t start, uint64_t end, uint32_t permissions, int shared, int replace)
{
    uint64_t covered = 0;
    struct kernel_vma vma;
    for (uint32_t i = 0; replace && i < kernel_vma_set_count(record->vmas); i++) {
        if (kernel_vma_set_get_at(record->vmas, i, &vma)) __builtin_trap();
        if (vma.end <= start) continue;
        if (vma.start >= end) break;
        uint64_t low = vma.start > start ? vma.start : start;
        uint64_t high = vma.end < end ? vma.end : end;
        covered += high - low;
    }
    vma = (struct kernel_vma){.permissions = permissions,
        .kind = shared ? KERNEL_VMA_KIND_ANON_SHARED : KERNEL_VMA_KIND_ANONYMOUS};
    return may_expand_current(record, &vma, end - start - covered);
}

static int may_lock_memory(const struct kernel_mm_record *record, uint64_t growth)
{
    uint64_t limit = kernel_task_current_memlock_limit();
    uint64_t used = kernel_vma_set_locked_bytes(record->vmas);
    return used <= limit && growth <= limit - used;
}

static uint32_t mapping_lock_flags(const struct kernel_mm_record *record, uint32_t flags)
{
    return (flags & KERNEL_MM_MAP_LOCKED) ? KERNEL_VMA_LOCKED | record->future_lock_flags : record->future_lock_flags;
}

static enum kernel_mm_status admit_mapping_lock(const struct kernel_mm_record *record,
    uint64_t start, uint64_t end, uint32_t flags)
{
    if (!mapping_lock_flags(record, flags)) return KERNEL_MM_STATUS_OK;
    uint64_t limit = kernel_task_current_memlock_limit();
    if (!limit && (flags & KERNEL_MM_MAP_LOCKED)) return KERNEL_MM_STATUS_LOCK_PERMISSION;
    uint64_t used = kernel_vma_set_locked_bytes(record->vmas);
    return used <= limit && end - start <= limit - used ? KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_LOCK_LIMIT;
}

static int populate_locked(struct kernel_mm *, uint64_t, uint64_t);

static enum kernel_mm_status mm_mmap_anonymous_impl(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_vma vma;
    struct kernel_vma_edit edit;
    struct kernel_memory_object *shared_object = 0;
    struct kernel_mm_shared_anon *shared_entry = 0;
    uint64_t aligned_length;
    uint64_t start;
    uint64_t end;
    uint32_t normalized;
    int overlaps;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if (address == 0 ||
        !normalize_user_permissions(permissions, &normalized) ||
        (flags & ~(KERNEL_MM_MAP_FIXED |
                   KERNEL_MM_MAP_FIXED_NOREPLACE |
                   KERNEL_MM_MAP_SHARED | KERNEL_MM_MAP_LOCKED)) != 0U ||
        (flags & (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) ==
            (KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_FIXED_NOREPLACE)) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (length == 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!align_mapping_length(length, &aligned_length)) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if ((flags & (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) != 0U) {
        if (hint < ARCH_MMU_PAGE_SIZE ||
            (hint & BOAROS_PAGE_MASK) != 0U ||
            hint > ARCH_MMU_USER_LIMIT - aligned_length) {
            return KERNEL_MM_STATUS_INVALID_ARGUMENT;
        }
        start = hint;
        if ((flags & KERNEL_MM_MAP_FIXED_NOREPLACE) != 0U) {
            vma_status = kernel_vma_set_overlaps(record->vmas,
                                                 start,
                                                 start + aligned_length,
                                                 &overlaps);
            if (vma_status != KERNEL_VMA_STATUS_OK) {
                return status_from_vma(vma_status);
            }
            if (overlaps != 0) {
                return KERNEL_MM_STATUS_CONFLICT;
            }
        }
    } else {
        if (record->mmap_base < ARCH_MMU_PAGE_SIZE ||
            aligned_length >
                record->mmap_base - ARCH_MMU_PAGE_SIZE) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        hint &= ~BOAROS_PAGE_MASK;
        if (hint < ARCH_MMU_PAGE_SIZE ||
            hint > record->mmap_base - aligned_length) {
            hint = 0U;
        }
        vma_status = kernel_vma_set_find_topdown_gap(
            record->vmas,
            hint,
            ARCH_MMU_PAGE_SIZE,
            record->mmap_base,
            aligned_length,
            &start);
        if (vma_status == KERNEL_VMA_STATUS_NOT_FOUND) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            return status_from_vma(vma_status);
        }
    }
    end = start + aligned_length;
    if (!may_map_memory(record, start, end, normalized, flags & KERNEL_MM_MAP_SHARED, flags & KERNEL_MM_MAP_FIXED))
        return KERNEL_MM_STATUS_NO_MEMORY;
    status = admit_mapping_lock(record, start, end, flags);
    if (status) return status;
    if ((flags & KERNEL_MM_MAP_SHARED) != 0U) {
        enum kernel_memory_object_status shared_status =
            kernel_memory_object_create(record->vma_heap, mm->allocator,
                                      &shared_object);

        if (shared_status != KERNEL_MEMORY_OBJECT_OK)
            return shared_status == KERNEL_MEMORY_OBJECT_NO_MEMORY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        enum kernel_heap_status heap_status = kernel_heap_allocate_zeroed(
            record->vma_heap, 1U, sizeof(*shared_entry),
            (void **)&shared_entry);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            kernel_memory_object_release(&shared_object);
            return heap_status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        }
        shared_entry->object = shared_object;
    }
    vma = (struct kernel_vma){
        .start = start,
        .end = end,
        .lock_flags = mapping_lock_flags(record, flags),
        .backing_offset = 0U,
        .permissions = normalized,
        .maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS,
        .kind = shared_object != 0 ? KERNEL_VMA_KIND_ANON_SHARED
                                   : KERNEL_VMA_KIND_ANONYMOUS,
        .role = KERNEL_VMA_ROLE_MMAP,
        .fault_policy = shared_object != 0
                            ? KERNEL_VMA_FAULT_ANON_SHARED
                            : KERNEL_VMA_FAULT_DEMAND_ZERO,
        .backing = shared_object,
    };
    if ((flags & KERNEL_MM_MAP_FIXED) == 0U) {
        status = status_from_vma(kernel_vma_set_insert(record->vmas,
                                                       &vma));
        if (status == KERNEL_MM_STATUS_OK) {
            if (shared_entry != 0) {
                shared_entry->next = record->shared_anon;
                record->shared_anon = shared_entry;
            }
            *address = start;
        } else {
            goto discard_shared;
        }
        return status;
    }
    vma_status = kernel_vma_set_prepare_replace(record->vmas,
                                                start,
                                                end,
                                                &vma,
                                                &edit);
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        status = status_from_vma(vma_status);
        goto discard_shared;
    }
    status = unmap_space_range(record, start, end);
    if (status != KERNEL_MM_STATUS_OK) {
        goto discard_shared;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) {
        status = KERNEL_MM_STATUS_STATE;
        goto discard_shared;
    }
    if (shared_entry != 0) {
        shared_entry->next = record->shared_anon;
        record->shared_anon = shared_entry;
    }
    drain_shared_anon(record, 1);
    (void)drain_file_sources(record, 1);
    (void)drain_elf_sources(record, 1);
    *address = start;
    return KERNEL_MM_STATUS_OK;

discard_shared:
    if (shared_entry != 0) kernel_heap_release(record->vma_heap, shared_entry);
    if (shared_object != 0)
        kernel_memory_object_release(&shared_object);
    return status;
}

static void discard_prepared_file_source(
    struct kernel_mm_record *record,
    struct kernel_mm_file_source *source)
{
    if (source != 0) {
        kernel_heap_release(record->vma_heap, source);
    }
}

static void finish_file_mapping(
    struct kernel_mm_record *record,
    struct kernel_open_file_description **file,
    struct kernel_mm_file_source *prepared)
{
    if (prepared != 0) {
        prepared->file = *file;
        prepared->next = record->file_sources;
        record->file_sources = prepared;
    } else {
        /* An existing source plus this caller pin is never the last ref. */
        if (kernel_open_file_release(file) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return;
        }
    }
    *file = 0;
    (void)drain_file_sources(record, 1);
    (void)drain_elf_sources(record, 1);
}

struct mm_file_mapping_plan {
    struct kernel_mm_record *record;
    uint64_t start;
    uint64_t end;
    uint32_t permissions;
};

static enum kernel_mm_status prepare_file_mapping(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags,
    struct mm_file_mapping_plan *plan)
{
    struct kernel_mm_record *record;
    uint64_t aligned_length;
    uint64_t start;
    uint32_t normalized;
    int overlaps;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if (plan == 0 || (file_offset & BOAROS_PAGE_MASK) != 0U ||
        !normalize_user_permissions(permissions, &normalized) ||
        (flags & ~(KERNEL_MM_MAP_FIXED |
                   KERNEL_MM_MAP_FIXED_NOREPLACE |
                   KERNEL_MM_MAP_SHARED | KERNEL_MM_MAP_LOCKED)) != 0U ||
        (flags & (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) ==
                 (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!align_mapping_length(length, &aligned_length)) {
        return length == 0U ? KERNEL_MM_STATUS_INVALID_ARGUMENT
                            : KERNEL_MM_STATUS_NO_MEMORY;
    }
    if (file_offset > UINT64_MAX - aligned_length) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if ((flags & (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) != 0U) {
        if (hint < ARCH_MMU_PAGE_SIZE ||
            (hint & BOAROS_PAGE_MASK) != 0U ||
            hint > ARCH_MMU_USER_LIMIT - aligned_length) {
            return KERNEL_MM_STATUS_INVALID_ARGUMENT;
        }
        start = hint;
        if ((flags & KERNEL_MM_MAP_FIXED_NOREPLACE) != 0U) {
            vma_status = kernel_vma_set_overlaps(record->vmas,
                                                 start,
                                                 start + aligned_length,
                                                 &overlaps);
            if (vma_status != KERNEL_VMA_STATUS_OK || overlaps != 0) {
                return vma_status == KERNEL_VMA_STATUS_OK
                           ? KERNEL_MM_STATUS_CONFLICT
                           : status_from_vma(vma_status);
            }
        }
    } else {
        if (record->mmap_base < ARCH_MMU_PAGE_SIZE ||
            aligned_length >
                record->mmap_base - ARCH_MMU_PAGE_SIZE) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        hint &= ~BOAROS_PAGE_MASK;
        if (hint < ARCH_MMU_PAGE_SIZE ||
            hint > record->mmap_base - aligned_length) {
            hint = 0U;
        }
        vma_status = kernel_vma_set_find_topdown_gap(
            record->vmas,
            hint,
            ARCH_MMU_PAGE_SIZE,
            record->mmap_base,
            aligned_length,
            &start);
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            return vma_status == KERNEL_VMA_STATUS_NOT_FOUND
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : status_from_vma(vma_status);
        }
    }
    *plan = (struct mm_file_mapping_plan){
        .record = record,
        .start = start,
        .end = start + aligned_length,
        .permissions = normalized,
    };
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status validate_file_mapping(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags)
{
    KERNEL_NO_RECLAIM_IO;
    struct mm_file_mapping_plan plan;

    return prepare_file_mapping(mm,
                                hint,
                                length,
                                file_offset,
                                permissions,
                                flags,
                                &plan);
}

static enum kernel_mm_status commit_file_mapping(
    struct kernel_open_file_description **file,
    uint64_t file_offset,
    uint32_t flags,
    uint64_t *address,
    const struct mm_file_mapping_plan *input,
    struct kernel_lock_guard *inode)
{
    KERNEL_NO_RECLAIM_IO;
    struct mm_file_mapping_plan plan = *input;
    struct kernel_mm_record *record;
    struct kernel_mm_file_source *prepared = 0;
    struct kernel_mm_file_source *existing;
    struct kernel_vma vma;
    struct mm_file_mapping *registration = 0;
    struct kernel_vma_edit edit;
    enum kernel_heap_status heap_status;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    record = plan.record;
    if (!may_map_memory(record, plan.start, plan.end, plan.permissions,
                        flags & KERNEL_MM_MAP_SHARED, flags & KERNEL_MM_MAP_FIXED)) return KERNEL_MM_STATUS_NO_MEMORY;
    status = admit_mapping_lock(record, plan.start, plan.end, flags);
    if (status) return status;
    existing = find_file_source(record, *file);
    if (existing == 0) {
        heap_status = kernel_heap_allocate_zeroed(record->vma_heap,
                                                  1U,
                                                  sizeof(*prepared),
                                                  (void **)&prepared);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            return heap_status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        }
    }
    status = prepare_file_registration(record, *file, &registration);
    if (status != KERNEL_MM_STATUS_OK) {
        discard_prepared_file_source(record, prepared);
        return status;
    }
    vma = (struct kernel_vma){
        .start = plan.start,
        .end = plan.end,
        .lock_flags = mapping_lock_flags(record, flags),
        .backing_offset = file_offset,
        .permissions = plan.permissions,
        .kind = (flags & KERNEL_MM_MAP_SHARED) != 0U
                    ? KERNEL_VMA_KIND_FILE_SHARED
                    : KERNEL_VMA_KIND_FILE_PRIVATE,
        .role = KERNEL_VMA_ROLE_MMAP,
        .fault_policy = (flags & KERNEL_MM_MAP_SHARED) != 0U
                            ? KERNEL_VMA_FAULT_FILE_SHARED
                            : KERNEL_VMA_FAULT_FILE_PRIVATE,
        .backing = *file,
        .maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS &
            (((flags & KERNEL_MM_MAP_SHARED) == 0U || kernel_open_file_writable(*file))
                 ? KERNEL_VMA_ALL_PERMISSIONS : ~KERNEL_MM_WRITE),
    };
    if ((flags & KERNEL_MM_MAP_FIXED) == 0U) {
        status = status_from_vma(kernel_vma_set_insert(record->vmas,
                                                       &vma));
        if (status != KERNEL_MM_STATUS_OK) {
            discard_prepared_file_source(record, prepared);
            if (registration != 0)
                kernel_heap_release(record->vma_heap, registration);
            return status;
        }
    } else {
        vma_status = kernel_vma_set_prepare_replace(record->vmas,
                                                    plan.start,
                                                    plan.end,
                                                    &vma,
                                                    &edit);
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            discard_prepared_file_source(record, prepared);
            if (registration != 0)
                kernel_heap_release(record->vma_heap, registration);
            return status_from_vma(vma_status);
        }
        status = unmap_space_range(record, plan.start, plan.end);
        if (status != KERNEL_MM_STATUS_OK ||
            kernel_vma_set_commit_edit(record->vmas, &edit) !=
                KERNEL_VMA_STATUS_OK) {
            discard_prepared_file_source(record, prepared);
            if (registration != 0)
                kernel_heap_release(record->vma_heap, registration);
            return status != KERNEL_MM_STATUS_OK
                       ? status
                       : KERNEL_MM_STATUS_STATE;
        }
    }
    if (registration != 0) {
        registration->next = record->file_mappings;
        record->file_mappings = registration;
        if (kernel_file_mapping_register(&registration->registration)) __builtin_trap();
    }
    /* 旧来源的最后close可能取得另一inode写锁，不能带着新inode读锁清理。 */
    if (inode->lock) kernel_lock_release(inode);
    finish_file_mapping(record, file, prepared);
    drain_shared_anon(record, 1);
    *address = plan.start;
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status mm_munmap_impl(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_vma_edit edit;
    uint64_t aligned_length;
    uint64_t end;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if ((address & BOAROS_PAGE_MASK) != 0U ||
        !align_mapping_length(length, &aligned_length) ||
        address >= ARCH_MMU_USER_LIMIT ||
        address > ARCH_MMU_USER_LIMIT - aligned_length) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    end = address + aligned_length;
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    vma_status = kernel_vma_set_prepare_replace(record->vmas,
                                                address,
                                                end,
                                                0,
                                                &edit);
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        return status_from_vma(vma_status);
    }
    status = unmap_space_range(record, address, end);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) {
        return KERNEL_MM_STATUS_STATE;
    }
    drain_shared_anon(record, 1);
    (void)drain_file_sources(record, 1);
    (void)drain_elf_sources(record, 1);
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_shmat(
    struct kernel_mm *mm,
    struct kernel_shm_segment *segment,
    uint64_t hint,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *out_address)
{
    KERNEL_SERVICE_SCOPE(service);
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_vma vma;
    struct kernel_vma_edit edit;
    struct kernel_mm_shared_anon *shared_entry = 0;
    struct kernel_memory_object *temporary_owner = 0;
    struct kernel_shm_attachment *attachment
        __attribute__((cleanup(kernel_shm_attachment_release))) = 0;
    uint64_t aligned_length;
    uint64_t start;
    uint64_t end;
    uint32_t normalized;
    int overlaps;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if (mm == 0 || segment == 0 || !segment->active || segment->memory == 0 ||
        out_address == 0 ||
        !normalize_user_permissions(permissions, &normalized)) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    aligned_length = segment->aligned_size;
    if (aligned_length == 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    struct mm_operation operation __attribute__((cleanup(mm_operation_end)))={.record=record,.mm=mm};
    status=mm_operation_lock(&operation);
    if (status) return status;

    if (hint != 0U) {
        if ((flags & KERNEL_SHM_RND) != 0U) {
            hint &= ~BOAROS_PAGE_MASK;
        }
        if ((hint & BOAROS_PAGE_MASK) != 0U ||
            hint < ARCH_MMU_PAGE_SIZE ||
            hint >= ARCH_MMU_USER_LIMIT ||
            aligned_length > ARCH_MMU_USER_LIMIT - hint) {
            return KERNEL_MM_STATUS_INVALID_ARGUMENT;
        }
        start = hint;
        if ((flags & KERNEL_SHM_REMAP) == 0U) {
            vma_status = kernel_vma_set_overlaps(record->vmas,
                                                 start,
                                                 start + aligned_length,
                                                 &overlaps);
            if (vma_status != KERNEL_VMA_STATUS_OK) {
                return status_from_vma(vma_status);
            }
            if (overlaps != 0) {
                return KERNEL_MM_STATUS_CONFLICT;
            }
        }
    } else {
        if ((flags & KERNEL_SHM_REMAP) != 0U) {
            return KERNEL_MM_STATUS_INVALID_ARGUMENT;
        }
        if (record->mmap_base < ARCH_MMU_PAGE_SIZE ||
            aligned_length > record->mmap_base - ARCH_MMU_PAGE_SIZE) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        vma_status = kernel_vma_set_find_topdown_gap(
            record->vmas,
            0U,
            ARCH_MMU_PAGE_SIZE,
            record->mmap_base,
            aligned_length,
            &start);
        if (vma_status == KERNEL_VMA_STATUS_NOT_FOUND) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            return status_from_vma(vma_status);
        }
    }
    end = start + aligned_length;
    if (!may_map_memory(record, start, end, normalized, 1, flags & KERNEL_SHM_REMAP)) return KERNEL_MM_STATUS_NO_MEMORY;
    status = admit_mapping_lock(record, start, end, (flags & KERNEL_SHM_REMAP) ? KERNEL_MM_MAP_FIXED : 0);
    if (status) return status;
    if (kernel_shm_attachment_create(segment, start, end, &attachment) !=
            KERNEL_SHM_STATUS_OK) return KERNEL_MM_STATUS_NO_MEMORY;

    int already_tracked = 0;
    for (struct kernel_mm_shared_anon *curr = record->shared_anon;
         curr != 0; curr = curr->next) {
        if (curr->object == segment->memory) {
            already_tracked = 1;
            break;
        }
    }
    if (!already_tracked) {
        if (kernel_memory_object_acquire(segment->memory) !=
            KERNEL_MEMORY_OBJECT_OK) {
            return KERNEL_MM_STATUS_STATE;
        }
        /* 新取得的引用独立回滚，不能消费段表 owner。 */
        temporary_owner = segment->memory;
        enum kernel_heap_status heap_status = kernel_heap_allocate_zeroed(
            record->vma_heap, 1U, sizeof(*shared_entry),
            (void **)&shared_entry);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            kernel_memory_object_release(&temporary_owner);
            return heap_status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_STATE;
        }
        shared_entry->object = temporary_owner;
        temporary_owner = 0;
    }

    vma = (struct kernel_vma){
        .start = start,
        .end = end,
        .backing_offset = 0U,
        .permissions = normalized,
        .maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS &
            ((flags & KERNEL_SHM_RDONLY) ? ~KERNEL_MM_WRITE : KERNEL_VMA_ALL_PERMISSIONS),
        .lock_flags = record->future_lock_flags,
        .kind = KERNEL_VMA_KIND_SYSV_SHM,
        .role = KERNEL_VMA_ROLE_SYSV_SHM,
        .fault_policy = KERNEL_VMA_FAULT_ANON_SHARED,
        .backing = segment->memory,
        .shm_attachment = attachment,
    };

    if ((flags & KERNEL_SHM_REMAP) == 0U) {
        status = status_from_vma(kernel_vma_set_insert(record->vmas, &vma));
        if (status == KERNEL_MM_STATUS_OK) {
            if (shared_entry != 0) {
                shared_entry->next = record->shared_anon;
                record->shared_anon = shared_entry;
            }
            *out_address = start;
        } else {
            if (shared_entry != 0) {
                kernel_memory_object_release(&shared_entry->object);
                kernel_heap_release(record->vma_heap, shared_entry);
            }
        }
        uint32_t policy = record->future_lock_flags;
        mm_operation_end(&operation);
        if (!status && policy == KERNEL_VMA_LOCKED) (void)populate_locked(mm, start, end);
        return status;
    }

    vma_status = kernel_vma_set_prepare_replace(record->vmas,
                                                start,
                                                end,
                                                &vma,
                                                &edit);
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        status = status_from_vma(vma_status);
        if (shared_entry != 0) {
            kernel_memory_object_release(&shared_entry->object);
            kernel_heap_release(record->vma_heap, shared_entry);
        }
        return status;
    }
    status = unmap_space_range(record, start, end);
    if (status != KERNEL_MM_STATUS_OK) {
        if (shared_entry != 0) {
            kernel_memory_object_release(&shared_entry->object);
            kernel_heap_release(record->vma_heap, shared_entry);
        }
        return status;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) {
        if (shared_entry != 0) {
            kernel_memory_object_release(&shared_entry->object);
            kernel_heap_release(record->vma_heap, shared_entry);
        }
        return KERNEL_MM_STATUS_STATE;
    }
    if (shared_entry != 0) {
        shared_entry->next = record->shared_anon;
        record->shared_anon = shared_entry;
    }
    drain_shared_anon(record, 1);
    (void)drain_file_sources(record, 1);
    (void)drain_elf_sources(record, 1);
    *out_address = start;
    uint32_t policy = record->future_lock_flags;
    mm_operation_end(&operation);
    if (policy == KERNEL_VMA_LOCKED) (void)populate_locked(mm, start, end);
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_shmdt(struct kernel_mm *mm, uint64_t address)
{
    KERNEL_SERVICE_SCOPE(service);
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    if (mm == 0 || (address & BOAROS_PAGE_MASK) ||
        address < ARCH_MMU_PAGE_SIZE || address >= ARCH_MMU_USER_LIMIT)
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end)))={.record=record,.mm=mm};
    status=mm_operation_lock(&operation);
    if (status) return status;
    struct kernel_shm_attachment *attachment
        __attribute__((cleanup(kernel_shm_attachment_release))) = 0;
    struct kernel_vma vma;
    for (uint32_t i = 0; i < kernel_vma_set_count(record->vmas); i++) {
        if (kernel_vma_set_get_at(record->vmas, i, &vma) != KERNEL_VMA_STATUS_OK)
            __builtin_trap();
        if (vma.shm_attachment && vma.shm_attachment->start == address) {
            attachment = vma.shm_attachment;
            kernel_shm_attachment_acquire(attachment);
            break;
        }
    }
    if (!attachment) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    /* 起始页可以已被撤销；仅移除同一附件片段，不吞掉洞内的新映射。 */
    for (uint32_t i = 0; i < kernel_vma_set_count(record->vmas);) {
        if (kernel_vma_set_get_at(record->vmas, i, &vma) != KERNEL_VMA_STATUS_OK)
            __builtin_trap();
        if (vma.shm_attachment != attachment) { i++; continue; }
        status = mm_munmap_impl(mm, vma.start, vma.end - vma.start);
        if (status != KERNEL_MM_STATUS_OK) return status;
        i = 0; /* 后备清理可睡眠，重取集合而不保存数组下标。 */
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status protect_vma_range(struct kernel_mm_record *record,
    const struct kernel_vma_edit *edit)
{
    uint64_t address = edit->start, end = edit->end;
    uint32_t normalized = edit->permissions;
    enum arch_mmu_status backend_status;
    uint64_t cursor = address, leaf;
    while ((backend_status = arch_mmu_user_next_owned(&record->space, &cursor,
                   end, &leaf)) == ARCH_MMU_STATUS_OK) {
        uint32_t leaf_permissions = normalized;
        struct mm_file_resident *resident = find_file_resident(record, leaf);
        if (resident) {
            COST_ADD(MPROTECT_RESIDENT_VISITS, 1);
            if (kernel_page_cache_alias_active(&resident->alias))
                leaf_permissions &= ~KERNEL_MM_WRITE;
        }
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        if (arch_mmu_user_protect_owned_page(&record->space, leaf,
                backend_permissions_from_mm(leaf_permissions))) __builtin_trap();
    }
    if (backend_status != ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
    {
        struct kernel_mm_retirement batch={0};
        uint32_t targets;
        { KERNEL_RAW_SCOPE(guard,&record->metadata);
          targets=note_tlb_change_locked(record,&batch,(normalized & KERNEL_MM_EXECUTE)!=0); }
        finish_tlb_change(record,&batch,targets,address,end-address,(normalized & KERNEL_MM_EXECUTE)!=0);
    }
    if (kernel_vma_set_commit_edit(record->vmas, edit) !=
        KERNEL_VMA_STATUS_OK) return KERNEL_MM_STATUS_STATE;
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status mm_mprotect_impl(struct kernel_mm *mm,
    uint64_t address, uint64_t length, uint32_t permissions)
{
    COST_SCOPE(cost_protect, MPROTECT_TICKS);
    COST_ADD(MPROTECT_CALLS, 1);
    KERNEL_NO_RECLAIM_IO;
    uint64_t aligned_length;
    uint32_t normalized;
    struct kernel_mm_record *record;
    if ((address & BOAROS_PAGE_MASK) || !normalize_user_permissions(permissions, &normalized))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (!length) return mutable_vma_record(mm, &record);
    if (!align_mapping_length(length, &aligned_length) ||
        address > ARCH_MMU_USER_LIMIT - aligned_length) return KERNEL_MM_STATUS_NO_MEMORY;
    enum kernel_mm_status status = mutable_vma_record(mm, &record);
    if (status) return status;
    struct kernel_vma_edit edit;
    status = status_from_vma(kernel_vma_set_prepare_mprotect(record->vmas,
        address, address + aligned_length, normalized, &edit));
    if (!edit.set) return status;
    /* VMA已按顺序接纳并预留前缀，页表和TLB仍只提交一次。 */
    enum kernel_mm_status committed = protect_vma_range(record, &edit);
    return committed ? committed : status;
}

static enum kernel_mm_status mm_brk_impl(
    struct kernel_mm *mm,
    uint64_t requested,
    uint64_t *result)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_vma_edit edit;
    uint64_t old_page_end;
    uint64_t new_page_end;
    uint64_t old_brk;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if (mm == 0 || result == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE ||
        record->vmas == 0 || record->brk_initialized != 1U) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    old_brk = record->current_brk;
    if (requested < record->start_brk || requested > record->brk_limit ||
        !align_brk_page(old_brk, &old_page_end) ||
        !align_brk_page(requested, &new_page_end)) {
        *result = old_brk;
        return KERNEL_MM_STATUS_OK;
    }
    struct kernel_memory_limits limits;
    kernel_task_current_memory_limits(&limits);
    uint64_t initial_data = record->end_data - record->start_data;
    if (initial_data > limits.data_soft ||
        requested - record->start_brk > limits.data_soft - initial_data) {
        *result = old_brk;
        return KERNEL_MM_STATUS_OK;
    }
    if (new_page_end == old_page_end) {
        __atomic_store_n(&record->current_brk, requested, __ATOMIC_RELEASE);
        *result = requested;
        return KERNEL_MM_STATUS_OK;
    }

    if (new_page_end > old_page_end) {
        const struct kernel_vma data = {.permissions = KERNEL_MM_WRITE,
            .kind = KERNEL_VMA_KIND_ANONYMOUS};
        if (!may_expand_memory(record, &data, new_page_end - old_page_end, &limits) ||
            (record->future_lock_flags && !may_lock_memory(record, new_page_end - old_page_end))) {
            *result = old_brk;
            return KERNEL_MM_STATUS_OK;
        }
        vma_status = kernel_vma_set_insert(
            record->vmas,
            &(const struct kernel_vma){
                .start = old_page_end,
                .end = new_page_end,
                .backing_offset = 0U,
                .permissions = KERNEL_MM_READ | KERNEL_MM_WRITE,
                .maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS,
                .kind = KERNEL_VMA_KIND_ANONYMOUS,
                .role = KERNEL_VMA_ROLE_HEAP,
                .lock_flags = record->future_lock_flags,
                .fault_policy = KERNEL_VMA_FAULT_DEMAND_ZERO,
                .backing = 0,
            });
        if (vma_status == KERNEL_VMA_STATUS_NO_MEMORY ||
            vma_status == KERNEL_VMA_STATUS_CONFLICT) {
            *result = old_brk;
            return KERNEL_MM_STATUS_OK;
        }
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            return status_from_vma(vma_status);
        }
        __atomic_store_n(&record->current_brk, requested, __ATOMIC_RELEASE);
        *result = requested;
        return KERNEL_MM_STATUS_OK;
    }

    vma_status = kernel_vma_set_prepare_replace(record->vmas,
                                                new_page_end,
                                                old_page_end,
                                                0,
                                                &edit);
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        return status_from_vma(vma_status);
    }
    status = unmap_space_range(record, new_page_end, old_page_end);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) {
        return KERNEL_MM_STATUS_STATE;
    }
    drain_shared_anon(record, 1);
    __atomic_store_n(&record->current_brk, requested, __ATOMIC_RELEASE);
    *result = requested;
    return KERNEL_MM_STATUS_OK;
}

static uint32_t backend_permissions_from_mm(uint32_t permissions)
{
    uint32_t result = 0U;

    if ((permissions & KERNEL_MM_READ) != 0U) {
        result |= ARCH_MMU_READ;
    }
    if ((permissions & KERNEL_MM_WRITE) != 0U) {
        result |= ARCH_MMU_WRITE;
    }
    if ((permissions & KERNEL_MM_EXECUTE) != 0U) {
        result |= ARCH_MMU_EXECUTE;
    }
    return result;
}

enum kernel_mm_status kernel_mm_context(
    const struct kernel_mm *mm,
    uint64_t *satp)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || satp == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != KERNEL_MM_RECORD_LIVE) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    return arch_mmu_user_space_context(&record->space, satp) ==
                   ARCH_MMU_STATUS_OK
               ? KERNEL_MM_STATUS_OK
               : KERNEL_MM_STATUS_ADDRESS_SPACE;
}

enum kernel_mm_status kernel_mm_check_files_domain(const struct kernel_mm *mm,
    const struct kernel_heap *heap)
{
    kernel_assert_service_context();
    struct kernel_mm_record *record;
    if (!mm || !heap) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status=resolve_record_storage(mm,&record);
    if (status) return status;
    KERNEL_RAW_SCOPE(guard,&record->metadata);
    return mm->state==KERNEL_MM_LIVE &&
        record->vma_heap==heap && mm->allocator==heap->page_allocator
        ? KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_STATE;
}

enum kernel_mm_status kernel_mm_activate(const struct kernel_mm *next, uint64_t kernel_context)
{
    /* 在整个本地交接期间不允许被抢占；这里不分配、不进固件、无两个MM锁嵌套。 */
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    struct kernel_mm_record *previous = cpu->active_mm_record, *record = 0;
    enum kernel_mm_status status = KERNEL_MM_STATUS_OK;
    uint32_t logical = 0;
#if BOAROS_MAX_CPUS > 1
    logical = cpu->logical_id;
#endif
    if (cpu->raw_locks || logical >= BOAROS_MAX_CPUS) __builtin_trap();
    if (next) {
        if (next->state != KERNEL_MM_LIVE) status = KERNEL_MM_STATUS_STATE;
        else status = resolve_record_storage(next, &record);
        if (status != KERNEL_MM_STATUS_OK) goto out;
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        uint64_t context;
        if (arch_mmu_user_space_context(&record->space, &context) != ARCH_MMU_STATUS_OK) {
            status = KERNEL_MM_STATUS_ADDRESS_SPACE; goto out;
        }
        if (record != previous && (record->active_cpus & (1U << logical))) __builtin_trap();
        record->active_cpus |= 1U << logical;
        int changed = arch_mmu_current_context() != context;
        if (arch_mmu_switch_context(context) != ARCH_MMU_STATUS_OK) __builtin_trap();
        /* 换根已由架构入口失效；同根仅补齐加入前或未观察到的PTE修改。 */
        if (!changed && (record != previous || cpu->active_mm_generation != record->tlb_generation))
            arch_mmu_flush_all();
        if (record != previous || cpu->active_mm_instruction_generation != record->instruction_generation)
            arch_mmu_sync_instructions();
        cpu->active_mm_record = record;
        cpu->active_mm_generation = record->tlb_generation;
        cpu->active_mm_instruction_generation = record->instruction_generation;
    } else {
        if (arch_mmu_switch_context(kernel_context) != ARCH_MMU_STATUS_OK) {
            status = KERNEL_MM_STATUS_ADDRESS_SPACE; goto out;
        }
        cpu->active_mm_record = 0;
        cpu->active_mm_generation = 0;
        cpu->active_mm_instruction_generation = 0;
    }
    if (previous && previous != record) {
        KERNEL_RAW_SCOPE(guard, &previous->metadata);
        if (!(previous->active_cpus & (1U << logical))) __builtin_trap();
        previous->active_cpus &= ~(1U << logical);
    }
out:
    arch_interrupt_restore(irq);
    return status;
}

enum kernel_mm_status kernel_mm_release(struct kernel_mm *mm)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    enum kernel_mm_status status;
    enum arch_mmu_status backend_status;
    enum kernel_vma_status vma_status;

    if (mm == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!owner_handle(mm)) {
        return KERNEL_MM_STATUS_STATE;
    }
    if (mm->state == KERNEL_MM_CLEANUP &&
        mm->cleanup_stage == KERNEL_MM_CLEANUP_RECORD) {
        return release_record_only(mm);
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (mm->state == KERNEL_MM_LIVE) {
        if (record->stage != KERNEL_MM_RECORD_LIVE) {
            return KERNEL_MM_STATUS_STATE;
        }
        uint32_t references = __atomic_load_n(&record->references, __ATOMIC_ACQUIRE);
        while (references > 1U) {
            if (__atomic_compare_exchange_n(&record->references, &references, references - 1U,
                0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                finish_handle(mm, KERNEL_MM_RELEASED);
                return KERNEL_MM_STATUS_OK;
            }
        }
        if (!references) __builtin_trap();
        KERNEL_LOCK_SCOPE(retire);
        kernel_mutex_lock(&record->operations,&retire);
        if (record->access_pins || record->write_pins || record->active_cpus ||
            record->pending_retirement || record->retired_file_mappings) __builtin_trap();
        if (record->vmas == 0 && record->shared_anon == 0 &&
            record->file_sources == 0 &&
            record->elf_sources == 0 && record->executable_file == 0 &&
            !record->retired_file_sources && !record->retired_elf_sources) {
            backend_status = arch_mmu_user_space_destroy(&record->space);
            if (backend_status != ARCH_MMU_STATUS_OK) {
                __builtin_trap();
            }
            record->stage = KERNEL_MM_RECORD_ONLY_CLEANUP;
            if (retire.lock) mm_gate_release(record,&retire);
            mm->state = KERNEL_MM_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
            status = release_record_only(mm);
            return status;
        }
        record->stage = KERNEL_MM_RECORD_VMAS_CLEANUP;
        if (retire.lock) mm_gate_release(record,&retire);
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_VMAS;
    } else if ((mm->cleanup_stage != KERNEL_MM_CLEANUP_VMAS ||
                record->stage != KERNEL_MM_RECORD_VMAS_CLEANUP) &&
               (mm->cleanup_stage !=
                    KERNEL_MM_CLEANUP_FILE_SOURCES ||
                record->stage !=
                    KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP) &&
               (mm->cleanup_stage != KERNEL_MM_CLEANUP_ELF_SOURCES ||
                record->stage !=
                    KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP)) {
        return KERNEL_MM_STATUS_STATE;
    }

    if (record->stage == KERNEL_MM_RECORD_VMAS_CLEANUP) {
        drain_file_registrations(record, 0);
        forget_file_residents(record);
        struct mm_file_resident *retired=record->retired_file_residents;
        record->retired_file_residents=0;
        drain_retired_residents(record,retired);
        if (record->resident_buckets != 0)
            kernel_heap_release(record->vma_heap, record->resident_buckets);
        record->resident_buckets = 0;
        record->resident_capacity = 0;
        if (record->vmas != 0) {
            vma_status = kernel_vma_set_destroy(&record->vmas);
            if (vma_status != KERNEL_VMA_STATUS_OK) {
                return KERNEL_MM_STATUS_STATE;
            }
        }
        drain_shared_anon(record, 0);
        record->stage = KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP;
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_FILE_SOURCES;
    }
    if (record->stage == KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP) {
        status = drain_file_sources(record, 0);
        if (status != KERNEL_MM_STATUS_OK) {
            mm->state = KERNEL_MM_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_FILE_SOURCES;
            return status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       ? status
                       : KERNEL_MM_STATUS_STATE;
        }
        record->stage = KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP;
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_ELF_SOURCES;
    }
    if (record->stage == KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP) {
        if (record->executable_file &&
            kernel_open_file_release(&record->executable_file) !=
                KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
        status = drain_elf_sources(record, 0);
        if (status != KERNEL_MM_STATUS_OK) {
            mm->state = KERNEL_MM_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_ELF_SOURCES;
            return status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       ? status
                       : KERNEL_MM_STATUS_STATE;
        }
        if (record->space.state == ARCH_MMU_USER_SPACE_EMPTY) {
            record->stage = KERNEL_MM_RECORD_ONLY_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
            status = release_record_only(mm);
            return status;
        }
    }

    backend_status = arch_mmu_user_space_destroy(&record->space);
    if (backend_status != ARCH_MMU_STATUS_OK) {
        __builtin_trap();
    }
    record->stage = KERNEL_MM_RECORD_ONLY_CLEANUP;
    mm->state = KERNEL_MM_CLEANUP;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
    status = release_record_only(mm);
    return status;
}

uint64_t kernel_mm_peak_resident_pages(const struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK) __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    return record->space.peak_resident_pages;
}

void kernel_mm_get_statistics(const struct kernel_mm *mm,
                                    struct kernel_mm_statistics *statistics)
{
    struct kernel_mm_record *record;
    if (statistics == 0 || resolve_record(mm, &record) != KERNEL_MM_STATUS_OK)
        __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    *statistics = (struct kernel_mm_statistics){record->resident_probes,
        record->space.protect_visits, record->space.protect_address_flushes,
        record->space.protect_global_flushes};
}
void kernel_mm_get_concurrency_state(const struct kernel_mm *mm,
    struct kernel_mm_concurrency_state *state)
{
    struct kernel_mm_record *record;
    if (!state || !mm || mm->state != KERNEL_MM_LIVE ||
        resolve_record(mm, &record) != KERNEL_MM_STATUS_OK) __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    *state = (struct kernel_mm_concurrency_state){
        .references = __atomic_load_n(&record->references, __ATOMIC_ACQUIRE),
        .users = __atomic_load_n(&record->users, __ATOMIC_ACQUIRE),
        .pins = record->access_pins, .write_pins = record->write_pins,
        .active_cpus = record->active_cpus, .pending_retirement = record->pending_retirement,
        .record_bytes = sizeof(*record), .pin_bytes = sizeof(struct kernel_mm_user_pin),
        .generation = record->tlb_generation, .pin_acquisitions = record->pin_acquisitions,
        .prepared_pages = record->prepared_pages, .fault_retries = record->fault_retries,
        .retirement_batches = record->retirement_batches, .retired_pages = record->retired_pages,
        .pin_waits = record->pin_waits,
    };
}

/* 入口均在已有MM owner下借用record；句柄不能被另一执行者并发move/release。 */
#define MM_OPERATION(mm_) \
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0}; \
    enum kernel_mm_status op_status = mm_operation_begin((mm_), &operation); \
    if (op_status != KERNEL_MM_STATUS_OK) return op_status

enum kernel_mm_status kernel_mm_set_data_layout(struct kernel_mm *mm,
    uint64_t start, uint64_t end)
{
    if (start > end || end >= ARCH_MMU_USER_LIMIT) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    MM_OPERATION(mm);
    operation.record->start_data = start;
    operation.record->end_data = end;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_check_memory_limits(const struct kernel_mm *mm,
    const struct kernel_memory_limits *limits)
{
    if (!limits) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    MM_OPERATION(mm);
    const struct kernel_vma data = {.permissions = KERNEL_MM_WRITE,
        .kind = KERNEL_VMA_KIND_ANONYMOUS};
    return may_expand_memory(operation.record, &data, 0, limits) ?
        KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_NO_MEMORY;
}

#if BOAROS_USER_SMP
/* Caller holds the MM gate; holes do not require visiting unrelated VMAs. */
static int range_has_shm(struct kernel_mm_record *record, uint64_t address, uint64_t length)
{
    if (!length || length > UINT64_MAX - BOAROS_PAGE_MASK || !record->vmas) return 0;
    uint64_t rounded = (length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    if (address >= ARCH_MMU_USER_LIMIT || rounded > ARCH_MMU_USER_LIMIT - address) return 0;
    uint64_t end = address + rounded, cursor = address;
    while (cursor < end) {
        struct kernel_vma vma;
        enum kernel_vma_status status = kernel_vma_set_lookup(record->vmas, cursor, &vma);
        if (status == KERNEL_VMA_STATUS_NOT_FOUND)
            status = kernel_vma_set_next(record->vmas, cursor, &vma);
        if (status == KERNEL_VMA_STATUS_NOT_FOUND || (!status && vma.start >= end)) break;
        if (status) __builtin_trap();
        if (vma.shm_attachment) return 1;
        cursor = vma.end;
    }
    return 0;
}
#endif

/* SysV附件的段目录仍由CPU0维护。两个实际编辑区间分别检查，避免把
 * mremap源尾和固定目标之间的无关附件也纳入执行位置判断。 */
static enum kernel_mm_status mm_ranges_operation_begin(const struct kernel_mm *mm,
    uint64_t address, uint64_t length, uint64_t second_address, uint64_t second_length,
    struct mm_operation *operation, struct kernel_service_scope *service)
{
    for (;;) {
        enum kernel_mm_status status = mm_operation_begin(mm, operation);
        if (status) return status;
#if BOAROS_USER_SMP
        if (kernel_task_has_user_context(kernel_task_current()) &&
            !kernel_scheduler_user_service_active() &&
            (range_has_shm(operation->record, address, length) ||
             range_has_shm(operation->record, second_address, second_length))) {
            mm_operation_end(operation);
            kernel_service_scope_begin(service);
            continue;
        }
#else
        (void)address; (void)length; (void)second_address; (void)second_length; (void)service;
#endif
        return KERNEL_MM_STATUS_OK;
    }
}
#define MM_RANGE_OPERATION(mm_, address_, length_) \
    KERNEL_SERVICE_SCOPE_IF(mm_service, 0); \
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0}; \
    enum kernel_mm_status op_status = mm_ranges_operation_begin((mm_), (address_), (length_), 0, 0, &operation, &mm_service); \
    if (op_status != KERNEL_MM_STATUS_OK) return op_status

/* 复制期间不能迁移或借用CPU0时，退休来源留在MM；真实用户返回前
 * 已结束所有offset/pin资格，再归还同一份来源owner。 */
void kernel_mm_drain_sources(const struct kernel_mm *mm)
{
    struct kernel_mm_record *record=mm->record;
    if (!__atomic_load_n(&record->retired_file_sources,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&record->retired_elf_sources,__ATOMIC_ACQUIRE)) return;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end)))={0};
    if (mm_operation_begin(mm,&operation)) __builtin_trap();
}

enum kernel_mm_status kernel_mm_mmap_file_private(struct kernel_mm *mm,
    struct kernel_open_file_description **file,uint64_t hint,uint64_t length,
    uint64_t offset,uint32_t permissions,uint32_t flags,uint64_t *address)
{
    if (!file || !*file || !address ||
        (kernel_open_file_mode(*file) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG)
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    for (;;) {
        MM_RANGE_OPERATION(mm, hint, (flags & KERNEL_MM_MAP_FIXED) ? length : 0);
        struct mm_file_mapping_plan plan;
        enum kernel_mm_status status=prepare_file_mapping(mm,hint,length,offset,permissions,flags,&plan);
        if (status != KERNEL_MM_STATUS_OK) return status;
        struct kernel_vfs_node *node=kernel_open_file_node(*file);
        uint64_t generation=kernel_vma_set_generation(operation.record->vmas);
        KERNEL_LOCK_SCOPE(inode);
        {
            kernel_vfs_node_lock(node,&inode,0);
            uint64_t epoch;
            if (kernel_file_mapping_epoch(node,&epoch)) {
                kernel_lock_release(&inode);
                if (operation.guard.lock) mm_gate_release(operation.record,&operation.guard);
                if (current_pins(mm)) return KERNEL_MM_STATUS_CONFLICT;
                kernel_file_mapping_wait(node);
                continue;
            }
            if (generation != kernel_vma_set_generation(operation.record->vmas)) continue;
        }
        status=commit_file_mapping(file,offset,flags,address,&plan,&inode);
        if (inode.lock) kernel_lock_release(&inode);
        uint32_t policy = mapping_lock_flags(operation.record, flags);
        mm_operation_end(&operation);
        if (!status && policy == KERNEL_VMA_LOCKED) (void)populate_locked(mm, *address, *address + ((length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK));
        return status;
    }
}

enum kernel_mm_status kernel_mm_validate_file_private_mapping(struct kernel_mm *mm,
    uint64_t hint,uint64_t length,uint64_t offset,uint32_t permissions,uint32_t flags)
{
    MM_OPERATION(mm);
    return validate_file_mapping(mm,hint,length,offset,permissions,flags);
}

static int sync_mapped_ranges(struct kernel_mm *mm,uint64_t address,uint64_t length,
    uint32_t flags,struct mm_operation *operation)
{
    uint64_t rounded=(length+BOAROS_PAGE_MASK)&~BOAROS_PAGE_MASK;
    uint64_t end=address+rounded,cursor=address;
    if (end<address) return -KERNEL_ENOMEM;
    int hole=0,result=0;
    while (cursor<end) {
        struct { struct kernel_mm_file_source *source; uint64_t start,end; } sources[16];
        unsigned count=0;
        while (cursor<end && count<16) {
            struct kernel_vma vma;
            enum kernel_vma_status status=kernel_vma_set_lookup(operation->record->vmas,cursor,&vma);
            if (status==KERNEL_VMA_STATUS_NOT_FOUND) {
                hole=1;
                if (flags==1U) { cursor=end; break; }
                status=kernel_vma_set_next(operation->record->vmas,cursor,&vma);
                if (status==KERNEL_VMA_STATUS_NOT_FOUND || vma.start>=end) { cursor=end; break; }
                if (status) __builtin_trap();
                cursor=vma.start;
            } else if (status) __builtin_trap();
            uint64_t finish=vma.end<end?vma.end:end;
            if ((flags&4U) && vma.kind==KERNEL_VMA_KIND_FILE_SHARED) {
                struct kernel_mm_file_source *source=find_file_source(operation->record,vma.backing);
                if (!source || source->operations==UINT32_MAX) __builtin_trap();
                source->operations++;
                sources[count].source=source;
                sources[count].start=vma.backing_offset+cursor-vma.start;
                sources[count].end=sources[count].start+finish-cursor;
                count++;
            }
            cursor=finish;
        }
        /* 来源节点的原OFD owner覆盖本批借用；最后VMA撤销也不能提前关闭。
         * 同步会回调同一MM，必须先结束gate，完成后再退休来源。 */
        mm_operation_end(operation);
        for (unsigned i=0;i<count;i++) {
            struct kernel_mm_file_source *source=sources[i].source;
            if (!result) result=kernel_open_file_sync_range(source->file,sources[i].start,sources[i].end);
            if (!__atomic_fetch_sub(&source->operations,1,__ATOMIC_RELEASE)) __builtin_trap();
        }
        enum kernel_mm_status acquired=mm_operation_begin(mm,operation);
        if (acquired) return acquired==KERNEL_MM_STATUS_CONFLICT?-KERNEL_EAGAIN:-KERNEL_ENOMEM;
        enum kernel_mm_status drained=drain_file_sources(operation->record,1);
        if (drained!=KERNEL_MM_STATUS_OK && drained!=KERNEL_MM_STATUS_CLEANUP_REQUIRED) __builtin_trap();
        if (result || cursor>=end) break;
    }
    return result?result:hole?-KERNEL_ENOMEM:0;
}

int kernel_mm_msync(struct kernel_mm *mm, uint64_t address,
    uint64_t length, uint32_t flags)
{
    if ((flags & ~7U) || (address & BOAROS_PAGE_MASK) ||
        ((flags & 1U) && (flags & 4U))) return -KERNEL_EINVAL;
    if (!length) {
        struct kernel_mm_record *record;
        return mutable_vma_record(mm, &record) == KERNEL_MM_STATUS_OK ? 0 : -KERNEL_ENOMEM;
    }
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    enum kernel_mm_status status = mm_operation_begin(mm, &operation);
    if (status != KERNEL_MM_STATUS_OK)
        return status == KERNEL_MM_STATUS_CONFLICT ? -KERNEL_EAGAIN : -KERNEL_ENOMEM;
    /* tmpfs同步也取得OFD rank10；所有并发MM都先固定来源并结束gate25，
     * 不能仅对磁盘来源释放gate后调用。 */
    return sync_mapped_ranges(mm,address,length,flags,&operation);
}

enum kernel_mm_status kernel_mm_mmap_anonymous(struct kernel_mm *mm,
    uint64_t hint, uint64_t length, uint32_t permissions, uint32_t flags, uint64_t *address)
{
    MM_RANGE_OPERATION(mm, hint, (flags & KERNEL_MM_MAP_FIXED) ? length : 0);
    enum kernel_mm_status result = mm_mmap_anonymous_impl(mm, hint, length, permissions, flags, address);
    uint32_t policy = mapping_lock_flags(operation.record, flags);
    mm_operation_end(&operation);
    if (!result && policy == KERNEL_VMA_LOCKED)
        (void)populate_locked(mm, *address, *address + ((length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK));
    return result;
}
enum kernel_mm_status kernel_mm_munmap(struct kernel_mm *mm, uint64_t address, uint64_t length)
{
    MM_RANGE_OPERATION(mm, address, length);
    return mm_munmap_impl(mm, address, length);
}
enum kernel_mm_status kernel_mm_mprotect(struct kernel_mm *mm,
    uint64_t address, uint64_t length, uint32_t permissions)
{
    uint32_t normalized; uint64_t aligned_length;
    if ((address & BOAROS_PAGE_MASK) || !normalize_user_permissions(permissions, &normalized))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (length && (!align_mapping_length(length, &aligned_length) ||
        address > ARCH_MMU_USER_LIMIT - aligned_length)) return KERNEL_MM_STATUS_NO_MEMORY;
    if (!length) return mm_mprotect_impl(mm, address, length, permissions);
    MM_RANGE_OPERATION(mm, address, length);
    close_pins(&operation, 0);
    return mm_mprotect_impl(mm, address, length, permissions);
}
enum kernel_mm_status kernel_mm_fork(struct kernel_mm *destination, struct kernel_mm *source)
{
    kernel_assert_service_context();
    if (!destination || !source || destination == source) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (!empty_handle(destination) || source->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    MM_OPERATION(source);
    close_pins(&operation, 1);
    return mm_fork_impl(destination, source);
}
enum kernel_mm_status kernel_mm_brk(struct kernel_mm *mm, uint64_t requested, uint64_t *result)
{
    if (!mm || !result) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    MM_OPERATION(mm);
    uint64_t old_end = (operation.record->current_brk + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    enum kernel_mm_status status = mm_brk_impl(mm, requested, result);
    uint64_t end = (*result + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    uint32_t policy = operation.record->future_lock_flags;
    mm_operation_end(&operation);
    if (!status && end > old_end && policy == KERNEL_VMA_LOCKED) (void)populate_locked(mm, old_end, end);
    return status;
}
enum kernel_mm_status kernel_mm_vma_lookup(const struct kernel_mm *mm,
    uint64_t address, struct kernel_vma *vma)
{
    if (!mm || !vma || address >= ARCH_MMU_USER_LIMIT) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    MM_OPERATION(mm);
    return mm_vma_lookup_impl(mm, address, vma);
}
static enum kernel_mm_status allocate_private_page(struct physical_page_allocator *allocator,
    uint64_t *address, void **pointer)
{
    enum physical_page_status status = physical_page_allocate(allocator, address);
    if (status == PHYSICAL_PAGE_STATUS_EMPTY) return KERNEL_MM_STATUS_NO_MEMORY;
    if (status != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (physical_page_map_owned(allocator, *address, pointer) != PHYSICAL_PAGE_STATUS_OK) {
        physical_page_release(allocator, *address); *address = KERNEL_MM_NO_PAGE;
        return KERNEL_MM_STATUS_PAGE_ACCESS;
    }
    return KERNEL_MM_STATUS_OK;
}

/* Reserve destination tables while all source leaves retain their owner. Empty
 * published tables are reclaimed through the normal TLB retirement path on OOM. */
static int prepare_remap_tables(struct kernel_mm_record *record,
    uint64_t from, uint64_t length, uint64_t to)
{
    uint64_t cursor = from, address;
    for (;;) {
        unsigned missing;
        {
            KERNEL_RAW_SCOPE(guard, &record->metadata);
            enum arch_mmu_status status = arch_mmu_user_next_owned(
                &record->space, &cursor, from + length, &address);
            if (status == ARCH_MMU_STATUS_NOT_MAPPED) return 0;
            if (status || arch_mmu_user_leaf_missing(&record->space,
                to + address - from, &missing)) __builtin_trap();
        }
        struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
        int error = 0;
        for (unsigned i = 0; i < 2; i++) if (missing & (1U << i)) {
            void *table;
            enum kernel_mm_status status = allocate_private_page(record->space.allocator,
                &prepared.tables[i], &table);
            if (status) { error = -KERNEL_ENOMEM; break; }
            clear_page(table);
        }
        if (!error) {
            KERNEL_RAW_SCOPE(guard, &record->metadata);
            if (arch_mmu_user_prepare_move(&record->space, to + address - from, &prepared))
                __builtin_trap();
        }
        for (unsigned i = 0; i < 2; i++) if (prepared.tables[i] != KERNEL_MM_NO_PAGE)
            if (physical_page_release(record->space.allocator, prepared.tables[i])) __builtin_trap();
        if (error) {
            if (unmap_space_range(record, to, to + length)) __builtin_trap();
            return error;
        }
    }
}

static void move_remap_leaves(struct kernel_mm_record *record,
    uint64_t from, uint64_t length, uint64_t to, int executable)
{
    uint64_t cursor = from, address;
    for (;;) {
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        enum arch_mmu_status status = arch_mmu_user_next_owned(
            &record->space, &cursor, from + length, &address);
        if (status == ARCH_MMU_STATUS_NOT_MAPPED) break;
        if (status || arch_mmu_user_move_leaf(&record->space, address, to + address - from))
            __builtin_trap();
        struct mm_file_resident *page = find_file_resident(record, address);
        if (page) {
            unindex_file_resident(record, page);
            page->address = to + address - from;
            publish_file_resident(record, page);
        }
    }
    struct kernel_mm_retirement batch = {0};
    uint32_t targets;
    {
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        targets = note_tlb_change_locked(record, &batch, executable);
    }
    /* 新旧VA不连续；先确认所有CPU都不再借用旧翻译，再回收源空表。 */
    finish_tlb_change(record, &batch, targets, 0, UINT64_MAX, executable);
    if (unmap_space_range(record, from, from + length)) __builtin_trap();
}

int kernel_mm_mremap(struct kernel_mm *mm, uint64_t old_address,
    uint64_t old_length, uint64_t new_length, uint64_t flags,
    uint64_t new_address, uint64_t *result)
{
    KERNEL_NO_RECLAIM_IO;
    uint64_t old_size = (old_length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    uint64_t new_size = (new_length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    if (!result || (flags & ~UINT64_C(7)) || (old_address & BOAROS_PAGE_MASK) ||
        !new_size || new_size > ARCH_MMU_USER_LIMIT) return -KERNEL_EINVAL;
    if (flags & 6U) {
        if (new_address > ARCH_MMU_USER_LIMIT - new_size ||
            (new_address & BOAROS_PAGE_MASK) || !(flags & 1U) ||
            ((flags & 4U) && old_size != new_size)) return -KERNEL_EINVAL;
        if (old_size && old_address < new_address + new_size &&
            (old_address > UINT64_MAX - old_size || new_address < old_address + old_size))
            return -KERNEL_EINVAL;
    }
    KERNEL_SERVICE_SCOPE_IF(service, 0);
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    uint64_t tail_length = new_size < old_size && old_address < ARCH_MMU_USER_LIMIT &&
        old_size <= ARCH_MMU_USER_LIMIT - old_address ? old_size - new_size : 0;
    enum kernel_mm_status status = mm_ranges_operation_begin(mm, new_address,
        (flags & 2U) ? new_size : 0, tail_length ? old_address + new_size : 0,
        tail_length, &operation, &service);
    if (status) return status == KERNEL_MM_STATUS_CONFLICT ? -KERNEL_EAGAIN : -KERNEL_ENOMEM;
    struct kernel_mm_record *record;
    if (mutable_vma_record(mm, &record)) return -KERNEL_ENOMEM;
    struct kernel_vma source;
    if (old_address >= ARCH_MMU_USER_LIMIT ||
        kernel_vma_set_lookup(record->vmas, old_address, &source)) return -KERNEL_EFAULT;
    if (!old_size && source.kind != KERNEL_VMA_KIND_ANON_SHARED &&
        source.kind != KERNEL_VMA_KIND_FILE_SHARED && source.kind != KERNEL_VMA_KIND_SYSV_SHM)
        return -KERNEL_EINVAL;
    uint64_t covered = new_size < old_size ? new_size : old_size;
    if (((flags & 6U) || new_size > old_size) && covered > source.end - old_address) {
        if ((flags & 6U) == 2U && new_size == old_size &&
            old_size <= ARCH_MMU_USER_LIMIT - old_address) {
            /* 多VMA等长批量搬移尚未实现；只在这条边界核验洞与合法未支持。 */
            uint64_t cursor = source.end;
            struct kernel_vma next;
            while (cursor < old_address + old_size) {
                if (kernel_vma_set_lookup(record->vmas, cursor, &next)) return -KERNEL_EFAULT;
                cursor = next.end;
            }
            return -KERNEL_EOPNOTSUPP;
        }
        return -KERNEL_EFAULT;
    }
    if (flags & 4U) return -KERNEL_EOPNOTSUPP;
    if (!old_size) return (flags & 1U) ? -KERNEL_EOPNOTSUPP : -KERNEL_ENOMEM;
    if (!(flags & 2U) && new_size <= old_size) {
        if (new_size < old_size) {
            status = mm_munmap_impl(mm, old_address + new_size, old_size - new_size);
            if (status) return status == KERNEL_MM_STATUS_INVALID_ARGUMENT ? -KERNEL_EINVAL : -KERNEL_ENOMEM;
        }
        *result = old_address;
        return 0;
    }
    if (new_size > old_size && !may_expand_current(record, &source, new_size - old_size))
        return -KERNEL_ENOMEM;
    if (new_size > old_size && source.lock_flags && !may_lock_memory(record, new_size - old_size))
        return -KERNEL_EAGAIN;
    if (!(flags & 2U)) {
        int overlaps = 1;
        if (old_address <= ARCH_MMU_USER_LIMIT - new_size &&
            old_address + old_size == source.end &&
            kernel_vma_set_overlaps(record->vmas, source.end,
                old_address + new_size, &overlaps)) __builtin_trap();
        if (!overlaps) new_address = old_address;
        else {
            if (!(flags & 1U)) return -KERNEL_ENOMEM;
            enum kernel_vma_status vs = kernel_vma_set_find_topdown_gap(record->vmas,
                0, BOAROS_PAGE_SIZE, record->mmap_base, new_size, &new_address);
            if (vs) return -KERNEL_ENOMEM;
        }
    }
    if (source.role != KERNEL_VMA_ROLE_MMAP || source.kind == KERNEL_VMA_KIND_SYSV_SHM ||
        source.kind == KERNEL_VMA_KIND_ELF_PRIVATE) return -KERNEL_EOPNOTSUPP;
    uint64_t offset = source.kind == KERNEL_VMA_KIND_ANONYMOUS ? 0 :
        source.backing_offset + old_address - source.start;
    if (offset < source.backing_offset || offset > UINT64_MAX - new_size)
        return -KERNEL_EINVAL;
    if (flags & 2U) {
        if (new_address < BOAROS_PAGE_SIZE) return -KERNEL_EPERM;
        /* Linux先撤固定目标；后续准备失败不恢复被替换映射。 */
        status = mm_munmap_impl(mm, new_address, new_size);
        if (status) return -KERNEL_ENOMEM;
        if (new_size < old_size) {
            status = mm_munmap_impl(mm, old_address + new_size, old_size - new_size);
            if (status) return -KERNEL_ENOMEM;
            old_size = new_size;
        }
        if (kernel_vma_set_lookup(record->vmas, old_address, &source)) return -KERNEL_EFAULT;
    }
    struct kernel_vma replacement = source;
    replacement.start = new_address;
    replacement.end = new_address + new_size;
    replacement.backing_offset = offset;
    struct kernel_vma_remap edit;
    if (kernel_vma_set_prepare_remap(record->vmas, old_address, old_address + old_size,
        &replacement, &edit)) return -KERNEL_ENOMEM;
    if (new_address != old_address) {
        if (current_pins(mm)) return -KERNEL_EAGAIN;
        close_pins(&operation, 0);
        int error = prepare_remap_tables(record, old_address, old_size, new_address);
        if (error) return error;
        move_remap_leaves(record, old_address, old_size, new_address,
            (source.permissions & KERNEL_MM_EXECUTE) != 0);
    }
    if (kernel_vma_set_commit_remap(record->vmas, &edit)) __builtin_trap();
    *result = new_address;
    mm_operation_end(&operation);
    if (new_size > old_size && source.lock_flags == KERNEL_VMA_LOCKED)
        (void)populate_locked(mm, new_address + old_size, new_address + new_size);
    return 0;
}

/* gate固定resident/alias，许可由页write状态仲裁；等待时结束全部对象保护。 */
static int fault_inode_lock(struct mm_operation *,struct kernel_vfs_node *,struct kernel_lock_guard *);
static enum kernel_mm_status cached_write_fault(struct mm_operation *operation,
    struct kernel_open_file_description *file,const struct kernel_vma *vma,uint64_t address,
    struct kernel_lock_guard *inode)
{
    struct kernel_mm_record *record=operation->record;
    struct kernel_vfs_node *node=kernel_open_file_node(file);
    uint64_t generation=kernel_vma_set_generation(record->vmas),epoch,latest;
    if (kernel_file_mapping_epoch(node,&epoch)) return KERNEL_MM_STATUS_CONFLICT;
    uint64_t offset=vma->backing_offset+address-vma->start;
    /* 准备磁盘空间不能持有 MM gate 或 inode 读资格；同文件源缺页、
     * truncate 与后端预算等待都需要这些对象继续推进。返回后重查身份。 */
    if (inode->lock) kernel_lock_release(inode);
    mm_gate_release(record,&operation->guard);
    int error=kernel_page_cache_prepare_mapping_write(kernel_open_file_page_cache(file),node,offset>>BOAROS_PAGE_SHIFT);
    enum kernel_mm_status locked=mm_operation_lock(operation);
    if (locked) return locked;
    if (generation!=kernel_vma_set_generation(record->vmas) || !fault_inode_lock(operation,node,inode))
        return KERNEL_MM_STATUS_CONFLICT;
    if (kernel_file_mapping_epoch(node,&latest) || latest!=epoch || offset>=kernel_open_file_size(file))
        return KERNEL_MM_STATUS_CONFLICT;
    if (error) return error==-KERNEL_ENOMEM?KERNEL_MM_STATUS_NO_MEMORY:
        error==-KERNEL_EBUSY?KERNEL_MM_STATUS_CONFLICT:KERNEL_MM_STATUS_BUS_FAULT;
    struct mm_file_resident *resident=find_file_resident(record,address);
    if (!resident || !kernel_page_cache_alias_active(&resident->alias)) return KERNEL_MM_STATUS_CONFLICT;
    struct kernel_page_cache_write_borrow permit={0};
    enum kernel_page_cache_status admitted=kernel_page_cache_alias_write_begin(&resident->alias,&permit);
    if (admitted==KERNEL_PAGE_CACHE_STATUS_BUSY) {
        uint64_t physical=resident->physical_address;
        if (inode->lock) kernel_lock_release(inode);
        if (operation->guard.lock) mm_gate_release(operation->record,&operation->guard);
        if (!current_pins(operation->mm)) {
            uint64_t index=(vma->backing_offset+address-vma->start)>>BOAROS_PAGE_SHIFT;
            (void)kernel_page_cache_wait_writable(kernel_open_file_page_cache(file),kernel_open_file_vfs_borrow(file),index,physical);
        }
        return KERNEL_MM_STATUS_CONFLICT;
    }
    if (admitted) __builtin_trap();
    kernel_page_cache_alias_mark_dirty(&resident->alias);
    struct kernel_mm_retirement batch={0}; uint32_t targets;
    {
        KERNEL_RAW_SCOPE(guard,&record->metadata);
        if (arch_mmu_user_protect_owned_page(&record->space,address,
            backend_permissions_from_mm(vma->permissions))) __builtin_trap();
        targets=note_tlb_change_locked(record, &batch, 0);
    }
    finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,0);
    kernel_page_cache_write_end(&permit);
    return KERNEL_MM_STATUS_OK;
}

/* inode等待可能与另一个映射编辑交错；不能带着gate阻止它完成。
 * 等待结束只重试，不沿用等待前取得的VMA或候选身份。 */
static int fault_inode_lock(struct mm_operation *operation, struct kernel_vfs_node *node,
    struct kernel_lock_guard *inode)
{
    if (kernel_vfs_node_try_read(node, inode)) return 1;
    mm_gate_release(operation->record, &operation->guard);
    kernel_vfs_node_lock(node, inode, 0);
    kernel_lock_release(inode);
    return 0;
}

/* File-map preparation is independent of ELF/anonymous faults. Keep its
 * retirement and COW storage off the nested ELF-read stack. */
static __attribute__((noinline)) enum kernel_mm_status mapped_file_fault(struct mm_operation *operation,
    uint64_t address, uint32_t access)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record = operation->record;
    struct physical_page_allocator *allocator = record->space.allocator;
    struct kernel_vma vma;
    if (!access || (access & (access - 1)) || (access & ~KERNEL_VMA_ALL_PERMISSIONS))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (kernel_vma_set_lookup(record->vmas, address, &vma) != KERNEL_VMA_STATUS_OK)
        return KERNEL_MM_STATUS_NOT_MAPPED;
    if (!(vma.permissions & access)) return KERNEL_MM_STATUS_ACCESS;
    address &= ~BOAROS_PAGE_MASK;
    if (vma.backing_offset > UINT64_MAX - (address - vma.start))
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    uint64_t offset = vma.backing_offset + address - vma.start;
    uint64_t generation = kernel_vma_set_generation(record->vmas);
    struct kernel_open_file_description *file = vma.backing;
    struct kernel_mm_file_source *file_source = find_file_source(record, file);
    if (!file_source || file_source->operations == UINT32_MAX) __builtin_trap();
    file_source->operations++;
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    KERNEL_LOCK_SCOPE(inode);
    uint64_t epoch;
    uint64_t cookie = 0, old = KERNEL_MM_NO_PAGE, replacement = KERNEL_MM_NO_PAGE;
    struct kernel_open_file_page_preparation source = {0};
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    struct kernel_mm_retirement batch = {0};
    struct mm_file_resident *resident = 0;
    void *old_bytes = 0, *copy = 0;
    unsigned missing = 0;
#if BOAROS_COST_DIAGNOSTICS
    unsigned count = 0;
#endif
    uint32_t targets = 0;
    int shared = vma.kind == KERNEL_VMA_KIND_FILE_SHARED;
    int disk = !kernel_open_file_memory_backed(file);
    enum kernel_mm_status status = KERNEL_MM_STATUS_OK;
    if (!fault_inode_lock(operation, node, &inode)) {
        status = KERNEL_MM_STATUS_CONFLICT;
        goto finish;
    }
    if (generation != kernel_vma_set_generation(record->vmas)) {
        status=KERNEL_MM_STATUS_CONFLICT;
        goto finish;
    }
    if (kernel_file_mapping_epoch(node, &epoch)) {
        kernel_lock_release(&inode);
        if (operation->guard.lock) mm_gate_release(operation->record,&operation->guard);
        if (!current_pins(operation->mm)) kernel_file_mapping_wait(node);
        status = KERNEL_MM_STATUS_CONFLICT;
        goto finish;
    }
    if (offset >= kernel_open_file_size(file)) { status = KERNEL_MM_STATUS_BUS_FAULT; goto finish; }
    {
        struct kernel_mm_mapping mapping;
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        status = lookup_record(record, address, &mapping);
        if (status == KERNEL_MM_STATUS_OK && shared && disk && access==KERNEL_MM_WRITE) {
            kernel_raw_lock_release(&guard);
            status=cached_write_fault(operation,file,&vma,address,&inode);
            goto finish;
        }
        if (status == KERNEL_MM_STATUS_OK && (mapping.permissions & access)) {
            goto finish;
        }
        if (status == KERNEL_MM_STATUS_OK) {
            if (shared || access != KERNEL_MM_WRITE ||
                arch_mmu_user_cow_snapshot(&record->space,address,&cookie,&old) != ARCH_MMU_STATUS_OK) {
                status = KERNEL_MM_STATUS_ADDRESS_SPACE;
                goto finish;
            }
            uint32_t refs;
            if (physical_page_reference_count(allocator,old,&refs)) __builtin_trap();
            if (refs == 1) {
                if (arch_mmu_user_cow_commit(&record->space,address,cookie,KERNEL_MM_NO_PAGE,
                        backend_permissions_from_mm(vma.permissions),&batch)) __builtin_trap();
                targets = note_tlb_change_locked(record, &batch, (vma.permissions & KERNEL_MM_EXECUTE) != 0);
                kernel_raw_lock_release(&guard);
                finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,
                    (vma.permissions & KERNEL_MM_EXECUTE) != 0);
                status = KERNEL_MM_STATUS_OK;
                old = KERNEL_MM_NO_PAGE;
                goto finish;
            }
            if (physical_page_acquire(allocator,old) || physical_page_map_owned(allocator,old,&old_bytes))
                __builtin_trap();
        } else if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
            if (arch_mmu_user_leaf_missing(&record->space,address,&missing)) __builtin_trap();
            status = KERNEL_MM_STATUS_OK;
        } else goto finish;
    }
    /* 来源引用覆盖inode/对象/预算；解锁后不保存registry节点或PTE指针。 */
    kernel_lock_release(&inode);
    if (operation->guard.lock) mm_gate_release(operation->record,&operation->guard);
    if (!cookie) {
        int error = kernel_open_file_prepare_mapping_page(file,offset >> BOAROS_PAGE_SHIFT,&source);
        status = !error ? KERNEL_MM_STATUS_OK : error == -KERNEL_EBUSY ? KERNEL_MM_STATUS_CONFLICT :
            error == -KERNEL_ENOMEM ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_BUS_FAULT;
        if (status == KERNEL_MM_STATUS_OK) {
            MM_DIAGNOSTIC(count += source.created);
            replacement = source.address;
        }
    }
    if (status == KERNEL_MM_STATUS_OK && !shared && access == KERNEL_MM_WRITE) {
        status = allocate_private_page(allocator,&replacement,&copy);
        if (status == KERNEL_MM_STATUS_OK) {
            void *bytes = old_bytes;
            if (!cookie && physical_page_map_owned(allocator,source.address,&bytes)) __builtin_trap();
            memcpy(copy, bytes, BOAROS_PAGE_SIZE);
            MM_DIAGNOSTIC(count++);
        }
    }
    if (status == KERNEL_MM_STATUS_OK && !cookie) {
        enum kernel_heap_status hs = kernel_heap_allocate_zeroed(record->vma_heap,1,sizeof(*resident),(void **)&resident);
        if (hs != KERNEL_HEAP_STATUS_OK)
            status = hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
    }
prepare_file_tables:
    for (unsigned i=0; i<2 && status==KERNEL_MM_STATUS_OK; i++) if ((missing & (1U << i)) && prepared.tables[i]==KERNEL_MM_NO_PAGE) {
        void *table;
        status = allocate_private_page(allocator,&prepared.tables[i],&table);
        if (status == KERNEL_MM_STATUS_OK) { clear_page(table); MM_DIAGNOSTIC(count++); }
    }
    enum kernel_mm_status gate_status = mm_operation_lock(operation);
    if (gate_status != KERNEL_MM_STATUS_OK) status = gate_status;
#if BOAROS_COST_DIAGNOSTICS
    { KERNEL_RAW_SCOPE(guard,&record->metadata); record->prepared_pages += count; count=0; }
#endif
    if (status == KERNEL_MM_STATUS_OK) {
        if (!fault_inode_lock(operation, node, &inode)) status = KERNEL_MM_STATUS_CONFLICT;
        uint64_t latest;
        if (!status && (generation != kernel_vma_set_generation(record->vmas) ||
            kernel_file_mapping_epoch(node,&latest) || latest != epoch ||
            (source.pending && source.epoch != epoch) || offset >= kernel_open_file_size(file)))
            status = KERNEL_MM_STATUS_CONFLICT;
    }
    if (status == KERNEL_MM_STATUS_OK && !cookie)
        status = reserve_file_residents(record,record->resident_count + 1);
    if (status == KERNEL_MM_STATUS_OK) {
        struct kernel_vma latest;
        if (generation != kernel_vma_set_generation(record->vmas) ||
            kernel_vma_set_lookup(record->vmas,address,&latest) != KERNEL_VMA_STATUS_OK ||
            latest.backing != file || latest.start != vma.start || latest.backing_offset != vma.backing_offset ||
            latest.permissions != vma.permissions || latest.kind != vma.kind ||
            latest.fault_policy != vma.fault_policy) status = KERNEL_MM_STATUS_CONFLICT;
    }
    if (status == KERNEL_MM_STATUS_OK && !cookie) {
        int refill = 0;
        {
            KERNEL_RAW_SCOPE(guard,&record->metadata);
            struct kernel_mm_mapping mapping;
            enum kernel_mm_status current=lookup_record(record,address,&mapping);
            if (current==KERNEL_MM_STATUS_NOT_MAPPED) {
                if (arch_mmu_user_leaf_missing(&record->space,address,&missing)) __builtin_trap();
                for (unsigned i=0;i<2;i++)
                    if ((missing & (1U<<i)) && prepared.tables[i]==KERNEL_MM_NO_PAGE) refill=1;
            } else status=current==KERNEL_MM_STATUS_OK?KERNEL_MM_STATUS_CONFLICT:current;
        }
        if (refill) {
            /* truncate可撤掉相邻文件的最后一叶，VMA代次不会因此改变。
             * 保留来源/数据页和已有表页，只在锁外补这次walk真正缺的槽。 */
            kernel_lock_release(&inode);
            mm_gate_release(record,&operation->guard);
            goto prepare_file_tables;
        }
    }
    if (status == KERNEL_MM_STATUS_OK) {
        KERNEL_RAW_SCOPE(guard,&record->metadata);
        enum arch_mmu_status backend;
        if (cookie) backend = arch_mmu_user_cow_commit(&record->space,address,cookie,replacement,
            backend_permissions_from_mm(vma.permissions),&batch);
        else {
            struct kernel_mm_mapping mapping;
            backend = lookup_record(record,address,&mapping) == KERNEL_MM_STATUS_NOT_MAPPED ?
                arch_mmu_user_map_prepared_page(&record->space,address,replacement,
                    backend_permissions_from_mm((!shared && access != KERNEL_MM_WRITE) || (shared && disk) ?
                        vma.permissions & ~KERNEL_MM_WRITE : vma.permissions),&prepared) : ARCH_MMU_STATUS_CONFLICT;
            /* 只读叶先发布，再标记COW；来源页从未短暂暴露为可写。 */
            if (!backend && !shared && access != KERNEL_MM_WRITE &&
                arch_mmu_user_mark_cow_page(&record->space,address)) __builtin_trap();
        }
        status = !backend ? KERNEL_MM_STATUS_OK : backend == ARCH_MMU_STATUS_CONFLICT ||
            backend == ARCH_MMU_STATUS_NOT_MAPPED ? KERNEL_MM_STATUS_CONFLICT : KERNEL_MM_STATUS_ADDRESS_SPACE;
        if (status == KERNEL_MM_STATUS_OK) {
            struct mm_file_resident *page = resident ? resident : find_file_resident(record,address);
            if (!page) __builtin_trap();
            page->address=address; page->physical_address=replacement;
            page->private=!shared && access==KERNEL_MM_WRITE;
            if (resident) { publish_file_resident(record,resident); resident=0; }
            targets=note_tlb_change_locked(record, &batch, (vma.permissions & KERNEL_MM_EXECUTE) != 0);
        }
    }
    if (status == KERNEL_MM_STATUS_OK) {
        finish_tlb_change(record,&batch,targets,missing?0:address,
            missing?UINT64_MAX:BOAROS_PAGE_SIZE,(vma.permissions & KERNEL_MM_EXECUTE)!=0);
        if (source.pending) {
            kernel_open_file_finish_mapping_page(file,&source,1);
            if (!shared && access==KERNEL_MM_WRITE) physical_page_release(allocator,source.address);
        }
        replacement=KERNEL_MM_NO_PAGE;
        if (shared && disk && !cookie) {
            struct mm_file_resident *page=find_file_resident(record,address);
            if (!page || kernel_open_file_alias_attach(file,offset>>BOAROS_PAGE_SHIFT,
                page->physical_address,&page->alias,page,address,rearm_shared_file_alias)) __builtin_trap();
        }
        if (shared && access==KERNEL_MM_WRITE) {
            if (disk) status=cached_write_fault(operation,file,&vma,address,&inode);
            else kernel_open_file_memory_modified(file);
        }
    }
finish:
    if (inode.lock) kernel_lock_release(&inode);
    if (source.pending) {
        if (replacement == source.address) replacement=KERNEL_MM_NO_PAGE;
        kernel_open_file_finish_mapping_page(file,&source,0);
    }
    if (replacement != KERNEL_MM_NO_PAGE) physical_page_release(allocator,replacement);
    if (old != KERNEL_MM_NO_PAGE && old_bytes) physical_page_release(allocator,old);
    for (unsigned i=0; i<2; i++) if (prepared.tables[i] != KERNEL_MM_NO_PAGE) physical_page_release(allocator,prepared.tables[i]);
    if (resident) kernel_heap_release(record->vma_heap,resident);
    if (!__atomic_fetch_sub(&file_source->operations, 1, __ATOMIC_RELEASE)) __builtin_trap();
    if (operation->guard.lock) (void)drain_file_sources(record, 1);
    mm_operation_end(operation);
    return status;
}
static __attribute__((noinline)) enum kernel_mm_status memory_fault(struct mm_operation *operation,
    uint64_t address, uint32_t access)
{
    struct kernel_mm_record *record = operation->record;
    struct physical_page_allocator *allocator = record->space.allocator;
    struct kernel_vma vma;
    if (!access || (access & (access - 1)) || (access & ~KERNEL_VMA_ALL_PERMISSIONS))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (address < BOAROS_PAGE_SIZE || address >= ARCH_MMU_USER_LIMIT)
        return KERNEL_MM_STATUS_NOT_MAPPED;
    if (!record->vmas) return KERNEL_MM_STATUS_NOT_MAPPED;
    enum kernel_vma_status vs = kernel_vma_set_lookup(record->vmas, address, &vma);
    if (vs == KERNEL_VMA_STATUS_NOT_FOUND) {
        uint64_t page = address & ~BOAROS_PAGE_MASK;
        if (kernel_vma_set_next(record->vmas, page, &vma) ||
            vma.role != KERNEL_VMA_ROLE_STACK || !(vma.permissions & access) ||
            !record->stack_top || record->stack_top < page ||
            record->stack_top - page > KERNEL_RLIMIT_STACK_CAP ||
            record->stack_top - page > kernel_task_current_stack_limit() ||
            !may_expand_current(record, &vma, vma.start - page) ||
            (vma.lock_flags && !may_lock_memory(record, vma.start - page)))
            return KERNEL_MM_STATUS_NOT_MAPPED;
        int overlaps;
        /* 保持栈前的一页guard；下一VMA检索也阻止跨越已有映射。 */
        if (page < 2 * BOAROS_PAGE_SIZE ||
            kernel_vma_set_overlaps(record->vmas, page - BOAROS_PAGE_SIZE, vma.start, &overlaps) || overlaps)
            return KERNEL_MM_STATUS_NOT_MAPPED;
        struct kernel_vma growth = vma;
        growth.start = page;
        growth.end = vma.start;
        vs = kernel_vma_set_insert(record->vmas, &growth);
        if (vs) return status_from_vma(vs);
        if (kernel_vma_set_lookup(record->vmas, address, &vma)) __builtin_trap();
    }
    if (vs != KERNEL_VMA_STATUS_OK) return KERNEL_MM_STATUS_STATE;
    if (!(vma.permissions & access)) return KERNEL_MM_STATUS_ACCESS;
    int shared = vma.kind == KERNEL_VMA_KIND_ANON_SHARED || vma.kind == KERNEL_VMA_KIND_SYSV_SHM;
    int elf = vma.kind == KERNEL_VMA_KIND_ELF_PRIVATE;
    if (!shared && !elf && vma.kind != KERNEL_VMA_KIND_ANONYMOUS) __builtin_trap();
    struct kernel_memory_object *object = shared ? vma.backing : 0;
    struct kernel_elf64_source *source = NULL;
    struct kernel_mm_elf_source *source_owner = NULL;
    int source_shared = 0;
    int created = 0;
    enum kernel_mm_status status = KERNEL_MM_STATUS_OK;
    address &= ~BOAROS_PAGE_MASK;
    uint64_t generation = kernel_vma_set_generation(record->vmas), cookie = 0, old = 0;
    uint64_t replacement = KERNEL_MM_NO_PAGE;
    uint64_t shared_preparation = KERNEL_MM_NO_PAGE;
    void *old_pointer = 0, *new_pointer;
    unsigned missing = 0;
#if BOAROS_COST_DIAGNOSTICS
    unsigned prepared_count = 0;
#endif
    uint32_t targets;
    struct kernel_mm_retirement batch = {0};
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    {
        struct kernel_mm_mapping mapping;
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        status = lookup_record(record, address, &mapping);
        if (status == KERNEL_MM_STATUS_OK && (mapping.permissions & access)) {
            return KERNEL_MM_STATUS_OK;
        }
        if (status == KERNEL_MM_STATUS_OK) {
            if (shared || access != KERNEL_MM_WRITE || arch_mmu_user_cow_snapshot(&record->space,
                address, &cookie, &old) != ARCH_MMU_STATUS_OK) return KERNEL_MM_STATUS_ADDRESS_SPACE;
            uint32_t references;
            if (physical_page_reference_count(allocator, old, &references) != PHYSICAL_PAGE_STATUS_OK)
                __builtin_trap();
            if (references == 1) {
                if (arch_mmu_user_cow_commit(&record->space, address, cookie, KERNEL_MM_NO_PAGE,
                    backend_permissions_from_mm(vma.permissions), &batch) != ARCH_MMU_STATUS_OK)
                    __builtin_trap();
                targets = note_tlb_change_locked(record, &batch, (vma.permissions & KERNEL_MM_EXECUTE) != 0);
                kernel_raw_lock_release(&guard);
                finish_tlb_change(record, &batch, targets, address, BOAROS_PAGE_SIZE,
                    (vma.permissions & KERNEL_MM_EXECUTE) != 0);
                return KERNEL_MM_STATUS_OK;
            }
            /* COW旧页为只读快照；实际引用覆盖解锁后的复制和竞争者提交。 */
            if (physical_page_acquire(allocator, old) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
            if (physical_page_map_owned(allocator, old, &old_pointer) != PHYSICAL_PAGE_STATUS_OK) {
                physical_page_release(allocator, old);
                return KERNEL_MM_STATUS_PAGE_ACCESS;
            }
        } else if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
            if (!shared && !elf && vma.fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO) return status;
            if (arch_mmu_user_leaf_missing(&record->space, address, &missing) != ARCH_MMU_STATUS_OK)
                return KERNEL_MM_STATUS_ADDRESS_SPACE;
        } else return status;
    }
    /* VMA注册覆盖此处取得实际后备owner；放gate后不再借用registry节点。 */
    if (object && kernel_memory_object_acquire(object) != KERNEL_MEMORY_OBJECT_OK)
        return KERNEL_MM_STATUS_STATE;
    if (elf && !cookie) {
        source = vma.backing;
        source_owner = find_elf_source(record, source);
        if (!source_owner || source_owner->faults == UINT32_MAX) __builtin_trap();
        source_owner->faults++;
    }
    mm_gate_release(operation->record,&operation->guard);
    if (source) {
        enum kernel_elf64_source_status result = kernel_elf64_source_page(source, allocator,
            vma.backing_offset + address - vma.start, &replacement, &source_shared);
        status = !result ? KERNEL_MM_STATUS_OK : result == KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY ?
            KERNEL_MM_STATUS_NO_MEMORY : result == KERNEL_ELF64_SOURCE_STATUS_IO ?
            KERNEL_MM_STATUS_BUS_FAULT : result == KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED ?
            KERNEL_MM_STATUS_CLEANUP_REQUIRED : KERNEL_MM_STATUS_ADDRESS_SPACE;
        if (!status && source_shared && access == KERNEL_MM_WRITE) {
            uint64_t original = replacement;
            void *bytes;
            replacement = KERNEL_MM_NO_PAGE;
            status = allocate_private_page(allocator, &replacement, &new_pointer);
            if (!status) {
                if (physical_page_map_owned(allocator, original, &bytes)) __builtin_trap();
                memcpy(new_pointer, bytes, BOAROS_PAGE_SIZE);
                source_shared = 0;
            }
            physical_page_release(allocator, original);
        }
    } else if (object) {
        enum kernel_memory_object_status result = kernel_memory_object_prepare_page(object,
            (vma.backing_offset + address - vma.start) >> BOAROS_PAGE_SHIFT,
            &replacement, &created);
        status = result == KERNEL_MEMORY_OBJECT_OK ? KERNEL_MM_STATUS_OK :
            result == KERNEL_MEMORY_OBJECT_NO_MEMORY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
        if (status == KERNEL_MM_STATUS_OK) {
            MM_DIAGNOSTIC(prepared_count += created != 0);
            shared_preparation = replacement;
        }
    } else {
        status = allocate_private_page(allocator, &replacement, &new_pointer);
        if (status == KERNEL_MM_STATUS_OK) {
            MM_DIAGNOSTIC(prepared_count++);
            if (cookie) {
                memcpy(new_pointer, old_pointer, BOAROS_PAGE_SIZE);
            } else clear_page(new_pointer);
        }
    }
prepare_memory_tables:
    for (unsigned i = 0; i < 2 && status == KERNEL_MM_STATUS_OK; i++) if ((missing & (1U << i)) && prepared.tables[i] == KERNEL_MM_NO_PAGE) {
        void *pointer;
        status = allocate_private_page(allocator, &prepared.tables[i], &pointer);
        if (status == KERNEL_MM_STATUS_OK) { clear_page(pointer); MM_DIAGNOSTIC(prepared_count++); }
    }
    enum kernel_mm_status gate_status = mm_operation_lock(operation);
    if (gate_status != KERNEL_MM_STATUS_OK) status = gate_status;
#if BOAROS_COST_DIAGNOSTICS
    { KERNEL_RAW_SCOPE(guard, &record->metadata); record->prepared_pages += prepared_count; prepared_count=0; }
#endif
    int refill = 0;
    if (status == KERNEL_MM_STATUS_OK) {
        struct kernel_mm_mapping current;
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        struct kernel_vma latest;
        if (generation != kernel_vma_set_generation(record->vmas) ||
            kernel_vma_set_lookup(record->vmas, address, &latest) != KERNEL_VMA_STATUS_OK ||
            latest.backing != vma.backing || latest.backing_offset != vma.backing_offset ||
            latest.start != vma.start || latest.permissions != vma.permissions ||
            latest.kind != vma.kind || latest.fault_policy != vma.fault_policy)
            status = KERNEL_MM_STATUS_CONFLICT;
        else {
            enum arch_mmu_status backend;
            if (cookie) backend = arch_mmu_user_cow_commit(&record->space, address, cookie,
                replacement, backend_permissions_from_mm(vma.permissions), &batch);
            else if (lookup_record(record, address, &current) != KERNEL_MM_STATUS_NOT_MAPPED)
                backend = ARCH_MMU_STATUS_CONFLICT;
            else {
                if (arch_mmu_user_leaf_missing(&record->space,address,&missing)) __builtin_trap();
                for (unsigned i=0;i<2;i++)
                    if ((missing & (1U<<i)) && prepared.tables[i]==KERNEL_MM_NO_PAGE) refill=1;
                if (refill) goto memory_tables_changed;
                backend = arch_mmu_user_map_prepared_page(&record->space, address, replacement,
                    backend_permissions_from_mm(source_shared ? vma.permissions & ~KERNEL_MM_WRITE :
                        vma.permissions), &prepared);
                if (!backend && source_shared && arch_mmu_user_mark_cow_page(&record->space, address))
                    __builtin_trap();
            }
            status = backend == ARCH_MMU_STATUS_OK ? KERNEL_MM_STATUS_OK :
                backend == ARCH_MMU_STATUS_CONFLICT || backend == ARCH_MMU_STATUS_NOT_MAPPED ?
                KERNEL_MM_STATUS_CONFLICT : KERNEL_MM_STATUS_ADDRESS_SPACE;
            if (status == KERNEL_MM_STATUS_OK) {
                replacement = KERNEL_MM_NO_PAGE;
                targets = note_tlb_change_locked(record, &batch, (vma.permissions & KERNEL_MM_EXECUTE) != 0);
            }
        }
    }
memory_tables_changed:
    if (refill) {
        mm_gate_release(record,&operation->guard);
        goto prepare_memory_tables;
    }
    if (status == KERNEL_MM_STATUS_OK) {
        /* 新非叶的walk cache也全量失效；旧COW PTE owner留到完成确认。 */
        int nonleaf = missing && ((missing & 1U && prepared.tables[0] == KERNEL_MM_NO_PAGE) ||
            (missing & 2U && prepared.tables[1] == KERNEL_MM_NO_PAGE));
        finish_tlb_change(record, &batch, targets, nonleaf ? 0 : address,
            nonleaf ? UINT64_MAX : BOAROS_PAGE_SIZE, (vma.permissions & KERNEL_MM_EXECUTE) != 0);
    }
    if (shared_preparation != KERNEL_MM_NO_PAGE) {
        kernel_memory_object_finish_page(object,
            (vma.backing_offset + address - vma.start) >> BOAROS_PAGE_SHIFT,
            shared_preparation, status == KERNEL_MM_STATUS_OK);
        replacement = KERNEL_MM_NO_PAGE; /* finish已消费失败引用，成功已转给PTE。 */
    }
    if (replacement != KERNEL_MM_NO_PAGE) {
        physical_page_release(allocator, replacement);
    }
    if (object) kernel_memory_object_release(&object);
    if (source_owner && !__atomic_fetch_sub(&source_owner->faults, 1, __ATOMIC_RELEASE)) __builtin_trap();
    if (operation->guard.lock && source_owner) (void)drain_elf_sources(record, 1);
    mm_operation_end(operation);
    for (unsigned i = 0; i < 2; i++) if (prepared.tables[i] != KERNEL_MM_NO_PAGE) physical_page_release(allocator, prepared.tables[i]);
    if (cookie) physical_page_release(allocator, old);
    return status;
}
static enum kernel_mm_status resolve_user_fault(struct kernel_mm *mm,
    uint64_t address, uint32_t access)
{
    for (;;) {
        MM_OPERATION(mm);
        struct kernel_vma vma;
        int file = operation.record->vmas &&
            !kernel_vma_set_lookup(operation.record->vmas, address, &vma) &&
            (vma.kind == KERNEL_VMA_KIND_FILE_SHARED || vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE);
        enum kernel_mm_status status = file ? mapped_file_fault(&operation, address, access) :
            memory_fault(&operation, address, access);
        if (status != KERNEL_MM_STATUS_CONFLICT || current_pins(mm)) return status;
#if BOAROS_COST_DIAGNOSTICS
        { KERNEL_RAW_SCOPE(guard, &operation.record->metadata); operation.record->fault_retries++; }
#endif
#if BOAROS_USER_SMP_COMPOSITION
        riscv_user_composition_fault_retry(address, 1);
#endif
        /* loser归还全部私有候选，下一轮重取VMA/PTE，不把旧许可带过睡眠。 */
    }
}

enum kernel_mm_status kernel_mm_prepare_futex(struct kernel_mm *mm,
    const uint64_t *addresses, unsigned count, int shared, unsigned pin_index,
    uint32_t access, struct kernel_futex_key *keys, struct kernel_mm_user_pin *pin)
{
    if (!addresses || !keys || !count || count > 2 ||
        (access && (!pin || pin_index >= count ||
         (access != KERNEL_MM_READ && access != KERNEL_MM_WRITE))))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (access && pin->registration) __builtin_trap();
    if (!shared) {
        uint64_t identity;
        enum kernel_mm_status status = kernel_mm_futex_id(mm, &identity);
        if (status) return status;
        for (unsigned i = 0; i < count; i++) {
            if (addresses[i] & 3U) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
            if (addresses[i] >= ARCH_MMU_USER_LIMIT) return KERNEL_MM_STATUS_NOT_MAPPED;
        }
        /* private身份只依赖存活MM和VA。驻留页沿共同pin协议取得实际owner，
         * 不等待与身份无关的映射gate；缺页/COW仍在发布pin和key前完成。 */
        if (access) {
            for (;;) {
                status = kernel_mm_pin_user_page(mm, addresses[pin_index], access, pin);
                if (status != KERNEL_MM_STATUS_NOT_MAPPED &&
                    status != KERNEL_MM_STATUS_ACCESS) break;
                status = kernel_mm_resolve_user_fault(mm, addresses[pin_index], access);
                if (status) return status;
            }
            if (status) return status;
        }
        for (unsigned i = 0; i < count; i++)
            keys[i] = (struct kernel_futex_key){ .identity = identity, .offset = addresses[i] };
        return KERNEL_MM_STATUS_OK;
    }
    for (;;) {
        MM_OPERATION(mm);
        struct kernel_mm_record *record = operation.record;
        struct kernel_vma vmas[2] = {0};
        unsigned fault_index = count;
        uint32_t fault_access = KERNEL_MM_READ;
        /* gate固定两份VMA；raw复查PTE与许可。先检查全部key，最后才取得
         * 唯一pin，因此目标冷页/撤权不会等待本调用已持有的源pin。 */
        for (unsigned i = 0; i < count; i++) {
            uint64_t address = addresses[i];
            if (address & 3U) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
            if (address >= ARCH_MMU_USER_LIMIT) return KERNEL_MM_STATUS_NOT_MAPPED;
            if (!record->vmas || kernel_vma_set_lookup(record->vmas, address, &vmas[i]))
                return KERNEL_MM_STATUS_NOT_MAPPED;
            struct kernel_mm_mapping mapping;
            enum kernel_mm_status status;
            { KERNEL_RAW_SCOPE(guard, &record->metadata);
              status = lookup_record(record, address, &mapping); }
            if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
                fault_index = i;
                fault_access = access && i == pin_index ? access : KERNEL_MM_READ;
                break;
            }
            if (status) return status;
            if (!(mapping.permissions & (KERNEL_MM_READ | KERNEL_MM_WRITE | KERNEL_MM_EXECUTE)))
                return KERNEL_MM_STATUS_ACCESS;
        }
        if (fault_index == count && access) {
            enum kernel_mm_status status;
            { KERNEL_RAW_SCOPE(guard, &record->metadata);
              if (record->access_closed) __builtin_trap();
              status = pin_mapping(mm, record, addresses[pin_index], access,
                                   pin, access == KERNEL_MM_WRITE); }
            if (status == KERNEL_MM_STATUS_NOT_MAPPED || status == KERNEL_MM_STATUS_ACCESS) {
                fault_index = pin_index; fault_access = access;
            } else if (status) return status;
        }
        if (fault_index != count) {
            if (operation.guard.lock) mm_gate_release(record, &operation.guard);
            enum kernel_mm_status status = kernel_mm_resolve_user_fault(mm,
                addresses[fault_index], fault_access);
            if (status) return status;
            continue;
        }
        for (unsigned i = 0; i < count; i++) {
            struct kernel_futex_key *key = &keys[i];
            struct kernel_vma *vma = &vmas[i];
            *key = (struct kernel_futex_key){ .identity = record->futex_id,
                                             .offset = addresses[i] };
            if (vma->kind == KERNEL_VMA_KIND_FILE_SHARED) {
                struct kernel_vfs_node *node = kernel_open_file_node(vma->backing);
                if (!node || kernel_vfs_node_acquire(node)) __builtin_trap();
                key->kind = KERNEL_FUTEX_KEY_SHARED_FILE;
                key->file_node = node; key->identity = (uintptr_t)node;
            } else if (vma->kind == KERNEL_VMA_KIND_ANON_SHARED ||
                       vma->kind == KERNEL_VMA_KIND_SYSV_SHM) {
                struct kernel_memory_object *object = vma->backing;
                if (!object || kernel_memory_object_acquire(object)) __builtin_trap();
                key->kind = KERNEL_FUTEX_KEY_SHARED_MEMORY;
                key->shared_object = object; key->identity = (uintptr_t)object;
            }
            if (key->kind != KERNEL_FUTEX_KEY_PRIVATE)
                key->offset = vma->backing_offset + addresses[i] - vma->start;
        }
        return KERNEL_MM_STATUS_OK;
    }
}

/* One successful resolution contributes one fault, whichever backing path won.
 * Only the caller's own user MM is charged; retries and failures contribute none. */
enum kernel_mm_status kernel_mm_resolve_user_fault(struct kernel_mm *mm,
    uint64_t address, uint32_t access)
{
    struct kernel_task *task = kernel_task_current();
    const struct kernel_mm *task_mm = 0;
    if (!mm || !task || !kernel_task_has_user_context(task) ||
        kernel_task_mm_borrow(task, &task_mm) !=
                     KERNEL_TASK_STATUS_OK ||
        task_mm->record != mm->record) task = 0;
    uint64_t block_reads = task ? kernel_task_read_bytes(task) : 0U;
    enum kernel_mm_status status = resolve_user_fault(mm, address, access);
    if (status == KERNEL_MM_STATUS_OK && task)
        kernel_task_account_fault(task,
            kernel_task_read_bytes(task) != block_reads);
    return status;
}

enum kernel_mm_status kernel_mm_check_task_domain(const struct kernel_mm *mm)
{
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    enum kernel_mm_status status = mm_operation_begin(mm, &operation);
    if (status) return status;
    struct kernel_mm_record *record = operation.record;
    if (mm->state != KERNEL_MM_LIVE || !record->vmas || record->elf_sources || record->executable_file)
        return KERNEL_MM_STATUS_STATE;
    for (uint32_t i = 0; i < kernel_vma_set_count(record->vmas); i++) {
        struct kernel_vma vma;
        if (kernel_vma_set_get_at(record->vmas, i, &vma)) __builtin_trap();
        int valid = vma.kind == KERNEL_VMA_KIND_ANONYMOUS ? !vma.backing :
            vma.kind == KERNEL_VMA_KIND_ANON_SHARED ?
                !kernel_memory_object_check_domain(vma.backing, record->vma_heap, mm->allocator) :
            (vma.kind == KERNEL_VMA_KIND_FILE_SHARED || vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE) &&
                !kernel_open_file_check_mapping_domain(vma.backing, record->vma_heap, mm->allocator);
        if (vma.shm_attachment || !valid) return KERNEL_MM_STATUS_STATE;
    }
    return KERNEL_MM_STATUS_OK;
}

static void proc_map_elf_backing(const struct kernel_vma *vma,
    struct kernel_mm_proc_map *row)
{
    struct kernel_elf64_source *source = vma->backing;
    uint64_t address = vma->backing_offset;
    uint32_t low = 0, high = kernel_elf64_source_run_count(source);
    const struct kernel_elf64_source_run *run = 0;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;
        run = kernel_elf64_source_run_at(source, middle);
        if (!run) __builtin_trap();
        if (address < run->start) high = middle;
        else if (address >= run->end) low = middle + 1U;
        else break;
    }
    if (low == high || !run || address < run->start || address >= run->end)
        __builtin_trap();
    if (run->kind == KERNEL_ELF64_SOURCE_RUN_ZERO) return;
    row->file = kernel_elf64_source_file(source);
    /* Reader-only ELF fixtures have no file identity; their bytes are anonymous. */
    if (!row->file) return;
    if (run->kind == KERNEL_ELF64_SOURCE_RUN_FILE) {
        row->offset = run->file_offset + address - run->start;
        return;
    }
    /* 边界合成页的内容仍来自PT_LOAD文件区；pgoff用其真实文件页。 */
    int found = 0;
    for (uint16_t i = 0; i < kernel_elf64_source_program_header_count(source); i++) {
        const struct kernel_elf64_program_header *header =
            kernel_elf64_source_program_header(source, i);
        if (!header) __builtin_trap();
        if (header->type != KERNEL_ELF64_PROGRAM_LOAD || !header->file_size) continue;
        uint64_t page = header->virtual_address & ~BOAROS_PAGE_MASK;
        if (address >= page && address < header->virtual_address + header->file_size) {
            row->offset = (header->offset & ~BOAROS_PAGE_MASK) + address - page;
            found = 1;
        }
    }
    if (!found) __builtin_trap();
}

enum kernel_mm_status kernel_mm_proc_maps_snapshot(const struct kernel_mm *mm,
    struct kernel_heap *heap, struct kernel_mm_proc_map **rows, uint32_t *count)
{
    if (!heap || !rows || *rows || !count) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    struct kernel_mm_proc_map *data = 0;
    uint32_t capacity = 0;
    *count = 0;
    enum kernel_mm_status status;
    for (;;) {
        struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
        status = mm_operation_begin(mm, &operation);
        if (status) break;
        struct kernel_mm_record *record = operation.record;
        if (record->stage != KERNEL_MM_RECORD_LIVE || !record->vmas) {
            status = KERNEL_MM_STATUS_STATE; break;
        }
        uint32_t needed = kernel_vma_set_count(record->vmas);
        if (needed > capacity) {
            mm_operation_end(&operation);
            if (data) kernel_heap_release(heap, data);
            data = 0;
            enum kernel_heap_status allocated = kernel_heap_allocate_zeroed(
                heap, needed, sizeof(*data), (void **)&data);
            if (allocated) {
                status = allocated == KERNEL_HEAP_STATUS_EMPTY ?
                    KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
                break;
            }
            capacity = needed;
            continue;
        }
        for (uint32_t i = 0; i < needed; i++) {
            struct kernel_vma vma;
            if (kernel_vma_set_get_at(record->vmas, i, &vma)) __builtin_trap();
            struct kernel_mm_proc_map *row = &data[i];
            *row = (struct kernel_mm_proc_map){ .start = vma.start, .end = vma.end,
                .permissions = vma.permissions, .role = vma.role,
                .shared = vma.kind == KERNEL_VMA_KIND_FILE_SHARED ||
                    vma.kind == KERNEL_VMA_KIND_ANON_SHARED ||
                    vma.kind == KERNEL_VMA_KIND_SYSV_SHM };
            if (vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE ||
                vma.kind == KERNEL_VMA_KIND_FILE_SHARED) {
                row->file = vma.backing; row->offset = vma.backing_offset;
            } else if (vma.kind == KERNEL_VMA_KIND_ELF_PRIVATE)
                proc_map_elf_backing(&vma, row);
            if (row->file && kernel_open_file_acquire(row->file)) __builtin_trap();
        }
        *rows = data; *count = needed;
        return KERNEL_MM_STATUS_OK;
    }
    if (data) kernel_heap_release(heap, data);
    return status;
}

/* VMA policy commits under the existing MM edit gate. Population is a separate
 * operation: file I/O and COW may sleep and must revalidate each current VMA. */
static int lock_range(struct kernel_mm_record *record, uint64_t start, uint64_t end, uint32_t flags)
{
    while (start < end) {
        struct kernel_vma vma;
        if (kernel_vma_set_lookup(record->vmas, start, &vma)) return -KERNEL_ENOMEM;
        uint64_t finish = vma.end < end ? vma.end : end;
        if (vma.role != KERNEL_VMA_ROLE_VDSO && vma.lock_flags != flags) {
            struct kernel_vma_edit edit;
            if (kernel_vma_set_prepare_lock(record->vmas, start, finish, flags, &edit)) return -KERNEL_ENOMEM;
            if (kernel_vma_set_commit_edit(record->vmas, &edit)) __builtin_trap();
        }
        start = finish;
    }
    return 0;
}

static int populate_locked(struct kernel_mm *mm, uint64_t start, uint64_t end)
{
    while (start < end) {
        uint32_t access = 0;
        {
            struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
            if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
            struct kernel_vma vma;
            if (kernel_vma_set_lookup(operation.record->vmas, start, &vma)) return -KERNEL_ENOMEM;
            if (vma.lock_flags != KERNEL_VMA_LOCKED) {
                start = vma.end < end ? vma.end : end;
                continue;
            }
            if (!(vma.permissions & (KERNEL_MM_READ | KERNEL_MM_WRITE | KERNEL_MM_EXECUTE)))
                return -KERNEL_ENOMEM;
            access = (vma.permissions & KERNEL_MM_WRITE) &&
                (vma.kind == KERNEL_VMA_KIND_ANONYMOUS || vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE ||
                 vma.kind == KERNEL_VMA_KIND_ELF_PRIVATE) ? KERNEL_MM_WRITE :
                (vma.permissions & KERNEL_MM_READ) ? KERNEL_MM_READ : KERNEL_MM_EXECUTE;
        }
        enum kernel_mm_status status = kernel_mm_resolve_user_fault(mm, start, access);
        if (status) return status == KERNEL_MM_STATUS_NO_MEMORY ? -KERNEL_EAGAIN : -KERNEL_ENOMEM;
        start += BOAROS_PAGE_SIZE;
    }
    return 0;
}

static int lock_interval(uint64_t address, uint64_t length, uint64_t *start, uint64_t *end)
{
    if (length > UINT64_MAX - address ||
        length + (address & BOAROS_PAGE_MASK) > UINT64_MAX - BOAROS_PAGE_MASK) return -KERNEL_EINVAL;
    *start = address & ~BOAROS_PAGE_MASK;
    uint64_t rounded = (length + (address & BOAROS_PAGE_MASK) + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    if (rounded > UINT64_MAX - *start) return -KERNEL_EINVAL;
    *end = *start + rounded;
    return 0;
}

int kernel_mm_mlock(struct kernel_mm *mm, uint64_t address, uint64_t length, uint32_t flags)
{
    if (flags & ~1U) return -KERNEL_EINVAL;
    uint64_t limit = kernel_task_current_memlock_limit(), start, end;
    if (!limit) return -KERNEL_EPERM;
    int error = lock_interval(address, length, &start, &end);
    if (error) return error;
    {
        struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
        if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
        uint64_t used = kernel_vma_set_locked_bytes(operation.record->vmas);
        uint64_t old = kernel_vma_set_locked_range(operation.record->vmas, start, end);
        if (used > limit || end - start - old > limit - used) return -KERNEL_ENOMEM;
        error = lock_range(operation.record, start, end, KERNEL_VMA_LOCKED | (flags ? KERNEL_VMA_LOCK_ONFAULT : 0));
    }
    return error || flags ? error : populate_locked(mm, start, end);
}

int kernel_mm_munlock(struct kernel_mm *mm, uint64_t address, uint64_t length)
{
    uint64_t start, end;
    int error = lock_interval(address, length, &start, &end);
    if (error) return error;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
    return lock_range(operation.record, start, end, 0);
}

int kernel_mm_mlockall(struct kernel_mm *mm, uint32_t flags)
{
    if (!flags || (flags & ~7U) || !(flags & 3U)) return -KERNEL_EINVAL;
    uint64_t limit = kernel_task_current_memlock_limit();
    if (!limit) return -KERNEL_EPERM;
    {
        struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
        if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
        struct kernel_mm_record *record = operation.record;
        if ((flags & 1U) && kernel_vma_set_total_bytes(record->vmas) > limit) return -KERNEL_ENOMEM;
        uint32_t policy = KERNEL_VMA_LOCKED | ((flags & 4U) ? KERNEL_VMA_LOCK_ONFAULT : 0);
        record->future_lock_flags = (flags & 2U) ? policy : 0;
        if (!(flags & 1U)) return 0;
        uint64_t cursor = 0;
        struct kernel_vma vma;
        while (!kernel_vma_set_next(record->vmas, cursor, &vma)) {
            (void)lock_range(record, vma.start, vma.end, policy);
            cursor = vma.end;
        }
    }
    if (!(flags & 4U)) {
        uint64_t cursor = 0;
        for (;;) {
            struct kernel_vma vma;
            {
                struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
            if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
                if (kernel_vma_set_next(operation.record->vmas, cursor, &vma)) break;
            }
            (void)populate_locked(mm, vma.start, vma.end);
            cursor = vma.end;
        }
    }
    return 0;
}

int kernel_mm_munlockall(struct kernel_mm *mm)
{
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
    operation.record->future_lock_flags = 0;
    kernel_vma_set_clear_locks(operation.record->vmas);
    return 0;
}

