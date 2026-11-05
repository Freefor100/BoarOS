#include <arch/context.h>
#include <arch/task.h>
#include <kernel/cpu.h>
#include <kernel/physical_page.h>
#include <platform/loongarch_virt.h>
void la_trap_dispatch(struct arch_trap_frame *);
void __wrap_la_boot_tasks(struct physical_page_allocator *allocator)
{
    (void)allocator;
    uintptr_t irq=arch_interrupt_save();
    struct kernel_cpu *cpu=kernel_cpu_current();
    struct kernel_task *owner=cpu->current;
    uint64_t enabled;__asm__ volatile("csrrd %0,4":"=r"(enabled));
    struct arch_trap_frame frame={0};
    la_trap_dispatch(&frame);
    /* A status snapshot can retain a masked source; it must not be dispatched. */
    uint64_t masked=enabled&~(UINT64_C(1)<<11);
    __asm__ volatile("csrwr %0,4":"+r"(masked)::"memory");
    frame.estat=UINT64_C(1)<<11;
    la_trap_dispatch(&frame);
    uint64_t restore=enabled;
    __asm__ volatile("csrwr %0,4":"+r"(restore)::"memory");
    if(cpu->current!=owner || cpu->raw_locks || cpu->preempt_depth)
        la_virt_fatal("empty IRQ owner");
    arch_interrupt_restore(irq);
    la_virt_puts("LA empty and masked interrupt snapshots preserve owner PASS\n");
    la_virt_shutdown();
}
