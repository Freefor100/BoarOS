#include <kernel/mm.h>

static int fail_first_release = 1;

enum kernel_mm_status __real_kernel_mm_release(struct kernel_mm *mm);

enum kernel_mm_status __wrap_kernel_mm_release(struct kernel_mm *mm)
{
    if (fail_first_release) {
        fail_first_release = 0;
        return KERNEL_MM_STATUS_PAGE_RELEASE;
    }
    return __real_kernel_mm_release(mm);
}
