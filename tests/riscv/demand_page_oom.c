#include <kernel/mm.h>
#include <kernel/scheduler.h>

#include <stdint.h>

static int oom_injected;

enum kernel_mm_status
__real_kernel_scheduler_resolve_current_user_fault(
    uint64_t virtual_address,
    uint32_t access);

enum kernel_mm_status
__wrap_kernel_scheduler_resolve_current_user_fault(
    uint64_t virtual_address,
    uint32_t access)
{
    if (oom_injected == 0 &&
        virtual_address == UINT64_C(0x1000) &&
        access == KERNEL_MM_READ) {
        oom_injected = 1;
        return KERNEL_MM_STATUS_NO_MEMORY;
    }
    return __real_kernel_scheduler_resolve_current_user_fault(
        virtual_address,
        access);
}
