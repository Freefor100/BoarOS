#include <arch/mmu.h>
#include <platform/loongarch_virt.h>
void la_fault_injection_set(int);
enum arch_mmu_status __real_la_mmu_kernel_window_initialize(struct physical_page_allocator *);
enum arch_mmu_status __wrap_la_mmu_kernel_window_initialize(struct physical_page_allocator *a)
{
    uint64_t before=physical_page_available(a);
    for(int failure=0;failure<32;failure++) {
        la_fault_injection_set(failure);
        enum arch_mmu_status status=__real_la_mmu_kernel_window_initialize(a);
        la_fault_injection_set(-1);
        if(status==ARCH_MMU_STATUS_OK) {
            if(!failure || !arch_mmu_kernel_window_active())la_virt_fatal("window OOM coverage");
            la_virt_puts("LA kernel stack skeleton OOM rollback passed\n");return status;
        }
        if(status!=ARCH_MMU_STATUS_NO_MEMORY || arch_mmu_kernel_window_active() ||
           physical_page_available(a)!=before)la_virt_fatal("window OOM owner");
    }
    la_virt_fatal("window OOM bound");
}
