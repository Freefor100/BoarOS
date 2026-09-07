#include <arch/riscv/mm.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_POOL_PAGES 512U
#define TEST_USER_PATH UINT64_C(0x10000)
#define TEST_USER_BUFFER UINT64_C(0x20000)
#define TEST_MMAP_FIRST UINT64_C(0x10000000)
#define TEST_MMAP_SECOND UINT64_C(0x10010000)
#define TEST_MMAP_LIMIT UINT64_C(0x30000000)
#define TEST_AT_FDCWD (-100)
#define TEST_O_WRONLY UINT64_C(1)
#define TEST_O_CREAT UINT64_C(0100)
#define TEST_O_LARGEFILE UINT64_C(00100000)
#define TEST_O_DIRECTORY UINT64_C(00200000)
#define TEST_O_CLOEXEC UINT64_C(02000000)

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_POOL_PAGES]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static int fail_next_heap_release;
static int count_open_file_releases;
static uint32_t counted_open_file_releases;
static char fork_resolved_path[KERNEL_FS_PATH_MAX];
static int use_test_satp;
static uint64_t test_satp;

enum kernel_heap_status __real_kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer);
enum kernel_open_file_status __real_kernel_open_file_release(
    struct kernel_open_file_description **owner);
uint64_t __real_riscv_sv39_current_satp(void);

uint64_t __wrap_riscv_sv39_current_satp(void)
{
    return use_test_satp != 0 ? test_satp
                              : __real_riscv_sv39_current_satp();
}

enum kernel_heap_status __wrap_kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer)
{
    if (fail_next_heap_release) {
        fail_next_heap_release = 0;
        return KERNEL_HEAP_STATUS_STATE;
    }
    return __real_kernel_heap_release(heap, pointer);
}

enum kernel_open_file_status __wrap_kernel_open_file_release(
    struct kernel_open_file_description **owner)
{
    if (count_open_file_releases != 0) {
        counted_open_file_releases++;
    }
    return __real_kernel_open_file_release(owner);
}

static void *identity_access(uint64_t physical_address)
{
    return (void *)(uintptr_t)physical_address;
}

static int heap_physical_address(const void *pointer,
                                 uint64_t *physical_address)
{
    uintptr_t start = (uintptr_t)&page_pool[0];
    uintptr_t value = (uintptr_t)pointer;

    if (physical_address == 0 || value < start ||
        value - start >= sizeof(page_pool)) {
        return 0;
    }
    *physical_address = (uint64_t)value;
    return 1;
}

static int dma_physical_address(const void *pointer,
                                uint64_t size,
                                uint64_t *physical_address)
{
    uintptr_t start = (uintptr_t)&page_pool[0];
    uintptr_t value = (uintptr_t)pointer;

    if (physical_address == 0 || size == 0U || value < start ||
        size > sizeof(page_pool) - (value - start)) {
        return 0;
    }
    *physical_address = (uint64_t)value;
    return 1;
}

static void fail_files(unsigned long case_id,
                       long expected,
                       long actual) __attribute__((noreturn));

