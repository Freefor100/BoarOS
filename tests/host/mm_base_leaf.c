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
int main(void)
{
 mm_test_attach(0);struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].size=sizeof(pool);
 assert(!physical_page_allocator_init(&pages,&layout)&&!physical_page_allocator_bind_access(&pages,access)&&!physical_page_allocator_finalize(&pages));
 kernel=(struct arch_mmu_page_table){.allocator=&pages,.root_address=allocate(),.table_pages=1,.state=ARCH_MMU_STATE_ACTIVE};
 uint64_t baseline=physical_page_available(&pages);struct arch_mmu_user_space space={0};
 assert(!arch_mmu_user_space_init(&space,&pages,&kernel));
 uint64_t va=0x300000,data=allocate();unsigned missing;
 assert(!arch_mmu_user_leaf_missing(&space,va,&missing));
 struct kernel_mm_leaf_preparation prepared=KERNEL_MM_LEAF_PREPARATION_INIT;
 for(unsigned i=0;i<2;i++)if(missing&(1U<<i))prepared.tables[i]=allocate();
 assert(!arch_mmu_user_map_prepared_page(&space,va,data,ARCH_MMU_READ|ARCH_MMU_WRITE,0,&prepared));
 struct kernel_mm_leaf leaf;assert(!arch_mmu_user_leaf_snapshot(&space,va+37,&leaf));
 assert(leaf.virtual_address==va&&leaf.physical_address==data&&!leaf.order);
 struct arch_mmu_mapping mapping;assert(!arch_mmu_user_lookup(&space,va+37,&mapping)&&mapping.physical_address==data+37);
 assert(!arch_mmu_user_space_destroy(&space));assert(physical_page_available(&pages)==baseline&&!physical_page_allocator_audit(&pages));
 printf("base leaf publication/snapshot use identical indices: page_shift=%u\n",BOAROS_PAGE_SHIFT);
}
