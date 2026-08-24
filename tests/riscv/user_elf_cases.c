#include <arch/riscv/sv39.h>
#include <arch/riscv/user_elf.h>
#include <kernel/boot_memory.h>
#include <kernel/elf64.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_POOL_PAGES 32U
#define TEST_POOL_WORDS \
    ((BOAROS_PAGE_SIZE * TEST_POOL_PAGES) / sizeof(uint64_t))
#define TEST_OOM_POOL_PAGES 8U
#define TEST_OOM_POOL_WORDS \
    ((BOAROS_PAGE_SIZE * TEST_OOM_POOL_PAGES) / sizeof(uint64_t))
#define TEST_IMAGE_SIZE 0x800U
#define TEST_HEADER_SIZE 64U
#define TEST_PROGRAM_HEADER_SIZE 56U
#define TEST_TEXT_VA UINT64_C(0x10100)
#define TEST_READ_ONLY_VA UINT64_C(0x20100)
#define TEST_DATA_VA UINT64_C(0x20200)
#define TEST_TEXT_OFFSET UINT64_C(0x200)
#define TEST_READ_ONLY_OFFSET UINT64_C(0x300)
#define TEST_DATA_OFFSET UINT64_C(0x400)
#define TEST_FILE_BYTES UINT64_C(0x80)

static uint64_t test_page_pool[TEST_POOL_WORDS]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t test_oom_page_pool[TEST_OOM_POOL_WORDS]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char test_image[TEST_IMAGE_SIZE];
static int force_destroy_failure;

enum riscv_sv39_status __real_riscv_sv39_user_space_destroy(
    struct riscv_sv39_user_space *space);

enum riscv_sv39_status __wrap_riscv_sv39_user_space_destroy(
    struct riscv_sv39_user_space *space)
{
    if (force_destroy_failure != 0) {
        return RISCV_SV39_STATUS_STATE;
    }
    return __real_riscv_sv39_user_space_destroy(space);
}

static void *identity_page_access(uint64_t address)
{
    return (void *)(uintptr_t)address;
}

static void put_u16(size_t offset, uint16_t value)
{
    test_image[offset] = (unsigned char)value;
    test_image[offset + 1U] = (unsigned char)(value >> 8U);
}

static void put_u32(size_t offset, uint32_t value)
{
    uint32_t index;

    for (index = 0U; index < 4U; index++) {
        test_image[offset + index] =
            (unsigned char)(value >> (index * 8U));
    }
}

static void put_u64(size_t offset, uint64_t value)
{
    uint32_t index;

    for (index = 0U; index < 8U; index++) {
        test_image[offset + index] =
            (unsigned char)(value >> (index * 8U));
    }
}

static void clear_bytes(unsigned char *bytes, size_t size)
{
    size_t index;

    for (index = 0U; index < size; index++) {
        bytes[index] = 0U;
    }
}

static void put_program_header(size_t offset,
                               uint32_t flags,
                               uint64_t file_offset,
                               uint64_t virtual_address,
                               uint64_t file_size,
                               uint64_t memory_size)
{
    put_u32(offset, KERNEL_ELF64_PROGRAM_LOAD);
    put_u32(offset + 4U, flags);
    put_u64(offset + 8U, file_offset);
    put_u64(offset + 16U, virtual_address);
    put_u64(offset + 24U, virtual_address);
    put_u64(offset + 32U, file_size);
    put_u64(offset + 40U, memory_size);
    put_u64(offset + 48U, 0x100U);
}