static void fail_files(unsigned long case_id,
                       long expected,
                       long actual)
{
    virt_uart_puts("BoarOS: process files test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex((unsigned long)expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex((unsigned long)actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static int write_user_bytes(const struct kernel_mm *mm,
                            uint64_t virtual_address,
                            const void *source_pointer,
                            size_t size)
{
    const unsigned char *source = source_pointer;
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
    void *pointer;

    if (kernel_mm_lookup(mm, virtual_address, &mapping) !=
            KERNEL_MM_STATUS_OK ||
        physical_page_resolve(mm->allocator,
                              mapping.physical_address &
                                  ~BOAROS_PAGE_MASK,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return 0;
    }
    *value = ((unsigned char *)pointer)
        [virtual_address & BOAROS_PAGE_MASK];
    return 1;
}

static int read_user_bytes(const struct kernel_mm *mm,
                           uint64_t virtual_address,
                           void *destination,
                           size_t size)
{
    unsigned char *destination_bytes = destination;
    size_t copied = 0U;

    while (copied < size) {
        struct kernel_mm_mapping mapping;
        const unsigned char *page;
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
            destination_bytes[copied + index] = page[offset + index];
        }
        copied += chunk;
    }
    return 1;
}

static void expect_user_pattern(const struct kernel_mm *mm,
                                uint64_t address,
                                uint64_t file_offset,
                                size_t size,
                                unsigned long case_id)
{
    size_t index;

    for (index = 0U; index < size; index++) {
        unsigned char actual = 0U;
        unsigned char expected =
            (unsigned char)('A' + ((file_offset + index) % 26U));

        if (!read_user_byte(mm, address + index, &actual) ||
            actual != expected) {
            fail_files(case_id, expected, actual);
        }
    }
}

static int create_user_mm(struct physical_page_allocator *allocator,
                          struct riscv_sv39_page_table *kernel_table,
                          struct kernel_mm *mm)
{
    struct riscv_sv39_user_space space = {0};
    uint64_t address;

    if (riscv_sv39_user_space_init(&space,
                                   allocator,
                                   kernel_table) !=
        RISCV_SV39_STATUS_OK) {
        return 0;
    }
    for (address = TEST_USER_PATH;
         address < TEST_USER_PATH + 2U * BOAROS_PAGE_SIZE;
         address += BOAROS_PAGE_SIZE) {
        if (riscv_sv39_user_map_zeroed_page(
                &space,
                address,
                RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK) {
            return 0;
        }
    }
    for (address = TEST_USER_BUFFER;
         address < TEST_USER_BUFFER + 3U * BOAROS_PAGE_SIZE;
         address += BOAROS_PAGE_SIZE) {
        if (riscv_sv39_user_map_zeroed_page(
                &space,
                address,
                RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_OK) {
            return 0;
        }
    }
    return riscv_kernel_mm_create(mm, &space) == KERNEL_MM_STATUS_OK;
}

static size_t text_length(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static void expect_open(struct kernel_files *files,
                        const struct kernel_fs_context *fs,
                        struct kernel_mm *mm,
                        int64_t dirfd,
                        const char *path,
                        uint64_t flags,
                        int64_t expected,
                        unsigned long case_id)
{
    int64_t result = INT64_MIN;

    if (!write_user_bytes(mm,
                          TEST_USER_PATH,
                          path,
                          text_length(path) + 1U) ||
        kernel_files_openat(files,
                            fs,
                            mm,
                            dirfd,
                            TEST_USER_PATH,
                            flags,
                            0U,
                            &result) != KERNEL_FILES_STATUS_OK ||
        result != expected) {
        fail_files(case_id, expected, result);
    }
}

static void run_file_operations(struct kernel_files *files,
                                const struct kernel_fs_context *fs,
                                struct kernel_mm *mm)
{
    struct kernel_files_statistics statistics;
    static unsigned char no_nul[KERNEL_FS_PATH_MAX];
    uint64_t cross_buffer =
        TEST_USER_BUFFER + BOAROS_PAGE_SIZE - 100U;
    int64_t result = INT64_MIN;
    size_t index;

    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 0, 10U);
    expect_open(files,
                fs,
                mm,
                TEST_AT_FDCWD,
                "data",
                TEST_O_LARGEFILE,
                1,
                11U);
    if (kernel_files_read(files,
                          mm,
                          0,
                          cross_buffer,
                          5000U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 5000) {
        fail_files(12U, 5000, result);
    }
    expect_user_pattern(mm, cross_buffer, 0U, 5000U, 13U);
    if (kernel_files_read(files,
                          mm,
                          1,
                          TEST_USER_BUFFER,
                          3U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 3) {
        fail_files(14U, 3, result);
    }
    expect_user_pattern(mm, TEST_USER_BUFFER, 0U, 3U, 15U);
    if (kernel_files_read(files,
                          mm,
                          1,
                          TEST_USER_BUFFER + 4U * BOAROS_PAGE_SIZE,
                          2U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EFAULT) {
        fail_files(16U, -KERNEL_EFAULT, result);
    }
    if (kernel_files_read(files,
                          mm,
                          1,
                          TEST_USER_BUFFER,
                          2U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 2) {
        fail_files(17U, 2, result);
    }
    expect_user_pattern(mm, TEST_USER_BUFFER, 3U, 2U, 18U);
    if (kernel_files_read(files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          5000U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 4000 ||
        kernel_files_read(files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_read(files,
                          mm,
                          1,
                          UINT64_MAX,
                          0U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(19U, 0, result);
    }

    expect_open(files, fs, mm, TEST_AT_FDCWD, "/missing", 0U,
                -KERNEL_ENOENT, 20U);
    expect_open(files, fs, mm, 7, "data", 0U, -KERNEL_EBADF, 21U);
    expect_open(files, fs, mm, 7, "/data", TEST_O_WRONLY,
                -KERNEL_EROFS, 22U);
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", TEST_O_CREAT,
                -KERNEL_EROFS, 23U);
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", TEST_O_DIRECTORY,
                -KERNEL_ENOTDIR, 24U);
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 3U,
                -KERNEL_EINVAL, 34U);
    expect_open(files,
                fs,
                mm,
                TEST_AT_FDCWD,
                "/data",
                UINT64_C(1) << 63U,
                -KERNEL_EINVAL,
                35U);
    /* The root directory opens read-only; reads stay reserved for
     * regular files and getdents64. */
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/", 0U, 2, 25U);
    if (kernel_files_read(files, mm, 2, TEST_USER_BUFFER, 1U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EISDIR ||
        kernel_files_close(files, 2, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(25U, 0, result);
    }
    expect_open(files, fs, mm, TEST_AT_FDCWD, "", 0U,
                -KERNEL_ENOENT, 26U);

    for (index = 0U; index < sizeof(no_nul); index++) {
        no_nul[index] = 'x';
    }
    if (!write_user_bytes(mm,
                          TEST_USER_PATH,
                          no_nul,
                          sizeof(no_nul)) ||
        kernel_files_openat(files,
                            fs,
                            mm,
                            TEST_AT_FDCWD,
                            TEST_USER_PATH,
                            0U,
                            0U,
                            &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_ENAMETOOLONG) {
        fail_files(27U, -KERNEL_ENAMETOOLONG, result);
    }

    if (kernel_files_close(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 ||
        kernel_files_close(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK || result != -KERNEL_EBADF) {
        fail_files(28U, -KERNEL_EBADF, result);
    }
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 0, 29U);
    expect_open(files,
                fs,
                mm,
                TEST_AT_FDCWD,
                "/data",
                TEST_O_CLOEXEC,
                2,
                30U);

    for (index = 3U; index < 34U; index++) {
        expect_open(files,
                    fs,
                    mm,
                    TEST_AT_FDCWD,
                    "/data",
                    0U,
                    (int64_t)index,
                    31U);
    }
    kernel_files_get_statistics(files, &statistics);
    if (statistics.capacity != 64U ||
        statistics.current_open_fds != 34U ||
        statistics.close_on_exec_fds != 1U ||
        statistics.read_chunks < 4U ||
        statistics.bytes_read != 9005U) {
        fail_files(32U, 64, statistics.capacity);
    }

    if (kernel_files_close_on_exec(files) != KERNEL_FILES_STATUS_OK) {
        fail_files(36U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    kernel_files_get_statistics(files, &statistics);
    if (statistics.current_open_fds != 33U ||
        statistics.close_on_exec_fds != 0U ||
        kernel_files_read(files,
                          mm,
                          2,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_read(files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 1) {
        fail_files(37U, 1, result);
    }
    expect_open(files,
                fs,
                mm,
                TEST_AT_FDCWD,
                "/data",
                TEST_O_CLOEXEC,
                2,
                38U);
    fail_next_heap_release = 1;
    if (kernel_files_close_on_exec(files) !=
            KERNEL_FILES_STATUS_CLEANUP_REQUIRED ||
        kernel_files_close(files, 2, &result) !=
            KERNEL_FILES_STATUS_OK || result != -KERNEL_EBADF) {
        fail_files(39U, -KERNEL_EBADF, result);
    }
}

static void run_fork_operations(struct kernel_files *parent_files,
                                const struct kernel_fs_context *parent_fs,
                                struct kernel_mm *mm)
{
    struct kernel_files child_files = {0};
    struct kernel_fs_context child_fs = {0};
    struct kernel_open_file_description *pinned = 0;
    struct kernel_vfs_mount *resolved_mount = 0;
    uint64_t pinned_page;
    size_t valid_bytes;
    int64_t result = INT64_MIN;
    int path_result = INT32_MIN;

    expect_open(parent_files,
                parent_fs,
                mm,
                TEST_AT_FDCWD,
                "/data",
                0U,
                0,
                40U);
    if (kernel_files_pin(parent_files, 0, &pinned, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 || pinned == 0 ||
        kernel_files_close(parent_files, 0, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 ||
        kernel_open_file_get_page(pinned,
                                  0U,
                                  &pinned_page,
                                  &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK || valid_bytes != 4096U ||
        physical_page_release(mm->allocator, pinned_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_open_file_release(&pinned) != KERNEL_OPEN_FILE_STATUS_OK ||
        pinned != 0) {
        fail_files(52U, 0, result);
    }
    expect_open(parent_files,
                parent_fs,
                mm,
                TEST_AT_FDCWD,
                "/data",
                0U,
                0,
                53U);
    if (kernel_files_read(parent_files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 1) {
        fail_files(41U, 1, result);
    }
    expect_user_pattern(mm, TEST_USER_BUFFER, 0U, 1U, 42U);
    if (kernel_files_fork(&child_files, parent_files) !=
            KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_fork(&child_fs, parent_fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        !kernel_files_is_live(&child_files) ||
        !kernel_fs_context_is_live(&child_fs)) {
        fail_files(43U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    if (kernel_files_read(&child_files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 1) {
        fail_files(44U, 1, result);
    }
    expect_user_pattern(mm, TEST_USER_BUFFER, 1U, 1U, 45U);
    if (kernel_files_close(&child_files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_read(parent_files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 1) {
        fail_files(46U, 1, result);
    }
    expect_user_pattern(mm, TEST_USER_BUFFER, 2U, 1U, 47U);
    if (kernel_fs_context_resolve_kernel_path(&child_fs,
                                              TEST_AT_FDCWD,
                                              "data",
                                              4U,
                                              fork_resolved_path,
                                              sizeof(fork_resolved_path),
                                              &resolved_mount,
                                              &path_result) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        path_result != 0 || resolved_mount == 0 ||
        fork_resolved_path[0] != '/' ||
        fork_resolved_path[1] != 'd') {
        fail_files(48U, 0, path_result);
    }
    if (kernel_files_release(&child_files) != KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_release(&child_fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        kernel_files_close(parent_files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(49U, 0, result);
    }
}

static void run_mmap_operations(struct kernel_files *files,
                                const struct kernel_fs_context *fs,
                                struct kernel_mm *mm,
                                struct physical_page_allocator *allocator)
{
    struct kernel_open_file_description *first_pin = 0;
    struct kernel_open_file_description *second_pin = 0;
    struct kernel_mm child = {0};
    struct kernel_mm_mapping first_mapping;
    struct kernel_mm_mapping second_mapping;
    uint64_t first_address = UINT64_MAX;
    uint64_t second_address = UINT64_MAX;
    uint64_t private_page;
    uint64_t child_satp;
    void *page;
    int64_t result = INT64_MIN;
    unsigned char replacement = 0xe1U;
    size_t copied = SIZE_MAX;

    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 0, 60U);
    if (kernel_files_pin(files, 0, &first_pin, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 || first_pin == 0 ||
        kernel_mm_mmap_file_private(
            mm,
            &first_pin,
            TEST_MMAP_FIRST,
            4U * BOAROS_PAGE_SIZE,
            0U,
            KERNEL_MM_READ | KERNEL_MM_WRITE,
            KERNEL_MM_MAP_FIXED_NOREPLACE,
            &first_address) != KERNEL_MM_STATUS_OK ||
        first_pin != 0 || first_address != TEST_MMAP_FIRST) {
        fail_files(61U, KERNEL_MM_STATUS_OK, result);
    }

    counted_open_file_releases = 0U;
    count_open_file_releases = 1;
    if (kernel_files_pin(files, 0, &second_pin, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 || second_pin == 0 ||
        kernel_mm_mmap_file_private(
            mm,
            &second_pin,
            TEST_MMAP_SECOND,
            3U * BOAROS_PAGE_SIZE,
            0U,
            KERNEL_MM_READ | KERNEL_MM_WRITE,
            KERNEL_MM_MAP_FIXED_NOREPLACE,
            &second_address) != KERNEL_MM_STATUS_OK ||
        second_pin != 0 || second_address != TEST_MMAP_SECOND) {
        count_open_file_releases = 0;
        fail_files(61U, KERNEL_MM_STATUS_OK, result);
    }
    count_open_file_releases = 0;
    if (counted_open_file_releases != 1U ||
        kernel_files_close(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0) {
        fail_files(68U, 1, counted_open_file_releases);
    }

    /* A write-first miss reads directly into a private page. */
    if (kernel_mm_resolve_user_fault(
            mm,
            first_address + 2U * BOAROS_PAGE_SIZE,
            KERNEL_MM_WRITE) != KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(mm,
                         first_address + 2U * BOAROS_PAGE_SIZE,
                         &first_mapping) != KERNEL_MM_STATUS_OK ||
        (first_mapping.permissions & KERNEL_MM_WRITE) == 0U ||
        physical_page_resolve(
            allocator,
            first_mapping.physical_address & ~BOAROS_PAGE_MASK,
            &page) != PHYSICAL_PAGE_STATUS_OK ||
        ((unsigned char *)page)[0] !=
            (unsigned char)('A' + ((2U * BOAROS_PAGE_SIZE) % 26U)) ||
        ((unsigned char *)page)[807] !=
            (unsigned char)('A' + (8999U % 26U)) ||
        ((unsigned char *)page)[808] != 0U) {
        fail_files(62U, 0, -1);
    }
    private_page = first_mapping.physical_address & ~BOAROS_PAGE_MASK;

    /* A read alias receives the shared cache page, not the private copy. */
    if (kernel_mm_resolve_user_fault(
            mm,
            second_address + 2U * BOAROS_PAGE_SIZE,
            KERNEL_MM_READ) != KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(mm,
                         second_address + 2U * BOAROS_PAGE_SIZE,
                         &second_mapping) != KERNEL_MM_STATUS_OK ||
        (second_mapping.physical_address & ~BOAROS_PAGE_MASK) ==
            private_page ||
        physical_page_resolve(
            allocator,
            second_mapping.physical_address & ~BOAROS_PAGE_MASK,
            &page) != PHYSICAL_PAGE_STATUS_OK ||
        ((unsigned char *)page)[0] !=
            (unsigned char)('A' + ((2U * BOAROS_PAGE_SIZE) % 26U)) ||
        ((unsigned char *)page)[808] != 0U ||
        kernel_mm_resolve_user_fault(
            mm,
            first_address + 3U * BOAROS_PAGE_SIZE,
            KERNEL_MM_READ) != KERNEL_MM_STATUS_BUS_FAULT) {
        fail_files(63U, KERNEL_MM_STATUS_OK, -1);
    }

    /* Two read mappings share cache storage; writing one breaks COW. */
    if (kernel_mm_resolve_user_fault(mm,
                                     first_address,
                                     KERNEL_MM_READ) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_resolve_user_fault(mm,
                                     second_address,
                                     KERNEL_MM_READ) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(mm, first_address, &first_mapping) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(mm, second_address, &second_mapping) !=
            KERNEL_MM_STATUS_OK ||
        (first_mapping.physical_address & ~BOAROS_PAGE_MASK) !=
            (second_mapping.physical_address & ~BOAROS_PAGE_MASK) ||
        kernel_copy_to_user(mm,
                            first_address,
                            &replacement,
                            sizeof(replacement),
                            &copied) != KERNEL_UACCESS_STATUS_OK ||
        copied != sizeof(replacement) ||
        kernel_mm_lookup(mm, first_address, &first_mapping) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_lookup(mm, second_address, &second_mapping) !=
            KERNEL_MM_STATUS_OK ||
        (first_mapping.physical_address & ~BOAROS_PAGE_MASK) ==
            (second_mapping.physical_address & ~BOAROS_PAGE_MASK) ||
        !read_user_byte(mm, first_address, &replacement) ||
        replacement != 0xe1U ||
        !read_user_byte(mm, second_address, &replacement) ||
        replacement != 'A') {
        fail_files(64U, 0, -1);
    }

    /* Fork keeps an independent OFD registry after the parent unmaps. */
    if (kernel_mm_fork(&child, mm) != KERNEL_MM_STATUS_OK ||
        riscv_kernel_mm_satp(&child, &child_satp) !=
            KERNEL_MM_STATUS_OK ||
        kernel_mm_munmap(mm,
                         first_address,
                         4U * BOAROS_PAGE_SIZE) != KERNEL_MM_STATUS_OK ||
        kernel_mm_munmap(mm,
                         second_address,
                         3U * BOAROS_PAGE_SIZE) != KERNEL_MM_STATUS_OK) {
        fail_files(65U, KERNEL_MM_STATUS_OK, -1);
    }
    test_satp = child_satp;
    if (kernel_mm_resolve_user_fault(
            &child,
            second_address + BOAROS_PAGE_SIZE,
            KERNEL_MM_READ) != KERNEL_MM_STATUS_OK ||
        !read_user_byte(&child,
                        second_address + BOAROS_PAGE_SIZE,
                        &replacement) ||
        replacement !=
            (unsigned char)('A' + (BOAROS_PAGE_SIZE % 26U))) {
        fail_files(66U, KERNEL_MM_STATUS_OK, -1);
    }
    test_satp = test_satp ^ UINT64_C(1);
    if (kernel_mm_release(&child) != KERNEL_MM_STATUS_OK) {
        fail_files(67U, KERNEL_MM_STATUS_OK, -1);
    }
    test_satp = 0U;
}

static void run_console_operations(struct kernel_files *files,
                                   const struct kernel_fs_context *fs,
                                   struct kernel_mm *mm)
{
    struct kernel_files child_files = {0};
    struct kernel_files_statistics statistics;
    static const char marker[] = "BoarOS: files console write ok\n";
    uint64_t partial_buffer =
        TEST_USER_BUFFER + 3U * BOAROS_PAGE_SIZE - 8U;
    int64_t result = INT64_MIN;

    if (kernel_files_open_console(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_open_console(files, 1, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_open_console(files, 2, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_open_console(files, 1, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_open_console(files, -1, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(70U, 0, result);
    }

    /* The marker must reach the serial console through the OFD sink. */
    if (!write_user_bytes(mm,
                          TEST_USER_PATH,
                          marker,
                          sizeof(marker) - 1U) ||
        kernel_files_write(files,
                           mm,
                           1,
                           TEST_USER_PATH,
                           sizeof(marker) - 1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(sizeof(marker) - 1U)) {
        fail_files(71U, (long)(sizeof(marker) - 1U), (long)result);
    }

    /* Regular descriptors are read-only, so writes report EBADF.  The
     * console read blocks until the tick poller feeds it, which only the
     * serial-input end-to-end run can exercise; here the non-blocking
     * edges are checked: an empty request returns 0 and a bad buffer
     * faults before any wait. */
        expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 3, 72U);
        if (kernel_files_write(files,
                           mm,
                           3,
                           TEST_USER_PATH,
                           1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_read(files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          0U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_read(files,
                          mm,
                          0,
                          UINT64_MAX,
                          4U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EFAULT ||
        kernel_files_write(files,
                           mm,
                           1,
                           UINT64_MAX,
                           1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EFAULT ||
        kernel_files_write(files,
                           mm,
                           1,
                           TEST_USER_PATH,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_write(files,
                           mm,
                           7,
                           TEST_USER_PATH,
                           1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(73U, 0, result);
    }
    
    /* A copy that faults partway reports the emitted prefix. */
        if (kernel_files_write(files,
                           mm,
                           1,
                           partial_buffer,
                           16U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 8) {
        fail_files(74U, 8, result);
    }
    
        /* Fork shares the console descriptions and the child keeps writing. */
    if (!write_user_bytes(mm,
                          TEST_USER_BUFFER + BOAROS_PAGE_SIZE,
                          marker,
                          sizeof(marker) - 1U) ||
        kernel_files_fork(&child_files, files) != KERNEL_FILES_STATUS_OK ||
        kernel_files_write(&child_files,
                           mm,
                           2,
                           TEST_USER_BUFFER + BOAROS_PAGE_SIZE,
                           sizeof(marker) - 1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(sizeof(marker) - 1U) ||
        kernel_files_release(&child_files) != KERNEL_FILES_STATUS_OK) {
        fail_files(75U, 0, result);
    }

        kernel_files_get_statistics(files, &statistics);
    if (statistics.write_calls != 6U ||
        statistics.write_failures != 4U ||
        statistics.bytes_written != (sizeof(marker) - 1U) + 8U ||
        statistics.current_open_fds != 4U) {
        fail_files(76U, 4, statistics.write_failures);
    }
}

static void run_seek_stat_operations(struct kernel_files *files,
                                     const struct kernel_fs_context *fs,
                                     struct kernel_mm *mm)
{
    struct kernel_linux_stat stat;
    static const char empty_path[] = "";
    uint64_t stat_buffer = TEST_USER_BUFFER + 2U * BOAROS_PAGE_SIZE;
    int64_t result = INT64_MIN;

    /* The console descriptors 0/1/2 and the /data descriptor 3 are open;
     * a fresh open therefore receives fd 4. */
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 4, 80U);

    /* The three whence forms and their exact repositioning results. */
    if (kernel_files_lseek(files, 4, 10U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 10 ||
        kernel_files_read(files, mm, 4, TEST_USER_BUFFER, 1U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        kernel_files_lseek(files, 4, 0U, KERNEL_FILES_SEEK_END, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 9000 ||
        kernel_files_read(files, mm, 4, TEST_USER_BUFFER, 1U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_lseek(files, 4, -5, KERNEL_FILES_SEEK_CUR, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 8995 ||
        kernel_files_lseek(files, 4, 0U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_lseek(files,
                           4,
                           INT64_MAX,
                           KERNEL_FILES_SEEK_CUR,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != INT64_MAX ||
        kernel_files_lseek(files,
                           4,
                           INT64_MAX,
                           KERNEL_FILES_SEEK_CUR,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL ||
        kernel_files_lseek(files, 4, -1, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL ||
        kernel_files_lseek(files, 4, 0U, 7U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL ||
        kernel_files_lseek(files, 9, 0U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_lseek(files, 1, 0U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_ESPIPE) {
        fail_files(82U, 0, result);
    }
    /* Rewind after the accepted huge seek; a rejected one does not move. */
    if (kernel_files_lseek(files, 4, 0U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_read(files, mm, 4, TEST_USER_BUFFER, 1U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 1) {
        fail_files(83U, 1, result);
    }

    /* fstat reports the ext4 mode/size/ino and the console character
     * device identity, with faults reported as EFAULT. */
    if (kernel_files_fstat(files, mm, 4, stat_buffer, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, stat_buffer, &stat, sizeof(stat))) {
        fail_files(84U, 0, result);
    }
    if ((stat.st_mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        stat.st_ino == 0U || stat.st_nlink != 1U ||
        stat.st_size != 9000 || stat.st_blksize != BOAROS_PAGE_SIZE ||
        stat.st_blocks != 18U || stat.st_rdev != 0U) {
        fail_files(85U, 0, stat.st_mode);
    }
    if (kernel_files_fstat(files, mm, 1, stat_buffer, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, stat_buffer, &stat, sizeof(stat)) ||
        (stat.st_mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFCHR ||
        stat.st_rdev != UINT64_C(0x501) ||
        kernel_files_fstat(files, mm, 5, stat_buffer, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_fstat(files, mm, 4, UINT64_MAX, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EFAULT) {
        fail_files(86U, 0, result);
    }

    /* newfstatat resolves absolute and cwd-relative paths, and reports
     * the documented negative cases. */
    if (!write_user_bytes(mm, TEST_USER_PATH, "/data", 6U) ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             TEST_AT_FDCWD,
                             TEST_USER_PATH,
                             stat_buffer,
                             0U,
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, stat_buffer, &stat, sizeof(stat)) ||
        stat.st_size != 9000 ||
        (stat.st_mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        fail_files(87U, 0, result);
    }
    if (!write_user_bytes(mm, TEST_USER_PATH, "data", 5U) ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             TEST_AT_FDCWD,
                             TEST_USER_PATH,
                             stat_buffer,
                             0U,
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, stat_buffer, &stat, sizeof(stat)) ||
        stat.st_size != 9000) {
        fail_files(88U, 0, result);
    }
    if (!write_user_bytes(mm, TEST_USER_PATH, empty_path, 1U) ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             TEST_AT_FDCWD,
                             TEST_USER_PATH,
                             stat_buffer,
                             0U,
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_ENOENT ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             TEST_AT_FDCWD,
                             TEST_USER_PATH,
                             stat_buffer,
                             KERNEL_FILES_AT_EMPTY_PATH,
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             1,
                             TEST_USER_PATH,
                             stat_buffer,
                             KERNEL_FILES_AT_EMPTY_PATH,
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, stat_buffer, &stat, sizeof(stat)) ||
        (stat.st_mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFCHR ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             TEST_AT_FDCWD,
                             TEST_USER_PATH,
                             stat_buffer,
                             UINT64_C(0x2000),
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL) {
        fail_files(89U, 0, result);
    }
    if (!write_user_bytes(mm, TEST_USER_PATH, "data", 5U) ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             7,
                             TEST_USER_PATH,
                             stat_buffer,
                             0U,
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(89U, 0, result);
    }
    if (kernel_files_close(files, 4, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(90U, 0, result);
    }
}

static char directory_name[256];

static int collect_dirent_name(struct kernel_mm *mm,
                                uint64_t address,
                                uint64_t *offset)
{
    unsigned char header[19];
    uint16_t record_length = 0;
    size_t index;

    if (!read_user_bytes(mm, address, header, sizeof(header))) {
        return -1;
    }
    for (index = 0U; index < sizeof(record_length); index++) {
        record_length |= (uint16_t)((uint16_t)header[16U + index] <<
                                    (8U * index));
    }
    if (record_length < 20U || record_length > 280U ||
        !read_user_bytes(mm,
                         address + 19U,
                         directory_name,
                         record_length - 19U)) {
        return -1;
    }
    directory_name[record_length - 19U < sizeof(directory_name)
                       ? record_length - 19U
                       : sizeof(directory_name) - 1U] = '\0';
    *offset = record_length;
    return (int)header[18U];
}

static int dirent_name_is(const char *name, const char *expected)
{
    size_t index = 0U;

    while (expected[index] != '\0') {
        if (name[index] != expected[index]) {
            return 0;
        }
        index++;
    }
    return name[index] == '\0';
}

static void run_directory_operations(struct kernel_files *files,
                                     const struct kernel_fs_context *fs,
                                     struct kernel_mm *mm)
{
    uint64_t dir_buffer = TEST_USER_BUFFER;
    uint64_t offset;
    int saw_data = 0;
    int saw_lost = 0;
    int64_t result = INT64_MIN;

    /* The root directory opens read-only and lists its entries. */
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/", 0U, 0, 110U);
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 1, 110U);
    if (kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result <= 0) {
        fail_files(111U, 0, result);
    }

    /* Walk the returned records: the image holds /data and lost+found. */
    {
        unsigned int dump;

        for (dump = 0U; dump < 64U; dump++) {
            unsigned char byte = 0U;

            (void)read_user_byte(mm, dir_buffer + dump, &byte);
            virt_uart_put_hex(byte);
            virt_uart_putc(dump % 16U == 15U ? '\n' : ' ');
        }
    }
    offset = 0U;
    while (offset < (uint64_t)result) {
        uint64_t record_size = 0U;
        int type = collect_dirent_name(mm,
                                       dir_buffer + offset,
                                       &record_size);

        if (type < 0) {
            fail_files(112U, 0, type);
        }
        offset += record_size;
        if (dirent_name_is(directory_name, "data") && type == KERNEL_VFS_DT_REG) {
            saw_data = 1;
        }
        if (dirent_name_is(directory_name, "lost+found") &&
            type == KERNEL_VFS_DT_DIR) {
            saw_lost = 1;
        }
    }
    if (!saw_data || !saw_lost) {
        fail_files(113U, 0, (long)saw_data);
    }

    /* The next call reports end-of-directory. */
    if (kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_getdents(files,
                              mm,
                              1,
                              dir_buffer,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_ENOTDIR ||
        kernel_files_getdents(files,
                              mm,
                              9,
                              dir_buffer,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_getdents(files,
                              mm,
                              0,
                              UINT64_MAX,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EFAULT) {
        fail_files(114U, 0, result);
    }
    /* A buffer too small for even one entry is EINVAL, from the start. */
    if (kernel_files_lseek(files, 0, 0U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              8U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL) {
        fail_files(118U, 0, result);
    }

    /* Seek repositions the entry cursor. */
    if (kernel_files_lseek(files, 0, 1U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result <= 0) {
        fail_files(115U, 0, result);
    }
    offset = 0U;
    {
        int type = collect_dirent_name(mm,
                                       dir_buffer + offset,
                                       &offset);

        if (type < 0 || !dirent_name_is(directory_name, "data")) {
            fail_files(116U, 0, type);
        }
    }
    if (kernel_files_close(files, 0, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 1, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(117U, 0, result);
    }
}

static void run_dup_fcntl_operations(struct kernel_files *files,
                                     const struct kernel_fs_context *fs,
                                     struct kernel_mm *mm)
{
    struct kernel_files child_files = {0};
    int64_t result = INT64_MIN;

    if (kernel_files_open_console(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_open_console(files, 1, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_open_console(files, 2, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(100U, 0, result);
    }
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 3, 100U);

    /* dup shares one open-file description, so the offset is common. */
    if (kernel_files_dup(files, 3, &result) != KERNEL_FILES_STATUS_OK ||
        result != 4 ||
        kernel_files_read(files, mm, 3, TEST_USER_BUFFER, 3U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 3 ||
        kernel_files_read(files, mm, 4, TEST_USER_BUFFER, 2U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 2 ||
        kernel_files_dup(files, 3, &result) != KERNEL_FILES_STATUS_OK ||
        result != 5 ||
        kernel_files_close(files, 4, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 5, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_dup(files, 40, &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(101U, 0, result);
    }

    /* fd flags and file flags are distinct; SETFL accepts only the
     * modifiable set. */
    if (kernel_files_fcntl(files, 3, KERNEL_FILES_F_GETFD, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_SETFD,
                           1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_GETFD,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        kernel_files_fcntl(files, 3, KERNEL_FILES_F_GETFL, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_SETFL,
                           KERNEL_FILES_O_NONBLOCK | KERNEL_FILES_O_APPEND,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files, 3, KERNEL_FILES_F_GETFL, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(KERNEL_FILES_O_NONBLOCK |
                            KERNEL_FILES_O_APPEND) ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_SETFL,
                           KERNEL_FILES_O_NONBLOCK |
                               KERNEL_FILES_O_LARGEFILE,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_SETFL,
                           UINT64_C(0x40000),
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL ||
        kernel_files_fcntl(files, 40, KERNEL_FILES_F_GETFD, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(102U, 0, result);
    }

    /* dup2 replaces the target, treats oldfd == newfd as a no-op, and
     * dup3 pins CLOEXEC but rejects other flags. */
    if (kernel_files_dup(files, 3, &result) != KERNEL_FILES_STATUS_OK ||
        result != 4 ||
        kernel_files_dup2(files, 3, 4, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 4 ||
        kernel_files_dup2(files, 3, 3, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 3) {
        fail_files(103U, 0, result);
    }
    if (kernel_files_dup3(files, 3, 3, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL ||
        kernel_files_dup3(files,
                          3,
                          6,
                          KERNEL_FILES_O_CLOEXEC,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 6) {
        fail_files(108U, 0, result);
    }
    if (kernel_files_fcntl(files, 6, KERNEL_FILES_F_GETFD, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        kernel_files_dup3(files, 3, 7, KERNEL_FILES_O_NONBLOCK, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL ||
        kernel_files_dup3(files, 3, -1, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL ||
        kernel_files_dup2(files, 40, 7, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_dup2(files, 3, 2000, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(109U, 0, result);
    }

    /* F_DUPFD honors the lower bound, and F_DUPFD_CLOEXEC marks it. */
    if (kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_DUPFD,
                           10U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 10 ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_DUPFD,
                           4U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 5 ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_DUPFD_CLOEXEC,
                           20U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 20 ||
        kernel_files_fcntl(files, 20, KERNEL_FILES_F_GETFD, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_DUPFD,
                           (uint64_t)-50,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EINVAL) {
        fail_files(104U, 0, result);
    }

    /* Fork keeps the duplicated descriptors and their fd flags. */
    if (kernel_files_fork(&child_files, files) != KERNEL_FILES_STATUS_OK ||
        kernel_files_fcntl(&child_files,
                           20,
                           KERNEL_FILES_F_GETFD,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        kernel_files_release(&child_files) != KERNEL_FILES_STATUS_OK) {
        fail_files(105U, 0, result);
    }

    if (kernel_files_close_on_exec(files) != KERNEL_FILES_STATUS_OK) {
        fail_files(106U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    /* CLOEXEC descriptors 6 and 20 are gone, and fd 3's CLOEXEC flag was
     * set through F_SETFD earlier; the plain dups remain. */
    if (kernel_files_close(files, 3, &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_close(files, 0, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 1, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 2, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 4, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 5, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 10, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(107U, 0, result);
    }
}

static void run_files_test(const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    struct kernel_page_cache page_cache = {0};
    struct riscv_sv39_page_table kernel_table = {0};
    struct riscv_virtio_mmio_block device = {0};
    struct kernel_vfs_mount mount = {0};
    struct kernel_mm mm = {0};
    struct kernel_files files = {0};
    struct kernel_fs_context fs = {0};
    uint64_t baseline;
    uint32_t index;
    int found = 0;

    if (dtb_read_boot_info(dtb, &info) != DTB_STATUS_OK ||
        info.timebase_frequency == 0U) {
        fail_files(1U, DTB_STATUS_OK, -1);
    }
    layout.usable_count = 1U;
    layout.usable[0].base = (uint64_t)(uintptr_t)&page_pool[0];
    layout.usable[0].size = sizeof(page_pool);
    if (physical_page_allocator_init(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(&allocator, identity_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_finalize(&allocator) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_heap_init(&heap, &allocator, heap_physical_address) !=
            KERNEL_HEAP_STATUS_OK ||
        riscv_sv39_page_table_init(&kernel_table, &allocator) !=
            RISCV_SV39_STATUS_OK) {
        fail_files(2U, 0, -1);
    }
    kernel_table.state = RISCV_SV39_STATE_ACTIVE;
    baseline = physical_page_available(&allocator);
    if (kernel_page_cache_init(&page_cache, &heap, &allocator) !=
        KERNEL_PAGE_CACHE_STATUS_OK) {
        fail_files(2U, 0, -1);
    }

    for (index = 0U; index < info.virtio_mmio_count; index++) {
        enum riscv_virtio_mmio_block_status status =
            riscv_virtio_mmio_block_init(
                &device,
                (volatile void *)(uintptr_t)info.virtio_mmio[index].base,
                info.virtio_mmio[index].size,
                &allocator,
                dma_physical_address,
                info.timebase_frequency);

        if (status == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) {
            found = 1;
            break;
        }
        if (status != RISCV_VIRTIO_MMIO_BLOCK_STATUS_NOT_BLOCK) {
            fail_files(3U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, status);
        }
    }
    if (!found ||
        kernel_vfs_mount_root_readonly(&mount,
                                       &device.block,
                                       &heap,
                                       &page_cache) != 0 ||
        !create_user_mm(&allocator, &kernel_table, &mm) ||
        kernel_mm_vma_enable(&mm, &heap) != KERNEL_MM_STATUS_OK ||
        kernel_mm_brk_initialize(&mm,
                                 UINT64_C(0x1000000),
                                 TEST_MMAP_LIMIT) != KERNEL_MM_STATUS_OK ||
        kernel_fs_context_create(&fs, &mount, &heap) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(4U, 0, -1);
    }
    if (riscv_kernel_mm_satp(&mm, &test_satp) != KERNEL_MM_STATUS_OK) {
        fail_files(4U, 0, -1);
    }
    use_test_satp = 1;

    run_fork_operations(&files, &fs, &mm);
    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK) {
        fail_files(50U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    files = (struct kernel_files){0};
    if (kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(51U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    run_file_operations(&files, &fs, &mm);

    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK) {
        fail_files(58U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    files = (struct kernel_files){0};
    if (kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(59U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    run_console_operations(&files, &fs, &mm);
    run_seek_stat_operations(&files, &fs, &mm);

    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK) {
        fail_files(58U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    files = (struct kernel_files){0};
    if (kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(99U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    run_dup_fcntl_operations(&files, &fs, &mm);

    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK) {
        fail_files(58U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    files = (struct kernel_files){0};
    if (kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(109U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    run_directory_operations(&files, &fs, &mm);

    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK) {
        fail_files(58U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    files = (struct kernel_files){0};
    if (kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(69U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    /* Last: the COW fork inside leaves the raw test mappings
     * write-protected for the remaining callers. */
    run_mmap_operations(&files, &fs, &mm, &allocator);

    use_test_satp = 0;
    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_release(&fs) != KERNEL_FS_CONTEXT_STATUS_OK ||
        kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        kernel_vfs_unmount(&mount) != 0 ||
        kernel_page_cache_destroy(&page_cache) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        riscv_virtio_mmio_block_destroy(&device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        fail_files(5U,
                   (long)baseline,
                   (long)physical_page_available(&allocator));
    }
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;

    run_files_test(dtb);
    virt_uart_puts("BoarOS: process files tests passed\n");
    sbi_shutdown();
}
