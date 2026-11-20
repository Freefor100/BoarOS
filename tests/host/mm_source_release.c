#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/tmpfs.h>
#include <kernel/open_file.h>
#include <kernel/uaccess.h>
#include <kernel/file_mapping.h>
#include <kernel/errno.h>
#include <kernel/elf64_source.h>
#include <kernel/vma.h>
#include <arch/riscv/plic.h>
#include <string.h>
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
void mm_file_unavailable(void);
/* madvise links disk-only paths in this tmpfs fixture. */
int kernel_scheduler_can_sleep(void) { return 1; }
int riscv_plic_in_interrupt(void) { return 0; }
enum kernel_page_cache_status kernel_page_cache_run_cpu0_borrowing(
    struct kernel_page_cache *cache, void (*execute)(void *), void *argument,
    const struct kernel_lock_guard *borrowed)
{
    (void)cache; (void)execute; (void)argument; (void)borrowed;
    mm_file_unavailable();
    abort();
}

extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[4*1024*1024] __attribute__((aligned(4096)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_mm mm;
static struct kernel_vfs_mount *mount;
static struct kernel_open_file_description *watched, *callback_file;
static struct kernel_elf64_source *watched_elf;
static uint64_t context;
static unsigned last_releases, pause_callback, callback_after_fence, callback_entered, callback_resume, unmap_done;
static unsigned fail_close;
static unsigned fail_page, mincore_pause, mincore_entered, mincore_resume;
static unsigned self_pin_prepare, self_pin_prepared, self_pin_hold, self_pin_writer_held, self_pin_returned;
static unsigned elf_pin_prepare, elf_releases;
static uint64_t self_pin_address;
static int mincore_result;
static unsigned mincore_completed;
static _Thread_local unsigned forbid_backing_scans, backing_scans;
static void outside_gate(void);
static void publish(unsigned *flag) { __atomic_store_n(flag,1,__ATOMIC_RELEASE); }
static void await(unsigned *flag) { while (!__atomic_load_n(flag,__ATOMIC_ACQUIRE)) sched_yield(); }
int __real_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *,uint64_t,int,
    struct kernel_open_file_page_preparation *);
int __wrap_kernel_open_file_prepare_mapping_page(struct kernel_open_file_description *file,uint64_t index,int read_window,
    struct kernel_open_file_page_preparation *prepared)
{
    int result=__real_kernel_open_file_prepare_mapping_page(file,index,read_window,prepared);
    if (!result && __atomic_exchange_n(&self_pin_prepare,0,__ATOMIC_ACQ_REL)) {
        outside_gate(); publish(&self_pin_prepared); await(&self_pin_writer_held);
    }
    return result;
}
enum kernel_elf64_source_status __real_kernel_elf64_source_page(struct kernel_elf64_source *,
    struct physical_page_allocator *,uint64_t,uint64_t *,int *,int);
enum kernel_elf64_source_status __wrap_kernel_elf64_source_page(struct kernel_elf64_source *source,
    struct physical_page_allocator *allocator,uint64_t offset,uint64_t *address,int *shared,int read_window)
{
    enum kernel_elf64_source_status result=__real_kernel_elf64_source_page(source,allocator,offset,address,shared,read_window);
    if (!result && __atomic_exchange_n(&elf_pin_prepare,0,__ATOMIC_ACQ_REL)) {
        outside_gate(); publish(&self_pin_prepared); await(&self_pin_writer_held);
    }
    return result;
}
void __real_kernel_lock_release(struct kernel_lock_guard *);
void __wrap_kernel_lock_release(struct kernel_lock_guard *guard)
{
    if (kernel_cpu_current()->hardware_id==8 && guard->lock && guard->lock->rank==25 && guard->write &&
        __atomic_exchange_n(&self_pin_hold,0,__ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks);
        publish(&self_pin_writer_held); await(&self_pin_returned);
    }
    __real_kernel_lock_release(guard);
}
enum kernel_vma_status __real_kernel_vma_set_backing_in_use(const struct kernel_vma_set *,const void *,int *);
enum kernel_vma_status __wrap_kernel_vma_set_backing_in_use(const struct kernel_vma_set *set,const void *backing,int *used)
{
    assert(!forbid_backing_scans && "unchanged VMA borrow must not scan backing membership");
    backing_scans++;
    return __real_kernel_vma_set_backing_in_use(set,backing,used);
}
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *allocator,uint64_t *address)
{
    if (__atomic_exchange_n(&fail_page,0,__ATOMIC_ACQ_REL)) return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(allocator,address);
}
int __real_kernel_vfs_file_mincore_allowed(const struct kernel_vfs_file *);
int __wrap_kernel_vfs_file_mincore_allowed(const struct kernel_vfs_file *file)
{
    if (__atomic_exchange_n(&mincore_pause,0,__ATOMIC_ACQ_REL)) {
        outside_gate(); publish(&mincore_entered); await(&mincore_resume);
    }
    return __real_kernel_vfs_file_mincore_allowed(file);
}
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
void __real_kernel_heap_release(struct kernel_heap *,void *);
void __wrap_kernel_heap_release(struct kernel_heap *h,void *pointer)
{
    if (kernel_cpu_current()->hardware_id==5 &&
        __atomic_exchange_n(&callback_after_fence,0,__ATOMIC_ACQ_REL)) {
        /* 此resident已摘索引；callback尚未返回，registration仍被实际借用。 */
        publish(&callback_entered); await(&callback_resume);
    }
    int last_elf=watched_elf && pointer==watched_elf;
    __real_kernel_heap_release(h,pointer);
    if (last_elf) {watched_elf=NULL;elf_releases++;}
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
static void *mincore_last_mapping(void *unused)
{
    (void)unused; mm_test_attach(7); kernel_cpu_current()->logical_id=0; mm_test_context=context;
    unsigned char resident;
    mincore_result=kernel_mm_mincore(&mm,0x60000,1,&resident,&mincore_completed);
    return NULL;
}
static void mincore_unmap(void)
{
    struct kernel_open_file_description *file=map_file("/mincore-unmap",0x60000);
    close_caller(&file);
    unsigned before=last_releases;
    mincore_pause=1; pthread_t reader;
    assert(!pthread_create(&reader,NULL,mincore_last_mapping,NULL)); await(&mincore_entered);
    assert(!kernel_mm_munmap(&mm,0x60000,BOAROS_PAGE_SIZE));
    assert(watched && watched->references==1 && last_releases==before);
    publish(&mincore_resume); assert(!pthread_join(reader,NULL));
    assert(mincore_result==-KERNEL_ENOMEM && !mincore_completed);
    assert(!watched && last_releases==before+1);
    puts("MM mincore last mapping removed during source borrow: no stale result and final owner returned");
}
static void *self_pin_unmap(void *unused)
{
    (void)unused; mm_test_attach(8); kernel_cpu_current()->logical_id=0; mm_test_context=context;
    await(&self_pin_prepared);
    assert(!kernel_mm_munmap(&mm,self_pin_address,BOAROS_PAGE_SIZE));
    return NULL;
}
static void self_pin_source_retirement(void)
{
    uint64_t placed;
    assert(!kernel_mm_mmap_anonymous(&mm,0xa0000,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&placed));
    assert(!kernel_mm_resolve_user_fault(&mm,placed,KERNEL_MM_WRITE));
    struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_pin_user_page(&mm,placed,KERNEL_MM_WRITE,&pin));
    struct kernel_open_file_description *file=map_file("/self-pin-unmap",0x70000);
    close_caller(&file);
    unsigned before=last_releases;
    self_pin_address=0x70000;
    self_pin_prepare=self_pin_hold=1;
    pthread_t modifier; assert(!pthread_create(&modifier,NULL,self_pin_unmap,NULL));
    assert(kernel_mm_resolve_user_fault(&mm,0x70000,KERNEL_MM_READ)==KERNEL_MM_STATUS_CONFLICT);
    /* The writer already checked/removes sources, but still owns its gate.
     * This pin forbids the fault from waiting for that writer to release it. */
    assert(self_pin_writer_held && watched && watched->references==1 && last_releases==before);
    publish(&self_pin_returned); assert(!pthread_join(modifier,NULL));
    kernel_mm_unpin_user_page(&pin);
    assert(!kernel_io_context_current()->user_pins);
    fail_close=1;
    kernel_mm_drain_sources(&mm); /* The production user-return consumer. */
    assert(!fail_close && watched && !watched->references && last_releases==before);
    kernel_mm_drain_sources(&mm);
    assert(!watched && last_releases==before+1);
    puts("MM self-pin last source borrow survives writer drain and failed close; user-return drain closes the retained owner");
    assert(!kernel_mm_pin_user_page(&mm,placed,KERNEL_MM_WRITE,&pin));
    file=map_file("/pinned-ready-source",0xb0000); close_caller(&file);
    before=last_releases; fail_close=1;
    assert(!kernel_mm_munmap(&mm,0xb0000,BOAROS_PAGE_SIZE));
    assert(fail_close && watched && watched->references==1 && last_releases==before);
    kernel_mm_unpin_user_page(&pin);
    kernel_mm_drain_sources(&mm);
    assert(!fail_close && watched && !watched->references && last_releases==before);
    kernel_mm_drain_sources(&mm);
    assert(!watched && last_releases==before+1);
    puts("MM idle retired source defers close while its actor holds a pin; user-return retries the actual close failure");
}
static void u16(unsigned char *p,uint16_t value)
{ for(unsigned i=0;i<2;i++)p[i]=(unsigned char)(value>>(8*i)); }
static void u32(unsigned char *p,uint32_t value)
{ for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i)); }
static void u64(unsigned char *p,uint64_t value)
{ for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(value>>(8*i)); }

