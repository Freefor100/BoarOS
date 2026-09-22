#include <string.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#ifndef VFS_EXPECT_RECOVERY
#include <kernel/open_file.h>
#include <ext4.h>
#include <ext4_errno.h>
#include "../../fs/lwext4_port.h"
#endif

#define TEST_POOL_PAGES 512U

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_POOL_PAGES]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

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

static void fail_vfs(unsigned long case_id,
                     long expected,
                     long actual)
    __attribute__((noreturn));

static void fail_vfs(unsigned long case_id,
                     long expected,
                     long actual)
{
    virt_uart_puts("BoarOS: VFS test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex((unsigned long)expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex((unsigned long)actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

#ifndef VFS_EXPECT_RECOVERY
static uint32_t fail_orphan_free_calls;
static uint32_t orphan_free_calls;
static uint32_t fail_fclose_calls;
static uint32_t failed_fclose_calls;
static struct kernel_page_cache *pressure_cache;
static uint64_t pressure_reclaimed;
static int fail_journal_start_allocation;
static int fail_journal_calloc;
int __real_ext4_journal_start(const char *mount);
void *__real_ext4_user_calloc(size_t count, size_t size);
int __wrap_ext4_journal_start(const char *mount)
{
    fail_journal_calloc = fail_journal_start_allocation;
    fail_journal_start_allocation = 0;
    return __real_ext4_journal_start(mount);
}
void *__wrap_ext4_user_calloc(size_t count, size_t size)
{
    if (fail_journal_calloc) {
        fail_journal_calloc = 0;
        return 0;
    }
    return __real_ext4_user_calloc(count, size);
}
enum kernel_heap_status __real_kernel_heap_allocate(
    struct kernel_heap *, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate(
    struct kernel_heap *heap, size_t size, void **out)
{
    if (pressure_cache != 0 && boaros_lwext4_allocation_active()) {
        struct kernel_page_cache *cache = pressure_cache;
        pressure_cache = 0;
        pressure_reclaimed = kernel_page_cache_reclaim(cache, UINT64_MAX);
    }
    return __real_kernel_heap_allocate(heap, size, out);
}
static uint32_t retried_fclose_calls;
static ext4_file *failed_fclose_files[4];

int __real_ext4_orphan_free(const char *path, uint32_t inode);
int __real_ext4_fclose(ext4_file *file);

int __wrap_ext4_orphan_free(const char *path, uint32_t inode)
{
    orphan_free_calls++;
    if (fail_orphan_free_calls != 0U) {
        fail_orphan_free_calls--;
        return EIO;
    }
    return __real_ext4_orphan_free(path, inode);
}

int __wrap_ext4_fclose(ext4_file *file)
{
    if (fail_fclose_calls != 0U) {
        fail_fclose_calls--;
        if (failed_fclose_calls <
            sizeof(failed_fclose_files) / sizeof(failed_fclose_files[0]))
            failed_fclose_files[failed_fclose_calls] = file;
        failed_fclose_calls++;
        return EIO;
    }
    for (size_t index = 0U;
         index < sizeof(failed_fclose_files) / sizeof(failed_fclose_files[0]);
         index++) {
        if (failed_fclose_files[index] == file) {
            failed_fclose_files[index] = 0;
            retried_fclose_calls++;
            break;
        }
    }
    return __real_ext4_fclose(file);
}

static int bytes_equal(const unsigned char *bytes,
                       const char *expected,
                       size_t length)
{
    size_t index;

    for (index = 0U; index < length; index++) {
        if (bytes[index] != (unsigned char)expected[index]) {
            return 0;
        }
    }
    return 1;
}

static void run_orphan_cleanup_regression(struct kernel_vfs_mount *mount,
                                          struct kernel_heap *heap)
{
    struct kernel_open_file_description *orphan_open = 0;
    struct kernel_open_file_description *unrelated = 0;
    struct kernel_vfs_file unopened = {0};
    struct kernel_vfs_file missing = {0};
    struct kernel_vfs_file orphan_stat_file = {0};
    struct kernel_vfs_path *root_path = 0;
    struct kernel_vfs_path *named_path = 0;
    struct kernel_vfs_path *missing_path = 0;
    struct kernel_vfs_path *replacement_path = 0;
    struct kernel_vfs_stat orphan_stat;
    int linux_result = -1;

    if (kernel_open_file_create_mode(heap,
                                     mount,
                                     "/orphan-open",
                                     0600U,
                                     &orphan_open,
                                     &linux_result) !=
            KERNEL_OPEN_FILE_STATUS_OK ||
        linux_result != 0 || orphan_open == 0 ||
        kernel_open_file_create(heap,
                                mount,
                                "/init",
                                &unrelated,
                                &linux_result) !=
            KERNEL_OPEN_FILE_STATUS_OK ||
        linux_result != 0 || unrelated == 0 ||
        kernel_vfs_unlink(mount, "/orphan-open") != 0) {
        fail_vfs(24U, 0, linux_result);
    }

    fail_orphan_free_calls = 1U;
    if (kernel_open_file_release(&orphan_open) !=
            KERNEL_OPEN_FILE_STATUS_OK ||
        orphan_open != 0 || orphan_free_calls != 1U ||
        kernel_open_file_release(&unrelated) !=
            KERNEL_OPEN_FILE_STATUS_OK ||
        unrelated != 0 || orphan_free_calls != 1U) {
        fail_vfs(25U, KERNEL_OPEN_FILE_STATUS_OK,
                 KERNEL_OPEN_FILE_STATUS_CLEANUP_REQUIRED);
    }

    if (kernel_vfs_create(mount, "/orphan-unopened", 0600U, &unopened) !=
            0 ||
        kernel_vfs_close(&unopened) != 0) {
        fail_vfs(26U, 0, -1);
    }
    fail_orphan_free_calls = 1U;
    if (kernel_vfs_unlink(mount, "/orphan-unopened") != 0 ||
        kernel_vfs_open(mount, "/orphan-unopened", &missing) !=
            -KERNEL_ENOENT ||
        missing.private_data != 0 || orphan_free_calls != 2U) {
        fail_vfs(27U, -KERNEL_ENOENT, orphan_free_calls);
    }
    if (kernel_vfs_create(mount, "/orphan-stat", 0640U,
                          &orphan_stat_file) != 0 ||
        kernel_vfs_path_root(mount, heap, &root_path) != 0 ||
        kernel_vfs_path_lookup(root_path, "orphan-stat", 11U,
                               &named_path) != 0 ||
        kernel_vfs_unlink(mount, "/orphan-stat") != 0 ||
        kernel_vfs_path_inode(named_path) !=
            kernel_vfs_file_inode(&orphan_stat_file) ||
        kernel_vfs_path_lookup(root_path, "orphan-stat", 11U,
                               &missing_path) != -KERNEL_ENOENT ||
        missing_path != 0 ||
        kernel_vfs_fstat(&orphan_stat_file, &orphan_stat) != 0 ||
        orphan_stat.nlink != 0U || orphan_stat.dev != mount->id ||
        (orphan_stat.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        kernel_vfs_path_release(&named_path) != 0 ||
        kernel_vfs_path_release(&root_path) != 0 ||
        kernel_vfs_close(&orphan_stat_file) != 0) {
        fail_vfs(28U, 0, orphan_stat.nlink);
    }
    if (kernel_vfs_mkdir(mount, "/held-directory", 0755U) != 0 ||
        kernel_vfs_path_root(mount, heap, &root_path) != 0 ||
        kernel_vfs_path_lookup(root_path, "held-directory", 14U,
                               &named_path) != 0 ||
        kernel_vfs_rmdir(mount, "/held-directory") != 0 ||
        kernel_vfs_path_stat(named_path, &orphan_stat) != 0 ||
        orphan_stat.nlink != 0U ||
        kernel_vfs_path_lookup(root_path, "held-directory", 14U,
                               &missing_path) != -KERNEL_ENOENT ||
        kernel_vfs_mkdir(mount, "/held-directory", 0755U) != 0 ||
        kernel_vfs_path_lookup(root_path, "held-directory", 14U,
                               &replacement_path) != 0 ||
        kernel_vfs_path_inode(replacement_path) ==
            kernel_vfs_path_inode(named_path) ||
        kernel_vfs_path_release(&replacement_path) != 0 ||
        kernel_vfs_path_release(&named_path) != 0 ||
        kernel_vfs_path_release(&root_path) != 0) {
        fail_vfs(29U, 0, orphan_stat.nlink);
    }
}

static void run_path_resolution_regression(struct kernel_vfs_mount *mount,
                                           struct kernel_heap *heap)
{
    struct kernel_vfs_file file = {0};
    struct kernel_vfs_path *root = 0;
    struct kernel_vfs_path *too_long = 0;
    struct kernel_vfs_stat stat;
    char long_name[257];

    if (kernel_vfs_mkdir(mount, "/path-test-dir", 0755U) != 0 ||
        kernel_vfs_symlink(mount, "../init", "/path-test-dir/relative") != 0 ||
        kernel_vfs_symlink(mount, "/init", "/path-test-dir/absolute") != 0 ||
        kernel_vfs_open(mount, "/path-test-dir/./relative", &file) != 0 ||
        kernel_vfs_close(&file) != 0 ||
        kernel_vfs_open(mount, "/path-test-dir/../path-test-dir/absolute",
                        &file) != 0 ||
        kernel_vfs_close(&file) != 0 ||
        kernel_vfs_open(mount, "/path-test-dir/relative/", &file) !=
            -KERNEL_ENOTDIR ||
        kernel_vfs_stat_path(mount, "/path-test-dir/relative", 0,
                             &stat) != 0 ||
        (stat.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFLNK)
        fail_vfs(30U, 0, -1);

    if (kernel_vfs_symlink(mount, "created", "/path-test-dir/dangling") != 0 ||
        kernel_vfs_create(mount, "/path-test-dir/dangling", 0600U,
                          &file) != 0 ||
        kernel_vfs_close(&file) != 0 ||
        kernel_vfs_open(mount, "/path-test-dir/created", &file) != 0 ||
        kernel_vfs_close(&file) != 0 ||
        kernel_vfs_symlink(mount, "loop-b", "/path-test-dir/loop-a") != 0 ||
        kernel_vfs_symlink(mount, "loop-a", "/path-test-dir/loop-b") != 0 ||
        kernel_vfs_open(mount, "/path-test-dir/loop-a", &file) !=
            -KERNEL_ELOOP)
        fail_vfs(31U, 0, -1);
    if (kernel_vfs_mkdir(mount, "/path-test-trailing/", 0755U) != 0 ||
        kernel_vfs_stat_path(mount, "/path-test-trailing/", 1, &stat) != 0 ||
        (stat.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR ||
        kernel_vfs_rmdir(mount, "/path-test-trailing/") != 0)
        fail_vfs(32U, 0, -1);
    if (kernel_vfs_mkdir(mount, "/path-test-rmdir-dot", 0755U) != 0 ||
        kernel_vfs_rmdir(mount, "/path-test-rmdir-dot/.") !=
            -KERNEL_EINVAL ||
        kernel_vfs_stat_path(mount, "/path-test-rmdir-dot", 1, &stat) != 0 ||
        kernel_vfs_symlink(mount, "/path-test-rmdir-dot",
                           "/path-test-rmdir-link") != 0 ||
        kernel_vfs_open_nofollow(mount, "/path-test-rmdir-link/", &file) !=
            0 ||
        (file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFDIR ||
        kernel_vfs_close(&file) != 0 ||
        kernel_vfs_unlink(mount, "/path-test-rmdir-link/") !=
            -KERNEL_ENOTDIR ||
        kernel_vfs_rmdir(mount, "/path-test-rmdir-link/") !=
            -KERNEL_ENOTDIR ||
        kernel_vfs_stat_path(mount, "/path-test-rmdir-dot", 1, &stat) != 0 ||
        kernel_vfs_unlink(mount, "/path-test-rmdir-link") != 0 ||
        kernel_vfs_rmdir(mount, "/path-test-rmdir-dot") != 0)
        fail_vfs(33U, 0, -1);
    for (size_t index = 0U; index < sizeof(long_name) - 1U; index++)
        long_name[index] = 'x';
    long_name[sizeof(long_name) - 1U] = '\0';
    if (kernel_vfs_path_root(mount, heap, &root) != 0 ||
        kernel_vfs_path_lookup(root, long_name, sizeof(long_name) - 1U,
                               &too_long) != -KERNEL_ENAMETOOLONG ||
        too_long != 0 || kernel_vfs_path_release(&root) != 0)
        fail_vfs(34U, -KERNEL_ENAMETOOLONG, -KERNEL_EINVAL);
    if (kernel_vfs_create(mount, "/path-test-missing/", 0600U, &file) !=
            -KERNEL_EISDIR ||
        file.private_data != 0 || kernel_vfs_rmdir(mount, "/") !=
            -KERNEL_EBUSY ||
        kernel_vfs_rmdir(mount, "///") != -KERNEL_EBUSY)
        fail_vfs(37U, -KERNEL_EBUSY, -1);
    if (kernel_vfs_symlink(mount, "/path-test-mkdir-target",
                           "/path-test-mkdir-link") != 0 ||
        kernel_vfs_mkdir(mount, "/path-test-mkdir-link/", 0755U) !=
            -KERNEL_EEXIST ||
        kernel_vfs_stat_path(mount, "/path-test-mkdir-target", 1, &stat) !=
            -KERNEL_ENOENT ||
        kernel_vfs_unlink(mount, "/path-test-mkdir-link") != 0)
        fail_vfs(40U, -KERNEL_EEXIST, -1);
}

static void run_shared_path_regression(struct kernel_vfs_mount *mount,
                                       struct kernel_heap *heap)
{
    struct kernel_vfs_path *root = 0, *root_alias = 0;
    struct kernel_vfs_path *first = 0, *alias = 0, *dot = 0;
    struct kernel_vfs_file directory = {0}, reopened = {0};
    char name[64];
    if (kernel_vfs_mkdir(mount, "/shared-path", 0700U) != 0 ||
        kernel_vfs_path_root(mount, heap, &root) != 0 ||
        kernel_vfs_path_root(mount, heap, &root_alias) != 0 ||
        root != root_alias ||
        kernel_vfs_path_lookup(root, "shared-path", 11U, &first) != 0 ||
        kernel_vfs_path_lookup(root_alias, "shared-path", 11U, &alias) != 0 ||
        first != alias ||
        kernel_vfs_open(mount, "/shared-path", &directory) != 0 ||
        directory.path != first ||
        kernel_vfs_path_string(first, root, name, sizeof(name)) != 0 ||
        strcmp(name, "/shared-path"))
        fail_vfs(80U, 0, -1);
    if (kernel_vfs_rmdir(mount, "/shared-path") != 0 ||
        kernel_vfs_path_lookup(first, ".", 1U, &dot) != 0 || dot != first ||
        kernel_vfs_path_release(&dot) != 0 ||
        kernel_vfs_path_lookup(first, "..", 2U, &dot) != 0 || dot != root ||
        kernel_vfs_path_release(&dot) != 0 ||
        kernel_vfs_open_at(first, root, ".", 1, &reopened) != 0 ||
        !kernel_vfs_files_share_node(&directory, &reopened) ||
        kernel_vfs_path_string(first, root, name, sizeof(name)) != -KERNEL_ENOENT ||
        kernel_vfs_close(&reopened) != 0 ||
        kernel_vfs_close(&directory) != 0 ||
        kernel_vfs_path_release(&alias) != 0 ||
        kernel_vfs_path_release(&first) != 0 ||
        kernel_vfs_path_release(&root_alias) != 0 ||
        kernel_vfs_path_release(&root) != 0)
        fail_vfs(81U, 0, -1);
}

static void run_rename_path_regression(struct kernel_vfs_mount *mount,
                                       struct kernel_heap *heap)
{
    struct kernel_vfs_path *root = 0, *parent = 0, *child = 0, *alias = 0;
    struct kernel_vfs_file source = {0}, target = {0}, moved = {0};
    char name[64], data[4];
    size_t count;
    if (kernel_vfs_path_root(mount, heap, &root) ||
        kernel_vfs_mkdir(mount, "/rename-parent", 0700) ||
        kernel_vfs_mkdir(mount, "/rename-parent/child", 0700) ||
        kernel_vfs_path_resolve(root, root, "/rename-parent", 1, &parent) ||
        kernel_vfs_path_resolve(parent, root, "child", 1, &child) ||
        kernel_vfs_rename_at(root, root, root, "/rename-parent", "/renamed", 0) ||
        kernel_vfs_path_string(child, root, name, sizeof(name)) ||
        strcmp(name, "/renamed/child") ||
        kernel_vfs_path_resolve(root, root, "/renamed/child", 1, &alias) ||
        alias != child || kernel_vfs_path_release(&alias))
        fail_vfs(82U, 0, -1);
    if (kernel_vfs_create_at(child, root, "source", 0600, &source) ||
        kernel_vfs_create_at(root, root, "/target", 0600, &target) ||
        kernel_vfs_pwrite(&target, 0, "old", 3, &count) || count != 3 ||
        kernel_vfs_pwrite(&source, 0, "new", 3, &count) || count != 3 ||
        kernel_vfs_rename_at(child, root, root, "source", "/target", 1) !=
            -KERNEL_EEXIST ||
        kernel_vfs_rename_at(child, root, root, "source", "/target", 0) ||
        kernel_vfs_open(mount, "/target", &moved) ||
        moved.path != source.path ||
        !kernel_vfs_files_share_node(&moved, &source) ||
        kernel_vfs_files_share_node(&moved, &target) ||
        kernel_vfs_pread(&target, 0, data, sizeof(data), &count) || count != 3 ||
        memcmp(data, "old", 3) ||
        kernel_vfs_pread(&moved, 0, data, sizeof(data), &count) || count != 3 ||
        memcmp(data, "new", 3) ||
        kernel_vfs_path_string(target.path, root, name, sizeof(name)) !=
            -KERNEL_ENOENT ||
        kernel_vfs_close(&moved) || kernel_vfs_close(&target) ||
        kernel_vfs_close(&source))
        fail_vfs(83U, 0, -1);
    if (kernel_vfs_rename_at(root, child, root, "/renamed", "cycle", 0) !=
            -KERNEL_EINVAL ||
        kernel_vfs_rename_at(root, root, root, "/target", "/another", 2) !=
            -KERNEL_ENOTSUP ||
        kernel_vfs_rename_at(root, root, root, "/target", "/another", 8) !=
            -KERNEL_EINVAL ||
        kernel_vfs_unlink(mount, "/target") ||
        kernel_vfs_rmdir(mount, "/renamed/child") ||
        kernel_vfs_rmdir(mount, "/renamed") ||
        kernel_vfs_path_release(&child) || kernel_vfs_path_release(&parent) ||
        kernel_vfs_path_release(&root))
        fail_vfs(84U, 0, -1);
}

static void run_deep_relative_path_regression(struct kernel_vfs_mount *mount,
                                               struct kernel_heap *heap)
{
    struct kernel_vfs_path *root = 0, *current = 0, *next = 0;
    struct kernel_vfs_file file = {0};
    char component[256];
    memset(component, 'd', 255U);
    component[255] = 0;
    if (kernel_vfs_path_root(mount, heap, &root) ||
        kernel_vfs_mkdir_at(root, root, "deep-chain", 0700) ||
        kernel_vfs_path_lookup(root, "deep-chain", 10, &current))
        fail_vfs(85U, 0, -1);
    for (unsigned i = 0; i < 17; i++) {
        int result = kernel_vfs_mkdir_at(current, root, component, 0700);
        if (result || kernel_vfs_path_lookup(current, component, 255U, &next))
            fail_vfs(86U, 0, result);
        (void)kernel_vfs_path_release(&current);
        current = next;
        next = 0;
    }
    if (kernel_vfs_create_at(current, root, "leaf", 0600, &file) ||
        kernel_vfs_close(&file) ||
        kernel_vfs_symlink_at(current, root, "leaf", "link") ||
        kernel_vfs_open_at(current, root, "link", 1, &file) ||
        kernel_vfs_close(&file) ||
        kernel_vfs_unlink_at(current, root, "link", 0) ||
        kernel_vfs_unlink_at(current, root, "leaf", 0))
        fail_vfs(87U, 0, -1);
    for (unsigned i = 0; i < 17; i++) {
        if (kernel_vfs_path_lookup(current, "..", 2U, &next) ||
            kernel_vfs_unlink_at(next, root, component, 1))
            fail_vfs(88U, 0, -1);
        (void)kernel_vfs_path_release(&current);
        current = next;
        next = 0;
    }
    if (kernel_vfs_path_release(&current) ||
        kernel_vfs_unlink_at(root, root, "deep-chain", 1) ||
        kernel_vfs_path_release(&root)) fail_vfs(89U, 0, -1);
}

static void run_path_cleanup_regression(struct kernel_vfs_mount *mount,
                                        struct kernel_heap *heap)
{
    struct kernel_vfs_file file = {0};
    struct kernel_vfs_file alias = {0};
    struct kernel_vfs_path *root = 0;
    struct kernel_vfs_path *child = 0;

    if (kernel_vfs_create(mount, "/path-cleanup", 0600U, &file) != 0 ||
        kernel_vfs_close(&file) != 0 ||
        kernel_vfs_path_root(mount, heap, &root) != 0 ||
        kernel_vfs_path_lookup(root, "path-cleanup", 12U, &child) != 0)
        fail_vfs(35U, 0, -1);
    fail_fclose_calls = 1U;
    if (kernel_vfs_path_release(&child) != 0 || child != 0 ||
        fail_fclose_calls != 0U || failed_fclose_calls != 1U ||
        kernel_vfs_path_release(&root) != 0 || root != 0)
        fail_vfs(36U, 0, -1);
    if (kernel_vfs_create(mount, "/path-merge-cleanup", 0600U, &file) != 0)
        fail_vfs(38U, 0, -1);
    fail_fclose_calls = 1U;
    if (kernel_vfs_open(mount, "/path-merge-cleanup", &alias) !=
            -KERNEL_EIO ||
        alias.private_data != 0 || fail_fclose_calls != 0U ||
        failed_fclose_calls != 2U || kernel_vfs_close(&file) != 0)
        fail_vfs(39U, -KERNEL_EIO, -1);
}
#endif

#ifndef VFS_EXPECT_RECOVERY
static void run_writeback_regression(struct kernel_vfs_mount *mount,
                                      struct kernel_page_cache *cache)
{
    struct kernel_vfs_file first = {0}, second = {0}, alias = {0};
    struct kernel_vfs_stat stat;
    ext4_file raw;
    uint64_t observed = 0;
    size_t count = 0;
    char result[8] = {0};
    if (kernel_vfs_create(mount, "/wb-first", 0600, &first) ||
        kernel_vfs_create(mount, "/wb-second", 0600, &second) ||
        kernel_vfs_pwrite(&first, 0, "first", 5, &count) || count != 5 ||
        kernel_vfs_pwrite(&second, 0, "second", 6, &count) || count != 6 ||
        kernel_vfs_open(mount, "/wb-first", &alias) ||
        kernel_vfs_fstat(&alias, &stat) || stat.size != 5 ||
        kernel_vfs_pread(&alias, 0, result, sizeof(result), &count) ||
        count != 5 || !bytes_equal((unsigned char *)result, "first", 5))
        fail_vfs(70, 0, -1);
    if (ext4_fopen(&raw, "/wb-first", "r") || ext4_fsize(&raw) != 0 ||
        ext4_fclose(&raw)) fail_vfs(71, 0, -1);
    struct kernel_vfs_file clean = {0};
    if (kernel_vfs_open(mount, "/init", &clean) ||
        kernel_vfs_pread(&clean, 0, result, 1, &count) || count != 1 ||
        kernel_vfs_close(&clean)) fail_vfs(76, 0, -1);
    /* Reclaim at a live lwext4 allocation boundary may release clean pages,
     * but cannot reenter the backend through dirty VFS pages. */
    pressure_cache = cache;
    void *allocation = ext4_user_malloc(4096);
    if (allocation == 0 || pressure_cache != 0 || pressure_reclaimed == 0 ||
        ext4_fopen(&raw, "/wb-first", "r") || ext4_fsize(&raw) != 0 ||
        ext4_fclose(&raw)) fail_vfs(77, 0, -1);
    ext4_user_free(allocation);
    if (kernel_vfs_sync(&first, 0, &observed)) fail_vfs(72, 0, -1);
    if (ext4_fopen(&raw, "/wb-first", "r") || ext4_fsize(&raw) != 5 ||
        ext4_fclose(&raw) || ext4_fopen(&raw, "/wb-second", "r") ||
        ext4_fsize(&raw) != 0 || ext4_fclose(&raw)) fail_vfs(73, 0, -1);
    if (kernel_vfs_close(&first) || kernel_vfs_close(&alias) ||
        kernel_vfs_close(&second)) fail_vfs(74, 0, -1);
    /* Dirty inode remains alive after the last fd, and a new open sees it. */
    if (kernel_vfs_open(mount, "/wb-second", &second) ||
        kernel_vfs_sync(&second, 1, &observed) ||
        kernel_vfs_close(&second)) fail_vfs(75, 0, -1);
}
#endif

static void run_vfs_test(const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    struct kernel_page_cache page_cache = {0};
    struct riscv_virtio_mmio_block device = {0};
    struct kernel_vfs_mount mount = {0};
#ifndef VFS_EXPECT_RECOVERY
    static const char expected[] =
        "BoarOS root init payload for VFS and ELF";
    struct riscv_virtio_mmio_block_statistics statistics;
    struct kernel_vfs_file file = {0};
    struct kernel_vfs_file alias = {0};
    struct kernel_vfs_file large = {0};
    struct kernel_vfs_file missing = {0};
    struct kernel_read_source source;
    struct kernel_vfs_stat metadata;
    unsigned char buffer[64];
    uint64_t first_page = 0U;
    uint64_t alias_page = 0U;
    size_t valid_bytes = 0U;
    struct kernel_page_cache_statistics cache_statistics;
    uint64_t cached_page = 0U;
#endif
    uint64_t baseline;
    uint32_t index;
    int result;
#ifndef VFS_EXPECT_RECOVERY
    size_t read_count = 0U;
#endif
    int found = 0;

    if (dtb_read_boot_info(dtb, &info) != DTB_STATUS_OK ||
        info.timebase_frequency == 0U) {
        fail_vfs(1U, DTB_STATUS_OK, -1);
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
            KERNEL_HEAP_STATUS_OK) {
        fail_vfs(2U, 0, -1);
    }
    baseline = physical_page_available(&allocator);
    if (kernel_page_cache_init(&page_cache, &heap, &allocator) !=
        KERNEL_PAGE_CACHE_STATUS_OK) {
        fail_vfs(2U, 0, -1);
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
            fail_vfs(3U, RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, status);
        }
    }
    if (!found) {
        fail_vfs(4U, 1, 0);
    }

#ifndef VFS_EXPECT_RECOVERY
    /* A journal-start allocation failure is known to precede commit. The
     * mount must remain cleanable, then permit a fresh successful mount. */
    fail_journal_start_allocation = 1;
    result = kernel_vfs_mount_root(&mount, &device.block, &heap, &page_cache);
    if (result != -KERNEL_ENOMEM ||
        (mount.private_data != 0 && kernel_vfs_unmount(&mount) != 0) ||
        mount.private_data != 0)
        fail_vfs(79U, -KERNEL_ENOMEM, result);
#endif
    result = kernel_vfs_mount_root(&mount,
                                    &device.block,
                                    &heap,
                                    &page_cache);
#ifdef VFS_EXPECT_RECOVERY
    if (result != -KERNEL_EUCLEAN || mount.private_data != 0 ||
        mount.state != 0U) {
        fail_vfs(5U, -KERNEL_EUCLEAN, result);
    }
    if (kernel_page_cache_destroy(&page_cache) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        riscv_virtio_mmio_block_destroy(&device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        fail_vfs(6U,
                 (long)baseline,
                 (long)physical_page_available(&allocator));
    }
#else
    if (result != 0) {
        fail_vfs(5U, 0, result);
    }
    result = kernel_vfs_open(&mount, "/missing", &missing);
    if (result != -KERNEL_ENOENT) {
        fail_vfs(6U, -KERNEL_ENOENT, result);
    }
    result = kernel_vfs_open(&mount, "/init", &file);
    if (result != 0 || file.size != sizeof(expected) - 1U ||
        (file.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG ||
        (file.mode & (KERNEL_VFS_S_IXUSR |
                      KERNEL_VFS_S_IXGRP |
                      KERNEL_VFS_S_IXOTH)) == 0U) {
        fail_vfs(7U, 0, result);
    }
    if (kernel_vfs_fstat(&file, &metadata) != 0 || metadata.dev != 1U ||
        metadata.ino == 0U || metadata.nlink != 2U ||
        metadata.size != sizeof(expected) - 1U || metadata.blocks == 0U ||
        metadata.blksize == 0U ||
        (metadata.mode & KERNEL_VFS_S_IFMT) != KERNEL_VFS_S_IFREG) {
        fail_vfs(29U, 2, metadata.nlink);
    }
    result = kernel_vfs_open(&mount, "/init-link", &alias);
    if (result != 0 || !kernel_vfs_files_share_node(&file, &alias) ||
        kernel_page_cache_get(&page_cache,
                              &file,
                              0U,
                              &first_page,
                              &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        valid_bytes != sizeof(expected) - 1U ||
        physical_page_release(&allocator, first_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_page_cache_get(&page_cache,
                              &alias,
                              0U,
                              &alias_page,
                              &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        alias_page != first_page) {
        fail_vfs(15U, 0, result);
    }
    if (kernel_page_cache_reclaim(&page_cache, 1U) != 0U ||
        physical_page_release(&allocator, alias_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_page_cache_reclaim(&page_cache, 1U) != 1U) {
        fail_vfs(16U, 1, 0);
    }
    result = kernel_vfs_open(&mount, "/large", &large);
    if (result != 0) {
        fail_vfs(18U, 0, result);
    }
    for (index = 0U; index < 20U; index++) {
        if (kernel_page_cache_get(&page_cache,
                                  &large,
                                  index,
                                  &cached_page,
                                  &valid_bytes) !=
                KERNEL_PAGE_CACHE_STATUS_OK ||
            valid_bytes != BOAROS_PAGE_SIZE ||
            physical_page_release(&allocator, cached_page) !=
                PHYSICAL_PAGE_STATUS_OK) {
            fail_vfs(19U, KERNEL_PAGE_CACHE_STATUS_OK,
                     KERNEL_PAGE_CACHE_STATUS_STATE);
        }
    }
    if (kernel_page_cache_get(&page_cache,
                              &large,
                              0U,
                              &cached_page,
                              &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        physical_page_release(&allocator, cached_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_page_cache_reclaim(&page_cache, 1U) != 1U ||
        kernel_page_cache_lookup(&page_cache,
                                 &large,
                                 1U,
                                 &cached_page,
                                 &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_NOT_FOUND ||
        kernel_page_cache_lookup(&page_cache,
                                 &large,
                                 0U,
                                 &cached_page,
                                 &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        physical_page_release(&allocator, cached_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_page_cache_get(&page_cache,
                              &large,
                              1U,
                              &cached_page,
                              &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        physical_page_release(&allocator, cached_page) !=
            PHYSICAL_PAGE_STATUS_OK) {
        fail_vfs(20U, KERNEL_PAGE_CACHE_STATUS_OK,
                 KERNEL_PAGE_CACHE_STATUS_STATE);
    }
    kernel_page_cache_get_statistics(&page_cache, &cache_statistics);
    if (cache_statistics.current_pages != 20U ||
        cache_statistics.peak_pages != 20U ||
        cache_statistics.hits < 3U ||
        cache_statistics.evictions != 2U) {
        fail_vfs(21U, 20, (long)cache_statistics.current_pages);
    }
    if (kernel_vfs_close(&large) != 0 ||
        kernel_vfs_open(&mount, "/large", &large) != 0 ||
        kernel_page_cache_lookup(&page_cache,
                                 &large,
                                 1U,
                                 &cached_page,
                                 &valid_bytes) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        physical_page_release(&allocator, cached_page) !=
            PHYSICAL_PAGE_STATUS_OK ||
        kernel_vfs_close(&large) != 0) {
        fail_vfs(22U, KERNEL_PAGE_CACHE_STATUS_OK,
                 KERNEL_PAGE_CACHE_STATUS_STATE);
    }

    result = kernel_vfs_file_read_source(&file, &source);
    if (result != 0 || source.size != file.size ||
        kernel_read_source_read_exact(
            &source,
            7U,
            buffer,
            sizeof(expected) - 1U - 7U) != 0 ||
        !bytes_equal(buffer,
                     expected + 7U,
                     sizeof(expected) - 1U - 7U)) {
        fail_vfs(8U, 0, result);
    }
    result = kernel_vfs_pread(&file,
                              file.size,
                              buffer,
                              sizeof(buffer),
                              &read_count);
    if (result != 0 || read_count != 0U) {
        fail_vfs(9U, 0, result);
    }

    result = kernel_vfs_unmount(&mount);
    if (result != -KERNEL_EBUSY) {
        fail_vfs(10U, -KERNEL_EBUSY, result);
    }

    result = kernel_vfs_close(&alias);
    if (result != 0) {
        fail_vfs(23U, 0, result);
    }
    result = kernel_vfs_close(&file);
    if (result != 0) {
        fail_vfs(11U, 0, result);
    }
    run_orphan_cleanup_regression(&mount, &heap);
    run_path_resolution_regression(&mount, &heap);
    run_shared_path_regression(&mount, &heap);
    run_rename_path_regression(&mount, &heap);
    run_deep_relative_path_regression(&mount, &heap);
    run_path_cleanup_regression(&mount, &heap);
    run_writeback_regression(&mount, &page_cache);
    result = kernel_vfs_unmount(&mount);
    if (result != 0 || retried_fclose_calls != failed_fclose_calls) {
        fail_vfs(12U, 0, result);
    }

    device.block.write = 0;
    device.block.flush = 0;
    device.block.cache_mode = KERNEL_BLOCK_CACHE_UNKNOWN;
    uint64_t observed_error = 0;
    if (kernel_vfs_mount_root(&mount, &device.block, &heap, &page_cache) ||
        kernel_vfs_open(&mount, "/init", &file) ||
        kernel_vfs_sync(&file, 0, &observed_error) ||
        kernel_vfs_close(&file) || kernel_vfs_unmount(&mount))
        fail_vfs(78U, 0, -1);

    riscv_virtio_mmio_block_get_statistics(&device, &statistics);
    if (statistics.requests == 0U || statistics.timeouts != 0U ||
        statistics.io_errors != 0U) {
        fail_vfs(13U, 1, (long)statistics.requests);
    }
    if (kernel_page_cache_destroy(&page_cache) !=
            KERNEL_PAGE_CACHE_STATUS_OK ||
        riscv_virtio_mmio_block_destroy(&device) !=
            RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK ||
        physical_page_available(&allocator) != baseline) {
        fail_vfs(14U,
                 (long)baseline,
                 (long)physical_page_available(&allocator));
    }
#endif
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;

    run_vfs_test(dtb);

#ifdef VFS_EXPECT_RECOVERY
    virt_uart_puts("BoarOS: VFS ext4 recovery rejection passed\n");
#else
    virt_uart_puts("BoarOS: VFS ext4 tests passed\n");
#endif
    sbi_shutdown();
}
