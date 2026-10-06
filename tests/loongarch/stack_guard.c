#include <arch/task.h>
#include <arch/mmu.h>
#include <arch/context.h>
#include <arch/timer.h>
#include <kernel/scheduler.h>
#include <kernel/physical_page.h>
#include <platform/loongarch_virt.h>
#include "../../kernel/sched/private.h"
#ifndef LA_STACK_CASE
#define LA_STACK_CASE 0
#endif
static void probe(void *unused)
{
    (void)unused;
    struct kernel_task *task=kernel_task_current();
    if(!arch_mmu_kernel_window_active() || task->stack_low<ARCH_KERNEL_STACK_WINDOW_BASE ||
       task->stack_high>=ARCH_KERNEL_STACK_WINDOW_BASE+ARCH_KERNEL_STACK_WINDOW_SIZE)
        la_virt_fatal("kernel stack window inactive");
    uintptr_t bottom=task->stack_low-16;
    if(LA_STACK_CASE==0) {
        la_virt_puts("LA actual guard write ready\n");
        *(volatile uint64_t *)(bottom-8)=1;
    } else if(LA_STACK_CASE==1) {
        la_virt_puts("LA actual guard SP overflow ready\n");
        uintptr_t sp=bottom+128;
        __asm__ volatile("move $sp,%0;break 0"::"r"(sp):"memory");
    } else if(LA_STACK_CASE==2) {
        la_virt_puts("LA actual stack NX ready\n");
        uintptr_t target=task->stack_low+128;
        *(uint32_t *)target=0x002a0000;
        arch_mmu_sync_instructions();((void (*)(void))target)();
    } else {
        uint64_t page;
        struct physical_page_allocator *a=unused;
        uint64_t address=ARCH_KERNEL_STACK_WINDOW_BASE+ARCH_KERNEL_STACK_WINDOW_SIZE-BOAROS_PAGE_SIZE;
        if(physical_page_allocate(a,&page)!=PHYSICAL_PAGE_STATUS_OK ||
           arch_mmu_kernel_window_map(a,address,page)!=ARCH_MMU_STATUS_OK)la_virt_fatal("stale setup");
        *(volatile uint64_t *)address=42;
        if(arch_mmu_kernel_window_unmap(a,address)!=ARCH_MMU_STATUS_OK ||
           physical_page_release(a,page)!=PHYSICAL_PAGE_STATUS_OK)la_virt_fatal("stale release");
        la_virt_puts("LA actual withdrawn translation ready\n");
        (void)*(volatile uint64_t *)address;
    }
    la_virt_fatal("guard missed");
}
void __wrap_la_boot_tasks(struct physical_page_allocator *a)
{
    if(kernel_thread_create(probe,a)!=KERNEL_SCHEDULER_STATUS_OK)la_virt_fatal("guard create");
    if(la_timer_start(la_timer_frequency(),100)!=ARCH_TIMER_STATUS_OK)la_virt_fatal("guard timer");
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    for(;;)__asm__ volatile("idle 0");
}
