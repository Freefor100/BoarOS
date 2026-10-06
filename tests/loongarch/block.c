#include <kernel/virtio_pci_block.h>
#include <platform/loongarch_pci.h>
#include <platform/loongarch_virt.h>
#include <arch/task.h>
#include <arch/timer.h>
#include <kernel/task.h>
#include <kernel/tick.h>
#include <string.h>
static struct virtio_pci_block devices[2];
static unsigned char data[8][512] __attribute__((aligned(16)));
static volatile unsigned io_done,cpu_progress;
void la_fault_injection_set(int);
static int dma(const void *pointer,uint64_t size,uint64_t *address)
{ return arch_direct_map_va_to_pa((uintptr_t)pointer,size,address)==ARCH_DIRECT_MAP_STATUS_OK; }
static void io_worker(void *argument)
{
    (void)argument;
    uint64_t mask=4,disabled=0;
    __asm__ volatile("csrxchg %0, %1, 4":"+r"(disabled):"r"(mask):"memory");
    struct kernel_block_read_span spans[8];
    for(unsigned i=0;i<8;i++) spans[i]=(struct kernel_block_read_span){16384+i*512,data[i],512,0,KERNEL_BLOCK_STATUS_NOT_SUBMITTED};
    if(kernel_block_read_batch(&devices[0].device.block,spans,8)!=KERNEL_BLOCK_STATUS_OK) la_virt_fatal("PCI batch read");
    for(unsigned i=0;i<8;i++) for(unsigned j=0;j<512;j++)
        if(data[i][j]!=(i*512+j)%251) la_virt_fatal("PCI batch contents");
    uint64_t until=arch_time_read()+la_timer_frequency()/20;
    while((int64_t)(arch_time_read()-until)<0) { }
    if(!cpu_progress) la_virt_fatal("PCI CPU preemption");
    io_done=1;
}
static void cpu_worker(void *argument)
{
    (void)argument;
    uint64_t until=arch_time_read()+la_timer_frequency()/50;
    while((int64_t)(arch_time_read()-until)<0) { }
    if(devices[0].device.statistics.interrupts || devices[1].device.statistics.interrupts)
        la_virt_fatal("masked PCI interrupt dispatched on timer");
    uint64_t mask=4,enabled=4;
    __asm__ volatile("csrxchg %0, %1, 4":"+r"(enabled):"r"(mask):"memory");
    unsigned char original[512];
    if(kernel_block_read_at(&devices[1].device.block,512,original,512)!=KERNEL_BLOCK_STATUS_OK ||
        memcmp(original,"BoarOS PCI original sector",26)) la_virt_fatal("PCI readonly IRQ read");
    uint64_t deadline=arch_time_read()+la_timer_frequency()*3;
    while(!io_done) {
        cpu_progress++;
        if((int64_t)(arch_time_read()-deadline)>=0) la_virt_fatal("PCI I/O IRQ timeout");
    }
}
void __wrap_la_boot_tasks(struct physical_page_allocator *allocator)
{
    uint64_t baseline=physical_page_available(allocator);
    struct pci_host *host=la_virt_pci_host();unsigned found=0;
    if(!la_virt_irq_initialize()) la_virt_fatal("PCI interrupt controller");
    for(unsigned bdf=0;bdf<256 && found<2;bdf++) {
        uint32_t id=host->read(host->context,bdf,0,4);
        if(id!=0x10421af4) continue;
        uint64_t before=physical_page_available(allocator);unsigned claims=pci_host_claimed(host);
        la_fault_injection_set(0);
        enum virtio_block_status rejected=virtio_pci_block_init(&devices[found],host,bdf,allocator,dma,la_timer_frequency());
        la_fault_injection_set(-1);
        if(rejected!=VIRTIO_BLOCK_DRIVER_STATUS_NO_MEMORY || devices[found].function.host ||
            physical_page_available(allocator)!=before || pci_host_claimed(host)!=claims)
            la_virt_fatal("PCI queue OOM owner rollback");
        enum virtio_block_status status=virtio_pci_block_init(&devices[found],host,bdf,allocator,dma,la_timer_frequency());
        if(status!=VIRTIO_BLOCK_DRIVER_STATUS_OK) {la_virt_puts("PCI init status=");la_virt_hex(status);la_virt_puts("\n");la_virt_fatal("PCI block init");}
        if(kernel_block_register(&devices[found].device.block,KERNEL_BLOCK_DEVICE_NUMBER(found))) la_virt_fatal("PCI registry");
        la_virt_puts("LA PCI block bdf=");la_virt_hex(bdf);la_virt_puts(" irq=");la_virt_hex(devices[found].irq);la_virt_puts("\n");found++;
    }
    if(found!=2 || devices[0].device.read_only || !devices[1].device.read_only) la_virt_fatal("PCI block discovery");
    unsigned char buffer[512];
    if(kernel_block_read_at(&devices[0].device.block,512,buffer,sizeof(buffer))!=KERNEL_BLOCK_STATUS_OK ||
        memcmp(buffer,"BoarOS PCI original sector",26)) la_virt_fatal("PCI boot read");
    if(kernel_block_write_at(&devices[1].device.block,32768,buffer,512)!=KERNEL_BLOCK_STATUS_UNSUPPORTED ||
        kernel_block_read_at(&devices[0].device.block,UINT64_MAX,buffer,1)!=KERNEL_BLOCK_STATUS_OUT_OF_RANGE)
        la_virt_fatal("PCI readonly and bounds");
    for(unsigned i=0;i<512;i++) buffer[i]=(i*13+7)%256;
    if(kernel_block_write_at(&devices[0].device.block,32768,buffer,512)!=KERNEL_BLOCK_STATUS_OK ||
        kernel_block_flush(&devices[0].device.block)!=KERNEL_BLOCK_STATUS_OK) la_virt_fatal("PCI write flush");
    uint64_t prepared=physical_page_available(allocator);
    if(kernel_thread_create(io_worker,0)!=KERNEL_SCHEDULER_STATUS_OK ||
        kernel_thread_create(cpu_worker,0)!=KERNEL_SCHEDULER_STATUS_OK ||
        !virtio_block_enable_irq(&devices[0].device,devices[0].irq) ||
        !virtio_block_enable_irq(&devices[1].device,devices[1].irq) ||
        la_timer_start(la_timer_frequency(),100)!=ARCH_TIMER_STATUS_OK) la_virt_fatal("PCI tasks");
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    unsigned reaped=0;uint64_t deadline=arch_time_read()+la_timer_frequency()*5;
    while(reaped<2) {
        struct kernel_thread_completion result;uintptr_t irq=arch_interrupt_save();
        enum kernel_scheduler_status status=kernel_scheduler_reap_one(&result);arch_interrupt_restore(irq);
        if(status==KERNEL_SCHEDULER_STATUS_OK) {
            if(result.reason!=KERNEL_THREAD_EXIT_RETURNED) la_virt_fatal("PCI task result");
            reaped++;
        } else if(status!=KERNEL_SCHEDULER_STATUS_EMPTY) la_virt_fatal("PCI reap");
        if((int64_t)(arch_time_read()-deadline)>=0) la_virt_fatal("PCI task deadline");
        if(reaped<2) arch_cpu_wait();
    }
    if(physical_page_available(allocator)!=prepared || !devices[0].device.statistics.interrupts ||
        devices[0].device.statistics.runtime_polls || devices[0].device.statistics.max_inflight!=8)
        la_virt_fatal("PCI IRQ and task owner");
    for(unsigned i=2;i;i--) {
        if(kernel_block_unregister(&devices[i-1].device.block) ||
            virtio_pci_block_destroy(&devices[i-1])!=VIRTIO_BLOCK_DRIVER_STATUS_OK) la_virt_fatal("PCI device release");
    }
    if(physical_page_available(allocator)!=baseline || pci_host_claimed(host)) la_virt_fatal("PCI resource baseline");
    la_virt_puts("LA PCI block contracts passed\n");
}
