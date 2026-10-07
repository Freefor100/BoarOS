#include <kernel/heap.h>
#include <platform/loongarch_virt.h>
void la_heap_contract(struct physical_page_allocator *allocator)
{
    struct kernel_heap heap={0}; void *objects[300];
    uint64_t before=physical_page_available(allocator);
    if (kernel_heap_init(&heap,allocator,la_virt_physical_address)!=KERNEL_HEAP_STATUS_OK) la_virt_fatal("heap init");
    for (unsigned i=0;i<300;i++) {
        if (kernel_heap_allocate(&heap,17,&objects[i])!=KERNEL_HEAP_STATUS_OK) la_virt_fatal("16K small heap allocation");
        unsigned char *p=objects[i]; for (unsigned j=0;j<17;j++) p[j]=(unsigned char)i;
    }
    for (unsigned i=0;i<300;i++) {
        unsigned char *p=objects[i];for (unsigned j=0;j<17;j++) if (p[j]!=(unsigned char)i) la_virt_fatal("heap isolation");
        kernel_heap_release(&heap,objects[i]);
    }
    if (physical_page_available(allocator)!=before) la_virt_fatal("heap recovery");
    la_virt_puts("LA 16K heap contracts passed\n");
}
