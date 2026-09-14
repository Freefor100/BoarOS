#include <arch/riscv/mm.h>
#include <kernel/elf64_source.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/vma.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_KERNEL_MM_RECORD_MAGIC UINT64_C(0x424f41524d4d5243)

enum riscv_kernel_mm_record_stage {
    RISCV_KERNEL_MM_RECORD_LIVE = 0,
    RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP,
    RISCV_KERNEL_MM_RECORD_FILE_SOURCES_CLEANUP,
    RISCV_KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP,
    RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP,
    RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP,
};

struct riscv_kernel_mm_file_source {
    struct riscv_kernel_mm_file_source *next;
    struct kernel_open_file_description *file;
};

struct riscv_kernel_mm_elf_source {
    struct riscv_kernel_mm_elf_source *next;
    struct kernel_elf64_source *source;
};

struct riscv_kernel_mm_record {
    uint64_t magic;
    uint32_t references;
    enum riscv_kernel_mm_record_stage stage;
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
    struct riscv_kernel_mm_elf_source *elf_sources;
};

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
        if (source->file == file) {
            return source;
        }
    }
    return 0;
}

static enum kernel_mm_status drain_file_sources(
    struct riscv_kernel_mm_record *record,
    int unused_only)
{
    struct riscv_kernel_mm_file_source **link = &record->file_sources;
    int failed = 0;

    while (*link != 0) {
        struct riscv_kernel_mm_file_source *source = *link;
        struct riscv_kernel_mm_file_source *next = source->next;
        int in_use = 0;

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

            if (kernel_open_file_release(&owner) !=
                KERNEL_OPEN_FILE_STATUS_OK) {
                failed = 1;
                link = &source->next;
                continue;
            }
            source->file = 0;
        }
        (void)kernel_heap_release(record->vma_heap, source);
        *link = next;
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
        if (entry->source == source) {
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
        struct riscv_kernel_mm_elf_source *next = entry->next;
        int in_use = 0;

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
                enum kernel_elf64_source_status source_status =
                    kernel_elf64_source_release(&owner);

                if (source_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
                    failed = 1;
                    link = &entry->next;
                    continue;
                }
                entry->source = 0;
            }
        }
        (void)kernel_heap_release(record->vma_heap, entry);
        *link = next;
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

