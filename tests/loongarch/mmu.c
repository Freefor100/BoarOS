#include <arch/mmu.h>
#include <platform/loongarch_virt.h>
#include <arch/task.h>
void la_stack_window_contract(struct physical_page_allocator *a)
{
    uint64_t before=physical_page_available(a),first,second;
    uint64_t address=ARCH_KERNEL_STACK_WINDOW_BASE+ARCH_KERNEL_STACK_WINDOW_SIZE-BOAROS_PAGE_SIZE;
    if(physical_page_allocate(a,&first)!=PHYSICAL_PAGE_STATUS_OK ||
       physical_page_allocate(a,&second)!=PHYSICAL_PAGE_STATUS_OK)la_virt_fatal("window allocation");
    *(uint64_t *)la_virt_page_access(first)=42;*(uint64_t *)la_virt_page_access(second)=77;
    if(arch_mmu_kernel_window_map(a,ARCH_KERNEL_STACK_WINDOW_BASE,first)!=ARCH_MMU_STATUS_INVALID ||
       arch_mmu_kernel_window_map(a,address,first)!=ARCH_MMU_STATUS_OK ||
       *(volatile uint64_t *)address!=42 ||
       arch_mmu_kernel_window_map(a,address,second)!=ARCH_MMU_STATUS_CONFLICT ||
       *(volatile uint64_t *)address!=42 ||
       arch_mmu_kernel_window_unmap(a,address+1)!=ARCH_MMU_STATUS_INVALID ||
       arch_mmu_kernel_window_unmap(a,address)!=ARCH_MMU_STATUS_OK ||
       arch_mmu_kernel_window_map(a,address,second)!=ARCH_MMU_STATUS_OK ||
       *(volatile uint64_t *)address!=77 ||
       arch_mmu_kernel_window_unmap(a,address)!=ARCH_MMU_STATUS_OK)la_virt_fatal("window ownership/translation");
    (void)physical_page_release(a,first);(void)physical_page_release(a,second);
    if(physical_page_available(a)!=before)la_virt_fatal("window page baseline");
    la_virt_puts("LA kernel window conflict/reuse/recovery passed\n");
}
void la_mmu_contract(struct physical_page_allocator *allocator)
{
    uint64_t before=physical_page_available(allocator);
    struct arch_mmu_page_table kernel={.allocator=allocator,.state=ARCH_MMU_STATE_ACTIVE};
    struct arch_mmu_user_space parent={0}, child={0};
    struct arch_mmu_mapping map;
    const uint64_t address=0x400000;
    if (arch_mmu_user_space_init(&parent,allocator,&kernel)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_map_zeroed_page(&parent,address,ARCH_MMU_READ|ARCH_MMU_WRITE)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_lookup(&parent,address,&map)!=ARCH_MMU_STATUS_OK) la_virt_fatal("MMU map");
    *(uint64_t *)la_virt_page_access(map.physical_address)=0x123456789abcdef0;
    if (arch_mmu_user_space_fork(&child,&parent,0,0)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_protect_owned_page(&child,address,0)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_lookup(&child,address,&map)!=ARCH_MMU_STATUS_NOT_MAPPED ||
        arch_mmu_user_protect_owned_page(&child,address,ARCH_MMU_READ|ARCH_MMU_WRITE)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_lookup(&child,address,&map)!=ARCH_MMU_STATUS_OK || (map.permissions&ARCH_MMU_WRITE))
        la_virt_fatal("COW PROT_NONE rearm");
    if (arch_mmu_user_resolve_cow(&child,address,ARCH_MMU_READ|ARCH_MMU_WRITE)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_lookup(&child,address,&map)!=ARCH_MMU_STATUS_OK) la_virt_fatal("MMU COW");
    uint64_t *data=la_virt_page_access(map.physical_address);
    if (*data!=0x123456789abcdef0) la_virt_fatal("COW contents");
    *data=42;
    if (arch_mmu_user_lookup(&parent,address,&map)!=ARCH_MMU_STATUS_OK ||
        *(uint64_t *)la_virt_page_access(map.physical_address)!=0x123456789abcdef0) la_virt_fatal("COW isolation");
    if (arch_mmu_user_protect_owned_page(&child,address,0)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_lookup(&child,address,&map)!=ARCH_MMU_STATUS_NOT_MAPPED ||
        arch_mmu_user_protect_owned_page(&child,address,ARCH_MMU_READ)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_lookup(&child,address,&map)!=ARCH_MMU_STATUS_OK || (map.permissions&ARCH_MMU_WRITE)) la_virt_fatal("MMU permissions");
    if (arch_mmu_user_space_destroy(&child)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_space_destroy(&parent)!=ARCH_MMU_STATUS_OK || physical_page_available(allocator)!=before)
        la_virt_fatal("MMU recovery");
    la_virt_puts("LA mmu contracts passed\n");
}
