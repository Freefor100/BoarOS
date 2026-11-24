#include <arch/mmu.h>
#include <platform/loongarch_virt.h>
#include <arch/task.h>
#include <kernel/irq.h>
#include <string.h>

/* Recover after one deliberately faulting kernel load without bypassing the
 * architecture's ordinary table walk and refill code. */
extern unsigned char la_secret_test_entry[];
__asm__(".pushsection .text.la_secret_test_entry, \"ax\", @progbits\n"
        ".balign 4096\n"
        "la_secret_test_entry:\n"
        "csrrd $t1, 0x5\n"
        "bstrpick.d $t1, $t1, 21, 16\n"
        "csrrd $t0, 0x6\n"
        "addi.d $t0, $t0, 4\n"
        "csrwr $t0, 0x6\n"
        "ertn\n"
        ".popsection");

static int secret_load_fault(uintptr_t address)
{
    KERNEL_IRQ_SCOPE(irq);
    uintptr_t entry = (uintptr_t)la_secret_test_entry, saved;
    unsigned code;
    __asm__ volatile("csrrd %[saved], 0xc\n\t"
                     "csrwr %[entry], 0xc\n\t"
                     "move $t1, $zero\n\t"
                     "ld.d $zero, %[address], 0\n\t"
                     "move %[code], $t1\n\t"
                     "csrwr %[saved], 0xc"
        : [saved] "=&r"(saved), [entry] "+&r"(entry), [code] "=&r"(code)
        : [address] "r"(address)
        : "$t0", "$t1", "memory");
    return code;
}

