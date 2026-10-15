#include <arch/riscv/context.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>
#include "../../fs/files/private.h"
#include "../../fs/vfs_internal.h"
#include "../../kernel/sched/private.h"
#include <string.h>

#ifndef BOAROS_PAGE_CACHE_READAHEAD_PAGES
#define BOAROS_PAGE_CACHE_READAHEAD_PAGES 0
#endif
_Static_assert(BOAROS_PAGE_CACHE_READAHEAD_PAGES == 0, "demand reads require RA0");
_Static_assert(KERNEL_PAGE_CACHE_READ_BATCH_MAX == 8, "exercise the existing eight-page bound");
#define USER UINT64_C(0x21000000)
#define USER_SIZE UINT64_C(0x20000)
#define BUFFER (USER + UINT64_C(0x4000))
#define LENGTH (20U * 4096U + 123U)
#define REQUEST (16U * 4096U)

static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct riscv_virtio_mmio_block device;
static struct kernel_files files;
static struct kernel_mm mm;
static struct riscv_sv39_page_table table;
static struct riscv_sv39_user_space space;
static uint64_t user_satp;
static unsigned model_user, completed;
static unsigned char scratch[4096];
static struct kernel_task *reader_task;
static unsigned reader_done, observe_dma;
static uint64_t dma_pages[8];
static size_t dma_count;
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];

static void check(int good, unsigned id)
{
    if (!good) {
        virt_uart_puts("demand-read failed: "); virt_uart_put_hex(id); virt_uart_putc('\n');
        sbi_shutdown();
    }
}
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *pointer, uint64_t *address) { *address = (uintptr_t)pointer; return 1; }
static int dma(const void *pointer, uint64_t size, uint64_t *address) { (void)size; return physical(pointer, address); }
static void yield(void)
{
    check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 10);
    riscv_interrupt_restore(RISCV_SSTATUS_SIE); (void)riscv_interrupt_save();
}
static void command(char expected)
{
    while (!virt_uart_rx_ready()) yield();
    check(virt_uart_getc() == expected, 11);
}

/* The kernel thread never installs the user page table. Keep real MM faults,
 * pins and copies; model only its active-context check during user access. */
uint64_t __real_riscv_sv39_current_satp(void);
uint64_t __wrap_riscv_sv39_current_satp(void)
{ return model_user ? user_satp : __real_riscv_sv39_current_satp(); }
struct kernel_task *__real_kernel_task_current(void);
struct kernel_task *__wrap_kernel_task_current(void)
{ return model_user ? kernel_cpu_current()->current : __real_kernel_task_current(); }
enum kernel_uaccess_status __real_kernel_copy_to_user(struct kernel_mm *, uint64_t, const void *, size_t, size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_to_user(struct kernel_mm *owner, uint64_t address,
    const void *source, size_t size, size_t *copied)
{
    unsigned previous = model_user; model_user = 1;
    enum kernel_uaccess_status status = __real_kernel_copy_to_user(owner, address, source, size, copied);
    model_user = previous; return status;
}
enum kernel_uaccess_status __real_kernel_copy_from_user(struct kernel_mm *, void *, uint64_t, size_t, size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_from_user(struct kernel_mm *owner, void *destination,
    uint64_t address, size_t size, size_t *copied)
{
    unsigned previous = model_user; model_user = 1;
    enum kernel_uaccess_status status = __real_kernel_copy_from_user(owner, destination, address, size, copied);
    model_user = previous; return status;
}
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *, struct kernel_vfs_read_span *, size_t);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node, struct kernel_vfs_read_span *spans, size_t count)
{
    if (observe_dma && !dma_count) {
        check(count == 8, 12);
        for (size_t i = 0; i < count; i++) {
            check(spans[i].size == 4096 && !((uintptr_t)spans[i].buffer & 4095), 13);
            dma_pages[i] = (uintptr_t)spans[i].buffer;
        }
        dma_count = count;
    }
    return __real_kernel_vfs_node_pread_batch(node, spans, count);
}