static void fork_fault_sources(struct kernel_elf64_source *source)
{
    const uint64_t page = BOAROS_PAGE_SIZE;
    const uint64_t first = 0xc0000, alias = 0xc2000, skip = 0xc4000;
    const uint64_t elf_page = 0x100000, elf_bias = 0x100000;
    uint64_t mapped;
    struct kernel_open_file_description *file = map_file("/forked-source", first);
    struct kernel_open_file_description *owner = file;
    assert(!kernel_open_file_acquire(owner));
    assert(!kernel_mm_mmap_file_private(&mm, &owner, alias, page, 0, KERNEL_MM_READ,
        KERNEL_MM_MAP_FIXED, &mapped) && !owner);
    struct kernel_open_file_description *excluded = map_file("/fork-excluded-source", skip);
    assert(!kernel_mm_madvise(&mm, skip, page, KERNEL_MM_ADVICE_DONTFORK));
    assert(!kernel_mm_map_elf_source(&mm, source, elf_bias));

    struct kernel_mm child = {0};
    assert(!kernel_mm_fork(&child, &mm));
    struct kernel_vma parent_vma, child_vma, child_alias;
    assert(!kernel_mm_vma_lookup(&mm, first, &parent_vma));
    assert(!kernel_mm_vma_lookup(&child, first, &child_vma));
    assert(!kernel_mm_vma_lookup(&child, alias, &child_alias));
    assert(child_vma.backing == file && child_alias.backing == file);
    assert(child_vma.backing_owner != parent_vma.backing_owner &&
        child_vma.backing_owner == child_alias.backing_owner);
    assert(!kernel_mm_vma_lookup(&mm, elf_page, &parent_vma));
    assert(!kernel_mm_vma_lookup(&child, elf_page + elf_bias, &child_vma));
    assert(!kernel_mm_vma_lookup(&child, elf_page, &child_alias));
    assert(child_vma.backing == source && child_alias.backing == source);
    assert(child_vma.backing_owner != parent_vma.backing_owner &&
        child_vma.backing_owner == child_alias.backing_owner);
    assert(kernel_mm_vma_lookup(&child, skip, &child_vma) == KERNEL_MM_STATUS_NOT_MAPPED);
    assert(excluded->references == 2);

    assert(!kernel_mm_munmap(&mm, first, skip + page - first));
    assert(!kernel_mm_munmap(&mm, elf_page, page));
    assert(!kernel_mm_munmap(&mm, elf_page + elf_bias, page));
    assert(file->references == 2 && excluded->references == 1);
    assert(!kernel_open_file_release(&excluded));
    assert(!kernel_mm_context(&child, &mm_test_context));
    assert(!kernel_mm_resolve_user_fault(&child, first, KERNEL_MM_READ));
    assert(!kernel_mm_resolve_user_fault(&child, alias, KERNEL_MM_READ));
    assert(!kernel_mm_resolve_user_fault(&child, elf_page + elf_bias, KERNEL_MM_EXECUTE));
    assert(!kernel_vfs_ftruncate(&file->file, 0));
    assert(kernel_mm_resolve_user_fault(&child, first, KERNEL_MM_READ) == KERNEL_MM_STATUS_BUS_FAULT);
    assert(kernel_mm_resolve_user_fault(&child, alias, KERNEL_MM_READ) == KERNEL_MM_STATUS_BUS_FAULT);
    mm_test_context = context;
    assert(!kernel_mm_release(&child) && file->references == 1);
    assert(!kernel_open_file_release(&file));
    puts("MM fork rebinds shared file and ELF owners once per child, skips DONTFORK and keeps truncate handling");
}

