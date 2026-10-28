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
    if(!strcmp(argv[1],"remount")){reconfiguration(mount,file);file=0;}
    else if(!strcmp(argv[1],"remount-race"))reconfiguration_race(mount,&file->file);
    else if(!strcmp(argv[1],"remount-sync"))reconfiguration_sync(mount);
    else if(!strcmp(argv[1],"execute"))execute(mount);
    else if(!strcmp(argv[1],"devices"))devices(mount);
    else if(!strcmp(argv[1],"mapping"))mapping(mount,&file);
    else {assert(!strcmp(argv[1],"admission")||!strcmp(argv[1],"locking"));admission(&file,!strcmp(argv[1],"locking"));}
    if(file)assert(!kernel_open_file_release(&file));
    assert(!kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations&&!stats.current_pages&&physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));printf("mount attributes: %s and final owners PASS\n",argv[1]);
}
