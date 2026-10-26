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
#include <kernel/file_mapping.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/sync.h>
#include <kernel/time.h>
#include <kernel/vfs.h>
#include "../../fs/vfs_internal.h"
#include "../../fs/open_file_internal.h"
#include "../../kernel/sched/private.h"
#include <string.h>

#include <kernel/open_file.h>
#ifndef BOAROS_PAGE_CACHE_READAHEAD_PAGES
#define BOAROS_PAGE_CACHE_READAHEAD_PAGES 0
#endif
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct riscv_virtio_mmio_block device;
#define INITIAL_READAHEAD_PAGES (BOAROS_PAGE_CACHE_READAHEAD_PAGES<4?BOAROS_PAGE_CACHE_READAHEAD_PAGES:4)
#ifndef BOAROS_TEST_READAHEAD_HELD
#define BOAROS_TEST_READAHEAD_HELD 0
#endif
static unsigned completed, fail_prefetch;
static uint64_t held_pages[4096];
static uint64_t timebase;
static unsigned char data[4096];
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];
static void check(int good, unsigned id)
{
    if (!good) { virt_uart_puts("readahead failed: "); virt_uart_put_hex(id); virt_uart_putc('\n'); sbi_shutdown(); }
}
static void number(const char *label, uint64_t value)
{ virt_uart_puts(label); virt_uart_put_hex(value); virt_uart_putc('\n'); }
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *pointer, uint64_t *address) { *address = (uintptr_t)pointer; return 1; }
static int dma(const void *pointer, uint64_t size, uint64_t *address) { (void)size; return physical(pointer, address); }

