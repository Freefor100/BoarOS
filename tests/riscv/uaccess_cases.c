#include <arch/riscv/mm.h>
#include <arch/riscv/sv39.h>
#include <kernel/boot_memory.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_PAGE_COUNT 16U
#define TEST_USER_BASE UINT64_C(0x10000)
#define TEST_READ_ONLY_ADDRESS UINT64_C(0x20000)
#define TEST_EXEC_ONLY_ADDRESS UINT64_C(0x30000)
#define TEST_USER_LIMIT (UINT64_C(1) << 38U)

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_PAGE_COUNT]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t inaccessible_page = UINT64_MAX;

static void *test_page_access(uint64_t address)
{
    if (address == inaccessible_page) {
        return 0;
    }
    return (void *)(uintptr_t)address;
}

static int setup_mm(struct physical_page_allocator *allocator,
                    struct riscv_sv39_page_table *kernel_table,
                    struct kernel_mm *mm,
                    uint64_t *baseline)
{
    struct boot_memory_layout layout;
    struct riscv_sv39_user_space space = {0};

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)page_pool;
    layout.usable[0].size = sizeof(page_pool);
    if (physical_page_allocator_init(allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(allocator,
                                            test_page_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(kernel_table, allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 0;
    }
    kernel_table->state = RISCV_SV39_STATE_ACTIVE;
    *baseline = physical_page_available(allocator);
    if (riscv_sv39_user_space_init(&space,
                                   allocator,
                                   kernel_table) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            TEST_USER_BASE,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            TEST_USER_BASE + BOAROS_PAGE_SIZE,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            TEST_READ_ONLY_ADDRESS,
            RISCV_SV39_READ) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            TEST_EXEC_ONLY_ADDRESS,
            RISCV_SV39_EXECUTE) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_map_zeroed_page(
            &space,
            TEST_USER_LIMIT - BOAROS_PAGE_SIZE,
            RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK ||
        riscv_kernel_mm_create(mm, &space) != KERNEL_MM_STATUS_OK) {
        return 0;
    }
    return 1;
}

static int write_user_bytes(const struct kernel_mm *mm,
                            uint64_t virtual_address,
                            const unsigned char *source,
                            size_t size)
{
    size_t copied = 0U;

    while (copied < size) {
        struct kernel_mm_mapping mapping;
        unsigned char *page;
        void *pointer;
        uint64_t current = virtual_address + copied;
        size_t offset = current & BOAROS_PAGE_MASK;
        size_t chunk = BOAROS_PAGE_SIZE - offset;
        size_t index;

        if (chunk > size - copied) {
            chunk = size - copied;
        }
        if (kernel_mm_lookup(mm, current, &mapping) !=
                KERNEL_MM_STATUS_OK ||
            physical_page_resolve(mm->allocator,
                                  mapping.physical_address &
                                      ~BOAROS_PAGE_MASK,
                                  &pointer) != PHYSICAL_PAGE_STATUS_OK) {
            return 0;
        }
        page = pointer;
        for (index = 0U; index < chunk; index++) {
            page[offset + index] = source[copied + index];
        }
        copied += chunk;
    }
    return 1;
}

static int read_user_byte(const struct kernel_mm *mm,
                          uint64_t virtual_address,
                          unsigned char *value)
{
    struct kernel_mm_mapping mapping;
    unsigned char *page;
    void *pointer;

    if (kernel_mm_lookup(mm, virtual_address, &mapping) !=
            KERNEL_MM_STATUS_OK ||
        physical_page_resolve(mm->allocator,
                              mapping.physical_address &
                                  ~BOAROS_PAGE_MASK,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return 0;
    }
    page = pointer;
    *value = page[virtual_address & BOAROS_PAGE_MASK];
    return 1;
}

static unsigned long run_cross_page_copy(void)
{
    static const unsigned char source[] = {
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U,
        0x77U, 0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU,
    };
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm mm = {0};
    uint64_t destination =
        TEST_USER_BASE + BOAROS_PAGE_SIZE - 5U;
    uint64_t baseline;
    size_t copied = SIZE_MAX;
    size_t index;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline)) {
        return 1U;
    }
    if (kernel_copy_to_user(&mm,
                            destination,
                            source,
                            sizeof(source),
                            &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(source)) {
        return 2U;
    }
    for (index = 0U; index < sizeof(source); index++) {
        unsigned char actual;

        if (!read_user_byte(&mm, destination + index, &actual) ||
            actual != source[index]) {
            return 3U;
        }
    }
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 4U;
    }
    return 0U;
}

