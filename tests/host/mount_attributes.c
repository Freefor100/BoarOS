#include <kernel/mm_backend.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/task.h>
#include "../../kernel/syscall/private.h"
#include "../../fs/files/private.h"
#include "../../fs/open_file_internal.h"
#include "../../fs/vfs_objects.h"
#include <pthread.h>
#include <sched.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
void mm_test_bind(struct kernel_mm *);
void mm_test_bind_files(struct kernel_files *);
enum kernel_task_status kernel_task_mm_borrow_mutable(struct kernel_task *task,struct kernel_mm **mm)
{
    const struct kernel_mm *bound=0;
    enum kernel_task_status status=kernel_task_mm_borrow(task,&bound);
    if(!status)*mm=(struct kernel_mm *)bound;
    return status;
}
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct arch_mmu_page_table kernel;
static uint64_t memlock_limit=UINT64_MAX;
uint64_t kernel_task_current_memlock_limit(void) { return memlock_limit; }
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
void mm_test_fence(uint64_t address,int instructions) { (void)address;(void)instructions; }
static void opened(struct kernel_vfs_mount *mount,const char *name,unsigned mode,
                   struct kernel_open_file_description **file)
{int error;assert(!kernel_open_file_create_mode(&heap,mount,name,mode,file,&error)&&!error);}
static void execute(struct kernel_vfs_mount *mount)
{
    struct kernel_open_file_description *file=0;int error;
    assert(!kernel_open_file_create_executable(&heap,mount,"/file",&file,&error));
    assert(error==-KERNEL_EACCES && !file);
    struct kernel_vfs_path *root=0;assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(kernel_vfs_access_at(root,root,"/file",1,1)==-KERNEL_EACCES);
    assert(!kernel_vfs_access_at(root,root,"/",1,1));
    assert(!kernel_vfs_path_release(&root));
}
static void devices(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=0;struct kernel_vfs_file file={0};
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_mknod_at(root,root,"/device",KERNEL_VFS_S_IFCHR|0666,0x103));
    assert(kernel_vfs_open(mount,"/device",&file)==-KERNEL_EACCES);
    const struct kernel_vfs_open_request path={.flags=KERNEL_VFS_OPEN_PATH_ONLY|KERNEL_VFS_OPEN_FOLLOW};
    assert(!kernel_vfs_open_at(root,root,"/device",&path,&file,0));
    struct kernel_vfs_stat st;assert(!kernel_vfs_path_stat(file.path,&st)&&st.rdev==0x103);
    assert(!kernel_vfs_access_at(root,root,"/device",6,1));
    assert(!kernel_vfs_path_release(&file.path));
    assert(!kernel_vfs_path_release(&root));
}
static void mapping(struct kernel_vfs_mount *mount,struct kernel_open_file_description **controller)
{
    const uint64_t p=BOAROS_PAGE_SIZE,a=0x40000,d=0x100000;
    struct arch_mmu_user_space space={0};
    struct kernel_mm mm={0},child={0};uint64_t out;
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel)&&!kernel_mm_create(&mm,&space)&&!kernel_mm_vma_enable(&mm,&heap));
    assert(!kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&mm_test_context));
    assert(!kernel_vfs_ftruncate(&(*controller)->file,3*p));
    for(unsigned shared=0;shared<2;shared++) {
        struct kernel_open_file_description *owner=*controller;assert(!kernel_open_file_acquire(owner));
        assert(!kernel_mm_mmap_file_private(&mm,&owner,a+shared*8*p,3*p,0,KERNEL_MM_READ,
            KERNEL_MM_MAP_FIXED|(shared?KERNEL_MM_MAP_SHARED:0),&out)&&!owner);
    }
    assert(!kernel_open_file_release(controller));
    assert(kernel_vfs_unmount(mount)==-KERNEL_EBUSY);
    assert(kernel_mm_mprotect(&mm,a,p,KERNEL_MM_READ|KERNEL_MM_EXECUTE)==KERNEL_MM_STATUS_ACCESS);
    assert(!kernel_mm_mprotect(&mm,a+p,p,0));
    assert(!kernel_mm_fork(&child,&mm));
    assert(kernel_mm_mprotect(&child,a+p,p,KERNEL_MM_EXECUTE)==KERNEL_MM_STATUS_ACCESS);
    assert(kernel_mm_mprotect(&child,a+8*p,p,KERNEL_MM_EXECUTE)==KERNEL_MM_STATUS_ACCESS);
    assert(!kernel_mm_mremap(&mm,a+p,p,p,3,d,&out)&&out==d);
    assert(kernel_mm_mprotect(&mm,d,p,KERNEL_MM_EXECUTE)==KERNEL_MM_STATUS_ACCESS);
    assert(!kernel_mm_mprotect(&mm,d,p,KERNEL_MM_READ));
    unsigned char byte=3;size_t done;
    assert(!kernel_copy_from_user(&mm,&byte,d,1,&done)&&done==1&&!byte);
    mm_test_context=0;assert(!kernel_mm_release(&child)&&!kernel_mm_release(&mm));
}
static void mmap_result_at(uint64_t address,int64_t fd,uint64_t protections,uint64_t flags,int expected)
{
    struct kernel_syscall_request request={.arguments={address,BOAROS_PAGE_SIZE,protections,flags,(uint64_t)fd,0}};
    struct kernel_syscall_result result;
    assert(!syscall_handle_mmap(kernel_task_current(),&request,&result));
    if(result.value!=expected)fprintf(stderr,"mmap admission fd=%lld prot=%llu flags=%llu got=%lld expected=%d\n",
        (long long)fd,(unsigned long long)protections,(unsigned long long)flags,(long long)result.value,expected);
    assert(result.action==KERNEL_SYSCALL_ACTION_RETURN&&result.value==expected);
}
static void mmap_result(int64_t fd,uint64_t protections,uint64_t flags,int expected)
{ mmap_result_at(0,fd,protections,flags,expected); }
static void admission(struct kernel_open_file_description **controller,int locking)
{
    struct arch_mmu_user_space space={0};struct kernel_mm mm={0};struct kernel_files files={0};
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel)&&!kernel_mm_create(&mm,&space)&&!kernel_mm_vma_enable(&mm,&heap));
    assert(!kernel_mm_brk_initialize(&mm,0x10000,0x20000)&&!kernel_mm_context(&mm,&mm_test_context));
    assert(!kernel_files_create(&files,&heap));mm_test_bind(&mm);mm_test_bind_files(&files);
    if(!locking) {
        int64_t epoll;assert(!kernel_files_epoll_create1(&files,0,&epoll)&&epoll>=0);
        mmap_result(epoll,1,2,-KERNEL_ENODEV);
        mmap_result(epoll,2,1,-KERNEL_ENODEV);
    }
    /* 对同一真实普通文件，以打开模式区分EACCES与随后NOEXEC的EPERM。 */
    uint32_t fd;int error;assert(!kernel_files_find_free_fd(&files,&fd,&error)&&!error);
    struct kernel_vfs_mount *mount=(*controller)->file.mount;
    struct kernel_open_file_description *file=*controller;*controller=0;
    file->open_flags=1;assert(!kernel_files_install_new_owned_at(&files,fd,0,&file));
    if(!locking)mmap_result(fd,4,2,-KERNEL_EACCES);
    assert(!kernel_open_file_create(&heap,mount,"/file",&file,&error)&&!error);
    assert(!kernel_files_find_free_fd(&files,&fd,&error)&&!error);
    assert(!kernel_files_install_new_owned_at(&files,fd,0,&file));
    if(!locking) {
        mmap_result(fd,2|4,1,-KERNEL_EACCES);
        mmap_result(fd,4,2,-KERNEL_EPERM);
    } else {
        uint64_t address=0x40000,out;unsigned char byte=91;size_t done;
        assert(!kernel_mm_mmap_anonymous(&mm,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&out));
        assert(!kernel_copy_to_user(&mm,address,&byte,1,&done)&&done==1);
        struct kernel_heap_statistics before,after;kernel_heap_get_statistics(&heap,&before);
        struct kernel_mm_proc_memory original,current;assert(!kernel_mm_proc_memory_snapshot(&mm,&original));
        uint64_t available=physical_page_available(&pages);memlock_limit=1;
        mmap_result(fd,4,2|0x2000,-KERNEL_EAGAIN);
        mmap_result_at(address,fd,4,2|0x2000|0x10,-KERNEL_EAGAIN);
        mmap_result_at(address,fd,4,2|0x2000|0x100000,-KERNEL_EEXIST);
        mmap_result(fd,2|4,1|0x2000,-KERNEL_EAGAIN);
        mmap_result(-1,4,2|0x2000,-KERNEL_EBADF);
        memlock_limit=UINT64_MAX;byte=0;
        assert(!kernel_copy_from_user(&mm,&byte,address,1,&done)&&done==1&&byte==91);
        assert(!kernel_mm_proc_memory_snapshot(&mm,&current)&&original.virtual_bytes==current.virtual_bytes&&
            original.locked_bytes==current.locked_bytes&&original.resident_pages==current.resident_pages);
        kernel_heap_get_statistics(&heap,&after);
        assert(before.allocation_calls==after.allocation_calls&&before.live_allocations==after.live_allocations&&
            before.current_pages==after.current_pages&&physical_page_available(&pages)==available);
    }
    mm_test_bind_files(0);mm_test_bind(0);
    assert(!kernel_files_release(&files));
    mm_test_context=0;assert(!kernel_mm_release(&mm));
}
static int conversion_error(struct kernel_vfs_mount *mount,int read_only)
{ (void)mount;(void)read_only;return -KERNEL_EIO; }
static void reconfiguration(struct kernel_vfs_mount *mount,struct kernel_open_file_description *file)
{
    struct kernel_vfs_path *root=0;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_file_acquire_write(&file->file));
    assert(kernel_vfs_remount(root,1,NULL)==-KERNEL_EBUSY);
    assert(!kernel_vfs_mount_is_readonly(mount)&&mount->nodev&&mount->noexec);
    /* The same lifetime owner is retained by any dup/fork/mapping. */
    assert(!kernel_open_file_acquire(file));
    struct kernel_open_file_description *alias=file;
    assert(!kernel_open_file_release(&alias));
    assert(kernel_vfs_remount(root,1,NULL)==-KERNEL_EBUSY);
    /* Close the only writer before using read-only and metadata operations. */
    assert(!kernel_open_file_release(&file));
    assert(!kernel_vfs_remount(root,1|2,NULL));
    assert(kernel_vfs_mount_is_readonly(mount)&&mount->nosuid&&!mount->nodev&&!mount->noexec);
    struct kernel_vfs_file denied={0};
    assert(kernel_vfs_create(mount,"/denied",0600,&denied)==-KERNEL_EROFS);
    struct kernel_vfs_file read_only={0};struct kernel_vfs_stat before,after;
    assert(!kernel_vfs_open(mount,"/file",&read_only) && !kernel_vfs_fstat(&read_only,&before));
    kernel_vfs_file_accessed(&read_only);
    assert(!kernel_vfs_fstat(&read_only,&after));
    assert(before.atime.seconds==after.atime.seconds && before.atime.nanoseconds==after.atime.nanoseconds);
    assert(!kernel_vfs_close(&read_only));
    assert(!kernel_vfs_remount(root,0,NULL));
    struct kernel_vfs_instance *instance=mount->private_data;
    struct kernel_vfs_backend failed=*instance->ops;const struct kernel_vfs_backend *original=instance->ops;
    failed.remount=conversion_error;instance->ops=&failed;
    assert(kernel_vfs_remount(root,1|2|4|8,NULL)==-KERNEL_EIO);
    assert(!instance->read_only&&!mount->nosuid&&!mount->nodev&&!mount->noexec);
    instance->ops=original;
    assert(!kernel_vfs_create(mount,"/after-failure",0600,&denied));
    assert(!kernel_vfs_close(&denied));
    assert(!kernel_vfs_path_release(&root));
}
static struct kernel_vfs_file *racing_file;
static unsigned remount_entered;
extern unsigned mm_test_parked(unsigned);
static int pause_conversion(struct kernel_vfs_mount *mount,int read_only)
{
    (void)mount;assert(read_only);
    __atomic_store_n(&remount_entered,1,__ATOMIC_RELEASE);
    while (!mm_test_parked(1)) sched_yield();
    return 0;
}
static void *new_writer(void *unused)
{
    (void)unused;mm_test_attach(1);
    while (!__atomic_load_n(&remount_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(kernel_vfs_file_acquire_write(racing_file)==-KERNEL_EROFS);
    assert(!racing_file->write_lease);
    return NULL;
}
static void reconfiguration_race(struct kernel_vfs_mount *mount,struct kernel_vfs_file *file)
{
    struct kernel_vfs_instance *instance=mount->private_data;
    struct kernel_vfs_backend paused=*instance->ops;const struct kernel_vfs_backend *original=instance->ops;
    paused.remount=pause_conversion;instance->ops=&paused;racing_file=file;
    struct kernel_vfs_path *root=0;assert(!kernel_vfs_path_root(mount,&heap,&root));
    pthread_t actor;assert(!pthread_create(&actor,NULL,new_writer,NULL));
    assert(!kernel_vfs_remount(root,1,NULL));
    assert(!pthread_join(actor,NULL));instance->ops=original;
    assert(!kernel_vfs_remount(root,0,NULL));
    assert(!kernel_vfs_path_release(&root));
}
static unsigned sync_entered,sync_calls;
static int pause_filesystem_sync(struct kernel_vfs_instance *instance,struct kernel_vfs_node *root)
{
    (void)instance;(void)root;
    if (__atomic_fetch_add(&sync_calls,1,__ATOMIC_RELAXED)) return 0;
    __atomic_store_n(&sync_entered,1,__ATOMIC_RELEASE);
    while (!mm_test_parked(0)) sched_yield();
    return 0;
}
static void *mount_sync(void *argument)
{
    mm_test_attach(1);
    assert(!kernel_vfs_sync_mount(argument,NULL));
    return NULL;
}
static void reconfiguration_sync(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_instance *instance=mount->private_data;
    struct kernel_vfs_backend paused=*instance->ops;const struct kernel_vfs_backend *original=instance->ops;
    paused.sync_filesystem=pause_filesystem_sync;instance->ops=&paused;
    struct kernel_vfs_path *root=0;assert(!kernel_vfs_path_root(mount,&heap,&root));
    pthread_t actor;assert(!pthread_create(&actor,NULL,mount_sync,mount));
    while (!__atomic_load_n(&sync_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_vfs_remount(root,1,NULL));
    assert(!pthread_join(actor,NULL) && sync_calls==2);
    instance->ops=original;
    assert(!kernel_vfs_remount(root,0,NULL));
    assert(!kernel_vfs_path_release(&root));
}
static void mount_views(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=0,*source=0,*covered=0,*bound=0,*held=0,*resolved=0;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"/source",0755));
    assert(!kernel_vfs_mkdir_at(root,root,"/source/child",0755));
    assert(!kernel_vfs_mkdir_at(root,root,"/bind",0755));
    assert(!kernel_vfs_path_resolve(root,root,"/source",1,&source));
    assert(!kernel_vfs_path_resolve(root,root,"/bind",1,&covered));
    int result=kernel_vfs_bind_mount(source,covered,0);
    assert(!result);
    assert(!kernel_vfs_path_resolve(root,root,"/bind",1,&bound));
    assert(kernel_vfs_path_mount(bound)!=mount);
    assert(!kernel_vfs_path_resolve(root,root,"/bind/child",1,&held));
    assert(!kernel_vfs_rename_at(root,root,root,"/source/child","/source/renamed",0));
    char text[128];assert(!kernel_vfs_path_string(held,root,text,sizeof(text))&&!strcmp(text,"/bind/renamed"));
    assert(!kernel_vfs_path_resolve(root,root,"/bind/renamed",1,&resolved));
    assert(kernel_vfs_path_inode(resolved)==kernel_vfs_path_inode(held));
    assert(!kernel_vfs_path_release(&resolved));
    struct kernel_vfs_mount *nested=NULL;
    struct kernel_vfs_path *point=NULL;
    assert(!kernel_tmpfs_create(&heap,0,NULL,&nested));
    assert(!kernel_vfs_path_resolve(root,root,"/source/renamed",1,&point));
    assert(!kernel_vfs_mount_attach(nested,point)&&!kernel_vfs_path_release(&point));
    assert(kernel_vfs_rename_at(root,root,root,"/bind/renamed","/bind/moved",0)==-KERNEL_EBUSY);
    assert(kernel_vfs_unlink_at(root,root,"/bind/renamed",1)==-KERNEL_EBUSY);
    assert(!kernel_vfs_path_resolve(root,root,"/source/renamed",1,&point));
    assert(!kernel_vfs_umount(point,0)&&!kernel_vfs_path_release(&point));
    assert(!kernel_vfs_remount(bound,4096U|1U,NULL));
    assert(kernel_vfs_mkdir_at(bound,root,"readonly",0700)==-KERNEL_EROFS);
    assert(!kernel_vfs_mkdir_at(source,root,"writable",0700));
    assert(kernel_vfs_umount(bound,0)==-KERNEL_EBUSY);
    assert(!kernel_vfs_umount(bound,2));
    struct kernel_vfs_mount *unattached=NULL;
    assert(!kernel_tmpfs_create(&heap,0,NULL,&unattached));
    assert(kernel_vfs_mount_attach(unattached,held)==-KERNEL_EINVAL&&!kernel_vfs_unmount(unattached));
    assert(kernel_vfs_path_resolve(root,root,"/bind/renamed",1,&resolved)==-KERNEL_ENOENT);
    assert(!kernel_vfs_path_resolve(held,root,".",1,&resolved));
    assert(!kernel_vfs_path_release(&resolved)&&!kernel_vfs_path_release(&held));
    assert(!kernel_vfs_path_release(&bound)&&!kernel_vfs_path_release(&source)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_release(&root));
}
void kernel_page_cache_put_batch(struct kernel_page_cache *cache,struct kernel_page_cache_read_page *pages,size_t count)
{ (void)cache;(void)pages;(void)count;__builtin_trap(); }
static _Thread_local unsigned delayed_constructor;
static unsigned constructor_entered,constructor_release,constructor_done,publish_during_snapshot;
static unsigned publish_sync_calls[3];
static struct kernel_vfs_backend delayed_observer;
static int delayed_sync(struct kernel_vfs_instance *instance,struct kernel_vfs_node *root)
{ (void)instance;(void)root;publish_sync_calls[2]++;return 0; }
extern void __real_kernel_vfs_mount_initialize(struct kernel_vfs_mount *);
void __wrap_kernel_vfs_mount_initialize(struct kernel_vfs_mount *mount)
{
    if (delayed_constructor) {
        delayed_constructor=0;
        struct kernel_vfs_instance *instance=mount->private_data;
        delayed_observer=*instance->ops;delayed_observer.sync_filesystem=delayed_sync;instance->ops=&delayed_observer;
        __atomic_store_n(&constructor_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&constructor_release,__ATOMIC_ACQUIRE)) sched_yield();
    }
    __real_kernel_vfs_mount_initialize(mount);
}
static unsigned allocation_fail_at,allocation_calls,plain_allocation_fail;
extern enum kernel_heap_status __real_kernel_heap_allocate(struct kernel_heap *,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate(struct kernel_heap *heap,size_t size,void **owner)
{
    if (plain_allocation_fail) {plain_allocation_fail=0;return KERNEL_HEAP_STATUS_EMPTY;}
    if (publish_during_snapshot) {
        publish_during_snapshot=0;__atomic_store_n(&constructor_release,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&constructor_done,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_heap_allocate(heap,size,owner);
}
extern enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{
    if (allocation_fail_at && ++allocation_calls==allocation_fail_at) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(h,n,size,out);
}
static void mount_view_tree(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=0,*source=0,*nested=0,*covered=0,*path=0,*copy=0,*tree=0,*held=0;
    struct kernel_vfs_mount *first=0,*second=0;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    const char *directories[]={"/instance","/copy","/tree","/failed"};
    for (unsigned i=0;i<4;i++) assert(!kernel_vfs_mkdir_at(root,root,directories[i],0755));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&first));
    assert(!kernel_vfs_path_resolve(root,root,"/instance",1,&covered));
    assert(!kernel_vfs_mount_attach(first,covered)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"/instance",1,&source));
    assert(!kernel_vfs_mkdir_at(source,root,"child",0755));
    struct kernel_vfs_file file={0};
    assert(!kernel_vfs_create_at(source,root,"data",0644,&file));
    size_t done=0;assert(!kernel_vfs_pwrite(&file,0,"ok",2,&done)&&done==2);
    assert(!kernel_vfs_close(&file));
    assert(!kernel_vfs_create_at(root,root,"/single",0644,&file)&&!kernel_vfs_close(&file));
    assert(!kernel_vfs_path_resolve(source,root,"data",1,&path));
    assert(!kernel_vfs_path_resolve(root,root,"/single",1,&covered));
    assert(!kernel_vfs_bind_mount(path,covered,0));
    struct kernel_vfs_stat original,bound;
    assert(!kernel_vfs_path_stat(path,&original));
    assert(!kernel_vfs_stat_at(root,root,"/single",1,&bound)&&original.dev==bound.dev&&original.ino==bound.ino);
    assert(kernel_vfs_path_resolve(root,root,"/single/",1,&held)==-KERNEL_ENOTDIR);
    assert(!kernel_vfs_path_release(&path)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&second));
    assert(!kernel_vfs_path_resolve(source,root,"child",1,&covered));
    assert(!kernel_vfs_mount_attach(second,covered)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(source,root,"child",1,&nested));
    assert(!kernel_vfs_create_at(nested,root,"marker",0644,&file)&&!kernel_vfs_close(&file));
    assert(!kernel_vfs_path_resolve(root,root,"/copy",1,&covered));
    assert(!kernel_vfs_bind_mount(source,covered,0)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"/copy",1,&copy));
    assert(kernel_vfs_path_resolve(copy,root,"child/marker",1,&path)==-KERNEL_ENOENT);
    assert(!kernel_vfs_path_resolve(root,root,"/failed",1,&covered));
    struct kernel_heap_statistics before,after;
    kernel_heap_get_statistics(&heap,&before);
    allocation_calls=0;allocation_fail_at=4;
    assert(kernel_vfs_bind_mount(source,covered,1)==-KERNEL_ENOMEM&&allocation_calls==4);
    allocation_fail_at=0;kernel_heap_get_statistics(&heap,&after);
    assert(before.live_allocations==after.live_allocations);
    assert(!covered->mounted_here&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"/tree",1,&covered));
    assert(!kernel_vfs_bind_mount(source,covered,1)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"/tree",1,&tree));
    assert(!kernel_vfs_path_resolve(tree,root,"child/marker",1,&held));
    assert(!kernel_vfs_umount(nested,0)&&!kernel_vfs_path_release(&nested));
    assert(!kernel_vfs_umount(source,0)&&!kernel_vfs_path_release(&source));
    assert(!kernel_vfs_path_resolve(copy,root,"data",1,&path));
    assert(!kernel_vfs_path_stat(path,&bound)&&original.dev==bound.dev&&original.ino==bound.ino);
    assert(!kernel_vfs_path_release(&path));
    assert(!kernel_vfs_umount(tree,2)&&!kernel_vfs_path_release(&tree));
    assert(!kernel_vfs_path_open(held,&file)&&!kernel_vfs_close(&file));
    assert(!kernel_vfs_path_release(&held));
    assert(!kernel_vfs_umount(copy,0)&&!kernel_vfs_path_release(&copy));
    assert(!kernel_vfs_path_resolve(root,root,"/single",1,&path));
    assert(!kernel_vfs_path_open(path,&file));
    assert(!kernel_vfs_umount(path,2)&&!kernel_vfs_path_release(&path));
    char bytes[3]={0};int miss=0;
    assert(!kernel_vfs_pread(&file,0,bytes,2,&done,&miss)&&done==2&&!strcmp(bytes,"ok"));
    assert(!kernel_vfs_close(&file)&&!kernel_vfs_path_release(&root));
}
static const struct kernel_vfs_backend *cleanup_original;
static unsigned cleanup_attempts;
static int failing_view_unmount(struct kernel_vfs_mount *mount)
{
    cleanup_attempts++;
    if (cleanup_attempts==1) return -KERNEL_EIO;
    return cleanup_original->unmount(mount);
}
static void mount_cleanup_failure(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=0,*covered=0,*child=0;
    struct kernel_vfs_mount *memory=0;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"/cleanup",0755));
    assert(!kernel_vfs_path_resolve(root,root,"/cleanup",1,&covered));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&memory));
    struct kernel_vfs_instance *instance=memory->private_data;
    struct kernel_vfs_backend failing=*instance->ops;cleanup_original=instance->ops;
    failing.unmount=failing_view_unmount;instance->ops=&failing;
    assert(!kernel_vfs_mount_attach(memory,covered));
    assert(!kernel_vfs_path_resolve(root,root,"/cleanup",1,&child));
    assert(!kernel_vfs_umount(child,2)&&!kernel_vfs_path_release(&child));
    assert(kernel_vfs_reap_mounts()==-KERNEL_EIO&&cleanup_attempts==1);
    assert(!kernel_vfs_reap_mounts()&&cleanup_attempts==1);
    assert(!kernel_vfs_cleanup_pending()&&cleanup_attempts==2);
    /* A synchronous caller keeps only the actual failed cleanup handle. */
    memory=NULL;
    assert(!kernel_tmpfs_create(&heap,0,NULL,&memory));
    instance=memory->private_data;instance->ops=&failing;cleanup_attempts=0;
    assert(kernel_vfs_unmount(memory)==-KERNEL_EIO&&memory->private_data&&memory->retired);
    assert(!kernel_vfs_unmount(memory)&&cleanup_attempts==2);
    assert(!kernel_vfs_path_release(&covered)&&!kernel_vfs_path_release(&root));
}
static unsigned detached_sync_calls,visible_sync_calls;
static int detached_sync(struct kernel_vfs_instance *instance,struct kernel_vfs_node *root)
{ (void)instance;(void)root;detached_sync_calls++;return 0; }
static int visible_sync(struct kernel_vfs_instance *instance,struct kernel_vfs_node *root)
{ (void)instance;(void)root;visible_sync_calls++;return 0; }
static void mount_detached_sync(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*covered=NULL,*child=NULL,*alias=NULL;
    struct kernel_vfs_mount *memory=NULL;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    struct kernel_vfs_instance *visible=mount->private_data;
    const struct kernel_vfs_backend *original=visible->ops;
    struct kernel_vfs_backend visible_observer=*original;
    visible_observer.sync_filesystem=visible_sync;visible->ops=&visible_observer;
    assert(!kernel_vfs_mkdir_at(root,root,"sync-alias",0755));
    assert(!kernel_vfs_path_resolve(root,root,"sync-alias",1,&covered));
    assert(!kernel_vfs_bind_mount(root,covered,0)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"sync-alias",1,&alias));
    assert(!kernel_vfs_mkdir_at(root,root,"detached-sync",0755));
    assert(!kernel_vfs_path_resolve(root,root,"detached-sync",1,&covered));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&memory));
    struct kernel_vfs_instance *instance=memory->private_data;
    struct kernel_vfs_backend observed=*instance->ops;
    observed.sync_filesystem=detached_sync;instance->ops=&observed;
    assert(!kernel_vfs_mount_attach(memory,covered));
    assert(!kernel_vfs_path_resolve(root,root,"detached-sync",1,&child));
    assert(!kernel_vfs_umount(child,2));
    struct kernel_vfs_file file={0};size_t done;
    assert(!kernel_vfs_create_at(child,root,"after-detach",0600,&file));
    assert(!kernel_vfs_pwrite(&file,0,"sync",4,&done)&&done==4);
    kernel_vfs_sync_all(root);assert(detached_sync_calls==1&&visible_sync_calls==1);
    detached_sync_calls=visible_sync_calls=0;plain_allocation_fail=1;
    kernel_vfs_sync_all(root);assert(!plain_allocation_fail&&detached_sync_calls==1&&visible_sync_calls==1);
    assert(!kernel_vfs_close(&file)&&!kernel_vfs_path_release(&child)&&!kernel_vfs_reap_mounts());
    assert(!kernel_vfs_umount(alias,0)&&!kernel_vfs_path_release(&alias));
    assert(!kernel_vfs_path_release(&covered)&&!kernel_vfs_path_release(&root));visible->ops=original;
}