enum kernel_mm_status kernel_mm_fork(
    struct kernel_mm *destination,
    struct kernel_mm *source)
{
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
    status = clone_elf_sources(source_record, destination_record);
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
                   ? KERNEL_MM_STATUS_CLEANUP_REQUIRED
                   : KERNEL_MM_STATUS_STATE;
    }

    sv39_status = riscv_sv39_user_space_fork(
        &destination_record->space,
        &source_record->space);
    if (sv39_status == RISCV_SV39_STATUS_OK) {
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
    struct riscv_kernel_mm_record *record;
    struct kernel_vma vma;
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

enum kernel_mm_status kernel_mm_vma_lookup(
    const struct kernel_mm *mm,
    uint64_t virtual_address,
    struct kernel_vma *vma)
{
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

enum kernel_mm_status kernel_mm_brk_initialize(
    struct kernel_mm *mm,
    uint64_t start,
    uint64_t limit)
{
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
    struct riscv_kernel_mm_record *record;
    struct kernel_vma vma;
    struct kernel_vma_edit edit;
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
                   KERNEL_MM_MAP_FIXED_NOREPLACE)) != 0U ||
        flags == (KERNEL_MM_MAP_FIXED |
                  KERNEL_MM_MAP_FIXED_NOREPLACE)) {
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
    if (flags != 0U) {
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
    vma = (struct kernel_vma){
        .start = start,
        .end = end,
        .backing_offset = 0U,
        .permissions = normalized,
        .kind = KERNEL_VMA_KIND_ANONYMOUS,
        .role = KERNEL_VMA_ROLE_MMAP,
        .fault_policy = KERNEL_VMA_FAULT_DEMAND_ZERO,
        .backing = 0,
    };
    if ((flags & KERNEL_MM_MAP_FIXED) == 0U) {
        status = status_from_vma(kernel_vma_set_insert(record->vmas,
                                                       &vma));
        if (status == KERNEL_MM_STATUS_OK) {
            *address = start;
        }
        return status;
    }
    vma_status = kernel_vma_set_prepare_replace(record->vmas,
                                                start,
                                                end,
                                                &vma,
                                                &edit);
    if (vma_status != KERNEL_VMA_STATUS_OK) {
        return status_from_vma(vma_status);
    }
    status = require_active_space(record);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    status = unmap_space_range(record, start, end);
    if (status != KERNEL_MM_STATUS_OK) {
        return status;
    }
    if (kernel_vma_set_commit_edit(record->vmas, &edit) !=
        KERNEL_VMA_STATUS_OK) {
        return KERNEL_MM_STATUS_STATE;
    }
    (void)drain_file_sources(record, 1);
    (void)drain_elf_sources(record, 1);
    *address = start;
    return KERNEL_MM_STATUS_OK;
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
    struct riscv_kernel_mm_record *record;
    struct riscv_kernel_mm_file_source *prepared = 0;
    struct riscv_kernel_mm_file_source *existing;
    struct kernel_vma vma;
    struct kernel_vma_edit edit;
    uint64_t aligned_length;
    uint64_t start;
    uint64_t end;
    uint32_t normalized;
    int overlaps;
    enum kernel_heap_status heap_status;
    enum kernel_mm_status status;
    enum kernel_vma_status vma_status;

    if (file == 0 || *file == 0 || address == 0 ||
        (kernel_open_file_mode(*file) & KERNEL_VFS_S_IFMT) !=
            KERNEL_VFS_S_IFREG ||
        (file_offset & BOAROS_PAGE_MASK) != 0U ||
        !normalize_user_permissions(permissions, &normalized) ||
        (flags & ~(KERNEL_MM_MAP_FIXED |
                   KERNEL_MM_MAP_FIXED_NOREPLACE)) != 0U ||
        flags == (KERNEL_MM_MAP_FIXED |
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
    if (flags != 0U) {
        if (hint < RISCV_SV39_PAGE_SIZE_4K ||
            (hint & BOAROS_PAGE_MASK) != 0U ||
            hint > RISCV_SV39_USER_LIMIT - aligned_length) {
            discard_prepared_file_source(record, prepared);
            return KERNEL_MM_STATUS_INVALID_ARGUMENT;
        }
        start = hint;
        if ((flags & KERNEL_MM_MAP_FIXED_NOREPLACE) != 0U) {
            vma_status = kernel_vma_set_overlaps(record->vmas,
                                                 start,
                                                 start + aligned_length,
                                                 &overlaps);
            if (vma_status != KERNEL_VMA_STATUS_OK || overlaps != 0) {
                discard_prepared_file_source(record, prepared);
                return vma_status == KERNEL_VMA_STATUS_OK
                           ? KERNEL_MM_STATUS_CONFLICT
                           : status_from_vma(vma_status);
            }
        }
    } else {
        if (record->mmap_base < RISCV_SV39_PAGE_SIZE_4K ||
            aligned_length >
                record->mmap_base - RISCV_SV39_PAGE_SIZE_4K) {
            discard_prepared_file_source(record, prepared);
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
            discard_prepared_file_source(record, prepared);
            return vma_status == KERNEL_VMA_STATUS_NOT_FOUND
                       ? KERNEL_MM_STATUS_NO_MEMORY
                       : status_from_vma(vma_status);
        }
    }
    end = start + aligned_length;
    vma = (struct kernel_vma){
        .start = start,
        .end = end,
        .backing_offset = file_offset,
        .permissions = normalized,
        .kind = KERNEL_VMA_KIND_FILE_PRIVATE,
        .role = KERNEL_VMA_ROLE_MMAP,
        .fault_policy = KERNEL_VMA_FAULT_FILE_PRIVATE,
        .backing = *file,
    };
    if ((flags & KERNEL_MM_MAP_FIXED) == 0U) {
        status = status_from_vma(kernel_vma_set_insert(record->vmas,
                                                       &vma));
        if (status != KERNEL_MM_STATUS_OK) {
            discard_prepared_file_source(record, prepared);
            return status;
        }
    } else {
        vma_status = kernel_vma_set_prepare_replace(record->vmas,
                                                    start,
                                                    end,
                                                    &vma,
                                                    &edit);
        if (vma_status != KERNEL_VMA_STATUS_OK) {
            discard_prepared_file_source(record, prepared);
            return status_from_vma(vma_status);
        }
        status = require_active_space(record);
        if (status == KERNEL_MM_STATUS_OK) {
            status = unmap_space_range(record, start, end);
        }
        if (status != KERNEL_MM_STATUS_OK ||
            kernel_vma_set_commit_edit(record->vmas, &edit) !=
                KERNEL_VMA_STATUS_OK) {
            discard_prepared_file_source(record, prepared);
            return status != KERNEL_MM_STATUS_OK
                       ? status
                       : KERNEL_MM_STATUS_STATE;
        }
    }
    finish_file_mapping(record, file, prepared);
    *address = start;
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_munmap(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length)
{
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
    (void)drain_file_sources(record, 1);
    (void)drain_elf_sources(record, 1);
    return KERNEL_MM_STATUS_OK;
}

enum kernel_mm_status kernel_mm_mprotect(
    struct kernel_mm *mm,
    uint64_t address,
    uint64_t length,
    uint32_t permissions)
{
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
    return kernel_vma_set_commit_edit(record->vmas, &edit) ==
                   KERNEL_VMA_STATUS_OK
               ? KERNEL_MM_STATUS_OK
               : KERNEL_MM_STATUS_STATE;
}

enum kernel_mm_status kernel_mm_brk(
    struct kernel_mm *mm,
    uint64_t requested,
    uint64_t *result)
{
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
    uint32_t access)
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

enum kernel_mm_status kernel_mm_resolve_user_fault(
    struct kernel_mm *mm,
    uint64_t virtual_address,
    uint32_t access)
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
        if (access == KERNEL_MM_WRITE &&
            (mapping.permissions & RISCV_SV39_WRITE) == 0U) {
            status = require_active_space(record);
            if (status != KERNEL_MM_STATUS_OK) {
                return status;
            }
            sv39_status = riscv_sv39_user_resolve_cow(
                &record->space,
                virtual_address & ~BOAROS_PAGE_MASK,
                sv39_permissions_from_mm(vma.permissions));
            if (sv39_status == RISCV_SV39_STATUS_OK) {
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
    if (riscv_sv39_user_space_satp(&record->space, &satp) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_current_satp() != satp) {
        return KERNEL_MM_STATUS_STATE;
    }
    page_address = virtual_address & ~BOAROS_PAGE_MASK;
    if (vma.kind == KERNEL_VMA_KIND_FILE_PRIVATE &&
        vma.fault_policy == KERNEL_VMA_FAULT_FILE_PRIVATE &&
        vma.backing != 0) {
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
        file_page_index = file_page_offset >> BOAROS_PAGE_SHIFT;
        status = access == KERNEL_MM_WRITE
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
                                   access);
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

enum kernel_mm_status riscv_kernel_mm_satp(
    const struct kernel_mm *mm,
    uint64_t *satp)
{
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
    struct riscv_kernel_mm_record *record;
    uint32_t table_pages;
    uint32_t leaf_pages;
    uint32_t protected_pages;
    uint32_t cow_pages;
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
        if (record->vmas == 0 && record->file_sources == 0 &&
            record->elf_sources == 0) {
            table_pages = record->space.table_pages;
            leaf_pages = record->space.leaf_pages;
            protected_pages = record->space.protected_pages;
            cow_pages = record->space.cow_pages;
            sv39_status = riscv_sv39_user_space_destroy(&record->space);
            if (sv39_status != RISCV_SV39_STATUS_OK) {
                if (record->space.table_pages != table_pages ||
                    record->space.leaf_pages != leaf_pages ||
                    record->space.protected_pages != protected_pages ||
                    record->space.cow_pages != cow_pages) {
                    record->stage = RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP;
                    mm->state = KERNEL_MM_CLEANUP;
                    mm->cleanup_stage = KERNEL_MM_CLEANUP_SPACE;
                }
                return KERNEL_MM_STATUS_ADDRESS_SPACE;
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
                    RISCV_KERNEL_MM_RECORD_ELF_SOURCES_CLEANUP) &&
               (mm->cleanup_stage != KERNEL_MM_CLEANUP_SPACE ||
                record->stage != RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP)) {
        return KERNEL_MM_STATUS_STATE;
    }

    if (record->stage == RISCV_KERNEL_MM_RECORD_VMAS_CLEANUP) {
        if (record->vmas != 0) {
            vma_status = kernel_vma_set_destroy(&record->vmas);
            if (vma_status != KERNEL_VMA_STATUS_OK) {
                return KERNEL_MM_STATUS_STATE;
            }
        }
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
        record->stage = RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP;
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_SPACE;
    }
    if (record->stage != RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP) {
        return KERNEL_MM_STATUS_STATE;
    }

    table_pages = record->space.table_pages;
    leaf_pages = record->space.leaf_pages;
    protected_pages = record->space.protected_pages;
    cow_pages = record->space.cow_pages;
    sv39_status = riscv_sv39_user_space_destroy(&record->space);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        if (record->space.table_pages != table_pages ||
            record->space.leaf_pages != leaf_pages ||
            record->space.protected_pages != protected_pages ||
            record->space.cow_pages != cow_pages) {
            record->stage = RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP;
            mm->state = KERNEL_MM_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_SPACE;
        }
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    record->stage = RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP;
    mm->state = KERNEL_MM_CLEANUP;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
    status = release_record_only(mm);
    return status;
}
