#include <arch/loongarch/smp.h>
#if BOAROS_MAX_CPUS > 1
#include <arch/mmu.h>
#include <arch/timer.h>
#include <kernel/dtb.h>
#include <kernel/console.h>
#include <kernel/page.h>
#include <kernel/raw_lock.h>
#include <kernel/scheduler.h>
#include <kernel/tick.h>
#include <platform/loongarch_virt.h>
#include <string.h>

static struct kernel_cpu secondary[BOAROS_MAX_CPUS-1];
static struct kernel_cpu *cpus[BOAROS_MAX_CPUS];
struct la_smp_boot la_smp_boot[BOAROS_MAX_CPUS];
static unsigned char secondary_stacks[BOAROS_MAX_CPUS-1][LA_SMP_STACK_SIZE]
    __attribute__((aligned(LA_SMP_STACK_SIZE)));
static struct { unsigned attached,ping_seen,timer_seen,stop_requested; } startup[BOAROS_MAX_CPUS];
static struct dtb_cpu_info topology;
static uint32_t frequency;
void la_secondary_entry(void);
void la_trap_initialize(void);
/* Each sender owns one payload until every named receiver releases its bit.
 * The short lock protects snapshots and acknowledgements, never a TLB flush. */
static struct tlb_request {
    struct kernel_raw_lock lock;
    uint64_t generation,address,size;
    unsigned instructions,targets,pending;
} fences[BOAROS_MAX_CPUS];
#if BOAROS_LA_SMP_TEST == 1
static unsigned test_phase,reciprocal_entered,reciprocal_done;
static unsigned held_target,release_target,non_target_seen;
#endif

