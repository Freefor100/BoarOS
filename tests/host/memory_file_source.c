#include "../../fs/open_file_internal.h"
#include "../../fs/vfs_objects.h"
#include <kernel/tmpfs.h>
#include <kernel/memory_object.h>
#include <kernel/file_mapping.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

void mm_test_attach(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_vfs_mount *mount;
static struct kernel_open_file_description *controller;
static unsigned started, release_refs, fail_metadata, allocations;
static unsigned prepare_actors, prepared_candidates, publish_candidates;
static size_t state_bytes;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{
    __atomic_fetch_add(&allocations,1,__ATOMIC_RELAXED);
    if (fail_metadata) { fail_metadata=0; return KERNEL_HEAP_STATUS_EMPTY; }
    enum kernel_heap_status result=__real_kernel_heap_allocate_zeroed(h,n,size,out);
    if (prepare_actors && result==KERNEL_HEAP_STATUS_OK) {
        __atomic_store_n(&state_bytes,n*size,__ATOMIC_RELAXED);
        __atomic_fetch_add(&prepared_candidates,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&publish_candidates,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return result;
}
static void *prepare(void *arg)
{
    mm_test_attach((unsigned)(uintptr_t)arg);
    assert(!kernel_file_mapping_prepare(kernel_open_file_node(controller)));
    return 0;
}
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p, uint64_t *out) { *out=(uintptr_t)p; return 1; }
static void *references(void *arg)
{
    mm_test_attach((unsigned)(uintptr_t)arg);
    for (unsigned i=0; i<10000; i++) assert(!kernel_open_file_acquire(controller));
    __atomic_fetch_add(&started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_refs, __ATOMIC_ACQUIRE)) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
    for (unsigned i=0; i<10000; i++) {
        struct kernel_open_file_description *owner=controller;
        assert(!kernel_open_file_release(&owner) && !owner);
    }
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
    uint64_t baseline=physical_page_available(&pages);
    assert(!kernel_tmpfs_create(&heap,0,"nr_blocks=8",&mount));
    int error;
    assert(!kernel_open_file_create_mode(&heap,mount,"/mapping",0600,&controller,&error) && !error && controller);
    struct kernel_vfs_node *node=kernel_open_file_node(controller);
    assert(!kernel_tmpfs_check_domain(node,&heap,&pages));
    assert(kernel_memory_object_check_domain(node->memory,&heap,&pages)==KERNEL_MEMORY_OBJECT_STATE);
    for (unsigned actors=2; actors<=4; actors+=2) {
        started=release_refs=0; pthread_t workers[4];
        for (unsigned i=0; i<actors; i++) assert(!pthread_create(&workers[i],0,references,(void *)(uintptr_t)(i+1)));
        while (__atomic_load_n(&started,__ATOMIC_ACQUIRE)!=actors) {}
        assert(__atomic_load_n(&controller->references,__ATOMIC_ACQUIRE)==1+actors*10000);
        __atomic_store_n(&release_refs,1,__ATOMIC_RELEASE);
        for (unsigned i=0; i<actors; i++) assert(!pthread_join(workers[i],0));
        assert(controller->references==1 && !node->closed);
    }
    assert(!kernel_vfs_ftruncate(&controller->file,2*BOAROS_PAGE_SIZE));
    uint64_t charged=pages.shared_anon_pages;
    struct kernel_open_file_page_preparation failed={0};
    fail_metadata=1;
    assert(kernel_open_file_prepare_memory_page(controller,0,&failed)==-KERNEL_ENOMEM);
    assert(!failed.pending && !node->mapping_state && pages.shared_anon_pages==charged);
    prepare_actors=4; pthread_t candidates[4];
    for (unsigned i=0;i<4;i++) assert(!pthread_create(&candidates[i],0,prepare,(void *)(uintptr_t)(i+1)));
    while (__atomic_load_n(&prepared_candidates,__ATOMIC_ACQUIRE)!=4) sched_yield();
    __atomic_store_n(&publish_candidates,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<4;i++) assert(!pthread_join(candidates[i],0));
    prepare_actors=0;
    unsigned before=allocations;
    assert(!kernel_file_mapping_prepare(node) && allocations==before);
    struct kernel_open_file_page_preparation a={0}, b={0};
    assert(!kernel_open_file_prepare_memory_page(controller,0,&a));
    assert(!kernel_open_file_prepare_memory_page(controller,0,&b));
    assert(a.address==b.address && a.created && !b.created && pages.shared_anon_pages==charged+1);
    kernel_open_file_finish_memory_page(controller,&b,1);
    physical_page_release(&pages,b.address);
    kernel_open_file_finish_memory_page(controller,&a,0);
    assert(pages.shared_anon_pages==charged+1);
    struct kernel_vfs_stat stat;
    assert(!kernel_vfs_fstat(&controller->file,&stat) && stat.blocks==BOAROS_PAGE_SIZE/512);
    assert(!kernel_vfs_ftruncate(&controller->file,0) && pages.shared_anon_pages==charged);
    assert(!kernel_vfs_ftruncate(&controller->file,2*BOAROS_PAGE_SIZE));
    assert(!kernel_open_file_prepare_memory_page(controller,0,&a));
    assert(!kernel_open_file_prepare_memory_page(controller,0,&b));
    kernel_open_file_finish_memory_page(controller,&a,0);
    assert(pages.shared_anon_pages==charged+1);
    kernel_open_file_finish_memory_page(controller,&b,0);
    assert(pages.shared_anon_pages==charged);
    assert(!kernel_open_file_prepare_memory_page(controller,1,&a));
    uint64_t old=a.address;
    assert(!kernel_vfs_ftruncate(&controller->file,0));
    uint32_t refs;
    assert(!physical_page_reference_count(&pages,old,&refs) && refs==1);
    kernel_open_file_finish_memory_page(controller,&a,0);
    assert(pages.shared_anon_pages==charged);
    assert(kernel_open_file_prepare_memory_page(controller,UINT64_MAX,&a)==-KERNEL_EFAULT);
    assert(!kernel_open_file_release(&controller) && !controller);
    assert(!kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    printf("memory file source %lu KiB: OFD refs, real tmpfs budget and final owners OK\n", (unsigned long)BOAROS_PAGE_SIZE/1024);
    printf("mapping state=%zu bytes, first prepare race losers returned, warm prepare allocations=0\n",state_bytes);
    printf("inode=%zu OFD=%zu registration=%zu bytes; controller final release on CPU0\n",
           sizeof(struct kernel_vfs_node), sizeof(struct kernel_open_file_description),
           sizeof(struct kernel_file_mapping));
    return 0;
}
