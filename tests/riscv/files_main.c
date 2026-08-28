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
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define TEST_POOL_PAGES 512U
#define TEST_USER_PATH UINT64_C(0x10000)
#define TEST_USER_BUFFER UINT64_C(0x20000)
#define TEST_AT_FDCWD (-100)
#define TEST_O_WRONLY UINT64_C(1)
#define TEST_O_CREAT UINT64_C(0100)
#define TEST_O_LARGEFILE UINT64_C(00100000)
#define TEST_O_DIRECTORY UINT64_C(00200000)
#define TEST_O_CLOEXEC UINT64_C(02000000)

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_POOL_PAGES]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static int fail_next_heap_release;

enum kernel_heap_status __real_kernel_heap_release(
    struct kernel_heap *heap,
    void *pointer);

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
                        const struct kernel_mm *mm,
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
                                const struct kernel_mm *mm)
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
                -KERNEL_ENOTSUP, 24U);
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
    expect_open(files, fs, mm, TEST_AT_FDCWD, "/", 0U,
                -KERNEL_EISDIR, 25U);
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

    fail_next_heap_release = 1;
    if (kernel_files_close(files, 2, &result) !=
            KERNEL_FILES_STATUS_OK || result != -KERNEL_EIO ||
        kernel_files_close(files, 2, &result) !=
            KERNEL_FILES_STATUS_OK || result != -KERNEL_EBADF) {
        fail_files(33U, -KERNEL_EBADF, result);
    }
}

static void run_files_test(const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
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
                                       &heap) != 0 ||
        !create_user_mm(&allocator, &kernel_table, &mm) ||
        kernel_fs_context_create(&fs, &mount, &heap) !=
            KERNEL_FS_CONTEXT_STATUS_OK ||
        kernel_files_create(&files, &heap) != KERNEL_FILES_STATUS_OK) {
        fail_files(4U, 0, -1);
    }

    run_file_operations(&files, &fs, &mm);

    if (kernel_files_release(&files) != KERNEL_FILES_STATUS_OK ||
        kernel_fs_context_release(&fs) != KERNEL_FS_CONTEXT_STATUS_OK ||
        kernel_mm_release(&mm) != KERNEL_MM_STATUS_OK ||
        kernel_vfs_unmount(&mount) != 0 ||
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