static unsigned long run_zero_length_copy(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm mm = {0};
    uint64_t baseline;
    size_t copied = SIZE_MAX;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline)) {
        return 1U;
    }
    if (kernel_copy_to_user(&mm,
                            UINT64_MAX,
                            0,
                            0U,
                            &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != 0U) {
        return 2U;
    }
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 3U;
    }
    return 0U;
}

static unsigned long run_user_range_copy(void)
{
    static const unsigned char source[2] = {0x5aU, 0xa5U};
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm mm = {0};
    unsigned char last_byte = UINT8_MAX;
    uint64_t baseline;
    size_t copied = SIZE_MAX;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline)) {
        return 1U;
    }
    if (kernel_copy_to_user(&mm,
                            TEST_USER_LIMIT - 1U,
                            source,
                            sizeof(source),
                            &copied) != KERNEL_UACCESS_STATUS_FAULT ||
        copied != 0U ||
        !read_user_byte(&mm, TEST_USER_LIMIT - 1U, &last_byte) ||
        last_byte != 0U) {
        return 2U;
    }
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 3U;
    }
    return 0U;
}

static unsigned long run_read_only_copy(void)
{
    static const unsigned char source = 0x5aU;
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm mm = {0};
    unsigned char actual = UINT8_MAX;
    uint64_t baseline;
    size_t copied = SIZE_MAX;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline)) {
        return 1U;
    }
    if (kernel_copy_to_user(&mm,
                            TEST_READ_ONLY_ADDRESS,
                            &source,
                            sizeof(source),
                            &copied) != KERNEL_UACCESS_STATUS_FAULT ||
        copied != 0U ||
        !read_user_byte(&mm, TEST_READ_ONLY_ADDRESS, &actual) ||
        actual != 0U) {
        return 2U;
    }
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 3U;
    }
    return 0U;
}

static unsigned long run_partial_unmapped_copy(void)
{
    static const unsigned char source[4] = {
        0x31U, 0x42U, 0x53U, 0x64U,
    };
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm mm = {0};
    uint64_t destination =
        TEST_USER_BASE + (2U * BOAROS_PAGE_SIZE) - 2U;
    uint64_t baseline;
    size_t copied = SIZE_MAX;
    size_t index;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline)) {
        return 1U;
    }
    if (kernel_copy_to_user(&mm,
                            destination,
                            source,
                            sizeof(source),
                            &copied) != KERNEL_UACCESS_STATUS_FAULT ||
        copied != 2U) {
        return 2U;
    }
    for (index = 0U; index < copied; index++) {
        unsigned char actual;

        if (!read_user_byte(&mm, destination + index, &actual) ||
            actual != source[index]) {
            return 3U;
        }
    }
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 4U;
    }
    return 0U;
}

static unsigned long run_invalid_and_state_copy(void)
{
    static const unsigned char source = 0x5aU;
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm_mapping mapping;
    struct kernel_mm mm = {0};
    uint64_t baseline;
    size_t copied = SIZE_MAX;

    if (kernel_copy_to_user(0,
                            TEST_USER_BASE,
                            &source,
                            sizeof(source),
                            &copied) !=
            KERNEL_UACCESS_STATUS_INVALID_ARGUMENT ||
        copied != SIZE_MAX ||
        kernel_copy_to_user(&mm,
                            TEST_USER_BASE,
                            0,
                            sizeof(source),
                            &copied) !=
            KERNEL_UACCESS_STATUS_INVALID_ARGUMENT ||
        copied != SIZE_MAX) {
        return 1U;
    }
    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline) ||
        kernel_mm_lookup(&mm, TEST_USER_BASE, &mapping) !=
            KERNEL_MM_STATUS_OK) {
        return 2U;
    }
    inaccessible_page =
        mapping.physical_address & ~BOAROS_PAGE_MASK;
    if (kernel_copy_to_user(&mm,
                            TEST_USER_BASE,
                            &source,
                            sizeof(source),
                            &copied) != KERNEL_UACCESS_STATUS_STATE ||
        copied != 0U) {
        inaccessible_page = UINT64_MAX;
        return 3U;
    }
    inaccessible_page = UINT64_MAX;
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 4U;
    }
    copied = SIZE_MAX;
    if (kernel_copy_to_user(&mm,
                            TEST_USER_BASE,
                            &source,
                            sizeof(source),
                            &copied) != KERNEL_UACCESS_STATUS_STATE ||
        copied != 0U) {
        return 5U;
    }
    return 0U;
}