static unsigned char pattern(uint64_t offset)
{ return (unsigned char)(offset * 17U + (offset / 4096U) * 31U + 7U); }
static void verify(uint64_t address, uint64_t offset, size_t size, int hole)
{
    for (size_t done = 0; done < size;) {
        size_t part = size - done < sizeof(scratch) ? size - done : sizeof(scratch), copied;
        check(kernel_copy_from_user(&mm, scratch, address + done, part, &copied) == KERNEL_UACCESS_STATUS_OK && copied == part, 20);
        for (size_t i = 0; i < part; i++) {
            uint64_t at = offset + done + i;
            unsigned char expected = hole && at >= 4U * 4096U && at < 12U * 4096U ? 0 : pattern(at);
            check(scratch[i] == expected, 21);
        }
        done += part;
    }
}
static void copy_in(uint64_t address, const void *source, size_t size)
{
    size_t copied;
    check(kernel_copy_to_user(&mm, address, source, size, &copied) == KERNEL_UACCESS_STATUS_OK && copied == size, 22);
}
static void open_file(const char *path)
{
    struct kernel_open_file_description *owner = 0;
    int result;
    check(kernel_open_file_create(&heap, &mount, path, &owner, &result) == KERNEL_OPEN_FILE_STATUS_OK && !result, 23);
    check(kernel_files_install_new_owned_at(&files, 0, 0, &owner) == KERNEL_FILES_STATUS_OK && !owner, 24);
}
static void close_file(void)
{
    int64_t result;
    check(kernel_files_close(&files, 0, &result) == KERNEL_FILES_STATUS_OK && !result, 25);
}
static void seek(uint64_t offset)
{
    int64_t result;
    check(kernel_files_lseek(&files, 0, offset, KERNEL_FILES_SEEK_SET, &result) == KERNEL_FILES_STATUS_OK && result == (int64_t)offset, 26);
}
static void position(uint64_t expected)
{
    int64_t result;
    check(kernel_files_lseek(&files, 0, 0, KERNEL_FILES_SEEK_CUR, &result) == KERNEL_FILES_STATUS_OK && result == (int64_t)expected, 27);
}
static __attribute__((noinline)) void cold(void)
{
    (void)kernel_page_cache_reclaim(&cache, 1024);
    struct kernel_page_cache_statistics stats;
    kernel_page_cache_get_statistics(&cache, &stats);
    check(!stats.current_pages, 28);
}
static void begin(const char *name)
{
    virt_uart_puts("demand-read begin "); virt_uart_puts(name); virt_uart_putc('\n'); command('g');
}
static void end(const char *name)
{
    struct kernel_page_cache_statistics stats;
    kernel_page_cache_get_statistics(&cache, &stats);
    check(!stats.readahead_queued && !stats.readahead_batches && !stats.readahead_loaded &&
          !device.queue.inflight && !device.active, 29);
    virt_uart_puts("demand-read end "); virt_uart_puts(name); virt_uart_putc('\n'); command('c');
}
static int64_t read(uint64_t address, uint64_t count)
{
    int64_t result;
    check(kernel_files_read(&files, &mm, 0, address, count, &result) == KERNEL_FILES_STATUS_OK, 30);
    return result;
}
static int64_t pread(uint64_t address, uint64_t count, uint64_t offset)
{
    int64_t result;
    check(kernel_files_pread(&files, &mm, 0, address, count, offset, &result) == KERNEL_FILES_STATUS_OK, 31);
    return result;
}
static void held_reader(void *unused)
{
    (void)unused; (void)riscv_interrupt_save(); reader_task = kernel_task_current();
    check(read(BUFFER, REQUEST) == REQUEST, 40);
    verify(BUFFER, 0, REQUEST, 0); reader_done = 1;
}
static void held_dma(void)
{
    cold(); seek(0); dma_count = reader_done = 0; reader_task = 0; observe_dma = 1;
    begin("held-owner");
    struct kernel_thread_join reader = {0};
    check(kernel_thread_create_joinable(held_reader, 0, &reader) == KERNEL_SCHEDULER_STATUS_OK, 41);
    while (!reader_task || reader_task->state != KERNEL_THREAD_STATE_BLOCKED) yield();
    check(dma_count == 8 && device.queue.inflight == 1 && device.active == 1 && !reader_done, 42);
    close_file();
    check(kernel_page_cache_reclaim(&cache, 1024) == 0, 43);
    for (size_t i = 0; i < dma_count; i++) {
        uint32_t references;
        check(physical_page_reference_count(&allocator, dma_pages[i], &references) == PHYSICAL_PAGE_STATUS_OK && references, 44);
    }
    reader_task->terminate_requested = 1;
    check(reader_task->state == KERNEL_THREAD_STATE_BLOCKED && !reader_done && device.queue.inflight == 1, 45);
    virt_uart_puts("demand-read held-owner\n");
    kernel_thread_join(&reader);
    check(reader_done, 46); observe_dma = 0; reader_task = 0;
    end("held-owner"); cold();
}

