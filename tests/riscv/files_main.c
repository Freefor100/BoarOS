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
#include <kernel/vma.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#ifdef FILES_PARTIAL_WRITE_TEST
#include "../../fs/vfs_internal.h"

#include <ext4.h>
#include <ext4_errno.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_POOL_PAGES 512U
#define TEST_USER_PATH UINT64_C(0x10000)
#define TEST_USER_BUFFER UINT64_C(0x20000)
#define TEST_MMAP_FIRST UINT64_C(0x10000000)
#define TEST_MMAP_SECOND UINT64_C(0x10010000)
#define TEST_MMAP_LIMIT UINT64_C(0x30000000)
#define TEST_AT_FDCWD (-100)
#define TEST_O_WRONLY UINT64_C(1)
#define TEST_O_CREAT UINT64_C(0100)
#define TEST_O_NONBLOCK UINT64_C(00004000)
#define TEST_O_DIRECT UINT64_C(00040000)
#define TEST_O_LARGEFILE UINT64_C(00100000)
#define TEST_O_DIRECTORY UINT64_C(00200000)
#define TEST_O_CLOEXEC UINT64_C(02000000)
#define TEST_EPOLL_CLOEXEC UINT32_C(0x80000)

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_POOL_PAGES]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static int count_open_file_releases;
static uint32_t counted_open_file_releases;
static char fork_resolved_path[KERNEL_FS_PATH_MAX];
static unsigned fail_physical_allocation;
enum physical_page_status __real_physical_page_allocate(
    struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(
    struct physical_page_allocator *allocator, uint64_t *out)
{
    if (fail_physical_allocation && --fail_physical_allocation == 0)
        return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(allocator, out);
}

static unsigned fail_metadata_allocation;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(
    struct kernel_heap *, size_t, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(
    struct kernel_heap *heap, size_t count, size_t size, void **out)
{
    if (fail_metadata_allocation && --fail_metadata_allocation == 0)
        return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(heap, count, size, out);
}

static int use_test_satp;
static uint64_t test_satp;
#ifdef FILES_PARTIAL_WRITE_TEST
static int inject_partial_write_error;
static int inject_truncate_error;
static int inject_usercopy_write_mode;
static unsigned int usercopy_write_skip;
#endif

enum kernel_heap_status __real_kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer);
enum kernel_open_file_status __real_kernel_open_file_release(
    struct kernel_open_file_description **owner);
uint64_t __real_riscv_sv39_current_satp(void);
#ifdef FILES_PARTIAL_WRITE_TEST
int __real_ext4_fwrite(ext4_file *file,
                       const void *buffer,
                       size_t size,
                       size_t *bytes_written);
int __real_ext4_ftruncate(ext4_file *file, uint64_t size);
#endif

uint64_t __wrap_riscv_sv39_current_satp(void)
{
    return use_test_satp != 0 ? test_satp
                              : __real_riscv_sv39_current_satp();
}

enum kernel_open_file_status __wrap_kernel_open_file_release(
    struct kernel_open_file_description **owner)
{
    if (count_open_file_releases != 0) {
        counted_open_file_releases++;
    }
    return __real_kernel_open_file_release(owner);
}

#ifdef FILES_PARTIAL_WRITE_TEST
int __wrap_ext4_fwrite(ext4_file *file,
                       const void *buffer,
                       size_t size,
                       size_t *bytes_written)
{
    size_t committed = 0U;
    int result;

    if (inject_usercopy_write_mode != 0) {
        if (usercopy_write_skip != 0U) {
            usercopy_write_skip--;
        } else {
            int mode = inject_usercopy_write_mode;
            size_t limit = mode == 3 ? 0U : (size < 5U ? size : 5U);

            inject_usercopy_write_mode = 0;
            result = __real_ext4_fwrite(file, buffer, limit, &committed);
            *bytes_written = committed;
            return result == EOK && mode != 1 ? EIO : result;
        }
    }
    if (!inject_partial_write_error) {
        return __real_ext4_fwrite(file, buffer, size, bytes_written);
    }
    inject_partial_write_error = 0;
    if (size < 5U) {
        return __real_ext4_fwrite(file, buffer, size, bytes_written);
    }

    result = __real_ext4_fwrite(file, buffer, 5U, &committed);
    if (bytes_written != 0) {
        *bytes_written = committed;
    }
    return result == EOK && committed == 5U ? EIO : result;
}

int __wrap_ext4_ftruncate(ext4_file *file, uint64_t size)
{
    int result;

    if (!inject_truncate_error) {
        return __real_ext4_ftruncate(file, size);
    }
    inject_truncate_error = 0;
    result = __real_ext4_ftruncate(file, size);
    return result == EOK ? EIO : result;
}
#endif

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
    expect_open(files,
                fs,
                mm,
                TEST_AT_FDCWD,
                "/data",
                TEST_O_NONBLOCK,
                2,
                212U);
    if (kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_GETFL,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)TEST_O_NONBLOCK ||
        kernel_files_read(files,
                          mm,
                          2,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_SETFL,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_GETFL,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, 2, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(213U, 0, result);
    }
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
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/missing", TEST_O_CREAT,
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
    expect_open(files,
                fs,
                mm,
                TEST_AT_FDCWD,
                "/data",
                TEST_O_DIRECT,
                -KERNEL_ENOTSUP,
                214U);
    /* The root directory opens read-only; reads stay reserved for
     * regular files and getdents64. */
    expect_open(files,
                fs,
                mm,
                TEST_AT_FDCWD,
                "/",
                TEST_O_DIRECTORY | TEST_O_NONBLOCK,
                2,
                215U);
    if (kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_GETFL,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(TEST_O_DIRECTORY | TEST_O_NONBLOCK) ||
        kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_SETFL,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_GETFL,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)TEST_O_DIRECTORY ||
        kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_SETFL,
                           TEST_O_NONBLOCK,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_fcntl(files,
                           2,
                           KERNEL_FILES_F_GETFL,
                           0U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(TEST_O_DIRECTORY | TEST_O_NONBLOCK) ||
        kernel_files_read(files, mm, 2, TEST_USER_BUFFER, 1U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EISDIR ||
        kernel_files_close(files, 2, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(216U, 0, result);
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
        statistics.bytes_read != 9006U) {
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
    if (kernel_files_close_on_exec(files) !=
            KERNEL_FILES_STATUS_OK ||
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

static void run_shared_handle_operations(
    struct kernel_files *files,
    const struct kernel_fs_context *fs,
    struct kernel_mm *mm)
{
    struct kernel_files shared_files = {0};
    struct kernel_fs_context shared_fs = {0};
    struct kernel_vfs_mount *resolved_mount = 0;
    int64_t result = INT64_MIN;
    int path_result = INT32_MIN;

    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 0, 128U);
    if (kernel_files_acquire(&shared_files, files) !=
            KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_acquire(&shared_fs, fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        !kernel_files_is_live(files) ||
        !kernel_files_is_live(&shared_files) ||
        !kernel_fs_context_is_live(fs) ||
        !kernel_fs_context_is_live(&shared_fs) ||
        kernel_fs_context_resolve_kernel_path(&shared_fs,
                                              TEST_AT_FDCWD,
                                              "data",
                                              4U,
                                              fork_resolved_path,
                                              sizeof(fork_resolved_path),
                                              &resolved_mount,
                                              &path_result) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        path_result != 0 || resolved_mount == 0) {
        fail_files(129U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }

    if (kernel_files_close(&shared_files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_read(files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(130U, -KERNEL_EBADF, result);
    }

    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 0, 131U);
    count_open_file_releases = 1;
    counted_open_file_releases = 0U;
    if (kernel_files_release(&shared_files) != KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_release(&shared_fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        counted_open_file_releases != 0U ||
        !kernel_files_is_live(files) ||
        !kernel_fs_context_is_live(fs)) {
        count_open_file_releases = 0;
        fail_files(132U, 0, counted_open_file_releases);
    }
    if (kernel_files_read(files,
                          mm,
                          0,
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 1 || counted_open_file_releases != 1U) {
        count_open_file_releases = 0;
        fail_files(133U, 1, counted_open_file_releases);
    }
    count_open_file_releases = 0;
    expect_user_pattern(mm, TEST_USER_BUFFER, 0U, 1U, 134U);
    if (kernel_files_close(files, 0, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(135U, 0, result);
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
    /* Sweep allocation failures through source/node-MM/VMA preparation.
     * A failed fixed replacement must retain the old resident anonymous page
     * and the caller's OFD pin, irrespective of private allocation order. */
    if (kernel_mm_mmap_anonymous(mm, TEST_MMAP_FIRST, BOAROS_PAGE_SIZE,
            KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED_NOREPLACE,
            &first_address) != KERNEL_MM_STATUS_OK ||
        kernel_mm_resolve_user_fault(mm, first_address, KERNEL_MM_WRITE) !=
            KERNEL_MM_STATUS_OK ||
        !write_user_bytes(mm, first_address, &replacement, 1U) ||
        kernel_files_pin(files, 0, &first_pin, &result) != KERNEL_FILES_STATUS_OK)
        fail_files(330U, 0, -1);
    for (unsigned failure = 1; ; failure++) {
        if (failure > 64) fail_files(331U, 0, failure);
        fail_metadata_allocation = failure;
        enum kernel_mm_status status = kernel_mm_mmap_file_private(mm,
            &first_pin, TEST_MMAP_FIRST, BOAROS_PAGE_SIZE, 0,
            KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED,
            &first_address);
        fail_metadata_allocation = 0;
        if (status == KERNEL_MM_STATUS_OK) break;
        struct kernel_vma old;
        unsigned char value;
        if (status != KERNEL_MM_STATUS_NO_MEMORY || first_pin == 0 ||
            kernel_mm_vma_lookup(mm, first_address, &old) != KERNEL_MM_STATUS_OK ||
            old.kind != KERNEL_VMA_KIND_ANONYMOUS ||
            !read_user_byte(mm, first_address, &value) || value != replacement)
            fail_files(332U, KERNEL_MM_STATUS_NO_MEMORY, status);
    }
    if (first_pin != 0 || kernel_mm_munmap(mm, first_address, BOAROS_PAGE_SIZE) !=
            KERNEL_MM_STATUS_OK) fail_files(333U, 0, -1);

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

    /* Fault-in allocation failure publishes neither PTE nor provenance. */
    fail_metadata_allocation = 1;
    enum kernel_mm_status resident_status = kernel_mm_resolve_user_fault(
        mm, second_address + BOAROS_PAGE_SIZE, KERNEL_MM_READ);
    fail_metadata_allocation = 0;
    if (resident_status != KERNEL_MM_STATUS_NO_MEMORY ||
        kernel_mm_lookup(mm, second_address + BOAROS_PAGE_SIZE, &second_mapping) !=
            KERNEL_MM_STATUS_NOT_MAPPED)
        fail_files(334U, KERNEL_MM_STATUS_NO_MEMORY, resident_status);
    /* A cached write-first fault publishes a COW PTE before copying. If the
     * private allocation fails, that temporary PTE must be removed as well. */
    struct kernel_vma alias_vma;
    uint64_t alias = first_address + 3U * BOAROS_PAGE_SIZE;
    if (kernel_mm_vma_lookup(mm, first_address, &alias_vma) != KERNEL_MM_STATUS_OK ||
        kernel_open_file_acquire(alias_vma.backing) != KERNEL_OPEN_FILE_STATUS_OK)
        fail_files(338U, 0, -1);
    first_pin = alias_vma.backing;
    if (kernel_mm_mmap_file_private(mm, &first_pin, alias, BOAROS_PAGE_SIZE, 0,
            KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &alias) !=
            KERNEL_MM_STATUS_OK) fail_files(338U, 0, -1);
    fail_physical_allocation = 1;
    resident_status = kernel_mm_resolve_user_fault(mm, alias, KERNEL_MM_WRITE);
    fail_physical_allocation = 0;
    if (resident_status != KERNEL_MM_STATUS_NO_MEMORY ||
        kernel_mm_lookup(mm, alias, &first_mapping) != KERNEL_MM_STATUS_NOT_MAPPED)
        fail_files(339U, KERNEL_MM_STATUS_NO_MEMORY, resident_status);
    if (kernel_mm_resolve_user_fault(mm, alias, KERNEL_MM_WRITE) != KERNEL_MM_STATUS_OK)
        fail_files(340U, 0, -1);
    /* Fork metadata failure leaves the parent and its private byte intact. */
    for (unsigned failure = 1; ; failure++) {
        struct kernel_mm trial = {0};
        if (failure > 64) fail_files(335U, 0, failure);
        fail_metadata_allocation = failure;
        enum kernel_mm_status status = kernel_mm_fork(&trial, mm);
        fail_metadata_allocation = 0;
        if (status == KERNEL_MM_STATUS_OK) {
            if (kernel_mm_release(&trial) != KERNEL_MM_STATUS_OK)
                fail_files(336U, 0, -1);
            break;
        }
        if (status != KERNEL_MM_STATUS_NO_MEMORY ||
            !read_user_byte(mm, first_address, &replacement) || replacement != 0xe1U)
            fail_files(337U, KERNEL_MM_STATUS_NO_MEMORY, status);
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
                          sizeof(marker) - 1U)) {
        fail_files(71U, 1, 0);
    }
    counted_open_file_releases = 0U;
    count_open_file_releases = 1;
    if (kernel_files_write(files,
                           mm,
                           1,
                           TEST_USER_PATH,
                           sizeof(marker) - 1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(sizeof(marker) - 1U) ||
        counted_open_file_releases != 1U) {
        count_open_file_releases = 0;
        fail_files(71U, (long)(sizeof(marker) - 1U), (long)result);
    }
    count_open_file_releases = 0;

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

/* A close must not drain cleanup work that belonged to an earlier OFD. */
static void run_close_cleanup_isolation(struct kernel_files *files)
{
    struct kernel_open_file_description *historical = 0;
    struct kernel_files_statistics before;
    struct kernel_files_statistics after;
    int64_t result = INT64_MIN;

    if (kernel_open_file_create_console(files->heap, &historical) !=
            KERNEL_OPEN_FILE_STATUS_OK ||
        kernel_open_file_detach(&historical) !=
            KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED ||
        historical == 0) {
        fail_files(165U, KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED,
                   KERNEL_OPEN_FILE_STATUS_STATE);
    }
    kernel_files_queue_description(files, historical);

    if (kernel_files_open_console(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(166U, 0, result);
    }
    kernel_files_get_statistics(files, &before);
    counted_open_file_releases = 0U;
    count_open_file_releases = 1;
    if (kernel_files_close(files, 0, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        count_open_file_releases = 0;
        fail_files(167U, 0, result);
    }
    count_open_file_releases = 0;
    kernel_files_get_statistics(files, &after);
    if (counted_open_file_releases != 1U ||
        after.close_calls != before.close_calls + 1U ||
        after.close_failures != before.close_failures ||
        after.current_open_fds + 1U != before.current_open_fds ||
        kernel_files_close(files, 0, &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(168U, 1, counted_open_file_releases);
    }
    if (kernel_files_drain_file_cleanup(files) != KERNEL_FILES_STATUS_OK) {
        fail_files(169U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_CLEANUP_REQUIRED);
    }
}

static void run_seek_stat_operations(struct kernel_files *files,
                                     const struct kernel_fs_context *fs,
                                     struct kernel_mm *mm)
{
    struct kernel_linux_stat stat;
    struct kernel_linux_stat file_stat;
    struct kernel_linux_stat path_stat;
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

    /* fstat reports metadata stored in the ext4 inode, including allocated
     * 512-byte blocks and extra timestamp nanoseconds. */
    if (kernel_files_fstat(files, mm, 4, stat_buffer, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, stat_buffer, &stat, sizeof(stat))) {
        fail_files(84U, 0, result);
    }
    if (stat.st_dev != 1U) {
        fail_files(219U, 1, stat.st_dev);
    }
    if (stat.st_mode != UINT32_C(0100640)) {
        fail_files(85U, 0100640, stat.st_mode);
    }
    if (stat.st_ino == 0U) {
        fail_files(220U, 1, 0);
    }
    if (stat.st_nlink != 1U) {
        fail_files(221U, 1, stat.st_nlink);
    }
    if (stat.st_uid != 1234U) {
        fail_files(222U, 1234, stat.st_uid);
    }
    if (stat.st_gid != 2345U) {
        fail_files(223U, 2345, stat.st_gid);
    }
    if (stat.st_size != 9000) {
        fail_files(224U, 9000, stat.st_size);
    }
    if (stat.st_blksize != 1024) {
        fail_files(225U, 1024, stat.st_blksize);
    }
    if (stat.st_blocks != 18U) {
        fail_files(226U, 18, stat.st_blocks);
    }
    if (stat.st_rdev != 0U) {
        fail_files(227U, 0, stat.st_rdev);
    }
    if (stat.st_atime != 1700000001) {
        fail_files(228U, 1700000001, stat.st_atime);
    }
    if (stat.st_atime_nsec != 111) {
        fail_files(229U, 111, stat.st_atime_nsec);
    }
    if (stat.st_mtime != 1700000002) {
        fail_files(230U, 1700000002, stat.st_mtime);
    }
    if (stat.st_mtime_nsec != 222) {
        fail_files(231U, 222, stat.st_mtime_nsec);
    }
    if (stat.st_ctime != 1700000003) {
        fail_files(232U, 1700000003, stat.st_ctime);
    }
    if (stat.st_ctime_nsec != 333) {
        fail_files(233U, 333, stat.st_ctime_nsec);
    }
    file_stat = stat;
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
        !read_user_bytes(mm, stat_buffer, &path_stat, sizeof(path_stat)) ||
        path_stat.st_size != 9000 ||
        (path_stat.st_mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        path_stat.st_dev != file_stat.st_dev ||
        path_stat.st_ino != file_stat.st_ino ||
        path_stat.st_mode != file_stat.st_mode ||
        path_stat.st_nlink != file_stat.st_nlink ||
        path_stat.st_uid != file_stat.st_uid ||
        path_stat.st_gid != file_stat.st_gid ||
        path_stat.st_blocks != file_stat.st_blocks ||
        path_stat.st_atime != file_stat.st_atime ||
        path_stat.st_atime_nsec != file_stat.st_atime_nsec ||
        path_stat.st_mtime != file_stat.st_mtime ||
        path_stat.st_mtime_nsec != file_stat.st_mtime_nsec ||
        path_stat.st_ctime != file_stat.st_ctime ||
        path_stat.st_ctime_nsec != file_stat.st_ctime_nsec) {
        fail_files(87U, 0, result);
    }
    /* Directory descriptors use the same VFS metadata representation. */
    {
        int64_t directory_fd = INT64_MIN;

        if (!write_user_bytes(mm, TEST_USER_PATH, "/", 2U) ||
            kernel_files_openat(files, fs, mm, TEST_AT_FDCWD,
                                TEST_USER_PATH, 0U, 0U, &directory_fd) !=
                KERNEL_FILES_STATUS_OK ||
            directory_fd < 0 ||
            kernel_files_fstat(files, mm, directory_fd, stat_buffer,
                               &result) != KERNEL_FILES_STATUS_OK ||
            result != 0 ||
            !read_user_bytes(mm, stat_buffer, &path_stat,
                             sizeof(path_stat)) ||
            path_stat.st_dev != 1U ||
            (path_stat.st_mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR ||
            path_stat.st_ino == 0U || path_stat.st_nlink < 2U ||
            path_stat.st_blocks <= 0 ||
            kernel_files_close(files, directory_fd, &result) !=
                KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(145U, 0, result);
        }
    }
    /* A one-byte allocated file consumes one 1 KiB ext4 block, reported as
     * two 512-byte st_blocks units rather than a logical-size estimate. */
    {
        int64_t allocated_fd = INT64_MIN;

        if (!write_user_bytes(mm, TEST_USER_PATH, "/allocated", 11U) ||
            kernel_files_openat(files, fs, mm, TEST_AT_FDCWD,
                                TEST_USER_PATH, 0U, 0U, &allocated_fd) !=
                KERNEL_FILES_STATUS_OK ||
            allocated_fd < 0 ||
            kernel_files_fstat(files, mm, allocated_fd, stat_buffer,
                               &result) != KERNEL_FILES_STATUS_OK ||
            result != 0 ||
            !read_user_bytes(mm, stat_buffer, &path_stat,
                             sizeof(path_stat)) ||
            path_stat.st_size != 1 || path_stat.st_blocks != 2 ||
            kernel_files_close(files, allocated_fd, &result) !=
                KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(146U, 2, path_stat.st_blocks);
        }
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
    if (!write_user_bytes(mm, TEST_USER_PATH, "/missing", 9U) ||
        kernel_files_fstatat(files,
                             fs,
                             mm,
                             TEST_AT_FDCWD,
                             TEST_USER_PATH,
                             stat_buffer,
                             0U,
                             &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_ENOENT) {
        fail_files(91U, -KERNEL_ENOENT, result);
    }
    if (kernel_files_close(files, 4, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(90U, 0, result);
    }

    /* Test openat semantics on read-only mount */
    {
        int64_t ro_fd = -1;
        /* existing + O_RDONLY: success */
        if (!write_user_bytes(mm, TEST_USER_PATH, "/data", 6U) ||
            kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                0U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd < 0 ||
            kernel_files_close(files, ro_fd, &result) != KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(92U, 0, (int32_t)ro_fd);
        }
        /* existing + O_RDONLY | O_APPEND (02000): success */
        if (kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                02000U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd < 0 ||
            kernel_files_close(files, ro_fd, &result) != KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(93U, 0, (int32_t)ro_fd);
        }
        /* existing + O_RDONLY | O_CREAT (0100): success */
        if (kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                0100U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd < 0 ||
            kernel_files_close(files, ro_fd, &result) != KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(94U, 0, (int32_t)ro_fd);
        }
        /* existing + O_RDONLY | O_CREAT | O_EXCL (0100 | 0200 = 0300): EEXIST */
        if (kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                0300U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd != -KERNEL_EEXIST) {
            fail_files(95U, -KERNEL_EEXIST, (int32_t)ro_fd);
        }
        /* missing + O_RDONLY | O_CREAT (0100): EROFS */
        if (!write_user_bytes(mm, TEST_USER_PATH, "/missing", 9U) ||
            kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                0100U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd != -KERNEL_EROFS) {
            fail_files(96U, -KERNEL_EROFS, (int32_t)ro_fd);
        }
        /* existing + O_WRONLY (1): EROFS */
        if (!write_user_bytes(mm, TEST_USER_PATH, "/data", 6U) ||
            kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                1U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd != -KERNEL_EROFS) {
            fail_files(97U, -KERNEL_EROFS, (int32_t)ro_fd);
        }
        /* existing + O_RDWR (2): EROFS */
        if (kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                2U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd != -KERNEL_EROFS) {
            fail_files(98U, -KERNEL_EROFS, (int32_t)ro_fd);
        }
        /* existing + O_RDONLY | O_TRUNC (01000): EROFS */
        if (kernel_files_openat(files, fs, mm, TEST_AT_FDCWD, TEST_USER_PATH,
                                01000U, 0U, &ro_fd) != KERNEL_FILES_STATUS_OK ||
            ro_fd != -KERNEL_EROFS) {
            fail_files(99U, -KERNEL_EROFS, (int32_t)ro_fd);
        }
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
                         record_length - 19U < sizeof(directory_name)
                             ? record_length - 19U : sizeof(directory_name))) {
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
    uint64_t resume_cookie = 0U;
    int have_resume_cookie = 0;
    uint64_t first_record_size = 0U;
    uint64_t second_record_size = 0U;
    uint64_t third_record_size = 0U;
    char first_name[256] = {0};
    char resume_name[256] = {0};
    char third_name[256] = {0};
    uint32_t record_index = 0U;
    int saw_data = 0;
    int saw_lost = 0;
    int saw_long = 0;
    int64_t result = INT64_MIN;

    /* The root directory opens read-only and lists its entries. */
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/", 0U, 0, 110U);
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 1, 110U);
    /* getdents64 exposes the real dot entries; the VFS must not silently
     * turn a directory stream into an inode-name-only index. */
    if (kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result <= 0) {
        fail_files(129U, 0, result);
    }
    offset = 0U;
    {
        uint64_t record_size = 0U;
        int type = collect_dirent_name(mm,
                                       dir_buffer + offset,
                                       &record_size);

        if (type < 0 || !dirent_name_is(directory_name, ".")) {
            fail_files(129U, KERNEL_VFS_DT_DIR, type);
        }
    }
    if (kernel_files_lseek(files, 0, 0U, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(130U, 0, result);
    }
    if (kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              4096U,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result <= 0) {
        fail_files(111U, 0, result);
    }

    /* The longest ext4 name requires a 280-byte aligned dirent, not the
     * 275 bytes occupied by its fields before alignment. */
    offset = 0U;
    while (offset < (uint64_t)result) {
        uint64_t record_size = 0U;
        unsigned char cookie_bytes[8];
        int type = collect_dirent_name(mm,
                                       dir_buffer + offset,
                                       &record_size);

        if (type < 0) {
            fail_files(112U, 0, type);
        }
        if (record_index == 0U) {
            first_record_size = record_size;
            memcpy(first_name, directory_name, sizeof(first_name));
            if (!read_user_bytes(mm,
                                 dir_buffer + 8U,
                                 cookie_bytes,
                                 sizeof(cookie_bytes))) {
                fail_files(112U, 0, -KERNEL_EFAULT);
            }
            resume_cookie = 0U;
            for (uint32_t byte = 0U; byte < sizeof(cookie_bytes); byte++) {
                resume_cookie |= (uint64_t)cookie_bytes[byte] << (8U * byte);
            }
            have_resume_cookie = 1;
        } else if (record_index == 1U) {
            second_record_size = record_size;
            memcpy(resume_name, directory_name, sizeof(resume_name));
        } else if (record_index == 2U) {
            third_record_size = record_size;
            memcpy(third_name, directory_name, sizeof(third_name));
        }
        record_index++;
        offset += record_size;
        if (dirent_name_is(directory_name, "data") && type == KERNEL_VFS_DT_REG) {
            saw_data = 1;
        }
        if (dirent_name_is(directory_name, "lost+found") &&
            type == KERNEL_VFS_DT_DIR) {
            saw_lost = 1;
        }
        if (directory_name[0] == 'n') {
            uint32_t length = 0U;

            while (length < 255U && directory_name[length] == 'n') {
                length++;
            }
            if (length != 255U || directory_name[length] != '\0' ||
                record_size != 280U) {
                fail_files(128U, 280, record_size);
            }
            saw_long = 1;
        }
    }
    if (!saw_data || !saw_lost || !saw_long) {
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

    /* d_off is an opaque resume cookie, not the entry ordinal. */
    if (!have_resume_cookie ||
        kernel_files_lseek(files,
                           0,
                           (int64_t)resume_cookie,
                           KERNEL_FILES_SEEK_SET,
                           &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != (int64_t)resume_cookie ||
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

        if (type < 0 || !dirent_name_is(directory_name, resume_name)) {
            fail_files(116U, 0, type);
        }
    }

    /* Separate open calls have independent cursors even for one VFS node. */
    {
        int64_t independent_fd = INT64_MIN;

        if (!write_user_bytes(mm, TEST_USER_PATH, "/", 2U) ||
            kernel_files_openat(files,
                                fs,
                                mm,
                                TEST_AT_FDCWD,
                                TEST_USER_PATH,
                                0U,
                                0U,
                                &independent_fd) != KERNEL_FILES_STATUS_OK ||
            independent_fd < 0 ||
            kernel_files_lseek(files,
                               0,
                               0U,
                               KERNEL_FILES_SEEK_SET,
                               &result) != KERNEL_FILES_STATUS_OK ||
            result != 0 ||
            kernel_files_getdents(files,
                                  mm,
                                  independent_fd,
                                  dir_buffer,
                                  first_record_size,
                                  &result) != KERNEL_FILES_STATUS_OK ||
            result != (int64_t)first_record_size) {
            fail_files(131U, (long)first_record_size, result);
        }
        offset = 0U;
        if (collect_dirent_name(mm, dir_buffer + offset, &offset) < 0 ||
            !dirent_name_is(directory_name, first_name) ||
            kernel_files_getdents(files,
                                  mm,
                                  0,
                                  dir_buffer,
                                  first_record_size,
                                  &result) != KERNEL_FILES_STATUS_OK ||
            result != (int64_t)first_record_size) {
            fail_files(132U, (long)first_record_size, result);
        }
    if (kernel_files_close(files, independent_fd, &result) !=
                KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(133U, 0, result);
        }
    }

    /* A fault after one complete record returns that prefix and leaves the
     * next record at the saved cookie for a retry. */
    if (kernel_files_lseek(files,
                           0,
                           0U,
                           KERNEL_FILES_SEEK_SET,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_getdents(files,
                              mm,
                              0,
                              TEST_USER_BUFFER + 3U * BOAROS_PAGE_SIZE -
                                  first_record_size,
                              first_record_size + second_record_size,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)first_record_size) {
        fail_files(142U, (long)first_record_size, result);
    }
    if (kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              second_record_size,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)second_record_size) {
        fail_files(143U, (long)second_record_size, result);
    }
    offset = 0U;
    if (collect_dirent_name(mm, dir_buffer + offset, &offset) < 0 ||
        !dirent_name_is(directory_name, resume_name)) {
        fail_files(144U, 0, 0);
    }

    /* dup shares the OFD cursor: consuming one record through the duplicate
     * advances the original descriptor to the following record. */
    if (kernel_files_lseek(files,
                           0,
                           0U,
                           KERNEL_FILES_SEEK_SET,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_getdents(files,
                              mm,
                              0,
                              dir_buffer,
                              first_record_size,
                              &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)first_record_size) {
        fail_files(134U, (long)first_record_size, result);
    }
    {
        int64_t duplicate_fd = INT64_MIN;

        if (kernel_files_dup(files, 0, &duplicate_fd) !=
                KERNEL_FILES_STATUS_OK ||
            duplicate_fd < 0 ||
            kernel_files_getdents(files,
                                  mm,
                                  duplicate_fd,
                                  dir_buffer,
                                  second_record_size,
                                  &result) != KERNEL_FILES_STATUS_OK ||
            result != (int64_t)second_record_size) {
            fail_files(135U, (long)second_record_size, result);
        }
        offset = 0U;
        if (collect_dirent_name(mm, dir_buffer + offset, &offset) < 0 ||
            !dirent_name_is(directory_name, resume_name) ||
            kernel_files_getdents(files,
                                  mm,
                                  0,
                                  dir_buffer,
                                  third_record_size,
                                  &result) != KERNEL_FILES_STATUS_OK ||
            result != (int64_t)third_record_size) {
            fail_files(136U, (long)third_record_size, result);
        }
        offset = 0U;
        if (collect_dirent_name(mm, dir_buffer + offset, &offset) < 0 ||
            !dirent_name_is(directory_name, third_name)) {
            fail_files(137U, 0, 0);
        }
        if (kernel_files_close(files, duplicate_fd, &result) !=
                KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(138U, 0, result);
        }
    }

    /* fork also shares the OFD cursor while duplicating the descriptor table. */
    {
        struct kernel_files child_files = {0};

        if (kernel_files_lseek(files,
                               0,
                               0U,
                               KERNEL_FILES_SEEK_SET,
                               &result) != KERNEL_FILES_STATUS_OK ||
            result != 0 ||
            kernel_files_fork(&child_files, files) !=
                KERNEL_FILES_STATUS_OK ||
            kernel_files_getdents(&child_files,
                                  mm,
                                  0,
                                  dir_buffer,
                                  first_record_size,
                                  &result) != KERNEL_FILES_STATUS_OK ||
            result != (int64_t)first_record_size) {
            fail_files(139U, (long)first_record_size, result);
        }
        offset = 0U;
        if (collect_dirent_name(mm, dir_buffer + offset, &offset) < 0 ||
            !dirent_name_is(directory_name, first_name) ||
            kernel_files_getdents(files,
                                  mm,
                                  0,
                                  dir_buffer,
                                  second_record_size,
                                  &result) != KERNEL_FILES_STATUS_OK ||
            result != (int64_t)second_record_size) {
            fail_files(140U, (long)second_record_size, result);
        }
        offset = 0U;
        if (collect_dirent_name(mm, dir_buffer + offset, &offset) < 0 ||
            !dirent_name_is(directory_name, resume_name) ||
            kernel_files_release(&child_files) != KERNEL_FILES_STATUS_OK) {
            fail_files(141U, 0, result);
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
    struct kernel_files_statistics statistics;
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
        result != -KERNEL_EBADF ||
        kernel_files_dup3(files, 3, 1024, 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
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
    if (kernel_files_fcntl(files,
                           3,
                           KERNEL_FILES_F_DUPFD,
                           100U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 100 ||
        kernel_files_close(files, 100, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(110U, 100, result);
    }
    kernel_files_get_statistics(files, &statistics);
    if (statistics.current_open_fds != 9U ||
        statistics.close_on_exec_fds != 3U) {
        fail_files(217U, 9, statistics.current_open_fds);
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

static void run_pipe_operations(struct kernel_files *files,
                                const struct kernel_fs_context *fs,
                                struct kernel_mm *mm)
{
    struct kernel_files_statistics statistics;
    struct kernel_uaccess_iovec iov[2];
    int32_t pair[2];
    int64_t result = INT64_MIN;
    uint32_t index;

    /* When only fd 31 is free, growing the table must allocate a distinct
     * second descriptor from the new slots. */
    for (index = 0U; index < 31U; index++) {
        expect_open(files,
                    fs,
                    mm,
                    TEST_AT_FDCWD,
                    "/data",
                    0U,
                    (int64_t)index,
                    120U);
    }
    if (kernel_files_pipe2(files,
                           mm,
                           TEST_USER_PATH,
                           KERNEL_FILES_O_NONBLOCK,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, TEST_USER_PATH, pair, sizeof(pair)) ||
        pair[0] != 31 || pair[1] != 32) {
        fail_files(121U, 32, pair[1]);
    }
    kernel_files_get_statistics(files, &statistics);
    if (statistics.current_open_fds != 33U ||
        statistics.close_on_exec_fds != 0U) {
        fail_files(218U, 33, statistics.current_open_fds);
    }
    for (index = 0U; index <= 32U; index++) {
        if (kernel_files_close(files, index, &result) !=
                KERNEL_FILES_STATUS_OK ||
            result != 0) {
            fail_files(122U, 0, result);
        }
    }

    if (kernel_files_pipe2(files,
                           mm,
                           TEST_USER_PATH,
                           KERNEL_FILES_O_NONBLOCK,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, TEST_USER_PATH, pair, sizeof(pair)) ||
        kernel_files_write(files,
                           mm,
                           pair[0],
                           TEST_USER_BUFFER,
                           1U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF ||
        kernel_files_read(files,
                          mm,
                          pair[1],
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(123U, -KERNEL_EBADF, result);
    }

    /* writev atomicity is decided from the aggregate request, not from
     * each iovec independently. */
    for (index = 0U; index < 15U; index++) {
        if (kernel_files_write(files,
                               mm,
                               pair[1],
                               TEST_USER_BUFFER,
                               BOAROS_PAGE_SIZE,
                               &result) != KERNEL_FILES_STATUS_OK ||
            result != BOAROS_PAGE_SIZE) {
            fail_files(124U, BOAROS_PAGE_SIZE, result);
        }
    }
    if (kernel_files_write(files,
                           mm,
                           pair[1],
                           TEST_USER_BUFFER,
                           BOAROS_PAGE_SIZE - 3U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != BOAROS_PAGE_SIZE - 3U) {
        fail_files(125U, BOAROS_PAGE_SIZE - 3U, result);
    }
    iov[0].base = TEST_USER_BUFFER;
    iov[0].length = 2U;
    iov[1].base = TEST_USER_BUFFER + 2U;
    iov[1].length = 2U;
    if (!write_user_bytes(mm, TEST_USER_PATH, iov, sizeof(iov))) {
        fail_files(126U, 1, 0);
    }
    counted_open_file_releases = 0U;
    count_open_file_releases = 1;
    if (kernel_files_writev(files,
                            mm,
                            pair[1],
                            TEST_USER_PATH,
                            2U,
                            &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EAGAIN ||
        counted_open_file_releases != 1U) {
        count_open_file_releases = 0;
        fail_files(126U, 1, counted_open_file_releases);
    }
    count_open_file_releases = 0;
    if (kernel_files_write(files,
                           mm,
                           pair[1],
                           TEST_USER_BUFFER,
                           3U,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 3) {
        fail_files(126U, 3, result);
    }

    /* Replacing the sole writer logically closes it before the new OFD is
     * installed, so an empty read observes EOF immediately. */
    if (kernel_files_close(files, pair[1], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, pair[0], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_pipe2(files,
                           mm,
                           TEST_USER_PATH,
                           KERNEL_FILES_O_NONBLOCK,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, TEST_USER_PATH, pair, sizeof(pair)) ||
        kernel_files_dup3(files, pair[0], pair[1], 0U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != pair[1] ||
        kernel_files_read(files,
                          mm,
                          pair[0],
                          TEST_USER_BUFFER,
                          1U,
                          &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, pair[0], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, pair[1], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(127U, 0, result);
    }
}

static void run_epoll_operations(struct kernel_files *files,
                                 const struct kernel_fs_context *fs,
                                 struct kernel_mm *mm)
{
    struct kernel_files_statistics before;
    struct kernel_files_statistics after;
    struct kernel_open_file_description *first_description;
    int32_t pipe_fds[2];
    int64_t result = INT64_MIN;
    int64_t epfd = -1;
    int64_t reused_epfd = -1;
    int64_t second_epfd = -1;
    uint32_t index;

    /* epoll_create1 installs an ordinary fd: statistics, CLOEXEC and the
     * lowest-free hint must follow the same contract as open and dup. */
    kernel_files_get_statistics(files, &before);
    if (kernel_files_epoll_create1(files, TEST_EPOLL_CLOEXEC, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(200U, 0, result);
    }
    kernel_files_get_statistics(files, &after);
    if (after.current_open_fds != before.current_open_fds + 1U ||
        after.peak_open_fds != before.peak_open_fds + 1U ||
        after.close_on_exec_fds != before.close_on_exec_fds + 1U ||
        files->record->next_fd != 1U) {
        fail_files(201U, before.current_open_fds + 1U,
                   after.current_open_fds);
    }
    if (kernel_files_close(files, 0, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(202U, 0, result);
    }
    kernel_files_get_statistics(files, &after);
    if (after.current_open_fds != before.current_open_fds ||
        after.close_on_exec_fds != before.close_on_exec_fds ||
        files->record->next_fd != 0U) {
        fail_files(203U, before.current_open_fds, after.current_open_fds);
    }

    if (kernel_files_epoll_create1(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(204U, 0, result);
    }
    epfd = result;
    if (kernel_files_epoll_create1(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 1) {
        fail_files(205U, 1, result);
    }
    second_epfd = result;
    if (kernel_files_close(files, epfd, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_epoll_create1(files, TEST_EPOLL_CLOEXEC, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != epfd || files->record->next_fd != 2U) {
        fail_files(206U, epfd, result);
    }
    reused_epfd = result;
    if (kernel_files_close(files, reused_epfd, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, second_epfd, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(207U, 0, result);
    }

    if (kernel_files_pipe2(files,
                           mm,
                           TEST_USER_PATH,
                           0,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, TEST_USER_PATH, pipe_fds, sizeof(pipe_fds))) {
        fail_files(140U, 0, result);
    }

    if (kernel_files_epoll_create1(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result < 0) {
        fail_files(141U, 0, result);
    }
    epfd = result;

    if (kernel_files_epoll_ctl(files,
                               epfd,
                               1 /* KERNEL_EPOLL_CTL_ADD */,
                               pipe_fds[0],
                               KERNEL_POLLIN,
                               42ULL,
                               &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(142U, 0, result);
    }

    if (kernel_files_close(files, epfd, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(143U, 0, result);
    }

    /* Descriptor is logically detached: second close must return -EBADF */
    if (kernel_files_close(files, epfd, &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(144U, -KERNEL_EBADF, result);
    }


    if (kernel_files_close(files, pipe_fds[0], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, pipe_fds[1], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(146U, 0, result);
    }

    if (kernel_files_epoll_create1(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result < 0) {
        fail_files(147U, 0, result);
    }
    epfd = result;

    if (kernel_files_close(files, epfd, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(148U, 0, result);
    }

    if (kernel_files_close(files, epfd, &result) != KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EBADF) {
        fail_files(149U, -KERNEL_EBADF, result);
    }


    /* A target file may close before its epoll instance. */
    if (kernel_files_pipe2(files,
                           mm,
                           TEST_USER_PATH,
                           0,
                           &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(mm, TEST_USER_PATH, pipe_fds, sizeof(pipe_fds))) {
        fail_files(151U, 0, result);
    }

    if (kernel_files_epoll_create1(files, 0, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result < 0) {
        fail_files(152U, 0, result);
    }
    epfd = result;

    if (kernel_files_epoll_ctl(files,
                               epfd,
                               1 /* KERNEL_EPOLL_CTL_ADD */,
                               pipe_fds[0],
                               KERNEL_POLLIN,
                               99ULL,
                               &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(153U, 0, result);
    }

    if (kernel_files_close(files, pipe_fds[0], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_close(files, pipe_fds[1], &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(154U, 0, result);
    }

    if (kernel_files_close(files, epfd, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(155U, 0, result);
    }

    /* A full table reports EMFILE transactionally.  In particular, fd 0 and
     * all counters remain owned by the pre-existing description. */
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/data", 0U, 0, 208U);
    first_description = kernel_files_lookup_description(files, 0);
    for (index = 1U; index < KERNEL_FILES_MAX_CAPACITY; index++) {
        if (kernel_files_dup(files, 0, &result) != KERNEL_FILES_STATUS_OK ||
            result != (int64_t)index) {
            fail_files(209U, index, result);
        }
    }
    kernel_files_get_statistics(files, &before);
    if (kernel_files_epoll_create1(files, TEST_EPOLL_CLOEXEC, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EMFILE) {
        fail_files(210U, -KERNEL_EMFILE, result);
    }
    kernel_files_get_statistics(files, &after);
    if (kernel_files_lookup_description(files, 0) != first_description ||
        after.current_open_fds != before.current_open_fds ||
        after.peak_open_fds != before.peak_open_fds ||
        after.close_on_exec_fds != before.close_on_exec_fds ||
        files->record->next_fd != KERNEL_FILES_MAX_CAPACITY) {
        fail_files(211U, before.current_open_fds, after.current_open_fds);
    }

}

#ifdef FILES_PARTIAL_WRITE_TEST
static void run_files_test(const void *dtb) __attribute__((unused));
#endif

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
        kernel_vfs_mount_root(&mount,
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

    run_shared_handle_operations(&files, &fs, &mm);
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
    run_close_cleanup_isolation(&files);
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
        fail_files(119U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    run_pipe_operations(&files, &fs, &mm);

    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK) {
        fail_files(58U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    files = (struct kernel_files){0};
    if (kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(139U, KERNEL_FILES_STATUS_OK,
                   KERNEL_FILES_STATUS_STATE);
    }
    run_epoll_operations(&files, &fs, &mm);

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

#ifdef FILES_PARTIAL_WRITE_TEST
/* The writable mapping ends at boundary; the next page has no mapping.
 * A readable final iovec detects accidentally continuing after the fault. */
static void check_usercopy_write(struct kernel_files *files,
                                 struct kernel_mm *mm, size_t prefix,
                                 int vector, int append, size_t prior,
                                 int backend_mode)
{
    const uint64_t boundary = TEST_USER_BUFFER + 3U * BOAROS_PAGE_SIZE;
    struct kernel_uaccess_iovec vectors[3];
    struct kernel_open_file_description *description =
        kernel_files_lookup_description(files, 0);
    struct kernel_vfs_stat stat;
    struct kernel_files_statistics before, after;
    unsigned char payload[80], expected[90], observed[90];
    size_t accepted = backend_mode == 3 ? 0U :
        backend_mode != 0 && prefix > 5U ? 5U : prefix;
    size_t progress = prior + accepted;
    size_t start = append ? 3U : 1U;
    size_t end = progress != 0U ? start + progress : 1U;
    size_t size = progress != 0U && start + progress > 3U ?
        start + progress : 3U;
    int64_t result;
    int64_t wanted = progress != 0U ? (int64_t)progress :
        backend_mode == 3 && prefix != 0U ? -KERNEL_EIO : -KERNEL_EFAULT;

    memset(payload, 'P', sizeof(payload));
    memset(expected, 'P', sizeof(expected));
    memcpy(expected, "old", 3U);
    if (progress != 0U) {
        memset(expected + start, 'P', progress);
    }
    if (kernel_files_fcntl(files, 0, KERNEL_FILES_F_SETFL, 0, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 ||
        kernel_files_ftruncate(files, 0, 0, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 ||
        kernel_files_lseek(files, 0, 0, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 ||
        !write_user_bytes(mm, TEST_USER_BUFFER, "old", 3U) ||
        kernel_files_write(files, mm, 0, TEST_USER_BUFFER, 3U, &result) !=
            KERNEL_FILES_STATUS_OK || result != 3 ||
        kernel_files_lseek(files, 0, 1, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK || result != 1 ||
        kernel_files_fcntl(files, 0, KERNEL_FILES_F_SETFL,
                           append ? KERNEL_FILES_O_APPEND : 0, &result) !=
            KERNEL_FILES_STATUS_OK || result != 0 ||
        !write_user_bytes(mm, TEST_USER_BUFFER, payload, sizeof(payload)) ||
        !write_user_bytes(mm, boundary - prefix, payload, prefix)) {
        fail_files(320U, 0, result);
    }
    vectors[0] = (struct kernel_uaccess_iovec){TEST_USER_BUFFER, prior};
    vectors[1] = (struct kernel_uaccess_iovec){boundary - prefix, prefix + 1U};
    vectors[2] = (struct kernel_uaccess_iovec){TEST_USER_BUFFER, 3U};
    if (!write_user_bytes(mm, TEST_USER_PATH, vectors, sizeof(vectors))) {
        fail_files(321U, 1, 0);
    }
    kernel_files_get_statistics(files, &before);
    inject_usercopy_write_mode = backend_mode;
    usercopy_write_skip = prior != 0U ? 1U : 0U;
    if ((vector ? kernel_files_writev(files, mm, 0, TEST_USER_PATH, 3U, &result)
                : kernel_files_write(files, mm, 0, boundary - prefix,
                                      prefix + 1U, &result)) !=
            KERNEL_FILES_STATUS_OK || result != wanted) {
        fail_files(322U + prefix, wanted, result);
    }
    inject_usercopy_write_mode = 0;
    kernel_files_get_statistics(files, &after);
    if (kernel_open_file_offset(description) != end ||
        kernel_vfs_fstat(&description->file, &stat) != 0 || stat.size != size ||
        after.bytes_written - before.bytes_written != progress ||
        kernel_files_pread(files, mm, 0, TEST_USER_BUFFER, sizeof(observed),
                           0, &result) != KERNEL_FILES_STATUS_OK ||
        result != (int64_t)size ||
        !read_user_bytes(mm, TEST_USER_BUFFER, observed, size) ||
        memcmp(observed, expected, size) != 0) {
        fail_files(390U, end, kernel_open_file_offset(description));
    }
}

static void run_partial_write_test(const void *dtb)
{
    static const char path[] = "/partial";
    static const char initial[] = "old";
    static const char payload[] = "PREFIX!!";
    static const char tail[] = "TAIL";
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
    struct kernel_open_file_description *description;
    struct kernel_linux_stat linux_stat;
    struct kernel_vfs_stat vfs_stat;
    struct kernel_files_statistics statistics;
    struct kernel_uaccess_iovec iov[2];
    unsigned char observed[sizeof(payload) - 1U];
    uint64_t cached_page = 0U;
    uint64_t baseline;
    uint64_t stat_buffer = TEST_USER_BUFFER + 2U * BOAROS_PAGE_SIZE;
    size_t valid_bytes = 0U;
    int64_t result = INT64_MIN;
    uint32_t index;
    int found = 0;

    if (dtb_read_boot_info(dtb, &info) != DTB_STATUS_OK ||
        info.timebase_frequency == 0U) {
        fail_files(300U, DTB_STATUS_OK, -1);
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
        fail_files(300U, 0, -1);
    }
    kernel_table.state = RISCV_SV39_STATE_ACTIVE;
    baseline = physical_page_available(&allocator);
    if (kernel_page_cache_init(&page_cache, &heap, &allocator) !=
        KERNEL_PAGE_CACHE_STATUS_OK) {
        fail_files(300U, 0, -1);
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
            fail_files(300U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, status);
        }
    }
    if (!found ||
        kernel_vfs_mount_root(&mount, &device.block, &heap, &page_cache) != 0 ||
        kernel_vfs_mount_is_readonly(&mount) ||
        !create_user_mm(&allocator, &kernel_table, &mm) ||
        kernel_mm_vma_enable(&mm, &heap) != KERNEL_MM_STATUS_OK ||
        kernel_mm_brk_initialize(&mm, UINT64_C(0x1000000),
                                 TEST_MMAP_LIMIT) != KERNEL_MM_STATUS_OK ||
        kernel_fs_context_create(&fs, &mount, &heap) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK ||
        riscv_kernel_mm_satp(&mm, &test_satp) != KERNEL_MM_STATUS_OK) {
        fail_files(300U, 0, -1);
    }
    use_test_satp = 1;

    if (!write_user_bytes(&mm, TEST_USER_PATH, path, sizeof(path)) ||
        kernel_files_openat(&files, &fs, &mm, TEST_AT_FDCWD,
                            TEST_USER_PATH, TEST_O_CREAT | 2U, 0600U,
                            &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !write_user_bytes(&mm, TEST_USER_BUFFER, initial,
                          sizeof(initial) - 1U) ||
        kernel_files_write(&files, &mm, 0, TEST_USER_BUFFER,
                           sizeof(initial) - 1U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(sizeof(initial) - 1U) ||
        kernel_files_lseek(&files, 0, 0, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_read(&files, &mm, 0, TEST_USER_BUFFER,
                          sizeof(initial) - 1U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != (int64_t)(sizeof(initial) - 1U)) {
        fail_files(301U, sizeof(initial) - 1U, result);
    }
    description = kernel_files_lookup_description(&files, 0);
    if (description == 0 ||
        kernel_open_file_lookup_page(description, 0U, &cached_page,
                                     &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        valid_bytes != sizeof(initial) - 1U ||
        physical_page_release(&allocator, cached_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_files_lseek(&files, 0, 8, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 8 ||
        !write_user_bytes(&mm, TEST_USER_BUFFER, payload,
                          sizeof(payload) - 1U) ||
        !write_user_bytes(&mm,
                          TEST_USER_BUFFER + sizeof(payload) - 1U,
                          tail,
                          sizeof(tail) - 1U)) {
        fail_files(302U, 8, result);
    }
    iov[0].base = TEST_USER_BUFFER;
    iov[0].length = sizeof(payload) - 1U;
    iov[1].base = TEST_USER_BUFFER + sizeof(payload) - 1U;
    iov[1].length = sizeof(tail) - 1U;
    if (!write_user_bytes(&mm, TEST_USER_PATH, iov, sizeof(iov))) {
        fail_files(302U, 1, 0);
    }

    inject_partial_write_error = 1;
    if (kernel_files_writev(&files, &mm, 0, TEST_USER_PATH, 2U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 5) {
        fail_files(303U, 5, result);
    }
    if (inject_partial_write_error != 0 ||
        kernel_open_file_offset(description) != 13U ||
        description->file.size != 13U ||
        kernel_vfs_file_size(&description->file) != 13U ||
        kernel_open_file_size(description) != 13U ||
        kernel_vfs_node_size(kernel_vfs_file_node(&description->file)) !=
            13U ||
        kernel_vfs_fstat(&description->file, &vfs_stat) != 0 ||
        vfs_stat.size != 13U ||
        kernel_files_fstat(&files, &mm, 0, stat_buffer, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(&mm, stat_buffer, &linux_stat,
                         sizeof(linux_stat)) ||
        linux_stat.st_size != 13) {
        fail_files(304U, 13, result);
    }
    if (kernel_open_file_lookup_page(description, 0U, &cached_page,
                                     &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_NOT_FOUND ||
        kernel_files_pread(&files, &mm, 0, TEST_USER_BUFFER,
                           5U, 8, &result) != KERNEL_FILES_STATUS_OK ||
        result != 5 ||
        !read_user_bytes(&mm, TEST_USER_BUFFER, observed, 5U) ||
        memcmp(observed, payload, 5U) != 0 ||
        kernel_files_pread(&files, &mm, 0, TEST_USER_BUFFER,
                           1U, 13, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(305U, 5, result);
    }
    kernel_files_get_statistics(&files, &statistics);
    if (statistics.write_calls != 2U ||
        statistics.write_failures != 0U ||
        statistics.bytes_written != 8U) {
        fail_files(306U, 8, statistics.bytes_written);
    }

    if (kernel_files_fcntl(&files, 0, KERNEL_FILES_F_SETFL,
                           KERNEL_FILES_O_APPEND, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_lseek(&files, 0, 1, KERNEL_FILES_SEEK_SET, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 1 ||
        !write_user_bytes(&mm, TEST_USER_BUFFER, payload,
                          sizeof(payload) - 1U) ||
        !write_user_bytes(&mm,
                          TEST_USER_BUFFER + sizeof(payload) - 1U,
                          tail,
                          sizeof(tail) - 1U)) {
        fail_files(307U, 1, result);
    }
    iov[0].base = TEST_USER_BUFFER;
    iov[0].length = sizeof(payload) - 1U;
    iov[1].base = TEST_USER_BUFFER + sizeof(payload) - 1U;
    iov[1].length = sizeof(tail) - 1U;
    if (!write_user_bytes(&mm, TEST_USER_PATH, iov, sizeof(iov))) {
        fail_files(307U, 1, 0);
    }
    inject_partial_write_error = 1;
    if (kernel_files_writev(&files, &mm, 0, TEST_USER_PATH, 2U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 5) {
        fail_files(308U, 5, result);
    }
    if (kernel_open_file_offset(description) != 18U ||
        description->file.size != 18U ||
        kernel_vfs_file_size(&description->file) != 18U ||
        kernel_open_file_size(description) != 18U ||
        kernel_vfs_node_size(kernel_vfs_file_node(&description->file)) !=
            18U ||
        kernel_vfs_fstat(&description->file, &vfs_stat) != 0 ||
        vfs_stat.size != 18U ||
        kernel_files_fstat(&files, &mm, 0, stat_buffer, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(&mm, stat_buffer, &linux_stat,
                         sizeof(linux_stat)) ||
        linux_stat.st_size != 18 ||
        kernel_open_file_lookup_page(description, 0U, &cached_page,
                                     &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_NOT_FOUND ||
        kernel_files_pread(&files, &mm, 0, TEST_USER_BUFFER,
                           5U, 13, &result) != KERNEL_FILES_STATUS_OK ||
        result != 5 ||
        !read_user_bytes(&mm, TEST_USER_BUFFER, observed, 5U) ||
        memcmp(observed, payload, 5U) != 0 ||
        kernel_files_pread(&files, &mm, 0, TEST_USER_BUFFER,
                           1U, 18, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(309U, 18, result);
    }
    kernel_files_get_statistics(&files, &statistics);
    if (statistics.write_calls != 3U ||
        statistics.write_failures != 0U ||
        statistics.bytes_written != 13U) {
        fail_files(310U, 13, statistics.bytes_written);
    }

    if (kernel_open_file_lookup_page(description, 0U, &cached_page,
                                     &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        physical_page_release(&allocator, cached_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_files(311U, KERNEL_PAGE_CACHE_STATUS_OK,
                   KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    inject_truncate_error = 1;
    if (kernel_files_ftruncate(&files, 0, 2U, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != -KERNEL_EIO || inject_truncate_error != 0) {
        fail_files(312U, -KERNEL_EIO, result);
    }
    if (kernel_open_file_offset(description) != 18U ||
        description->file.size != 2U ||
        kernel_vfs_file_size(&description->file) != 2U ||
        kernel_open_file_size(description) != 2U ||
        kernel_vfs_node_size(kernel_vfs_file_node(&description->file)) != 2U ||
        kernel_vfs_fstat(&description->file, &vfs_stat) != 0 ||
        vfs_stat.size != 2U ||
        kernel_files_fstat(&files, &mm, 0, stat_buffer, &result) !=
            KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        !read_user_bytes(&mm, stat_buffer, &linux_stat,
                         sizeof(linux_stat)) ||
        linux_stat.st_size != 2 ||
        kernel_open_file_lookup_page(description, 0U, &cached_page,
                                     &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_NOT_FOUND ||
        kernel_files_pread(&files, &mm, 0, TEST_USER_BUFFER,
                           1U, 2, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0) {
        fail_files(313U, 2, result);
    }

    static const size_t prefixes[] = {0U, 1U, 32U, 63U, 64U, 65U};
    for (size_t n = 0U; n < sizeof(prefixes) / sizeof(prefixes[0]); n++) {
        for (int append = 0; append < 2; append++) {
            check_usercopy_write(&files, &mm, prefixes[n], 0, append, 0U, 0);
            check_usercopy_write(&files, &mm, prefixes[n], 1, append, 0U, 0);
            check_usercopy_write(&files, &mm, prefixes[n], 1, append, 7U, 0);
        }
    }
    for (int mode = 1; mode <= 3; mode++) {
        for (int append = 0; append < 2; append++) {
            check_usercopy_write(&files, &mm, 32U, 0, append, 0U, mode);
            check_usercopy_write(&files, &mm, 32U, 1, append, 7U, mode);
        }
    }

    use_test_satp = 0;
    if (kernel_files_close(&files, 0, &result) != KERNEL_FILES_STATUS_OK ||
        result != 0 ||
        kernel_files_release(&files) != KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_release(&fs) != KERNEL_FS_CONTEXT_STATUS_OK ||
        kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        kernel_vfs_unmount(&mount) != 0 ||
        kernel_page_cache_destroy(&page_cache) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        riscv_virtio_mmio_block_destroy(&device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        fail_files(314U, baseline, physical_page_available(&allocator));
    }
}
#endif

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;

#ifdef FILES_PARTIAL_WRITE_TEST
    run_partial_write_test(dtb);
    virt_uart_puts("BoarOS: process files partial write tests passed\n");
#else
    run_files_test(dtb);
    virt_uart_puts("BoarOS: process files tests passed\n");
#endif
    sbi_shutdown();
}
