#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/vma.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void mm_test_attach(unsigned);
static unsigned char pool[16*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
void kernel_console_putc(char c) { fputc(c,stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t v,int instructions) { (void)v;(void)instructions; }
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p;return 1; }
#define P BOAROS_PAGE_SIZE
static void usage(struct kernel_vma_set *set,uint64_t total,uint64_t data)
{ assert(kernel_vma_set_total_bytes(set)==total*P);assert(kernel_vma_set_data_bytes(set)==data*P); }
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0};layout.usable_count=1;layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout)&&!physical_page_allocator_bind_access(&pages,access)&&!physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    uint64_t baseline=physical_page_available(&pages);
    struct kernel_vma_set *set=0,*copy=0;assert(!kernel_vma_set_create(&heap,&set));
    struct kernel_vma v={.start=P,.end=2*P,.permissions=3,.maximum_permissions=7,
        .kind=KERNEL_VMA_KIND_ANONYMOUS,.role=KERNEL_VMA_ROLE_MMAP,.fault_policy=KERNEL_VMA_FAULT_DEMAND_ZERO};
    assert(!kernel_vma_set_insert(set,&v));usage(set,1,1);
    v.start=2*P;v.end=3*P;v.permissions=1;assert(!kernel_vma_set_insert(set,&v));usage(set,2,1);
    v.start=3*P;v.end=4*P;v.permissions=3;v.role=KERNEL_VMA_ROLE_STACK;
    assert(!kernel_vma_set_insert(set,&v));usage(set,3,1);
    int backing;
    v.start=4*P;v.end=5*P;v.role=KERNEL_VMA_ROLE_MMAP;v.kind=KERNEL_VMA_KIND_ANON_SHARED;
    v.backing=&backing;v.fault_policy=KERNEL_VMA_FAULT_ANON_SHARED;
    assert(!kernel_vma_set_insert(set,&v));usage(set,4,1);
    v.start=5*P;v.end=6*P;v.kind=KERNEL_VMA_KIND_FILE_PRIVATE;v.fault_policy=KERNEL_VMA_FAULT_FILE_PRIVATE;
    assert(!kernel_vma_set_insert(set,&v));usage(set,5,2);
    struct kernel_vma_edit edit;
    assert(!kernel_vma_set_prepare_protect(set,2*P,3*P,3,&edit)&&!kernel_vma_set_commit_edit(set,&edit));usage(set,5,3);
    assert(!kernel_vma_set_prepare_protect(set,P,2*P,1,&edit)&&!kernel_vma_set_commit_edit(set,&edit));usage(set,5,2);
    assert(!kernel_vma_set_prepare_replace(set,5*P,6*P,0,&edit)&&!kernel_vma_set_commit_edit(set,&edit));usage(set,4,1);
    assert(!kernel_vma_set_clone(set,&copy));usage(copy,4,1);
    assert(!kernel_vma_set_prepare_protect(copy,2*P,3*P,1,&edit)&&!kernel_vma_set_commit_edit(copy,&edit));usage(copy,4,0);usage(set,4,1);
    assert(!kernel_vma_set_destroy(&copy)&&!kernel_vma_set_destroy(&set));
    struct kernel_heap_statistics stats;kernel_heap_get_statistics(&heap,&stats);
    assert(!stats.live_allocations&&!stats.current_pages&&physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    printf("VMA_LIMITS page_shift=%u classification/edit/clone/owners PASS\n",BOAROS_PAGE_SHIFT);
}
