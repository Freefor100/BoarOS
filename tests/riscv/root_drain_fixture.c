/* Linked only by ROOT_DRAIN_FIXTURE=1; ordinary kernels contain no hook. */
#include <kernel/vfs.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
int __real_kernel_vfs_unmount(struct kernel_vfs_mount *mount);
int __wrap_kernel_vfs_unmount(struct kernel_vfs_mount *mount)
{
    /* Boot teardown detaches child mounts before the root. The final record
     * measures root unmount; earlier records retain child teardown costs. */
    int root = mount && !mount->parent && !mount->release_owner;
    uint64_t start = root ? riscv_time_read() : 0;
    int result = __real_kernel_vfs_unmount(mount);
    if (root) {
        uint64_t elapsed = riscv_time_read() - start;
        virt_uart_puts("ROOT DRAIN FIXTURE ticks=");virt_uart_put_hex(elapsed);
        virt_uart_puts(" status=");virt_uart_put_hex((unsigned long)result);virt_uart_putc('\n');
    }
    return result;
}
