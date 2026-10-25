#include <kernel/mm_backend.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include "../../fs/open_file_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct arch_mmu_page_table kernel;
void kernel_pidfd_close(void *file) { (void)file; __builtin_trap(); }
int kernel_socket_is_unix(const void *socket) { (void)socket; __builtin_trap(); }
void kernel_socket_rights_changed(void) { __builtin_trap(); }
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
    if(!strcmp(argv[1],"execute"))execute(mount);
    else if(!strcmp(argv[1],"devices"))devices(mount);
    else {assert(!strcmp(argv[1],"mapping"));mapping(mount,&file);}
    if(file)assert(!kernel_open_file_release(&file));
    assert(!kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations&&!stats.current_pages&&physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));printf("mount attributes: %s and final owners PASS\n",argv[1]);
}