static void make_valid_image(void)
{
    size_t index;

    clear_bytes(test_image, sizeof(test_image));
    test_image[0] = 0x7fU;
    test_image[1] = 'E';
    test_image[2] = 'L';
    test_image[3] = 'F';
    test_image[4] = KERNEL_ELF64_CLASS_64;
    test_image[5] = KERNEL_ELF64_DATA_LITTLE_ENDIAN;
    test_image[6] = KERNEL_ELF64_VERSION_CURRENT;
    put_u16(16U, KERNEL_ELF64_TYPE_EXECUTABLE);
    put_u16(18U, KERNEL_ELF64_MACHINE_RISCV);
    put_u32(20U, KERNEL_ELF64_VERSION_CURRENT);
    put_u64(24U, TEST_TEXT_VA);
    put_u64(32U, TEST_HEADER_SIZE);
    put_u16(52U, TEST_HEADER_SIZE);
    put_u16(54U, TEST_PROGRAM_HEADER_SIZE);
    put_u16(56U, 3U);

    put_program_header(TEST_HEADER_SIZE,
                       KERNEL_ELF64_FLAG_READ |
                           KERNEL_ELF64_FLAG_EXECUTE,
                       TEST_TEXT_OFFSET,
                       TEST_TEXT_VA,
                       TEST_FILE_BYTES,
                       TEST_FILE_BYTES);
    put_program_header(TEST_HEADER_SIZE + TEST_PROGRAM_HEADER_SIZE,
                       KERNEL_ELF64_FLAG_READ,
                       TEST_READ_ONLY_OFFSET,
                       TEST_READ_ONLY_VA,
                       TEST_FILE_BYTES,
                       0x100U);
    put_program_header(TEST_HEADER_SIZE + 2U * TEST_PROGRAM_HEADER_SIZE,
                       KERNEL_ELF64_FLAG_READ | KERNEL_ELF64_FLAG_WRITE,
                       TEST_DATA_OFFSET,
                       TEST_DATA_VA,
                       TEST_FILE_BYTES,
                       0x180U);

    for (index = 0U; index < TEST_FILE_BYTES; index++) {
        test_image[TEST_TEXT_OFFSET + index] =
            (unsigned char)(0x40U + index);
        test_image[TEST_READ_ONLY_OFFSET + index] =
            (unsigned char)(0x80U + index);
        test_image[TEST_DATA_OFFSET + index] =
            (unsigned char)(0xc0U + index);
    }
}