static void *publish_constructor(void *argument)
{
    mm_test_attach(1);delayed_constructor=1;
    assert(!kernel_tmpfs_create(&heap,0,NULL,argument));
    __atomic_store_n(&constructor_done,1,__ATOMIC_RELEASE);return NULL;
}
static int publish_root_sync(struct kernel_vfs_instance *instance,struct kernel_vfs_node *root)
{ (void)instance;(void)root;publish_sync_calls[0]++;return 0; }
static int publish_limit_sync(struct kernel_vfs_instance *instance,struct kernel_vfs_node *root)
{ (void)instance;(void)root;publish_sync_calls[1]++;return 0; }
static void mount_sync_publication(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_mount *late=NULL,*limit=NULL;
    pthread_t actor;assert(!pthread_create(&actor,NULL,publish_constructor,&late));
    while (!__atomic_load_n(&constructor_entered,__ATOMIC_ACQUIRE)) sched_yield();
    /* late 已取得较早 id，但还未发布；limit 使该 id 位于本次 sync 上界内。 */
    assert(!kernel_tmpfs_create(&heap,0,NULL,&limit));
    struct kernel_vfs_instance *visible=mount->private_data,*last=limit->private_data;
    const struct kernel_vfs_backend *original=visible->ops;
    struct kernel_vfs_backend root_observer=*original,limit_observer=*last->ops;
    root_observer.sync_filesystem=publish_root_sync;limit_observer.sync_filesystem=publish_limit_sync;
    visible->ops=&root_observer;last->ops=&limit_observer;
    struct kernel_vfs_path *root=NULL;assert(!kernel_vfs_path_root(mount,&heap,&root));
    publish_during_snapshot=1;kernel_vfs_sync_all(root);
    assert(!pthread_join(actor,NULL)&&!publish_during_snapshot);
    assert(publish_sync_calls[0]==1&&publish_sync_calls[1]==1&&publish_sync_calls[2]==1);
    assert(!kernel_vfs_unmount(late)&&!kernel_vfs_unmount(limit));
    assert(!kernel_vfs_path_release(&root));visible->ops=original;
}
static void mount_deep_views(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*covered=NULL,*source=NULL,*cursor=NULL,*target=NULL;
    struct kernel_vfs_mount *memory=NULL;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"deep-source",0755)&&!kernel_vfs_mkdir_at(root,root,"deep-target",0755));
    assert(!kernel_vfs_path_resolve(root,root,"deep-source",1,&covered));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&memory)&&!kernel_vfs_mount_attach(memory,covered));
    assert(!kernel_vfs_path_release(&covered)&&!kernel_vfs_path_root(memory,&heap,&source));
    cursor=source;assert(!kernel_vfs_path_acquire(cursor));
    for (unsigned depth=0;depth<64;depth++) {
        struct kernel_vfs_mount *next=NULL;struct kernel_vfs_path *next_root=NULL;
        assert(!kernel_tmpfs_create(&heap,0,NULL,&next)&&!kernel_vfs_mount_attach(next,cursor));
        assert(!kernel_vfs_path_root(next,&heap,&next_root)&&!kernel_vfs_path_release(&cursor));cursor=next_root;
    }
    assert(!kernel_vfs_path_release(&cursor));
    assert(!kernel_vfs_path_resolve(root,root,"deep-target",1,&target));
    struct kernel_heap_statistics before,after;kernel_heap_get_statistics(&heap,&before);
    allocation_calls=0;allocation_fail_at=32;
    assert(kernel_vfs_bind_mount(source,target,1)==-KERNEL_ENOMEM&&allocation_calls==32);
    allocation_fail_at=0;kernel_heap_get_statistics(&heap,&after);
    assert(before.live_allocations==after.live_allocations&&!target->mounted_here);
    assert(!kernel_vfs_bind_mount(source,target,1));
    struct kernel_vfs_path *copy=target->mounted_here->root_path;
    assert(!kernel_vfs_path_acquire(copy)&&!kernel_vfs_umount(copy,2)&&!kernel_vfs_path_release(&copy));
    assert(!kernel_vfs_umount(source,2)&&!kernel_vfs_path_release(&source)&&!kernel_vfs_reap_mounts());
    assert(!kernel_vfs_path_release(&target)&&!kernel_vfs_path_release(&root));
}
static struct kernel_open_file_description *chain_backing;
static const struct kernel_vfs_backend *chain_original;
static unsigned chain_released[2];
static int chain_parent_unmount(struct kernel_vfs_mount *mount)
{
    chain_released[0]++;
    assert(!kernel_open_file_release(&chain_backing)&&!chain_backing);
    return chain_original->unmount(mount);
}
static int chain_backing_unmount(struct kernel_vfs_mount *mount)
{ chain_released[1]++;return chain_original->unmount(mount); }
static void mount_cleanup_chain(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*covered=NULL,*a=NULL,*b=NULL;
    struct kernel_vfs_mount *first=NULL,*second=NULL;
    assert(!kernel_vfs_path_root(mount,&heap,&root));
    assert(!kernel_vfs_mkdir_at(root,root,"chain-a",0755)&&!kernel_vfs_mkdir_at(root,root,"chain-b",0755));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&first)&&!kernel_tmpfs_create(&heap,0,NULL,&second));
    struct kernel_vfs_instance *ai=first->private_data,*bi=second->private_data;
    chain_original=ai->ops;
    struct kernel_vfs_backend ao=*chain_original,bo=*chain_original;
    ao.unmount=chain_parent_unmount;bo.unmount=chain_backing_unmount;ai->ops=&ao;bi->ops=&bo;
    assert(!kernel_vfs_path_resolve(root,root,"chain-a",1,&covered));
    assert(!kernel_vfs_mount_attach(first,covered)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"chain-b",1,&covered));
    assert(!kernel_vfs_mount_attach(second,covered)&&!kernel_vfs_path_release(&covered));
    assert(!kernel_vfs_path_resolve(root,root,"chain-a",1,&a)&&!kernel_vfs_path_resolve(root,root,"chain-b",1,&b));
    opened(second,"backing",0600,&chain_backing);
    assert(!kernel_vfs_umount(b,2)&&!kernel_vfs_path_release(&b));
    assert(!kernel_vfs_umount(a,2)&&!kernel_vfs_path_release(&a));
    assert(!kernel_vfs_reap_mounts()&&chain_released[0]==1&&chain_released[1]==1&&!chain_backing);
    assert(!kernel_vfs_path_release(&root));
}
static unsigned sync_owner_entered,sync_owner_done;
static int sync_owner_result;
static int snapshot_owner_sync(struct kernel_vfs_instance *instance,struct kernel_vfs_node *node)
{
    (void)instance;(void)node;__atomic_store_n(&sync_owner_entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&sync_owner_done,__ATOMIC_ACQUIRE)) sched_yield();
    return 0;
}
static void *snapshot_unmount(void *argument)
{
    mm_test_attach(1);
    struct kernel_vfs_path *root=argument,*child=NULL;
    while (!__atomic_load_n(&sync_owner_entered,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_vfs_path_resolve(root,root,"snapshot-owner",1,&child));
    sync_owner_result=kernel_vfs_umount(child,0);
    assert(!kernel_vfs_path_release(&child));__atomic_store_n(&sync_owner_done,1,__ATOMIC_RELEASE);return NULL;
}
static void mount_sync_owner(struct kernel_vfs_mount *mount)
{
    struct kernel_vfs_path *root=NULL,*covered=NULL,*child=NULL;
    struct kernel_vfs_mount *memory=NULL;
    assert(!kernel_vfs_path_root(mount,&heap,&root)&&!kernel_vfs_mkdir_at(root,root,"snapshot-owner",0755));
    assert(!kernel_vfs_path_resolve(root,root,"snapshot-owner",1,&covered));
    assert(!kernel_tmpfs_create(&heap,0,NULL,&memory)&&!kernel_vfs_mount_attach(memory,covered));
    assert(!kernel_vfs_path_release(&covered));
    struct kernel_vfs_instance *instance=mount->private_data;
    const struct kernel_vfs_backend *original=instance->ops;
    struct kernel_vfs_backend observer=*original;observer.sync_filesystem=snapshot_owner_sync;instance->ops=&observer;
    pthread_t actor;assert(!pthread_create(&actor,NULL,snapshot_unmount,root));
    kernel_vfs_sync_all(root);
    assert(!pthread_join(actor,NULL)&&sync_owner_result==-KERNEL_EBUSY);instance->ops=original;
    assert(!kernel_vfs_path_resolve(root,root,"snapshot-owner",1,&child));
    assert(!kernel_vfs_umount(child,0)&&!kernel_vfs_path_release(&child)&&!kernel_vfs_reap_mounts());
    assert(!kernel_vfs_path_release(&root));
}
int main(int argc,char **argv)
{
    assert(argc==2);mm_test_attach(0);
    struct boot_memory_layout layout={0};layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout)&&!physical_page_allocator_bind_access(&pages,access)&&!physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages);
    struct kernel_vfs_mount *mount=0;struct kernel_open_file_description *file=0;
    assert(!kernel_tmpfs_create(&heap,0,"nr_blocks=8",&mount));mount->noexec=mount->nodev=1;
    opened(mount,"/file",0755,&file);file->open_flags=2;
    if(!strcmp(argv[1],"views"))mount_views(mount);
    else if(!strcmp(argv[1],"view-tree"))mount_view_tree(mount);
    else if(!strcmp(argv[1],"view-cleanup"))mount_cleanup_failure(mount);
    else if(!strcmp(argv[1],"view-sync"))mount_detached_sync(mount);
    else if(!strcmp(argv[1],"view-sync-publish"))mount_sync_publication(mount);
    else if(!strcmp(argv[1],"view-deep"))mount_deep_views(mount);
    else if(!strcmp(argv[1],"view-chain"))mount_cleanup_chain(mount);
    else if(!strcmp(argv[1],"view-sync-owner"))mount_sync_owner(mount);
    else if(!strcmp(argv[1],"remount")){reconfiguration(mount,file);file=0;}
    else if(!strcmp(argv[1],"remount-race"))reconfiguration_race(mount,&file->file);
    else if(!strcmp(argv[1],"remount-sync"))reconfiguration_sync(mount);
    else if(!strcmp(argv[1],"execute"))execute(mount);
    else if(!strcmp(argv[1],"devices"))devices(mount);
    else if(!strcmp(argv[1],"mapping"))mapping(mount,&file);
    else {assert(!strcmp(argv[1],"admission")||!strcmp(argv[1],"locking"));admission(&file,!strcmp(argv[1],"locking"));}
    if(file)assert(!kernel_open_file_release(&file));
    assert(!kernel_vfs_unmount(mount));
    assert(!kernel_vfs_reap_mounts());
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations&&!stats.current_pages&&physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));printf("mount attributes: %s and final owners PASS\n",argv[1]);
}
