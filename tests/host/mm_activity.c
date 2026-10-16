/* 模型只替换CSR/跨核通知；活动集合与切换交接来自生产MM。 */
#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/uaccess.h>
#include <kernel/cpu.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#if BOAROS_USER_SMP && defined(REMOTE_USER_CASE)
#include "../../kernel/sched/private.h"
#include "../../mm/uaccess_internal.h"
void mm_test_bind(struct kernel_mm *);
unsigned mm_test_parked(unsigned);
#endif

void mm_test_attach(unsigned);
#ifdef REMOTE_USER_CASE
extern enum kernel_mm_status kernel_mm_enable_remote_user_execution(struct kernel_mm *);
#endif
extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[4 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_mm mm;
static uint64_t context, kernel_context;
static unsigned sampled, entered, allow_send, pause_exit, exiting, allow_exit;
static _Thread_local unsigned flushes, instruction_sync;
#ifdef REMOTE_USER_CASE
static unsigned copying, cow_fences;
static uint64_t cow_old;
static unsigned sparse, walk_calls;
static unsigned expect_nonleaf_sync;
static unsigned fail_copy;
static uint64_t failed_page;
static unsigned table_oom, table_allocations, oom_preparing, oom_observed;
static unsigned allocation_calls;
static uint64_t *oom_root_slot, observed_parent;
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *address)
{
    __atomic_fetch_add(&allocation_calls,1,__ATOMIC_RELAXED);
    if (table_oom && ++table_allocations==2) {
        __atomic_store_n(&oom_preparing,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&oom_observed,__ATOMIC_ACQUIRE)) sched_yield();
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }
    enum physical_page_status status=__real_physical_page_allocate(a,address);
    if (!status && fail_copy) failed_page=*address;
    return status;
}
enum physical_page_status __real_physical_page_map_owned(const struct physical_page_allocator *,uint64_t,void **);
enum physical_page_status __wrap_physical_page_map_owned(const struct physical_page_allocator *a,uint64_t address,void **pointer)
{
    if (fail_copy && address==failed_page) { fail_copy=0; return PHYSICAL_PAGE_STATUS_INVALID; }
    return __real_physical_page_map_owned(a,address,pointer);
}
enum riscv_sv39_status __real_riscv_sv39_user_next_owned(struct riscv_sv39_user_space *,uint64_t *,uint64_t *);
enum riscv_sv39_status __wrap_riscv_sv39_user_next_owned(struct riscv_sv39_user_space *space,uint64_t *cursor,uint64_t *address)
{ if (sparse) walk_calls++; return __real_riscv_sv39_user_next_owned(space,cursor,address); }
#endif
static void await(unsigned *flag)
{ while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) sched_yield(); }
static void publish(unsigned *flag)
{ __atomic_store_n(flag, 1, __ATOMIC_RELEASE); }
static void *page_access(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer, uint64_t *address)
{ *address = (uintptr_t)pointer; return 1; }
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void kernel_assert_service_context(void) { assert(kernel_cpu_current()->logical_id == 0); }
void kernel_cpu_flush_notifications(void) {}
void kernel_cpu_notify(struct kernel_cpu *cpu, unsigned events) { (void)cpu; (void)events; abort(); }
void mm_test_fence(uint64_t address, int instructions)
{
    (void)address;
    if (instructions) instruction_sync++; else flushes++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (kernel_cpu_current()->logical_id == 1 && mm_test_context == kernel_context &&
        __atomic_load_n(&pause_exit, __ATOMIC_ACQUIRE)) {
        assert(!kernel_cpu_current()->raw_locks);
        publish(&exiting); await(&allow_exit);
    }
}
void riscv_smp_sync_user(uint32_t targets, uint64_t address, uint64_t size, int instructions)
{
    (void)address; (void)size; (void)instructions;
    assert(!kernel_cpu_current()->raw_locks && targets == 0);
#ifdef REMOTE_USER_CASE
    if (expect_nonleaf_sync) { assert(address==0 && size==UINT64_MAX); expect_nonleaf_sync=0; }
    if (copying) {
        uint32_t references;
        assert(!physical_page_reference_count(&allocator, cow_old, &references));
        assert(references >= 2); /* child与尚未确认失效的旧父PTE owner均存在。 */
        cow_fences++;
    }
#endif
    /* 修改者已采样空集合；CPU1随后首次进入，不能靠本次远端通知。 */
    publish(&sampled); await(&allow_send);
}
static void *modify(void *unused)
{
    (void)unused; mm_test_attach(0); mm_test_context = context;
    assert(!kernel_mm_mprotect(&mm, 0x40000, BOAROS_PAGE_SIZE, KERNEL_MM_READ));
    return 0;
}
static void *enter(void *unused)
{
    (void)unused; mm_test_attach(1); kernel_cpu_current()->logical_id = 1;
    await(&sampled);
    assert(!kernel_mm_activate(&mm, kernel_context));
    assert(flushes >= 2 && instruction_sync == 1);
    unsigned before = flushes;
    assert(!kernel_mm_activate(&mm, kernel_context));
    assert(flushes == before + 1 && instruction_sync == 2); /* 同SATP也失效。 */
    publish(&entered); await(&pause_exit);
    assert(!kernel_mm_activate(0, kernel_context));
    assert(!kernel_cpu_current()->active_mm_record);
    return 0;
}
#ifdef REMOTE_USER_CASE
static void *observe_partial_fault(void *unused)
{
    (void)unused; mm_test_attach(1); kernel_cpu_current()->logical_id=1;
    await(&oom_preparing);
    assert(!kernel_mm_activate(&mm,kernel_context));
    observed_parent=__atomic_load_n(oom_root_slot,__ATOMIC_ACQUIRE);
    assert(!kernel_mm_activate(0,kernel_context));
    publish(&oom_observed);
    return 0;
}
#if BOAROS_USER_SMP
static unsigned resident_protect, resident_started, resident_done;
static struct kernel_mm resident_child;
static void *resident_mutate(void *unused)
{
    (void)unused; mm_test_attach(3); kernel_cpu_current()->logical_id=0;
    mm_test_context=context; publish(&resident_started);
    if (resident_protect)
        assert(!kernel_mm_mprotect(&mm,0x40000,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
    else assert(!kernel_mm_fork(&resident_child,&mm));
    assert(!kernel_io_context_current()->user_pins && !kernel_io_context_current()->locks);
    publish(&resident_done); return NULL;
}
static void resident_enter(void)
{
    mm_test_attach(1); mm_test_context=context; mm_test_bind(&mm);
    kernel_task_current()->arch.user_mode=1;
}
static void resident_leave(void)
{
    assert(!kernel_io_context_current()->user_pins && !kernel_io_context_current()->locks);
    kernel_task_current()->arch.user_mode=0; mm_test_bind(NULL);
    kernel_cpu_current()->logical_id=0;
}
static void resident_wait_drain(unsigned before)
{
    await(&resident_started);
    /* 只有该actor3变更者在运行；其park必须等待这次真实pin归还。 */
    while (mm_test_parked(3)==before && !__atomic_load_n(&resident_done,__ATOMIC_ACQUIRE)) sched_yield();
    assert(mm_test_parked(3)>before && !__atomic_load_n(&resident_done,__ATOMIC_ACQUIRE));
}
static void resident_pin_cases(uint64_t address)
{
    struct kernel_mm_user_pin pin={0}, rejected={0};
    pthread_t mutator;
    resident_enter();
    unsigned calls=__atomic_load_n(&allocation_calls,__ATOMIC_RELAXED);
    assert(!kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_WRITE,&pin));
    assert(__atomic_load_n(&allocation_calls,__ATOMIC_RELAXED)==calls);
    unsigned before=mm_test_parked(3), local_before=mm_test_parked(1);
    resident_started=resident_done=resident_protect=0;
    assert(!pthread_create(&mutator,NULL,resident_mutate,NULL));
    resident_wait_drain(before);
    assert(kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_READ,&rejected)==KERNEL_MM_STATUS_CONFLICT);
    assert(!rejected.registration && mm_test_parked(1)==local_before);
    ((unsigned char *)pin.page)[0]=0x66;
    kernel_mm_unpin_user_page(&pin);
    assert(!pthread_join(mutator,NULL)); resident_leave();
    unsigned char byte=0; size_t copied;
    assert(!kernel_copy_from_user(&resident_child,&byte,address,1,&copied) && copied==1 && byte==0x66);
    resident_enter(); calls=__atomic_load_n(&allocation_calls,__ATOMIC_RELAXED);
    assert(kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_WRITE,&rejected)==KERNEL_MM_STATUS_CONFLICT);
    assert(!rejected.registration && __atomic_load_n(&allocation_calls,__ATOMIC_RELAXED)==calls);
    struct kernel_uaccess_write_cursor cursor={.resident_only=1};
    byte=0x77; copied=99;
    assert(kernel_uaccess_write_cursor_copy(&mm,&cursor,address,&byte,1,&copied)==KERNEL_UACCESS_STATUS_RETRY);
    assert(!copied && !cursor.pin.registration); kernel_uaccess_write_cursor_finish(&cursor);
    assert(!kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_READ,&pin));
    assert(((unsigned char *)pin.page)[0]==0x66); kernel_mm_unpin_user_page(&pin);
    resident_leave(); byte=0x77;
    assert(!kernel_copy_to_user(&mm,address,&byte,1,&copied) && copied==1);
    assert(!kernel_copy_from_user(&resident_child,&byte,address,1,&copied) && byte==0x66);
    assert(!kernel_mm_release(&resident_child));

    resident_enter();
    assert(!kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_WRITE,&pin));
    before=mm_test_parked(3); local_before=mm_test_parked(1);
    resident_started=resident_done=0; resident_protect=1;
    assert(!pthread_create(&mutator,NULL,resident_mutate,NULL));
    resident_wait_drain(before);
    assert(kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_WRITE,&rejected)==KERNEL_MM_STATUS_CONFLICT);
    assert(!rejected.registration && mm_test_parked(1)==local_before);
    ((unsigned char *)pin.page)[1]=0x88; kernel_mm_unpin_user_page(&pin);
    assert(!pthread_join(mutator,NULL));
    assert(kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_WRITE,&rejected)==KERNEL_MM_STATUS_CONFLICT);
    assert(!rejected.registration && !kernel_mm_try_pin_user_page(&mm,address,KERNEL_MM_READ,&pin));
    assert(((unsigned char *)pin.page)[1]==0x88); kernel_mm_unpin_user_page(&pin);
    resident_leave();
    assert(!kernel_mm_mprotect(&mm,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
    uint64_t placed;
    assert(!kernel_mm_mmap_anonymous(&mm,address+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&placed));
    resident_enter(); calls=__atomic_load_n(&allocation_calls,__ATOMIC_RELAXED);
    const unsigned char source[]={0x95,0x96}; unsigned char imported[]={0,0};
    cursor=(struct kernel_uaccess_write_cursor){.resident_only=1};
    assert(kernel_uaccess_write_cursor_copy(&mm,&cursor,address+BOAROS_PAGE_SIZE-1,source,2,&copied)==KERNEL_UACCESS_STATUS_RETRY);
    assert(copied==1 && !cursor.pin.registration); kernel_uaccess_write_cursor_finish(&cursor);
    assert(kernel_copy_from_user_resident(&mm,imported,address+BOAROS_PAGE_SIZE-1,2,&copied)==KERNEL_UACCESS_STATUS_RETRY);
    assert(copied==1 && imported[0]==0x95 && !imported[1]);
    assert(!kernel_io_context_current()->user_pins && __atomic_load_n(&allocation_calls,__ATOMIC_RELAXED)==calls);
    resident_leave();
    assert(!kernel_copy_from_user(&mm,&byte,address+BOAROS_PAGE_SIZE-1,1,&copied) && byte==0x95);
    assert(!kernel_mm_munmap(&mm,address+BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE));
    byte=19; assert(!kernel_copy_to_user(&mm,address,&byte,1,&copied));
    struct kernel_mm_concurrency_state state;
    kernel_mm_get_concurrency_state(&mm,&state);
    assert(!state.pins && !state.write_pins && !state.pending_retirement);
    puts("MM resident pin fork/mprotect drain, COW retry and exact copy/import prefix verified");
}
#endif
#endif
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout = {0};
    layout.usable_count = 1; layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    assert(!physical_page_allocator_init(&allocator, &layout));
    assert(!physical_page_allocator_bind_access(&allocator, page_access));
    assert(!physical_page_allocator_finalize(&allocator));
    assert(!kernel_heap_init(&heap, &allocator, page_address));
    struct arch_mmu_page_table table = {0}; struct arch_mmu_user_space space = {0};
    assert(!riscv_sv39_page_table_init(&table, &allocator)); table.state = ARCH_MMU_STATE_ACTIVE;
    kernel_context = (UINT64_C(8) << 60) | (table.root_address >> 12);
    uint64_t baseline = physical_page_available(&allocator);
    assert(!arch_mmu_user_space_init(&space, &allocator, &table));
    assert(!kernel_mm_create(&mm, &space) && !kernel_mm_vma_enable(&mm, &heap));
    assert(!kernel_mm_brk_initialize(&mm, 0x10000, 0x20000));
    assert(!kernel_mm_context(&mm, &context)); mm_test_context = context;
    uint64_t address; size_t copied; unsigned char byte = 19;
    assert(!kernel_mm_mmap_anonymous(&mm, 0x40000, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &address));
    assert(!kernel_copy_to_user(&mm, address, &byte, 1, &copied));
#ifdef REMOTE_USER_CASE
    assert(!kernel_mm_enable_remote_user_execution(&mm));
#else
    assert(!kernel_mm_enable_concurrency(&mm));
#endif
    pthread_t modifier, visitor;
    assert(!pthread_create(&visitor, 0, enter, 0) && !pthread_create(&modifier, 0, modify, 0));
    await(&entered);
    struct kernel_mm_concurrency_state state;
    kernel_mm_get_concurrency_state(&mm, &state); assert(state.active_cpus == 2);
    publish(&allow_send); assert(!pthread_join(modifier, 0));
    publish(&pause_exit); await(&exiting);
    kernel_mm_get_concurrency_state(&mm, &state); assert(state.active_cpus == 2);
    publish(&allow_exit); assert(!pthread_join(visitor, 0));
    kernel_mm_get_concurrency_state(&mm, &state); assert(!state.active_cpus && !state.pending_retirement);
#ifdef REMOTE_USER_CASE
    assert(!kernel_mm_mprotect(&mm, address, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
#if BOAROS_USER_SMP
    resident_pin_cases(address);
#endif
    struct kernel_mm child = {0};
    assert(!kernel_mm_fork(&child, &mm));
    struct kernel_mm_mapping original;
    assert(!kernel_mm_lookup(&mm,address,&original)); cow_old=original.physical_address;
    uint64_t before_failure=physical_page_available(&allocator);
    fail_copy=1;
    assert(kernel_mm_resolve_user_fault(&mm,address,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_STATE);
    assert(!fail_copy && physical_page_available(&allocator)==before_failure);
    assert(!kernel_mm_lookup(&mm,address,&original) && original.physical_address==cow_old && !(original.permissions&KERNEL_MM_WRITE));
    copying=1; byte=27;
    assert(!kernel_copy_to_user(&mm,address,&byte,1,&copied) && copied==1);
    copying=0; assert(cow_fences);
    assert(!kernel_copy_from_user(&child,&byte,address,1,&copied) && byte==19);
    assert(!kernel_mm_release(&child));
    uint64_t failed_va=UINT64_C(0x800000000), placed;
    assert(!kernel_mm_mmap_anonymous(&mm,failed_va,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&placed));
    uint64_t *user_root=(void *)(uintptr_t)((context&((UINT64_C(1)<<44)-1))<<12);
    oom_root_slot=&user_root[(failed_va>>30)&511];
    assert(!*oom_root_slot);
    before_failure=physical_page_available(&allocator);
    table_oom=1; pthread_t observer;
    assert(!pthread_create(&observer,0,observe_partial_fault,0));
    assert(kernel_mm_resolve_user_fault(&mm,failed_va,KERNEL_MM_WRITE)==KERNEL_MM_STATUS_NO_MEMORY);
    assert(!pthread_join(observer,0)); table_oom=0;
    /* 远端在二级准备失败前不能见到随后直接归还的非叶owner。 */
    assert(!observed_parent && !*oom_root_slot && physical_page_available(&allocator)==before_failure);
    assert(kernel_mm_lookup(&mm,failed_va,&original)==KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!kernel_mm_munmap(&mm,failed_va,BOAROS_PAGE_SIZE));
    uint64_t far=UINT64_C(0x1000000000);
    assert(!kernel_mm_mmap_anonymous(&mm,far,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE,
        KERNEL_MM_MAP_FIXED,&placed) && placed==far);
    expect_nonleaf_sync=1;
    assert(!kernel_copy_to_user(&mm,far,&byte,1,&copied) && !expect_nonleaf_sync);
    sparse=1;
    assert(!kernel_mm_munmap(&mm,address,far+BOAROS_PAGE_SIZE-address));
    sparse=0; assert(walk_calls<=4); /* 64GiB空洞不按虚拟页枚举。 */
#endif
    mm_test_context = 0; assert(!kernel_mm_release(&mm));
    assert(physical_page_available(&allocator) == baseline && !physical_page_allocator_audit(&allocator));
    puts("MM sampled-target late entry, same-root local invalidation and leave-before-retirement verified");
}
