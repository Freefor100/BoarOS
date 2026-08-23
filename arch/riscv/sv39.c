#include <arch/riscv/sv39.h>
#include <kernel/page.h>

#include <stdint.h>

#define RISCV_SV39_PTE_VALID UINT64_C(0x001)
#define RISCV_SV39_PTE_READ UINT64_C(0x002)
#define RISCV_SV39_PTE_WRITE UINT64_C(0x004)
#define RISCV_SV39_PTE_EXECUTE UINT64_C(0x008)
#define RISCV_SV39_PTE_ACCESSED UINT64_C(0x040)
#define RISCV_SV39_PTE_DIRTY UINT64_C(0x080)
#define RISCV_SV39_PTE_FLAGS_MASK UINT64_C(0x3ff)
#define RISCV_SV39_INDEX_MASK UINT64_C(0x1ff)
#define RISCV_SV39_PHYSICAL_BITS 56U
#define RISCV_SV39_LOW_MAX ((UINT64_C(1) << 38U) - UINT64_C(1))
#define RISCV_SV39_HIGH_MIN (~RISCV_SV39_LOW_MAX)
#define RISCV_SV39_PHYSICAL_LIMIT \
    (UINT64_C(1) << RISCV_SV39_PHYSICAL_BITS)
#define RISCV_SV39_PHYSICAL_MAX (RISCV_SV39_PHYSICAL_LIMIT - UINT64_C(1))
#define RISCV_SV39_SATP_MODE (UINT64_C(8) << 60U)
#define RISCV_SV39_SATP_PPN_MASK ((UINT64_C(1) << 44U) - UINT64_C(1))

static int canonical_virtual_address(uint64_t address)
{
    uint64_t upper = address >> 39U;
    uint64_t sign = (address >> 38U) & UINT64_C(1);

    return sign == 0U ? upper == 0U : upper == UINT64_C(0x1ffffff);
}

static int valid_permissions(uint32_t permissions)
{
    uint32_t known = RISCV_SV39_READ |
                     RISCV_SV39_WRITE |
                     RISCV_SV39_EXECUTE;

    if ((permissions & ~known) != 0U ||
        (permissions & known) == 0U) {
        return 0;
    }
    if ((permissions & RISCV_SV39_WRITE) != 0U &&
        (permissions & RISCV_SV39_READ) == 0U) {
        return 0;
    }

    return 1;
}

static int valid_virtual_range(uint64_t address, uint64_t size)
{
    uint64_t last;

    if (size == 0U || size - 1U > UINT64_MAX - address) {
        return 0;
    }
    last = address + size - 1U;

    if (address <= RISCV_SV39_LOW_MAX) {
        return last <= RISCV_SV39_LOW_MAX;
    }

    return address >= RISCV_SV39_HIGH_MIN &&
           last >= RISCV_SV39_HIGH_MIN;
}

static uint64_t table_entry(uint64_t address)
{
    return ((address >> BOAROS_PAGE_SHIFT) << 10U) |
           RISCV_SV39_PTE_VALID;
}

static uint64_t leaf_entry(uint64_t address, uint32_t permissions)
{
    uint64_t result = ((address >> BOAROS_PAGE_SHIFT) << 10U) |
                      RISCV_SV39_PTE_VALID |
                      RISCV_SV39_PTE_ACCESSED;

    if ((permissions & RISCV_SV39_READ) != 0U) {
        result |= RISCV_SV39_PTE_READ;
    }
    if ((permissions & RISCV_SV39_WRITE) != 0U) {
        result |= RISCV_SV39_PTE_WRITE | RISCV_SV39_PTE_DIRTY;
    }
    if ((permissions & RISCV_SV39_EXECUTE) != 0U) {
        result |= RISCV_SV39_PTE_EXECUTE;
    }

    return result;
}

static void clear_table(uint64_t address)
{
    uint64_t *entries = (uint64_t *)(uintptr_t)address;
    uint32_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE / sizeof(*entries); index++) {
        entries[index] = 0U;
    }
}

static int allocator_tables_representable(
    const struct physical_page_allocator *allocator)
{
    uint32_t index;

    if (physical_page_available(allocator) == 0U) {
        return 1;
    }
    for (index = 0U; index < allocator->range_count; index++) {
        if (allocator->ranges[index].end > RISCV_SV39_PHYSICAL_LIMIT) {
            return 0;
        }
    }

    return 1;
}

