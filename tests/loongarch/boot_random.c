#include <kernel/physical_page.h>
#include <kernel/random.h>
#include <kernel/errno.h>
#include <platform/loongarch_virt.h>

void __wrap_la_boot_tasks(struct physical_page_allocator *allocator)
{
    uint64_t before = physical_page_available(allocator);
    unsigned char bytes[32];
    if (!kernel_random_available() || kernel_random_ready() ||
        kernel_random_credited_bits() || kernel_random_wait_ready(1) != -KERNEL_EAGAIN ||
        kernel_random_fill(bytes, sizeof(bytes)) != KERNEL_RANDOM_STATUS_OK)
        la_virt_fatal("DTB random material/trust contract");
    kernel_random_erase(bytes, sizeof(bytes));
    if (physical_page_available(allocator) != before)
        la_virt_fatal("DTB random page owner");
    la_virt_puts("LA untrusted DTB random material passed\nLA user contracts passed\n");
}
