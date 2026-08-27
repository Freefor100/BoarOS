#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#if BOAROS_PAGE_SHIFT != 12
#error "the RISC-V target requires 4 KiB pages"
#endif

#define TEST_PHYSICAL_BASE UINT64_C(0x48000000)
#define TEST_POOL_PAGES 256U

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_POOL_PAGES]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

static void *mapped_page_access(uint64_t address)
{
    if (address < TEST_PHYSICAL_BASE ||
        address - TEST_PHYSICAL_BASE >= sizeof(page_pool)) {
        return 0;
    }

    return &page_pool[address - TEST_PHYSICAL_BASE];
}

static int mapped_page_address(const void *pointer, uint64_t *address)
{
    uintptr_t start = (uintptr_t)&page_pool[0];
    uintptr_t value = (uintptr_t)pointer;

    if (address == 0 || value < start || value - start >= sizeof(page_pool)) {
        return 0;
    }

    *address = TEST_PHYSICAL_BASE + (uint64_t)(value - start);
    return 1;
}

static void fail_heap(unsigned long case_id,
                      enum kernel_heap_status expected,
                      enum kernel_heap_status actual)
    __attribute__((noreturn));

static void fail_heap(unsigned long case_id,
                      enum kernel_heap_status expected,
                      enum kernel_heap_status actual)
{
    virt_uart_puts("BoarOS: kernel heap test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex((unsigned long)expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex((unsigned long)actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static void fail_value(unsigned long case_id,
                       unsigned long expected,
                       unsigned long actual)
    __attribute__((noreturn));

static void fail_value(unsigned long case_id,
                       unsigned long expected,
                       unsigned long actual)
{
    virt_uart_puts("BoarOS: kernel heap value failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex(expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex(actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static void initialize_heap(struct physical_page_allocator *allocator,
                            struct kernel_heap *heap)
{
    struct boot_memory_layout layout;
    enum physical_page_status page_status;
    enum kernel_heap_status heap_status;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = sizeof(page_pool);

    page_status = physical_page_allocator_init(allocator, &layout);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        fail_value(1U, PHYSICAL_PAGE_STATUS_OK, page_status);
    }
    page_status = physical_page_allocator_bind_access(allocator,
                                                       mapped_page_access);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        fail_value(2U, PHYSICAL_PAGE_STATUS_OK, page_status);
    }
    page_status = physical_page_allocator_finalize(allocator);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        fail_value(3U, PHYSICAL_PAGE_STATUS_OK, page_status);
    }

    heap_status = kernel_heap_init(heap, allocator, mapped_page_address);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        fail_heap(4U, KERNEL_HEAP_STATUS_OK, heap_status);
    }
}

static void test_small_classes_reuse_and_release_pages(void)
{
    static const size_t sizes[] = {1U, 16U, 17U, 63U, 513U, 2048U};
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    void *objects[sizeof(sizes) / sizeof(sizes[0])];
    uint64_t available;
    size_t index;

    initialize_heap(&allocator, &heap);
    available = physical_page_available(&allocator);

    for (index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        enum kernel_heap_status status =
            kernel_heap_allocate(&heap, sizes[index], &objects[index]);

        if (status != KERNEL_HEAP_STATUS_OK) {
            fail_heap(10U + index, KERNEL_HEAP_STATUS_OK, status);
        }
        if (objects[index] == 0 ||
            ((uintptr_t)objects[index] & (KERNEL_HEAP_ALIGNMENT - 1U)) != 0U) {
            fail_value(20U + index, 0U, (unsigned long)objects[index]);
        }
    }

    for (index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        enum kernel_heap_status status =
            kernel_heap_release(&heap, objects[index]);

        if (status != KERNEL_HEAP_STATUS_OK) {
            fail_heap(30U + index, KERNEL_HEAP_STATUS_OK, status);
        }
    }

    if (physical_page_available(&allocator) != available) {
        fail_value(40U,
                   (unsigned long)available,
                   (unsigned long)physical_page_available(&allocator));
    }
}

static void test_exact_page_uses_one_buddy_page(void)
{
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    uint64_t before;
    void *pointer = 0;
    uint32_t order = UINT32_MAX;
    uint64_t address;
    enum kernel_heap_status heap_status;
    enum physical_page_status page_status;

    initialize_heap(&allocator, &heap);
    before = physical_page_available(&allocator);

    heap_status = kernel_heap_allocate(&heap, BOAROS_PAGE_SIZE, &pointer);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        fail_heap(50U, KERNEL_HEAP_STATUS_OK, heap_status);
    }
    if (((uintptr_t)pointer & BOAROS_PAGE_MASK) != 0U ||
        physical_page_available(&allocator) + 1U != before) {
        fail_value(51U,
                   (unsigned long)(before - 1U),
                   (unsigned long)physical_page_available(&allocator));
    }
    if (!mapped_page_address(pointer, &address)) {
        fail_value(52U, 1U, 0U);
    }
    page_status = physical_page_allocation_order(&allocator,
                                                  address,
                                                  &order);
    if (page_status != PHYSICAL_PAGE_STATUS_OK || order != 0U) {
        fail_value(53U, 0U, order);
    }

    heap_status = kernel_heap_release(&heap, pointer);
    if (heap_status != KERNEL_HEAP_STATUS_OK ||
        physical_page_available(&allocator) != before) {
        fail_heap(54U, KERNEL_HEAP_STATUS_OK, heap_status);
    }
}