static unsigned long run_copy_from_user(void)
{
    static const unsigned char source[] = {
        0x10U, 0x21U, 0x32U, 0x43U, 0x54U, 0x65U,
        0x76U, 0x87U, 0x98U, 0xa9U, 0xbaU, 0xcbU,
    };
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm mm = {0};
    unsigned char destination[sizeof(source)];
    uint64_t source_address =
        TEST_USER_BASE + BOAROS_PAGE_SIZE - 5U;
    uint64_t baseline;
    size_t copied = SIZE_MAX;
    size_t index;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline) ||
        !write_user_bytes(&mm,
                          source_address,
                          source,
                          sizeof(source))) {
        return 1U;
    }
    for (index = 0U; index < sizeof(destination); index++) {
        destination[index] = 0U;
    }
    if (kernel_copy_from_user(&mm,
                              destination,
                              source_address,
                              sizeof(destination),
                              &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(destination)) {
        return 2U;
    }
    for (index = 0U; index < sizeof(destination); index++) {
        if (destination[index] != source[index]) {
            return 3U;
        }
    }
    if (!write_user_bytes(&mm,
                          TEST_READ_ONLY_ADDRESS,
                          source,
                          sizeof(source)) ||
        kernel_copy_from_user(&mm,
                              destination,
                              TEST_READ_ONLY_ADDRESS,
                              sizeof(destination),
                              &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(destination)) {
        return 4U;
    }
    copied = SIZE_MAX;
    if (kernel_copy_from_user(&mm,
                              destination,
                              TEST_EXEC_ONLY_ADDRESS,
                              1U,
                              &copied) != KERNEL_UACCESS_STATUS_FAULT ||
        copied != 0U) {
        return 5U;
    }
    copied = SIZE_MAX;
    if (kernel_copy_from_user(&mm,
                              destination,
                              UINT64_MAX,
                              0U,
                              &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != 0U ||
        kernel_user_range_check(TEST_USER_LIMIT - 1U, 1U) !=
            KERNEL_UACCESS_STATUS_OK ||
        kernel_user_range_check(TEST_USER_LIMIT - 1U, 2U) !=
            KERNEL_UACCESS_STATUS_FAULT ||
        kernel_user_range_check(UINT64_MAX - 1U, 4U) !=
            KERNEL_UACCESS_STATUS_FAULT) {
        return 6U;
    }
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 7U;
    }
    copied = SIZE_MAX;
    if (kernel_copy_from_user(&mm,
                              destination,
                              TEST_USER_BASE,
                              1U,
                              &copied) != KERNEL_UACCESS_STATUS_STATE ||
        copied != 0U) {
        return 8U;
    }
    return 0U;
}

static unsigned long run_partial_and_state_copy_from_user(void)
{
    static const unsigned char source[2] = {0x7cU, 0xd3U};
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm_mapping mapping;
    struct kernel_mm mm = {0};
    unsigned char destination[4] = {0U, 0U, 0xa5U, 0x5aU};
    uint64_t source_address =
        TEST_USER_BASE + (2U * BOAROS_PAGE_SIZE) - 2U;
    uint64_t baseline;
    size_t copied = SIZE_MAX;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline) ||
        !write_user_bytes(&mm, source_address, source, sizeof(source))) {
        return 1U;
    }
    if (kernel_copy_from_user(&mm,
                              destination,
                              source_address,
                              sizeof(destination),
                              &copied) != KERNEL_UACCESS_STATUS_FAULT ||
        copied != 2U || destination[0] != source[0] ||
        destination[1] != source[1] || destination[2] != 0xa5U ||
        destination[3] != 0x5aU) {
        return 2U;
    }
    if (kernel_mm_lookup(&mm, TEST_USER_BASE, &mapping) !=
        KERNEL_MM_STATUS_OK) {
        return 3U;
    }
    inaccessible_page = mapping.physical_address & ~BOAROS_PAGE_MASK;
    copied = SIZE_MAX;
    if (kernel_copy_from_user(&mm,
                              destination,
                              TEST_USER_BASE,
                              1U,
                              &copied) != KERNEL_UACCESS_STATUS_STATE ||
        copied != 0U) {
        inaccessible_page = UINT64_MAX;
        return 4U;
    }
    inaccessible_page = UINT64_MAX;
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 5U;
    }
    return 0U;
}

