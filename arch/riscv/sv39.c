#include <kernel/cost.h>
#include <kernel/irq.h>
#include <arch/riscv/smp.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/mmu_context.h>
#include <arch/riscv/memory_layout.h>
#include <kernel/page.h>

#include <stdint.h>

unsigned riscv_mmu_svvptc;
unsigned riscv_mmu_cboz64;

#define RISCV_SV39_PTE_VALID UINT64_C(0x001)
#define RISCV_SV39_PTE_READ UINT64_C(0x002)
#define RISCV_SV39_PTE_WRITE UINT64_C(0x004)
#define RISCV_SV39_PTE_EXECUTE UINT64_C(0x008)
#define RISCV_SV39_PTE_USER UINT64_C(0x010)
#define RISCV_SV39_PTE_ACCESSED UINT64_C(0x040)
#define RISCV_SV39_PTE_DIRTY UINT64_C(0x080)
#define RISCV_SV39_PTE_COW UINT64_C(0x100)
#define RISCV_SV39_PTE_PROTECTED UINT64_C(0x200)
#define RISCV_SV39_PTE_PROTECTED_COW UINT64_C(0x300)
/* RSW[1] marks file provenance while valid. A PROT_NONE leaf uses that
 * bit to identify its software encoding, so its otherwise-clear R bit
 * preserves provenance while V is clear. */
#define RISCV_SV39_PTE_FILE RISCV_SV39_PTE_PROTECTED
#define RISCV_SV39_PTE_PROTECTED_FILE RISCV_SV39_PTE_READ
#define RISCV_SV39_PTE_FLAGS_MASK UINT64_C(0x3ff)
#define RISCV_SV39_PTE_ALLOWED_MASK ((UINT64_C(1) << 54U) - UINT64_C(1))
#define RISCV_SV39_INDEX_MASK UINT64_C(0x1ff)
#define RISCV_SV39_USER_ROOT_ENTRIES 256U
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

static void resident_peak(struct riscv_sv39_user_space *space)
{
    uint64_t pages = (uint64_t)space->leaf_pages + space->protected_pages;
    if (space->peak_resident_pages < pages) space->peak_resident_pages = pages;
}

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
    return riscv_mmu_context_read();
}

static void write_satp(uint64_t value)
{
    riscv_mmu_context_write(value);
    riscv_mmu_flush_all();
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
    status = physical_page_map_owned(allocator, address, &pointer);
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

enum riscv_sv39_status riscv_sv39_user_discard_owned_page(
    struct riscv_sv39_user_space *space,
    uint64_t physical_address)
{
    if (space == 0 || space->state != RISCV_SV39_USER_SPACE_LIVE ||
        space->allocator == 0 ||
        (physical_address & BOAROS_PAGE_MASK) != 0U) {
        return RISCV_SV39_STATUS_INVALID;
    }
    (void)physical_page_release(space->allocator, physical_address);
    return RISCV_SV39_STATUS_OK;
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
        (void)physical_page_release(space->allocator, *address);
        return status;
    }
    clear_runtime_table(*entries);
    space->table_pages++;
    return RISCV_SV39_STATUS_OK;
}

/*
 * Kernel stack window.  The empty subtree is reserved while the kernel
 * table is BUILDING so every user root copy inherits it; stack leaves are
 * inserted and removed through the two runtime entry points below.
 */
static struct {
    uint64_t level1_address;
    uint64_t base;
    uint64_t size;
    uint32_t active;
    uint16_t mapped_leaves[RISCV_SV39_INDEX_MASK + 1U];
} kernel_stack_window;
static struct kernel_raw_lock kernel_window_lock = {
    .rank = KERNEL_RAW_RANK_MM, .initialized = KERNEL_RAW_INITIALIZED,
};

enum riscv_sv39_status riscv_sv39_kernel_window_reserve(
    struct riscv_sv39_page_table *table,
    uint64_t virtual_address,
    uint64_t size)
{
    uint64_t *root;
    uint64_t *root_entry;
    uint64_t table_address;
    enum riscv_sv39_status status;

    if (table == 0 || table->state != RISCV_SV39_STATE_BUILDING ||
        kernel_stack_window.active != 0U || size == 0U ||
        (virtual_address & (RISCV_SV39_PAGE_SIZE_4K - 1U)) != 0U ||
        (size & (RISCV_SV39_PAGE_SIZE_4K - 1U)) != 0U ||
        !canonical_virtual_address(virtual_address) ||
        !valid_virtual_range(virtual_address, size) ||
        ((virtual_address >> 30U) & RISCV_SV39_INDEX_MASK) !=
            (((virtual_address + size - 1U) >> 30U) &
                RISCV_SV39_INDEX_MASK)) {
        return RISCV_SV39_STATUS_INVALID;
    }
    root = (uint64_t *)(uintptr_t)table->root_address;
    root_entry = &root[(virtual_address >> 30U) & RISCV_SV39_INDEX_MASK];
    if (*root_entry != 0U) {
        return RISCV_SV39_STATUS_CONFLICT;
    }
    status = allocate_table_page(table, &table_address);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    *root_entry = table_entry(table_address);
    kernel_stack_window.level1_address = table_address;
    kernel_stack_window.base = virtual_address;
    kernel_stack_window.size = size;
    kernel_stack_window.active = 1U;
    return RISCV_SV39_STATUS_OK;
}

int riscv_sv39_kernel_window_active(void)
{
    return kernel_stack_window.active != 0U;
}

static int kernel_window_contains(uint64_t address)
{
    return address >= kernel_stack_window.base &&
           address - kernel_stack_window.base <=
               kernel_stack_window.size - RISCV_SV39_PAGE_SIZE_4K;
}