static int init_allocator_and_kernel_table(
    struct physical_page_allocator *allocator,
    struct riscv_sv39_page_table *kernel_table,
    void *pool,
    size_t pool_size)
{
    struct boot_memory_layout layout;

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)pool;
    layout.usable[0].size = pool_size;
    if (physical_page_allocator_init(allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(allocator,
                                            identity_page_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(kernel_table, allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 0;
    }
    kernel_table->state = RISCV_SV39_STATE_ACTIVE;
    return 1;
}

static int expect_pattern(const unsigned char *page,
                          size_t start,
                          size_t size,
                          unsigned char base)
{
    size_t index;

    for (index = 0U; index < size; index++) {
        if (page[start + index] != (unsigned char)(base + index)) {
            return 0;
        }
    }
    return 1;
}

static int expect_zero(const unsigned char *page,
                       size_t start,
                       size_t end)
{
    size_t index;

    for (index = start; index < end; index++) {
        if (page[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static int resolve_user_page(
    const struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    uint64_t virtual_address,
    struct riscv_sv39_mapping *mapping,
    unsigned char **page)
{
    void *pointer;

    if (riscv_sv39_user_lookup(space, virtual_address, mapping) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_resolve(allocator,
                              mapping->physical_address &
                                  ~BOAROS_PAGE_MASK,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return 0;
    }
    *page = pointer;
    return 1;
}

unsigned long run_user_elf_cases(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_entry entry = {
        .entry = UINT64_C(0x1111111111111111),
        .stack_pointer = UINT64_C(0x2222222222222222),
    };
    struct riscv_sv39_mapping text_mapping;
    struct riscv_sv39_mapping data_mapping;
    struct riscv_sv39_mapping stack_mapping;
    unsigned char *text_page;
    unsigned char *data_page;
    unsigned char *stack_page;
    uint64_t available_before;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }
    available_before = physical_page_available(&allocator);
    if (riscv_user_elf_load(test_image,
                            sizeof(test_image),
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_OK) {
        return 2U;
    }
    if (entry.entry != TEST_TEXT_VA ||
        entry.stack_pointer != RISCV_USER_ELF_STACK_TOP ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        space.leaf_pages != 3U || space.table_pages != 5U) {
        return 3U;
    }
    if (!resolve_user_page(&space,
                           &allocator,
                           TEST_TEXT_VA,
                           &text_mapping,
                           &text_page) ||
        text_mapping.permissions !=
            (RISCV_SV39_USER | RISCV_SV39_READ |
             RISCV_SV39_EXECUTE) ||
        !expect_zero(text_page, 0U, 0x100U) ||
        !expect_pattern(text_page, 0x100U, TEST_FILE_BYTES, 0x40U) ||
        !expect_zero(text_page, 0x180U, BOAROS_PAGE_SIZE)) {
        return 4U;
    }
    if (!resolve_user_page(&space,
                           &allocator,
                           TEST_READ_ONLY_VA,
                           &data_mapping,
                           &data_page) ||
        data_mapping.permissions !=
            (RISCV_SV39_USER | RISCV_SV39_READ | RISCV_SV39_WRITE) ||
        !expect_zero(data_page, 0U, 0x100U) ||
        !expect_pattern(data_page, 0x100U, TEST_FILE_BYTES, 0x80U) ||
        !expect_zero(data_page, 0x180U, 0x200U) ||
        !expect_pattern(data_page, 0x200U, TEST_FILE_BYTES, 0xc0U) ||
        !expect_zero(data_page, 0x280U, BOAROS_PAGE_SIZE)) {
        return 5U;
    }
    if (!resolve_user_page(&space,
                           &allocator,
                           RISCV_USER_ELF_STACK_BASE,
                           &stack_mapping,
                           &stack_page) ||
        stack_mapping.permissions !=
            (RISCV_SV39_USER | RISCV_SV39_READ | RISCV_SV39_WRITE) ||
        !expect_zero(stack_page, 0U, BOAROS_PAGE_SIZE) ||
        riscv_sv39_user_lookup(&space,
                               RISCV_USER_ELF_STACK_TOP,
                               &stack_mapping) !=
            RISCV_SV39_STATUS_NOT_MAPPED) {
        return 6U;
    }
    if (riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != available_before) {
        return 7U;
    }
    return 0U;
}

static int entry_changed(const struct riscv_user_elf_entry *entry)
{
    return entry->entry != UINT64_C(0x1111111111111111) ||
           entry->stack_pointer != UINT64_C(0x2222222222222222);
}

static unsigned long expect_load_failure(
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    size_t image_size,
    enum riscv_user_elf_status expected)
{
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_entry entry = {
        .entry = UINT64_C(0x1111111111111111),
        .stack_pointer = UINT64_C(0x2222222222222222),
    };
    uint64_t available = physical_page_available(allocator);

    if (riscv_user_elf_load(test_image,
                            image_size,
                            allocator,
                            kernel_table,
                            &space,
                            &entry) != expected ||
        space.state != RISCV_SV39_USER_SPACE_EMPTY ||
        entry_changed(&entry) ||
        physical_page_available(allocator) != available) {
        return 1U;
    }
    return 0U;
}

static unsigned long run_argument_and_format_failures(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_entry entry = {
        .entry = UINT64_C(0x1111111111111111),
        .stack_pointer = UINT64_C(0x2222222222222222),
    };
    unsigned long failures = 0U;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }
    if (riscv_user_elf_load(0,
                            sizeof(test_image),
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        riscv_user_elf_load(test_image,
                            sizeof(test_image),
                            0,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        riscv_user_elf_load(test_image,
                            sizeof(test_image),
                            &allocator,
                            0,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        riscv_user_elf_load(test_image,
                            sizeof(test_image),
                            &allocator,
                            &kernel_table,
                            0,
                            &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        riscv_user_elf_load(test_image,
                            sizeof(test_image),
                            &allocator,
                            &kernel_table,
                            &space,
                            0) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        space.state != RISCV_SV39_USER_SPACE_EMPTY || entry_changed(&entry)) {
        failures++;
    }

    make_valid_image();
    failures += expect_load_failure(&allocator,
                                    &kernel_table,
                                    TEST_HEADER_SIZE - 1U,
                                    RISCV_USER_ELF_STATUS_TRUNCATED);
    make_valid_image();
    test_image[0] = 0U;
    failures += expect_load_failure(&allocator,
                                    &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_MALFORMED);
    make_valid_image();
    put_u16(18U, KERNEL_ELF64_MACHINE_LOONGARCH);
    failures += expect_load_failure(&allocator,
                                    &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_WRONG_ARCH);
    make_valid_image();
    put_u16(16U, KERNEL_ELF64_TYPE_SHARED);
    failures += expect_load_failure(&allocator,
                                    &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_UNSUPPORTED);
    return failures;
}

static unsigned long run_program_type_failures(void)
{
    static const uint32_t unsupported_types[] = {
        KERNEL_ELF64_PROGRAM_INTERPRETER,
        KERNEL_ELF64_PROGRAM_DYNAMIC,
        KERNEL_ELF64_PROGRAM_TLS,
    };
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    unsigned long failures = 0U;
    size_t index;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }
    for (index = 0U;
         index < sizeof(unsupported_types) / sizeof(unsupported_types[0]);
         index++) {
        make_valid_image();
        put_u32(TEST_HEADER_SIZE + TEST_PROGRAM_HEADER_SIZE,
                unsupported_types[index]);
        failures += expect_load_failure(&allocator,
                                        &kernel_table,
                                        sizeof(test_image),
                                        RISCV_USER_ELF_STATUS_UNSUPPORTED);
    }
    make_valid_image();
    put_u32(TEST_HEADER_SIZE, KERNEL_ELF64_PROGRAM_NOTE);
    put_u32(TEST_HEADER_SIZE + TEST_PROGRAM_HEADER_SIZE,
            KERNEL_ELF64_PROGRAM_NOTE);
    put_u32(TEST_HEADER_SIZE + 2U * TEST_PROGRAM_HEADER_SIZE,
            KERNEL_ELF64_PROGRAM_NOTE);
    failures += expect_load_failure(&allocator,
                                    &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);
    return failures;
}

static unsigned long run_layout_failures(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    unsigned long failures = 0U;
    size_t data_ph = TEST_HEADER_SIZE + 2U * TEST_PROGRAM_HEADER_SIZE;
    size_t read_only_ph = TEST_HEADER_SIZE + TEST_PROGRAM_HEADER_SIZE;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }

    make_valid_image();
    put_u32(data_ph + 4U, KERNEL_ELF64_FLAG_WRITE);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);
    make_valid_image();
    put_u32(data_ph + 4U,
            KERNEL_ELF64_FLAG_READ | KERNEL_ELF64_FLAG_WRITE |
                KERNEL_ELF64_FLAG_EXECUTE);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);
    make_valid_image();
    put_u32(data_ph + 4U, 0U);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);

    make_valid_image();
    put_u64(TEST_HEADER_SIZE + 16U, 0x800U);
    put_u64(TEST_HEADER_SIZE + 24U, 0x800U);
    put_u64(TEST_HEADER_SIZE + 48U, 1U);
    put_u64(24U, 0x800U);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);

    make_valid_image();
    put_u64(data_ph + 16U, RISCV_USER_ELF_STACK_BASE);
    put_u64(data_ph + 24U, RISCV_USER_ELF_STACK_BASE);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);

    make_valid_image();
    put_u64(24U, TEST_TEXT_VA + 1U);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);
    make_valid_image();
    put_u64(24U, TEST_TEXT_VA + TEST_FILE_BYTES);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);

    make_valid_image();
    put_u64(read_only_ph + 40U, 0x200U);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);

    make_valid_image();
    put_u64(data_ph + 8U, 0x480U);
    put_u64(data_ph + 16U, TEST_TEXT_VA + TEST_FILE_BYTES);
    put_u64(data_ph + 24U, TEST_TEXT_VA + TEST_FILE_BYTES);
    failures += expect_load_failure(&allocator, &kernel_table,
                                    sizeof(test_image),
                                    RISCV_USER_ELF_STATUS_INVALID_LAYOUT);
    return failures;
}

