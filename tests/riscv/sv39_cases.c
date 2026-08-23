#include <arch/riscv/sv39.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>

static unsigned char page_pool[BOAROS_PAGE_SIZE * 8U]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char scale_page_pool[BOAROS_PAGE_SIZE * 32U]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char mixed_page_pool[BOAROS_PAGE_SIZE * 8U]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char exhausted_page_pool[BOAROS_PAGE_SIZE * 2U]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

static int init_allocator(struct physical_page_allocator *allocator)
{
    struct boot_memory_layout layout;

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)page_pool;
    layout.usable[0].size = sizeof(page_pool);
    return physical_page_allocator_init(allocator, &layout) ==
           PHYSICAL_PAGE_STATUS_OK;
}

int run_sv39_tests(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table table;
    struct boot_memory_layout scale_layout;
    uint64_t table_pages;
    uint64_t leaf_4k;
    uint64_t leaf_2m;
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t root_entry;
    uint64_t level1_entry;

    if (!init_allocator(&allocator) ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 1;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x40000000),
                             UINT64_C(0x80000000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ | RISCV_SV39_WRITE) !=
        RISCV_SV39_STATUS_OK) {
        return 2;
    }

    root = (uint64_t *)(uintptr_t)table.root_address;
    root_entry = root[1];
    if ((root_entry & UINT64_C(0x3ff)) != UINT64_C(0x1)) {
        return 3;
    }

    level1 = (uint64_t *)(uintptr_t)((root_entry >> 10U) << 12U);
    if (level1[0] != UINT64_C(0x200000c7)) {
        return 4;
    }
    if (table.table_pages != 2U || table.leaf_2m != 1U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 6U) {
        return 5;
    }

    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x40200000),
                             UINT64_C(0x90001000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ | RISCV_SV39_EXECUTE) !=
        RISCV_SV39_STATUS_OK) {
        return 6;
    }

    level1_entry = level1[1];
    if ((level1_entry & UINT64_C(0x3ff)) != UINT64_C(0x1)) {
        return 7;
    }
    level0 = (uint64_t *)(uintptr_t)((level1_entry >> 10U) << 12U);
    if (level0[0] != UINT64_C(0x2400044b)) {
        return 8;
    }
    if (table.table_pages != 3U || table.leaf_2m != 1U ||
        table.leaf_4k != 1U || physical_page_available(&allocator) != 5U) {
        return 9;
    }

    scale_layout.usable_count = 1U;
    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)scale_page_pool;
    scale_layout.usable[0].size = sizeof(scale_page_pool);
    if (physical_page_allocator_init(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x80000000),
                             UINT64_C(0x80000000),
                             UINT64_C(0x400000000),
                             RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK) {
        return 10;
    }
    if (table.table_pages != 17U || table.leaf_2m != 8192U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 15U) {
        return 11;
    }

    table_pages = table.table_pages;
    leaf_4k = table.leaf_4k;
    leaf_2m = table.leaf_2m;
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x80000000),
                             UINT64_C(0x80000000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_CONFLICT ||
        table.table_pages != table_pages || table.leaf_4k != leaf_4k ||
        table.leaf_2m != leaf_2m) {
        return 12;
    }
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x4000000000),
                             UINT64_C(0x10000000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x20000000),
                             UINT64_C(0x10000000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_INVALID ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x20000001),
                             UINT64_C(0x10000000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_INVALID) {
        return 13;
    }

    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)mixed_page_pool;
    scale_layout.usable[0].size = sizeof(mixed_page_pool);
    if (physical_page_allocator_init(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x1000),
                             UINT64_C(0x80001000),
                             UINT64_C(0x400000),
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK) {
        return 14;
    }
    if (table.table_pages != 4U || table.leaf_2m != 1U ||
        table.leaf_4k != 512U || physical_page_available(&allocator) != 4U) {
        return 15;
    }

    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)exhausted_page_pool;
    scale_layout.usable[0].size = sizeof(exhausted_page_pool);
    if (physical_page_allocator_init(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 16;
    }
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x1000),
                             RISCV_SV39_PAGE_SIZE_4K,
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_NO_MEMORY ||
        table.table_pages != 2U || table.leaf_2m != 0U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 0U) {
        return 17;
    }

    scale_layout.usable_count = 2U;
    scale_layout.usable[0].base = (uint64_t)(uintptr_t)page_pool;
    scale_layout.usable[0].size = BOAROS_PAGE_SIZE;
    scale_layout.usable[1].base = UINT64_C(1) << 56U;
    scale_layout.usable[1].size = BOAROS_PAGE_SIZE;
    if (physical_page_allocator_init(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_INVALID ||
        physical_page_available(&allocator) != 2U) {
        return 18;
    }

    scale_layout.usable_count = 1U;
    scale_layout.usable[0].base =
        (uint64_t)(uintptr_t)exhausted_page_pool;
    scale_layout.usable[0].size = sizeof(exhausted_page_pool);
    if (physical_page_allocator_init(&allocator, &scale_layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x0),
                             UINT64_C(0x40200000),
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_NO_MEMORY ||
        table.table_pages != 2U || table.leaf_2m != 512U ||
        table.leaf_4k != 0U || physical_page_available(&allocator) != 0U) {
        return 19;
    }

    if (!init_allocator(&allocator) ||
        riscv_sv39_page_table_init(&table, &allocator) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_map_range(&table,
                             UINT64_C(0x200000),
                             UINT64_C(0x200000),
                             RISCV_SV39_PAGE_SIZE_2M,
                             RISCV_SV39_READ) != RISCV_SV39_STATUS_OK) {
        return 20;
    }
    if (riscv_sv39_map_range(&table,
                             UINT64_C(0x0),
                             UINT64_C(0x400000),
                             UINT64_C(0x400000),
                             RISCV_SV39_READ) !=
            RISCV_SV39_STATUS_CONFLICT ||
        table.table_pages != 2U || table.leaf_2m != 2U ||
        table.leaf_4k != 0U) {
        return 21;
    }
    root = (uint64_t *)(uintptr_t)table.root_address;
    level1 = (uint64_t *)(uintptr_t)((root[0] >> 10U) << 12U);
    if (level1[0] != UINT64_C(0x100043) ||
        level1[1] != UINT64_C(0x80043)) {
        return 22;
    }

    return 0;
}