static enum riscv_sv39_status kernel_window_allocate_level0(
    struct physical_page_allocator *allocator,
    uint64_t *address,
    uint64_t **entries)
{
    enum physical_page_status page_status;
    enum riscv_sv39_status status;

    page_status = physical_page_allocate(allocator, address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = resolve_runtime_table(allocator, *address, entries);
    if (status != RISCV_SV39_STATUS_OK) {
        (void)physical_page_release(allocator, *address);
        return status;
    }
    return RISCV_SV39_STATUS_OK;
}

static void kernel_window_sync(uint64_t address, uint64_t size)
{
#if BOAROS_MAX_CPUS > 1
    riscv_smp_sync_kernel(address, size);
#else
    riscv_mmu_flush_kernel_range(address, size);
#endif
}

/* Supervisor software bits distinguish excluded leaves and runtime-owned
 * tables. Boot-created fine tables have no split ownership. */
#define RISCV_DIRECT_SECRET RISCV_SV39_PTE_COW
#define RISCV_DIRECT_PENDING RISCV_SV39_PTE_COW
#define RISCV_DIRECT_SPLIT RISCV_SV39_PTE_PROTECTED
static uint64_t secret_kernel_root;
static struct kernel_raw_lock secret_direct_map_lock = {
    .rank = KERNEL_RAW_RANK_MM, .initialized = KERNEL_RAW_INITIALIZED,
};

enum riscv_sv39_status riscv_sv39_secret_direct_map_enable(
    const struct riscv_sv39_page_table *table)
{
    if (!table || table->state != RISCV_SV39_STATE_ACTIVE ||
        riscv_sv39_current_satp() !=
            (RISCV_SV39_SATP_MODE | (table->root_address >> BOAROS_PAGE_SHIFT)))
        return RISCV_SV39_STATUS_INVALID;
    if (__atomic_load_n(&secret_kernel_root, __ATOMIC_RELAXED))
        return RISCV_SV39_STATUS_STATE;
    __atomic_store_n(&secret_kernel_root, table->root_address, __ATOMIC_RELEASE);
    return RISCV_SV39_STATUS_OK;
}

int riscv_sv39_secret_page_supported(void)
{
    return __atomic_load_n(&secret_kernel_root, __ATOMIC_ACQUIRE) != 0;
}

/* Runtime direct-map roots already share this level-one directory across all
 * address spaces. Only the published leaf or its child table changes here. */
static enum riscv_sv39_status direct_map_parent(
    const struct physical_page_allocator *allocator, uint64_t va, uint64_t **parent)
{
    uint64_t physical = __atomic_load_n(&secret_kernel_root, __ATOMIC_ACQUIRE);
    if (!physical) return RISCV_SV39_STATUS_STATE;
    uint64_t *root, *directory;
    enum riscv_sv39_status status = resolve_runtime_table(allocator, physical, &root);
    if (status) return status;
    uint64_t root_entry = root[(va >> 30U) & RISCV_SV39_INDEX_MASK];
    if ((root_entry & RISCV_SV39_PTE_FLAGS_MASK) != RISCV_SV39_PTE_VALID)
        return RISCV_SV39_STATUS_NOT_MAPPED;
    status = resolve_runtime_table(allocator, (root_entry >> 10U) << 12U, &directory);
    if (status) return status;
    *parent = &directory[(va >> 21U) & RISCV_SV39_INDEX_MASK];
    return RISCV_SV39_STATUS_OK;
}

/* A split contains 512 leaves. Five pairs of supervisor software bits
 * record its live exclusions without touching physical or permission bits. */
#define RISCV_DIRECT_COUNT_WORDS 5U
#define RISCV_DIRECT_COUNT_MASK (RISCV_DIRECT_PENDING | RISCV_DIRECT_SPLIT)
static unsigned direct_exclusions(const uint64_t *leaves)
{
    unsigned count = 0;
    for (unsigned i = 0; i < RISCV_DIRECT_COUNT_WORDS; i++)
        count |= (unsigned)((leaves[i] & RISCV_DIRECT_COUNT_MASK) >> 8) << (2 * i);
    return count;
}

static void direct_set_exclusions(uint64_t *leaves, unsigned count)
{
    for (unsigned i = 0; i < RISCV_DIRECT_COUNT_WORDS; i++) {
        uint64_t old = leaves[i];
        uint64_t bits = (uint64_t)(count & 3U) << 8;
        if ((old & RISCV_DIRECT_COUNT_MASK) != bits)
            __atomic_store_n(&leaves[i], (old & ~RISCV_DIRECT_COUNT_MASK) | bits, __ATOMIC_RELEASE);
        count >>= 2;
    }
}

static enum riscv_sv39_status change_secret_direct_leaf(
    struct physical_page_allocator *allocator, uint64_t physical, int protect)
{
    if (!allocator || (physical & BOAROS_PAGE_MASK) || physical >= RISCV_DIRECT_MAP_SIZE)
        return RISCV_SV39_STATUS_INVALID;
    const uint64_t va = RISCV_DIRECT_MAP_BASE + physical;
    const unsigned leaf_index = (va >> BOAROS_PAGE_SHIFT) & RISCV_SV39_INDEX_MASK;
    const uint32_t permissions = RISCV_SV39_READ | RISCV_SV39_WRITE;
    const uint64_t original = leaf_entry(physical, permissions);
    const uint64_t excluded = (original & ~RISCV_SV39_PTE_VALID) | RISCV_DIRECT_SECRET;
    const uint64_t base = physical & ~(RISCV_SV39_PAGE_SIZE_2M - 1U);
    const uint64_t large = leaf_entry(base, permissions);
    const uint64_t nonleaf_flags = RISCV_SV39_PTE_FLAGS_MASK &
        ~(RISCV_DIRECT_PENDING | RISCV_DIRECT_SPLIT);
    uint64_t *parent;
    enum riscv_sv39_status status = direct_map_parent(allocator, va, &parent);
    if (status) return status;
    uint64_t candidate = 0;
    uint64_t *children = NULL;

    for (;;) {
        uint64_t entry, retired = 0;
        int prepare = 0, split = 0;
        uintptr_t irq = arch_interrupt_save();
        {
            KERNEL_RAW_SCOPE(guard, &secret_direct_map_lock);
            entry = *parent;
            if (entry == large) {
                if (!protect) status = RISCV_SV39_STATUS_CONFLICT;
                else if (!candidate) prepare = 1;
                else {
                    entry = table_entry(candidate) | RISCV_DIRECT_SPLIT | RISCV_DIRECT_PENDING;
                    __atomic_store_n(parent, entry, __ATOMIC_RELEASE);
                    candidate = 0;
                    split = 1;
                }
            } else if ((entry & nonleaf_flags) != RISCV_SV39_PTE_VALID) {
                status = RISCV_SV39_STATUS_NOT_MAPPED;
            } else {
                uint64_t table = (entry >> 10U) << BOAROS_PAGE_SHIFT;
                uint64_t *leaves;
                status = resolve_runtime_table(allocator, table, &leaves);
                if (!status) {
                    uint64_t *slot = &leaves[leaf_index];
                    int owned = (entry & RISCV_DIRECT_SPLIT) != 0;
                    uint64_t mask = owned ? RISCV_DIRECT_COUNT_MASK : 0;
                    uint64_t invalid = excluded & ~mask;
                    if ((*slot & ~mask) != (protect ? original : invalid))
                        status = RISCV_SV39_STATUS_CONFLICT;
                    else {
                        uint64_t replacement = (protect ? invalid : original) | (*slot & mask);
                        __atomic_store_n(slot, replacement, __ATOMIC_RELEASE);
                        if (owned) {
                            unsigned count = direct_exclusions(leaves);
                            if ((!protect && !count) || (protect && count > RISCV_SV39_INDEX_MASK))
                                __builtin_trap();
                            count = protect ? count + 1 : count - 1;
                            if (!count) {
                                if (entry & RISCV_DIRECT_PENDING) __builtin_trap();
                                __atomic_store_n(parent, large, __ATOMIC_RELEASE);
                                retired = table;
                            } else direct_set_exclusions(leaves, count);
                        }
                    }
                }
            }
        }
        if (!status && !prepare) {
            if ((entry & RISCV_DIRECT_PENDING) || retired)
                kernel_window_sync(0, UINT64_MAX);
            else
                kernel_window_sync(va, BOAROS_PAGE_SIZE);
            if (split) {
                /* The first excluded page stays private to this call until
                 * confirmation, keeping the table alive during the fence. */
                KERNEL_RAW_SCOPE(confirm, &secret_direct_map_lock);
                __atomic_store_n(parent, entry & ~RISCV_DIRECT_PENDING, __ATOMIC_RELEASE);
            }
        }
        arch_interrupt_restore(irq);
        if (retired && physical_page_release(allocator, retired)) __builtin_trap();
        if (!prepare) break;
        /* Prepare the whole replacement privately. A concurrent split can
         * win; this path never allocates under the page-table lock. */
        status = kernel_window_allocate_level0(allocator, &candidate, &children);
        if (status) return status;
        for (unsigned i = 0; i <= RISCV_SV39_INDEX_MASK; i++)
            children[i] = leaf_entry(base + (uint64_t)i * BOAROS_PAGE_SIZE, permissions);
        children[leaf_index] = excluded & ~RISCV_DIRECT_COUNT_MASK;
        direct_set_exclusions(children, 1);
    }
    if (candidate && physical_page_release(allocator, candidate)) __builtin_trap();
    return status;
}

enum riscv_sv39_status riscv_sv39_secret_page_protect(
    struct physical_page_allocator *allocator, uint64_t physical)
{ return change_secret_direct_leaf(allocator, physical, 1); }

enum riscv_sv39_status riscv_sv39_secret_page_restore(
    struct physical_page_allocator *allocator, uint64_t physical)
{ return change_secret_direct_leaf(allocator, physical, 0); }

static int kernel_window_valid(uint64_t address, unsigned pages)
{
    return kernel_stack_window.active && pages && pages <= 2 &&
        !(address & BOAROS_PAGE_MASK) && kernel_window_contains(address) &&
        kernel_window_contains(address + (pages - 1U) * BOAROS_PAGE_SIZE);
}

enum riscv_sv39_status riscv_sv39_kernel_window_map_pages(
    struct physical_page_allocator *allocator, uint64_t va, uint64_t pa, unsigned pages)
{
    if (!allocator || !kernel_window_valid(va, pages) || (pa & BOAROS_PAGE_MASK) ||
        pa > RISCV_SV39_PHYSICAL_LIMIT - pages * BOAROS_PAGE_SIZE)
        return RISCV_SV39_STATUS_INVALID;
    uint64_t candidates[2] = {0}, *candidate_data[2] = {0};
    unsigned indexes[2] = {(va >> 21) & RISCV_SV39_INDEX_MASK,
        ((va + (pages - 1U) * BOAROS_PAGE_SIZE) >> 21) & RISCV_SV39_INDEX_MASK};
    unsigned tables = indexes[0] == indexes[1] ? 1 : 2;
    enum riscv_sv39_status status;
    for (;;) {
        uintptr_t irq = arch_interrupt_save();
        KERNEL_RAW_SCOPE(guard, &kernel_window_lock);
        uint64_t *level1, *data[2] = {0};
        status = resolve_runtime_table(allocator, kernel_stack_window.level1_address,
                                       &level1);
        unsigned missing = tables;
        if (status != RISCV_SV39_STATUS_OK) {
            kernel_raw_lock_release(&guard);
            arch_interrupt_restore(irq);
            break;
        }
        for (unsigned i = 0; i < tables; i++) {
            uint64_t entry = level1[indexes[i]];
            if (!entry) {
                if (!candidates[i]) { missing = i; break; }
                data[i] = candidate_data[i];
            } else if ((entry & RISCV_SV39_PTE_FLAGS_MASK) != RISCV_SV39_PTE_VALID) {
                status = RISCV_SV39_STATUS_INVALID;
                break;
            } else {
                status = resolve_runtime_table(allocator, (entry >> 10) << 12, &data[i]);
                if (status != RISCV_SV39_STATUS_OK) break;
            }
        }
        if (status == RISCV_SV39_STATUS_OK && missing == tables) {
            for (unsigned page = 0; page < pages; page++) {
                uint64_t address = va + page * BOAROS_PAGE_SIZE;
                unsigned group = ((address >> 21) & RISCV_SV39_INDEX_MASK) == indexes[0] ? 0 : 1;
                if (data[group][(address >> 12) & RISCV_SV39_INDEX_MASK]) {
                    status = RISCV_SV39_STATUS_CONFLICT;
                    break;
                }
            }
            int changed_parent = 0;
            if (status == RISCV_SV39_STATUS_OK) {
                /* 没有分配/睡眠：全部预检后发布，候选表的owner转给共享骨架。 */
                for (unsigned i = 0; i < tables; i++) if (!level1[indexes[i]]) {
                    changed_parent = 1;
                    __atomic_store_n(&level1[indexes[i]], table_entry(candidates[i]), __ATOMIC_RELEASE);
                    candidates[i] = 0;
                }
                for (unsigned page = 0; page < pages; page++) {
                    uint64_t address = va + page * BOAROS_PAGE_SIZE;
                    unsigned group = ((address >> 21) & RISCV_SV39_INDEX_MASK) == indexes[0] ? 0 : 1;
                    __atomic_store_n(&data[group][(address >> 12) & RISCV_SV39_INDEX_MASK],
                        leaf_entry(pa + page * BOAROS_PAGE_SIZE, RISCV_SV39_READ | RISCV_SV39_WRITE),
                        __ATOMIC_RELEASE);
                    kernel_stack_window.mapped_leaves[indexes[group]]++;
                }
            }
            kernel_raw_lock_release(&guard);
            if (status == RISCV_SV39_STATUS_OK) {
                /* 非叶PTE改变必须覆盖walk缓存，不能只请求远端叶子VA失效。 */
                kernel_window_sync(changed_parent ? 0 : va,
                    changed_parent ? UINT64_MAX : pages * BOAROS_PAGE_SIZE);
            }
            arch_interrupt_restore(irq);
            break;
        }
        kernel_raw_lock_release(&guard);
        arch_interrupt_restore(irq);
        if (status != RISCV_SV39_STATUS_OK) break;
        /* allocation可显式睡眠；下轮重新解析，绝不携带裸PTE。已有表不分配。 */
        status = kernel_window_allocate_level0(allocator, &candidates[missing],
                                                &candidate_data[missing]);
        if (status != RISCV_SV39_STATUS_OK) break;
        clear_runtime_table(candidate_data[missing]);
    }
    for (unsigned i = 0; i < tables; i++) if (candidates[i])
        physical_page_release(allocator, candidates[i]);
    return status;
}

enum riscv_sv39_status riscv_sv39_kernel_window_unmap_pages(
    struct physical_page_allocator *allocator, uint64_t va, unsigned pages)
{
    if (!allocator || !kernel_window_valid(va, pages)) return RISCV_SV39_STATUS_INVALID;
    KERNEL_IRQ_SCOPE(irq);
    KERNEL_RAW_SCOPE(guard, &kernel_window_lock);
    uint64_t *level1, *data[2] = {0}, retired[2] = {0};
    unsigned indexes[2] = {(va >> 21) & RISCV_SV39_INDEX_MASK,
        ((va + (pages - 1U) * BOAROS_PAGE_SIZE) >> 21) & RISCV_SV39_INDEX_MASK};
    unsigned tables = indexes[0] == indexes[1] ? 1 : 2;
    enum riscv_sv39_status status = resolve_runtime_table(
        allocator, kernel_stack_window.level1_address, &level1);
    if (status != RISCV_SV39_STATUS_OK) goto out;
    for (unsigned i = 0; i < tables; i++) {
        uint64_t entry = level1[indexes[i]];
        if (!entry) { status = RISCV_SV39_STATUS_NOT_MAPPED; goto out; }
        if ((entry & RISCV_SV39_PTE_FLAGS_MASK) != RISCV_SV39_PTE_VALID) {
            status = RISCV_SV39_STATUS_INVALID; goto out;
        }
        status = resolve_runtime_table(allocator, (entry >> 10) << 12, &data[i]);
        if (status != RISCV_SV39_STATUS_OK) goto out;
    }
    for (unsigned page = 0; page < pages; page++) {
        uint64_t address = va + page * BOAROS_PAGE_SIZE;
        unsigned group = ((address >> 21) & RISCV_SV39_INDEX_MASK) == indexes[0] ? 0 : 1;
        if (!data[group][(address >> 12) & RISCV_SV39_INDEX_MASK]) {
            status = RISCV_SV39_STATUS_NOT_MAPPED; goto out;
        }
    }
    for (unsigned page = 0; page < pages; page++) {
        uint64_t address = va + page * BOAROS_PAGE_SIZE;
        unsigned group = ((address >> 21) & RISCV_SV39_INDEX_MASK) == indexes[0] ? 0 : 1;
        __atomic_store_n(&data[group][(address >> 12) & RISCV_SV39_INDEX_MASK], 0, __ATOMIC_RELEASE);
        kernel_stack_window.mapped_leaves[indexes[group]]--;
    }
    for (unsigned i = 0; i < tables; i++) {
        if (!kernel_stack_window.mapped_leaves[indexes[i]]) {
            retired[i] = (level1[indexes[i]] >> 10) << 12;
            __atomic_store_n(&level1[indexes[i]], 0, __ATOMIC_RELEASE);
        }
    }
    /* 空表退休也覆盖walk缓存；确认所有在线hart停止旧翻译后才能归还owner。 */
    int changed_parent = retired[0] || retired[1];
    kernel_raw_lock_release(&guard);
    kernel_window_sync(changed_parent ? 0 : va,
        changed_parent ? UINT64_MAX : pages * BOAROS_PAGE_SIZE);
    for (unsigned i = 0; i < tables; i++) if (retired[i])
        physical_page_release(allocator, retired[i]);
out:
    return status;
}

enum riscv_sv39_status riscv_sv39_kernel_window_map(
    struct physical_page_allocator *allocator, uint64_t va, uint64_t pa)
{ return riscv_sv39_kernel_window_map_pages(allocator, va, pa, 1); }

enum riscv_sv39_status riscv_sv39_kernel_window_unmap(
    struct physical_page_allocator *allocator, uint64_t va)
{ return riscv_sv39_kernel_window_unmap_pages(allocator, va, 1); }

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
           (!(entry & RISCV_SV39_PTE_FILE) || (entry & RISCV_SV39_PTE_COW)) &&
           !((entry & RISCV_SV39_PTE_WRITE) != 0U &&
             (entry & RISCV_SV39_PTE_READ) == 0U) &&
           !((entry & RISCV_SV39_PTE_COW) != 0U &&
             (entry & RISCV_SV39_PTE_WRITE) != 0U);
}

static int present_cow_entry(uint64_t entry)
{
    return valid_user_leaf_entry(entry) &&
           (entry & RISCV_SV39_PTE_COW) != 0U;
}

static int invalidated_owned_entry(uint64_t entry)
{
    return (entry & ~RISCV_SV39_PTE_ALLOWED_MASK) == 0U &&
           (entry & RISCV_SV39_PTE_FLAGS_MASK) ==
               RISCV_SV39_PTE_COW;
}

static int valid_protected_entry(uint64_t entry)
{
    uint64_t flags = entry & RISCV_SV39_PTE_FLAGS_MASK;
    return (entry & ~RISCV_SV39_PTE_ALLOWED_MASK) == 0 &&
        (flags == RISCV_SV39_PTE_PROTECTED ||
         (flags & ~RISCV_SV39_PTE_PROTECTED_FILE) == RISCV_SV39_PTE_PROTECTED_COW);
}