static void exercise(void *unused)
{
    (void)unused; uintptr_t irq = riscv_interrupt_save();
    check(riscv_sv39_page_table_init(&table, &allocator) == RISCV_SV39_STATUS_OK, 50);
    table.state = RISCV_SV39_STATE_ACTIVE;
    check(riscv_sv39_user_space_init(&space, &allocator, &table) == RISCV_SV39_STATUS_OK &&
          riscv_kernel_mm_create(&mm, &space) == KERNEL_MM_STATUS_OK &&
          kernel_mm_vma_enable(&mm, &heap) == KERNEL_MM_STATUS_OK &&
          kernel_mm_brk_initialize(&mm, 0x10000000, 0x70000000) == KERNEL_MM_STATUS_OK &&
          riscv_kernel_mm_satp(&mm, &user_satp) == KERNEL_MM_STATUS_OK &&
          kernel_files_create(&files, &heap) == KERNEL_FILES_STATUS_OK, 51);
    uint64_t address;
    check(kernel_mm_mmap_anonymous(&mm, USER, USER_SIZE, KERNEL_MM_READ | KERNEL_MM_WRITE,
          KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK && address == USER, 52);
    check(kernel_vfs_start_journal_worker(&mount) == 0 && kernel_page_cache_start_worker(&cache) == 0, 53);
    open_file("/data"); cold();
    begin("read");
    check(read(BUFFER, REQUEST) == REQUEST, 60); position(REQUEST); verify(BUFFER, 0, REQUEST, 0);
    end("read");

    seek(37); cold(); begin("pread");
    check(pread(BUFFER, REQUEST, 0) == REQUEST, 61); position(37); verify(BUFFER, 0, REQUEST, 0);
    end("pread");
    seek(0); begin("hot");
    check(read(BUFFER, REQUEST) == REQUEST, 62); position(REQUEST); verify(BUFFER, 0, REQUEST, 0);
    end("hot");

    struct kernel_uaccess_iovec vectors[3] = {{BUFFER + 3, 32755}, {USER, 0}, {USER + 0x10000 + 5, 32781}};
    copy_in(USER, vectors, sizeof(vectors)); seek(0); cold(); begin("readv");
    int64_t result;
    check(kernel_files_readv(&files, &mm, 0, USER, 3, &result) == KERNEL_FILES_STATUS_OK && result == REQUEST, 63);
    position(REQUEST); verify(vectors[0].base, 0, vectors[0].length, 0);
    verify(vectors[2].base, vectors[0].length, vectors[2].length, 0); end("readv");

    seek(37); cold(); begin("unaligned");
    check(pread(BUFFER + 7, REQUEST, 13) == REQUEST, 64);
    position(37); verify(BUFFER + 7, 13, REQUEST, 0); end("unaligned");

    cold();
    static const unsigned warm[] = {1, 4, 7, 10, 13};
    for (size_t i = 0; i < sizeof(warm) / sizeof(warm[0]); i++)
        check(pread(BUFFER, 4096, warm[i] * 4096U) == 4096, 65);
    seek(0); begin("mixed");
    check(read(BUFFER, REQUEST) == REQUEST, 66); position(REQUEST); verify(BUFFER, 0, REQUEST, 0); end("mixed");

    cold(); seek(20U * 4096U - 17); begin("eof");
    check(read(BUFFER, 8192) == 140, 67); position(LENGTH); verify(BUFFER, LENGTH - 140, 140, 0); end("eof");
    begin("at-eof"); check(read(BUFFER, REQUEST) == 0 && read(USER + USER_SIZE, 0) == 0, 68);
    position(LENGTH); end("at-eof");

    cold(); seek(0); begin("efault-prefix");
    check(read(USER + USER_SIZE - 4096 + 13, REQUEST) == 4083, 69);
    position(4083); verify(USER + USER_SIZE - 4096 + 13, 0, 4083, 0); end("efault-prefix");
    cold(); seek(0); begin("efault-first");
    check(read(USER + USER_SIZE, REQUEST) == -KERNEL_EFAULT, 70); position(0); end("efault-first");

    cold(); seek(0); memset(scratch, 0xcd, sizeof(scratch)); copy_in(BUFFER + 32768, scratch, sizeof(scratch));
    begin("eio-prefix"); check(read(BUFFER, REQUEST) == 32768, 71); position(32768); verify(BUFFER, 0, 32768, 0);
    size_t copied;
    check(kernel_copy_from_user(&mm, scratch, BUFFER + 32768, sizeof(scratch), &copied) == KERNEL_UACCESS_STATUS_OK && copied == sizeof(scratch), 72);
    for (size_t i = 0; i < sizeof(scratch); i++) check(scratch[i] == 0xcd, 73);
    end("eio-prefix");
    cold(); seek(0); begin("eio-first"); check(read(BUFFER, REQUEST) == -KERNEL_EIO, 74); position(0); end("eio-first");
    cold(); check(pread(BUFFER, 4096, 4U * 4096U) == 4096, 79); seek(0);
    begin("eio-gap"); check(read(BUFFER, REQUEST) == 5U * 4096U, 80);
    position(5U * 4096U); verify(BUFFER, 0, 5U * 4096U, 0); end("eio-gap");
    cold(); seek(0); begin("retry"); check(read(BUFFER, REQUEST) == REQUEST, 75);
    position(REQUEST); verify(BUFFER, 0, REQUEST, 0); end("retry");

    held_dma();
    open_file("/hole"); cold(); begin("hole");
    check(read(BUFFER, REQUEST) == REQUEST, 76); position(REQUEST); verify(BUFFER, 0, REQUEST, 1); end("hole"); close_file();
    cold();
    kernel_page_cache_stop_worker(&cache);
    check(kernel_files_release(&files) == KERNEL_FILES_STATUS_OK && kernel_mm_release(&mm) == KERNEL_MM_STATUS_OK &&
          physical_page_release(&allocator, table.root_address) == PHYSICAL_PAGE_STATUS_OK, 77);
    check(kernel_vfs_unmount(&mount) == 0 && kernel_page_cache_destroy(&cache) == KERNEL_PAGE_CACHE_STATUS_OK &&
          riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, 78);
    completed = 1; riscv_interrupt_restore(irq);
}

void kernel_main(unsigned long hart, const void *dtb)
{
    struct dtb_boot_info info; struct dtb_irq_info routing; struct boot_memory_layout layout = {0};
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
    for (unsigned i = 0; i < routing.route_count; i++)
        if (routing.routes[i].base == (uintptr_t)riscv_virtio_mmio_block_base(&device)) source = routing.routes[i].source;
    check(riscv_virtio_mmio_block_enable_irq(&device, source), 6);
    check(kernel_time_init(info.timebase_frequency, 0) == KERNEL_TIME_STATUS_OK &&
          riscv_timer_start(info.timebase_frequency, 100) == RISCV_TIMER_STATUS_OK, 7);
    check(kernel_thread_create(exercise, 0) == KERNEL_SCHEDULER_STATUS_OK, 8);
    while (!completed) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 9);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    virt_uart_puts("BoarOS: demand-read tests passed\n"); sbi_shutdown();
}
