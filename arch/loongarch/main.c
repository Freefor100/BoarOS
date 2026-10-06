#include <platform/loongarch_virt.h>
#include <kernel/physical_page.h>
#include <kernel/page.h>
#include <arch/mmu.h>
#include <arch/timer.h>
#include <kernel/time.h>
#include <kernel/scheduler.h>
void la_trap_initialize(void);
void la_boot_tasks(struct physical_page_allocator *);
extern unsigned char __boot_stack_bottom[], __boot_stack_top[];
void la_heap_contract(struct physical_page_allocator *);
void la_mmu_contract(struct physical_page_allocator *);
void la_stack_window_contract(struct physical_page_allocator *);
static struct physical_page_allocator allocator;
extern unsigned char __kernel_start[], __kernel_end[];
void la_kernel_main(uint64_t systab)
{
    la_virt_puts("BoarOS: LA64 QEMU virt, 16 KiB pages\n");
    la_trap_initialize();
    struct boot_memory_layout layout;
    if (!la_virt_boot_memory(systab, (uintptr_t)__kernel_start - LA_DIRECT_BASE,
        (uintptr_t)__kernel_end - LA_DIRECT_BASE, &layout)) la_virt_fatal("boot memory");
    enum physical_page_status status = physical_page_allocator_init(&allocator, &layout);
    la_virt_puts("allocator init="); la_virt_hex(status); la_virt_puts(" ranges="); la_virt_hex(layout.usable_count); la_virt_puts("\n");
    if (status != PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("allocator init");
    status = physical_page_allocator_bind_access(&allocator, la_virt_page_access);
    if (status != PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("allocator access");
    status = physical_page_allocator_finalize(&allocator);
    la_virt_puts("allocator finalize="); la_virt_hex(status); la_virt_puts("\n");
    if (status != PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("allocator finalize");
    uint64_t before = physical_page_available(&allocator), page;
    if (physical_page_allocate(&allocator, &page) != PHYSICAL_PAGE_STATUS_OK) la_virt_fatal("allocate");
    uint64_t *data = la_virt_page_access(page);
    for (unsigned i = 0; i < BOAROS_PAGE_SIZE/8; i++) data[i] = page ^ i;
    for (unsigned i = 0; i < BOAROS_PAGE_SIZE/8; i++) if (data[i] != (page ^ i)) la_virt_fatal("RAM contents");
    if (physical_page_release(&allocator, page) != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_available(&allocator) != before) la_virt_fatal("page recovery");
    la_virt_puts("LA boot contracts passed; available pages="); la_virt_hex(before); la_virt_puts("\n");
    la_heap_contract(&allocator);
    la_mmu_initialize();
    la_mmu_contract(&allocator);
    if(la_mmu_kernel_window_initialize(&allocator)!=ARCH_MMU_STATUS_OK)la_virt_fatal("kernel stack page tables");
    la_stack_window_contract(&allocator);
    if (kernel_time_init(la_timer_frequency(),0)!=KERNEL_TIME_STATUS_OK ||
        kernel_scheduler_init(&allocator,(uintptr_t)__boot_stack_bottom,(uintptr_t)__boot_stack_top)!=KERNEL_SCHEDULER_STATUS_OK) la_virt_fatal("scheduler init");
    la_boot_tasks(&allocator);
    la_virt_shutdown();
}
