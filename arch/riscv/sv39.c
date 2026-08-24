#include <arch/riscv/sv39.h>
#include <kernel/page.h>

#include <stdint.h>

#define RISCV_SV39_PTE_VALID UINT64_C(0x001)
#define RISCV_SV39_PTE_READ UINT64_C(0x002)
#define RISCV_SV39_PTE_WRITE UINT64_C(0x004)
#define RISCV_SV39_PTE_EXECUTE UINT64_C(0x008)
#define RISCV_SV39_PTE_USER UINT64_C(0x010)
#define RISCV_SV39_PTE_ACCESSED UINT64_C(0x040)
#define RISCV_SV39_PTE_DIRTY UINT64_C(0x080)
#define RISCV_SV39_PTE_FLAGS_MASK UINT64_C(0x3ff)
#define RISCV_SV39_PTE_ALLOWED_MASK ((UINT64_C(1) << 54U) - UINT64_C(1))
#define RISCV_SV39_INDEX_MASK UINT64_C(0x1ff)
#define RISCV_SV39_USER_ROOT_ENTRIES 256U
#define RISCV_SV39_USER_LIMIT (UINT64_C(1) << 38U)
#define RISCV_SV39_PHYSICAL_BITS 56U
#define RISCV_SV39_LOW_MAX ((UINT64_C(1) << 38U) - UINT64_C(1))
#define RISCV_SV39_HIGH_MIN (~RISCV_SV39_LOW_MAX)
#define RISCV_SV39_PHYSICAL_LIMIT \
    (UINT64_C(1) << RISCV_SV39_PHYSICAL_BITS)
#define RISCV_SV39_PHYSICAL_MAX (RISCV_SV39_PHYSICAL_LIMIT - UINT64_C(1))
#define RISCV_SV39_SATP_MODE (UINT64_C(8) << 60U)
#define RISCV_SV39_SATP_MODE_MASK (UINT64_C(0xf) << 60U)
#define RISCV_SV39_SATP_ASID_MASK (UINT64_C(0xffff) << 44U)
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