static void read_page(struct kernel_open_file_description *file, unsigned index)
{
    size_t read=0;
    check(kernel_open_file_pread(file,index*4096,data,sizeof(data),&read)==0 && read==sizeof(data),20);
    for(unsigned j=0;j<sizeof(data);j++)check(data[j]==index+1,21);
}
static void yield(void)
{
    check(kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK,22);
    riscv_interrupt_restore(RISCV_SSTATUS_SIE);(void)riscv_interrupt_save();
}
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
#if !BOAROS_TEST_READAHEAD_HELD
static void wait_prefetch(uint64_t completed_before, unsigned loaded)
{
    struct kernel_page_cache_statistics stats;
    uint64_t deadline=riscv_time_read()+2*timebase;
    do {yield();kernel_page_cache_get_statistics(&cache,&stats);}
    while(stats.readahead_completed==completed_before && riscv_time_read()<deadline);
    check(stats.readahead_completed==completed_before+1 && stats.current_pages==loaded,110);
}
static void adaptive_windows(struct kernel_open_file_description *file)
{
    struct kernel_vfs_node *node=kernel_vfs_file_node(&file->file);
    check(!kernel_open_file_seek(file,0),111);
    kernel_page_cache_stop_worker(&cache);
    check(!kernel_page_cache_invalidate_node(&cache,node) && !kernel_page_cache_start_worker(&cache),112);
    struct kernel_page_cache_statistics before,after;
    kernel_page_cache_get_statistics(&cache,&before);
    read_page(file,0);read_page(file,1);
    unsigned end=2+INITIAL_READAHEAD_PAGES,pages=INITIAL_READAHEAD_PAGES,start=2;
    wait_prefetch(before.readahead_completed,end);
    for(unsigned round=0;round<2;round++) {
        kernel_page_cache_get_statistics(&cache,&before);
        for(unsigned page=start;page<end;page++)read_page(file,page);
        start=end;pages*=2;
        if(pages>BOAROS_PAGE_CACHE_READAHEAD_PAGES)pages=BOAROS_PAGE_CACHE_READAHEAD_PAGES;
        end+=pages;wait_prefetch(before.readahead_completed,end);
        check(file->readahead_pages==pages && file->readahead_end==end,113);
    }
    kernel_page_cache_get_statistics(&cache,&before);
    struct kernel_page_cache_prediction hot=kernel_page_cache_readahead(&cache,node,2,INITIAL_READAHEAD_PAGES,0);
    kernel_page_cache_get_statistics(&cache,&after);
    check(!hot.cookie && hot.end==2+INITIAL_READAHEAD_PAGES && after.readahead_queued==before.readahead_queued,114);
    check(!kernel_open_file_seek(file,0),115);
    kernel_page_cache_stop_worker(&cache);
    check(!kernel_page_cache_invalidate_node(&cache,node) && !kernel_page_cache_start_worker(&cache),116);
    kernel_page_cache_get_statistics(&cache,&before);
    read_page(file,0);read_page(file,1);wait_prefetch(before.readahead_completed,2+INITIAL_READAHEAD_PAGES);
    virt_uart_puts("readahead: adaptive windows and resident coverage passed\n");
}
#endif
struct truncate_observation { struct kernel_page_cache_statistics before; unsigned called; };
static void truncate_notification(void *owner, struct kernel_vfs_node *node, uint64_t size)
{
    struct truncate_observation *observation = owner;
    check(size == 52 * 4096 + 37, 90);
    observation->called++;
    KERNEL_LOCK_SCOPE(probe);
    check(kernel_vfs_node_try_read(node, &probe), 99);
    uint64_t epoch;
    check(kernel_file_mapping_epoch(node, &epoch) == -KERNEL_EBUSY, 100);
    kernel_lock_release(&probe);
    kernel_page_cache_get_statistics(&cache, &observation->before);
    check(kernel_page_cache_readahead(&cache,node,52,INITIAL_READAHEAD_PAGES,0).cookie != 0, 94);
    struct kernel_page_cache_statistics stats;
    uint64_t deadline = riscv_time_read() + 2 * timebase;
    do { yield(); kernel_page_cache_get_statistics(&cache, &stats); }
    while (stats.readahead_cancelled == observation->before.readahead_cancelled &&
        stats.readahead_loaded == observation->before.readahead_loaded && riscv_time_read() < deadline);
    check(stats.readahead_cancelled != observation->before.readahead_cancelled ||
        stats.readahead_loaded != observation->before.readahead_loaded, 91);
}
static void truncate_with_queued_read(struct kernel_open_file_description *file)
{
    struct kernel_vfs_node *node = kernel_vfs_file_node(&file->file);
    check(!kernel_open_file_seek(file, 0), 92);
    kernel_page_cache_stop_worker(&cache);
    check(!kernel_page_cache_invalidate_node(&cache, node) && !kernel_page_cache_start_worker(&cache), 92);
    struct truncate_observation observation = {0};
    struct kernel_file_mapping mapping = {.node=node,.owner=&observation,.truncate=truncate_notification};
    KERNEL_LOCK_SCOPE(inode);
    kernel_vfs_node_lock(node, &inode, 1);
    check(!kernel_file_mapping_prepare(node) && !kernel_file_mapping_register(&mapping), 93);
    kernel_lock_release(&inode);
    /* notify窗口实际运行worker；边界页在resize_prepare时尚不存在。 */
    check(!kernel_vfs_ftruncate(&file->file, 52 * 4096 + 37) && observation.called == 1, 95);
    kernel_file_mapping_unregister(&mapping);
    uint64_t address; size_t valid;
    check(!kernel_open_file_get_page(file, 52, &address, &valid) && valid == 37, 96);
    const unsigned char *bytes = access_page(address);
    for (unsigned i = 0; i < 4096; i++) check(bytes[i] == (i < 37 ? 53 : 0), 97);
    check(!physical_page_release(&allocator, address), 98);
    virt_uart_puts("readahead: closed truncate window, absent boundary page and zero tail passed\n");
}
#endif
#if BOAROS_TEST_READAHEAD_HELD
static unsigned prefetch_entered, demand_finished, owner_checked;
static struct kernel_task *producer_task, *demand_task, *stopping_task, *exercise_task;
static struct kernel_thread_join demand_join, observer_join;
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *, struct file_read_span *, size_t, struct file_read_workspace *, const struct file_read_completion *);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node, struct file_read_span *spans, size_t count, struct file_read_workspace *workspace, const struct file_read_completion *completion)
{
    if (kernel_task_current() == exercise_task)
        return __real_kernel_vfs_node_pread_batch(node, spans, count, workspace, completion);
    producer_task = kernel_task_current(); prefetch_entered = 1;
    check(count == INITIAL_READAHEAD_PAGES, 51);
    int inject = fail_prefetch && count;
    int result=__real_kernel_vfs_node_pread_batch(node,spans,count,workspace, inject ? NULL : completion);
    if(inject){
        fail_prefetch=0;spans[0].error=-KERNEL_EIO;spans[0].completed=17;
        if(completion)for(size_t i=0;i<count;i++)completion->complete(completion->context,i,&spans[i]);
        return -KERNEL_EIO;
    }
    return result;
}
static void held_demand(void *argument)
{
    (void)riscv_interrupt_save();
    struct kernel_open_file_description *file=argument;
    while(!prefetch_entered)yield();
    demand_task=kernel_task_current();
    uint64_t address;size_t valid;
    check(kernel_open_file_get_page(file,2,&address,&valid)==KERNEL_PAGE_CACHE_STATUS_OK && valid==4096,52);
    check(physical_page_release(&allocator,address)==PHYSICAL_PAGE_STATUS_OK,53);
    demand_finished=1;
}
static void held_observer(void *unused)
{
    (void)unused;(void)riscv_interrupt_save();
    while(!producer_task || !demand_task || !stopping_task ||
        producer_task->state!=KERNEL_THREAD_STATE_BLOCKED || demand_task->state!=KERNEL_THREAD_STATE_BLOCKED ||
        stopping_task->state!=KERNEL_THREAD_STATE_BLOCKED)yield();
    check(!demand_finished,54);
    virt_uart_puts("readahead: pending\n");
    while(!virt_uart_rx_ready())yield();
    check(virt_uart_getc()=='q',55);
    while(device.queue.inflight!=1 || producer_task->state!=KERNEL_THREAD_STATE_BLOCKED) {
        check(!demand_finished,56);yield();
    }
    check(!demand_finished && producer_task->state==KERNEL_THREAD_STATE_BLOCKED &&
        demand_task->state==KERNEL_THREAD_STATE_BLOCKED && stopping_task->state==KERNEL_THREAD_STATE_BLOCKED,56);
    owner_checked=1;virt_uart_puts("readahead: held-owner\n");
}
#else
int __real_kernel_vfs_node_pread_batch(struct kernel_vfs_node *, struct file_read_span *, size_t, struct file_read_workspace *, const struct file_read_completion *);
int __wrap_kernel_vfs_node_pread_batch(struct kernel_vfs_node *node, struct file_read_span *spans, size_t count, struct file_read_workspace *workspace, const struct file_read_completion *completion)
{
    int inject = fail_prefetch && count;
    int result=__real_kernel_vfs_node_pread_batch(node,spans,count,workspace, inject ? NULL : completion);
    if(inject){
        fail_prefetch=0;spans[0].error=-KERNEL_EIO;spans[0].completed=17;
        if(completion)for(size_t i=0;i<count;i++)completion->complete(completion->context,i,&spans[i]);
        return -KERNEL_EIO;
    }
    return result;
}
#endif
static void exercise(void *unused)
{
    (void)unused;uintptr_t irq=riscv_interrupt_save();
#if BOAROS_TEST_READAHEAD_HELD
    exercise_task = kernel_task_current();
#endif
    struct kernel_open_file_description *file=NULL;int error;
    check(kernel_vfs_start_journal_worker(&mount)==0,15);
    check(kernel_open_file_create(&heap,&mount,"/data",&file,&error)==KERNEL_OPEN_FILE_STATUS_OK && !error,10);
    read_page(file,0);read_page(file,1);
    check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK && kernel_page_cache_start_worker(&cache)==0,11);
