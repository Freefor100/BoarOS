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

void mm_test_attach(unsigned);
extern _Thread_local uint64_t mm_test_context;
static unsigned char pool[4 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_mm mm;
static uint64_t context, kernel_context;
static unsigned sampled, entered, allow_send, pause_exit, exiting, allow_exit;
static _Thread_local unsigned flushes, instruction_sync;
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
    assert(!kernel_mm_enable_concurrency(&mm));
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
    mm_test_context = 0; assert(!kernel_mm_release(&mm));
    assert(physical_page_available(&allocator) == baseline && !physical_page_allocator_audit(&allocator));
    puts("MM sampled-target late entry, same-root local invalidation and leave-before-retirement verified");
}
