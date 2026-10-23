#include <kernel/files.h>
#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm_backend.h>
#include <kernel/open_file.h>
#include <kernel/physical_page.h>
#include <kernel/tmpfs.h>
#include <kernel/uaccess.h>
#include "../../fs/files/private.h"
#include "../../fs/vfs_objects.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void mm_test_attach(unsigned);
void mm_test_bind(struct kernel_mm *);
extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int page_address(const void *p, uint64_t *out)
{ *out = (uintptr_t)p; return 1; }
static unsigned failures, checks, pathless_stats;
static struct kernel_files files;
static struct kernel_fs_context fs;
static struct kernel_mm mm;
static uint64_t user_path;

static void check(const char *name, int64_t actual, int64_t expected)
{
    checks++;
    if (actual != expected) {
        fprintf(stderr, "FAIL %s: actual=%ld expected=%ld\n", name,
                (long)actual, (long)expected);
        failures++;
    }
}
static void access_case(const char *name, int64_t dirfd, const char *path,
                        uint64_t mode, int64_t expected)
{
    size_t copied;
    int64_t result = 1;
    assert(!kernel_copy_to_user(&mm, user_path, path, strlen(path) + 1, &copied));
    assert(copied == strlen(path) + 1);
    assert(!kernel_files_faccessat(&files, &fs, &mm, dirfd, user_path, mode, &result));
    check(name, result, expected);
}
/* A pathless backend object supplies metadata, never its enclosing mount.
 * The production proc pipe/socket/anon-fd callbacks use this same contract. */
static int stat_pathless(struct kernel_vfs_node *node, struct kernel_vfs_stat *stat)
{
    assert((node->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK);
    pathless_stats++;
    *stat = (struct kernel_vfs_stat){.mode=KERNEL_VFS_S_IFIFO | 0600};
    return 0;
}
int main(void)
{
    mm_test_attach(0);
    struct physical_page_allocator pages = {0};
    struct kernel_heap heap = {0};
    struct boot_memory_layout layout = {.usable_count=1,
        .usable={{(uintptr_t)pool, sizeof(pool)}}};
    assert(!physical_page_allocator_init(&pages, &layout) &&
        !physical_page_allocator_bind_access(&pages, access_page) &&
        !physical_page_allocator_finalize(&pages) &&
        !kernel_heap_init(&heap, &pages, page_address));
    uint64_t baseline = physical_page_available(&pages);
    struct arch_mmu_page_table kernel = {0};
    struct arch_mmu_user_space space = {0};
    assert(!riscv_sv39_page_table_init(&kernel, &pages));
    kernel.state = ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&space, &pages, &kernel) &&
        !kernel_mm_create(&mm, &space) && !kernel_mm_vma_enable(&mm, &heap));
    assert(!kernel_mm_brk_initialize(&mm, 0x10000, 0x20000));
    mm_test_bind(&mm);
    assert(!kernel_mm_context(&mm, &mm_test_context));
    assert(!kernel_mm_mmap_anonymous(&mm, 0x300000, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE, &user_path));
    struct kernel_vfs_mount *rw = 0, *ro = 0;
    struct kernel_vfs_path *root = 0, *covered = 0, *readonly = 0;
    assert(!kernel_tmpfs_create(&heap, 0, "", &rw) &&
        !kernel_tmpfs_create(&heap, 0, "", &ro));
    assert(!kernel_vfs_path_root(rw, &heap, &root));
    assert(!kernel_vfs_mkdir_at(root, root, "/ro", 0755));
    assert(!kernel_vfs_mknod_at(root, root, "/rw-file", 0600, 0));
    assert(!kernel_vfs_symlink_at(root, root, "/rw-file", "/rw-link"));
    assert(!kernel_vfs_mknod_at(root, root, "/fifo", KERNEL_VFS_S_IFIFO | 0600, 0));
    assert(!kernel_vfs_path_resolve(root, root, "/ro", 1, &covered));
    assert(!kernel_vfs_mount_attach(ro, covered));
    assert(!kernel_vfs_path_resolve(root, root, "/ro", 1, &readonly));
    assert(!kernel_vfs_mknod_at(readonly, root, "regular", 0600, 0));
    assert(!kernel_vfs_symlink_at(readonly, root, "missing", "pathless"));
    struct kernel_vfs_instance *instance = ro->private_data;
    struct kernel_vfs_backend ops = *instance->ops;
    ops.stat_link = stat_pathless;
    instance->ops = &ops;
    instance->read_only = 1;
    assert(!kernel_files_create(&files, &heap) && !kernel_fs_context_create(&fs, rw, &heap));
    struct kernel_open_file_description *description = 0;
    int error;
    assert(!kernel_open_file_create_at(&heap, root, root, "/ro",
        &(struct kernel_vfs_open_request){.flags=KERNEL_VFS_OPEN_FOLLOW},
        &description, 0, &error) && !error);
    assert(!kernel_files_install_new_owned_at(&files, 5, 0, &description));
    access_case("rw to readonly mount", -100, "/ro", 2, -KERNEL_EROFS);
    access_case("rw to readonly regular", -100, "/ro/regular", 2, -KERNEL_EROFS);
    access_case("readonly dirfd to rw file", 5, "../rw-file", 2, 0);
    access_case("readonly dirfd through rw symlink", 5, "../rw-link", 2, 0);
    access_case("execute permission before readonly", -100, "/ro/regular", 3, -KERNEL_EACCES);
    access_case("fifo execute", -100, "/fifo", 1, -KERNEL_EACCES);
    access_case("fifo read and write", -100, "/fifo", 6, 0);
    access_case("pathless execute", -100, "/ro/pathless", 1, -KERNEL_EACCES);
    access_case("pathless read and write", -100, "/ro/pathless", 6, 0);
    check("pathless metadata calls", pathless_stats, 2);
    access_case("missing before permission", -100, "/ro/missing", 3, -KERNEL_ENOENT);
    access_case("empty path", -100, "", 0, -KERNEL_ENOENT);
    access_case("relative invalid dirfd", -9, "rw-file", 0, -KERNEL_EBADF);
    access_case("absolute ignores dirfd", -9, "/rw-file", 0, 0);
    int64_t result;
    assert(!kernel_files_faccessat(&files, &fs, &mm, -9, 1, 8, &result));
    check("invalid mode before pointer and dirfd", result, -KERNEL_EINVAL);
    assert(!kernel_files_faccessat(&files, &fs, &mm, -9, 1, 0, &result));
    check("invalid pointer before dirfd", result, -KERNEL_EFAULT);
    assert(!kernel_mm_munmap(&mm, user_path, BOAROS_PAGE_SIZE));
    mm_test_bind(0); mm_test_context = 0;
    assert(!kernel_files_release(&files) && !kernel_fs_context_release(&fs));
    assert(!kernel_vfs_mount_detach(ro, readonly));
    assert(!kernel_vfs_path_release(&readonly) && !kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_unmount(ro) && !kernel_vfs_path_release(&root) && !kernel_vfs_unmount(rw));
    assert(!kernel_mm_release(&mm) && !physical_page_release(&pages, kernel.root_address));
    assert(!physical_page_allocator_audit(&pages));
    struct kernel_heap_statistics final;
    kernel_heap_get_statistics(&heap, &final);
    assert(!final.live_allocations && !final.current_pages);
    assert(physical_page_available(&pages) == baseline);
    printf("access: %u checks, %u failures; all paths, mounts, fd owners, MM and heap released\n", checks, failures);
    return failures != 0;
}
