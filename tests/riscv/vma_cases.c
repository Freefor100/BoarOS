#include <arch/riscv/mm.h>
#include <arch/riscv/sv39.h>
#include <kernel/boot_memory.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/vma.h>

#include <stddef.h>
#include <stdint.h>

#define VMA_TEST_PAGE_COUNT 128U
#define VMA_TEST_TEXT_BASE UINT64_C(0x10000)
#define VMA_TEST_TEXT_END UINT64_C(0x13000)
#define VMA_TEST_POLICY_BASE UINT64_C(0x20000)
#define VMA_TEST_ORPHAN_PTE_BASE UINT64_C(0x31000)
#define VMA_TEST_HEAP_BASE UINT64_C(0x1000000)
#define VMA_TEST_HEAP_LIMIT UINT64_C(0x2000000)
#define VMA_TEST_STACK_BASE UINT64_C(0x3f800000)
#define VMA_TEST_STACK_END UINT64_C(0x40000000)

static unsigned char vma_page_pool[BOAROS_PAGE_SIZE * VMA_TEST_PAGE_COUNT]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t held_pages[VMA_TEST_PAGE_COUNT];
static int force_vma_release_failure;
static int force_vma_resize_failure;
static int use_test_satp;
static uint64_t test_satp;
static int force_fault_page_access_failure;
static int force_fault_page_release_failure;
static uint64_t fault_page_address;

enum kernel_heap_status __real_kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer);
enum kernel_heap_status __real_kernel_heap_resize(
    struct kernel_heap *heap,
    void *old_pointer,
    size_t new_size,
    void **new_pointer);
uint64_t __real_riscv_sv39_current_satp(void);
enum physical_page_status __real_physical_page_allocate(
    struct physical_page_allocator *allocator,
    uint64_t *address);
enum physical_page_status __real_physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address);

uint64_t __wrap_riscv_sv39_current_satp(void)
{
    return use_test_satp != 0 ? test_satp
                              : __real_riscv_sv39_current_satp();
}

enum physical_page_status __wrap_physical_page_allocate(
    struct physical_page_allocator *allocator,
    uint64_t *address)
{
    enum physical_page_status status;

    status = __real_physical_page_allocate(allocator, address);
    if (status == PHYSICAL_PAGE_STATUS_OK &&
        force_fault_page_access_failure != 0) {
        fault_page_address = *address;
    }
    return status;
}

enum physical_page_status __wrap_physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address)
{
    if (force_fault_page_release_failure != 0 &&
        address == fault_page_address) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }
    return __real_physical_page_release(allocator, address);
}

enum kernel_heap_status __wrap_kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer)
{
    if (force_vma_release_failure != 0) {
        return KERNEL_HEAP_STATUS_STATE;
    }
    return __real_kernel_heap_release(heap, pointer);
}

enum kernel_heap_status __wrap_kernel_heap_resize(
    struct kernel_heap *heap,
    void *old_pointer,
    size_t new_size,
    void **new_pointer)
{
    if (force_vma_resize_failure != 0) {
        return KERNEL_HEAP_STATUS_EMPTY;
    }
    return __real_kernel_heap_resize(heap,
                                     old_pointer,
                                     new_size,
                                     new_pointer);
}

static void *vma_page_access(uint64_t address)
{
    uint64_t base = (uint64_t)(uintptr_t)vma_page_pool;

    if (force_fault_page_access_failure != 0 &&
        address == fault_page_address) {
        return 0;
    }
    if (address < base || address - base >= sizeof(vma_page_pool)) {
        return 0;
    }
    return &vma_page_pool[address - base];
}

static int vma_page_address(const void *pointer, uint64_t *address)
{
    uintptr_t base = (uintptr_t)vma_page_pool;
    uintptr_t value = (uintptr_t)pointer;

    if (address == 0 || value < base ||
        value - base >= sizeof(vma_page_pool)) {
        return 0;
    }
    *address = (uint64_t)value;
    return 1;
}

