#ifndef BOAROS_ARCH_RISCV_DIRECT_MAP_H
#define BOAROS_ARCH_RISCV_DIRECT_MAP_H

#include <stdint.h>

enum riscv_direct_map_status {
    RISCV_DIRECT_MAP_STATUS_OK = 0,
    RISCV_DIRECT_MAP_STATUS_INVALID,
};

/* Convert a non-empty range fully contained in the fixed direct-map window. */
enum riscv_direct_map_status riscv_direct_map_pa_to_va(
    uint64_t physical_address,
    uint64_t size,
    uint64_t *virtual_address);

enum riscv_direct_map_status riscv_direct_map_va_to_pa(
    uint64_t virtual_address,
    uint64_t size,
    uint64_t *physical_address);

#endif
