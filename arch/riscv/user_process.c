#include <arch/riscv/user_process.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_USER_PROCESS_RECORD_MAGIC UINT64_C(0x4250524f43455353)

struct riscv_user_process_record {
    uint64_t magic;
    struct riscv_sv39_user_space space;
};

_Static_assert(sizeof(struct riscv_user_process_record) <= BOAROS_PAGE_SIZE,
               "RISC-V process record must fit in one physical page");

static int empty_handle(const struct riscv_user_process *process)
{
    return process->state == RISCV_USER_PROCESS_EMPTY &&
           process->allocator == 0 &&
           process->record_page_address == 0U;
}

static int owner_handle(const struct riscv_user_process *process)
{
    return process->allocator != 0 &&
           (process->record_page_address & BOAROS_PAGE_MASK) == 0U &&
           (process->state == RISCV_USER_PROCESS_LIVE ||
            process->state == RISCV_USER_PROCESS_CLEANUP);
}

static void clear_page(void *pointer)
{
    volatile unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE; index++) {
        bytes[index] = 0U;
    }
}

static void finish_handle(struct riscv_user_process *process,
                          enum riscv_user_process_state state)
{
    process->allocator = 0;
    process->record_page_address = 0U;
    process->state = state;
}

static enum riscv_user_process_status release_unowned_record(
    struct riscv_user_process *process,
    struct physical_page_allocator *allocator,
    uint64_t record_page_address,
    enum riscv_user_process_status failure)
{
    if (physical_page_release(allocator, record_page_address) ==
        PHYSICAL_PAGE_STATUS_OK) {
        return failure;
    }
    process->allocator = allocator;
    process->record_page_address = record_page_address;
    process->state = RISCV_USER_PROCESS_CLEANUP;
    return RISCV_USER_PROCESS_STATUS_CLEANUP_REQUIRED;
}

static enum riscv_user_process_status resolve_record(
    const struct riscv_user_process *process,
    struct riscv_user_process_record **record)
{
    void *pointer;

    if (!owner_handle(process) ||
        process->state != RISCV_USER_PROCESS_LIVE) {
        return RISCV_USER_PROCESS_STATUS_STATE;
    }
    if (physical_page_resolve(process->allocator,
                              process->record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_USER_PROCESS_STATUS_PAGE_ACCESS;
    }
    *record = pointer;
    if ((*record)->magic != RISCV_USER_PROCESS_RECORD_MAGIC ||
        (*record)->space.allocator != process->allocator ||
        ((*record)->space.state != RISCV_SV39_USER_SPACE_LIVE &&
         (*record)->space.state != RISCV_SV39_USER_SPACE_CLEANUP)) {
        return RISCV_USER_PROCESS_STATUS_STATE;
    }
    return RISCV_USER_PROCESS_STATUS_OK;
}

enum riscv_user_process_status riscv_user_process_create(
    struct riscv_user_process *process,
    struct riscv_sv39_user_space *space)
{
    struct physical_page_allocator *allocator;
    struct riscv_user_process_record *record;
    uint64_t record_page_address;
    void *pointer;
    enum physical_page_status page_status;

    if (process == 0 || space == 0) {
        return RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(process) ||
        space->state != RISCV_SV39_USER_SPACE_LIVE ||
        space->allocator == 0) {
        return RISCV_USER_PROCESS_STATUS_STATE;
    }
    allocator = space->allocator;
    page_status = physical_page_allocate(allocator,
                                         &record_page_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_USER_PROCESS_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_USER_PROCESS_STATUS_STATE;
    }
    if (physical_page_resolve(allocator,
                              record_page_address,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return release_unowned_record(
            process,
            allocator,
            record_page_address,
            RISCV_USER_PROCESS_STATUS_PAGE_ACCESS);
    }

    clear_page(pointer);
    record = pointer;
    record->magic = RISCV_USER_PROCESS_RECORD_MAGIC;
    if (riscv_sv39_user_space_move(&record->space, space) !=
        RISCV_SV39_STATUS_OK) {
        return release_unowned_record(
            process,
            allocator,
            record_page_address,
            RISCV_USER_PROCESS_STATUS_ADDRESS_SPACE);
    }
    process->allocator = allocator;
    process->record_page_address = record_page_address;
    process->state = RISCV_USER_PROCESS_LIVE;
    return RISCV_USER_PROCESS_STATUS_OK;
}

enum riscv_user_process_status riscv_user_process_move(
    struct riscv_user_process *destination,
    struct riscv_user_process *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT;
    }
    if (!empty_handle(destination) || !owner_handle(source)) {
        return RISCV_USER_PROCESS_STATUS_STATE;
    }