static void remote_last_executable_close(void)
{
    unsigned char bytes[128]={0};memcpy(bytes,"\177ELF\2\1\1",7);
    u16(bytes+16,KERNEL_ELF64_TYPE_EXECUTABLE);u16(bytes+18,KERNEL_ELF64_MACHINE_RISCV);
    u32(bytes+20,1);u64(bytes+24,0x100078);u64(bytes+32,64);
    u16(bytes+52,64);u16(bytes+54,56);u16(bytes+56,1);
    u32(bytes+64,KERNEL_ELF64_PROGRAM_LOAD);u32(bytes+68,KERNEL_ELF64_FLAG_READ|KERNEL_ELF64_FLAG_EXECUTE);
    u64(bytes+80,0x100000);u64(bytes+96,sizeof(bytes));u64(bytes+104,BOAROS_PAGE_SIZE);u64(bytes+112,BOAROS_PAGE_SIZE);
    struct kernel_read_source reader;
    struct kernel_elf64_source *source=NULL;
    assert(!kernel_read_source_from_memory(bytes,sizeof(bytes),&reader));
    assert(!kernel_elf64_source_create_reader(&heap,&reader,BOAROS_PAGE_SIZE,KERNEL_ELF64_MACHINE_RISCV,&source));
    watched_elf=source;
    assert(!kernel_mm_map_elf_source(&mm,source,0));assert(!kernel_elf64_source_release(&source));
    struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_pin_user_page(&mm,0xa0000,KERNEL_MM_WRITE,&pin));
    unsigned released_before=elf_releases;
    self_pin_address=0x100000;
    self_pin_prepared=self_pin_writer_held=self_pin_returned=0;
    elf_pin_prepare=self_pin_hold=1;
    pthread_t modifier; assert(!pthread_create(&modifier,NULL,self_pin_unmap,NULL));
    assert(kernel_mm_resolve_user_fault(&mm,0x100078,KERNEL_MM_EXECUTE)==KERNEL_MM_STATUS_CONFLICT);
    assert(self_pin_writer_held && watched_elf && elf_releases==released_before);
    publish(&self_pin_returned); assert(!pthread_join(modifier,NULL));
    kernel_mm_unpin_user_page(&pin);
    kernel_mm_drain_sources(&mm);
    assert(!watched_elf && elf_releases==released_before+1);
    puts("MM self-pin ELF borrow remains retired through writer drain; user-return drain returns its source owner");
    assert(!kernel_elf64_source_create_reader(&heap,&reader,BOAROS_PAGE_SIZE,KERNEL_ELF64_MACHINE_RISCV,&source));
    assert(!kernel_mm_map_elf_source(&mm,source,0));
    forbid_backing_scans=1;
    assert(!kernel_mm_resolve_user_fault(&mm,0x100078,KERNEL_MM_EXECUTE));
    forbid_backing_scans=0;
    puts("MM ordinary ELF fault skips unchanged VMA backing scans");
    fork_fault_sources(source);
    assert(!kernel_mm_map_elf_source(&mm, source, 0));
    assert(!kernel_elf64_source_release(&source));
    struct kernel_open_file_description *file=NULL;int error;
    assert(!kernel_open_file_create_mode(&heap,mount,"/remote-executable",0700,&file,&error)&&!error);
    assert(!kernel_mm_set_executable(&mm,file));close_caller(&file);
    struct kernel_mm remote={0};assert(!kernel_mm_acquire(&remote,&mm));
    mm_test_context=0;assert(!kernel_mm_release(&mm));
    unsigned before=last_releases;fail_close=1;
    assert(kernel_mm_release(&remote)==KERNEL_MM_STATUS_CLEANUP_REQUIRED);
    assert(!fail_close&&watched&&!watched->references&&last_releases==before);
    assert(remote.state==KERNEL_MM_CLEANUP&&remote.cleanup_stage==KERNEL_MM_CLEANUP_ELF_SOURCES);
    assert(!kernel_mm_release(&remote)&&!watched&&last_releases==before+1);
    puts("MM remote last executable close failure retains actual MM/OFD for retry");
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
    assert(!kernel_tmpfs_create(&heap,0,"nr_blocks=8",&mount));

    struct kernel_open_file_description *normal=map_file("/normal",0x90000);
    forbid_backing_scans=1; fail_page=1;
    assert(kernel_mm_resolve_user_fault(&mm,0x90000,KERNEL_MM_READ)==KERNEL_MM_STATUS_NO_MEMORY && !fail_page);
    struct kernel_mm_mapping missing;
    assert(kernel_mm_lookup(&mm,0x90000,&missing)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!kernel_mm_resolve_user_fault(&mm,0x90000,KERNEL_MM_READ));
    assert(!kernel_mm_resolve_user_fault(&mm,0x90000,KERNEL_MM_READ));
    unsigned char resident; unsigned completed;
    assert(!kernel_mm_mincore(&mm,0x90000,1,&resident,&completed) && completed==1 && resident);
    forbid_backing_scans=0;

    struct kernel_open_file_description *file=map_file("/unmap",0x40000);
    backing_scans=0;
    close_caller(&file); assert(!kernel_mm_munmap(&mm,0x40000,BOAROS_PAGE_SIZE) && last_releases==1 && !watched);
    assert(backing_scans==2 && "each of the two active OFDs needs one VMA membership check");
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
    forbid_backing_scans=1;
    assert(!kernel_mm_resolve_user_fault(&mm,0x90000,KERNEL_MM_READ) && !watched && last_releases==5);
    forbid_backing_scans=0;
    assert(!kernel_mm_munmap(&mm,0x90000,BOAROS_PAGE_SIZE) && !kernel_open_file_release(&normal));
    puts("MM ordinary file faults and mincore skip unchanged VMA scans; OOM preserves source and retired close retries still progress");
    mincore_unmap();
    self_pin_source_retirement();

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
    assert(!kernel_mm_pin_user_page(&mm,0x80000,KERNEL_MM_WRITE,&pin)); kernel_mm_unpin_user_page(&pin);
    kernel_task_current()->arch.user_mode=0; mm_test_bind(NULL); kernel_cpu_current()->logical_id=0;
    publish(&callback_resume);
    assert(!pthread_join(notifier,NULL) && !pthread_join(remover,NULL) && unmap_done);
    assert(!kernel_open_file_release(&callback_file));
    remote_last_executable_close();assert(!kernel_vfs_unmount(mount));
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations && !stats.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("MM remote sources: last close + unmap/MAP_FIXED, failed close owner retry, active callback drain outside gate, final owners verified");
}
