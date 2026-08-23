#include <arch/riscv/direct_map.h>
#include <arch/riscv/memory_layout.h>

#include <stdint.h>

_Static_assert(RISCV_DIRECT_MAP_SIZE - 1U <=
                   UINT64_MAX - RISCV_DIRECT_MAP_BASE,
               "RISC-V direct map must not wrap");

static int range_within(uint64_t offset, uint64_t size, uint64_t limit)
{
    return size != 0U && offset < limit && size <= limit - offset;
}

enum riscv_direct_map_status riscv_direct_map_pa_to_va(
    uint64_t physical_address,
    uint64_t size,
    uint64_t *virtual_address)
{
    if (virtual_address == 0 ||
        !range_within(physical_address, size, RISCV_DIRECT_MAP_SIZE)) {
        return RISCV_DIRECT_MAP_STATUS_INVALID;
    }

    *virtual_address = RISCV_DIRECT_MAP_BASE + physical_address;
    return RISCV_DIRECT_MAP_STATUS_OK;
}

enum riscv_direct_map_status riscv_direct_map_va_to_pa(
    uint64_t virtual_address,
    uint64_t size,
    uint64_t *physical_address)
{
    uint64_t offset;

    if (physical_address == 0 || virtual_address < RISCV_DIRECT_MAP_BASE) {
        return RISCV_DIRECT_MAP_STATUS_INVALID;
    }
    offset = virtual_address - RISCV_DIRECT_MAP_BASE;
    if (!range_within(offset, size, RISCV_DIRECT_MAP_SIZE)) {
        return RISCV_DIRECT_MAP_STATUS_INVALID;
    }

    *physical_address = offset;
    return RISCV_DIRECT_MAP_STATUS_OK;
}
