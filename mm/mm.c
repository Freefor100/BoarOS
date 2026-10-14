#include <kernel/mm_backend.h>
#include <kernel/cpu.h>
#include <kernel/elf64_source.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/heap.h>
#include <kernel/irq.h>
#include <kernel/open_file.h>
#include <kernel/proc_task.h>
#include <kernel/page.h>
#include <kernel/memory_object.h>
#include <kernel/task.h>
#include <kernel/sync.h>
#include <kernel/shm.h>
#include <kernel/vma.h>
#include <kernel/vfs.h>
#include <kernel/tmpfs.h>

#include <stddef.h>
#include <stdint.h>

#if BOAROS_USER_SMP_COMPOSITION
void riscv_user_composition_fault_retry(uint64_t address, int resumed);
#endif

#define KERNEL_MM_RECORD_MAGIC UINT64_C(0x424f41524d4d5243)

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
    uint32_t operations;
    int draining;
};

struct fault_page_pin {
    struct physical_page_allocator *allocator;
    struct kernel_open_file_description *file;
    uint64_t address, index;
    int created;
};
static void release_fault_page(struct fault_page_pin *pin)
{
    if (physical_page_release(pin->allocator, pin->address) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    if (pin->created)
        kernel_open_file_discard_new_page(pin->file, pin->index, pin->address);
}

static void unpin_file_source(struct kernel_mm_file_source **source)
{
    if (!(*source)->operations) __builtin_trap();
    (*source)->operations--;
}

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
    uint32_t faults;
    int draining;
};

static void unpin_elf_fault_source(struct kernel_mm_elf_source **source)
{
    if (!(*source)->faults) __builtin_trap();
    (*source)->faults--;
}

struct kernel_mm_record {
    uint64_t magic;
    uint64_t futex_id;
    uint32_t references;
    uint32_t users;
    struct kernel_raw_lock metadata;
    uint32_t access_pins, write_pins;
    struct kernel_mutex operations;
    struct kernel_wait_queue pins_changed;
    uint32_t access_closed, concurrent, remote_user;
    uint32_t active_cpus, pending_retirement;
    uint64_t tlb_generation;
    uint64_t synchronized_table_pages;
    uint64_t pin_acquisitions, prepared_pages, fault_retries;
    uint64_t retirement_batches, retired_pages, pin_waits;
    enum kernel_mm_record_stage stage;
    uint64_t start_code, end_code, start_stack;
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
    struct kernel_mm_shared_anon *shared_anon;
    struct mm_file_mapping *file_mappings;
    struct mm_file_mapping *retired_file_mappings;
    struct mm_file_resident *file_residents;
    struct mm_file_resident *retired_file_residents;
    uint8_t cache_files;
    uint64_t cache_gate_generation;
    struct mm_file_resident *cache_retry_head;
    uint64_t resident_probes;
    struct mm_file_resident **resident_buckets;
    size_t resident_capacity;
    size_t resident_count;
    struct kernel_mm_elf_source *elf_sources;
    struct kernel_open_file_description *executable_file;
};

/* 远端用户执行仅扩展翻译/退休域，不能绕过CPU0业务限制或启用gate。 */
static int remote_translation(const struct kernel_mm_record *record)
{ return record->concurrent || record->remote_user; }

static uint32_t backend_permissions_from_mm(uint32_t permissions);
static uint32_t note_tlb_change_locked(struct kernel_mm_record *,
    const struct kernel_mm_retirement *);
static void finish_tlb_change(struct kernel_mm_record *,
    struct kernel_mm_retirement *, uint32_t, uint64_t, uint64_t, int);

_Static_assert(sizeof(struct kernel_mm_record) <= BOAROS_PAGE_SIZE,
               "RISC-V MM record must fit in one physical page");

static int empty_handle(const struct kernel_mm *mm)
{
    return mm->state == KERNEL_MM_EMPTY && mm->allocator == 0 &&
           mm->record_page_address == 0U &&
           mm->cleanup_stage == KERNEL_MM_CLEANUP_NONE;
}

static int owner_handle(const struct kernel_mm *mm)
{
    return mm->allocator != 0 &&
           (mm->record_page_address & BOAROS_PAGE_MASK) == 0U &&
           (mm->state == KERNEL_MM_LIVE ||
            mm->state == KERNEL_MM_CLEANUP);
}

static void finish_handle(struct kernel_mm *mm, enum kernel_mm_state state)
{
    mm->allocator = 0;
    mm->record_page_address = 0U;
    mm->state = state;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
}

static void clear_page(void *pointer)
{
    volatile unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE; index++) {
        bytes[index] = 0U;
    }
}

static enum kernel_mm_status resolve_record_storage(
    const struct kernel_mm *mm,
    struct kernel_mm_record **record)
{
    void *pointer;
    int empty_space_cleanup;

    if (!owner_handle(mm) ||
        physical_page_resolve(mm->allocator,
                              mm->record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return owner_handle(mm) ? KERNEL_MM_STATUS_PAGE_ACCESS
                                : KERNEL_MM_STATUS_STATE;
    }
    *record = pointer;
    empty_space_cleanup =
        ((*record)->stage == KERNEL_MM_RECORD_VMAS_CLEANUP ||
         (*record)->stage ==
             KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP ||
         (*record)->stage ==
             KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP) &&
        (*record)->space.state == ARCH_MMU_USER_SPACE_EMPTY;
    if ((*record)->magic != KERNEL_MM_RECORD_MAGIC ||
        (*record)->futex_id == 0U ||
        __atomic_load_n(&(*record)->references, __ATOMIC_ACQUIRE) == 0U ||
        (*record)->stage > KERNEL_MM_RECORD_ONLY_CLEANUP ||
        (*record)->brk_initialized > 1U ||
        ((*record)->vmas != 0 && (*record)->vma_heap == 0) ||
        ((*record)->file_sources != 0 && (*record)->vma_heap == 0) ||
        ((*record)->elf_sources != 0 && (*record)->vma_heap == 0) ||
        ((*record)->stage == KERNEL_MM_RECORD_LIVE &&
         ((*record)->file_sources != 0 || (*record)->elf_sources != 0) &&
         (*record)->vmas == 0) ||
        ((*record)->brk_initialized != 0U &&
         (((*record)->stage == KERNEL_MM_RECORD_LIVE &&
           (*record)->vmas == 0) ||
          ((*record)->start_brk & BOAROS_PAGE_MASK) != 0U ||
          ((*record)->brk_limit & BOAROS_PAGE_MASK) != 0U ||
          (*record)->start_brk > __atomic_load_n(&(*record)->current_brk, __ATOMIC_ACQUIRE) ||
          __atomic_load_n(&(*record)->current_brk, __ATOMIC_ACQUIRE) > (*record)->brk_limit)) ||
        ((*record)->brk_initialized != 0U &&
         ((*record)->mmap_base < ARCH_MMU_PAGE_SIZE ||
          (*record)->mmap_base > ARCH_MMU_USER_LIMIT ||
          (*record)->vdso_address >= ARCH_MMU_USER_LIMIT)) ||
        (!empty_space_cleanup &&
         ((*record)->space.allocator != mm->allocator ||
          (*record)->space.state != ARCH_MMU_USER_SPACE_LIVE))) {
        return KERNEL_MM_STATUS_STATE;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status resolve_record(const struct kernel_mm *mm,
    struct kernel_mm_record **record)
{
    enum kernel_mm_status status = resolve_record_storage(mm, record);
    if (status != KERNEL_MM_STATUS_OK) return status;
#if BOAROS_MAX_CPUS > 1
    const struct kernel_mm *bound = 0;
    struct kernel_task *task = kernel_task_current();
    if ((*record)->concurrent && task && kernel_task_mm_borrow(task, &bound) == KERNEL_TASK_STATUS_OK &&
        bound->allocator == mm->allocator && bound->record_page_address == mm->record_page_address)
        return KERNEL_MM_STATUS_OK;
#endif
    kernel_assert_service_context();
    return KERNEL_MM_STATUS_OK;
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
    if (physical_page_resolve(allocator,
                              record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return abandon_unresolved_record(mm,
                                         allocator,
                                         record_page_address,
                                         KERNEL_MM_STATUS_PAGE_ACCESS);
    }

    clear_page(pointer);
    record = pointer;
    record->magic = KERNEL_MM_RECORD_MAGIC;
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
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    if (record->stage != KERNEL_MM_RECORD_LIVE ||
        __atomic_load_n(&record->references, __ATOMIC_RELAXED) == UINT32_MAX) {
        return KERNEL_MM_STATUS_STATE;
    }
    __atomic_fetch_add(&record->references, 1U, __ATOMIC_ACQ_REL);
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
    for (struct mm_file_resident *page = record->file_residents;
         page != 0; page = page->next) {
        size_t index = resident_bucket(page->address, capacity);
        page->hash_next = buckets[index];
        buckets[index] = page;
    }
    if (record->resident_buckets != 0)
        (void)kernel_heap_release(record->vma_heap, record->resident_buckets);
    record->resident_buckets = buckets;
    record->resident_capacity = capacity;
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
    record->file_residents = page;
}

/* 只遍历真实登记的重试借用，不扫描MM/来源；gate释放后通知缓存。 */
static void mm_gate_release(struct kernel_mm_record *record,struct kernel_lock_guard *guard)
{
    if (!guard->lock) return;
    int cached=record->concurrent && __atomic_load_n(&record->cache_files,__ATOMIC_ACQUIRE);
    kernel_lock_release(guard);
    if (!cached) return;
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
}

static void forget_file_residents(struct kernel_mm_record *record,
                                  uint64_t start, uint64_t end)
{
    struct mm_file_resident **link = &record->file_residents;
    while (*link != 0) {
        struct mm_file_resident *page = *link;
        if (page->address >= start && page->address < end) {
            *link = page->next;
            unindex_file_resident(record, page);
            retire_file_resident(record,page);
        } else {
            link = &page->next;
        }
    }
}

static int rearm_shared_file_alias(void *owner, uint64_t address)
{
    struct mm_file_resident *page=owner;
    struct kernel_mm_record *record = page->record;
    struct mm_gate_scope operation __attribute__((cleanup(mm_gate_scope_end)))={.record=record};
    if (record->concurrent && !kernel_lock_held(&record->operations.lock,1)) {
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
        if (remote_translation(record)) targets=note_tlb_change_locked(record,&batch);
    }
    if (remote_translation(record)) finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,0);
    return 0;
}

static struct mm_file_resident *find_file_resident(
    struct kernel_mm_record *record, uint64_t address)
{
    if (record->resident_capacity == 0) return 0;
    size_t index = resident_bucket(address, record->resident_capacity);
    for (struct mm_file_resident *page = record->resident_buckets[index];
         page != 0; page = page->hash_next) {
        record->resident_probes++;
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
    if (record->concurrent) kernel_mutex_lock(&record->operations,&operation);
    if (record->stage != KERNEL_MM_RECORD_LIVE) return;

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
        if (offset >= size) {
            /* All resident file offsets are page aligned; this includes the
             * exact-boundary case without overflowing a round-up operation. */
            if (remote_translation(record)) {
                struct kernel_mm_retirement batch={0};
                uint32_t targets;
                {
                    KERNEL_RAW_SCOPE(guard,&record->metadata);
                    if (arch_mmu_user_detach_page(&record->space,page->address,&batch)) __builtin_trap();
                    targets=note_tlb_change_locked(record,&batch);
                }
                finish_tlb_change(record,&batch,targets,0,UINT64_MAX,0);
            } else if (arch_mmu_user_unmap_owned_range(&record->space,
                    page->address, page->address + BOAROS_PAGE_SIZE)) __builtin_trap();
            *link = page->next;
            unindex_file_resident(record, page);
            retire_file_resident(record,page);
            continue;
        }
        if (kernel_open_file_memory_backed(vma.backing) && offset == last_page && !page->private &&
            vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE) {
            unsigned char *bytes;
            if (physical_page_resolve(record->space.allocator,
                    page->physical_address, (void **)&bytes) !=
                    PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
            for (size_t i = (size_t)(size & BOAROS_PAGE_MASK);
                 i < BOAROS_PAGE_SIZE; i++) bytes[i] = 0U;
            cleared = 1;
        }
        link = &page->next;
    }
    if (cleared) arch_mmu_sync_instructions();
    struct mm_file_resident *retired=record->retired_file_residents;
    record->retired_file_residents=0;
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
            if (record->concurrent && record->stage == KERNEL_MM_RECORD_LIVE) {
                kernel_file_mapping_stop(&entry->registration);
                /* 控制者的独立OFD引用覆盖inode；当前MM句柄覆盖record。
                 * 摘登记时不等回调，operation尾部放gate后才drain。 */
                entry->next=record->retired_file_mappings;
                record->retired_file_mappings=entry;
            } else {
                kernel_file_mapping_unregister(&entry->registration);
                (void)kernel_heap_release(record->vma_heap, entry);
            }
        }
    }
}

static enum kernel_mm_status drain_file_sources(
    struct kernel_mm_record *record,
    int unused_only)
{
    struct kernel_mm_file_source **link = &record->file_sources;
    int failed = 0;

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
                    return KERNEL_MM_STATUS_STATE;
                }
            }
            if (in_use != 0) {
                link = &source->next;
                continue;
            }
            struct kernel_open_file_description *owner = source->file;