static uint32_t online_mask(void)
{return (1U<<topology.count)-1;}
static void failure(const char *stage,uint64_t target) __attribute__((noreturn));
static void failure(const char *stage,uint64_t target)
{
    unsigned count=topology.count<BOAROS_MAX_CPUS ? topology.count : BOAROS_MAX_CPUS;
    kernel_console_emergency_begin();la_virt_puts("LA SMP failure stage=");la_virt_puts(stage);
    la_virt_puts(" target=");la_virt_hex(target);
    la_virt_puts(" CPU=");la_virt_hex(kernel_cpu_current()->hardware_id);
    for(unsigned i=0;i<count;i++)if(topology.hart_ids[i]==target && cpus[i]) {
        la_virt_puts(" retained-state=");la_virt_hex(__atomic_load_n(&cpus[i]->state,__ATOMIC_ACQUIRE));
    }
    for(unsigned i=0;i<count;i++) {
        la_virt_puts(" source=");la_virt_hex(i);
        la_virt_puts(" generation=");la_virt_hex(__atomic_load_n(&fences[i].generation,__ATOMIC_RELAXED));
        la_virt_puts(" pending=");la_virt_hex(__atomic_load_n(&fences[i].pending,__ATOMIC_ACQUIRE));
    }
    la_virt_puts("\n");la_virt_shutdown();
}
struct kernel_cpu *la_smp_cpu(unsigned logical)
{return logical<topology.count ? cpus[logical] : NULL;}
void la_smp_discover(const void *dtb)
{
    if (topology.count || dtb_read_cpu_info(dtb,kernel_cpu_current()->hardware_id,&topology)!=DTB_STATUS_OK ||
        !topology.count || topology.count>BOAROS_MAX_CPUS || topology.hart_ids[0]!=kernel_cpu_current()->hardware_id)
        failure("CPU topology",0);
    for(unsigned i=0;i<BOAROS_MAX_CPUS;i++)la_smp_boot[i].hardware_id=UINT64_MAX;
    for(unsigned i=0;i<topology.count;i++) {
        if(topology.hart_ids[i]>1023)failure("CPU ID",topology.hart_ids[i]);
        la_smp_boot[i].hardware_id=topology.hart_ids[i];
    }
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
static void consume_fences(struct kernel_cpu *cpu)
{
    unsigned bit=1U<<cpu->logical_id;
    for(unsigned i=0;i<topology.count;i++) {
        if(i==cpu->logical_id)continue;
        struct tlb_request *r=&fences[i];
        uint64_t generation,address,size;unsigned instructions;
        {
            KERNEL_RAW_SCOPE(guard,&r->lock);
            if(!(__atomic_load_n(&r->pending,__ATOMIC_RELAXED)&bit)) {
#if BOAROS_LA_SMP_TEST == 1
                if(i==0 && cpu->logical_id && !(r->targets&bit) && r->targets==(1U<<(topology.count-1)) &&
                    __atomic_load_n(&test_phase,__ATOMIC_ACQUIRE)==3)
                    __atomic_fetch_or(&non_target_seen,bit,__ATOMIC_RELEASE);
#endif
                continue;
            }
            if(!(r->targets&bit))__builtin_trap();
            generation=r->generation;address=r->address;size=r->size;instructions=r->instructions;
        }
        if(size==BOAROS_PAGE_SIZE)arch_mmu_flush_address(address);else arch_mmu_flush_all();
        if(instructions)arch_mmu_sync_instructions();
#if BOAROS_LA_SMP_TEST == 4
        if(cpu->logical_id==topology.count-1)continue;
#endif
#if BOAROS_LA_SMP_TEST == 1
        unsigned phase=__atomic_load_n(&test_phase,__ATOMIC_ACQUIRE);
        if(i==0 && cpu->logical_id==topology.count-1 && (phase==2 || phase==3) &&
            !__atomic_load_n(&release_target,__ATOMIC_ACQUIRE)) {
            __atomic_store_n(&held_target,1,__ATOMIC_RELEASE);
            continue;
        }
#endif
        {
            KERNEL_RAW_SCOPE(guard,&r->lock);
            /* This bit keeps the payload alive across the unlocked flush. A
             * delayed IPI may inspect a new generation but cannot ack the old one. */
            unsigned pending=__atomic_load_n(&r->pending,__ATOMIC_RELAXED);
            if(r->generation!=generation || !(r->targets&bit) || !(pending&bit))__builtin_trap();
            __atomic_store_n(&r->pending,pending&~bit,__ATOMIC_RELEASE);
        }
    }
}
static void consume(int interrupt)
{
    uint32_t status=la_iocsr_read(0x1000);la_iocsr_write(0x100c,status);
    /* Complete the hardware clear before consuming events; a later sender's
     * interrupt must not be removed after its software publication. */
    __asm__ volatile("dbar 0" ::: "memory");
    struct kernel_cpu *cpu=kernel_cpu_current();if(interrupt)cpu->ipi_received++;
    unsigned events=__atomic_exchange_n(&cpu->events,0,__ATOMIC_ACQ_REL);
    /* IRQ-masked senders also consume requests, so reciprocal waits progress. */
    consume_fences(cpu);
    if(events&LA_IPI_SCHEDULE)kernel_cpu_request_schedule(cpu);
    if(events&LA_IPI_DEADLINE)kernel_scheduler_deadline_changed();
#if BOAROS_LA_SMP_TEST == 3
    if((events&LA_IPI_HANDSHAKE) && cpu->logical_id!=topology.count-1)
#else
    if(events&LA_IPI_HANDSHAKE)
#endif
        __atomic_store_n(&startup[cpu->logical_id].ping_seen,1,__ATOMIC_RELEASE);
    if(events&LA_IPI_STOP)__atomic_store_n(&startup[cpu->logical_id].stop_requested,1,__ATOMIC_RELEASE);
}
void la_smp_handle_ipi(void){consume(1);}
void la_smp_timer_seen(void)
{
    unsigned cpu=kernel_cpu_current()->logical_id;
    if(cpu)__atomic_store_n(&startup[cpu].timer_seen,1,__ATOMIC_RELEASE);
}
void la_smp_idle(void)
{
    uintptr_t irq=arch_interrupt_save();struct kernel_cpu *cpu=kernel_cpu_current();
    if(__atomic_load_n(&cpu->events,__ATOMIC_ACQUIRE))consume(0);
    kernel_scheduler_prepare_idle_return();
    if(!kernel_cpu_schedule_requested(cpu) && !__atomic_load_n(&cpu->events,__ATOMIC_ACQUIRE))arch_cpu_wait();
    arch_interrupt_restore(irq|ARCH_INTERRUPT_ENABLE_MASK);
}
void la_smp_secondary(struct la_smp_boot *boot)
{
    struct kernel_cpu *cpu=kernel_cpu_current();unsigned logical=cpu->logical_id;
    if(!logical || logical>=topology.count || boot!=&la_smp_boot[logical] ||
        cpu!=cpus[logical] || boot->hardware_id!=cpu->hardware_id)__builtin_trap();
    la_trap_initialize();la_mmu_secondary_initialize();
    kernel_cpu_secondary_attach(cpu,boot->stack_low,boot->stack_high);
    la_iocsr_write(0x100c,UINT32_MAX);la_iocsr_write(0x1004,UINT32_MAX);
    uint64_t ipi=1U<<12;__asm__ volatile("csrxchg %0,%1,4":"+r"(ipi):"r"(ipi):"memory");
    if(la_timer_start(frequency,KERNEL_TICKS_PER_SECOND)!=ARCH_TIMER_STATUS_OK)failure("AP timer",cpu->hardware_id);
#if BOAROS_LA_SMP_TEST == 2
    if(logical==topology.count-1)for(;;)arch_cpu_wait();
#endif
    __atomic_store_n(&startup[logical].attached,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&startup[logical].ping_seen,__ATOMIC_ACQUIRE) ||
          !__atomic_load_n(&startup[logical].timer_seen,__ATOMIC_ACQUIRE))la_smp_idle();
    __atomic_store_n(&cpu->state,KERNEL_CPU_ONLINE,__ATOMIC_RELEASE);
#if BOAROS_LA_SMP_TEST == 1
    while(__atomic_load_n(&test_phase,__ATOMIC_ACQUIRE)!=1)la_smp_idle();
    la_smp_sync_user(online_mask(),0,UINT64_MAX,1);
    __atomic_fetch_or(&reciprocal_done,1U<<logical,__ATOMIC_RELEASE);
#endif
    for(;;) {
        la_smp_idle();
        if(__atomic_load_n(&startup[logical].stop_requested,__ATOMIC_ACQUIRE)) {
            (void)arch_interrupt_save();
            if(cpu->current!=cpu->idle || cpu->switch_previous || cpu->raw_locks || cpu->preempt_depth)
                failure("AP stop owner",cpu->hardware_id);
            __atomic_store_n(&cpu->state,KERNEL_CPU_QUIESCING,__ATOMIC_RELEASE);
            __asm__ volatile("csrwr $zero,4\ncsrwr $zero,0x41" ::: "memory");
#if BOAROS_LA_SMP_TEST == 5
            if(logical!=topology.count-1)
#endif
                __atomic_store_n(&cpu->state,KERNEL_CPU_STOPPED,__ATOMIC_RELEASE);
            for(;;)arch_cpu_wait();
        }
    }
}
static void mail(unsigned mailbox,uint64_t value,uint64_t target)
{
    uint64_t command=0x80000000U|(target<<16)|((uint64_t)(mailbox*2+1)<<2)|
        (value&UINT64_C(0xffffffff00000000)),address=0x1048;
    __asm__ volatile("iocsrwr.d %0,%1"::"r"(command),"r"(address):"memory");
    command=0x80000000U|(target<<16)|((uint64_t)(mailbox*2)<<2)|(value<<32);
    __asm__ volatile("iocsrwr.d %0,%1"::"r"(command),"r"(address):"memory");
}
void la_smp_start(uint32_t hz)
{
    if(!topology.count || cpus[0] || !hz)failure("start input",0);
    frequency=hz;cpus[0]=kernel_cpu_current();cpus[0]->logical_id=0;cpus[0]->state=KERNEL_CPU_ONLINE;
    for(unsigned i=0;i<topology.count;i++)kernel_raw_lock_init(&fences[i].lock,KERNEL_RAW_RANK_MM);
    la_iocsr_write(0x100c,UINT32_MAX);la_iocsr_write(0x1004,UINT32_MAX);
    uint64_t ipi=1U<<12;__asm__ volatile("csrxchg %0,%1,4":"+r"(ipi):"r"(ipi):"memory");
    for(unsigned i=1;i<topology.count;i++) {
        struct kernel_cpu *cpu=&secondary[i-1];struct la_smp_boot *boot=&la_smp_boot[i];cpus[i]=cpu;
        boot->thread=kernel_cpu_secondary_prepare(cpu,topology.hart_ids[i],i);
        boot->stack_low=(uintptr_t)secondary_stacks[i-1];boot->stack_high=boot->stack_low+LA_SMP_STACK_SIZE;
        memset((void *)boot->stack_low,0xa5,LA_SMP_STACK_SIZE);cpu->state=KERNEL_CPU_STARTING;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        /* QEMU clears MBUF0 while parked, then reads it as the entry. MBUF1
         * carries the target's permanent descriptor independently of that loop. */
        mail(1,(uintptr_t)boot,cpu->hardware_id);
        mail(0,(uintptr_t)la_secondary_entry-LA_DIRECT_BASE,cpu->hardware_id);send(cpu);
        uint64_t end=arch_time_read()+(uint64_t)hz*5;
        while(!__atomic_load_n(&startup[i].attached,__ATOMIC_ACQUIRE))
            if((int64_t)(arch_time_read()-end)>=0)failure("AP attach",cpu->hardware_id);
        kernel_cpu_notify(cpu,LA_IPI_HANDSHAKE);
        while(!kernel_cpu_online(cpu))
            if((int64_t)(arch_time_read()-end)>=0)failure("AP timer/IPI handshake",cpu->hardware_id);
        la_virt_puts("LA SMP online logical=");la_virt_hex(i);
        la_virt_puts(" hardware=");la_virt_hex(cpu->hardware_id);la_virt_puts("\n");
    }
#if BOAROS_LA_SMP_TEST == 1
    if(topology.count>1) {
        __atomic_store_n(&test_phase,1,__ATOMIC_RELEASE);
        for(unsigned i=1;i<topology.count;i++)kernel_cpu_notify(cpus[i],LA_IPI_SCHEDULE);
        la_smp_sync_user(online_mask(),0,UINT64_MAX,1);
        uint64_t end=arch_time_read()+(uint64_t)hz*5;
        while(__atomic_load_n(&reciprocal_done,__ATOMIC_ACQUIRE)!=(online_mask()&~1U)) {
            consume(0);
            if((int64_t)(arch_time_read()-end)>=0)failure("reciprocal completion",topology.hart_ids[topology.count-1]);
        }
        if(reciprocal_entered!=online_mask())failure("reciprocal ownership",0);
        for(unsigned i=0;i<topology.count;i++)if(fences[i].pending)failure("reciprocal pending",topology.hart_ids[i]);
        la_virt_puts("LA SMP reciprocal masked-IRQ TLB acknowledgements PASS\n");
        __atomic_store_n(&test_phase,2,__ATOMIC_RELEASE);
        la_smp_sync_user(online_mask(),0,UINT64_MAX,1);
        if(!__atomic_load_n(&held_target,__ATOMIC_ACQUIRE) || !__atomic_load_n(&release_target,__ATOMIC_ACQUIRE))
            failure("last-target premature completion",topology.hart_ids[topology.count-1]);
        la_virt_puts("LA SMP last-target TLB acknowledgement PASS\n");
        uint64_t generation=fences[0].generation;
        __atomic_store_n(&held_target,0,__ATOMIC_RELAXED);__atomic_store_n(&release_target,0,__ATOMIC_RELAXED);
        __atomic_store_n(&test_phase,3,__ATOMIC_RELEASE);
        la_smp_sync_user(1U<<(topology.count-1),BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,1);
        if(fences[0].generation!=generation+1 || !__atomic_load_n(&held_target,__ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&release_target,__ATOMIC_ACQUIRE))
            failure("late-IPI premature completion",topology.hart_ids[topology.count-1]);
        la_virt_puts("LA SMP late-IPI TLB generation reuse PASS\n");
        __atomic_store_n(&test_phase,0,__ATOMIC_RELEASE);
    }
#endif
}
void la_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{
    uintptr_t irq=arch_interrupt_save();struct kernel_cpu *cpu=kernel_cpu_current();
    if(cpu->raw_locks || (targets&~((1U<<BOAROS_MAX_CPUS)-1)))__builtin_trap();
    if(size==BOAROS_PAGE_SIZE)arch_mmu_flush_address(address);else arch_mmu_flush_all();
    if(instructions)arch_mmu_sync_instructions();
    if(!cpus[0]){arch_interrupt_restore(irq);return;}
    if(targets&~online_mask())__builtin_trap();
    unsigned remote=targets&~(1U<<cpu->logical_id);
    if(remote) {
        for(unsigned i=0;i<topology.count;i++)if((remote&(1U<<i)) && !kernel_cpu_online(cpus[i]))
            failure("inactive TLB target",cpus[i]->hardware_id);
        struct tlb_request *r=&fences[cpu->logical_id];
        {
            KERNEL_RAW_SCOPE(guard,&r->lock);
            if(r->pending || r->generation==UINT64_MAX)__builtin_trap();
            r->address=address;r->size=size;r->instructions=instructions;r->targets=remote;r->generation++;
            __atomic_store_n(&r->pending,remote,__ATOMIC_RELEASE);
        }
#if BOAROS_LA_SMP_TEST == 1
        unsigned phase=__atomic_load_n(&test_phase,__ATOMIC_ACQUIRE);
        if(phase==1) {
            __atomic_fetch_or(&reciprocal_entered,1U<<cpu->logical_id,__ATOMIC_RELEASE);
            uint64_t timeout=arch_time_read()+(uint64_t)frequency*5;
            while(__atomic_load_n(&reciprocal_entered,__ATOMIC_ACQUIRE)!=online_mask())
                if((int64_t)(arch_time_read()-timeout)>=0)failure("reciprocal rendezvous",cpu->hardware_id);
        }
        /* Deliver old/spurious hardware IPIs after source 0 has reused its
         * payload with a different target set; these CPUs must not ack it. */
        if(phase==3 && !cpu->logical_id)for(unsigned i=1;i+1<topology.count;i++)send(cpus[i]);
#endif
        for(unsigned i=0;i<topology.count;i++)if(remote&(1U<<i)) {
            kernel_cpu_notify(cpus[i],LA_IPI_TLB);cpu->rfence_requests++;
        }
        uint64_t end=arch_time_read()+(uint64_t)frequency*5;
        while(__atomic_load_n(&r->pending,__ATOMIC_ACQUIRE)) {
            consume(0);
#if BOAROS_LA_SMP_TEST == 1
            if(!cpu->logical_id && (phase==2 || phase==3) &&
                __atomic_load_n(&held_target,__ATOMIC_ACQUIRE) &&
                __atomic_load_n(&r->pending,__ATOMIC_ACQUIRE)==(1U<<(topology.count-1)) &&
                (phase!=3 || __atomic_load_n(&non_target_seen,__ATOMIC_ACQUIRE)==(online_mask()&~1U&~remote)) &&
                !__atomic_load_n(&release_target,__ATOMIC_RELAXED)) {
                __atomic_store_n(&release_target,1,__ATOMIC_RELEASE);
                kernel_cpu_notify(cpus[topology.count-1],LA_IPI_TLB);
            }
#endif
            if((int64_t)(arch_time_read()-end)>=0) {
                unsigned pending=__atomic_load_n(&r->pending,__ATOMIC_ACQUIRE);
                if(pending)failure("TLB acknowledgement",topology.hart_ids[__builtin_ctz(pending)]);
            }
        }
    }
    /* Our last acknowledgement may precede the peer's publication. Consume
     * peer requests before returning to a caller which still has IRQs masked. */
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
    if(topology.count<2)return;
    kernel_preempt_disable();
    for(unsigned i=1;i<topology.count;i++) {
        if(__atomic_load_n(&cpus[i]->state,__ATOMIC_ACQUIRE)==KERNEL_CPU_STOPPED)continue;
        if(!kernel_scheduler_core_quiescent(cpus[i]))failure("stop live owner",cpus[i]->hardware_id);
    }
    for(unsigned i=1;i<topology.count;i++)if(__atomic_load_n(&cpus[i]->state,__ATOMIC_ACQUIRE)!=KERNEL_CPU_STOPPED)
        kernel_cpu_notify(cpus[i],LA_IPI_STOP);
    uint64_t end=arch_time_read()+(uint64_t)frequency*5;
    for(unsigned i=1;i<topology.count;i++)while(__atomic_load_n(&cpus[i]->state,__ATOMIC_ACQUIRE)!=KERNEL_CPU_STOPPED) {
        if((int64_t)(arch_time_read()-end)>=0)failure("stop acknowledgement",cpus[i]->hardware_id);
    }
    kernel_preempt_enable();
}
void la_smp_report(void)
{
    struct kernel_stack_statistics stacks;kernel_scheduler_stack_statistics(&stacks);
    la_virt_puts("LA stacks released=");la_virt_hex(stacks.stacks_released);
    la_virt_puts(" min-free=");la_virt_hex(stacks.minimum_free_bytes);la_virt_puts(" max-used=");la_virt_hex(stacks.maximum_used_bytes);la_virt_puts("\n");
    for(unsigned i=0;i<topology.count;i++) {
        struct kernel_cpu *c=cpus[i];
        if(i) {
            unsigned char *stack=(void *)la_smp_boot[i].stack_low;size_t free=0;
            while(free<LA_SMP_STACK_SIZE && stack[free]==0xa5)free++;
            la_virt_puts("LA AP idle stack CPU=");la_virt_hex(i);la_virt_puts(" free=");la_virt_hex(free);la_virt_puts("\n");
        }
        la_virt_puts("LA SMP CPU=");la_virt_hex(i);la_virt_puts(" traps=");la_virt_hex(c->user_traps);
        la_virt_puts(" returns=");la_virt_hex(c->user_returns);la_virt_puts(" IPI=");la_virt_hex(c->ipi_received);
        la_virt_puts(" TLB=");la_virt_hex(c->rfence_requests);
        la_virt_puts(" state=");la_virt_hex(c->state);la_virt_puts("\n");
    }
}
#endif
