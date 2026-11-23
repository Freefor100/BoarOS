#include <kernel/aio.h>
#include <kernel/vdso.h>
#include <kernel/mm_backend.h>
#include <kernel/hugetlb.h>
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
#include <kernel/task_state.h>
#include <kernel/sync.h>
#include <kernel/shm.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/vma.h>
#include <kernel/vfs.h>
#include <kernel/tmpfs.h>
#include <arch/elf.h>
#include "../fs/vfs_internal.h"
#include "userfaultfd_internal.h"

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
    struct kernel_mm_file_source *fork_peer;
};

struct mm_huge_map {
    struct mm_huge_map *next;
    struct kernel_hugetlb_map *map;
    struct kernel_mm mm; /* registration-borrowed view; not a cyclic MM owner */
    uint64_t pages, fork_epoch;
    struct kernel_hugetlb_mapping registration;
};
struct kernel_mm_shared_anon {
    struct kernel_mm_shared_anon *next;
    struct kernel_memory_object *object;
    struct kernel_memory_mapping registration;
    uint64_t fork_epoch;
};

struct mm_file_mapping {
    struct mm_file_mapping *next;
    struct kernel_file_mapping registration;
};

/* PTE owns the physical reference. This metadata survives cache eviction and
 * PROT_NONE; fork copies provenance, not a guess based on the PTE COW bit. */
struct mm_file_resident {
    struct mm_file_resident *next;
    struct mm_file_resident **previous;
    struct mm_file_resident *hash_next;
    uint64_t address;
    uint64_t physical_address;
    unsigned private:1, with_alias:1;
};

/* Only a shared disk mapping participates in cache writeback callbacks. */
struct mm_file_alias_resident {
    struct mm_file_resident base;
    struct kernel_mm_record *record;
    struct mm_file_alias_resident *clone_source;
    struct mm_file_alias_resident *cache_retry_next;
    struct kernel_page_cache_alias alias;
    int cache_retry_registered;
};

static struct mm_file_alias_resident *file_alias_resident(struct mm_file_resident *page)
{
    return page && page->with_alias ? (struct mm_file_alias_resident *)page : NULL;
}

struct kernel_mm_elf_source {
    struct kernel_mm_elf_source *next;
    struct kernel_elf64_source *source;
    _Atomic uint32_t faults;
    int draining;
    struct kernel_mm_elf_source *fork_peer;
};

struct mm_lazy_page {
    struct mm_lazy_page *next, **previous, *hash_next;
    uint64_t address, physical, birth;
};

struct mm_lazy_scan_cursor { struct mm_lazy_page *next; };

static struct kernel_raw_lock lazy_registry_lock = {
    .rank = KERNEL_RAW_RANK_OBJECT, .initialized = KERNEL_RAW_INITIALIZED,
};
static struct kernel_mm_record *lazy_registry;
static unsigned lazy_registry_count;
/* One pressure round owns registry traversal; no MM gate is held while
 * waiting for this qualification. */
static struct kernel_mutex lazy_scan_lock;
static int lazy_scan_initialized;
static uint64_t lazy_scan_round;
static uint64_t lazy_birth_sequence;

struct kernel_mm_range_claim {
    uint64_t start, end;
    struct kernel_mm_range_claim *next;
};

struct kernel_mm_record {
    uint64_t futex_id;
    uint32_t references;
    uint32_t users;
    uint32_t record_references; /* MM owner and weak pagemap/userfaultfd handles. */
    struct kernel_userfault_ctx *userfaults; /* MM operations writer updates the list. */
    struct kernel_aio_context *aio_contexts;
    uint32_t aio_destinations, aio_fork_waiters;
    uint32_t coredump_filter; /* MM-owned: CLONE_VM shares; fork/exec copy */
    uint32_t core_remote; /* high bit: dump owner, low bits: remote writers */
    struct kernel_raw_rwlock metadata;
    uint64_t access_pins, write_pins;
    struct kernel_rwlock operations;
    struct kernel_rwlock vma_lock;
    struct kernel_wait_queue range_changed;
    struct kernel_mm_range_claim *range_claims;
    uint64_t range_edit_generation;
    uint64_t anonymous_zero_page;
#if !defined(BOAROS_ARCH_LOONGARCH)
    struct {
        uint64_t last;
        int direction;
    } anonymous_access[2]; /* Read and write histories are independent. */
#endif
    struct kernel_wait_queue pins_changed;
    uint32_t active_cpus, pending_retirement,borrowed_cpus;
    uint64_t tlb_generation, instruction_generation;
    uint64_t pin_acquisitions, prepared_pages, fault_retries;
    uint64_t retirement_batches, retired_pages, pin_waits;
    enum kernel_mm_record_stage stage;
    uint64_t start_code, end_code, start_stack;
    uint64_t start_data, end_data;
    uint64_t start_brk;
    uint64_t current_brk;
    uint64_t brk_limit;
    uint64_t mmap_base;
    uint64_t vdso_address;
#if KERNEL_HAS_VDSO_ELF
    uint64_t vvar_page;
#endif
    uint32_t brk_initialized;
    uint32_t recent_fault_vma;
    struct arch_mmu_user_space space;
    struct kernel_vma_set *vmas;
    struct kernel_heap *vma_heap;
    struct kernel_mm_file_source *file_sources;
    _Atomic(struct kernel_mm_file_source *) retired_file_sources;
    struct mm_huge_map *huge_maps, *retired_huge_maps;
    struct kernel_mm_shared_anon *shared_anon;
    struct kernel_mm_shared_anon *retired_shared_anon;
    struct mm_file_mapping *file_mappings;
    struct mm_file_mapping *retired_file_mappings;
    struct mm_file_resident *file_residents;
    struct mm_file_resident *retired_file_residents;
    uint64_t cache_gate_progress; /* Release generation, with low bit set for queued retries. */
    struct mm_file_alias_resident *cache_retry_head;
    uint64_t resident_probes;
    struct mm_file_resident **resident_buckets;
    size_t resident_capacity;
    size_t resident_count;
    unsigned resident_bucket_shift;
    struct kernel_mm_elf_source *elf_sources;
    _Atomic(struct kernel_mm_elf_source *) retired_elf_sources;
    struct kernel_open_file_description *executable_file;
    uint32_t future_lock_flags;
    struct mm_lazy_page *lazy_pages, *lazy_retired, **lazy_buckets;
    struct mm_lazy_scan_cursor *lazy_scan_cursor;
    size_t lazy_capacity, lazy_count;
    struct kernel_mm_record *lazy_next, *lazy_previous;
    unsigned membarrier_registrations;
    struct kernel_wait_queue lazy_drained;
    uint32_t lazy_borrows, lazy_registered;
    uint64_t lazy_seen_round, lazy_birth;
    /* Each CPU owns its slot while preemption is disabled. Zero is unseen;
     * publication generations start at one. Keep completion across MM switches
     * without a new shared lock. */
    uint64_t instruction_seen[BOAROS_MAX_CPUS];
};

/* 关闭位与归还计数共用原子字：最后归还者不需要跨变量猜测是否有人排空。
 * 低32位保持原计数上限；只有metadata writer修改关闭位。 */
#define MM_PIN_CLOSED (UINT64_C(1) << 63)
#if BOAROS_MAX_CPUS > 1
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0),
    "MM pin states require inline 64-bit atomics");
#endif
static inline uint64_t pin_state_read(const uint64_t *state)
{
#if BOAROS_MAX_CPUS > 1
    return __atomic_load_n(state, __ATOMIC_ACQUIRE);
#else
    return *state;
#endif
}
static inline uint32_t pin_count_add(uint64_t *count)
{
#if BOAROS_MAX_CPUS > 1
    return (uint32_t)__atomic_fetch_add(count, 1U, __ATOMIC_RELAXED);
#else
    return (uint32_t)(*count)++;
#endif
}
static inline uint64_t pin_count_sub(uint64_t *count,unsigned amount)
{
#if BOAROS_MAX_CPUS > 1
    return __atomic_fetch_sub(count, amount, __ATOMIC_RELEASE);
#else
    uint64_t previous=*count;
    *count-=amount;
    return previous;
#endif
}
static int pin_admission_closed(const struct kernel_mm_record *record)
{
#if BOAROS_MAX_CPUS > 1
    /* 许可检查已有metadata资格；这里只取关闭位，不承担访问完成的acquire。 */
    if (__atomic_load_n(&record->access_pins, __ATOMIC_RELAXED) & MM_PIN_CLOSED) return 1;
    return __atomic_load_n(&record->write_pins, __ATOMIC_RELAXED) & MM_PIN_CLOSED ? 2 : 0;
#else
    if (record->access_pins & MM_PIN_CLOSED) return 1;
    return record->write_pins & MM_PIN_CLOSED ? 2 : 0;
#endif
}
static void pin_admission_close(struct kernel_mm_record *record, int writers_only)
{
    uint64_t *state = writers_only ? &record->write_pins : &record->access_pins;
#if BOAROS_MAX_CPUS > 1
    __atomic_fetch_or(state, MM_PIN_CLOSED, __ATOMIC_ACQ_REL);
#else
    *state |= MM_PIN_CLOSED;
#endif
}
static void pin_admission_open(struct kernel_mm_record *record)
{
#if BOAROS_MAX_CPUS > 1
    __atomic_fetch_and(&record->access_pins, ~MM_PIN_CLOSED, __ATOMIC_RELEASE);
    __atomic_fetch_and(&record->write_pins, ~MM_PIN_CLOSED, __ATOMIC_RELEASE);
#else
    record->access_pins &= ~MM_PIN_CLOSED;
    record->write_pins &= ~MM_PIN_CLOSED;
#endif
}
static inline uint64_t mm_generation_read(const uint64_t *generation)
{
#if BOAROS_MAX_CPUS > 1
    return __atomic_load_n(generation, __ATOMIC_ACQUIRE);
#else
    return *generation;
#endif
}
static uint64_t mm_generation_advance(uint64_t *generation)
{
    uint64_t next = *generation + 1U;
    if (!next) __builtin_trap();
#if BOAROS_MAX_CPUS > 1
    __atomic_store_n(generation, next, __ATOMIC_RELEASE);
#else
    *generation = next;
#endif
    return next;
}
static inline uint32_t mm_active_cpus(const struct kernel_mm_record *record)
{
#if BOAROS_MAX_CPUS > 1
    return __atomic_load_n(&record->active_cpus, __ATOMIC_ACQUIRE);
#else
    return record->active_cpus;
#endif
}
static inline uint32_t mm_sync_targets(const struct kernel_mm_record *record, int include_borrowed)
{
#if BOAROS_MAX_CPUS > 1
    /* Retiring translations includes kernel-only roots; pure instruction
     * publication is caught by their generation check on user re-entry.
     * PTE/代次先于目标采样可见；与加入者的SC置位形成两种交接：采到它
     * 就等待远端确认，没采到则其置位后的本地全量失效覆盖这些写入。 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t active=__atomic_load_n(&record->active_cpus,__ATOMIC_SEQ_CST);
    uint32_t borrowed=include_borrowed ? __atomic_load_n(&record->borrowed_cpus,__ATOMIC_SEQ_CST) : 0;
    return active | (include_borrowed ? borrowed : 0);
#else
    return record->active_cpus | (include_borrowed ? record->borrowed_cpus : 0);
#endif
}

/* A writable PTE is also readable on both supported architectures. RISC-V
 * Linux records VM_READ too, as VM_WRITE alone denotes a shadow stack. */
static inline uint32_t add_read_to_write(uint32_t permissions)
{
    return (permissions & KERNEL_MM_WRITE) ? permissions | KERNEL_MM_READ : permissions;
}

static inline uint32_t user_effective_permissions(uint32_t permissions)
{
#if defined(BOAROS_ARCH_LOONGARCH)
    return add_read_to_write(permissions);
#else
    return permissions;
#endif
}

static inline uint32_t user_vma_permissions(uint32_t permissions)
{
#if defined(BOAROS_ARCH_LOONGARCH)
    return permissions;
#else
    return add_read_to_write(permissions);
#endif
}

static uint32_t backend_permissions_from_mm(uint32_t permissions);
static enum kernel_mm_status unmap_space_range(struct kernel_mm_record *,uint64_t,uint64_t);
static enum kernel_mm_status unmap_vma_space_range(struct kernel_mm_record *,uint64_t,uint64_t);
static uint32_t note_tlb_change_locked(struct kernel_mm_record *,
    const struct kernel_mm_retirement *, int);
static void finish_tlb_change(struct kernel_mm_record *,
    struct kernel_mm_retirement *, uint32_t, uint64_t, uint64_t, int);
static void invalidate_object_residents(void *,struct kernel_memory_object *,uint64_t,uint64_t);
static void unmap_object_ranges(struct kernel_mm_record *,struct kernel_memory_object *,uint64_t,uint64_t);

static void prepare_shared_registration(struct kernel_mm_shared_anon *entry,struct kernel_mm_record *record)
{
    entry->registration.object=entry->object;
    entry->registration.owner=record;
    entry->registration.invalidate=invalidate_object_residents;
}
static void register_shared_anon(struct kernel_mm_shared_anon *entry,struct kernel_mm_record *record)
{
    prepare_shared_registration(entry,record);
    if (kernel_memory_object_mapping_register(&entry->registration,0)) __builtin_trap();
}

_Static_assert(sizeof(struct kernel_mm_record) <= BOAROS_PAGE_SIZE,
               "RISC-V MM record must fit in one physical page");

static size_t resident_bucket(uint64_t address,size_t capacity);

static uint64_t lazy_next_birth(void)
{
    uint64_t birth=__atomic_add_fetch(&lazy_birth_sequence,1,__ATOMIC_ACQ_REL);
    if (!birth) __builtin_trap();
    return birth;
}

/* The caller holds exclusive MM qualification (or constructs an unpublished
 * child), so other users cannot follow hash links while they are rebuilt. */
static enum kernel_heap_status lazy_reserve(struct kernel_mm_record *record,size_t needed)
{
    if (needed<=record->lazy_capacity) return KERNEL_HEAP_STATUS_OK;
    size_t capacity=record->lazy_capacity?record->lazy_capacity:16;
    while (capacity<needed) {
        if (capacity>SIZE_MAX/2) return KERNEL_HEAP_STATUS_EMPTY;
        capacity*=2;
    }
    struct mm_lazy_page **buckets;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(record->vma_heap,capacity,
        sizeof(*buckets),(void **)&buckets);
    if (status) return status;
    for (struct mm_lazy_page *page=record->lazy_pages;page;page=page->next) {
        size_t bucket=resident_bucket(page->address,capacity);
        page->hash_next=buckets[bucket];buckets[bucket]=page;
    }
    struct mm_lazy_page **old;
    {
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        old=record->lazy_buckets;record->lazy_buckets=buckets;record->lazy_capacity=capacity;
    }
    if (old) kernel_heap_release(record->vma_heap,old);
    return KERNEL_HEAP_STATUS_OK;
}

static void lazy_remove_registration_locked(struct kernel_mm_record *record)
{
    if (!record->lazy_registered) return;
    if (!lazy_registry_count--) __builtin_trap();
    if (record->lazy_next==record) lazy_registry=NULL;
    else {
        record->lazy_next->lazy_previous=record->lazy_previous;
        record->lazy_previous->lazy_next=record->lazy_next;
        if (lazy_registry==record) lazy_registry=record->lazy_next;
    }
    record->lazy_next=record->lazy_previous=NULL;
    record->lazy_registered=0;
}

/* Callers hold the MM raw lock; retiring never frees under that lock. */
static struct mm_lazy_page *lazy_find(struct kernel_mm_record *record, uint64_t address)
{
    if (!record->lazy_count) return NULL;
    for (struct mm_lazy_page *page=record->lazy_buckets[resident_bucket(address,record->lazy_capacity)]; page; page=page->hash_next)
        if (page->address==address) return page;
    return NULL;
}
static void lazy_insert(struct kernel_mm_record *record,struct mm_lazy_page *page)
{
    if (record->lazy_count>=record->lazy_capacity || lazy_find(record,page->address)) __builtin_trap();
    page->birth=lazy_next_birth();
    size_t bucket=resident_bucket(page->address,record->lazy_capacity);
    page->hash_next=record->lazy_buckets[bucket];record->lazy_buckets[bucket]=page;
    page->next=record->lazy_pages;page->previous=&record->lazy_pages;
    if (page->next) page->next->previous=&page->next;
    record->lazy_pages=page;record->lazy_count++;
}
static void lazy_retire(struct kernel_mm_record *record, struct mm_lazy_page *page)
{
    struct mm_lazy_page **link=&record->lazy_buckets[resident_bucket(page->address,record->lazy_capacity)];
    while (*link && *link!=page) link=&(*link)->hash_next;
    if (!*link || !page->previous) __builtin_trap();
    *link=page->hash_next;
    if (record->lazy_scan_cursor && record->lazy_scan_cursor->next==page)
        record->lazy_scan_cursor->next=page->next;
    *page->previous=page->next;
    if (page->next) page->next->previous=page->previous;
    page->next=record->lazy_retired;
    record->lazy_retired=page;
    if (!record->lazy_count--) __builtin_trap();
    if (!record->lazy_count) {
        KERNEL_RAW_SCOPE(guard,&lazy_registry_lock);
        lazy_remove_registration_locked(record);
    }
}
static void lazy_drain(struct kernel_mm_record *record)
{
    struct mm_lazy_page *pages,**buckets=NULL;
    {
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        pages=record->lazy_retired;
        record->lazy_retired=NULL;
        if (!record->lazy_count) {
            buckets=record->lazy_buckets;record->lazy_buckets=NULL;record->lazy_capacity=0;
        }
    }
    while (pages) {
        struct mm_lazy_page *page=pages;
        pages=page->next;
        kernel_heap_release(record->vma_heap,page);
    }
    if (buckets) kernel_heap_release(record->vma_heap,buckets);
}

/* The registry borrows records only while a scan is in progress. Exit removes
 * the record first, then drains these borrows before tearing down its MM. */
static void lazy_register(struct kernel_mm_record *record)
{
    __atomic_store_n(&record->space.allocator->lazy_reclaim,kernel_mm_reclaim_lazy,__ATOMIC_RELEASE);
    KERNEL_RAW_SCOPE(guard,&lazy_registry_lock);
    if (!lazy_scan_initialized) {
        kernel_mutex_init(&lazy_scan_lock,24,(uintptr_t)&lazy_scan_lock);
        lazy_scan_initialized=1;
    }
    if (record->lazy_registered) return;
    if (!lazy_registry) {
        lazy_registry=record;
        record->lazy_next=record->lazy_previous=record;
    } else {
        record->lazy_next=lazy_registry;
        record->lazy_previous=lazy_registry->lazy_previous;
        record->lazy_previous->lazy_next=record;
        lazy_registry->lazy_previous=record;
    }
    record->lazy_registered=1;
    record->lazy_seen_round=0;
    record->lazy_birth=lazy_next_birth();
    if (++lazy_registry_count==0) __builtin_trap();
}
static void lazy_unregister(struct kernel_mm_record *record)
{
    for (;;) {
        struct kernel_wait_token token={0};
        {
            KERNEL_RAW_SCOPE(guard,&lazy_registry_lock);
            lazy_remove_registration_locked(record);
            if (!record->lazy_borrows) return;
            if (kernel_wait_prepare(&record->lazy_drained,0,0,&token)!=KERNEL_SCHEDULER_STATUS_OK)
                __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token,&reason)!=KERNEL_SCHEDULER_STATUS_OK ||
            kernel_wait_finish(&token)!=KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
}
static void lazy_borrow_put(struct kernel_mm_record *record)
{
    KERNEL_RAW_SCOPE(guard,&lazy_registry_lock);
    if (!record->lazy_borrows) __builtin_trap();
    if (!--record->lazy_borrows && !record->lazy_registered) {
        enum kernel_scheduler_status status=kernel_wait_queue_wake_all(&record->lazy_drained);
        if (status!=KERNEL_SCHEDULER_STATUS_OK && status!=KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED)
            __builtin_trap();
    }
}

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
    arch_zero_owned_memory(pointer, BOAROS_PAGE_SIZE);
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
    if (mm->record->anonymous_zero_page != KERNEL_MM_NO_PAGE &&
        physical_page_release(mm->allocator, mm->record->anonymous_zero_page))
        __builtin_trap();
    if (__atomic_fetch_sub(&mm->record->record_references, 1U, __ATOMIC_ACQ_REL) == 1U)
        (void)physical_page_release(mm->allocator, mm->record_page_address);
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
    record->instruction_generation = 1;
    record->anonymous_zero_page = KERNEL_MM_NO_PAGE;
    record->futex_id = reserve_mm_identity();
    if (!record->futex_id) return abandon_unresolved_record(mm, allocator,
        record_page_address, KERNEL_MM_STATUS_NO_MEMORY);
    record->references = 1U;
    record->record_references = 1U;
    record->coredump_filter = 0x33U;
    kernel_raw_rwlock_init(&record->metadata, KERNEL_RAW_RANK_MM);
    kernel_rwlock_init(&record->operations, 25, (uintptr_t)record);
    kernel_rwlock_init(&record->vma_lock, 31, (uintptr_t)record);
    kernel_wait_queue_init(&record->range_changed);
    record->range_claims = 0;
    kernel_wait_queue_init(&record->pins_changed);
    kernel_wait_queue_init(&record->lazy_drained);
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

static size_t file_resident_bucket(uint64_t address, unsigned shift)
{
    uint64_t page = address >> BOAROS_PAGE_SHIFT;
    return (size_t)((page * UINT64_C(0x9e3779b97f4a7c15)) >> shift);
}

/* Allocate before publishing PTEs/aliases. Rehash itself cannot sleep/fail. */
static enum kernel_mm_status reserve_file_residents(
    struct kernel_mm_record *record, size_t needed)
{
    if (needed <= record->resident_capacity) return KERNEL_MM_STATUS_OK;
    size_t capacity = record->resident_capacity ? record->resident_capacity : 16;
    unsigned shift = record->resident_capacity ? record->resident_bucket_shift : 60;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) return KERNEL_MM_STATUS_NO_MEMORY;
        capacity *= 2;
        shift--;
    }
    struct mm_file_resident **buckets;
    enum kernel_heap_status hs = kernel_heap_allocate_zeroed(record->vma_heap,
        capacity, sizeof(*buckets), (void **)&buckets);
    if (hs != KERNEL_HEAP_STATUS_OK)
        return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                               : KERNEL_MM_STATUS_STATE;
    struct mm_file_resident **old;
    {
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
    for (struct mm_file_resident *page = record->file_residents;
         page != 0; page = page->next) {
        size_t index = file_resident_bucket(page->address, shift);
        page->hash_next = buckets[index];
        buckets[index] = page;
    }
    old = record->resident_buckets;
    record->resident_buckets = buckets;
    record->resident_capacity = capacity;
    record->resident_bucket_shift = shift;
    }
    if (old) kernel_heap_release(record->vma_heap, old);
    return KERNEL_MM_STATUS_OK;
}

static void publish_file_resident(struct kernel_mm_record *record,
                                   struct mm_file_resident *page)
{
    struct mm_file_alias_resident *shared = file_alias_resident(page);
    if (shared) shared->record = record;
    if (record->resident_count >= record->resident_capacity) __builtin_trap();
    size_t index = file_resident_bucket(page->address, record->resident_bucket_shift);
#if BOAROS_LOCK_DEBUG
    /* Normal publication follows successful installation in an empty PTE,
     * a move from the old key, or cloning a unique source address. */
    for (struct mm_file_resident *old = record->resident_buckets[index];
         old != 0; old = old->hash_next)
        if (old->address == page->address) __builtin_trap();
#endif
    page->hash_next = record->resident_buckets[index];
    record->resident_buckets[index] = page;
    record->resident_count++;
    page->next = record->file_residents;
    page->previous = &record->file_residents;
    if (page->next) page->next->previous = &page->next;
    record->file_residents = page;
}

#define MM_CACHE_RETRY_QUEUED UINT64_C(1)
#define MM_CACHE_GATE_STEP UINT64_C(2)

/* Gate release and retry registration share a single atomic word. Only a
 * release that observes registered work needs the short metadata guard. */
static void mm_gate_release(struct kernel_mm_record *record,struct kernel_lock_guard *guard)
{
    if (!guard->lock) return;
    int indexed = record->resident_capacity != 0;
    kernel_lock_release(guard);
    if (!indexed) return;
    uint64_t progress = __atomic_fetch_add(&record->cache_gate_progress,
        MM_CACHE_GATE_STEP, __ATOMIC_ACQ_REL);
    if (progress > UINT64_MAX - MM_CACHE_GATE_STEP) __builtin_trap();
    if (!(progress & MM_CACHE_RETRY_QUEUED)) return;
    struct mm_file_alias_resident *pending;
    {
        KERNEL_RAW_WRITE_SCOPE(metadata, &record->metadata);
        pending=record->cache_retry_head; record->cache_retry_head=0;
        __atomic_fetch_and(&record->cache_gate_progress, ~MM_CACHE_RETRY_QUEUED,
            __ATOMIC_RELAXED);
    }
    while (pending) {
        struct mm_file_alias_resident *page=pending;
        {
            KERNEL_RAW_WRITE_SCOPE(metadata, &record->metadata);
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
static void defer_cache_rearm(struct mm_file_alias_resident *page,uint64_t generation)
{
    struct kernel_mm_record *record=page->record;
    kernel_page_cache_alias_hold(&page->alias);
    int queued=0;
    {
        KERNEL_RAW_WRITE_SCOPE(metadata, &record->metadata);
        uint64_t expected = __atomic_load_n(&record->cache_gate_progress, __ATOMIC_RELAXED);
        if (record->stage==KERNEL_MM_RECORD_LIVE && kernel_page_cache_alias_active(&page->alias) &&
            generation==(expected & ~MM_CACHE_RETRY_QUEUED) && !page->cache_retry_registered &&
            __atomic_compare_exchange_n(&record->cache_gate_progress, &expected,
                expected | MM_CACHE_RETRY_QUEUED, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
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

static void retire_file_residents(struct kernel_mm_record *record,
    struct mm_file_resident **pages, unsigned count)
{
    struct kernel_page_cache_alias *aliases[KERNEL_MM_RETIRE_CAPACITY];
    unsigned alias_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        struct mm_file_alias_resident *shared = file_alias_resident(pages[i]);
        if (shared && kernel_page_cache_alias_active(&shared->alias))
            aliases[alias_count++] = &shared->alias;
    }
    kernel_page_cache_alias_detach_batch(aliases, alias_count);
    void *released[KERNEL_MM_RETIRE_CAPACITY];
    unsigned release_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        struct mm_file_resident *page = pages[i];
        struct mm_file_alias_resident *shared = file_alias_resident(page);
        if (shared && shared->alias.entry) {
            page->next = record->retired_file_residents;
            record->retired_file_residents = page;
        } else released[release_count++] = page;
    }
    kernel_heap_release_small_batch(record->vma_heap, released, release_count);
}
static void drain_retired_residents(struct kernel_mm_record *record,struct mm_file_resident *pages)
{
    while (pages) {
        struct mm_file_resident *page=pages; pages=page->next;
        struct mm_file_alias_resident *shared = file_alias_resident(page);
        kernel_page_cache_alias_drain(&shared->alias);
        kernel_heap_release(record->vma_heap,page);
    }
}

static void unlink_file_resident(struct kernel_mm_record *record,
    struct mm_file_resident **link)
{
    struct mm_file_resident *page = *link;
    *link = page->hash_next;
    record->resident_count--;
    *page->previous = page->next;
    if (page->next) page->next->previous = page->previous;
    page->previous = 0;
}

static void unindex_file_resident(struct kernel_mm_record *record,
    struct mm_file_resident *page)
{
    size_t index = file_resident_bucket(page->address, record->resident_bucket_shift);
    struct mm_file_resident **link = &record->resident_buckets[index];
    while (*link != page) {
        if (!*link) __builtin_trap();
        link = &(*link)->hash_next;
    }
    unlink_file_resident(record, link);
}

static void forget_file_residents(struct kernel_mm_record *record)
{
    while (record->file_residents) {
        struct mm_file_resident *detached[KERNEL_MM_RETIRE_CAPACITY];
        unsigned count = 0;
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            while (record->file_residents && count < KERNEL_MM_RETIRE_CAPACITY) {
                struct mm_file_resident *page = record->file_residents;
                unindex_file_resident(record, page);
                detached[count++] = page;
            }
        }
        /* Cache detachment and any final reference release stay outside MM
         * metadata. Pending alias borrowers still own their callbacks. */
        retire_file_residents(record, detached, count);
    }
}

static int rearm_shared_file_alias(void *owner, uint64_t address)
{
    struct mm_file_alias_resident *page=owner;
    struct kernel_mm_record *record = page->record;
    struct mm_gate_scope operation __attribute__((cleanup(mm_gate_scope_end)))={.record=record};
    if (!kernel_lock_held(&record->operations,1)) {
        int background=kernel_io_context_current()->background_reclaim;
        uint64_t generation = __atomic_load_n(&record->cache_gate_progress, __ATOMIC_ACQUIRE) &
            ~MM_CACHE_RETRY_QUEUED;
        if (background || kernel_io_context_current()->user_pins) {
            if (!kernel_rwlock_try_write(&record->operations,&operation.guard)) {
                if (background) defer_cache_rearm(page,generation);
                return -KERNEL_EBUSY;
            }
        } else kernel_rwlock_write(&record->operations,&operation.guard);
    }
    /* 回调可能在mremap之前借出；gate内由resident owner读取当前地址。 */
    address = page->base.address;
    struct kernel_vma vma;
    uint32_t permissions;
    if (record->stage!=KERNEL_MM_RECORD_LIVE || !kernel_page_cache_alias_active(&page->alias) ||
        kernel_vma_set_lookup(record->vmas, address, &vma) !=
            KERNEL_VMA_STATUS_OK ||
        vma.kind != KERNEL_VMA_KIND_FILE_SHARED) return 0;
    permissions = user_effective_permissions(vma.permissions) & ~KERNEL_MM_WRITE;
    struct kernel_mm_retirement batch={0}; uint32_t targets=0;
    {
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        struct arch_mmu_mapping mapping;
        if (arch_mmu_user_lookup(&record->space,address,&mapping)!=ARCH_MMU_STATUS_OK ||
            (mapping.physical_address&~BOAROS_PAGE_MASK)!=page->base.physical_address) return 0;
        if (!(mapping.permissions&ARCH_MMU_WRITE)) return 0;
        if (arch_mmu_user_protect_owned_page(&record->space,address,
                backend_permissions_from_mm(permissions))) __builtin_trap();
        targets=note_tlb_change_locked(record, &batch, 0);
    }
    finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,0);
    return 0;
}

static struct mm_file_resident **find_file_resident_link(
    struct kernel_mm_record *record, uint64_t address)
{
    if (!record->resident_count) return 0;
    size_t index = file_resident_bucket(address, record->resident_bucket_shift);
    for (struct mm_file_resident **link = &record->resident_buckets[index];
         *link != 0; link = &(*link)->hash_next) {
#if BOAROS_COST_DIAGNOSTICS
        __atomic_fetch_add(&record->resident_probes, 1U, __ATOMIC_RELAXED);
#endif
        if ((*link)->address == address) return link;
    }
    return 0;
}

static struct mm_file_resident *find_file_resident(
    struct kernel_mm_record *record, uint64_t address)
{
    struct mm_file_resident **link = find_file_resident_link(record, address);
    return link ? *link : 0;
}

static void truncate_file_residents(void *owner, struct kernel_vfs_node *node,
                                    uint64_t size)
{
    struct kernel_mm_record *record = owner;
    struct mm_file_resident **link = &record->file_residents;
    uint64_t last_page = size & ~BOAROS_PAGE_MASK;
    int cleared = 0;
    KERNEL_LOCK_SCOPE(operation);
    kernel_rwlock_write(&record->operations,&operation);
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
        { KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
          mm_generation_advance(&record->instruction_generation);
          targets = mm_sync_targets(record,0); }
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
    kernel_rwlock_write(&record->operations,&operation);
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
    enum kernel_open_file_kind kind=kernel_open_file_kind(file);
    if (kind==KERNEL_OPEN_FILE_KIND_BPF || kind==KERNEL_OPEN_FILE_KIND_SOCKET)
        return KERNEL_MM_STATUS_OK;
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
            if (kernel_open_file_node(source->file) == entry->registration.node) {
                used = 1;
                break;
            }
        }
        if (used) {
            link = &entry->next;
        } else {
            *link = entry->next;
            if (record->stage == KERNEL_MM_RECORD_LIVE &&
                kernel_lock_held(&record->operations,1)) {
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

/* Keep busy retired nodes published while holding MM write qualification.
 * Their last borrower may return without that qualification (self-pin try),
 * so a later operation/user return must still find the cleanup obligation. */
static struct kernel_mm_file_source *take_ready_file_sources(struct kernel_mm_record *record)
{
    struct kernel_mm_file_source *pending=record->retired_file_sources;
    struct kernel_mm_file_source *ready=NULL,*busy=NULL;
    while (pending) {
        struct kernel_mm_file_source *source=pending; pending=source->next;
        if (__atomic_load_n(&source->operations,__ATOMIC_ACQUIRE)) {
            if (record->stage!=KERNEL_MM_RECORD_LIVE) __builtin_trap();
            source->next=busy; busy=source;
        } else { source->next=ready; ready=source; }
    }
    record->retired_file_sources=busy;
    return ready;
}

/* Detached, idle nodes keep their real OFD owner through callback drain and
 * close. A failed close returns the same node to the existing retry path. */
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
        kernel_rwlock_write(&record->operations,&guard);
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
    if (unused_only && !record->vmas) return KERNEL_MM_STATUS_STATE;
    int defer = kernel_lock_held(&record->operations, 1);

    while (*link != 0) {
        struct kernel_mm_file_source *source = *link;
        int in_use = 0;

        if (!unused_only && source->operations) __builtin_trap();
        if (source->file != 0) {
            if (unused_only && kernel_vma_set_backing_in_use(record->vmas,
                    source->file, &in_use) != KERNEL_VMA_STATUS_OK)
                __builtin_trap();
            if (in_use != 0) {
                link = &source->next;
                continue;
            }
        }
        *link = source->next;
        source->draining=1;
        source->next=record->retired_file_sources; record->retired_file_sources=source;
    }
    /* Active sources are now exactly those with VMAs. The retired chain
     * still owns its OFDs until registration callbacks have stopped. */
    drain_file_registrations(record, unused_only);
    int failed=release_file_sources(record,defer ? NULL : take_ready_file_sources(record));
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
        kernel_rwlock_write(&record->operations,&guard);
    while (failed) {
        struct kernel_mm_elf_source *source=failed; failed=source->next;
        source->next=record->retired_elf_sources; record->retired_elf_sources=source;
    }
    return 1;
}

static struct kernel_mm_elf_source *take_ready_elf_sources(struct kernel_mm_record *record)
{
    struct kernel_mm_elf_source *pending=record->retired_elf_sources;
    struct kernel_mm_elf_source *ready=NULL,*busy=NULL;
    while (pending) {
        struct kernel_mm_elf_source *source=pending; pending=source->next;
        if (__atomic_load_n(&source->faults,__ATOMIC_ACQUIRE)) {
            if (record->stage!=KERNEL_MM_RECORD_LIVE) __builtin_trap();
            source->next=busy; busy=source;
        } else { source->next=ready; ready=source; }
    }
    record->retired_elf_sources=busy;
    return ready;
}

static enum kernel_mm_status drain_elf_sources(
    struct kernel_mm_record *record,
    int unused_only)
{
    struct kernel_mm_elf_source **link = &record->elf_sources;
    int invalid=0,defer=kernel_lock_held(&record->operations,1);

    while (*link != 0) {
        struct kernel_mm_elf_source *entry = *link;
        int in_use = 0;

        if (!unused_only && entry->faults) __builtin_trap();
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
        entry->next=record->retired_elf_sources; record->retired_elf_sources=entry;
    }
    int failed=release_elf_sources(record,defer ? NULL : take_ready_elf_sources(record));
    if (invalid) return KERNEL_MM_STATUS_STATE;
    return failed != 0 ? KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       : KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status clone_elf_source(struct kernel_mm_record *child,
    struct kernel_mm_elf_source *parent)
{
    struct kernel_mm_elf_source *copy;
    enum kernel_heap_status hs = kernel_heap_allocate_zeroed(child->vma_heap, 1U,
        sizeof(*copy), (void **)&copy);
    if (hs) return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                                 : KERNEL_MM_STATUS_STATE;
    if (kernel_elf64_source_acquire(parent->source)) {
        kernel_heap_release(child->vma_heap, copy);
        return KERNEL_MM_STATUS_STATE;
    }
    copy->source = parent->source;
    copy->next = child->elf_sources;
    child->elf_sources = copy;
    copy->fork_peer = parent;
    parent->fork_peer = copy;
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status clone_file_source(struct kernel_mm_record *child,
    struct kernel_mm_file_source *parent)
{
    struct kernel_mm_file_source *copy;
    enum kernel_heap_status hs = kernel_heap_allocate_zeroed(child->vma_heap, 1U,
        sizeof(*copy), (void **)&copy);
    if (hs) return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                                 : KERNEL_MM_STATUS_STATE;
    if (kernel_open_file_acquire(parent->file)) {
        kernel_heap_release(child->vma_heap, copy);
        return KERNEL_MM_STATUS_STATE;
    }
    copy->file = parent->file;
    copy->next = child->file_sources;
    child->file_sources = copy;
    copy->fork_peer = parent;
    parent->fork_peer = copy;
    return KERNEL_MM_STATUS_OK;
}

/* The parent MM writer keeps each source live while the VMA copy loop
 * memoizes its child source and replaces the borrowed parent pointer. */
static enum kernel_vma_status rebind_fork_source(struct kernel_vma *entry,void *context)
{
    struct kernel_mm_record *child=context;
    enum kernel_mm_status status = KERNEL_MM_STATUS_OK;
    if (entry->kind==KERNEL_VMA_KIND_ELF_PRIVATE) {
        struct kernel_mm_elf_source *source=entry->backing_owner;
        if (!source->fork_peer) status=clone_elf_source(child,source);
        if (!status) entry->backing_owner=source->fork_peer;
    } else {
        struct kernel_mm_file_source *source=entry->backing_owner;
        if (!source->fork_peer) status=clone_file_source(child,source);
        if (!status) entry->backing_owner=source->fork_peer;
    }
    return !status ? KERNEL_VMA_STATUS_OK : status==KERNEL_MM_STATUS_NO_MEMORY ?
        KERNEL_VMA_STATUS_NO_MEMORY : KERNEL_VMA_STATUS_STATE;
}

/* Clear temporary links on either outcome before releasing the parent gate
 * or reclaiming an unpublished child. */
static void finish_fork_sources(struct kernel_mm_record *child)
{
    for (struct kernel_mm_file_source *copy = child->file_sources; copy; copy = copy->next) {
        copy->fork_peer->fork_peer = NULL;
        copy->fork_peer = NULL;
    }
    for (struct kernel_mm_elf_source *copy = child->elf_sources; copy; copy = copy->next) {
        copy->fork_peer->fork_peer = NULL;
        copy->fork_peer = NULL;
    }
}

static void prepare_huge_registration(struct mm_huge_map *);
static int huge_vma(const struct kernel_vma *v)
{ return v->kind==KERNEL_VMA_KIND_HUGETLB_PRIVATE || v->kind==KERNEL_VMA_KIND_HUGETLB_SHARED; }
static enum kernel_mm_status huge_status(enum kernel_hugetlb_status status)
{
    switch(status) {
    case KERNEL_HUGETLB_OK: return KERNEL_MM_STATUS_OK;
    case KERNEL_HUGETLB_NO_MEMORY: return KERNEL_MM_STATUS_NO_MEMORY;
    case KERNEL_HUGETLB_BUS: return KERNEL_MM_STATUS_BUS_FAULT;
    case KERNEL_HUGETLB_BUSY: return KERNEL_MM_STATUS_CONFLICT;
    default: return KERNEL_MM_STATUS_STATE;
    }
}
static void drain_huge_maps(struct kernel_mm_record *record,int unused_only)
{
    struct mm_huge_map **link=&record->huge_maps;
    while (*link) {
        struct mm_huge_map *entry=*link;
        uint64_t cursor=0; int used=0;
        if (unused_only && record->vmas) {
            for (uint32_t i=0;i<kernel_vma_set_count(record->vmas);i++) {
                struct kernel_vma v;
                if (kernel_vma_set_get_at(record->vmas,i,&v)) __builtin_trap();
                if (v.backing!=entry->map) continue;
                used=1;
                if (v.kind == KERNEL_VMA_KIND_HUGETLB_SHARED) break;
                uint64_t first=v.backing_offset/ARCH_MMU_HUGE_SIZE;
                uint64_t end=first+(v.end-v.start)/ARCH_MMU_HUGE_SIZE;
                if (first<cursor || end>entry->pages) __builtin_trap();
                if (kernel_hugetlb_map_unmap(entry->map,cursor,first)) __builtin_trap();
                cursor=end;
            }
        }
        if (kernel_hugetlb_map_unmap(entry->map,cursor,entry->pages)) __builtin_trap();
        if (used) { link=&entry->next; continue; }
        *link=entry->next;
        kernel_hugetlb_mapping_stop(&entry->registration);
        if (record->stage==KERNEL_MM_RECORD_LIVE && kernel_lock_held(&record->operations,1)) {
            entry->next=record->retired_huge_maps;record->retired_huge_maps=entry;
        } else {
            kernel_hugetlb_mapping_drain(&entry->registration);
            kernel_hugetlb_map_release(&entry->map);
            kernel_heap_release(record->vma_heap,entry);
        }
    }
}
static enum kernel_mm_status clone_huge_maps(const struct kernel_mm_record *source,
    struct kernel_mm_record *child,uint64_t record_address)
{
    for (struct mm_huge_map *entry=source->huge_maps;entry;entry=entry->next) {
        int used;
        if (kernel_vma_set_backing_in_use(child->vmas,entry->map,&used)) __builtin_trap();
        if (!used) continue;
        struct mm_huge_map *copy;
        if (kernel_heap_allocate_zeroed(child->vma_heap,1,sizeof(*copy),(void **)&copy))
            return KERNEL_MM_STATUS_NO_MEMORY;
        enum kernel_hugetlb_status hs=kernel_hugetlb_map_fork(entry->map,&copy->map);
        if (hs) { kernel_heap_release(child->vma_heap,copy); return huge_status(hs); }
        copy->pages=entry->pages;
        copy->mm=(struct kernel_mm){.allocator=source->space.allocator,.record=child,
            .record_page_address=record_address,.state=KERNEL_MM_LIVE};
        if (copy->map!=entry->map && kernel_vma_set_rebind_backing(child->vmas,entry->map,copy->map))
            __builtin_trap();
        prepare_huge_registration(copy);
        if (kernel_hugetlb_map_epoch(entry->map,&copy->fork_epoch)) {
            kernel_hugetlb_map_release(&copy->map);kernel_heap_release(child->vma_heap,copy);
            return KERNEL_MM_STATUS_CONFLICT;
        }
        copy->next=child->huge_maps;child->huge_maps=copy;
    }
    return KERNEL_MM_STATUS_OK;
}

static void drain_shared_anon(struct kernel_mm_record *record,
                              int unused_only)
{
    drain_huge_maps(record,unused_only);
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
        kernel_memory_object_mapping_stop(&entry->registration);
        if (record->stage==KERNEL_MM_RECORD_LIVE && kernel_lock_held(&record->operations,1)) {
            entry->next=record->retired_shared_anon;record->retired_shared_anon=entry;
        } else {
            kernel_memory_object_mapping_drain(&entry->registration);
            kernel_memory_object_release(&entry->object);
            kernel_heap_release(record->vma_heap,entry);
        }
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
        int used = 0;
        if (kernel_vma_set_backing_in_use(destination_record->vmas, source->object, &used))
            __builtin_trap();
        if (!used) continue;

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
        prepare_shared_registration(copy,destination_record);
        (void)kernel_memory_object_epoch(copy->object,&copy->fork_epoch);
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
           user_vma_permissions(permissions) == permissions;
}

static enum kernel_mm_status mutable_vma_record(
    struct kernel_mm *mm,
    struct kernel_mm_record **record);

/* Fork holds the parent's MM edit qualification throughout both page-table
 * passes. Adjacent leaves can use the same VMA decision until its end. */
struct fork_policy_cursor {
    const struct kernel_vma_set *vmas;
    uint64_t start, end;
    int action;
};

static int fork_vma_action(const struct kernel_vma *vma)
{
    if (vma->flags & (KERNEL_VMA_DONTFORK | KERNEL_VMA_WIPEONFORK))
        return ARCH_MMU_FORK_SKIP;
    /* A private file VMA first acquires anonymous history before a write
     * (including a debugger write) can publish private bytes. Until then
     * the child's inherited OFD can fault the clean contents back in. */
    if (vma->kind == KERNEL_VMA_KIND_FILE_PRIVATE &&
        vma->fault_policy == KERNEL_VMA_FAULT_FILE_PRIVATE &&
        !(vma->flags & KERNEL_VMA_ANON_HISTORY))
        return ARCH_MMU_FORK_SKIP;
    if (vma->role == KERNEL_VMA_ROLE_VVAR ||
        vma->kind == KERNEL_VMA_KIND_HUGETLB_SHARED ||
        vma->kind == KERNEL_VMA_KIND_ANON_SHARED ||
        vma->kind == KERNEL_VMA_KIND_FILE_SHARED ||
        vma->kind == KERNEL_VMA_KIND_SYSV_SHM ||
        vma->kind == KERNEL_VMA_KIND_OBJECT_SHARED)
        return ARCH_MMU_FORK_SHARED;
    return ARCH_MMU_FORK_PRIVATE;
}

static int fork_leaf_action(void *context, uint64_t address)
{
    struct fork_policy_cursor *cursor = context;
    if (address >= cursor->start && address < cursor->end)
        return cursor->action;
    struct kernel_vma vma;
    enum kernel_vma_status status = kernel_vma_set_lookup(cursor->vmas, address, &vma);
    if (status == KERNEL_VMA_STATUS_NOT_FOUND) return ARCH_MMU_FORK_PRIVATE;
    if (status != KERNEL_VMA_STATUS_OK) __builtin_trap();
    cursor->start = vma.start;
    cursor->end = vma.end;
    cursor->action = fork_vma_action(&vma);
    return cursor->action;
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
    struct kernel_mm_record *child, struct fork_file_guards *owner)
{
    if (!child->file_mappings) return KERNEL_MM_STATUS_OK;
    if (!kernel_lock_held(&record->operations,1)) __builtin_trap();
    size_t count=0, bytes=sizeof(*owner->nodes)+sizeof(*owner->guards);
    for (struct mm_file_mapping *m=child->file_mappings; m; m=m->next) {
        if (++count > SIZE_MAX/bytes) return KERNEL_MM_STATUS_NO_MEMORY;
    }
    enum kernel_heap_status hs=kernel_heap_allocate_zeroed(record->vma_heap,count,bytes,(void **)&owner->nodes);
    if (hs != KERNEL_HEAP_STATUS_OK)
        return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
    owner->heap=record->vma_heap; owner->count=count;
    owner->guards=(void *)(owner->nodes+count);
    size_t index=0;
    for (struct mm_file_mapping *m=child->file_mappings; m; m=m->next)
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
    destination_record->instruction_generation = 1;
    destination_record->anonymous_zero_page = KERNEL_MM_NO_PAGE;
    destination_record->futex_id = reserve_mm_identity();
    if (!destination_record->futex_id) return abandon_unresolved_record(destination,
        source->allocator, record_page_address, KERNEL_MM_STATUS_NO_MEMORY);
    destination_record->references = 1U;
    destination_record->record_references = 1U;
    destination_record->coredump_filter = kernel_mm_coredump_filter_get(source);
    destination_record->membarrier_registrations =
        __atomic_load_n(&source_record->membarrier_registrations, __ATOMIC_ACQUIRE);
    kernel_raw_rwlock_init(&destination_record->metadata, KERNEL_RAW_RANK_MM);
    kernel_rwlock_init(&destination_record->operations, 25, (uintptr_t)destination_record);
    kernel_rwlock_init(&destination_record->vma_lock, 31, (uintptr_t)destination_record);
    kernel_wait_queue_init(&destination_record->range_changed);
    destination_record->range_claims = 0;
    kernel_wait_queue_init(&destination_record->pins_changed);
    kernel_wait_queue_init(&destination_record->lazy_drained);
    destination_record->stage = KERNEL_MM_RECORD_LIVE;
    destination_record->vma_heap = source_record->vma_heap;

    /*
     * Clone allocation-bearing metadata before sharing leaves.  The Sv39
     * fork commit removes write permission from the parent and therefore is
     * intentionally the final fallible construction step.
     */
    if (source_record->vmas != 0) {
        status = status_from_vma(kernel_vma_set_fork(
            source_record->vmas,
            &destination_record->vmas,rebind_fork_source,destination_record));
        finish_fork_sources(destination_record);
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
    status = clone_huge_maps(source_record,destination_record,record_page_address);
    if (status==KERNEL_MM_STATUS_OK) status = clone_shared_anon(source_record, destination_record);
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
    struct fork_policy_cursor policy = {.vmas = source_record->vmas};
    size_t inherited_residents = 0;
    struct mm_file_resident *residents = NULL;
    struct mm_file_resident **tail = &residents;
    /* Classify each resident once. The copies have no alias or PTE to
     * retire yet, so allocation failure can release the staging list. */
    for (struct mm_file_resident *page = source_record->file_residents;
         page != 0 && status == KERNEL_MM_STATUS_OK; page = page->next) {
        if (source_record->vmas && fork_leaf_action(&policy, page->address) == ARCH_MMU_FORK_SKIP)
            continue;
        struct mm_file_resident *copy;
        size_t size = page->with_alias ? sizeof(struct mm_file_alias_resident) : sizeof(*copy);
        enum kernel_heap_status hs = kernel_heap_allocate_zeroed(
            destination_record->vma_heap, 1U, size, (void **)&copy);
        if (hs != KERNEL_HEAP_STATUS_OK) {
            status = hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                                     : KERNEL_MM_STATUS_STATE;
            break;
        }
        *copy = *page;
        if (copy->with_alias)
            file_alias_resident(copy)->clone_source = file_alias_resident(page);
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
        inherited_residents++;
    }
    if (status == KERNEL_MM_STATUS_OK)
        status = reserve_file_residents(destination_record, inherited_residents);
    while (residents) {
        struct mm_file_resident *copy = residents;
        residents = copy->next;
        /* The child record is still private to this fork. */
        if (status == KERNEL_MM_STATUS_OK)
            publish_file_resident(destination_record, copy);
        else
            kernel_heap_release(destination_record->vma_heap, copy);
    }
    for (struct mm_lazy_page *page=source_record->lazy_pages;
         page && status==KERNEL_MM_STATUS_OK;page=page->next) {
        if (source_record->vmas && fork_leaf_action(&policy,page->address)==ARCH_MMU_FORK_SKIP)
            continue;
        struct mm_lazy_page *copy;
        enum kernel_heap_status hs=lazy_reserve(destination_record,destination_record->lazy_count+1);
        if (!hs) hs=kernel_heap_allocate_zeroed(destination_record->vma_heap,1,sizeof(*copy),(void **)&copy);
        if (hs) { status=hs==KERNEL_HEAP_STATUS_EMPTY?KERNEL_MM_STATUS_NO_MEMORY:KERNEL_MM_STATUS_STATE;break; }
        copy->address=page->address;copy->physical=page->physical;
        lazy_insert(destination_record,copy);
    }
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

    status=fork_file_guards_begin(source_record,destination_record,&files);
    if (status != KERNEL_MM_STATUS_OK) {
        destination_record->stage=KERNEL_MM_RECORD_VMAS_CLEANUP;
        destination->allocator=source->allocator;
        destination->record=destination_record;
        destination->record_page_address=record_page_address;
        destination->state=KERNEL_MM_CLEANUP;
        destination->cleanup_stage=KERNEL_MM_CLEANUP_VMAS;
        if (kernel_mm_release(destination) != KERNEL_MM_STATUS_OK)
            return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
        finish_handle(destination,KERNEL_MM_EMPTY);
        return status;
    }
    backend_status = arch_mmu_user_space_fork_prepare(&destination_record->space,
        &source_record->space, source_record->vmas ? fork_leaf_action : 0, &policy);
    if (backend_status == ARCH_MMU_STATUS_OK) {
        uint64_t cursor = 0, address;
        int changed = 0;
        /* Search sparse holes in the private child tree outside the parent
         * lock, then update a bounded window from both leaf tables. */
        while ((backend_status = arch_mmu_user_next_owned(&destination_record->space,
            &cursor, ARCH_MMU_USER_LIMIT, &address)) == ARCH_MMU_STATUS_OK) {
            KERNEL_RAW_WRITE_SCOPE(guard, &source_record->metadata);
            if (arch_mmu_user_fork_commit_window(&source_record->space,
                &destination_record->space, address, &cursor,
                source_record->vmas ? fork_leaf_action : NULL, &policy, &changed))
                __builtin_trap();
        }
        if (backend_status != ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
        if (changed) {
            struct kernel_mm_retirement batch = {0};
            uint32_t targets;
            {
                KERNEL_RAW_WRITE_SCOPE(guard, &source_record->metadata);
                targets = note_tlb_change_locked(source_record, &batch, 0);
            }
            finish_tlb_change(source_record, &batch, targets, 0, UINT64_MAX, 0);
        }
        backend_status = ARCH_MMU_STATUS_OK;
    }
    if (backend_status == ARCH_MMU_STATUS_OK) {
        for (struct mm_file_resident *page = destination_record->file_residents;
             page != 0; page = page->next) {
            struct mm_file_alias_resident *shared = file_alias_resident(page);
            if (!shared) continue;
            if (shared->clone_source && kernel_page_cache_alias_active(&shared->clone_source->alias)) {
                /* 新别名不在已捕获的快照游标中，发布前必须保持只读。 */
                {
                    struct kernel_vma vma;
                    if (kernel_vma_set_lookup(destination_record->vmas,page->address,&vma) ||
                        arch_mmu_user_protect_owned_page(&destination_record->space,page->address,
                            backend_permissions_from_mm(user_effective_permissions(vma.permissions) &
                                ~KERNEL_MM_WRITE))) __builtin_trap();
                }
                kernel_page_cache_alias_clone(&shared->alias, &shared->clone_source->alias, shared);
            }
            shared->clone_source = 0;
        }
        destination_record->start_code = source_record->start_code;
        destination_record->end_code = source_record->end_code;
        destination_record->start_stack = source_record->start_stack;
        destination_record->start_data = source_record->start_data;
        destination_record->end_data = source_record->end_data;
        destination_record->start_brk = source_record->start_brk;
        destination_record->current_brk = source_record->current_brk;
        destination_record->brk_limit = source_record->brk_limit;
        destination_record->mmap_base = source_record->mmap_base;
        destination_record->vdso_address = source_record->vdso_address;
#if KERNEL_HAS_VDSO_ELF
        destination_record->vvar_page = source_record->vvar_page;
        if (destination_record->vdso_address) kernel_time_vvar_retain(destination_record->vvar_page);
#endif
        destination_record->brk_initialized =
            source_record->brk_initialized;
        for (struct mm_huge_map *entry=destination_record->huge_maps;entry;entry=entry->next) {
            if (kernel_hugetlb_mapping_register(&entry->registration,entry->fork_epoch)) {
                backend_status=ARCH_MMU_STATUS_CONFLICT;
                goto fork_failed;
            }
        }
        for (struct kernel_mm_shared_anon *entry=destination_record->shared_anon;entry;entry=entry->next) {
            if (kernel_memory_object_mapping_register(&entry->registration,entry->fork_epoch)) {
                /* 已关闭的对象不能接纳刚复制的旧叶；child尚未发布，撤掉它
                 * 的共享叶后再登记，首访沿当前对象重新缺页。 */
                unmap_object_ranges(destination_record,entry->object,0,UINT64_MAX);
                if (kernel_memory_object_mapping_register(&entry->registration,0)) __builtin_trap();
            }
        }
        /* 可失败准备和父失效已完成。inode读保护阻止关闭新登记，回调只会
         * 看到完整的child来源/VMA/页表/驻留记录，不存在半构造发布。 */
        for (struct mm_file_mapping *entry=destination_record->file_mappings; entry; entry=entry->next)
            if (kernel_file_mapping_register(&entry->registration)) __builtin_trap();
        destination->allocator = source->allocator;
        destination->record = destination_record;
        destination->record_page_address = record_page_address;
        destination->state = KERNEL_MM_LIVE;
        destination->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
        if (destination_record->lazy_pages) lazy_register(destination_record);
        return KERNEL_MM_STATUS_OK;
    }

fork_failed:
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

_Static_assert(ARCH_MMU_READ == KERNEL_MM_READ && ARCH_MMU_WRITE == KERNEL_MM_WRITE &&
    ARCH_MMU_EXECUTE == KERNEL_MM_EXECUTE && ARCH_MMU_USER == KERNEL_MM_USER,
    "leaf permissions use the MM bit layout");

static enum kernel_mm_status lookup_record_mode(
    const struct kernel_mm_record *record,
    uint64_t virtual_address,
    struct kernel_mm_mapping *mapping, int force)
{
    struct kernel_mm_leaf riscv_mapping;
    enum arch_mmu_status backend_status;
    backend_status = arch_mmu_user_leaf_snapshot(&record->space,
                                         virtual_address,
                                         &riscv_mapping);
    if (backend_status == ARCH_MMU_STATUS_NOT_MAPPED ||
        (backend_status == ARCH_MMU_STATUS_OK && !riscv_mapping.permissions && !force)) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    if (backend_status != ARCH_MMU_STATUS_OK) {
        return backend_status == ARCH_MMU_STATUS_INVALID
                   ? KERNEL_MM_STATUS_INVALID_ARGUMENT
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    mapping->physical_address = riscv_mapping.physical_address + virtual_address - riscv_mapping.virtual_address;
    mapping->order = riscv_mapping.order;
    mapping->permissions = riscv_mapping.permissions ? riscv_mapping.permissions : KERNEL_MM_USER;
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status lookup_record(const struct kernel_mm_record *record,
    uint64_t address, struct kernel_mm_mapping *mapping)
{ return lookup_record_mode(record,address,mapping,0); }

/* FOLL_FORCE grants data access through VM_MAY*, never a writable shared
 * mapping. Readonly private stores still resolve COW before borrowing a page. */
static int remote_access_allowed(const struct kernel_vma *vma, uint32_t access, int force)
{
    if (user_effective_permissions(vma->permissions) & access) return 1;
    if (!force || !(vma->maximum_permissions & access)) return 0;
    if (access == KERNEL_MM_READ) return 1;
    return (vma->kind == KERNEL_VMA_KIND_HUGETLB_PRIVATE || vma->kind == KERNEL_VMA_KIND_ANONYMOUS ||
            vma->kind == KERNEL_VMA_KIND_FILE_PRIVATE ||
            vma->kind == KERNEL_VMA_KIND_ELF_PRIVATE) &&
           vma->role != KERNEL_VMA_ROLE_VVAR && vma->role != KERNEL_VMA_ROLE_VDSO;
}

void kernel_mm_pagemap_hold(const struct kernel_mm *mm, struct kernel_mm_pagemap *view)
{
    /* The task resource guard stabilizes the live MM while opening procfs. */
    struct kernel_mm_record *record = mm->record;
    if (__atomic_fetch_add(&record->record_references, 1U, __ATOMIC_RELAXED) == UINT32_MAX)
        __builtin_trap();
    *view = (struct kernel_mm_pagemap){record, mm->allocator, mm->record_page_address};
}

void kernel_mm_pagemap_put(struct kernel_mm_pagemap *view)
{
    if (__atomic_fetch_sub(&view->record->record_references, 1U, __ATOMIC_ACQ_REL) == 1U &&
        physical_page_release(view->allocator, view->record_page))
        __builtin_trap();
}

int kernel_mm_pagemap_live(const struct kernel_mm_pagemap *view)
{
    return __atomic_load_n(&view->record->users, __ATOMIC_ACQUIRE) != 0;
}

int kernel_mm_pagemap_read(const struct kernel_mm_pagemap *view, uint64_t first,
    unsigned count, int show_pfn, uint64_t *entries, unsigned *completed)
{
    *completed = 0;
    uint64_t limit = ARCH_MMU_USER_LIMIT >> BOAROS_PAGE_SHIFT;
    if (first >= limit || !kernel_mm_pagemap_live(view)) return 0;
    if (count > KERNEL_MM_PAGEMAP_BATCH) count = KERNEL_MM_PAGEMAP_BATCH;
    if (count > limit - first) count = (unsigned)(limit - first);
    struct kernel_mm_record *record = view->record;
    KERNEL_LOCK_SCOPE(gate);
    kernel_rwlock_read(&record->operations, &gate);
    /* After the last task's user reference drops, teardown takes the same
     * gate for writing. The weak owner only retains the record page. */
    if (!kernel_mm_pagemap_live(view)) return 0;
    KERNEL_LOCK_SCOPE(vmas);
    kernel_rwlock_read(&record->vma_lock, &vmas);
    KERNEL_RAW_READ_SCOPE(metadata, &record->metadata);
    uint64_t address = first << BOAROS_PAGE_SHIFT;
    for (unsigned i = 0; i < count; ++i, address += BOAROS_PAGE_SIZE) {
        struct kernel_mm_leaf leaf;
        enum arch_mmu_status status = arch_mmu_user_leaf_snapshot(&record->space, address, &leaf);
        if (status == ARCH_MMU_STATUS_NOT_MAPPED) {
            entries[i] = 0;
            continue;
        }
        if (status != ARCH_MMU_STATUS_OK) return -KERNEL_EIO;
        uint64_t entry = UINT64_C(1) << 63; /* PROT_NONE leaves still own a page. */
        struct kernel_vma vma;
        if (record->vmas && !kernel_vma_set_lookup(record->vmas, address, &vma)) {
            struct mm_file_resident *file_page = find_file_resident(record, address);
            if ((file_page && !file_page->private) ||
                vma.kind == KERNEL_VMA_KIND_ANON_SHARED ||
                vma.kind == KERNEL_VMA_KIND_SYSV_SHM ||
                vma.kind == KERNEL_VMA_KIND_HUGETLB_SHARED)
                entry |= UINT64_C(1) << 61;
        }
        if (show_pfn) {
            uint64_t physical = leaf.physical_address + address - leaf.virtual_address;
            entry |= (physical >> BOAROS_PAGE_SHIFT) & ((UINT64_C(1) << 55) - 1);
        }
        entries[i] = entry;
    }
    *completed = count;
    return 0;
}
static enum kernel_mm_status remote_lookup_record(struct kernel_mm_record *record,
    uint64_t address, struct kernel_mm_mapping *mapping)
{ return lookup_record_mode(record,address,mapping,1); }

/* One metadata-qualified walk returns the real leaf or the preparation hint.
 * Keep only snapshot values if the caller has to drop the qualification. */
static enum kernel_mm_status fault_lookup_leaf(struct kernel_mm_record *record,
    uint64_t address, int force, struct kernel_mm_leaf *leaf, unsigned *missing, uint64_t **slot)
{
    enum arch_mmu_status found = arch_mmu_user_leaf_probe_slot(&record->space,address,leaf,missing,slot);
    if (found == ARCH_MMU_STATUS_NOT_MAPPED) return KERNEL_MM_STATUS_NOT_MAPPED;
    if (found) return KERNEL_MM_STATUS_ADDRESS_SPACE;
    if (!leaf->permissions && !force)
        return leaf->order ? KERNEL_MM_STATUS_ADDRESS_SPACE : KERNEL_MM_STATUS_NOT_MAPPED;
    return KERNEL_MM_STATUS_OK;
}

static int fault_cow_source(struct kernel_mm_record *record, uint64_t address,
    const struct kernel_mm_leaf *leaf, int force, uint64_t *cookie, uint64_t *physical)
{
    if (force) {
        /* Debugger writes can change a non-COW leaf before borrowing it. */
        if (arch_mmu_user_mark_cow_page(&record->space,address)) __builtin_trap();
        return arch_mmu_user_cow_snapshot(&record->space,address,cookie,physical)==ARCH_MMU_STATUS_OK;
    }
    if (leaf->order || !leaf->cow) return 0;
    *cookie=leaf->cookie;
    *physical=leaf->physical_address;
    return 1;
}

/* 文件来源登记、resident和alias由writer维护；缺页初查共享VMA资格。
 * MM raw写资格发布PTE、resident索引和pin许可，驻留pin共享短读资格；候选与TLB确认不持raw。 */
struct mm_operation {
    struct kernel_mm_record *record;
    const struct kernel_mm *mm;
    struct kernel_lock_guard guard;
    unsigned shared, populate;
    enum kernel_userfault_origin fault_origin;
    struct kernel_mm_user_pin *fault_pin;
#if BOAROS_MAX_CPUS > 1
    uint64_t sync_start, sync_end;
    uint32_t sync_targets;
#endif
    struct kernel_mm_range_claim claim;
    unsigned range_claimed;
    unsigned pins_closed;
};

static int current_pins(const struct kernel_mm *mm)
{
    for (const struct kernel_mm_user_pin *pin = kernel_io_context_current()->user_pins;
         pin; pin = pin->owner_next)
        if (pin->mm->allocator == mm->allocator &&
            pin->mm->record == mm->record) return 1;
    return 0;
}

static int mm_range_claim_try_acquire(struct kernel_mm_record *record,
    struct kernel_mm_range_claim *claim, uint64_t start, uint64_t end)
{
    claim->start = start;
    claim->end = end;
    claim->next = 0;
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
    for (struct kernel_mm_range_claim *c = record->range_claims; c; c = c->next) {
        if (c->start < claim->end && c->end > claim->start)
            return 0;
    }
    claim->next = record->range_claims;
    __atomic_store_n(&record->range_claims,claim,__ATOMIC_RELEASE);
    return 1;
}

static void mm_range_claim_acquire(struct kernel_mm_record *record,
    struct kernel_mm_range_claim *claim, uint64_t start, uint64_t end)
{
    claim->start = start;
    claim->end = end;
    claim->next = 0;
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            int conflict = 0;
            for (struct kernel_mm_range_claim *c = record->range_claims; c; c = c->next) {
                if (c->start < claim->end && c->end > claim->start) {
                    conflict = 1;
                    break;
                }
            }
            if (!conflict) {
                claim->next = record->range_claims;
                __atomic_store_n(&record->range_claims,claim,__ATOMIC_RELEASE);
                return;
            }
            if (kernel_wait_prepare(&record->range_changed, 0, 0, &token) !=
                KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
}

static void mm_range_claim_release(struct kernel_mm_record *record,
    struct kernel_mm_range_claim *claim)
{
    {
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        struct kernel_mm_range_claim **curr = &record->range_claims;
        while (*curr && *curr != claim)
            curr = &(*curr)->next;
        if (*curr != claim) __builtin_trap();
        if (curr == &record->range_claims)
            __atomic_store_n(&record->range_claims,claim->next,__ATOMIC_RELEASE);
        else
            *curr = claim->next;
        /* Faults can reuse a VMA snapshot while retaining the MM read gate.
         * The generation covers edits whose claims ended before publication. */
        uint64_t generation = __atomic_load_n(&record->range_edit_generation, __ATOMIC_RELAXED);
        if (generation == UINT64_MAX) __builtin_trap();
        __atomic_store_n(&record->range_edit_generation, generation + 1, __ATOMIC_RELEASE);
        /* Both range wait paths register while holding metadata. A later
         * registrant rechecks the list after this removal. */
        if (!kernel_wait_queue_registrations(&record->range_changed)) return;
    }
    KERNEL_IRQ_SCOPE(irq);
    enum kernel_scheduler_status status = kernel_wait_queue_wake_all(&record->range_changed);
    if (status != KERNEL_SCHEDULER_STATUS_OK && status != KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED)
        __builtin_trap();
}

static enum kernel_mm_status mm_wait_range_claim(struct kernel_mm_record *record,
    const struct kernel_mm *mm, uint64_t address)
{
    for (;;) {
        /* The optimistic test observes only emptiness, never a stack node.
         * Publication still validates the address against live claims. */
        if (!__atomic_load_n(&record->range_claims,__ATOMIC_ACQUIRE))
            return KERNEL_MM_STATUS_OK;
        struct kernel_wait_token token = {0};
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            int conflict = 0;
            for (struct kernel_mm_range_claim *c = record->range_claims; c; c = c->next) {
                if (address >= c->start && address < c->end) {
                    conflict = 1;
                    break;
                }
            }
            if (!conflict) return KERNEL_MM_STATUS_OK;
            if (current_pins(mm)) return KERNEL_MM_STATUS_CONFLICT;
            if (kernel_wait_prepare(&record->range_changed, 0, 0, &token) !=
                KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
}

static enum kernel_mm_status mm_operation_claim_range(struct mm_operation *operation,
    uint64_t start, uint64_t end)
{
#if BOAROS_MAX_CPUS == 1
    /* A UP modifier owns the whole MM; no second modifier can reach the
     * range until that owner has released its operation. */
    if (operation->guard.write) return KERNEL_MM_STATUS_OK;
#endif
    if (operation->range_claimed) __builtin_trap();
    if (current_pins(operation->mm)) {
        if (!mm_range_claim_try_acquire(operation->record, &operation->claim, start, end))
            return KERNEL_MM_STATUS_CONFLICT;
        operation->range_claimed = 1;
        return KERNEL_MM_STATUS_OK;
    }
    mm_range_claim_acquire(operation->record, &operation->claim, start, end);
    operation->range_claimed = 1;
    return KERNEL_MM_STATUS_OK;
}

#if BOAROS_MAX_CPUS > 1
static void mm_operation_unclaim_range(struct mm_operation *operation)
{
    if (!operation->range_claimed) return;
    mm_range_claim_release(operation->record, &operation->claim);
    operation->range_claimed = 0;
}

#endif
static uint32_t note_tlb_change_locked(struct kernel_mm_record *record,
    const struct kernel_mm_retirement *batch, int instructions)
{
    if (batch->count && record->pending_retirement == UINT32_MAX) __builtin_trap();
    mm_generation_advance(&record->tlb_generation);
    if (instructions) mm_generation_advance(&record->instruction_generation);
    if (batch->count) {
        record->pending_retirement++;
#if BOAROS_COST_DIAGNOSTICS
        record->retirement_batches++;
        record->retired_pages += batch->count;
#endif
    }
    return mm_sync_targets(record,1);
}
static uint32_t note_fault_change_locked(struct kernel_mm_record *record,
    const struct kernel_mm_retirement *batch, int replacement, int instructions)
{
#if !defined(BOAROS_ARCH_LOONGARCH)
    if (!replacement) {
        if (batch->count) __builtin_trap();
        /* A new translation cannot leave a stale permission or physical page.
         * Invalid walk-cache entries on other harts may fault once; that fault
         * flushes locally after finding the published PTE. Joining CPUs flush
         * the whole root, so no invalidation generation needs to advance. */
        if (!instructions) return 0;
        mm_generation_advance(&record->instruction_generation);
        return mm_sync_targets(record,0);
    }
#else
    (void)replacement;
#endif
    return note_tlb_change_locked(record, batch, instructions);
}
static void sync_tlb_change(uint32_t targets, uint64_t address,
    uint64_t size, int instructions)
{
    COST_SCOPE(sync_cost, MM_SYNC_TICKS);
    arch_mmu_sync_user(targets, address, size, instructions);
}

static void retirement_complete(struct kernel_mm_record *record, unsigned batches)
{
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
    if (record->pending_retirement < batches) __builtin_trap();
    record->pending_retirement -= batches;
}

static void finish_tlb_change(struct kernel_mm_record *record,
    struct kernel_mm_retirement *batch, uint32_t targets, uint64_t address,
    uint64_t size, int instructions)
{
    sync_tlb_change(targets, address, size, instructions);
    if (batch->count) {
        _Static_assert(KERNEL_MM_RETIRE_CAPACITY <= PHYSICAL_PAGE_REFERENCE_BATCH_MAX,
                       "one allocator guard per retirement batch");
        kernel_mm_retirement_release(record->space.allocator, batch);
        retirement_complete(record, 1);
    }
}
static void finish_fault_change(struct kernel_mm_record *record,
    struct kernel_mm_retirement *batch,uint32_t targets,uint64_t address,
    uint64_t size,int replacement,int instructions)
{
#if !defined(BOAROS_ARCH_LOONGARCH)
    if (!replacement) {
        if (batch->count) __builtin_trap();
        COST_SCOPE(sync_cost,MM_SYNC_TICKS);
        int translations=arch_mmu_new_translation_needs_sync();
        if (!translations && !instructions) return;
        kernel_preempt_disable();
        if (translations) arch_mmu_sync_user(0,address,size,0);
        if (instructions) arch_mmu_sync_instruction_cpus(targets);
        kernel_preempt_enable();
        return;
    }
#else
    (void)replacement;
#endif
    finish_tlb_change(record,batch,targets,address,size,instructions);
}
static void mm_operation_release(struct mm_operation *operation)
{
#if BOAROS_MAX_CPUS > 1
    if (operation->sync_end) {
        /* 仅新建非执行匿名叶，没有退休引用；读资格固定它们的VMA寿命。
         * 先确认本操作的范围，再让writer或内存压力等待继续推进。 */
        if (!operation->guard.lock || operation->guard.write || !operation->populate) __builtin_trap();
        {
            COST_SCOPE(sync_cost, MM_SYNC_TICKS);
#if defined(BOAROS_ARCH_LOONGARCH)
            arch_mmu_sync_user(operation->sync_targets,operation->sync_start,
                operation->sync_end-operation->sync_start,0);
#else
            if (operation->sync_targets) __builtin_trap();
            if (arch_mmu_new_translation_needs_sync())
                arch_mmu_sync_user(0,operation->sync_start,operation->sync_end-operation->sync_start,0);
#endif
        }
        operation->sync_start = operation->sync_end = 0;
        operation->sync_targets = 0;
    }
#endif
    mm_gate_release(operation->record, &operation->guard);
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
    if (operation->range_claimed) {
        mm_range_claim_release(operation->record, &operation->claim);
        operation->range_claimed = 0;
    }
    if (operation->guard.lock) {
        struct kernel_mm_record *record = operation->record;
        if (operation->pins_closed || (operation->guard.write && pin_admission_closed(record))) {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            pin_admission_open(record);
            kernel_raw_lock_release(&guard);
            wake_pin_waiters(record);
            operation->pins_closed = 0;
        }
        if (!operation->guard.write) {
            mm_operation_release(operation);
            return;
        }
        struct mm_file_mapping *retired=record->retired_file_mappings;
        record->retired_file_mappings=0;
        struct mm_file_resident *residents=record->retired_file_residents;
        record->retired_file_residents=0;
        /* Close/retry can wait for another MM writer. Keep ready sources
         * published too while this actor still owes any user-page pins. */
        int may_close = kernel_io_context_current()->user_pins == NULL;
        struct kernel_mm_file_source *files=may_close ? take_ready_file_sources(record) : NULL;
        struct kernel_mm_elf_source *elfs=may_close ? take_ready_elf_sources(record) : NULL;
        struct mm_huge_map *huge=record->retired_huge_maps;
        record->retired_huge_maps=0;
        struct kernel_mm_shared_anon *shared=record->retired_shared_anon;
        record->retired_shared_anon=0;
        mm_operation_release(operation);
        drain_retired_residents(record,residents);
        while (huge) {
            struct mm_huge_map *entry=huge;huge=entry->next;
            kernel_hugetlb_mapping_drain(&entry->registration);
            kernel_hugetlb_map_release(&entry->map);
            kernel_heap_release(record->vma_heap,entry);
        }
        while (shared) {
            struct kernel_mm_shared_anon *entry=shared;shared=entry->next;
            kernel_memory_object_mapping_drain(&entry->registration);
            kernel_memory_object_release(&entry->object);
            kernel_heap_release(record->vma_heap,entry);
        }
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
static enum kernel_mm_status mm_operation_lock(struct mm_operation *operation)
{
    /* gate持有者可能正排空本owner的pin；不把仍有义务归还的owner排进等待环。 */
    if (current_pins(operation->mm)) {
        int acquired;
        if (operation->shared)
            acquired = kernel_rwlock_try_read(&operation->record->operations, &operation->guard);
        else
            acquired = kernel_rwlock_try_write(&operation->record->operations, &operation->guard);
        if (!acquired) return KERNEL_MM_STATUS_CONFLICT;
    } else if (operation->shared) {
        kernel_rwlock_read(&operation->record->operations, &operation->guard);
    } else kernel_rwlock_write(&operation->record->operations, &operation->guard);
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
struct fault_vma {
    struct kernel_vma vma;
    uint64_t generation;
    uint64_t range_edit_generation;
};

/* Anonymous faults establish mapping history. A private file mapping
 * acquires that history when servicing its first write. */
static inline int fault_needs_anon_history(const struct kernel_vma *vma,
    uint32_t access, int force)
{
    if (vma->flags & KERNEL_VMA_ANON_HISTORY) return 0;
    int anonymous = (vma->kind == KERNEL_VMA_KIND_ANONYMOUS &&
        vma->role != KERNEL_VMA_ROLE_VDSO && vma->role != KERNEL_VMA_ROLE_VVAR) ||
        (vma->flags & KERNEL_VMA_ANON_SOURCE);
    return (anonymous || (access == KERNEL_MM_WRITE &&
        (vma->kind == KERNEL_VMA_KIND_FILE_PRIVATE || vma->kind == KERNEL_VMA_KIND_ELF_PRIVATE))) &&
        remote_access_allowed(vma, access, force);
}

/* The caller holds VMA qualification. Metadata serializes simultaneous
 * history marks and the merge hint without upgrading the VMA reader. */
static __attribute__((noinline)) enum kernel_mm_status mark_fault_history(
    struct mm_operation *operation, uint64_t address, struct fault_vma *view)
{
    struct kernel_mm_record *record = operation->record;
    struct kernel_raw_guard metadata __attribute__((cleanup(kernel_raw_scope_release))) = {0};
    if (operation->shared) kernel_raw_rwlock_write_acquire(&record->metadata, &metadata);
    if (!kernel_vma_set_mark_anonymous(record->vmas, address, view->generation, &view->vma))
        return KERNEL_MM_STATUS_CONFLICT;
    view->generation = kernel_vma_set_generation(record->vmas);
    return KERNEL_MM_STATUS_OK;
}

/* A file or ELF registration cannot disappear while the MM gate is held.
 * Shared-gate range edits still own vma_lock: copy descriptor and version
 * together, after any conflicting range claim has finished. */
static enum kernel_mm_status read_fault_vma(struct mm_operation *operation,
    uint64_t address, struct fault_vma *view, uint32_t access, int force)
{
    struct kernel_mm_record *record = operation->record;
    if (!record->vmas) return KERNEL_MM_STATUS_NOT_MAPPED;
    KERNEL_LOCK_SCOPE(vma_guard);
    if (operation->shared) {
        enum kernel_mm_status status = mm_wait_range_claim(record, operation->mm, address);
        if (status) return status;
        kernel_rwlock_read(&record->vma_lock, &vma_guard);
        view->range_edit_generation =
            __atomic_load_n(&record->range_edit_generation, __ATOMIC_ACQUIRE);
    }
    enum kernel_mm_status status = status_from_vma(kernel_vma_set_lookup_recent(record->vmas, address,
        &record->recent_fault_vma, &view->vma, &view->generation));
    if (!status && access && fault_needs_anon_history(&view->vma, access, force))
        return mark_fault_history(operation, address, view);
    return status;
}
/* 在MM raw内登记并复查，解锁后park；末次pin归还也以该锁同步登记后wake。
 * 注册owner由局部token持有，退出函数前finish，不携带裸队列游标。 */
static void wait_pin_condition(struct kernel_mm_record *record, int drain)
{
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            int closed = pin_admission_closed(record);
            int blocked = drain ? (uint32_t)pin_state_read(closed == 2 ?
                &record->write_pins : &record->access_pins) != 0 : closed != 0;
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
static void close_record_pins(struct kernel_mm_record *record,int writers_only)
{
    /* 不等待自身尚未结束的片段；munmap不排空pin，仍允许同owner撤映射。 */
    for (struct kernel_mm_user_pin *pin = kernel_io_context_current()->user_pins;
         pin; pin = pin->owner_next)
        if (pin->mm->record == record &&
            (!writers_only || (pin->access & KERNEL_MM_WRITE)))
            __builtin_trap();
    for (;;) {
        struct kernel_wait_token token = {0};
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            if (!pin_admission_closed(record)) {
                pin_admission_close(record, writers_only);
                break;
            }
            if (kernel_wait_prepare(&record->pins_changed, 0, 0, &token) !=
                KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) != KERNEL_SCHEDULER_STATUS_OK ||
            kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    }
    wait_pin_condition(record, 1);
}
static void close_pins(struct mm_operation *operation,int writers_only)
{
    if (operation->guard.lock) {
        close_record_pins(operation->record, writers_only);
        operation->pins_closed = 1;
    }
}
static void unmap_object_ranges(struct kernel_mm_record *record,struct kernel_memory_object *object,
    uint64_t start,uint64_t end)
{
    for (uint32_t i=0;i<kernel_vma_set_count(record->vmas);i++) {
        struct kernel_vma vma;
        if (kernel_vma_set_get_at(record->vmas,i,&vma)) __builtin_trap();
        if ((vma.kind!=KERNEL_VMA_KIND_ANON_SHARED && vma.kind!=KERNEL_VMA_KIND_SYSV_SHM) ||
            vma.backing!=object || vma.backing_offset>=end) continue;
        uint64_t offset=vma.backing_offset, length=vma.end-vma.start;
        if (offset<=start && start-offset>=length) continue;
        uint64_t first=start>offset?start-offset:0;
        uint64_t last=end-offset<length?end-offset:length;
        if (unmap_space_range(record,vma.start+first,vma.start+last)) __builtin_trap();
    }
}
static void invalidate_object_residents(void *owner,struct kernel_memory_object *object,
    uint64_t start,uint64_t end)
{
    struct kernel_mm_record *record=owner;
    struct mm_gate_scope operation __attribute__((cleanup(mm_gate_scope_end)))={.record=record};
    kernel_rwlock_write(&record->operations,&operation.guard);
    if (record->stage!=KERNEL_MM_RECORD_LIVE) return;
    close_record_pins(record,0);
    unmap_object_ranges(record,object,start,end);
    { KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);pin_admission_open(record); }
    wake_pin_waiters(record);
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
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
    if (record->stage != KERNEL_MM_RECORD_LIVE) return KERNEL_MM_STATUS_STATE;
    return lookup_record(record, address, mapping);
}

static inline enum kernel_mm_status lookup_user_mapping(const struct kernel_mm_record *record,
    uint64_t address, uint32_t access, struct kernel_mm_mapping *mapping)
{
    enum kernel_mm_status status = lookup_record(record, address, mapping);
    if (status) return status;
    return (mapping->permissions & (access | KERNEL_MM_USER)) ==
        (access | KERNEL_MM_USER) ? KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_ACCESS;
}

static __attribute__((always_inline)) inline void register_user_pin(const struct kernel_mm *mm,
    struct kernel_mm_record *record,uint64_t physical,uint32_t order,void *pointer,uint32_t access,
    struct kernel_mm_user_pin *pin)
{
    uint64_t head=physical & ~(((uint64_t)BOAROS_PAGE_SIZE<<order)-1);
    if (physical_page_acquire_order(mm->allocator, head, order) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    if (pin_count_add(&record->access_pins) == UINT32_MAX) __builtin_trap();
#if BOAROS_COST_DIAGNOSTICS
    __atomic_fetch_add(&record->pin_acquisitions, 1U, __ATOMIC_RELAXED);
#endif
    if ((access & KERNEL_MM_WRITE) && pin_count_add(&record->write_pins) == UINT32_MAX)
        __builtin_trap();
    struct kernel_io_context *owner = kernel_io_context_current();
    pin->mm=mm; pin->record=record; pin->owner=owner; pin->registration=pin;
    pin->owner_next=owner->user_pins; pin->owner_previous=&owner->user_pins;
    if (pin->owner_next) pin->owner_next->owner_previous=&pin->owner_next;
    pin->page=pointer; pin->physical_address=physical; pin->access=access;
    pin->owner_address=head; pin->order=order;
    owner->user_pins = pin;
}

/* PTE、resident索引与权限许可由同一短读资格固定；实际页acquire不能移出。 */
static __attribute__((always_inline)) inline enum kernel_mm_status pin_mapping(const struct kernel_mm *mm,
    struct kernel_mm_record *record, uint64_t address, uint32_t access,
    struct kernel_mm_user_pin *pin, int cache_write, int force)
{
    struct kernel_mm_mapping mapping;
    void *pointer;
    enum kernel_mm_status status;
    if (force) {
        struct kernel_vma vma;
        if (!record->vmas || kernel_vma_set_lookup(record->vmas,address,&vma))
            return KERNEL_MM_STATUS_NOT_MAPPED;
        if (!remote_access_allowed(&vma,access,1)) return KERNEL_MM_STATUS_ACCESS;
        status=remote_lookup_record(record,address,&mapping);
        if (!status && access == KERNEL_MM_WRITE) {
            struct kernel_mm_leaf leaf;
            if (arch_mmu_user_leaf_snapshot(&record->space,address,&leaf))
                return KERNEL_MM_STATUS_ADDRESS_SPACE;
            if (leaf.cow) return KERNEL_MM_STATUS_ACCESS;
        }
    } else status=lookup_user_mapping(record,address,access,&mapping);
    if (status != KERNEL_MM_STATUS_OK) return status;
    uint64_t physical = mapping.physical_address & ~BOAROS_PAGE_MASK;
    /* permission许可和取得实际页引用不能被撤映射插入。 */
    uint64_t head=physical & ~(((uint64_t)BOAROS_PAGE_SIZE<<mapping.order)-1);
    if (physical_page_map_owned(mm->allocator, head, &pointer) !=
        PHYSICAL_PAGE_STATUS_OK) return KERNEL_MM_STATUS_PAGE_ACCESS;
    pointer=(unsigned char *)pointer+(physical-head);
    if (cache_write) {
        struct mm_file_alias_resident *resident = file_alias_resident(
            find_file_resident(record, address & ~BOAROS_PAGE_MASK));
        if (resident && kernel_page_cache_alias_active(&resident->alias)) {
            enum kernel_page_cache_status admitted=kernel_page_cache_alias_write_begin(&resident->alias,&pin->cache_write);
            if (admitted==KERNEL_PAGE_CACHE_STATUS_BUSY) return KERNEL_MM_STATUS_ACCESS;
            if (admitted) __builtin_trap();
        }
    }
    register_user_pin(mm,record,physical,mapping.order,pointer,access,pin);
    return KERNEL_MM_STATUS_OK;
}

/* The publishing private fault already validated its VMA and PTE under raw.
 * Acquire the final copy loan there instead of releasing and walking again.
 * Closed admission simply leaves the ordinary post-fault pin path in charge. */
static __attribute__((always_inline)) inline void pin_private_fault(struct mm_operation *operation,
    const struct kernel_vma *vma,uint64_t physical,uint32_t access)
{
    if (!operation->fault_pin || (vma->kind!=KERNEL_VMA_KIND_ANONYMOUS &&
        vma->kind!=KERNEL_VMA_KIND_ELF_PRIVATE)) return;
    struct kernel_mm_record *record=operation->record;
    int closed=pin_admission_closed(record);
    if (closed==1 || (closed==2 && (access&KERNEL_MM_WRITE))) return;
    void *pointer;
    if (physical_page_map_owned(record->space.allocator,physical&~BOAROS_PAGE_MASK,&pointer))
        __builtin_trap();
    register_user_pin(operation->mm,record,physical&~BOAROS_PAGE_MASK,0,pointer,access,operation->fault_pin);
}

static inline enum kernel_mm_status pin_user_prefix(const struct kernel_mm *mm,
    uint64_t address,uint64_t second,uint32_t access,struct kernel_mm_user_pin *pin,
    unsigned *count,int may_wait)
{
    struct kernel_mm_record *record;
    if (!mm || !pin || !access ||
        (access & ~(KERNEL_MM_READ | KERNEL_MM_WRITE | KERNEL_MM_EXECUTE)))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (pin->registration) __builtin_trap();
    if (count) {
        if (pin[1].registration) __builtin_trap();
        *count=0;
    }
    if (mm->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
retry:;
    KERNEL_RAW_READ_SCOPE(guard, &record->metadata);
    int closed = pin_admission_closed(record);
    if (closed == 1 || (closed == 2 && (access & KERNEL_MM_WRITE))) {
        if (!may_wait || current_pins(mm)) return KERNEL_MM_STATUS_CONFLICT;
        kernel_raw_rwlock_read_release(&guard);
        wait_pin_condition(record, 0);
        goto retry;
    }
    status=pin_mapping(mm,record,address,access,pin,(access & KERNEL_MM_WRITE)!=0,0);
    if (status || !count) return status;
    *count=1;
    /* The metadata qualification still fixes both mappings. The first pin
     * is live, so resolving or waiting for the second would deadlock an editor. */
    status=pin_mapping(mm,record,second,access,&pin[1],(access & KERNEL_MM_WRITE)!=0,0);
    if (!status) *count=2;
    else if (status!=KERNEL_MM_STATUS_NOT_MAPPED && status!=KERNEL_MM_STATUS_ACCESS &&
             status!=KERNEL_MM_STATUS_ADDRESS_SPACE) __builtin_trap();
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_pin_user_page(const struct kernel_mm *mm,
    uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{ return pin_user_prefix(mm,address,0,access,pin,NULL,1); }

enum kernel_mm_status kernel_mm_try_pin_user_page(const struct kernel_mm *mm,
    uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{ return pin_user_prefix(mm,address,0,access,pin,NULL,0); }

enum kernel_mm_status kernel_mm_pin_user_pair(const struct kernel_mm *mm,
    uint64_t first,uint64_t second,uint32_t access,struct kernel_mm_user_pin pins[2],unsigned *count)
{
    if (!count || (first>>BOAROS_PAGE_SHIFT)==(second>>BOAROS_PAGE_SHIFT))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    return pin_user_prefix(mm,first,second,access,pins,count,1);
}

static inline void unpin_user_pages(struct kernel_mm_user_pin *pins,unsigned count)
{
    if (!pins || !pins[0].mm || pins[0].mm->state!=KERNEL_MM_LIVE || !pins[0].record)
        __builtin_trap();
    const struct kernel_mm *mm=pins[0].mm;
    struct kernel_io_context *owner=kernel_io_context_current();
    /* caller保留LIVE句柄直到本函数返回；计数归零后record仍有实际MM owner。 */
    struct kernel_mm_record *record=pins[0].record;
    int wake;
    {
#if BOAROS_MAX_CPUS == 1
        /* UP没有并行归还；仅此无调用片段禁止中断抢占，计数无需原子RMW。 */
        KERNEL_IRQ_SCOPE(irq);
#endif
        unsigned writes=0;
        for (unsigned i=count;i>0;) {
            struct kernel_mm_user_pin *pin=&pins[--i];
            if (pin->registration!=pin || pin->owner!=owner || pin->mm!=mm ||
                pin->record!=record || !pin->owner_previous || *pin->owner_previous!=pin)
                __builtin_trap();
            *pin->owner_previous=pin->owner_next;
            if (pin->owner_next) pin->owner_next->owner_previous=pin->owner_previous;
            writes+=!!(pin->access&KERNEL_MM_WRITE);
        }
        uint64_t last_write = 0;
        if (writes) last_write=pin_count_sub(&record->write_pins,writes);
        /* 全部access归零也必须意味着该writer的write计数已归还。 */
        uint64_t last_access=pin_count_sub(&record->access_pins,count);
        if ((uint32_t)last_access<count || (uint32_t)last_write<writes) __builtin_trap();
        wake=last_access==(MM_PIN_CLOSED|count) || (writes && last_write==(MM_PIN_CLOSED|writes));
    }
    if (wake) {
        /* 若drainer先看见旧计数，它必在此资格前完成wait_prepare；若先看见
         * 归零则不等待。关闭和末次归还作用于同一原子字，不丢掉通知责任。 */
        KERNEL_RAW_READ_SCOPE(guard, &record->metadata);
    }
    /* The pin owns this reference independently of the current PTE. Drain
     * waits for completed access; allocator return needs no MM qualification. */
    if (count==1) {
        if (physical_page_release_order(mm->allocator,pins[0].owner_address,pins[0].order)) __builtin_trap();
    } else {
        const uint64_t physical[]={pins[0].owner_address,pins[1].owner_address};
        if (pins[0].order || pins[1].order) {
            const uint8_t orders[]={(uint8_t)pins[0].order,(uint8_t)pins[1].order};
            physical_page_release_order_batch(mm->allocator,physical,orders,2);
        } else physical_page_release_batch(mm->allocator,physical,2);
    }
    for (unsigned i=count;i>0;) {
        struct kernel_mm_user_pin *pin=&pins[--i];
        if (pin->cache_write.entry) kernel_page_cache_write_end(&pin->cache_write);
        *pin=(struct kernel_mm_user_pin){0};
    }
    if (wake) wake_pin_waiters(record);
}

void kernel_mm_unpin_user_page(struct kernel_mm_user_pin *pin)
{ unpin_user_pages(pin,1); }
void kernel_mm_unpin_user_pair(struct kernel_mm_user_pin pins[2])
{ unpin_user_pages(pins,2); }

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
    vma.maximum_permissions = role == KERNEL_VMA_ROLE_VVAR ? KERNEL_MM_READ : KERNEL_VMA_ALL_PERMISSIONS;
    vma.kind = KERNEL_VMA_KIND_ANONYMOUS;
    vma.role = role;
    if (role==KERNEL_VMA_ROLE_VVAR) vma.flags|=KERNEL_VMA_DONTDUMP;
    if (role==KERNEL_VMA_ROLE_STACK || fault_policy==KERNEL_VMA_FAULT_RESIDENT_REQUIRED)
        vma.flags|=KERNEL_VMA_ANON_HISTORY;
    vma.fault_policy = fault_policy;
    vma.backing = 0;
    status = status_from_vma(kernel_vma_set_insert(record->vmas, &vma));
    return status;
}

static uint32_t mm_permissions_from_elf_flags(uint32_t flags)
{
    uint32_t permissions = 0U;

    if ((flags & KERNEL_ELF64_FLAG_READ) != 0U) {
        permissions |= KERNEL_MM_READ;
    }
    if ((flags & KERNEL_ELF64_FLAG_WRITE) != 0U) {
        permissions |= KERNEL_MM_WRITE;
    }
    if ((flags & KERNEL_ELF64_FLAG_EXECUTE) != 0U) {
        permissions |= KERNEL_MM_EXECUTE;
    }
    return user_vma_permissions(permissions);
}

enum kernel_mm_status kernel_mm_map_elf_source(
    struct kernel_mm *mm,
    struct kernel_elf64_source *source,
    uint64_t load_bias)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record;
    struct kernel_mm_elf_source *source_owner;
    int added_source = 0;
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
    source_owner = find_elf_source(record, source);
    if (!source_owner) {
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
        added_source = 1;
    }
    run_count = kernel_elf64_source_run_count(source);
    if (run_count == 0U) {
        if (added_source) {
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
            .backing_owner = source_owner,
        };

        /* Linux padzero creates private anonymous backing for the ELF tail.
         * Our composite source synthesizes it lazily, but even a read may
         * publish its writable private page without a later write fault. */
        if (run->kind==KERNEL_ELF64_SOURCE_RUN_ZERO)
            vma.flags |= KERNEL_VMA_ANON_SOURCE;
        else if (run->kind==KERNEL_ELF64_SOURCE_RUN_COMPOSITE || !kernel_elf64_source_file(source))
            vma.flags |= KERNEL_VMA_ANON_HISTORY;
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
    if (added_source) {
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
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
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
    struct kernel_lock_guard vma_guard = {0};
    kernel_rwlock_read(&record->vma_lock, &vma_guard);
    enum kernel_vma_status vs = kernel_vma_set_lookup(record->vmas,
                                                      virtual_address,
                                                      vma);
    kernel_lock_release(&vma_guard);
    return status_from_vma(vs);
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
enum { UFFD_EVENT_PAGEFAULT = 0x12, UFFD_PAGEFAULT_WRITE = 1 };

/* The active list owns one reference. Each enrolled fault owns one until its
 * wait token is finished. Wake removes the list owner before notifying. */
struct kernel_userfault_page {
    RB_ENTRY(kernel_userfault_page) index;
    struct kernel_userfault_page *next, **previous; /* Unread FIFO. */
    struct kernel_userfault_page *wake_next;
    struct kernel_wait_token direct_waiter;
    struct kernel_wait_queue waiters;
    struct uffd_message message;
    unsigned references, enrolled;
    int active;
};

static int compare_pages(struct kernel_userfault_page *a, struct kernel_userfault_page *b)
{
    return (a->message.address > b->message.address) - (a->message.address < b->message.address);
}
RB_GENERATE_INTERNAL(userfault_pages, kernel_userfault_page, index, compare_pages, static inline)

static void free_userfault_page(struct kernel_userfault_ctx *ctx, struct kernel_userfault_page *page)
{
    if (kernel_wait_queue_close(&page->waiters) ||
        kernel_wait_queue_destroy(&page->waiters)) __builtin_trap();
    kernel_heap_release(ctx->heap, page);
}

void kernel_userfault_context_put(struct kernel_userfault_ctx *ctx)
{
    if (__atomic_fetch_sub(&ctx->references, 1, __ATOMIC_ACQ_REL) != 1) return;
    if (!RB_EMPTY(&ctx->pages) || kernel_wait_queue_close(&ctx->ready) ||
        kernel_wait_queue_destroy(&ctx->ready)) __builtin_trap();
    if (ctx->spare) free_userfault_page(ctx, ctx->spare);
    if (ctx->zero_page != KERNEL_MM_NO_PAGE &&
        physical_page_release(ctx->target.allocator, ctx->zero_page)) __builtin_trap();
    kernel_mm_pagemap_put(&ctx->target);
    kernel_heap_release(ctx->heap, ctx);
}

static void page_put(struct kernel_userfault_ctx *ctx, struct kernel_userfault_page *page)
{
    if (__atomic_fetch_sub(&page->references, 1, __ATOMIC_ACQ_REL) != 1) return;
    if (page->active || page->previous || page->enrolled || page->direct_waiter.task ||
        kernel_wait_queue_registrations(&page->waiters)) __builtin_trap();
    /* Every enrolled fault has finished its token and a detached waker has
     * returned its cursor before dropping the final reference. Keep one
     * empty queue for the next event on this context. */
    {
        KERNEL_RAW_SCOPE(guard, &ctx->lock);
        if (!ctx->spare) {
            ctx->spare = page;
            return;
        }
    }
    free_userfault_page(ctx, page);
}

static void dequeue_message(struct kernel_userfault_ctx *ctx, struct kernel_userfault_page *page)
{
    if (!page->previous) return;
    *page->previous = page->next;
    if (page->next) page->next->previous = page->previous;
    else ctx->pending_tail = page->previous;
    page->next = NULL;
    page->previous = NULL;
}

static void unlink_page(struct kernel_userfault_ctx *ctx, struct kernel_userfault_page *page)
{
    RB_REMOVE(userfault_pages, &ctx->pages, page);
    page->active = 0;
    dequeue_message(ctx, page);
}

static int kernel_userfault_prepare(struct kernel_userfault_ctx *ctx, uint64_t address,
    uint32_t access, enum kernel_userfault_origin origin, struct kernel_userfault_wait *wait)
{
    if (origin == KERNEL_USERFAULT_INTERNAL ||
        (origin == KERNEL_USERFAULT_COPY && (ctx->flags & UFFD_USER_MODE_ONLY)))
        return -KERNEL_EFAULT;

    struct kernel_userfault_page key;
    key.message.address = address;
    struct kernel_userfault_page *candidate = NULL;
    int allocated = 0;
    for (;;) {
        KERNEL_RAW_SCOPE(guard, &ctx->lock);
        struct kernel_userfault_page *page = RB_FIND(userfault_pages, &ctx->pages, &key);
        if (ctx->dead || (!page && allocated && !candidate)) {
            int error = ctx->dead ? -KERNEL_EAGAIN : -KERNEL_ENOMEM;
            kernel_raw_lock_release(&guard);
            if (candidate) page_put(ctx, candidate);
            return error;
        }
        if (!page && !candidate && ctx->spare) {
            candidate = ctx->spare;
            ctx->spare = NULL;
            candidate->references = 1;
        }
        if (!page && !candidate) {
            kernel_raw_lock_release(&guard);
            allocated = 1;
            if (!kernel_heap_allocate_zeroed_nosleep(ctx->heap, 1, sizeof(*candidate),
                    (void **)&candidate)) {
                candidate->references = 1;
                kernel_wait_queue_init(&candidate->waiters);
            }
            continue;
        }
        if (!page) {
            page = candidate;
            candidate = NULL;
            page->message = (struct uffd_message){
                .event = UFFD_EVENT_PAGEFAULT, .address = address,
                .flags = access == KERNEL_MM_WRITE ? UFFD_PAGEFAULT_WRITE : 0,
            };
            if (RB_INSERT(userfault_pages, &ctx->pages, page)) __builtin_trap();
            page->active = 1;
            page->previous = ctx->pending_tail;
            *ctx->pending_tail = page;
            ctx->pending_tail = &page->next;
            (void)kernel_wait_cursor_prepare_event(&ctx->ready, &wait->ready, KERNEL_POLLIN, NULL);
        }
        if (page->previous && access == KERNEL_MM_WRITE)
            page->message.flags |= UFFD_PAGEFAULT_WRITE;
        struct kernel_wait_queue *queue = page->enrolled ? &page->waiters : NULL;
        if (kernel_wait_prepare(queue, 0, KERNEL_WAIT_KILLABLE, &wait->token))
            __builtin_trap();
        if (!queue) page->direct_waiter = wait->token;
        __atomic_fetch_add(&ctx->references, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&page->references, 1, __ATOMIC_RELAXED);
        page->enrolled++;
        wait->page = page;
        kernel_raw_lock_release(&guard);
        if (candidate) page_put(ctx, candidate);
        return 0;
    }
}

static enum kernel_wait_wake_reason kernel_userfault_wait(struct kernel_userfault_ctx *ctx,
    struct kernel_userfault_wait *wait)
{
    if (wait->ready.queue) kernel_wait_cursor_wake(&wait->ready, 1);
    enum kernel_wait_wake_reason reason;
    if (kernel_wait_park(&wait->token, &reason)) __builtin_trap();
    struct kernel_userfault_page *page = wait->page;
    int drop_list = 0;
    {
        KERNEL_RAW_SCOPE(guard, &ctx->lock);
        /* Stop new borrows before finish can observe that none remain.
         * Once detached, only the waker may touch the direct token. */
        if (page->active && page->direct_waiter.generation == wait->token.generation)
            page->direct_waiter = (struct kernel_wait_token){0};
        if (!--page->enrolled && page->active) {
            unlink_page(ctx, page);
            drop_list = 1;
        }
    }
    if (kernel_wait_finish(&wait->token)) __builtin_trap();
    if (drop_list) page_put(ctx, page);
    page_put(ctx, page);
    kernel_userfault_context_put(ctx);
    return reason;
}

void kernel_userfault_wake(struct kernel_userfault_ctx *ctx, uint64_t start, uint64_t end)
{
    struct kernel_userfault_page *woken = NULL;
    {
        KERNEL_RAW_SCOPE(guard, &ctx->lock);
        struct kernel_userfault_page key;
        key.message.address = start;
        struct kernel_userfault_page *page = RB_NFIND(userfault_pages, &ctx->pages, &key);
        while (page && page->message.address < end) {
            struct kernel_userfault_page *next = RB_NEXT(userfault_pages, &ctx->pages, page);
            unlink_page(ctx, page);
            if (page->direct_waiter.task)
                kernel_wait_task_pin(page->direct_waiter.task);
            page->wake_next = woken;
            woken = page;
            page = next;
        }
    }
    while (woken) {
        struct kernel_userfault_page *page = woken;
        woken = page->wake_next;
        if (page->direct_waiter.task) {
            KERNEL_IRQ_SCOPE(irq);
            if (kernel_wait_notify(&page->direct_waiter, KERNEL_WAIT_WOKEN)) __builtin_trap();
            kernel_wait_task_unpin(page->direct_waiter.task);
            page->direct_waiter = (struct kernel_wait_token){0};
        }
        if (kernel_wait_queue_registrations(&page->waiters)) {
            KERNEL_IRQ_SCOPE(irq);
            if (kernel_wait_queue_wake_all(&page->waiters)) __builtin_trap();
        }
        page_put(ctx, page);
    }
}

static void kernel_userfault_dead(struct kernel_userfault_ctx *ctx)
{
    struct kernel_wait_cursor ready = {0};
    {
        KERNEL_RAW_SCOPE(guard, &ctx->lock);
        if (ctx->dead) return;
        ctx->dead = 1;
        (void)kernel_wait_cursor_prepare_event(&ctx->ready, &ready, KERNEL_POLLHUP, NULL);
    }
    kernel_userfault_wake(ctx, 0, ARCH_MMU_USER_LIMIT);
    if (ready.queue) kernel_wait_cursor_wake(&ready, 1);
}

int kernel_userfault_next(struct kernel_userfault_ctx *ctx, struct uffd_message *message, int no_wait)
{
    for (;;) {
        struct kernel_wait_token token;
        {
            KERNEL_RAW_SCOPE(guard, &ctx->lock);
            if (!__atomic_load_n(&ctx->initialized, __ATOMIC_RELAXED)) return -KERNEL_EINVAL;
            if (ctx->dead) return 0;
            struct kernel_userfault_page *page = ctx->pending;
            if (page) {
                *message = page->message;
                dequeue_message(ctx, page);
                return 1;
            }
            if (no_wait) return -KERNEL_EAGAIN;
            if (kernel_wait_prepare(&ctx->ready, 0, KERNEL_WAIT_INTERRUPTIBLE, &token))
                __builtin_trap();
        }
        enum kernel_wait_wake_reason reason;
        if (kernel_wait_park(&token, &reason) || kernel_wait_finish(&token)) __builtin_trap();
        if (reason == KERNEL_WAIT_SIGNALLED) return -KERNEL_ERESTARTSYS;
    }
}

int kernel_mm_userfault_attach(struct kernel_mm *mm, struct kernel_userfault_ctx *ctx)
{
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
    struct kernel_mm_record *record = operation.record;
    if (!__atomic_load_n(&record->users, __ATOMIC_ACQUIRE)) return -KERNEL_ESRCH;
    kernel_mm_pagemap_hold(mm, &ctx->target);
    ctx->next = record->userfaults;
    __atomic_store_n(&record->userfaults, ctx, __ATOMIC_RELEASE);
    return 0;
}

void kernel_mm_userfault_detach(struct kernel_userfault_ctx *ctx)
{
    struct kernel_mm_record *record = ctx->target.record;
    KERNEL_LOCK_SCOPE(gate);
    kernel_rwlock_write(&record->operations, &gate);
    struct kernel_userfault_ctx *head = record->userfaults;
    struct kernel_userfault_ctx **link = &head;
    while (*link != ctx) {
        if (!*link) __builtin_trap();
        link = &(*link)->next;
    }
    *link = ctx->next;
    __atomic_store_n(&record->userfaults, head, __ATOMIC_RELEASE);
    {
        KERNEL_RAW_SCOPE(guard, &ctx->lock);
        /* The last task clears the VMA registrations before marking its
         * contexts dead. If close wins the writer gate, it still owns the
         * VMA cleanup, even after users reached zero. */
        if (ctx->dead) return;
    }
    kernel_vma_set_clear_userfault(record->vmas, ctx);
    mm_gate_release(record, &gate);
    kernel_userfault_dead(ctx);
}

int kernel_mm_userfault_register(struct kernel_userfault_ctx *ctx,
    uint64_t start, uint64_t end, int enable)
{
    struct kernel_mm_record *record = ctx->target.record;
    KERNEL_LOCK_SCOPE(gate);
    kernel_rwlock_write(&record->operations, &gate);
    if (!kernel_mm_pagemap_live(&ctx->target)) return -KERNEL_ENOMEM;
    int error = kernel_vma_set_userfault(record->vmas, start, end, ctx, enable);
    if (!error && !enable) kernel_userfault_wake(ctx, start, end);
    mm_gate_release(record, &gate);
    return error;
}

static void userfault_mm_dead(struct kernel_mm_record *record)
{
    KERNEL_LOCK_SCOPE(gate);
    kernel_rwlock_write(&record->operations, &gate);
    if (record->userfaults) {
        kernel_vma_set_clear_userfault(record->vmas, NULL);
        for (struct kernel_userfault_ctx *ctx = record->userfaults; ctx; ctx = ctx->next)
            kernel_userfault_dead(ctx);
    }
    mm_gate_release(record, &gate);
}

uint32_t kernel_mm_remove_user(struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK || mm->state != KERNEL_MM_LIVE)
        __builtin_trap();
    uint32_t old = __atomic_fetch_sub(&record->users, 1U, __ATOMIC_ACQ_REL);
    if (!old) __builtin_trap();
    if (old == 1U && __atomic_load_n(&record->userfaults, __ATOMIC_ACQUIRE))
        userfault_mm_dead(record);
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
        address > ARCH_MMU_USER_LIMIT - (1U + KERNEL_HAS_VDSO_ELF) * BOAROS_PAGE_SIZE ||
        (address & BOAROS_PAGE_MASK) != 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
#if KERNEL_HAS_VDSO_ELF
    if (record->vdso_address) return KERNEL_MM_STATUS_STATE;
    uint64_t vvar_address = address + BOAROS_PAGE_SIZE, physical;
    status = kernel_mm_vma_insert_anon(mm, vvar_address, vvar_address + BOAROS_PAGE_SIZE,
        KERNEL_MM_READ, KERNEL_VMA_ROLE_VVAR, KERNEL_VMA_FAULT_RESIDENT_REQUIRED);
    if (status) return status;
    if (kernel_time_vvar_acquire(mm->allocator, &physical)) return KERNEL_MM_STATUS_NO_MEMORY;
    if (physical_page_acquire(mm->allocator, physical) != PHYSICAL_PAGE_STATUS_OK)
        __builtin_trap();
    enum arch_mmu_status mapped = arch_mmu_user_map_owned_page(&record->space,
        vvar_address, physical, ARCH_MMU_READ);
    if (mapped) {
        if (physical_page_release(mm->allocator, physical) != PHYSICAL_PAGE_STATUS_OK)
            __builtin_trap();
        kernel_time_vvar_release(mm->allocator, physical);
        return mapped == ARCH_MMU_STATUS_NO_MEMORY ?
            KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    record->vvar_page = physical;
#endif
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
    *normalized = user_vma_permissions(permissions);
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
    uint64_t first, last;
    int lazy_removed;
};
static void unmap_resident_before_pte(void *owner,uint64_t address,uint64_t physical)
{
    struct mm_unmap_residents *residents=owner;
    if (!residents->first) residents->first = address;
    residents->last = address;
    struct mm_lazy_page *lazy=lazy_find(residents->record,address);
    if (lazy) {
        if (lazy->physical!=physical) __builtin_trap();
        lazy_retire(residents->record,lazy);
        residents->lazy_removed=1;
    }
    struct mm_file_resident **link = find_file_resident_link(residents->record, address);
    if (!link) return;
    struct mm_file_resident *page = *link;
    if (page->physical_address!=physical) __builtin_trap();
    unlink_file_resident(residents->record, link);
    page->next=residents->removed; residents->removed=page;
}
static enum kernel_mm_status unmap_space_range(
    struct kernel_mm_record *record,uint64_t start,uint64_t end)
{
    if (end<=ARCH_MMU_PAGE_SIZE) return KERNEL_MM_STATUS_OK;
    if (start<ARCH_MMU_PAGE_SIZE) start=ARCH_MMU_PAGE_SIZE;
    struct kernel_mm_detach_cursor cursor={0};
    enum arch_mmu_status status;
    enum { DETACH_CHUNKS_PER_LOCK = 2, DETACH_CHUNKS_LARGE = 8 };
    struct kernel_mm_retirement local[DETACH_CHUNKS_PER_LOCK];
    struct kernel_mm_retirement *batches = local;
    unsigned capacity = DETACH_CHUNKS_PER_LOCK;
    unsigned scan_limit = DETACH_CHUNKS_PER_LOCK;
#if BOAROS_MAX_CPUS > 1
    bool can_expand = end - start >= 32U * BOAROS_PAGE_SIZE;
    if (can_expand) scan_limit = DETACH_CHUNKS_LARGE;
#endif
    do {
        struct mm_unmap_residents residents={.record=record};
        uint32_t targets = 0;
        unsigned count = 0, publications = 0;
        int nonleaf = 0;
        uint64_t last_end = 0;
        for (unsigned scanned = 0; scanned < scan_limit;) {
            /* Each short section has its own publication. CPUs joining between
             * sections can cache later leaves and must be included again. */
            unsigned before = count;
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            for (unsigned i = 0; i < DETACH_CHUNKS_PER_LOCK && count < capacity &&
                 scanned < scan_limit; i++, scanned++) {
                struct kernel_mm_retirement *batch = &batches[count];
                batch->count = batch->nonleaf = batch->compound = 0;
                status = arch_mmu_user_detach_range(&record->space, start, end,
                    &cursor, batch, unmap_resident_before_pte, &residents);
                if (status!=ARCH_MMU_STATUS_OK && status!=ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
                if (batch->count) {
                    nonleaf |= batch->nonleaf;
                    last_end = residents.last + ((uint64_t)BOAROS_PAGE_SIZE <<
                        batch->orders[batch->count - 1]);
                    count++;
                }
                if (status == ARCH_MMU_STATUS_NOT_MAPPED) break;
            }
            if (count != before) {
                targets |= note_tlb_change_locked(record, &batches[before], 0);
                publications++;
#if BOAROS_COST_DIAGNOSTICS
                for (unsigned i = before + 1; i < count; i++)
                    record->retired_pages += batches[i].count;
#endif
            }
            if (status == ARCH_MMU_STATUS_NOT_MAPPED) break;
#if BOAROS_MAX_CPUS > 1
            if (can_expand && count == capacity && scanned < scan_limit) {
                /* A large virtual extent can contain very few mapped pages.
                 * Grow only when actual retirement fills the stack batches. */
                can_expand = false;
                kernel_raw_lock_release(&guard);
                void *storage;
                if (!kernel_heap_allocate_nosleep(record->vma_heap,
                    DETACH_CHUNKS_LARGE * sizeof(*batches), &storage)) {
                    memcpy(storage,local,sizeof(local));
                    batches = storage;
                    capacity = DETACH_CHUNKS_LARGE;
                }
            }
#endif
            if (count == capacity) break;
        }
        if (count) {
            sync_tlb_change(targets, nonleaf ? 0 : residents.first,
                nonleaf ? UINT64_MAX : last_end - residents.first, 0);
            for (unsigned i = 0; i < count; i++)
                kernel_mm_retirement_release(record->space.allocator, &batches[i]);
            retirement_complete(record, publications);
        }
        if (residents.lazy_removed) lazy_drain(record);
        while (residents.removed) {
            struct mm_file_resident *pages[KERNEL_MM_RETIRE_CAPACITY];
            unsigned n = 0;
            do {
                pages[n++] = residents.removed;
                residents.removed = residents.removed->next;
            } while (residents.removed && n < KERNEL_MM_RETIRE_CAPACITY);
            retire_file_residents(record, pages, n);
        }
    } while (status==ARCH_MMU_STATUS_OK);
    if (batches != local) kernel_heap_release(record->vma_heap, batches);
    return KERNEL_MM_STATUS_OK;
}

uint64_t kernel_mm_reclaim_lazy(struct physical_page_allocator *allocator,uint64_t target,
                               uint64_t *mappings_detached)
{
    enum { LAZY_RECLAIM_BATCH = 16 };
    *mappings_detached=0;
    struct kernel_lock_guard *held=kernel_io_context_current()->locks;
    if (!target || (held && held->lock->rank>=24)) return 0;
    {
        KERNEL_RAW_SCOPE(guard,&lazy_registry_lock);
        if (!lazy_registry) return 0;
        if (!lazy_scan_initialized) __builtin_trap();
    }
    /* A contender waits for the current traversal, then retries the real
     * allocation. A busy MM gate is waited for only after dropping the scan
     * lock and only when the caller owns neither a pin nor another lock. */
    KERNEL_LOCK_SCOPE(scan);
    uint64_t round,cutoff=0;
    int snapshot_taken=0;
    unsigned remaining;
restart_round:
    kernel_mutex_lock(&lazy_scan_lock,&scan);
    {
        KERNEL_RAW_SCOPE(guard,&lazy_registry_lock);
        if (++lazy_scan_round==0) __builtin_trap();
        round=lazy_scan_round;
        if (!snapshot_taken) {
            cutoff=__atomic_load_n(&lazy_birth_sequence,__ATOMIC_ACQUIRE);
            snapshot_taken=1;
        }
        remaining=lazy_registry_count;
    }
    uint64_t released=0;
    struct kernel_mm_record *waiting=NULL;
    /* A round visits the registry snapshot until it finds work or reaches
     * the end. Each MM qualification inspects at most sixteen candidates. */
    while (remaining-- && released<target && !*mappings_detached) {
        struct kernel_mm_record *record;
        {
            KERNEL_RAW_SCOPE(guard,&lazy_registry_lock);
            record=lazy_registry;
            if (!record || record->lazy_seen_round==round) break;
            lazy_registry=record->lazy_next;
            record->lazy_seen_round=round;
            if (record->lazy_birth>cutoff) continue;
            if (record->lazy_borrows==UINT32_MAX) __builtin_trap();
            record->lazy_borrows++;
        }
        struct mm_lazy_scan_cursor cursor={0};
        int registered=0,busy=0;
        while (record->space.allocator==allocator) {
            KERNEL_LOCK_SCOPE(gate);
            if (!kernel_rwlock_try_write(&record->operations,&gate)) { busy=1;break; }
            int closed=0,more=0;
            {
                KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
                if (record->stage==KERNEL_MM_RECORD_LIVE && !pin_admission_closed(record)) {
                    /* FREE 排空旧写 pin 并撤可写权限；之后写 pin 必须先
                     * 取消候选。已有读 pin 自持物理引用，可跨撤映射存活。 */
                    pin_admission_close(record, 0);
                    closed=1;
                    if (!registered) {
                        if (record->lazy_scan_cursor) __builtin_trap();
                        cursor.next=record->lazy_pages;
                        record->lazy_scan_cursor=&cursor;
                        registered=1;
                    }
                }
            }
            if (closed) {
                unsigned examined=0;
                while (cursor.next && examined++<LAZY_RECLAIM_BATCH &&
                       released<target && *mappings_detached<LAZY_RECLAIM_BATCH) {
                    struct mm_lazy_page *page=cursor.next;
                    cursor.next=page->next;
                    if (page->birth>cutoff) continue;
                    struct kernel_vma vma;
                    uint64_t physical;uint32_t refs;int cow;
                    if (!kernel_vma_set_lookup(record->vmas,page->address,&vma) &&
                        !(vma.flags & KERNEL_VMA_LOCKED) &&
                        !arch_mmu_user_owned_snapshot(&record->space,page->address,&physical,&cow) &&
                        cow && physical==page->physical &&
                        !physical_page_reference_count(allocator,physical,&refs)) {
                        if (unmap_space_range(record,page->address,page->address+BOAROS_PAGE_SIZE)) __builtin_trap();
                        (*mappings_detached)++;
                        /* A shared PTE removal is progress, not a free page. */
                        if (refs==1) released++;
                    }
                }
                {
                    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
                    pin_admission_open(record);
                    more=cursor.next!=NULL;
                }
            }
            mm_gate_release(record,&gate);
            if (closed) wake_pin_waiters(record);
            if (!closed || !more || released || *mappings_detached) break;
        }
        if (registered) {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            if (record->lazy_scan_cursor!=&cursor) __builtin_trap();
            record->lazy_scan_cursor=NULL;
        }
        if (busy && !held && !kernel_io_context_current()->user_pins && !waiting)
            waiting=record; /* The borrow covers the later gate wait. */
        else lazy_borrow_put(record);
    }
    if (waiting) {
        if (!released && !*mappings_detached) {
            /* First inspect other MMs; then let any pinned scanner finish
             * before waiting for the real busy gate. */
            kernel_lock_release(&scan);
            KERNEL_LOCK_SCOPE(wait_gate);
            kernel_rwlock_write(&waiting->operations,&wait_gate);
            mm_gate_release(waiting,&wait_gate);
            lazy_borrow_put(waiting);
            goto restart_round;
        }
        lazy_borrow_put(waiting);
    }
    return released;
}

/* 调用者拥有资源限制；MM只拥有用量。检查和提交都在同一个操作锁内。 */
/* The editor holds the MM writer or an overlapping range claim through
 * PTE removal and VMA publication. Faults cannot enroll against the removed VMA. */
static enum kernel_mm_status unmap_vma_space_range(struct kernel_mm_record *record,
    uint64_t start, uint64_t end)
{
    for (struct kernel_userfault_ctx *ctx = record->userfaults; ctx; ctx = ctx->next)
        kernel_userfault_wake(ctx, start, end);
    return unmap_space_range(record, start, end);
}

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
    uint64_t start, uint64_t end, uint32_t permissions, int shared, int replace, int grows_down)
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
        .kind = shared ? KERNEL_VMA_KIND_ANON_SHARED : KERNEL_VMA_KIND_ANONYMOUS,
        .flags = grows_down ? KERNEL_VMA_GROWSDOWN : 0};
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
    uint64_t *address, struct kernel_memory_object **ring)
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
                   KERNEL_MM_MAP_SHARED | KERNEL_MM_MAP_LOCKED | KERNEL_MM_MAP_GROWSDOWN)) != 0U ||
        (flags & (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) ==
            (KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_FIXED_NOREPLACE)) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & KERNEL_MM_MAP_GROWSDOWN) && (flags & KERNEL_MM_MAP_SHARED))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
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
    if (!may_map_memory(record, start, end, normalized, flags & KERNEL_MM_MAP_SHARED, flags & KERNEL_MM_MAP_FIXED, flags & KERNEL_MM_MAP_GROWSDOWN))
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
        kernel_memory_object_set_mapping_size(shared_object, aligned_length);
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
        .flags = mapping_lock_flags(record, flags) | (ring ? KERNEL_VMA_DONTFORK | KERNEL_VMA_AIO_RING : 0U) |
            ((flags & KERNEL_MM_MAP_GROWSDOWN) ? KERNEL_VMA_GROWSDOWN : 0),
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
                register_shared_anon(shared_entry,record);
            }
            if (ring) {
                if (kernel_memory_object_acquire(shared_object)) __builtin_trap();
                *ring = shared_object;
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
    status = unmap_vma_space_range(record, start, end);
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
        register_shared_anon(shared_entry,record);
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
    struct kernel_mm_file_source *prepared, int borrow_caller)
{
    if (prepared != 0) {
        /* Object mmap keeps the caller pin through admission-lock release.
         * Its source therefore needs an independent OFD owner. */
        if (borrow_caller && kernel_open_file_acquire(*file)) __builtin_trap();
        prepared->file = *file;
        prepared->next = record->file_sources;
        record->file_sources = prepared;
    } else if (!borrow_caller) {
        /* An existing source plus this caller pin is never the last ref. */
        if (kernel_open_file_release(file) !=
            KERNEL_OPEN_FILE_STATUS_OK) {
            return;
        }
    }
    if (!borrow_caller) *file = 0;
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

    enum kernel_mm_status status=prepare_file_mapping(mm,
                                hint,
                                length,
                                file_offset,
                                permissions,
                                flags,
                                &plan);
    /* Linux先检查锁页接纳，再检查FD权限；提交仍重新核对当前额度。 */
    return status ? status : admit_mapping_lock(plan.record,plan.start,plan.end,flags);
}

static enum kernel_mm_status allocate_private_page(struct physical_page_allocator *,uint64_t *,void **);

/* The exclusive MM operation covers installation and VMA publication. Table
 * allocation is outside raw metadata protection; each PTE takes one map-page
 * reference. The caller retires any partially installed range on failure. */
static enum kernel_mm_status populate_object_mapping(struct kernel_mm_record *record,
    struct kernel_open_file_description *file,uint64_t offset,
    const struct mm_file_mapping_plan *plan,const struct kernel_mm_mapped_object *object)
{
    struct physical_page_allocator *allocator=record->space.allocator;
    struct kernel_mm_retirement batch={0};
    uint32_t targets=0;
    int changed=0;
    enum kernel_mm_status status=KERNEL_MM_STATUS_OK;
    for (uint64_t address=plan->start;address<plan->end;address+=BOAROS_PAGE_SIZE) {
        unsigned missing;
        {
            KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
            if (arch_mmu_user_leaf_missing(&record->space,address,&missing)) __builtin_trap();
        }
        struct kernel_mm_leaf_preparation prepared=KERNEL_MM_LEAF_PREPARATION_INIT;
        uint64_t physical=KERNEL_MM_NO_PAGE;
        int error=object->acquire_page(file,offset+address-plan->start,
            !!(plan->permissions & KERNEL_MM_WRITE),&physical);
        status=!error ? KERNEL_MM_STATUS_OK : error==-KERNEL_ENOMEM ?
            KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_BUS_FAULT;
        for (unsigned i=0;i<2 && !status;i++) if (missing & (1U<<i)) {
            void *table;
            status=allocate_private_page(allocator,&prepared.tables[i],&table);
            if (!status) clear_page(table);
        }
        if (!status) {
            KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
            if (arch_mmu_user_map_prepared_page(&record->space,address,physical,
                    backend_permissions_from_mm(plan->permissions),0,&prepared)) __builtin_trap();
            physical=KERNEL_MM_NO_PAGE;
            targets|=note_tlb_change_locked(record,&batch,0);
            changed=1;
        }
        if (physical!=KERNEL_MM_NO_PAGE && physical_page_release(allocator,physical)) __builtin_trap();
        for (unsigned i=0;i<2;i++) if (prepared.tables[i]!=KERNEL_MM_NO_PAGE &&
            physical_page_release(allocator,prepared.tables[i])) __builtin_trap();
        if (status) break;
    }
    if (changed) finish_tlb_change(record,&batch,targets,0,UINT64_MAX,0);
    return status;
}

static enum kernel_mm_status commit_file_mapping(
    struct kernel_open_file_description **file,
    uint64_t file_offset,
    uint32_t flags,
    uint64_t *address,
    const struct mm_file_mapping_plan *input,
    struct kernel_lock_guard *inode,const struct kernel_mm_mapped_object *object)
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
                        flags & KERNEL_MM_MAP_SHARED, flags & KERNEL_MM_MAP_FIXED, 0)) return KERNEL_MM_STATUS_NO_MEMORY;
    status = admit_mapping_lock(record, plan.start, plan.end, flags);
    if (status) return status;
    int may_write=object ? object->may_write : kernel_open_file_writable(*file),sealed=0;
    uint64_t *mapping_bytes=object ? object->mapping_bytes : NULL;
    if (!object && (flags&KERNEL_MM_MAP_SHARED) && may_write) {
        if (kernel_vfs_node_attributes(kernel_open_file_node(*file))&KERNEL_VFS_INODE_APPEND)
            return KERNEL_MM_STATUS_ACCESS;
        mapping_bytes=kernel_vfs_node_shared_write_counter(kernel_open_file_node(*file),&sealed);
        if (sealed) {
            if (plan.permissions&KERNEL_MM_WRITE) return KERNEL_MM_STATUS_SEALED;
            may_write=0;
            mapping_bytes=NULL;
        }
    }
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
        .flags = mapping_lock_flags(record, flags) | (object ? KERNEL_VMA_DONTDUMP : 0),
        .backing_offset = file_offset,
        .permissions = plan.permissions,
        .kind = object ? KERNEL_VMA_KIND_OBJECT_SHARED : (flags & KERNEL_MM_MAP_SHARED) != 0U
                    ? KERNEL_VMA_KIND_FILE_SHARED
                    : KERNEL_VMA_KIND_FILE_PRIVATE,
        .role = KERNEL_VMA_ROLE_MMAP,
        .fault_policy = object ? KERNEL_VMA_FAULT_OBJECT_SHARED : (flags & KERNEL_MM_MAP_SHARED) != 0U
                            ? KERNEL_VMA_FAULT_FILE_SHARED
                            : KERNEL_VMA_FAULT_FILE_PRIVATE,
        .backing = *file,
        .backing_owner = prepared ? prepared : existing,
        .shared_mapping_bytes=mapping_bytes,
        .maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS &
            (((flags & KERNEL_MM_MAP_SHARED) == 0U || may_write)
                 ? KERNEL_VMA_ALL_PERMISSIONS : ~KERNEL_MM_WRITE) &
            ((object || kernel_open_file_mapping_noexec(*file)) ? ~KERNEL_MM_EXECUTE : KERNEL_VMA_ALL_PERMISSIONS),
    };
    if (object) {
        vma_status=kernel_vma_set_prepare_replace(record->vmas,plan.start,plan.end,&vma,&edit);
        if (vma_status) {
            discard_prepared_file_source(record,prepared);
            return status_from_vma(vma_status);
        }
        if (unmap_vma_space_range(record,plan.start,plan.end)) __builtin_trap();
        status=populate_object_mapping(record,*file,file_offset,&plan,object);
        if (status) {
            /* Replacement reserved enough VMA capacity for removal as well.
             * A failed MAP_FIXED leaves a hole, never an unpopulated object VMA. */
            if (unmap_vma_space_range(record,plan.start,plan.end)) __builtin_trap();
            edit.kind=KERNEL_VMA_EDIT_REMOVE;
        }
        if (kernel_vma_set_commit_edit(record->vmas,&edit)) __builtin_trap();
        if (status) {
            discard_prepared_file_source(record,prepared);
            drain_shared_anon(record,1);
            (void)drain_file_sources(record,1);
            (void)drain_elf_sources(record,1);
            return status;
        }
    } else if ((flags & KERNEL_MM_MAP_FIXED) == 0U) {
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
        status = unmap_vma_space_range(record, plan.start, plan.end);
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
    finish_file_mapping(record, file, prepared, object!=NULL);
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
    status = unmap_vma_space_range(record, address, end);
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

static enum kernel_mm_status mmap_hugetlb(struct kernel_mm *, uint64_t, uint64_t,
    uint32_t, uint32_t, int, int, uint64_t *, struct kernel_shm_segment *, uint32_t);

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
    if (segment && segment->hugetlb) {
        uint32_t map_flags = 0;
        if (hint) {
            if (flags & KERNEL_SHM_RND) hint &= ~BOAROS_PAGE_MASK;
            map_flags = (flags & KERNEL_SHM_REMAP) ? KERNEL_MM_MAP_FIXED : KERNEL_MM_MAP_FIXED_NOREPLACE;
        } else if (flags & KERNEL_SHM_REMAP) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
        return mmap_hugetlb(mm, hint, segment->aligned_size, permissions, map_flags,
            1, 0, out_address, segment, flags);
    }
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
    if (!may_map_memory(record, start, end, normalized, 1, flags & KERNEL_SHM_REMAP, 0)) return KERNEL_MM_STATUS_NO_MEMORY;
    status = admit_mapping_lock(record, start, end, (flags & KERNEL_SHM_REMAP) ? KERNEL_MM_MAP_FIXED : 0);
    if (status) return status;
    if (kernel_shm_attachment_create(segment, start, end, 0, &attachment) !=
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
        .flags = record->future_lock_flags,
        .kind = KERNEL_VMA_KIND_SYSV_SHM,
        .role = KERNEL_VMA_ROLE_SYSV_SHM,
        .fault_policy = KERNEL_VMA_FAULT_ANON_SHARED,
        .backing = segment->memory,
        .backing_owner = attachment,
    };

    if ((flags & KERNEL_SHM_REMAP) == 0U) {
        status = status_from_vma(kernel_vma_set_insert(record->vmas, &vma));
        if (status == KERNEL_MM_STATUS_OK) {
            if (shared_entry != 0) {
                shared_entry->next = record->shared_anon;
                record->shared_anon = shared_entry;
                register_shared_anon(shared_entry,record);
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
    status = unmap_vma_space_range(record, start, end);
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
        register_shared_anon(shared_entry,record);
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
    if (mm == 0 || (address & BOAROS_PAGE_MASK) || address >= ARCH_MMU_USER_LIMIT)
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
        struct kernel_shm_attachment *candidate = kernel_vma_shm_attachment(&vma);
        if (candidate && vma.start >= vma.backing_offset &&
            vma.start - vma.backing_offset == address) {
            attachment = candidate;
            kernel_shm_attachment_acquire(attachment);
            break;
        }
    }
    if (!attachment) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    uint64_t file_identity = attachment->file_identity;
    uint64_t segment_size = attachment->segment->aligned_size;
    int first = 1;
    /* mremap 可产生多个附件；只撤同一 shmat 文件和偏移关系匹配的片段。 */
    for (uint32_t i = 0; i < kernel_vma_set_count(record->vmas);) {
        if (kernel_vma_set_get_at(record->vmas, i, &vma) != KERNEL_VMA_STATUS_OK)
            __builtin_trap();
        struct kernel_shm_attachment *candidate = kernel_vma_shm_attachment(&vma);
        if (vma.start < address || vma.start - address != vma.backing_offset ||
            !candidate || candidate->file_identity != file_identity ||
            (!first && vma.end - address > segment_size)) { i++; continue; }
        status = mm_munmap_impl(mm, vma.start, vma.end - vma.start);
        if (status != KERNEL_MM_STATUS_OK) return status;
        first = 0;
        i = 0; /* 后备清理可睡眠，重取集合而不保存数组下标。 */
    }
    return KERNEL_MM_STATUS_OK;
}

static uint32_t protection_for_leaf(struct kernel_mm_record *record, uint64_t leaf, uint32_t permissions)
{
    if (!(permissions & KERNEL_MM_WRITE)) return permissions;
    struct mm_file_resident *resident=find_file_resident(record,leaf);
    if (resident) {
        COST_ADD(MPROTECT_RESIDENT_VISITS, 1);
        struct mm_file_alias_resident *shared = file_alias_resident(resident);
        if (shared && kernel_page_cache_alias_active(&shared->alias))
            permissions = user_effective_permissions(permissions) & ~KERNEL_MM_WRITE;
    }
    return permissions;
}

static enum kernel_mm_status protect_vma_pages(struct kernel_mm_record *record,
    const struct kernel_vma_edit *edit)
{
    uint64_t address = edit->start, end = edit->end;
    uint32_t normalized = edit->permissions;
    struct kernel_mm_retirement batch;
    batch.count = 0;
    uint32_t targets;
    int instructions = (normalized & KERNEL_MM_EXECUTE) != 0;
    if (end-address==BOAROS_PAGE_SIZE) {
        /* One base page cannot cover a large leaf. The backend performs the
         * lookup and update together and reports a hole without a fence.
         * Capture the invalidation targets before dropping this guard. */
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        uint32_t leaf_permissions=protection_for_leaf(record,address,normalized);
        enum arch_mmu_status status=arch_mmu_user_protect_resident_page(&record->space,address,
            backend_permissions_from_mm(leaf_permissions));
        if (status==ARCH_MMU_STATUS_NOT_MAPPED) return KERNEL_MM_STATUS_OK;
        if (status) __builtin_trap();
        targets=note_tlb_change_locked(record,&batch,instructions);
    } else {
        uint64_t cursor = address, leaf;
        int touched=0;
        for (;;) {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            enum arch_mmu_status status=arch_mmu_user_next_owned(&record->space,&cursor,end,&leaf);
            if (status==ARCH_MMU_STATUS_NOT_MAPPED) break;
            if (status) __builtin_trap();
            touched=1;
            uint32_t leaf_permissions=protection_for_leaf(record,leaf,normalized);
            struct kernel_mm_leaf snapshot;
            if (arch_mmu_user_leaf_snapshot(&record->space,leaf,&snapshot)) __builtin_trap();
            if (snapshot.order) {
                if (leaf<address || leaf+kernel_mm_leaf_size(&snapshot)>end ||
                    arch_mmu_user_protect_owned_range(&record->space,leaf,leaf+kernel_mm_leaf_size(&snapshot),
                        backend_permissions_from_mm(leaf_permissions))) __builtin_trap();
            } else if (arch_mmu_user_protect_owned_page(&record->space, leaf,
                    backend_permissions_from_mm(leaf_permissions))) __builtin_trap();
        }
        if (!touched) return KERNEL_MM_STATUS_OK;
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        targets=note_tlb_change_locked(record,&batch,instructions);
    }
    finish_tlb_change(record,&batch,targets,address,end-address,instructions);
    return KERNEL_MM_STATUS_OK;
}
static enum kernel_mm_status protect_vma_range(
    struct kernel_mm_record *record, const struct kernel_vma_edit *edit)
{
    enum kernel_mm_status status=protect_vma_pages(record,edit);
    if (status) return status;
    {
        struct kernel_lock_guard vma_guard = {0};
        kernel_rwlock_write(&record->vma_lock, &vma_guard);
        enum kernel_vma_status vs = kernel_vma_set_commit_edit(record->vmas, edit);
        kernel_lock_release(&vma_guard);
        if (vs != KERNEL_VMA_STATUS_OK) return KERNEL_MM_STATUS_STATE;
    }
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
    {
        struct kernel_lock_guard vma_guard = {0};
        kernel_rwlock_write(&record->vma_lock, &vma_guard);
        status = status_from_vma(kernel_vma_set_prepare_mprotect(record->vmas,
            address, address + aligned_length, normalized, &edit));
        kernel_lock_release(&vma_guard);
    }
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
                .flags = record->future_lock_flags,
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
    status = unmap_vma_space_range(record, new_page_end, old_page_end);
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

    if ((user_effective_permissions(permissions) & KERNEL_MM_READ) != 0U) {
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
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
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
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
    return mm->state==KERNEL_MM_LIVE &&
        record->vma_heap==heap && mm->allocator==heap->page_allocator
        ? KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_STATE;
}

#if !defined(BOAROS_ARCH_LOONGARCH)
enum kernel_mm_status kernel_mm_flush_instructions(struct kernel_mm *mm, int local_only)
{
    struct kernel_mm owner={0};
    if (!mm) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status=kernel_mm_acquire(&owner,mm);
    if (status) return status;
    struct kernel_mm_record *record=owner.record;
    /* 固定本核身份直到固件确认；快照后的新进入者由activate核对代次。 */
    kernel_preempt_disable();
    struct kernel_cpu *cpu=kernel_cpu_current();
    uint32_t targets;
    uint64_t generation;
    {
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        generation=mm_generation_advance(&record->instruction_generation);
        targets=local_only?0:mm_sync_targets(record,0);
    }
    arch_mmu_sync_instruction_cpus(targets);
    /* 远端可在等待期间发布更新代次，不能把它冒充为本次已同步。 */
    if (cpu->active_mm_record==record) {
        unsigned logical = 0;
#if BOAROS_MAX_CPUS > 1
        logical = cpu->logical_id;
#endif
        record->instruction_seen[logical] = generation;
    }
    kernel_preempt_enable();
    if (kernel_mm_release(&owner)) __builtin_trap();
    return KERNEL_MM_STATUS_OK;
}
#endif

static void release_borrowed_root(struct kernel_cpu *cpu)
{
    struct physical_page_allocator *allocator=cpu->borrowed_mm_allocator;
    if (!allocator) return;
    struct kernel_mm_record *record=cpu->borrowed_mm_record;
    uint64_t pages[2]={cpu->borrowed_mm_root,cpu->borrowed_mm_record_page};
    unsigned logical=0;
#if BOAROS_MAX_CPUS > 1
    logical=cpu->logical_id;
#endif
    if (!(__atomic_fetch_and(&record->borrowed_cpus,~(1U<<logical),__ATOMIC_SEQ_CST) & (1U<<logical)))
        __builtin_trap();
    cpu->borrowed_mm_allocator=NULL;cpu->borrowed_mm_root=0;
    cpu->borrowed_mm_record=NULL;cpu->borrowed_mm_record_page=0;
    cpu->borrowed_mm_context=cpu->borrowed_mm_identity=0;
    cpu->borrowed_mm_generation=cpu->borrowed_mm_kernel_context=0;
    physical_page_release_batch(allocator,pages,2);
}

static enum kernel_mm_status activate_mm(const struct kernel_mm *next,
    uint64_t kernel_context,int borrow_kernel_root)
{
#if defined(BOAROS_ARCH_LOONGARCH)
    (void)borrow_kernel_root;
#endif
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
        int joining = record != previous;
#if BOAROS_MAX_CPUS == 1
        struct kernel_raw_guard guard __attribute__((cleanup(kernel_raw_scope_release))) = {0};
        if (joining) kernel_raw_rwlock_write_acquire(&record->metadata, &guard);
#endif
        /* next的实际MM引用固定root；活动成员只发布本CPU的进入/离开。 */
        uint64_t context;
        if (arch_mmu_user_space_context(&record->space, &context) != ARCH_MMU_STATUS_OK) {
            status = KERNEL_MM_STATUS_ADDRESS_SPACE; goto out;
        }
#if BOAROS_MAX_CPUS > 1
        if (joining && (__atomic_fetch_or(&record->active_cpus, 1U << logical,
            __ATOMIC_SEQ_CST) & (1U << logical))) __builtin_trap();
#endif
        /* 置位后捕获待同步代次，再切root/本地失效；不能在flush后记录
         * 并发LOCAL指令发布的新代次，否则该CPU会漏掉下次FENCE.I。 */
        uint64_t generation = mm_generation_read(&record->tlb_generation);
        uint64_t instructions = mm_generation_read(&record->instruction_generation);
#if BOAROS_MAX_CPUS == 1
        if (joining) {
            if (record->active_cpus & (1U << logical)) __builtin_trap();
            record->active_cpus |= 1U << logical;
        }
#endif
        int changed = arch_mmu_current_context() != context;
        if (arch_mmu_switch_context(context) != ARCH_MMU_STATUS_OK) __builtin_trap();
        /* A root switch already flushed translations. Record only captured
         * generations; a concurrent newer publication needs its own sync. */
        int resumed=0;
#if !defined(BOAROS_ARCH_LOONGARCH)
        resumed=cpu->borrowed_mm_allocator && cpu->borrowed_mm_context==context &&
            cpu->borrowed_mm_identity==record->futex_id &&
            cpu->borrowed_mm_generation==generation && !arch_mmu_new_translation_needs_sync();
#endif
        if (!changed && !resumed && (joining || cpu->active_mm_generation != generation)) arch_mmu_flush_all();
        if (record->instruction_seen[logical] != instructions) {
            COST_ADD(MM_ACTIVATE_INSTRUCTION_FLUSHES, 1);
            arch_mmu_sync_instructions();
            /* Only the captured generation was completed. A concurrent newer
             * publication remains pending even if it raced with this fence. */
            record->instruction_seen[logical] = instructions;
        }
        cpu->active_mm_record = record;
        cpu->active_mm_record_page=next->record_page_address;
        cpu->active_mm_generation = generation;
    }
#if !defined(BOAROS_ARCH_LOONGARCH)
    else if (borrow_kernel_root && (previous || cpu->borrowed_mm_allocator)) {
        if (previous) {
            if (cpu->borrowed_mm_allocator) __builtin_trap();
            /* Publish the page owner before clearing the old active bit. A
             * final MM cleanup can then find the root's extra reference. */
            uint64_t pages[2]={previous->space.root_address,cpu->active_mm_record_page};
            physical_page_acquire_batch(previous->space.allocator,pages,2);
            cpu->borrowed_mm_root=previous->space.root_address;
            cpu->borrowed_mm_context=arch_mmu_current_context();
            cpu->borrowed_mm_identity=previous->futex_id;
            cpu->borrowed_mm_generation=cpu->active_mm_generation;
            cpu->borrowed_mm_kernel_context=kernel_context;
            cpu->borrowed_mm_record=previous;
            cpu->borrowed_mm_record_page=cpu->active_mm_record_page;
            cpu->borrowed_mm_allocator=previous->space.allocator;
            if (__atomic_fetch_or(&previous->borrowed_cpus,1U<<logical,__ATOMIC_SEQ_CST) & (1U<<logical))
                __builtin_trap();
        }
        cpu->active_mm_record=NULL;cpu->active_mm_generation=0;cpu->active_mm_record_page=0;
    }
#endif
    else {
        if (arch_mmu_switch_context(kernel_context) != ARCH_MMU_STATUS_OK) {
            status = KERNEL_MM_STATUS_ADDRESS_SPACE; goto out;
        }
        cpu->active_mm_record = 0;
        cpu->active_mm_record_page=0;
        cpu->active_mm_generation = 0;
    }
    if (previous && previous != record) {
#if BOAROS_MAX_CPUS > 1
        /* 旧root已停止使用并全量失效；先前采到本CPU的修改者仍须等其确认。 */
        if (!(__atomic_fetch_and(&previous->active_cpus, ~(1U << logical),
            __ATOMIC_SEQ_CST) & (1U << logical))) __builtin_trap();
#else
        KERNEL_RAW_WRITE_SCOPE(guard, &previous->metadata);
        if (!(previous->active_cpus & (1U << logical))) __builtin_trap();
        previous->active_cpus &= ~(1U << logical);
#endif
    }
    if (next || !borrow_kernel_root) release_borrowed_root(cpu);
out:
    arch_interrupt_restore(irq);
    return status;
}

unsigned kernel_mm_membarrier_registrations(const struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record_storage(mm, &record)) __builtin_trap();
    return __atomic_load_n(&record->membarrier_registrations, __ATOMIC_ACQUIRE);
}

void kernel_mm_membarrier_register(struct kernel_mm *mm, unsigned command)
{
    struct kernel_mm_record *record;
    if (resolve_record_storage(mm, &record)) __builtin_trap();
    __atomic_fetch_or(&record->membarrier_registrations, command, __ATOMIC_RELEASE);
}

unsigned kernel_mm_membarrier_active(const struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record_storage(mm, &record)) __builtin_trap();
    return __atomic_load_n(&record->active_cpus, __ATOMIC_SEQ_CST);
}

enum kernel_mm_status kernel_mm_activate(const struct kernel_mm *next,uint64_t kernel_context)
{ return activate_mm(next,kernel_context,0); }

enum kernel_mm_status kernel_mm_activate_for_task(const struct kernel_mm *next,uint64_t kernel_context)
{ return activate_mm(next,kernel_context,1); }

enum kernel_mm_status kernel_mm_release_for_task(struct kernel_mm *mm,const struct kernel_task *task)
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
        /* This CPU may be the kernel executor that inherited the dying root.
         * Leave it before the backend detaches and frees user page tables. */
        struct kernel_cpu *cpu=kernel_cpu_current();
        if (cpu->borrowed_mm_allocator==mm->allocator &&
            cpu->borrowed_mm_root==record->space.root_address &&
            kernel_mm_activate(NULL,cpu->borrowed_mm_kernel_context)) __builtin_trap();
        lazy_unregister(record);
        KERNEL_LOCK_SCOPE(retire);
        kernel_rwlock_write(&record->operations,&retire);
        if (pin_state_read(&record->access_pins) || pin_state_read(&record->write_pins) || mm_active_cpus(record) ||
            record->pending_retirement || record->retired_file_mappings || record->retired_shared_anon || record->retired_huge_maps) __builtin_trap();
        if (record->vmas == 0 && !record->huge_maps && record->shared_anon == 0 &&
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
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            while (record->lazy_pages) lazy_retire(record,record->lazy_pages);
        }
        lazy_drain(record);
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
            vma_status = kernel_vma_set_destroy_for_task(&record->vmas,task);
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
        if (record->executable_file) {
            enum kernel_open_file_status closed=kernel_open_file_release(&record->executable_file);
            if (closed==KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED)
                return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
            if (closed) __builtin_trap();
        }
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
#if KERNEL_HAS_VDSO_ELF
    if (record->vdso_address) kernel_time_vvar_release(mm->allocator, record->vvar_page);
#endif
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
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
    return record->space.peak_resident_pages;
}

void kernel_mm_get_statistics(const struct kernel_mm *mm,
                                    struct kernel_mm_statistics *statistics)
{
    struct kernel_mm_record *record;
    if (statistics == 0 || resolve_record(mm, &record) != KERNEL_MM_STATUS_OK)
        __builtin_trap();
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
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
    KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
    *state = (struct kernel_mm_concurrency_state){
        .references = __atomic_load_n(&record->references, __ATOMIC_ACQUIRE),
        .users = __atomic_load_n(&record->users, __ATOMIC_ACQUIRE),
        .pins = (uint32_t)pin_state_read(&record->access_pins),
        .write_pins = (uint32_t)pin_state_read(&record->write_pins),
        .active_cpus = mm_active_cpus(record), .pending_retirement = record->pending_retirement,
        .record_bytes = sizeof(*record), .pin_bytes = sizeof(struct kernel_mm_user_pin),
        .generation = record->tlb_generation, .pin_acquisitions = record->pin_acquisitions,
        .prepared_pages = record->prepared_pages, .fault_retries = record->fault_retries,
        .retirement_batches = record->retirement_batches, .retired_pages = record->retired_pages,
        .pin_waits = record->pin_waits,
        .lazy_pages = record->lazy_count, .lazy_index_bytes = record->lazy_capacity*sizeof(*record->lazy_buckets),
    };
    { KERNEL_RAW_SCOPE(registry,&lazy_registry_lock);
      state->lazy_registered=record->lazy_registered;state->lazy_borrows=record->lazy_borrows; }
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
            (kernel_vma_set_has_shm(operation->record->vmas, address, length) ||
             kernel_vma_set_has_shm(operation->record->vmas, second_address, second_length))) {
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

static enum kernel_hugetlb_status revoke_huge_child(void *owner,
    struct kernel_hugetlb_map *map,uint64_t index,uint64_t physical)
{
    struct mm_huge_map *entry=owner;
    if (entry->map!=map) __builtin_trap();
    if (current_pins(&entry->mm)) return KERNEL_HUGETLB_BUSY;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end)))={0};
    enum kernel_mm_status status=mm_operation_begin(&entry->mm,&operation);
    if (status) return status==KERNEL_MM_STATUS_CONFLICT?KERNEL_HUGETLB_BUSY:KERNEL_HUGETLB_OK;
    struct kernel_mm_record *record=operation.record;
    uint64_t offset=index*ARCH_MMU_HUGE_SIZE;
    if (!record->vmas) return KERNEL_HUGETLB_OK;
    for (uint32_t i=0;i<kernel_vma_set_count(record->vmas);i++) {
        struct kernel_vma v;
        if (kernel_vma_set_get_at(record->vmas,i,&v)) __builtin_trap();
        if (v.backing!=map || offset<v.backing_offset || offset-v.backing_offset>=v.end-v.start) continue;
        uint64_t address=v.start+offset-v.backing_offset;
        struct kernel_mm_leaf leaf;
        {
            KERNEL_RAW_READ_SCOPE(raw,&record->metadata);
            enum arch_mmu_status found=arch_mmu_user_leaf_snapshot(&record->space,address,&leaf);
            if (found==ARCH_MMU_STATUS_NOT_MAPPED) continue;
            if (found) __builtin_trap();
            if (leaf.physical_address!=physical) continue;
        }
        close_record_pins(record,0);operation.pins_closed=1;
        if (unmap_space_range(record,address,address+ARCH_MMU_HUGE_SIZE)) __builtin_trap();
        if (kernel_hugetlb_map_revoke(map,index)) __builtin_trap();
    }
    return KERNEL_HUGETLB_OK;
}
static void prepare_huge_registration(struct mm_huge_map *entry)
{
    entry->registration=(struct kernel_hugetlb_mapping){.map=entry->map,
        .owner=entry,.revoke=revoke_huge_child};
}

static enum kernel_mm_status mmap_hugetlb(struct kernel_mm *mm,
    uint64_t hint,uint64_t length,uint32_t permissions,uint32_t flags,
    int shared,int noreserve,uint64_t *address, struct kernel_shm_segment *segment,
    uint32_t shm_flags)
{
    KERNEL_NO_RECLAIM_IO;
    uint32_t normalized;
    uint64_t mask=ARCH_MMU_HUGE_SIZE-1;
    if (!address || !length || (shared!=0 && shared!=1) || (noreserve!=0 && noreserve!=1) ||
        !normalize_user_permissions(permissions,&normalized) ||
        (flags & ~(KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_FIXED_NOREPLACE|KERNEL_MM_MAP_LOCKED|KERNEL_MM_MAP_SHARED)) ||
        ((flags & KERNEL_MM_MAP_FIXED) && (flags & KERNEL_MM_MAP_FIXED_NOREPLACE)))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (length>UINT64_MAX-mask) return KERNEL_MM_STATUS_NO_MEMORY;
    length=(length+mask)&~mask;
    MM_RANGE_OPERATION(mm,hint,(flags&KERNEL_MM_MAP_FIXED)?length:0);
    struct kernel_mm_record *record=operation.record;
    if (!record->vmas || !record->vma_heap) return KERNEL_MM_STATUS_STATE;
    uint64_t start=0;
    if (flags&(KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_FIXED_NOREPLACE)) {
        if ((hint&mask) || hint<ARCH_MMU_HUGE_SIZE || length>ARCH_MMU_USER_LIMIT-hint || hint>=ARCH_MMU_USER_LIMIT)
            return KERNEL_MM_STATUS_INVALID_ARGUMENT;
        start=hint;
        int overlaps;
        if ((flags&KERNEL_MM_MAP_FIXED_NOREPLACE) &&
            (kernel_vma_set_overlaps(record->vmas,start,start+length,&overlaps) || overlaps))
            return KERNEL_MM_STATUS_CONFLICT;
    } else {
        hint&=~mask;
        int overlaps=1;
        if (hint>=ARCH_MMU_HUGE_SIZE && hint<record->mmap_base && length<=record->mmap_base-hint &&
            !kernel_vma_set_overlaps(record->vmas,hint,hint+length,&overlaps) && !overlaps) {
            struct kernel_vma next;
            enum kernel_vma_status found=kernel_vma_set_next(record->vmas,hint+length,&next);
            uint64_t gap=KERNEL_VMA_STACK_GUARD_PAGES*BOAROS_PAGE_SIZE;
            uint64_t boundary=found?record->mmap_base:kernel_vma_grows_down(&next)?
                (next.start>gap?next.start-gap:0):next.start;
            if (hint+length<=boundary) start=hint;
        }
        if (!start) {
            uint64_t upper=record->mmap_base & ~mask;
            for (uint32_t i=kernel_vma_set_count(record->vmas);i>0;) {
                struct kernel_vma v;
                if (kernel_vma_set_get_at(record->vmas,--i,&v)) __builtin_trap();
                if (upper>=length && upper-length>=v.end && upper-length>=ARCH_MMU_HUGE_SIZE) { start=upper-length;break; }
                uint64_t gap=KERNEL_VMA_STACK_GUARD_PAGES*BOAROS_PAGE_SIZE;
                uint64_t boundary=kernel_vma_grows_down(&v)?(v.start>gap?v.start-gap:0):v.start;
                if (boundary<upper) upper=boundary & ~mask;
            }
            if (!start && upper>=length && upper-length>=ARCH_MMU_HUGE_SIZE) start=upper-length;
        }
        if (!start) return KERNEL_MM_STATUS_NO_MEMORY;
    }
    uint64_t end=start+length;
    if (!may_map_memory(record,start,end,normalized,shared,flags&KERNEL_MM_MAP_FIXED,0))
        return KERNEL_MM_STATUS_NO_MEMORY;
    enum kernel_mm_status locked=admit_mapping_lock(record,start,end,flags);
    if (locked) return locked;
    struct kernel_shm_attachment *attachment
        __attribute__((cleanup(kernel_shm_attachment_release))) = 0;
    if (segment && kernel_shm_attachment_create(segment,start,end,0,&attachment))
        return KERNEL_MM_STATUS_NO_MEMORY;
    struct mm_huge_map *entry = 0;
    if (segment) {
        for (entry=record->huge_maps; entry; entry=entry->next)
            if (entry->map==segment->hugetlb) break;
    }
    int new_entry = !entry;
    if (new_entry) {
        if (kernel_heap_allocate_zeroed(record->vma_heap,1,sizeof(*entry),(void **)&entry))
            return KERNEL_MM_STATUS_NO_MEMORY;
        enum kernel_hugetlb_status hs = KERNEL_HUGETLB_OK;
        if (segment) {
            entry->map=segment->hugetlb;
            kernel_hugetlb_map_acquire(entry->map);
        } else {
            struct kernel_hstate *h = 0;
            hs=kernel_hstate_get_or_create(record->vma_heap,mm->allocator,ARCH_MMU_HUGE_ORDER,&h);
            if (!hs) hs=kernel_hugetlb_map_create(h,length/ARCH_MMU_HUGE_SIZE,
                shared?KERNEL_HUGETLB_SHARED:KERNEL_HUGETLB_PRIVATE,noreserve,&entry->map);
            if (h) kernel_hstate_put(h);
        }
        if (hs) { kernel_heap_release(record->vma_heap,entry);return huge_status(hs); }
        entry->pages=length/ARCH_MMU_HUGE_SIZE;entry->mm=*mm;
        prepare_huge_registration(entry);
    }
    struct kernel_vma v={.start=start,.end=end,.permissions=normalized,
        .maximum_permissions=KERNEL_VMA_ALL_PERMISSIONS &
            ((shm_flags & KERNEL_SHM_RDONLY) ? ~KERNEL_MM_WRITE : KERNEL_VMA_ALL_PERMISSIONS),
        .kind=shared?KERNEL_VMA_KIND_HUGETLB_SHARED:KERNEL_VMA_KIND_HUGETLB_PRIVATE,
        .role=segment?KERNEL_VMA_ROLE_SYSV_SHM:KERNEL_VMA_ROLE_MMAP,.backing_owner=attachment,.fault_policy=KERNEL_VMA_FAULT_HUGETLB,.backing=entry->map,
        .flags=mapping_lock_flags(record,flags)};
    enum kernel_mm_status status;
    if (flags&KERNEL_MM_MAP_FIXED) {
        struct kernel_vma_edit edit;
        status=status_from_vma(kernel_vma_set_prepare_replace(record->vmas,start,end,&v,&edit));
        if (!status) status=unmap_vma_space_range(record,start,end);
        if (!status) status=status_from_vma(kernel_vma_set_commit_edit(record->vmas,&edit));
    } else status=status_from_vma(kernel_vma_set_insert(record->vmas,&v));
    if (status) {
        if (new_entry) { kernel_hugetlb_map_release(&entry->map);kernel_heap_release(record->vma_heap,entry); }
        return status;
    }
    if (new_entry) {
        entry->next=record->huge_maps;record->huge_maps=entry;
        if (kernel_hugetlb_mapping_register(&entry->registration,0)) __builtin_trap();
    }
    drain_shared_anon(record,1);
    (void)drain_file_sources(record,1);(void)drain_elf_sources(record,1);
    *address=start;
    uint32_t policy=v.flags & KERNEL_VMA_LOCK_MASK;
    mm_operation_end(&operation);
    if (policy==KERNEL_VMA_LOCKED) (void)populate_locked(mm,start,end);
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_mmap_hugetlb(struct kernel_mm *mm,
    uint64_t hint,uint64_t length,uint32_t permissions,uint32_t flags,
    int shared,int noreserve,uint64_t *address)
{
    return mmap_hugetlb(mm,hint,length,permissions,flags,shared,noreserve,address,0,0);
}

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

enum kernel_mm_status kernel_mm_mmap_object_shared(struct kernel_mm *mm,
    struct kernel_open_file_description *file,uint64_t hint,uint64_t length,
    uint64_t offset,uint32_t permissions,uint32_t flags,const struct kernel_mm_mapped_object *object,
    uint64_t *address)
{
    if (!file || !address || !object || !object->acquire_page ||
        !(flags & KERNEL_MM_MAP_SHARED) || (permissions & KERNEL_MM_EXECUTE))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    MM_RANGE_OPERATION(mm, hint, (flags & KERNEL_MM_MAP_FIXED) ? length : 0);
    struct mm_file_mapping_plan plan;
    enum kernel_mm_status status=prepare_file_mapping(mm,hint,length,offset,permissions,flags,&plan);
    if (status) return status;
    struct kernel_lock_guard inode={0};
    status=commit_file_mapping(&file,offset,flags,address,&plan,&inode,object);
    return status;
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
                if (operation.guard.lock) mm_operation_release(&operation);
                if (current_pins(mm)) return KERNEL_MM_STATUS_CONFLICT;
                kernel_file_mapping_wait(node);
                continue;
            }
            if (generation != kernel_vma_set_generation(operation.record->vmas)) continue;
        }
        status=commit_file_mapping(file,offset,flags,address,&plan,&inode,NULL);
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

enum {
    LINUX_MS_ASYNC = 1U,
    LINUX_MS_INVALIDATE = 2U,
    LINUX_MS_SYNC = 4U,
};

static int sync_mapped_ranges(struct kernel_mm *mm,uint64_t address,uint64_t length,
    uint32_t flags,struct mm_operation *operation)
{
    uint64_t rounded=(length+BOAROS_PAGE_MASK)&~BOAROS_PAGE_MASK;
    uint64_t end=address+rounded,cursor=address;
    if (end<address) return -KERNEL_ENOMEM;
    int hole=0,result=0;
    while (cursor<end) {
        struct { struct kernel_mm_file_source *source; uint64_t start,end; } sources[16];
        /* A synchronous invalidation must finish each earlier range before
         * checking a later mapping's lock state. */
        const unsigned batch_limit=(flags & LINUX_MS_INVALIDATE) ? 1U : 16U;
        unsigned count=0;
        while (cursor<end && count<batch_limit) {
            struct kernel_vma vma;
            enum kernel_vma_status status=kernel_vma_set_lookup(operation->record->vmas,cursor,&vma);
            if (status==KERNEL_VMA_STATUS_NOT_FOUND) {
                hole=1;
                if (flags==LINUX_MS_ASYNC) { cursor=end; break; }
                status=kernel_vma_set_next(operation->record->vmas,cursor,&vma);
                if (status==KERNEL_VMA_STATUS_NOT_FOUND || vma.start>=end) { cursor=end; break; }
                if (status) __builtin_trap();
                cursor=vma.start;
            } else if (status) __builtin_trap();
            if ((flags & LINUX_MS_INVALIDATE) && (vma.flags & KERNEL_VMA_LOCKED))
                return -KERNEL_EBUSY;
            uint64_t finish=vma.end<end?vma.end:end;
            if ((flags & LINUX_MS_SYNC) && vma.kind==KERNEL_VMA_KIND_FILE_SHARED) {
                struct kernel_mm_file_source *source=vma.backing_owner;
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
        if (result || cursor>=end) break;
    }
    return result?result:hole?-KERNEL_ENOMEM:0;
}

int kernel_mm_msync(struct kernel_mm *mm, uint64_t address,
    uint64_t length, uint32_t flags)
{
    if ((flags & ~(LINUX_MS_ASYNC | LINUX_MS_INVALIDATE | LINUX_MS_SYNC)) ||
        (address & BOAROS_PAGE_MASK) || ((flags & LINUX_MS_ASYNC) && (flags & LINUX_MS_SYNC)))
        return -KERNEL_EINVAL;
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

struct kernel_aio_context **kernel_mm_aio_contexts(struct kernel_mm *mm)
{
    return &mm->record->aio_contexts;
}

int kernel_mm_aio_map(struct kernel_mm *mm, uint64_t length, uint64_t *address,
    struct kernel_memory_object **object)
{
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    if (mm_operation_begin(mm,&operation)) return -KERNEL_ENOMEM;
    return mm_mmap_anonymous_impl(mm,0,length,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_SHARED,address,object) ? -KERNEL_ENOMEM : 0;
}

void kernel_mm_aio_unmap(struct kernel_mm *mm, uint64_t address, uint64_t bytes,
    struct kernel_memory_object *object)
{
    /* Users can split or replace the mapping while the context is live. */
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    if (mm_operation_begin(mm,&operation)) return;
    uint64_t end=address+bytes;
    while (address<end) {
        struct kernel_vma vma;
        if (kernel_vma_set_lookup(operation.record->vmas,address,&vma)) {
            address+=BOAROS_PAGE_SIZE;
            continue;
        }
        uint64_t next=vma.end<end?vma.end:end;
        if (vma.backing==object)
            (void)mm_munmap_impl(mm,address,next-address);
        address=next;
    }
}

enum kernel_mm_status kernel_mm_mmap_anonymous(struct kernel_mm *mm,
    uint64_t hint, uint64_t length, uint32_t permissions, uint32_t flags, uint64_t *address)
{
#if BOAROS_MAX_CPUS == 1
    MM_RANGE_OPERATION(mm, hint, (flags & KERNEL_MM_MAP_FIXED) ? length : 0);
    enum kernel_mm_status result = mm_mmap_anonymous_impl(mm, hint, length, permissions, flags, address, NULL);
    uint32_t policy = mapping_lock_flags(operation.record, flags);
    mm_operation_end(&operation);
    if (!result && policy == KERNEL_VMA_LOCKED)
        (void)populate_locked(mm, *address, *address + ((length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK));
    return result;
#else
    uint64_t aligned_length;
    uint32_t normalized;
    if (address == 0 ||
        !normalize_user_permissions(permissions, &normalized) ||
        (flags & ~(KERNEL_MM_MAP_FIXED |
                   KERNEL_MM_MAP_FIXED_NOREPLACE |
                   KERNEL_MM_MAP_SHARED | KERNEL_MM_MAP_LOCKED | KERNEL_MM_MAP_GROWSDOWN)) != 0U ||
        (flags & (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) ==
            (KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_FIXED_NOREPLACE)) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & KERNEL_MM_MAP_GROWSDOWN) && (flags & KERNEL_MM_MAP_SHARED))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (length == 0U) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (!align_mapping_length(length, &aligned_length)) return KERNEL_MM_STATUS_NO_MEMORY;

    struct kernel_mm_record *record;
    enum kernel_mm_status status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;

    if (flags & KERNEL_MM_MAP_SHARED) {
        MM_RANGE_OPERATION(mm, hint, (flags & KERNEL_MM_MAP_FIXED) ? length : 0);
        enum kernel_mm_status result = mm_mmap_anonymous_impl(mm, hint, length, permissions, flags, address, NULL);
        uint32_t policy = mapping_lock_flags(operation.record, flags);
        mm_operation_end(&operation);
        if (!result && policy == KERNEL_VMA_LOCKED)
            (void)populate_locked(mm, *address, *address + aligned_length);
        return result;
    }

    if (flags & (KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_FIXED_NOREPLACE)) {
        /* Replacement changes a common address and accounting; preserve the
         * original edit until the PTEs and all backing owners are retired. */
        MM_RANGE_OPERATION(mm, hint, (flags & KERNEL_MM_MAP_FIXED) ? length : 0);
        enum kernel_mm_status result = mm_mmap_anonymous_impl(mm, hint, length, permissions, flags, address, NULL);
        uint32_t policy = mapping_lock_flags(operation.record, flags);
        mm_operation_end(&operation);
        if (!result && policy == KERNEL_VMA_LOCKED)
            (void)populate_locked(mm, *address, *address + aligned_length);
        return result;
    }

    /* Non-fixed private anonymous mmap */
    if (record->mmap_base < ARCH_MMU_PAGE_SIZE ||
        aligned_length > record->mmap_base - ARCH_MMU_PAGE_SIZE) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    hint &= ~BOAROS_PAGE_MASK;
    if (hint < ARCH_MMU_PAGE_SIZE || hint > record->mmap_base - aligned_length)
        hint = 0U;

    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    operation.shared = 1;
    status = mm_operation_begin(mm, &operation);
    if (status != KERNEL_MM_STATUS_OK) return status;

    uint64_t upper = record->mmap_base, start = 0;
    enum kernel_vma_status vma_status;
    struct kernel_lock_guard vma_guard = {0};
    kernel_rwlock_write(&record->vma_lock, &vma_guard);
    for (;;) {
        vma_status = kernel_vma_set_find_topdown_gap(
            record->vmas, hint, ARCH_MMU_PAGE_SIZE, upper, aligned_length, &start);
        if (vma_status != KERNEL_VMA_STATUS_OK) break;
        uint64_t conflict_start = 0;
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            for (struct kernel_mm_range_claim *c = record->range_claims; c; c = c->next) {
                if (c->start < start + aligned_length && c->end > start) {
                    conflict_start = c->start;
                    break;
                }
            }
        }
        if (!conflict_start) break;
        if (conflict_start <= ARCH_MMU_PAGE_SIZE ||
            conflict_start - ARCH_MMU_PAGE_SIZE < aligned_length) {
            vma_status = KERNEL_VMA_STATUS_NOT_FOUND;
            break;
        }
        upper = conflict_start;
        hint = 0U;
    }
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        kernel_lock_release(&vma_guard);
        return vma_status == KERNEL_VMA_STATUS_NOT_FOUND ? KERNEL_MM_STATUS_NO_MEMORY : status_from_vma(vma_status);
    }

    uint64_t end = start + aligned_length;
    if (!may_map_memory(record, start, end, normalized, 0, 0, flags & KERNEL_MM_MAP_GROWSDOWN)) {
        kernel_lock_release(&vma_guard);
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    status = admit_mapping_lock(record, start, end, flags);
    if (status) {
        kernel_lock_release(&vma_guard);
        return status;
    }

    struct kernel_vma vma = {
        .start = start, .end = end,
        .flags = mapping_lock_flags(record, flags) |
            ((flags & KERNEL_MM_MAP_GROWSDOWN) ? KERNEL_VMA_GROWSDOWN : 0),
        .backing_offset = 0U, .permissions = normalized,
        .maximum_permissions = KERNEL_VMA_ALL_PERMISSIONS,
        .kind = KERNEL_VMA_KIND_ANONYMOUS, .role = KERNEL_VMA_ROLE_MMAP,
        .fault_policy = KERNEL_VMA_FAULT_DEMAND_ZERO,
    };
    vma_status = kernel_vma_set_insert(record->vmas, &vma);
    kernel_lock_release(&vma_guard);
    if (vma_status != KERNEL_VMA_STATUS_OK) return status_from_vma(vma_status);

    *address = start;
    mm_operation_end(&operation);
    if ((vma.flags & KERNEL_VMA_LOCK_MASK) == KERNEL_VMA_LOCKED)
        (void)populate_locked(mm, start, end);
    return KERNEL_MM_STATUS_OK;
#endif
}
enum kernel_mm_status kernel_mm_munmap(struct kernel_mm *mm, uint64_t address, uint64_t length)
{
#if BOAROS_MAX_CPUS == 1
    MM_RANGE_OPERATION(mm, address, length);
    return mm_munmap_impl(mm, address, length);
#else
    uint64_t aligned_length;
    if ((address & BOAROS_PAGE_MASK) != 0U ||
        !align_mapping_length(length, &aligned_length) ||
        address >= ARCH_MMU_USER_LIMIT ||
        address > ARCH_MMU_USER_LIMIT - aligned_length) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    uint64_t end = address + aligned_length;
    struct kernel_mm_record *record;
    enum kernel_mm_status status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;

    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    operation.shared = 1;
    status = mm_operation_begin(mm, &operation);
    if (status != KERNEL_MM_STATUS_OK) return status;

    int pure_anon;
    {
        struct kernel_lock_guard vma_guard = {0};
        kernel_rwlock_read(&record->vma_lock, &vma_guard);
        pure_anon = kernel_vma_set_range_is_anonymous(record->vmas, address, end);
        kernel_lock_release(&vma_guard);
    }
    if (!pure_anon) {
        mm_operation_end(&operation);
        MM_RANGE_OPERATION(mm, address, length);
        return mm_munmap_impl(mm, address, length);
    }

    status = mm_operation_claim_range(&operation, address, end);
    if (status != KERNEL_MM_STATUS_OK) return status;

    struct kernel_vma_edit edit;
    struct kernel_lock_guard vma_guard = {0};
    kernel_rwlock_write(&record->vma_lock, &vma_guard);
    enum kernel_vma_status vma_status = kernel_vma_set_prepare_replace(
        record->vmas, address, end, 0, &edit);
    if (vma_status == KERNEL_VMA_STATUS_OK) {
        if (kernel_vma_set_commit_edit(record->vmas, &edit) != KERNEL_VMA_STATUS_OK)
            __builtin_trap();
    }
    kernel_lock_release(&vma_guard);

    if (vma_status != KERNEL_VMA_STATUS_OK) {
        mm_operation_unclaim_range(&operation);
        return status_from_vma(vma_status);
    }

    status = unmap_vma_space_range(record, address, end);
    mm_operation_unclaim_range(&operation);
    return status;
#endif
}
#if BOAROS_MAX_CPUS > 1
/* Adding anonymous access does not revoke any pin or replace a source.
 * The range claim excludes overlapping edits and fault publication, while
 * unrelated faults keep their shared MM ownership. */
static enum kernel_mm_status mprotect_addition(struct kernel_mm *mm,uint64_t address,
    uint64_t length,uint32_t permissions,int *handled)
{
    struct mm_operation operation __attribute__((cleanup(mm_operation_end)))={.shared=1,.mm=mm};
    enum kernel_mm_status status=mutable_vma_record(mm,&operation.record);
    if (!status) status=mm_operation_lock(&operation);
    if (status) { *handled=1;return status; }
    struct kernel_mm_record *record=operation.record;
    uint64_t end=address+length;
    int unchanged;
    KERNEL_LOCK_SCOPE(vma);
    kernel_rwlock_read(&record->vma_lock,&vma);
    if (!kernel_vma_set_permission_addition(record->vmas,address,end,permissions,&unchanged))
        return KERNEL_MM_STATUS_OK;
    if (unchanged) { *handled=1;return KERNEL_MM_STATUS_OK; }

    if (kernel_rwlock_try_upgrade(&vma) &&
        mm_range_claim_try_acquire(record,&operation.claim,address,end)) {
        operation.range_claimed=1;
    } else {
        /* Dropping VMA before a range wait lets the existing claimant finish.
         * A queued writer keeps its place when the read guard cannot upgrade. */
        uint64_t generation=kernel_vma_set_generation(record->vmas);
        kernel_lock_release(&vma);
        status=mm_operation_claim_range(&operation,address,end);
        if (status) { *handled=1;return status; }
        kernel_rwlock_write(&record->vma_lock,&vma);
        if (kernel_vma_set_generation(record->vmas)!=generation &&
            !kernel_vma_set_permission_addition(record->vmas,address,end,permissions,&unchanged))
            return KERNEL_MM_STATUS_OK;
    }
    *handled=1;
    if (unchanged) return KERNEL_MM_STATUS_OK;
    struct kernel_vma_edit edit={0};
    status=status_from_vma(kernel_vma_set_prepare_mprotect(record->vmas,
        address,end,permissions,&edit));
    if (edit.set && kernel_vma_set_commit_edit(record->vmas,&edit)) __builtin_trap();
    kernel_lock_release(&vma);
    if (edit.set) {
        enum kernel_mm_status published=protect_vma_pages(record,&edit);
        if (published) return published;
    }
    return status;
}
#endif

/* PROT_GROWSDOWN changes the containing stack fragment down to its current
 * start. Serialize lookup and edit together; normal mprotect keeps its range
 * concurrency and this uncommon operation adds no cost to that path. */
enum kernel_mm_status kernel_mm_mprotect_growdown(struct kernel_mm *mm,
    uint64_t address, uint64_t length, uint32_t permissions, int read_implies_exec)
{
    uint32_t normalized;
    uint64_t aligned_length;
    if ((address & BOAROS_PAGE_MASK) || !normalize_user_permissions(permissions, &normalized))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (!length) return mm_mprotect_impl(mm,address,length,permissions);
    if (!align_mapping_length(length,&aligned_length) ||
        address > ARCH_MMU_USER_LIMIT-aligned_length) return KERNEL_MM_STATUS_NO_MEMORY;
    MM_OPERATION(mm);
    close_pins(&operation, 0);
    struct kernel_vma vma;
    enum kernel_vma_status found=kernel_vma_set_lookup(operation.record->vmas,address,&vma);
    if (found==KERNEL_VMA_STATUS_NOT_FOUND)
        found=kernel_vma_set_next(operation.record->vmas,address,&vma);
    if (found==KERNEL_VMA_STATUS_NOT_FOUND) return KERNEL_MM_STATUS_NOT_MAPPED;
    if (found) return status_from_vma(found);
    if (vma.start >= address+aligned_length) return KERNEL_MM_STATUS_NOT_MAPPED;
    if (!kernel_vma_grows_down(&vma)) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    uint64_t end=address+aligned_length;
    address=vma.start;
    if (!read_implies_exec || !(permissions & KERNEL_MM_READ) || (permissions & KERNEL_MM_EXECUTE))
        return mm_mprotect_impl(mm,address,end-address,permissions);
    while (address < end) {
        if (kernel_vma_set_lookup(operation.record->vmas,address,&vma)) return KERNEL_MM_STATUS_NOT_MAPPED;
        uint64_t finish=vma.end < end ? vma.end : end;
        uint32_t effective=permissions;
        if (vma.maximum_permissions & KERNEL_MM_EXECUTE) effective |= KERNEL_MM_EXECUTE;
        enum kernel_mm_status status=mm_mprotect_impl(mm,address,finish-address,effective);
        if (status) return status;
        address=finish;
    }
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_mprotect_personality(struct kernel_mm *mm,
    uint64_t address, uint64_t length, uint32_t permissions, int read_implies_exec)
{
    uint32_t normalized; uint64_t aligned_length;
    if ((address & BOAROS_PAGE_MASK) || !normalize_user_permissions(permissions, &normalized))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (length && (!align_mapping_length(length, &aligned_length) ||
        address > ARCH_MMU_USER_LIMIT - aligned_length)) return KERNEL_MM_STATUS_NO_MEMORY;
    if (!length) return mm_mprotect_impl(mm, address, length, permissions);
#if BOAROS_MAX_CPUS > 1
    if (!read_implies_exec || !(permissions & KERNEL_MM_READ) || (permissions & KERNEL_MM_EXECUTE)) {
        int handled=0;
        enum kernel_mm_status status=mprotect_addition(mm,address,aligned_length,normalized,&handled);
        if (handled) return status;
    }
#endif
    MM_RANGE_OPERATION(mm, address, length);
    close_pins(&operation, 0);
    if (!read_implies_exec || !(permissions & KERNEL_MM_READ) ||
        (permissions & KERNEL_MM_EXECUTE))
        return mm_mprotect_impl(mm, address, length, permissions);
    /* The range claim protects the VMA sequence across each commit. noexec
     * files retain readable, non-executable VMAs without rejecting PROT_READ. */
    uint64_t end = address + aligned_length;
    while (address < end) {
        struct kernel_vma vma;
        {
            KERNEL_LOCK_SCOPE(guard);
            kernel_rwlock_read(&operation.record->vma_lock, &guard);
            enum kernel_vma_status status = kernel_vma_set_lookup(operation.record->vmas, address, &vma);
            if (status == KERNEL_VMA_STATUS_NOT_FOUND) return KERNEL_MM_STATUS_NO_MEMORY;
            if (status) return KERNEL_MM_STATUS_STATE;
        }
        uint64_t finish = vma.end < end ? vma.end : end;
        uint32_t effective = permissions;
        if (vma.maximum_permissions & KERNEL_MM_EXECUTE) effective |= KERNEL_MM_EXECUTE;
        enum kernel_mm_status status = mm_mprotect_impl(mm, address, finish-address, effective);
        if (status) return status;
        address = finish;
    }
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_mprotect(struct kernel_mm *mm,
    uint64_t address, uint64_t length, uint32_t permissions)
{
    return kernel_mm_mprotect_personality(mm, address, length, permissions, 0);
}
/* A direct-read destination owns actual physical pages independently of VMA
 * edits. Fork must finish copying the page-table view before new destination
 * captures, or observe their completed data. Storage revocation may enter the
 * MM gate, so fork waits outside that gate. */
int kernel_mm_aio_destination_begin(struct kernel_mm *mm)
{
    struct mm_operation operation __attribute__((cleanup(mm_operation_end)))={0};
    if (mm_operation_begin(mm,&operation)) return 0;
    if (__atomic_load_n(&operation.record->aio_fork_waiters,__ATOMIC_ACQUIRE)) return 0;
    __atomic_fetch_add(&operation.record->aio_destinations,1,__ATOMIC_ACQ_REL);
    return 1;
}

void kernel_mm_aio_destination_end(struct kernel_mm_record *record)
{
    uint32_t previous=__atomic_fetch_sub(&record->aio_destinations,1,__ATOMIC_ACQ_REL);
    if (!previous) __builtin_trap();
    if (previous==1 && __atomic_load_n(&record->aio_fork_waiters,__ATOMIC_ACQUIRE))
        wake_pin_waiters(record);
}

enum kernel_mm_status kernel_mm_fork(struct kernel_mm *destination, struct kernel_mm *source)
{
    if (!destination || !source || destination == source) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (!empty_handle(destination) || source->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    /* SysV attachment open/close updates its CPU0-owned segment. Check under
     * the parent MM gate and reacquire it after any service migration. */
    MM_RANGE_OPERATION(source, 0, ARCH_MMU_USER_LIMIT);
    struct kernel_mm_record *record=operation.record;
    if (__atomic_load_n(&record->aio_destinations,__ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&record->aio_fork_waiters,1,__ATOMIC_ACQ_REL);
        mm_operation_end(&operation);
        while (__atomic_load_n(&record->aio_destinations,__ATOMIC_ACQUIRE)) {
            KERNEL_IRQ_SCOPE(irq);
            enum kernel_wait_wake_reason reason;
            if (KERNEL_WAIT_RECHECK(&record->pins_changed,0,0,&reason,
                    __atomic_load_n(&record->aio_destinations,__ATOMIC_ACQUIRE))) __builtin_trap();
        }
        op_status=mm_ranges_operation_begin(source,0,ARCH_MMU_USER_LIMIT,0,0,&operation,&mm_service);
        __atomic_fetch_sub(&record->aio_fork_waiters,1,__ATOMIC_ACQ_REL);
        if (op_status) return op_status;
    }
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
static enum kernel_mm_status map_private_page(struct physical_page_allocator *allocator,
    uint64_t *address, void **pointer)
{
    if (physical_page_map_owned(allocator, *address, pointer) != PHYSICAL_PAGE_STATUS_OK) {
        physical_page_release(allocator, *address); *address = KERNEL_MM_NO_PAGE;
        return KERNEL_MM_STATUS_PAGE_ACCESS;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status allocate_private_page(struct physical_page_allocator *allocator,
    uint64_t *address, void **pointer)
{
    COST_SCOPE(allocate_cost, MM_PAGE_ALLOC_TICKS);
    enum physical_page_status status = physical_page_allocate(allocator, address);
    if (status == PHYSICAL_PAGE_STATUS_EMPTY) return KERNEL_MM_STATUS_NO_MEMORY;
    if (status != PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
    return map_private_page(allocator, address, pointer);
}

static enum kernel_mm_status try_allocate_fault_page(struct physical_page_allocator *allocator,
    uint64_t *address, void **pointer)
{
    COST_SCOPE(allocate_cost, MM_PAGE_ALLOC_TICKS);
    if (!physical_page_try_fault(allocator, address)) return KERNEL_MM_STATUS_NO_MEMORY;
    return map_private_page(allocator, address, pointer);
}

static enum kernel_mm_status allocate_fault_page(struct mm_operation *operation,
    uint64_t *address, void **pointer)
{
    struct physical_page_allocator *allocator = operation->mm->allocator;
#if BOAROS_MAX_CPUS == 1
    if (operation->guard.lock) {
        enum kernel_mm_status status = try_allocate_fault_page(allocator, address, pointer);
        if (status != KERNEL_MM_STATUS_NO_MEMORY) return status;
        /* Free-page allocation can keep the VMA stable. Reclaim can wait
         * for another MM operation, so release the gate before running it. */
        mm_operation_release(operation);
    }
    return allocate_private_page(allocator, address, pointer);
#else
    enum kernel_mm_status status = allocate_private_page(allocator, address, pointer);
    if (status == KERNEL_MM_STATUS_NO_MEMORY && operation->shared && operation->guard.lock) {
        mm_operation_release(operation);
        status = allocate_private_page(allocator, address, pointer);
    }
    return status;
#endif
}

/* The MM owns the template. Each published leaf owns another reference, so
 * fork and retirement can use the ordinary read-only COW ownership rules. */
static enum kernel_mm_status anonymous_zero_template(struct mm_operation *operation,
    uint64_t *address)
{
    struct kernel_mm_record *record = operation->record;
    struct physical_page_allocator *allocator = operation->mm->allocator;
    uint64_t page = __atomic_load_n(&record->anonymous_zero_page, __ATOMIC_ACQUIRE);
    if (page == KERNEL_MM_NO_PAGE) {
        uint64_t allocated = KERNEL_MM_NO_PAGE;
        void *bytes;
        enum kernel_mm_status status = allocate_fault_page(operation, &allocated, &bytes);
        if (status != KERNEL_MM_STATUS_OK) return status;
        clear_page(bytes);
        if (__atomic_compare_exchange_n(&record->anonymous_zero_page, &page, allocated,
            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            page = allocated;
        else if (physical_page_release(allocator, allocated))
            __builtin_trap();
    }
    *address = page;
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
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
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
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
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
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        enum arch_mmu_status status = arch_mmu_user_next_owned(
            &record->space, &cursor, from + length, &address);
        if (status == ARCH_MMU_STATUS_NOT_MAPPED) break;
        if (status || arch_mmu_user_move_leaf(&record->space, address, to + address - from))
            __builtin_trap();
        struct mm_lazy_page *lazy=lazy_find(record,address);
        if (lazy) {
            struct mm_lazy_page **link=&record->lazy_buckets[resident_bucket(address,record->lazy_capacity)];
            while (*link && *link!=lazy) link=&(*link)->hash_next;
            if (!*link) __builtin_trap();
            *link=lazy->hash_next;
            lazy->address=to+address-from;
            size_t bucket=resident_bucket(lazy->address,record->lazy_capacity);
            lazy->hash_next=record->lazy_buckets[bucket];
            record->lazy_buckets[bucket]=lazy;
        }
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
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        targets = note_tlb_change_locked(record, &batch, executable);
    }
    /* 新旧VA不连续；先确认所有CPU都不再借用旧翻译，再回收源空表。 */
    finish_tlb_change(record, &batch, targets, 0, UINT64_MAX, executable);
    if (unmap_vma_space_range(record, from, from + length)) __builtin_trap();
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
        if (old_address < new_address + new_size &&
            (old_address > UINT64_MAX - old_size || new_address < old_address + old_size))
            return -KERNEL_EINVAL;
    }
    KERNEL_SERVICE_SCOPE_IF(service, 0);
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    uint64_t tail_length = new_size < old_size && old_address < ARCH_MMU_USER_LIMIT &&
        old_size <= ARCH_MMU_USER_LIMIT - old_address ? old_size - new_size : 0;
    int preserve = !old_size || (flags & 4U);
    enum kernel_mm_status status = mm_ranges_operation_begin(mm, new_address,
        (flags & 2U) ? new_size : 0,
        preserve ? old_address : tail_length ? old_address + new_size : 0,
        preserve ? BOAROS_PAGE_SIZE : tail_length, &operation, &service);
    if (status) return status == KERNEL_MM_STATUS_CONFLICT ? -KERNEL_EAGAIN : -KERNEL_ENOMEM;
    struct kernel_mm_record *record;
    if (mutable_vma_record(mm, &record)) return -KERNEL_ENOMEM;
    struct kernel_vma source;
    if (old_address >= ARCH_MMU_USER_LIMIT ||
        kernel_vma_set_lookup(record->vmas, old_address, &source)) return -KERNEL_EFAULT;
    if (huge_vma(&source)) return -KERNEL_EOPNOTSUPP;
    if (!old_size && source.kind != KERNEL_VMA_KIND_ANON_SHARED &&
        source.kind != KERNEL_VMA_KIND_FILE_SHARED && source.kind != KERNEL_VMA_KIND_SYSV_SHM &&
        source.kind != KERNEL_VMA_KIND_OBJECT_SHARED)
        return -KERNEL_EINVAL;
    /* remap_vmalloc_range mappings carry VM_DONTEXPAND: neither leaving
     * the old range behind nor duplicating it through old_size=0 is allowed. */
    if (source.flags & KERNEL_VMA_AIO_RING) {
        if ((flags & 4U) || !old_size) return -KERNEL_EINVAL;
        if (new_size>old_size) return -KERNEL_EFAULT;
        if ((flags & 2U) && (source.backing_offset || source.start!=old_address ||
            source.end-source.start!=old_size || new_size!=old_size ||
            kernel_memory_object_contains_offset(source.backing,old_size))) return -KERNEL_EINVAL;
    }
    if (source.kind==KERNEL_VMA_KIND_OBJECT_SHARED && (flags & 4U))
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
    if (source.kind==KERNEL_VMA_KIND_OBJECT_SHARED && new_size>old_size)
        return -KERNEL_EFAULT;
    if (!old_size && !(flags & 1U)) return -KERNEL_ENOMEM;
    if (!(flags & 6U) && old_size && new_size <= old_size) {
        if (new_size < old_size) {
            status = mm_munmap_impl(mm, old_address + new_size, old_size - new_size);
            if (status) return status == KERNEL_MM_STATUS_INVALID_ARGUMENT ? -KERNEL_EINVAL : -KERNEL_ENOMEM;
        }
        *result = old_address;
        return 0;
    }
    if (new_size > old_size && !may_expand_current(record, &source, new_size - old_size))
        return -KERNEL_ENOMEM;
    if (new_size > old_size && (source.flags & KERNEL_VMA_LOCKED) && !may_lock_memory(record, new_size - old_size))
        return -KERNEL_EAGAIN;
    if (!(flags & 2U)) {
        int overlaps = 1;
        if ((flags & 4U) && new_address >= BOAROS_PAGE_SIZE &&
            kernel_vma_set_overlaps(record->vmas, new_address,
                new_address + new_size, &overlaps)) __builtin_trap();
        if (!(flags & 4U) && old_size && old_address <= ARCH_MMU_USER_LIMIT - new_size &&
            old_address + old_size == source.end &&
            kernel_vma_set_overlaps(record->vmas, source.end,
                old_address + new_size, &overlaps)) __builtin_trap();
        if (!overlaps) {
            if (!(flags & 4U)) new_address = old_address;
        } else {
            if (!(flags & 1U)) return -KERNEL_ENOMEM;
            enum kernel_vma_status vs = kernel_vma_set_find_topdown_gap(record->vmas,
                0, BOAROS_PAGE_SIZE, record->mmap_base, new_size, &new_address);
            if (vs) return -KERNEL_ENOMEM;
        }
    }
    if ((source.role != KERNEL_VMA_ROLE_MMAP &&
         !(preserve && source.role == KERNEL_VMA_ROLE_SYSV_SHM)) ||
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
    if ((flags & 4U) && !may_expand_current(record, &source, new_size))
        return -KERNEL_ENOMEM;
    struct kernel_vma replacement = source;
    replacement.start = new_address;
    replacement.end = new_address + new_size;
    replacement.backing_offset = offset;
    struct kernel_shm_attachment *attachment
        __attribute__((cleanup(kernel_shm_attachment_release))) = 0;
    if (source.kind == KERNEL_VMA_KIND_SYSV_SHM) {
        struct kernel_shm_attachment *original = kernel_vma_shm_attachment(&source);
        if (kernel_shm_attachment_create(original->segment,
                new_address, new_address + new_size, original,
                &attachment) != KERNEL_SHM_STATUS_OK)
            return -KERNEL_ENOMEM;
        replacement.backing_owner = attachment;
    }
    /* Without EVENT_REMAP a moved destination starts with no registration. */
    if (new_address != old_address && kernel_vma_userfault(&replacement))
        kernel_vma_clear_userfault(&replacement);
    struct kernel_vma_remap edit;
    enum kernel_vma_status prepared = preserve ?
        kernel_vma_set_prepare_duplicate(record->vmas, old_address, old_address + old_size,
            &replacement, (flags & 4U) && (source.flags & KERNEL_VMA_LOCK_MASK), &edit) :
        kernel_vma_set_prepare_remap(record->vmas, old_address, old_address + old_size,
            &replacement, &edit);
    if (prepared) return -KERNEL_ENOMEM;
    if (old_size && new_address != old_address) {
        if (current_pins(mm)) return -KERNEL_EAGAIN;
        close_pins(&operation, 0);
        int error = prepare_remap_tables(record, old_address, old_size, new_address);
        if (error) return error;
        /* All fallible mapping preparations have finished. Publish the move
         * against io_destroy before changing any source PTE. An admitted
         * destroy can now remove only the published new address. */
        if ((source.flags & KERNEL_VMA_AIO_RING) &&
            kernel_aio_ring_move(mm,source.backing,old_address,new_address))
            return -KERNEL_EINVAL;
        move_remap_leaves(record, old_address, old_size, new_address,
            (source.permissions & KERNEL_MM_EXECUTE) != 0);
    }
    if (kernel_vma_set_commit_remap(record->vmas, &edit)) __builtin_trap();
    *result = new_address;
    mm_operation_end(&operation);
    if (new_size > old_size && (source.flags & KERNEL_VMA_LOCK_MASK) == KERNEL_VMA_LOCKED)
        (void)populate_locked(mm, new_address + old_size, new_address + new_size);
    return 0;
}

/* gate固定resident/alias，许可由页write状态仲裁；等待时结束全部对象保护。 */
static int fault_inode_lock(struct mm_operation *,struct kernel_vfs_node *,struct kernel_lock_guard *);
static enum kernel_mm_status cached_write_fault(struct mm_operation *operation,
    struct kernel_open_file_description *file,const struct kernel_vma *vma,uint64_t address,uint64_t generation,
    struct kernel_lock_guard *inode)
{
    struct kernel_mm_record *record=operation->record;
    struct kernel_vfs_node *node=kernel_open_file_node(file);
    uint64_t epoch,latest;
    if (kernel_vfs_node_attributes(node)&KERNEL_VFS_INODE_IMMUTABLE) return KERNEL_MM_STATUS_BUS_FAULT;
    if (kernel_file_mapping_epoch(node,&epoch)) return KERNEL_MM_STATUS_CONFLICT;
    uint64_t offset=vma->backing_offset+address-vma->start;
    /* 准备磁盘空间不能持有 MM gate 或 inode 读资格；同文件源缺页、
     * truncate 与后端预算等待都需要这些对象继续推进。返回后重查身份。 */
    if (inode->lock) kernel_lock_release(inode);
    mm_operation_release(operation);
    struct kernel_page_cache *cache=kernel_open_file_page_cache(file);
    struct kernel_page_cache_reservation reservation={0};
    int error=kernel_page_cache_prepare_mapping_write(cache,node,offset>>BOAROS_PAGE_SHIFT,&reservation);
    enum kernel_mm_status status;
    operation->shared = 0;
    enum kernel_mm_status locked=mm_operation_lock(operation);
    if (locked) {status=locked;goto finish;}
    if (generation!=kernel_vma_set_generation(record->vmas) || !fault_inode_lock(operation,node,inode))
        {status=KERNEL_MM_STATUS_CONFLICT;goto finish;}
    if (kernel_file_mapping_epoch(node,&latest) || latest!=epoch || offset>=kernel_open_file_size(file))
        {status=KERNEL_MM_STATUS_CONFLICT;goto finish;}
    if (error) {status=error==-KERNEL_ENOMEM?KERNEL_MM_STATUS_NO_MEMORY:
        error==-KERNEL_EBUSY?KERNEL_MM_STATUS_CONFLICT:KERNEL_MM_STATUS_BUS_FAULT;goto finish;}
    struct mm_file_alias_resident *resident = file_alias_resident(find_file_resident(record,address));
    if (!resident || !kernel_page_cache_alias_active(&resident->alias)) {status=KERNEL_MM_STATUS_CONFLICT;goto finish;}
    struct kernel_page_cache_write_borrow permit={0};
    enum kernel_page_cache_status admitted=kernel_page_cache_alias_write_begin(&resident->alias,&permit);
    if (admitted==KERNEL_PAGE_CACHE_STATUS_BUSY) {
        uint64_t physical=resident->base.physical_address;
        if (inode->lock) kernel_lock_release(inode);
        if (operation->guard.lock) mm_operation_release(operation);
        if (!current_pins(operation->mm)) {
            uint64_t index=(vma->backing_offset+address-vma->start)>>BOAROS_PAGE_SHIFT;
            (void)kernel_page_cache_wait_writable(kernel_open_file_page_cache(file),kernel_open_file_vfs_borrow(file),index,physical);
        }
        {status=KERNEL_MM_STATUS_CONFLICT;goto finish;}
    }
    if (admitted) __builtin_trap();
    kernel_page_cache_alias_mark_dirty(&resident->alias);
    /* Commit this generation before releasing the inode: a later truncate
     * and regrowth must not be accepted by this old fault's delayed finish. */
    kernel_page_cache_accept_mapping_write(node,&reservation);
#if defined(BOAROS_ARCH_LOONGARCH)
    struct kernel_mm_retirement batch={0}; uint32_t targets;
#endif
    {
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        if (arch_mmu_user_protect_owned_page(&record->space,address,
            backend_permissions_from_mm(vma->permissions))) __builtin_trap();
#if defined(BOAROS_ARCH_LOONGARCH)
        targets=note_tlb_change_locked(record, &batch, 0);
#endif
    }
#if defined(BOAROS_ARCH_LOONGARCH)
    finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,0);
#else
    /* The physical page is unchanged and write permission only increases.
     * A hart retaining a read-only entry fences locally if its store faults. */
    sync_tlb_change(0,address,BOAROS_PAGE_SIZE,0);
#endif
    kernel_page_cache_write_end(&permit);
    status=KERNEL_MM_STATUS_OK;
finish:
    if (reservation.ticket.owner) {
        if (inode->lock) kernel_lock_release(inode);
        if (operation->guard.lock) mm_operation_release(operation);
        kernel_page_cache_finish_mapping_write(cache,node,&reservation,status==KERNEL_MM_STATUS_OK);
    }
    return status;
}

/* inode等待可能与另一个映射编辑交错；不能带着gate阻止它完成。
 * 等待结束只重试，不沿用等待前取得的VMA或候选身份。 */
static int fault_inode_lock(struct mm_operation *operation, struct kernel_vfs_node *node,
    struct kernel_lock_guard *inode)
{
    if (kernel_vfs_node_try_read(node, inode)) return 1;
    mm_operation_release(operation);
    kernel_vfs_node_lock(node, inode, 0);
    kernel_lock_release(inode);
    return 0;
}

/* Object mappings are populated at mmap, with no refill callback.
 * A missing PTE (including after DONTNEED) is a bus error, not another borrow. */
static enum kernel_mm_status mapped_object_fault(struct mm_operation *operation,
    uint64_t address,uint32_t access,const struct kernel_vma *vma)
{
    struct kernel_mm_record *record=operation->record;
    if (!(user_effective_permissions(vma->permissions) & access)) return KERNEL_MM_STATUS_ACCESS;
    KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
    struct kernel_mm_mapping mapping;
    enum kernel_mm_status status=lookup_record(record,address,&mapping);
    if (status==KERNEL_MM_STATUS_NOT_MAPPED) return KERNEL_MM_STATUS_BUS_FAULT;
    return status ? status : (mapping.permissions & access) ? KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_ACCESS;
}

static uint64_t file_fault_window(const struct kernel_vma *vma, uint64_t address,
    uint64_t *end)
{
    const uint64_t span = KERNEL_PAGE_CACHE_READY_WINDOW * BOAROS_PAGE_SIZE;
    uint64_t first = address & ~(span - 1);
    uint64_t limit = first + span;
    *end = limit < vma->end ? limit : vma->end;
    return first > vma->start ? first : vma->start;
}

static int fault_range_claimed(struct kernel_mm_record *, uint64_t, uint64_t);

/* The fault owns the actual source and MM qualification. Borrow ready pages
 * once, prepare optional resident records outside metadata, then publish the
 * bounded same-leaf batch under one metadata protection. */
static __attribute__((noinline)) uint64_t map_file_neighbours(
    struct mm_operation *operation,struct kernel_open_file_description *file,
    struct kernel_elf64_source *source,const struct fault_vma *view,
    uint64_t address,uint64_t *begin,
    const struct kernel_mm_retirement *batch,uint32_t *targets,
    struct mm_file_resident *demand, int include_demand)
{
    struct kernel_mm_record *record=operation->record;
    const struct kernel_vma *vma=&view->vma;
    uint64_t end=include_demand ? 0 : address+BOAROS_PAGE_SIZE;
    uint64_t limit, first=file_fault_window(vma,address,&limit);
    int shared = vma->kind == KERNEL_VMA_KIND_FILE_SHARED;
    if (limit-first==BOAROS_PAGE_SIZE) return end;
    unsigned required = include_demand ? 1U << ((address-first)>>BOAROS_PAGE_SHIFT) : 0;
    int revalidate = include_demand && operation->shared;
    unsigned selected=0,slots=KERNEL_PAGE_CACHE_READY_WINDOW;
    {
        /* A continuously held MM writer also stabilizes the empty-slot scan. */
        struct kernel_raw_read_guard raw __attribute__((cleanup(kernel_raw_read_scope_release)))={0};
        if (!include_demand || revalidate) kernel_raw_rwlock_read_acquire(&record->metadata,&raw);
        if (!source && record->resident_capacity-record->resident_count<slots)
            slots=record->resident_capacity-record->resident_count;
        if (!slots) return end;
        if (arch_mmu_user_empty_window(&record->space,first,
            (limit-first)>>BOAROS_PAGE_SHIFT,&selected)) return end;
    }
    if (!selected || (selected & required) != required) return end;
    uint64_t refs[KERNEL_PAGE_CACHE_READY_WINDOW];
    struct kernel_page_cache_alias_binding aliases[KERNEL_PAGE_CACHE_READY_WINDOW];
    struct kernel_page_cache *cache = file ? kernel_open_file_page_cache(file) : NULL;
    const struct kernel_vfs_file *backing = file ? kernel_open_file_vfs_borrow(file) : NULL;
    uint64_t offset=vma->backing_offset+first-vma->start;
    unsigned ready=source ? kernel_elf64_source_acquire_ready(source,offset,selected,refs,slots) :
        kernel_page_cache_acquire_ready(cache,backing,offset>>BOAROS_PAGE_SHIFT,
            selected,refs,slots,required,shared ? aliases : NULL);
    if (!ready) return end;
    void *residents[KERNEL_PAGE_CACHE_READY_WINDOW] = {0};
    unsigned count = 0;
    for (unsigned bits = ready; bits; bits &= bits - 1) count++;
    size_t resident_size = shared ? sizeof(struct mm_file_alias_resident) : sizeof(struct mm_file_resident);
    unsigned allocated = source ? count : kernel_heap_allocate_zeroed_batch_nosleep(
        record->vma_heap, count, resident_size, residents);
    unsigned installed = 0;
    KERNEL_LOCK_SCOPE(vmas);
    if (allocated && (!revalidate || kernel_rwlock_try_read(&record->vma_lock,&vmas))) {
        KERNEL_RAW_WRITE_SCOPE(raw, &record->metadata);
        unsigned capacity = source ? allocated : record->resident_capacity - record->resident_count;
        if (capacity > allocated) capacity = allocated;
        unsigned unprepared = ready;
        /* Drop the unprepared suffix of the packed reference vector. */
        for (unsigned i = 0; i < capacity; i++) unprepared &= unprepared - 1;
        unsigned prepared_mask = ready ^ unprepared;
        int current = 1;
        if (revalidate) {
            struct kernel_mm_leaf leaf;
            /* A private MM reader can race another publisher. If it won the
             * demand slot, let the scalar retry handle that fault. */
            current = kernel_vma_set_generation(record->vmas) == view->generation &&
                !fault_range_claimed(record,first,limit) &&
                arch_mmu_user_leaf_probe(&record->space,address,&leaf,NULL) == ARCH_MMU_STATUS_NOT_MAPPED;
        }
        if (current && (prepared_mask & required) == required)
            installed = arch_mmu_user_map_owned_window(&record->space, first, prepared_mask, refs,
                backend_permissions_from_mm(user_effective_permissions(vma->permissions) & ~KERNEL_MM_WRITE), !shared);
        if (installed && (installed & required) != required) __builtin_trap();
        unsigned i = 0;
        for (unsigned bits = ready, index = 0; bits; bits >>= 1, index++) {
            if (!(bits & 1)) continue;
            struct mm_file_resident *page = residents[i];
            uint64_t physical = refs[i++];
            if (!(installed & (1U << index))) continue;
            uint64_t address = first + (uint64_t)index * BOAROS_PAGE_SIZE;
            if (page) {
                page->address = address;
                page->physical_address = physical;
                page->with_alias = shared;
                publish_file_resident(record, page);
            }
            if (address < *begin) *begin = address;
            if (address + BOAROS_PAGE_SIZE > end) end = address + BOAROS_PAGE_SIZE;
        }
        if (installed && targets)
            *targets = note_fault_change_locked(record,batch,0,
                (vma->permissions & KERNEL_MM_EXECUTE) != 0);
    }
    kernel_lock_scope_release(&vmas);
    if (!shared && installed == ready) return end;
    unsigned demand_bit = demand && installed ?
        1U << ((demand->address - first) >> BOAROS_PAGE_SHIFT) : 0;
    unsigned unused = 0, i = 0;
    for (unsigned bits = ready | demand_bit, index = 0; bits; bits >>= 1, index++) {
        if (!(bits & 1)) continue;
        unsigned bit = 1U << index;
        struct mm_file_resident *page;
        if (bit == demand_bit) {
            page = demand;
            aliases[index].entry = NULL;
        } else {
            page = residents[i];
            if (!(installed & bit)) {
                refs[unused++] = refs[i++];
                if (page) kernel_heap_release(record->vma_heap, page);
                continue;
            }
            i++;
        }
        if (shared) {
            /* Shared leaves are initially read-only. The MM writer and
             * inode reader exclude write faults and revocation until the
             * cache knows about every newly published alias. */
            struct mm_file_alias_resident *alias_owner = file_alias_resident(page);
            alias_owner->alias.owner = alias_owner;
            alias_owner->alias.virtual_address = page->address;
            alias_owner->alias.rearm = rearm_shared_file_alias;
            aliases[index].alias = &alias_owner->alias;
            aliases[index].physical_address = page->physical_address;
        }
    }
    if (shared && installed)
        kernel_page_cache_alias_attach_window(cache,backing,
            offset>>BOAROS_PAGE_SHIFT,installed | demand_bit,aliases);
    physical_page_release_batch(record->space.allocator,refs,unused);
    return end;
}

static int file_fault_read_window(const struct kernel_vma *vma, uint32_t access)
{
    if (vma->flags&KERNEL_VMA_RANDOM_READ) return KERNEL_PAGE_CACHE_FAULT_SINGLE;
    return access==KERNEL_MM_EXECUTE ? KERNEL_PAGE_CACHE_FAULT_EXEC_AHEAD :
        KERNEL_PAGE_CACHE_FAULT_READ_AHEAD;
}

/* File-map preparation is independent of ELF/anonymous faults. Keep its
 * retirement and COW storage off the nested ELF-read stack. */
static __attribute__((noinline)) enum kernel_mm_status mapped_file_fault(struct mm_operation *operation,
    uint64_t address, uint32_t access, const struct fault_vma *view, int force)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_mm_record *record = operation->record;
    struct physical_page_allocator *allocator = record->space.allocator;
    const struct kernel_vma *vma = &view->vma;
    if (!remote_access_allowed(vma,access,force)) return KERNEL_MM_STATUS_ACCESS;
    address &= ~BOAROS_PAGE_MASK;
    if (vma->backing_offset > UINT64_MAX - (address - vma->start))
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    uint64_t offset = vma->backing_offset + address - vma->start;
    uint64_t generation = view->generation;
    struct kernel_open_file_description *file = vma->backing;
    struct kernel_mm_file_source *file_source = vma->backing_owner;
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    KERNEL_LOCK_SCOPE(inode);
    uint64_t epoch;
    uint64_t cookie = 0, old = KERNEL_MM_NO_PAGE, replacement = KERNEL_MM_NO_PAGE;
    struct kernel_open_file_page_preparation source = {0};
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    struct kernel_mm_retirement batch;
    batch.count = batch.nonleaf = batch.compound = 0;
    struct mm_file_resident *resident = 0;
    struct mm_file_resident *published = 0;
    void *old_bytes = 0, *copy = 0;
    unsigned missing = 0;
#if BOAROS_COST_DIAGNOSTICS
    unsigned count = 0;
#endif
    uint32_t targets = 0;
    int shared = vma->kind == KERNEL_VMA_KIND_FILE_SHARED;
    int disk = !kernel_open_file_memory_backed(file);
    int continuous_cow = 0;
    int absent = 0;
    enum kernel_mm_status status = KERNEL_MM_STATUS_OK;
    /* A missing disk read leaf does not access mutable inode contents
     * here. The cache admits the read against current EOF, and publication
     * still takes inode I/O qualification and rechecks the captured epoch. */
    if (disk && !force && access != KERNEL_MM_WRITE &&
        !kernel_file_mapping_epoch(node, &epoch) && offset < kernel_vfs_node_size(node)) {
        if (shared) {
            struct kernel_mm_leaf leaf;
            KERNEL_RAW_READ_SCOPE(guard, &record->metadata);
            absent = fault_lookup_leaf(record,address,0,&leaf,&missing,NULL) == KERNEL_MM_STATUS_NOT_MAPPED;
        }
        /* The private window already tests whether the demand slot is
         * empty. Shared aliases probe before upgrading the MM reader. */
        if ((!shared || (absent && !missing)) && !operation->populate && !operation->fault_pin &&
            !(vma->flags & KERNEL_VMA_RANDOM_READ) &&
            kernel_open_file_speculative_read_allowed(file) &&
            (!shared || !operation->shared || kernel_rwlock_try_upgrade(&operation->guard))) {
            if (shared) operation->shared = 0;
            if (kernel_vfs_node_try_read(node, &inode)) {
                uint64_t latest;
                if (!kernel_file_mapping_epoch(node,&latest) && latest == epoch) {
                    uint64_t first = address;
                    uint64_t end = map_file_neighbours(operation,file,NULL,view,address,&first,
                        &batch,&targets,NULL,1);
                    if (end) {
                        finish_fault_change(record,&batch,targets,first,end-first,0,
                            (vma->permissions & KERNEL_MM_EXECUTE) != 0);
                        return KERNEL_MM_STATUS_OK;
                    }
                }
                kernel_lock_release(&inode);
            }
        }
        if (!shared) {
            struct kernel_mm_leaf leaf;
            KERNEL_RAW_READ_SCOPE(guard, &record->metadata);
            absent = fault_lookup_leaf(record,address,0,&leaf,&missing,NULL) == KERNEL_MM_STATUS_NOT_MAPPED;
        }
    }
    /* The READY window stays under the original VMA qualification. Only
     * the remaining paths may drop it and need a separate source borrow. */
    if (!file_source || __atomic_fetch_add(&file_source->operations, 1, __ATOMIC_RELAXED) == UINT32_MAX) __builtin_trap();
    if (absent) {
        if (kernel_open_file_try_prepare_disk_mapping_page(file,offset >> BOAROS_PAGE_SHIFT,&source)) {
            replacement = source.address;
            if (shared || !operation->shared) mm_operation_release(operation);
        }
        goto prepare_page;
    }
    if (!fault_inode_lock(operation, node, &inode)) {
        status = KERNEL_MM_STATUS_CONFLICT;
        goto finish;
    }
    if (kernel_file_mapping_epoch(node, &epoch)) {
        kernel_lock_release(&inode);
        if (operation->guard.lock) mm_operation_release(operation);
        if (!current_pins(operation->mm)) kernel_file_mapping_wait(node);
        status = KERNEL_MM_STATUS_CONFLICT;
        goto finish;
    }
    if (offset >= kernel_open_file_size(file)) { status = KERNEL_MM_STATUS_BUS_FAULT; goto finish; }
    {
        struct kernel_mm_leaf leaf;
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        uint64_t *cow_slot = NULL;
        status = fault_lookup_leaf(record,address,force,&leaf,&missing,
            access == KERNEL_MM_WRITE ? &cow_slot : NULL);
        if (status == KERNEL_MM_STATUS_OK && shared && disk && access==KERNEL_MM_WRITE) {
            kernel_raw_lock_release(&guard);
#if !defined(BOAROS_ARCH_LOONGARCH)
            if ((leaf.permissions & KERNEL_MM_WRITE) && !force && !operation->populate && !operation->fault_pin) {
                arch_mmu_flush_address(address);
                goto finish;
            }
#endif
            status=cached_write_fault(operation,file,vma,address,generation,&inode);
            goto finish;
        }
        if (status == KERNEL_MM_STATUS_OK && ((leaf.permissions & access) || (force && access == KERNEL_MM_READ))) {
            kernel_raw_lock_release(&guard);
            if (!operation->populate) {
                arch_mmu_flush_all();
                if (vma->permissions & KERNEL_MM_EXECUTE) arch_mmu_sync_instructions();
            }
            goto finish;
        }
        if (status == KERNEL_MM_STATUS_OK) {
            if (shared || access != KERNEL_MM_WRITE ||
                !fault_cow_source(record,address,&leaf,force,&cookie,&old)) {
                status = KERNEL_MM_STATUS_ADDRESS_SPACE;
                goto finish;
            }
            uint32_t refs=physical_page_cow_borrow(allocator,old);
            if (refs == 1) {
                if (arch_mmu_user_cow_upgrade(&record->space,cow_slot,cookie,
                        backend_permissions_from_mm(vma->permissions))) __builtin_trap();
                targets = note_tlb_change_locked(record, &batch, 0);
                kernel_raw_lock_release(&guard);
                finish_tlb_change(record,&batch,targets,address,BOAROS_PAGE_SIZE,0);
                status = KERNEL_MM_STATUS_OK;
                old = KERNEL_MM_NO_PAGE;
                goto finish;
            }
            if (physical_page_map_owned(allocator,old,&old_bytes)) __builtin_trap();
        } else if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
            status = KERNEL_MM_STATUS_OK;
        } else goto finish;
    }
prepare_page:
    if (cookie && !operation->shared && !force) {
        /* The MM writer and inode reader already stabilize the copied
         * leaf's VMA and file range. Keep both on free-page allocation. */
        status = try_allocate_fault_page(allocator, &replacement, &copy);
        if (status == KERNEL_MM_STATUS_OK) continuous_cow = 1;
        else if (status == KERNEL_MM_STATUS_NO_MEMORY) status = KERNEL_MM_STATUS_OK;
        else goto finish;
    }
    if (!continuous_cow) {
        kernel_lock_scope_release(&inode);
        if (source.pending) goto prepare_file_tables;
        if (operation->guard.lock) mm_operation_release(operation);
        if (!cookie) {
            int error = kernel_open_file_prepare_mapping_page(file,offset >> BOAROS_PAGE_SHIFT,
                file_fault_read_window(vma,access),&source);
            status = !error ? KERNEL_MM_STATUS_OK : error == -KERNEL_EBUSY ? KERNEL_MM_STATUS_CONFLICT :
                error == -KERNEL_ENOMEM ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_BUS_FAULT;
            if (status == KERNEL_MM_STATUS_OK) {
                MM_DIAGNOSTIC(count += source.created);
                replacement = source.address;
            }
        }
    }
    if (status == KERNEL_MM_STATUS_OK && !shared && access == KERNEL_MM_WRITE) {
        if (!continuous_cow) status = allocate_private_page(allocator,&replacement,&copy);
        if (status == KERNEL_MM_STATUS_OK) {
            void *bytes = old_bytes;
            if (!cookie && physical_page_map_owned(allocator,source.address,&bytes)) __builtin_trap();
            { COST_SCOPE(copy_cost, MM_COW_COPY_TICKS);
              memcpy(copy, bytes, BOAROS_PAGE_SIZE); }
            MM_DIAGNOSTIC(count++);
        }
    }
prepare_file_tables:
    for (unsigned i=0; i<2 && status==KERNEL_MM_STATUS_OK; i++) if ((missing & (1U << i)) && prepared.tables[i]==KERNEL_MM_NO_PAGE) {
        void *table;
        status = allocate_fault_page(operation,&prepared.tables[i],&table);
        if (status == KERNEL_MM_STATUS_OK) { clear_page(table); MM_DIAGNOSTIC(count++); }
    }
    /* Complete the demand's table preparation before allocating mapping
     * metadata. Keep a READY page's qualification if neither step waits. */
    if (!status && !cookie && !resident) {
        size_t size = shared && disk ? sizeof(struct mm_file_alias_resident) : sizeof(*resident);
        if (operation->guard.lock &&
            kernel_heap_allocate_zeroed_nosleep(record->vma_heap,1,size,(void **)&resident))
            mm_operation_release(operation);
        if (!resident) {
            enum kernel_heap_status hs=kernel_heap_allocate_zeroed(record->vma_heap,
                1,size,(void **)&resident);
            if (hs) status=hs==KERNEL_HEAP_STATUS_EMPTY ?
                KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
        }
    }
    /* Private first faults publish only their PTE and resident under metadata.
     * Shared aliases and COW replacement retain the exclusive operation. */
    operation->shared = !shared && !cookie;

    enum kernel_mm_status gate_status = operation->guard.lock ? KERNEL_MM_STATUS_OK :
        mm_operation_lock(operation);
    if (gate_status != KERNEL_MM_STATUS_OK) status = gate_status;
    if (!gate_status && operation->shared) {
        int full;
        { KERNEL_RAW_READ_SCOPE(raw, &record->metadata);
          full = record->resident_count == record->resident_capacity; }
        if (full) {
            mm_operation_release(operation);
            operation->shared = 0;
            gate_status = mm_operation_lock(operation);
            if (gate_status) status = gate_status;
        }
    }
#if BOAROS_COST_DIAGNOSTICS
    { KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata); record->prepared_pages += count; count=0; }
#endif
    if (status == KERNEL_MM_STATUS_OK && !continuous_cow) {
        if (!fault_inode_lock(operation, node, &inode)) status = KERNEL_MM_STATUS_CONFLICT;
        uint64_t latest, latest_generation;
        {
            KERNEL_LOCK_SCOPE(vma_guard);
            if (operation->shared) kernel_rwlock_read(&record->vma_lock, &vma_guard);
            latest_generation = kernel_vma_set_generation(record->vmas);
        }
        if (!status && (generation != latest_generation ||
            kernel_file_mapping_epoch(node,&latest) || latest != epoch ||
            (!disk && source.pending && source.epoch != epoch) || offset >= kernel_open_file_size(file)))
            status = KERNEL_MM_STATUS_CONFLICT;
    }
    if (status == KERNEL_MM_STATUS_OK && !cookie && !operation->shared)
        status = reserve_file_residents(record,record->resident_count + 1);
    int map_neighbours = 0, defer_sync = 0;
    int instructions = (vma->permissions & KERNEL_MM_EXECUTE) != 0;
    if (status == KERNEL_MM_STATUS_OK) {
        map_neighbours = !cookie && disk && !force && !operation->populate &&
            access != KERNEL_MM_WRITE && !(vma->flags & KERNEL_VMA_RANDOM_READ) &&
            kernel_open_file_speculative_read_allowed(file);
#if defined(BOAROS_ARCH_LOONGARCH)
        defer_sync = map_neighbours;
#else
        /* A new RV data leaf changes no tracked invalidation generation. */
        defer_sync = map_neighbours && instructions;
#endif
        enum arch_mmu_status backend;
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            if (!cookie && record->resident_count == record->resident_capacity)
                backend = ARCH_MMU_STATUS_CONFLICT;
            else if (cookie) backend = arch_mmu_user_cow_commit(&record->space,address,cookie,replacement,
                backend_permissions_from_mm(vma->permissions),&batch);
            else {
                backend = arch_mmu_user_map_prepared_page(&record->space,address,replacement,
                    backend_permissions_from_mm((!shared && access != KERNEL_MM_WRITE) || (shared && disk) ?
                        user_effective_permissions(vma->permissions) & ~KERNEL_MM_WRITE : vma->permissions),
                    !shared && access != KERNEL_MM_WRITE,&prepared);
                missing = prepared.table_mask;
            }
            if (!backend) {
                struct mm_file_resident *page = resident ? resident : find_file_resident(record,address);
                if (!page) __builtin_trap();
                page->address=address; page->physical_address=replacement;
                page->private=!shared && access==KERNEL_MM_WRITE;
                if (resident) {
                    resident->with_alias = shared && disk;
                    publish_file_resident(record,resident);
                    resident=0;
                }
                published = page;
                if (!defer_sync)
                    targets=note_fault_change_locked(record,&batch,cookie!=0,instructions);
            }
        }
        if (backend == ARCH_MMU_STATUS_NEED_TABLES) {
            /* truncate may retire a neighbouring mapping's last table without
             * changing our VMA. Keep our data and allocate only missing tables. */
            kernel_lock_release(&inode);
            mm_operation_release(operation);
            goto prepare_file_tables;
        }
        status = !backend ? KERNEL_MM_STATUS_OK : backend == ARCH_MMU_STATUS_CONFLICT ||
            backend == ARCH_MMU_STATUS_NOT_MAPPED ? KERNEL_MM_STATUS_CONFLICT : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (status == KERNEL_MM_STATUS_OK) {
        uint64_t begin=address,end=address+BOAROS_PAGE_SIZE;
        if (map_neighbours)
            end = map_file_neighbours(operation,file,NULL,view,address,&begin,
                &batch,defer_sync ? &targets : NULL, shared && disk ? published : NULL,0);
        if (defer_sync && end - begin == BOAROS_PAGE_SIZE) {
            KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
            targets=note_fault_change_locked(record,&batch,0,instructions);
        }
        if (cookie) {
            /* The old PTE and the copy borrow hold two references to the
             * same page. Return both after the replacement is confirmed. */
            kernel_mm_retirement_add(&batch, old, 0);
            old = KERNEL_MM_NO_PAGE;
        }
        finish_fault_change(record,&batch,targets,missing?0:begin,
            missing?UINT64_MAX:end-begin,cookie!=0,instructions);
        if (source.pending) {
            kernel_open_file_finish_mapping_page(file,&source,1);
            if (!shared && access==KERNEL_MM_WRITE) physical_page_release(allocator,source.address);
        }
        replacement=KERNEL_MM_NO_PAGE;
        if (shared && disk && !cookie) {
            struct mm_file_alias_resident *alias_owner = file_alias_resident(published);
            if (!kernel_page_cache_alias_active(&alias_owner->alias) &&
                kernel_open_file_alias_attach(file,offset>>BOAROS_PAGE_SHIFT,
                    published->physical_address,&alias_owner->alias,alias_owner,address,
                    rearm_shared_file_alias)) __builtin_trap();
        }
        if (shared && access==KERNEL_MM_WRITE) {
            if (disk) status=cached_write_fault(operation,file,vma,address,generation,&inode);
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
    int drain = operation->shared && !kernel_io_context_current()->user_pins;
    mm_operation_end(operation);
    if (drain) kernel_mm_drain_sources(operation->mm);
    return status;
}
/* Stack growth edits and its VMA candidates are a separate operation from
 * the normal file-fault path; do not retain those frames through device waits. */
static __attribute__((noinline)) enum kernel_mm_status grow_fault_stack(struct kernel_mm_record *record,
    uint64_t address,uint32_t access,struct kernel_vma *result,uint64_t *epoch)
{
    struct kernel_vma vma;
    uint64_t generation=0;
    enum kernel_vma_status vs;
    struct kernel_lock_guard vma_guard = {0};
    kernel_rwlock_write(&record->vma_lock, &vma_guard);
    uint64_t page = address & ~BOAROS_PAGE_MASK;
    if (kernel_vma_set_next(record->vmas, page, &vma) ||
        !kernel_vma_grows_down(&vma) ||
        !(user_effective_permissions(vma.permissions) & access) ||
        vma.end - page > kernel_task_current_stack_limit() ||
        !may_expand_current(record, &vma, vma.start - page) ||
        ((vma.flags & KERNEL_VMA_LOCKED) && !may_lock_memory(record, vma.start - page))) {
        kernel_lock_release(&vma_guard);
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    int overlaps;
    /* Linux 栈增长与可访问的前一映射保留 256 个架构页的间隔。 */
    if (page < 2 * BOAROS_PAGE_SIZE ||
        kernel_vma_set_overlaps(record->vmas, page, vma.start, &overlaps) || overlaps) {
        kernel_lock_release(&vma_guard);
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    struct kernel_vma previous;
    enum kernel_vma_status prev_status = kernel_vma_set_previous(record->vmas, page, &previous);
    if (prev_status != KERNEL_VMA_STATUS_OK && prev_status != KERNEL_VMA_STATUS_NOT_FOUND)
        __builtin_trap();
    if (prev_status == KERNEL_VMA_STATUS_OK && !kernel_vma_grows_down(&previous) &&
        (previous.permissions & (KERNEL_MM_READ | KERNEL_MM_WRITE | KERNEL_MM_EXECUTE)) &&
        page - previous.end < KERNEL_VMA_STACK_GUARD_PAGES * BOAROS_PAGE_SIZE) {
        kernel_lock_release(&vma_guard);
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    struct kernel_vma growth = vma;
    growth.start = page;
    growth.end = vma.start;
    vs = kernel_vma_set_insert(record->vmas, &growth);
    if (vs) {
        kernel_lock_release(&vma_guard);
        return status_from_vma(vs);
    }
    if (kernel_vma_set_lookup(record->vmas, address, &vma)) __builtin_trap();
    generation = kernel_vma_set_generation(record->vmas);
    kernel_lock_release(&vma_guard);
    *result=vma;*epoch=generation;return KERNEL_MM_STATUS_OK;
}
/* The raw metadata guard stabilizes the range-claim list. */
static int fault_range_claimed(struct kernel_mm_record *record, uint64_t first, uint64_t end)
{
    for (struct kernel_mm_range_claim *c=record->range_claims;c;c=c->next)
        if (c->start < end && c->end > first) return 1;
    return 0;
}

/* The caller holds VMA qualification and raw metadata through publication.
 * Only anonymous mappings accept an unrelated edit under a new generation. */
static __attribute__((noinline)) int memory_fault_view_changed(struct kernel_mm_record *record,
    uint64_t address, const struct fault_vma *view)
{
    if (fault_range_claimed(record, address, address+1)) return 1;
    if (kernel_vma_set_generation(record->vmas) == view->generation) return 0;
    const struct kernel_vma *original = &view->vma;
    struct kernel_vma latest;
    return original->kind != KERNEL_VMA_KIND_ANONYMOUS ||
        kernel_vma_set_lookup(record->vmas, address, &latest) ||
        latest.kind != original->kind || latest.start != original->start ||
        latest.end != original->end || latest.permissions != original->permissions ||
        latest.role != original->role || latest.fault_policy != original->fault_policy ||
        latest.backing_owner != original->backing_owner;
}

/* With a continuous MM read gate, VMA writers that can change this anonymous
 * address must hold a range claim. Nonfixed anonymous mmap only inserts into
 * holes and anonymous-history marking does not change the fault's access.
 * Judge both live claims and completed edits under the PTE publication guard. */
static int anonymous_fault_view_current(struct kernel_mm_record *record,
    uint64_t first, uint64_t end, const struct fault_vma *view)
{
    return view->vma.kind == KERNEL_VMA_KIND_ANONYMOUS &&
        __atomic_load_n(&record->range_edit_generation, __ATOMIC_RELAXED) ==
            view->range_edit_generation &&
        !fault_range_claimed(record, first, end);
}

#if !defined(BOAROS_ARCH_LOONGARCH)
#define ANONYMOUS_FAULT_WINDOW 8U
#define ANONYMOUS_FAULT_WINDOW_MAX 16U
_Static_assert(ANONYMOUS_FAULT_WINDOW_MAX <= PHYSICAL_PAGE_ALLOCATION_BATCH_MAX &&
    ARCH_MMU_HUGE_SIZE % (ANONYMOUS_FAULT_WINDOW_MAX * BOAROS_PAGE_SIZE) == 0,
    "anonymous faults must fit one allocation batch and one leaf table");

struct anonymous_fault_window {
    uint64_t first, end;
    uint64_t pages[ANONYMOUS_FAULT_WINDOW_MAX];
    unsigned mask, demand, installed;
    int direction;
};

/* The demand lookup already holds metadata. Candidate contents carry no
 * page-table pointer across allocation or a concurrent range edit. */
static void select_anonymous_window(struct kernel_mm_record *record,
    const struct fault_vma *view, uint64_t address, int write, struct anonymous_fault_window *window)
{
    const struct kernel_vma *vma = &view->vma;
    const uint64_t page = BOAROS_PAGE_SIZE;
    uint64_t previous = record->anonymous_access[write].last;
    if (previous < vma->start || previous >= vma->end) return;
    int direction = address == previous + page ? 1 : address + page == previous ? -1 : 0;
    if (!direction) return;

    /* A fault beyond the previous boundary confirms the direction. Align
     * the batch to keep widely spaced writes from prefilling both windows. */
    window->direction = direction;
    uint64_t span = page * (record->anonymous_access[write].direction == direction ?
        ANONYMOUS_FAULT_WINDOW_MAX : ANONYMOUS_FAULT_WINDOW);
    uint64_t base = address & ~(span-1);
    uint64_t first = direction > 0 ? address : (base > vma->start ? base : vma->start);
    uint64_t end = direction < 0 ? address + page :
        (base + span < vma->end ? base + span : vma->end);
    unsigned empty, slot = (address-first)/page, demand = 1U << slot;
    if (end-first == page || !anonymous_fault_view_current(record,first,end,view) ||
        arch_mmu_user_empty_window(&record->space,first,(end-first)/page,&empty) ||
        !(empty & demand) || empty == demand) return;
    window->first = first;
    window->end = end;
    window->demand = slot;
    window->mask = empty;
}

static __attribute__((noinline)) int allocate_anonymous_write_window(
    struct physical_page_allocator *allocator, struct anonymous_fault_window *window,
    uint64_t *replacement)
{
    if (physical_page_available(allocator) <= physical_page_total(allocator) / 128 + 64)
        return 0;
    unsigned count = 0;
    for (unsigned bits = window->mask; bits; bits &= bits-1) ++count;
    unsigned prepared = physical_page_allocate_batch(allocator, window->pages, count);
    if (prepared != count) {
        physical_page_release_batch(allocator, window->pages, prepared);
        return 0;
    }
    unsigned index = 0;
    for (unsigned bits = window->mask; bits; bits &= bits-1) {
        void *mapped;
        if (physical_page_map_owned(allocator, window->pages[index], &mapped)) __builtin_trap();
        clear_page(mapped);
        if ((bits & -bits) == (1U << window->demand)) *replacement = window->pages[index];
        ++index;
    }
    return count;
}

static void release_unused_anonymous_neighbours(struct physical_page_allocator *allocator,
    struct anonymous_fault_window *window)
{
    unsigned unused = 0, index = 0;
    for (unsigned bits = window->mask; bits; bits &= bits-1) {
        if (!((window->installed | (1U << window->demand)) & (bits & -bits)))
            window->pages[unused++] = window->pages[index];
        ++index;
    }
    physical_page_release_batch(allocator, window->pages, unused);
    window->mask = 0;
}
#endif

static __attribute__((noinline)) int userfault_resume_resident_trap(struct kernel_mm_record *record,
    uint64_t address, uint32_t access, const struct fault_vma *view)
{
    struct mm_gate_scope gate __attribute__((cleanup(mm_gate_scope_end))) = {.record = record};
    if (!kernel_rwlock_try_read(&record->operations, &gate.guard)) return 0;
    KERNEL_LOCK_SCOPE(vmas);
    if (BOAROS_MAX_CPUS > 1 && !kernel_rwlock_try_read(&record->vma_lock, &vmas)) return 0;
    {
        KERNEL_RAW_READ_SCOPE(raw, &record->metadata);
        struct kernel_mm_leaf leaf;
        if (kernel_vma_set_generation(record->vmas) != view->generation ||
            fault_range_claimed(record, address, address + 1) ||
            arch_mmu_user_leaf_snapshot(&record->space, address, &leaf) ||
            leaf.order || !(leaf.permissions & access))
            return 0;
    }
    /* The unchanged registration delegates every missing leaf to fill,
     * which completes local translation and instruction sync under the
     * writer gate. On SMP the faulting task may resume on a different CPU. */
    if (BOAROS_MAX_CPUS > 1) {
        arch_mmu_flush_all();
        if (view->vma.permissions & KERNEL_MM_EXECUTE) arch_mmu_sync_instructions();
    }
    return 1;
}

static __attribute__((noinline)) enum kernel_mm_status wait_for_userfault(
    struct mm_operation *operation, uint64_t address, uint32_t access,
    const struct fault_vma *view)
{
    struct kernel_mm_record *record = operation->record;
    /* A delegated fault can wait indefinitely. Only an actor that can
     * release all of its MM qualifications and source pins may enroll. */
    struct kernel_task *task = kernel_task_current();
    if (!task || task->proc_exiting || operation->guard.previous ||
        kernel_io_context_current()->user_pins) return KERNEL_MM_STATUS_BUS_FAULT;

    struct kernel_userfault_ctx *ctx = kernel_vma_userfault(&view->vma);
    struct kernel_userfault_wait wait = {0};
    int error;
    {
        KERNEL_LOCK_SCOPE(vmas);
        if (BOAROS_MAX_CPUS > 1) {
            if (operation->shared) kernel_rwlock_read(&record->vma_lock, &vmas);
            KERNEL_RAW_READ_SCOPE(raw, &record->metadata);
            struct kernel_mm_leaf leaf;
            if (memory_fault_view_changed(record, address, view) ||
                arch_mmu_user_leaf_snapshot(&record->space, address, &leaf) !=
                    ARCH_MMU_STATUS_NOT_MAPPED)
                return KERNEL_MM_STATUS_CONFLICT;
        }
        /* A manager's PTE publication and registration changes take the
         * exclusive MM gate. The UP caller has held it since the absent
         * leaf probe, while the VMA reader excludes SMP shared-gate edits. */
        error = kernel_userfault_prepare(ctx, address, access, operation->fault_origin, &wait);
    }
    if (error) {
        if (error == -KERNEL_EAGAIN) return KERNEL_MM_STATUS_CONFLICT;
        return error == -KERNEL_ENOMEM ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_BUS_FAULT;
    }
    /* The prepared token already owns the caller's scheduler wait slot.
     * Release qualifications without draining unrelated blocking OFD work. */
    mm_operation_release(operation);
    enum kernel_wait_wake_reason reason = kernel_userfault_wait(ctx, &wait);
    if (reason == KERNEL_WAIT_SIGNALLED)
        return operation->fault_origin == KERNEL_USERFAULT_TRAP ?
            KERNEL_MM_STATUS_OK : KERNEL_MM_STATUS_BUS_FAULT;
    if (reason == KERNEL_WAIT_WOKEN && operation->fault_origin == KERNEL_USERFAULT_TRAP &&
        userfault_resume_resident_trap(record, address, access, view))
        return KERNEL_MM_STATUS_OK;
    return KERNEL_MM_STATUS_CONFLICT;
}

static __attribute__((noinline)) enum kernel_mm_status memory_fault(struct mm_operation *operation,
    uint64_t address, uint32_t access, const struct fault_vma *view, int force)
{
    struct kernel_mm_record *record = operation->record;
    struct physical_page_allocator *allocator = record->space.allocator;
    const struct kernel_vma *vma = &view->vma;
    if (!remote_access_allowed(vma,access,force)) return KERNEL_MM_STATUS_ACCESS;
    int shared = vma->kind == KERNEL_VMA_KIND_ANON_SHARED || vma->kind == KERNEL_VMA_KIND_SYSV_SHM;
    int elf = vma->kind == KERNEL_VMA_KIND_ELF_PRIVATE;
    if (!shared && !elf && vma->kind != KERNEL_VMA_KIND_ANONYMOUS) __builtin_trap();
    struct kernel_memory_object *object = shared ? vma->backing : 0;
    struct kernel_elf64_source *source = NULL;
    struct kernel_mm_elf_source *source_owner = NULL;
    int source_shared = 0, zero_alias = 0;
    int created = 0;
    enum kernel_mm_status status = KERNEL_MM_STATUS_OK;
    address &= ~BOAROS_PAGE_MASK;
    if (object && !kernel_memory_object_contains_offset(object,
            vma->backing_offset + address - vma->start)) return KERNEL_MM_STATUS_BUS_FAULT;
    uint64_t cookie = 0, old = 0, object_epoch = 0;
    uint64_t replacement = KERNEL_MM_NO_PAGE;
    uint64_t shared_preparation = KERNEL_MM_NO_PAGE;
    void *old_pointer = 0, *new_pointer;
    unsigned missing = 0;
#if BOAROS_COST_DIAGNOSTICS
    unsigned prepared_count = 0;
#endif
    uint32_t targets = 0;
    /* Retirement slots are initialized by append. A new translation leaves
     * the batch empty and must not clear those slots on every first fault. */
    struct kernel_mm_retirement batch;
    batch.count = batch.nonleaf = batch.compound = 0;
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
#if !defined(BOAROS_ARCH_LOONGARCH)
    struct anonymous_fault_window window;
    window.mask = window.installed = 0;
    window.direction = 0;
    int track_anonymous = 0;
#endif
    {
        struct kernel_mm_leaf leaf;
        KERNEL_LOCK_SCOPE(vma_guard);
        KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
        uint64_t *cow_slot = NULL;
        status = fault_lookup_leaf(record,address,force,&leaf,&missing,
            access == KERNEL_MM_WRITE ? &cow_slot : NULL);
        if (status == KERNEL_MM_STATUS_OK && ((leaf.permissions & access) || (force && access == KERNEL_MM_READ))) {
            pin_private_fault(operation,vma,leaf.physical_address+address-leaf.virtual_address,access);
            kernel_raw_lock_release(&guard);
            if (!operation->populate) {
                /* Another CPU may have published this leaf or its parents
                 * after the hardware fault cached an invalid translation. */
                arch_mmu_flush_all();
                if (vma->permissions & KERNEL_MM_EXECUTE) arch_mmu_sync_instructions();
            }
            return KERNEL_MM_STATUS_OK;
        }
        if (status == KERNEL_MM_STATUS_OK) {
            if (operation->shared && access==KERNEL_MM_WRITE &&
                !anonymous_fault_view_current(record, address, address+1, view)) {
                /* A single-owner COW page can be upgraded without copying.
                 * Its saved VMA still needs the same publication check as
                 * a prepared page, including a concurrent mprotect upgrade. */
                kernel_raw_lock_release(&guard);
                kernel_rwlock_read(&record->vma_lock,&vma_guard);
                kernel_raw_rwlock_write_acquire(&record->metadata,&guard);
                if (memory_fault_view_changed(record,address,view))
                    return KERNEL_MM_STATUS_CONFLICT;
                status=fault_lookup_leaf(record,address,force,&leaf,&missing,&cow_slot);
                if (status!=KERNEL_MM_STATUS_OK || (leaf.permissions&access))
                    return KERNEL_MM_STATUS_CONFLICT;
            }
            if (shared || access != KERNEL_MM_WRITE ||
                !fault_cow_source(record,address,&leaf,force,&cookie,&old))
                return KERNEL_MM_STATUS_ADDRESS_SPACE;
            uint32_t references=physical_page_cow_borrow(allocator,old);
            if (references == 1) {
                if (arch_mmu_user_cow_upgrade(&record->space,cow_slot,cookie,
                    backend_permissions_from_mm(vma->permissions)) != ARCH_MMU_STATUS_OK)
                    __builtin_trap();
                struct mm_lazy_page *lazy=lazy_find(record,address);
                if (lazy) lazy_retire(record,lazy);
                pin_private_fault(operation,vma,old,access);
#if defined(BOAROS_ARCH_LOONGARCH)
                targets = note_tlb_change_locked(record, &batch, 0);
#endif
                kernel_raw_lock_release(&guard);
                kernel_lock_scope_release(&vma_guard);
                if (lazy) lazy_drain(record);
#if defined(BOAROS_ARCH_LOONGARCH)
                finish_tlb_change(record, &batch, targets, address, BOAROS_PAGE_SIZE,0);
#else
                /* The PTE still refers to the same page. Remote read-only
                 * translations can retry through the already-writable case. */
                if (batch.count) __builtin_trap();
                sync_tlb_change(0, address, BOAROS_PAGE_SIZE, 0);
#endif
                return KERNEL_MM_STATUS_OK;
            }
            /* COW旧页为只读快照；实际引用覆盖解锁后的复制和竞争者提交。 */
            if (old != __atomic_load_n(&record->anonymous_zero_page, __ATOMIC_RELAXED) &&
                physical_page_map_owned(allocator, old, &old_pointer) != PHYSICAL_PAGE_STATUS_OK) {
                physical_page_release(allocator, old);
                return KERNEL_MM_STATUS_PAGE_ACCESS;
            }
        } else if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
            if (__builtin_expect(vma->flags & KERNEL_VMA_USERFAULT_MISSING, 0)) {
                kernel_raw_lock_release(&guard);
                return wait_for_userfault(operation, address, access, view);
            }
            if (!shared && !elf && vma->role != KERNEL_VMA_ROLE_VDSO &&
                vma->role != KERNEL_VMA_ROLE_VVAR &&
                vma->fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO) return status;
            zero_alias = vma->kind == KERNEL_VMA_KIND_ANONYMOUS &&
                vma->fault_policy == KERNEL_VMA_FAULT_DEMAND_ZERO &&
                vma->role != KERNEL_VMA_ROLE_VDSO && vma->role != KERNEL_VMA_ROLE_VVAR &&
                access == KERNEL_MM_READ && !operation->populate && !force &&
                !(vma->flags & KERNEL_VMA_LOCK_MASK);
#if !defined(BOAROS_ARCH_LOONGARCH)
            track_anonymous = (BOAROS_MAX_CPUS == 1 || operation->shared) &&
                (access == KERNEL_MM_WRITE || zero_alias) && operation->fault_origin == KERNEL_USERFAULT_TRAP &&
                vma->kind == KERNEL_VMA_KIND_ANONYMOUS && !operation->populate &&
                !operation->fault_pin && !force &&
                !(vma->permissions & KERNEL_MM_EXECUTE) &&
                !(vma->flags & (KERNEL_VMA_RANDOM_READ | KERNEL_VMA_LOCK_MASK));
            if (track_anonymous && !missing)
                select_anonymous_window(record,view,address,!zero_alias,&window);
#endif
        } else return status;
    }
    /* VMA注册覆盖此处取得实际后备owner；放gate后不再借用registry节点。 */
    if (object && kernel_memory_object_acquire(object) != KERNEL_MEMORY_OBJECT_OK)
        return KERNEL_MM_STATUS_STATE;
    if (object && kernel_memory_object_epoch(object,&object_epoch)) {
        mm_operation_release(operation);
        /* 关闭者随后要排空本MM的pin；持pin的缺页者须先返回冲突归还它，
         * 不能等待仍依赖该pin的对象重新开放。 */
        if (!current_pins(operation->mm)) kernel_memory_object_wait(object);
        kernel_memory_object_release(&object);
        return KERNEL_MM_STATUS_CONFLICT;
    }
    if (elf && !cookie) {
        source = vma->backing;
        source_owner = vma->backing_owner;
        if (!source_owner || __atomic_fetch_add(&source_owner->faults, 1, __ATOMIC_RELAXED) == UINT32_MAX) __builtin_trap();
    }
    /* The source COW reference covers the copy; a nonblocking allocation
     * keeps the original gate on the single-core anonymous path. */
#if BOAROS_MAX_CPUS > 1
    int keep_gate = operation->shared;
#else
    int keep_gate = vma->kind == KERNEL_VMA_KIND_ANONYMOUS && !force;
#endif
    if (!keep_gate || object || elf || current_pins(operation->mm))
        mm_operation_release(operation);
    /* The publishing writer also drains retired ELF owners after the fault. */
    if (elf) operation->shared = 0;
    if (source) {
        enum kernel_elf64_source_status result = kernel_elf64_source_page(source, allocator,
            vma->backing_offset + address - vma->start, &replacement, &source_shared,
            file_fault_read_window(vma,access));
        status = !result ? KERNEL_MM_STATUS_OK : result == KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY ?
            KERNEL_MM_STATUS_NO_MEMORY : result == KERNEL_ELF64_SOURCE_STATUS_IO ?
            KERNEL_MM_STATUS_BUS_FAULT : result == KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED ?
            KERNEL_MM_STATUS_CLEANUP_REQUIRED : KERNEL_MM_STATUS_ADDRESS_SPACE;
        if (!status && source_shared && access == KERNEL_MM_WRITE) {
            uint64_t original = replacement;
            void *bytes;
            replacement = KERNEL_MM_NO_PAGE;
            status = allocate_fault_page(operation, &replacement, &new_pointer);
            if (!status) {
                if (physical_page_map_owned(allocator, original, &bytes)) __builtin_trap();
                { COST_SCOPE(copy_cost, MM_COW_COPY_TICKS);
                  memcpy(new_pointer, bytes, BOAROS_PAGE_SIZE); }
                source_shared = 0;
            }
            physical_page_release(allocator, original);
        }
    } else if (object) {
        enum kernel_memory_object_status result = kernel_memory_object_prepare_page(object,
            (vma->backing_offset + address - vma->start) >> BOAROS_PAGE_SHIFT,
            &replacement, &created);
        status = result == KERNEL_MEMORY_OBJECT_OK ? KERNEL_MM_STATUS_OK :
            result == KERNEL_MEMORY_OBJECT_NO_MEMORY ? KERNEL_MM_STATUS_NO_MEMORY : KERNEL_MM_STATUS_STATE;
        if (status == KERNEL_MM_STATUS_OK) {
            MM_DIAGNOSTIC(prepared_count += created != 0);
            shared_preparation = replacement;
        }
#if KERNEL_HAS_VDSO_ELF
    } else if (vma->role == KERNEL_VMA_ROLE_VVAR) {
        /* Even after DONTNEED the MM still owns the shared clock page. */
        replacement = record->vvar_page;
        if (cookie || !record->vdso_address ||
            physical_page_acquire(allocator, replacement) != PHYSICAL_PAGE_STATUS_OK)
            __builtin_trap();
        status = KERNEL_MM_STATUS_OK;
#endif
    } else if (zero_alias) {
        uint64_t page;
        status = anonymous_zero_template(operation, &page);
#if !defined(BOAROS_ARCH_LOONGARCH)
        if (status != KERNEL_MM_STATUS_OK || !operation->guard.lock) window.mask = 0;
        if (window.mask) {
            unsigned count = 0;
            for (unsigned bits = window.mask; bits; bits &= bits-1)
                window.pages[count++] = page;
            physical_page_acquire_batch(allocator, window.pages, count);
            replacement = page;
        } else
#endif
        if (status == KERNEL_MM_STATUS_OK) {
            if (physical_page_acquire(allocator, page)) __builtin_trap();
            replacement = page;
        }
    } else {
#if !defined(BOAROS_ARCH_LOONGARCH)
        int allocated = window.mask && operation->guard.lock ?
            allocate_anonymous_write_window(allocator,&window,&replacement) : 0;
        if (allocated) {
            status = KERNEL_MM_STATUS_OK;
            MM_DIAGNOSTIC(prepared_count += allocated);
        } else {
            window.mask = 0;
#endif
        status = allocate_fault_page(operation, &replacement, &new_pointer);
        if (status == KERNEL_MM_STATUS_OK) {
            MM_DIAGNOSTIC(prepared_count++);
            if (cookie && old_pointer) {
                { COST_SCOPE(copy_cost, MM_COW_COPY_TICKS);
                  memcpy(new_pointer, old_pointer, BOAROS_PAGE_SIZE); }
            } else {
                clear_page(new_pointer);
                if (!cookie && vma->role == KERNEL_VMA_ROLE_VDSO) {
                    size_t size = (size_t)(arch_elf_vdso_end - arch_elf_vdso_start);
                    if (size > BOAROS_PAGE_SIZE) __builtin_trap();
                    memcpy(new_pointer, arch_elf_vdso_start, size);
                }
            }
        }
#if !defined(BOAROS_ARCH_LOONGARCH)
        }
#endif
    }
prepare_memory_tables:
    for (unsigned i = 0; i < 2 && status == KERNEL_MM_STATUS_OK; i++) if ((missing & (1U << i)) && prepared.tables[i] == KERNEL_MM_NO_PAGE) {
        void *pointer;
        status = allocate_fault_page(operation, &prepared.tables[i], &pointer);
        if (status == KERNEL_MM_STATUS_OK) { clear_page(pointer); MM_DIAGNOSTIC(prepared_count++); }
    }
    int continuous_gate = operation->guard.lock != NULL;
    enum kernel_mm_status gate_status = continuous_gate ?
        KERNEL_MM_STATUS_OK : mm_operation_lock(operation);
    if (gate_status != KERNEL_MM_STATUS_OK) status = gate_status;
#if BOAROS_COST_DIAGNOSTICS
    { KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata); record->prepared_pages += prepared_count; prepared_count=0; }
#endif
    int refill = 0, lazy_retired = 0;
    int fault_batch=source && source_shared && access!=KERNEL_MM_WRITE && !force &&
        !operation->populate && !(vma->flags&KERNEL_VMA_RANDOM_READ);
    if (status == KERNEL_MM_STATUS_OK) {
        KERNEL_LOCK_SCOPE(vma_guard);
        struct kernel_raw_guard guard __attribute__((cleanup(kernel_raw_scope_release))) = {0};
        int current_view = 0;
#if BOAROS_MAX_CPUS == 1
        if (continuous_gate) {
            kernel_raw_rwlock_write_acquire(&record->metadata, &guard);
            current_view = 1;
        }
#else
        if (operation->shared && continuous_gate &&
            vma->kind == KERNEL_VMA_KIND_ANONYMOUS) {
            kernel_raw_rwlock_write_acquire(&record->metadata, &guard);
            current_view = anonymous_fault_view_current(record, address, address+1, view);
            if (!current_view) kernel_raw_lock_release(&guard);
        }
#endif
        if (!current_view) {
            /* A dropped MM gate or completed range edit needs the current VMA. */
            if (operation->shared)
                kernel_rwlock_read(&record->vma_lock, &vma_guard);
            kernel_raw_rwlock_write_acquire(&record->metadata, &guard);
        }
        uint64_t latest_epoch=0;
        if ((!current_view && memory_fault_view_changed(record,address,view)) ||
            (object && (kernel_memory_object_epoch(object,&latest_epoch) || latest_epoch!=object_epoch)))
            status = KERNEL_MM_STATUS_CONFLICT;
        else {
            enum arch_mmu_status backend;
            if (cookie) backend = arch_mmu_user_cow_commit(&record->space, address, cookie,
                replacement, backend_permissions_from_mm(vma->permissions), &batch);
            else {
#if !defined(BOAROS_ARCH_LOONGARCH)
                if (window.mask && current_view && anonymous_fault_view_current(record,
                    window.first,window.end,view))
                    window.installed = riscv_sv39_user_map_required_window(&record->space,
                        window.first,window.mask,window.pages,backend_permissions_from_mm(zero_alias ?
                            user_effective_permissions(vma->permissions) & ~KERNEL_MM_WRITE :
                            vma->permissions),zero_alias,window.demand);
                if (window.installed) backend = ARCH_MMU_STATUS_OK;
                else
#endif
                {
                    int cow = source_shared || zero_alias;
                    uint32_t permissions = cow ? user_effective_permissions(vma->permissions) &
                        ~KERNEL_MM_WRITE : vma->permissions;
                    backend = arch_mmu_user_map_prepared_page(&record->space, address, replacement,
                        backend_permissions_from_mm(permissions), cow, &prepared);
                }
                missing = prepared.table_mask;
                if (backend == ARCH_MMU_STATUS_NEED_TABLES) {
                    refill = 1;
                    goto memory_tables_changed;
                }
            }
            status = backend == ARCH_MMU_STATUS_OK ? KERNEL_MM_STATUS_OK :
                backend == ARCH_MMU_STATUS_CONFLICT || backend == ARCH_MMU_STATUS_NOT_MAPPED ?
                KERNEL_MM_STATUS_CONFLICT : KERNEL_MM_STATUS_ADDRESS_SPACE;
            if (status == KERNEL_MM_STATUS_OK) {
#if !defined(BOAROS_ARCH_LOONGARCH)
                if (current_view && track_anonymous) {
                    uint64_t last = address;
                    if (window.installed && window.installed == window.mask)
                        last = window.direction > 0 ? window.end-BOAROS_PAGE_SIZE : window.first;
                    record->anonymous_access[!zero_alias].last = last;
                    record->anonymous_access[!zero_alias].direction = window.direction;
                }
#endif
                if (cookie) {
                    struct mm_lazy_page *lazy=lazy_find(record,address);
                    if (lazy) { lazy_retire(record,lazy);lazy_retired=1; }
                }
                pin_private_fault(operation,vma,replacement,access);
                replacement = KERNEL_MM_NO_PAGE;
                if (!fault_batch)
                    targets=note_fault_change_locked(record,&batch,cookie!=0,
                        (vma->permissions&KERNEL_MM_EXECUTE)!=0);
            }
        }
    }
    if (lazy_retired) lazy_drain(record);
memory_tables_changed:
#if !defined(BOAROS_ARCH_LOONGARCH)
    if (window.mask) release_unused_anonymous_neighbours(allocator,&window);
#endif
    if (refill) {
        mm_operation_release(operation);
        goto prepare_memory_tables;
    }
    if (status == KERNEL_MM_STATUS_OK) {
        uint64_t begin=address,end=address+BOAROS_PAGE_SIZE;
#if !defined(BOAROS_ARCH_LOONGARCH)
        if (window.installed) {
            begin = window.first;
            end = window.end;
        }
#endif
        if (fault_batch) {
            end=map_file_neighbours(operation,NULL,source,view,address,&begin,&batch,&targets,NULL,0);
            if (end - begin == BOAROS_PAGE_SIZE) {
                KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
                targets=note_fault_change_locked(record,&batch,0,
                    (vma->permissions&KERNEL_MM_EXECUTE)!=0);
            }
        }
        if (cookie) kernel_mm_retirement_add(&batch, old, 0);
        /* Without Svvptc a new non-leaf needs full walk-cache invalidation;
         * replaced COW owners always remain until actual sync completes. */
        int nonleaf = missing != 0;
#if BOAROS_MAX_CPUS > 1
        if (operation->populate && operation->shared && !cookie && !nonleaf &&
            vma->kind == KERNEL_VMA_KIND_ANONYMOUS && !(vma->permissions & KERNEL_MM_EXECUTE)) {
            if (batch.count || !operation->guard.lock) __builtin_trap();
            if (!operation->sync_end || address < operation->sync_start) operation->sync_start = address;
            if (address + BOAROS_PAGE_SIZE > operation->sync_end) operation->sync_end = address + BOAROS_PAGE_SIZE;
            operation->sync_targets |= targets;
        } else
#endif
            finish_fault_change(record, &batch, targets, nonleaf ? 0 : begin,
                nonleaf ? UINT64_MAX : end-begin, cookie != 0, (vma->permissions & KERNEL_MM_EXECUTE) != 0);
    }
    if (shared_preparation != KERNEL_MM_NO_PAGE) {
        kernel_memory_object_finish_page(object,
            (vma->backing_offset + address - vma->start) >> BOAROS_PAGE_SHIFT,
            shared_preparation, status == KERNEL_MM_STATUS_OK);
        replacement = KERNEL_MM_NO_PAGE; /* finish已消费失败引用，成功已转给PTE。 */
    }
    if (replacement != KERNEL_MM_NO_PAGE) {
        physical_page_release(allocator, replacement);
    }
    if (object) kernel_memory_object_release(&object);
    if (source_owner && !__atomic_fetch_sub(&source_owner->faults, 1, __ATOMIC_RELEASE)) __builtin_trap();
    for (unsigned i = 0; i < 2; i++) if (prepared.tables[i] != KERNEL_MM_NO_PAGE) physical_page_release(allocator, prepared.tables[i]);
    if (cookie && status != KERNEL_MM_STATUS_OK) physical_page_release(allocator, old);
    return status;
}
static int userfault_fill_vma(struct kernel_userfault_ctx *ctx,
    uint64_t start, uint64_t end, struct kernel_vma *vma)
{
    if (!kernel_mm_pagemap_live(&ctx->target)) return -KERNEL_ESRCH;
    if (kernel_vma_set_lookup(ctx->target.record->vmas, start, vma) ||
        kernel_vma_userfault(vma) != ctx || end > vma->end) return -KERNEL_ENOENT;
    return 0;
}

int kernel_mm_userfault_check_fill(struct kernel_userfault_ctx *ctx, uint64_t start, uint64_t end,
    uint64_t *generation)
{
    struct kernel_mm_record *record = ctx->target.record;
    KERNEL_LOCK_SCOPE(gate);
    kernel_rwlock_read(&record->operations, &gate);
    KERNEL_LOCK_SCOPE(vmas);
    kernel_rwlock_read(&record->vma_lock, &vmas);
    struct kernel_vma vma;
    int error = userfault_fill_vma(ctx, start, end, &vma);
    if (!error) *generation = kernel_vma_set_generation(record->vmas);
    kernel_lock_release(&vmas);
    mm_gate_release(record, &gate);
    return error;
}

/* The caller supplies one reference for the new leaf. Holding the exclusive
 * gate through translation sync prevents remap and unregister from publishing
 * between validation and installation. A cached zero page needs no unlocked
 * source preparation, so its first fill can also validate the range here. */
int kernel_mm_userfault_fill(struct kernel_userfault_ctx *ctx, uint64_t address, uint64_t end,
    uint64_t *generation, uint64_t physical, int cow)
{
    struct kernel_mm_record *record = ctx->target.record;
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    struct kernel_mm_retirement batch;
    batch.count = batch.nonleaf = batch.compound = 0;
    struct physical_page_allocator *allocator = ctx->target.allocator;
    struct mm_gate_scope gate __attribute__((cleanup(mm_gate_scope_end))) = {.record = record};
    kernel_rwlock_write(&record->operations, &gate.guard);
    struct kernel_vma vma;
    int error = userfault_fill_vma(ctx, address, end, &vma);
    if (error) return error;
    /* UFFDIO_COPY acquires the source bytes without the target gate. A
     * replacement may have re-registered this address in the meantime. */
    uint64_t current_generation = kernel_vma_set_generation(record->vmas);
    if (*generation && *generation != current_generation) return -KERNEL_EAGAIN;
    *generation = current_generation;

    uint32_t targets;
    int executable = !!(vma.permissions & KERNEL_MM_EXECUTE);
    for (;;) {
        enum arch_mmu_status state;
        {
            KERNEL_RAW_WRITE_SCOPE(raw, &record->metadata);
            state = arch_mmu_user_map_prepared_page(&record->space, address,
                physical, backend_permissions_from_mm(vma.permissions), cow, &prepared);
            if (state == ARCH_MMU_STATUS_OK) {
                if (!(vma.flags & KERNEL_VMA_ANON_HISTORY) &&
                    !kernel_vma_set_mark_anonymous(record->vmas, address, current_generation, &vma))
                    __builtin_trap();
                targets = note_fault_change_locked(record, &batch, 0, executable);
                break;
            }
        }
        if (state != ARCH_MMU_STATUS_NEED_TABLES) {
            error = state == ARCH_MMU_STATUS_CONFLICT ? -KERNEL_EEXIST : -KERNEL_EFAULT;
            goto out;
        }
        /* There is no competing editor under the writer gate. Retry the
         * walk after preparing only the reported missing table pages. */
        for (unsigned i = 0; i < 2; i++) {
            if (!(prepared.table_mask & (1U << i))) continue;
            if (prepared.tables[i] != KERNEL_MM_NO_PAGE) __builtin_trap();
            if (physical_page_allocate_order_nosleep(allocator, 0, &prepared.tables[i])) {
                error = -KERNEL_ENOMEM;
                goto out;
            }
            void *table;
            if (physical_page_map_owned(allocator, prepared.tables[i], &table)) __builtin_trap();
            clear_page(table);
        }
    }
    unsigned missing = prepared.table_mask;
    finish_fault_change(record, &batch, targets, missing ? 0 : address,
        missing ? UINT64_MAX : BOAROS_PAGE_SIZE, 0, executable);
    error = 0;
out:
    mm_gate_release(record, &gate.guard);
    for (unsigned i = 0; i < 2; i++) {
        if (prepared.tables[i] != KERNEL_MM_NO_PAGE &&
            physical_page_release(allocator, prepared.tables[i])) __builtin_trap();
    }
    return error;
}

static void fault_retry(struct kernel_mm_record *record, uint64_t address)
{
#if BOAROS_COST_DIAGNOSTICS
    { KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata); record->fault_retries++; }
#else
    (void)record;
#endif
#if BOAROS_USER_SMP_COMPOSITION
    riscv_user_composition_fault_retry(address, 1);
#else
    (void)address;
#endif
}
static enum kernel_mm_status huge_fault(struct mm_operation *operation,uint64_t address,
    uint32_t access,const struct fault_vma *view,int force)
{
    struct kernel_mm_record *record=operation->record;
    const struct kernel_vma *v=&view->vma;
    if (operation->shared || !huge_vma(v)) __builtin_trap();
    if (!remote_access_allowed(v,access,force)) return KERNEL_MM_STATUS_ACCESS;
    uint64_t base=address & ~(ARCH_MMU_HUGE_SIZE-1);
    uint64_t index=(v->backing_offset+base-v->start)/ARCH_MMU_HUGE_SIZE;
    struct kernel_hugetlb_map *map=v->backing;
    struct kernel_mm_leaf old={0};
    int cow=0, borrowed=0;
    unsigned missing=0;
    struct kernel_mm_retirement batch={0};
    uint32_t targets=0;
    {
        KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
        enum arch_mmu_status found=arch_mmu_user_leaf_snapshot(&record->space,address,&old);
        if (!found) {
            if (old.order!=ARCH_MMU_HUGE_ORDER || old.virtual_address!=base) return KERNEL_MM_STATUS_ADDRESS_SPACE;
            if ((old.permissions & access) || (force && access==KERNEL_MM_READ)) {
                if (operation->fault_pin) {
                    enum kernel_mm_status pinned=pin_mapping(operation->mm,record,address,access,operation->fault_pin,0,force);
                    if (pinned) return pinned;
                }
                arch_mmu_flush_all();
                return KERNEL_MM_STATUS_OK;
            }
            if (v->kind!=KERNEL_VMA_KIND_HUGETLB_PRIVATE || access!=KERNEL_MM_WRITE)
                return KERNEL_MM_STATUS_ACCESS;
            if (force && !old.cow) {
                if (arch_mmu_user_mark_cow_page(&record->space,base) ||
                    arch_mmu_user_leaf_snapshot(&record->space,address,&old)) __builtin_trap();
            }
            if (!old.cow) return KERNEL_MM_STATUS_ACCESS;
            if (!operation->pins_closed) {
                kernel_raw_lock_release(&raw);
                if (current_pins(operation->mm)) return KERNEL_MM_STATUS_CONFLICT;
                close_record_pins(record,0);operation->pins_closed=1;
                return huge_fault(operation,address,access,view,force);
            }
            cow=1;
            uint32_t refs=physical_page_cow_borrow_order(record->space.allocator,old.physical_address,old.order);
            if (refs==1) {
                if (arch_mmu_user_cow_commit_order(&record->space,base,old.cookie,KERNEL_MM_NO_PAGE,
                    backend_permissions_from_mm(v->permissions),old.order,&batch)) __builtin_trap();
                if (operation->fault_pin && pin_mapping(operation->mm,record,address,access,operation->fault_pin,0,force)) __builtin_trap();
                targets=note_tlb_change_locked(record,&batch,0);
                kernel_raw_lock_release(&raw);
                finish_tlb_change(record,&batch,targets,base,ARCH_MMU_HUGE_SIZE,0);
                return KERNEL_MM_STATUS_OK;
            }
            borrowed=1;
        } else if (found==ARCH_MMU_STATUS_NOT_MAPPED) {
            if (arch_mmu_user_leaf_missing_order(&record->space,base,ARCH_MMU_HUGE_ORDER,&missing))
                return KERNEL_MM_STATUS_ADDRESS_SPACE;
        } else return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    struct kernel_hugetlb_claim claim={0};
    enum kernel_hugetlb_status hs=kernel_hugetlb_fault_begin(map,index,cow,&claim);
    if (hs==KERNEL_HUGETLB_REVOKE_CHILDREN) {
        kernel_hugetlb_map_acquire(map);
        if (operation->pins_closed) {
            { KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);pin_admission_open(record); }
            operation->pins_closed=0;wake_pin_waiters(record);
        }
        mm_operation_release(operation);
        hs=kernel_hugetlb_revoke_children(map,index,old.physical_address);
        enum kernel_mm_status retry=KERNEL_MM_STATUS_CONFLICT;
        if (hs==KERNEL_HUGETLB_OK && !current_pins(operation->mm) &&
            !mm_operation_begin(operation->mm,operation)) {
            struct fault_vma fresh;
            if (!read_fault_vma(operation,address,&fresh,0,0) && fresh.vma.backing==map &&
                fresh.vma.kind==KERNEL_VMA_KIND_HUGETLB_PRIVATE &&
                (fresh.vma.backing_offset+base-fresh.vma.start)/ARCH_MMU_HUGE_SIZE==index &&
                remote_access_allowed(&fresh.vma,access,force)) {
                close_record_pins(record,0);operation->pins_closed=1;
                physical_page_release_order(record->space.allocator,old.physical_address,old.order);
                borrowed=0;
                KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
                struct kernel_mm_leaf now;
                if (!arch_mmu_user_leaf_snapshot(&record->space,base,&now) &&
                    now.cookie==old.cookie && now.physical_address==old.physical_address && now.cow) {
                    uint32_t refs=physical_page_cow_borrow_order(record->space.allocator,now.physical_address,now.order);
                    if (refs==1) {
                        if (arch_mmu_user_cow_commit_order(&record->space,base,now.cookie,KERNEL_MM_NO_PAGE,
                            backend_permissions_from_mm(fresh.vma.permissions),now.order,&batch)) __builtin_trap();
                        if (operation->fault_pin && pin_mapping(operation->mm,record,address,access,operation->fault_pin,0,force)) __builtin_trap();
                        targets=note_tlb_change_locked(record,&batch,0);
                        kernel_raw_lock_release(&raw);
                        finish_tlb_change(record,&batch,targets,base,ARCH_MMU_HUGE_SIZE,0);
                        retry=KERNEL_MM_STATUS_OK;
                    } else physical_page_release_order(record->space.allocator,now.physical_address,now.order);
                }
            }
        }
        if (borrowed) physical_page_release_order(operation->mm->allocator,old.physical_address,old.order);
        kernel_hugetlb_map_release(&map);
        return hs==KERNEL_HUGETLB_OK || hs==KERNEL_HUGETLB_BUSY ? retry : huge_status(hs);
    }
    enum kernel_mm_status status=huge_status(hs);
    struct kernel_mm_leaf_preparation prepared=KERNEL_MM_LEAF_PREPARATION_INIT;
    if (!status && cow) {
        void *from,*to;
        if (physical_page_map_owned(record->space.allocator,old.physical_address,&from) ||
            physical_page_map_owned(record->space.allocator,claim.physical_address,&to)) __builtin_trap();
        memcpy(to,from,ARCH_MMU_HUGE_SIZE);
    }
    for (unsigned i=0;i<2 && !status;i++) if (missing&(1U<<i)) {
        if (physical_page_allocate_order_nosleep(record->space.allocator,0,&prepared.tables[i])) {
            status=KERNEL_MM_STATUS_NO_MEMORY;break;
        }
        void *table;
        if (physical_page_map_owned(record->space.allocator,prepared.tables[i],&table)) __builtin_trap();
        clear_page(table);
    }
    if (!status) {
        KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
        enum arch_mmu_status published=cow ?
            arch_mmu_user_cow_commit_order(&record->space,base,old.cookie,claim.physical_address,
                backend_permissions_from_mm(v->permissions),ARCH_MMU_HUGE_ORDER,&batch) :
            arch_mmu_user_map_prepared_order(&record->space,base,claim.physical_address,
                backend_permissions_from_mm(v->permissions),0,ARCH_MMU_HUGE_ORDER,&prepared);
        if (published) status=published==ARCH_MMU_STATUS_CONFLICT?KERNEL_MM_STATUS_CONFLICT:KERNEL_MM_STATUS_ADDRESS_SPACE;
        else {
            if (operation->fault_pin && pin_mapping(operation->mm,record,address,access,operation->fault_pin,0,force)) __builtin_trap();
            targets=note_fault_change_locked(record,&batch,cow,(v->permissions&KERNEL_MM_EXECUTE)!=0);
        }
    }
    if (!status) finish_fault_change(record,&batch,targets,base,ARCH_MMU_HUGE_SIZE,cow,(v->permissions&KERNEL_MM_EXECUTE)!=0);
    for (unsigned i=0;i<2;i++) if (prepared.tables[i]!=KERNEL_MM_NO_PAGE)
        physical_page_release(record->space.allocator,prepared.tables[i]);
    if (claim.map) kernel_hugetlb_fault_finish(&claim,!status);
    if (borrowed) physical_page_release_order(record->space.allocator,old.physical_address,old.order);
    return status;
}

/* The continuous read gate keeps the source live. Its executable lease
 * excludes file writes and resize while cache references become PTE owners. */
static __attribute__((noinline)) int try_elf_ready_fault(struct mm_operation *operation,
    uint64_t address, const struct fault_vma *view)
{
    struct kernel_mm_record *record = operation->record;
    const struct kernel_vma *vma = &view->vma;
    struct kernel_elf64_source *source = vma->backing;
    struct kernel_open_file_description *file = kernel_elf64_source_file(source);
    if (!file || !kernel_open_file_vfs_borrow(file)->exec_lease) return 0;

    address &= ~BOAROS_PAGE_MASK;
    uint64_t limit, first = file_fault_window(vma,address,&limit);
    unsigned demand = 1U << ((address-first) >> BOAROS_PAGE_SHIFT);
    unsigned selected, missing;
    {
        struct kernel_mm_leaf leaf;
        KERNEL_RAW_READ_SCOPE(raw, &record->metadata);
        if (arch_mmu_user_leaf_probe(&record->space,address,&leaf,&missing) !=
                ARCH_MMU_STATUS_NOT_MAPPED || missing ||
            arch_mmu_user_empty_window(&record->space,first,
                (limit-first) >> BOAROS_PAGE_SHIFT,&selected) || !(selected & demand))
            return 0;
    }

    uint64_t refs[KERNEL_PAGE_CACHE_READY_WINDOW];
    unsigned ready = kernel_elf64_source_acquire_ready(source,
        vma->backing_offset + (first-vma->start),selected,refs,KERNEL_PAGE_CACHE_READY_WINDOW);
    if (!ready) return 0;

    unsigned installed = 0;
    uint32_t targets = 0;
    int instructions = (vma->permissions & KERNEL_MM_EXECUTE) != 0;
    struct kernel_mm_retirement batch;
    batch.count = batch.nonleaf = batch.compound = 0;
    if (ready & demand) {
        KERNEL_LOCK_SCOPE(vma_guard);
        kernel_rwlock_read(&record->vma_lock,&vma_guard);
        KERNEL_RAW_WRITE_SCOPE(raw, &record->metadata);
        if (kernel_vma_set_generation(record->vmas) == view->generation &&
            !fault_range_claimed(record,first,limit)) {
            installed = arch_mmu_user_map_owned_window(&record->space,first,ready,refs,
                backend_permissions_from_mm(user_effective_permissions(vma->permissions) & ~KERNEL_MM_WRITE),1);
            if (installed) targets = note_fault_change_locked(record,&batch,0,instructions);
        }
    }
    if (installed != ready) {
        unsigned index = 0, unused = 0;
        for (unsigned bits = ready; bits; bits &= bits-1) {
            if (!(installed & (bits & -bits))) refs[unused++] = refs[index];
            ++index;
        }
        physical_page_release_batch(record->space.allocator,refs,unused);
    }
    if (installed)
        finish_fault_change(record,&batch,targets,installed == demand ? address : first,
            installed == demand ? BOAROS_PAGE_SIZE : limit-first,0,instructions);
    return !!(installed & demand);
}

static enum kernel_mm_status dispatch_fault(struct mm_operation *operation, uint64_t address,
    uint32_t access, struct fault_vma *view, int force)
{
    /* Stack growth and explicit population choose the effective access
     * after the initial lookup. Hardware faults already handled this. */
    if (fault_needs_anon_history(&view->vma, access, force)) {
        KERNEL_LOCK_SCOPE(vma_guard);
        if (operation->shared) kernel_rwlock_read(&operation->record->vma_lock, &vma_guard);
        enum kernel_mm_status status=mark_fault_history(operation,address,view);
        if (status) return status;
    }
    switch (view->vma.kind) {
    case KERNEL_VMA_KIND_HUGETLB_PRIVATE:
    case KERNEL_VMA_KIND_HUGETLB_SHARED:
        return huge_fault(operation,address,access,view,force);
    case KERNEL_VMA_KIND_OBJECT_SHARED:
        return mapped_object_fault(operation, address, access, &view->vma);
    case KERNEL_VMA_KIND_FILE_SHARED:
    case KERNEL_VMA_KIND_FILE_PRIVATE:
        return mapped_file_fault(operation, address, access, view, force);
    case KERNEL_VMA_KIND_ELF_PRIVATE:
        if (operation->shared && !force && !operation->populate && !operation->fault_pin &&
            access != KERNEL_MM_WRITE && (user_effective_permissions(view->vma.permissions) & access) &&
            !(view->vma.flags & KERNEL_VMA_RANDOM_READ) && try_elf_ready_fault(operation,address,view))
            return KERNEL_MM_STATUS_OK;
        return memory_fault(operation, address, access, view, force);
    default:
        return memory_fault(operation, address, access, view, force);
    }
}

static enum kernel_mm_status resolve_user_fault(struct kernel_mm *mm,
    uint64_t address, uint32_t access, int force, struct kernel_mm_user_pin *pin,
    enum kernel_userfault_origin origin)
{
    COST_SCOPE(fault_cost, MM_FAULT_TICKS);
    if (!mm || !access || (access & (access - 1)) || (access & ~KERNEL_VMA_ALL_PERMISSIONS))
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (address < BOAROS_PAGE_SIZE || address >= ARCH_MMU_USER_LIMIT)
        return KERNEL_MM_STATUS_NOT_MAPPED;
    int shared = BOAROS_MAX_CPUS > 1 && !force;
    for (;;) {
        struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {
            .shared = shared, .fault_pin = pin, .fault_origin = origin,
        };
        shared = BOAROS_MAX_CPUS > 1 && !force;
        enum kernel_mm_status status = mm_operation_begin(mm, &operation);
        if (status) return status;
        struct fault_vma view={0};
        status = read_fault_vma(&operation, address, &view, access, force);
        if (status == KERNEL_MM_STATUS_NOT_MAPPED && operation.record->vmas) {
            if (operation.shared) {
                shared = 0; /* Re-read the hole under the stack editor's writer qualification. */
                continue;
            }
            status = grow_fault_stack(operation.record, address, access, &view.vma, &view.generation);
        }
        if (!status && huge_vma(&view.vma) && operation.shared) { shared=0;continue; }
        if (!status) status = dispatch_fault(&operation, address, access, &view, force);
        if (status != KERNEL_MM_STATUS_CONFLICT || current_pins(mm) ||
            (huge_vma(&view.vma) && kernel_io_context_current()->user_pins)) return status;
        fault_retry(operation.record, address);
    }
}

unsigned kernel_mm_coredump_filter_get(const struct kernel_mm *mm)
{
    return mm && mm->record ? __atomic_load_n(&mm->record->coredump_filter,__ATOMIC_RELAXED) : 0;
}
void kernel_mm_coredump_filter_set(struct kernel_mm *mm,unsigned value)
{
    __atomic_store_n(&mm->record->coredump_filter,value & 0x1ffU,__ATOMIC_RELAXED);
}

#define MM_CORE_ACTIVE UINT32_C(0x80000000)
int kernel_mm_core_begin(struct kernel_mm *mm)
{
    struct kernel_mm_record *record;
    if (resolve_record(mm,&record) || mm->state!=KERNEL_MM_LIVE) return 0;
    if (pin_state_read(&record->write_pins)) return 0;
    uint32_t expected=0;
    return __atomic_compare_exchange_n(&record->core_remote,&expected,MM_CORE_ACTIVE,
        0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
void kernel_mm_core_end(struct kernel_mm *mm)
{
    if (__atomic_exchange_n(&mm->record->core_remote,0,__ATOMIC_RELEASE)!=MM_CORE_ACTIVE)
        __builtin_trap();
}
int kernel_mm_core_remote_begin(struct kernel_mm *mm)
{
    struct kernel_mm_record *record=mm->record;
    uint32_t old=__atomic_load_n(&record->core_remote,__ATOMIC_ACQUIRE);
    do {
        if (old&MM_CORE_ACTIVE) return 0;
        if (old==MM_CORE_ACTIVE-1) __builtin_trap();
    } while (!__atomic_compare_exchange_n(&record->core_remote,&old,old+1,
                 1,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    return 1;
}
void kernel_mm_core_remote_end(struct kernel_mm *mm)
{
    uint32_t old=__atomic_fetch_sub(&mm->record->core_remote,1,__ATOMIC_RELEASE);
    if (!old || (old&MM_CORE_ACTIVE)) __builtin_trap();
}

/* Both debugger and process_vm use the existing fault and actual-page borrow.
 * No source pin survives into a destination fault; executable bytes are
 * published only after the data stores and before returning to userland. */
int kernel_mm_remote_transfer(struct kernel_mm *mm, uint64_t address,
    void *buffer, size_t length, int writing, int force, size_t *copied)
{
    if (!mm || !copied || (length && !buffer)) return -KERNEL_EINVAL;
    *copied=0;
    uint32_t access=writing?KERNEL_MM_WRITE:KERNEL_MM_READ;
    while (*copied < length) {
        uint64_t current=address+*copied;
        if (current<address || current<BOAROS_PAGE_SIZE || current>=ARCH_MMU_USER_LIMIT)
            return -KERNEL_EFAULT;
        size_t chunk=BOAROS_PAGE_SIZE-(current & BOAROS_PAGE_MASK);
        if (chunk>length-*copied) chunk=length-*copied;
        struct kernel_mm_user_pin pin={0};
        struct mm_operation operation __attribute__((cleanup(mm_operation_end)))={0};
        enum kernel_mm_status status;
        for (;;) {
            if (force) {
                status=resolve_user_fault(mm,current,access,1,NULL,KERNEL_USERFAULT_INTERNAL);
                if (status) break;
                status=mm_operation_begin(mm,&operation);
                if (status) break;
                {
                    KERNEL_RAW_READ_SCOPE(raw,&operation.record->metadata);
                    if (pin_admission_closed(operation.record)) status=KERNEL_MM_STATUS_CONFLICT;
                    else status=pin_mapping(mm,operation.record,current,access,&pin,writing,1);
                }
            } else status=kernel_mm_pin_user_page(mm,current,access,&pin);
            if (!status) break;
            mm_operation_end(&operation);
            if (status!=KERNEL_MM_STATUS_ACCESS && status!=KERNEL_MM_STATUS_NOT_MAPPED &&
                status!=KERNEL_MM_STATUS_CONFLICT) break;
            status=resolve_user_fault(mm,current,access,force,NULL,KERNEL_USERFAULT_INTERNAL);
            if (status) break;
        }
        if (status) return status==KERNEL_MM_STATUS_NO_MEMORY ? -KERNEL_ENOMEM : -KERNEL_EFAULT;
        unsigned char *page=(unsigned char *)pin.page+(current & BOAROS_PAGE_MASK);
        if (writing) memcpy(page,(unsigned char *)buffer+*copied,chunk);
        else memcpy((unsigned char *)buffer+*copied,page,chunk);
        kernel_mm_unpin_user_page(&pin);
        *copied+=chunk;
        if (writing) {
            /* Flush even when the active mapping is temporarily non-executable:
             * another thread may restore execute permission before MM entry. */
            struct kernel_mm_record *record=mm->record;
            uint32_t targets;
            { KERNEL_RAW_WRITE_SCOPE(raw,&record->metadata);
              mm_generation_advance(&record->instruction_generation);
              targets=mm_sync_targets(record,0); }
            arch_mmu_sync_user(targets,current & ~BOAROS_PAGE_MASK,BOAROS_PAGE_SIZE,1);
        }
    }
    return 0;
}

enum kernel_mm_status kernel_mm_try_futex_read(struct kernel_mm *mm, uint64_t address,
    uint32_t *value)
{
    if (!mm || !value || (address & 3U)) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (mm->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    struct kernel_mm_record *record;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status) return status;
    KERNEL_RAW_READ_SCOPE(raw, &record->metadata);
    if (pin_admission_closed(record) == 1) return KERNEL_MM_STATUS_CONFLICT;
    struct kernel_mm_mapping mapping;
    status = lookup_user_mapping(record, address, KERNEL_MM_READ, &mapping);
    if (status == KERNEL_MM_STATUS_NOT_MAPPED || status == KERNEL_MM_STATUS_ACCESS)
        return KERNEL_MM_STATUS_CONFLICT;
    if (status) return status;
    void *page;
    uint64_t head=mapping.physical_address & ~(((uint64_t)BOAROS_PAGE_SIZE<<mapping.order)-1);
    if (physical_page_map_owned(mm->allocator, head,
        &page) != PHYSICAL_PAGE_STATUS_OK) return KERNEL_MM_STATUS_PAGE_ACCESS;
    /* 整次读在PTE稳定资格内完成；PTE的物理owner覆盖它，不借出指针或pin。
     * 调用者的桶操作继续串行比较值与登记，private key只依赖MM和VA。 */
    *value = __atomic_load_n((const uint32_t *)((const unsigned char *)page +
        (mapping.physical_address-head)), __ATOMIC_ACQUIRE);
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_try_futex_pin(struct kernel_mm *mm,uint64_t address,
    int shared,uint32_t access,const struct kernel_futex_key *key,struct kernel_mm_user_pin *pin)
{
    struct kernel_mm_record *record;
    enum kernel_mm_status status=resolve_record(mm,&record);
    if(status)return status;
    KERNEL_LOCK_SCOPE(gate);
    if(shared && !kernel_rwlock_try_read(&record->operations,&gate))return KERNEL_MM_STATUS_CONFLICT;
    if(shared) {
        struct kernel_vma vma;
        KERNEL_LOCK_SCOPE(vma_guard);
        if(!kernel_rwlock_try_read(&record->vma_lock,&vma_guard))return KERNEL_MM_STATUS_CONFLICT;
        if(!record->vmas || kernel_vma_set_lookup(record->vmas,address,&vma))return KERNEL_MM_STATUS_CONFLICT;
        if (vma.kind==KERNEL_VMA_KIND_OBJECT_SHARED) return KERNEL_MM_STATUS_ACCESS;
        struct kernel_futex_key actual={.identity=record->futex_id,.offset=address};
        if(vma.kind==KERNEL_VMA_KIND_FILE_SHARED) {
            actual.kind=KERNEL_FUTEX_KEY_SHARED_FILE;
            actual.identity=(uintptr_t)kernel_open_file_node(vma.backing);
        } else if(vma.kind==KERNEL_VMA_KIND_ANON_SHARED || vma.kind==KERNEL_VMA_KIND_SYSV_SHM) {
            actual.kind=KERNEL_FUTEX_KEY_SHARED_MEMORY;actual.identity=(uintptr_t)vma.backing;
        } else if(vma.kind==KERNEL_VMA_KIND_HUGETLB_SHARED) {
            actual.kind=KERNEL_FUTEX_KEY_SHARED_HUGETLB;actual.identity=(uintptr_t)vma.backing;
        }
        if(actual.kind!=KERNEL_FUTEX_KEY_PRIVATE)actual.offset=vma.backing_offset+address-vma.start;
        if(actual.kind!=key->kind || actual.identity!=key->identity || actual.offset!=key->offset)
            return KERNEL_MM_STATUS_CONFLICT;
        kernel_lock_release(&vma_guard);
    }
    KERNEL_RAW_READ_SCOPE(raw, &record->metadata);
    int closed = pin_admission_closed(record);
    if(closed==1 || (closed==2 && (access&KERNEL_MM_WRITE)))
        return KERNEL_MM_STATUS_CONFLICT;
    status=pin_mapping(mm,record,address,access,pin,access==KERNEL_MM_WRITE,0);
    return status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS ?
        KERNEL_MM_STATUS_CONFLICT:status;
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
            status=kernel_mm_pin_user_page(mm,addresses[pin_index],access,pin);
            if (status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS)
                status=kernel_mm_resolve_user_pin(mm,addresses[pin_index],access,pin);
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
            /* These directly mapped object pages have no inode/anonymous futex identity. */
            if (vmas[i].kind==KERNEL_VMA_KIND_OBJECT_SHARED) return KERNEL_MM_STATUS_ACCESS;
            if (count == 1 && access == KERNEL_MM_READ) {
                /* A single wait can check the mapping and borrow its page
                 * together. Multi-address operations still validate all keys first. */
                enum kernel_mm_status status;
                {
                    KERNEL_RAW_READ_SCOPE(guard, &record->metadata);
                    if (pin_admission_closed(record)) __builtin_trap();
                    status = pin_mapping(mm, record, address, access, pin, 0, 0);
                }
                if (status == KERNEL_MM_STATUS_NOT_MAPPED || status == KERNEL_MM_STATUS_ACCESS) {
                    fault_index = i;
                    break;
                }
                if (status) return status;
                continue;
            }
            struct kernel_mm_mapping mapping;
            enum kernel_mm_status status;
            { KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
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
        if (fault_index == count && access && !pin->registration) {
            enum kernel_mm_status status;
            { KERNEL_RAW_READ_SCOPE(guard, &record->metadata);
              if (pin_admission_closed(record)) __builtin_trap();
              status = pin_mapping(mm, record, addresses[pin_index], access,
                                   pin, access == KERNEL_MM_WRITE,0); }
            if (status == KERNEL_MM_STATUS_NOT_MAPPED || status == KERNEL_MM_STATUS_ACCESS) {
                fault_index = pin_index; fault_access = access;
            } else if (status) return status;
        }
        if (fault_index != count) {
            if (operation.guard.lock) mm_operation_release(&operation);
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
            } else if (vma->kind==KERNEL_VMA_KIND_HUGETLB_SHARED) {
                struct kernel_hugetlb_map *map=vma->backing;
                kernel_hugetlb_map_acquire(map);
                key->kind=KERNEL_FUTEX_KEY_SHARED_HUGETLB;
                key->hugetlb_map=map;key->identity=(uintptr_t)map;
            }
            if (key->kind != KERNEL_FUTEX_KEY_PRIVATE)
                key->offset = vma->backing_offset + addresses[i] - vma->start;
        }
        return KERNEL_MM_STATUS_OK;
    }
}

/* A resolution is charged only to the executing task's own MM. Population
 * uses the same actor and completion rule as a user trap, including retries. */
static struct kernel_task *fault_actor(struct kernel_mm *mm)
{
    struct kernel_task *task = kernel_task_current();
    const struct kernel_mm *task_mm = 0;
    if (!mm || !task || !kernel_task_has_user_context(task) ||
        kernel_task_mm_borrow(task, &task_mm) != KERNEL_TASK_STATUS_OK ||
        task_mm->record != mm->record) return 0;
    return task;
}
static enum kernel_mm_status resolve_fault_accounted(struct kernel_mm *mm,
    uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin, struct kernel_task *task,
    enum kernel_userfault_origin origin)
{
    uint64_t block_reads = task ? kernel_task_read_bytes(task) : 0U;
    if (!task) origin = KERNEL_USERFAULT_INTERNAL;
    enum kernel_mm_status status = resolve_user_fault(mm,address,access,0,pin,origin);
    if (status == KERNEL_MM_STATUS_OK && task)
        kernel_task_account_fault(task,
            kernel_task_read_bytes(task) != block_reads);
    return status;
}

enum kernel_mm_status kernel_mm_resolve_user_fault(struct kernel_mm *mm,uint64_t address,uint32_t access)
{ return resolve_fault_accounted(mm,address,access,NULL,fault_actor(mm),KERNEL_USERFAULT_COPY); }

enum kernel_mm_status kernel_mm_resolve_owned_user_fault(struct kernel_mm *mm,
    struct kernel_task *task,uint64_t address,uint32_t access)
{ return resolve_fault_accounted(mm,address,access,NULL,task,KERNEL_USERFAULT_TRAP); }

enum kernel_mm_status kernel_mm_resolve_user_pin(struct kernel_mm *mm,uint64_t address,
    uint32_t access,struct kernel_mm_user_pin *pin)
{
    if (!pin) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (pin->registration) __builtin_trap();
    for (;;) {
        enum kernel_mm_status status=resolve_fault_accounted(mm,address,access,pin,fault_actor(mm),KERNEL_USERFAULT_COPY);
        if (status || pin->registration) return status;
        status=kernel_mm_pin_user_page(mm,address,access,pin);
        if (status!=KERNEL_MM_STATUS_NOT_MAPPED && status!=KERNEL_MM_STATUS_ACCESS) return status;
    }
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
        if (!valid) return KERNEL_MM_STATUS_STATE;
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
                .permissions = vma.permissions, .flags = vma.flags, .kind = vma.kind, .role = vma.role,
                .shared = vma.kind == KERNEL_VMA_KIND_FILE_SHARED ||
                    vma.kind == KERNEL_VMA_KIND_HUGETLB_SHARED ||
                    vma.kind == KERNEL_VMA_KIND_ANON_SHARED ||
                    vma.kind == KERNEL_VMA_KIND_SYSV_SHM ||
                    vma.kind == KERNEL_VMA_KIND_OBJECT_SHARED };
            if (vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE ||
                vma.kind == KERNEL_VMA_KIND_FILE_SHARED || vma.kind == KERNEL_VMA_KIND_OBJECT_SHARED) {
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
static int lock_range_locked(struct kernel_mm_record *record, uint64_t start, uint64_t end, uint32_t flags)
{
    while (start < end) {
        struct kernel_vma vma;
        if (kernel_vma_set_lookup(record->vmas, start, &vma)) return -KERNEL_ENOMEM;
        uint64_t finish = vma.end < end ? vma.end : end;
        if (vma.role != KERNEL_VMA_ROLE_VDSO && vma.role != KERNEL_VMA_ROLE_VVAR && (vma.flags & KERNEL_VMA_LOCK_MASK) != flags) {
            struct kernel_vma_edit edit;
            if (kernel_vma_set_prepare_flags(record->vmas, start, finish, KERNEL_VMA_LOCK_MASK, flags, &edit))
                return -KERNEL_ENOMEM;
            if (kernel_vma_set_commit_edit(record->vmas, &edit)) __builtin_trap();
        }
        start = finish;
    }
    return 0;
}

static int lock_range(struct kernel_mm_record *record, uint64_t start, uint64_t end, uint32_t flags)
{
#if BOAROS_MAX_CPUS > 1
    struct kernel_lock_guard guard = {0};
    kernel_rwlock_write(&record->vma_lock, &guard);
#endif
    int result = lock_range_locked(record, start, end, flags);
#if BOAROS_MAX_CPUS > 1
    kernel_lock_release(&guard);
#endif
    return result;
}

static int populate_range(struct kernel_mm *mm, uint64_t start, uint64_t end,
    uint32_t requested, int locked_only)
{
    struct kernel_task *task = fault_actor(mm);
    uint64_t block_reads = task ? kernel_task_read_bytes(task) : 0U;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
#if BOAROS_MAX_CPUS > 1
    operation.populate = 1;
#endif
    unsigned pages = 0;
    while (start < end) {
        if (task && kernel_signal_termination_requested(task)) return -KERNEL_EINTR;
        if (!operation.guard.lock) {
            operation.shared = BOAROS_MAX_CPUS > 1;
            if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
        }
        struct fault_vma view;
        enum kernel_mm_status status = read_fault_vma(&operation, start, &view, 0, 0);
        if (status) return -KERNEL_ENOMEM;
        if (huge_vma(&view.vma) && operation.shared) {
            mm_operation_end(&operation);operation.shared=0;
            if (mm_operation_begin(mm,&operation) || read_fault_vma(&operation,start,&view,0,0)) return -KERNEL_EAGAIN;
        }
        const struct kernel_vma *vma = &view.vma;
        if (locked_only && (vma->flags & KERNEL_VMA_LOCK_MASK) != KERNEL_VMA_LOCKED) {
            start = vma->end < end ? vma->end : end;
            mm_operation_end(&operation); pages = 0;
            block_reads = task ? kernel_task_read_bytes(task) : 0U;
            continue;
        }
        if (requested && (!(vma->permissions & requested) || vma->role==KERNEL_VMA_ROLE_VVAR))
            return -KERNEL_EINVAL;
        if (!(vma->permissions & (KERNEL_MM_READ | KERNEL_MM_WRITE | KERNEL_MM_EXECUTE))) {
            if (locked_only) return -KERNEL_ENOMEM;
            start = vma->end < end ? vma->end : end;
            mm_operation_end(&operation); pages=0;continue;
        }
        uint32_t access = requested ? requested : (vma->permissions & KERNEL_MM_WRITE) &&
            (vma->kind == KERNEL_VMA_KIND_ANONYMOUS || vma->kind == KERNEL_VMA_KIND_FILE_PRIVATE ||
             vma->kind == KERNEL_VMA_KIND_ELF_PRIVATE) ? KERNEL_MM_WRITE :
            (user_effective_permissions(vma->permissions) & KERNEL_MM_READ) ? KERNEL_MM_READ : KERNEL_MM_EXECUTE;
        if ((access & (access - 1)) || (access & ~KERNEL_VMA_ALL_PERMISSIONS))
            return -KERNEL_EFAULT;
        status = dispatch_fault(&operation, start, access, &view, 0);
        if (status == KERNEL_MM_STATUS_CONFLICT && !current_pins(mm)) {
            mm_operation_end(&operation); pages = 0;
            fault_retry(operation.record, start);
            continue;
        }
        if (status) {
            if (locked_only) return status == KERNEL_MM_STATUS_NO_MEMORY ? -KERNEL_EAGAIN : -KERNEL_ENOMEM;
            return status == KERNEL_MM_STATUS_NO_MEMORY ? -KERNEL_ENOMEM :
                status == KERNEL_MM_STATUS_ACCESS ? -KERNEL_EINVAL : -KERNEL_EFAULT;
        }
        if (task) kernel_task_account_fault(task, kernel_task_read_bytes(task) != block_reads);
        start=huge_vma(vma) ? (start & ~(ARCH_MMU_HUGE_SIZE-1))+ARCH_MMU_HUGE_SIZE : start+BOAROS_PAGE_SIZE;
        /* 有界借用允许已排队的VMA writer推进，不逐页排入FIFO。 */
        if (++pages == KERNEL_MM_RETIRE_CAPACITY || start >= vma->end) {
            mm_operation_end(&operation); pages = 0;
        }
        block_reads = task ? kernel_task_read_bytes(task) : 0U;
    }
    return 0;
}

static int populate_locked(struct kernel_mm *mm, uint64_t start, uint64_t end)
{ return populate_range(mm,start,end,0,1); }
int kernel_mm_populate(struct kernel_mm *mm, uint64_t start, uint64_t end, uint32_t access)
{ return populate_range(mm,start,end,access,0); }

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

/* WILLNEED is cache advice, not a demand fault: it never installs a PTE. The
 * existing source borrow covers unlocked file I/O and concurrent last unmap. */
static int advise_file(struct mm_operation *operation, const struct kernel_vma *vma,
    uint64_t address, uint64_t *end, unsigned *budget)
{
    struct kernel_open_file_description *file = 0;
    _Atomic uint32_t *borrow = 0;
    uint64_t offset = vma->backing_offset + address - vma->start;
    if (vma->kind == KERNEL_VMA_KIND_ANONYMOUS) return -KERNEL_EBADF;
    if (vma->kind == KERNEL_VMA_KIND_ANON_SHARED || vma->kind == KERNEL_VMA_KIND_SYSV_SHM ||
        vma->kind == KERNEL_VMA_KIND_OBJECT_SHARED)
        return 0; /* shmem has no swap-in work in this kernel. */
    if (vma->kind == KERNEL_VMA_KIND_FILE_PRIVATE || vma->kind == KERNEL_VMA_KIND_FILE_SHARED) {
        file = vma->backing;
        struct kernel_mm_file_source *source = vma->backing_owner;
        if (!source) __builtin_trap();
        borrow = &source->operations;
    } else if (vma->kind == KERNEL_VMA_KIND_ELF_PRIVATE) {
        struct kernel_elf64_source *source = vma->backing;
        file = kernel_elf64_source_file(source);
        if (!file) return -KERNEL_EBADF;
        for (uint32_t i = 0; i < kernel_elf64_source_run_count(source); i++) {
            const struct kernel_elf64_source_run *run = kernel_elf64_source_run_at(source, i);
            if (offset < run->start || offset >= run->end) continue;
            uint64_t limit = address + run->end - offset;
            if (*end > limit) *end = limit;
            if (run->kind == KERNEL_ELF64_SOURCE_RUN_ZERO) return -KERNEL_EBADF;
            if (run->kind != KERNEL_ELF64_SOURCE_RUN_FILE) return 0;
            offset = run->file_offset + offset - run->start;
            break;
        }
        struct kernel_mm_elf_source *owner = vma->backing_owner;
        if (!owner) __builtin_trap();
        borrow = &owner->faults;
    } else return -KERNEL_EINVAL;
    if (!*budget || kernel_open_file_memory_backed(file)) return 0;
    if (*borrow == UINT32_MAX || ++*borrow == 0) __builtin_trap();
    const struct kernel_mm *mm = operation->mm;
    uint64_t length = *end - address;
    mm_operation_end(operation);
    for (uint64_t done = 0; done < length && *budget; done += BOAROS_PAGE_SIZE) {
        struct kernel_open_file_page_preparation prepared = {0};
        (*budget)--;
        if (kernel_open_file_prepare_mapping_page(file, (offset + done) >> BOAROS_PAGE_SHIFT, 0, &prepared))
            break;
        kernel_open_file_finish_mapping_page(file, &prepared, 0);
    }
    if (!__atomic_fetch_sub(borrow, 1, __ATOMIC_RELEASE)) __builtin_trap();
    if (mm_operation_begin(mm, operation)) return -KERNEL_EAGAIN;
    /* Linux accepts the hint even if fadvise/page-in cannot complete it. The
     * remaining range is still validated, without a residency guarantee. */
    return 0;
}

static int advise_fork(struct kernel_mm_record *record, const struct kernel_vma *vma,
    uint64_t address, uint64_t *end, int32_t advice)
{
    if (vma->role == KERNEL_VMA_ROLE_VVAR ||
        (advice == KERNEL_MM_ADVICE_DOFORK && vma->role == KERNEL_VMA_ROLE_VDSO))
        return -KERNEL_EINVAL;
    if (advice == KERNEL_MM_ADVICE_WIPEONFORK) {
        if (vma->role == KERNEL_VMA_ROLE_VDSO) return -KERNEL_EINVAL;
        if (vma->kind == KERNEL_VMA_KIND_ELF_PRIVATE) {
            /* ELF BSS has the same anonymous zero source as Linux. Keep the
             * source recipe, but fork must not borrow the parent's pages. */
            struct kernel_elf64_source *source = vma->backing;
            uint64_t offset = vma->backing_offset + address - vma->start;
            const struct kernel_elf64_source_run *run = 0;
            for (uint32_t i = 0; i < kernel_elf64_source_run_count(source); i++) {
                const struct kernel_elf64_source_run *candidate = kernel_elf64_source_run_at(source, i);
                if (candidate->start <= offset && offset < candidate->end) { run = candidate; break; }
            }
            if (!run) __builtin_trap();
            if (run->kind != KERNEL_ELF64_SOURCE_RUN_ZERO) return -KERNEL_EINVAL;
            uint64_t finish = address + run->end - offset;
            if (*end > finish) *end = finish;
        } else if (vma->kind != KERNEL_VMA_KIND_ANONYMOUS) return -KERNEL_EINVAL;
    }
    uint32_t mask = advice == KERNEL_MM_ADVICE_DONTFORK || advice == KERNEL_MM_ADVICE_DOFORK ?
        KERNEL_VMA_DONTFORK : KERNEL_VMA_WIPEONFORK;
    uint32_t value = advice == KERNEL_MM_ADVICE_DONTFORK || advice == KERNEL_MM_ADVICE_WIPEONFORK ? mask : 0;
    if ((vma->flags & mask) == value) return 0;
    struct kernel_vma_edit edit;
    enum kernel_vma_status status = kernel_vma_set_prepare_flags(record->vmas,address,*end,mask,value,&edit);
    if (status == KERNEL_VMA_STATUS_NO_MEMORY) return -KERNEL_EAGAIN;
    if (status || kernel_vma_set_commit_edit(record->vmas,&edit)) __builtin_trap();
    return 0;
}

static int advise_remove(struct mm_operation *operation,const struct kernel_vma *vma,
    uint64_t address,uint64_t end)
{
    if (vma->flags & KERNEL_VMA_AIO_RING) return -KERNEL_EOPNOTSUPP;
    if (vma->flags & KERNEL_VMA_LOCKED) return -KERNEL_EINVAL;
    if (vma->kind==KERNEL_VMA_KIND_ELF_PRIVATE) {
        struct kernel_elf64_source *source=vma->backing;
        if (!kernel_elf64_source_file(source)) return -KERNEL_EINVAL;
        uint64_t offset=vma->backing_offset+address-vma->start;
        for (uint32_t i=0;i<kernel_elf64_source_run_count(source);i++) {
            const struct kernel_elf64_source_run *run=kernel_elf64_source_run_at(source,i);
            if (run->start<=offset && offset<run->end)
                return run->kind==KERNEL_ELF64_SOURCE_RUN_ZERO ? -KERNEL_EINVAL : -KERNEL_EACCES;
        }
        __builtin_trap();
    }
    if (vma->kind==KERNEL_VMA_KIND_ANON_SHARED || vma->kind==KERNEL_VMA_KIND_SYSV_SHM) {
        if (!(vma->maximum_permissions & KERNEL_MM_WRITE)) return -KERNEL_EACCES;
        struct kernel_memory_object *object=vma->backing;
        if (kernel_memory_object_acquire(object)) __builtin_trap();
        uint64_t offset=vma->backing_offset+address-vma->start;
        const struct kernel_mm *mm=operation->mm;
        mm_operation_end(operation);
        kernel_memory_object_remove(object,offset,offset+end-address);
        kernel_memory_object_release(&object);
        return mm_operation_begin(mm,operation) ? -KERNEL_EAGAIN : 0;
    }
    if (vma->kind != KERNEL_VMA_KIND_FILE_SHARED && vma->kind != KERNEL_VMA_KIND_FILE_PRIVATE)
        return -KERNEL_EINVAL;
    if (vma->kind != KERNEL_VMA_KIND_FILE_SHARED || !(vma->maximum_permissions & KERNEL_MM_WRITE))
        return -KERNEL_EACCES;
    struct kernel_open_file_description *file=vma->backing;
    struct kernel_mm_file_source *source=vma->backing_owner;
    if (!source || source->operations==UINT32_MAX) __builtin_trap();
    source->operations++;
    uint64_t offset=vma->backing_offset+address-vma->start;
    const struct kernel_mm *mm=operation->mm;
    /* 打洞会回调所有映射的MM；真实来源借用覆盖最后unmap/close，不能
     * 带MM资格进入inode写路径，也不重新按地址查找可能已替换的文件。 */
    mm_operation_end(operation);
    int error=kernel_open_file_fallocate(file,KERNEL_VFS_FALLOC_PUNCH_HOLE|KERNEL_VFS_FALLOC_KEEP_SIZE,
        (int64_t)offset,(int64_t)(end-address),NULL);
    if (!__atomic_fetch_sub(&source->operations,1,__ATOMIC_RELEASE)) __builtin_trap();
    if (mm_operation_begin(mm,operation)) return error ? error : -KERNEL_EAGAIN;
    return error;
}

static int advise_free(struct mm_operation *operation,const struct kernel_vma *vma,
    uint64_t address,uint64_t *end)
{
    struct kernel_mm_record *record=operation->record;
    if ((vma->flags & KERNEL_VMA_LOCKED) || vma->role==KERNEL_VMA_ROLE_VDSO ||
        vma->role==KERNEL_VMA_ROLE_VVAR)
        return -KERNEL_EINVAL;
    if (vma->kind==KERNEL_VMA_KIND_ELF_PRIVATE) {
        struct kernel_elf64_source *source=vma->backing;
        uint64_t offset=vma->backing_offset+address-vma->start;
        const struct kernel_elf64_source_run *run=NULL;
        for (uint32_t i=0;i<kernel_elf64_source_run_count(source);i++) {
            const struct kernel_elf64_source_run *candidate=kernel_elf64_source_run_at(source,i);
            if (candidate->start<=offset && offset<candidate->end) { run=candidate;break; }
        }
        if (!run) __builtin_trap();
        if (run->kind!=KERNEL_ELF64_SOURCE_RUN_ZERO) return -KERNEL_EINVAL;
        uint64_t finish=address+run->end-offset;
        if (*end>finish) *end=finish;
    } else if (vma->kind!=KERNEL_VMA_KIND_ANONYMOUS) return -KERNEL_EINVAL;
    if (!pin_admission_closed(record)) close_pins(operation,1);
    uint64_t cursor=address,leaf;
    enum arch_mmu_status status;
    int error=0,changed=0;
    while ((status=arch_mmu_user_next_owned(&record->space,&cursor,*end,&leaf))==ARCH_MMU_STATUS_OK) {
        struct mm_lazy_page *page=NULL;
        uint64_t physical;int cow;
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            if (lazy_find(record,leaf)) continue;
            if (arch_mmu_user_owned_snapshot(&record->space,leaf,&physical,&cow)) __builtin_trap();
        }
        enum kernel_heap_status hs=lazy_reserve(record,record->lazy_count+1);
        if (hs) { error=hs==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EINVAL;break; }
        hs=kernel_heap_allocate_zeroed(record->vma_heap,1,sizeof(*page),(void **)&page);
        if (hs) { error=hs==KERNEL_HEAP_STATUS_EMPTY?-KERNEL_ENOMEM:-KERNEL_EINVAL;break; }
        {
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            /* The exclusive MM operation keeps the page and VMA stable. */
            if (arch_mmu_user_mark_cow_page(&record->space,leaf)) __builtin_trap();
            page->address=leaf;page->physical=physical;
            lazy_insert(record,page);
            changed=1;
        }
    }
    if (status!=ARCH_MMU_STATUS_NOT_MAPPED && !error) __builtin_trap();
    if (changed) {
        struct kernel_mm_retirement batch={0};uint32_t targets;
        { KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata); targets=note_tlb_change_locked(record,&batch,0); }
        finish_tlb_change(record,&batch,targets,address,*end-address,0);
        lazy_register(record);
    }
    /* A bucket table can exist before the first candidate is allocated. */
    if (error && !record->lazy_count) lazy_drain(record);
    return error;
}

/* Reserve replacement capacity before detaching; never turn failed advice into
 * data loss. The token consumes precisely the old PTE reference after sync. */
static int advise_huge_discard(struct mm_operation *op,const struct kernel_vma *v,
    uint64_t start,uint64_t end)
{
    struct kernel_mm_record *r=op->record;
    if ((start|end)&(ARCH_MMU_HUGE_SIZE-1)) return -KERNEL_EINVAL;
    if (current_pins(op->mm)) return -KERNEL_EAGAIN;
    if (!op->pins_closed) { close_record_pins(r,0);op->pins_closed=1; }
    if (v->kind==KERNEL_VMA_KIND_HUGETLB_SHARED)
        return unmap_space_range(r,start,end)?-KERNEL_EAGAIN:0;
    for (uint64_t va=start;va<end;va+=ARCH_MMU_HUGE_SIZE) {
        struct kernel_mm_leaf leaf;
        { KERNEL_RAW_READ_SCOPE(raw,&r->metadata);
          enum arch_mmu_status st=arch_mmu_user_leaf_snapshot(&r->space,va,&leaf);
          if (st==ARCH_MMU_STATUS_NOT_MAPPED) continue;
          if (st || leaf.order!=ARCH_MMU_HUGE_ORDER) __builtin_trap(); }
        struct kernel_hugetlb_discard_claim claim={0};
        enum kernel_hugetlb_status hs=kernel_hugetlb_discard_begin(v->backing,
            (v->backing_offset+va-v->start)/ARCH_MMU_HUGE_SIZE,leaf.physical_address,1,&claim);
        if (hs) return hs==KERNEL_HUGETLB_BUSY?-KERNEL_EAGAIN:-KERNEL_ENOMEM;
        struct kernel_mm_detach_cursor cursor={0};
        enum arch_mmu_status st;
        unsigned detached=0;
        do {
            struct kernel_mm_retirement batch={0};uint32_t targets;
            { KERNEL_RAW_WRITE_SCOPE(raw,&r->metadata);
              st=arch_mmu_user_detach_range(&r->space,va,va+ARCH_MMU_HUGE_SIZE,&cursor,&batch,0,0);
              if (st!=ARCH_MMU_STATUS_OK && st!=ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
              for (unsigned i=0;i<batch.count;i++) if (batch.orders[i]) {
                  if (detached++ || batch.pages[i]!=leaf.physical_address || batch.orders[i]!=leaf.order) __builtin_trap();
                  --batch.count;batch.pages[i]=batch.pages[batch.count];batch.orders[i]=batch.orders[batch.count];--i;
              }
              targets=note_tlb_change_locked(r,&batch,0); }
            finish_tlb_change(r,&batch,targets,batch.nonleaf?0:va,batch.nonleaf?UINT64_MAX:ARCH_MMU_HUGE_SIZE,0);
        } while (st==ARCH_MMU_STATUS_OK);
        if (!detached) __builtin_trap();
        kernel_hugetlb_discard_finish(&claim,1);
    }
    return 0;
}

int kernel_mm_madvise(struct kernel_mm *mm, uint64_t address, uint64_t length, int32_t advice)
{
    int fork_policy = advice == KERNEL_MM_ADVICE_DONTFORK || advice == KERNEL_MM_ADVICE_DOFORK ||
        advice == KERNEL_MM_ADVICE_WIPEONFORK || advice == KERNEL_MM_ADVICE_KEEPONFORK;
    int dump_policy=advice==KERNEL_MM_ADVICE_DONTDUMP || advice==KERNEL_MM_ADVICE_DODUMP;
    int populate=advice==KERNEL_MM_ADVICE_POPULATE_READ || advice==KERNEL_MM_ADVICE_POPULATE_WRITE;
    if ((!fork_policy && !dump_policy && !populate && advice != KERNEL_MM_ADVICE_REMOVE && advice != KERNEL_MM_ADVICE_FREE &&
        (advice < KERNEL_MM_ADVICE_NORMAL || advice > KERNEL_MM_ADVICE_DONTNEED)) ||
        (address & BOAROS_PAGE_MASK) || length > UINT64_MAX - BOAROS_PAGE_MASK) return -KERNEL_EINVAL;
    uint64_t rounded = (length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    if (rounded > UINT64_MAX - address) return -KERNEL_EINVAL;
    if (!length) return 0;
    uint64_t end = address + rounded;
    if (populate) return kernel_mm_populate(mm,address,end,
        advice==KERNEL_MM_ADVICE_POPULATE_WRITE ? KERNEL_MM_WRITE : KERNEL_MM_READ);
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
    struct kernel_mm_record *record = operation.record;
    int hole = 0;
    unsigned budget = 16;
    while (address < end) {
        struct kernel_vma vma;
        enum kernel_vma_status status = kernel_vma_set_lookup(record->vmas, address, &vma);
        if (status == KERNEL_VMA_STATUS_NOT_FOUND) {
            hole = 1;
            status = kernel_vma_set_next(record->vmas, address, &vma);
            if (status == KERNEL_VMA_STATUS_NOT_FOUND || vma.start >= end) break;
            address = vma.start;
        }
        if (status) __builtin_trap();
        uint64_t finish = vma.end < end ? vma.end : end;
        int error = 0;
        if (huge_vma(&vma) && (address&(ARCH_MMU_HUGE_SIZE-1))) return -KERNEL_EINVAL;
        if (huge_vma(&vma) && advice!=KERNEL_MM_ADVICE_DONTNEED &&
            (finish&(ARCH_MMU_HUGE_SIZE-1))) return -KERNEL_EINVAL;
        if (advice == KERNEL_MM_ADVICE_DONTNEED) {
            if (vma.flags & KERNEL_VMA_LOCKED) return -KERNEL_EINVAL;
            if (huge_vma(&vma)) error=advise_huge_discard(&operation,&vma,address,finish & ~(ARCH_MMU_HUGE_SIZE-1));
            else if (unmap_space_range(record, address, finish)) __builtin_trap();
        } else if (advice == KERNEL_MM_ADVICE_WILLNEED)
            error = advise_file(&operation, &vma, address, &finish, &budget);
        else if (advice == KERNEL_MM_ADVICE_REMOVE)
            error = advise_remove(&operation, &vma, address, finish);
        else if (advice == KERNEL_MM_ADVICE_FREE)
            error = advise_free(&operation,&vma,address,&finish);
        else if (advice == KERNEL_MM_ADVICE_NORMAL || advice == KERNEL_MM_ADVICE_RANDOM ||
                 advice == KERNEL_MM_ADVICE_SEQUENTIAL) {
            uint32_t value=advice==KERNEL_MM_ADVICE_RANDOM?KERNEL_VMA_RANDOM_READ:0;
            if ((vma.flags&KERNEL_VMA_RANDOM_READ)!=value) {
                struct kernel_vma_edit edit;
                enum kernel_vma_status changed=kernel_vma_set_prepare_flags(record->vmas,address,finish,
                    KERNEL_VMA_RANDOM_READ,value,&edit);
                if (changed==KERNEL_VMA_STATUS_NO_MEMORY) error=-KERNEL_EAGAIN;
                else if (changed || kernel_vma_set_commit_edit(record->vmas,&edit)) __builtin_trap();
            }
        }
        else if (fork_policy) error = advise_fork(record, &vma, address, &finish, advice);
        else if (dump_policy) {
            if (advice==KERNEL_MM_ADVICE_DODUMP && (vma.role==KERNEL_VMA_ROLE_VVAR ||
                vma.role==KERNEL_VMA_ROLE_VDSO || vma.kind==KERNEL_VMA_KIND_OBJECT_SHARED))
                return -KERNEL_EINVAL;
            struct kernel_vma_edit edit;
            enum kernel_vma_status changed=kernel_vma_set_prepare_flags(record->vmas,address,finish,
                KERNEL_VMA_DONTDUMP,advice==KERNEL_MM_ADVICE_DONTDUMP?KERNEL_VMA_DONTDUMP:0,&edit);
            if (changed==KERNEL_VMA_STATUS_NO_MEMORY) error=-KERNEL_EAGAIN;
            else if (changed || kernel_vma_set_commit_edit(record->vmas,&edit)) __builtin_trap();
        }
        if (error) return error;
        address = finish;
    }
    return hole ? -KERNEL_ENOMEM : 0;
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
#if BOAROS_MAX_CPUS > 1
        operation.shared = 1;
#endif
        if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
        if (mm_operation_claim_range(&operation, start, end)) return -KERNEL_EAGAIN;
        struct kernel_mm_record *record = operation.record;
        if (!record->vmas || record->stage != KERNEL_MM_RECORD_LIVE || !record->brk_initialized)
            return -KERNEL_EINVAL;
#if BOAROS_MAX_CPUS > 1
        struct kernel_lock_guard vma_guard = {0};
        kernel_rwlock_write(&record->vma_lock, &vma_guard);
#endif
        uint64_t used = kernel_vma_set_locked_bytes(record->vmas);
        uint64_t old = kernel_vma_set_locked_range(record->vmas, start, end);
        if (used > limit || end - start - old > limit - used)
            error = -KERNEL_ENOMEM;
        else
            error = lock_range_locked(record, start, end,
                KERNEL_VMA_LOCKED | (flags ? KERNEL_VMA_LOCK_ONFAULT : 0));
#if BOAROS_MAX_CPUS > 1
        kernel_lock_release(&vma_guard);
#endif
    }
    return error || flags ? error : populate_locked(mm, start, end);
}

int kernel_mm_munlock(struct kernel_mm *mm, uint64_t address, uint64_t length)
{
    uint64_t start, end;
    int error = lock_interval(address, length, &start, &end);
    if (error) return error;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
#if BOAROS_MAX_CPUS > 1
    operation.shared = 1;
#endif
    if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
    if (mm_operation_claim_range(&operation, start, end)) return -KERNEL_EAGAIN;
    if (!operation.record->vmas || operation.record->stage != KERNEL_MM_RECORD_LIVE ||
        !operation.record->brk_initialized) return -KERNEL_EINVAL;
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

int kernel_mm_mincore(struct kernel_mm *mm, uint64_t address, unsigned count,
    unsigned char *output, unsigned *completed)
{
    *completed = 0;
    if (!count || count > 16 || (address & BOAROS_PAGE_MASK)) return -KERNEL_EINVAL;
    struct mm_operation operation __attribute__((cleanup(mm_operation_end))) = {0};
    if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
    struct kernel_mm_record *record = operation.record;
    for (unsigned i = 0; i < count;) {
        struct kernel_vma vma;
        if (kernel_vma_set_lookup(record->vmas, address, &vma)) return -KERNEL_ENOMEM;
        uint64_t generation = kernel_vma_set_generation(record->vmas);
        uint64_t offset = vma.backing_offset + address - vma.start;
        struct kernel_open_file_description *file = 0;
        struct kernel_memory_object *object = 0;
        _Atomic uint32_t *borrow = 0;
        if (vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE || vma.kind == KERNEL_VMA_KIND_FILE_SHARED) {
            file = vma.backing;
            struct kernel_mm_file_source *source = vma.backing_owner;
            if (!source) __builtin_trap();
            borrow = &source->operations;
        } else if (vma.kind == KERNEL_VMA_KIND_ELF_PRIVATE) {
            struct kernel_elf64_source *source = vma.backing;
            file = kernel_elf64_source_file(source);
            for (uint32_t j = 0; j < kernel_elf64_source_run_count(source); j++) {
                const struct kernel_elf64_source_run *run = kernel_elf64_source_run_at(source, j);
                if (offset >= run->start && offset < run->end) {
                    if (run->kind == KERNEL_ELF64_SOURCE_RUN_FILE) offset = run->file_offset + offset - run->start;
                    else file = 0;
                    break;
                }
            }
            if (file) {
                struct kernel_mm_elf_source *owner = vma.backing_owner;
                if (!owner) __builtin_trap();
                borrow = &owner->faults;
            }
        } else if (vma.kind == KERNEL_VMA_KIND_ANON_SHARED || vma.kind == KERNEL_VMA_KIND_SYSV_SHM)
            object = vma.backing;
        int present;
        {
            uint64_t cursor = address, found;
            KERNEL_RAW_WRITE_SCOPE(guard, &record->metadata);
            enum arch_mmu_status status = arch_mmu_user_next_owned(&record->space, &cursor,
                address + BOAROS_PAGE_SIZE, &found);
            if (status != ARCH_MMU_STATUS_OK && status != ARCH_MMU_STATUS_NOT_MAPPED) __builtin_trap();
            present = status == ARCH_MMU_STATUS_OK;
        }
        if (!present && vma.kind==KERNEL_VMA_KIND_HUGETLB_SHARED)
            present=kernel_hugetlb_map_resident(vma.backing,
                (vma.backing_offset+address-vma.start)/ARCH_MMU_HUGE_SIZE);
        if (file || object) {
            if (borrow && (*borrow == UINT32_MAX || ++*borrow == 0)) __builtin_trap();
            if (object && kernel_memory_object_acquire(object)) __builtin_trap();
            /* inode mutation can invalidate this MM: retain the actual source,
             * end the MM gate before taking inode/cache/object qualifications. */
            mm_operation_end(&operation);
            if (file && !kernel_vfs_file_mincore_allowed(kernel_open_file_vfs_borrow(file))) present = 1;
            else if (!present) {
                struct kernel_memory_object *backing = object ? object : kernel_vfs_file_memory(kernel_open_file_vfs_borrow(file));
                if (backing) {
                    uint64_t physical;
                    enum kernel_memory_object_status status = kernel_memory_object_find_page(backing, offset >> BOAROS_PAGE_SHIFT, &physical);
                    if (!status) {
                        present = 1;
                        if (physical_page_release(mm->allocator, physical)) __builtin_trap();
                    } else if (status != KERNEL_MEMORY_OBJECT_NOT_FOUND) __builtin_trap();
                } else present = kernel_page_cache_resident(kernel_open_file_page_cache(file),
                    kernel_open_file_vfs_borrow(file), offset >> BOAROS_PAGE_SHIFT);
            }
            if (object) kernel_memory_object_release(&object);
            if (borrow && !__atomic_fetch_sub(borrow, 1, __ATOMIC_RELEASE)) __builtin_trap();
            if (mm_operation_begin(mm, &operation)) return -KERNEL_EAGAIN;
            int changed = generation != kernel_vma_set_generation(record->vmas);
            if (changed) continue;
        }
        output[i] = (unsigned char)present;
        *completed = ++i;
        address += BOAROS_PAGE_SIZE;
    }
    return 0;
}

enum kernel_mm_status kernel_mm_release(struct kernel_mm *mm)
{ return kernel_mm_release_for_task(mm,kernel_task_current()); }
