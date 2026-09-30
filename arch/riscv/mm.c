#include <arch/riscv/mm.h>
#include <kernel/elf64_source.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/proc_task.h>
#include <kernel/page.h>
#include <kernel/memory_object.h>
#include <kernel/task.h>
#include <kernel/sync.h>
#include <kernel/shm.h>
#include <kernel/vma.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_KERNEL_MM_RECORD_MAGIC UINT64_C(0x424f41524d4d5243)

/* Single-hart allocation; zero is permanently reserved as exhaustion. */
static uint64_t next_futex_mm_id = 1U;

enum riscv_kernel_mm_record_stage {
    RISCV_KERNEL_MM_RECORD_LIVE = 0,
    RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP,
    RISCV_KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP,
    RISCV_KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP,
    RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP,
};

struct riscv_kernel_mm_file_source {
    struct riscv_kernel_mm_file_source *next;
    struct kernel_open_file_description *file;
    uint32_t faults;
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

static void unpin_fault_source(struct riscv_kernel_mm_file_source **source)
{
    if (!(*source)->faults) __builtin_trap();
    (*source)->faults--;
}

struct riscv_kernel_mm_shared_anon {
    struct riscv_kernel_mm_shared_anon *next;
    struct kernel_memory_object *object;
};

struct riscv_file_mapping {
    struct riscv_file_mapping *next;
    struct kernel_file_mapping registration;
};

/* PTE owns the physical reference. This metadata survives cache eviction and
 * PROT_NONE; fork copies provenance, not a guess based on the PTE COW bit. */
struct riscv_file_resident {
    struct riscv_file_resident *next;
    struct riscv_file_resident *clone_source;
    struct riscv_file_resident *hash_next;
    uint64_t address;
    uint64_t physical_address;
    int private;
    struct kernel_page_cache_alias alias;
};

struct riscv_kernel_mm_elf_source {
    struct riscv_kernel_mm_elf_source *next;
    struct kernel_elf64_source *source;
    uint32_t faults;
    int draining;
};

static void unpin_elf_fault_source(struct riscv_kernel_mm_elf_source **source)
{
    if (!(*source)->faults) __builtin_trap();
    (*source)->faults--;
}

struct riscv_kernel_mm_record {
    uint64_t magic;
    uint64_t futex_id;
    uint32_t references;
    uint32_t users;
    enum riscv_kernel_mm_record_stage stage;
    uint64_t start_code, end_code, start_stack;
    uint64_t start_brk;
    uint64_t current_brk;
    uint64_t brk_limit;
    uint64_t mmap_base;
    uint64_t vdso_address;
    uint32_t brk_initialized;
    struct riscv_sv39_user_space space;
    struct kernel_vma_set *vmas;
    struct kernel_heap *vma_heap;
    struct riscv_kernel_mm_file_source *file_sources;
    struct riscv_kernel_mm_shared_anon *shared_anon;
    struct riscv_file_mapping *file_mappings;
    struct riscv_file_resident *file_residents;
    uint64_t resident_probes;
    struct riscv_file_resident **resident_buckets;
    size_t resident_capacity;
    size_t resident_count;
    struct riscv_kernel_mm_elf_source *elf_sources;
    struct kernel_open_file_description *executable_file;
};

static uint32_t sv39_permissions_from_mm(uint32_t permissions);

_Static_assert(sizeof(struct riscv_kernel_mm_record) <= BOAROS_PAGE_SIZE,
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

static enum kernel_mm_status resolve_record(
    const struct kernel_mm *mm,
    struct riscv_kernel_mm_record **record)
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
        ((*record)->stage == RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP ||
         (*record)->stage ==
             RISCV_KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP ||
         (*record)->stage ==
             RISCV_KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP) &&
        (*record)->space.state == RISCV_SV39_USER_SPACE_EMPTY;
    if ((*record)->magic != RISCV_KERNEL_MM_RECORD_MAGIC ||
        (*record)->futex_id == 0U ||
        (*record)->references == 0U ||
        (*record)->stage > RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP ||
        (*record)->brk_initialized > 1U ||
        ((*record)->vmas != 0 && (*record)->vma_heap == 0) ||
        ((*record)->file_sources != 0 && (*record)->vma_heap == 0) ||
        ((*record)->elf_sources != 0 && (*record)->vma_heap == 0) ||
        ((*record)->stage == RISCV_KERNEL_MM_RECORD_LIVE &&
         ((*record)->file_sources != 0 || (*record)->elf_sources != 0) &&
         (*record)->vmas == 0) ||
        ((*record)->brk_initialized != 0U &&
         (((*record)->stage == RISCV_KERNEL_MM_RECORD_LIVE &&
           (*record)->vmas == 0) ||
          ((*record)->start_brk & BOAROS_PAGE_MASK) != 0U ||
          ((*record)->brk_limit & BOAROS_PAGE_MASK) != 0U ||
          (*record)->start_brk > (*record)->current_brk ||
          (*record)->current_brk > (*record)->brk_limit)) ||
        ((*record)->brk_initialized != 0U &&
         ((*record)->mmap_base < RISCV_SV39_PAGE_SIZE_4K ||
          (*record)->mmap_base > RISCV_SV39_USER_LIMIT ||
          (*record)->vdso_address >= RISCV_SV39_USER_LIMIT)) ||
        (!empty_space_cleanup &&
         ((*record)->space.allocator != mm->allocator ||
          (*record)->space.state != RISCV_SV39_USER_SPACE_LIVE))) {
        return KERNEL_MM_STATUS_STATE;
    }
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

enum kernel_mm_status riscv_kernel_mm_create(
    struct kernel_mm *mm,
    struct riscv_sv39_user_space *space)
{
    KERNEL_NO_RECLAIM_IO;
    struct physical_page_allocator *allocator;
    struct riscv_kernel_mm_record *record;
    uint64_t record_page_address;
    void *pointer;
    enum physical_page_status page_status;

    if (mm == 0 || space == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(mm) ||
        space->state != RISCV_SV39_USER_SPACE_LIVE ||
        space->allocator == 0) {
        return KERNEL_MM_STATUS_STATE;
    }
    if (next_futex_mm_id == 0U) return KERNEL_MM_STATUS_NO_MEMORY;
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
    record->magic = RISCV_KERNEL_MM_RECORD_MAGIC;
    record->futex_id = next_futex_mm_id++;
    record->references = 1U;
    record->stage = RISCV_KERNEL_MM_RECORD_LIVE;
    if (riscv_sv39_user_space_move(&record->space, space) !=
        RISCV_SV39_STATUS_OK) {
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
    struct riscv_kernel_mm_record *record;
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
    if (record->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
        record->references == UINT32_MAX) {
        return KERNEL_MM_STATUS_STATE;
    }
    record->references++;
    *destination = *source;
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status fork_status_from_sv39(
    enum riscv_sv39_status status)
{
    switch (status) {
    case RISCV_SV39_STATUS_NO_MEMORY:
        return KERNEL_MM_STATUS_NO_MEMORY;
    case RISCV_SV39_STATUS_INVALID:
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    case RISCV_SV39_STATUS_CONFLICT:
        return KERNEL_MM_STATUS_CONFLICT;
    case RISCV_SV39_STATUS_STATE:
        return KERNEL_MM_STATUS_STATE;
    case RISCV_SV39_STATUS_NOT_MAPPED:
    case RISCV_SV39_STATUS_OK:
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

static struct riscv_kernel_mm_file_source *find_file_source(
    const struct riscv_kernel_mm_record *record,
    const struct kernel_open_file_description *file)
{
    struct riscv_kernel_mm_file_source *source;

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
    struct riscv_kernel_mm_record *record, size_t needed)
{
    if (needed <= record->resident_capacity) return KERNEL_MM_STATUS_OK;
    size_t capacity = record->resident_capacity ? record->resident_capacity : 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) return KERNEL_MM_STATUS_NO_MEMORY;
        capacity *= 2;
    }
    struct riscv_file_resident **buckets;
    enum kernel_heap_status hs = kernel_heap_allocate_zeroed(record->vma_heap,
        capacity, sizeof(*buckets), (void **)&buckets);
    if (hs != KERNEL_HEAP_STATUS_OK)
        return hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                               : KERNEL_MM_STATUS_STATE;
    for (struct riscv_file_resident *page = record->file_residents;
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

static void publish_file_resident(struct riscv_kernel_mm_record *record,
                                   struct riscv_file_resident *page)
{
    if (record->resident_count >= record->resident_capacity) __builtin_trap();
    size_t index = resident_bucket(page->address, record->resident_capacity);
    for (struct riscv_file_resident *old = record->resident_buckets[index];
         old != 0; old = old->hash_next)
        if (old->address == page->address) __builtin_trap();
    page->hash_next = record->resident_buckets[index];
    record->resident_buckets[index] = page;
    record->resident_count++;
    page->next = record->file_residents;
    record->file_residents = page;
}

static void unindex_file_resident(struct riscv_kernel_mm_record *record,
                                  struct riscv_file_resident *page)
{
    if (record->resident_count == 0) __builtin_trap();
    size_t index = resident_bucket(page->address, record->resident_capacity);
    struct riscv_file_resident **link = &record->resident_buckets[index];
    while (*link != page) {
        if (*link == 0) __builtin_trap();
        link = &(*link)->hash_next;
    }
    *link = page->hash_next;
    record->resident_count--;
}

static void forget_file_residents(struct riscv_kernel_mm_record *record,
                                  uint64_t start, uint64_t end)
{
    struct riscv_file_resident **link = &record->file_residents;
    while (*link != 0) {
        struct riscv_file_resident *page = *link;
        if (page->address >= start && page->address < end) {
            *link = page->next;
            unindex_file_resident(record, page);
            if (page->alias.previous != 0)
                kernel_page_cache_alias_detach(&page->alias);
            (void)kernel_heap_release(record->vma_heap, page);
        } else {
            link = &page->next;
        }
    }
}

static void rearm_shared_file_alias(void *owner, uint64_t address)
{
    struct riscv_kernel_mm_record *record = owner;
    struct kernel_vma vma;
    uint32_t permissions;
    if (kernel_vma_set_lookup(record->vmas, address, &vma) !=
            KERNEL_VMA_STATUS_OK ||
        vma.kind != KERNEL_VMA_KIND_FILE_SHARED) __builtin_trap();
    permissions = vma.permissions & ~KERNEL_MM_WRITE;
    if (riscv_sv39_user_protect_owned_page(&record->space, address,
            sv39_permissions_from_mm(permissions)) != RISCV_SV39_STATUS_OK)
        __builtin_trap();
}

static struct riscv_file_resident *find_file_resident(
    struct riscv_kernel_mm_record *record, uint64_t address)
{
    if (record->resident_capacity == 0) return 0;
    size_t index = resident_bucket(address, record->resident_capacity);
    for (struct riscv_file_resident *page = record->resident_buckets[index];
         page != 0; page = page->hash_next) {
        record->resident_probes++;
        if (page->address == address) return page;
    }
    return 0;
}

static void truncate_file_residents(void *owner, struct kernel_vfs_node *node,
                                    uint64_t size)
{
    struct riscv_kernel_mm_record *record = owner;
    struct riscv_file_resident **link = &record->file_residents;
    uint64_t last_page = size & ~BOAROS_PAGE_MASK;
    int cleared = 0;

    while (*link != 0) {
        struct riscv_file_resident *page = *link;
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
            if (riscv_sv39_user_unmap_owned_range(&record->space,
                    page->address, page->address + BOAROS_PAGE_SIZE) !=
                    RISCV_SV39_STATUS_OK) __builtin_trap();
            *link = page->next;
            unindex_file_resident(record, page);
            if (page->alias.previous != 0)
                kernel_page_cache_alias_detach(&page->alias);
            (void)kernel_heap_release(record->vma_heap, page);
            continue;
        }
        if (offset == last_page && !page->private &&
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
    if (cleared) __asm__ volatile("fence.i" : : : "memory");
}

static enum kernel_mm_status prepare_file_registration(
    struct riscv_kernel_mm_record *record,
    struct kernel_open_file_description *file,
    struct riscv_file_mapping **prepared)
{
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    enum kernel_heap_status status;
    *prepared = 0;
    if (node == 0) return KERNEL_MM_STATUS_STATE;
    for (struct riscv_file_mapping *entry = record->file_mappings;
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

static void drain_file_registrations(struct riscv_kernel_mm_record *record,
                                     int unused_only)
{
    struct riscv_file_mapping **link = &record->file_mappings;
    while (*link != 0) {
        struct riscv_file_mapping *entry = *link;
        int used = 0;
        for (struct riscv_kernel_mm_file_source *source = record->file_sources;
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
            kernel_file_mapping_unregister(&entry->registration);
            *link = entry->next;
            (void)kernel_heap_release(record->vma_heap, entry);
        }
    }
}

static enum kernel_mm_status drain_file_sources(
    struct riscv_kernel_mm_record *record,
    int unused_only)
{
    struct riscv_kernel_mm_file_source **link = &record->file_sources;
    int failed = 0;

    drain_file_registrations(record, unused_only);

    while (*link != 0) {
        struct riscv_kernel_mm_file_source *source = *link;
        int in_use = 0;

        if (source->faults || source->draining) {
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

static struct riscv_kernel_mm_elf_source *find_elf_source(
    const struct riscv_kernel_mm_record *record,
    const struct kernel_elf64_source *source)
{
    struct riscv_kernel_mm_elf_source *entry;

    for (entry = record->elf_sources; entry != 0; entry = entry->next) {
        if (!entry->draining && entry->source == source) {
            return entry;
        }
    }
    return 0;
}

static enum kernel_mm_status drain_elf_sources(
    struct riscv_kernel_mm_record *record,
    int unused_only)
{
    struct riscv_kernel_mm_elf_source **link = &record->elf_sources;
    int failed = 0;

    while (*link != 0) {
        struct riscv_kernel_mm_elf_source *entry = *link;
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
    const struct riscv_kernel_mm_record *source_record,
    struct riscv_kernel_mm_record *destination_record)
{
    struct riscv_kernel_mm_elf_source *source;

    for (source = source_record->elf_sources;
         source != 0;
         source = source->next) {
        struct riscv_kernel_mm_elf_source *copy;
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
    const struct riscv_kernel_mm_record *source_record,
    struct riscv_kernel_mm_record *destination_record)
{
    struct riscv_kernel_mm_file_source *source;

    for (source = source_record->file_sources;
         source != 0;
         source = source->next) {
        struct riscv_kernel_mm_file_source *copy;
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

static void drain_shared_anon(struct riscv_kernel_mm_record *record,
                              int unused_only)
{
    struct riscv_kernel_mm_shared_anon **link = &record->shared_anon;

    while (*link != 0) {
        struct riscv_kernel_mm_shared_anon *entry = *link;
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
    const struct riscv_kernel_mm_record *source_record,
    struct riscv_kernel_mm_record *destination_record)
{
    for (const struct riscv_kernel_mm_shared_anon *source =
             source_record->shared_anon;
         source != 0; source = source->next) {
        struct riscv_kernel_mm_shared_anon *copy;
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

    return start >= RISCV_SV39_PAGE_SIZE_4K && start < end &&
           end <= RISCV_SV39_USER_LIMIT &&
           (start & BOAROS_PAGE_MASK) == 0U &&
           (end & BOAROS_PAGE_MASK) == 0U &&
           (permissions & ~known) == 0U &&
           (permissions & known) != 0U &&
           !((permissions & KERNEL_MM_WRITE) != 0U &&
             (permissions & KERNEL_MM_READ) == 0U);
}

static enum kernel_mm_status mutable_vma_record(
    struct kernel_mm *mm,
    struct riscv_kernel_mm_record **record);

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

enum kernel_mm_status kernel_mm_fork(
    struct kernel_mm *destination,
    struct kernel_mm *source)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *source_record;
    struct riscv_kernel_mm_record *destination_record;
    uint64_t record_page_address;
    void *pointer;
    enum physical_page_status page_status;
    enum kernel_mm_status status;
    enum kernel_mm_status failure;
    enum riscv_sv39_status sv39_status;

    if (destination == 0 || source == 0 || destination == source) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(destination) || source->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(source, &source_record);
    if (status != KERNEL_MM_STATUS_OK ||
        source_record->stage != RISCV_KERNEL_MM_RECORD_LIVE) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    if (next_futex_mm_id == 0U) return KERNEL_MM_STATUS_NO_MEMORY;
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
    destination_record->magic = RISCV_KERNEL_MM_RECORD_MAGIC;
    destination_record->futex_id = next_futex_mm_id++;
    destination_record->references = 1U;
    destination_record->stage = RISCV_KERNEL_MM_RECORD_LIVE;
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
                    RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP;
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
        destination_record->stage = RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP;
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
    for (struct riscv_kernel_mm_file_source *source = destination_record->file_sources;
         source != 0 && status == KERNEL_MM_STATUS_OK; source = source->next) {
        struct riscv_file_mapping *prepared;
        status = prepare_file_registration(destination_record, source->file,
                                             &prepared);
        if (status == KERNEL_MM_STATUS_OK && prepared != 0) {
            prepared->next = destination_record->file_mappings;
            destination_record->file_mappings = prepared;
        }
    }
    if (status == KERNEL_MM_STATUS_OK)
        status = reserve_file_residents(destination_record, source_record->resident_count);
    for (struct riscv_file_resident *page = source_record->file_residents;
         page != 0 && status == KERNEL_MM_STATUS_OK; page = page->next) {
        struct riscv_file_resident *copy;
        enum kernel_heap_status hs = kernel_heap_allocate_zeroed(
            destination_record->vma_heap, 1U, sizeof(*copy), (void **)&copy);
        if (hs != KERNEL_HEAP_STATUS_OK) {
            status = hs == KERNEL_HEAP_STATUS_EMPTY ? KERNEL_MM_STATUS_NO_MEMORY
                                                     : KERNEL_MM_STATUS_STATE;
            break;
        }
        *copy = *page;
        copy->clone_source = page;
        copy->alias.next = 0;
        copy->alias.previous = 0;
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
        destination_record->stage = RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP;
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

    sv39_status = riscv_sv39_user_space_fork(
        &destination_record->space,
        &source_record->space,
        source_record->vmas != 0 ? shared_anon_leaf : 0,
        source_record->vmas);
    if (sv39_status == RISCV_SV39_STATUS_OK) {
        for (struct riscv_file_resident *page = destination_record->file_residents;
             page != 0; page = page->next) {
            if (page->clone_source != 0 &&
                page->clone_source->alias.previous != 0)
                kernel_page_cache_alias_clone(&page->alias,
                    &page->clone_source->alias, destination_record);
            page->clone_source = 0;
        }
        for (struct riscv_file_mapping *entry = destination_record->file_mappings;
             entry != 0; entry = entry->next)
            kernel_file_mapping_register(&entry->registration);
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
        destination->allocator = source->allocator;
        destination->record_page_address = record_page_address;
        destination->state = KERNEL_MM_LIVE;
        destination->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
        return KERNEL_MM_STATUS_OK;
    }

    failure = fork_status_from_sv39(sv39_status);
    destination_record->stage = RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP;
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

enum kernel_mm_status kernel_mm_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_mm_mapping *mapping)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    struct riscv_sv39_mapping riscv_mapping;
    enum kernel_mm_status status;
    enum riscv_sv39_status sv39_status;

    if (mm == 0 || mapping == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    sv39_status = riscv_sv39_user_lookup(&record->space,
                                         virtual_address,
                                         &riscv_mapping);
    if (sv39_status == RISCV_SV39_STATUS_NOT_MAPPED) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        return sv39_status == RISCV_SV39_STATUS_INVALID
                   ? KERNEL_MM_STATUS_INVALID_ARGUMENT
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    mapping->physical_address = riscv_mapping.physical_address;
    mapping->permissions = 0U;
    if ((riscv_mapping.permissions & RISCV_SV39_READ) != 0U) {
        mapping->permissions |= KERNEL_MM_READ;
    }
    if ((riscv_mapping.permissions & RISCV_SV39_WRITE) != 0U) {
        mapping->permissions |= KERNEL_MM_WRITE;
    }
    if ((riscv_mapping.permissions & RISCV_SV39_EXECUTE) != 0U) {
        mapping->permissions |= KERNEL_MM_EXECUTE;
    }
    if ((riscv_mapping.permissions & RISCV_SV39_USER) != 0U) {
        mapping->permissions |= KERNEL_MM_USER;
    }
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_vma_enable(
    struct kernel_mm *mm,
    struct kernel_heap *heap)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || heap == 0 || heap->page_allocator != mm->allocator) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
        record->references != 1U || record->vmas != 0) {
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
    struct riscv_kernel_mm_record *record;
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
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
        record->references != 1U || record->vmas == 0) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    vma.start = start;
    vma.end = end;
    vma.backing_offset = 0U;
    vma.permissions = permissions;
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
    struct riscv_kernel_mm_record *record;
    struct riscv_kernel_mm_elf_source *source_owner = 0;
    uint32_t run_count;
    uint32_t index;
    uint32_t inserted = 0U;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;
    enum kernel_heap_status heap_status;

    if (mm == 0 || source == 0 ||
        load_bias > RISCV_SV39_USER_LIMIT - RISCV_SV39_PAGE_SIZE_4K) {
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
            load_bias + run->start < RISCV_SV39_PAGE_SIZE_4K ||
            load_bias > UINT64_MAX - run->end ||
            load_bias + run->end > RISCV_SV39_USER_LIMIT ||
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
    struct riscv_kernel_mm_record *record;
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
    struct riscv_kernel_mm_record *record;
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
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (record->stage != RISCV_KERNEL_MM_RECORD_LIVE)
        return KERNEL_MM_STATUS_STATE;
    if (start_code > end_code || end_code >= RISCV_SV39_USER_LIMIT ||
        start_stack >= RISCV_SV39_USER_LIMIT)
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
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
    if (record->stage != RISCV_KERNEL_MM_RECORD_LIVE || !record->vmas)
        return KERNEL_MM_STATUS_STATE;
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

enum kernel_mm_status kernel_mm_vma_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_vma *vma)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || vma == 0 || virtual_address >= RISCV_SV39_USER_LIMIT) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
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
    struct riscv_kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK ||
        mm->state != KERNEL_MM_LIVE || record->users == UINT32_MAX)
        __builtin_trap();
    record->users++;
}

uint32_t kernel_mm_user_count(const struct kernel_mm *mm)
{
    struct riscv_kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK ||
        mm->state != KERNEL_MM_LIVE || !record->users)
        __builtin_trap();
    return record->users;
}

uint32_t kernel_mm_remove_user(struct kernel_mm *mm)
{
    struct riscv_kernel_mm_record *record;
    if (resolve_record(mm, &record) != KERNEL_MM_STATUS_OK ||
        mm->state != KERNEL_MM_LIVE || !record->users)
        __builtin_trap();
    return --record->users;
}

enum kernel_mm_status kernel_mm_futex_id(
    const struct kernel_mm *mm, uint64_t *identity)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || identity == 0) return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    if (mm->state != KERNEL_MM_LIVE) return KERNEL_MM_STATUS_STATE;
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE)
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
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || start < RISCV_SV39_PAGE_SIZE_4K || start > limit ||
        limit > RISCV_SV39_USER_LIMIT ||
        (start & BOAROS_PAGE_MASK) != 0U ||
        (limit & BOAROS_PAGE_MASK) != 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
        record->references != 1U || record->vmas == 0 ||
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
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || base < RISCV_SV39_PAGE_SIZE_4K ||
        base > RISCV_SV39_USER_LIMIT ||
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
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || address < RISCV_SV39_PAGE_SIZE_4K ||
        address > RISCV_SV39_USER_LIMIT - BOAROS_PAGE_SIZE ||
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
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || address == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
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
    if (address > RISCV_SV39_USER_LIMIT - BOAROS_PAGE_MASK) {
        return 0;
    }
    *aligned = (address + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    return 1;
}

static int align_mapping_length(uint64_t length, uint64_t *aligned)
{
    if (length == 0U ||
        length > RISCV_SV39_USER_LIMIT - BOAROS_PAGE_MASK) {
        return 0;
    }
    *aligned = (length + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    return *aligned != 0U;
}

static uint32_t sv39_permissions_from_mm(uint32_t permissions);

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
    const struct riscv_kernel_mm_record *record)
{
    uint64_t satp;

    if (riscv_sv39_user_space_satp(&record->space, &satp) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_current_satp() != satp) {
        return KERNEL_MM_STATUS_STATE;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status mutable_vma_record(
    struct kernel_mm *mm,
    struct riscv_kernel_mm_record **record)
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
        (*record)->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
        (*record)->vmas == 0 || (*record)->brk_initialized != 1U) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status unmap_space_range(
    struct riscv_kernel_mm_record *record,
    uint64_t start,
    uint64_t end)
{
    enum riscv_sv39_status status;

    if (end <= RISCV_SV39_PAGE_SIZE_4K) {
        return KERNEL_MM_STATUS_OK;
    }
    if (start < RISCV_SV39_PAGE_SIZE_4K) {
        start = RISCV_SV39_PAGE_SIZE_4K;
    }
    status = riscv_sv39_user_unmap_owned_range(&record->space,
                                               start,
                                               end);
    if (status == RISCV_SV39_STATUS_OK) {
        forget_file_residents(record, start, end);
        return KERNEL_MM_STATUS_OK;
    }
    return status == RISCV_SV39_STATUS_STATE
               ? KERNEL_MM_STATUS_STATE
               : KERNEL_MM_STATUS_ADDRESS_SPACE;
}

enum kernel_mm_status kernel_mm_mmap_anonymous(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    struct kernel_vma vma;
    struct kernel_vma_edit edit;
    struct kernel_memory_object *shared_object = 0;
    struct riscv_kernel_mm_shared_anon *shared_entry = 0;
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
        if (hint < RISCV_SV39_PAGE_SIZE_4K ||
            (hint & BOAROS_PAGE_MASK) != 0U ||
            hint > RISCV_SV39_USER_LIMIT - aligned_length) {
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
        if (record->mmap_base < RISCV_SV39_PAGE_SIZE_4K ||
            aligned_length >
                record->mmap_base - RISCV_SV39_PAGE_SIZE_4K) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        hint &= ~BOAROS_PAGE_MASK;
        if (hint < RISCV_SV39_PAGE_SIZE_4K ||
            hint > record->mmap_base - aligned_length) {
            hint = 0U;
        }
        vma_status = kernel_vma_set_find_topdown_gap(
            record->vmas,
            hint,
            RISCV_SV39_PAGE_SIZE_4K,
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
    struct riscv_kernel_mm_record *record,
    struct riscv_kernel_mm_file_source *source)
{
    if (source != 0) {
        (void)kernel_heap_release(record->vma_heap, source);
    }
}

static void finish_file_mapping(
    struct riscv_kernel_mm_record *record,
    struct kernel_open_file_description **file,
    struct riscv_kernel_mm_file_source *prepared)
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

struct riscv_file_mapping_plan {
    struct riscv_kernel_mm_record *record;
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
    struct riscv_file_mapping_plan *plan)
{
    struct riscv_kernel_mm_record *record;
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
        if (hint < RISCV_SV39_PAGE_SIZE_4K ||
            (hint & BOAROS_PAGE_MASK) != 0U ||
            hint > RISCV_SV39_USER_LIMIT - aligned_length) {
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
        if (record->mmap_base < RISCV_SV39_PAGE_SIZE_4K ||
            aligned_length >
                record->mmap_base - RISCV_SV39_PAGE_SIZE_4K) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        hint &= ~BOAROS_PAGE_MASK;
        if (hint < RISCV_SV39_PAGE_SIZE_4K ||
            hint > record->mmap_base - aligned_length) {
            hint = 0U;
        }
        vma_status = kernel_vma_set_find_topdown_gap(
            record->vmas,
            hint,
            RISCV_SV39_PAGE_SIZE_4K,
            record->mmap_base,
            aligned_length,
            &start);
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            return vma_status == KERNEL_VMA_STATUS_NOT_FOUND
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : status_from_vma(vma_status);
        }
    }
    *plan = (struct riscv_file_mapping_plan){
        .record = record,
        .start = start,
        .end = start + aligned_length,
        .permissions = normalized,
    };
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_validate_file_private_mapping(
    struct kernel_mm *mm,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_file_mapping_plan plan;

    return prepare_file_mapping(mm,
                                hint,
                                length,
                                file_offset,
                                permissions,
                                flags,
                                &plan);
}

enum kernel_mm_status kernel_mm_mmap_file_private(
    struct kernel_mm *mm,
    struct kernel_open_file_description **file,
    uint64_t hint,
    uint64_t length,
    uint64_t file_offset,
    uint32_t permissions,
    uint32_t flags,
    uint64_t *address)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_file_mapping_plan plan;
    struct riscv_kernel_mm_record *record;
    struct riscv_kernel_mm_file_source *prepared = 0;
    struct riscv_kernel_mm_file_source *existing;
    struct kernel_vma vma;
    struct riscv_file_mapping *registration = 0;
    struct kernel_vma_edit edit;
    enum kernel_heap_status heap_status;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if (file == 0 || *file == 0 || address == 0 ||
        (kernel_open_file_mode(*file) & KERNEL_VFS_S_IFMT) !=
            KERNEL_VFS_S_IFREG) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    status = prepare_file_mapping(mm,
                                  hint,
                                  length,
                                  file_offset,
                                  permissions,
                                  flags,
                                  &plan);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
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
        .file_shared_may_write =
            (flags & KERNEL_MM_MAP_SHARED) != 0U &&
            kernel_open_file_writable(*file),
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
        kernel_file_mapping_register(&registration->registration);
    }
    finish_file_mapping(record, file, prepared);
    drain_shared_anon(record, 1);
    *address = plan.start;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_munmap(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    struct kernel_vma_edit edit;
    uint64_t aligned_length;
    uint64_t end;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if ((address & BOAROS_PAGE_MASK) != 0U ||
        !align_mapping_length(length, &aligned_length) ||
        address >= RISCV_SV39_USER_LIMIT ||
        address > RISCV_SV39_USER_LIMIT - aligned_length) {
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
    struct riscv_kernel_mm_record *record;
    struct kernel_vma vma;
    struct kernel_vma_edit edit;
    struct riscv_kernel_mm_shared_anon *shared_entry = 0;
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

    if (hint != 0U) {
        if ((flags & KERNEL_SHM_RND) != 0U) {
            hint &= ~BOAROS_PAGE_MASK;
        }
        if ((hint & BOAROS_PAGE_MASK) != 0U ||
            hint < RISCV_SV39_PAGE_SIZE_4K ||
            hint >= RISCV_SV39_USER_LIMIT ||
            aligned_length > RISCV_SV39_USER_LIMIT - hint) {
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
        if (record->mmap_base < RISCV_SV39_PAGE_SIZE_4K ||
            aligned_length > record->mmap_base - RISCV_SV39_PAGE_SIZE_4K) {
            return KERNEL_MM_STATUS_NO_MEMORY;
        }
        vma_status = kernel_vma_set_find_topdown_gap(
            record->vmas,
            0U,
            RISCV_SV39_PAGE_SIZE_4K,
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
    for (struct riscv_kernel_mm_shared_anon *curr = record->shared_anon;
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
    struct riscv_kernel_mm_record *record;
    if (mm == 0 || (address & BOAROS_PAGE_MASK) ||
        address < RISCV_SV39_PAGE_SIZE_4K || address >= RISCV_SV39_USER_LIMIT)
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    enum kernel_mm_status status = mutable_vma_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK) return status;
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

enum kernel_mm_status kernel_mm_mprotect(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length,
    uint32_t permissions)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    struct kernel_vma_edit edit;
    uint64_t aligned_length;
    uint64_t end;
    uint32_t normalized;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;
    enum riscv_sv39_status sv39_status;

    if ((address & BOAROS_PAGE_MASK) != 0U ||
        !normalize_user_permissions(permissions, &normalized)) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (length == 0U) {
        return mutable_vma_record(mm, &record);
    }
    if (!align_mapping_length(length, &aligned_length) ||
        address > RISCV_SV39_USER_LIMIT - aligned_length) {
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
    sv39_status = riscv_sv39_user_protect_owned_range(
        &record->space,
        address,
        end,
        sv39_permissions_from_mm(normalized));
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        return sv39_status == RISCV_SV39_STATUS_STATE
                   ? KERNEL_MM_STATUS_STATE
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) return KERNEL_MM_STATUS_STATE;
    for (struct riscv_file_resident *page = record->file_residents;
         page != 0; page = page->next) {
        if (page->address >= address && page->address < end &&
            page->alias.previous != 0)
            rearm_shared_file_alias(record, page->address);
    }
    return KERNEL_MM_STATUS_OK;
}

int kernel_mm_msync(struct kernel_mm *mm, uint64_t address,
                    uint64_t length, uint32_t flags)
{
    struct riscv_kernel_mm_record *record;
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
            int result = kernel_open_file_sync_range(vma.backing,
                                                      file_start, file_end);
            if (result != 0) return result;
        }
        cursor = segment_end;
    }
    return hole ? -KERNEL_ENOMEM : 0;
}

enum kernel_mm_status kernel_mm_brk(
    struct kernel_mm *mm,
    uint64_t requested,
    uint64_t *result)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
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
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE ||
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
        record->current_brk = requested;
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
        record->current_brk = requested;
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
    record->current_brk = requested;
    *result = requested;
    return KERNEL_MM_STATUS_OK;
}

static uint32_t sv39_permissions_from_mm(uint32_t permissions)
{
    uint32_t result = 0U;

    if ((permissions & KERNEL_MM_READ) != 0U) {
        result |= RISCV_SV39_READ;
    }
    if ((permissions & KERNEL_MM_WRITE) != 0U) {
        result |= RISCV_SV39_WRITE;
    }
    if ((permissions & KERNEL_MM_EXECUTE) != 0U) {
        result |= RISCV_SV39_EXECUTE;
    }
    return result;
}

static void flush_user_page(uint64_t virtual_address, uint32_t permissions)
{
    __asm__ volatile("sfence.vma %0, zero"
                     :
                     : "r"(virtual_address)
                     : "memory");
    if ((permissions & KERNEL_MM_EXECUTE) != 0U) {
        __asm__ volatile("fence.i" : : : "memory");
    }
}

static enum kernel_mm_status map_file_page_status(
    enum riscv_sv39_status status)
{
    if (status == RISCV_SV39_STATUS_OK) {
        return KERNEL_MM_STATUS_OK;
    }
    if (status == RISCV_SV39_STATUS_NO_MEMORY) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    return status == RISCV_SV39_STATUS_STATE
               ? KERNEL_MM_STATUS_STATE
               : KERNEL_MM_STATUS_ADDRESS_SPACE;
}

static enum kernel_mm_status discard_file_page(
    struct riscv_sv39_user_space *space,
    uint64_t physical_address,
    enum kernel_mm_status result)
{
    enum riscv_sv39_status status =
        riscv_sv39_user_discard_owned_page(space, physical_address);

    if (status == RISCV_SV39_STATUS_OK) {
        return result;
    }
    return KERNEL_MM_STATUS_STATE;
}

/* Undo a leaf that was already published before a later COW step failed. */
static enum kernel_mm_status discard_mapped_page(
    struct riscv_sv39_user_space *space,
    uint64_t page_address,
    enum kernel_mm_status result)
{
    enum riscv_sv39_status status;

    status = riscv_sv39_user_unmap_owned_range(
        space,
        page_address,
        page_address + BOAROS_PAGE_SIZE);
    if (status != RISCV_SV39_STATUS_OK) {
        return KERNEL_MM_STATUS_STATE;
    }
    return result;
}

static enum kernel_mm_status map_cached_file_page(
    struct riscv_kernel_mm_record *record,
    const struct kernel_vma *vma,
    uint64_t page_address,
    uint64_t file_page_index)
{
    struct kernel_open_file_description *file = vma->backing;
    uint64_t physical_address;
    size_t valid_bytes;
    enum kernel_page_cache_status cache_status;
    enum riscv_sv39_status sv39_status;

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
    sv39_status = riscv_sv39_user_map_cow_page(
        &record->space,
        page_address,
        physical_address,
        sv39_permissions_from_mm(vma->permissions));
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        return discard_file_page(&record->space,
                                 physical_address,
                                 map_file_page_status(sv39_status));
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status map_shared_file_page(
    struct riscv_kernel_mm_record *record, const struct kernel_vma *vma,
    uint64_t page_address, uint64_t file_page_index, uint32_t access,
    struct riscv_file_resident *resident,
    int *translation_synchronized)
{
    struct kernel_open_file_description *file = vma->backing;
    uint64_t physical_address;
    size_t valid_bytes;
    enum kernel_page_cache_status cache_status;
    enum riscv_sv39_status sv39_status;
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
    sv39_status = riscv_sv39_user_map_owned_page(&record->space,
        page_address, physical_address, sv39_permissions_from_mm(initial));
    if (sv39_status != RISCV_SV39_STATUS_OK)
        return discard_file_page(&record->space, physical_address,
                                 map_file_page_status(sv39_status));
    if (memory_backed) {
        if (access == KERNEL_MM_WRITE)
            kernel_open_file_memory_modified(file);
        return KERNEL_MM_STATUS_OK;
    }
    cache_status = kernel_open_file_alias_attach(file, file_page_index,
        physical_address, &resident->alias, record, page_address,
        rearm_shared_file_alias);
    if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK)
        return discard_mapped_page(&record->space, page_address,
                                   KERNEL_MM_STATUS_STATE);
    if (access == KERNEL_MM_WRITE) {
        kernel_page_cache_alias_mark_dirty(&resident->alias);
        sv39_status = riscv_sv39_user_protect_owned_range(&record->space,
            page_address, page_address + BOAROS_PAGE_SIZE,
            sv39_permissions_from_mm(vma->permissions));
        if (sv39_status != RISCV_SV39_STATUS_OK) __builtin_trap();
        *translation_synchronized = 1;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status map_private_file_page(
    struct riscv_kernel_mm_record *record,
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
    enum riscv_sv39_status sv39_status;

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
        sv39_status = riscv_sv39_user_map_cow_page(
            &record->space,
            page_address,
            cached_address,
            sv39_permissions_from_mm(vma->permissions));
        if (sv39_status != RISCV_SV39_STATUS_OK) {
            return discard_file_page(&record->space,
                                     cached_address,
                                     map_file_page_status(sv39_status));
        }
        sv39_status = riscv_sv39_user_resolve_cow(
            &record->space,
            page_address,
            sv39_permissions_from_mm(vma->permissions));
        if (sv39_status == RISCV_SV39_STATUS_OK) {
            /* The COW resolver already published its local TLB/I-cache fence. */
            *translation_synchronized = 1;
        } else {
            /* No provenance is published for a failed fault. Do not leave
             * the temporary cache PTE behind after private allocation fails. */
            return discard_mapped_page(&record->space, page_address,
                                         map_file_page_status(sv39_status));
        }
        return map_file_page_status(sv39_status);
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
    sv39_status = riscv_sv39_user_map_owned_page(
        &record->space,
        page_address,
        private_address,
        sv39_permissions_from_mm(vma->permissions));
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        return discard_file_page(&record->space,
                                 private_address,
                                 map_file_page_status(sv39_status));
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status map_elf_source_page(
    struct riscv_kernel_mm_record *record,
    const struct kernel_vma *vma,
    uint64_t page_address,
    uint32_t access, int *retry)
{
    uint64_t source_offset;
    uint64_t physical_address;
    int shared = 0;
    int mapped = 0;
    enum kernel_elf64_source_status source_status;
    enum riscv_sv39_status sv39_status;
    enum kernel_mm_status status;

    if (vma->backing_offset > UINT64_MAX -
                             (page_address - vma->start)) {
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    struct riscv_kernel_mm_elf_source *source __attribute__((cleanup(unpin_elf_fault_source))) =
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
    struct riscv_sv39_mapping existing;
    if (version != kernel_vma_set_generation(record->vmas) ||
        riscv_sv39_user_lookup(&record->space, page_address, &existing) != RISCV_SV39_STATUS_NOT_MAPPED) {
        *retry = 1;
        return discard_file_page(&record->space, physical_address, KERNEL_MM_STATUS_OK);
    }
    if (shared != 0) {
        sv39_status = riscv_sv39_user_map_cow_page(
            &record->space,
            page_address,
            physical_address,
            sv39_permissions_from_mm(vma->permissions));
        if (sv39_status == RISCV_SV39_STATUS_OK) {
            mapped = 1;
        }
        if (sv39_status == RISCV_SV39_STATUS_OK &&
            access == KERNEL_MM_WRITE) {
            sv39_status = riscv_sv39_user_resolve_cow(
                &record->space,
                page_address,
                sv39_permissions_from_mm(vma->permissions));
        }
        if (sv39_status != RISCV_SV39_STATUS_OK) {
            enum kernel_mm_status failure = map_file_page_status(sv39_status);

            if (mapped != 0) {
                return discard_mapped_page(&record->space,
                                           page_address,
                                           failure);
            }
            return discard_file_page(&record->space,
                                     physical_address,
                                     failure);
        }
    } else {
        sv39_status = riscv_sv39_user_map_owned_page(
            &record->space,
            page_address,
            physical_address,
            sv39_permissions_from_mm(vma->permissions));
        if (sv39_status != RISCV_SV39_STATUS_OK) {
            return discard_file_page(&record->space,
                                     physical_address,
                                     map_file_page_status(sv39_status));
        }
    }
    status = KERNEL_MM_STATUS_OK;
    flush_user_page(page_address, vma->permissions);
    return status;
}

static enum kernel_mm_status resolve_user_fault_once(
    struct kernel_mm *mm,
    uint64_t virtual_address,
    uint32_t access, int *retry, int resumed)
{
    struct riscv_kernel_mm_record *record;
    struct riscv_sv39_mapping mapping;
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
    enum riscv_sv39_status sv39_status;

    if (mm == 0 || (access & ~known) != 0U || access == 0U ||
        (access & (access - 1U)) != 0U) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    if (virtual_address < RISCV_SV39_PAGE_SIZE_4K ||
        virtual_address >= RISCV_SV39_USER_LIMIT ||
        record->vmas == 0) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    vma_status = kernel_vma_set_lookup(record->vmas,
                                       virtual_address,
                                       &vma);
    if (vma_status == KERNEL_VMA_STATUS_NOT_FOUND) {
        sv39_status = riscv_sv39_user_lookup(&record->space,
                                             virtual_address,
                                             &mapping);
        if (sv39_status == RISCV_SV39_STATUS_OK) {
            return KERNEL_MM_STATUS_ADDRESS_SPACE;
        }
        return sv39_status == RISCV_SV39_STATUS_NOT_MAPPED
                   ? KERNEL_MM_STATUS_NOT_MAPPED
                   : KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        return status_from_vma(vma_status);
    }
    if ((vma.permissions & access) == 0U) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    sv39_status = riscv_sv39_user_lookup(&record->space,
                                         virtual_address,
                                         &mapping);
    if (sv39_status == RISCV_SV39_STATUS_OK) {
        uint32_t requested = access == KERNEL_MM_READ ? RISCV_SV39_READ :
                             access == KERNEL_MM_WRITE ? RISCV_SV39_WRITE : RISCV_SV39_EXECUTE;
        if (resumed && (mapping.permissions & requested)) {
            flush_user_page(virtual_address & ~BOAROS_PAGE_MASK, vma.permissions);
            return KERNEL_MM_STATUS_OK;
        }

        if (access == KERNEL_MM_WRITE &&
            (mapping.permissions & RISCV_SV39_WRITE) == 0U) {
            if (vma.kind == KERNEL_VMA_KIND_FILE_SHARED) {
                struct riscv_file_resident *page = find_file_resident(
                    record, virtual_address & ~BOAROS_PAGE_MASK);
                if (page == 0) __builtin_trap();
                if (kernel_open_file_memory_backed(vma.backing)) {
                    kernel_open_file_memory_modified(vma.backing);
                } else {
                    if (!page->alias.previous) __builtin_trap();
                    kernel_page_cache_alias_mark_dirty(&page->alias);
                }
                sv39_status = riscv_sv39_user_protect_owned_page(
                    &record->space, page->address,
                    sv39_permissions_from_mm(vma.permissions));
                if (sv39_status != RISCV_SV39_STATUS_OK) __builtin_trap();
                return KERNEL_MM_STATUS_OK;
            }
            status = require_active_space(record);
            if (status != KERNEL_MM_STATUS_OK) {
                return status;
            }
            sv39_status = riscv_sv39_user_resolve_cow(
                &record->space,
                virtual_address & ~BOAROS_PAGE_MASK,
                sv39_permissions_from_mm(vma.permissions));
            if (sv39_status == RISCV_SV39_STATUS_OK) {
                if (vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE) {
                    struct riscv_file_resident *page = find_file_resident(record,
                        virtual_address & ~BOAROS_PAGE_MASK);
                    if (page == 0 || riscv_sv39_user_lookup(&record->space,
                            page->address, &mapping) != RISCV_SV39_STATUS_OK)
                        __builtin_trap();
                    page->private = 1;
                    page->physical_address = mapping.physical_address;
                }
                return KERNEL_MM_STATUS_OK;
            }
            if (sv39_status == RISCV_SV39_STATUS_NO_MEMORY) {
                return KERNEL_MM_STATUS_NO_MEMORY;
            }
            return sv39_status == RISCV_SV39_STATUS_STATE
                       ? KERNEL_MM_STATUS_STATE
                       : KERNEL_MM_STATUS_ADDRESS_SPACE;
        }
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (sv39_status != RISCV_SV39_STATUS_NOT_MAPPED) {
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    if (vma.role == KERNEL_VMA_ROLE_STACK &&
        vma.end - (virtual_address & ~BOAROS_PAGE_MASK) >
            kernel_task_current_stack_limit()) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    if (riscv_sv39_user_space_satp(&record->space, &satp) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_current_satp() != satp) {
        return KERNEL_MM_STATUS_STATE;
    }
    page_address = virtual_address & ~BOAROS_PAGE_MASK;
    if ((vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE ||
         vma.kind == KERNEL_VMA_KIND_FILE_SHARED) &&
        (vma.fault_policy == KERNEL_VMA_FAULT_FILE_PRIVATE ||
         vma.fault_policy == KERNEL_VMA_FAULT_FILE_SHARED) &&
        vma.backing != 0) {
        uint64_t version = kernel_vma_set_generation(record->vmas);
        struct riscv_kernel_mm_file_source *source __attribute__((cleanup(unpin_fault_source))) =
            find_file_source(record, vma.backing);
        if (!source || source->faults == UINT32_MAX) __builtin_trap();
        source->faults++;
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
            riscv_sv39_user_lookup(&record->space, page_address, &mapping) != RISCV_SV39_STATUS_NOT_MAPPED) {
            *retry = 1;
            return KERNEL_MM_STATUS_OK;
        }
        status = reserve_file_residents(record, record->resident_count + 1);
        if (status != KERNEL_MM_STATUS_OK) return status;
        struct riscv_file_resident *resident;
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
            if (riscv_sv39_user_lookup(&record->space, page_address, &mapping) !=
                    RISCV_SV39_STATUS_OK) __builtin_trap();
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
            flush_user_page(page_address, vma.permissions);
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

        shared_status = kernel_memory_object_get_page(object, index,
                                                   &physical_address,
                                                   &created);
        if (shared_status == KERNEL_MEMORY_OBJECT_NO_MEMORY)
            return KERNEL_MM_STATUS_NO_MEMORY;
        if (shared_status != KERNEL_MEMORY_OBJECT_OK)
            return KERNEL_MM_STATUS_STATE;
        sv39_status = riscv_sv39_user_map_owned_page(
            &record->space, page_address, physical_address,
            sv39_permissions_from_mm(vma.permissions));
        if (sv39_status != RISCV_SV39_STATUS_OK) {
            if (physical_page_release(mm->allocator, physical_address) !=
                PHYSICAL_PAGE_STATUS_OK) __builtin_trap();
            if (created != 0)
                kernel_memory_object_discard_new_page(object, index,
                                                    physical_address);
            return sv39_status == RISCV_SV39_STATUS_NO_MEMORY
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : KERNEL_MM_STATUS_ADDRESS_SPACE;
        }
        flush_user_page(page_address, vma.permissions);
        return KERNEL_MM_STATUS_OK;
    }
    if (vma.kind != KERNEL_VMA_KIND_ANONYMOUS ||
        vma.fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO) {
        return KERNEL_MM_STATUS_NOT_MAPPED;
    }
    sv39_status = riscv_sv39_user_map_zeroed_page(
        &record->space,
        page_address,
        sv39_permissions_from_mm(vma.permissions));
    if (sv39_status == RISCV_SV39_STATUS_OK) {
        flush_user_page(page_address, vma.permissions);
        return KERNEL_MM_STATUS_OK;
    }
    if (sv39_status == RISCV_SV39_STATUS_NO_MEMORY) {
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    return KERNEL_MM_STATUS_ADDRESS_SPACE;
}

enum kernel_mm_status kernel_mm_resolve_user_fault(
    struct kernel_mm *mm, uint64_t address, uint32_t access)
{
    KERNEL_NO_RECLAIM_IO;
    struct kernel_task *task = kernel_task_current();
    const struct kernel_mm *task_mm = 0;
    if (!mm || !task || kernel_task_mm_borrow(task, &task_mm) !=
                     KERNEL_TASK_STATUS_OK ||
        task_mm->record_page_address != mm->record_page_address) task = 0;
    uint64_t block_reads = task ? kernel_proc_task_block_reads(task) : 0U;
    int resumed = 0;
    for (;;) {
        int retry = 0;
        enum kernel_mm_status result = resolve_user_fault_once(mm, address, access, &retry, resumed);
        if (!retry) {
            if (result == KERNEL_MM_STATUS_OK && task)
                kernel_proc_task_note_fault(task,
                    kernel_proc_task_block_reads(task) != block_reads);
            return result;
        }
        resumed = 1;
    }
}

enum kernel_mm_status riscv_kernel_mm_satp(
    const struct kernel_mm *mm,
    uint64_t *satp)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;

    if (mm == 0 || satp == 0) {
        return KERNEL_MM_STATUS_INVALID_ARGUMENT;
    }
    if (mm->state != KERNEL_MM_LIVE) {
        return KERNEL_MM_STATUS_STATE;
    }
    status = resolve_record(mm, &record);
    if (status != KERNEL_MM_STATUS_OK ||
        record->stage != RISCV_KERNEL_MM_RECORD_LIVE) {
        return status == KERNEL_MM_STATUS_OK ? KERNEL_MM_STATUS_STATE
                                             : status;
    }
    return riscv_sv39_user_space_satp(&record->space, satp) ==
                   RISCV_SV39_STATUS_OK
               ? KERNEL_MM_STATUS_OK
               : KERNEL_MM_STATUS_ADDRESS_SPACE;
}

enum kernel_mm_status kernel_mm_release(struct kernel_mm *mm)
{
    KERNEL_NO_RECLAIM_IO;
    struct riscv_kernel_mm_record *record;
    enum kernel_mm_status status;
    enum riscv_sv39_status sv39_status;
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
        if (record->stage != RISCV_KERNEL_MM_RECORD_LIVE) {
            return KERNEL_MM_STATUS_STATE;
        }
        if (record->references > 1U) {
            record->references--;
            finish_handle(mm, KERNEL_MM_RELEASED);
            return KERNEL_MM_STATUS_OK;
        }
        if (record->vmas == 0 && record->shared_anon == 0 &&
            record->file_sources == 0 &&
            record->elf_sources == 0 && record->executable_file == 0) {
            sv39_status = riscv_sv39_user_space_destroy(&record->space);
            if (sv39_status != RISCV_SV39_STATUS_OK) {
                __builtin_trap();
            }
            record->stage = RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP;
            mm->state = KERNEL_MM_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
            status = release_record_only(mm);
            return status;
        }
        record->stage = RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP;
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_VMAS;
    } else if ((mm->cleanup_stage != KERNEL_MM_CLEANUP_VMAS ||
                record->stage != RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP) &&
               (mm->cleanup_stage !=
                    KERNEL_MM_CLEANUP_FILE_SOURCES ||
                record->stage !=
                    RISCV_KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP) &&
               (mm->cleanup_stage != KERNEL_MM_CLEANUP_ELF_SOURCES ||
                record->stage !=
                    RISCV_KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP)) {
        return KERNEL_MM_STATUS_STATE;
    }

    if (record->stage == RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP) {
        drain_file_registrations(record, 0);
        forget_file_residents(record, 0, UINT64_MAX);
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
        record->stage = RISCV_KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP;
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_FILE_SOURCES;
    }
    if (record->stage == RISCV_KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP) {
        status = drain_file_sources(record, 0);
        if (status != KERNEL_MM_STATUS_OK) {
            mm->state = KERNEL_MM_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_FILE_SOURCES;
            return status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       ? status
                       : KERNEL_MM_STATUS_STATE;
        }
        record->stage = RISCV_KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP;
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_ELF_SOURCES;
    }
    if (record->stage == RISCV_KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP) {
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
        if (record->space.state == RISCV_SV39_USER_SPACE_EMPTY) {
            record->stage = RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
            status = release_record_only(mm);
            return status;
        }
    }

    sv39_status = riscv_sv39_user_space_destroy(&record->space);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        __builtin_trap();
    }
    record->stage = RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP;
    mm->state = KERNEL_MM_CLEANUP;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
    status = release_record_only(mm);
    return status;
}

void riscv_kernel_mm_get_statistics(const struct kernel_mm *mm,
                                    struct riscv_mm_statistics *statistics)
{
    struct riscv_kernel_mm_record *record;
    if (statistics == 0 || resolve_record(mm, &record) != KERNEL_MM_STATUS_OK)
        __builtin_trap();
    *statistics = (struct riscv_mm_statistics){record->resident_probes,
        record->space.protect_visits, record->space.protect_address_flushes,
        record->space.protect_global_flushes};
}
