/* Linked only by ROOT_DRAIN_FIXTURE=1; ordinary kernels contain no hook. */
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include "../../fs/vfs_objects.h"
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_uart.h>
int __real_kernel_vfs_unmount(struct kernel_vfs_mount *mount);
int __wrap_kernel_vfs_unmount(struct kernel_vfs_mount *mount)
{
    /* Boot teardown detaches child mounts before the root. The final record
     * measures root unmount; earlier records retain child teardown costs. */
    int root = mount && !mount->parent && !mount->release_owner;
    struct kernel_heap *heap = root && mount->private_data ?
        ((struct kernel_vfs_instance *)mount->private_data)->heap : 0;
    uint64_t start = root ? riscv_time_read() : 0;
    int result = __real_kernel_vfs_unmount(mount);
    if (root) {
        uint64_t elapsed = riscv_time_read() - start;
        virt_uart_puts("ROOT DRAIN FIXTURE ticks=");virt_uart_put_hex(elapsed);
        virt_uart_puts(" status=");virt_uart_put_hex((unsigned long)result);virt_uart_putc('\n');
    }
    if (heap) {
        struct kernel_heap_statistics statistics;
        kernel_heap_get_statistics(heap, &statistics);
        virt_uart_puts("ROOT HEAP FIXTURE peak-pages=");virt_uart_put_hex(statistics.peak_pages);
        virt_uart_puts(" allocation-calls=");virt_uart_put_hex(statistics.allocation_calls);virt_uart_putc('\n');
    }
    return result;
}
