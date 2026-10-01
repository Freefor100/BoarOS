#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/cost.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/vfs.h>
#include "../../fs/ext4_backend.h"
#include <ext4_fs.h>
#include <ext4_journal.h>
#include <string.h>

static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_page_cache cache;
static struct kernel_vfs_mount mount;
static struct riscv_virtio_mmio_block device;
static unsigned completed;
static unsigned char bytes[4096], actual[4096];
static volatile unsigned irq_completed;
static unsigned char irq_bytes[512];
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];
static void check(int good, unsigned id)
{
    if (!good) { virt_uart_puts("journal group failed: "); virt_uart_put_hex(id); virt_uart_putc('\n'); sbi_shutdown(); }
}
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *pointer, uint64_t *address) { *address=(uintptr_t)pointer;return 1; }
static int dma(const void *pointer, uint64_t size, uint64_t *address) { (void)size;return physical(pointer,address); }
static void irq_only_reader(void *argument)
{
    (void)argument;
    check(kernel_block_read_at(&device.block,0,irq_bytes,sizeof(irq_bytes))==KERNEL_BLOCK_STATUS_OK,10);
    irq_completed=1;
}
/* Used only by the counterfactual fixture: suppress the public idle return
 * hook while preserving the same actual device interrupt and sleeping task. */
void __wrap_kernel_scheduler_prepare_idle_return(void) { }

static void exercise(void *argument)
{
    (void)argument;
    uintptr_t irq=riscv_interrupt_save();
    int startup=kernel_vfs_start_journal_worker(&mount);
    if(startup){virt_uart_puts("journal startup errno=");virt_uart_put_hex((uint64_t)(int64_t)startup);virt_uart_putc('\n');
        struct lwext4_mount_adapter *a=mount.private_data;
        virt_uart_puts("journal state error/memory/limit=");virt_uart_put_hex(a->device.fs->jbd_journal->error);virt_uart_putc(' ');virt_uart_put_hex(a->device.fs->jbd_journal->memory_used);virt_uart_putc(' ');virt_uart_put_hex(a->device.fs->jbd_journal->memory_limit);virt_uart_putc('\n');}
    check(startup==0,20);
    struct lwext4_mount_adapter *adapter=mount.private_data;
    struct jbd_journal *journal=adapter->device.fs->jbd_journal;
    struct kernel_vfs_file file={0};
    check(kernel_vfs_create(&mount,"/group",0600,&file)==0,21);
    size_t count;uint64_t error=0;
    for(unsigned page=0;page<128;page++) {
        memset(bytes,(int)(page+1),sizeof(bytes));
        check(kernel_vfs_file_modified(&file, 0, 1)==0 &&
            kernel_vfs_pwrite(&file,(uint64_t)page*sizeof(bytes),bytes,sizeof(bytes),&count)==0 && count==sizeof(bytes),22);
    }
    check(kernel_vfs_sync(&file,1,&error)==0,23);
    check(journal->durable_sequence==journal->accepted_sequence && journal->memory_peak<=journal->memory_limit,24);
    for(unsigned page=0;page<128;page++) {
        memset(bytes,(int)(page+1),sizeof(bytes));
        check(kernel_vfs_pread(&file,(uint64_t)page*sizeof(bytes),actual,sizeof(actual),&count)==0 && count==sizeof(actual) && !memcmp(bytes,actual,sizeof(bytes)),25);
    }
    uint64_t before=journal->accepted_sequence;
    check(kernel_vfs_file_modified(&file, 0, 1)==0 && journal->accepted_sequence>before,26);
    uint64_t deadline;
    check(kernel_time_deadline_from_monotonic(kernel_time_monotonic_ns()+200000000,&deadline)==KERNEL_TIME_STATUS_OK,27);
    enum kernel_wait_wake_reason reason;
    check(kernel_scheduler_block_current(NULL,deadline,1,&reason)==KERNEL_SCHEDULER_STATUS_OK && reason==KERNEL_WAIT_TIMEOUT,28);
    check(journal->checkpoint_sequence==journal->accepted_sequence,29);
    check(kernel_vfs_ftruncate(&file,4097)==0 && kernel_vfs_sync(&file,0,&error)==0,30);
    check(kernel_vfs_unlink(&mount,"/group")==0,31);
    memset(bytes,0x92,sizeof(bytes));
    check(kernel_vfs_pwrite(&file,0,bytes,sizeof(bytes),&count)==0 && count==sizeof(bytes) && kernel_vfs_sync(&file,0,&error)==0,32);
    check(kernel_vfs_close(&file)==0,33);
    check(kernel_vfs_unmount(&mount)==0,34);
    check(kernel_page_cache_destroy(&cache)==KERNEL_PAGE_CACHE_STATUS_OK,35);
    check(riscv_virtio_mmio_block_destroy(&device)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK,36);
    completed=1;
    riscv_interrupt_restore(irq);
}

