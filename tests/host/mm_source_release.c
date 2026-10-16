#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>
#include <kernel/file_mapping.h>
#include <kernel/errno.h>
#include "../../fs/open_file_internal.h"
#include "../../kernel/sched/private.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

void mm_test_attach(unsigned);
void mm_test_bind(struct kernel_mm *);
unsigned mm_test_parked(unsigned);
extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[4*1024*1024] __attribute__((aligned(4096)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_mm mm;
static struct kernel_vfs_mount *mount;
static struct kernel_open_file_description *watched, *callback_file;
static uint64_t context;
static unsigned last_releases, pause_callback, callback_after_fence, callback_entered, callback_resume, unmap_done;
static unsigned fail_close;
static void publish(unsigned *flag) { __atomic_store_n(flag,1,__ATOMIC_RELEASE); }
static void await(unsigned *flag) { while (!__atomic_load_n(flag,__ATOMIC_ACQUIRE)) sched_yield(); }
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int address_of(const void *p,uint64_t *address) { *address=(uintptr_t)p; return 1; }
static void outside_gate(void)
{
    for (struct kernel_lock_guard *g=kernel_io_context_current()->locks;g;g=g->previous)
        assert(g->lock->rank!=25);
}
enum kernel_open_file_status __real_kernel_open_file_release(struct kernel_open_file_description **);
enum kernel_open_file_status __wrap_kernel_open_file_release(struct kernel_open_file_description **owner)
{
    int last=watched && *owner==watched && __atomic_load_n(&watched->references,__ATOMIC_ACQUIRE)<=1;
    if (last) outside_gate();
    enum kernel_open_file_status status=__real_kernel_open_file_release(owner);
    if (last && !status) { last_releases++; watched=NULL; }
    return status;
}
int __real_kernel_vfs_close(struct kernel_vfs_file *);
int __wrap_kernel_vfs_close(struct kernel_vfs_file *file)
{
    if (watched && file==&watched->file && __atomic_exchange_n(&fail_close,0,__ATOMIC_ACQ_REL))
        return -KERNEL_EIO;
    return __real_kernel_vfs_close(file);
}
void __real_kernel_file_mapping_drain(struct kernel_file_mapping *);
void __wrap_kernel_file_mapping_drain(struct kernel_file_mapping *mapping)
{ outside_gate(); __real_kernel_file_mapping_drain(mapping); }
enum kernel_heap_status __real_kernel_heap_release(struct kernel_heap *,void *);
enum kernel_heap_status __wrap_kernel_heap_release(struct kernel_heap *h,void *pointer)
{
    if (kernel_cpu_current()->hardware_id==5 &&
        __atomic_exchange_n(&callback_after_fence,0,__ATOMIC_ACQ_REL)) {
        /* 此resident已摘索引；callback尚未返回，registration仍被实际借用。 */
        publish(&callback_entered); await(&callback_resume);
    }
    return __real_kernel_heap_release(h,pointer);
}
void kernel_assert_service_context(void) { assert(!kernel_cpu_current()->logical_id); }
void kernel_cpu_flush_notifications(void) {}
void kernel_cpu_notify(struct kernel_cpu *cpu,unsigned events) { (void)cpu; (void)events; abort(); }
void mm_test_fence(uint64_t address,int instructions)
{ (void)address; (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
void riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{
    (void)address; (void)size; (void)instructions; assert(!targets && !kernel_cpu_current()->raw_locks);
    if (kernel_cpu_current()->hardware_id==5 && __atomic_exchange_n(&pause_callback,0,__ATOMIC_ACQ_REL)) {
        publish(&callback_after_fence);
    }
}
static struct kernel_open_file_description *map_file(const char *path,uint64_t address)
{
    struct kernel_open_file_description *file=NULL,*source;
    int error; uint64_t mapped;
    assert(!kernel_open_file_create_mode(&heap,mount,path,0600,&file,&error) && !error);
    file->open_flags=2; assert(!kernel_vfs_ftruncate(&file->file,BOAROS_PAGE_SIZE));
    source=file; assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,address,BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&mapped));
    assert(!source && mapped==address);
    return file;
}
static void close_caller(struct kernel_open_file_description **file)
{ watched=*file; assert(!kernel_open_file_release(file) && !*file && watched->references==1); }
static void *truncate_callback(void *unused)
{
    (void)unused; mm_test_attach(5); kernel_cpu_current()->logical_id=0; mm_test_context=context;
    assert(!kernel_vfs_ftruncate(&callback_file->file,0)); return NULL;
}
static void *unmap_callback(void *unused)
{
    (void)unused; mm_test_attach(6); kernel_cpu_current()->logical_id=0; mm_test_context=context;
    assert(!kernel_mm_munmap(&mm,0x40000,BOAROS_PAGE_SIZE)); publish(&unmap_done); return NULL;
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout) &&
        !physical_page_allocator_bind_access(&pages,access_page) && !physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space space={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages),address;
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel) && !kernel_mm_create(&mm,&space) &&
        !kernel_mm_vma_enable(&mm,&heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&context)); mm_test_context=context;
    assert(!kernel_mm_enable_remote_user_execution(&mm));
    assert(!kernel_tmpfs_create(&heap,0,"nr_blocks=8",&mount));

    struct kernel_open_file_description *file=map_file("/unmap",0x40000);
    close_caller(&file); assert(!kernel_mm_munmap(&mm,0x40000,BOAROS_PAGE_SIZE) && last_releases==1 && !watched);
    file=map_file("/replace",0x40000); close_caller(&file);
    assert(!kernel_mm_mmap_anonymous(&mm,0x40000,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED,&address) && last_releases==2 && !watched);
    file=map_file("/old",0x40000); close_caller(&file);
    struct kernel_open_file_description *replacement=map_file("/new",0x40000);
    assert(last_releases==3 && !watched); close_caller(&replacement);
    assert(!kernel_mm_munmap(&mm,0x40000,BOAROS_PAGE_SIZE) && last_releases==4 && !watched);
    file=map_file("/close-failure",0x40000); close_caller(&file); fail_close=1;
    assert(!kernel_mm_munmap(&mm,0x40000,BOAROS_PAGE_SIZE));
    assert(!fail_close && watched && !watched->references && last_releases==4);
    assert(!kernel_mm_mmap_anonymous(&mm,0x40000,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED,&address) && !watched && last_releases==5);

    callback_file=map_file("/callback",0x40000);
    unsigned char byte=0; size_t copied;
    assert(!kernel_copy_from_user(&mm,&byte,0x40000,1,&copied) && copied==1);
    assert(!kernel_mm_mmap_anonymous(&mm,0x80000,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED,&address));
    assert(!kernel_copy_to_user(&mm,0x80000,&byte,1,&copied));
    pause_callback=1; pthread_t notifier,remover;
    assert(!pthread_create(&notifier,NULL,truncate_callback,NULL)); await(&callback_entered);
    unsigned before=mm_test_parked(6);
    assert(!pthread_create(&remover,NULL,unmap_callback,NULL));
    while (mm_test_parked(6)==before && !__atomic_load_n(&unmap_done,__ATOMIC_ACQUIRE)) sched_yield();
    assert(mm_test_parked(6)>before && !unmap_done);
    /* 真实登记仍被callback借用，等待者已提交unmap；不挡同MM无关resident pin。 */
    kernel_cpu_current()->logical_id=1; mm_test_bind(&mm); kernel_task_current()->arch.user_mode=1;
    struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_try_pin_user_page(&mm,0x80000,KERNEL_MM_WRITE,&pin)); kernel_mm_unpin_user_page(&pin);
    kernel_task_current()->arch.user_mode=0; mm_test_bind(NULL); kernel_cpu_current()->logical_id=0;
    publish(&callback_resume);
    assert(!pthread_join(notifier,NULL) && !pthread_join(remover,NULL) && unmap_done);
    assert(!kernel_open_file_release(&callback_file));
    mm_test_context=0; assert(!kernel_mm_release(&mm) && !kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("MM remote sources: last close + unmap/MAP_FIXED, failed close owner retry, active callback drain outside gate, final owners verified");
}