static enum riscv_sv39_status allocate_table_page(
    struct riscv_sv39_page_table *table,
    uint64_t *address)
{
    enum physical_page_status status;

    if (!allocator_tables_representable(table->allocator)) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = physical_page_allocate(table->allocator, address);

    if (status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }

    clear_table(*address);
    table->table_pages++;
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_page_table_init(
    struct riscv_sv39_page_table *table,
    struct physical_page_allocator *allocator)
{
    struct riscv_sv39_page_table result;
    enum physical_page_status status;
    uint64_t root_address;

    if (table == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (table->state != RISCV_SV39_STATE_UNINITIALIZED) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (allocator == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }

    if (!allocator_tables_representable(allocator)) {
        return RISCV_SV39_STATUS_INVALID;
    }

    status = physical_page_allocate(allocator, &root_address);
    if (status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }

    clear_table(root_address);
    result.allocator = allocator;
    result.root_address = root_address;
    result.table_pages = 1U;
    result.leaf_4k = 0U;
    result.leaf_2m = 0U;
    result.state = RISCV_SV39_STATE_BUILDING;
    *table = result;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status map_leaf(
    struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t size,
    uint32_t permissions)
{
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t *root_entry;
    uint64_t *leaf;
    uint64_t next_address;
    enum riscv_sv39_status status;

    root = (uint64_t *)(uintptr_t)table->root_address;
    root_entry = &root[(virtual_address >> 30U) & RISCV_SV39_INDEX_MASK];
    if (*root_entry == 0U) {
        status = allocate_table_page(table, &next_address);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
        *root_entry = table_entry(next_address);
    } else if ((*root_entry & RISCV_SV39_PTE_FLAGS_MASK) !=
               RISCV_SV39_PTE_VALID) {
        return RISCV_SV39_STATUS_CONFLICT;
    }

    level1 = (uint64_t *)(uintptr_t)((*root_entry >> 10U) <<
                                     BOAROS_PAGE_SHIFT);
    leaf = &level1[(virtual_address >> 21U) & RISCV_SV39_INDEX_MASK];
    if (size == RISCV_SV39_PAGE_SIZE_2M) {
        if (*leaf != 0U) {
            return RISCV_SV39_STATUS_CONFLICT;
        }

        *leaf = leaf_entry(physical_address, permissions);
        table->leaf_2m++;
        return RISCV_SV39_STATUS_OK;
    }

    if (*leaf == 0U) {
        status = allocate_table_page(table, &next_address);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
        *leaf = table_entry(next_address);
    } else if ((*leaf & RISCV_SV39_PTE_FLAGS_MASK) !=
               RISCV_SV39_PTE_VALID) {
        return RISCV_SV39_STATUS_CONFLICT;
    }

    level0 = (uint64_t *)(uintptr_t)((*leaf >> 10U) <<
                                     BOAROS_PAGE_SHIFT);
    leaf = &level0[(virtual_address >> 12U) & RISCV_SV39_INDEX_MASK];
    if (*leaf != 0U) {
        return RISCV_SV39_STATUS_CONFLICT;
    }
    *leaf = leaf_entry(physical_address, permissions);
    table->leaf_4k++;
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_map_range(
    struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t size,
    uint32_t permissions)
{
    uint64_t remaining = size;

    if (table == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (table->state != RISCV_SV39_STATE_BUILDING) {
        return RISCV_SV39_STATUS_STATE;
    }
    if ((virtual_address & (RISCV_SV39_PAGE_SIZE_4K - 1U)) != 0U ||
        (physical_address & (RISCV_SV39_PAGE_SIZE_4K - 1U)) != 0U ||
        (size & (RISCV_SV39_PAGE_SIZE_4K - 1U)) != 0U ||
        !canonical_virtual_address(virtual_address) ||
        !valid_virtual_range(virtual_address, size) ||
        !valid_permissions(permissions) ||
        physical_address > RISCV_SV39_PHYSICAL_MAX ||
        size == 0U ||
        size - 1U > RISCV_SV39_PHYSICAL_MAX - physical_address) {
        return RISCV_SV39_STATUS_INVALID;
    }

    while (remaining != 0U) {
        uint64_t leaf_size = RISCV_SV39_PAGE_SIZE_4K;
        enum riscv_sv39_status status;

        if (remaining >= RISCV_SV39_PAGE_SIZE_2M &&
            (virtual_address & (RISCV_SV39_PAGE_SIZE_2M - 1U)) == 0U &&
            (physical_address & (RISCV_SV39_PAGE_SIZE_2M - 1U)) == 0U) {
            leaf_size = RISCV_SV39_PAGE_SIZE_2M;
        }

        status = map_leaf(table,
                          virtual_address,
                          physical_address,
                          leaf_size,
                          permissions);
        if (status != RISCV_SV39_STATUS_OK) {
            table->state = RISCV_SV39_STATE_FAILED;
            return status;
        }

        virtual_address += leaf_size;
        physical_address += leaf_size;
        remaining -= leaf_size;
    }

    return RISCV_SV39_STATUS_OK;
}

uint64_t riscv_sv39_current_satp(void)
{
    uint64_t value;

    __asm__ volatile("csrr %0, satp" : "=r"(value));
    return value;
}

static void switch_satp(uint64_t value)
{
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
    __asm__ volatile("csrw satp, %0" : : "r"(value) : "memory");
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

enum riscv_sv39_status riscv_sv39_activate(
    struct riscv_sv39_page_table *table)
{
    uint64_t previous_satp;
    uint64_t root_ppn;
    uint64_t satp;

    if (table == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (table->state != RISCV_SV39_STATE_BUILDING) {
        return RISCV_SV39_STATUS_STATE;
    }
    if ((table->root_address & BOAROS_PAGE_MASK) != 0U) {
        table->state = RISCV_SV39_STATE_FAILED;
        return RISCV_SV39_STATUS_INVALID;
    }

    root_ppn = table->root_address >> BOAROS_PAGE_SHIFT;
    if ((root_ppn & ~RISCV_SV39_SATP_PPN_MASK) != 0U) {
        table->state = RISCV_SV39_STATE_FAILED;
        return RISCV_SV39_STATUS_INVALID;
    }
    satp = RISCV_SV39_SATP_MODE | root_ppn;

    previous_satp = riscv_sv39_current_satp();
    table->state = RISCV_SV39_STATE_ACTIVE;
    switch_satp(satp);
    if (riscv_sv39_current_satp() != satp) {
        switch_satp(previous_satp);
        table->state = RISCV_SV39_STATE_FAILED;
        return RISCV_SV39_STATUS_INVALID;
    }

    return RISCV_SV39_STATUS_OK;
}
