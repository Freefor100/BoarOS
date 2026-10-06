#include <arch/mmu.h>
#include <platform/loongarch_virt.h>

void __wrap_la_boot_tasks(struct physical_page_allocator *allocator)
{
    uint64_t page;
    if (physical_page_allocate(allocator,&page)!=PHYSICAL_PAGE_STATUS_OK)
        la_virt_fatal("fatal fixture allocation");
#if LA_FATAL_CASE == 1
    la_virt_puts("LA fatal fixture invalid release\n");
    (void)physical_page_release(allocator,page+1);
#elif LA_FATAL_CASE == 2
    la_virt_puts("LA fatal fixture duplicate release\n");
    (void)physical_page_release(allocator,page);
    (void)physical_page_release(allocator,page);
#else
    struct arch_mmu_page_table kernel={.allocator=allocator,.state=ARCH_MMU_STATE_ACTIVE};
    struct arch_mmu_user_space space={0};
    struct arch_mmu_mapping mapping;
    if (arch_mmu_user_space_init(&space,allocator,&kernel)!=ARCH_MMU_STATUS_OK)
        la_virt_fatal("fatal fixture space");
    la_virt_puts("LA fatal fixture damaged page table owner\n");
    /* 非对齐目录地址违反硬件页表和物理 owner 契约，必须 fail-stop。 */
    ((uint64_t *)la_virt_page_access(space.root_address))[0]=page+1;
    (void)arch_mmu_user_lookup(&space,0,&mapping);
#endif
    la_virt_puts("LA fatal fixture unexpectedly returned\n");
    la_virt_shutdown();
}
