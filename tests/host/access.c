#include <kernel/files.h>
#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm_backend.h>
#include <kernel/open_file.h>
#include <kernel/physical_page.h>
#include <kernel/tmpfs.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/credentials.h>
#include "../../fs/files/private.h"
#include "../../fs/vfs_objects.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/char_device_internal.h"
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
static unsigned failures, checks, pathless_stats, pathless_reopens;
static unsigned lookup_calls, watch_lookups;
static uint64_t pathless_inode;
static int pathless_follow_error=-KERNEL_ENOENT;
static unsigned replace_on_follow;
static struct kernel_vfs_path *replace_parent,*replace_root;
static int (*base_lookup)(struct kernel_vfs_instance *,uint64_t,const char *,size_t,uint64_t *,uint32_t *);
static struct kernel_files files;
static struct kernel_fs_context fs;
static struct kernel_mm mm;
static uint64_t user_path;
/* Access checks construct path OFDs without asynchronous subscriptions. */
void kernel_open_file_async_close(struct kernel_open_file_description *file)
{ assert(file && !file->async_io); }
const struct kernel_char_device *kernel_char_device_lookup(uint64_t rdev)
{ (void)rdev;assert(!"access fixture must not open a character device");return NULL; }
int kernel_char_device_open(struct kernel_open_file_description *file,uint32_t flags)
{ (void)file;(void)flags;assert(0);return -KERNEL_ENXIO; }
void kernel_signal_note_syscall_restart(struct kernel_task *task)
{ (void)task;assert(!"access fixture must not wait for a FIFO peer"); }

/* This fixture's timer ticks use the same nanoseconds as disk_context.c. */
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns,uint64_t *deadline)
{
    *deadline=ns;
    return ns<=kernel_time_monotonic_ns() ? KERNEL_TIME_STATUS_DEADLINE_PASSED :
                                          KERNEL_TIME_STATUS_OK;
}

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
    assert(!kernel_files_faccessat(&files, &fs, &mm, dirfd, user_path, mode, 0, &result));
    check(name, result, expected);
}
/* A pathless backend object supplies metadata, never its enclosing mount.
 * The production proc pipe/socket/anon-fd callbacks use this same contract. */