#if BOAROS_TEST_READAHEAD_HELD
    virt_uart_puts("readahead: ready\n");
    while(!virt_uart_rx_ready())yield();
    check(virt_uart_getc()=='g',50);
#endif
    read_page(file,0);read_page(file,1);
#if BOAROS_TEST_READAHEAD_HELD
    check(kernel_thread_create_joinable(held_demand,file,&demand_join)==KERNEL_SCHEDULER_STATUS_OK &&
        kernel_thread_create_joinable(held_observer,NULL,&observer_join)==KERNEL_SCHEDULER_STATUS_OK,57);
    while(!prefetch_entered || !demand_task || demand_task->state!=KERNEL_THREAD_STATE_BLOCKED)yield();
    stopping_task=kernel_task_current();
    kernel_page_cache_stop_worker(&cache);
    kernel_thread_join(&demand_join);kernel_thread_join(&observer_join);
    check(owner_checked && demand_finished && !device.queue.inflight && !device.active,58);
    check(kernel_page_cache_start_worker(&cache)==0,59);
#endif
    uint64_t deadline=riscv_time_read()+timebase*2;
    struct kernel_page_cache_statistics stats;
    do {yield();kernel_page_cache_get_statistics(&cache,&stats);}
    while(stats.readahead_loaded!=INITIAL_READAHEAD_PAGES && riscv_time_read()<deadline);
    number("readahead cached pages: ",stats.current_pages);
    check(stats.current_pages==2+INITIAL_READAHEAD_PAGES,30);
    for(unsigned i=0;i!=INITIAL_READAHEAD_PAGES;i++){
        uint64_t address;size_t valid;void *page;
        check(kernel_open_file_lookup_page(file,i+2,&address,&valid)==KERNEL_PAGE_CACHE_STATUS_OK && valid==4096,31);
        check(physical_page_resolve(&allocator,address,&page)==PHYSICAL_PAGE_STATUS_OK,32);
        for(unsigned j=0;j<4096;j++)check(((unsigned char *)page)[j]==i+3,33);
        check(physical_page_release(&allocator,address)==PHYSICAL_PAGE_STATUS_OK,34);
    }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES && !BOAROS_TEST_READAHEAD_HELD
    adaptive_windows(file);
