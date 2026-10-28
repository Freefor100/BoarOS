#include <arch/loongarch/smp.h>
#if BOAROS_MAX_CPUS > 1
#include <arch/mmu.h>
#include <arch/timer.h>
#include <kernel/dtb.h>
#include <kernel/console.h>
#include <kernel/page.h>
#include <kernel/scheduler.h>
#include <kernel/tick.h>
#include <platform/loongarch_virt.h>
#include <string.h>

static struct kernel_cpu secondary;
static struct kernel_cpu *cpus[2];
static struct dtb_cpu_info topology;
static uint32_t frequency;
static unsigned attached,ping_seen,timer_seen,stop_requested;
#if BOAROS_LA_SMP_TEST == 1
static unsigned reciprocal_active,reciprocal_entered,reciprocal_done;
#endif
uint64_t la_secondary_thread;
extern unsigned char la_secondary_stack[],la_secondary_stack_top[];
void la_secondary_entry(void);
void la_trap_initialize(void);
/* One synchronous request per source. A reply ends the remote storage borrow. */
static struct tlb_request {uint64_t address,size;unsigned instructions,request,complete;} fences[2];

static void failure(const char *stage,uint64_t target) __attribute__((noreturn));
static void failure(const char *stage,uint64_t target)
{
    kernel_console_emergency_begin();la_virt_puts("LA SMP failure stage=");la_virt_puts(stage);
    la_virt_puts(" target=");la_virt_hex(target);
    la_virt_puts(" CPU=");la_virt_hex(kernel_cpu_current()->hardware_id);
    la_virt_puts(" retained-state=");la_virt_hex(secondary.state);
    la_virt_puts(" request=");la_virt_hex(fences[0].request);la_virt_puts(" ack=");la_virt_hex(fences[0].complete);
    la_virt_puts("\n");la_virt_shutdown();
}
struct kernel_cpu *la_smp_cpu(unsigned logical)
{return logical<topology.count ? cpus[logical] : NULL;}
void la_smp_discover(const void *dtb)
{
    if (topology.count || dtb_read_cpu_info(dtb,kernel_cpu_current()->hardware_id,&topology)!=DTB_STATUS_OK ||
        !topology.count || topology.count>BOAROS_MAX_CPUS || topology.hart_ids[0]!=kernel_cpu_current()->hardware_id)
        failure("CPU topology",0);
    for (unsigned i=0;i<topology.count;i++)if(topology.hart_ids[i]>1023)failure("CPU ID",topology.hart_ids[i]);
    kernel_cpu_set_boot_count(topology.count);
}
static void send(struct kernel_cpu *target)
{
    /* IOCSR blocking send orders the device request, not the remote callback. */
    __asm__ volatile("dbar 0" ::: "memory");
    la_iocsr_write(0x1040,0x80000000U|((uint32_t)target->hardware_id<<16));
    kernel_cpu_current()->ipi_sent++;
}
void kernel_cpu_notify(struct kernel_cpu *target,unsigned events)
{
    uintptr_t irq=arch_interrupt_save();struct kernel_cpu *source=kernel_cpu_current();
    if (!target) __builtin_trap();
    int fresh=kernel_cpu_publish_events(target,events);
    if (target!=source) {
        COST_ADD(SCHED_IPI_REQUESTS,1);
        if(!fresh) {COST_ADD(SCHED_IPI_COALESCED,1);}
        else {
            unsigned bit=1U<<target->logical_id;
            if(source->notification_batch)source->notification_batch->targets|=bit;
            else source->outbound|=bit;
        }
    }
    if (!source->raw_locks && source->outbound)kernel_cpu_flush_notifications();
    arch_interrupt_restore(irq);
}
void kernel_cpu_flush_notifications(void)
{
    struct kernel_cpu *source=kernel_cpu_current();if(source->raw_locks)__builtin_trap();
    unsigned mask=source->outbound;source->outbound=0;
    for(unsigned i=0;i<topology.count;i++)if(mask&(1U<<i))send(cpus[i]);
}
static void consume(int interrupt)
{
    uint32_t status=la_iocsr_read(0x1000);la_iocsr_write(0x100c,status);
    /* Finish the hardware clear before consuming software events, so a
     * later sender cannot have its newly raised interrupt cleared by us. */
    __asm__ volatile("dbar 0" ::: "memory");
    struct kernel_cpu *cpu=kernel_cpu_current();if(interrupt)cpu->ipi_received++;
    unsigned events=__atomic_exchange_n(&cpu->events,0,__ATOMIC_ACQ_REL);
    /* Also inspect requests while IRQs are masked: two senders may wait on
     * one another after editing different address spaces. No MM locks here. */
    for(unsigned i=0;i<topology.count;i++) {
        if(i==cpu->logical_id)continue;
        struct tlb_request *r=&fences[i];unsigned request=__atomic_load_n(&r->request,__ATOMIC_ACQUIRE);
        if(request==__atomic_load_n(&r->complete,__ATOMIC_RELAXED))continue;
        if(r->size==BOAROS_PAGE_SIZE)arch_mmu_flush_address(r->address);else arch_mmu_flush_all();
        if(r->instructions)arch_mmu_sync_instructions();
#if BOAROS_LA_SMP_TEST == 4
        if(cpu->logical_id)continue;
#endif
        __atomic_store_n(&r->complete,request,__ATOMIC_RELEASE);
    }
    if(events&LA_IPI_SCHEDULE)kernel_cpu_request_schedule(cpu);
    if(events&LA_IPI_DEADLINE)kernel_scheduler_deadline_changed();
#if BOAROS_LA_SMP_TEST != 3
    if(events&LA_IPI_HANDSHAKE)__atomic_store_n(&ping_seen,1,__ATOMIC_RELEASE);
#endif
    if(events&LA_IPI_STOP)__atomic_store_n(&stop_requested,1,__ATOMIC_RELEASE);
}
void la_smp_handle_ipi(void){consume(1);}
void la_smp_timer_seen(void){if(kernel_cpu_current()->logical_id)__atomic_store_n(&timer_seen,1,__ATOMIC_RELEASE);}
void la_smp_idle(void)
{
    uintptr_t irq=arch_interrupt_save();struct kernel_cpu *cpu=kernel_cpu_current();
    if(__atomic_load_n(&cpu->events,__ATOMIC_ACQUIRE))consume(0);
    kernel_scheduler_prepare_idle_return();
    if(!kernel_cpu_schedule_requested(cpu) && !__atomic_load_n(&cpu->events,__ATOMIC_ACQUIRE))arch_cpu_wait();
    arch_interrupt_restore(irq|ARCH_INTERRUPT_ENABLE_MASK);
}
void la_smp_secondary(void)
{
    la_trap_initialize();la_mmu_secondary_initialize();
    kernel_cpu_secondary_attach(&secondary,(uintptr_t)la_secondary_stack,(uintptr_t)la_secondary_stack_top);
    la_iocsr_write(0x100c,UINT32_MAX);la_iocsr_write(0x1004,UINT32_MAX);
    uint64_t ipi=1U<<12;__asm__ volatile("csrxchg %0,%1,4":"+r"(ipi):"r"(ipi):"memory");
    if(la_timer_start(frequency,KERNEL_TICKS_PER_SECOND)!=ARCH_TIMER_STATUS_OK)failure("AP timer",secondary.hardware_id);
#if BOAROS_LA_SMP_TEST == 2
    for(;;)arch_cpu_wait();
#endif
    __atomic_store_n(&attached,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&ping_seen,__ATOMIC_ACQUIRE)||!__atomic_load_n(&timer_seen,__ATOMIC_ACQUIRE))la_smp_idle();
    __atomic_store_n(&secondary.state,KERNEL_CPU_ONLINE,__ATOMIC_RELEASE);