static void test_zero_calloc_overflow_and_realloc(void)
{
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    unsigned char *small = 0;
    unsigned char *large = 0;
    void *zero = (void *)(uintptr_t)1U;
    void *unchanged = (void *)(uintptr_t)UINT64_C(0x1122334455667788);
    size_t index;
    enum kernel_heap_status status;

    initialize_heap(&allocator, &heap);

    status = kernel_heap_allocate(&heap, 0U, &zero);
    if (status != KERNEL_HEAP_STATUS_OK || zero != 0) {
        fail_heap(60U, KERNEL_HEAP_STATUS_OK, status);
    }
    status = kernel_heap_allocate_zeroed(&heap, SIZE_MAX, 2U, &unchanged);
    if (status != KERNEL_HEAP_STATUS_OVERFLOW ||
        unchanged != (void *)(uintptr_t)UINT64_C(0x1122334455667788)) {
        fail_heap(61U, KERNEL_HEAP_STATUS_OVERFLOW, status);
    }

    status = kernel_heap_allocate_zeroed(&heap, 64U, 2U, (void **)&small);
    if (status != KERNEL_HEAP_STATUS_OK) {
        fail_heap(62U, KERNEL_HEAP_STATUS_OK, status);
    }
    for (index = 0U; index < 128U; index++) {
        if (small[index] != 0U) {
            fail_value(63U, 0U, small[index]);
        }
        small[index] = (unsigned char)(index ^ 0x5aU);
    }

    status = kernel_heap_resize(&heap,
                                small,
                                BOAROS_PAGE_SIZE * 3U,
                                (void **)&large);
    if (status != KERNEL_HEAP_STATUS_OK || large == small) {
        fail_heap(64U, KERNEL_HEAP_STATUS_OK, status);
    }
    for (index = 0U; index < 128U; index++) {
        if (large[index] != (unsigned char)(index ^ 0x5aU)) {
            fail_value(65U,
                       (unsigned long)(unsigned char)(index ^ 0x5aU),
                       large[index]);
        }
    }
    status = kernel_heap_release(&heap, large);
    if (status != KERNEL_HEAP_STATUS_OK) {
        fail_heap(66U, KERNEL_HEAP_STATUS_OK, status);
    }
}

static void test_rejects_double_free_and_preserves_failed_realloc(void)
{
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    void *pointer = 0;
    void *result = (void *)(uintptr_t)UINT64_C(0xa5a5a5a5a5a5a5a5);
    enum kernel_heap_status status;

    initialize_heap(&allocator, &heap);

    status = kernel_heap_allocate(&heap, 32U, &pointer);
    if (status != KERNEL_HEAP_STATUS_OK) {
        fail_heap(70U, KERNEL_HEAP_STATUS_OK, status);
    }
    status = kernel_heap_resize(&heap, pointer, SIZE_MAX, &result);
    if (status != KERNEL_HEAP_STATUS_OVERFLOW ||
        result != (void *)(uintptr_t)UINT64_C(0xa5a5a5a5a5a5a5a5)) {
        fail_heap(71U, KERNEL_HEAP_STATUS_OVERFLOW, status);
    }
    status = kernel_heap_resize(&heap,
                                pointer,
                                BOAROS_PAGE_SIZE * TEST_POOL_PAGES * 2U,
                                &result);
    if (status != KERNEL_HEAP_STATUS_EMPTY ||
        result != (void *)(uintptr_t)UINT64_C(0xa5a5a5a5a5a5a5a5)) {
        fail_heap(72U, KERNEL_HEAP_STATUS_EMPTY, status);
    }
    ((unsigned char *)pointer)[0] = 0x7bU;
    if (((unsigned char *)pointer)[0] != 0x7bU) {
        fail_value(73U, 0x7bU, ((unsigned char *)pointer)[0]);
    }
    status = kernel_heap_release(&heap, pointer);
    if (status != KERNEL_HEAP_STATUS_OK) {
        fail_heap(74U, KERNEL_HEAP_STATUS_OK, status);
    }
    status = kernel_heap_release(&heap, pointer);
    if (status != KERNEL_HEAP_STATUS_DOUBLE_FREE) {
        fail_heap(75U, KERNEL_HEAP_STATUS_DOUBLE_FREE, status);
    }
    status = kernel_heap_release(&heap, 0);
    if (status != KERNEL_HEAP_STATUS_OK) {
        fail_heap(76U, KERNEL_HEAP_STATUS_OK, status);
    }
}

void run_kernel_heap_tests(void)
{
    test_small_classes_reuse_and_release_pages();
    test_exact_page_uses_one_buddy_page();
    test_zero_calloc_overflow_and_realloc();
    test_rejects_double_free_and_preserves_failed_realloc();
}
