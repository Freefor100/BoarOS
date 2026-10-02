/* Worker boundary model: a failed NIC must not stop unrelated protocol timers. */
#include <setjmp.h>
#include <stdio.h>
#include <kernel/errno.h>
#include "../../net/ethernet.c"

static jmp_buf stopped;
static uint64_t blocked_deadline;
static unsigned expired, errors;
uint64_t riscv_time_read(void) { return 1000; }
int riscv_virtio_mmio_net_service(struct riscv_virtio_mmio_net *d)
{ d->failed = 1; return -KERNEL_EIO; }
int riscv_virtio_mmio_net_receive(struct riscv_virtio_mmio_net *d, struct riscv_net_frame *f)
{ (void)d; (void)f; return 0; }
void netif_set_link_down(struct netif *n) { n->flags &= (u8_t)~NETIF_FLAG_LINK_UP; }
void netif_set_link_up(struct netif *n) { n->flags |= NETIF_FLAG_LINK_UP; }
void kernel_socket_network_failed(uint32_t address) { (void)address; errors++; }
void kernel_socket_network_process(void) { expired = riscv_time_read() >= 950; }
uint64_t kernel_socket_next_timer_deadline(void) { return 1200; }
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *q,
    uint64_t deadline, int interruptible, enum kernel_wait_wake_reason *reason)
{ (void)q; (void)interruptible; (void)reason; blocked_deadline = deadline; longjmp(stopped, 1); }
enum kernel_scheduler_status kernel_scheduler_yield_current(void) { return KERNEL_SCHEDULER_STATUS_OK; }
int main(void)
{
    struct kernel_network owner = {0};
    owner.device.frequency = 100;
    owner.interface.flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    if (!setjmp(stopped)) worker(&owner);
    if (!expired || errors != 1 || blocked_deadline != 1200) {
        printf("FAIL failed-NIC protocol-expired=%u errors=%u deadline=%llu\n",
               expired, errors, (unsigned long long)blocked_deadline);
        return 1;
    }
    puts("PASS failed-NIC preserves unrelated protocol timer progress");
    return 0;
}
