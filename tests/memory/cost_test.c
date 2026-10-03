#include <kernel/cost.h>
#include <kernel/console.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/sync.h>
#include <kernel/uaccess.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char pool[128 * 4096] __attribute__((aligned(4096)));
static struct kernel_cost_task actor;
static struct kernel_io_context io_context;
static uint64_t ticks=100, mapped[2];
static unsigned mapped_count=2;
uint64_t kernel_cost_clock(void) { return ticks++; }
uint64_t kernel_cost_lock(void) { return 0; }
void kernel_cost_unlock(uint64_t s) { (void)s; }
struct kernel_cost_task *kernel_cost_current(void) { return &actor; }
struct kernel_io_context *kernel_io_context_current(void) { return &io_context; }
void kernel_console_putc(char c) { (void)c; assert(0); }
static void *page_access(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *p,uint64_t *address) { *address=(uintptr_t)p; return 1; }
enum kernel_mm_status kernel_mm_lookup(const struct kernel_mm *mm,uint64_t va,struct kernel_mm_mapping *mapping)
{
    (void)mm;
    if (va < 0x4000 || va >= 0x4000 + mapped_count*4096) return KERNEL_MM_STATUS_NOT_MAPPED;
    mapping->physical_address=mapped[(va-0x4000)/4096] + (va & 4095);
    mapping->permissions=KERNEL_MM_USER|KERNEL_MM_READ|KERNEL_MM_WRITE;
    return KERNEL_MM_STATUS_OK;
}
enum kernel_mm_status kernel_mm_resolve_user_fault(struct kernel_mm *mm,uint64_t va,uint32_t access)
{ (void)mm; (void)va; (void)access; return KERNEL_MM_STATUS_NOT_MAPPED; }
static char snapshot[1048576];
static void expect(const char *name,unsigned value,unsigned samples,unsigned maximum)
{
    char line[128];
    snprintf(line,sizeof(line),"foreground.%s.value=%u\n",name,value); assert(strstr(snapshot,line));
    snprintf(line,sizeof(line),"foreground.%s.samples=%u\n",name,samples); assert(strstr(snapshot,line));
    snprintf(line,sizeof(line),"foreground.%s.max=%u\n",name,maximum); assert(strstr(snapshot,line));
}
int main(void)
{
    struct physical_page_allocator allocator={0}; struct boot_memory_layout layout={0};
    struct kernel_heap heap; struct kernel_mm mm={.allocator=&allocator};
    layout.usable_count=1; layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(physical_page_allocator_init(&allocator,&layout)==0);
    assert(physical_page_allocator_bind_access(&allocator,page_access)==0);
    assert(physical_page_allocator_finalize(&allocator)==0);
    assert(kernel_heap_init(&heap,&allocator,page_address)==0);
    for(unsigned i=0;i<2;i++) assert(physical_page_allocate(&allocator,&mapped[i])==0);
    assert(kernel_cost_begin(1,10000000,1,0)==0);
    void *old,*fresh,*zero;
    assert(kernel_heap_allocate_zeroed(&heap,1,17,&old)==0);
    assert(kernel_heap_allocate_zeroed(&heap,0,17,&zero)==0 && zero==0);
    assert(kernel_heap_allocate_zeroed(&heap,SIZE_MAX,17,&zero)==KERNEL_HEAP_STATUS_OVERFLOW);
    assert(kernel_heap_resize(&heap,old,65,&fresh)==0);
    unsigned char input[11]={1,2,3,4,5,6,7,8,9,10,11}, output[11]={0}; size_t copied;
    assert(kernel_copy_to_user(&mm,0x4ffd,input,11,&copied)==0 && copied==11);
    assert(kernel_copy_from_user(&mm,output,0x4ffd,11,&copied)==0 && copied==11);
    assert(memcmp(input,output,11)==0);
    mapped_count=1;
    assert(kernel_copy_from_user(&mm,output,0x4ffd,11,&copied)==KERNEL_UACCESS_STATUS_FAULT && copied==3);
    assert(kernel_copy_from_user(&mm,0,UINT64_MAX,0,&copied)==0 && copied==0);
    assert(kernel_cost_end(1,0)==0);
    assert(kernel_cost_format(snapshot,sizeof(snapshot))>0);
    expect("heap_zero_bytes",17,2,17); expect("heap_copy_bytes",32,1,32);
    expect("uaccess_copy_bytes",25,5,8);
    /* The controlled clock advances once between the two reads around each body. */
    expect("heap_zero_ticks",2,2,1); expect("heap_copy_ticks",1,1,1);
    expect("uaccess_copy_ticks",5,5,1);
    assert(strstr(snapshot,"storage_bytes=65436\n"));
    assert(!strstr(snapshot,"heap_zero_ticks.bucket."));
    assert(kernel_heap_release(&heap,fresh)==0);
    for(unsigned i=0;i<2;i++) assert(physical_page_release(&allocator,mapped[i])==0);
    puts("memory cost: successful heap bodies and resolved page chunks counted exactly");
}
