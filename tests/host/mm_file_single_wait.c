#include <kernel/mm_backend.h>
#include <kernel/uaccess.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include "../../fs/open_file_internal.h"
#include "../../fs/vfs_objects.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
unsigned mm_test_parked(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_mm parent;
static struct kernel_vfs_mount *mount;
static struct kernel_open_file_description *controller;
static unsigned source_prepared, final_inject;
static unsigned final_entered, final_writer_claimed, final_replaced, final_before_park;
static uint64_t final_root;
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
void mm_test_fence(uint64_t v,int instructions)
{ (void)v;(void)instructions;__atomic_thread_fence(__ATOMIC_SEQ_CST); }
int __real_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *,uint64_t,
    struct kernel_open_file_page_preparation *);
int __wrap_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *file,uint64_t index,
    struct kernel_open_file_page_preparation *prepared)
{
    int result=__real_kernel_open_file_prepare_mapping_page(file,index,prepared);
    if (!result && final_inject) source_prepared=1;
    return result;
}
int __real_kernel_rwlock_try_read(struct kernel_rwlock *, struct kernel_lock_guard *);
int __wrap_kernel_rwlock_try_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    if (final_inject && source_prepared && lock == &kernel_open_file_node(controller)->io_lock) {
        assert(!kernel_cpu_current()->raw_locks);
        final_inject = 0;
        __atomic_store_n(&final_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&final_writer_claimed,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_rwlock_try_read(lock, guard);
}
void __real_kernel_rwlock_read(struct kernel_rwlock *, struct kernel_lock_guard *);
void __wrap_kernel_rwlock_read(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    if (source_prepared && lock == &kernel_open_file_node(controller)->io_lock)
        assert(!kernel_io_context_current()->locks && !kernel_cpu_current()->raw_locks);
    __real_kernel_rwlock_read(lock,guard);
}
static void *final_writer(void *unused)
{
    (void)unused; mm_test_attach(1);
    while (!__atomic_load_n(&final_entered,__ATOMIC_ACQUIRE)) sched_yield();
    KERNEL_LOCK_SCOPE(guard);
    kernel_vfs_node_lock(kernel_open_file_node(controller),&guard,1);
    __atomic_store_n(&final_writer_claimed,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&final_replaced,__ATOMIC_ACQUIRE)) sched_yield();
    return 0;
}
static void *final_replace(void *unused)
{
    (void)unused; mm_test_attach(2); mm_test_context=final_root;
    while (mm_test_parked(0)==final_before_park) sched_yield();
    assert(!kernel_io_context_current()->locks && !kernel_cpu_current()->raw_locks);
    uint64_t replaced;
    assert(!kernel_mm_mmap_anonymous(&parent,0x40000,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&replaced));
    assert(replaced==0x40000);
    __atomic_store_n(&final_replaced,1,__ATOMIC_RELEASE);
    return 0;
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout) &&
        !physical_page_allocator_bind_access(&pages,access) && !physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    struct arch_mmu_page_table kernel={0}; struct arch_mmu_user_space space={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages)); kernel.state=ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline=physical_page_available(&pages);
    assert(!arch_mmu_user_space_init(&space,&pages,&kernel) && !kernel_mm_create(&parent,&space) && !kernel_mm_vma_enable(&parent,&heap));
    assert(!kernel_mm_brk_initialize(&parent,0x10000,0x20000) && !kernel_mm_context(&parent,&mm_test_context));
    assert(!kernel_tmpfs_create(&heap,0,"nr_blocks=8",&mount));
    int error; assert(!kernel_open_file_create_mode(&heap,mount,"/mapping",0600,&controller,&error) && !error);
    controller->open_flags=2;
    unsigned char value=0x77; size_t done;
    assert(!kernel_vfs_pwrite(&controller->file,0,&value,1,&done) && done==1);
    uint64_t mapped; struct kernel_open_file_description *source=controller;
    assert(!kernel_open_file_acquire(source) && !kernel_mm_mmap_file_private(&parent,&source,0x40000,
        BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&mapped));
    final_inject=1; final_root=mm_test_context; final_before_park=mm_test_parked(0);
    pthread_t writer, modifier;
    assert(!pthread_create(&writer,0,final_writer,0) && !pthread_create(&modifier,0,final_replace,0));
    unsigned char observed=0; done=0;
    enum kernel_uaccess_status status=kernel_copy_from_user(&parent,&observed,mapped,1,&done);
    assert(!pthread_join(writer,0) && !pthread_join(modifier,0));
    assert(mm_test_parked(0)>final_before_park);
    struct kernel_vma latest; assert(!kernel_mm_vma_lookup(&parent,mapped,&latest));
    assert(status==KERNEL_UACCESS_STATUS_OK && done==1 && !observed && latest.kind==KERNEL_VMA_KIND_ANONYMOUS);
    mm_test_context=0;
    assert(!kernel_mm_release(&parent) && !kernel_open_file_release(&controller) && !kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("single-CPU file fault: actual final I/O park, VMA replacement and owner closure passed");
    return 0;
}
