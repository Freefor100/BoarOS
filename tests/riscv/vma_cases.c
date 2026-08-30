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
#define VMA_TEST_STACK_BASE UINT64_C(0x3f800000)
#define VMA_TEST_STACK_END UINT64_C(0x40000000)

static unsigned char vma_page_pool[BOAROS_PAGE_SIZE * VMA_TEST_PAGE_COUNT]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static int force_vma_release_failure;
static int force_vma_resize_failure;

enum kernel_heap_status __real_kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer);
enum kernel_heap_status __real_kernel_heap_resize(
    struct kernel_heap *heap,
    void *old_pointer,
    size_t new_size,
    void **new_pointer);

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
    struct kernel_vma descriptor;
    struct kernel_heap_statistics statistics;
    uint64_t baseline;
    uint64_t available_before_fork;

    if (!setup(&allocator, &kernel_table, &heap, &baseline) ||
        !create_mm(&allocator, &kernel_table, &parent) ||
        kernel_mm_vma_enable(&parent, &foreign_heap) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT ||
        kernel_mm_vma_enable(&parent, &heap) != KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_BASE,
                                  VMA_TEST_TEXT_BASE + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ | KERNEL_MM_EXECUTE,
                                  KERNEL_VMA_ROLE_ELF) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_BASE + BOAROS_PAGE_SIZE,
                                  VMA_TEST_TEXT_END,
                                  KERNEL_MM_READ | KERNEL_MM_EXECUTE,
                                  KERNEL_VMA_ROLE_ELF) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_STACK_BASE,
                                  VMA_TEST_STACK_END,
                                  KERNEL_MM_READ | KERNEL_MM_WRITE,
                                  KERNEL_VMA_ROLE_STACK) !=
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
        kernel_mm_vma_lookup(&parent,
                             VMA_TEST_TEXT_END,
                             &descriptor) != KERNEL_MM_STATUS_NOT_MAPPED ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_BASE + BOAROS_PAGE_SIZE,
                                  VMA_TEST_TEXT_END + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ | KERNEL_MM_EXECUTE,
                                  KERNEL_VMA_ROLE_ELF) !=
            KERNEL_MM_STATUS_CONFLICT ||
        kernel_mm_vma_insert_anon(&parent,
                                  VMA_TEST_TEXT_END,
                                  VMA_TEST_TEXT_END + BOAROS_PAGE_SIZE,
                                  KERNEL_MM_READ,
                                  (enum kernel_vma_role)-1) !=
            KERNEL_MM_STATUS_INVALID_ARGUMENT) {
        return 2U;
    }
    available_before_fork = physical_page_available(&allocator);
    force_vma_resize_failure = 1;
    if (kernel_mm_fork(&child, &parent) != KERNEL_MM_STATUS_NO_MEMORY ||
        child.state != KERNEL_MM_EMPTY || child.allocator != 0 ||
        child.record_page_address != 0U ||
        child.cleanup_stage != KERNEL_MM_CLEANUP_NONE ||
        physical_page_available(&allocator) != available_before_fork) {
        force_vma_resize_failure = 0;
        return 3U;
    }
    force_vma_resize_failure = 0;
    if (kernel_mm_fork(&child, &parent) != KERNEL_MM_STATUS_OK ||
        kernel_mm_vma_lookup(&child,
                             VMA_TEST_STACK_END - 1U,
                             &descriptor) != KERNEL_MM_STATUS_OK ||
        descriptor.start != VMA_TEST_STACK_BASE ||
        descriptor.end != VMA_TEST_STACK_END ||
        descriptor.permissions != (KERNEL_MM_READ | KERNEL_MM_WRITE) ||
        descriptor.kind != KERNEL_VMA_KIND_ANONYMOUS ||
        descriptor.role != KERNEL_VMA_ROLE_STACK) {
        return 4U;
    }
    if (kernel_mm_release(&child) != KERNEL_MM_STATUS_OK) {
        return 5U;
    }
    force_vma_release_failure = 1;
    if (kernel_mm_release(&parent) != KERNEL_MM_STATUS_CLEANUP_REQUIRED ||
        parent.state != KERNEL_MM_CLEANUP ||
        parent.cleanup_stage != KERNEL_MM_CLEANUP_VMAS) {
        force_vma_release_failure = 0;
        return 6U;
    }
    force_vma_release_failure = 0;
    if (kernel_mm_release(&parent) != KERNEL_MM_STATUS_OK) {
        return 7U;
    }
    kernel_heap_get_statistics(&heap, &statistics);
    if (statistics.live_allocations != 0U ||
        statistics.current_pages != 0U ||
        physical_page_available(&allocator) != baseline) {
        return 8U;
    }
    return 0U;
}
