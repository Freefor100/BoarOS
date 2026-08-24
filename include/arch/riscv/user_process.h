#ifndef BOAROS_ARCH_RISCV_USER_PROCESS_H
#define BOAROS_ARCH_RISCV_USER_PROCESS_H

#include <arch/riscv/sv39.h>
#include <kernel/physical_page.h>

#include <stdint.h>

enum riscv_user_process_status {
    RISCV_USER_PROCESS_STATUS_OK = 0,
    RISCV_USER_PROCESS_STATUS_INVALID_ARGUMENT,
    RISCV_USER_PROCESS_STATUS_NO_MEMORY,
    RISCV_USER_PROCESS_STATUS_NOT_MAPPED,
    RISCV_USER_PROCESS_STATUS_PAGE_ACCESS,
    RISCV_USER_PROCESS_STATUS_PAGE_RELEASE,
    RISCV_USER_PROCESS_STATUS_ADDRESS_SPACE,
    RISCV_USER_PROCESS_STATUS_CLEANUP_REQUIRED,
    RISCV_USER_PROCESS_STATUS_STATE,
};

enum riscv_user_process_state {
    RISCV_USER_PROCESS_EMPTY = 0,
    RISCV_USER_PROCESS_LIVE,
    RISCV_USER_PROCESS_MOVED,
    RISCV_USER_PROCESS_DESTROYED,
    RISCV_USER_PROCESS_CLEANUP,
};

struct riscv_user_process {
    struct physical_page_allocator *allocator;
    uint64_t record_page_address;
    enum riscv_user_process_state state;
};

/* Success consumes a LIVE address space and publishes one LIVE process. */
enum riscv_user_process_status riscv_user_process_create(
    struct riscv_user_process *process,
    struct riscv_sv39_user_space *space);

/* Success consumes a LIVE or CLEANUP source; failure changes neither handle. */
enum riscv_user_process_status riscv_user_process_move(
    struct riscv_user_process *destination,
    struct riscv_user_process *source);

enum riscv_user_process_status riscv_user_process_satp(
    const struct riscv_user_process *process,
    uint64_t *satp);

enum riscv_user_process_status riscv_user_process_lookup(
    const struct riscv_user_process *process,
    uint64_t virtual_address,
    struct riscv_sv39_mapping *mapping);

/* LIVE and CLEANUP destruction is retryable; the active root is rejected. */
enum riscv_user_process_status riscv_user_process_destroy(
    struct riscv_user_process *process);

#endif
