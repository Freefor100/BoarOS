#include <kernel/cost.h>
#include <kernel/console.h>
#include <kernel/physical_page.h>
#include <assert.h>
#include <stdio.h>
static unsigned char pool[128*4096] __attribute__((aligned(4096)));
static struct kernel_cost_task task;
static uint32_t depth=1;
static uint32_t *reclaim_depth(void){return &depth;}
static uint64_t reclaim(void *context,uint64_t pages){(void)context;(void)pages;assert(0);return 0;}
static void *access_page(uint64_t address){return (void *)(uintptr_t)address;}
/* The host has no UART; this legal/OOM workload must never emit a fatal. */
void kernel_console_putc(char character){(void)character;assert(0);}
uint64_t kernel_cost_clock(void){return 100;}
uint64_t kernel_cost_lock(void){return 0;}
void kernel_cost_unlock(uint64_t s){(void)s;}
struct kernel_cost_task *kernel_cost_current(void){return &task;}
int main(void)
{
    struct physical_page_allocator allocator={0};struct boot_memory_layout layout={0};
    layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(physical_page_allocator_init(&allocator,&layout)==0);
    assert(physical_page_allocator_bind_access(&allocator,access_page)==0);
    assert(physical_page_allocator_finalize(&allocator)==0);
    uint64_t pages[128],address;unsigned count=0;
    while(physical_page_available(&allocator)){assert(count<128);assert(physical_page_allocate(&allocator,&pages[count++])==0);}
    assert(physical_page_allocator_set_reclaimer(&allocator,reclaim,0)==0);allocator.reclaim_depth=reclaim_depth;
    assert(kernel_cost_begin(1,10000000,1,0)==0);
    assert(physical_page_allocate_order(&allocator,0,&address)==PHYSICAL_PAGE_STATUS_EMPTY && depth==1);
    assert(kernel_cost_end(1,0)==0);uint64_t value;
    assert(kernel_cost_read(0,COST_PAGE_CALLS,&value)==0 && value==1);
    assert(kernel_cost_read(0,COST_PAGE_FAILURES,&value)==0 && value==1);
    assert(kernel_cost_read(0,COST_PAGE_ACCEPTED,&value)==0 && value==0);
    allocator.reclaim_depth=0;assert(physical_page_allocator_clear_reclaimer(&allocator)==0);
    while(count)assert(physical_page_release(&allocator,pages[--count])==0);
    puts("real allocator reentry OOM preserves errno and reports one failed allocation");
}