static int stat_pathless(struct kernel_vfs_node *node, struct kernel_vfs_stat *stat)
{
    assert((node->mode & KERNEL_VFS_S_IFMT) == KERNEL_VFS_S_IFLNK);
    if (node->inode!=pathless_inode) return -KERNEL_ENOTSUP;
    pathless_stats++;
    *stat = (struct kernel_vfs_stat){.mode=KERNEL_VFS_S_IFIFO | 0600};
    return 0;
}
static int follow_pathless(struct kernel_vfs_node *node,unsigned resolve,
    struct kernel_vfs_path **owner)
{
    (void)owner;
    if (node->inode!=pathless_inode) return -KERNEL_ENOTSUP;
    if (resolve & KERNEL_VFS_RESOLVE_NO_MAGICLINKS) return -KERNEL_ELOOP;
    if (resolve & (KERNEL_VFS_RESOLVE_SCOPED|KERNEL_VFS_RESOLVE_NO_XDEV)) return -KERNEL_EXDEV;
    if (replace_on_follow) {
        replace_on_follow=0;
        unsigned readonly=node->instance->read_only;
        node->instance->read_only=0;
        assert(!kernel_vfs_unlink_at(replace_parent,replace_root,"pathless",0));
        assert(!kernel_vfs_symlink_at(replace_parent,replace_root,"missing","pathless"));
        node->instance->read_only=readonly;
    }
    return pathless_follow_error;
}
static int reopen_pathless(struct kernel_vfs_node *node,struct kernel_heap *heap,
    uint32_t flags,struct kernel_open_file_description **owner)
{
    (void)flags;
    if (node->inode!=pathless_inode) return -KERNEL_ENOTSUP;
    pathless_reopens++;
    return kernel_open_file_create_memfd(heap,"pathless",0,owner);
}
static int count_lookup(struct kernel_vfs_instance *instance,uint64_t parent,
    const char *name,size_t length,uint64_t *inode,uint32_t *mode)
{
    if (watch_lookups) lookup_calls++;
    return base_lookup(instance,parent,name,length,inode,mode);
}
static void missing_stat_case(struct kernel_vfs_path *start,struct kernel_vfs_path *root,
    const char *name,unsigned expected)
{
    struct kernel_vfs_stat stat;
    lookup_calls=0;watch_lookups=1;
    int error=kernel_vfs_stat_at(start,root,name,1,&stat);
    watch_lookups=0;
    check(name,error,-KERNEL_ENOENT);
    check("one missing stat walk",lookup_calls,expected);
}
static void open_lookup_case(const char *name,uint32_t flags,int error,unsigned expected)
{
    size_t copied;int64_t result;
    assert(!kernel_copy_to_user(&mm,user_path,name,strlen(name)+1,&copied));
    lookup_calls=0;watch_lookups=1;
    assert(!kernel_files_openat(&files,&fs,&mm,-100,user_path,flags,0,&result));
    watch_lookups=0;
    check(name,result<0?result:0,error);
    check("one open walk",lookup_calls,expected);
    if (result>=0) assert(!kernel_files_close(&files,result,&result) && !result);
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
    assert(!kernel_vfs_mkdir_at(root, root, "/ro", 0755,0));
    assert(!kernel_vfs_mknod_at(root, root, "/rw-file", 0600, 0,0));
    assert(!kernel_vfs_symlink_at(root, root, "/rw-file", "/rw-link"));
    assert(!kernel_vfs_symlink_at(root, root, "missing", "/dangling"));
    assert(!kernel_vfs_symlink_at(root, root, "/ro/pathless", "/pathless-chain"));
    assert(!kernel_vfs_symlink_at(root, root, "cycle", "/cycle"));
    assert(!kernel_vfs_mkdir_at(root, root, "/denied", 0000,0));
    assert(!kernel_vfs_mknod_at(root, root, "/fifo", KERNEL_VFS_S_IFIFO | 0600, 0,0));
    assert(!kernel_vfs_path_resolve(root, root, "/ro", 1, &covered));
    assert(!kernel_vfs_mount_attach(ro, covered));
    assert(!kernel_vfs_path_resolve(root, root, "/ro", 1, &readonly));
    assert(!kernel_vfs_mknod_at(readonly, root, "regular", 0600, 0,0));
    assert(!kernel_vfs_symlink_at(readonly, root, "missing", "pathless"));
    struct kernel_vfs_path *pathless=NULL;
    assert(!kernel_vfs_path_resolve(readonly,root,"pathless",0,&pathless));
    pathless_inode=kernel_vfs_path_inode(pathless);
    assert(!kernel_vfs_path_release(&pathless));
    struct kernel_vfs_instance *instance = ro->private_data;
    struct kernel_vfs_backend ops = *instance->ops;
    base_lookup=ops.lookup;ops.lookup=count_lookup;
    ops.follow_link=follow_pathless;
    ops.reopen_link=reopen_pathless;
    ops.stat_link = stat_pathless;
    instance->ops = &ops;
    struct kernel_vfs_instance *rw_instance=rw->private_data;
    struct kernel_vfs_backend rw_ops=*rw_instance->ops;
    rw_ops.lookup=count_lookup;rw_instance->ops=&rw_ops;
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
    missing_stat_case(root,root,"/missing",1);
    missing_stat_case(root,root,"/ro/missing",2);
    missing_stat_case(readonly,root,"missing",1);
    missing_stat_case(root,root,"/dangling",2);
    access_case("pathless through text link",-100,"/pathless-chain",6,0);
    open_lookup_case("/ro/pathless",0,0,2);
    open_lookup_case("/pathless-chain",0,0,3);
    check("pathless reopen calls",pathless_reopens,2);
    open_lookup_case("/missing",0,-KERNEL_ENOENT,1);
    open_lookup_case("/ro/missing",0,-KERNEL_ENOENT,2);
    open_lookup_case("missing",0,-KERNEL_ENOENT,1);
    open_lookup_case("/dangling",0,-KERNEL_ENOENT,2);
    open_lookup_case("/ro/pathless",0400000,-KERNEL_ELOOP,2);
    open_lookup_case("/ro/pathless",0200000,-KERNEL_ENOTDIR,2);
    open_lookup_case("/ro/pathless",0100|0200,-KERNEL_EEXIST,2);
    open_lookup_case("/ro/pathless/",0,-KERNEL_ENOENT,2);
    pathless_follow_error=-KERNEL_ENXIO;
    access_case("ENXIO pathless metadata",-100,"/ro/pathless",6,0);
    unsigned reopens=pathless_reopens;
    open_lookup_case("/ro/pathless",0,-KERNEL_ENXIO,2);
    check("ENXIO must not reopen",pathless_reopens,reopens);
    pathless_follow_error=-KERNEL_ENOENT;
    struct kernel_vfs_stat stat;
    check("link cycle",kernel_vfs_stat_at(root,root,"/cycle",1,&stat),-KERNEL_ELOOP);
    struct kernel_credentials actor=*kernel_credentials_current();
    actor.fsuid=1000;actor.fsgid=1000;actor.effective=0;
    const struct kernel_credentials *previous=kernel_credentials_override(&actor);
    check("search permission before missing",kernel_vfs_stat_at(root,root,"/denied/missing",1,&stat),-KERNEL_EACCES);
    (void)kernel_credentials_override(previous);
    for (unsigned no_magic=0;no_magic<2;no_magic++) {
        struct kernel_vfs_file file={0};struct kernel_vfs_open_result opened={0};
        struct kernel_vfs_open_request request={.flags=KERNEL_VFS_OPEN_FOLLOW,
            .resolve=no_magic?KERNEL_VFS_RESOLVE_NO_MAGICLINKS:KERNEL_VFS_RESOLVE_NO_XDEV};
        check("scoped magic link",kernel_vfs_open_at(readonly,readonly,"pathless",&request,&file,&opened),
            no_magic?-KERNEL_ELOOP:-KERNEL_EXDEV);
        assert(!opened.pathless_link && !file.private_data && !file.path);
    }
    access_case("missing before permission", -100, "/ro/missing", 3, -KERNEL_ENOENT);
    access_case("empty path", -100, "", 0, -KERNEL_ENOENT);
    access_case("relative invalid dirfd", -9, "rw-file", 0, -KERNEL_EBADF);
    access_case("absolute ignores dirfd", -9, "/rw-file", 0, 0);
    int64_t result;
    assert(!kernel_files_faccessat(&files, &fs, &mm, -9, 1, 8, 0, &result));
    check("invalid mode before pointer and dirfd", result, -KERNEL_EINVAL);
    assert(!kernel_files_faccessat(&files, &fs, &mm, -9, 1, 0, 0, &result));
    check("invalid pointer before dirfd", result, -KERNEL_EFAULT);
    /* Replacement happens at the real follow callback, with its namespace
     * qualification released. Stat must consume the original owned link. */
    replace_parent=readonly;replace_root=root;replace_on_follow=1;
    check("pathless identity across name replacement",kernel_vfs_stat_at(root,root,"/ro/pathless",1,&stat),0);
    assert(!replace_on_follow && (stat.mode&KERNEL_VFS_S_IFMT)==KERNEL_VFS_S_IFIFO);
    check("replacement has its own identity",kernel_vfs_stat_at(root,root,"/ro/pathless",1,&stat),-KERNEL_ENOENT);
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
