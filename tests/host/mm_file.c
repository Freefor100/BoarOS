#include <kernel/mm_backend.h>
#include <kernel/futex.h>
#include <kernel/uaccess.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/file_mapping.h>
#include "../../fs/open_file_internal.h"
#include "../../fs/vfs_objects.h"
#include <assert.h>
#include <stdlib.h>
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
static struct kernel_mm parent, child;
static struct kernel_vfs_mount *mount;
static struct kernel_open_file_description *controller;
static struct kernel_mm views[5];
static uint64_t roots[5], target;
static unsigned capture, entered[5], resume, fail_heap, fail_page;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{
    if (fail_heap && !--fail_heap) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(h,n,size,out);
}
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *out)
{
    if (fail_page && !--fail_page) return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(a,out);
}
static enum kernel_uaccess_status results[5];
static unsigned char read_byte(struct kernel_mm *,uint64_t);
static unsigned callback_pause, callback_entered, callback_resume, truncate_done, unmap_done;
void __real_kernel_rwlock_write(struct kernel_rwlock *,struct kernel_lock_guard *);
void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock,struct kernel_lock_guard *guard)
{
    if (kernel_cpu_current()->hardware_id==5 && lock->rank==25 &&
        __atomic_exchange_n(&callback_pause,0,__ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks && !kernel_lock_held(&kernel_open_file_node(controller)->io_lock,0));
        __atomic_store_n(&callback_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&callback_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    __real_kernel_rwlock_write(lock,guard);
}
static void *truncate_file(void *arg)
{
    (void)arg; mm_test_attach(5);
    assert(!kernel_vfs_ftruncate(&controller->file,BOAROS_PAGE_SIZE));
    __atomic_store_n(&truncate_done,1,__ATOMIC_RELEASE);
    return 0;
}
static void *unmap_child(void *arg)
{
    (void)arg; mm_test_attach(6); mm_test_context=roots[2];
    assert(!kernel_mm_munmap(&child,0x40000,3*BOAROS_PAGE_SIZE));
    __atomic_store_n(&unmap_done,1,__ATOMIC_RELEASE);
    return 0;
}
static void unregister_callback(void)
{
    callback_pause=1; callback_entered=callback_resume=truncate_done=unmap_done=0;
    pthread_t truncator,remover;
    assert(!pthread_create(&truncator,0,truncate_file,0));
    while (!__atomic_load_n(&callback_entered,__ATOMIC_ACQUIRE)) sched_yield();
    struct kernel_mm rejected={0};
    assert(kernel_mm_fork(&rejected,&parent)==KERNEL_MM_STATUS_CONFLICT && rejected.state==KERNEL_MM_EMPTY);
    unsigned parked=mm_test_parked(6);
    assert(!pthread_create(&remover,0,unmap_child,0));
    while (mm_test_parked(6)==parked && !__atomic_load_n(&unmap_done,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!unmap_done && !truncate_done);
    __atomic_store_n(&callback_resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(truncator,0) && !pthread_join(remover,0));
    assert(unmap_done && truncate_done);
    assert(!kernel_vfs_ftruncate(&controller->file,3*BOAROS_PAGE_SIZE));
    mm_test_context=roots[2];
    struct kernel_open_file_description *owner=controller; uint64_t address;
    assert(!kernel_open_file_acquire(owner) && !kernel_mm_mmap_file_private(&child,&owner,
        0x40000,3*BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&address));
    mm_test_context=roots[1];
}
static void private_case(void)
{
    uint64_t address; size_t done; unsigned char value=0x37;
    struct kernel_open_file_description *owner=controller;
    assert(!kernel_open_file_acquire(owner) && !kernel_mm_mmap_file_private(&parent,&owner,
        0x80000,3*BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
    assert(read_byte(&parent,address)==read_byte(&parent,0x40000));
    struct kernel_mm_mapping p,s;
    assert(!kernel_mm_lookup(&parent,address,&p) && !kernel_mm_lookup(&parent,0x40000,&s) &&
        p.physical_address==s.physical_address && !(p.permissions&KERNEL_MM_WRITE));
    assert(!kernel_copy_to_user(&parent,address,&value,1,&done) && done==1 && read_byte(&parent,0x40000)!=value);
    value=0x55; assert(!kernel_copy_to_user(&parent,address+2*BOAROS_PAGE_SIZE+100,&value,1,&done));
    assert(!kernel_mm_mprotect(&parent,address,BOAROS_PAGE_SIZE,0));
    struct kernel_mm descendant={0};
    assert(!kernel_mm_fork(&descendant,&parent));
    assert(!kernel_mm_mprotect(&parent,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    uint64_t root; assert(!kernel_mm_context(&descendant,&root)); mm_test_context=root;
    assert(!kernel_mm_mprotect(&descendant,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    assert(read_byte(&descendant,address)==0x37);
    value=0x61; assert(!kernel_copy_to_user(&descendant,address,&value,1,&done));
    mm_test_context=roots[1]; assert(read_byte(&parent,address)==0x37);
    assert(!kernel_vfs_ftruncate(&controller->file,2*BOAROS_PAGE_SIZE+50));
    assert(read_byte(&parent,address+2*BOAROS_PAGE_SIZE+100)==0x55);
    assert(read_byte(&parent,0x40000+2*BOAROS_PAGE_SIZE+100)==0);
    assert(!kernel_vfs_ftruncate(&controller->file,BOAROS_PAGE_SIZE));
    unsigned char out; done=123;
    assert(kernel_copy_from_user(&parent,&out,address+2*BOAROS_PAGE_SIZE+100,1,&done)==KERNEL_UACCESS_STATUS_FAULT && !done);
    assert(!kernel_vfs_ftruncate(&controller->file,3*BOAROS_PAGE_SIZE));
    assert(read_byte(&parent,address+2*BOAROS_PAGE_SIZE+100)==0);
    mm_test_context=0; assert(!kernel_mm_release(&descendant)); mm_test_context=roots[1];
}
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p, uint64_t *out) { *out=(uintptr_t)p; return 1; }
void mm_test_fence(uint64_t v, int instructions)
{ (void)v; (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
int __real_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *,uint64_t,
    struct kernel_open_file_page_preparation *);
int __wrap_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *file,uint64_t index,
    struct kernel_open_file_page_preparation *prepared)
{
    int result=__real_kernel_open_file_prepare_mapping_page(file,index,prepared);
    unsigned id=kernel_cpu_current()->hardware_id;
    if (!result && id<5 && (__atomic_fetch_and(&capture,~(1U<<id),__ATOMIC_ACQ_REL)&(1U<<id))) {
        assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->locks);
        __atomic_store_n(&entered[id],1,__ATOMIC_RELEASE);
        while (!(__atomic_load_n(&resume,__ATOMIC_ACQUIRE)&(1U<<id))) sched_yield();
    }
    return result;
}
static void *fault(void *arg)
{
    unsigned id=(unsigned)(uintptr_t)arg; mm_test_attach(id); mm_test_context=roots[id];
    unsigned char byte=(unsigned char)(0x80+id); size_t done;
    results[id]=kernel_copy_to_user(&views[id],target+id,&byte,1,&done);
    assert(done==(results[id]==KERNEL_UACCESS_STATUS_OK?1U:0U));
    return 0;
}
static unsigned char read_byte(struct kernel_mm *mm,uint64_t address)
{
    unsigned char byte; size_t done;
    assert(!kernel_copy_from_user(mm,&byte,address,1,&done) && done==1);
    return byte;
}
static void ready(unsigned id)
{ while (!__atomic_load_n(&entered[id],__ATOMIC_ACQUIRE)) sched_yield(); }
static void first_fault(unsigned actors)
{
    target=0x40000+(actors==2?1:2)*BOAROS_PAGE_SIZE;
    uint64_t before=pages.shared_anon_pages;
    memset(entered,0,sizeof(entered)); resume=0; capture=(1U<<(actors+1))-2;
    pthread_t workers[4];
    for (unsigned id=1; id<=actors; id++) assert(!pthread_create(&workers[id-1],0,fault,(void *)(uintptr_t)id));
    for (unsigned id=1; id<=actors; id++) ready(id);
    assert(pages.shared_anon_pages==before+1);
    __atomic_store_n(&resume,UINT32_MAX,__ATOMIC_RELEASE);
    for (unsigned id=1; id<=actors; id++) assert(!pthread_join(workers[id-1],0) && !results[id]);
    struct kernel_mm_mapping a,b;
    assert(!kernel_mm_lookup(&parent,target,&a) && !kernel_mm_lookup(&child,target,&b) && a.physical_address==b.physical_address);
    for (unsigned id=1; id<=actors; id++) assert(read_byte(&parent,target+id)==0x80+id);
    assert(pages.shared_anon_pages==before+1);
}
static void fault_failures(void)
{
    assert(!kernel_vfs_ftruncate(&controller->file,0));
    assert(!kernel_vfs_ftruncate(&controller->file,3*BOAROS_PAGE_SIZE));
    uint64_t charged=pages.shared_anon_pages; struct kernel_mm_mapping mapping;
    const uint64_t cold=UINT64_C(0x40000000);
    uint64_t cold_address; struct kernel_open_file_description *cold_owner=controller;
    assert(!kernel_open_file_acquire(cold_owner) && !kernel_mm_mmap_file_private(&parent,&cold_owner,
        cold,BOAROS_PAGE_SIZE,2*BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&cold_address));
    for (unsigned kind=0;kind<2;kind++) for (unsigned nth=1;nth<=4;nth++) {
        if (!kind) fail_page=nth; else fail_heap=nth;
        unsigned char value=17; size_t done=123;
        enum kernel_uaccess_status status=kernel_copy_to_user(&parent,cold,&value,1,&done);
        fail_page=fail_heap=0;
        if (status) {
            assert(!done && pages.shared_anon_pages==charged &&
                kernel_mm_lookup(&parent,cold,&mapping)==KERNEL_MM_STATUS_NOT_MAPPED);
        } else {
            assert(done==1 && read_byte(&parent,cold)==17);
            assert(!kernel_vfs_ftruncate(&controller->file,0));
            assert(!kernel_vfs_ftruncate(&controller->file,3*BOAROS_PAGE_SIZE));
        }
    }
    /* preparing 期间 fixed 替换必须丢弃旧来源页，不能写入新匿名 VMA。 */
    target=0x40000+2*BOAROS_PAGE_SIZE; memset(entered,0,sizeof(entered)); capture=2; resume=0;
    pthread_t worker; assert(!pthread_create(&worker,0,fault,(void *)(uintptr_t)1)); ready(1);
    uint64_t address;
    assert(!kernel_mm_mmap_anonymous(&parent,target,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
    __atomic_store_n(&resume,2,__ATOMIC_RELEASE); assert(!pthread_join(worker,0));
    assert(!results[1] && read_byte(&parent,target+1)==0x81 && pages.shared_anon_pages==charged);
    struct kernel_open_file_description *owner=controller;
    assert(!kernel_open_file_acquire(owner) && !kernel_mm_mmap_file_private(&parent,&owner,
        0x40000,3*BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&address));
}
static void stale_shrink(void)
{
    assert(!kernel_vfs_ftruncate(&controller->file,0));
    assert(!kernel_vfs_ftruncate(&controller->file,3*BOAROS_PAGE_SIZE));
    target=0x40000+2*BOAROS_PAGE_SIZE;
    uint64_t before=pages.shared_anon_pages;
    memset(entered,0,sizeof(entered)); capture=2; resume=0;
    pthread_t worker; assert(!pthread_create(&worker,0,fault,(void *)(uintptr_t)1)); ready(1);
    assert(pages.shared_anon_pages==before+1);
    assert(!kernel_vfs_ftruncate(&controller->file,BOAROS_PAGE_SIZE));
    assert(pages.shared_anon_pages==before);
    __atomic_store_n(&resume,2,__ATOMIC_RELEASE);
    assert(!pthread_join(worker,0) && results[1]==KERNEL_UACCESS_STATUS_FAULT);
    assert(pages.shared_anon_pages==before);
    assert(!kernel_vfs_ftruncate(&controller->file,3*BOAROS_PAGE_SIZE));
    assert(read_byte(&parent,target)==0);
    mm_test_context=roots[2];
    assert(read_byte(&child,target)==0);
    mm_test_context=roots[1];
}
static void futex_key_lifetime(void)
{
    struct kernel_open_file_description *file = 0;
    int error;
    assert(!kernel_open_file_create_mode(&heap,mount,"/futex-key",0600,&file,&error) && !error);
    file->open_flags = 2;
    assert(!kernel_vfs_ftruncate(&file->file,2*BOAROS_PAGE_SIZE));
    struct kernel_vfs_node *node = kernel_open_file_node(file);
    uint64_t addresses[2];
    struct kernel_futex_key keys[2];
    for (unsigned i=0; i<2; i++) {
        struct kernel_open_file_description *source = file;
        assert(!kernel_open_file_acquire(source));
        assert(!kernel_mm_mmap_file_private(&parent,&source,0,BOAROS_PAGE_SIZE,
            BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_SHARED,&addresses[i]));
        assert(!kernel_mm_acquire_futex_key(&parent,addresses[i]+12,1,&keys[i]));
        assert(keys[i].kind==KERNEL_FUTEX_KEY_SHARED_FILE && keys[i].file_node==node &&
               keys[i].offset==BOAROS_PAGE_SIZE+12);
    }
    assert(keys[0].identity==keys[1].identity && addresses[0]!=addresses[1]);
    assert(!kernel_vfs_unlink(mount,"/futex-key"));
    for (unsigned i=0; i<2; i++) assert(!kernel_mm_munmap(&parent,addresses[i],BOAROS_PAGE_SIZE));
    assert(!kernel_open_file_release(&file));
    assert(node->references==2); /* 最后两个映射及 OFD 已结束，仅 key 持有 inode。 */
    assert(!kernel_vfs_node_release(&keys[0].file_node) && node->references==1);
    assert(!kernel_vfs_node_release(&keys[1].file_node));

    /* 冷共享页取身份可能真实分配失败；不发布后备引用，之后仍可正常解除映射。 */
    uint64_t cold;
    assert(!kernel_mm_mmap_anonymous(&parent,0,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_SHARED,&cold));
    size_t capacity=physical_page_total(&pages), count=0;
    uint64_t *held=malloc(capacity*sizeof(*held));
    assert(held);
    while (count<capacity && !physical_page_allocate(&pages,&held[count])) count++;
    struct kernel_futex_key failed={0};
    assert(kernel_mm_acquire_futex_key(&parent,cold,1,&failed)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!failed.identity && !failed.shared_object);
    while (count) assert(!physical_page_release(&pages,held[--count]));
    free(held);
    assert(!kernel_mm_munmap(&parent,cold,BOAROS_PAGE_SIZE));
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
    int error;
    assert(!kernel_open_file_create_mode(&heap,mount,"/mapping",0600,&controller,&error) && !error);
    controller->open_flags=2;
    assert(!kernel_vfs_ftruncate(&controller->file,3*BOAROS_PAGE_SIZE));
    uint64_t mapped;
    struct kernel_open_file_description *source=controller;
    assert(!kernel_open_file_acquire(source));
    fail_heap=1;
    assert(kernel_mm_mmap_file_private(&parent,&source,0x40000,3*BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&mapped)==KERNEL_MM_STATUS_NO_MEMORY && source);
    struct kernel_mm_mapping absent;
    assert(kernel_mm_lookup(&parent,0x40000,&absent)==KERNEL_MM_STATUS_NOT_MAPPED &&
        !kernel_open_file_node(controller)->mapping_state);
    assert(!kernel_mm_mmap_file_private(&parent,&source,0x40000,3*BOAROS_PAGE_SIZE,0,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED|KERNEL_MM_MAP_SHARED,&mapped) && !source && mapped==0x40000);
    /* 初次状态 OOM 必须在 VMA/登记发布之前失败，OFD 引用仍归调用者。 */
    /* 真实tmpfs来源已经受其实例预算约束；旧实现无条件拒绝文件域。 */
    assert(kernel_mm_check_task_domain(&parent)==KERNEL_MM_STATUS_OK);
    assert(!kernel_mm_fork(&child,&parent));
    for (unsigned id=1; id<=4; id++) {
        assert(!kernel_mm_acquire(&views[id],id==2 || id==4 ? &child : &parent));
        assert(!kernel_mm_context(&views[id],&roots[id]));
    }
    unsigned char byte=0x73; size_t done;
    assert(!kernel_copy_to_user(&parent,mapped,&byte,1,&done) && done==1);
    uint64_t child_root; assert(!kernel_mm_context(&child,&child_root)); mm_test_context=child_root;
    byte=0; assert(!kernel_copy_from_user(&child,&byte,mapped,1,&done) && done==1 && byte==0x73);
    mm_test_context=roots[1];
    futex_key_lifetime();
    first_fault(2); first_fault(4); stale_shrink(); fault_failures(); unregister_callback(); private_case();
    mm_test_context=0;
    for (unsigned id=1; id<=4; id++) assert(!kernel_mm_release(&views[id]));
    assert(!kernel_mm_release(&child) && !kernel_mm_release(&parent));
    assert(!kernel_open_file_release(&controller) && !controller && !kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("file MM: real tmpfs source, fork sharing and final owners OK");
    return 0;
}
