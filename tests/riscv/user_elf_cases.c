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
#define TEST_LARGE_ARGUMENT_SIZE (0x20000U - 161U)

static uint64_t test_page_pool[TEST_POOL_WORDS]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t test_oom_page_pool[TEST_OOM_POOL_WORDS]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t test_cleanup_page_pool[TEST_POOL_WORDS]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static uint64_t test_large_page_pool[
    (BOAROS_PAGE_SIZE * 64U) / sizeof(uint64_t)]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static char test_large_argument[TEST_LARGE_ARGUMENT_SIZE + 1U];
static unsigned char test_image[TEST_IMAGE_SIZE];
static int force_destroy_failure;
static uint64_t inaccessible_page;
static uint64_t fail_after_first_access_page;
static uint32_t fail_after_first_access_count;

struct failing_read_context {
    uint64_t fail_at;
};

static int failing_read_at(void *context,
                           uint64_t offset,
                           void *buffer,
                           size_t size)
{
    struct failing_read_context *failure = context;
    unsigned char *destination = buffer;
    size_t index;

    if (offset >= failure->fail_at) {
        return -5;
    }
    for (index = 0U; index < size; index++) {
        destination[index] = test_image[offset + index];
    }
    return 0;
}

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

static void *faulting_page_access(uint64_t address)
{
    if (address == inaccessible_page) {
        return 0;
    }
    if (address == fail_after_first_access_page) {
        if (fail_after_first_access_count != 0U) {
            return 0;
        }
        fail_after_first_access_count++;
    }
    return identity_page_access(address);
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

static int init_allocator_and_kernel_table_with_access(
    struct physical_page_allocator *allocator,
    struct riscv_sv39_page_table *kernel_table,
    void *pool,
    size_t pool_size,
    physical_page_access_fn access)
{
    struct boot_memory_layout layout;

    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)pool;
    layout.usable[0].size = pool_size;
    if (physical_page_allocator_init(allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(allocator,
                                            access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        riscv_sv39_page_table_init(kernel_table, allocator) !=
            RISCV_SV39_STATUS_OK) {
        return 0;
    }
    kernel_table->state = RISCV_SV39_STATE_ACTIVE;
    return 1;
}

static int init_allocator_and_kernel_table(
    struct physical_page_allocator *allocator,
    struct riscv_sv39_page_table *kernel_table,
    void *pool,
    size_t pool_size)
{
    return init_allocator_and_kernel_table_with_access(
        allocator,
        kernel_table,
        pool,
        pool_size,
        identity_page_access);
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

static int read_user_bytes(
    const struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    uint64_t virtual_address,
    unsigned char *bytes,
    size_t size)
{
    struct riscv_sv39_mapping mapping;
    unsigned char *page;
    size_t index;

    for (index = 0U; index < size; index++) {
        if (!resolve_user_page(space,
                               allocator,
                               virtual_address + index,
                               &mapping,
                               &page)) {
            return 0;
        }
        bytes[index] = page[(virtual_address + index) & BOAROS_PAGE_MASK];
    }
    return 1;
}

static int read_user_u64(
    const struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    uint64_t virtual_address,
    uint64_t *value)
{
    unsigned char bytes[8];
    uint64_t result = 0U;
    uint32_t index;

    if (!read_user_bytes(space,
                         allocator,
                         virtual_address,
                         bytes,
                         sizeof(bytes))) {
        return 0;
    }
    for (index = 0U; index < sizeof(bytes); index++) {
        result |= (uint64_t)bytes[index] << (index * 8U);
    }
    *value = result;
    return 1;
}

static int expect_user_string(
    const struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    uint64_t virtual_address,
    const char *expected,
    size_t length)
{
    unsigned char actual;
    size_t index;

    for (index = 0U; index <= length; index++) {
        if (!read_user_bytes(space,
                             allocator,
                             virtual_address + index,
                             &actual,
                             1U) ||
            actual != (index == length
                           ? 0U
                           : (unsigned char)expected[index])) {
            return 0;
        }
    }
    return 1;
}

static int find_aux_value(
    const struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator,
    uint64_t auxiliary_vector,
    uint64_t expected_type,
    uint64_t *value)
{
    uint64_t type;
    uint64_t current_value;
    uint32_t index;

    for (index = 0U; index < 16U; index++) {
        if (!read_user_u64(space,
                           allocator,
                           auxiliary_vector + index * 16U,
                           &type) ||
            !read_user_u64(space,
                           allocator,
                           auxiliary_vector + index * 16U + 8U,
                           &current_value)) {
            return 0;
        }
        if (type == expected_type) {
            *value = current_value;
            return 1;
        }
        if (type == 0U) {
            return 0;
        }
    }
    return 0;
}

static enum riscv_user_elf_status load_image(
    const void *image,
    size_t image_size,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry)
{
    struct riscv_user_elf_request request = {0};

    (void)kernel_read_source_from_memory(image,
                                         image_size,
                                         &request.source);

    return riscv_user_elf_load(&request,
                               allocator,
                               kernel_table,
                               space,
                               entry);
}

unsigned long run_user_elf_cases(void)
{
    static const char argument_zero[] = "alpha";
    static const char argument_one[] = "beta";
    static const char environment_zero[] = "KEY=value";
    struct riscv_user_elf_string arguments[2];
    struct riscv_user_elf_string environment[1];
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_request request;
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
    uint64_t argv_zero;
    uint64_t argv_one;
    uint64_t argv_null;
    uint64_t env_zero;
    uint64_t env_null;
    uint64_t auxiliary_value;
    uint64_t committed_stack_base;
    uint64_t available_before;

    arguments[0].bytes = argument_zero;
    arguments[0].length = sizeof(argument_zero) - 1U;
    arguments[1].bytes = argument_one;
    arguments[1].length = sizeof(argument_one) - 1U;
    environment[0].bytes = environment_zero;
    environment[0].length = sizeof(environment_zero) - 1U;
    if (kernel_read_source_from_memory(test_image,
                                       sizeof(test_image),
                                       &request.source) != 0) {
        return 1U;
    }
    request.arguments = arguments;
    request.argument_count = sizeof(arguments) / sizeof(arguments[0]);
    request.environment = environment;
    request.environment_count = sizeof(environment) / sizeof(environment[0]);
    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }
    available_before = physical_page_available(&allocator);
    if (riscv_user_elf_load(&request,
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_OK) {
        return 2U;
    }
    committed_stack_base =
        (entry.stack_pointer - RISCV_USER_ELF_STACK_INITIAL_HEADROOM) &
        ~BOAROS_PAGE_MASK;
    if (entry.entry != TEST_TEXT_VA ||
        entry.stack_pointer >= RISCV_USER_ELF_LIMIT ||
        (entry.stack_pointer & 15U) != 0U ||
        RISCV_USER_ELF_STACK_TOP != RISCV_USER_ELF_LIMIT ||
        RISCV_USER_ELF_STACK_RESERVE_BASE !=
            RISCV_USER_ELF_LIMIT - UINT64_C(0x800000) ||
        space.state != RISCV_SV39_USER_SPACE_LIVE ||
        space.leaf_pages != 19U || space.table_pages != 5U) {
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
                           entry.stack_pointer,
                           &stack_mapping,
                           &stack_page) ||
        stack_mapping.permissions !=
            (RISCV_SV39_USER | RISCV_SV39_READ | RISCV_SV39_WRITE) ||
        *(const uint64_t *)(stack_page +
                            (entry.stack_pointer & BOAROS_PAGE_MASK)) !=
            UINT64_C(2) ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 8U,
                       &argv_zero) ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 16U,
                       &argv_one) ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 24U,
                       &argv_null) ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 32U,
                       &env_zero) ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 40U,
                       &env_null) ||
        argv_zero == 0U || argv_one == 0U || env_zero == 0U ||
        argv_null != 0U || env_null != 0U ||
        !expect_user_string(&space,
                            &allocator,
                            argv_zero,
                            argument_zero,
                            sizeof(argument_zero) - 1U) ||
        !expect_user_string(&space,
                            &allocator,
                            argv_one,
                            argument_one,
                            sizeof(argument_one) - 1U) ||
        !expect_user_string(&space,
                            &allocator,
                            env_zero,
                            environment_zero,
                            sizeof(environment_zero) - 1U) ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        6U,
                        &auxiliary_value) ||
        auxiliary_value != UINT64_C(4096) ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        3U,
                        &auxiliary_value) ||
        auxiliary_value != 0U ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        4U,
                        &auxiliary_value) ||
        auxiliary_value != UINT64_C(56) ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        5U,
                        &auxiliary_value) ||
        auxiliary_value != UINT64_C(3) ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        7U,
                        &auxiliary_value) ||
        auxiliary_value != 0U ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        8U,
                        &auxiliary_value) ||
        auxiliary_value != 0U ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        9U,
                        &auxiliary_value) ||
        auxiliary_value != TEST_TEXT_VA ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 48U,
                        0U,
                        &auxiliary_value) ||
        auxiliary_value != 0U ||
        riscv_sv39_user_lookup(&space,
                               committed_stack_base,
                               &stack_mapping) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_lookup(&space,
                               committed_stack_base - 1U,
                               &stack_mapping) !=
            RISCV_SV39_STATUS_NOT_MAPPED ||
        riscv_sv39_user_lookup(&space,
                               RISCV_USER_ELF_STACK_RESERVE_BASE,
                               &stack_mapping) !=
            RISCV_SV39_STATUS_NOT_MAPPED ||
        riscv_sv39_user_lookup(&space,
                               RISCV_USER_ELF_STACK_GUARD_BASE,
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

    if (load_image(test_image,
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
    if (load_image(0,
                   sizeof(test_image),
                   &allocator,
                   &kernel_table,
                   &space,
                   &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        load_image(test_image,
                   sizeof(test_image),
                   0,
                   &kernel_table,
                   &space,
                   &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        load_image(test_image,
                   sizeof(test_image),
                   &allocator,
                   0,
                   &space,
                   &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        load_image(test_image,
                   sizeof(test_image),
                   &allocator,
                   &kernel_table,
                   0,
                   &entry) != RISCV_USER_ELF_STATUS_INVALID_ARGUMENT ||
        load_image(test_image,
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

static unsigned long expect_request_failure(
    const struct riscv_user_elf_request *request,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    enum riscv_user_elf_status expected)
{
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_entry entry = {
        .entry = UINT64_C(0x1111111111111111),
        .stack_pointer = UINT64_C(0x2222222222222222),
    };
    uint64_t available = physical_page_available(allocator);

    if (riscv_user_elf_load(request,
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

static unsigned long run_request_failures(void)
{
    static const char embedded_nul[3] = {'a', '\0', 'b'};
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_user_elf_string string;
    struct riscv_user_elf_request request;
    unsigned long failures = 0U;
    size_t index;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }
    if (kernel_read_source_from_memory(test_image,
                                       sizeof(test_image),
                                       &request.source) != 0) {
        return 1U;
    }
    request.arguments = &string;
    request.argument_count = 1U;
    request.environment = 0;
    request.environment_count = 0U;

    for (index = 0U; index < sizeof(test_large_argument); index++) {
        test_large_argument[index] = 'x';
    }

    string.bytes = 0;
    string.length = 0U;
    failures += expect_request_failure(
        &request,
        &allocator,
        &kernel_table,
        RISCV_USER_ELF_STATUS_INVALID_ARGUMENT);

    string.bytes = embedded_nul;
    string.length = sizeof(embedded_nul);
    failures += expect_request_failure(
        &request,
        &allocator,
        &kernel_table,
        RISCV_USER_ELF_STATUS_INVALID_ARGUMENT);

    string.bytes = (const char *)test_image;
    string.length = RISCV_USER_ELF_STACK_IMAGE_LIMIT;
    failures += expect_request_failure(
        &request,
        &allocator,
        &kernel_table,
        RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE);

    string.bytes = test_large_argument;
    string.length = TEST_LARGE_ARGUMENT_SIZE + 1U;
    failures += expect_request_failure(
        &request,
        &allocator,
        &kernel_table,
        RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE);

    string.length = 0U;
    request.argument_count = SIZE_MAX;
    failures += expect_request_failure(
        &request,
        &allocator,
        &kernel_table,
        RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE);
    return failures;
}

static unsigned long run_stack_boundary_cases(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_string argument;
    struct riscv_user_elf_request request;
    struct riscv_user_elf_entry entry = {0};
    struct riscv_sv39_mapping mapping;
    uint64_t argument_address;
    uint64_t value;
    uint64_t available;
    unsigned char byte;
    size_t index;

    make_valid_image();
    put_program_header(TEST_HEADER_SIZE,
                       KERNEL_ELF64_FLAG_READ |
                           KERNEL_ELF64_FLAG_EXECUTE,
                       0U,
                       UINT64_C(0x10000),
                       UINT64_C(0x280),
                       UINT64_C(0x280));
    if (kernel_read_source_from_memory(test_image,
                                       sizeof(test_image),
                                       &request.source) != 0) {
        return 1U;
    }
    request.arguments = 0;
    request.argument_count = 0U;
    request.environment = 0;
    request.environment_count = 0U;
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }
    available = physical_page_available(&allocator);
    if (riscv_user_elf_load(&request,
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_OK ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer,
                       &value) ||
        value != 1U ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 8U,
                       &argument_address) ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 16U,
                       &value) ||
        value != 0U ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 24U,
                       &value) ||
        value != 0U ||
        !read_user_bytes(&space,
                         &allocator,
                         argument_address,
                         &byte,
                         sizeof(byte)) ||
        byte != 0U ||
        !find_aux_value(&space,
                        &allocator,
                        entry.stack_pointer + 32U,
                        3U,
                        &value) ||
        value != UINT64_C(0x10040)) {
        return 2U;
    }
    if (riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != available) {
        return 3U;
    }

    for (index = 0U; index < TEST_LARGE_ARGUMENT_SIZE; index++) {
        test_large_argument[index] = 'x';
    }
    make_valid_image();
    argument.bytes = test_large_argument;
    argument.length = TEST_LARGE_ARGUMENT_SIZE;
    request.arguments = &argument;
    request.argument_count = 1U;
    clear_bytes((unsigned char *)&allocator, sizeof(allocator));
    clear_bytes((unsigned char *)&kernel_table, sizeof(kernel_table));
    clear_bytes((unsigned char *)&space, sizeof(space));
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_large_page_pool,
                                         sizeof(test_large_page_pool))) {
        return 4U;
    }
    available = physical_page_available(&allocator);
    if (riscv_user_elf_load(&request,
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_OK ||
        entry.stack_pointer !=
            RISCV_USER_ELF_STACK_TOP -
                RISCV_USER_ELF_STACK_IMAGE_LIMIT ||
        space.leaf_pages != 50U || space.table_pages != 5U ||
        !read_user_u64(&space,
                       &allocator,
                       entry.stack_pointer + 8U,
                       &argument_address) ||
        !read_user_bytes(&space,
                         &allocator,
                         argument_address,
                         &byte,
                         sizeof(byte)) ||
        byte != 'x' ||
        !read_user_bytes(&space,
                         &allocator,
                         argument_address +
                             TEST_LARGE_ARGUMENT_SIZE - 1U,
                         &byte,
                         sizeof(byte)) ||
        byte != 'x' ||
        !read_user_bytes(&space,
                         &allocator,
                         argument_address +
                             TEST_LARGE_ARGUMENT_SIZE,
                         &byte,
                         sizeof(byte)) ||
        byte != 0U ||
        riscv_sv39_user_lookup(
            &space,
            RISCV_USER_ELF_STACK_TOP - UINT64_C(0x30000),
            &mapping) != RISCV_SV39_STATUS_OK ||
        riscv_sv39_user_lookup(
            &space,
            RISCV_USER_ELF_STACK_TOP - UINT64_C(0x30000) - 1U,
            &mapping) != RISCV_SV39_STATUS_NOT_MAPPED) {
        return 5U;
    }
    if (riscv_sv39_user_space_destroy(&space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&allocator) != available) {
        return 6U;
    }
    return 0U;
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
    put_u64(data_ph + 16U, RISCV_USER_ELF_STACK_RESERVE_BASE);
    put_u64(data_ph + 24U, RISCV_USER_ELF_STACK_RESERVE_BASE);
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
    struct physical_page_allocator cleanup_allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_page_table cleanup_kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_sv39_user_space cleanup_space = {0};
    struct riscv_sv39_user_space rollback_cleanup_space = {0};
    struct riscv_sv39_user_space root_cleanup_space = {0};
    struct riscv_user_elf_entry entry = {
        .entry = UINT64_C(0x1111111111111111),
        .stack_pointer = UINT64_C(0x2222222222222222),
    };
    uint64_t available;
    enum riscv_sv39_status sv39_status;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_oom_page_pool,
                                         sizeof(test_oom_page_pool))) {
        return 1U;
    }
    available = physical_page_available(&allocator);
    if (load_image(test_image,
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
    if (load_image(test_image,
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

    inaccessible_page = 0U;
    if (!init_allocator_and_kernel_table_with_access(
            &cleanup_allocator,
            &cleanup_kernel_table,
            test_cleanup_page_pool,
            sizeof(test_cleanup_page_pool),
            faulting_page_access)) {
        return 5U;
    }
    available = physical_page_available(&cleanup_allocator);
    /* The kernel root is page zero; user-space init allocates page one. */
    inaccessible_page =
        (uint64_t)(uintptr_t)test_cleanup_page_pool +
        BOAROS_PAGE_SIZE;
    make_valid_image();
    if (load_image(test_image,
                   sizeof(test_image),
                   &cleanup_allocator,
                   &cleanup_kernel_table,
                   &root_cleanup_space,
                   &entry) !=
            RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED ||
        root_cleanup_space.state != RISCV_SV39_USER_SPACE_CLEANUP ||
        root_cleanup_space.table_pages != 0U ||
        root_cleanup_space.leaf_pages != 0U ||
        root_cleanup_space.cleanup_page_owned == 0U ||
        root_cleanup_space.cleanup_page_address != inaccessible_page ||
        entry_changed(&entry) ||
        physical_page_available(&cleanup_allocator) >= available) {
        inaccessible_page = 0U;
        return 6U;
    }
    inaccessible_page = 0U;
    if (riscv_sv39_user_space_destroy(&root_cleanup_space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&cleanup_allocator) != available) {
        return 7U;
    }

    if (riscv_sv39_user_space_init(&rollback_cleanup_space,
                                   &cleanup_allocator,
                                   &cleanup_kernel_table) !=
            RISCV_SV39_STATUS_OK) {
        return 8U;
    }
    inaccessible_page =
        (uint64_t)(uintptr_t)test_cleanup_page_pool +
        3U * BOAROS_PAGE_SIZE;
    fail_after_first_access_page =
        (uint64_t)(uintptr_t)test_cleanup_page_pool +
        2U * BOAROS_PAGE_SIZE;
    fail_after_first_access_count = 0U;
    sv39_status = riscv_sv39_user_map_zeroed_page(
        &rollback_cleanup_space,
        TEST_TEXT_VA & ~BOAROS_PAGE_MASK,
        RISCV_SV39_READ);
    if (sv39_status != RISCV_SV39_STATUS_CLEANUP_REQUIRED ||
        rollback_cleanup_space.state !=
            RISCV_SV39_USER_SPACE_CLEANUP ||
        rollback_cleanup_space.table_pages != 2U ||
        rollback_cleanup_space.leaf_pages != 0U ||
        rollback_cleanup_space.cleanup_page_owned == 0U ||
        rollback_cleanup_space.cleanup_page_address != inaccessible_page) {
        inaccessible_page = 0U;
        fail_after_first_access_page = 0U;
        return 9U;
    }
    inaccessible_page = 0U;
    fail_after_first_access_page = 0U;
    if (riscv_sv39_user_space_destroy(&rollback_cleanup_space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&cleanup_allocator) != available) {
        return 10U;
    }

    /* kernel root, user root, L1 and L0 precede the first ELF leaf. */
    inaccessible_page =
        (uint64_t)(uintptr_t)test_cleanup_page_pool +
        4U * BOAROS_PAGE_SIZE;
    make_valid_image();
    if (load_image(test_image,
                   sizeof(test_image),
                   &cleanup_allocator,
                   &cleanup_kernel_table,
                   &cleanup_space,
                   &entry) !=
            RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED ||
        cleanup_space.state != RISCV_SV39_USER_SPACE_CLEANUP ||
        cleanup_space.table_pages != 3U ||
        cleanup_space.leaf_pages != 0U ||
        cleanup_space.cleanup_page_owned == 0U ||
        cleanup_space.cleanup_page_address != inaccessible_page ||
        entry_changed(&entry) ||
        physical_page_available(&cleanup_allocator) >= available) {
        inaccessible_page = 0U;
        return 11U;
    }
    inaccessible_page = 0U;
    if (riscv_sv39_user_space_destroy(&cleanup_space) !=
            RISCV_SV39_STATUS_OK ||
        physical_page_available(&cleanup_allocator) != available) {
        return 12U;
    }
    return 0U;
}

static unsigned long run_source_io_failure(void)
{
    struct physical_page_allocator allocator;
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_sv39_user_space space = {0};
    struct riscv_user_elf_entry entry = {
        .entry = UINT64_C(0x1111111111111111),
        .stack_pointer = UINT64_C(0x2222222222222222),
    };
    struct failing_read_context failure = {
        .fail_at = TEST_TEXT_OFFSET,
    };
    struct riscv_user_elf_request request = {
        .source = {
            .context = &failure,
            .size = sizeof(test_image),
            .read_at = failing_read_at,
        },
    };
    uint64_t available;

    make_valid_image();
    if (!init_allocator_and_kernel_table(&allocator,
                                         &kernel_table,
                                         test_page_pool,
                                         sizeof(test_page_pool))) {
        return 1U;
    }
    available = physical_page_available(&allocator);
    if (riscv_user_elf_load(&request,
                            &allocator,
                            &kernel_table,
                            &space,
                            &entry) != RISCV_USER_ELF_STATUS_IO ||
        space.state != RISCV_SV39_USER_SPACE_EMPTY ||
        entry_changed(&entry) ||
        physical_page_available(&allocator) != available) {
        return 2U;
    }
    return 0U;
}

unsigned long run_all_user_elf_cases(void)
{
    unsigned long result = run_user_elf_cases();

    if (result != 0U) {
        return UINT64_C(0x100) + result;
    }
    result = run_argument_and_format_failures();
    if (result != 0U) {
        return UINT64_C(0x200) + result;
    }
    result = run_request_failures();
    if (result != 0U) {
        return UINT64_C(0x300) + result;
    }
    result = run_stack_boundary_cases();
    if (result != 0U) {
        return UINT64_C(0x400) + result;
    }
    result = run_program_type_failures();
    if (result != 0U) {
        return UINT64_C(0x500) + result;
    }
    result = run_layout_failures();
    if (result != 0U) {
        return UINT64_C(0x600) + result;
    }
    result = run_oom_and_cleanup_cases();
    if (result != 0U) {
        return UINT64_C(0x700) + result;
    }
    result = run_source_io_failure();
    return result == 0U ? 0U : UINT64_C(0x800) + result;
}