static void secret_direct_map_contract(struct physical_page_allocator *allocator)
{
    uint64_t page, first;
    if (!arch_mmu_secret_page_supported() || physical_page_allocate(allocator, &page) ||
        physical_page_allocate(allocator, &first))
        la_virt_fatal("secret page setup");
    volatile uint64_t *bytes = la_virt_page_access(page);
    volatile uint64_t *neighbour = la_virt_page_access(first);
    *bytes = 0x1234;
    *neighbour = 0x5678;
    uint64_t available = physical_page_available(allocator);
    if (secret_load_fault((uintptr_t)bytes) ||
        !secret_load_fault(LA_BOOT_CACHED_BASE + page) ||
        !secret_load_fault(LA_BOOT_UNCACHED_BASE + page) ||
        !secret_load_fault(LA_UNCACHED_BASE + page) ||
        arch_mmu_secret_page_protect(allocator, page) ||
        !secret_load_fault((uintptr_t)bytes) || *neighbour != 0x5678)
        la_virt_fatal("secret page direct aliases");
    if (arch_mmu_secret_page_protect(allocator, first) ||
        !secret_load_fault((uintptr_t)neighbour) ||
        arch_mmu_secret_page_restore(allocator, page) || *bytes != 0x1234 ||
        !secret_load_fault((uintptr_t)neighbour) ||
        arch_mmu_secret_page_restore(allocator, first) || *neighbour != 0x5678 ||
        physical_page_available(allocator) != available ||
        physical_page_release(allocator, page) || physical_page_release(allocator, first))
        la_virt_fatal("secret page restore");
    la_virt_puts("LA secret direct alias withdrawal and restoration passed\n");
}

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
static void huge_check(int condition, unsigned stage)
{
    if (condition) return;
    la_virt_puts("LA huge stage="); la_virt_hex(stage); la_virt_puts("\n");
    la_virt_fatal("huge leaf contract");
}
static int huge_share(void *context, uint64_t address)
{ (void)context; (void)address; return ARCH_MMU_FORK_SHARED; }
static int huge_skip(void *context, uint64_t address)
{ (void)context; (void)address; return ARCH_MMU_FORK_SKIP; }
static void huge_leaf_contract(struct physical_page_allocator *allocator,
    const struct arch_mmu_page_table *kernel)
{
    const uint64_t size = ARCH_MMU_HUGE_SIZE, address = size * 4;
    const uint32_t order = ARCH_MMU_HUGE_ORDER, pages = 1U << order;
    uint64_t before = physical_page_available(allocator), physical, replacement;
    struct arch_mmu_user_space parent = {0}, child = {0}, shared = {0}, skipped = {0};
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    struct kernel_mm_leaf leaf, original;
    struct arch_mmu_mapping mapping;
    unsigned missing;
    huge_check(!arch_mmu_user_space_init(&parent, allocator, kernel) &&
        !physical_page_allocate_order(allocator, order, &physical), 1);
    memset(la_virt_page_access(physical), 0x4b, size);
    huge_check(arch_mmu_user_map_owned_page(&parent, address, physical, 3) == ARCH_MMU_STATUS_INVALID, 2);
    huge_check(!arch_mmu_user_leaf_missing_order(&parent, address, order, &missing) && missing == 1, 3);
    huge_check(!physical_page_allocate(allocator, &prepared.tables[0]), 4);
    memset(la_virt_page_access(prepared.tables[0]), 0, BOAROS_PAGE_SIZE);
    huge_check(!arch_mmu_user_map_prepared_order(&parent, address, physical, 3, 0, order, &prepared) &&
        prepared.tables[0] == KERNEL_MM_NO_PAGE && parent.table_pages == 2 && parent.leaf_pages == pages, 5);
    huge_check(arch_mmu_user_leaf_missing_order(&parent, address, 9, &missing) == ARCH_MMU_STATUS_INVALID &&
        arch_mmu_user_leaf_missing(&parent, address, &missing) == ARCH_MMU_STATUS_CONFLICT &&
        arch_mmu_user_map_prepared_order(&parent, address, physical + BOAROS_PAGE_SIZE, 3, 0, order, &prepared) == ARCH_MMU_STATUS_INVALID &&
        arch_mmu_user_map_prepared_order(&parent, address, physical, 3, 0, order, &prepared) == ARCH_MMU_STATUS_CONFLICT &&
        prepared.tables[0] == KERNEL_MM_NO_PAGE && parent.table_pages == 2, 29);
    huge_check(!arch_mmu_user_leaf_snapshot(&parent, address + size - 1, &original) &&
        original.virtual_address == address && original.physical_address == physical && original.order == order, 6);
    huge_check(!arch_mmu_user_lookup(&parent, address + size - 1, &mapping) &&
        mapping.physical_address == physical + size - 1, 7);
    uint64_t cursor = address + BOAROS_PAGE_SIZE, found;
    huge_check(!arch_mmu_user_next_owned(&parent, &cursor, address + size, &found) &&
        found == address && cursor == address + size, 8);
    huge_check(arch_mmu_user_protect_owned_range(&parent, address + BOAROS_PAGE_SIZE, address + size, 0) == ARCH_MMU_STATUS_INVALID &&
        arch_mmu_user_unmap_owned_range(&parent, address, address + size - BOAROS_PAGE_SIZE) == ARCH_MMU_STATUS_INVALID &&
        arch_mmu_user_protect_owned_page(&parent, address, 0) != ARCH_MMU_STATUS_OK &&
        !arch_mmu_user_leaf_snapshot(&parent, address, &leaf) && leaf.cookie == original.cookie, 9);
    struct kernel_mm_detach_cursor rejected_cursor = {0};
    struct kernel_mm_retirement rejected_batch = {0};
    huge_check(arch_mmu_user_detach_range(&parent, address + BOAROS_PAGE_SIZE, address + size,
        &rejected_cursor, &rejected_batch, NULL, NULL) == ARCH_MMU_STATUS_INVALID &&
        !rejected_cursor.phase && !rejected_batch.count, 30);
    huge_check(!arch_mmu_user_map_zeroed_page(&parent, address + size, 3), 10);
    huge_check(arch_mmu_user_leaf_missing_order(&parent, address + size, order, &missing) == ARCH_MMU_STATUS_CONFLICT, 31);
    /* The refill must cover BOTH EntryLo halves, the last byte, and then
     * return to a base-page translation without retaining a huge PS. */
    uint64_t old_context = arch_mmu_current_context();
    huge_check(!arch_mmu_switch_context(parent.root_address), 11);
    volatile unsigned char *v = (volatile unsigned char *)(uintptr_t)address;
    huge_check(v[0] == 0x4b && v[size/2] == 0x4b && v[size-1] == 0x4b, 12);
    v[0] = 0x11; v[size/2] = 0x22; v[size-1] = 0x33; v[size] = 0x44;
    arch_mmu_flush_all();
    huge_check(v[size] == 0x44 && v[size-1] == 0x33 && v[0] == 0x11 && v[size/2] == 0x22, 13);
    huge_check(!arch_mmu_switch_context(old_context), 14);
    huge_check(!arch_mmu_user_space_fork(&shared, &parent, huge_share, NULL) &&
        !arch_mmu_user_leaf_snapshot(&shared, address, &leaf) && !leaf.cow && (leaf.permissions & ARCH_MMU_WRITE), 15);
    huge_check(!arch_mmu_user_space_fork(&skipped, &parent, huge_skip, NULL) &&
        !skipped.leaf_pages && skipped.table_pages == 1 && !arch_mmu_user_space_destroy(&skipped) &&
        !arch_mmu_user_space_destroy(&shared), 16);
    huge_check(!arch_mmu_user_space_fork(&child, &parent, NULL, NULL) &&
        parent.cow_pages == pages + 1 && child.cow_pages == pages + 1, 17);
    huge_check(!arch_mmu_user_protect_owned_range(&child, address, address + size, 0) &&
        child.protected_pages == pages && !arch_mmu_user_leaf_snapshot(&child, address + size/2, &leaf) &&
        leaf.cow && !leaf.permissions && arch_mmu_user_lookup(&child, address, &mapping) == ARCH_MMU_STATUS_NOT_MAPPED, 18);
    huge_check(!arch_mmu_user_protect_owned_range(&child, address, address + size, 3) &&
        !arch_mmu_user_leaf_snapshot(&child, address, &leaf) && leaf.cow && !(leaf.permissions & ARCH_MMU_WRITE), 19);
    huge_check(!physical_page_acquire_order(allocator, physical, order) &&
        !physical_page_allocate_order(allocator, order, &replacement), 20);
    memcpy(la_virt_page_access(replacement), la_virt_page_access(physical), size);
    ((unsigned char *)la_virt_page_access(replacement))[size/2] = 0x55;
    huge_check(!arch_mmu_switch_context(child.root_address) && v[size/2] == 0x22, 32);
    struct kernel_mm_retirement batch = {0};
    huge_check(arch_mmu_user_cow_commit(&child, address, leaf.cookie, replacement, 3, &batch) == ARCH_MMU_STATUS_INVALID &&
        arch_mmu_user_cow_commit_order(&child, address, leaf.cookie ^ 1, replacement, 3, order, &batch) == ARCH_MMU_STATUS_CONFLICT &&
        !batch.count, 21);
    huge_check(!arch_mmu_user_cow_commit_order(&child, address, leaf.cookie, replacement, 3, order, &batch) &&
        batch.count == 1 && batch.orders[0] == order && batch.pages[0] == physical, 22);
    arch_mmu_sync_user(0, address, size, 0);
    huge_check(v[size/2] == 0x55 && v[size-1] == 0x33 &&
        !arch_mmu_switch_context(old_context), 33);
    kernel_mm_retirement_release(allocator, &batch);
    uint32_t refs;
    huge_check(!physical_page_reference_count(allocator, physical, &refs) && refs == 2 &&
        !physical_page_release_order(allocator, physical, order), 23);
    huge_check(!arch_mmu_user_leaf_snapshot(&parent, address, &leaf) &&
        !arch_mmu_user_cow_commit_order(&parent, address, leaf.cookie, KERNEL_MM_NO_PAGE, 3, order, &batch) &&
        !batch.count && parent.cow_pages == 1, 24);
    huge_check(!physical_page_acquire_order(allocator, replacement, order), 25);
    uint64_t before_detach = physical_page_available(allocator);
    struct kernel_mm_detach_cursor detach = {0};
    enum arch_mmu_status status;
    do {
        status = arch_mmu_user_detach_range(&child, address, address + size, &detach, &batch, NULL, NULL);
        huge_check(status == ARCH_MMU_STATUS_OK || status == ARCH_MMU_STATUS_NOT_MAPPED, 26);
        arch_mmu_sync_user(0, address, size, 0);
        kernel_mm_retirement_release(allocator, &batch);
    } while (status == ARCH_MMU_STATUS_OK);
    huge_check(physical_page_available(allocator) == before_detach &&
        !physical_page_reference_count(allocator, replacement, &refs) && refs == 1 &&
        ((unsigned char *)la_virt_page_access(replacement))[size-1] == 0x33, 27);
    huge_check(!physical_page_release_order(allocator, replacement, order) &&
        !arch_mmu_user_space_destroy(&child) && !arch_mmu_user_space_destroy(&parent) &&
        physical_page_available(allocator) == before && !physical_page_allocator_audit(allocator), 28);
    la_virt_puts("LA huge PMD halves/refill/fork/COW/protect/pin/retire passed\n");
}
void la_mmu_contract(struct physical_page_allocator *allocator)
{
    secret_direct_map_contract(allocator);
    uint64_t before=physical_page_available(allocator);
    struct arch_mmu_page_table kernel={.allocator=allocator,.state=ARCH_MMU_STATE_ACTIVE};
    huge_leaf_contract(allocator,&kernel);
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
