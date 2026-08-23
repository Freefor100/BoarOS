#include <arch/riscv/direct_map.h>
#include <kernel/page.h>

#include <stdint.h>

int run_direct_map_tests(void)
{
    uint64_t address;

    address = UINT64_C(0x1122334455667788);
    if (riscv_direct_map_pa_to_va(UINT64_C(0x80000000),
                                  UINT64_C(0x200000),
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_OK ||
        address != UINT64_C(0xffffffc080000000)) {
        return 1;
    }

    if (riscv_direct_map_pa_to_va(UINT64_C(0x1ffffff000),
                                  BOAROS_PAGE_SIZE,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_OK ||
        address != UINT64_C(0xffffffdffffff000)) {
        return 2;
    }

    if (riscv_direct_map_va_to_pa(UINT64_C(0xffffffc080000000),
                                  UINT64_C(0x200000),
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_OK ||
        address != UINT64_C(0x80000000)) {
        return 3;
    }

    if (riscv_direct_map_va_to_pa(UINT64_C(0xffffffdffffff000),
                                  BOAROS_PAGE_SIZE,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_OK ||
        address != UINT64_C(0x1ffffff000)) {
        return 4;
    }

    address = UINT64_C(0x1122334455667788);
    if (riscv_direct_map_pa_to_va(UINT64_C(0x80000000), 0U, &address) !=
            RISCV_DIRECT_MAP_STATUS_INVALID ||
        address != UINT64_C(0x1122334455667788)) {
        return 5;
    }
    if (riscv_direct_map_pa_to_va(UINT64_C(0x1ffffff000),
                                  BOAROS_PAGE_SIZE + 1U,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_INVALID ||
        address != UINT64_C(0x1122334455667788)) {
        return 6;
    }
    if (riscv_direct_map_pa_to_va(UINT64_C(0x2000000000),
                                  BOAROS_PAGE_SIZE,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_INVALID ||
        address != UINT64_C(0x1122334455667788) ||
        riscv_direct_map_pa_to_va(0U, BOAROS_PAGE_SIZE, 0) !=
            RISCV_DIRECT_MAP_STATUS_INVALID) {
        return 7;
    }

    if (riscv_direct_map_va_to_pa(UINT64_C(0xffffffbfffffffff),
                                  1U,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_INVALID ||
        address != UINT64_C(0x1122334455667788)) {
        return 8;
    }
    if (riscv_direct_map_va_to_pa(UINT64_C(0xffffffe000000000),
                                  BOAROS_PAGE_SIZE,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_INVALID ||
        address != UINT64_C(0x1122334455667788)) {
        return 9;
    }
    if (riscv_direct_map_va_to_pa(UINT64_C(0xffffffdffffff000),
                                  BOAROS_PAGE_SIZE + 1U,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_INVALID ||
        address != UINT64_C(0x1122334455667788) ||
        riscv_direct_map_va_to_pa(UINT64_C(0xffffffc000000000),
                                  BOAROS_PAGE_SIZE,
                                  0) != RISCV_DIRECT_MAP_STATUS_INVALID) {
        return 10;
    }

    if (riscv_direct_map_pa_to_va(0U, BOAROS_PAGE_SIZE, &address) !=
            RISCV_DIRECT_MAP_STATUS_OK ||
        address != UINT64_C(0xffffffc000000000)) {
        return 11;
    }
    if (riscv_direct_map_va_to_pa(UINT64_C(0xffffffc000000000),
                                  BOAROS_PAGE_SIZE,
                                  &address) !=
            RISCV_DIRECT_MAP_STATUS_OK ||
        address != 0U) {
        return 12;
    }

    return 0;
}
