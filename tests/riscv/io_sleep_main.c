#include <arch/riscv/context.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/timer.h>
#include <kernel/time.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>
#include "../../fs/vfs_internal.h"
#include "../../kernel/sched/private.h"
#include <string.h>

static unsigned char pool[4 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct kernel_vfs_file file;
static struct riscv_virtio_mmio_block device;
static unsigned char payload[4096];
static unsigned loading_probe, loads, done, write_probe, background_fail;
static uint64_t pressure_pages[1024], failed_offset;
static struct kernel_wait_queue held_write;
static unsigned stop_probe, stop_entered, stop_called, stop_done;
static uint64_t observed[2], inserted_page;
static struct kernel_page_cache_alias alias;
static void rearm(void *owner, uint64_t address) { (void)owner; (void)address; }
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];
static void check(int good, unsigned id)
{
    if (!good) { virt_uart_puts("I/O sleep failed: "); virt_uart_put_hex(id); virt_uart_putc('\n'); sbi_shutdown(); }
}
static void print_counters(const char *phase, const struct riscv_virtio_mmio_block_statistics *stats)
{
    virt_uart_puts("I/O counters: "); virt_uart_puts(phase);
    virt_uart_puts(" submitted="); virt_uart_put_hex(stats->requests);
    virt_uart_puts(" max-inflight="); virt_uart_put_hex(stats->max_inflight);
    virt_uart_puts(" irq="); virt_uart_put_hex(stats->interrupts);
    virt_uart_puts(" sleeps="); virt_uart_put_hex(stats->sleeps);
    virt_uart_puts(" wakes="); virt_uart_put_hex(stats->wakes);
    virt_uart_puts(" queue-waits="); virt_uart_put_hex(stats->queue_waits);
    virt_uart_puts(" runtime-polls="); virt_uart_put_hex(stats->runtime_polls);
    virt_uart_putc('\n');
}
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
static int dma(const void *p, uint64_t n, uint64_t *out) { (void)n; *out = (uintptr_t)p; return 1; }
int __real_kernel_vfs_node_pread(struct kernel_vfs_node *, uint64_t, void *, size_t, size_t *);
int __wrap_kernel_vfs_node_pread(struct kernel_vfs_node *node, uint64_t offset, void *buffer, size_t size, size_t *count)
{
    if (loading_probe) {
        loads++;
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 10);
        riscv_interrupt_restore(irq);
    }
    return __real_kernel_vfs_node_pread(node, offset, buffer, size, count);
}
int __real_kernel_vfs_node_writeback(struct kernel_vfs_node *, uint64_t, const void *, size_t, size_t *);
int __wrap_kernel_vfs_node_writeback(struct kernel_vfs_node *node, uint64_t offset, const void *buffer, size_t size, size_t *written)
{
    if (stop_probe && kernel_io_context_current()->background_reclaim) {
        stop_probe = 0;
        stop_entered = 1;
        enum kernel_wait_wake_reason reason;
        check(kernel_scheduler_block_current(&held_write, 0, 0, &reason) == KERNEL_SCHEDULER_STATUS_OK, 116);
    }
    if (background_fail && kernel_io_context_current()->background_reclaim) {
        background_fail = 0;
        failed_offset = offset;
        *written = 0;
        return -5;
    }
    if (write_probe) {
        unsigned char first = *(const unsigned char *)buffer;
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 20);
        check(*(const unsigned char *)buffer == first, 21);
        riscv_interrupt_restore(irq);
    }
    return __real_kernel_vfs_node_writeback(node, offset, buffer, size, written);
}
static void write_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!argument) {
        uint64_t sequence = 0;
        check(kernel_vfs_sync(&file, 0, &sequence) == 0, 22);
    } else {
        size_t valid;
        check(kernel_page_cache_get(&cache, &file, 1, &inserted_page, &valid) == KERNEL_PAGE_CACHE_STATUS_OK && valid == 4096, 28);
        check(physical_page_release(&allocator, inserted_page) == PHYSICAL_PAGE_STATUS_OK, 29);
        payload[0]++;
        kernel_page_cache_alias_mark_dirty(&alias);
        ((unsigned char *)access_page(observed[0]))[0] = payload[0];
    }
    riscv_interrupt_restore(irq);
}
static struct kernel_vfs_file orphan;
static void orphan_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!argument)
        check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(&orphan)) == 0, 71);
    else
        check(kernel_vfs_close(&orphan) == 0, 72);
    riscv_interrupt_restore(irq);
}
static void reader(void *arg)
{
    uintptr_t irq = riscv_interrupt_save();
    unsigned index = (uintptr_t)arg;
    size_t valid;
    check(kernel_page_cache_get(&cache, &file, 0, &observed[index], &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 11);
    check(valid == sizeof(payload) && !memcmp(access_page(observed[index]), payload, valid), 12);
    check(physical_page_release(&allocator, observed[index]) == PHYSICAL_PAGE_STATUS_OK, 13);
    done++;
    riscv_interrupt_restore(irq);
}
static struct kernel_vfs_file cold[2];
static unsigned cold_done, progress_done;
static struct kernel_task *cold_tasks[2];
static void cold_reader(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    unsigned index = (uintptr_t)argument;
    cold_tasks[index] = kernel_task_current();
    uint64_t address;
    size_t valid;
    check(kernel_page_cache_get(&cache, &cold[index], 0, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 30);
    unsigned char *data = access_page(address);
    check(valid == 4096, 31);
    for (unsigned i = 0; i < 4096; i++) check(data[i] == (unsigned char)(i * 19 + index), 32);
    check(physical_page_release(&allocator, address) == PHYSICAL_PAGE_STATUS_OK, 33);
    cold_done++;
    riscv_interrupt_restore(irq);
}
static void progress_worker(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    uint64_t address;
    size_t valid;
    check(!cold_done && kernel_page_cache_get(&cache, &file, 0, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 34);
    check(valid == sizeof(payload) && !memcmp(access_page(address), payload, valid), 35);
    check(physical_page_release(&allocator, address) == PHYSICAL_PAGE_STATUS_OK, 36);
    unsigned char cached[64];
    size_t copied;
    check(kernel_vfs_pread(&file, 17, cached, sizeof(cached), &copied) == 0 &&
          copied == sizeof(cached) && !memcmp(cached, payload + 17, copied) && !cold_done, 78);
    /* Pending termination must not detach an internal DMA wait. The normal
     * return-to-user path handles the pending request after stack unwind. */
    for (unsigned i = 0; i < 2; i++) {
        check(cold_tasks[i] && cold_tasks[i]->state == KERNEL_THREAD_STATE_BLOCKED, 76);
        cold_tasks[i]->terminate_requested = 1;
        check(kernel_scheduler_wake_signal(cold_tasks[i]) == KERNEL_SCHEDULER_STATUS_OK &&
              cold_tasks[i]->state == KERNEL_THREAD_STATE_BLOCKED, 77);
    }
    volatile unsigned sum = 0;
    for (unsigned i = 0; i < 10000; i++) sum += i;
    check(sum == 49995000 && !cold_done, 37);
    progress_done = 1;
    virt_uart_puts("I/O handshake: progress\n");
    riscv_interrupt_restore(irq);
}
static unsigned queue_writers, flush_done, queue_done;
static void queue_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    uintptr_t index = (uintptr_t)argument;
    unsigned char sector[512] __attribute__((aligned(16)));
    if (index == 8) {
        check(kernel_block_flush(&device.block) == KERNEL_BLOCK_STATUS_OK && queue_writers == 8, 50);
        flush_done = 1;
    } else {
        memset(sector, (int)index + 1, sizeof(sector));
        enum kernel_block_status result = index < 8
            ? kernel_block_write_at(&device.block, 120 * 1024 * 1024 + index * 4096, sector, sizeof(sector))
            : kernel_block_read_at(&device.block, 120 * 1024 * 1024, sector, sizeof(sector));
        check(result == KERNEL_BLOCK_STATUS_OK, 51);
        if (index < 8) queue_writers++;
        else {
            check(flush_done, 52);
            for (unsigned i = 0; i < sizeof(sector); i++) check(sector[i] == 1, 79);
        }
    }
    queue_done++;
    riscv_interrupt_restore(irq);
}
static unsigned timed_out;
static uint64_t timebase;
static void timeout_worker(void *argument)
{
    uintptr_t irq = riscv_interrupt_save();
    unsigned char sector[512] __attribute__((aligned(16)));
    check(kernel_block_read_at(&device.block, 121 * 1024 * 1024 + (uintptr_t)argument * 4096,
                              sector, sizeof(sector)) == KERNEL_BLOCK_STATUS_TIMEOUT, 60);
    check(kernel_block_read_at(&device.block, 0, sector, sizeof(sector)) == KERNEL_BLOCK_STATUS_IO, 61);
    timed_out++;
    riscv_interrupt_restore(irq);
}
static void stop_writeback_worker(void *unused)
{
    (void)unused;
    (void)riscv_interrupt_save();
    while (!stop_entered) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 117);
    stop_called = 1;
    kernel_page_cache_stop_worker(&cache);
    stop_done = 1;
}
static void background_writeback_probe(void *unused)
{
    (void)unused;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_vfs_file target = {0};
    check(kernel_vfs_create(&mount, "/background", 0600, &target) == 0, 90);
    /* 分别耗尽快照页和线程构造资源；启动失败必须完整回滚。 */
    unsigned startup_held = 0;
    while (startup_held < 1024 && physical_page_allocate(&allocator,
                &pressure_pages[startup_held]) == PHYSICAL_PAGE_STATUS_OK) startup_held++;
    check(startup_held && startup_held < 1024 && !physical_page_available(&allocator), 111);
    check(kernel_page_cache_start_worker(&cache) == -12 && !physical_page_available(&allocator), 112);
    check(physical_page_release(&allocator, pressure_pages[--startup_held]) == PHYSICAL_PAGE_STATUS_OK, 113);
    check(kernel_page_cache_start_worker(&cache) == -12 && physical_page_available(&allocator) == 1, 114);
    while (startup_held) check(physical_page_release(&allocator,
                pressure_pages[--startup_held]) == PHYSICAL_PAGE_STATUS_OK, 115);
    check(kernel_page_cache_start_worker(&cache) == 0, 91);
    /* 没有候选的一轮必须返回并休眠，不能自行反复扫描。 */
    allocator.pressure_wait(allocator.pressure_context);
    struct kernel_page_cache_statistics empty_before, empty_after;
    kernel_page_cache_get_statistics(&cache, &empty_before);
    for (unsigned i = 0; i < 8; i++)
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 127);
    kernel_page_cache_get_statistics(&cache, &empty_after);
    check(empty_before.worker_scanned == empty_after.worker_scanned &&
          empty_before.worker_written == empty_after.worker_written, 128);
    background_fail = 1;
    unsigned pages = (unsigned)(physical_page_total(&allocator) / 10 + 16);
    for (unsigned i = 0; i < pages; i++) {
        size_t written;
        memset(payload, (int)(i % 251 + 1), sizeof(payload));
        check(kernel_vfs_pwrite(&target, (uint64_t)i * sizeof(payload), payload,
                               sizeof(payload), &written) == 0 && written == sizeof(payload), 92);
    }
    struct kernel_memory_statistics memory;
    uint64_t deadline = riscv_time_read() + 10 * timebase;
    do {
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 93);
        kernel_memory_snapshot(&allocator, &memory);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
        (void)riscv_interrupt_save();
    } while (memory.dirty > physical_page_total(&allocator) / 20 * 4096 && riscv_time_read() < deadline);
    check(memory.dirty <= physical_page_total(&allocator) / 20 * 4096, 94);
    struct kernel_page_cache_statistics worker_stats = {0};
    kernel_page_cache_get_statistics(&cache, &worker_stats);
    check(!background_fail && worker_stats.worker_failed == 1 && worker_stats.worker_written > 0, 100);
    check(worker_stats.worker_batches > 1 &&
          worker_stats.worker_scanned <= worker_stats.worker_batches * 64, 125);
    struct riscv_virtio_mmio_block_statistics query_before, query_after;
    riscv_virtio_mmio_block_get_statistics(&device, &query_before);
    kernel_memory_snapshot(&allocator, &memory);
    riscv_virtio_mmio_block_get_statistics(&device, &query_after);
    check(query_before.requests == query_after.requests && memory.available <= memory.total &&
          memory.dirty <= memory.cached && memory.writeback <= memory.cached, 126);
    /* 改写失败页只允许重试，不能提前把它计入可回收预算。 */
    kernel_memory_snapshot(&allocator, &memory);
    uint64_t reclaimable_before = memory.reclaimable;
    size_t changed;
    check(kernel_vfs_pwrite(&target, failed_offset, "R", 1, &changed) == 0 && changed == 1, 109);
    kernel_memory_snapshot(&allocator, &memory);
    check(memory.reclaimable == reclaimable_before, 110);
    uint64_t seq = 0;
    check(kernel_vfs_sync(&target, 0, &seq) == -5, 101);
    check(kernel_vfs_sync(&target, 0, &seq) == 0, 95);
    unsigned held = 0;
    uint64_t pinned;
    size_t valid;
    check(kernel_page_cache_get(&cache, &target, 0, &pinned, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 102);
    while (physical_page_available(&allocator) > physical_page_total(&allocator) / 50) {
        check(held < 1024 && physical_page_allocate(&allocator, &pressure_pages[held]) == PHYSICAL_PAGE_STATUS_OK, 103);
        held++;
    }
    deadline = riscv_time_read() + 10 * timebase;
    while (physical_page_available(&allocator) < physical_page_total(&allocator) / 25 && riscv_time_read() < deadline) {
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 104);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
        (void)riscv_interrupt_save();
    }
    check(physical_page_available(&allocator) >= physical_page_total(&allocator) / 25, 105);
    uint32_t references;
    check(physical_page_reference_count(&allocator, pinned, &references) == PHYSICAL_PAGE_STATUS_OK && references == 2, 106);
    check(physical_page_release(&allocator, pinned) == PHYSICAL_PAGE_STATUS_OK, 107);
    while (held) check(physical_page_release(&allocator, pressure_pages[--held]) == PHYSICAL_PAGE_STATUS_OK, 108);
    kernel_page_cache_stop_worker(&cache);
    /* 暂扣在途写回，stop 必须等待它完成且不能继续提交本批其他页。 */
    for (unsigned i = 0; i < pages; i++) {
        size_t n;
        memset(payload, (int)(i % 251 + 1), sizeof(payload));
        check(kernel_vfs_pwrite(&target, (uint64_t)i * sizeof(payload), payload,
                               sizeof(payload), &n) == 0 && n == sizeof(payload), 118);
    }
    kernel_wait_queue_init(&held_write);
    stop_probe = 1;
    kernel_page_cache_get_statistics(&cache, &worker_stats);
    uint64_t writes_before_stop = worker_stats.worker_written;
    check(kernel_page_cache_start_worker(&cache) == 0, 119);
    struct kernel_thread_join stopper = {0};
    check(kernel_thread_create_joinable(stop_writeback_worker, 0, &stopper) == KERNEL_SCHEDULER_STATUS_OK, 120);
    while (!stop_called) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 121);
    check(stop_entered && !stop_done, 122);
    check(kernel_wait_queue_wake_all(&held_write) == KERNEL_SCHEDULER_STATUS_OK, 123);
    kernel_thread_join(&stopper);
    kernel_page_cache_get_statistics(&cache, &worker_stats);
    check(stop_done && worker_stats.worker_written == writes_before_stop + 1, 124);
    (void)kernel_page_cache_reclaim(&cache, UINT64_MAX);
    size_t count;
    check(kernel_vfs_pread(&target, (uint64_t)(pages - 1) * 4096, payload, sizeof(payload), &count) == 0 &&
          count == sizeof(payload) && payload[0] == (pages - 1) % 251 + 1 && payload[4095] == payload[0], 96);
    check(kernel_vfs_close(&target) == 0, 97);
    riscv_interrupt_restore(irq);
}
static void cleanup_worker(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    for (unsigned i = 0; i < 2; i++) check(kernel_vfs_close(&cold[i]) == 0, 38);
    check(kernel_vfs_close(&file) == 0, 150);
    int unmounted = kernel_vfs_unmount(&mount);
    if (unmounted) { virt_uart_puts("unmount result="); virt_uart_put_hex((unsigned long)unmounted); virt_uart_putc('\n'); }
    check(unmounted == 0, 151);
    check(kernel_page_cache_destroy(&cache) == KERNEL_PAGE_CACHE_STATUS_OK, 152);
    check(riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, 153);
    riscv_interrupt_restore(irq);
}
void kernel_main(unsigned long hart, const void *dtb)
{
    (void)hart;
    struct dtb_boot_info info;
    struct boot_memory_layout layout = {0};
    check(dtb_read_boot_info(dtb, &info) == DTB_STATUS_OK, 1);
    layout.usable_count = 1; layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    check(physical_page_allocator_init(&allocator, &layout) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_bind_access(&allocator, access_page) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_OK &&
          kernel_heap_init(&heap, &allocator, physical) == KERNEL_HEAP_STATUS_OK, 2);
    uint64_t baseline = physical_page_available(&allocator);
    timebase = info.timebase_frequency;
    check(kernel_page_cache_init(&cache, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK, 3);
    int found = 0;
    for (unsigned i = 0; i < info.virtio_mmio_count; i++)
        if (riscv_virtio_mmio_block_init(&device, (void *)(uintptr_t)info.virtio_mmio[i].base,
            info.virtio_mmio[i].size, &allocator, dma, info.timebase_frequency) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) { found = 1; break; }
    check(found && kernel_vfs_mount_root(&mount, &device.block, &heap, &cache) == 0 &&
          kernel_vfs_create(&mount, "/concurrent", 0600, &file) == 0, 4);
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = i * 31;
    size_t written;
    uint64_t sequence = 0;
    check(kernel_vfs_pwrite(&file, 0, payload, sizeof(payload), &written) == 0 && written == sizeof(payload) &&
          kernel_vfs_sync(&file, 0, &sequence) == 0, 5);
    check(kernel_vfs_pwrite(&file, 4096, payload, sizeof(payload), &written) == 0 &&
          kernel_vfs_sync(&file, 0, &sequence) == 0, 5);
    check(kernel_page_cache_reclaim(&cache, 64) != 0, 6);
    check(kernel_scheduler_init(&allocator, (uintptr_t)__boot_stack_bottom, (uintptr_t)__boot_stack_top) == KERNEL_SCHEDULER_STATUS_OK, 7);
    loading_probe = 1;
    for (uintptr_t i = 0; i < 2; i++) check(kernel_thread_create(reader, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 8);
    for (unsigned i = 0; i < 2; i++) {
        struct kernel_thread_completion completion;
        check(kernel_scheduler_on_tick(1) == KERNEL_SCHEDULER_STATUS_OK &&
              kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK, 9);
    }
    loading_probe = 0;
    check(done == 2 && loads == 1 && observed[0] == observed[1], 14);
    payload[0]++;
    check(kernel_vfs_pwrite(&file, 0, payload, sizeof(payload), &written) == 0, 24);
    check(kernel_page_cache_alias_attach(&cache, &file, 0, observed[0],
          &alias, &file, 0, rearm) == KERNEL_PAGE_CACHE_STATUS_OK, 23);
    write_probe = 1;
    check(kernel_thread_create(write_worker, 0) == KERNEL_SCHEDULER_STATUS_OK &&
          kernel_thread_create(write_worker, (void *)1) == KERNEL_SCHEDULER_STATUS_OK, 25);
    for (unsigned i = 0; i < 2; i++) {
        struct kernel_thread_completion completion;
        check(kernel_scheduler_on_tick(1) == KERNEL_SCHEDULER_STATUS_OK &&
              kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK, 26);
    }
    write_probe = 0;
    uint32_t references;
    check(physical_page_reference_count(&allocator, inserted_page, &references) == PHYSICAL_PAGE_STATUS_OK && references == 1, 70);
    kernel_page_cache_alias_detach(&alias);
    check(kernel_vfs_sync(&file, 0, &sequence) == 0, 27);
    check(kernel_vfs_create(&mount, "/orphan", 0600, &orphan) == 0 &&
          kernel_vfs_pwrite(&orphan, 0, payload, sizeof(payload), &written) == 0 &&
          kernel_vfs_unlink(&mount, "/orphan") == 0, 73);
    write_probe = 1;
    check(kernel_thread_create(orphan_worker, 0) == KERNEL_SCHEDULER_STATUS_OK &&
          kernel_thread_create(orphan_worker, (void *)1) == KERNEL_SCHEDULER_STATUS_OK, 74);
    for (unsigned i = 0; i < 2; i++) {
        struct kernel_thread_completion completion;
        check(kernel_scheduler_on_tick(1) == KERNEL_SCHEDULER_STATUS_OK &&
              kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK, 75);
    }
    write_probe = 0;
    /* Clean eviction must release healthy inode metadata before unmount. */
    struct kernel_vfs_file reclaim_file = {0};
    check(kernel_vfs_create(&mount, "/reclaim-owner", 0600, &reclaim_file) == 0 &&
          kernel_vfs_pwrite(&reclaim_file, 0, payload, sizeof(payload), &written) == 0 &&
          kernel_vfs_sync(&reclaim_file, 0, &sequence) == 0 &&
          kernel_vfs_close(&reclaim_file) == 0, 84);
    (void)kernel_page_cache_reclaim(&cache, 64);
    struct kernel_heap_statistics before_reclaim, after_reclaim;
    for (unsigned i = 0; i < 16; i++) {
        struct kernel_vfs_file temporary = {0};
        uint64_t address;
        size_t valid;
        check(kernel_vfs_open(&mount, "/reclaim-owner", &temporary) == 0 &&
              kernel_page_cache_get(&cache, &temporary, 0, &address, &valid) == KERNEL_PAGE_CACHE_STATUS_OK, 80);
        check(physical_page_release(&allocator, address) == PHYSICAL_PAGE_STATUS_OK &&
              kernel_vfs_close(&temporary) == 0, 81);
        check(kernel_page_cache_reclaim(&cache, 64) != 0, 82);
        if (!i) kernel_heap_get_statistics(&heap, &before_reclaim);
    }
    kernel_heap_get_statistics(&heap, &after_reclaim);
    check(before_reclaim.live_allocations == after_reclaim.live_allocations, 83);
    size_t warm_valid;
    check(kernel_page_cache_get(&cache, &file, 0, &inserted_page, &warm_valid) == KERNEL_PAGE_CACHE_STATUS_OK &&
          physical_page_release(&allocator, inserted_page) == PHYSICAL_PAGE_STATUS_OK, 85);
    check(kernel_vfs_open(&mount, "/cold0", &cold[0]) == 0 &&
          kernel_vfs_open(&mount, "/cold1", &cold[1]) == 0, 39);
    struct dtb_irq_info irq_info;
    check(dtb_read_irq_info(dtb, hart, &irq_info) == DTB_STATUS_OK &&
          riscv_plic_init((void *)(uintptr_t)irq_info.plic.base, irq_info.plic.size,
                          irq_info.context, irq_info.source_count), 40);
    uint32_t source = 0;
    for (unsigned i = 0; i < irq_info.route_count; i++)
        if (irq_info.routes[i].base == (uintptr_t)device.mmio) source = irq_info.routes[i].source;
    check(riscv_virtio_mmio_block_enable_irq(&device, source), 41);
    virt_uart_puts("I/O handshake: ready\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 42);
    for (uintptr_t i = 0; i < 2; i++) check(kernel_thread_create(cold_reader, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 43);
    check(kernel_thread_create(progress_worker, 0) == KERNEL_SCHEDULER_STATUS_OK, 44);
    unsigned reaped = 0;
    while (reaped < 3) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 45);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    struct riscv_virtio_mmio_block_statistics stats;
    riscv_virtio_mmio_block_get_statistics(&device, &stats);
    check(cold_done == 2 && progress_done && stats.max_inflight >= 2 && stats.sleeps >= 2 && !stats.runtime_polls, 46);
    virt_uart_puts("I/O handshake: queue\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 53);
    for (uintptr_t i = 0; i < 10; i++) check(kernel_thread_create(queue_worker, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 54);
    reaped = 0;
    while (reaped < 10) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 55);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    riscv_virtio_mmio_block_get_statistics(&device, &stats);
    check(queue_done == 10 && stats.max_inflight == 8 && stats.queue_waits >= 2, 56);
    print_counters("queue", &stats);
    check(kernel_thread_create(background_writeback_probe, 0) == KERNEL_SCHEDULER_STATUS_OK, 98);
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 99);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) break;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    check(kernel_thread_create(cleanup_worker, 0) == KERNEL_SCHEDULER_STATUS_OK, 47);
    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 48);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) break;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }

    check(physical_page_available(&allocator) == baseline, 16);
    volatile void *mmio = device.mmio;
    uint64_t mmio_size = device.mmio_size;
    device = (struct riscv_virtio_mmio_block){0};
    check(riscv_virtio_mmio_block_init(&device, mmio, mmio_size, &allocator, dma,
                                      info.timebase_frequency) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK &&
          riscv_virtio_mmio_block_enable_irq(&device, source), 62);
    virt_uart_puts("I/O handshake: timeout\n");
    while (!virt_uart_rx_ready()) { }
    check(virt_uart_getc() == 'g', 63);
    check(kernel_time_init(info.timebase_frequency, 0) == KERNEL_TIME_STATUS_OK &&
          riscv_timer_start(info.timebase_frequency, 100) == RISCV_TIMER_STATUS_OK, 64);
    for (uintptr_t i = 0; i < 8; i++) check(kernel_thread_create(timeout_worker, (void *)i) == KERNEL_SCHEDULER_STATUS_OK, 65);
    reaped = 0;
    while (reaped < 8) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 66);
        struct kernel_thread_completion completion;
        if (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) reaped++;
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    (void)riscv_interrupt_save();
    riscv_virtio_mmio_block_get_statistics(&device, &stats);
    print_counters("timeout", &stats);
    check(timed_out == 8 && stats.timeouts == 1 && !stats.runtime_polls &&
          riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK &&
          physical_page_available(&allocator) == baseline, 67);
    virt_uart_puts("BoarOS: I/O sleep tests passed\n"); sbi_shutdown();
}
