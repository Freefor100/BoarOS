/* 数字物理地址0的owner与空候选必须可区分；映射回独立宿主pool。 */
#include <arch/mmu.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void mm_test_attach(unsigned);
static unsigned char pool[4 * 1024 * 1024] __attribute__((aligned(4096)));
static struct physical_page_allocator pages;
static struct arch_mmu_page_table kernel;
static unsigned access_calls;
static void *access(uint64_t physical)
{ access_calls++; return physical < sizeof(pool) ? pool + physical : 0; }
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t address, int instructions)
{ (void)address; (void)instructions; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static uint64_t allocate(void)
{
    uint64_t physical;
    assert(!physical_page_allocate(&pages, &physical));
    memset(access(physical), 0, BOAROS_PAGE_SIZE);
    return physical;
}
static void take_zero(void)
{
    uint64_t held[1024]; unsigned count = 0;
    while (1) {
        uint64_t physical = allocate();
        if (!physical) break;
        assert(count < 1024); held[count++] = physical;
    }
    while (count) assert(!physical_page_release(&pages, held[--count]));
}
static void drain(struct kernel_mm_retirement *batch)
{
    arch_mmu_flush_all();
    for (unsigned i = 0; i < batch->count; i++)
        assert(!physical_page_release(&pages, batch->pages[i]));
    batch->count = 0;
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout = {0};
    layout.usable_count = 1; layout.usable[0].size = sizeof(pool);
    assert(!physical_page_allocator_init(&pages, &layout));
    assert(!physical_page_allocator_bind_access(&pages, access));
    assert(allocate() == 0); /* finalize 导入真实bootstrap零页owner。 */
    assert(!physical_page_allocator_finalize(&pages));
    /* 启动builder使用未分页物理直访；本模型只借用一个真实拥有的空template。 */
    kernel = (struct arch_mmu_page_table){.allocator = &pages, .root_address = allocate(),
        .table_pages = 1, .state = ARCH_MMU_STATE_ACTIVE};
    uint64_t baseline = physical_page_available(&pages) + 1;
    struct arch_mmu_user_space parent = {0}, child = {0};
    assert(!arch_mmu_user_space_init(&parent, &pages, &kernel));
    struct kernel_mm_leaf_preparation prepared = KERNEL_MM_LEAF_PREPARATION_INIT;
    prepared.tables[0] = 0; prepared.tables[1] = allocate();
    uint64_t data = allocate();
    assert(!arch_mmu_user_map_prepared_page(&parent, 0x40000, data,
        ARCH_MMU_READ | ARCH_MMU_WRITE, &prepared));
    assert(prepared.tables[0] == KERNEL_MM_NO_PAGE && prepared.tables[1] == KERNEL_MM_NO_PAGE);
    struct arch_mmu_mapping mapping;
    assert(!arch_mmu_user_lookup(&parent, 0x40000, &mapping) && mapping.physical_address == data);
    const uint64_t far = UINT64_C(0x1000000000);
    assert(!arch_mmu_user_map_zeroed_page(&parent,far,ARCH_MMU_READ|ARCH_MMU_WRITE));
    assert(!arch_mmu_user_protect_owned_page(&parent,far,0));
    uint64_t cursor = 0x40000, found;
    assert(!arch_mmu_user_next_owned(&parent,&cursor,0x41000,&found) && found==0x40000);
    unsigned before = access_calls;
    assert(arch_mmu_user_next_owned(&parent,&cursor,0x41000,&found)==ARCH_MMU_STATUS_NOT_MAPPED &&
           access_calls==before);
    cursor = 0x41000; before = access_calls;
    assert(arch_mmu_user_next_owned(&parent,&cursor,far,&found)==ARCH_MMU_STATUS_NOT_MAPPED &&
           cursor==far && access_calls-before<=3); /* 不进入 end 外的驻留子树。 */
    assert(!arch_mmu_user_next_owned(&parent,&cursor,far+BOAROS_PAGE_SIZE,&found) && found==far);
    assert(!arch_mmu_user_space_destroy(&parent));
    assert(physical_page_available(&pages) == baseline);

    take_zero();
    parent = (struct arch_mmu_user_space){0};
    assert(!arch_mmu_user_space_init(&parent, &pages, &kernel));
    assert(!arch_mmu_user_map_owned_page(&parent, 0x40000, 0, ARCH_MMU_READ | ARCH_MMU_WRITE));
    *(unsigned char *)access(0) = 19;
    assert(!arch_mmu_user_space_fork(&child, &parent, 0, 0));
    uint64_t cookie, original;
    assert(!arch_mmu_user_cow_snapshot(&parent, 0x40000, &cookie, &original) && original == 0);
    uint64_t replacement = allocate(); *(unsigned char *)access(replacement) = 31;
    struct kernel_mm_retirement batch = {0};
    assert(!arch_mmu_user_cow_commit(&parent, 0x40000, cookie, replacement,
        ARCH_MMU_READ | ARCH_MMU_WRITE, &batch));
    assert(batch.count == 1 && batch.pages[0] == 0); drain(&batch);
    assert(!arch_mmu_user_cow_snapshot(&child, 0x40000, &cookie, &original) && original == 0);
    assert(!arch_mmu_user_cow_commit(&child, 0x40000, cookie, KERNEL_MM_NO_PAGE,
        ARCH_MMU_READ | ARCH_MMU_WRITE, &batch) && !batch.count);
    assert(*(unsigned char *)access(0) == 19 && *(unsigned char *)access(replacement) == 31);
    assert(!arch_mmu_user_space_destroy(&child) && !arch_mmu_user_space_destroy(&parent));
    assert(physical_page_available(&pages) == baseline);

    take_zero();
    parent = (struct arch_mmu_user_space){0}; child = (struct arch_mmu_user_space){0};
    assert(!arch_mmu_user_space_init(&parent, &pages, &kernel));
    data = allocate(); *(unsigned char *)access(data) = 49;
    assert(!arch_mmu_user_map_owned_page(&parent, 0x40000, data, ARCH_MMU_READ | ARCH_MMU_WRITE));
    assert(!arch_mmu_user_space_fork(&child, &parent, 0, 0));
    assert(!arch_mmu_user_cow_snapshot(&parent, 0x40000, &cookie, &original));
    *(unsigned char *)access(0) = 77;
    assert(!arch_mmu_user_cow_commit(&parent, 0x40000, cookie, 0,
        ARCH_MMU_READ | ARCH_MMU_WRITE, &batch) && batch.count == 1); drain(&batch);
    assert(!arch_mmu_user_lookup(&parent, 0x40000, &mapping) && mapping.physical_address == 0);
    assert(*(unsigned char *)access(0) == 77 && *(unsigned char *)access(original) == 49);
    assert(!arch_mmu_user_space_destroy(&child) && !arch_mmu_user_space_destroy(&parent));
    assert(physical_page_available(&pages) == baseline && !physical_page_allocator_audit(&pages));
    puts("MM physical-zero prepared table, COW source/in-place/replacement and owners verified");
}
