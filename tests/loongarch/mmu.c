#include <arch/mmu.h>
#include <platform/loongarch_virt.h>
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
