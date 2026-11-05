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
static void detach_range_contract(struct physical_page_allocator *allocator,
    const struct arch_mmu_page_table *kernel)
{
    struct arch_mmu_user_space space={0};
    uint64_t start=(UINT64_C(1)<<25)-32*BOAROS_PAGE_SIZE, end=start+64*BOAROS_PAGE_SIZE;
    if (arch_mmu_user_space_init(&space,allocator,kernel)) la_virt_fatal("range space");
    for (uint64_t v=start-BOAROS_PAGE_SIZE;v<=end;v+=BOAROS_PAGE_SIZE)
        if (arch_mmu_user_map_zeroed_page(&space,v,ARCH_MMU_READ|ARCH_MMU_WRITE)) la_virt_fatal("range leaf");
    if (arch_mmu_user_protect_owned_page(&space,start,0)) la_virt_fatal("range protected");
    for (unsigned pass=0;pass<2;pass++) {
        struct kernel_mm_detach_cursor cursor={0};
        unsigned calls=0, retired=0;
        enum arch_mmu_status status;
        do {
            struct kernel_mm_retirement batch={0};
            status=arch_mmu_user_detach_range(&space,pass?start-BOAROS_PAGE_SIZE:start,
                pass?end+BOAROS_PAGE_SIZE:end,&cursor,&batch,0,0);
            if ((status!=ARCH_MMU_STATUS_OK && status!=ARCH_MMU_STATUS_NOT_MAPPED) ||
                batch.count>KERNEL_MM_RETIRE_CAPACITY || ++calls>512) la_virt_fatal("bounded range");
            retired+=batch.count;
            if (batch.count) arch_mmu_sync_user(0,0,UINT64_MAX,0);
            for (unsigned i=0;i<batch.count;i++)
                if (physical_page_release(allocator,batch.pages[i])) la_virt_fatal("range owner");
        } while (status==ARCH_MMU_STATUS_OK);
        if ((!pass && (space.leaf_pages!=2 || retired!=64)) ||
            (pass && (space.leaf_pages || space.protected_pages || space.table_pages!=1 || retired!=5)))
            la_virt_fatal("range recovery");
    }
    if (arch_mmu_user_space_destroy(&space)) la_virt_fatal("range destroy");
    la_virt_puts("LA bounded range detach/protected boundaries passed\n");
}
static enum arch_mmu_status detach_step(struct physical_page_allocator *allocator,
    struct arch_mmu_user_space *space,uint64_t start,uint64_t end,
    struct kernel_mm_detach_cursor *cursor)
{
    struct kernel_mm_retirement batch={0};
    enum arch_mmu_status status=arch_mmu_user_detach_range(space,start,end,cursor,&batch,0,0);
    if (status!=ARCH_MMU_STATUS_OK && status!=ARCH_MMU_STATUS_NOT_MAPPED)
        la_virt_fatal("interleaved detach status");
    if (batch.count) arch_mmu_sync_user(0,0,UINT64_MAX,0);
    for (unsigned i=0;i<batch.count;i++)
        if (physical_page_release(allocator,batch.pages[i])) la_virt_fatal("interleaved detach owner");
    return status;
}
static void detach_interleaving_contract(struct physical_page_allocator *allocator,
    const struct arch_mmu_page_table *kernel)
{
    struct arch_mmu_user_space space={0};
    uint64_t neighbor=UINT64_C(1)<<25,start=neighbor+BOAROS_PAGE_SIZE,end=start+17*BOAROS_PAGE_SIZE;
    if (arch_mmu_user_space_init(&space,allocator,kernel)) la_virt_fatal("neighbor space");
    for (uint64_t at=start;at<end;at+=BOAROS_PAGE_SIZE)
        if (arch_mmu_user_map_zeroed_page(&space,at,ARCH_MMU_READ|ARCH_MMU_WRITE)) la_virt_fatal("neighbor target");
    struct kernel_mm_detach_cursor cursor={0};
    if (detach_step(allocator,&space,start,end,&cursor)!=ARCH_MMU_STATUS_OK ||
        arch_mmu_user_map_zeroed_page(&space,neighbor,ARCH_MMU_READ|ARCH_MMU_WRITE))
        la_virt_fatal("neighbor interleave");
    struct arch_mmu_mapping mapping;
    if (arch_mmu_user_lookup(&space,neighbor,&mapping)) la_virt_fatal("neighbor lookup");
    *(uint64_t *)la_virt_page_access(mapping.physical_address)=UINT64_C(0x5317c049);
    while (detach_step(allocator,&space,start,end,&cursor)==ARCH_MMU_STATUS_OK) {}
    if (arch_mmu_user_lookup(&space,neighbor,&mapping) || space.leaf_pages!=1 || space.table_pages!=3 ||
        *(uint64_t *)la_virt_page_access(mapping.physical_address)!=UINT64_C(0x5317c049))
        la_virt_fatal("neighbor retained");
    if (arch_mmu_user_space_destroy(&space)) la_virt_fatal("neighbor destroy");
    for (unsigned replace=0;replace<2;replace++) {
        space=(struct arch_mmu_user_space){0};cursor=(struct kernel_mm_detach_cursor){0};
        end=UINT64_C(1)<<25;start=end-14*BOAROS_PAGE_SIZE;
        if (arch_mmu_user_space_init(&space,allocator,kernel)) la_virt_fatal("parent space");
        for (uint64_t at=start;at<end;at+=BOAROS_PAGE_SIZE)
            if (arch_mmu_user_map_zeroed_page(&space,at,ARCH_MMU_READ|ARCH_MMU_WRITE)) la_virt_fatal("parent target");
        do {
            if (detach_step(allocator,&space,start,end,&cursor)!=ARCH_MMU_STATUS_OK) la_virt_fatal("parent pending");
        } while (space.leaf_pages);
        if (space.table_pages!=2) la_virt_fatal("parent pending owner");
        struct kernel_mm_detach_cursor other={0};neighbor=BOAROS_PAGE_SIZE;
        while (detach_step(allocator,&space,neighbor,neighbor+BOAROS_PAGE_SIZE,&other)==ARCH_MMU_STATUS_OK) {}
        if (space.table_pages!=1 || (replace &&
            arch_mmu_user_map_zeroed_page(&space,neighbor,ARCH_MMU_READ|ARCH_MMU_WRITE)))
            la_virt_fatal("parent replacement");
        while (detach_step(allocator,&space,start,end,&cursor)==ARCH_MMU_STATUS_OK) {}
        if ((replace && arch_mmu_user_lookup(&space,neighbor,&mapping)) ||
            arch_mmu_user_space_destroy(&space)) la_virt_fatal("parent current identity");
    }
    la_virt_puts("LA detach concurrent neighbor and retired/replaced parent passed\n");
}
void la_mmu_contract(struct physical_page_allocator *allocator)
{
    uint64_t before=physical_page_available(allocator);
    struct arch_mmu_page_table kernel={.allocator=allocator,.state=ARCH_MMU_STATE_ACTIVE};
    detach_range_contract(allocator,&kernel);
    detach_interleaving_contract(allocator,&kernel);
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