static unsigned long run_copy_string_from_user(void)
{
    static const unsigned char cross_page[] = {'b', 'o', 'a', 'r', 0U};
    static const unsigned char no_nul[] = {'a', 'b', 'c', 'd'};
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct kernel_mm mm = {0};
    char destination[8] = {0};
    uint64_t cross_address =
        TEST_USER_BASE + BOAROS_PAGE_SIZE - 2U;
    uint64_t baseline;
    size_t length = SIZE_MAX;

    if (!setup_mm(&allocator, &kernel_table, &mm, &baseline) ||
        !write_user_bytes(&mm,
                          cross_address,
                          cross_page,
                          sizeof(cross_page)) ||
        !write_user_bytes(&mm,
                          TEST_READ_ONLY_ADDRESS,
                          no_nul,
                          sizeof(no_nul))) {
        return 1U;
    }
    if (kernel_copy_string_from_user(&mm,
                                     destination,
                                     cross_address,
                                     sizeof(cross_page),
                                     &length) != KERNEL_UACCESS_STATUS_OK ||
        length != 4U || destination[0] != 'b' ||
        destination[1] != 'o' || destination[2] != 'a' ||
        destination[3] != 'r' || destination[4] != '\0') {
        return 2U;
    }
    length = SIZE_MAX;
    if (kernel_copy_string_from_user(&mm,
                                     destination,
                                     TEST_READ_ONLY_ADDRESS,
                                     sizeof(no_nul),
                                     &length) !=
            KERNEL_UACCESS_STATUS_TOO_LONG ||
        length != sizeof(no_nul)) {
        return 3U;
    }
    length = SIZE_MAX;
    if (kernel_copy_string_from_user(&mm,
                                     destination,
                                     TEST_USER_BASE +
                                         (2U * BOAROS_PAGE_SIZE),
                                     sizeof(destination),
                                     &length) != KERNEL_UACCESS_STATUS_FAULT ||
        length != 0U) {
        return 4U;
    }
    if (kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        return 5U;
    }
    return 0U;
}

unsigned long run_all_uaccess_cases(void)
{
    unsigned long result = run_cross_page_copy();

    if (result != 0U) {
        return UINT64_C(0x100) + result;
    }
    result = run_zero_length_copy();
    if (result != 0U) {
        return UINT64_C(0x200) + result;
    }
    result = run_user_range_copy();
    if (result != 0U) {
        return UINT64_C(0x300) + result;
    }
    result = run_read_only_copy();
    if (result != 0U) {
        return UINT64_C(0x400) + result;
    }
    result = run_partial_unmapped_copy();
    if (result != 0U) {
        return UINT64_C(0x500) + result;
    }
    result = run_invalid_and_state_copy();
    if (result != 0U) {
        return UINT64_C(0x600) + result;
    }
    result = run_copy_from_user();
    if (result != 0U) {
        return UINT64_C(0x700) + result;
    }
    result = run_partial_and_state_copy_from_user();
    if (result != 0U) {
        return UINT64_C(0x800) + result;
    }
    result = run_copy_string_from_user();
    return result == 0U ? 0U : UINT64_C(0x900) + result;
}
