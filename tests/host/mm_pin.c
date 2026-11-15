#define _POSIX_C_SOURCE 200809L
#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/uaccess.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern _Thread_local uint64_t mm_test_context;
extern _Thread_local int mm_test_wrong_owner;
void mm_test_attach(unsigned);
static unsigned char pool[4 * 1024 * 1024] __attribute__((aligned(16384)));
void kernel_console_putc(char c) { fputc(c, stderr); }
static _Thread_local unsigned tlb_fences,instruction_fences;
static unsigned root_fence_watch,root_fence_entered,root_fence_ack;
static void root_fence_confirm(void)
{
    if (__atomic_load_n(&root_fence_watch,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&root_fence_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&root_fence_ack,__ATOMIC_ACQUIRE))sched_yield();
    }
}
void riscv_smp_sync_kernel(uint64_t address,uint64_t size)
{ assert(!address && size==UINT64_MAX);mm_test_fence(address,0);root_fence_confirm(); }
void riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{
    assert(!(targets&~3U));
    mm_test_fence(size==BOAROS_PAGE_SIZE?address:UINT64_MAX,0);
    if (instructions) mm_test_fence(0,1);
    if (targets&2U) { assert(root_fence_watch && !instructions);root_fence_confirm(); }
}
void riscv_smp_sync_instructions(uint32_t targets)
{
    /* A kernel-only root borrower executes no user instructions. Its next
     * user activation must observe the published instruction generation. */
    assert(!targets);mm_test_fence(0,1);
}
void mm_test_fence(uint64_t address, int instructions)
{
    (void)address;
    if (instructions) instruction_fences++;else tlb_fences++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
void mm_test_unavailable(void) { fputs("excluded business path reached\n", stderr); abort(); }
struct root_borrow_request {
    struct kernel_mm *mm;struct physical_page_allocator *allocator;
    uint64_t context,root,page;unsigned entered,release;
};
static void *remote_root_borrow(void *argument)
{
    struct root_borrow_request *request=argument;
    mm_test_attach(1);kernel_cpu_current()->logical_id=1;mm_test_context=0;
    assert(!kernel_mm_activate(request->mm,0) && !kernel_mm_activate_for_task(NULL,0));
    __atomic_store_n(&request->entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&root_fence_entered,__ATOMIC_ACQUIRE))sched_yield();
    uint32_t references;void *bytes;
    assert(!physical_page_reference_count(request->allocator,request->root,&references) && references==2);
    assert(!physical_page_reference_count(request->allocator,request->page,&references) && references==1);
    assert(!physical_page_map_owned(request->allocator,request->root,&bytes));
    for (unsigned i=0;i<256;i++)assert(!((uint64_t *)bytes)[i]);
    __atomic_store_n(&root_fence_ack,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&request->release,__ATOMIC_ACQUIRE))sched_yield();
    /* Every user owner is gone. Root and record storage remain solely for
     * this CPU's mask removal and final physical owner return. */
    assert(mm_test_context==request->context && !kernel_cpu_current()->active_mm_record);
    assert(!physical_page_reference_count(request->allocator,request->root,&references) && references==1);
    assert(!kernel_mm_activate(NULL,0) && !kernel_cpu_current()->borrowed_mm_allocator);
    return NULL;
}
static void kernel_root_borrow_case(struct physical_page_allocator *allocator,
    struct kernel_heap *heap,struct arch_mmu_page_table *kernel)
{
    pid_t child=fork();assert(child>=0);
    if (!child) {
        struct arch_mmu_user_space space={0};struct kernel_mm mm={0};
        uint64_t baseline=physical_page_available(allocator),address,context;
        assert(!arch_mmu_user_space_init(&space,allocator,kernel) && !kernel_mm_create(&mm,&space));
        assert(!kernel_mm_vma_enable(&mm,heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
        assert(!kernel_mm_mmap_anonymous(&mm,0x80000,BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
        assert(!kernel_mm_resolve_user_fault(&mm,address,KERNEL_MM_WRITE));
        assert(!kernel_mm_context(&mm,&context) && !kernel_mm_activate(&mm,0));
        riscv_mmu_svvptc=1;
        uint64_t root=(context&((UINT64_C(1)<<44)-1))<<BOAROS_PAGE_SHIFT;uint32_t references;
        assert(!physical_page_reference_count(allocator,root,&references) && references==1);
        assert(!kernel_mm_activate_for_task(NULL,0));
        assert(mm_test_context==context && !kernel_cpu_current()->active_mm_record);
        assert(!physical_page_reference_count(allocator,root,&references) && references==2);
        unsigned before=tlb_fences;
        assert(!kernel_mm_activate_for_task(&mm,0) && mm_test_context==context && tlb_fences==before);
        assert(!physical_page_reference_count(allocator,root,&references) && references==1);
        assert(!kernel_mm_activate_for_task(NULL,0));
        assert(!kernel_mm_mprotect(&mm,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
        before=tlb_fences;
        assert(!kernel_mm_activate_for_task(&mm,0) && tlb_fences==before+1);
        assert(!kernel_mm_activate_for_task(NULL,0));
        /* Last MM owner ends userspace even if this CPU retained the root. */
        assert(!kernel_mm_release(&mm) && !mm_test_context);
        assert(physical_page_available(allocator)==baseline && !physical_page_allocator_audit(allocator));
        for (unsigned live_unmap=0;live_unmap<2;live_unmap++) {
        mm=(struct kernel_mm){0};space=(struct arch_mmu_user_space){0};
        assert(!arch_mmu_user_space_init(&space,allocator,kernel) && !kernel_mm_create(&mm,&space));
        assert(!kernel_mm_vma_enable(&mm,heap) && !kernel_mm_brk_initialize(&mm,0x10000,0x20000));
        assert(!kernel_mm_mmap_anonymous(&mm,0x80000,BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
        assert(!kernel_mm_resolve_user_fault(&mm,address,KERNEL_MM_WRITE) && !kernel_mm_context(&mm,&context));
        struct kernel_mm_mapping mapped;assert(!kernel_mm_lookup(&mm,address,&mapped));
        struct root_borrow_request request={.mm=&mm,.allocator=allocator,.context=context,
            .root=(context&((UINT64_C(1)<<44)-1))<<BOAROS_PAGE_SHIFT,.page=mapped.physical_address};
        root_fence_watch=1;root_fence_entered=root_fence_ack=0;
        pthread_t remote;assert(!pthread_create(&remote,NULL,remote_root_borrow,&request));
        while (!__atomic_load_n(&request.entered,__ATOMIC_ACQUIRE))sched_yield();
        assert(!kernel_mm_flush_instructions(&mm,0));
        if (live_unmap) {
            assert(!kernel_mm_munmap(&mm,address,BOAROS_PAGE_SIZE));
            assert(root_fence_entered && root_fence_ack);
        }
        assert(!kernel_mm_release(&mm));
        assert(root_fence_entered && root_fence_ack && physical_page_available(allocator)==baseline-2);
        __atomic_store_n(&request.release,1,__ATOMIC_RELEASE);
        assert(!pthread_join(remote,NULL));root_fence_watch=0;
        assert(physical_page_available(allocator)==baseline && !physical_page_allocator_audit(allocator));
        }
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("kernel root loan: unchanged return, generation recheck, userspace teardown and final page owner verified");
}
static unsigned watch_pin_entries,pin_entries;
enum kernel_mm_status __real_kernel_mm_pin_user_page(const struct kernel_mm *,uint64_t,uint32_t,struct kernel_mm_user_pin *);
enum kernel_mm_status __wrap_kernel_mm_pin_user_page(const struct kernel_mm *mm,uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{
    if(watch_pin_entries)pin_entries++;
    return __real_kernel_mm_pin_user_page(mm,address,access,pin);
}
static void cold_copy_case(struct kernel_mm *mm)
{
    uint64_t address;size_t copied=0;unsigned char value=0x37;
    assert(!kernel_mm_mmap_anonymous(mm,0x80000,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
    watch_pin_entries=1;pin_entries=0;
    assert(!kernel_copy_to_user(mm,address,&value,1,&copied) && copied==1);
    watch_pin_entries=0;
    printf("cold copy pin entries=%u\n",pin_entries);fflush(stdout);
    assert(pin_entries==1);
    value=0;assert(!kernel_copy_from_user(mm,&value,address,1,&copied) && copied==1 && value==0x37);
    assert(!kernel_mm_munmap(mm,address,BOAROS_PAGE_SIZE));
}
static void new_mapping_sync_case(struct kernel_mm *mm)
{
    const uint64_t hint=0x180000;
    uint64_t address;
    for (unsigned supported=0;supported<=1;supported++) {
        riscv_mmu_svvptc=supported;
        assert(!kernel_mm_mmap_anonymous(mm,hint,BOAROS_PAGE_SIZE,
            KERNEL_MM_READ|KERNEL_MM_WRITE,KERNEL_MM_MAP_FIXED,&address));
        tlb_fences=instruction_fences=0;
        assert(!kernel_mm_resolve_user_fault(mm,address,KERNEL_MM_WRITE));
        printf("new anonymous mapping Svvptc=%u TLB fences=%u\n",supported,tlb_fences);fflush(stdout);
        assert(supported ? !tlb_fences : tlb_fences!=0);
        assert(!instruction_fences);
        tlb_fences=0;
        assert(!kernel_mm_mprotect(mm,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ));
        assert(tlb_fences && "Svvptc must not bypass permission invalidation");
        assert(!kernel_mm_mprotect(mm,address,BOAROS_PAGE_SIZE,KERNEL_MM_READ|KERNEL_MM_WRITE));
        struct kernel_mm child={0};struct kernel_mm_mapping old,current;
        assert(!kernel_mm_lookup(mm,address,&old) && !kernel_mm_fork(&child,mm));
        tlb_fences=0;
        assert(!kernel_mm_resolve_user_fault(mm,address,KERNEL_MM_WRITE));
        assert(tlb_fences && !kernel_mm_lookup(mm,address,&current) && current.physical_address!=old.physical_address);
        assert(!kernel_mm_release(&child));
        tlb_fences=0;assert(!kernel_mm_munmap(mm,address,BOAROS_PAGE_SIZE));
        assert(tlb_fences && "retired pages still wait for invalidation");
    }
    riscv_mmu_svvptc=1;
    assert(!kernel_mm_mmap_anonymous(mm,hint,BOAROS_PAGE_SIZE,
        KERNEL_MM_READ|KERNEL_MM_WRITE|KERNEL_MM_EXECUTE,KERNEL_MM_MAP_FIXED,&address));
    tlb_fences=instruction_fences=0;
    assert(!kernel_mm_resolve_user_fault(mm,address,KERNEL_MM_WRITE));
    assert(!tlb_fences && instruction_fences);
    assert(!kernel_mm_munmap(mm,address,BOAROS_PAGE_SIZE));
    riscv_mmu_svvptc=0;
}

static uint64_t watched_record;
static unsigned record_resolutions;
static uint64_t qualifying_page;
static unsigned qualifying_entered, qualifying_release;
static unsigned hold_metadata, metadata_entered, metadata_release;
static void await(unsigned *value)
{ while (!__atomic_load_n(value, __ATOMIC_ACQUIRE)) sched_yield(); }
static void *page_access(uint64_t address)
{
    if (address == watched_record) record_resolutions++;
    if (__atomic_load_n(&hold_metadata, __ATOMIC_ACQUIRE) &&
        kernel_cpu_current()->hardware_id == 1 && kernel_cpu_current()->raw_locks &&
        kernel_cpu_current()->raw_locks->lock->rank == KERNEL_RAW_RANK_MM) {
        __atomic_store_n(&metadata_entered, 1, __ATOMIC_RELEASE);
        await(&metadata_release);
    }
    if (address == qualifying_page && kernel_cpu_current()->hardware_id == 1) {
        /* 停在PTE资格内、真实页引用取得前；另一读者必须能完成同页pin。 */
        __atomic_store_n(&qualifying_entered, 1, __ATOMIC_RELEASE);
        await(&qualifying_release);
    }
    return (void *)(uintptr_t)address;
}
static int page_address(const void *pointer, uint64_t *address)
{ *address = (uintptr_t)pointer; return 1; }
static unsigned raw_acquisitions, checked_resolutions;
void __real_kernel_raw_lock_acquire(struct kernel_raw_lock *, struct kernel_raw_guard *);
void __wrap_kernel_raw_lock_acquire(struct kernel_raw_lock *lock, struct kernel_raw_guard *guard)
{ __atomic_fetch_add(&raw_acquisitions, 1, __ATOMIC_RELAXED); __real_kernel_raw_lock_acquire(lock, guard); }
enum physical_page_status __real_physical_page_resolve(const struct physical_page_allocator *, uint64_t, void **);
enum physical_page_status __wrap_physical_page_resolve(const struct physical_page_allocator *a, uint64_t address, void **out)
{ checked_resolutions++; return __real_physical_page_resolve(a, address, out); }
struct parallel_pin_argument { struct kernel_mm *mm; uint64_t address; };
static void *paused_lookup(void *opaque)
{
    struct parallel_pin_argument *argument = opaque;
    mm_test_attach(1);
    struct kernel_mm_mapping mapping;
    assert(!kernel_mm_lookup(argument->mm, argument->address, &mapping));
    return NULL;
}
static void return_while_metadata_busy(struct kernel_mm *mm, uint64_t address)
{
    struct kernel_mm_user_pin pin = {0};
    assert(!kernel_mm_pin_user_page(mm, address, KERNEL_MM_WRITE, &pin));
    struct parallel_pin_argument argument = {mm, address};
    pthread_t writer;
    __atomic_store_n(&hold_metadata, 1, __ATOMIC_RELEASE);
    alarm(10);
    assert(!pthread_create(&writer, NULL, paused_lookup, &argument));
    await(&metadata_entered);
    /* writer停在真实页表查询内；已完成复制的owner无需等该MM资格。 */
    kernel_mm_unpin_user_page(&pin);
    assert(!pin.registration && !kernel_io_context_current()->user_pins);
    __atomic_store_n(&metadata_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(writer, NULL));
    alarm(0);
    __atomic_store_n(&hold_metadata, 0, __ATOMIC_RELEASE);
    struct kernel_mm_concurrency_state state;
    kernel_mm_get_concurrency_state(mm, &state);
    assert(!state.pins && !state.write_pins);
    printf("MM ordinary pin return bypasses busy metadata; record_bytes=%u\n", state.record_bytes);
}
static void *parallel_pin(void *opaque)
{
    struct parallel_pin_argument *argument = opaque;
    mm_test_attach(1);
    struct kernel_mm_user_pin pin = {0};
    assert(!kernel_mm_pin_user_page(argument->mm, argument->address, KERNEL_MM_READ, &pin));
    assert(pin.physical_address == qualifying_page);
    kernel_mm_unpin_user_page(&pin);
    assert(!kernel_cpu_current()->raw_locks && !kernel_io_context_current()->user_pins);
    return NULL;
}
static void parallel_resident_pins(struct kernel_mm *mm, uint64_t address)
{
    struct kernel_mm_mapping mapping;
    assert(!kernel_mm_lookup(mm, address, &mapping));
    qualifying_page = mapping.physical_address & ~BOAROS_PAGE_MASK;
    struct parallel_pin_argument argument = {mm, address};
    pthread_t first;
    alarm(10);
    assert(!pthread_create(&first, NULL, parallel_pin, &argument));
    await(&qualifying_entered);
    struct kernel_mm_user_pin second = {0};
    assert(!kernel_mm_pin_user_page(mm, address, KERNEL_MM_WRITE, &second));
    struct kernel_mm_concurrency_state state;
    /* snapshot是writer；这里只检查独立物理owner，不等待暂停的读资格。 */
    uint32_t references;
    assert(!physical_page_reference_count(mm->allocator, second.physical_address, &references));
    assert(references == 2);
    kernel_mm_unpin_user_page(&second);
    __atomic_store_n(&qualifying_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(first, NULL));
    alarm(0);
    qualifying_page = 0;
    kernel_mm_get_concurrency_state(mm, &state);
    assert(!state.pins && !state.write_pins);
    puts("MM concurrent resident pin qualifications overlap before physical acquire");
}
static void resident_copy_cost(struct kernel_mm *mm, uint64_t address)
{
    unsigned char data[4096] = {0};
    const size_t sizes[] = {1, 64, 4096};
    for (unsigned write = 0; write < 2; write++) {
        for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
            size_t copied;
            raw_acquisitions = checked_resolutions = 0;
            enum kernel_uaccess_status result = write ?
                kernel_copy_to_user(mm, address, data, sizes[i], &copied) :
                kernel_copy_from_user(mm, data, address, sizes[i], &copied);
            unsigned locks = raw_acquisitions;
            printf("resident %s %zu bytes: exclusive_locks=%u checked_resolutions=%u\n",
                   write ? "write" : "read", sizes[i], locks, checked_resolutions);
            assert(!result && copied == sizes[i] && !checked_resolutions);
        }
    }
}
static void copy_fragments(struct kernel_mm *mm, uint64_t address)
{
    unsigned char source[BOAROS_PAGE_SIZE + 64] __attribute__((aligned(16)));
    unsigned char destination[BOAROS_PAGE_SIZE + 64] __attribute__((aligned(16)));
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i * 29 + i / 251);
    for (size_t from = 0; from < 16; from++) for (size_t to = 0; to < 16; to++) {
        size_t size = BOAROS_PAGE_SIZE - to, copied;
        assert(kernel_copy_to_user(mm, address + to, source + from, size, &copied) == 0 && copied == size);
        memset(destination, 0x95, sizeof(destination));
        size_t start = 15 - from;
        assert(kernel_copy_from_user(mm, destination + start, address + to, size, &copied) == 0 && copied == size);
        assert(!memcmp(destination + start, source + from, size));
        for (size_t i = 0; i < start; i++) assert(destination[i] == 0x95);
        for (size_t i = start + size; i < sizeof(destination); i++) assert(destination[i] == 0x95);
    }
    const size_t prefixes[] = {0, 1, 7, 8, 9, 31, 32, 33, 63, 64, 65,
        BOAROS_PAGE_SIZE - 1, BOAROS_PAGE_SIZE};
    for (unsigned p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        for (size_t offset = 0; offset < 16; offset++) {
            size_t prefix = prefixes[p], copied;
            uint64_t start = address + BOAROS_PAGE_SIZE - prefix;
            assert(kernel_copy_to_user(mm, start, source + offset, prefix + 37, &copied) ==
                KERNEL_UACCESS_STATUS_FAULT && copied == prefix);
            memset(destination, 0x95, sizeof(destination));
            assert(kernel_copy_from_user(mm, destination + offset, start, prefix + 37, &copied) ==
                KERNEL_UACCESS_STATUS_FAULT && copied == prefix);
            assert(!memcmp(destination + offset, source + offset, prefix));
            for (size_t i = 0; i < offset; i++) assert(destination[i] == 0x95);
            for (size_t i = offset + prefix; i < sizeof(destination); i++) assert(destination[i] == 0x95);
        }
    }
}
static void fatal_pins(struct kernel_mm *mm, uint64_t address)
{
    struct rlimit limit = {0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    for (unsigned which = 0; which < 5; which++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            struct kernel_mm_user_pin pin = {0};
            assert(!kernel_mm_pin_user_page(mm, address, KERNEL_MM_READ, &pin));
            if (which == 0) mm_test_wrong_owner = 1;
            if (which == 1) {
                struct kernel_mm_user_pin copy = pin;
                kernel_mm_unpin_user_page(&copy);
            }
            if (which == 2) kernel_mm_unpin_user_page(&pin);
            if (which == 3) kernel_mm_pin_user_page(mm, address, KERNEL_MM_READ, &pin);
            if (which == 4) { mm_test_context = 0; kernel_mm_release(mm); }
            kernel_mm_unpin_user_page(&pin);
            _exit(0);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
}
int main(void)
{
    struct physical_page_allocator allocator = {0};
    struct boot_memory_layout layout = {0};
    struct arch_mmu_page_table kernel_table = {0};
    struct arch_mmu_user_space space = {0};
    struct kernel_heap heap = {0};
    struct kernel_mm mm = {0};
    mm_test_attach(0);
    layout.usable_count = 1;
    layout.usable[0].base = (uintptr_t)pool;
    layout.usable[0].size = sizeof(pool);
    assert(physical_page_allocator_init(&allocator, &layout) == 0);
    assert(physical_page_allocator_bind_access(&allocator, page_access) == 0);
    assert(physical_page_allocator_finalize(&allocator) == 0);
    assert(kernel_heap_init(&heap, &allocator, page_address) == 0);
    assert(riscv_sv39_page_table_init(&kernel_table, &allocator) == 0);
    kernel_table.state = ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline = physical_page_available(&allocator);
    kernel_root_borrow_case(&allocator,&heap,&kernel_table);
    assert(arch_mmu_user_space_init(&space, &allocator, &kernel_table) == 0);
    assert(kernel_mm_create(&mm, &space) == 0);
    assert(kernel_mm_vma_enable(&mm, &heap) == 0);
    assert(kernel_mm_brk_initialize(&mm, 0x10000, 0x20000) == 0);
    uint64_t address, context;
    assert(kernel_mm_context(&mm, &context) == 0);
    mm_test_context = context;
    new_mapping_sync_case(&mm);
    cold_copy_case(&mm);
    assert(kernel_mm_mmap_anonymous(&mm, 0x40000, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &address) == 0);
    /* 构造resident页，准备完成后测试lookup到复制结束的物理owner。 */
    assert(kernel_mm_resolve_user_fault(&mm, address, KERNEL_MM_WRITE) == 0);
    copy_fragments(&mm, address);
    unsigned char source[4] = {1, 2, 3, 4}, destination[4] = {0}; size_t copied;
    assert(kernel_copy_to_user(&mm, address + BOAROS_PAGE_SIZE - 2,
        source, sizeof(source), &copied) == KERNEL_UACCESS_STATUS_FAULT && copied == 2);
    assert(kernel_copy_from_user(&mm, destination, address + BOAROS_PAGE_SIZE - 2,
        sizeof(destination), &copied) == KERNEL_UACCESS_STATUS_FAULT && copied == 2);
    assert(destination[0] == 1 && destination[1] == 2 && !destination[2]);
    assert(kernel_copy_to_user(&mm, address, "hi", 3, &copied) == 0 && copied == 3);
    char string[8]; size_t length;
    assert(kernel_copy_string_from_user(&mm, string, address, sizeof(string), &length) == 0);
    assert(length == 2 && !strcmp(string, "hi"));
    uint32_t value = 17, observed;
    assert(!kernel_copy_to_user(&mm, address, &value, sizeof(value), &copied));
    assert(!kernel_user_cmpxchg_u32(&mm, address, 17, 39, &observed) && observed == 17);
    assert(!kernel_user_cmpxchg_u32(&mm, address, 17, 91, &observed) && observed == 39);
    assert(!kernel_mm_mprotect(&mm, address, BOAROS_PAGE_SIZE, KERNEL_MM_READ));
    struct kernel_mm_user_pin rejected = {0};
    assert(kernel_mm_pin_user_page(&mm, address, KERNEL_MM_WRITE, &rejected) == KERNEL_MM_STATUS_ACCESS);
    assert(!rejected.registration && !rejected.page);
    assert(!kernel_mm_mprotect(&mm, address, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
    resident_copy_cost(&mm, address);
    parallel_resident_pins(&mm, address);
    return_while_metadata_busy(&mm, address);
    fatal_pins(&mm, address);
    struct kernel_mm_mapping mapping;
    assert(kernel_mm_lookup(&mm, address, &mapping) == 0);
    struct kernel_mm_user_pin pin = {0};
    assert(kernel_mm_pin_user_page(&mm, address, KERNEL_MM_READ, &pin) == 0);
    uint32_t references;
    assert(physical_page_reference_count(&allocator,
        mapping.physical_address, &references) == 0);
    assert(references == 2);
    struct kernel_mm_user_pin extra[3] = {0};
    for (unsigned i=0;i<3;i++)
        assert(!kernel_mm_pin_user_page(&mm,address,KERNEL_MM_READ,&extra[i]));
    kernel_mm_unpin_user_page(&extra[1]);
    kernel_mm_unpin_user_page(&extra[0]);
    kernel_mm_unpin_user_page(&extra[2]);
    assert(kernel_io_context_current()->user_pins == &pin);
    memset(pin.page, 0x37, BOAROS_PAGE_SIZE);
    assert(kernel_mm_munmap(&mm, address, BOAROS_PAGE_SIZE) == 0);
    assert(physical_page_reference_count(&allocator,
        pin.physical_address, &references) == 0 && references == 1);
    uint64_t allocated[1024]; unsigned count = 0;
    while (count < 1024 && physical_page_allocate(&allocator, &allocated[count]) == 0) {
        assert(allocated[count] != pin.physical_address);
        count++;
    }
    for (size_t i = 0; i < BOAROS_PAGE_SIZE; i++)
        assert(((unsigned char *)pin.page)[i] == 0x37);
    watched_record = mm.record_page_address;
    record_resolutions = 0;
    kernel_mm_unpin_user_page(&pin);
    assert(record_resolutions == 0);
    watched_record = 0;
    for (unsigned i = 0; i < count; i++)
        assert(physical_page_release(&allocator, allocated[i]) == 0);
    mm_test_context = 0;
    assert(kernel_mm_release(&mm) == 0);
    assert(physical_page_available(&allocator) == baseline);
    assert(physical_page_allocator_audit(&allocator) == 0);
    puts("MM pin ownership and cleanup verified");
}