static void write_satp(uint64_t value)
{
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
    __asm__ volatile("csrw satp, %0" : : "r"(value) : "memory");
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

enum riscv_sv39_status riscv_sv39_switch_satp(uint64_t satp)
{
    uint64_t mode = satp & RISCV_SV39_SATP_MODE_MASK;
    uint64_t previous;

    if ((mode == 0U && satp != 0U) ||
        (mode != 0U && mode != RISCV_SV39_SATP_MODE) ||
        (satp & RISCV_SV39_SATP_ASID_MASK) != 0U) {
        return RISCV_SV39_STATUS_INVALID;
    }

    previous = riscv_sv39_current_satp();
    if (previous == satp) {
        return RISCV_SV39_STATUS_OK;
    }
    write_satp(satp);
    if (riscv_sv39_current_satp() != satp) {
        write_satp(previous);
        return RISCV_SV39_STATUS_INVALID;
    }
    return RISCV_SV39_STATUS_OK;
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
    if (riscv_sv39_switch_satp(satp) != RISCV_SV39_STATUS_OK) {
        (void)riscv_sv39_switch_satp(previous_satp);
        table->state = RISCV_SV39_STATE_FAILED;
        return RISCV_SV39_STATUS_INVALID;
    }

    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status resolve_runtime_table(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint64_t **entries)
{
    void *pointer;
    enum physical_page_status status;

    if (entries == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = physical_page_resolve(allocator, address, &pointer);
    if (status == PHYSICAL_PAGE_STATUS_STATE) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }

    *entries = pointer;
    return RISCV_SV39_STATUS_OK;
}

static void clear_runtime_table(uint64_t *entries)
{
    uint32_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE / sizeof(*entries); index++) {
        entries[index] = 0U;
    }
}

static enum riscv_sv39_status allocate_runtime_table(
    struct riscv_sv39_user_space *space,
    uint64_t *address,
    uint64_t **entries)
{
    enum physical_page_status page_status;
    enum riscv_sv39_status status;

    page_status = physical_page_allocate(space->allocator, address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status == PHYSICAL_PAGE_STATUS_STATE
                   ? RISCV_SV39_STATUS_STATE
                   : RISCV_SV39_STATUS_INVALID;
    }

    status = resolve_runtime_table(space->allocator, *address, entries);
    if (status != RISCV_SV39_STATUS_OK) {
        if (physical_page_release(space->allocator, *address) !=
            PHYSICAL_PAGE_STATUS_OK) {
            return RISCV_SV39_STATUS_STATE;
        }
        return status;
    }
    clear_runtime_table(*entries);
    space->table_pages++;
    return RISCV_SV39_STATUS_OK;
}

static int valid_nonleaf_entry(uint64_t entry)
{
    return (entry & ~RISCV_SV39_PTE_ALLOWED_MASK) == 0U &&
           (entry & RISCV_SV39_PTE_FLAGS_MASK) == RISCV_SV39_PTE_VALID;
}

static int valid_user_leaf_entry(uint64_t entry)
{
    uint64_t rwx = entry & (RISCV_SV39_PTE_READ |
                            RISCV_SV39_PTE_WRITE |
                            RISCV_SV39_PTE_EXECUTE);

    return (entry & ~RISCV_SV39_PTE_ALLOWED_MASK) == 0U &&
           (entry & RISCV_SV39_PTE_VALID) != 0U &&
           (entry & RISCV_SV39_PTE_USER) != 0U && rwx != 0U &&
           !((entry & RISCV_SV39_PTE_WRITE) != 0U &&
             (entry & RISCV_SV39_PTE_READ) == 0U);
}

static uint64_t entry_address(uint64_t entry)
{
    return (entry >> 10U) << BOAROS_PAGE_SHIFT;
}

static int valid_user_permissions(uint32_t permissions)
{
    uint32_t known = RISCV_SV39_READ |
                     RISCV_SV39_WRITE |
                     RISCV_SV39_EXECUTE;

    return (permissions & ~known) == 0U &&
           (permissions & known) != 0U &&
           !((permissions & RISCV_SV39_WRITE) != 0U &&
             (permissions & RISCV_SV39_READ) == 0U);
}

static uint64_t user_leaf_entry(uint64_t address, uint32_t permissions)
{
    return leaf_entry(address, permissions) | RISCV_SV39_PTE_USER;
}

enum riscv_sv39_status riscv_sv39_user_space_init(
    struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table)
{
    struct riscv_sv39_user_space result;
    uint64_t *kernel_root;
    uint64_t *user_root;
    uint64_t root_address;
    uint32_t index;
    enum physical_page_status page_status;
    enum riscv_sv39_status status;

    if (space == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_EMPTY) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (allocator == 0 || kernel_table == 0 ||
        kernel_table->state != RISCV_SV39_STATE_ACTIVE ||
        kernel_table->allocator != allocator ||
        !allocator_tables_representable(allocator)) {
        return RISCV_SV39_STATUS_INVALID;
    }

    status = resolve_runtime_table(allocator,
                                   kernel_table->root_address,
                                   &kernel_root);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    page_status = physical_page_allocate(allocator, &root_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status == PHYSICAL_PAGE_STATUS_STATE
                   ? RISCV_SV39_STATUS_STATE
                   : RISCV_SV39_STATUS_INVALID;
    }
    status = resolve_runtime_table(allocator, root_address, &user_root);
    if (status != RISCV_SV39_STATUS_OK) {
        if (physical_page_release(allocator, root_address) !=
            PHYSICAL_PAGE_STATUS_OK) {
            return RISCV_SV39_STATUS_STATE;
        }
        return status;
    }

    clear_runtime_table(user_root);
    for (index = RISCV_SV39_USER_ROOT_ENTRIES;
         index < BOAROS_PAGE_SIZE / sizeof(*user_root);
         index++) {
        user_root[index] = kernel_root[index];
    }

    result.allocator = allocator;
    result.root_address = root_address;
    result.table_pages = 1U;
    result.leaf_pages = 0U;
    result.state = RISCV_SV39_USER_SPACE_LIVE;
    *space = result;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status rollback_runtime_table(
    struct riscv_sv39_user_space *space,
    uint64_t *parent_entry,
    uint64_t address)
{
    uint64_t saved_entry = *parent_entry;

    *parent_entry = 0U;
    if (physical_page_release(space->allocator, address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        *parent_entry = saved_entry;
        return RISCV_SV39_STATUS_STATE;
    }
    if (space->table_pages <= 1U) {
        return RISCV_SV39_STATUS_STATE;
    }
    space->table_pages--;
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_map_owned_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions)
{
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t *root_entry;
    uint64_t *level1_entry;
    uint64_t *leaf;
    uint64_t level1_address;
    uint64_t level0_address;
    void *owned_page;
    int allocated_level1 = 0;
    enum riscv_sv39_status status;

    if (space == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (virtual_address < RISCV_SV39_PAGE_SIZE_4K ||
        virtual_address >= RISCV_SV39_USER_LIMIT ||
        (virtual_address & (RISCV_SV39_PAGE_SIZE_4K - 1U)) != 0U ||
        (physical_address & (RISCV_SV39_PAGE_SIZE_4K - 1U)) != 0U ||
        physical_address > RISCV_SV39_PHYSICAL_MAX -
                               (RISCV_SV39_PAGE_SIZE_4K - 1U) ||
        !valid_user_permissions(permissions)) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (physical_page_resolve(space->allocator,
                              physical_address,
                              &owned_page) != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    (void)owned_page;

    status = resolve_runtime_table(space->allocator,
                                   space->root_address,
                                   &root);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    root_entry = &root[(virtual_address >> 30U) & RISCV_SV39_INDEX_MASK];
    if (*root_entry == 0U) {
        status = allocate_runtime_table(space,
                                        &level1_address,
                                        &level1);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
        *root_entry = table_entry(level1_address);
        allocated_level1 = 1;
    } else {
        if (!valid_nonleaf_entry(*root_entry)) {
            return RISCV_SV39_STATUS_CONFLICT;
        }
        level1_address = entry_address(*root_entry);
        status = resolve_runtime_table(space->allocator,
                                       level1_address,
                                       &level1);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
    }

    level1_entry = &level1[(virtual_address >> 21U) &
                           RISCV_SV39_INDEX_MASK];
    if (*level1_entry == 0U) {
        status = allocate_runtime_table(space,
                                        &level0_address,
                                        &level0);
        if (status != RISCV_SV39_STATUS_OK) {
            if (allocated_level1 != 0 &&
                rollback_runtime_table(space,
                                       root_entry,
                                       level1_address) !=
                    RISCV_SV39_STATUS_OK) {
                return RISCV_SV39_STATUS_STATE;
            }
            return status;
        }
        *level1_entry = table_entry(level0_address);
    } else {
        if (!valid_nonleaf_entry(*level1_entry)) {
            return RISCV_SV39_STATUS_CONFLICT;
        }
        level0_address = entry_address(*level1_entry);
        status = resolve_runtime_table(space->allocator,
                                       level0_address,
                                       &level0);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
    }

    leaf = &level0[(virtual_address >> BOAROS_PAGE_SHIFT) &
                   RISCV_SV39_INDEX_MASK];
    if (*leaf != 0U) {
        return RISCV_SV39_STATUS_CONFLICT;
    }
    *leaf = user_leaf_entry(physical_address, permissions);
    space->leaf_pages++;
    return RISCV_SV39_STATUS_OK;
}

static uint32_t entry_permissions(uint64_t entry)
{
    uint32_t permissions = 0U;

    if ((entry & RISCV_SV39_PTE_READ) != 0U) {
        permissions |= RISCV_SV39_READ;
    }
    if ((entry & RISCV_SV39_PTE_WRITE) != 0U) {
        permissions |= RISCV_SV39_WRITE;
    }
    if ((entry & RISCV_SV39_PTE_EXECUTE) != 0U) {
        permissions |= RISCV_SV39_EXECUTE;
    }
    if ((entry & RISCV_SV39_PTE_USER) != 0U) {
        permissions |= RISCV_SV39_USER;
    }
    return permissions;
}

enum riscv_sv39_status riscv_sv39_user_lookup(
    const struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    struct riscv_sv39_mapping *mapping)
{
    struct riscv_sv39_mapping result;
    uint64_t *entries;
    uint64_t entry;
    uint32_t shifts[3] = {30U, 21U, BOAROS_PAGE_SHIFT};
    uint32_t level;
    enum riscv_sv39_status status;

    if (space == 0 || mapping == 0 ||
        virtual_address >= RISCV_SV39_USER_LIMIT) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = resolve_runtime_table(space->allocator,
                                   space->root_address,
                                   &entries);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    for (level = 0U; level < 3U; level++) {
        entry = entries[(virtual_address >> shifts[level]) &
                        RISCV_SV39_INDEX_MASK];
        if (entry == 0U) {
            return RISCV_SV39_STATUS_NOT_MAPPED;
        }
        if (level == 2U) {
            if (!valid_user_leaf_entry(entry)) {
                return RISCV_SV39_STATUS_STATE;
            }
            result.physical_address =
                entry_address(entry) |
                (virtual_address & (RISCV_SV39_PAGE_SIZE_4K - 1U));
            result.permissions = entry_permissions(entry);
            *mapping = result;
            return RISCV_SV39_STATUS_OK;
        }
        if (!valid_nonleaf_entry(entry)) {
            return RISCV_SV39_STATUS_STATE;
        }
        status = resolve_runtime_table(space->allocator,
                                       entry_address(entry),
                                       &entries);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
    }

    return RISCV_SV39_STATUS_STATE;
}

enum riscv_sv39_status riscv_sv39_user_space_satp(
    const struct riscv_sv39_user_space *space,
    uint64_t *satp)
{
    uint64_t root_ppn;
    uint64_t result;

    if (space == 0 || satp == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    if ((space->root_address & BOAROS_PAGE_MASK) != 0U) {
        return RISCV_SV39_STATUS_INVALID;
    }
    root_ppn = space->root_address >> BOAROS_PAGE_SHIFT;
    if ((root_ppn & ~RISCV_SV39_SATP_PPN_MASK) != 0U) {
        return RISCV_SV39_STATUS_INVALID;
    }

    result = RISCV_SV39_SATP_MODE | root_ppn;
    *satp = result;
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_space_move(
    struct riscv_sv39_user_space *destination,
    struct riscv_sv39_user_space *source)
{
    if (destination == 0 || source == 0 || destination == source) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (destination->state != RISCV_SV39_USER_SPACE_EMPTY ||
        source->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }

    *destination = *source;
    source->allocator = 0;
    source->root_address = 0U;
    source->table_pages = 0U;
    source->leaf_pages = 0U;
    source->state = RISCV_SV39_USER_SPACE_MOVED;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status release_leaf(
    struct riscv_sv39_user_space *space,
    uint64_t *entry)
{
    uint64_t saved_entry = *entry;

    if (!valid_user_leaf_entry(saved_entry) ||
        space->leaf_pages == 0U) {
        return RISCV_SV39_STATUS_STATE;
    }
    *entry = 0U;
    if (physical_page_release(space->allocator,
                              entry_address(saved_entry)) !=
        PHYSICAL_PAGE_STATUS_OK) {
        *entry = saved_entry;
        return RISCV_SV39_STATUS_STATE;
    }
    space->leaf_pages--;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status release_child_table(
    struct riscv_sv39_user_space *space,
    uint64_t *entry)
{
    uint64_t saved_entry = *entry;

    if (!valid_nonleaf_entry(saved_entry) || space->table_pages <= 1U) {
        return RISCV_SV39_STATUS_STATE;
    }
    *entry = 0U;
    if (physical_page_release(space->allocator,
                              entry_address(saved_entry)) !=
        PHYSICAL_PAGE_STATUS_OK) {
        *entry = saved_entry;
        return RISCV_SV39_STATUS_STATE;
    }
    space->table_pages--;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status destroy_level0(
    struct riscv_sv39_user_space *space,
    uint64_t *parent_entry)
{
    uint64_t *entries;
    uint32_t index;
    enum riscv_sv39_status status;

    if (!valid_nonleaf_entry(*parent_entry)) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = resolve_runtime_table(space->allocator,
                                   entry_address(*parent_entry),
                                   &entries);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    for (index = 0U; index < BOAROS_PAGE_SIZE / sizeof(*entries); index++) {
        if (entries[index] == 0U) {
            continue;
        }
        status = release_leaf(space, &entries[index]);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
    }
    return release_child_table(space, parent_entry);
}

static enum riscv_sv39_status destroy_level1(
    struct riscv_sv39_user_space *space,
    uint64_t *parent_entry)
{
    uint64_t *entries;
    uint32_t index;
    enum riscv_sv39_status status;

    if (!valid_nonleaf_entry(*parent_entry)) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = resolve_runtime_table(space->allocator,
                                   entry_address(*parent_entry),
                                   &entries);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    for (index = 0U; index < BOAROS_PAGE_SIZE / sizeof(*entries); index++) {
        if (entries[index] == 0U) {
            continue;
        }
        status = destroy_level0(space, &entries[index]);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
    }
    return release_child_table(space, parent_entry);
}

enum riscv_sv39_status riscv_sv39_user_space_destroy(
    struct riscv_sv39_user_space *space)
{
    uint64_t *root;
    uint64_t satp;
    uint64_t root_ppn;
    uint32_t index;
    enum riscv_sv39_status status;

    if (space == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    satp = riscv_sv39_current_satp();
    root_ppn = space->root_address >> BOAROS_PAGE_SHIFT;
    if ((satp >> 60U) == 8U &&
        (satp & RISCV_SV39_SATP_PPN_MASK) == root_ppn) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = resolve_runtime_table(space->allocator,
                                   space->root_address,
                                   &root);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    for (index = 0U; index < RISCV_SV39_USER_ROOT_ENTRIES; index++) {
        if (root[index] == 0U) {
            continue;
        }
        status = destroy_level1(space, &root[index]);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
    }
    if (space->leaf_pages != 0U || space->table_pages != 1U) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (physical_page_release(space->allocator,
                              space->root_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_STATE;
    }

    space->allocator = 0;
    space->root_address = 0U;
    space->table_pages = 0U;
    space->leaf_pages = 0U;
    space->state = RISCV_SV39_USER_SPACE_DESTROYED;
    return RISCV_SV39_STATUS_OK;
}