static unsigned long run_oom_and_cleanup_cases(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_entry entry = {
        .entry = UINT64_C(0x1111111111111111),
        .stack_pointer = UINT64_C(0x2222222222222222),
    };
    uint64_t available;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_oom_page_pool,
                                         sizeof(test_oom_page_pool))) {
        return 1U;
    }
    available = physical_page_available(&allocator);
    if (riscv_user_elf_load(test_image,
                            sizeof(test_image),
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_NO_MEMORY ||
        space.state != RISCV_SV39_USER_SPACE_EMPTY ||
        entry_changed(&entry) ||
        physical_page_available(&allocator) != available) {
        return 2U;
    }

    force_destroy_failure = 1;
    if (riscv_user_elf_load(test_image,
                            sizeof(test_image),
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) !=
            RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        entry_changed(&entry)) {
        force_destroy_failure = 0;
        return 3U;
    }
    force_destroy_failure = 0;
    if (riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != available) {
        return 4U;
    }
    return 0U;
}

unsigned long run_all_user_elf_cases(void)
{
    unsigned long failures = run_user_elf_cases();

    failures += run_argument_and_format_failures();
    failures += run_program_type_failures();
    failures += run_layout_failures();
    failures += run_oom_and_cleanup_cases();
    return failures;
}
