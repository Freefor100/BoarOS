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

static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct kernel_vfs_file file;
static struct riscv_virtio_mmio_block device;
static unsigned char payload[4096];
static unsigned loading_probe, loads, done, write_probe;
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
static void cleanup_worker(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    for (unsigned i = 0; i < 2; i++) check(kernel_vfs_close(&cold[i]) == 0, 38);
    check(kernel_vfs_close(&file) == 0 && kernel_vfs_unmount(&mount) == 0 &&
          kernel_page_cache_destroy(&cache) == KERNEL_PAGE_CACHE_STATUS_OK &&
          riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, 15);
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