#endif
    /* Queued work must not survive seek, a nonsequential read or the last close. */
    unsigned baseline=2+INITIAL_READAHEAD_PAGES;
    for(unsigned mode=0;mode<3;mode++) {
        check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,40);
        read_page(file,INITIAL_READAHEAD_PAGES);
        read_page(file,INITIAL_READAHEAD_PAGES+1);
        if(mode==0)check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,41);
        if(mode==1)read_page(file,0);
        if(mode==2)check(kernel_open_file_release(&file)==KERNEL_OPEN_FILE_STATUS_OK && !file,42);
        for(unsigned i=0;i<5;i++)yield();
        kernel_page_cache_get_statistics(&cache,&stats);
        check(stats.current_pages==baseline,43);
        if(mode==2)check(kernel_open_file_create(&heap,&mount,"/data",&file,&error)==KERNEL_OPEN_FILE_STATUS_OK && !error,44);
    }
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
    /* A cold discontinuity sleeps; cancel the old queued prediction before that wait. */
    struct kernel_page_cache_statistics discontinuity;
    check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,77);
    read_page(file,INITIAL_READAHEAD_PAGES);read_page(file,INITIAL_READAHEAD_PAGES+1);
    kernel_page_cache_get_statistics(&cache,&discontinuity);
    read_page(file,50);
    for(unsigned i=0;i<5;i++)yield();
    kernel_page_cache_get_statistics(&cache,&stats);
    check(stats.readahead_batches==discontinuity.readahead_batches,78);
    /* A failed speculative page is discarded; other pages and writeback errors stay independent. */
    check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,60);
    struct kernel_page_cache_statistics before;kernel_page_cache_get_statistics(&cache,&before);
    fail_prefetch=1;
    read_page(file,INITIAL_READAHEAD_PAGES);read_page(file,INITIAL_READAHEAD_PAGES+1);
    deadline=riscv_time_read()+timebase*2;
    do {yield();kernel_page_cache_get_statistics(&cache,&stats);}
    while(stats.readahead_failed==before.readahead_failed && riscv_time_read()<deadline);
    check(stats.readahead_failed==before.readahead_failed+1 && !fail_prefetch,61);
    uint64_t address;size_t valid;
    check(kernel_open_file_lookup_page(file,2+INITIAL_READAHEAD_PAGES,&address,&valid)==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND,62);
    read_page(file,2+INITIAL_READAHEAD_PAGES);
    uint64_t observed=0;check(kernel_vfs_sync(&file->file,1,&observed)==0,63);
    /* Low-water cancellation may evict old clean pages, but cannot start this queued read. */
    check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,64);
    read_page(file,2*INITIAL_READAHEAD_PAGES);
    read_page(file,2*INITIAL_READAHEAD_PAGES+1);
    kernel_page_cache_get_statistics(&cache,&before);
    KERNEL_LOCK_SCOPE(pressure_inode);
    kernel_vfs_node_lock(kernel_vfs_file_node(&file->file), &pressure_inode, 1);
    size_t held=0;
    while(physical_page_available(&allocator)>physical_page_total(&allocator)/50) {
        check(held<4096 && physical_page_allocate(&allocator,&held_pages[held])==PHYSICAL_PAGE_STATUS_OK,65);held++;
    }
    /* inode资格让预测保持未发布，实际低水位建立后才允许worker处理取消。 */
    kernel_lock_release(&pressure_inode);
    deadline=riscv_time_read()+timebase*2;
    do {yield();kernel_page_cache_get_statistics(&cache,&stats);}
    while(stats.readahead_cancelled==before.readahead_cancelled && riscv_time_read()<deadline);
    check(stats.readahead_batches==before.readahead_batches && stats.readahead_cancelled>before.readahead_cancelled,66);
    for(size_t i=0;i<held;i++)check(physical_page_release(&allocator,held_pages[i])==PHYSICAL_PAGE_STATUS_OK,67);
    truncate_with_queued_read(file);
    /* Truncate cancels an unpublished prediction before exposing the new EOF. */
    for(unsigned i=0;i<2;i++) {check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,68);read_page(file,20+i);}
    check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,69);
    read_page(file,20);read_page(file,21);
    kernel_page_cache_get_statistics(&cache,&before);
    check(kernel_vfs_ftruncate(&file->file,8192)==0,70);
    for(unsigned i=0;i<5;i++)yield();
    kernel_page_cache_get_statistics(&cache,&stats);
    check(stats.readahead_batches==before.readahead_batches,71);
    check(kernel_open_file_get_page(file,2,&address,&valid)==KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE,72);
    /* Unlink plus last close cancels its weak prediction before orphan retirement. */
    check(kernel_vfs_ftruncate(&file->file,64*4096)==0,73);
    check(kernel_open_file_seek(file,0)==KERNEL_OPEN_FILE_STATUS_OK,74);
    read_page(file,0);read_page(file,1);
    check(kernel_vfs_unlink(&mount,"/data")==0,75);
    check(kernel_open_file_release(&file)==KERNEL_OPEN_FILE_STATUS_OK && !file,76);
    number("readahead cancelled jobs: ",stats.readahead_cancelled);
    number("readahead failed pages: ",stats.readahead_failed);
