/* Reuse the real MM/VFS/CPU fixture; this entry runs only REMOVE cases. */
#define main file_io_fixture_main
#include "file_io_concurrency.c"
#undef main
#include <kernel/shm.h>
#include <kernel/memory_object.h>
#include <stdlib.h>

#define REMOVE_BASE UINT64_C(0x700000)
#define REMOVE_ALIAS UINT64_C(0x800000)
#define REMOVE_PRIVATE UINT64_C(0x900000)
#define REMOVE_ADVICE 9
static unsigned remove_pause_prepare,remove_prepared,remove_continue_prepare;
static unsigned remove_pause_fork,remove_fork_ready,remove_continue_fork;
static unsigned remove_entered,remove_completed,remove_pause_file,remove_file_ready,remove_continue_file;
static unsigned remove_watch;
static unsigned remove_self_pin_watch,remove_self_pin_waited;
void __real_kernel_memory_object_wait(struct kernel_memory_object *);
void __wrap_kernel_memory_object_wait(struct kernel_memory_object *object)
{
    if (remove_self_pin_watch) __atomic_store_n(&remove_self_pin_waited,1,__ATOMIC_RELEASE);
    __real_kernel_memory_object_wait(object);
}
enum kernel_memory_object_status __real_kernel_memory_object_prepare_page(struct kernel_memory_object *,uint64_t,uint64_t *,int *);
enum kernel_memory_object_status __wrap_kernel_memory_object_prepare_page(struct kernel_memory_object *object,
    uint64_t index,uint64_t *address,int *created)
{
    enum kernel_memory_object_status status=__real_kernel_memory_object_prepare_page(object,index,address,created);
    if (__atomic_exchange_n(&remove_pause_prepare,0,__ATOMIC_ACQ_REL)) {
        assert(!status);__atomic_store_n(&remove_prepared,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&remove_continue_prepare,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}
enum arch_mmu_status __real_riscv_sv39_user_space_fork_prepare(struct arch_mmu_user_space *,struct arch_mmu_user_space *,int (*)(void *,uint64_t),void *);
enum arch_mmu_status __wrap_riscv_sv39_user_space_fork_prepare(struct arch_mmu_user_space *to,
    struct arch_mmu_user_space *from,int (*action)(void *,uint64_t),void *owner)
{
    enum arch_mmu_status status=__real_riscv_sv39_user_space_fork_prepare(to,from,action,owner);
    if (__atomic_exchange_n(&remove_pause_fork,0,__ATOMIC_ACQ_REL)) {
        assert(!status);__atomic_store_n(&remove_fork_ready,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&remove_continue_fork,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}
void __real_kernel_memory_object_remove(struct kernel_memory_object *,uint64_t,uint64_t);
void __wrap_kernel_memory_object_remove(struct kernel_memory_object *object,uint64_t start,uint64_t end)
{
    if (remove_watch) __atomic_store_n(&remove_entered,1,__ATOMIC_RELEASE);
    __real_kernel_memory_object_remove(object,start,end);
    if (remove_watch) __atomic_store_n(&remove_completed,1,__ATOMIC_RELEASE);
}
int __real_kernel_open_file_fallocate(struct kernel_open_file_description *,unsigned,int64_t,int64_t,struct kernel_vfs_size_limit *);
int __wrap_kernel_open_file_fallocate(struct kernel_open_file_description *file,unsigned mode,int64_t offset,
    int64_t length,struct kernel_vfs_size_limit *limit)
{
    if (__atomic_exchange_n(&remove_pause_file,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&remove_file_ready,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&remove_continue_file,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return __real_kernel_open_file_fallocate(file,mode,offset,length,limit);
}
static uint64_t remove_map(struct kernel_open_file_description *file,uint64_t at,
    uint64_t length,uint64_t offset,uint32_t permissions,int shared)
{
    struct kernel_open_file_description *source=file;uint64_t address;
    assert(!kernel_open_file_acquire(source));
    assert(!kernel_mm_mmap_file_private(&mm,&source,at,length,offset,permissions,
        KERNEL_MM_MAP_FIXED_NOREPLACE|(shared?KERNEL_MM_MAP_SHARED:0),&address)&&!source);
    return address;
}
static unsigned char remove_byte(struct kernel_mm *owner,uint64_t address)
{
    unsigned char byte;size_t copied;
    assert(!kernel_copy_from_user(owner,&byte,address,1,&copied)&&copied==1);
    return byte;
}
static void remove_store(struct kernel_mm *owner,uint64_t address,unsigned char byte)
{
    size_t copied;assert(!kernel_copy_to_user(owner,address,&byte,1,&copied)&&copied==1);
}
struct remove_actor {int action;uint64_t address,length;struct kernel_mm *target;struct kernel_mm child;int result;};
static void *remove_actor_run(void *opaque)
{
    struct remove_actor *actor=opaque;mm_test_attach((unsigned)actor->action+1);mm_test_activate_task();
    struct kernel_mm *target=actor->target?actor->target:&mm;
    if (actor->action!=2) mm_test_bind(target);
    kernel_cpu_current()->logical_id=actor->action==2?0:1;
    if (actor->action==1) actor->result=kernel_mm_resolve_user_fault(target,actor->address,KERNEL_MM_READ);
    else if (actor->action==2) actor->result=kernel_mm_fork(&actor->child,target);
    else actor->result=kernel_mm_madvise(target,actor->address,actor->length,REMOVE_ADVICE);
    mm_test_finish();return 0;
}
static void remove_object_races(void)
{
    uint64_t address;struct kernel_vma vma;pthread_t worker,remover;
    assert(!kernel_mm_mmap_anonymous(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE|KERNEL_MM_MAP_SHARED,&address));
    remove_store(&mm,address,0x63);assert(!kernel_mm_vma_lookup(&mm,address,&vma));
    assert(!kernel_mm_madvise(&mm,address,BOAROS_PAGE_SIZE,4));
    remove_prepared=remove_continue_prepare=0;remove_pause_prepare=1;
    struct remove_actor fault={.action=1,.address=address};
    assert(!pthread_create(&worker,0,remove_actor_run,&fault));
    while (!__atomic_load_n(&remove_prepared,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_madvise(&mm,address,BOAROS_PAGE_SIZE,REMOVE_ADVICE));
    assert(!kernel_memory_object_resident_pages(vma.backing));
    __atomic_store_n(&remove_continue_prepare,1,__ATOMIC_RELEASE);assert(!pthread_join(worker,0)&&!fault.result);
    assert(!remove_byte(&mm,address));
    puts("PASS MADV_REMOVE rejects an old prepared shared-object page after backing removal");

    remove_store(&mm,address,0x64);struct kernel_mm removal_mm={0};assert(!kernel_mm_fork(&removal_mm,&mm));
    remove_fork_ready=remove_continue_fork=0;remove_pause_fork=1;
    struct remove_actor forked={.action=2},punch={.address=address,.length=BOAROS_PAGE_SIZE,.target=&removal_mm};
    assert(!pthread_create(&worker,0,remove_actor_run,&forked));
    while (!__atomic_load_n(&remove_fork_ready,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_create(&remover,0,remove_actor_run,&punch));
    uint64_t epoch;while (!kernel_memory_object_epoch(vma.backing,&epoch)) sched_yield();
    __atomic_store_n(&remove_continue_fork,1,__ATOMIC_RELEASE);
    assert(!pthread_join(worker,0)&&!forked.result&&!pthread_join(remover,0)&&!punch.result);
    assert(!remove_byte(&mm,address)&&!remove_byte(&forked.child,address));
    assert(!kernel_mm_release(&forked.child)&&!kernel_mm_release(&removal_mm));punch.target=0;
    puts("PASS MADV_REMOVE closes an epoch while fork copies leaves; child publishes current backing");

    remove_store(&mm,address,0x65);struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_pin_user_page(&mm,address,KERNEL_MM_WRITE,&pin));
    remove_entered=remove_completed=0;remove_watch=1;
    assert(!pthread_create(&remover,0,remove_actor_run,&punch));
    while (!kernel_memory_object_epoch(vma.backing,&epoch)) sched_yield();
    assert(!__atomic_load_n(&remove_completed,__ATOMIC_ACQUIRE));
    kernel_mm_unpin_user_page(&pin);assert(!pthread_join(remover,0)&&!punch.result);remove_watch=0;
    assert(!remove_byte(&mm,address)&&!kernel_mm_munmap(&mm,address,BOAROS_PAGE_SIZE));
    puts("PASS MADV_REMOVE waits for an actual user-page pin before retiring shared backing");
}
struct remove_self_pin {uint64_t address;unsigned ready,go,done;};
static void *remove_self_pin_fault(void *opaque)
{
    struct remove_self_pin *state=opaque;mm_test_attach(5);mm_test_activate_task();mm_test_bind(&mm);
    kernel_cpu_current()->logical_id=1;struct kernel_mm_user_pin pin={0};
    assert(!kernel_mm_pin_user_page(&mm,state->address,KERNEL_MM_READ,&pin));
    __atomic_store_n(&state->ready,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&state->go,__ATOMIC_ACQUIRE)) sched_yield();
    uint64_t available=physical_page_available(mm.allocator);
    assert(kernel_mm_resolve_user_fault(&mm,state->address+BOAROS_PAGE_SIZE,KERNEL_MM_READ)==KERNEL_MM_STATUS_CONFLICT);
    assert(pin.registration==&pin&&kernel_io_context_current()->user_pins==&pin&&
        !kernel_io_context_current()->locks&&physical_page_available(mm.allocator)==available);
    kernel_mm_unpin_user_page(&pin);__atomic_store_n(&state->done,1,__ATOMIC_RELEASE);
    mm_test_finish();return 0;
}
static void remove_self_pin_case(void)
{
    uint64_t address;assert(!kernel_mm_mmap_anonymous(&mm,REMOVE_BASE,2*BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED_NOREPLACE|KERNEL_MM_MAP_SHARED,&address));
    remove_store(&mm,address,0x73);struct kernel_vma vma;assert(!kernel_mm_vma_lookup(&mm,address,&vma));
    struct kernel_mm earlier={0};assert(!kernel_mm_fork(&earlier,&mm));
    /* The newer registration is visited first. Its pin holds REMOVE before
     * the faulting MM callback, leaving that MM gate free while epoch closes. */
    struct kernel_mm_user_pin earlier_pin={0};assert(!kernel_mm_pin_user_page(&earlier,address,KERNEL_MM_READ,&earlier_pin));
    struct remove_self_pin state={.address=address};pthread_t faulter,remover;
    assert(!pthread_create(&faulter,0,remove_self_pin_fault,&state));
    while (!__atomic_load_n(&state.ready,__ATOMIC_ACQUIRE)) sched_yield();
    struct remove_actor punch={.address=address,.length=2*BOAROS_PAGE_SIZE,.target=&earlier};
    remove_entered=remove_completed=remove_self_pin_waited=0;remove_watch=remove_self_pin_watch=1;
    assert(!pthread_create(&remover,0,remove_actor_run,&punch));
    uint64_t epoch;while (!kernel_memory_object_epoch(vma.backing,&epoch)) sched_yield();
    __atomic_store_n(&state.go,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&state.done,__ATOMIC_ACQUIRE)&&
           !__atomic_load_n(&remove_self_pin_waited,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!__atomic_load_n(&remove_self_pin_waited,__ATOMIC_ACQUIRE)&&
        !__atomic_load_n(&remove_completed,__ATOMIC_ACQUIRE));
    kernel_mm_unpin_user_page(&earlier_pin);
    assert(!pthread_join(faulter,0)&&!pthread_join(remover,0)&&!punch.result);
    remove_watch=remove_self_pin_watch=0;
    assert(!remove_byte(&mm,address)&&!remove_byte(&mm,address+BOAROS_PAGE_SIZE));
    assert(!kernel_mm_release(&earlier)&&!kernel_mm_munmap(&mm,address,2*BOAROS_PAGE_SIZE));
    puts("PASS MADV_REMOVE closing epoch returns self-pinned fault conflict without waiting or losing owners");
}
static const struct kernel_vfs_backend *remove_real_backend;
static unsigned remove_backend_partial;
static int remove_backend_failure(struct kernel_vfs_node *node,unsigned mode,uint64_t offset,
    uint64_t length,uint64_t *actual,int *changed)
{
    *actual=node->size;*changed=0;
    if (remove_backend_partial) assert(!remove_real_backend->fallocate(node,mode,offset,length,actual,changed));
    return -KERNEL_EIO;
}
static void remove_file_case(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_open_file_description *file=0;int error;size_t done;
    assert(!kernel_open_file_create_mode(heap,mount,"/remove",0600,&file,&error)&&!error);
    file->open_flags=2;assert(!kernel_vfs_file_acquire_write(&file->file));
    unsigned char bytes[4*BOAROS_PAGE_SIZE];memset(bytes,0x45,sizeof(bytes));
    assert(!kernel_vfs_pwrite(&file->file,0,bytes,sizeof(bytes),&done)&&done==sizeof(bytes));
    remove_map(file,REMOVE_BASE,3*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,1);
    remove_map(file,REMOVE_ALIAS,3*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,1);
    remove_map(file,REMOVE_PRIVATE,3*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,0);
    remove_store(&mm,REMOVE_PRIVATE+BOAROS_PAGE_SIZE,0x76);
    assert(remove_byte(&mm,REMOVE_BASE+BOAROS_PAGE_SIZE)==0x45);
    assert(remove_byte(&mm,REMOVE_ALIAS+BOAROS_PAGE_SIZE)==0x45);
    struct kernel_mm child={0};assert(!kernel_mm_fork(&child,&mm));
    struct kernel_vma before,after;assert(!kernel_mm_vma_lookup(&mm,REMOVE_BASE,&before));
    assert(!kernel_open_file_seek(file,37));
    /* The writable permission is the mapping's maximum, not current mprotect. */
    assert(!kernel_mm_mprotect(&mm,REMOVE_BASE,3*BOAROS_PAGE_SIZE,KERNEL_MM_READ));
    assert(!kernel_mm_madvise(&mm,REMOVE_BASE+BOAROS_PAGE_SIZE,1,REMOVE_ADVICE));
    assert(kernel_open_file_size(file)==sizeof(bytes)&&kernel_open_file_offset(file)==37);
    assert(!kernel_mm_vma_lookup(&mm,REMOVE_BASE,&after)&&after.start==before.start&&
        after.end==before.end&&after.backing==before.backing&&after.backing_offset==before.backing_offset);
    struct kernel_mm_mapping pte;
    assert(kernel_mm_lookup(&mm,REMOVE_ALIAS+BOAROS_PAGE_SIZE,&pte)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(kernel_mm_lookup(&child,REMOVE_ALIAS+BOAROS_PAGE_SIZE,&pte)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!remove_byte(&mm,REMOVE_BASE+BOAROS_PAGE_SIZE)&&!remove_byte(&child,REMOVE_ALIAS+BOAROS_PAGE_SIZE));
    assert(remove_byte(&mm,REMOVE_PRIVATE+BOAROS_PAGE_SIZE)==0x76&&
        remove_byte(&child,REMOVE_PRIVATE+BOAROS_PAGE_SIZE)==0x76);
    assert(remove_byte(&mm,REMOVE_BASE)==0x45&&remove_byte(&mm,REMOVE_BASE+2*BOAROS_PAGE_SIZE)==0x45);
    assert(!kernel_open_file_pread(file,0,bytes,sizeof(bytes),&done)&&done==sizeof(bytes));
    for(size_t i=0;i<sizeof(bytes);i++)assert(bytes[i]==(i>=2*BOAROS_PAGE_SIZE&&i<3*BOAROS_PAGE_SIZE?0:0x45));
    assert(kernel_mm_madvise(&mm,REMOVE_PRIVATE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EACCES);
    assert(!kernel_mm_mlock(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,1));
    assert(kernel_mm_madvise(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EINVAL);
    assert(!kernel_mm_munlock(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE));
    assert(!kernel_mm_release(&child));
    struct kernel_open_file_description *readonly=0;
    assert(!kernel_open_file_create(heap,mount,"/remove",&readonly,&error)&&!error);
    remove_map(readonly,REMOVE_ALIAS+4*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,0,KERNEL_MM_READ,1);
    assert(kernel_mm_madvise(&mm,REMOVE_ALIAS+4*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EACCES);
    assert(!kernel_mm_munmap(&mm,REMOVE_ALIAS+4*BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE)&&!kernel_open_file_release(&readonly));
    struct kernel_vfs_node *node=kernel_open_file_node(file);struct kernel_vfs_instance *instance=node->instance;
    file->file.mount->read_only=1;
    assert(kernel_mm_madvise(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EROFS);
    file->file.mount->read_only=0;
    instance->read_only=1;
    assert(kernel_mm_madvise(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EROFS);
    instance->read_only=0;
    remove_real_backend=instance->ops;struct kernel_vfs_backend injected=*remove_real_backend;
    injected.fallocate=0;instance->ops=&injected;
    assert(kernel_mm_madvise(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EOPNOTSUPP);
    injected.fallocate=remove_backend_failure;
    assert(kernel_mm_madvise(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EIO);
    instance->ops=remove_real_backend;assert(remove_byte(&mm,REMOVE_BASE)==0x45);
    instance->ops=&injected;remove_backend_partial=1;
    assert(kernel_mm_madvise(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_EIO);
    remove_backend_partial=0;instance->ops=remove_real_backend;assert(!remove_byte(&mm,REMOVE_BASE));
    assert(!kernel_mm_munmap(&mm,REMOVE_BASE+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    assert(kernel_mm_madvise(&mm,REMOVE_BASE,3*BOAROS_PAGE_SIZE,REMOVE_ADVICE)==-KERNEL_ENOMEM);
    assert(!remove_byte(&mm,REMOVE_BASE)&&!remove_byte(&mm,REMOVE_BASE+2*BOAROS_PAGE_SIZE));
    assert(!kernel_mm_munmap(&mm,REMOVE_BASE,3*BOAROS_PAGE_SIZE));
    assert(!kernel_mm_munmap(&mm,REMOVE_ALIAS,3*BOAROS_PAGE_SIZE));
    assert(!kernel_mm_munmap(&mm,REMOVE_PRIVATE,3*BOAROS_PAGE_SIZE));
    assert(file->references==1&&!kernel_open_file_release(&file));
    puts("PASS MADV_REMOVE file offset, rounding, readonly mprotect, cross-MM aliases and private COW");
}
static void remove_last_file(struct kernel_heap *heap,struct kernel_vfs_mount *mount)
{
    struct kernel_open_file_description *file=0;int error;size_t done;uint64_t address;
    assert(!kernel_open_file_create_mode(heap,mount,"/remove-last",0600,&file,&error)&&!error);
    file->open_flags=2;assert(!kernel_vfs_file_acquire_write(&file->file));
    assert(!kernel_vfs_pwrite(&file->file,0,"old-source",10,&done)&&done==10);
    remove_map(file,REMOVE_BASE,BOAROS_PAGE_SIZE,0,KERNEL_MM_READ|KERNEL_MM_WRITE,1);
    struct kernel_vfs_node *node=kernel_open_file_node(file);assert(!kernel_vfs_node_acquire(node));
    struct kernel_open_file_description *borrowed=file;
    remove_file_ready=remove_continue_file=0;remove_pause_file=1;
    struct remove_actor punch={.address=REMOVE_BASE,.length=BOAROS_PAGE_SIZE};pthread_t worker;
    assert(!pthread_create(&worker,0,remove_actor_run,&punch));
    while (!__atomic_load_n(&remove_file_ready,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_munmap(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE)&&!kernel_open_file_release(&file));
    assert(borrowed->references==1&&node->open_files);
    assert(!kernel_mm_mmap_anonymous(&mm,REMOVE_BASE,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED_NOREPLACE,&address));remove_store(&mm,address,0x29);
    __atomic_store_n(&remove_continue_file,1,__ATOMIC_RELEASE);assert(!pthread_join(worker,0)&&!punch.result);
    /* The production user-return path drains a CPU1 final-close owner on
     * CPU0, after all MM and I/O qualifications have ended. */
    kernel_mm_drain_sources(&mm);
    assert(!node->open_files&&remove_byte(&mm,address)==0x29);
    assert(!kernel_open_file_create(heap,mount,"/remove-last",&file,&error)&&!error);
    unsigned char bytes[10];assert(!kernel_open_file_pread(file,0,bytes,sizeof(bytes),&done)&&done==sizeof(bytes));
    for (unsigned i=0;i<sizeof(bytes);i++) assert(!bytes[i]);
    assert(!kernel_open_file_release(&file)&&!kernel_vfs_node_release(&node)&&!kernel_mm_munmap(&mm,address,BOAROS_PAGE_SIZE));
    puts("PASS MADV_REMOVE retains original OFD through last unmap, close and same-address replacement");
}
static void remove_sysv_case(struct kernel_heap *heap,struct physical_page_allocator *pages)
{
    assert(!kernel_shm_init(heap,pages));int32_t id;uint64_t address,alias;
    assert(!kernel_shm_get(0,0,3*BOAROS_PAGE_SIZE,0600,&id));
    assert(!kernel_shm_at_mm(&mm,1,id,REMOVE_BASE,0,&address));
    assert(!kernel_shm_at_mm(&mm,1,id,REMOVE_ALIAS,0,&alias));
    remove_store(&mm,address+BOAROS_PAGE_SIZE,0x69);
    assert(remove_byte(&mm,alias+BOAROS_PAGE_SIZE)==0x69);
    struct kernel_mm child={0};assert(!kernel_mm_fork(&child,&mm));
    int64_t result;assert(!kernel_shm_ctl_mm(0,id,KERNEL_IPC_RMID,0,&result)&&!result);
    assert(!kernel_mm_madvise(&mm,address+BOAROS_PAGE_SIZE,1,REMOVE_ADVICE));
    assert(!remove_byte(&mm,alias+BOAROS_PAGE_SIZE)&&!remove_byte(&child,address+BOAROS_PAGE_SIZE));
    assert(!kernel_mm_release(&child)&&!kernel_shm_dt_mm(&mm,address)&&!kernel_shm_dt_mm(&mm,alias));
    puts("PASS MADV_REMOVE shared SysV backing survives RMID, invalidates attachments and returns owners");
}
int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,0,_IONBF,0);mm_test_attach(0);mm_test_activate_task();kernel_futex_init();
    struct physical_page_allocator pages={0},backend_pages={0};
    struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t baseline=physical_page_available(&pages),backend_baseline=physical_page_available(&backend_pages);
    struct arch_mmu_page_table kernel={0};struct arch_mmu_user_space user={0};
    assert(!riscv_sv39_page_table_init(&kernel,&pages));kernel.state=ARCH_MMU_STATE_ACTIVE;
    assert(!arch_mmu_user_space_init(&user,&pages,&kernel)&&!kernel_mm_create(&mm,&user)&&
        !kernel_mm_vma_enable(&mm,&heap)&&!kernel_mm_brk_initialize(&mm,0x10000,0x20000));
    assert(!kernel_mm_context(&mm,&context));mm_test_context=context;
    remove_self_pin_case();
    if (!getenv("MADVISE_REMOVE_SELF_PIN_ONLY")) {
        remove_object_races();remove_sysv_case(&heap,&pages);
        struct kernel_vfs_mount *memory=0;assert(!kernel_tmpfs_create(&heap,0,"",&memory));
        remove_file_case(&heap,memory);remove_last_file(&heap,memory);assert(!kernel_vfs_unmount(memory));
        image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
        struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
        struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,
            .capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
        assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(101)));
        struct kernel_vfs_mount mount={0};assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
        assert(!kernel_vfs_start_journal_worker(&mount)&&!kernel_page_cache_start_worker(&cache));
        remove_file_case(&heap,&mount);remove_last_file(&heap,&mount);kernel_page_cache_stop_worker(&cache);
        assert(!kernel_vfs_unmount(&mount)&&!kernel_page_cache_destroy(&cache)&&!kernel_block_unregister(&disk)&&!close(image));
    }
    mm_test_context=0;assert(!kernel_mm_release(&mm)&&!physical_page_release(&pages,kernel.root_address));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);assert(!stats.live_allocations&&!stats.current_pages);
    kernel_heap_get_statistics(&backend_heap,&stats);assert(!stats.live_allocations&&!stats.current_pages);
    assert(!physical_page_allocator_audit(&pages)&&!physical_page_allocator_audit(&backend_pages)&&
        physical_page_available(&pages)==baseline&&physical_page_available(&backend_pages)==backend_baseline);
    puts("PASS MADV_REMOVE owners returned");return 0;
}