static int page_is_zero(const void *pointer)
{
    const unsigned char *bytes = pointer;
    size_t index;

    for (index = 0U; index < BOAROS_PAGE_SIZE; index++) {
        if (bytes[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static int setup(struct physical_page_allocator *allocator,
                 struct riscv_sv39_page_table *kernel_table,
                 struct kernel_heap *heap,
                 uint64_t *baseline)
{
    struct boot_memory_layout layout = {0};

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)vma_page_pool;
    layout.usable[0].size = sizeof(vma_page_pool);
    if (physical_page_allocator_init(allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(allocator, vma_page_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(allocator) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(kernel_table, allocator) !=
            RISCV_SV39_STATUS_OK ||
        kernel_heap_init(heap, allocator, vma_page_address) !=
            KERNEL_HEAP_STATUS_OK) {
        return 0;
    }
    kernel_table->state = RISCV_SV39_STATE_ACTIVE;
    *baseline = physical_page_available(allocator);
    return 1;
}

static int create_mm(struct physical_page_allocator *allocator,
                     const struct riscv_sv39_page_table *kernel_table,
                     struct kernel_mm *mm)
{
    struct riscv_sv39_user_space space = {0};

    if (riscv_sv39_user_space_init(&space, allocator, kernel_table) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(&space,
                                        VMA_TEST_TEXT_BASE,
                                        RISCV_SV39_READ |
                                            RISCV_SV39_EXECUTE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(&space,
                                        VMA_TEST_ORPHAN_PTE_BASE,
                                        RISCV_SV39_READ |
                                            RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_kernel_mm_create(mm, &space) != KERNEL_MM_STATUS_OK) {
        if (space.state == RISCV_SV39_USER_SPACE_LIVE ||
            space.state == RISCV_SV39_USER_SPACE_CLEANUP) {
            (void)riscv_sv39_user_space_destroy(&space);
        }
        return 0;
    }
    return 1;
}

unsigned long run_all_vma_cases(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_heap heap;
    struct kernel_heap foreign_heap = {0};
    struct kernel_mm parent = {0};
    struct kernel_mm child = {0};
    struct kernel_mm cleanup_mm = {0};
    struct kernel_vma descriptor;
    struct kernel_mm_mapping mapping;
    struct kernel_heap_statistics statistics;
    void *page;
    uint64_t baseline;
    uint64_t available_before_fork;
    uint64_t available_before_oom;
    uint64_t available_before_reclaim;
    uint64_t brk_result;
    uint64_t heap_page_address;
    uint64_t parent_satp;
    uint64_t cleanup_satp;
    uint32_t held_count;
    uint32_t index;
    enum kernel_mm_status brk_status;
    enum physical_page_status page_status;

    if (!setup(&allocator, &kernel_table, &heap, &baseline) ||
        !create_mm(&allocator, &kernel_table, &parent) ||
        kernel_mm_vma_enable(&parent, &foreign_heap) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT ||
        kernel_mm_vma_enable(&parent, &heap) != KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_BASE,
                                  VMA_TEST_TEXT_BASE + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ | KERNEL_MM_EXECUTE,
                                  KERNEL_VMA_ROLE_ELF,
                                  KERNEL_VMA_FAULT_RESIDENT_REQUIRED) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_BASE + BOAROS_PAGE_SIZE,
                                  VMA_TEST_TEXT_END,
                                  KERNEL_MM_READ | KERNEL_MM_EXECUTE,
                                  KERNEL_VMA_ROLE_ELF,
                                  KERNEL_VMA_FAULT_RESIDENT_REQUIRED) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_POLICY_BASE,
                                  VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ | KERNEL_MM_WRITE,
                                  KERNEL_VMA_ROLE_STACK,
                                  KERNEL_VMA_FAULT_RESIDENT_REQUIRED) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(
            &parent,
            VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE,
            VMA_TEST_POLICY_BASE + 2U * BOAROS_PAGE_SIZE,
            KERNEL_MM_READ | KERNEL_MM_WRITE,
            KERNEL_VMA_ROLE_STACK,
            KERNEL_VMA_FAULT_DEMAND_ZERO) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_STACK_BASE,
                                  VMA_TEST_STACK_END,
                                  KERNEL_MM_READ | KERNEL_MM_WRITE,
                                  KERNEL_VMA_ROLE_STACK,
                                  KERNEL_VMA_FAULT_DEMAND_ZERO) !=
            KERNEL_MM_STATUS_OK) {
        return 1U;
    }
    if (kernel_mm_vma_lookup(&parent,
                             VMA_TEST_TEXT_BASE + UINT64_C(0x800),
                             &descriptor) != KERNEL_MM_STATUS_OK ||
        descriptor.start != VMA_TEST_TEXT_BASE ||
        descriptor.end != VMA_TEST_TEXT_END ||
        descriptor.permissions !=
            (KERNEL_MM_READ | KERNEL_MM_EXECUTE) ||
        descriptor.kind != KERNEL_VMA_KIND_ANONYMOUS ||
        descriptor.role != KERNEL_VMA_ROLE_ELF ||
        descriptor.fault_policy !=
            KERNEL_VMA_FAULT_RESIDENT_REQUIRED ||
        kernel_mm_vma_lookup(
            &parent,
            VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE - 1U,
            &descriptor) != KERNEL_MM_STATUS_OK ||
        descriptor.start != VMA_TEST_POLICY_BASE ||
        descriptor.end != VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE ||
        descriptor.fault_policy !=
            KERNEL_VMA_FAULT_RESIDENT_REQUIRED ||
        kernel_mm_vma_lookup(
            &parent,
            VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE,
            &descriptor) != KERNEL_MM_STATUS_OK ||
        descriptor.start != VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE ||
        descriptor.end != VMA_TEST_POLICY_BASE + 2U * BOAROS_PAGE_SIZE ||
        descriptor.fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO ||
        kernel_mm_vma_lookup(&parent,
                             VMA_TEST_TEXT_END,
                             &descriptor) != KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_BASE + BOAROS_PAGE_SIZE,
                                  VMA_TEST_TEXT_END + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ | KERNEL_MM_EXECUTE,
                                  KERNEL_VMA_ROLE_ELF,
                                  KERNEL_VMA_FAULT_RESIDENT_REQUIRED) !=
            KERNEL_MM_STATUS_CONFLICT ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_END,
                                  VMA_TEST_TEXT_END + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ,
                                  (enum kernel_vma_role)-1,
                                  KERNEL_VMA_FAULT_RESIDENT_REQUIRED) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_END,
                                  VMA_TEST_TEXT_END + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ,
                                  KERNEL_VMA_ROLE_ELF,
                                  (enum kernel_vma_fault_policy)-1) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT) {
        return 2U;
    }
    if (riscv_kernel_mm_satp(&parent, &parent_satp) !=
            KERNEL_MM_STATUS_OK) {
        return 3U;
    }
    use_test_satp = 1;
    test_satp = parent_satp ^ UINT64_C(1);
    if (kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_STACK_BASE,
                                     KERNEL_MM_READ) !=
            KERNEL_MM_STATUS_STATE) {
        use_test_satp = 0;
        return 4U;
    }
    test_satp = parent_satp;
    if (kernel_mm_brk_initialize(&parent,
                                  VMA_TEST_HEAP_BASE,
                                  VMA_TEST_HEAP_LIMIT) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_brk(&parent, 0U, &brk_result) !=
            KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE ||
        kernel_mm_brk(&parent,
                       VMA_TEST_HEAP_BASE +
                           2U * BOAROS_PAGE_SIZE + UINT64_C(0x321),
                       &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE +
                          2U * BOAROS_PAGE_SIZE + UINT64_C(0x321) ||
        kernel_mm_vma_lookup(&parent,
                             VMA_TEST_HEAP_BASE +
                                 2U * BOAROS_PAGE_SIZE,
                             &descriptor) != KERNEL_MM_STATUS_OK ||
        descriptor.start != VMA_TEST_HEAP_BASE ||
        descriptor.end != VMA_TEST_HEAP_BASE + 3U * BOAROS_PAGE_SIZE ||
        descriptor.permissions != (KERNEL_MM_READ | KERNEL_MM_WRITE) ||
        descriptor.role != KERNEL_VMA_ROLE_HEAP ||
        descriptor.fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO ||
        kernel_mm_lookup(&parent,
                         VMA_TEST_HEAP_BASE,
                         &mapping) != KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_HEAP_BASE,
                                     KERNEL_MM_WRITE) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(&parent,
                         VMA_TEST_HEAP_BASE,
                         &mapping) != KERNEL_MM_STATUS_OK) {
        use_test_satp = 0;
        return 19U;
    }
    heap_page_address = mapping.physical_address & ~BOAROS_PAGE_MASK;
    if (kernel_mm_resolve_user_fault(
            &parent,
            VMA_TEST_HEAP_BASE + 2U * BOAROS_PAGE_SIZE,
            KERNEL_MM_WRITE) != KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(&parent,
                         VMA_TEST_HEAP_BASE + 2U * BOAROS_PAGE_SIZE,
                         &mapping) != KERNEL_MM_STATUS_OK ||
        physical_page_resolve(
            &allocator,
            mapping.physical_address & ~BOAROS_PAGE_MASK,
            &page) != PHYSICAL_PAGE_STATUS_OK) {
        use_test_satp = 0;
        return 19U;
    }
    ((unsigned char *)page)[0] = 0xa5U;
    fault_page_address = mapping.physical_address & ~BOAROS_PAGE_MASK;
    force_fault_page_release_failure = 1;
    available_before_reclaim = physical_page_available(&allocator);
    brk_status = kernel_mm_brk(&parent,
                               VMA_TEST_HEAP_BASE + UINT64_C(0x321),
                               &brk_result);
    if (brk_status != KERNEL_MM_STATUS_OK) {
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return UINT64_C(0x200) + (unsigned long)brk_status;
    }
    if (brk_result != VMA_TEST_HEAP_BASE + UINT64_C(0x321)) {
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return 22U;
    }
    if (kernel_mm_lookup(&parent,
                         VMA_TEST_HEAP_BASE + 2U * BOAROS_PAGE_SIZE,
                         &mapping) != KERNEL_MM_STATUS_NOT_MAPPED) {
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return 23U;
    }
    if (kernel_mm_vma_lookup(&parent,
                             VMA_TEST_HEAP_BASE + BOAROS_PAGE_SIZE,
                             &descriptor) != KERNEL_MM_STATUS_NOT_MAPPED) {
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return 24U;
    }
    if (physical_page_available(&allocator) != available_before_reclaim) {
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return 25U;
    }
    if (kernel_mm_brk(&parent,
                       VMA_TEST_HEAP_BASE +
                           2U * BOAROS_PAGE_SIZE + UINT64_C(0x321),
                       &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE + UINT64_C(0x321) ||
        kernel_mm_vma_lookup(&parent,
                             VMA_TEST_HEAP_BASE + BOAROS_PAGE_SIZE,
                             &descriptor) != KERNEL_MM_STATUS_NOT_MAPPED ||
        physical_page_available(&allocator) != available_before_reclaim) {
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return 27U;
    }
    if (kernel_mm_fork(&child, &parent) != KERNEL_MM_STATUS_OK ||
        kernel_mm_brk(&child, 0U, &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE + UINT64_C(0x321) ||
        kernel_mm_lookup(&child,
                         VMA_TEST_HEAP_BASE + 2U * BOAROS_PAGE_SIZE,
                         &mapping) != KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_release(&child) != KERNEL_MM_STATUS_OK) {
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return 26U;
    }
    child = (struct kernel_mm){0};
    force_fault_page_release_failure = 0;
    if (kernel_mm_brk(&parent,
                       VMA_TEST_HEAP_BASE +
                           2U * BOAROS_PAGE_SIZE + UINT64_C(0x321),
                       &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE +
                          2U * BOAROS_PAGE_SIZE + UINT64_C(0x321) ||
        physical_page_available(&allocator) !=
            available_before_reclaim + 1U ||
        kernel_mm_resolve_user_fault(
            &parent,
            VMA_TEST_HEAP_BASE + 2U * BOAROS_PAGE_SIZE,
            KERNEL_MM_READ) != KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(&parent,
                         VMA_TEST_HEAP_BASE + 2U * BOAROS_PAGE_SIZE,
                         &mapping) != KERNEL_MM_STATUS_OK ||
        physical_page_resolve(
            &allocator,
            mapping.physical_address & ~BOAROS_PAGE_MASK,
            &page) != PHYSICAL_PAGE_STATUS_OK ||
        !page_is_zero(page) ||
        kernel_mm_brk(&parent,
                       VMA_TEST_HEAP_BASE + UINT64_C(0x321),
                       &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE + UINT64_C(0x321) ||
        kernel_mm_brk(&parent,
                       VMA_TEST_HEAP_BASE - 1U,
                       &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE + UINT64_C(0x321) ||
        kernel_mm_brk(&parent,
                       VMA_TEST_HEAP_LIMIT + BOAROS_PAGE_SIZE,
                       &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE + UINT64_C(0x321)) {
        use_test_satp = 0;
        return 21U;
    }
    fault_page_address = 0U;
    if (kernel_mm_resolve_user_fault(
            &parent,
            VMA_TEST_STACK_BASE + UINT64_C(0x321),
            KERNEL_MM_READ) != KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(&parent,
                         VMA_TEST_STACK_BASE + UINT64_C(0x321),
                         &mapping) != KERNEL_MM_STATUS_OK ||
        mapping.permissions != (KERNEL_MM_READ | KERNEL_MM_WRITE |
                                KERNEL_MM_USER) ||
        physical_page_resolve(
            &allocator,
            mapping.physical_address & ~BOAROS_PAGE_MASK,
            &page) != PHYSICAL_PAGE_STATUS_OK ||
        !page_is_zero(page) ||
        kernel_mm_resolve_user_fault(
            &parent,
            VMA_TEST_STACK_BASE + BOAROS_PAGE_SIZE,
            KERNEL_MM_WRITE) != KERNEL_MM_STATUS_OK ||
        kernel_mm_resolve_user_fault(
            &parent,
            VMA_TEST_STACK_BASE + 2U * BOAROS_PAGE_SIZE,
            KERNEL_MM_EXECUTE) != KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_lookup(&parent,
                         VMA_TEST_STACK_BASE + 2U * BOAROS_PAGE_SIZE,
                         &mapping) != KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_TEXT_BASE,
                                     KERNEL_MM_WRITE) !=
            KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_TEXT_BASE,
                                     KERNEL_MM_EXECUTE) !=
            KERNEL_MM_STATUS_ADDRESS_SPACE ||
        kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_POLICY_BASE,
                                     KERNEL_MM_READ) !=
            KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_resolve_user_fault(&parent,
                                     UINT64_C(0x30000),
                                     KERNEL_MM_READ) !=
            KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_ORPHAN_PTE_BASE,
                                     KERNEL_MM_READ) !=
            KERNEL_MM_STATUS_ADDRESS_SPACE ||
        kernel_mm_resolve_user_fault(
            &parent,
            RISCV_SV39_USER_LIMIT,
            KERNEL_MM_READ) != KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_STACK_BASE,
                                     0U) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT ||
        kernel_mm_resolve_user_fault(&parent,
                                     VMA_TEST_STACK_BASE,
                                     KERNEL_MM_READ | KERNEL_MM_WRITE) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT) {
        use_test_satp = 0;
        return 5U;
    }
    available_before_oom = physical_page_available(&allocator);
    held_count = 0U;
    while (held_count < VMA_TEST_PAGE_COUNT) {
        page_status = physical_page_allocate(&allocator,
                                             &held_pages[held_count]);
        if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
            break;
        }
        if (page_status != PHYSICAL_PAGE_STATUS_OK) {
            use_test_satp = 0;
            return 6U;
        }
        held_count++;
    }
    if (kernel_mm_resolve_user_fault(
            &parent,
            VMA_TEST_STACK_BASE + 3U * BOAROS_PAGE_SIZE,
            KERNEL_MM_WRITE) != KERNEL_MM_STATUS_NO_MEMORY ||
        parent.state != KERNEL_MM_LIVE ||
        kernel_mm_lookup(&parent,
                         VMA_TEST_STACK_BASE +
                             3U * BOAROS_PAGE_SIZE,
                         &mapping) != KERNEL_MM_STATUS_NOT_MAPPED) {
        use_test_satp = 0;
        return 7U;
    }
    for (index = 0U; index < held_count; index++) {
        if (physical_page_release(&allocator, held_pages[index]) !=
            PHYSICAL_PAGE_STATUS_OK) {
            use_test_satp = 0;
            return 8U;
        }
    }
    if (physical_page_available(&allocator) != available_before_oom) {
        use_test_satp = 0;
        return 9U;
    }
    if (!create_mm(&allocator, &kernel_table, &cleanup_mm) ||
        kernel_mm_vma_enable(&cleanup_mm, &heap) != KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(
            &cleanup_mm,
            VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE,
            VMA_TEST_POLICY_BASE + 2U * BOAROS_PAGE_SIZE,
            KERNEL_MM_READ | KERNEL_MM_WRITE,
            KERNEL_VMA_ROLE_STACK,
            KERNEL_VMA_FAULT_DEMAND_ZERO) != KERNEL_MM_STATUS_OK ||
        riscv_kernel_mm_satp(&cleanup_mm, &cleanup_satp) !=
            KERNEL_MM_STATUS_OK) {
        use_test_satp = 0;
        return 10U;
    }
    test_satp = cleanup_satp;
    fault_page_address = 0U;
    force_fault_page_access_failure = 1;
    force_fault_page_release_failure = 1;
    if (kernel_mm_resolve_user_fault(
            &cleanup_mm,
            VMA_TEST_POLICY_BASE + BOAROS_PAGE_SIZE,
            KERNEL_MM_WRITE) != KERNEL_MM_STATUS_CLEANUP_REQUIRED ||
        fault_page_address == 0U) {
        force_fault_page_access_failure = 0;
        force_fault_page_release_failure = 0;
        use_test_satp = 0;
        return 11U;
    }
    force_fault_page_access_failure = 0;
    force_fault_page_release_failure = 0;
    fault_page_address = 0U;
    test_satp = parent_satp;
    if (kernel_mm_release(&cleanup_mm) != KERNEL_MM_STATUS_OK) {
        use_test_satp = 0;
        return 12U;
    }
    use_test_satp = 0;
    available_before_fork = physical_page_available(&allocator);
    force_vma_resize_failure = 1;
    brk_status = kernel_mm_fork(&child, &parent);
    if (brk_status != KERNEL_MM_STATUS_NO_MEMORY) {
        force_vma_resize_failure = 0;
        return UINT64_C(0x300) + (unsigned long)brk_status;
    }
    if (child.state != KERNEL_MM_EMPTY || child.allocator != 0 ||
        child.record_page_address != 0U ||
        child.cleanup_stage != KERNEL_MM_CLEANUP_NONE ||
        physical_page_available(&allocator) != available_before_fork) {
        force_vma_resize_failure = 0;
        return 13U;
    }
    force_vma_resize_failure = 0;
    if (kernel_mm_fork(&child, &parent) != KERNEL_MM_STATUS_OK ||
        kernel_mm_brk(&child, 0U, &brk_result) != KERNEL_MM_STATUS_OK ||
        brk_result != VMA_TEST_HEAP_BASE + UINT64_C(0x321) ||
        kernel_mm_vma_lookup(&child,
                             VMA_TEST_STACK_END - 1U,
                             &descriptor) != KERNEL_MM_STATUS_OK ||
        descriptor.start != VMA_TEST_STACK_BASE ||
        descriptor.end != VMA_TEST_STACK_END ||
        descriptor.permissions != (KERNEL_MM_READ | KERNEL_MM_WRITE) ||
        descriptor.kind != KERNEL_VMA_KIND_ANONYMOUS ||
        descriptor.role != KERNEL_VMA_ROLE_STACK ||
        descriptor.fault_policy != KERNEL_VMA_FAULT_DEMAND_ZERO) {
        return 14U;
    }
    if (kernel_mm_release(&child) != KERNEL_MM_STATUS_OK) {
        return 15U;
    }
    force_vma_release_failure = 1;
    if (kernel_mm_release(&parent) != KERNEL_MM_STATUS_CLEANUP_REQUIRED ||
        parent.state != KERNEL_MM_CLEANUP ||
        parent.cleanup_stage != KERNEL_MM_CLEANUP_VMAS) {
        force_vma_release_failure = 0;
        return 16U;
    }
    force_vma_release_failure = 0;
    fault_page_address = heap_page_address;
    force_fault_page_release_failure = 1;
    if (kernel_mm_release(&parent) != KERNEL_MM_STATUS_CLEANUP_REQUIRED ||
        parent.state != KERNEL_MM_CLEANUP ||
        parent.cleanup_stage != KERNEL_MM_CLEANUP_SPACE) {
        force_fault_page_release_failure = 0;
        return 17U;
    }
    force_fault_page_release_failure = 0;
    fault_page_address = 0U;
    if (kernel_mm_release(&parent) != KERNEL_MM_STATUS_OK) {
        return 28U;
    }
    kernel_heap_get_statistics(&heap, &statistics);
    if (statistics.live_allocations != 0U ||
        statistics.current_pages != 0U ||
        physical_page_available(&allocator) != baseline) {
        return 18U;
    }
    return 0U;
}
