/* Worker boundary model: a failed NIC must not stop unrelated protocol timers. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
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

/* RX boundary model: actual input decision, public pbuf refs and DMA loan API. */
struct stats_ lwip_stats;
static struct stats_mem pool_stats;
static struct pbuf *received;
static unsigned released, copied, allocation_fail;
static unsigned char lent[64];
static err_t input_result;
struct pbuf *pbuf_alloced_custom(pbuf_layer layer, u16_t length, pbuf_type type,
    struct pbuf_custom *p, void *payload, u16_t capacity)
{
    if (layer != PBUF_RAW || type != PBUF_REF || length > capacity) abort();
    memset(&p->pbuf, 0, sizeof(p->pbuf));
    p->pbuf.payload = payload; p->pbuf.len = p->pbuf.tot_len = length;
    p->pbuf.ref = 1; p->pbuf.flags = PBUF_FLAG_IS_CUSTOM;
    return &p->pbuf;
}
struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type)
{
    if (layer != PBUF_RAW || type != PBUF_POOL) abort();
    if (allocation_fail) return NULL;
    struct pbuf *p = calloc(1, sizeof(*p) + length);
    if (!p) abort();
    p->payload = p + 1; p->len = p->tot_len = length; p->ref = 1;
    pool_stats.used++; return p;
}
err_t pbuf_take(struct pbuf *p, const void *bytes, u16_t length)
{ memcpy(p->payload, bytes, length); copied += length; return ERR_OK; }
u8_t pbuf_free(struct pbuf *p)
{
    if (!--p->ref) {
        if (p->flags & PBUF_FLAG_IS_CUSTOM)
            ((struct pbuf_custom *)p)->custom_free_function(p);
        else { pool_stats.used--; free(p); }
        return 1;
    }
    return 0;
}
int riscv_virtio_mmio_net_lend(struct riscv_virtio_mmio_net *d, unsigned b)
{ if (d->loaned == 32) return -KERNEL_EAGAIN; lent[b] = 1; d->loaned++; return 0; }
void riscv_virtio_mmio_net_release(struct riscv_virtio_mmio_net *d, unsigned b)
{ released++; if (lent[b]) { lent[b] = 0; d->loaned--; } }
static err_t receive_input(struct pbuf *p, struct netif *n)
{ (void)n; received = p; return input_result; }
static int receive_contract(void)
{
    struct kernel_network owner = {0};
    unsigned char bytes[1514]; memset(bytes, 0x5a, sizeof(bytes));
    bytes[0] = 1; bytes[12] = 8; bytes[13] = 0; bytes[23] = 17;
    struct riscv_net_frame frame = {.data = bytes, .size = sizeof(bytes), .buffer = 0};
    owner.interface.input = receive_input;
    lwip_stats.memp[MEMP_PBUF_POOL] = &pool_stats;
    input_frame(&owner, &frame);
    if (received->payload != bytes || copied || released || owner.device.loaned != 1) return 1;
    received->ref++; if (pbuf_free(received) || released) return 1;
    if (!pbuf_free(received) || released != 1 || owner.device.loaned) return 1;
    owner.device.loaned = 32; released = 0;
    input_frame(&owner, &frame);
    if (received->payload == bytes || copied != sizeof(bytes) || released != 1 ||
        memcmp(received->payload, bytes, sizeof(bytes)) || owner.device.loaned != 32) return 1;
    if (!pbuf_free(received) || pool_stats.used) return 1;
    allocation_fail = 1; input_frame(&owner, &frame);
    if (released != 2 || owner.device.statistics.drops != 1) return 1;
    allocation_fail = 0; pool_stats.used = PBUF_POOL_SIZE - 16;
    input_frame(&owner, &frame);
    if (released != 3 || owner.device.statistics.drops != 2 || copied != sizeof(bytes)) return 1;
    pool_stats.used = 0; owner.device.loaned = 0; input_result = ERR_IF;
    input_frame(&owner, &frame);
    if (released != 4 || owner.device.loaned) return 1;
    printf("PASS same RX input: loan copies=0 fallback copies=%u; final reference, OOM, reserve and input error balanced\n", copied);
    return 0;
}

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
    return receive_contract();
}