void kernel_main(unsigned long hart,const void *dtb)
{
    struct dtb_boot_info info;struct dtb_irq_info routing;
    struct boot_memory_layout layout={0};
    check(dtb_read_boot_info(dtb,&info)==DTB_STATUS_OK && dtb_read_irq_info(dtb,hart,&routing)==DTB_STATUS_OK,1);
    layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    check(physical_page_allocator_init(&allocator,&layout)==PHYSICAL_PAGE_STATUS_OK &&
        physical_page_allocator_bind_access(&allocator,access_page)==PHYSICAL_PAGE_STATUS_OK &&
        physical_page_allocator_finalize(&allocator)==PHYSICAL_PAGE_STATUS_OK &&
        kernel_heap_init(&heap,&allocator,physical)==KERNEL_HEAP_STATUS_OK &&
        kernel_page_cache_init(&cache,&heap,&allocator)==KERNEL_PAGE_CACHE_STATUS_OK,2);
    int found=0;
    for(unsigned i=0;i<info.virtio_mmio_count;i++)
        if(riscv_virtio_mmio_block_init(&device,(void*)(uintptr_t)info.virtio_mmio[i].base,info.virtio_mmio[i].size,&allocator,dma,info.timebase_frequency)==RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK){found=1;break;}
    check(found && kernel_vfs_mount_root(&mount,&device.block,&heap,&cache)==0,3);
    check(kernel_scheduler_init(&allocator,(uintptr_t)__boot_stack_bottom,(uintptr_t)__boot_stack_top)==KERNEL_SCHEDULER_STATUS_OK,4);
    check(riscv_plic_init((void*)(uintptr_t)routing.plic.base,routing.plic.size,routing.context,routing.source_count),5);
    uint32_t source=0;
    for(unsigned i=0;i<routing.route_count;i++) if(routing.routes[i].base==(uintptr_t)device.mmio)source=routing.routes[i].source;
    check(riscv_virtio_mmio_block_enable_irq(&device,source),6);
    /* Timer is not started. Hold SIE clear until the device IRQ is pending,
     * then expose precisely the enable-to-wfi interval in the idle caller. */
    check(kernel_thread_create(irq_only_reader,NULL)==KERNEL_SCHEDULER_STATUS_OK &&
        kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK,11);
    uint64_t irq_limit=riscv_time_read()+2*info.timebase_frequency;
    volatile uint32_t *interrupt_status=(void*)((uintptr_t)device.mmio+0x60);
    while(!(*interrupt_status&1) && riscv_time_read()<irq_limit) { }
    check((*interrupt_status&1)!=0,12);
    riscv_interrupt_restore(RISCV_SSTATUS_SIE);
    __asm__ volatile("nop; nop; nop; nop" ::: "memory");
    check(irq_completed,13);
    (void)riscv_interrupt_save();
    struct kernel_thread_completion completion;
    check(kernel_scheduler_reap_one(&completion)==KERNEL_SCHEDULER_STATUS_OK,14);
    check(kernel_time_init(info.timebase_frequency,0)==KERNEL_TIME_STATUS_OK && riscv_timer_start(info.timebase_frequency,100)==RISCV_TIMER_STATUS_OK,7);
#if BOAROS_COST_DIAGNOSTICS
    check(kernel_cost_begin(1,info.timebase_frequency,1,0)==0,40);
#endif
    check(kernel_thread_create(exercise,NULL)==KERNEL_SCHEDULER_STATUS_OK,8);
    while(!completed) {
        uintptr_t irq=riscv_interrupt_save();
        check(kernel_scheduler_yield_current()==KERNEL_SCHEDULER_STATUS_OK,9);
        riscv_interrupt_restore(irq|RISCV_SSTATUS_SIE);
    }
#if BOAROS_COST_DIAGNOSTICS
    uint64_t groups, data_writes;
    check(kernel_cost_end(1,0)==0 &&
        kernel_cost_read(1,COST_JOURNAL_GROUPS,&groups)==0 && groups>0 &&
        kernel_cost_read(1,COST_DEVICE_OTHER_DATA_WRITE,&data_writes)==0 && data_writes>0,41);
#endif
    virt_uart_puts("BoarOS: journal group tests passed\n");sbi_shutdown();
}
