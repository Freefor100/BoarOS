#ifndef BOAROS_ARCH_RISCV_EXEC_H
#define BOAROS_ARCH_RISCV_EXEC_H

#include <arch/riscv/sv39.h>
#include <kernel/physical_page.h>

enum riscv_exec_status {
    RISCV_EXEC_STATUS_OK = 0,
    RISCV_EXEC_STATUS_INVALID_ARGUMENT,
    RISCV_EXEC_STATUS_ALREADY_INITIALIZED,
};

enum riscv_exec_status riscv_exec_init(
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table);

#endif
