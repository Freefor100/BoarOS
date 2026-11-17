/* 数字物理地址0的owner与空候选必须可区分；映射回独立宿主pool。 */
#include <kernel/mm_backend.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void mm_test_attach(unsigned);
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator pages;
static struct arch_mmu_page_table kernel;
static unsigned access_calls, fences;
void mm_test_fence(uint64_t,int);
static void *access(uint64_t physical)
{ access_calls++; return physical < sizeof(pool) ? pool + physical : 0; }
void riscv_smp_sync_user(uint32_t targets,uint64_t address,uint64_t size,int instructions)
{ (void)targets;(void)size;mm_test_fence(address,instructions); }
void riscv_smp_sync_instructions(uint32_t targets)
{ (void)targets;mm_test_fence(0,1); }
void riscv_smp_sync_kernel(uint64_t address,uint64_t size)
{ (void)size;mm_test_fence(address,0); }
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; fences++; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static uint64_t allocate(void)
{
    uint64_t physical;
    assert(!physical_page_allocate(&pages, &physical));
    memset(access(physical), 0, BOAROS_PAGE_SIZE);
    return physical;
}
#define HUGE (UINT64_C(1)<<21)
static void drain(struct kernel_mm_retirement *batch)
{
    arch_mmu_flush_all();
    kernel_mm_retirement_release(&pages,batch);
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0};
    layout.usable_count=1; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout));
    assert(!physical_page_allocator_bind_access(&pages,access));
    assert(!physical_page_allocator_finalize(&pages));
    kernel=(struct arch_mmu_page_table){.allocator=&pages,.root_address=allocate(),
        .table_pages=1,.state=ARCH_MMU_STATE_ACTIVE};
    uint64_t baseline=physical_page_available(&pages);
    struct arch_mmu_user_space parent={0},child={0};
    assert(!arch_mmu_user_space_init(&parent,&pages,&kernel));
    uint64_t data,replacement;
    assert(!physical_page_allocate_order(&pages,9,&data));
    memset(access(data),0x53,HUGE);
    struct kernel_mm_leaf_preparation prepared=KERNEL_MM_LEAF_PREPARATION_INIT;
    unsigned missing;
    assert(!arch_mmu_user_leaf_missing_order(&parent,HUGE,9,&missing) && missing==1);
    prepared.tables[0]=allocate();
    assert(!arch_mmu_user_map_prepared_order(&parent,HUGE,data,ARCH_MMU_READ|ARCH_MMU_WRITE,0,9,&prepared));
    assert(prepared.tables[0]==KERNEL_MM_NO_PAGE && parent.table_pages==2 && parent.leaf_pages==512);
    struct kernel_mm_leaf leaf;
    assert(!arch_mmu_user_leaf_snapshot(&parent,HUGE+12345,&leaf));
    assert(leaf.virtual_address==HUGE && leaf.physical_address==data && leaf.order==9 && !leaf.cow);
    struct arch_mmu_mapping map;
    assert(!arch_mmu_user_lookup(&parent,2*HUGE-1,&map) && map.physical_address==data+HUGE-1);
    assert(arch_mmu_user_protect_owned_page(&parent,HUGE,0)==ARCH_MMU_STATUS_INVALID);
    assert(arch_mmu_user_protect_owned_range(&parent,HUGE+BOAROS_PAGE_SIZE,2*HUGE,0)==ARCH_MMU_STATUS_INVALID);
    assert(arch_mmu_user_leaf_missing(&parent,HUGE,&missing)==ARCH_MMU_STATUS_CONFLICT);
    struct kernel_mm_detach_cursor cursor={0}; struct kernel_mm_retirement batch={0};
    assert(arch_mmu_user_detach_range(&parent,HUGE+BOAROS_PAGE_SIZE,2*HUGE,&cursor,&batch,0,0)==ARCH_MMU_STATUS_INVALID);
    assert(!batch.count && parent.leaf_pages==512);
    assert(!arch_mmu_user_space_fork(&child,&parent,0,0));
    assert(parent.cow_pages==512 && child.cow_pages==512 && child.leaf_pages==512 && child.table_pages==2);
    assert(!arch_mmu_user_leaf_snapshot(&child,HUGE,&leaf) && leaf.cow && !(leaf.permissions&ARCH_MMU_WRITE));
    assert(!physical_page_acquire_order(&pages,data,9)); /* independent copy pin */
    assert(physical_page_cow_borrow_order(&pages,data,9)==3);
    assert(!physical_page_allocate_order(&pages,9,&replacement));
    memcpy(access(replacement),access(data),HUGE);
    assert(arch_mmu_user_cow_commit_order(&child,HUGE,leaf.cookie,replacement+BOAROS_PAGE_SIZE,ARCH_MMU_READ|ARCH_MMU_WRITE,9,&batch)==ARCH_MMU_STATUS_INVALID);
    assert(!batch.count && child.cow_pages==512);
    assert(!arch_mmu_user_cow_commit_order(&child,HUGE,leaf.cookie,replacement,ARCH_MMU_READ|ARCH_MMU_WRITE,9,&batch));
    assert(batch.count==1 && batch.compound && batch.orders[0]==9);
    drain(&batch);
    assert(!physical_page_release_order(&pages,data,9)); /* COW source borrow */
    assert(!physical_page_release_order(&pages,data,9)); /* copy pin */
    assert(!arch_mmu_user_leaf_snapshot(&parent,HUGE,&leaf));
    assert(!arch_mmu_user_cow_commit_order(&parent,HUGE,leaf.cookie,KERNEL_MM_NO_PAGE,ARCH_MMU_READ|ARCH_MMU_WRITE,9,&batch));
    assert(!batch.count && !parent.cow_pages && !child.cow_pages);
    assert(!arch_mmu_user_protect_owned_range(&child,HUGE,2*HUGE,0));
    assert(child.protected_pages==512 && !child.leaf_pages);
    unsigned char byte=99;
    assert(arch_mmu_user_space_populate(&child,HUGE+777,&byte,1)==ARCH_MMU_STATUS_NOT_MAPPED);
    assert(arch_mmu_user_lookup(&child,HUGE,&map)==ARCH_MMU_STATUS_NOT_MAPPED);
    assert(!arch_mmu_user_leaf_snapshot(&child,HUGE+777,&leaf) && !leaf.permissions && leaf.order==9);
    assert(!arch_mmu_user_protect_owned_range(&child,HUGE,2*HUGE,ARCH_MMU_READ));
    assert(child.leaf_pages==512 && !child.protected_pages);
    assert(!arch_mmu_user_space_destroy(&child));
    uint64_t before=physical_page_available(&pages);
    cursor=(struct kernel_mm_detach_cursor){0};
    enum arch_mmu_status status;
    do {
        status=arch_mmu_user_detach_range(&parent,HUGE,2*HUGE,&cursor,&batch,0,0);
        assert(status==ARCH_MMU_STATUS_OK || status==ARCH_MMU_STATUS_NOT_MAPPED);
        if (batch.compound) assert(physical_page_available(&pages)==before);
        drain(&batch);
    } while (status==ARCH_MMU_STATUS_OK);
    assert(!parent.leaf_pages && parent.table_pages==1);
    assert(!arch_mmu_user_space_destroy(&parent));
    assert(physical_page_available(&pages)==baseline && !physical_page_allocator_audit(&pages));
    struct arch_mmu_user_space pinned_space={0};
    assert(!arch_mmu_user_space_init(&pinned_space,&pages,&kernel));
    assert(!physical_page_allocate_order(&pages,9,&data));
    memset(access(data),0x42,HUGE);
    prepared=(struct kernel_mm_leaf_preparation)KERNEL_MM_LEAF_PREPARATION_INIT;
    prepared.tables[0]=allocate();
    assert(!arch_mmu_user_map_prepared_order(&pinned_space,HUGE,data,ARCH_MMU_READ|ARCH_MMU_WRITE,0,9,&prepared));
    struct kernel_mm mm={0};
    assert(!kernel_mm_create(&mm,&pinned_space));
    struct kernel_mm_user_pin pins[2]={{0}}; unsigned count;
    assert(!kernel_mm_pin_user_pair(&mm,HUGE+12345,2*HUGE-1,KERNEL_MM_READ,pins,&count) && count==2);
    assert(pins[0].owner_address==data && pins[1].owner_address==data && pins[0].order==9);
    assert(pins[0].physical_address==data+(12345 & ~BOAROS_PAGE_MASK));
    assert(pins[0].page==(unsigned char *)access(data)+(12345 & ~BOAROS_PAGE_MASK));
    uint32_t refs;
    assert(!physical_page_reference_count(&pages,data,&refs) && refs==3);
    kernel_mm_unpin_user_pair(pins);
    assert(!physical_page_reference_count(&pages,data,&refs) && refs==1);
    uint32_t value=0;
    assert(!kernel_mm_try_futex_read(&mm,HUGE+12344,&value) && value==0x42424242);
    assert(!kernel_mm_release(&mm));
    assert(physical_page_available(&pages)==baseline && !physical_page_allocator_audit(&pages));
    puts("MM compound pins retain one head for interior base-page windows and futex reads");
    puts("RV 2MiB compound leaf: offsets, PROT_NONE, fork/COW, deferred retirement and recovery passed");
}
