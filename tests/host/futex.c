/* futex/MM/park/sleeping locks use production code; only CPU switching is hosted. */
#include "../../kernel/sched/private.h"
#include <kernel/mm_backend.h>
#include <kernel/time.h>
#include <kernel/errno.h>
#include <kernel/uaccess.h>
#include <kernel/memory_object.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

extern _Thread_local uintptr_t sync_test_irq;
extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
void mm_test_bind(struct kernel_mm *);
unsigned mm_test_parked(unsigned);
static unsigned char pool[8 * 1024 * 1024] __attribute__((aligned(16384)));
static struct kernel_mm mm;
static uint64_t context;
static const uint64_t source = 0x40000, target = 0x41000;
static unsigned opposite_arm, bucket_entered, bucket_waiter;
static unsigned remap_arm, fault_entered, fault_resume;
static unsigned gate_arm, gate_entered, gate_resume, gate_attempted;
static unsigned admission_arm, admission_waited;
static const uint64_t cold_target = 0x80000;
static struct kernel_memory_object *cold_object;
void __real_kernel_rwlock_write(struct kernel_rwlock *, struct kernel_lock_guard *);
void __wrap_kernel_rwlock_write(struct kernel_rwlock *lock, struct kernel_lock_guard *guard)
{
    if (lock->rank == 25 && kernel_cpu_current()->hardware_id != 2 &&
        __atomic_load_n(&gate_entered, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&gate_resume, __ATOMIC_ACQUIRE)) {
        /* Release the real gate so the old path fails an assertion, not a timeout. */
        __atomic_store_n(&gate_attempted, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&gate_resume, 1, __ATOMIC_RELEASE);
    }
    __real_kernel_rwlock_write(lock, guard);
    if (lock->rank == 25 && kernel_cpu_current()->hardware_id == 2 &&
        __atomic_exchange_n(&gate_arm, 0, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&gate_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&gate_resume, __ATOMIC_ACQUIRE)) sched_yield();
    }
    if (lock->rank == 26 && kernel_cpu_current()->hardware_id == 2 &&
        __atomic_exchange_n(&opposite_arm, 0, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&bucket_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&bucket_waiter, __ATOMIC_ACQUIRE)) sched_yield();
    }
}
enum kernel_scheduler_status __real_kernel_wait_prepare(struct kernel_wait_queue *, uint64_t, int, struct kernel_wait_token *);
enum kernel_scheduler_status __wrap_kernel_wait_prepare(struct kernel_wait_queue *queue,
    uint64_t deadline, int interruptible, struct kernel_wait_token *token)
{
    enum kernel_scheduler_status result = __real_kernel_wait_prepare(queue, deadline, interruptible, token);
    if (kernel_cpu_current()->hardware_id == 1 && !interruptible &&
        __atomic_load_n(&admission_arm, __ATOMIC_ACQUIRE)) {
        assert(!kernel_io_context_current()->locks && !kernel_io_context_current()->user_pins);
        __atomic_store_n(&admission_waited, 1, __ATOMIC_RELEASE);
    }
    if (kernel_cpu_current()->hardware_id == 3 && !interruptible &&
        __atomic_load_n(&bucket_entered, __ATOMIC_ACQUIRE)) {
        assert(result == KERNEL_SCHEDULER_STATUS_OK && queue->registrations && token->generation);
        __atomic_store_n(&bucket_waiter, 1, __ATOMIC_RELEASE);
    }
    return result;
}
enum kernel_memory_object_status __real_kernel_memory_object_prepare_page(struct kernel_memory_object *, uint64_t, uint64_t *, int *);
enum kernel_memory_object_status __wrap_kernel_memory_object_prepare_page(struct kernel_memory_object *object,
    uint64_t index, uint64_t *page, int *created)
{
    enum kernel_memory_object_status result = __real_kernel_memory_object_prepare_page(object, index, page, created);
    if (object == cold_object && kernel_cpu_current()->hardware_id == 2 &&
        __atomic_exchange_n(&remap_arm, 0, __ATOMIC_ACQ_REL)) {
        assert(!result && !kernel_io_context_current()->locks && !kernel_io_context_current()->user_pins);
        __atomic_store_n(&fault_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&fault_resume, __ATOMIC_ACQUIRE)) sched_yield();
    }
    return result;
}
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static void *page_access(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer, uint64_t *address)
{ *address = (uintptr_t)pointer; return 1; }
int kernel_vfs_node_acquire(struct kernel_vfs_node *node)
{ (void)node; abort(); }
int kernel_vfs_node_release(struct kernel_vfs_node **node)
{ (void)node; abort(); }
uint64_t kernel_time_monotonic_ns(void) { return 1; }
enum kernel_time_status kernel_time_deadline_from_monotonic(uint64_t ns, uint64_t *ticks)
{ *ticks = ns; return ns ? KERNEL_TIME_STATUS_OK : KERNEL_TIME_STATUS_DEADLINE_PASSED; }
uint64_t kernel_time_boot_realtime_offset(void) { return 0; }
void kernel_signal_note_futex_timed_restart(struct kernel_task *task, uint64_t address,
    uint32_t operation, uint32_t value, uint64_t deadline, uint32_t bitset)
{ (void)task; (void)address; (void)operation; (void)value; (void)deadline; (void)bitset; abort(); }
static void attach(unsigned id)
{
    mm_test_attach(id);
    struct kernel_task *task = kernel_wait_current_task();
    assert(!kernel_mm_acquire(&task->mm, &mm));
    mm_test_bind(&task->mm);
    kernel_cpu_current()->current = task;
    task->cpu = kernel_cpu_current();
    mm_test_context = context;
    sync_test_irq = 0;
}
static void detach(void)
{
    struct kernel_task *task = kernel_wait_current_task();
    assert(!task->wait.borrows && !task->wait.nodes && !task->futex_key.identity);
    assert(!kernel_io_context_current()->locks && !kernel_io_context_current()->user_pins);
    assert(!kernel_mm_release(&task->mm));
    mm_test_bind(0);
    kernel_cpu_current()->current = 0;
}
static int64_t call(uint64_t a, uint32_t op, uint32_t val, uint64_t count,
                    uint64_t b, uint32_t arg)
{
    enum kernel_scheduler_status status;
    int64_t result = kernel_futex(kernel_wait_current_task(), a, op, val, count, b, arg, &status);
    assert(status == KERNEL_SCHEDULER_STATUS_OK && !sync_test_irq);
    return result;
}
static void put(uint64_t address, uint32_t value)
{ size_t n; assert(!kernel_copy_to_user(&mm, address, &value, sizeof(value), &n) && n == sizeof(value)); }
static uint32_t get(uint64_t address)
{ uint32_t value; size_t n; assert(!kernel_copy_from_user(&mm, &value, address, sizeof(value), &n) && n == sizeof(value)); return value; }
static unsigned encode(unsigned op, unsigned cmp, int arg, int comparison)
{ return op << 28 | cmp << 24 | ((unsigned)arg & 4095) << 12 | ((unsigned)comparison & 4095); }
struct waiter { unsigned id, bitset; uint64_t address; int64_t result; uint32_t expected; };
static void *waiter(void *arg)
{
    struct waiter *w = arg;
    attach(w->id);
    w->result = call(w->address, 9 | 128, w->expected, 0, 0, w->bitset);
    detach();
    return 0;
}
static void queued(unsigned id, unsigned previous)
{ while (mm_test_parked(id) == previous) sched_yield(); }
static void operations(void)
{
    /* The old implementation returns ENOSYS for both commands. */
    assert(call(source, 4 | 128, 0, 0, target, 1) == -KERNEL_EAGAIN);
    assert(call(source, 4 | 128, 0, 0, target, 0) == 0);
    put(target, 0);
    assert(call(source, 5 | 128, 0, 0, target, encode(1, 0, 7, 0)) == 0);
    assert(get(target) == 7);
    const unsigned ops[] = {0, 1, 2, 3, 4, 8, 9};
    const uint32_t expected[] = {0xfffffffd, 0xfffffffa, 0xfffffffb, 2, 0xfffffffd, 0x80000000, 0};
    const int args[] = {-3, -3, 1, -3, -1, 31, -1};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        assert(call(source, 5 | 128, 0, 0, target, encode(ops[i], 2, args[i], 0)) == 0);
        assert(get(target) == expected[i]);
    }
    put(target, 17);
    assert(call(source, 5 | 128, 0, 0, target, encode(7, 0, 0, 0)) == -KERNEL_ENOSYS && get(target) == 17);
    assert(call(source, 5 | 128, 0, 0, target, encode(0, 7, 3, 0)) == -KERNEL_ENOSYS && get(target) == 3);
    assert(call(source, 3 | 128, 0, UINT64_C(0x100000000), target, 0) == 0);
    assert(call(source, 4 | 128, 0, UINT64_C(0x100000000), target, 0) == 0);
    assert(call(source, 4 | 128, 0, UINT32_MAX, target, 0) == -KERNEL_EINVAL);
    assert(call(source, 3 | 128, 0, 0, source, 0) == 0);
    assert(call(source, 4 | 128, 0, 0, source, 0) == 0);
    assert(call(8, 1 | 128, 1, 0, 0, 0) == 0);
    assert(call(8, 1, 1, 0, 0, 0) == -KERNEL_EFAULT);
}
static void *hold_mm_gate(void *unused)
{
    (void)unused; attach(2);
    struct kernel_vma vma;
    assert(!kernel_mm_vma_lookup(&kernel_wait_current_task()->mm, source, &vma));
    detach(); return 0;
}
static void private_with_mm_gate_busy(void)
{
    put(source, 0); put(target, 0);
    gate_entered = gate_resume = gate_attempted = 0; gate_arm = 1;
    pthread_t holder, waiting;
    assert(!pthread_create(&holder, 0, hold_mm_gate, 0));
    while (!__atomic_load_n(&gate_entered, __ATOMIC_ACQUIRE)) sched_yield();
    assert(call(8, 1 | 128, 1, 0, 0, 0) == 0);
    assert(!gate_attempted && !gate_resume);
    assert(call(source, 3 | 128, 0, 1, 8, 0) == 0);
    assert(call(source, 4 | 128, 0, 1, target, 0) == 0);
    assert(call(source, 5 | 128, 0, 0, target, encode(1, 0, 1, 0)) == 0);
    assert(call(source, 0 | 128, 1, 0, 0, 0) == -KERNEL_EAGAIN);
    struct waiter w = {1, 1, source, -99, 0};
    unsigned before = mm_test_parked(1);
    assert(!pthread_create(&waiting, 0, waiter, &w)); queued(1, before);
    assert(call(source, 1 | 128, 1, 0, 0, 0) == 1);
    assert(!pthread_join(waiting, 0) && w.result == 0);
    assert(!gate_attempted && !gate_resume);
    __atomic_store_n(&gate_resume, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(holder, 0)); gate_entered = 0;
    assert(get(target) == 1);
}
static void private_access(void)
{
    const uint64_t address = 0xc0000;
    uint64_t out;
    assert(call(address, 0 | 128, 1, 0, 0, 0) == -KERNEL_EFAULT);
    assert(call(source, 4 | 128, 0, 0, target + 1, 0) == -KERNEL_EINVAL);
    assert(call(source, 3 | 128, 0, 0, ARCH_MMU_USER_LIMIT, 0) == -KERNEL_EFAULT);
    assert(!kernel_mm_mmap_anonymous(&mm, address, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &out));
    /* Cold READ and WRITE use the real resolver before borrowing the page. */
    assert(call(address, 0 | 128, 1, 0, 0, 0) == -KERNEL_EAGAIN);
    assert(call(source, 5 | 128, 0, 0, address, encode(1, 0, 3, 0)) == 0);
    assert(get(address) == 3);
    struct kernel_mm child = {0};
    assert(!kernel_mm_fork(&child, &mm));
    assert(call(source, 5 | 128, 0, 0, address, encode(1, 0, 1, 3)) == 0);
    uint32_t copied; size_t bytes;
    assert(get(address) == 4 && !kernel_copy_from_user(&child, &copied, address,
        sizeof(copied), &bytes) && bytes == sizeof(copied) && copied == 3);
    assert(!kernel_mm_release(&child));
    assert(!kernel_mm_mprotect(&mm, address, BOAROS_PAGE_SIZE, KERNEL_MM_READ));
    assert(call(address, 0 | 128, 3, 0, 0, 0) == -KERNEL_EAGAIN);
    assert(call(source, 5 | 128, 0, 0, address, encode(1, 0, 1, 0)) == -KERNEL_EFAULT);
    assert(get(address) == 4);
    assert(!kernel_mm_mprotect(&mm, address, BOAROS_PAGE_SIZE, 0));
    assert(call(address, 0 | 128, 3, 0, 0, 0) == -KERNEL_EFAULT);
    assert(call(address, 1 | 128, 1, 0, 0, 0) == 0);
    assert(!kernel_mm_munmap(&mm, address, BOAROS_PAGE_SIZE));
}
static void *revoke_private_read(void *unused)
{
    (void)unused; attach(2);
    assert(!kernel_mm_mprotect(&kernel_wait_current_task()->mm,
        source, BOAROS_PAGE_SIZE, 0));
    detach(); return 0;
}
static void private_admission(void)
{
    put(source, 0);
    struct kernel_mm_user_pin held = {0};
    assert(!kernel_mm_pin_user_page(&mm, source, KERNEL_MM_READ, &held));
    unsigned before_edit = mm_test_parked(2), before_read = mm_test_parked(1);
    pthread_t edit, read;
    assert(!pthread_create(&edit, 0, revoke_private_read, 0)); queued(2, before_edit);
    struct waiter w = {1, 1, source, -99, 1};
    admission_waited = 0; admission_arm = 1;
    assert(!pthread_create(&read, 0, waiter, &w)); queued(1, before_read);
    assert(__atomic_load_n(&admission_waited, __ATOMIC_ACQUIRE));
    kernel_mm_unpin_user_page(&held);
    assert(!pthread_join(edit, 0) && !pthread_join(read, 0));
    assert(w.result == -KERNEL_EFAULT); admission_arm = 0;
    assert(!kernel_mm_mprotect(&mm, source, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
}
static void comparisons(void)
{
    for (unsigned cmp = 0; cmp < 6; cmp++) for (int value = -1; value <= 1; value++) {
        put(target, (uint32_t)value);
        struct waiter w = {.id = 1, .bitset = 1, .address = target, .result = -99, .expected = (uint32_t)value};
        unsigned before = mm_test_parked(1); pthread_t worker;
        assert(!pthread_create(&worker, 0, waiter, &w)); queued(1, before);
        int expected = cmp == 0 ? value == 0 : cmp == 1 ? value != 0 :
                       cmp == 2 ? value < 0 : cmp == 3 ? value <= 0 :
                       cmp == 4 ? value > 0 : value >= 0;
        int64_t result = call(source, 5 | 128, 1, 1, target, encode(1, cmp, 0, 0));
        if (result != expected) fprintf(stderr, "comparison %u old=%d result=%lld expected=%d\n", cmp, value, (long long)result, expected);
        assert(result == expected && get(target) == (uint32_t)value);
        if (!expected) assert(call(target, 1 | 128, 1, 0, 0, 0) == 1);
        assert(!pthread_join(worker, 0) && w.result == 0);
    }
}
static void requeue(uint64_t destination)
{
    put(source, 0); put(target, 0);
    struct waiter a = {1, 1, source, -99, 0}, b = {2, 2, source, -99, 0};
    unsigned before_a = mm_test_parked(1), before_b = mm_test_parked(2);
    pthread_t x, y;
    assert(!pthread_create(&x, 0, waiter, &a)); queued(1, before_a);
    assert(!pthread_create(&y, 0, waiter, &b)); queued(2, before_b);
    assert(call(source, 3 | 128, 0, INT32_MAX, source, 0) == 2);
    assert(call(source, 4 | 128, 0, 2, target, 1) == -KERNEL_EAGAIN);
    assert(call(source, 4 | 128, 0, 2, destination, 0) == 2);
    assert(call(source, 1 | 128, 2, 0, 0, 0) == 0);
    assert(call(destination, 10 | 128, 2, 0, 0, 1) == 1);
    assert(!pthread_join(x, 0) && a.result == 0);
    assert(call(destination, 10 | 128, 2, 0, 0, 2) == 1);
    assert(!pthread_join(y, 0) && b.result == 0);
}
struct mover { unsigned id; uint64_t from, to; int64_t result; };
static void *mover(void *argument)
{
    struct mover *m = argument; attach(m->id);
    if (m->id == 3) while (!__atomic_load_n(&bucket_entered, __ATOMIC_ACQUIRE)) sched_yield();
    m->result = call(m->from, 3 | 128, 0, 1, m->to, 0);
    detach(); return 0;
}
static void opposite(uint64_t destination)
{
    put(source, 0);
    struct waiter w = {1, 1, source, -99, 0};
    unsigned before = mm_test_parked(1); pthread_t wait_thread, first, second;
    assert(!pthread_create(&wait_thread, 0, waiter, &w)); queued(1, before);
    bucket_entered = bucket_waiter = 0; opposite_arm = 1;
    struct mover a = {2, source, destination, -99}, b = {3, destination, source, -99};
    assert(!pthread_create(&first, 0, mover, &a) && !pthread_create(&second, 0, mover, &b));
    assert(!pthread_join(first, 0) && !pthread_join(second, 0));
    assert(a.result == 1 && b.result == 1 && bucket_waiter);
    assert(call(destination, 1 | 128, 1, 0, 0, 0) == 0);
    assert(call(source, 1 | 128, 1, 0, 0, 0) == 1);
    assert(!pthread_join(wait_thread, 0) && w.result == 0);
    bucket_entered = 0;
}
static uint64_t old_identity;
static void *prepare_after_remap(void *unused)
{
    (void)unused; attach(2);
    struct kernel_futex_key keys[2]; struct kernel_mm_user_pin pin = {0};
    uint64_t addresses[2] = {source, cold_target};
    assert(!kernel_mm_prepare_futex(&kernel_wait_current_task()->mm,
        addresses, 2, 1, 0, KERNEL_MM_READ, keys, &pin));
    assert(keys[0].identity != old_identity && keys[0].kind == KERNEL_FUTEX_KEY_SHARED_MEMORY);
    uint32_t *word = (uint32_t *)((unsigned char *)pin.page + (source & BOAROS_PAGE_MASK));
    assert(__atomic_load_n(word, __ATOMIC_ACQUIRE) == UINT32_C(0x44556677));
    kernel_mm_unpin_user_page(&pin);
    for (unsigned i = 0; i < 2; i++) kernel_memory_object_release(&keys[i].shared_object);
    detach(); return 0;
}
static void coherent_remap(void)
{
    uint64_t out;
    assert(!kernel_mm_mmap_anonymous(&mm, source, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_SHARED, &out));
    put(source, 0);
    struct kernel_futex_key old;
    assert(!kernel_mm_prepare_futex(&mm, &source, 1, 1, 0, 0, &old, 0));
    old_identity = old.identity;
    assert(!kernel_mm_mmap_anonymous(&mm, cold_target, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_SHARED, &out));
    struct kernel_vma vma;
    assert(!kernel_mm_vma_lookup(&mm, cold_target, &vma)); cold_object = vma.backing;
    remap_arm = 1; fault_entered = fault_resume = 0;
    pthread_t worker; assert(!pthread_create(&worker, 0, prepare_after_remap, 0));
    while (!__atomic_load_n(&fault_entered, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!kernel_mm_munmap(&mm, source, BOAROS_PAGE_SIZE));
    assert(!kernel_mm_mmap_anonymous(&mm, source, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_SHARED, &out));
    put(source, UINT32_C(0x44556677));
    __atomic_store_n(&fault_resume, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(worker, 0));
    kernel_memory_object_release(&old.shared_object);
}
int main(void)
{
    struct physical_page_allocator allocator = {0};
    struct boot_memory_layout layout = {.usable_count = 1};
    struct arch_mmu_page_table table = {0}; struct arch_mmu_user_space space = {0};
    struct kernel_heap heap = {0};
    mm_test_attach(0);
    layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    assert(!physical_page_allocator_init(&allocator, &layout));
    assert(!physical_page_allocator_bind_access(&allocator, page_access));
    assert(!physical_page_allocator_finalize(&allocator));
    assert(!kernel_heap_init(&heap, &allocator, page_address));
    assert(!riscv_sv39_page_table_init(&table, &allocator)); table.state = ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline = physical_page_available(&allocator), out;
    assert(!arch_mmu_user_space_init(&space, &allocator, &table));
    assert(!kernel_mm_create(&mm, &space) && !kernel_mm_vma_enable(&mm, &heap));
    assert(!kernel_mm_brk_initialize(&mm, 0x10000, 0x20000));
    assert(!kernel_mm_context(&mm, &context)); mm_test_context = context;
    assert(!kernel_mm_mmap_anonymous(&mm, source, 2 * BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &out));
    attach(0);
    operations(); private_with_mm_gate_busy(); private_access(); private_admission();
    requeue(target); requeue(source + 1024); comparisons();
    opposite(target); opposite(source + 1024); coherent_remap();
    detach(); mm_test_context = 0;
    assert(!kernel_mm_release(&mm));
    assert(physical_page_available(&allocator) == baseline && !physical_page_allocator_audit(&allocator));
    puts("futex native operations, gate-independent private keys/pins, admission/COW, requeue/bitset, coherent remap and owners PASS");
}