            source->draining = 1;
            enum kernel_open_file_status released = kernel_open_file_release(&owner);
            source->draining = 0;
            if (released != KERNEL_OPEN_FILE_STATUS_OK) {
                failed = 1;
                link = &source->next;
                continue;
            }
            source->file = 0;
        }
        link = &record->file_sources;
        while (*link && *link != source) link = &(*link)->next;
        if (*link != source) __builtin_trap();
        *link = source->next;
        (void)kernel_heap_release(record->vma_heap, source);
    }
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

static enum kernel_mm_status drain_elf_sources(
    struct kernel_mm_record *record,
    int unused_only)
{
    struct kernel_mm_elf_source **link = &record->elf_sources;
    int failed = 0;

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
                    return KERNEL_MM_STATUS_STATE;
                }
            }
            if (in_use != 0) {
                link = &entry->next;
                continue;
            }
            {
                struct kernel_elf64_source *owner = entry->source;
                entry->draining = 1;
                enum kernel_elf64_source_status source_status = kernel_elf64_source_release(&owner);
                entry->draining = 0;

                if (source_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
                    failed = 1;
                    link = &entry->next;
                    continue;
                }
                entry->source = 0;
            }
        }
        link = &record->elf_sources;
        while (*link && *link != entry) link = &(*link)->next;
        if (*link != entry) __builtin_trap();
        *link = entry->next;
        (void)kernel_heap_release(record->vma_heap, entry);
    }
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
            (void)kernel_heap_release(destination_record->vma_heap, copy);
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
            (void)kernel_heap_release(destination_record->vma_heap,
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
        if (kernel_heap_release(record->vma_heap, entry) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
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
            (void)kernel_heap_release(destination_record->vma_heap, copy);
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
    if (kernel_heap_release(owner->heap,owner->nodes)) __builtin_trap();
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
    if (!record->concurrent || !record->file_mappings) return KERNEL_MM_STATUS_OK;
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
    if (physical_page_resolve(source->allocator,
                              record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return abandon_unresolved_record(destination,
                                         source->allocator,
                                         record_page_address,
                                         KERNEL_MM_STATUS_PAGE_ACCESS);
    }
    clear_page(pointer);
    destination_record = pointer;
    destination_record->magic = KERNEL_MM_RECORD_MAGIC;
    destination_record->futex_id = reserve_mm_identity();
    if (!destination_record->futex_id) return abandon_unresolved_record(destination,
        source->allocator, record_page_address, KERNEL_MM_STATUS_NO_MEMORY);
    destination_record->references = 1U;
    kernel_raw_lock_init(&destination_record->metadata, KERNEL_RAW_RANK_MM);
    kernel_mutex_init(&destination_record->operations, 25, (uintptr_t)destination_record);
    kernel_wait_queue_init(&destination_record->pins_changed);
    destination_record->stage = KERNEL_MM_RECORD_LIVE;
    destination_record->vma_heap = source_record->vma_heap;
    destination_record->concurrent = source_record->concurrent;
    destination_record->remote_user = source_record->remote_user;
    destination_record->cache_files = source_record->cache_files;

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
    status = clone_shared_anon(source_record, destination_record);
    if (status == KERNEL_MM_STATUS_OK)
        status = clone_file_sources(source_record, destination_record);
    if (status != KERNEL_MM_STATUS_OK) {
        destination_record->stage = KERNEL_MM_RECORD_VMAS_CLEANUP;
        destination->allocator = source->allocator;
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
        publish_file_resident(destination_record, copy);
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
        destination->record_page_address=record_page_address;
        destination->state=KERNEL_MM_CLEANUP;
        destination->cleanup_stage=KERNEL_MM_CLEANUP_VMAS;
        if (kernel_mm_release(destination)) __builtin_trap();
        finish_handle(destination,KERNEL_MM_EMPTY);
        return status;
    }
    backend_status = remote_translation(source_record) ? arch_mmu_user_space_fork_prepare(
        &destination_record->space,
        &source_record->space,
        source_record->vmas != 0 ? shared_anon_leaf : 0,
        source_record->vmas) : arch_mmu_user_space_fork(
            &destination_record->space, &source_record->space,
            source_record->vmas != 0 ? shared_anon_leaf : 0, source_record->vmas);
    if (backend_status == ARCH_MMU_STATUS_OK && remote_translation(source_record)) {
        uint64_t cursor = 0, address;
        /* child尚未发布，遍历私有树不持MM raw；每次只提交一个父叶。 */
        while ((backend_status = arch_mmu_user_next_owned(&destination_record->space,
            &cursor, &address)) == ARCH_MMU_STATUS_OK) {
            /* 按VMA语义分类，不能把合法shared/protected页当作私有COW。 */
            if (shared_anon_leaf(source_record->vmas, address)) continue;
            KERNEL_RAW_SCOPE(guard, &source_record->metadata);
            if (arch_mmu_user_mark_cow_page(&source_record->space, address) !=
                ARCH_MMU_STATUS_OK) __builtin_trap();
        }
        if (backend_status != ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
        struct kernel_mm_retirement batch = {0};
        uint32_t targets;
        {
            KERNEL_RAW_SCOPE(guard, &source_record->metadata);
            targets = note_tlb_change_locked(source_record, &batch);
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
                if (destination_record->concurrent && destination_record->cache_files) {
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
        destination->record_page_address = record_page_address;
        destination->state = KERNEL_MM_LIVE;
        destination->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
        return KERNEL_MM_STATUS_OK;
    }

    fork_file_guards_end(&files);
    failure = fork_status_from_backend(backend_status);
    destination_record->stage = KERNEL_MM_RECORD_VMAS_CLEANUP;
    destination->allocator = source->allocator;
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

/* 只有显式开放的私有匿名域走operation gate；既有文件/业务仍限定CPU0。
 * gate串行长操作，rank15 raw只保护短PTE/许可提交，不覆盖分配或复制。 */
struct mm_operation {
    struct kernel_mm_record *record;
    const struct kernel_mm *mm;
    struct kernel_lock_guard guard;
};
static uint32_t note_tlb_change_locked(struct kernel_mm_record *record,
    const struct kernel_mm_retirement *batch)
{
    if (record->tlb_generation == UINT64_MAX ||
        (batch->count && record->pending_retirement == UINT32_MAX)) __builtin_trap();
    record->tlb_generation++;
    if (batch->count) {
        record->pending_retirement++;
        record->retirement_batches++;
        record->retired_pages += batch->count;
    }
    return record->active_cpus;
}
static void finish_tlb_change(struct kernel_mm_record *record,
    struct kernel_mm_retirement *batch, uint32_t targets, uint64_t address,
    uint64_t size, int instructions)
{
    if (record->remote_user) {
        KERNEL_RAW_SCOPE(guard,&record->metadata);
        /* 普通fault沿用CPU0 mapper；新父项或摘空表必须同步walk cache。
         * 每次非叶变更都会经过确认点，不能用后续相同表数量掩盖中间变化。 */
        if (record->synchronized_table_pages != record->space.table_pages) {
            address=0; size=UINT64_MAX;
        }
    }
    arch_mmu_sync_user(targets, address, size, instructions);
    if (record->remote_user) {
        KERNEL_RAW_SCOPE(guard,&record->metadata);
        record->synchronized_table_pages=record->space.table_pages;
    }
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
    if (kernel_wait_queue_wake_all(&record->pins_changed) != KERNEL_SCHEDULER_STATUS_OK)
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
        {
            KERNEL_RAW_SCOPE(guard, &record->metadata);
            record->access_closed = 0;
        }
        wake_pin_waiters(record);
        mm_gate_release(operation->record,&operation->guard);
        drain_retired_residents(record,residents);
        while (retired) {
            struct mm_file_mapping *entry=retired;
            retired=entry->next;
            kernel_file_mapping_drain(&entry->registration);
            if (kernel_heap_release(record->vma_heap,entry)) __builtin_trap();
        }
    }
}
static int current_pins(const struct kernel_mm *mm)
{
    for (const struct kernel_mm_user_pin *pin = kernel_io_context_current()->user_pins;
         pin; pin = pin->owner_next)
        if (pin->mm->allocator == mm->allocator &&
            pin->mm->record_page_address == mm->record_page_address) return 1;
    return 0;
}
static enum kernel_mm_status mm_operation_lock(struct mm_operation *operation)
{
    if (!__atomic_load_n(&operation->record->concurrent, __ATOMIC_ACQUIRE))
        return KERNEL_MM_STATUS_OK;
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
            record->pin_waits++;
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
            pin->mm->record_page_address == operation->mm->record_page_address &&
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

enum kernel_mm_status kernel_mm_pin_user_page(const struct kernel_mm *mm,
    uint64_t address, uint32_t access, struct kernel_mm_user_pin *pin)
{
    struct kernel_mm_record *record;
    struct kernel_mm_mapping mapping;
    void *pointer;
    if (!mm || !pin || !access ||
        (access & ~(KERNEL_MM_READ | KERNEL_MM_WRITE | KERNEL_MM_EXECUTE)))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (pin->registration) __builtin_trap();
    if (mm->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    struct mm_gate_scope pin_scope __attribute__((cleanup(mm_gate_scope_end)))={.record=record};
    if (record->concurrent && __atomic_load_n(&record->cache_files,__ATOMIC_ACQUIRE) && (access & KERNEL_MM_WRITE)) {
        if (current_pins(mm)) {
            if (!kernel_mutex_try_lock(&record->operations,&pin_scope.guard)) return KERNEL_MM_STATUS_CONFLICT;
        } else kernel_mutex_lock(&record->operations,&pin_scope.guard);
    }
retry:
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    if (record->access_closed == 1 ||
        (record->access_closed == 2 && (access & KERNEL_MM_WRITE))) {
        if (current_pins(mm)) return KERNEL_MM_STATUS_CONFLICT;
        kernel_raw_lock_release(&guard);
        wait_pin_condition(record, 0);
        goto retry;
    }
    status = lookup_record(record, address, &mapping);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if ((mapping.permissions & (access | KERNEL_MM_USER)) !=
        (access | KERNEL_MM_USER)) return KERNEL_MM_STATUS_ACCESS;
    uint64_t physical = mapping.physical_address & ~BOAROS_PAGE_MASK;
    /* permission许可和取得实际页引用不能被撤映射插入。 */
    if (physical_page_resolve(mm->allocator, physical, &pointer) !=
        PHYSICAL_PAGE_STATUS_OK) return KERNEL_MM_STATUS_PAGE_ACCESS;
    if (record->access_pins == UINT32_MAX || record->write_pins == UINT32_MAX)
        __builtin_trap();
    if (pin_scope.guard.lock) {
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
    record->pin_acquisitions++;
    if (access & KERNEL_MM_WRITE) record->write_pins++;
    struct kernel_io_context *owner = kernel_io_context_current();
    pin->mm=mm; pin->owner=owner; pin->registration=pin; pin->owner_next=owner->user_pins;
    pin->page=pointer; pin->physical_address=physical; pin->access=access;
    owner->user_pins = pin;
    return KERNEL_MM_STATUS_OK;
}

void kernel_mm_unpin_user_page(struct kernel_mm_user_pin *pin)
{
    struct kernel_mm_record *record;
    if (!pin || pin->registration != pin ||
        pin->owner != kernel_io_context_current() || !pin->mm ||
        pin->mm->state != KERNEL_MM_LIVE ||
        resolve_record(pin->mm, &record) != KERNEL_MM_STATUS_OK)
        __builtin_trap();
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    struct kernel_mm_user_pin **registration = &pin->owner->user_pins;
    while (*registration && *registration != pin) registration = &(*registration)->owner_next;
    if (*registration != pin) __builtin_trap();
    *registration = pin->owner_next;
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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
    return status_from_vma(kernel_vma_set_insert(record->vmas, &vma));
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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
            (void)kernel_heap_release(record->vma_heap, source_owner);
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
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

static enum kernel_mm_status require_active_space(
    const struct kernel_mm_record *record)
{
    uint64_t satp;

    if (arch_mmu_user_space_context(&record->space, &satp) !=
            ARCH_MMU_STATUS_OK ||
        arch_mmu_current_context() != satp) {
        return KERNEL_MM_STATUS_STATE;
    }
    return KERNEL_MM_STATUS_OK;
}
/* 受控fault仅使用owned PTE及完成确认后的退休路径。普通CPU0服务任务
 * 可以在kernel根上复制另一MM；真实owner+gate25固定物理页表，不能借此
 * 放宽用户任务、受控任务或普通MM的活动地址空间检查。 */
static enum kernel_mm_status require_fault_space(const struct kernel_mm_record *record)
{
    enum kernel_mm_status status=require_active_space(record);
    if (status==KERNEL_MM_STATUS_OK) return status;
#if BOAROS_MAX_CPUS>1
    struct kernel_task *task=kernel_task_current(); const struct kernel_mm *bound=0;
    if (record->concurrent && !kernel_cpu_current()->logical_id &&
        kernel_lock_held(&record->operations.lock,1) &&
        (!task || (!kernel_task_has_user_context(task) && kernel_task_mm_borrow(task,&bound)))) {
        kernel_assert_service_context();
        return KERNEL_MM_STATUS_OK;
    }
#endif
    return status;
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

static enum kernel_mm_status unmap_space_range(
    struct kernel_mm_record *record,
    uint64_t start,
    uint64_t end)
{
    enum arch_mmu_status status;

    if (end <= ARCH_MMU_PAGE_SIZE) {
        return KERNEL_MM_STATUS_OK;
    }
    if (start < ARCH_MMU_PAGE_SIZE) {
        start = ARCH_MMU_PAGE_SIZE;
    }
    if (remote_translation(record)) {
        uint64_t cursor = start;
        while (cursor < end) {
            struct kernel_mm_retirement batch = {0};
            uint64_t first = cursor;
            uint32_t targets;
            {
                KERNEL_RAW_SCOPE(guard, &record->metadata);
                unsigned visited = 0;
                while (cursor < end && visited++ < 16 &&
                    batch.count <= KERNEL_MM_RETIRE_CAPACITY - 3) {
                    uint64_t address;
                    status = arch_mmu_user_next_owned(&record->space, &cursor, &address);
                    if (status == ARCH_MMU_STATUS_NOT_MAPPED) { cursor = end; break; }
                    if (status != ARCH_MMU_STATUS_OK) __builtin_trap();
                    if (address >= end) { cursor = end; break; }
                    status = arch_mmu_user_detach_page(&record->space, address, &batch);
                    if (status != ARCH_MMU_STATUS_OK && status != ARCH_MMU_STATUS_NOT_MAPPED)
                        __builtin_trap(); /* 已提交退休不能丢弃owner。 */
                }
                targets = batch.count ? note_tlb_change_locked(record, &batch) : record->active_cpus;
            }
            if (batch.count) {
                /* raw外固件确认；非叶覆盖walk cache，错误保留批次owner并fatal。 */
                finish_tlb_change(record, &batch, targets, batch.nonleaf ? 0 : first,
                    batch.nonleaf ? UINT64_MAX : cursor - first, 0);
            }
        }
        forget_file_residents(record,start,end);
        return KERNEL_MM_STATUS_OK;
    }
    status = arch_mmu_user_unmap_owned_range(&record->space,
                                               start,
                                               end);
    if (status == ARCH_MMU_STATUS_OK) {
        forget_file_residents(record, start, end);
        return KERNEL_MM_STATUS_OK;
    }
    return status == ARCH_MMU_STATUS_STATE
               ? KERNEL_MM_STATUS_STATE
               : KERNEL_MM_STATUS_ADDRESS_SPACE;
}

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
                   KERNEL_MM_MAP_SHARED)) != 0U ||
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
    status = require_active_space(record);
    if (status != KERNEL_MM_STATUS_OK) {
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
    if (shared_entry != 0 &&
        kernel_heap_release(record->vma_heap, shared_entry) !=
            KERNEL_HEAP_STATUS_OK) __builtin_trap();
    if (shared_object != 0)
        kernel_memory_object_release(&shared_object);
    return status;
}

static void discard_prepared_file_source(
    struct kernel_mm_record *record,
    struct kernel_mm_file_source *source)
{
    if (source != 0) {
        (void)kernel_heap_release(record->vma_heap, source);
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
                   KERNEL_MM_MAP_SHARED)) != 0U ||
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
                (void)kernel_heap_release(record->vma_heap, registration);
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
                (void)kernel_heap_release(record->vma_heap, registration);
            return status_from_vma(vma_status);
        }
        status = require_active_space(record);
        if (status == KERNEL_MM_STATUS_OK) {
            status = unmap_space_range(record, plan.start, plan.end);
        }
        if (status != KERNEL_MM_STATUS_OK ||
            kernel_vma_set_commit_edit(record->vmas, &edit) !=
                KERNEL_VMA_STATUS_OK) {
            discard_prepared_file_source(record, prepared);
            if (registration != 0)
                (void)kernel_heap_release(record->vma_heap, registration);
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
    status = require_active_space(record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
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
    if (status == KERNEL_MM_STATUS_OK && record->concurrent) return KERNEL_MM_STATUS_STATE;
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }

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
                (void)kernel_heap_release(record->vma_heap, shared_entry);
            }
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
        if (shared_entry != 0) {
            kernel_memory_object_release(&shared_entry->object);
            (void)kernel_heap_release(record->vma_heap, shared_entry);
        }
        return status;
    }
    status = require_active_space(record);
    if (status != KERNEL_MM_STATUS_OK) {
        if (shared_entry != 0) {
            kernel_memory_object_release(&shared_entry->object);
            (void)kernel_heap_release(record->vma_heap, shared_entry);
        }
        return status;
    }
    status = unmap_space_range(record, start, end);
    if (status != KERNEL_MM_STATUS_OK) {
        if (shared_entry != 0) {
            kernel_memory_object_release(&shared_entry->object);
            (void)kernel_heap_release(record->vma_heap, shared_entry);
        }
        return status;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) {
        if (shared_entry != 0) {
            kernel_memory_object_release(&shared_entry->object);
            (void)kernel_heap_release(record->vma_heap, shared_entry);
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
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_shmdt(struct kernel_mm *mm, uint64_t address)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    if (mm == 0 || (address & BOAROS_PAGE_MASK) ||
        address < ARCH_MMU_PAGE_SIZE || address >= ARCH_MMU_USER_LIMIT)
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (record->concurrent) return KERNEL_MM_STATUS_STATE;
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
        status = kernel_mm_munmap(mm, vma.start, vma.end - vma.start);
        if (status != KERNEL_MM_STATUS_OK) return status;
        i = 0; /* 后备清理可睡眠，重取集合而不保存数组下标。 */
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status mm_mprotect_impl(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length,
    uint32_t permissions)
{
    COST_SCOPE(cost_protect, MPROTECT_TICKS);
    COST_ADD(MPROTECT_CALLS, 1);
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_vma_edit edit;
    uint64_t aligned_length;
    uint64_t end;
    uint32_t normalized;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;
    enum arch_mmu_status backend_status;

    if ((address & BOAROS_PAGE_MASK) != 0U ||
        !normalize_user_permissions(permissions, &normalized)) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (length == 0U) {
        return mutable_vma_record(mm, &record);
    }
    if (!align_mapping_length(length, &aligned_length) ||
        address > ARCH_MMU_USER_LIMIT - aligned_length) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    end = address + aligned_length;
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    vma_status = kernel_vma_set_prepare_protect(record->vmas,
                                                address,
                                                end,
                                                normalized,
                                                &edit);
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        return status_from_vma(vma_status);
    }
    status = require_active_space(record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (remote_translation(record)) {
        backend_status = ARCH_MMU_STATUS_OK;
        uint32_t targets;
        uint64_t cursor = address, leaf;
        while (arch_mmu_user_next_owned(&record->space, &cursor, &leaf) == ARCH_MMU_STATUS_OK && leaf < end) {
            uint32_t leaf_permissions=normalized;
            if (record->cache_files) {
                struct mm_file_resident *resident=find_file_resident(record,leaf);
                if (resident && kernel_page_cache_alias_active(&resident->alias)) leaf_permissions &= ~KERNEL_MM_WRITE;
            }
            KERNEL_RAW_SCOPE(guard, &record->metadata);
            backend_status = arch_mmu_user_protect_owned_page(&record->space, leaf,
                backend_permissions_from_mm(leaf_permissions));
            if (backend_status != ARCH_MMU_STATUS_OK) __builtin_trap();
        }
        {
            KERNEL_RAW_SCOPE(guard, &record->metadata);
            if (record->tlb_generation == UINT64_MAX) __builtin_trap();
            record->tlb_generation++;
            targets = record->active_cpus;
        }
        arch_mmu_sync_user(targets, address, end - address,
            (normalized & KERNEL_MM_EXECUTE) != 0);
    } else {
        backend_status = arch_mmu_user_protect_owned_range(
            &record->space, address, end, backend_permissions_from_mm(normalized));
    }
    if (backend_status != ARCH_MMU_STATUS_OK) {
        return backend_status == ARCH_MMU_STATUS_STATE
                   ? KERNEL_MM_STATUS_STATE
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) return KERNEL_MM_STATUS_STATE;
    for (struct mm_file_resident *page = record->file_residents;
         page != 0; page = page->next) {
        COST_ADD(MPROTECT_RESIDENT_VISITS, 1);
        if (page->address >= address && page->address < end &&
            kernel_page_cache_alias_active(&page->alias))
            rearm_shared_file_alias(page, page->address);
    }
    return KERNEL_MM_STATUS_OK;
}

static int mm_msync_impl(struct kernel_mm *mm, uint64_t address,
                    uint64_t length, uint32_t flags)
{
    struct kernel_mm_record *record;
    uint64_t rounded, end, cursor;
    int hole = 0;
    if ((flags & ~7U) != 0U || (address & BOAROS_PAGE_MASK) != 0U ||
        ((flags & 1U) != 0U && (flags & 4U) != 0U))
        return -KERNEL_EINVAL;
    rounded = (length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    end = address + rounded;
    if (end < address) return -KERNEL_ENOMEM;
    if (end == address) return 0;
    if (mutable_vma_record(mm, &record) != KERNEL_MM_STATUS_OK)
        return -KERNEL_ENOMEM;
    cursor = address;
    while (cursor < end) {
        struct kernel_vma vma;
        enum kernel_vma_status vs = kernel_vma_set_lookup(
            record->vmas, cursor, &vma);
        if (vs == KERNEL_VMA_STATUS_NOT_FOUND) {
            hole = 1;
            if (flags == 1U) break;
            vs = kernel_vma_set_next(record->vmas, cursor, &vma);
            if (vs == KERNEL_VMA_STATUS_NOT_FOUND) break;
            if (vs != KERNEL_VMA_STATUS_OK) __builtin_trap();
            if (vma.start >= end) break;
            cursor = vma.start;
        } else if (vs != KERNEL_VMA_STATUS_OK) {
            __builtin_trap();
        }
        uint64_t segment_end = vma.end < end ? vma.end : end;
        if ((flags & 4U) != 0U && vma.kind == KERNEL_VMA_KIND_FILE_SHARED) {
            uint64_t file_start = vma.backing_offset + cursor - vma.start;
            uint64_t file_end = file_start + segment_end - cursor;
            int result;
            {
                struct kernel_mm_file_source *source
                    __attribute__((cleanup(unpin_file_source))) =
                        find_file_source(record, vma.backing);
                if (!source || source->operations == UINT32_MAX) __builtin_trap();
                /* sync_range 的 inode 等待和错误游标访问都借用这个 OFD。 */
                source->operations++;
                result = kernel_open_file_sync_range(source->file, file_start, file_end);
            }
            enum kernel_mm_status drained = drain_file_sources(record, 1);
            if (drained == KERNEL_MM_STATUS_STATE) __builtin_trap();
            /* 真实 close 失败仍由 registry 保留；不覆盖本次同步结果。 */
            if (result != 0) return result;
        }
        cursor = segment_end;
    }
    return hole ? -KERNEL_ENOMEM : 0;
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
    if (new_page_end == old_page_end) {
        __atomic_store_n(&record->current_brk, requested, __ATOMIC_RELEASE);
        *result = requested;
        return KERNEL_MM_STATUS_OK;
    }

    if (new_page_end > old_page_end) {
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
    status = require_active_space(record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
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

static void flush_user_page(struct kernel_mm_record *record,
    uint64_t virtual_address, uint32_t permissions)
{
    if (remote_translation(record)) {
        struct kernel_mm_retirement batch = {0};
        uint32_t targets;
        { KERNEL_RAW_SCOPE(guard, &record->metadata);
          targets = note_tlb_change_locked(record, &batch); }
        finish_tlb_change(record, &batch, targets, virtual_address, BOAROS_PAGE_SIZE,
            (permissions & KERNEL_MM_EXECUTE) != 0);
        return;
    }
    arch_mmu_flush_address(virtual_address);
    if ((permissions & KERNEL_MM_EXECUTE) != 0U) {
        arch_mmu_sync_instructions();
    }
}

static enum arch_mmu_status resolve_record_cow(struct kernel_mm_record *record,
    uint64_t address, uint32_t permissions)
{
    if (!remote_translation(record))
        return arch_mmu_user_resolve_cow(&record->space, address, permissions);
    struct kernel_mm_retirement batch = {0};
    struct physical_page_allocator *allocator = record->space.allocator;
    uint64_t cookie, old, replacement = KERNEL_MM_NO_PAGE;
    uint32_t targets = 0, references;
    void *source = 0, *destination;
    enum arch_mmu_status status;
    {
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        status = arch_mmu_user_cow_snapshot(&record->space,address,&cookie,&old);
        if (status) return status;
        if (physical_page_reference_count(allocator,old,&references)) __builtin_trap();
        if (references == 1) {
            status = arch_mmu_user_cow_commit(&record->space,address,cookie,
                KERNEL_MM_NO_PAGE,permissions,&batch);
            if (status) return status;
            targets = note_tlb_change_locked(record,&batch);
        } else {
            /* 快照实际引用覆盖锁外复制；旧PTE引用另外留在退休批次。 */
            if (physical_page_acquire(allocator,old)) __builtin_trap();
            if (physical_page_resolve(allocator,old,&source)) {
                physical_page_release(allocator,old);
                return ARCH_MMU_STATUS_STATE;
            }
        }
    }
    if (source) {
        enum physical_page_status allocated = physical_page_allocate(allocator,&replacement);
        if (allocated != PHYSICAL_PAGE_STATUS_OK) {
            physical_page_release(allocator,old);
            return allocated == PHYSICAL_PAGE_STATUS_EMPTY ? ARCH_MMU_STATUS_NO_MEMORY : ARCH_MMU_STATUS_STATE;
        }
        if (physical_page_resolve(allocator,replacement,&destination)) {
            physical_page_release(allocator,replacement);
            physical_page_release(allocator,old);
            return ARCH_MMU_STATUS_STATE;
        }
        unsigned char *out = destination, *in = source;
        for (size_t i=0; i<BOAROS_PAGE_SIZE; i++) out[i]=in[i];
        {
            KERNEL_RAW_SCOPE(guard,&record->metadata);
            status=arch_mmu_user_cow_commit(&record->space,address,cookie,replacement,permissions,&batch);
            if (!status) targets=note_tlb_change_locked(record,&batch);
        }
        if (status) physical_page_release(allocator,replacement);
    }
    if (!status) finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,
        (permissions & ARCH_MMU_EXECUTE) != 0);
    if (source) physical_page_release(allocator,old);
    return status;
}

static enum kernel_mm_status map_file_page_status(
    enum arch_mmu_status status)
{
    if (status == ARCH_MMU_STATUS_OK) {
        return KERNEL_MM_STATUS_OK;
    }
    if (status == ARCH_MMU_STATUS_NO_MEMORY) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    return status == ARCH_MMU_STATUS_STATE
               ? KERNEL_MM_STATUS_STATE
               : KERNEL_MM_STATUS_ADDRESS_SPACE;
}

static enum kernel_mm_status discard_file_page(
    struct arch_mmu_user_space *space,
    uint64_t physical_address,
    enum kernel_mm_status result)
{
    enum arch_mmu_status status =
        arch_mmu_user_discard_owned_page(space, physical_address);

    if (status == ARCH_MMU_STATUS_OK) {
        return result;
    }
    return KERNEL_MM_STATUS_STATE;
}

/* Undo a leaf that was already published before a later COW step failed. */
static enum kernel_mm_status discard_mapped_page(
    struct kernel_mm_record *record,
    uint64_t page_address,
    enum kernel_mm_status result)
{
    enum arch_mmu_status status;

    if (remote_translation(record)) {
        return unmap_space_range(record,page_address,page_address+BOAROS_PAGE_SIZE) ==
            KERNEL_MM_STATUS_OK ? result : KERNEL_MM_STATUS_STATE;
    }
    status = arch_mmu_user_unmap_owned_range(&record->space,
        page_address, page_address + BOAROS_PAGE_SIZE);
    if (status != ARCH_MMU_STATUS_OK) {
        return KERNEL_MM_STATUS_STATE;
    }
    return result;
}

static enum kernel_mm_status map_cached_file_page(
    struct kernel_mm_record *record,
    const struct kernel_vma *vma,
    uint64_t page_address,
    uint64_t file_page_index)
{
    struct kernel_open_file_description *file = vma->backing;
    uint64_t physical_address;
    size_t valid_bytes;
    enum kernel_page_cache_status cache_status;
    enum arch_mmu_status backend_status;

    cache_status = kernel_open_file_get_page(file,
                                             file_page_index,
                                             &physical_address,
                                             &valid_bytes);
    (void)valid_bytes;
    if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK) {
        if (cache_status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        return cache_status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE ||
                       cache_status == KERNEL_PAGE_CACHE_STATUS_IO
                   ? KERNEL_MM_STATUS_BUS_FAULT
                   : KERNEL_MM_STATUS_STATE;
    }
    backend_status = arch_mmu_user_map_cow_page(
        &record->space,
        page_address,
        physical_address,
        backend_permissions_from_mm(vma->permissions));
    if (backend_status != ARCH_MMU_STATUS_OK) {
        return discard_file_page(&record->space,
                                 physical_address,
                                 map_file_page_status(backend_status));
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status map_shared_file_page(
    struct kernel_mm_record *record, const struct kernel_vma *vma,
    uint64_t page_address, uint64_t file_page_index, uint32_t access,
    struct mm_file_resident *resident,
    int *translation_synchronized)
{
    struct kernel_open_file_description *file = vma->backing;
    uint64_t physical_address;
    size_t valid_bytes;
    enum kernel_page_cache_status cache_status;
    enum arch_mmu_status backend_status;
    int memory_backed = kernel_open_file_memory_backed(file);
    uint32_t initial = memory_backed ? vma->permissions : vma->permissions & ~KERNEL_MM_WRITE;

    cache_status = kernel_open_file_get_page(file, file_page_index,
                                            &physical_address, &valid_bytes);
    (void)valid_bytes;
    if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK)
        return cache_status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                   ? KERNEL_MM_STATUS_NO_MEMORY
                   : cache_status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE ||
                     cache_status == KERNEL_PAGE_CACHE_STATUS_IO
                       ? KERNEL_MM_STATUS_BUS_FAULT
                       : KERNEL_MM_STATUS_STATE;
    backend_status = arch_mmu_user_map_owned_page(&record->space,
        page_address, physical_address, backend_permissions_from_mm(initial));
    if (backend_status != ARCH_MMU_STATUS_OK)
        return discard_file_page(&record->space, physical_address,
                                 map_file_page_status(backend_status));
    if (memory_backed) {
        if (access == KERNEL_MM_WRITE)
            kernel_open_file_memory_modified(file);
        return KERNEL_MM_STATUS_OK;
    }
    resident->record=record;
    cache_status = kernel_open_file_alias_attach(file, file_page_index,
        physical_address, &resident->alias, resident, page_address,
        rearm_shared_file_alias);
    if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK)
        return discard_mapped_page(record, page_address,
                                   KERNEL_MM_STATUS_STATE);
    if (access == KERNEL_MM_WRITE) {
        kernel_page_cache_alias_mark_dirty(&resident->alias);
        backend_status = arch_mmu_user_protect_owned_range(&record->space,
            page_address, page_address + BOAROS_PAGE_SIZE,
            backend_permissions_from_mm(vma->permissions));
        if (backend_status != ARCH_MMU_STATUS_OK) __builtin_trap();
        *translation_synchronized = 1;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status map_private_file_page(
    struct kernel_mm_record *record,
    const struct kernel_vma *vma,
    uint64_t page_address,
    uint64_t file_page_index,
    uint64_t file_page_offset,
    uint64_t file_size,
    int *translation_synchronized)
{
    struct kernel_open_file_description *file = vma->backing;
    uint64_t cached_address;
    uint64_t private_address;
    size_t valid_bytes = 0U;
    size_t bytes_read = 0U;
    size_t requested;
    void *private_page;
    enum kernel_page_cache_status cache_status;
    enum physical_page_status page_status;
    enum arch_mmu_status backend_status;

    *translation_synchronized = 0;

    cache_status = kernel_open_file_lookup_page(file,
                                                file_page_index,
                                                &cached_address,
                                                &valid_bytes);
    if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK &&
        cache_status != KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) {
        return cache_status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                   ? KERNEL_MM_STATUS_NO_MEMORY
                   : KERNEL_MM_STATUS_STATE;
    }
    if (cache_status == KERNEL_PAGE_CACHE_STATUS_OK) {
        backend_status = arch_mmu_user_map_cow_page(
            &record->space,
            page_address,
            cached_address,
            backend_permissions_from_mm(vma->permissions));
        if (backend_status != ARCH_MMU_STATUS_OK) {
            return discard_file_page(&record->space,
                                     cached_address,
                                     map_file_page_status(backend_status));
        }
        backend_status = resolve_record_cow(
            record,
            page_address,
            backend_permissions_from_mm(vma->permissions));
        if (backend_status == ARCH_MMU_STATUS_OK) {
            /* The COW resolver already published its local TLB/I-cache fence. */
            *translation_synchronized = 1;
        } else {
            /* No provenance is published for a failed fault. Do not leave
             * the temporary cache PTE behind after private allocation fails. */
            return discard_mapped_page(record, page_address,
                                         map_file_page_status(backend_status));
        }
        return map_file_page_status(backend_status);
    }
    page_status = physical_page_allocate(record->space.allocator,
                                         &private_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_MM_STATUS_NO_MEMORY
                   : KERNEL_MM_STATUS_STATE;
    }
    if (physical_page_resolve(record->space.allocator,
                              private_address,
                              &private_page) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return discard_file_page(&record->space,
                                 private_address,
                                 KERNEL_MM_STATUS_STATE);
    }
    clear_page(private_page);
    requested = file_size - file_page_offset < BOAROS_PAGE_SIZE
                    ? (size_t)(file_size - file_page_offset)
                    : (size_t)BOAROS_PAGE_SIZE;
    if (kernel_open_file_pread(file,
                               file_page_offset,
                               private_page,
                               requested,
                               &bytes_read) != 0 ||
        bytes_read > requested) {
        return discard_file_page(&record->space,
                                 private_address,
                                 KERNEL_MM_STATUS_BUS_FAULT);
    }
    backend_status = arch_mmu_user_map_owned_page(
        &record->space,
        page_address,
        private_address,
        backend_permissions_from_mm(vma->permissions));
    if (backend_status != ARCH_MMU_STATUS_OK) {
        return discard_file_page(&record->space,
                                 private_address,
                                 map_file_page_status(backend_status));
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status map_elf_source_page(
    struct kernel_mm_record *record,
    const struct kernel_vma *vma,
    uint64_t page_address,
    uint32_t access, int *retry)
{
    uint64_t source_offset;
    uint64_t physical_address;
    int shared = 0;
    int mapped = 0;
    enum kernel_elf64_source_status source_status;
    enum arch_mmu_status backend_status;
    enum kernel_mm_status status;

    if (vma->backing_offset > UINT64_MAX -
                             (page_address - vma->start)) {
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    struct kernel_mm_elf_source *source __attribute__((cleanup(unpin_elf_fault_source))) =
        find_elf_source(record, vma->backing);
    if (!source || source->faults == UINT32_MAX) __builtin_trap();
    source->faults++;
    uint64_t version = kernel_vma_set_generation(record->vmas);
    source_offset = vma->backing_offset + (page_address - vma->start);
    source_status = kernel_elf64_source_page(
        vma->backing,
        record->space.allocator,
        source_offset,
        &physical_address,
        &shared);
    if (source_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
        if (source_status == KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        if (source_status ==
            KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED) {
            return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
        }
        return source_status == KERNEL_ELF64_SOURCE_STATUS_IO
                   ? KERNEL_MM_STATUS_BUS_FAULT
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    struct arch_mmu_mapping existing;
    if (version != kernel_vma_set_generation(record->vmas) ||
        arch_mmu_user_lookup(&record->space, page_address, &existing) != ARCH_MMU_STATUS_NOT_MAPPED) {
        *retry = 1;
        return discard_file_page(&record->space, physical_address, KERNEL_MM_STATUS_OK);
    }
    if (shared != 0) {
        backend_status = arch_mmu_user_map_cow_page(
            &record->space,
            page_address,
            physical_address,
            backend_permissions_from_mm(vma->permissions));
        if (backend_status == ARCH_MMU_STATUS_OK) {
            mapped = 1;
        }
        if (backend_status == ARCH_MMU_STATUS_OK &&
            access == KERNEL_MM_WRITE) {
            backend_status = resolve_record_cow(
                record,
                page_address,
                backend_permissions_from_mm(vma->permissions));
        }
        if (backend_status != ARCH_MMU_STATUS_OK) {
            enum kernel_mm_status failure = map_file_page_status(backend_status);

            if (mapped != 0) {
                return discard_mapped_page(record,
                                           page_address,
                                           failure);
            }
            return discard_file_page(&record->space,
                                     physical_address,
                                     failure);
        }
    } else {
        backend_status = arch_mmu_user_map_owned_page(
            &record->space,
            page_address,
            physical_address,
            backend_permissions_from_mm(vma->permissions));
        if (backend_status != ARCH_MMU_STATUS_OK) {
            return discard_file_page(&record->space,
                                     physical_address,
                                     map_file_page_status(backend_status));
        }
    }
    status = KERNEL_MM_STATUS_OK;
    flush_user_page(record, page_address, vma->permissions);
    return status;
}

static enum kernel_mm_status resolve_user_fault_once(
    struct kernel_mm *mm,
    uint64_t virtual_address,
    uint32_t access, int *retry, int resumed)
{
    struct kernel_mm_record *record;
    struct arch_mmu_mapping mapping;
    struct kernel_vma vma;
    uint64_t satp;
    uint64_t page_address;
    uint64_t file_page_offset;
    uint64_t file_page_index;
    uint64_t file_size;
    uint32_t known = KERNEL_MM_READ | KERNEL_MM_WRITE |
                     KERNEL_MM_EXECUTE;
    int translation_synchronized = 0;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;
    enum arch_mmu_status backend_status;

    if (mm == 0 || (access & ~known) != 0U || access == 0U ||
        (access & (access - 1U)) != 0U) {
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
    if (virtual_address < ARCH_MMU_PAGE_SIZE ||
        virtual_address >= ARCH_MMU_USER_LIMIT ||
        record->vmas == 0) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    vma_status = kernel_vma_set_lookup(record->vmas,
                                       virtual_address,
                                       &vma);
    if (vma_status == KERNEL_VMA_STATUS_NOT_FOUND) {
        backend_status = arch_mmu_user_lookup(&record->space,
                                             virtual_address,
                                             &mapping);
        if (backend_status == ARCH_MMU_STATUS_OK) {
            return KERNEL_MM_STATUS_ADDRESS_SPACE;
        }
        return backend_status == ARCH_MMU_STATUS_NOT_MAPPED
                   ? KERNEL_MM_STATUS_NOT_MAPPED
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        return status_from_vma(vma_status);
    }
    if ((vma.permissions & access) == 0U) {
        return KERNEL_MM_STATUS_ACCESS;
    }
    backend_status = arch_mmu_user_lookup(&record->space,
                                         virtual_address,
                                         &mapping);
    if (backend_status == ARCH_MMU_STATUS_OK) {
        uint32_t requested = access == KERNEL_MM_READ ? ARCH_MMU_READ :
                             access == KERNEL_MM_WRITE ? ARCH_MMU_WRITE : ARCH_MMU_EXECUTE;
        /* CPU0服务或文件取页等待期间，其他任务可能已提交该页。
         * 远端MM由提交者同步，返回前激活再做本地刷新，迟到fault不重复RFENCE；
         * 非远端resumed路径保留既有本地刷新。 */
        int remote = remote_translation(record);
        if ((resumed || remote) && (mapping.permissions & requested)) {
            if (resumed && !remote) {
                flush_user_page(record,
                    virtual_address & ~BOAROS_PAGE_MASK, vma.permissions);
            }
            return KERNEL_MM_STATUS_OK;
        }

        if (access == KERNEL_MM_WRITE &&
            (mapping.permissions & ARCH_MMU_WRITE) == 0U) {
            if (vma.kind == KERNEL_VMA_KIND_FILE_SHARED) {
                struct mm_file_resident *page = find_file_resident(
                    record, virtual_address & ~BOAROS_PAGE_MASK);
                if (page == 0) __builtin_trap();
                if (kernel_open_file_memory_backed(vma.backing)) {
                    kernel_open_file_memory_modified(vma.backing);
                } else {
                    if (!kernel_page_cache_alias_active(&page->alias)) __builtin_trap();
                    kernel_page_cache_alias_mark_dirty(&page->alias);
                }
                backend_status = arch_mmu_user_protect_owned_page(
                    &record->space, page->address,
                    backend_permissions_from_mm(vma.permissions));
                if (backend_status != ARCH_MMU_STATUS_OK) __builtin_trap();
                flush_user_page(record,page->address,vma.permissions);
                return KERNEL_MM_STATUS_OK;
            }
            status = require_active_space(record);
            if (status != KERNEL_MM_STATUS_OK) {
                return status;
            }
            backend_status = resolve_record_cow(
                record,
                virtual_address & ~BOAROS_PAGE_MASK,
                backend_permissions_from_mm(vma.permissions));
            if (backend_status == ARCH_MMU_STATUS_OK) {
                if (vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE) {
                    struct mm_file_resident *page = find_file_resident(record,
                        virtual_address & ~BOAROS_PAGE_MASK);
                    if (page == 0 || arch_mmu_user_lookup(&record->space,
                            page->address, &mapping) != ARCH_MMU_STATUS_OK)
                        __builtin_trap();
                    page->private = 1;
                    page->physical_address = mapping.physical_address;
                }
                return KERNEL_MM_STATUS_OK;
            }
            if (backend_status == ARCH_MMU_STATUS_NO_MEMORY) {
                return KERNEL_MM_STATUS_NO_MEMORY;
            }
            return backend_status == ARCH_MMU_STATUS_STATE
                       ? KERNEL_MM_STATUS_STATE
                       : KERNEL_MM_STATUS_ADDRESS_SPACE;
        }
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (backend_status != ARCH_MMU_STATUS_NOT_MAPPED) {
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (vma.role == KERNEL_VMA_ROLE_STACK &&
        vma.end - (virtual_address & ~BOAROS_PAGE_MASK) >
            kernel_task_current_stack_limit()) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    if (arch_mmu_user_space_context(&record->space, &satp) !=
            ARCH_MMU_STATUS_OK ||
        arch_mmu_current_context() != satp) {
        return KERNEL_MM_STATUS_STATE;
    }
    page_address = virtual_address & ~BOAROS_PAGE_MASK;
    if ((vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE ||
         vma.kind == KERNEL_VMA_KIND_FILE_SHARED) &&
        (vma.fault_policy == KERNEL_VMA_FAULT_FILE_PRIVATE ||
         vma.fault_policy == KERNEL_VMA_FAULT_FILE_SHARED) &&
        vma.backing != 0) {
        uint64_t version = kernel_vma_set_generation(record->vmas);
        struct kernel_mm_file_source *source __attribute__((cleanup(unpin_file_source))) =
            find_file_source(record, vma.backing);
        if (!source || source->operations == UINT32_MAX) __builtin_trap();
        source->operations++;
        KERNEL_LOCK_SCOPE(node_guard);
        kernel_vfs_node_lock(kernel_open_file_node(vma.backing), &node_guard, 0);
        if (version != kernel_vma_set_generation(record->vmas)) {
            *retry = 1;
            return KERNEL_MM_STATUS_OK;
        }
        if (vma.backing_offset >
            UINT64_MAX - (page_address - vma.start)) {
            return KERNEL_MM_STATUS_ADDRESS_SPACE;
        }
        file_page_offset = vma.backing_offset +
                           (page_address - vma.start);
        file_size = kernel_open_file_size(vma.backing);
        if (file_page_offset >= file_size) {
            return KERNEL_MM_STATUS_BUS_FAULT;
        }
        /* Disk I/O owns the source and inode, but no MM mutation lock. */
        uint64_t prefetched;
        size_t valid;
        int created = 0;
        enum kernel_page_cache_status loaded = kernel_open_file_memory_backed(vma.backing)
            ? kernel_open_file_get_page_for_fault(vma.backing,
                file_page_offset >> BOAROS_PAGE_SHIFT, &prefetched, &valid, &created)
            : kernel_open_file_get_page(vma.backing,
                file_page_offset >> BOAROS_PAGE_SHIFT, &prefetched, &valid);
        if (loaded != KERNEL_PAGE_CACHE_STATUS_OK)
            return loaded == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_BUS_FAULT;
        struct fault_page_pin pin __attribute__((cleanup(release_fault_page))) = {
            .allocator = mm->allocator, .file = vma.backing, .address = prefetched,
            .index = file_page_offset >> BOAROS_PAGE_SHIFT, .created = created,
        };
        if (version != kernel_vma_set_generation(record->vmas) ||
            arch_mmu_user_lookup(&record->space, page_address, &mapping) != ARCH_MMU_STATUS_NOT_MAPPED) {
            *retry = 1;
            return KERNEL_MM_STATUS_OK;
        }
        status = reserve_file_residents(record, record->resident_count + 1);
        if (status != KERNEL_MM_STATUS_OK) return status;
        struct mm_file_resident *resident;
        enum kernel_heap_status hs = kernel_heap_allocate_zeroed(
            record->vma_heap, 1U, sizeof(*resident), (void **)&resident);
        if (hs != KERNEL_HEAP_STATUS_OK)
            return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                                   : KERNEL_MM_STATUS_STATE;
        file_page_index = file_page_offset >> BOAROS_PAGE_SHIFT;
        status = vma.kind == KERNEL_VMA_KIND_FILE_SHARED
                     ? map_shared_file_page(record, &vma, page_address,
                                            file_page_index, access, resident,
                                            &translation_synchronized)
                 : access == KERNEL_MM_WRITE
                     ? map_private_file_page(record,
                                             &vma,
                                             page_address,
                                             file_page_index,
                                             file_page_offset,
                                             file_size,
                                             &translation_synchronized)
                     : map_cached_file_page(record,
                                            &vma,
                                            page_address,
                                            file_page_index);
        if (status == KERNEL_MM_STATUS_OK) {
            if (arch_mmu_user_lookup(&record->space, page_address, &mapping) !=
                    ARCH_MMU_STATUS_OK) __builtin_trap();
            resident->address = page_address;
            resident->physical_address = mapping.physical_address;
            resident->private = vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE &&
                                access == KERNEL_MM_WRITE;
            publish_file_resident(record, resident);
            pin.created = 0;
        } else {
            if (resident->alias.previous != 0)
                kernel_page_cache_alias_detach(&resident->alias);
            (void)kernel_heap_release(record->vma_heap, resident);
        }
        if (status == KERNEL_MM_STATUS_OK &&
            translation_synchronized == 0) {
            flush_user_page(record, page_address, vma.permissions);
        }
        return status;
    }
    if (vma.kind == KERNEL_VMA_KIND_ELF_PRIVATE &&
        vma.fault_policy == KERNEL_VMA_FAULT_ELF &&
        vma.backing != 0) {
        return map_elf_source_page(record,
                                   &vma,
                                   page_address,
                                   access, retry);
    }
    if ((vma.kind == KERNEL_VMA_KIND_ANON_SHARED ||
         vma.kind == KERNEL_VMA_KIND_SYSV_SHM) &&
        vma.fault_policy == KERNEL_VMA_FAULT_ANON_SHARED &&
        vma.backing != 0) {
        struct kernel_memory_object *object = vma.backing;
        uint64_t index = (vma.backing_offset +
                          page_address - vma.start) >> BOAROS_PAGE_SHIFT;
        uint64_t physical_address;
        int created;
        enum kernel_memory_object_status shared_status;

        shared_status = kernel_memory_object_prepare_page(object, index,
                                                   &physical_address,
                                                   &created);
        if (shared_status == KERNEL_MEMORY_OBJECT_NO_MEMORY)
            return KERNEL_MM_STATUS_NO_MEMORY;
        if (shared_status != KERNEL_MEMORY_OBJECT_OK)
            return KERNEL_MM_STATUS_STATE;
        backend_status = arch_mmu_user_map_owned_page(
            &record->space, page_address, physical_address,
            backend_permissions_from_mm(vma.permissions));
        kernel_memory_object_finish_page(object, index, physical_address,
                                        backend_status == ARCH_MMU_STATUS_OK);
        if (backend_status != ARCH_MMU_STATUS_OK) {
            return backend_status == ARCH_MMU_STATUS_NO_MEMORY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_ADDRESS_SPACE;
        }
        flush_user_page(record, page_address, vma.permissions);
        return KERNEL_MM_STATUS_OK;
    }
    if (vma.kind != KERNEL_VMA_KIND_ANONYMOUS ||
        vma.fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    backend_status = arch_mmu_user_map_zeroed_page(
        &record->space,
        page_address,
        backend_permissions_from_mm(vma.permissions));
    if (backend_status == ARCH_MMU_STATUS_OK) {
        flush_user_page(record, page_address, vma.permissions);
        return KERNEL_MM_STATUS_OK;
    }
    if (backend_status == ARCH_MMU_STATUS_NO_MEMORY) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    return KERNEL_MM_STATUS_ADDRESS_SPACE;
}

static enum kernel_mm_status mm_resolve_user_fault_impl(
    struct kernel_mm *mm, uint64_t address, uint32_t access)
{
    KERNEL_NO_RECLAIM_IO;
    int resumed = 0;
    for (;;) {
        int retry = 0;
        enum kernel_mm_status result = resolve_user_fault_once(mm, address, access, &retry, resumed);
        if (!retry) {
            return result;
        }
        resumed = 1;
#if BOAROS_USER_SMP_COMPOSITION
        riscv_user_composition_fault_retry(address, resumed);
#endif
    }
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

enum kernel_mm_status kernel_mm_concurrent_context(const struct kernel_mm *mm,
    uint64_t *context)
{
    kernel_assert_service_context();
    struct kernel_mm_record *record;
    if (!mm || !context) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status = resolve_record_storage(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (mm->state != KERNEL_MM_LIVE || !record->concurrent) return KERNEL_MM_STATUS_STATE;
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    return arch_mmu_user_space_context(&record->space, context) == ARCH_MMU_STATUS_OK ?
        KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_ADDRESS_SPACE;
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
    return mm->state==KERNEL_MM_LIVE && record->concurrent &&
        record->vma_heap==heap && mm->allocator==heap->page_allocator &&
        !mm->allocator->reclaimer && !mm->allocator->pressure_notify &&
        !mm->allocator->pressure_wait
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
        if (logical && !remote_translation(record)) __builtin_trap();
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        uint64_t context;
        if (arch_mmu_user_space_context(&record->space, &context) != ARCH_MMU_STATUS_OK) {
            status = KERNEL_MM_STATUS_ADDRESS_SPACE; goto out;
        }
        if (record != previous && (record->active_cpus & (1U << logical))) __builtin_trap();
        record->active_cpus |= 1U << logical;
        if (arch_mmu_switch_context(context) != ARCH_MMU_STATUS_OK) __builtin_trap();
        if (remote_translation(record)) { arch_mmu_flush_all(); arch_mmu_sync_instructions(); }
        cpu->active_mm_record = record;
        cpu->active_mm_generation = record->tlb_generation;
    } else {
        if (arch_mmu_switch_context(kernel_context) != ARCH_MMU_STATUS_OK) {
            status = KERNEL_MM_STATUS_ADDRESS_SPACE; goto out;
        }
        cpu->active_mm_record = 0;
        cpu->active_mm_generation = 0;
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
        if (record->concurrent) kernel_mutex_lock(&record->operations,&retire);
        if (record->access_pins || record->write_pins || record->active_cpus ||
            record->pending_retirement || record->retired_file_mappings) __builtin_trap();
        if (record->vmas == 0 && record->shared_anon == 0 &&
            record->file_sources == 0 &&
            record->elf_sources == 0 && record->executable_file == 0) {
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
        forget_file_residents(record, 0, UINT64_MAX);
        struct mm_file_resident *retired=record->retired_file_residents;
        record->retired_file_residents=0;
        drain_retired_residents(record,retired);
        if (record->resident_buckets != 0)
            (void)kernel_heap_release(record->vma_heap, record->resident_buckets);
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

enum kernel_mm_status kernel_mm_mmap_file_private(struct kernel_mm *mm,
    struct kernel_open_file_description **file,uint64_t hint,uint64_t length,
    uint64_t offset,uint32_t permissions,uint32_t flags,uint64_t *address)
{
    if (!file || !*file || !address ||
        (kernel_open_file_mode(*file) & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG)
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    for (;;) {
        MM_OPERATION(mm);
        struct mm_file_mapping_plan plan;
        enum kernel_mm_status status=prepare_file_mapping(mm,hint,length,offset,permissions,flags,&plan);
        if (status != KERNEL_MM_STATUS_OK) return status;
        struct kernel_vfs_node *node=kernel_open_file_node(*file);
        if (operation.record->concurrent && kernel_open_file_check_mapping_domain(*file,
                operation.record->vma_heap,mm->allocator)) return KERNEL_MM_STATUS_STATE;
        int cached=operation.record->concurrent && !kernel_open_file_memory_backed(*file);
        uint64_t generation=kernel_vma_set_generation(operation.record->vmas);
        KERNEL_LOCK_SCOPE(inode);
        if (kernel_open_file_memory_backed(*file) || operation.record->concurrent) {
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
        if (!status && cached) __atomic_store_n(&operation.record->cache_files,1,__ATOMIC_RELEASE);
        return status;
    }
}

enum kernel_mm_status kernel_mm_validate_file_private_mapping(struct kernel_mm *mm,
    uint64_t hint,uint64_t length,uint64_t offset,uint32_t permissions,uint32_t flags)
{
    MM_OPERATION(mm);
    return validate_file_mapping(mm,hint,length,offset,permissions,flags);
}

static int msync_concurrent(struct kernel_mm *mm,uint64_t address,uint64_t length,
    uint32_t flags,struct mm_operation *operation)
{
    uint64_t rounded=(length+BOAROS_PAGE_MASK)&~BOAROS_PAGE_MASK;
    uint64_t end=address+rounded,cursor=address;
    if (end<address) return -KERNEL_ENOMEM;
    int hole=0,result=0;
    while (cursor<end) {
        struct { struct kernel_open_file_description *file; uint64_t start,end; } sources[16];
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
                if (!source || kernel_open_file_acquire(source->file)) __builtin_trap();
                sources[count].file=source->file;
                sources[count].start=vma.backing_offset+cursor-vma.start;
                sources[count].end=sources[count].start+finish-cursor;
                count++;
            }
            cursor=finish;
        }
        /* OFD实际引用固定本批范围；同步会回调同一MM，必须先结束gate保护。 */
        mm_operation_end(operation);
        for (unsigned i=0;i<count;i++) {
            if (!result) result=kernel_open_file_sync_range(sources[i].file,sources[i].start,sources[i].end);
            if (kernel_open_file_release(&sources[i].file)) __builtin_trap();
        }
        if (result || cursor>=end) break;
        enum kernel_mm_status acquired=mm_operation_begin(mm,operation);
        if (acquired) return acquired==KERNEL_MM_STATUS_CONFLICT?-KERNEL_EAGAIN:-KERNEL_ENOMEM;
    }
    return result?result:hole?-KERNEL_ENOMEM:0;
}

int kernel_mm_msync(struct kernel_mm *mm, uint64_t address,
    uint64_t length, uint32_t flags)
{
    /* 保留参数及零长度的既有 errno 顺序；匿名区间不产生文件 I/O。 */
    if ((flags & ~7U) || (address & BOAROS_PAGE_MASK) ||
        ((flags & 1U) && (flags & 4U)) || !length)
        return mm_msync_impl(mm, address, length, flags);
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    enum kernel_mm_status status = mm_operation_begin(mm, &operation);
    if (status != KERNEL_MM_STATUS_OK)
        return status == KERNEL_MM_STATUS_CONFLICT ? -KERNEL_EAGAIN : -KERNEL_ENOMEM;
    /* tmpfs同步也取得OFD rank10；所有并发MM都先固定来源并结束gate25，
     * 不能仅对磁盘来源释放gate后调用。 */
    if (operation.record->concurrent)
        return msync_concurrent(mm,address,length,flags,&operation);
    return mm_msync_impl(mm, address, length, flags);
}

enum kernel_mm_status kernel_mm_mmap_anonymous(struct kernel_mm *mm,
    uint64_t hint, uint64_t length, uint32_t permissions, uint32_t flags, uint64_t *address)
{
    MM_OPERATION(mm);
    return mm_mmap_anonymous_impl(mm, hint, length, permissions, flags, address);
}
enum kernel_mm_status kernel_mm_munmap(struct kernel_mm *mm, uint64_t address, uint64_t length)
{
    MM_OPERATION(mm);
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
    MM_OPERATION(mm);
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
    return mm_brk_impl(mm, requested, result);
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
    if (physical_page_resolve(allocator, *address, pointer) != PHYSICAL_PAGE_STATUS_OK) {
        physical_page_release(allocator, *address); *address = KERNEL_MM_NO_PAGE;
        return KERNEL_MM_STATUS_PAGE_ACCESS;
    }
    return KERNEL_MM_STATUS_OK;
}

/* gate固定resident/alias，许可由页write状态仲裁；等待时结束全部对象保护。 */
static enum kernel_mm_status cached_write_fault(struct mm_operation *operation,
    struct kernel_open_file_description *file,const struct kernel_vma *vma,uint64_t address,
    struct kernel_lock_guard *inode)
{
    struct kernel_mm_record *record=operation->record;
    struct mm_file_resident *resident=find_file_resident(record,address);
    if (!resident || !kernel_page_cache_alias_active(&resident->alias)) __builtin_trap();
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
        targets=note_tlb_change_locked(record,&batch);
    }
    finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,0);
    kernel_page_cache_write_end(&permit);
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status mapped_file_fault(struct mm_operation *operation,
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
    if (kernel_open_file_acquire(file)) return KERNEL_MM_STATUS_STATE;
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    KERNEL_LOCK_SCOPE(inode);
    kernel_vfs_node_lock(node, &inode, 0);
    uint64_t epoch;
    uint64_t cookie = 0, old = KERNEL_MM_NO_PAGE, replacement = KERNEL_MM_NO_PAGE;
    struct kernel_open_file_page_preparation source = {0};
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    struct kernel_mm_retirement batch = {0};
    struct mm_file_resident *resident = 0;
    void *old_bytes = 0, *copy = 0;
    unsigned missing = 0, count = 0;
    uint32_t targets = 0;
    int shared = vma.kind == KERNEL_VMA_KIND_FILE_SHARED;
    int disk = !kernel_open_file_memory_backed(file);
    enum kernel_mm_status status = require_fault_space(record);
    if (status != KERNEL_MM_STATUS_OK) goto finish;
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
            targets = record->active_cpus;
            kernel_raw_lock_release(&guard);
            finish_tlb_change(record, &batch, targets, address, BOAROS_PAGE_SIZE,
                (vma.permissions & KERNEL_MM_EXECUTE) != 0);
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
                targets = note_tlb_change_locked(record,&batch);
                kernel_raw_lock_release(&guard);
                finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,
                    (vma.permissions & KERNEL_MM_EXECUTE) != 0);
                status = KERNEL_MM_STATUS_OK;
                old = KERNEL_MM_NO_PAGE;
                goto finish;
            }
            if (physical_page_acquire(allocator,old) || physical_page_resolve(allocator,old,&old_bytes))
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
            count += source.created;
            replacement = source.address;
        }
    }
    if (status == KERNEL_MM_STATUS_OK && !shared && access == KERNEL_MM_WRITE) {
        status = allocate_private_page(allocator,&replacement,&copy);
        if (status == KERNEL_MM_STATUS_OK) {
            void *bytes = old_bytes;
            if (!cookie && physical_page_resolve(allocator,source.address,&bytes)) __builtin_trap();
            for (size_t i=0; i<BOAROS_PAGE_SIZE; i++) ((unsigned char *)copy)[i]=((unsigned char *)bytes)[i];
            count++;
        }
    }
    if (status == KERNEL_MM_STATUS_OK && !cookie) {
        enum kernel_heap_status hs = kernel_heap_allocate_zeroed(record->vma_heap,1,sizeof(*resident),(void **)&resident);
        if (hs != KERNEL_HEAP_STATUS_OK)
            status = hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
    }
    for (unsigned i=0; i<2 && status==KERNEL_MM_STATUS_OK; i++) if (missing & (1U << i)) {
        void *table;
        status = allocate_private_page(allocator,&prepared.tables[i],&table);
        if (status == KERNEL_MM_STATUS_OK) { clear_page(table); count++; }
    }
    enum kernel_mm_status gate_status = mm_operation_lock(operation);
    if (gate_status != KERNEL_MM_STATUS_OK) status = gate_status;
    { KERNEL_RAW_SCOPE(guard,&record->metadata); record->prepared_pages += count; }
    if (status == KERNEL_MM_STATUS_OK) {
        kernel_vfs_node_lock(node,&inode,0);
        uint64_t latest;
        /* 普通单CPU MM 没有 gate：等待 inode 或扩容期间仍能显式调度。
         * 先拒绝已过期候选，再于最后可睡眠准备之后重查完整 VMA 身份。 */
        if (generation != kernel_vma_set_generation(record->vmas) ||
            kernel_file_mapping_epoch(node,&latest) || latest != epoch ||
            (source.pending && source.epoch != epoch) || offset >= kernel_open_file_size(file))
            status = KERNEL_MM_STATUS_CONFLICT;
    }
    if (status == KERNEL_MM_STATUS_OK && !cookie)
        status = reserve_file_residents(record,record->resident_count + 1);
    if (status == KERNEL_MM_STATUS_OK) {
        struct kernel_vma latest;
        if (generation != kernel_vma_set_generation(record->vmas) ||
            kernel_vma_set_lookup(record->vmas,address,&latest) != KERNEL_VMA_STATUS_OK ||
            latest.backing != file || latest.start != vma.start || latest.backing_offset != vma.backing_offset ||
            latest.permissions != vma.permissions) status = KERNEL_MM_STATUS_CONFLICT;
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
            targets=note_tlb_change_locked(record,&batch);
        }
    }
    if (status == KERNEL_MM_STATUS_OK) {
        finish_tlb_change(record,&batch,targets,0,UINT64_MAX,(vma.permissions & KERNEL_MM_EXECUTE)!=0);
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
    if (kernel_open_file_release(&file)) __builtin_trap();
    return status;
}
static enum kernel_mm_status anonymous_fault(struct mm_operation *operation,
    uint64_t address, uint32_t access)
{
    struct kernel_mm_record *record = operation->record;
    struct physical_page_allocator *allocator = record->space.allocator;
    struct kernel_vma vma;
    if (!access || (access & (access - 1)) || (access & ~KERNEL_VMA_ALL_PERMISSIONS))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (address < BOAROS_PAGE_SIZE || address >= ARCH_MMU_USER_LIMIT)
        return KERNEL_MM_STATUS_NOT_MAPPED;
    enum kernel_vma_status vs = kernel_vma_set_lookup(record->vmas, address, &vma);
    if (vs == KERNEL_VMA_STATUS_NOT_FOUND) return KERNEL_MM_STATUS_NOT_MAPPED;
    if (vs != KERNEL_VMA_STATUS_OK) return KERNEL_MM_STATUS_STATE;
    if (!(vma.permissions & access)) return KERNEL_MM_STATUS_ACCESS;
    int shared = vma.kind == KERNEL_VMA_KIND_ANON_SHARED;
    if (vma.shm_attachment || (!shared && (vma.kind != KERNEL_VMA_KIND_ANONYMOUS || vma.backing)) ||
        (shared && (!vma.backing || vma.fault_policy != KERNEL_VMA_FAULT_ANON_SHARED)))
        __builtin_trap();
    struct kernel_memory_object *object = shared ? vma.backing : 0;
    int created = 0;
    enum kernel_mm_status status = require_fault_space(record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    address &= ~BOAROS_PAGE_MASK;
    uint64_t generation = kernel_vma_set_generation(record->vmas), cookie = 0, old = 0;
    uint64_t replacement = KERNEL_MM_NO_PAGE;
    uint64_t shared_preparation = KERNEL_MM_NO_PAGE;
    void *old_pointer = 0, *new_pointer;
    unsigned missing = 0;
    unsigned prepared_count = 0;
    uint32_t targets;
    struct kernel_mm_retirement batch = {0};
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    {
        struct kernel_mm_mapping mapping;
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        status = lookup_record(record, address, &mapping);
        if (status == KERNEL_MM_STATUS_OK && (mapping.permissions & access)) {
            targets = record->active_cpus;
            kernel_raw_lock_release(&guard);
            finish_tlb_change(record, &batch, targets, address, BOAROS_PAGE_SIZE,
                (vma.permissions & KERNEL_MM_EXECUTE) != 0);
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
                targets = note_tlb_change_locked(record, &batch);
                kernel_raw_lock_release(&guard);
                finish_tlb_change(record, &batch, targets, address, BOAROS_PAGE_SIZE,
                    (vma.permissions & KERNEL_MM_EXECUTE) != 0);
                return KERNEL_MM_STATUS_OK;
            }
            /* COW旧页为只读快照；实际引用覆盖解锁后的复制和竞争者提交。 */
            if (physical_page_acquire(allocator, old) != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
            if (physical_page_resolve(allocator, old, &old_pointer) != PHYSICAL_PAGE_STATUS_OK) {
                physical_page_release(allocator, old);
                return KERNEL_MM_STATUS_PAGE_ACCESS;
            }
        } else if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
            if (!shared && vma.fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO) return status;
            if (arch_mmu_user_leaf_missing(&record->space, address, &missing) != ARCH_MMU_STATUS_OK)
                return KERNEL_MM_STATUS_ADDRESS_SPACE;
        } else return status;
    }
    /* VMA注册覆盖此处取得实际后备owner；放gate后不再借用registry节点。 */
    if (object && kernel_memory_object_acquire(object) != KERNEL_MEMORY_OBJECT_OK)
        return KERNEL_MM_STATUS_STATE;
    mm_gate_release(operation->record,&operation->guard);
    if (object) {
        enum kernel_memory_object_status result = kernel_memory_object_prepare_page(object,
            (vma.backing_offset + address - vma.start) >> BOAROS_PAGE_SHIFT,
            &replacement, &created);
        status = result == KERNEL_MEMORY_OBJECT_OK ? KERNEL_MM_STATUS_OK :
            result == KERNEL_MEMORY_OBJECT_NO_MEMORY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
        if (status == KERNEL_MM_STATUS_OK) {
            prepared_count += created != 0;
            shared_preparation = replacement;
        }
    } else {
        status = allocate_private_page(allocator, &replacement, &new_pointer);
        if (status == KERNEL_MM_STATUS_OK) {
            prepared_count++;
            if (cookie) {
                unsigned char *destination = new_pointer, *source = old_pointer;
                for (size_t i = 0; i < BOAROS_PAGE_SIZE; i++) destination[i] = source[i];
            } else clear_page(new_pointer);
        }
    }
    for (unsigned i = 0; i < 2 && status == KERNEL_MM_STATUS_OK; i++) if (missing & (1U << i)) {
        void *pointer;
        status = allocate_private_page(allocator, &prepared.tables[i], &pointer);
        if (status == KERNEL_MM_STATUS_OK) { clear_page(pointer); prepared_count++; }
    }
    enum kernel_mm_status gate_status = mm_operation_lock(operation);
    if (gate_status != KERNEL_MM_STATUS_OK) status = gate_status;
    { KERNEL_RAW_SCOPE(guard, &record->metadata); record->prepared_pages += prepared_count; }
    if (status == KERNEL_MM_STATUS_OK) {
        struct kernel_mm_mapping current;
        KERNEL_RAW_SCOPE(guard, &record->metadata);
        struct kernel_vma latest;
        if (generation != kernel_vma_set_generation(record->vmas) ||
            kernel_vma_set_lookup(record->vmas, address, &latest) != KERNEL_VMA_STATUS_OK ||
            latest.backing != vma.backing || latest.backing_offset != vma.backing_offset ||
            latest.start != vma.start || latest.permissions != vma.permissions)
            status = KERNEL_MM_STATUS_CONFLICT;
        else {
            enum arch_mmu_status backend;
            if (cookie) backend = arch_mmu_user_cow_commit(&record->space, address, cookie,
                replacement, backend_permissions_from_mm(vma.permissions), &batch);
            else if (lookup_record(record, address, &current) != KERNEL_MM_STATUS_NOT_MAPPED)
                backend = ARCH_MMU_STATUS_CONFLICT;
            else backend = arch_mmu_user_map_prepared_page(&record->space, address, replacement,
                backend_permissions_from_mm(vma.permissions), &prepared);
            status = backend == ARCH_MMU_STATUS_OK ? KERNEL_MM_STATUS_OK :
                backend == ARCH_MMU_STATUS_CONFLICT || backend == ARCH_MMU_STATUS_NOT_MAPPED ?
                KERNEL_MM_STATUS_CONFLICT : KERNEL_MM_STATUS_ADDRESS_SPACE;
            if (status == KERNEL_MM_STATUS_OK) {
                replacement = KERNEL_MM_NO_PAGE;
                targets = note_tlb_change_locked(record, &batch);
            }
        }
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
        int memory_file = operation.record->vmas &&
            kernel_vma_set_lookup(operation.record->vmas,address,&vma) == KERNEL_VMA_STATUS_OK &&
            (vma.kind == KERNEL_VMA_KIND_FILE_SHARED || vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE) &&
            (kernel_open_file_memory_backed(vma.backing) || operation.record->concurrent);
        if (!operation.guard.lock && !memory_file) return mm_resolve_user_fault_impl(mm, address, access);
        enum kernel_mm_status status = memory_file ? mapped_file_fault(&operation,address,access) :
            anonymous_fault(&operation, address, access);
        if (status != KERNEL_MM_STATUS_CONFLICT || current_pins(mm)) return status;
        { KERNEL_RAW_SCOPE(guard, &operation.record->metadata); operation.record->fault_retries++; }
        /* loser归还全部私有候选，下一轮重取VMA/PTE，不把旧许可带过睡眠。 */
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
        task_mm->record_page_address != mm->record_page_address) task = 0;
    uint64_t block_reads = task ? kernel_proc_task_block_reads(task) : 0U;
    enum kernel_mm_status status = resolve_user_fault(mm, address, access);
    if (status == KERNEL_MM_STATUS_OK && task)
        kernel_proc_task_note_fault(task,
            kernel_proc_task_block_reads(task) != block_reads);
    return status;
}

enum kernel_mm_status kernel_mm_enable_concurrency(struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    kernel_assert_service_context();
    if (!mm) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (mm->state != KERNEL_MM_LIVE || __atomic_load_n(&record->references, __ATOMIC_ACQUIRE) != 1 || record->concurrent ||
        !record->vmas || record->elf_sources || record->executable_file ||
        mm->allocator->reclaimer || mm->allocator->pressure_notify)
        return KERNEL_MM_STATUS_STATE;
    for (uint32_t i = 0; i < kernel_vma_set_count(record->vmas); i++) {
        struct kernel_vma vma;
        if (kernel_vma_set_get_at(record->vmas, i, &vma) != KERNEL_VMA_STATUS_OK) __builtin_trap();
        int valid = vma.kind == KERNEL_VMA_KIND_ANONYMOUS ? !vma.backing :
            vma.kind == KERNEL_VMA_KIND_ANON_SHARED ?
                kernel_memory_object_check_domain(vma.backing, record->vma_heap, mm->allocator) == KERNEL_MEMORY_OBJECT_OK :
            (vma.kind == KERNEL_VMA_KIND_FILE_SHARED || vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE) &&
                !kernel_open_file_check_mapping_domain(vma.backing,record->vma_heap,mm->allocator);
        if (vma.shm_attachment || !valid)
            return KERNEL_MM_STATUS_STATE;
    }
    for (struct kernel_mm_file_source *source=record->file_sources; source; source=source->next)
        if (source->draining || source->operations ||
            kernel_open_file_check_mapping_domain(source->file,record->vma_heap,mm->allocator))
            return KERNEL_MM_STATUS_STATE;
    for (struct kernel_mm_file_source *source=record->file_sources;source;source=source->next)
        if (!kernel_open_file_memory_backed(source->file)) record->cache_files=1;
    /* 尚未发布且独占；以后才能创建另一MM句柄/绑定受控任务。 */
    __atomic_store_n(&record->concurrent, 1, __ATOMIC_RELEASE);
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_enable_remote_user_execution(struct kernel_mm *mm)
{
    kernel_assert_service_context();
    struct kernel_mm_record *record;
    if (!mm) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status) return status;
    KERNEL_RAW_SCOPE(guard, &record->metadata);
    if (mm->state != KERNEL_MM_LIVE || record->stage != KERNEL_MM_RECORD_LIVE ||
        record->concurrent) return KERNEL_MM_STATUS_STATE;
    if (record->remote_user) return KERNEL_MM_STATUS_OK;
    if (__atomic_load_n(&record->references, __ATOMIC_ACQUIRE) != 1 ||
        record->active_cpus || record->pending_retirement) return KERNEL_MM_STATUS_STATE;
    record->remote_user = 1;
    record->synchronized_table_pages=record->space.table_pages;
    return KERNEL_MM_STATUS_OK;
}
