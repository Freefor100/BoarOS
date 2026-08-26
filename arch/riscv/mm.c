#include <arch/riscv/mm.h>
#include <kernel/page.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_KERNEL_MM_RECORD_MAGIC UINT64_C(0x424f41524d4d5243)

enum riscv_kernel_mm_record_stage {
    RISCV_KERNEL_MM_RECORD_LIVE = 0,
    RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP,
    RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP,
};

struct riscv_kernel_mm_record {
    uint64_t magic;
    uint32_t references;
    enum riscv_kernel_mm_record_stage stage;
    struct riscv_sv39_user_space space;
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

    if (!owner_handle(mm) ||
        physical_page_resolve(mm->allocator,
                              mm->record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return owner_handle(mm) ? KERNEL_MM_STATUS_PAGE_ACCESS
                                : KERNEL_MM_STATUS_STATE;
    }
    *record = pointer;
    if ((*record)->magic != RISCV_KERNEL_MM_RECORD_MAGIC ||
        (*record)->references == 0U ||
        (*record)->space.allocator != mm->allocator ||
        ((*record)->space.state != RISCV_SV39_USER_SPACE_LIVE &&
         (*record)->space.state != RISCV_SV39_USER_SPACE_CLEANUP)) {
        return KERNEL_MM_STATUS_STATE;
    }
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status release_record_only(struct kernel_mm *mm)
{
    if (physical_page_release(mm->allocator,
                              mm->record_page_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        mm->state = KERNEL_MM_CLEANUP;
        mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
        return KERNEL_MM_STATUS_PAGE_RELEASE;
    }
    finish_handle(mm, KERNEL_MM_RELEASED);
    return KERNEL_MM_STATUS_OK;
}

static enum kernel_mm_status abandon_unresolved_record(
    struct kernel_mm *mm,
    struct physical_page_allocator *allocator,
    uint64_t record_page_address,
    enum kernel_mm_status failure)
{
    if (physical_page_release(allocator, record_page_address) ==
        PHYSICAL_PAGE_STATUS_OK) {
        return failure;
    }
    mm->allocator = allocator;
    mm->record_page_address = record_page_address;
    mm->state = KERNEL_MM_CLEANUP;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
    return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
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
    enum kernel_mm_status status;
    enum riscv_sv39_status sv39_status;

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
    } else if (mm->cleanup_stage != KERNEL_MM_CLEANUP_SPACE ||
               record->stage != RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP) {
        return KERNEL_MM_STATUS_STATE;
    }

    table_pages = record->space.table_pages;
    leaf_pages = record->space.leaf_pages;
    sv39_status = riscv_sv39_user_space_destroy(&record->space);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        if (record->space.table_pages != table_pages ||
            record->space.leaf_pages != leaf_pages ||
            sv39_status == RISCV_SV39_STATUS_CLEANUP_REQUIRED) {
            record->stage = RISCV_KERNEL_MM_RECORD_SPACE_CLEANUP;
            mm->state = KERNEL_MM_CLEANUP;
            mm->cleanup_stage = KERNEL_MM_CLEANUP_SPACE;
            return KERNEL_MM_STATUS_CLEANUP_REQUIRED;
        }
        return KERNEL_MM_STATUS_ADDRESS_SPACE;
    }
    record->stage = RISCV_KERNEL_MM_RECORD_ONLY_CLEANUP;
    mm->state = KERNEL_MM_CLEANUP;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_RECORD;
    status = release_record_only(mm);
    return status == KERNEL_MM_STATUS_PAGE_RELEASE
               ? KERNEL_MM_STATUS_CLEANUP_REQUIRED
               : status;
}
