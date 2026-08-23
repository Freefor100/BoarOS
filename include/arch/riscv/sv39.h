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
};

struct riscv_sv39_page_table {
    struct physical_page_allocator *allocator;
    uint64_t root_address;
    uint64_t table_pages;
    uint64_t leaf_4k;
    uint64_t leaf_2m;
    uint32_t initialized;
};

/* Every allocator range must end at or below the Sv39 56-bit PA limit. */
enum riscv_sv39_status riscv_sv39_page_table_init(
    struct riscv_sv39_page_table *table,
    struct physical_page_allocator *allocator);

/*
 * Build mappings before activation, while every page-table physical address is
 * directly accessible.  A failure may leave earlier leaves and intermediate
 * tables installed; the caller must then abandon this page table and must not
 * activate it.
 */
enum riscv_sv39_status riscv_sv39_map_range(
    struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t size,
    uint32_t permissions);

enum riscv_sv39_status riscv_sv39_activate(
    const struct riscv_sv39_page_table *table);

uint64_t riscv_sv39_current_satp(void);

#endif