    *destination = *source;
    finish_handle(source, RISCV_USER_PROCESS_MOVED);
    return RISCV_USER_PROCESS_STATUS_OK;
}

enum riscv_user_process_status riscv_user_process_satp(
    const struct riscv_user_process *process,
    uint64_t *satp)
{
    struct riscv_user_process_record *record;
    enum riscv_user_process_status status;
    enum riscv_sv39_status sv39_status;

    if (process == 0 || satp == 0) {
        return RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT;
    }
    status = resolve_record(process, &record);
    if (status != RISCV_USER_PROCESS_STATUS_OK) {
        return status;
    }
    sv39_status = riscv_sv39_user_space_satp(&record->space, satp);
    return sv39_status == RISCV_SV39_STATUS_OK
               ? RISCV_USER_PROCESS_STATUS_OK
               : RISCV_USER_PROCESS_STATUS_ADDRESS_SPACE;
}

enum riscv_user_process_status riscv_user_process_lookup(
    const struct riscv_user_process *process,
    uint64_t virtual_address,
    struct riscv_sv39_mapping *mapping)
{
    struct riscv_user_process_record *record;
    enum riscv_user_process_status status;
    enum riscv_sv39_status sv39_status;

    if (process == 0 || mapping == 0) {
        return RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT;
    }
    status = resolve_record(process, &record);
    if (status != RISCV_USER_PROCESS_STATUS_OK) {
        return status;
    }
    sv39_status = riscv_sv39_user_lookup(&record->space,
                                         virtual_address,
                                         mapping);
    if (sv39_status == RISCV_SV39_STATUS_OK) {
        return RISCV_USER_PROCESS_STATUS_OK;
    }
    if (sv39_status == RISCV_SV39_STATUS_NOT_MAPPED) {
        return RISCV_USER_PROCESS_STATUS_NOT_MAPPED;
    }
    return sv39_status == RISCV_SV39_STATUS_INVALID
               ? RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT
               : RISCV_USER_PROCESS_STATUS_ADDRESS_SPACE;
}

enum riscv_user_process_status riscv_user_process_destroy(
    struct riscv_user_process *process)
{
    struct riscv_user_process_record *record;
    enum riscv_user_process_status status;

    if (process == 0) {
        return RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT;
    }
    if (!owner_handle(process)) {
        return RISCV_USER_PROCESS_STATUS_STATE;
    }
    if (process->state == RISCV_USER_PROCESS_CLEANUP) {
        if (physical_page_release(process->allocator,
                                  process->record_page_address) !=
            PHYSICAL_PAGE_STATUS_OK) {
            return RISCV_USER_PROCESS_STATUS_PAGE_RELEASE;
        }
        finish_handle(process, RISCV_USER_PROCESS_DESTROYED);
        return RISCV_USER_PROCESS_STATUS_OK;
    }

    status = resolve_record(process, &record);
    if (status != RISCV_USER_PROCESS_STATUS_OK) {
        return status;
    }
    if (riscv_sv39_user_space_destroy(&record->space) !=
        RISCV_SV39_STATUS_OK) {
        return RISCV_USER_PROCESS_STATUS_ADDRESS_SPACE;
    }
    if (physical_page_release(process->allocator,
                              process->record_page_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        process->state = RISCV_USER_PROCESS_CLEANUP;
        return RISCV_USER_PROCESS_STATUS_CLEANUP_REQUIRED;
    }
    finish_handle(process, RISCV_USER_PROCESS_DESTROYED);
    return RISCV_USER_PROCESS_STATUS_OK;
}
