#ifndef BOAROS_ARCH_RISCV_SV39_H
#define BOAROS_ARCH_RISCV_SV39_H

#include <kernel/physical_page.h>

#include <stdint.h>

#define RISCV_SV39_PAGE_SIZE_4K UINT64_C(0x1000)
#define RISCV_SV39_PAGE_SIZE_2M UINT64_C(0x200000)

#define RISCV_SV39_READ (UINT32_C(1) << 0U)
#define RISCV_SV39_WRITE (UINT32_C(1) << 1U)
#define RISCV_SV39_EXECUTE (UINT32_C(1) << 2U)

enum riscv_sv39_status {
    RISCV_SV39_STATUS_OK = 0,
    RISCV_SV39_STATUS_INVALID,
    RISCV_SV39_STATUS_NO_MEMORY,
    RISCV_SV39_STATUS_CONFLICT,
    RISCV_SV39_STATUS_STATE,
};

enum riscv_sv39_page_table_state {
    RISCV_SV39_STATE_UNINITIALIZED = 0,
    RISCV_SV39_STATE_BUILDING,
    RISCV_SV39_STATE_FAILED,
    RISCV_SV39_STATE_ACTIVE,
};

struct riscv_sv39_page_table {
    struct physical_page_allocator *allocator;
    uint64_t root_address;
    uint64_t table_pages;
    uint64_t leaf_4k;
    uint64_t leaf_2m;
    enum riscv_sv39_page_table_state state;
};

/* The table object must be zero-initialized before its first initialization. */
enum riscv_sv39_status riscv_sv39_page_table_init(
    struct riscv_sv39_page_table *table,
    struct physical_page_allocator *allocator);

/*
 * Build mappings before activation, while every page-table physical address is
 * directly accessible.  Invalid inputs leave a BUILDING table reusable.  An
 * allocation or mapping conflict changes it to FAILED and may leave earlier
 * leaves and intermediate tables installed.
 */
enum riscv_sv39_status riscv_sv39_map_range(
    struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t size,
    uint32_t permissions);

/* Both address spaces must keep the current execution context and table writable. */
enum riscv_sv39_status riscv_sv39_activate(
    struct riscv_sv39_page_table *table);

uint64_t riscv_sv39_current_satp(void);

#endif