#else
    (void)held_pages; (void)fail_prefetch;
#endif
    kernel_page_cache_stop_worker(&cache);
    if(file)check(kernel_open_file_release(&file)==KERNEL_OPEN_FILE_STATUS_OK && !file,12);
    check(kernel_vfs_unmount(&mount)==0 && kernel_page_cache_destroy(&cache)==KERNEL_PAGE_CACHE_STATUS_OK,13);
    check(riscv_virtio_mmio_block_destroy(&device)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK,14);
    completed=1;riscv_interrupt_restore(irq);
}
void kernel_main(unsigned long hart, const void *dtb)
{
    struct dtb_boot_info info; struct dtb_irq_info routing;
    struct boot_memory_layout layout = {0};
    check(dtb_read_boot_info(dtb, &info) == DTB_STATUS_OK && dtb_read_irq_info(dtb, hart, &routing) == DTB_STATUS_OK, 1);
    timebase = info.timebase_frequency;
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
    for (unsigned i = 0; i < routing.route_count; i++) if (routing.routes[i].base == (uintptr_t)riscv_virtio_mmio_block_base(&device)) source = routing.routes[i].source;
    check(riscv_virtio_mmio_block_enable_irq(&device, source), 6);
    check(kernel_time_init(info.timebase_frequency, 0) == KERNEL_TIME_STATUS_OK &&
          riscv_timer_start(info.timebase_frequency, 100) == RISCV_TIMER_STATUS_OK, 7);
    check(kernel_thread_create(exercise, NULL) == KERNEL_SCHEDULER_STATUS_OK, 8);
    while (!completed) {
        uintptr_t irq = riscv_interrupt_save();
        check(kernel_scheduler_yield_current() == KERNEL_SCHEDULER_STATUS_OK, 9);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
    }
    virt_uart_puts("BoarOS: readahead tests passed\n"); sbi_shutdown();
}
