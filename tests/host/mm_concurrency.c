#define _POSIX_C_SOURCE 200809L
#include <kernel/mm_backend.h>
#include <kernel/heap.h>
#include <kernel/cpu.h>
#include <kernel/uaccess.h>
#include <kernel/sync.h>
#include <kernel/errno.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern _Thread_local uint64_t mm_test_context;
void mm_test_attach(unsigned);
unsigned mm_test_parked(unsigned);
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_mm mm[4];
static uint64_t context, address;
static unsigned done[4];
static unsigned park_before[4];
static unsigned hold_fence, fence_entered, fence_release;
static struct kernel_mm child;
static uint64_t child_context;
static pthread_barrier_t writers;
static unsigned capture_mask, prepare_entered[4], prepare_release;
static unsigned fail_next_access;
static uint64_t fail_access_address = KERNEL_MM_NO_PAGE;
static uint64_t race_address;
static uint64_t parallel_address[4];
static unsigned population_capture, population_entered, population_release;
static uint64_t population_start;
static uint64_t batch_test_start;
static unsigned batch_test_mode, batch_test_prefix, batch_test_syncs, batch_test_full, batch_test_local, batch_test_instructions, batch_test_neighbors;
static void *batch_visible_fault(void *unused);
static enum kernel_uaccess_status race_status[4];
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *pages, uint64_t *physical)
{
    unsigned id = kernel_cpu_current()->hardware_id;
    struct kernel_lock_guard *held = kernel_io_context_current()->locks;
    int fail_batch_access = 0;
    if (pages == &allocator && batch_test_start && id == 0) {
        struct kernel_mm_mapping first;
        if (held && !held->write && !kernel_mm_lookup(&mm[0], batch_test_start, &first))
            assert(batch_test_full == 1 && "new nonleaf must be confirmed before another leaf preparation");
        if (held && !held->write && !batch_test_prefix &&
            !kernel_mm_lookup(&mm[0], batch_test_start + BOAROS_PAGE_SIZE, &first)) {
            batch_test_prefix = 1;
            assert(batch_test_syncs == (batch_test_mode == 4 ? 2U : 1U));
            if (!batch_test_mode) {
                done[2] = 0;
                pthread_t peer;
                assert(!pthread_create(&peer, 0, batch_visible_fault, 0));
                assert(!pthread_join(peer, 0));
                assert(done[2] && batch_test_local == 3 && batch_test_neighbors == 2 && batch_test_syncs == 1);
            }
            if (batch_test_mode == 1 || batch_test_mode == 2) return PHYSICAL_PAGE_STATUS_EMPTY;
            fail_batch_access = batch_test_mode == 3;
        } else if (!held && batch_test_prefix && (batch_test_mode == 1 || batch_test_mode == 2)) {
            assert(batch_test_syncs == 2); /* nonleaf and pending leaf confirmed before pressure retry */
            if (batch_test_mode == 2) return PHYSICAL_PAGE_STATUS_EMPTY;
        }
    }
    if (pages == &allocator && id == 1 && held && held->lock->rank == 25 && !held->write &&
        __atomic_exchange_n(&population_capture, 0, __ATOMIC_ACQ_REL)) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&population_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&population_release, __ATOMIC_ACQUIRE)) sched_yield();
    }
    if (pages == &allocator && id < 4 && !kernel_io_context_current()->locks &&
        (__atomic_fetch_and(&capture_mask, ~(1U << id), __ATOMIC_ACQ_REL) & (1U << id))) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&prepare_entered[id], 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&prepare_release, __ATOMIC_ACQUIRE)) sched_yield();
    }
    enum physical_page_status status = __real_physical_page_allocate(pages, physical);
    if (status == PHYSICAL_PAGE_STATUS_OK && fail_batch_access)
        __atomic_store_n(&fail_access_address, *physical, __ATOMIC_RELEASE);
    if (status == PHYSICAL_PAGE_STATUS_OK && pages == &allocator && !kernel_io_context_current()->locks &&
        __atomic_exchange_n(&fail_next_access, 0, __ATOMIC_ACQ_REL))
        __atomic_store_n(&fail_access_address, *physical, __ATOMIC_RELEASE);
    return status;
}
static void *page_access(uint64_t physical)
{
    if (physical == __atomic_load_n(&fail_access_address, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&fail_access_address, KERNEL_MM_NO_PAGE, __ATOMIC_RELEASE);
        return 0;
    }
    return (void *)(uintptr_t)physical;
}
static int page_address(const void *pointer, uint64_t *physical)
{ *physical = (uintptr_t)pointer; return 1; }
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { fputs("excluded business path reached\n", stderr); abort(); }
void mm_test_fence(uint64_t v, int instructions)
{
    (void)v;
    assert(!kernel_cpu_current()->raw_locks); __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!instructions && batch_test_start && kernel_cpu_current()->hardware_id == 2)
        batch_test_local++;
    if (!instructions && kernel_cpu_current()->hardware_id == 1 &&
        __atomic_load_n(&hold_fence, __ATOMIC_ACQUIRE)) {
        assert(!kernel_cpu_current()->raw_locks);
        __atomic_store_n(&fence_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&fence_release, __ATOMIC_ACQUIRE)) sched_yield();
    }
}
void riscv_smp_sync_user(uint32_t targets, uint64_t start, uint64_t length, int instructions)
{
    assert(!(targets & ~1U));
    if (batch_test_start && kernel_cpu_current()->hardware_id == 0) {
        batch_test_syncs++;
        batch_test_instructions += instructions != 0;
        if (!start && length == UINT64_MAX) batch_test_full++;
    }
    if (batch_test_start && kernel_cpu_current()->hardware_id == 2 &&
        (start == batch_test_start + 4 * BOAROS_PAGE_SIZE || start == batch_test_start + 5 * BOAROS_PAGE_SIZE)) {
        assert(batch_test_full == 1 && length == BOAROS_PAGE_SIZE);
        assert(instructions == (start == batch_test_start + 5 * BOAROS_PAGE_SIZE));
        batch_test_neighbors++;
    }
    if (population_start && kernel_cpu_current()->hardware_id == 1 &&
        start == population_start + 16 * BOAROS_PAGE_SIZE) {
        struct kernel_mm_mapping first;
        assert(!kernel_mm_lookup(&mm[1], population_start, &first));
        assert(!(first.permissions & KERNEL_MM_WRITE));
    }
    mm_test_fence(length == BOAROS_PAGE_SIZE ? start : UINT64_MAX, 0);
    if (instructions) mm_test_fence(0, 1);
}
static void *downgrade(void *unused)
{
    (void)unused; mm_test_attach(1); mm_test_context = context;
    assert(!kernel_mm_mprotect(&mm[1], address, BOAROS_PAGE_SIZE, KERNEL_MM_READ));
    __atomic_store_n(&done[1], 1, __ATOMIC_RELEASE);
    return 0;
}
static void *new_access(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    mm_test_attach(id); mm_test_context = context;
    struct kernel_mm_user_pin pin = {0};
    enum kernel_mm_status status = kernel_mm_pin_user_page(&mm[id], address,
        KERNEL_MM_WRITE, &pin);
    assert(status == KERNEL_MM_STATUS_ACCESS && !pin.registration);
    __atomic_store_n(&done[id], 1, __ATOMIC_RELEASE);
    return 0;
}
static void *query(void *unused)
{
    (void)unused; mm_test_attach(3); mm_test_context = context;
    struct kernel_mm_proc_memory memory;
    assert(!kernel_mm_proc_memory_snapshot(&mm[3], &memory));
    assert(memory.virtual_bytes == BOAROS_PAGE_SIZE && memory.resident_pages == 1);
    struct kernel_mm_statistics statistics;
    kernel_mm_get_statistics(&mm[3], &statistics);
    assert(!kernel_mm_msync(&mm[3], address, BOAROS_PAGE_SIZE, 4));
    assert(kernel_mm_msync(&mm[3], address, 2 * BOAROS_PAGE_SIZE, 4) == -KERNEL_ENOMEM);
    assert(kernel_mm_msync(&mm[3], address + 1, 1, 4) == -KERNEL_EINVAL);
    assert(kernel_mm_shmdt(&mm[3], address) == KERNEL_MM_STATUS_INVALID_ARGUMENT);
    uint64_t unused_address;
    assert(!kernel_mm_mmap_anonymous(&mm[3], 0, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ, KERNEL_MM_MAP_SHARED, &unused_address));
    assert(!kernel_mm_munmap(&mm[3], unused_address, BOAROS_PAGE_SIZE));
    __atomic_store_n(&done[3], 1, __ATOMIC_RELEASE);
    return 0;
}
static void await_registration(unsigned id)
{
    while (mm_test_parked(id) == park_before[id] && !__atomic_load_n(&done[id], __ATOMIC_ACQUIRE)) sched_yield();
    assert(mm_test_parked(id) > park_before[id] && !__atomic_load_n(&done[id], __ATOMIC_ACQUIRE));
}
static void *unmap(void *unused)
{
    (void)unused; mm_test_attach(1); mm_test_context = context;
    assert(!kernel_mm_munmap(&mm[1], address, BOAROS_PAGE_SIZE));
    __atomic_store_n(&done[1], 1, __ATOMIC_RELEASE);
    return 0;
}
static void retirement(void)
{
    struct kernel_mm_user_pin old = {0};
    assert(!kernel_mm_pin_user_page(&mm[0], address, KERNEL_MM_READ, &old));
    uint64_t before = physical_page_available(&allocator);
    hold_fence = 1; fence_entered = fence_release = done[1] = 0;
    pthread_t worker; assert(!pthread_create(&worker, 0, unmap, 0));
    while (!__atomic_load_n(&fence_entered, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!__atomic_load_n(&done[1], __ATOMIC_ACQUIRE));
    struct kernel_mm_mapping mapping;
    assert(kernel_mm_lookup(&mm[0], address, &mapping) == KERNEL_MM_STATUS_NOT_MAPPED);
    /* 第一批数据叶已摘，PTE转移的owner仍等待确认；空表由后续有界扫描回收。 */
    assert(physical_page_available(&allocator) == before);
    uint32_t references;
    assert(!physical_page_reference_count(&allocator, old.physical_address, &references));
    assert(references == 2 && ((unsigned char *)old.page)[0] == 49);
    uint64_t pressure[4096]; unsigned used = 0;
    while (used < 4096 && !physical_page_allocate(&allocator, &pressure[used])) {
        assert(pressure[used] != old.physical_address);
        used++;
    }
    assert(!physical_page_available(&allocator));
    __atomic_store_n(&fence_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(worker, 0)); hold_fence = 0;
    assert(!physical_page_reference_count(&allocator, old.physical_address, &references));
    assert(references == 1); /* 同步完成后仅访问片段pin继续存活。 */
    kernel_mm_unpin_user_page(&old);
    for (unsigned i = 0; i < used; i++) assert(!physical_page_release(&allocator, pressure[i]));
}
static void *fork_mm(void *unused)
{
    (void)unused; mm_test_attach(1); mm_test_context = context;
    assert(!kernel_mm_fork(&child, &mm[1]));
    __atomic_store_n(&done[1], 1, __ATOMIC_RELEASE);
    return 0;
}
static void *write_cow(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    mm_test_attach(id); mm_test_context = id == 1 ? context : child_context;
    int result = pthread_barrier_wait(&writers);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    unsigned char value = id == 1 ? 0xa1 : 0xb2;
    size_t copied;
    for (unsigned repeat = 0; repeat < 64; repeat++)
        assert(!kernel_copy_to_user(id == 1 ? &mm[1] : &child, address,
            &value, sizeof(value), &copied) && copied == 1);
    mm_test_context = 0;
    return 0;
}
static void fork_cow(void)
{
    assert(!kernel_mm_mprotect(&mm[0], address, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
    struct kernel_mm_user_pin write = {0};
    assert(!kernel_mm_pin_user_page(&mm[0], address, KERNEL_MM_WRITE, &write));
    done[1] = 0;
    park_before[1] = mm_test_parked(1);
    pthread_t worker; assert(!pthread_create(&worker, 0, fork_mm, 0));
    await_registration(1);
    memset(write.page, 0x66, BOAROS_PAGE_SIZE);
    kernel_mm_unpin_user_page(&write);
    assert(!pthread_join(worker, 0));
    assert(!kernel_mm_context(&child, &child_context));
    unsigned char value; size_t copied;
    mm_test_context = child_context;
    assert(!kernel_copy_from_user(&child, &value, address, 1, &copied) && value == 0x66);
    mm_test_context = context;
    struct kernel_mm_mapping parent_mapping, child_mapping;
    assert(!kernel_mm_lookup(&mm[0], address, &parent_mapping));
    assert(!kernel_mm_lookup(&child, address, &child_mapping));
    assert(parent_mapping.physical_address == child_mapping.physical_address);
    /* 真实OOM不改变COW身份、内容或引用；恢复资源后才允许提交。 */
    uint64_t pressure[4096]; unsigned used = 0;
    while (used < 4096 && !physical_page_allocate(&allocator, &pressure[used])) used++;
    assert(!physical_page_available(&allocator));
    value = 0x91;
    assert(kernel_copy_to_user(&mm[0], address, &value, 1, &copied) == KERNEL_UACCESS_STATUS_FAULT && !copied);
    uint32_t references;
    assert(!physical_page_reference_count(&allocator, parent_mapping.physical_address, &references) && references == 2);
    for (unsigned i = 0; i < used; i++) assert(!physical_page_release(&allocator, pressure[i]));
    assert(!pthread_barrier_init(&writers, 0, 3));
    pthread_t parent_writer, child_writer;
    assert(!pthread_create(&parent_writer, 0, write_cow, (void *)(uintptr_t)1));
    assert(!pthread_create(&child_writer, 0, write_cow, (void *)(uintptr_t)2));
    int result = pthread_barrier_wait(&writers);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!pthread_join(parent_writer, 0) && !pthread_join(child_writer, 0));
    assert(!pthread_barrier_destroy(&writers));
    assert(!kernel_copy_from_user(&mm[0], &value, address, 1, &copied) && value == 0xa1);
    mm_test_context = child_context;
    assert(!kernel_copy_from_user(&child, &value, address, 1, &copied) && value == 0xb2);
    mm_test_context = context;
    assert(!kernel_mm_lookup(&mm[0], address, &parent_mapping));
    assert(!kernel_mm_lookup(&child, address, &child_mapping));
    assert(parent_mapping.physical_address != child_mapping.physical_address);
    assert(!kernel_mm_release(&child)); child = (struct kernel_mm){0};
    assert(!kernel_mm_fork(&child, &mm[0]));
    assert(!kernel_mm_release(&child)); child = (struct kernel_mm){0};
    value = 49;
    assert(!kernel_copy_to_user(&mm[0], address, &value, 1, &copied));
    assert(!kernel_mm_lookup(&mm[0], address, &child_mapping));
    assert(parent_mapping.physical_address == child_mapping.physical_address); /* 最后引用原地恢复。 */
}
static void *race_fault(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    mm_test_attach(id); mm_test_context = context;
    unsigned char value = (unsigned char)(0x90 + id); size_t copied;
    race_status[id] = kernel_copy_to_user(&mm[id], race_address + id, &value, 1, &copied);
    assert(copied == (race_status[id] == KERNEL_UACCESS_STATUS_OK ? 1U : 0U));
    return 0;
}
static void prepared(unsigned id)
{
    while (!__atomic_load_n(&prepare_entered[id], __ATOMIC_ACQUIRE)) sched_yield();
}
static void *downgrade_during_prepare(void *unused)
{
    prepared(0);
    return downgrade(unused);
}
static void *resume_after_drain_registration(void *unused)
{
    (void)unused;
    await_registration(1);
    __atomic_store_n(&prepare_release, 1, __ATOMIC_RELEASE);
    return 0;
}
static void self_pin_fault(void)
{
    /* 缺表候选与COW旧页引用都必须在重取gate冲突后归还。 */
    for (unsigned cow = 0; cow < 2; cow++) {
        assert(!kernel_mm_mprotect(&mm[0], address, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
        uint64_t target = address, mapped;
        if (cow) assert(!kernel_mm_fork(&child, &mm[0]));
        else {
            target = UINT64_C(0x40000000);
            assert(!kernel_mm_mmap_anonymous(&mm[0], target, BOAROS_PAGE_SIZE,
                KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &mapped));
        }
        struct kernel_mm_user_pin pin = {0};
        assert(!kernel_mm_pin_user_page(&mm[0], address, KERNEL_MM_READ, &pin));
        uint64_t available = physical_page_available(&allocator);
        uint32_t references, after;
        assert(!physical_page_reference_count(&allocator, pin.physical_address, &references));
        memset(prepare_entered, 0, sizeof(prepare_entered));
        prepare_release = done[1] = 0;
        capture_mask = 1;
        park_before[1] = mm_test_parked(1);
        pthread_t modifier, resumer;
        assert(!pthread_create(&modifier, 0, downgrade_during_prepare, 0));
        assert(!pthread_create(&resumer, 0, resume_after_drain_registration, 0));
        assert(kernel_mm_resolve_user_fault(&mm[0], target, KERNEL_MM_WRITE) == KERNEL_MM_STATUS_CONFLICT);
        assert(physical_page_available(&allocator) == available);
        assert(!physical_page_reference_count(&allocator, pin.physical_address, &after) && after == references);
        assert(pin.registration == &pin && kernel_io_context_current()->user_pins == &pin);
        assert(!kernel_io_context_current()->locks && !__atomic_load_n(&done[1], __ATOMIC_ACQUIRE));
        kernel_mm_unpin_user_page(&pin);
        assert(!pthread_join(resumer, 0) && !pthread_join(modifier, 0));
        if (cow) { assert(!kernel_mm_release(&child)); child = (struct kernel_mm){0}; }
        else assert(!kernel_mm_munmap(&mm[0], target, BOAROS_PAGE_SIZE));
    }
    assert(!kernel_mm_mprotect(&mm[0], address, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
    assert(!kernel_mm_resolve_user_fault(&mm[0], address, KERNEL_MM_WRITE));
    puts("MM self-pin gate conflict returns prepared pages and COW snapshot owners");
}
static void fault_candidates(void)
{
    uint64_t mapped;
    race_address = address + BOAROS_PAGE_SIZE;
    assert(!kernel_mm_mmap_anonymous(&mm[0], race_address, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &mapped));
    uint64_t baseline = physical_page_available(&allocator);
    memset(prepare_entered, 0, sizeof(prepare_entered)); prepare_release = 0; capture_mask = 6;
    pthread_t one, two;
    assert(!pthread_create(&one, 0, race_fault, (void *)(uintptr_t)1));
    assert(!pthread_create(&two, 0, race_fault, (void *)(uintptr_t)2));
    prepared(1); prepared(2); /* 两个真实快照均已释放gate，尚未发布候选。 */
    __atomic_store_n(&prepare_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(one, 0) && !pthread_join(two, 0));
    assert(!race_status[1] && !race_status[2]);
    assert(physical_page_available(&allocator) + 1 == baseline);
    unsigned char result[3]; size_t copied;
    assert(!kernel_copy_from_user(&mm[0], result, race_address, sizeof(result), &copied));
    assert(!result[0] && result[1] == 0x91 && result[2] == 0x92);
    assert(!kernel_mm_munmap(&mm[0], race_address, BOAROS_PAGE_SIZE));
    assert(physical_page_available(&allocator) == baseline);

    assert(!kernel_mm_mmap_anonymous(&mm[0], race_address, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &mapped));
    memset(prepare_entered, 0, sizeof(prepare_entered)); prepare_release = 0; capture_mask = 2;
    assert(!pthread_create(&one, 0, race_fault, (void *)(uintptr_t)1)); prepared(1);
    assert(!kernel_mm_munmap(&mm[0], race_address, BOAROS_PAGE_SIZE));
    assert(!kernel_mm_mmap_anonymous(&mm[0], race_address, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ, KERNEL_MM_MAP_FIXED, &mapped));
    __atomic_store_n(&prepare_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(one, 0) && race_status[1] == KERNEL_UACCESS_STATUS_FAULT);
    struct kernel_mm_mapping mapping;
    assert(kernel_mm_lookup(&mm[0], race_address, &mapping) == KERNEL_MM_STATUS_NOT_MAPPED);
    assert(!kernel_mm_munmap(&mm[0], race_address, BOAROS_PAGE_SIZE));
    assert(physical_page_available(&allocator) == baseline);

    /* 新root分支需要data+两级表；只留两页，第二张表OOM后全部回滚。 */
    race_address = UINT64_C(0x40000000);
    assert(!kernel_mm_mmap_anonymous(&mm[0], race_address, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &mapped));
    uint64_t pressure[4096]; unsigned used = 0;
    while (used < 4096 && physical_page_available(&allocator) > 2)
        assert(!physical_page_allocate(&allocator, &pressure[used++]));
    unsigned char value = 1;
    assert(kernel_copy_to_user(&mm[0], race_address, &value, 1, &copied) == KERNEL_UACCESS_STATUS_FAULT && !copied);
    assert(physical_page_available(&allocator) == 2);
    assert(kernel_mm_lookup(&mm[0], race_address, &mapping) == KERNEL_MM_STATUS_NOT_MAPPED);
    for (unsigned i = 0; i < used; i++) assert(!physical_page_release(&allocator, pressure[i]));
    assert(!kernel_mm_munmap(&mm[0], race_address, BOAROS_PAGE_SIZE));
    puts("MM simultaneous faults, stale candidates and second-table OOM return actual owners");
}
/* A publisher waiting for its real fence must not serialize faults at other VAs. */
static void *parallel_fault(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    mm_test_attach(id); mm_test_context = context;
    assert(!kernel_mm_resolve_user_fault(&mm[id], parallel_address[id], KERNEL_MM_WRITE));
    __atomic_store_n(&done[id], 1, __ATOMIC_RELEASE);
    return 0;
}
static void parallel_publication(void)
{
    uint64_t mapped;
    uint64_t start = UINT64_C(0x80000000);
    assert(!kernel_mm_mmap_anonymous(&mm[0], start, 2 * BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &mapped));
    parallel_address[1] = start; parallel_address[2] = start + BOAROS_PAGE_SIZE;
    for (unsigned cow = 0; cow < 2; cow++) {
        struct kernel_mm_mapping old_first = {0}, old_second = {0};
        if (cow) {
            assert(!kernel_mm_fork(&child, &mm[0]));
            assert(!kernel_mm_lookup(&child, start, &old_first));
            assert(!kernel_mm_lookup(&child, start + BOAROS_PAGE_SIZE, &old_second));
        }
        done[1] = done[2] = fence_entered = fence_release = 0;
        hold_fence = 1;
        pthread_t one, two;
        assert(!pthread_create(&one, 0, parallel_fault, (void *)(uintptr_t)1));
        while (!__atomic_load_n(&fence_entered, __ATOMIC_ACQUIRE)) sched_yield();
        park_before[2] = mm_test_parked(2);
        assert(!pthread_create(&two, 0, parallel_fault, (void *)(uintptr_t)2));
        while (!__atomic_load_n(&done[2], __ATOMIC_ACQUIRE) && mm_test_parked(2) == park_before[2]) sched_yield();
        assert(__atomic_load_n(&done[2], __ATOMIC_ACQUIRE) && "independent fault blocked behind another publisher's fence");
        assert(!__atomic_load_n(&done[1], __ATOMIC_ACQUIRE));
        struct kernel_mm_mapping first, second;
        assert(!kernel_mm_lookup(&mm[0], start, &first));
        assert(!kernel_mm_lookup(&mm[0], start + BOAROS_PAGE_SIZE, &second));
        assert(first.physical_address != second.physical_address);
        if (cow) {
            uint32_t references;
            assert(first.physical_address != old_first.physical_address &&
                second.physical_address != old_second.physical_address);
            assert(!physical_page_reference_count(&allocator, old_first.physical_address, &references) && references == 3);
            assert(!physical_page_reference_count(&allocator, old_second.physical_address, &references) && references == 1);
            struct kernel_mm_concurrency_state state;
            kernel_mm_get_concurrency_state(&mm[0], &state);
            assert(state.pending_retirement == 1); /* peer completion cannot return this publisher's old owners */
        }
        __atomic_store_n(&fence_release, 1, __ATOMIC_RELEASE);
        assert(!pthread_join(one, 0) && !pthread_join(two, 0));
        hold_fence = 0;
        if (cow) { assert(!kernel_mm_release(&child)); child = (struct kernel_mm){0}; }
    }
    assert(!kernel_mm_munmap(&mm[0], start, 2 * BOAROS_PAGE_SIZE));
    struct kernel_mm_concurrency_state state;
    kernel_mm_get_concurrency_state(&mm[0], &state);
    assert(!state.pins && !state.write_pins && !state.pending_retirement);
    puts("MM distinct-address anonymous/COW publication progresses with independent delayed-fence owners");
}
static void *populate_parallel(void *unused)
{
    (void)unused; mm_test_attach(1); mm_test_context = context;
    assert(!kernel_mm_mlock(&mm[1], population_start, 32 * BOAROS_PAGE_SIZE, 0));
    __atomic_store_n(&done[1], 1, __ATOMIC_RELEASE);
    return 0;
}
static void *population_edit(void *unused)
{
    (void)unused; mm_test_attach(3); mm_test_context = context;
    assert(!kernel_mm_mprotect(&mm[3], population_start, BOAROS_PAGE_SIZE, KERNEL_MM_READ));
    __atomic_store_n(&done[3], 1, __ATOMIC_RELEASE);
    return 0;
}
static void population_batches(void)
{
    uint64_t mapped;
    population_start = UINT64_C(0xa0000000);
    assert(!kernel_mm_mmap_anonymous(&mm[0], population_start, 33 * BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &mapped));
    parallel_address[2] = population_start + 32 * BOAROS_PAGE_SIZE;
    population_capture = 1; population_entered = population_release = 0;
    done[1] = done[2] = done[3] = 0;
    pthread_t population, fault, editor;
    assert(!pthread_create(&population, 0, populate_parallel, 0));
    while (!__atomic_load_n(&population_entered, __ATOMIC_ACQUIRE)) sched_yield();
    park_before[2] = mm_test_parked(2);
    assert(!pthread_create(&fault, 0, parallel_fault, (void *)(uintptr_t)2));
    while (!__atomic_load_n(&done[2], __ATOMIC_ACQUIRE) && mm_test_parked(2) == park_before[2]) sched_yield();
    assert(__atomic_load_n(&done[2], __ATOMIC_ACQUIRE));
    park_before[3] = mm_test_parked(3);
    assert(!pthread_create(&editor, 0, population_edit, 0));
    await_registration(3);
    __atomic_store_n(&population_release, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(population, 0) && !pthread_join(fault, 0) && !pthread_join(editor, 0));
    assert(done[1] && done[2] && done[3]);
    for (unsigned i = 0; i < 33; i++) {
        struct kernel_mm_mapping mapping;
        assert(!kernel_mm_lookup(&mm[0], population_start + i * BOAROS_PAGE_SIZE, &mapping));
    }
    assert(!kernel_mm_munmap(&mm[0], population_start, 33 * BOAROS_PAGE_SIZE));
    population_start = 0;
    puts("MM shared population preparation and bounded writer admission verified");
}
struct pressure_case { uint64_t pages[4096]; unsigned count, locked, progressed; };
static uint64_t pressure_reclaim(void *context, uint64_t target)
{ (void)context; (void)target; return 0; }
static void pressure_progress(void *context)
{
    struct pressure_case *pressure = context;
    /* Same admission rule as page-cache dirty-pressure waiting: never wait
     * for an owner which may require this caller's MM qualification. */
    if (kernel_io_context_current()->locks) { pressure->locked++; return; }
    assert(pressure->count);
    assert(!physical_page_release(&allocator, pressure->pages[--pressure->count]));
    pressure->progressed++;
}
static void population_pressure(void)
{
    uint64_t start = UINT64_C(0xc0000000), mapped;
    assert(!kernel_mm_mmap_anonymous(&mm[0], start, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &mapped));
    uint64_t baseline = physical_page_available(&allocator);
    struct pressure_case pressure = {0};
    while (physical_page_available(&allocator)) {
        assert(pressure.count < 4096);
        assert(!physical_page_allocate(&allocator, &pressure.pages[pressure.count++]));
    }
    struct physical_page_reclaim_hooks hooks = {
        .reclaim = pressure_reclaim, .wait = pressure_progress, .context = &pressure};
    assert(!physical_page_allocator_set_reclaimer(&allocator, &hooks));
    assert(!kernel_mm_mlock(&mm[0], start, BOAROS_PAGE_SIZE, 0));
    assert(pressure.locked && pressure.progressed);
    assert(!physical_page_allocator_clear_reclaimer(&allocator));
    while (pressure.count) assert(!physical_page_release(&allocator, pressure.pages[--pressure.count]));
    assert(!kernel_mm_munmap(&mm[0], start, BOAROS_PAGE_SIZE));
    assert(physical_page_available(&allocator) == baseline);
    puts("MM population exhausts allocator then permits unlocked pressure progress and returns owners");
}
static void *batch_visible_fault(void *unused)
{
    (void)unused; mm_test_attach(2); mm_test_context = context;
    assert(!kernel_mm_resolve_user_fault(&mm[2], batch_test_start + BOAROS_PAGE_SIZE, KERNEL_MM_WRITE));
    assert(!kernel_mm_resolve_user_fault(&mm[2], batch_test_start + 4 * BOAROS_PAGE_SIZE, KERNEL_MM_WRITE));
    assert(!kernel_mm_resolve_user_fault(&mm[2], batch_test_start + 5 * BOAROS_PAGE_SIZE, KERNEL_MM_EXECUTE));
    __atomic_store_n(&done[2], 1, __ATOMIC_RELEASE);
    return 0;
}
static void population_confirmation(void)
{
    for (unsigned mode = 0; mode < 5; mode++) {
        uint64_t start = UINT64_C(0x100000000) + (uint64_t)mode * UINT64_C(0x40000000), mapped;
        uint32_t permissions = KERNEL_MM_READ | (mode == 4 ? KERNEL_MM_EXECUTE : KERNEL_MM_WRITE);
        assert(!kernel_mm_mmap_anonymous(&mm[0], start, 3 * BOAROS_PAGE_SIZE,
            permissions, KERNEL_MM_MAP_FIXED, &mapped));
        if (!mode) {
            assert(!kernel_mm_mmap_anonymous(&mm[0], start + 4 * BOAROS_PAGE_SIZE, BOAROS_PAGE_SIZE,
                KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED | KERNEL_MM_MAP_SHARED, &mapped));
            assert(!kernel_mm_mmap_anonymous(&mm[0], start + 5 * BOAROS_PAGE_SIZE, BOAROS_PAGE_SIZE,
                KERNEL_MM_READ | KERNEL_MM_EXECUTE, KERNEL_MM_MAP_FIXED, &mapped));
        }
        uint64_t baseline = physical_page_available(&allocator);
        batch_test_start = start; batch_test_mode = mode;
        batch_test_prefix = batch_test_syncs = batch_test_full = batch_test_local = batch_test_instructions = batch_test_neighbors = 0;
        int status = kernel_mm_mlock(&mm[0], start, 3 * BOAROS_PAGE_SIZE, 0);
        assert(status == (mode == 2 ? -KERNEL_EAGAIN : mode == 3 ? -KERNEL_ENOMEM : 0));
        assert(batch_test_prefix && batch_test_full == 1);
        assert(batch_test_instructions == (mode == 4 ? 3U : 0U));
        assert(batch_test_syncs == (mode == 1 || mode == 4 ? 3U : 2U));
        struct kernel_mm_mapping mapping;
        assert(!kernel_mm_lookup(&mm[0], start, &mapping));
        assert(kernel_mm_lookup(&mm[0], start + 2 * BOAROS_PAGE_SIZE, &mapping) ==
            (mode == 2 || mode == 3 ? KERNEL_MM_STATUS_NOT_MAPPED : KERNEL_MM_STATUS_OK));
        batch_test_start = 0;
        assert(!kernel_mm_munmap(&mm[0], start, 3 * BOAROS_PAGE_SIZE));
        if (!mode) {
            assert(!kernel_mm_munmap(&mm[0], start + 4 * BOAROS_PAGE_SIZE, 2 * BOAROS_PAGE_SIZE));
        } else assert(physical_page_available(&allocator) == baseline);
    }
    puts("MM nonleaf confirms immediately before neighboring shared/exec faults; leaf batches flush on release/OOM/error");
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout = {0};
    layout.usable_count = 1; layout.usable[0].base = (uintptr_t)pool;
    layout.usable[0].size = sizeof(pool);
    assert(!physical_page_allocator_init(&allocator, &layout));
    assert(!physical_page_allocator_bind_access(&allocator, page_access));
    assert(!physical_page_allocator_finalize(&allocator));
    assert(!kernel_heap_init(&heap, &allocator, page_address));
    struct arch_mmu_page_table kernel_table = {0};
    struct arch_mmu_user_space space = {0};
    assert(!riscv_sv39_page_table_init(&kernel_table, &allocator));
    kernel_table.state = ARCH_MMU_STATE_ACTIVE;
    uint64_t baseline = physical_page_available(&allocator);
    assert(!arch_mmu_user_space_init(&space, &allocator, &kernel_table));
    assert(!kernel_mm_create(&mm[0], &space));
    assert(!kernel_mm_vma_enable(&mm[0], &heap));
    assert(!kernel_mm_brk_initialize(&mm[0], 0x10000, 0x20000));
    assert(!kernel_mm_context(&mm[0], &context)); mm_test_context = context;
    assert(!kernel_mm_mmap_anonymous(&mm[0], 0x40000, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &address));
    unsigned char value = 37; size_t copied;
    assert(!kernel_copy_to_user(&mm[0], address, &value, 1, &copied) && copied == 1);
#ifndef MM_TEST_BASELINE
    assert(!kernel_mm_check_task_domain(&mm[0]));
#endif
    for (unsigned id = 1; id < 4; id++) assert(!kernel_mm_acquire(&mm[id], &mm[0]));
    for (unsigned actors = 2; actors <= 4; actors += 2) {
        memset(done, 0, sizeof(done));
        assert(!kernel_mm_mprotect(&mm[0], address, BOAROS_PAGE_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE));
        struct kernel_mm_user_pin old = {0};
        assert(!kernel_mm_pin_user_page(&mm[0], address, KERNEL_MM_WRITE, &old));
        assert(!kernel_mm_mprotect(&mm[0], address, 0, KERNEL_MM_READ));
        assert(kernel_mm_mprotect(&mm[0], address + 1, BOAROS_PAGE_SIZE, KERNEL_MM_READ) == KERNEL_MM_STATUS_INVALID_ARGUMENT);
        pthread_t threads[3];
        park_before[1] = mm_test_parked(1);
        assert(!pthread_create(&threads[0], 0, downgrade, 0));
        await_registration(1); /* 已提交阻塞，不用sleep猜许可关闭。 */
        assert(!kernel_mm_mprotect(&mm[0], address, 0, KERNEL_MM_READ));
        struct kernel_mm_user_pin untouched = {0};
        struct kernel_mm_user_pin saved = old;
        uint64_t available = physical_page_available(&allocator);
        struct kernel_mm_mapping before, after;
        assert(!kernel_mm_lookup(&mm[0], address, &before));
        fputs("self-pin: modifier registered; operations must not park\n", stderr);
        assert(kernel_mm_munmap(&mm[0], address, BOAROS_PAGE_SIZE) == KERNEL_MM_STATUS_CONFLICT);
        assert(kernel_mm_msync(&mm[0], address, BOAROS_PAGE_SIZE, 4) == -KERNEL_EAGAIN);
        assert(kernel_mm_pin_user_page(&mm[0], address, KERNEL_MM_READ, &untouched) == KERNEL_MM_STATUS_CONFLICT);
        assert(!memcmp(&old, &saved, sizeof(old)) && !untouched.registration);
        assert(!kernel_mm_lookup(&mm[0], address, &after));
        assert(before.physical_address == after.physical_address && before.permissions == after.permissions);
        assert(physical_page_available(&allocator) == available);
        assert(!kernel_io_context_current()->locks && kernel_io_context_current()->user_pins == &old);
        for (unsigned id = 2; id < actors; id++) {
            park_before[id] = mm_test_parked(id);
            assert(!pthread_create(&threads[id - 1], 0, id == 3 ? query : new_access, (void *)(uintptr_t)id));
            await_registration(id);
        }
        ((unsigned char *)old.page)[0] = 49;
        kernel_mm_unpin_user_page(&old);
        for (unsigned id = 1; id < actors; id++) assert(!pthread_join(threads[id - 1], 0));
        for (unsigned id = 1; id < actors; id++) assert(__atomic_load_n(&done[id], __ATOMIC_ACQUIRE));
        assert(!kernel_copy_from_user(&mm[0], &value, address, 1, &copied) && value == 49);
    }
#ifndef MM_TEST_BASELINE
    uint64_t unavailable_address;
    assert(!kernel_mm_mmap_anonymous(&mm[0], 0x60000000, BOAROS_PAGE_SIZE,
        KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED, &unavailable_address));
    uint64_t before_access = physical_page_available(&allocator);
    __atomic_store_n(&fail_next_access, 1, __ATOMIC_RELEASE);
    assert(kernel_mm_resolve_user_fault(&mm[0], unavailable_address, KERNEL_MM_WRITE) == KERNEL_MM_STATUS_PAGE_ACCESS);
    assert(physical_page_available(&allocator) == before_access);
    assert(!kernel_mm_munmap(&mm[0], unavailable_address, BOAROS_PAGE_SIZE));
    population_confirmation();
    parallel_publication();
    population_batches();
    population_pressure();
    self_pin_fault();
    fault_candidates();
    fork_cow();
    retirement();
#else
    assert(!kernel_mm_munmap(&mm[0], address, BOAROS_PAGE_SIZE));
#endif
    mm_test_context = 0;
    for (unsigned id = 0; id < 4; id++) assert(!kernel_mm_release(&mm[id]));
    assert(physical_page_available(&allocator) == baseline);
    assert(!physical_page_allocator_audit(&allocator));
    puts("MM 2/4 actor permission drain, delayed fence retirement and final owners verified");
}