#if BOAROS_LA_SMP_TEST == 1
    while(!__atomic_load_n(&reciprocal_active,__ATOMIC_ACQUIRE))la_smp_idle();
    la_smp_sync_user(3,0,UINT64_MAX,1);
    __atomic_store_n(&reciprocal_done,1,__ATOMIC_RELEASE);
#endif
    for(;;) {
        la_smp_idle();
        if(__atomic_load_n(&stop_requested,__ATOMIC_ACQUIRE)) {
            (void)arch_interrupt_save();
            if(secondary.current!=secondary.idle || secondary.switch_previous || secondary.raw_locks || secondary.preempt_depth)
                failure("AP stop owner",secondary.hardware_id);
            __atomic_store_n(&secondary.state,KERNEL_CPU_QUIESCING,__ATOMIC_RELEASE);
            __asm__ volatile("csrwr $zero,4\ncsrwr $zero,0x41" ::: "memory");
#if BOAROS_LA_SMP_TEST != 5
            __atomic_store_n(&secondary.state,KERNEL_CPU_STOPPED,__ATOMIC_RELEASE);
#endif
            for(;;)arch_cpu_wait();
        }
    }
}
static void mail(uint64_t value,uint64_t target)
{
    uint64_t command=0x80000000U|(target<<16)|(value<<32),address=0x1048;
    __asm__ volatile("iocsrwr.d %0,%1"::"r"(command),"r"(address):"memory");
    command=0x80000004U|(target<<16)|(value&UINT64_C(0xffffffff00000000));
    __asm__ volatile("iocsrwr.d %0,%1"::"r"(command),"r"(address):"memory");
}
void la_smp_start(uint32_t hz)
{
    if(!topology.count || cpus[0] || !hz)failure("start input",0);
    frequency=hz;cpus[0]=kernel_cpu_current();cpus[0]->logical_id=0;cpus[0]->state=KERNEL_CPU_ONLINE;
    la_iocsr_write(0x100c,UINT32_MAX);la_iocsr_write(0x1004,UINT32_MAX);
    uint64_t ipi=1U<<12;__asm__ volatile("csrxchg %0,%1,4":"+r"(ipi):"r"(ipi):"memory");
    if(topology.count==1)return;
    cpus[1]=&secondary;
    struct kernel_task *idle=kernel_cpu_secondary_prepare(&secondary,topology.hart_ids[1],1);
    memset(la_secondary_stack,0xa5,(size_t)(la_secondary_stack_top-la_secondary_stack));
    la_secondary_thread=(uintptr_t)idle;secondary.state=KERNEL_CPU_STARTING;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    mail((uintptr_t)la_secondary_entry-LA_DIRECT_BASE,secondary.hardware_id);send(&secondary);
    uint64_t end=arch_time_read()+(uint64_t)hz*5;
    while(!__atomic_load_n(&attached,__ATOMIC_ACQUIRE))if((int64_t)(arch_time_read()-end)>=0)failure("AP attach",secondary.hardware_id);
    kernel_cpu_notify(&secondary,LA_IPI_HANDSHAKE);
    while(!kernel_cpu_online(&secondary))if((int64_t)(arch_time_read()-end)>=0)failure("AP timer/IPI handshake",secondary.hardware_id);
#if BOAROS_LA_SMP_TEST == 1
    __atomic_store_n(&reciprocal_active,1,__ATOMIC_RELEASE);
    kernel_cpu_notify(&secondary,LA_IPI_SCHEDULE);
    la_smp_sync_user(3,0,UINT64_MAX,1);
    while(!__atomic_load_n(&reciprocal_done,__ATOMIC_ACQUIRE))
        if((int64_t)(arch_time_read()-end)>=0)failure("reciprocal completion",secondary.hardware_id);
    __atomic_store_n(&reciprocal_active,0,__ATOMIC_RELEASE);
    if(reciprocal_entered!=3 || fences[0].request!=fences[0].complete || fences[1].request!=fences[1].complete)
        failure("reciprocal ownership",secondary.hardware_id);
    la_virt_puts("LA SMP reciprocal masked-IRQ TLB acknowledgements PASS\n");
#endif
    la_virt_puts("LA SMP online logical=1 hardware=");la_virt_hex(secondary.hardware_id);la_virt_puts("\n");
}
void la_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{
    uintptr_t irq=arch_interrupt_save();
    struct kernel_cpu *cpu=kernel_cpu_current();if(cpu->raw_locks || (targets&~3U))__builtin_trap();
    if(size==BOAROS_PAGE_SIZE)arch_mmu_flush_address(address);else arch_mmu_flush_all();
    if(instructions)arch_mmu_sync_instructions();
    if(!cpus[0]){arch_interrupt_restore(irq);return;}
    for(unsigned i=0;i<topology.count;i++) {
        if(!(targets&(1U<<i)) || cpus[i]==cpu)continue;
        if(!kernel_cpu_online(cpus[i]))failure("inactive TLB target",cpus[i]->hardware_id);
        struct tlb_request *r=&fences[cpu->logical_id];
        unsigned sequence=__atomic_load_n(&r->request,__ATOMIC_RELAXED)+1;
        r->address=address;r->size=size;r->instructions=instructions;
        __atomic_store_n(&r->request,sequence,__ATOMIC_RELEASE);
#if BOAROS_LA_SMP_TEST == 1
        if(__atomic_load_n(&reciprocal_active,__ATOMIC_ACQUIRE)) {
            __atomic_fetch_or(&reciprocal_entered,1U<<cpu->logical_id,__ATOMIC_RELEASE);
            uint64_t timeout=arch_time_read()+(uint64_t)frequency*5;
            while(__atomic_load_n(&reciprocal_entered,__ATOMIC_ACQUIRE)!=3)
                if((int64_t)(arch_time_read()-timeout)>=0)failure("reciprocal rendezvous",cpus[i]->hardware_id);
        }
#endif
        kernel_cpu_notify(cpus[i],LA_IPI_TLB);cpu->rfence_requests++;
        uint64_t end=arch_time_read()+(uint64_t)frequency*5;
        while(__atomic_load_n(&r->complete,__ATOMIC_ACQUIRE)!=sequence) {
            consume(0);
            if((int64_t)(arch_time_read()-end)>=0)failure("TLB acknowledgement",cpus[i]->hardware_id);
        }
    }
    /* The peer can acknowledge us before we enter the loop. Still consume
     * its published request before returning to an IRQ-masked caller. */
    consume(0);
    arch_interrupt_restore(irq);
}
void la_smp_sync_kernel(uint64_t address,uint64_t size)
{
    uint32_t targets=0;
    for(unsigned i=0;i<topology.count;i++)if(cpus[i] && kernel_cpu_online(cpus[i]))targets|=1U<<i;
    la_smp_sync_user(targets,address,size,0);
}
void la_smp_stop(void)
{
    if(topology.count<2 || __atomic_load_n(&secondary.state,__ATOMIC_ACQUIRE)==KERNEL_CPU_STOPPED)return;
    kernel_preempt_disable();
    if(!kernel_scheduler_core_quiescent(&secondary))failure("stop live owner",secondary.hardware_id);
    kernel_cpu_notify(&secondary,LA_IPI_STOP);uint64_t end=arch_time_read()+(uint64_t)frequency*5;
    while(__atomic_load_n(&secondary.state,__ATOMIC_ACQUIRE)!=KERNEL_CPU_STOPPED)
        if((int64_t)(arch_time_read()-end)>=0)failure("stop acknowledgement",secondary.hardware_id);
    kernel_preempt_enable();
}
void la_smp_report(void)
{
    struct kernel_stack_statistics stacks;kernel_scheduler_stack_statistics(&stacks);
    la_virt_puts("LA stacks released=");la_virt_hex(stacks.stacks_released);
    la_virt_puts(" min-free=");la_virt_hex(stacks.minimum_free_bytes);la_virt_puts(" max-used=");la_virt_hex(stacks.maximum_used_bytes);la_virt_puts("\n");
    if(topology.count>1) {
        size_t free=0;while(free<(size_t)(la_secondary_stack_top-la_secondary_stack) && la_secondary_stack[free]==0xa5)free++;
        la_virt_puts("LA AP idle stack free=");la_virt_hex(free);la_virt_puts("\n");
    }
    for(unsigned i=0;i<topology.count;i++) {
        struct kernel_cpu *c=cpus[i];
        la_virt_puts("LA SMP CPU=");la_virt_hex(i);la_virt_puts(" traps=");la_virt_hex(c->user_traps);
        la_virt_puts(" returns=");la_virt_hex(c->user_returns);la_virt_puts(" IPI=");la_virt_hex(c->ipi_received);
        la_virt_puts(" TLB=");la_virt_hex(c->rfence_requests);
        la_virt_puts(" state=");la_virt_hex(c->state);la_virt_puts("\n");
    }
}
#endif
