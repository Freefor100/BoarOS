#include <kernel/mm.h>
#include <stdint.h>

uint64_t kernel_test_pin_acquisitions;
enum kernel_mm_status __real_kernel_mm_pin_user_page(const struct kernel_mm *,
    uint64_t, uint32_t, struct kernel_mm_user_pin *);
enum kernel_mm_status __wrap_kernel_mm_pin_user_page(const struct kernel_mm *mm,
    uint64_t address, uint32_t access, struct kernel_mm_user_pin *pin)
{
    enum kernel_mm_status status = __real_kernel_mm_pin_user_page(mm,address,access,pin);
    if (!status) kernel_test_pin_acquisitions++;
    return status;
}