static int protected_cow_entry(uint64_t entry)
{
    return (entry & ~RISCV_SV39_PTE_ALLOWED_MASK) == 0U &&
           (entry & (RISCV_SV39_PTE_FLAGS_MASK & ~RISCV_SV39_PTE_PROTECTED_FILE)) ==
               RISCV_SV39_PTE_PROTECTED_COW;
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

static uint64_t invalidated_entry(uint64_t address)
{
    return ((address >> BOAROS_PAGE_SHIFT) << 10U) |
           RISCV_SV39_PTE_COW;
}

static uint64_t protected_entry(uint64_t address, int cow)
{
    return ((address >> BOAROS_PAGE_SHIFT) << 10U) |
           (cow ? RISCV_SV39_PTE_PROTECTED_COW
                : RISCV_SV39_PTE_PROTECTED);
}

static uint64_t user_cow_entry(uint64_t address,
                               uint32_t permissions)
{
    return user_leaf_entry(address,
                           permissions & ~RISCV_SV39_WRITE) |
           RISCV_SV39_PTE_COW;
}

static int user_entry_file_backed(uint64_t entry)
{
    return (entry & (entry & RISCV_SV39_PTE_VALID ?
        RISCV_SV39_PTE_FILE : RISCV_SV39_PTE_PROTECTED_FILE)) != 0;
}

static unsigned user_entry_flags(uint64_t entry)
{
    return ((entry & RISCV_SV39_PTE_COW) ? KERNEL_MM_LEAF_COW : 0) |
        (user_entry_file_backed(entry) ? KERNEL_MM_LEAF_FILE : 0);
}

static uint64_t encode_user_entry(uint64_t address, uint32_t permissions, unsigned flags)
{
    int cow = (flags & KERNEL_MM_LEAF_COW) != 0;
    int file = (flags & KERNEL_MM_LEAF_FILE) != 0;
    if (!permissions)
        return protected_entry(address, cow) | (file ? RISCV_SV39_PTE_PROTECTED_FILE : 0);
    uint64_t leaf = cow ? user_cow_entry(address, permissions) : user_leaf_entry(address, permissions);
    return leaf | (file ? RISCV_SV39_PTE_FILE : 0);
}

enum riscv_sv39_status riscv_sv39_user_space_init(
    struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table)
{
    struct riscv_sv39_user_space result = {0};
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
        (void)physical_page_release(allocator, root_address);
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
    result.root_entries = user_root;
    result.table_pages = 1U;
    result.leaf_pages = 0U;
    result.protected_pages = 0U;
    result.cow_pages = 0U;
    result.cow_copies = 0U;
    result.cow_in_place = 0U;
    result.state = RISCV_SV39_USER_SPACE_LIVE;
    *space = result;
    return RISCV_SV39_STATUS_OK;
}

/* 只归还从未发布的候选；已发布非叶必须由上层失效/退休协议回收。 */
static enum riscv_sv39_status discard_private_runtime_table(
    struct riscv_sv39_user_space *space,
    uint64_t address)
{
    (void)physical_page_release(space->allocator, address);
    if (space->table_pages <= 1U) {
        return RISCV_SV39_STATUS_STATE;
    }
    space->table_pages--;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status resolve_user_order_slot(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint64_t **leaf, uint32_t order)
{
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t *root_entry;
    uint64_t *level1_entry;
    uint64_t *result;
    uint64_t level1_address;
    uint64_t level0_address;
    int allocated_level1 = 0;
    enum riscv_sv39_status status;

    root = space->root_entries;
    root_entry = &root[(virtual_address >> 30U) & RISCV_SV39_INDEX_MASK];
    if (*root_entry == 0U) {
        status = allocate_runtime_table(space,
                                        &level1_address,
                                        &level1);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
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
    if (order) {
        if (*level1_entry) return RISCV_SV39_STATUS_CONFLICT;
        if (allocated_level1)
            __atomic_store_n(root_entry,table_entry(level1_address),__ATOMIC_RELEASE);
        *leaf=level1_entry;
        return RISCV_SV39_STATUS_OK;
    }
    if (*level1_entry == 0U) {
        status = allocate_runtime_table(space,
                                        &level0_address,
                                        &level0);
        if (status != RISCV_SV39_STATUS_OK) {
            if (allocated_level1 != 0 &&
                discard_private_runtime_table(space,
                                              level1_address) !=
                    RISCV_SV39_STATUS_OK) {
                return RISCV_SV39_STATUS_STATE;
            }
            return status;
        }
        __atomic_store_n(level1_entry,table_entry(level0_address),__ATOMIC_RELEASE);
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

    result = &level0[(virtual_address >> BOAROS_PAGE_SHIFT) &
                     RISCV_SV39_INDEX_MASK];
    if (*result != 0U) {
        return RISCV_SV39_STATUS_CONFLICT;
    }
    /* 缺失的整条路径准备完才发布root；二级OOM不会留下远端可见后
     * 直接归还的L1。已有L1的路径只发布这次准备完整的L0。 */
    if (allocated_level1)
        __atomic_store_n(root_entry,table_entry(level1_address),__ATOMIC_RELEASE);
    *leaf = result;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status resolve_user_leaf_slot(struct riscv_sv39_user_space *s,
    uint64_t v, uint64_t **leaf)
{ return resolve_user_order_slot(s,v,leaf,0); }

enum riscv_sv39_status riscv_sv39_user_map_owned_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions)
{
    uint64_t *leaf;
    void *owned_page;
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
    if (physical_page_map_owned(space->allocator,
                              physical_address,
                              &owned_page) != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    (void)owned_page;
    uint32_t owner_order;
    if (physical_page_allocator_is_finalized(space->allocator) &&
        (physical_page_allocation_order(space->allocator,physical_address,&owner_order) || owner_order))
        return RISCV_SV39_STATUS_INVALID;

    status = resolve_user_leaf_slot(space, virtual_address, &leaf);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    *leaf = user_leaf_entry(physical_address, permissions);
    space->leaf_pages++;
    resident_peak(space);
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_map_cow_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions)
{
    uint64_t *leaf;
    void *owned_page;
    enum riscv_sv39_status status;

    if (space == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (virtual_address < RISCV_SV39_PAGE_SIZE_4K ||
        virtual_address >= RISCV_SV39_USER_LIMIT ||
        (virtual_address & BOAROS_PAGE_MASK) != 0U ||
        (physical_address & BOAROS_PAGE_MASK) != 0U ||
        physical_address > RISCV_SV39_PHYSICAL_MAX - BOAROS_PAGE_MASK ||
        !valid_user_permissions(permissions) ||
        space->cow_pages == UINT32_MAX) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (physical_page_map_owned(space->allocator,
                              physical_address,
                              &owned_page) != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    (void)owned_page;
    uint32_t owner_order;
    if (physical_page_allocator_is_finalized(space->allocator) &&
        (physical_page_allocation_order(space->allocator,physical_address,&owner_order) || owner_order))
        return RISCV_SV39_STATUS_INVALID;
    status = resolve_user_leaf_slot(space, virtual_address, &leaf);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    *leaf = user_cow_entry(physical_address, permissions);
    space->leaf_pages++;
    space->cow_pages++;
    resident_peak(space);
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_map_zeroed_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions)
{
    uint64_t physical_address;
    uint64_t *leaf;
    uint64_t *words;
    uint32_t index;
    enum physical_page_status page_status;
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
        !valid_user_permissions(permissions)) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = resolve_user_leaf_slot(space, virtual_address, &leaf);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    page_status = physical_page_allocate(space->allocator,
                                         &physical_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status == PHYSICAL_PAGE_STATUS_STATE
                   ? RISCV_SV39_STATUS_STATE
                   : RISCV_SV39_STATUS_INVALID;
    }
    status = resolve_runtime_table(space->allocator,
                                   physical_address,
                                   &words);
    if (status != RISCV_SV39_STATUS_OK) {
        (void)physical_page_release(space->allocator,
                                  physical_address);
        return status;
    }
    _Static_assert(BOAROS_PAGE_SIZE % (8U * sizeof(*words)) == 0,
                   "private page zeroing uses complete integer groups");
    /* 新页仍由构造方独占，清完才发布PTE；不借读或覆盖页外字节。 */
    for (index = 0U; index < BOAROS_PAGE_SIZE / sizeof(*words); index += 8U) {
        words[index] = 0U;
        words[index + 1U] = 0U;
        words[index + 2U] = 0U;
        words[index + 3U] = 0U;
        words[index + 4U] = 0U;
        words[index + 5U] = 0U;
        words[index + 6U] = 0U;
        words[index + 7U] = 0U;
    }
    *leaf = user_leaf_entry(physical_address, permissions);
    space->leaf_pages++;
    resident_peak(space);
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

/* Resolve either a base leaf or a native 2 MiB PMD leaf. Never interpret
 * a leaf (including protected software leaves) as a table address. */
static enum riscv_sv39_status resolve_leaf_order(const struct riscv_sv39_user_space *s,
    uint64_t v, uint64_t **slot, uint32_t *order, uint64_t *visits)
{
    if (!s || v >= RISCV_SV39_USER_LIMIT) return RISCV_SV39_STATUS_INVALID;
    if (s->state != RISCV_SV39_USER_SPACE_LIVE) return RISCV_SV39_STATUS_STATE;
    uint64_t *entries = s->root_entries;
    enum riscv_sv39_status status;
    for (int level=2; level>=0; level--) {
        if (visits) { (*visits)++; COST_ADD(MPROTECT_PTE_VISITS,1); }
        /* Match resolve_user_order_slot/install_prepared_leaf exactly. The
         * host base-page model also exercises 16 KiB allocation granules;
         * changing its upper-level indices loses already published leaves. */
        unsigned shift=level==2?30U:level==1?21U:BOAROS_PAGE_SHIFT;
        *slot=&entries[(v>>shift) & RISCV_SV39_INDEX_MASK];
        uint64_t entry=**slot;
        if (!entry) { *order=9*level;return RISCV_SV39_STATUS_NOT_MAPPED; }
        if (valid_user_leaf_entry(entry) || valid_protected_entry(entry)) {
            if (level==2) return RISCV_SV39_STATUS_STATE;
            *order=9*level;
            if (entry_address(entry) & (((uint64_t)BOAROS_PAGE_SIZE<<*order)-1))
                return RISCV_SV39_STATUS_STATE;
            return RISCV_SV39_STATUS_OK;
        }
        if (!level || !valid_nonleaf_entry(entry)) return RISCV_SV39_STATUS_STATE;
        status=resolve_runtime_table(s->allocator,entry_address(entry),&entries);
        if (status) return status;
    }
    return RISCV_SV39_STATUS_STATE;
}
enum riscv_sv39_status riscv_sv39_user_leaf_probe_slot(const struct riscv_sv39_user_space *s,
    uint64_t v, struct kernel_mm_leaf *out, unsigned *missing_tables, uint64_t **base_slot)
{
    if (base_slot) *base_slot = NULL;
    if (!out) return RISCV_SV39_STATUS_INVALID;
    uint64_t *slot; uint32_t order;
    enum riscv_sv39_status status=resolve_leaf_order(s,v,&slot,&order,0);
    if (missing_tables) {
        if (status==RISCV_SV39_STATUS_NOT_MAPPED)
            *missing_tables=order==18 ? 3 : order==9 ? 2 : 0;
        else if (status==RISCV_SV39_STATUS_OK) *missing_tables=0;
    }
    if (status) return status;
    if (base_slot && !order) *base_slot = slot;
    uint64_t entry=*slot, size=(uint64_t)BOAROS_PAGE_SIZE<<order;
    *out=(struct kernel_mm_leaf){.virtual_address=v&~(size-1),
        .physical_address=entry_address(entry),.cookie=entry,.order=order,
        .permissions=valid_protected_entry(entry)?0:entry_permissions(entry),
        .cow=present_cow_entry(entry)||protected_cow_entry(entry),
        .file_backed=user_entry_file_backed(entry)};
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_leaf_probe(const struct riscv_sv39_user_space *s,
    uint64_t v, struct kernel_mm_leaf *out, unsigned *missing_tables)
{ return riscv_sv39_user_leaf_probe_slot(s,v,out,missing_tables,NULL); }

enum riscv_sv39_status riscv_sv39_user_leaf_snapshot(const struct riscv_sv39_user_space *s,
    uint64_t v, struct kernel_mm_leaf *out)
{ return riscv_sv39_user_leaf_probe(s,v,out,NULL); }

#if BOAROS_MAX_CPUS == 1
unsigned riscv_sv39_user_accessible_pages(const struct riscv_sv39_user_space *space,
    uint64_t first, unsigned count, uint32_t access, uint64_t *physical)
{
    if (first & BOAROS_PAGE_MASK) return 0;
    const uint64_t permission=access==RISCV_SV39_WRITE ? RISCV_SV39_PTE_WRITE : RISCV_SV39_PTE_READ;
    unsigned done=0;
    while (done<count) {
        uint64_t address=first+(uint64_t)done*BOAROS_PAGE_SIZE;
        uint64_t *slot;
        uint32_t order;
        if (resolve_leaf_order(space,address,&slot,&order,NULL) || order)
            break;
        unsigned remaining=(RISCV_SV39_PAGE_SIZE_2M-
            (address & (RISCV_SV39_PAGE_SIZE_2M-1)))>>BOAROS_PAGE_SHIFT;
        if (remaining>count-done) remaining=count-done;
        for (unsigned i=0;i<remaining;i++) {
            uint64_t entry=slot[i];
            if (!(entry & permission) || !valid_user_leaf_entry(entry))
                return done;
            physical[done++]=entry_address(entry);
        }
    }
    return done;
}
#endif

enum riscv_sv39_status riscv_sv39_user_empty_window_slot(const struct riscv_sv39_user_space *s,
    uint64_t address, unsigned count, unsigned *empty, uint64_t **base_slot)
{
    const uint64_t span=UINT64_C(1)<<21;
    if (!empty || !count || count>32 || (address&BOAROS_PAGE_MASK) ||
        count>(span-(address&(span-1)))/BOAROS_PAGE_SIZE) return RISCV_SV39_STATUS_INVALID;
    uint64_t *slot;
    uint32_t order;
    enum riscv_sv39_status status=resolve_leaf_order(s,address,&slot,&order,0);
    if (status && status!=RISCV_SV39_STATUS_NOT_MAPPED) return status;
    if (base_slot) *base_slot = order ? NULL : slot;
    unsigned mask=0;
    if (order) mask=status ? UINT32_MAX>>(32-count) : 0;
    else for (unsigned i=0;i<count;i++) if (!slot[i]) mask|=1U<<i;
    *empty=mask;
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_empty_window(const struct riscv_sv39_user_space *s,
    uint64_t address, unsigned count, unsigned *empty)
{ return riscv_sv39_user_empty_window_slot(s, address, count, empty, NULL); }
enum riscv_sv39_status riscv_sv39_user_lookup(const struct riscv_sv39_user_space *s,
    uint64_t v, struct riscv_sv39_mapping *out)
{
    if (!out) return RISCV_SV39_STATUS_INVALID;
    struct kernel_mm_leaf leaf;
    enum riscv_sv39_status status=riscv_sv39_user_leaf_snapshot(s,v,&leaf);
    if (status) return status;
    if (!leaf.permissions) return RISCV_SV39_STATUS_NOT_MAPPED;
    *out=(struct riscv_sv39_mapping){leaf.physical_address+v-leaf.virtual_address,leaf.permissions};
    return RISCV_SV39_STATUS_OK;
}
static enum riscv_sv39_status resolve_existing_user_leaf(struct riscv_sv39_user_space *s,
    uint64_t v, uint64_t **slot, uint64_t *visits)
{
    uint32_t order;
    enum riscv_sv39_status status=resolve_leaf_order(s,v,slot,&order,visits);
    return status ? status : order ? RISCV_SV39_STATUS_INVALID : RISCV_SV39_STATUS_OK;
}

static unsigned next_table_entry(const uint64_t *entries, unsigned first, unsigned limit)
{
    if (first == limit || entries[first]) return first;
    first++;
    for (; limit - first >= 8; first += 8) {
        const uint64_t *entry = entries + first;
        if (entry[0] | entry[1] | entry[2] | entry[3] |
            entry[4] | entry[5] | entry[6] | entry[7])
            break;
    }
    while (first < limit && !entries[first]) first++;
    return first;
}

static int runtime_table_empty(const uint64_t *entries)
{
    const unsigned limit = RISCV_SV39_INDEX_MASK + 1U;
    return next_table_entry(entries, 0, limit) == limit;
}

static enum riscv_sv39_status validate_leaf_boundaries(struct riscv_sv39_user_space *s,
    uint64_t start, uint64_t end)
{
    if (start == end) return RISCV_SV39_STATUS_OK;
    struct kernel_mm_leaf leaf;
    enum riscv_sv39_status status=riscv_sv39_user_leaf_snapshot(s,start,&leaf);
    if (!status && leaf.virtual_address < start) return RISCV_SV39_STATUS_INVALID;
    if (status && status != RISCV_SV39_STATUS_NOT_MAPPED) return status;
    status=riscv_sv39_user_leaf_snapshot(s,end-1,&leaf);
    if (!status && leaf.virtual_address+kernel_mm_leaf_size(&leaf)>end)
        return RISCV_SV39_STATUS_INVALID;
    return status == RISCV_SV39_STATUS_NOT_MAPPED ? RISCV_SV39_STATUS_OK : status;
}

enum riscv_sv39_status riscv_sv39_user_detach_range(
    struct riscv_sv39_user_space *s, uint64_t start, uint64_t end,
    struct kernel_mm_detach_cursor *cursor, struct kernel_mm_retirement *batch,
    void (*before_detach)(void *,uint64_t,uint64_t), void *owner)
{
    if (!s || s->state != RISCV_SV39_USER_SPACE_LIVE || !cursor || !batch || start>end ||
        end>RISCV_SV39_USER_LIMIT || ((start|end)&BOAROS_PAGE_MASK)) return RISCV_SV39_STATUS_INVALID;
    const uint64_t leaf_span=UINT64_C(1)<<21, middle_span=UINT64_C(1)<<30;
    if (cursor->phase==3 || start==end) return RISCV_SV39_STATUS_NOT_MAPPED;
    if (!cursor->phase) {
        enum riscv_sv39_status boundary=validate_leaf_boundaries(s,start,end);
        if (boundary) return boundary;
        cursor->address=start; cursor->phase=1;
    }
    uint64_t *root = s->root_entries;
    enum riscv_sv39_status status;
    unsigned visits=0, holes=0;
    enum { MAX_DETACH_VISITS=16, MAX_EMPTY_LEAVES=64 };
    while (visits<MAX_DETACH_VISITS && holes<MAX_EMPTY_LEAVES &&
           batch->count<=KERNEL_MM_RETIRE_CAPACITY-3) {
        if (cursor->phase==2) {
            unsigned r=cursor->middle_address>>30;
            /* Another disjoint range may have retired or replaced this
             * parent between batches. Only the currently published table
             * can be inspected, and it need not exist anymore. */
            if (!root[r]) { cursor->phase=1; continue; }
            if (!valid_nonleaf_entry(root[r])) return RISCV_SV39_STATUS_STATE;
            uint64_t *middle;
            status=resolve_runtime_table(s->allocator,entry_address(root[r]),&middle);
            if (status) return status;
            visits++;
            /* Empty prefixes are not stable across MM qualification. Check
             * the actual entire table in the same scope that detaches it. */
            if (runtime_table_empty(middle)) {
                kernel_mm_retirement_add(batch,entry_address(root[r]),1);
                __atomic_store_n(&root[r],0,__ATOMIC_RELEASE);
                s->table_pages--;
            }
            cursor->phase=1;
            continue;
        }
        if (cursor->address>=end) { cursor->phase=3; return RISCV_SV39_STATUS_NOT_MAPPED; }
        uint64_t address=cursor->address;
        unsigned r=address>>30, m=(address>>21)&(512-1);
        unsigned l=(address>>BOAROS_PAGE_SHIFT)&(512-1);
        if (!root[r]) {
            cursor->address=(address & ~(middle_span-1))+middle_span;
            visits++; continue;
        }
        if (!valid_nonleaf_entry(root[r])) return RISCV_SV39_STATUS_STATE;
        uint64_t *middle;
        status=resolve_runtime_table(s->allocator,entry_address(root[r]),&middle);
            if (status) return status;
        if (!middle[m]) {
            cursor->address=(address & ~(leaf_span-1))+leaf_span;
            visits++;
        } else if (valid_user_leaf_entry(middle[m]) || valid_protected_entry(middle[m])) {
            uint64_t old=middle[m], base=address & ~(leaf_span-1);
            if (address!=base || base+leaf_span>end) return RISCV_SV39_STATUS_INVALID;
            if (cursor->file_only) {
                cursor->address=base+leaf_span;
                visits++;
                continue;
            }
            int present=valid_user_leaf_entry(old), cow=present_cow_entry(old)||protected_cow_entry(old);
            if ((present ? s->leaf_pages : s->protected_pages)<512 || (cow && s->cow_pages<512))
                return RISCV_SV39_STATUS_STATE;
            if (before_detach) before_detach(owner,base,entry_address(old));
            __atomic_store_n(&middle[m],0,__ATOMIC_RELEASE);
            kernel_mm_retirement_add_order(batch,entry_address(old),9,0);
            if (present) s->leaf_pages-=512; else s->protected_pages-=512;
            if (cow) s->cow_pages-=512;
            cursor->address=base+leaf_span; visits++;
        } else {
            if (!valid_nonleaf_entry(middle[m])) return RISCV_SV39_STATUS_STATE;
            uint64_t *leaves;
            status=resolve_runtime_table(s->allocator,entry_address(middle[m]),&leaves);
            if (status) return status;
            /* This metadata scope owns the table throughout the batch. Only
             * the virtual cursor survives a return to the MM caller. */
            uint64_t limit = (address & ~(leaf_span - 1)) + leaf_span;
            if (limit > end) limit = end;
            /* An empty slot only costs a load. Keep a separate bound so
             * sparse ranges do not restart the same table walk every 16
             * slots while the number of actual removals stays bounded. */
            do {
                uint64_t old=leaves[l];
                if (old && (!cursor->file_only || user_entry_file_backed(old))) {
                    visits++;
                    int present=valid_user_leaf_entry(old), protected=valid_protected_entry(old);
                    int cow=present_cow_entry(old)||protected_cow_entry(old);
                    if ((!present && !protected) || (present && !s->leaf_pages) ||
                        (protected && !s->protected_pages) || (cow && !s->cow_pages) || s->table_pages<3)
                        return RISCV_SV39_STATUS_STATE;
                    uint64_t physical=entry_address(old);
#if BOAROS_LOCK_DEBUG
                    void *owned;
                    if (physical_page_map_owned(s->allocator,physical,&owned)!=PHYSICAL_PAGE_STATUS_OK)
                        return RISCV_SV39_STATUS_STATE;
#endif
                    /* resident必须在身份仍属于这个PTE时解绑；释放仍由MM在raw外完成。 */
                    if (before_detach) before_detach(owner,address,physical);
                    __atomic_store_n(&leaves[l],0,__ATOMIC_RELEASE);
                    kernel_mm_retirement_add(batch,physical,0);
                    if (present) s->leaf_pages--; else s->protected_pages--;
                    if (cow) s->cow_pages--;
                } else holes++;
                address += BOAROS_PAGE_SIZE;
                l++;
            } while (address < limit && visits<MAX_DETACH_VISITS &&
                     holes<MAX_EMPTY_LEAVES &&
                     batch->count <= KERNEL_MM_RETIRE_CAPACITY - 3);
            cursor->address = address;
            if (address < limit) return RISCV_SV39_STATUS_OK;
            if (runtime_table_empty(leaves)) {
                kernel_mm_retirement_add(batch,entry_address(middle[m]),1);
                __atomic_store_n(&middle[m],0,__ATOMIC_RELEASE);
                s->table_pages--;
            }
        }
        if ((cursor->address>>30)!=r || cursor->address>=end) {
            cursor->middle_address=(uint64_t)r<<30;
            cursor->phase=2;
        }
    }
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_resolve_cow(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions)
{
    uint64_t *leaf;
    uint64_t old_entry;
    uint64_t old_address;
    uint64_t new_address;
    uint64_t *old_words;
    uint64_t *new_words;
    uint32_t references;
    uint32_t index;
    enum physical_page_status page_status;
    enum riscv_sv39_status status;

    if (space == 0 ||
        space->state != RISCV_SV39_USER_SPACE_LIVE ||
        virtual_address < RISCV_SV39_PAGE_SIZE_4K ||
        virtual_address >= RISCV_SV39_USER_LIMIT ||
        !valid_user_permissions(permissions) ||
        (permissions & RISCV_SV39_WRITE) == 0U) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = resolve_existing_user_leaf(space, virtual_address, &leaf, 0);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    old_entry = *leaf;
    if (!present_cow_entry(old_entry)) {
        return valid_user_leaf_entry(old_entry)
                   ? RISCV_SV39_STATUS_NOT_MAPPED
                   : RISCV_SV39_STATUS_STATE;
    }
    if (space->cow_pages == 0U ||
        physical_page_reference_count(space->allocator,
                                      entry_address(old_entry),
                                      &references) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_STATE;
    }
    old_address = entry_address(old_entry);
    if (references == 1U) {
        *leaf = user_leaf_entry(old_address, permissions);
        space->cow_pages--;
        space->cow_in_place++;
        riscv_mmu_flush_address(virtual_address & ~BOAROS_PAGE_MASK);
        return RISCV_SV39_STATUS_OK;
    }

    page_status = physical_page_allocate(space->allocator, &new_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = resolve_runtime_table(space->allocator,
                                   old_address,
                                   &old_words);
    if (status == RISCV_SV39_STATUS_OK) {
        status = resolve_runtime_table(space->allocator,
                                       new_address,
                                       &new_words);
    }
    if (status != RISCV_SV39_STATUS_OK) {
        (void)physical_page_release(space->allocator, new_address);
        return status;
    }
    for (index = 0U;
         index < BOAROS_PAGE_SIZE / sizeof(*old_words);
         index++) {
        new_words[index] = old_words[index];
    }
    *leaf = user_leaf_entry(new_address, permissions);
    space->cow_pages--;
    space->cow_copies++;
    if ((permissions & RISCV_SV39_EXECUTE) != 0U) {
        riscv_mmu_sync_instructions();
    }
    riscv_mmu_flush_address(virtual_address & ~BOAROS_PAGE_MASK);
    (void)physical_page_release(space->allocator, old_address);
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_leaf_missing_order(struct riscv_sv39_user_space *s,
    uint64_t address, uint32_t order, unsigned *mask)
{
    if ((order != 0 && order != 9) || !s || s->state != RISCV_SV39_USER_SPACE_LIVE || !mask ||
        address < BOAROS_PAGE_SIZE || address >= RISCV_SV39_USER_LIMIT ||
        (address & (((uint64_t)BOAROS_PAGE_SIZE<<order)-1))) return RISCV_SV39_STATUS_INVALID;
    uint64_t *root = s->root_entries, *middle;
    enum riscv_sv39_status status;
    uint64_t r = root[(address >> 30) & RISCV_SV39_INDEX_MASK];
    if (!r) { *mask = order ? 1 : 3; return RISCV_SV39_STATUS_OK; }
    if (!valid_nonleaf_entry(r)) return RISCV_SV39_STATUS_STATE;
    status = resolve_runtime_table(s->allocator, entry_address(r), &middle);
    if (status != RISCV_SV39_STATUS_OK) return status;
    uint64_t m = middle[(address >> 21) & RISCV_SV39_INDEX_MASK];
    if (order) { *mask=0; return m ? RISCV_SV39_STATUS_CONFLICT : RISCV_SV39_STATUS_OK; }
    if (m && !valid_nonleaf_entry(m)) return RISCV_SV39_STATUS_CONFLICT;
    *mask = m ? 0 : 2;
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_leaf_missing(struct riscv_sv39_user_space *s,
    uint64_t address, unsigned *mask)
{ return riscv_sv39_user_leaf_missing_order(s,address,0,mask); }
static enum riscv_sv39_status install_prepared_leaf(struct riscv_sv39_user_space *s,
    uint64_t address, uint64_t entry, uint32_t order,
    struct kernel_mm_leaf_preparation *prepared)
{
    if (!s || s->state != RISCV_SV39_USER_SPACE_LIVE || !prepared ||
        address < BOAROS_PAGE_SIZE || address >= RISCV_SV39_USER_LIMIT ||
        (address & BOAROS_PAGE_MASK)) return RISCV_SV39_STATUS_INVALID;
    uint64_t *root = s->root_entries, *middle, *leaves;
    enum riscv_sv39_status status;
    unsigned r = (address >> 30) & RISCV_SV39_INDEX_MASK;
    unsigned m = (address >> 21) & RISCV_SV39_INDEX_MASK;
    unsigned l = (address >> BOAROS_PAGE_SHIFT) & RISCV_SV39_INDEX_MASK;
    uint64_t parent = root[r];
    if (parent && !valid_nonleaf_entry(parent)) return RISCV_SV39_STATUS_STATE;
    unsigned missing = parent ? 0 : 1;
    uint64_t middle_address = parent ? entry_address(parent) : prepared->tables[0];
    if (middle_address == KERNEL_MM_NO_PAGE) {
        prepared->table_mask=order ? 1 : 3;
        return RISCV_SV39_STATUS_NEED_TABLES;
    }
    if ((missing & 1) && !physical_page_owned_order(s->allocator,middle_address,0))
        return RISCV_SV39_STATUS_INVALID;
    status = resolve_runtime_table(s->allocator, middle_address, &middle);
    if (status != RISCV_SV39_STATUS_OK) return status;
    if (order) {
        if (middle[m]) return RISCV_SV39_STATUS_CONFLICT;
        if ((missing & 1) && s->table_pages == UINT32_MAX) return RISCV_SV39_STATUS_STATE;
        __atomic_store_n(&middle[m],entry,__ATOMIC_RELEASE);
        if (missing & 1) {
            __atomic_store_n(&root[r],table_entry(middle_address),__ATOMIC_RELEASE);
            prepared->tables[0]=KERNEL_MM_NO_PAGE; s->table_pages++;
        }
        prepared->table_mask=missing;
        return RISCV_SV39_STATUS_OK;
    }
    parent = middle[m];
    if (parent && !valid_nonleaf_entry(parent))
        return valid_user_leaf_entry(parent) || valid_protected_entry(parent)
            ? RISCV_SV39_STATUS_CONFLICT : RISCV_SV39_STATUS_STATE;
    if (!parent) missing |= 2;
    uint64_t leaf_address = parent ? entry_address(parent) : prepared->tables[1];
    if (leaf_address == KERNEL_MM_NO_PAGE) {
        prepared->table_mask=missing;
        return RISCV_SV39_STATUS_NEED_TABLES;
    }
    if (leaf_address == middle_address) return RISCV_SV39_STATUS_INVALID;
    if ((missing & 2) && !physical_page_owned_order(s->allocator,leaf_address,0))
        return RISCV_SV39_STATUS_INVALID;
    status = resolve_runtime_table(s->allocator, leaf_address, &leaves);
    if (status != RISCV_SV39_STATUS_OK) return status;
    unsigned tables = ((missing & 1) != 0) + ((missing & 2) != 0);
    if (s->table_pages > UINT32_MAX - tables) return RISCV_SV39_STATUS_STATE;
    if (leaves[l]) return RISCV_SV39_STATUS_CONFLICT;
    /* 搬移准备传零，只发布空表；普通fault仍在发布父链接前填好叶。 */
    __atomic_store_n(&leaves[l], entry, __ATOMIC_RELEASE);
    if (missing & 2) {
        __atomic_store_n(&middle[m], table_entry(leaf_address), __ATOMIC_RELEASE);
        prepared->tables[1] = KERNEL_MM_NO_PAGE;
    }
    if (missing & 1) {
        __atomic_store_n(&root[r], table_entry(middle_address), __ATOMIC_RELEASE);
        prepared->tables[0] = KERNEL_MM_NO_PAGE;
    }
    s->table_pages += tables;
    prepared->table_mask=missing;
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_prepare_move(struct riscv_sv39_user_space *s,
    uint64_t address, uint32_t order, struct kernel_mm_leaf_preparation *prepared)
{
    return install_prepared_leaf(s, address, 0, order, prepared);
}
enum riscv_sv39_status riscv_sv39_user_map_prepared_order(struct riscv_sv39_user_space *s,
    uint64_t address, uint64_t physical, uint32_t permissions, int flags, uint32_t order,
    struct kernel_mm_leaf_preparation *prepared)
{
    if ((order != 0 && order != 9) || !s || (permissions && !valid_user_permissions(permissions)) ||
        ((flags & KERNEL_MM_LEAF_FILE) && !(flags & KERNEL_MM_LEAF_COW)) ||
        ((physical|address) & (((uint64_t)BOAROS_PAGE_SIZE<<order)-1)))
        return RISCV_SV39_STATUS_INVALID;
    int cow = (flags & KERNEL_MM_LEAF_COW) != 0;
    void *owner;
    uint32_t pages=1U<<order;
    if (!physical_page_owned_order(s->allocator,physical,order) ||
        (permissions ? s->leaf_pages : s->protected_pages) > UINT32_MAX-pages ||
        (cow && s->cow_pages > UINT32_MAX-pages) ||
        physical_page_map_owned(s->allocator, physical, &owner) != PHYSICAL_PAGE_STATUS_OK)
        return RISCV_SV39_STATUS_STATE;
    enum riscv_sv39_status status = install_prepared_leaf(s, address,
        encode_user_entry(physical, permissions, flags), order, prepared);
    if (status) return status;
    if (permissions) s->leaf_pages+=pages; else s->protected_pages+=pages;
    if (cow) s->cow_pages+=pages;
    resident_peak(s);
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_map_prepared_page(struct riscv_sv39_user_space *s,
    uint64_t address, uint64_t physical, uint32_t permissions, int flags,
    struct kernel_mm_leaf_preparation *prepared)
{ return riscv_sv39_user_map_prepared_order(s,address,physical,permissions,flags,0,prepared); }
enum riscv_sv39_status riscv_sv39_user_move_leaf(struct riscv_sv39_user_space *s,
    uint64_t from, uint64_t to, uint32_t order)
{
    uint64_t *source, *target = 0;
    if ((order!=0 && order!=9) || ((from|to)&(((uint64_t)BOAROS_PAGE_SIZE<<order)-1)) || from==to)
        return RISCV_SV39_STATUS_INVALID;
    uint32_t found;
    enum riscv_sv39_status status=resolve_leaf_order(s,from,&source,&found,NULL);
    if (status) return status;
    if (found!=order) return RISCV_SV39_STATUS_STATE;
    status=resolve_leaf_order(s,to,&target,&found,NULL);
    if (status != RISCV_SV39_STATUS_NOT_MAPPED)
        return status ? status : RISCV_SV39_STATUS_CONFLICT;
    if (found!=order) return RISCV_SV39_STATUS_STATE;
    uint64_t entry = __atomic_exchange_n(source, 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(target, entry, __ATOMIC_RELEASE);
    return RISCV_SV39_STATUS_OK;
}

uint64_t riscv_sv39_user_thp_base_entry(uint64_t physical,const struct kernel_mm_leaf *leaf)
{
    return encode_user_entry(physical,leaf->permissions&~RISCV_SV39_USER,
        leaf->cow ? KERNEL_MM_LEAF_COW : 0);
}

void riscv_sv39_user_thp_install_split(struct riscv_sv39_user_space *s,
    const struct kernel_mm_leaf *leaf,uint64_t table)
{
    uint64_t *slot;
    uint32_t order;
    if (resolve_leaf_order(s,leaf->virtual_address,&slot,&order,NULL) || order!=9 ||
        !valid_protected_entry(*slot) || entry_address(*slot)!=leaf->physical_address)
        __builtin_trap();
    __atomic_store_n(slot,table_entry(table),__ATOMIC_RELEASE);
    s->table_pages++;
    if (leaf->permissions) { s->protected_pages-=1U<<order;s->leaf_pages+=1U<<order; }
    resident_peak(s);
}
/* The caller keeps MM metadata stable through inspecting or replacing this
 * leaf. Return that actual slot so commit does not repeat the same walk. */
static enum riscv_sv39_status resolve_cow_leaf(struct riscv_sv39_user_space *s,
    uint64_t address,uint64_t **leaf)
{
    if (!s || s->state != RISCV_SV39_USER_SPACE_LIVE ||
        address < BOAROS_PAGE_SIZE || address >= RISCV_SV39_USER_LIMIT ||
        (address & BOAROS_PAGE_MASK)) return RISCV_SV39_STATUS_INVALID;
    enum riscv_sv39_status status=resolve_existing_user_leaf(s,address,leaf,0);
    if (status != RISCV_SV39_STATUS_OK) return status;
    if (!present_cow_entry(**leaf) && !protected_cow_entry(**leaf))
        return (valid_user_leaf_entry(**leaf) || valid_protected_entry(**leaf)) ?
            RISCV_SV39_STATUS_NOT_MAPPED : RISCV_SV39_STATUS_STATE;
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_cow_snapshot(struct riscv_sv39_user_space *s,
    uint64_t address, uint64_t *cookie, uint64_t *physical)
{
    if (!cookie || !physical) return RISCV_SV39_STATUS_INVALID;
    uint64_t *leaf;
    enum riscv_sv39_status status=resolve_cow_leaf(s,address,&leaf);
    if (status != RISCV_SV39_STATUS_OK) return status;
    *cookie=*leaf;*physical=entry_address(*leaf);
    return RISCV_SV39_STATUS_OK;
}
static void store_cow_leaf(struct riscv_sv39_user_space *s, uint64_t *leaf,
    uint64_t old, uint64_t target, uint32_t permissions, uint32_t pages)
{
    if (valid_protected_entry(old) && permissions) {
        s->protected_pages -= pages;
        s->leaf_pages += pages;
    }
    if (!valid_protected_entry(old) && !permissions) {
        s->leaf_pages -= pages;
        s->protected_pages += pages;
    }
    __atomic_store_n(leaf, permissions ? user_leaf_entry(target, permissions) :
        protected_entry(target, 0), __ATOMIC_RELEASE);
    s->cow_pages -= pages;
}
enum riscv_sv39_status riscv_sv39_user_cow_upgrade(struct riscv_sv39_user_space *s,
    uint64_t *slot, uint64_t cookie, uint32_t permissions)
{
    if (!slot || (permissions && !valid_user_permissions(permissions)))
        return RISCV_SV39_STATUS_INVALID;
    uint64_t actual = *slot;
    if (!present_cow_entry(actual) && !protected_cow_entry(actual)) return RISCV_SV39_STATUS_NOT_MAPPED;
    if (actual != cookie) return RISCV_SV39_STATUS_CONFLICT;
    if (!s->cow_pages) return RISCV_SV39_STATUS_STATE;
    store_cow_leaf(s, slot, actual, entry_address(actual), permissions, 1);
    s->cow_in_place++;
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_cow_replace_prepared(struct riscv_sv39_user_space *s,
    uint64_t *slot, uint64_t cookie, uint64_t replacement, uint32_t permissions,
    struct kernel_mm_retirement *batch)
{
    if (*slot != cookie) return RISCV_SV39_STATUS_CONFLICT;
    store_cow_leaf(s, slot, cookie, replacement, permissions, 1);
    s->cow_copies++;
    kernel_mm_retirement_add(batch, entry_address(cookie), 0);
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_cow_commit_order(struct riscv_sv39_user_space *s,
    uint64_t address, uint64_t cookie, uint64_t replacement, uint32_t permissions, uint32_t order,
    struct kernel_mm_retirement *batch)
{
    if ((order!=0 && order!=9) || (address & (((uint64_t)BOAROS_PAGE_SIZE<<order)-1)) || !batch || batch->count >= KERNEL_MM_RETIRE_CAPACITY ||
        (permissions && !valid_user_permissions(permissions)) ||
        (replacement != KERNEL_MM_NO_PAGE && (replacement & (((uint64_t)BOAROS_PAGE_SIZE<<order)-1)))) return RISCV_SV39_STATUS_INVALID;
    uint64_t *leaf;
    uint32_t actual_order;
    enum riscv_sv39_status status=resolve_leaf_order(s,address,&leaf,&actual_order,0);
    if (status != RISCV_SV39_STATUS_OK) return status;
    if (actual_order != order) return RISCV_SV39_STATUS_INVALID;
    uint32_t pages=1U<<order;
    uint64_t actual=*leaf,physical=entry_address(actual);
    if (!present_cow_entry(actual) && !protected_cow_entry(actual)) return RISCV_SV39_STATUS_NOT_MAPPED;
    if (actual != cookie) return RISCV_SV39_STATUS_CONFLICT;
    if (s->cow_pages<pages) return RISCV_SV39_STATUS_STATE;
    if (replacement == KERNEL_MM_NO_PAGE) {
        uint32_t references;
        if (physical_page_reference_count(s->allocator, physical, &references) != PHYSICAL_PAGE_STATUS_OK)
            return RISCV_SV39_STATUS_STATE;
        if (references != 1) return RISCV_SV39_STATUS_CONFLICT;
    } else if (!physical_page_owned_order(s->allocator,replacement,order)) {
        return RISCV_SV39_STATUS_INVALID;
    }
    void *pointer;
    if (replacement != KERNEL_MM_NO_PAGE && physical_page_map_owned(s->allocator, replacement, &pointer) != PHYSICAL_PAGE_STATUS_OK)
        return RISCV_SV39_STATUS_STATE;
    uint64_t target=replacement == KERNEL_MM_NO_PAGE ? physical : replacement;
    store_cow_leaf(s, leaf, actual, target, permissions, pages);
    if (replacement != KERNEL_MM_NO_PAGE) { s->cow_copies++; kernel_mm_retirement_add_order(batch, physical, order, 0); }
    else s->cow_in_place++;
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_cow_commit(struct riscv_sv39_user_space *s,
    uint64_t address, uint64_t cookie, uint64_t replacement, uint32_t permissions,
    struct kernel_mm_retirement *batch)
{ return riscv_sv39_user_cow_commit_order(s,address,cookie,replacement,permissions,0,batch); }
static enum riscv_sv39_status next_owned_leaf(struct riscv_sv39_user_space *s,
    uint64_t *cursor, uint64_t end, uint64_t *address, uint64_t **slot, unsigned *remaining)
{
    if (!s || !cursor || !address || s->state != RISCV_SV39_USER_SPACE_LIVE ||
        *cursor > end || end > RISCV_SV39_USER_LIMIT || ((*cursor | end) & BOAROS_PAGE_MASK)) return RISCV_SV39_STATUS_INVALID;
    if (*cursor == end) return RISCV_SV39_STATUS_NOT_MAPPED;
    uint64_t *root = s->root_entries, *middle, *leaves;
    enum riscv_sv39_status status;
    unsigned first_r = *cursor >> 30, first_m = (*cursor >> 21) & RISCV_SV39_INDEX_MASK;
    unsigned first_l = (*cursor >> BOAROS_PAGE_SHIFT) & RISCV_SV39_INDEX_MASK;
    unsigned last_r = (end - 1) >> 30, last_m = ((end - 1) >> 21) & RISCV_SV39_INDEX_MASK;
    unsigned last_l = ((end - 1) >> BOAROS_PAGE_SHIFT) & RISCV_SV39_INDEX_MASK;
    const unsigned entries = RISCV_SV39_INDEX_MASK + 1U;
    for (unsigned r = next_table_entry(root, first_r, last_r + 1); r <= last_r;
         r = next_table_entry(root, r + 1, last_r + 1)) {
        if (!valid_nonleaf_entry(root[r])) return RISCV_SV39_STATUS_STATE;
        status = resolve_runtime_table(s->allocator, entry_address(root[r]), &middle);
        if (status != RISCV_SV39_STATUS_OK) return status;
        unsigned middle_limit = r == last_r ? last_m + 1 : entries;
        for (unsigned m = next_table_entry(middle, r == first_r ? first_m : 0, middle_limit);
             m < middle_limit; m = next_table_entry(middle, m + 1, middle_limit)) {
            if (valid_user_leaf_entry(middle[m]) || valid_protected_entry(middle[m])) {
                if (entry_address(middle[m]) & (RISCV_SV39_PAGE_SIZE_2M-1)) return RISCV_SV39_STATUS_STATE;
                *address=((uint64_t)r<<30)|((uint64_t)m<<21);
                *cursor=*address+RISCV_SV39_PAGE_SIZE_2M;
                if (slot) { *slot = &middle[m]; *remaining = 0; }
                return RISCV_SV39_STATUS_OK;
            }
            if (!valid_nonleaf_entry(middle[m])) return RISCV_SV39_STATUS_STATE;
            status = resolve_runtime_table(s->allocator, entry_address(middle[m]), &leaves);
            if (status != RISCV_SV39_STATUS_OK) return status;
            unsigned leaf_limit = r == last_r && m == last_m ? last_l + 1 : entries;
            unsigned l = next_table_entry(leaves,
                r == first_r && m == first_m ? first_l : 0, leaf_limit);
            if (l == leaf_limit) continue;
            if (!valid_user_leaf_entry(leaves[l]) && !valid_protected_entry(leaves[l]))
                return RISCV_SV39_STATUS_STATE;
            *address = ((uint64_t)r << 30) | ((uint64_t)m << 21) | ((uint64_t)l << BOAROS_PAGE_SHIFT);
            *cursor = *address + BOAROS_PAGE_SIZE;
            if (slot) { *slot = &leaves[l]; *remaining = leaf_limit - l; }
            return RISCV_SV39_STATUS_OK;
        }
    }
    *cursor = end;
    return RISCV_SV39_STATUS_NOT_MAPPED;
}

enum riscv_sv39_status riscv_sv39_user_next_owned(struct riscv_sv39_user_space *s,
    uint64_t *cursor, uint64_t end, uint64_t *address)
{ return next_owned_leaf(s, cursor, end, address, NULL, NULL); }
enum riscv_sv39_status riscv_sv39_user_owned_snapshot(struct riscv_sv39_user_space *s,
    uint64_t address,uint64_t *physical,int *cow)
{
    if (!s || !physical || !cow || (address & BOAROS_PAGE_MASK)) return RISCV_SV39_STATUS_INVALID;
    uint64_t *leaf;
    enum riscv_sv39_status status=resolve_existing_user_leaf(s,address,&leaf,0);
    if (status) return status;
    uint64_t entry=*leaf;
    if (!valid_user_leaf_entry(entry) && !valid_protected_entry(entry)) return RISCV_SV39_STATUS_STATE;
    *physical=entry_address(entry);
    *cow=present_cow_entry(entry)||protected_cow_entry(entry);
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status mark_cow_leaf(struct riscv_sv39_user_space *s,
    uint64_t *leaf, uint32_t order)
{
    uint32_t pages=1U<<order;
    uint64_t old = *leaf, replacement;
    if (present_cow_entry(old) || protected_cow_entry(old)) return RISCV_SV39_STATUS_OK;
    if (valid_user_leaf_entry(old)) replacement = user_cow_entry(entry_address(old), entry_permissions(old) & ~RISCV_SV39_USER);
    else if (valid_protected_entry(old)) replacement = protected_entry(entry_address(old), 1);
    else return RISCV_SV39_STATUS_STATE;
    if (s->cow_pages > UINT32_MAX-pages) return RISCV_SV39_STATUS_STATE;
    __atomic_store_n(leaf, replacement, __ATOMIC_RELEASE); s->cow_pages+=pages;
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_mark_cow_page(struct riscv_sv39_user_space *s, uint64_t address)
{
    uint64_t *leaf;
    uint32_t order;
    enum riscv_sv39_status status = resolve_leaf_order(s,address,&leaf,&order,0);
    if (status != RISCV_SV39_STATUS_OK) return status;
    if (address & (((uint64_t)BOAROS_PAGE_SIZE<<order)-1)) return RISCV_SV39_STATUS_INVALID;
    return mark_cow_leaf(s, leaf, order);
}

static inline __attribute__((always_inline)) unsigned publish_window(struct riscv_sv39_user_space *s,
    uint64_t *slot, unsigned pages, const uint64_t *physical, uint32_t permissions,
    int flags, int already_empty)
{
    int cow = (flags & KERNEL_MM_LEAF_COW) != 0;
    uint64_t attributes = encode_user_entry(0, permissions, flags);
    uint32_t *owned = permissions ? &s->leaf_pages : &s->protected_pages;
    uint32_t available = UINT32_MAX - *owned;
    if (cow && available > UINT32_MAX - s->cow_pages)
        available = UINT32_MAX - s->cow_pages;
    /* Order the prepared contents before publishing this window. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    unsigned installed = 0, count = 0;
    for (unsigned index = 0; pages; pages >>= 1, index++) {
        if (!(pages & 1)) continue;
        uint64_t p = *physical++;
        if (!already_empty && slot[index]) continue;
        if ((p & BOAROS_PAGE_MASK) || count == available) __builtin_trap();
        uint64_t entry = ((p >> BOAROS_PAGE_SHIFT) << 10U) | attributes;
        __atomic_store_n(&slot[index], entry, __ATOMIC_RELAXED);
        installed |= 1U << index;
        count++;
    }
    *owned += count;
    if (cow) s->cow_pages += count;
    resident_peak(s);
    return installed;
}
static unsigned map_owned_window(struct riscv_sv39_user_space *s, uint64_t address,
    unsigned pages, const uint64_t *physical, uint32_t permissions, int flags,
    int required_slot)
{
    if (!pages) return 0;
    int cow = (flags & KERNEL_MM_LEAF_COW) != 0;
    unsigned slots = (RISCV_SV39_PAGE_SIZE_2M - (address & (RISCV_SV39_PAGE_SIZE_2M-1))) / BOAROS_PAGE_SIZE;
    if ((address & BOAROS_PAGE_MASK) || address < BOAROS_PAGE_SIZE ||
        (slots < 32 && (pages >> slots)) || (permissions && !valid_user_permissions(permissions)) ||
        ((flags & KERNEL_MM_LEAF_FILE) && !cow))
        __builtin_trap();
    uint64_t *slot;
    uint32_t order;
    enum riscv_sv39_status result = resolve_leaf_order(s, address, &slot, &order, NULL);
    if (result && result != RISCV_SV39_STATUS_NOT_MAPPED) __builtin_trap();
    if (order || (required_slot >= 0 && slot[required_slot])) return 0;
    return publish_window(s, slot, pages, physical, permissions, flags, 0);
}

void riscv_sv39_user_publish_empty_window(struct riscv_sv39_user_space *s,
    uint64_t *slot, unsigned pages, const uint64_t *physical, uint32_t permissions, int flags)
{
    publish_window(s, slot, pages, physical, permissions, flags, 1);
}

unsigned riscv_sv39_user_map_owned_window(struct riscv_sv39_user_space *s, uint64_t address,
    unsigned pages, const uint64_t *physical, uint32_t permissions, int flags)
{
    return map_owned_window(s, address, pages, physical, permissions, flags, -1);
}

unsigned riscv_sv39_user_map_required_window(struct riscv_sv39_user_space *s, uint64_t address,
    unsigned pages, const uint64_t *physical, uint32_t permissions, int flags, unsigned required_slot)
{
    if (required_slot >= 32 || !(pages & (1U << required_slot))) __builtin_trap();
    return map_owned_window(s, address, pages, physical, permissions, flags, (int)required_slot);
}


static int user_range_overlaps(uint64_t base,
                               uint64_t span,
                               uint64_t start,
                               uint64_t end)
{
    return base < end && start < base + span;
}

static enum riscv_sv39_status validate_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end,
    int active_leaves_allowed,
    uint32_t *active_pages,
    uint32_t *protected_pages)
{
    *active_pages=0; *protected_pages=0;
    enum riscv_sv39_status status=validate_leaf_boundaries(space,start,end);
    if (status) return status;
    uint64_t cursor=start, address;
    while (!(status=riscv_sv39_user_next_owned(space,&cursor,end,&address))) {
        struct kernel_mm_leaf leaf;
        status=riscv_sv39_user_leaf_snapshot(space,address,&leaf);
        if (status) return status;
        uint32_t pages=1U<<leaf.order;
        void *owned;
        if (physical_page_map_owned(space->allocator,leaf.physical_address,&owned))
            return RISCV_SV39_STATUS_STATE;
        if (physical_page_allocator_is_finalized(space->allocator) &&
            !physical_page_owned_order(space->allocator,leaf.physical_address,leaf.order))
            return RISCV_SV39_STATUS_STATE;
        uint32_t *count=leaf.permissions?active_pages:protected_pages;
        if ((leaf.permissions && !active_leaves_allowed) || *count>UINT32_MAX-pages)
            return RISCV_SV39_STATUS_STATE;
        *count+=pages;
    }
    if (status!=RISCV_SV39_STATUS_NOT_MAPPED) return status;
    if (*active_pages>space->leaf_pages || *protected_pages>space->protected_pages ||
        space->leaf_pages>UINT32_MAX-space->protected_pages ||
        space->cow_pages>space->leaf_pages+space->protected_pages) return RISCV_SV39_STATUS_STATE;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status invalidate_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end,
    int *changed)
{
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t root_base;
    uint64_t level1_base;
    uint64_t page_base;
    uint32_t root_index;
    uint32_t level1_index;
    uint32_t level0_index;
    enum riscv_sv39_status status;

    root = space->root_entries;
    for (root_index = 0U;
         root_index < RISCV_SV39_USER_ROOT_ENTRIES;
         root_index++) {
        root_base = (uint64_t)root_index << 30U;
        if (!user_range_overlaps(root_base,
                                 UINT64_C(1) << 30U,
                                 start,
                                 end) ||
            root[root_index] == 0U) {
            continue;
        }
        status = resolve_runtime_table(space->allocator,
                                       entry_address(root[root_index]),
                                       &level1);
        if (status != RISCV_SV39_STATUS_OK) {
            __builtin_trap();
        }
        for (level1_index = 0U;
             level1_index < BOAROS_PAGE_SIZE / sizeof(*level1);
             level1_index++) {
            level1_base = root_base |
                          ((uint64_t)level1_index << 21U);
            if (!user_range_overlaps(level1_base,
                                     UINT64_C(1) << 21U,
                                     start,
                                     end) ||
                level1[level1_index] == 0U) {
                continue;
            }
            uint64_t old=level1[level1_index];
            if (valid_user_leaf_entry(old) || valid_protected_entry(old)) {
                if (valid_user_leaf_entry(old)) space->leaf_pages-=512; else space->protected_pages-=512;
                if (present_cow_entry(old)||protected_cow_entry(old)) space->cow_pages-=512;
                __atomic_store_n(&level1[level1_index],invalidated_entry(entry_address(old)),__ATOMIC_RELEASE);
                *changed=1; continue;
            }
            status = resolve_runtime_table(
                space->allocator,
                entry_address(level1[level1_index]),
                &level0);
            if (status != RISCV_SV39_STATUS_OK) {
                __builtin_trap();
            }
            for (level0_index = 0U;
                 level0_index < BOAROS_PAGE_SIZE / sizeof(*level0);
                 level0_index++) {
                page_base = level1_base |
                            ((uint64_t)level0_index <<
                             BOAROS_PAGE_SHIFT);
                if (page_base < start || page_base >= end ||
                    (!valid_user_leaf_entry(level0[level0_index]) &&
                     !valid_protected_entry(level0[level0_index]))) {
                    continue;
                }
                if ((valid_user_leaf_entry(level0[level0_index]) &&
                     space->leaf_pages == 0U) ||
                    (valid_protected_entry(level0[level0_index]) &&
                     space->protected_pages == 0U)) {
                    __builtin_trap();
                }
                if (valid_user_leaf_entry(level0[level0_index])) {
                    space->leaf_pages--;
                } else {
                    space->protected_pages--;
                }
                if ((present_cow_entry(level0[level0_index]) ||
                     protected_cow_entry(level0[level0_index]))) {
                    if (space->cow_pages == 0U) {
                        __builtin_trap();
                    }
                    space->cow_pages--;
                }
                level0[level0_index] = invalidated_entry(
                    entry_address(level0[level0_index]));
                *changed = 1;
            }
        }
    }
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status release_invalidated_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end)
{
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t root_base;
    uint64_t level1_base;
    uint64_t page_base;
    uint32_t root_index;
    uint32_t level1_index;
    uint32_t level0_index;
    enum riscv_sv39_status status;

    root = space->root_entries;
    for (root_index = 0U;
         root_index < RISCV_SV39_USER_ROOT_ENTRIES;
         root_index++) {
        root_base = (uint64_t)root_index << 30U;
        if (!user_range_overlaps(root_base,
                                 UINT64_C(1) << 30U,
                                 start,
                                 end) ||
            root[root_index] == 0U) {
            continue;
        }
        status = resolve_runtime_table(space->allocator,
                                       entry_address(root[root_index]),
                                       &level1);
        if (status != RISCV_SV39_STATUS_OK) {
            __builtin_trap();
        }
        for (level1_index = 0U;
             level1_index < BOAROS_PAGE_SIZE / sizeof(*level1);
             level1_index++) {
            level1_base = root_base |
                          ((uint64_t)level1_index << 21U);
            if (!user_range_overlaps(level1_base,
                                     UINT64_C(1) << 21U,
                                     start,
                                     end) ||
                level1[level1_index] == 0U) {
                continue;
            }
            if (invalidated_owned_entry(level1[level1_index])) {
                physical_page_release_order(space->allocator,entry_address(level1[level1_index]),9);
                level1[level1_index]=0; continue;
            }
            status = resolve_runtime_table(
                space->allocator,
                entry_address(level1[level1_index]),
                &level0);
            if (status != RISCV_SV39_STATUS_OK) {
                __builtin_trap();
            }
            for (level0_index = 0U;
                 level0_index < BOAROS_PAGE_SIZE / sizeof(*level0);
                 level0_index++) {
                page_base = level1_base |
                            ((uint64_t)level0_index <<
                             BOAROS_PAGE_SHIFT);
                if (page_base < start || page_base >= end ||
                    !invalidated_owned_entry(level0[level0_index])) {
                    continue;
                }
                (void)physical_page_release(
                    space->allocator,
                    entry_address(level0[level0_index]));
                level0[level0_index] = 0U;
            }
        }
    }
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_unmap_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end)
{
    int changed = 0;
    uint32_t active_pages;
    uint32_t protected_pages;
    enum riscv_sv39_status status;

    if (space == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (start < RISCV_SV39_PAGE_SIZE_4K || start >= end ||
        end > RISCV_SV39_USER_LIMIT ||
        (start & BOAROS_PAGE_MASK) != 0U ||
        (end & BOAROS_PAGE_MASK) != 0U) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = validate_owned_range(space,
                                  start,
                                  end,
                                  1,
                                  &active_pages,
                                  &protected_pages);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = invalidate_owned_range(space, start, end, &changed);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    if (changed != 0) {
        riscv_mmu_flush_all();
    }
    return release_invalidated_range(space,
                                 start,
                                 end);
}

static enum riscv_sv39_status protect_leaf(struct riscv_sv39_user_space *space,
    uint64_t *leaf, uint32_t permissions)
{
    if (space->leaf_pages > UINT32_MAX - space->protected_pages ||
        space->cow_pages > space->leaf_pages + space->protected_pages)
        return RISCV_SV39_STATUS_STATE;
    uint64_t old = *leaf, replacement;
    int present = valid_user_leaf_entry(old);
    if (!present && !valid_protected_entry(old)) return RISCV_SV39_STATUS_STATE;
    void *owned_page;
    if (physical_page_map_owned(space->allocator, entry_address(old), &owned_page) !=
            PHYSICAL_PAGE_STATUS_OK) return RISCV_SV39_STATUS_STATE;
    int cow = present ? present_cow_entry(old) : protected_cow_entry(old);
    if ((present && space->leaf_pages == 0) ||
        (!present && space->protected_pages == 0) ||
        (cow && space->cow_pages == 0)) return RISCV_SV39_STATUS_STATE;
    if (permissions == 0) {
        if (!present) return RISCV_SV39_STATUS_OK;
        if (space->protected_pages == UINT32_MAX) return RISCV_SV39_STATUS_STATE;
        replacement = encode_user_entry(entry_address(old), 0, user_entry_flags(old));
        space->leaf_pages--;
        space->protected_pages++;
    } else {
        if (!present) {
            if (space->leaf_pages == UINT32_MAX) return RISCV_SV39_STATUS_STATE;
            space->protected_pages--;
            space->leaf_pages++;
        }
        replacement = encode_user_entry(entry_address(old), permissions, user_entry_flags(old));
    }
    if (replacement != old) {
        __atomic_store_n(leaf, replacement, __ATOMIC_RELEASE);
    }
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_protect_resident_page(
    struct riscv_sv39_user_space *space, uint64_t address, uint32_t permissions)
{
    uint64_t *leaf;
    if (space == 0 || address < BOAROS_PAGE_SIZE ||
        address >= RISCV_SV39_USER_LIMIT || (address & BOAROS_PAGE_MASK) != 0 ||
        (permissions != 0 && !valid_user_permissions(permissions)))
        return RISCV_SV39_STATUS_INVALID;
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) return RISCV_SV39_STATUS_STATE;
    enum riscv_sv39_status status = resolve_existing_user_leaf(space, address, &leaf, &space->protect_visits);
    return status ? status : protect_leaf(space, leaf, permissions);
}

enum riscv_sv39_status riscv_sv39_user_protect_batch(struct riscv_sv39_user_space *space,
    uint64_t *cursor, uint64_t begin, uint64_t end,
    uint32_t (*permissions)(void *, uint64_t), void *context)
{
    uint64_t address, *entries;
    unsigned count;
    enum riscv_sv39_status status = next_owned_leaf(space, cursor, end, &address, &entries, &count);
    if (status) return status;
    if (!count) {
        if (address < begin || *cursor > end) return RISCV_SV39_STATUS_INVALID;
        return riscv_sv39_user_protect_owned_range(space, address, *cursor,
                                                 permissions(context, address));
    }
    /* The page-table writer keeps this leaf table alive for the whole batch.
     * Resume by address after dropping the lock. */
    if (count > 16) count = 16;
    space->protect_visits += count;
    COST_ADD(MPROTECT_PTE_VISITS, count);
    for (unsigned i = 0; i < count; i++, address += BOAROS_PAGE_SIZE) {
        if (!entries[i]) continue;
        status = protect_leaf(space, &entries[i], permissions(context, address));
        if (status) return status;
    }
    *cursor = address;
    return RISCV_SV39_STATUS_OK;
}
enum riscv_sv39_status riscv_sv39_user_protect_owned_page(
    struct riscv_sv39_user_space *space, uint64_t address, uint32_t permissions)
{
    enum riscv_sv39_status status=riscv_sv39_user_protect_resident_page(space,address,permissions);
    return status==RISCV_SV39_STATUS_NOT_MAPPED ? RISCV_SV39_STATUS_OK : status;
}

enum riscv_sv39_status riscv_sv39_user_protect_owned_range(
    struct riscv_sv39_user_space *space,
    uint64_t start,
    uint64_t end,
    uint32_t permissions)
{
    if (start < end && end - start == BOAROS_PAGE_SIZE) {
        enum riscv_sv39_status status=riscv_sv39_user_protect_owned_page(space,start,permissions);
        if (status == RISCV_SV39_STATUS_OK) {
            riscv_mmu_flush_address(start);
            space->protect_address_flushes++;
            COST_ADD(MPROTECT_ADDRESS_TLB,1);
            if (permissions & RISCV_SV39_EXECUTE) riscv_mmu_sync_instructions();
        }
        return status;
    }
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    uint64_t root_base;
    uint64_t level1_base;
    uint64_t page_base;
    uint64_t replacement;
    uint32_t root_index;
    uint32_t level1_index;
    uint32_t level0_index;
    uint32_t active_pages;
    uint32_t protected_pages;
    int changed = 0;
    enum riscv_sv39_status status;

    if (space == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (start < RISCV_SV39_PAGE_SIZE_4K || start >= end ||
        end > RISCV_SV39_USER_LIMIT ||
        (start & BOAROS_PAGE_MASK) != 0U ||
        (end & BOAROS_PAGE_MASK) != 0U ||
        (permissions != 0U && !valid_user_permissions(permissions))) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = validate_owned_range(space,
                                  start,
                                  end,
                                  1,
                                  &active_pages,
                                  &protected_pages);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    if ((permissions == 0U &&
         active_pages > UINT32_MAX - space->protected_pages) ||
        (permissions != 0U &&
         protected_pages > UINT32_MAX - space->leaf_pages)) {
        return RISCV_SV39_STATUS_STATE;
    }
    if ((permissions & RISCV_SV39_EXECUTE) != 0U &&
        (active_pages != 0U || protected_pages != 0U)) {
        riscv_mmu_sync_instructions();
    }
    root = space->root_entries;
    for (root_index = 0U;
         root_index < RISCV_SV39_USER_ROOT_ENTRIES;
         root_index++) {
        space->protect_visits++;
        COST_ADD(MPROTECT_PTE_VISITS, 1);
        root_base = (uint64_t)root_index << 30U;
        if (!user_range_overlaps(root_base,
                                 UINT64_C(1) << 30U,
                                 start,
                                 end) ||
            root[root_index] == 0U) {
            continue;
        }
        status = resolve_runtime_table(space->allocator,
                                       entry_address(root[root_index]),
                                       &level1);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
        for (level1_index = 0U;
             level1_index < BOAROS_PAGE_SIZE / sizeof(*level1);
             level1_index++) {
            space->protect_visits++;
        COST_ADD(MPROTECT_PTE_VISITS, 1);
            level1_base = root_base |
                          ((uint64_t)level1_index << 21U);
            if (!user_range_overlaps(level1_base,
                                     UINT64_C(1) << 21U,
                                     start,
                                     end) ||
                level1[level1_index] == 0U) {
                continue;
            }
            uint64_t old=level1[level1_index];
            if (valid_user_leaf_entry(old) || valid_protected_entry(old)) {
                uint64_t replacement=encode_user_entry(entry_address(old), permissions, user_entry_flags(old));
                if (replacement!=old) {
                    __atomic_store_n(&level1[level1_index],replacement,__ATOMIC_RELEASE); changed=1;
                }
                if (permissions && valid_protected_entry(old)) { space->protected_pages-=512; space->leaf_pages+=512; }
                if (!permissions && valid_user_leaf_entry(old)) { space->leaf_pages-=512; space->protected_pages+=512; }
                continue;
            }
            status = resolve_runtime_table(
                space->allocator,
                entry_address(level1[level1_index]),
                &level0);
            if (status != RISCV_SV39_STATUS_OK) {
                return status;
            }
            for (level0_index = 0U;
                 level0_index < BOAROS_PAGE_SIZE / sizeof(*level0);
                 level0_index++) {
                space->protect_visits++;
        COST_ADD(MPROTECT_PTE_VISITS, 1);
                page_base = level1_base |
                            ((uint64_t)level0_index <<
                             BOAROS_PAGE_SHIFT);
                if (page_base < start || page_base >= end) {
                    continue;
                }
                uint64_t leaf = level0[level0_index];
                if (valid_user_leaf_entry(leaf)) {
                    replacement = encode_user_entry(entry_address(leaf), permissions, user_entry_flags(leaf));
                    if (replacement != level0[level0_index]) {
                        level0[level0_index] = replacement;
                        changed = 1;
                    }
                    if (permissions == 0U) {
                        space->leaf_pages--;
                        space->protected_pages++;
                    }
                } else if (permissions != 0U && valid_protected_entry(leaf)) {
                    level0[level0_index] = encode_user_entry(entry_address(leaf), permissions,
                        user_entry_flags(leaf));
                    space->protected_pages--;
                    space->leaf_pages++;
                    changed = 1;
                }
            }
        }
    }
    if (changed != 0) {
        space->protect_global_flushes++;
        COST_ADD(MPROTECT_GLOBAL_TLB, 1);
        riscv_mmu_flush_all();
    }
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_space_populate(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    const void *bytes,
    size_t size)
{
    const unsigned char *source = bytes;
    struct kernel_mm_leaf mapping;
    unsigned char *page;
    uint64_t current;
    uint64_t chunk;
    uint64_t copied = 0U;
    uint64_t user_satp;
    void *pointer;
    size_t index;

    if (space == 0 || space->state != RISCV_SV39_USER_SPACE_LIVE ||
        (size != 0U && bytes == 0) ||
        virtual_address < RISCV_SV39_PAGE_SIZE_4K ||
        virtual_address >= RISCV_SV39_USER_LIMIT ||
        (uint64_t)size > RISCV_SV39_USER_LIMIT - virtual_address ||
        riscv_sv39_user_space_satp(space, &user_satp) !=
            RISCV_SV39_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (riscv_sv39_current_satp() == user_satp) {
        return RISCV_SV39_STATUS_STATE;
    }

    while (copied < (uint64_t)size) {
        current = virtual_address + copied;
        if (riscv_sv39_user_leaf_snapshot(space, current, &mapping) !=
            RISCV_SV39_STATUS_OK || !mapping.permissions) {
            return RISCV_SV39_STATUS_NOT_MAPPED;
        }
        if (physical_page_map_owned(
                space->allocator,
                mapping.physical_address,
                &pointer) != PHYSICAL_PAGE_STATUS_OK) {
            return RISCV_SV39_STATUS_STATE;
        }
        page = (unsigned char *)pointer + ((current & ~BOAROS_PAGE_MASK)-mapping.virtual_address);
        chunk = BOAROS_PAGE_SIZE - (current & BOAROS_PAGE_MASK);
        if (chunk > (uint64_t)size - copied) {
            chunk = (uint64_t)size - copied;
        }
        for (index = 0U; index < (size_t)chunk; index++) {
            page[(current & BOAROS_PAGE_MASK) + index] =
                source[(size_t)copied + index];
        }
        copied += chunk;
    }
    return RISCV_SV39_STATUS_OK;
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

static enum riscv_sv39_status init_forked_user_space(
    struct riscv_sv39_user_space *destination,
    const struct riscv_sv39_user_space *source)
{
    struct riscv_sv39_user_space result = {0};
    uint64_t *source_root = source->root_entries;
    uint64_t *destination_root;
    uint64_t root_address;
    uint32_t index;
    enum physical_page_status page_status;
    enum riscv_sv39_status status;

    page_status = physical_page_allocate(source->allocator, &root_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        return RISCV_SV39_STATUS_NO_MEMORY;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status == PHYSICAL_PAGE_STATUS_STATE
                   ? RISCV_SV39_STATUS_STATE
                   : RISCV_SV39_STATUS_INVALID;
    }
    status = resolve_runtime_table(source->allocator,
                                   root_address,
                                   &destination_root);
    if (status != RISCV_SV39_STATUS_OK) {
        (void)physical_page_release(source->allocator, root_address);
        return status;
    }

    clear_runtime_table(destination_root);
    for (index = RISCV_SV39_USER_ROOT_ENTRIES;
         index < BOAROS_PAGE_SIZE / sizeof(*destination_root);
         index++) {
        destination_root[index] = source_root[index];
    }
    result.allocator = source->allocator;
    result.root_address = root_address;
    result.root_entries = destination_root;
    result.table_pages = 1U;
    result.leaf_pages = 0U;
    result.protected_pages = 0U;
    result.cow_pages = 0U;
    result.cow_copies = 0U;
    result.cow_in_place = 0U;
    result.state = RISCV_SV39_USER_SPACE_LIVE;
    *destination = result;
    return RISCV_SV39_STATUS_OK;
}

/* Same encoding for a private fork leaf at either page size. Shared
 * mappings already have their final permissions, including PROT_NONE. */
static inline __attribute__((always_inline)) enum riscv_sv39_status fork_leaf_entry(
    uint64_t old, int action, uint64_t *entry)
{
    if (action == RISCV_SV39_FORK_SHARED) {
        if (old & RISCV_SV39_PTE_COW)
            return RISCV_SV39_STATUS_STATE;
        *entry = old;
    } else if (action == RISCV_SV39_FORK_PRIVATE) {
        /* Both callers have validated the leaf. Preserve its frame and file
         * marker. A present COW leaf is accessed and has neither W nor D. */
        const uint64_t keep = ~RISCV_SV39_PTE_FLAGS_MASK | RISCV_SV39_PTE_VALID |
            RISCV_SV39_PTE_READ | RISCV_SV39_PTE_EXECUTE | RISCV_SV39_PTE_USER |
            RISCV_SV39_PTE_FILE;
        *entry = old & RISCV_SV39_PTE_VALID ?
            (old & keep) | RISCV_SV39_PTE_ACCESSED | RISCV_SV39_PTE_COW :
            old | RISCV_SV39_PTE_COW;
    } else return RISCV_SV39_STATUS_STATE;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status share_huge_leaf(struct riscv_sv39_user_space *child,
    uint64_t address, uint64_t old, int action)
{
    uint64_t entry, *slot;
    enum riscv_sv39_status status = fork_leaf_entry(old, action, &entry);
    if (status) return status;
    if (action == RISCV_SV39_FORK_PRIVATE && child->cow_pages > UINT32_MAX-512)
        return RISCV_SV39_STATUS_STATE;
    status = resolve_user_order_slot(child, address, &slot, 9);
    if (status) return status;
    if (physical_page_acquire_order(child->allocator, entry_address(old), 9))
        return RISCV_SV39_STATUS_STATE;
    *slot = entry;
    if (valid_protected_entry(old)) child->protected_pages += 512;
    else child->leaf_pages += 512;
    if (action == RISCV_SV39_FORK_PRIVATE) child->cow_pages += 512;
    resident_peak(child);
    return RISCV_SV39_STATUS_OK;
}

static __attribute__((noinline)) enum riscv_sv39_status share_leaf_table(
    struct riscv_sv39_user_space *child, const uint64_t *parent_entries,
    uint64_t base, int (*policy)(void *, uint64_t), void *context)
{
    const unsigned batch = PHYSICAL_PAGE_REFERENCE_BATCH_MAX;
    const unsigned limit = RISCV_SV39_INDEX_MASK + 1U;
    uint64_t *child_entries = NULL;
    for (unsigned index = 0; index < limit;) {
        uint64_t refs[PHYSICAL_PAGE_REFERENCE_BATCH_MAX];
        uint64_t entries[PHYSICAL_PAGE_REFERENCE_BATCH_MAX];
        uint16_t slots[PHYSICAL_PAGE_REFERENCE_BATCH_MAX];
        unsigned count = 0, protected = 0, cow = 0;
        while (index < limit && count < batch) {
            unsigned slot = index++;
            uint64_t old = parent_entries[slot];
            if (!old) {
                index = next_table_entry(parent_entries, index, limit);
                continue;
            }
            int present = valid_user_leaf_entry(old);
            if (!present && !valid_protected_entry(old))
                return RISCV_SV39_STATUS_STATE;
            uint64_t address = base | ((uint64_t)slot << BOAROS_PAGE_SHIFT);
            int action = policy ? policy(context, address) : RISCV_SV39_FORK_PRIVATE;
            if (action == RISCV_SV39_FORK_SKIP) continue;
            enum riscv_sv39_status status = fork_leaf_entry(old, action, &entries[count]);
            if (status) return status;
            refs[count] = entry_address(old);
            slots[count++] = slot;
            protected += !present;
            cow += action == RISCV_SV39_FORK_PRIVATE;
        }
        if (!count) break;
        if (child->cow_pages > UINT32_MAX-cow) return RISCV_SV39_STATUS_STATE;
        if (!child_entries) {
            uint64_t *slot;
            uint64_t address = base | ((uint64_t)slots[0] << BOAROS_PAGE_SHIFT);
            enum riscv_sv39_status status = resolve_user_order_slot(child, address, &slot, 0);
            if (status) return status;
            child_entries = slot - slots[0];
        }
        /* Complete fallible work before taking references. Pack up to 16
         * inherited leaves across holes within this private child table. */
        physical_page_acquire_batch(child->allocator, refs, count);
        for (unsigned i = 0; i < count; i++)
            child_entries[slots[i]] = entries[i];
        child->protected_pages += protected;
        child->leaf_pages += count-protected;
        child->cow_pages += cow;
        resident_peak(child);
    }
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_space_fork_prepare(
    struct riscv_sv39_user_space *destination,
    struct riscv_sv39_user_space *source,
    int (*fork_leaf)(void *context, uint64_t virtual_address),
    void *context)
{
    uint64_t *root;
    uint64_t *level1;
    uint64_t *level0;
    enum riscv_sv39_status status;

    if (destination == 0 || source == 0 || destination == source) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (destination->state != RISCV_SV39_USER_SPACE_EMPTY ||
        source->state != RISCV_SV39_USER_SPACE_LIVE ||
        source->allocator == 0) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = init_forked_user_space(destination, source);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    root = source->root_entries;
    const unsigned root_limit = RISCV_SV39_USER_ROOT_ENTRIES;
    const unsigned middle_limit = RISCV_SV39_INDEX_MASK + 1U;
    for (unsigned root_index = next_table_entry(root, 0, root_limit);
         root_index < root_limit;
         root_index = next_table_entry(root, root_index + 1, root_limit)) {
        if (!valid_nonleaf_entry(root[root_index])) {
            return RISCV_SV39_STATUS_STATE;
        }
        status = resolve_runtime_table(source->allocator,
                                       entry_address(root[root_index]),
                                       &level1);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
        for (unsigned level1_index = next_table_entry(level1, 0, middle_limit);
             level1_index < middle_limit;
             level1_index = next_table_entry(level1, level1_index + 1, middle_limit)) {
            if (valid_user_leaf_entry(level1[level1_index]) || valid_protected_entry(level1[level1_index])) {
                uint64_t va=((uint64_t)root_index<<30)|((uint64_t)level1_index<<21);
                int action=fork_leaf?fork_leaf(context,va):RISCV_SV39_FORK_PRIVATE;
                if (action==RISCV_SV39_FORK_SKIP) continue;
                status=share_huge_leaf(destination,va,level1[level1_index],action);
                if (status) return status;
                continue;
            }
            if (!valid_nonleaf_entry(level1[level1_index])) {
                return RISCV_SV39_STATUS_STATE;
            }
            status = resolve_runtime_table(
                source->allocator,
                entry_address(level1[level1_index]),
                &level0);
            if (status != RISCV_SV39_STATUS_OK) {
                return status;
            }
            uint64_t base = ((uint64_t)root_index << 30) | ((uint64_t)level1_index << 21);
            status = share_leaf_table(destination, level0, base, fork_leaf, context);
            if (status) return status;
        }
    }
    return RISCV_SV39_STATUS_OK;
}

/* Prepare rejects COW on shared leaves. The child COW bit therefore records
 * the private policy, and replacing a long-pinned page in the child clears it.
 * The parent operation gate keeps the PTEs stable while we find changed slots.
 * Publish up to 16 changes per raw lock hold, with no walk or policy callback
 * in the critical section. */
static enum riscv_sv39_status fork_commit_leaf_table(struct riscv_sv39_user_space *parent,
    uint64_t *parent_entries, const uint64_t *child_entries,
    struct kernel_raw_rwlock *metadata, int *changed)
{
    enum { WINDOW = 16, ENTRIES = RISCV_SV39_INDEX_MASK + 1U };
    for (unsigned index = 0; index < ENTRIES;) {
        index = next_table_entry(child_entries, index, ENTRIES);
        if (index == ENTRIES) break;
        unsigned first = index & ~(WINDOW - 1U);
        unsigned pending = 0, count = 0;
        for (; index < first + WINDOW; index++) {
            uint64_t inherited = child_entries[index];
            if (!(inherited & RISCV_SV39_PTE_COW)) continue;
            uint64_t old = parent_entries[index];
            if (entry_address(old) != entry_address(inherited)) return RISCV_SV39_STATUS_STATE;
            if (old & RISCV_SV39_PTE_COW) continue;
            pending |= 1U << (index - first);
            count++;
        }
        if (!pending) continue;

        struct kernel_raw_guard guard __attribute__((cleanup(kernel_raw_scope_release))) = {0};
        if (metadata) kernel_raw_rwlock_write_acquire(metadata, &guard);
        if (parent->cow_pages > UINT32_MAX - count) return RISCV_SV39_STATUS_STATE;
        /* Prepare already validated and encoded these private leaves. A
         * parent leaf that is still writable cannot hold a file COW marker.
         * Keep each full-width PTE store atomic and order the batch once
         * before the existing parent TLB synchronization. */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for (unsigned slot = first; pending; pending >>= 1, slot++) {
            if (!(pending & 1U)) continue;
            __atomic_store_n(&parent_entries[slot], child_entries[slot], __ATOMIC_RELAXED);
        }
        parent->cow_pages += count;
        *changed = 1;
    }
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_space_fork_commit(
    struct riscv_sv39_user_space *parent, const struct riscv_sv39_user_space *child,
    struct kernel_raw_rwlock *metadata, int *changed)
{
    const unsigned root_limit = RISCV_SV39_USER_ROOT_ENTRIES;
    const unsigned middle_limit = RISCV_SV39_INDEX_MASK + 1U;
    const uint64_t *child_root = child->root_entries;
    const uint64_t *parent_root = parent->root_entries;
    if (!child->cow_pages) return RISCV_SV39_STATUS_OK;

    for (unsigned r = next_table_entry(child_root, 0, root_limit);
         r < root_limit; r = next_table_entry(child_root, r + 1, root_limit)) {
        uint64_t *parent_middle, *child_middle;
        if (!valid_nonleaf_entry(child_root[r]) || !valid_nonleaf_entry(parent_root[r]))
            return RISCV_SV39_STATUS_STATE;
        enum riscv_sv39_status status = resolve_runtime_table(parent->allocator,
            entry_address(parent_root[r]), &parent_middle);
        if (!status) status = resolve_runtime_table(child->allocator,
            entry_address(child_root[r]), &child_middle);
        if (status) return status;

        for (unsigned m = next_table_entry(child_middle, 0, middle_limit);
             m < middle_limit; m = next_table_entry(child_middle, m + 1, middle_limit)) {
            uint64_t inherited = child_middle[m], old = parent_middle[m];
            if (valid_nonleaf_entry(inherited)) {
                uint64_t *parent_entries, *child_entries;
                if (!valid_nonleaf_entry(old)) return RISCV_SV39_STATUS_STATE;
                status = resolve_runtime_table(parent->allocator, entry_address(old), &parent_entries);
                if (!status) status = resolve_runtime_table(child->allocator,
                    entry_address(inherited), &child_entries);
                if (!status) status = fork_commit_leaf_table(parent, parent_entries,
                    child_entries, metadata, changed);
                if (status) return status;
                continue;
            }
            if (!(inherited & RISCV_SV39_PTE_COW)) continue;
            if (entry_address(inherited) != entry_address(old)) return RISCV_SV39_STATUS_STATE;
            if (old & RISCV_SV39_PTE_COW) continue;

            struct kernel_raw_guard guard __attribute__((cleanup(kernel_raw_scope_release))) = {0};
            if (metadata) kernel_raw_rwlock_write_acquire(metadata, &guard);
            status = mark_cow_leaf(parent, &parent_middle[m], 9);
            if (status) return status;
            *changed = 1;
        }
    }
    return RISCV_SV39_STATUS_OK;
}

enum riscv_sv39_status riscv_sv39_user_space_fork(
    struct riscv_sv39_user_space *destination, struct riscv_sv39_user_space *source,
    int (*fork_leaf)(void *, uint64_t), void *context)
{
    enum riscv_sv39_status status = riscv_sv39_user_space_fork_prepare(
        destination, source, fork_leaf, context);
    if (status) return status;
    int changed = 0;
    status = riscv_sv39_user_space_fork_commit(source, destination, NULL, &changed);
    if (!status) riscv_mmu_flush_all();
    return status;
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
    source->root_entries = NULL;
    source->table_pages = 0U;
    source->leaf_pages = 0U;
    source->protected_pages = 0U;
    source->cow_pages = 0U;
    source->cow_copies = 0U;
    source->cow_in_place = 0U;
    source->protect_visits = 0;
    source->protect_address_flushes = 0;
    source->protect_global_flushes = 0;
    source->state = RISCV_SV39_USER_SPACE_MOVED;
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
    (void)physical_page_release(space->allocator,
                              entry_address(saved_entry));
    space->table_pages--;
    return RISCV_SV39_STATUS_OK;
}

static enum riscv_sv39_status destroy_level0(
    struct riscv_sv39_user_space *space,
    uint64_t *parent_entry)
{
    uint64_t *entries;
    enum riscv_sv39_status status;

    if (!valid_nonleaf_entry(*parent_entry)) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = resolve_runtime_table(space->allocator,
                                   entry_address(*parent_entry),
                                   &entries);
    if (status != RISCV_SV39_STATUS_OK) {
        __builtin_trap();
    }
    uint64_t refs[PHYSICAL_PAGE_REFERENCE_BATCH_MAX];
    unsigned count=0;
    uint32_t present = 0, protected = 0, cow = 0;
    const unsigned limit = RISCV_SV39_INDEX_MASK + 1U;
    /* The root entry was detached and any remote walks have completed.
     * Classify once and account for this leaf table as a unit. */
    unsigned index = next_table_entry(entries, 0, limit);
    for (; index < limit;
         index = next_table_entry(entries, index + 1, limit)) {
        uint64_t old=entries[index];
        if (valid_user_leaf_entry(old)) {
            if (++present > space->leaf_pages) break;
            if ((old & RISCV_SV39_PTE_COW) && ++cow > space->cow_pages) break;
        } else if (valid_protected_entry(old)) {
            if (++protected > space->protected_pages) break;
            if (protected_cow_entry(old) && ++cow > space->cow_pages) break;
        } else break;
        refs[count++]=entry_address(old);
        if (count==PHYSICAL_PAGE_REFERENCE_BATCH_MAX) {
            physical_page_release_batch(space->allocator,refs,count);
            count=0;
        }
    }
    physical_page_release_batch(space->allocator,refs,count);
    if (index != limit) return RISCV_SV39_STATUS_STATE;
    space->leaf_pages -= present;
    space->protected_pages -= protected;
    space->cow_pages -= cow;
    return release_child_table(space, parent_entry);
}

static enum riscv_sv39_status destroy_level1(
    struct riscv_sv39_user_space *space,
    uint64_t *parent_entry)
{
    uint64_t *entries;
    enum riscv_sv39_status status;

    if (!valid_nonleaf_entry(*parent_entry)) {
        return RISCV_SV39_STATUS_STATE;
    }
    status = resolve_runtime_table(space->allocator,
                                   entry_address(*parent_entry),
                                   &entries);
    if (status != RISCV_SV39_STATUS_OK) {
        __builtin_trap();
    }
    const unsigned limit = RISCV_SV39_INDEX_MASK + 1U;
    for (unsigned index = next_table_entry(entries, 0, limit); index < limit;
         index = next_table_entry(entries, index + 1, limit)) {
        uint64_t old=entries[index];
        int present = valid_user_leaf_entry(old);
        if (present || valid_protected_entry(old)) {
            const unsigned pages = 1U << 9;
            uint32_t *total = present ? &space->leaf_pages : &space->protected_pages;
            int cow = present ? (old & RISCV_SV39_PTE_COW) != 0 : protected_cow_entry(old);
            if (*total < pages || (cow && space->cow_pages < pages))
                return RISCV_SV39_STATUS_STATE;
            *total -= pages;
            if (cow) space->cow_pages -= pages;
            physical_page_release_order(space->allocator,entry_address(old),9);
            status = RISCV_SV39_STATUS_OK;
        } else status=destroy_level0(space,&entries[index]);
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
    enum riscv_sv39_status status;

    if (space == 0) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (space->state != RISCV_SV39_USER_SPACE_LIVE) {
        return RISCV_SV39_STATUS_STATE;
    }
    if (space->table_pages != 0U) {
        satp = riscv_sv39_current_satp();
        root_ppn = space->root_address >> BOAROS_PAGE_SHIFT;
        if ((satp >> 60U) == 8U &&
            (satp & RISCV_SV39_SATP_PPN_MASK) == root_ppn) {
            return RISCV_SV39_STATUS_STATE;
        }
    }
    if (space->table_pages == 0U) {
        if (space->root_address != 0U || space->root_entries || space->leaf_pages != 0U ||
            space->protected_pages != 0U ||
            space->cow_pages != 0U) {
            return RISCV_SV39_STATUS_STATE;
        }
        space->allocator = 0;
        space->state = RISCV_SV39_USER_SPACE_DESTROYED;
        return RISCV_SV39_STATUS_OK;
    }
    root = space->root_entries;

    uint32_t references;
    if (physical_page_reference_count(space->allocator,space->root_address,&references))
        __builtin_trap();
    const unsigned limit = RISCV_SV39_USER_ROOT_ENTRIES;
    for (unsigned index = next_table_entry(root, 0, limit); index < limit;) {
        uint64_t detached[16];unsigned count=0;
        while (index < limit && count < 16) {
            if (!valid_nonleaf_entry(root[index])) return RISCV_SV39_STATUS_STATE;
            detached[count++]=root[index];
            __atomic_store_n(&root[index],0,__ATOMIC_RELEASE);
            index = next_table_entry(root, index + 1, limit);
        }
        /* Root-only borrowers can still have speculative user walks. Detach
         * their parent entries and finish every RFENCE before freeing children. */
        if (count && references>1) kernel_window_sync(0,UINT64_MAX);
        for (unsigned i=0;i<count;i++) {
            status=destroy_level1(space,&detached[i]);
            if (status) return status;
        }
    }
    if (space->leaf_pages != 0U || space->protected_pages != 0U ||
        space->cow_pages != 0U ||
        space->table_pages != 1U) {
        return RISCV_SV39_STATUS_STATE;
    }
    (void)physical_page_release(space->allocator,
                              space->root_address);

    space->allocator = 0;
    space->root_address = 0U;
    space->root_entries = NULL;
    space->table_pages = 0U;
    space->leaf_pages = 0U;
    space->protected_pages = 0U;
    space->cow_pages = 0U;
    space->cow_copies = 0U;
    space->cow_in_place = 0U;
    space->protect_visits = 0;
    space->protect_address_flushes = 0;
    space->protect_global_flushes = 0;
    space->state = RISCV_SV39_USER_SPACE_DESTROYED;
    return RISCV_SV39_STATUS_OK;
}
