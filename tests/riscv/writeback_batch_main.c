#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/sync.h>
#include <kernel/time.h>
#include <kernel/vfs.h>
#include "../../fs/vfs_internal.h"
#include <string.h>

#ifndef BOAROS_PAGE_CACHE_WRITEBACK_PAGES
#define BOAROS_PAGE_CACHE_WRITEBACK_PAGES 1
#endif
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct riscv_virtio_mmio_block device;
static unsigned completed, probe, calls, maximum, reject_order, reject_page, injected, fail_write;
static unsigned hold_snapshot, snapshot_waiting, snapshot_changed, alias_rearms;
static uint64_t alias_pages[8];
static struct kernel_page_cache_alias aliases[8];
static struct kernel_wait_queue snapshot_ready;
static size_t submitted;
static unsigned offsets[16], lengths[16];
static unsigned char bytes[4096], actual[4096];
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];

static void check(int good, unsigned id)
{
    if (!good) { virt_uart_puts("writeback failed: "); virt_uart_put_hex(id); virt_uart_putc('\n'); sbi_shutdown(); }
}
static void number(const char *label, uint64_t value)
{ virt_uart_puts(label); virt_uart_put_hex(value); virt_uart_putc('\n'); }
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *pointer, uint64_t *address) { *address = (uintptr_t)pointer; return 1; }
static int dma(const void *pointer, uint64_t size, uint64_t *address) { (void)size; return physical(pointer, address); }
enum physical_page_status __real_physical_page_allocate_order(struct physical_page_allocator *, uint32_t, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate_order(struct physical_page_allocator *a, uint32_t order, uint64_t *out)
{
    if (reject_order && order) {
        if (reject_order != UINT32_MAX) reject_order--;
        injected++; return PHYSICAL_PAGE_STATUS_EMPTY;
    }
    return __real_physical_page_allocate_order(a, order, out);
}
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a, uint64_t *out)
{
    if (reject_page) { injected++; return PHYSICAL_PAGE_STATUS_EMPTY; }
    return __real_physical_page_allocate(a, out);
}
int __real_kernel_vfs_node_writeback(struct kernel_vfs_node *, uint64_t, const void *, size_t, size_t *);
int __wrap_kernel_vfs_node_writeback(struct kernel_vfs_node *node, uint64_t offset, const void *buffer, size_t size, size_t *written)
{
    if (probe) {
        if (calls < 16) { offsets[calls] = (unsigned)offset; lengths[calls] = (unsigned)size; }
        calls++; submitted += size;
        if (maximum < size) maximum = (unsigned)size;
    }
    if (fail_write) {
        *written = fail_write == 1 ? 0 : size - 1;
        return fail_write == 1 ? -KERNEL_EIO : 0;
    }
    if (hold_snapshot) {
        hold_snapshot = 0; snapshot_waiting = 1;
        while (!snapshot_changed) {
            enum kernel_wait_wake_reason reason;
            check(kernel_scheduler_block_current(&snapshot_ready, 0, 0, &reason) == KERNEL_SCHEDULER_STATUS_OK, 70);
        }
        const unsigned char *data = buffer;
        for (size_t i = 0; i < size; i++)
            check(data[i] == (unsigned char)((offset + i) / 4096 + 1), 71);
    }
    return __real_kernel_vfs_node_writeback(node, offset, buffer, size, written);
}
static void reset_probe(void)
{ calls = maximum = 0; submitted = 0; probe = 1; }
static void write_pages(struct kernel_vfs_file *file, unsigned count)
{
    for (unsigned page = 0; page < count; page++) {
        size_t written;
        memset(bytes, (int)(page % 251 + 1), sizeof(bytes));
        check(kernel_vfs_pwrite(file, (uint64_t)page * 4096, bytes, sizeof(bytes), &written) == 0 && written == sizeof(bytes), 20);
    }
}
static void read_pages(struct kernel_vfs_file *file, unsigned count)
{
    for (unsigned page = 0; page < count; page++) {
        size_t read;
        memset(bytes, (int)(page % 251 + 1), sizeof(bytes));
        check(kernel_vfs_pread(file, (uint64_t)page * 4096, actual, sizeof(actual), &read) == 0 && read == sizeof(actual) && !memcmp(bytes, actual, sizeof(bytes)), 21);
    }
}
static void range_cases(struct kernel_vfs_file *file)
{
    uint64_t observed = 0;
    write_pages(file, 9);
    reset_probe();
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 30);
    probe = 0;
    number("writeback range calls: ", calls);
    /* Nine fully dirty pages need the configured bounded number of handoffs. */
    check(calls == (9 + BOAROS_PAGE_CACHE_WRITEBACK_PAGES - 1) / BOAROS_PAGE_CACHE_WRITEBACK_PAGES &&
          maximum == BOAROS_PAGE_CACHE_WRITEBACK_PAGES * 4096 && submitted == 9 * 4096, 31);
    check(kernel_vfs_sync(file, 1, &observed) == 0 &&
          kernel_page_cache_invalidate_node(&cache, kernel_vfs_file_node(file)) == KERNEL_PAGE_CACHE_STATUS_OK, 32);
    read_pages(file, 9);
    /* Adjacent pages with disjoint dirty bytes must keep the clean gap. */
    size_t written;
    unsigned char value = 0x74;
    for (unsigned page = 0; page < 3; page++)
        check(kernel_vfs_pwrite(file, page * 4096 + 17, &value, 1, &written) == 0 && written == 1, 33);
    reset_probe();
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 34);
    probe = 0;
    check(calls == 3 && maximum == 1 && submitted == 3, 35);
    write_pages(file, 9);
    reset_probe();
    check(kernel_page_cache_writeback_range(&cache, kernel_vfs_file_node(file), 0, 2 * 4096 + 37) == 0, 36);
    probe = 0;
    check(submitted == 2 * 4096 + 37 && offsets[calls - 1] + lengths[calls - 1] == 2 * 4096 + 37, 37);
    reset_probe();
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 38);
    probe = 0;
    check(submitted == 7 * 4096 - 37 && offsets[0] == 2 * 4096 + 37, 39);
    check(kernel_vfs_sync(file, 1, &observed) == 0, 40);
    memset(bytes, 0x6b, sizeof(bytes));
    check(kernel_vfs_pwrite(file, 17, bytes, 4096 - 17, &written) == 0 && written == 4096 - 17, 41);
    for (unsigned page = 1; page <= 2; page++)
        check(kernel_vfs_pwrite(file, page * 4096, bytes, 4096, &written) == 0 && written == 4096, 42);
    check(kernel_vfs_pwrite(file, 3 * 4096, bytes, 46, &written) == 0 && written == 46, 43);
    reset_probe();
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 44);
    probe = 0;
    check(submitted == 3 * 4096 + 29 && offsets[0] == 17 &&
          calls == (4 + BOAROS_PAGE_CACHE_WRITEBACK_PAGES - 1) / BOAROS_PAGE_CACHE_WRITEBACK_PAGES, 45);
    check(kernel_vfs_sync(file, 1, &observed) == 0 &&
          kernel_page_cache_invalidate_node(&cache, kernel_vfs_file_node(file)) == KERNEL_PAGE_CACHE_STATUS_OK, 46);
    for (unsigned page = 0; page < 4; page++) {
        size_t read;
        check(kernel_vfs_pread(file, page * 4096, actual, sizeof(actual), &read) == 0 && read == sizeof(actual), 47);
        for (unsigned i = 0; i < 4096; i++) {
            unsigned offset = page * 4096 + i;
            check(actual[i] == (offset >= 17 && offset < 3 * 4096 + 46 ? 0x6b : page + 1), 48);
        }
    }
}
static void allocation_cases(struct kernel_vfs_file *file)
{
    write_pages(file, 8);
    reject_order = UINT32_MAX; injected = 0;
    reset_probe();
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 50);
    probe = reject_order = 0;
    check(calls == 8 && maximum == 4096 && submitted == 8 * 4096, 51);
    if (BOAROS_PAGE_CACHE_WRITEBACK_PAGES > 1) check(injected != 0, 52);
    write_pages(file, 1);
    reject_page = 1;
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == -KERNEL_ENOMEM, 53);
    reject_page = 0;
    reset_probe();
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 54);
    probe = 0;
    check(calls == 1 && submitted == 4096, 55);
}
static void rearm(void *owner, uint64_t address)
{ (void)owner; (void)address; alias_rearms++; }
static void redirty_worker(void *unused)
{
    (void)unused;
    (void)riscv_interrupt_save();
    while (!snapshot_waiting) check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 72);
    for (unsigned i = 0; i < BOAROS_PAGE_CACHE_WRITEBACK_PAGES; i++) {
        kernel_page_cache_alias_mark_dirty(&aliases[i]);
        memset(access_page(alias_pages[i]), (int)(0xa0 + i), 4096);
    }
    snapshot_changed = 1;
    check(kernel_wait_queue_wake_all(&snapshot_ready) == KERNEL_SCHEDULER_STATUS_OK, 73);
}
static void redirty_case(struct kernel_vfs_file *file)
{
    write_pages(file, 8);
    for (unsigned i = 0; i < BOAROS_PAGE_CACHE_WRITEBACK_PAGES; i++) {
        size_t valid;
        check(kernel_page_cache_get(&cache, file, i, &alias_pages[i], &valid) == KERNEL_PAGE_CACHE_STATUS_OK &&
              kernel_page_cache_alias_attach(&cache, file, i, alias_pages[i], &aliases[i], &aliases[i], i * 4096, rearm) == KERNEL_PAGE_CACHE_STATUS_OK, 74);
    }
    kernel_wait_queue_init(&snapshot_ready);
    hold_snapshot = 1; snapshot_waiting = snapshot_changed = alias_rearms = 0;
    struct kernel_thread_join writer = {0};
    check(kernel_thread_create_joinable(redirty_worker, NULL, &writer) == KERNEL_SCHEDULER_STATUS_OK, 75);
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 76);
    kernel_thread_join(&writer);
    check(snapshot_changed && alias_rearms == BOAROS_PAGE_CACHE_WRITEBACK_PAGES, 77);
    reset_probe();
    check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 78);
    probe = 0;
    /* The completed old generation must not remove any redirtied member. */
    check(calls == 1 && submitted == BOAROS_PAGE_CACHE_WRITEBACK_PAGES * 4096, 79);
    for (unsigned i = 0; i < BOAROS_PAGE_CACHE_WRITEBACK_PAGES; i++) {
        kernel_page_cache_alias_detach(&aliases[i]);
        check(physical_page_release(&allocator, alias_pages[i]) == PHYSICAL_PAGE_STATUS_OK, 80);
    }
    uint64_t observed = 0;
    check(kernel_vfs_sync(file, 1, &observed) == 0 &&
          kernel_page_cache_invalidate_node(&cache, kernel_vfs_file_node(file)) == KERNEL_PAGE_CACHE_STATUS_OK, 81);
    for (unsigned i = 0; i < 8; i++) {
        size_t read;
        check(kernel_vfs_pread(file, i * 4096, actual, sizeof(actual), &read) == 0 && read == sizeof(actual), 82);
        memset(bytes, i < BOAROS_PAGE_CACHE_WRITEBACK_PAGES ? (int)(0xa0 + i) : (int)(i + 1), sizeof(bytes));
        check(!memcmp(actual, bytes, sizeof(bytes)), 83);
    }
}
static void error_cases(struct kernel_vfs_file *file)
{
    for (unsigned kind = 1; kind <= 2; kind++) {
        write_pages(file, 8);
        fail_write = kind;
        reset_probe();
        check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == -KERNEL_EIO, 90);
        probe = fail_write = 0;
        struct kernel_memory_statistics memory;
        kernel_memory_snapshot(&allocator, &memory);
        check(!memory.writeback && memory.dirty == 8 * 4096, 91);
        reset_probe();
        check(kernel_page_cache_writeback(&cache, kernel_vfs_file_node(file)) == 0, 92);
        probe = 0;
        check(submitted == 8 * 4096, 93);
    }
}
static void background_case(struct kernel_vfs_file *file, int degraded)
{
    uint64_t observed = 0;
    check(kernel_vfs_sync(file, 1, &observed) == 0 && kernel_vfs_ftruncate(file, 0) == 0, 60);
    /* Publish all dirty pages before starting the threshold worker. */
    unsigned pages = (unsigned)(physical_page_total(&allocator) / 10 + 8);
    write_pages(file, pages);
    struct kernel_page_cache_statistics before;
    kernel_page_cache_get_statistics(&cache, &before);
    /* 只拒绝预留降级的候选分配，随后创建 worker 的内核栈仍正常分配。 */
    reject_order = degraded ? (unsigned)__builtin_ctz(BOAROS_PAGE_CACHE_WRITEBACK_PAGES) : 0;
    injected = 0;
    reset_probe();
    check(kernel_page_cache_start_worker(&cache) == 0, 61);
    reject_order = 0;
    if (degraded && BOAROS_PAGE_CACHE_WRITEBACK_PAGES > 1) check(injected != 0, 66);
    uint64_t deadline;
    check(kernel_time_deadline_from_monotonic(kernel_time_monotonic_ns() + UINT64_C(5000000000), &deadline) == KERNEL_TIME_STATUS_OK, 62);
    struct kernel_page_cache_statistics stats = {0};
    while (stats.worker_written < before.worker_written + pages / 3 && riscv_time_read() < deadline) {
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 63);
        riscv_interrupt_restore(RISCV_SSTATUS_SIE); (void)riscv_interrupt_save();
        kernel_page_cache_get_statistics(&cache, &stats);
    }
    kernel_page_cache_stop_worker(&cache);
    probe = 0;
    number(degraded ? "writeback degraded background maximum bytes: " : "writeback background maximum bytes: ", maximum);
    check(stats.worker_written >= before.worker_written + pages / 3 && stats.worker_failed == before.worker_failed &&
          maximum == (degraded ? 1 : BOAROS_PAGE_CACHE_WRITEBACK_PAGES) * 4096, 64);
    check(kernel_vfs_sync(file, 1, &observed) == 0 &&
          kernel_page_cache_invalidate_node(&cache, kernel_vfs_file_node(file)) == KERNEL_PAGE_CACHE_STATUS_OK, 65);
    read_pages(file, pages);
}
static void exercise(void *argument)
{
    (void)argument;
    uintptr_t irq = riscv_interrupt_save();
    check(kernel_vfs_start_journal_worker(&mount) == 0, 10);
    struct kernel_vfs_file file = {0};
    check(kernel_vfs_create(&mount, "/writeback", 0600, &file) == 0, 11);
    range_cases(&file);
    allocation_cases(&file);
    redirty_case(&file);
    error_cases(&file);
    background_case(&file, 0);
    background_case(&file, 1);
    check(kernel_vfs_close(&file) == 0 && kernel_vfs_unmount(&mount) == 0, 12);
    check(kernel_page_cache_destroy(&cache) == KERNEL_PAGE_CACHE_STATUS_OK &&
          riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, 13);
    completed = 1;
    riscv_interrupt_restore(irq);
}
void kernel_main(unsigned long hart, const void *dtb)
{
    struct dtb_boot_info info; struct dtb_irq_info routing;
    struct boot_memory_layout layout = {0};
    check(dtb_read_boot_info(dtb, &info) == DTB_STATUS_OK && dtb_read_irq_info(dtb, hart, &routing) == DTB_STATUS_OK, 1);
    layout.usable_count = 1; layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    check(physical_page_allocator_init(&allocator, &layout) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_bind_access(&allocator, access_page) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_OK &&
          kernel_heap_init(&heap, &allocator, physical) == KERNEL_HEAP_STATUS_OK &&
          kernel_page_cache_init(&cache, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK, 2);
    int found = 0;
    for (unsigned i = 0; i < info.virtio_mmio_count; i++)
        if (riscv_virtio_mmio_block_init(&device, (void *)(uintptr_t)info.virtio_mmio[i].base, info.virtio_mmio[i].size,
                &allocator, dma, info.timebase_frequency) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) { found = 1; break; }
    check(found && kernel_vfs_mount_root(&mount, &device.block, &heap, &cache) == 0, 3);
    check(kernel_scheduler_init(&allocator, (uintptr_t)__boot_stack_bottom, (uintptr_t)__boot_stack_top) == KERNEL_SCHEDULER_STATUS_OK, 4);
    check(riscv_plic_init((void *)(uintptr_t)routing.plic.base, routing.plic.size, routing.context, routing.source_count), 5);
    uint32_t source = 0;
    for (unsigned i = 0; i < routing.route_count; i++) if (routing.routes[i].base == (uintptr_t)device.mmio) source = routing.routes[i].source;
    check(riscv_virtio_mmio_block_enable_irq(&device, source), 6);
    check(kernel_time_init(info.timebase_frequency, 0) == KERNEL_TIME_STATUS_OK &&
          riscv_timer_start(info.timebase_frequency, 100) == RISCV_TIMER_STATUS_OK, 7);
    check(kernel_thread_create(exercise, NULL) == KERNEL_SCHEDULER_STATUS_OK, 8);
    while (!completed) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 9);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    virt_uart_puts("BoarOS: writeback batch tests passed\n"); sbi_shutdown();
}
