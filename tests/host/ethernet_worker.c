/* Worker boundary model: a failed NIC must not stop unrelated protocol timers. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/errno.h>
#include "../../net/ethernet.c"

static jmp_buf stopped;
static uint64_t blocked_deadline;
static unsigned expired, errors, worker_mode, service_calls, protocol_attempts, sent, held, yields;
uint64_t riscv_time_read(void) { return 1000; }
int virtio_net_service(struct virtio_net_device *d)
{
    if (!worker_mode) { d->failed = 1; return -KERNEL_EIO; }
    if (++service_calls > 8) abort();
    d->link_up = 1;
    if (worker_mode == 2 && service_calls == 2) d->tx_done_count = 64;
    if (worker_mode == 4 && service_calls == 2) { held = 0; d->tx_capacity_generation += 64; }
    return 0;
}
int virtio_net_receive(struct virtio_net_device *d, struct virtio_net_frame *f)
{ (void)d; (void)f; return 0; }
void netif_set_link_down(struct netif *n) { n->flags &= (u8_t)~NETIF_FLAG_LINK_UP; }
void netif_set_link_up(struct netif *n) { n->flags |= NETIF_FLAG_LINK_UP; }
void kernel_socket_network_failed(uint32_t address) { (void)address; errors++; }
struct kernel_socket_service_result kernel_socket_service_pending(struct kernel_socket_service_budget budget)
{
    (void)budget;
    expired = riscv_time_read() >= 950;
    if (worker_mode) {
        protocol_attempts++;
        if (!sent && worker_mode != 3 && held < 64) { sent++; held++; }
    }
    return (struct kernel_socket_service_result){.runnable = worker_mode >= 5 && protocol_attempts == 1, .next_deadline = 1200};
}
uintptr_t kernel_socket_protocol_enter(void) { return 0; }
void kernel_socket_protocol_leave(uintptr_t irq) { (void)irq; }
int kernel_socket_work_pending(void) { return worker_mode >= 5 && protocol_attempts == 1; }
void kernel_socket_network_capacity(void) {}
void kernel_socket_network_blocked(void) {}
uint64_t kernel_socket_next_timer_deadline(void) { return 1200; }
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *q,
    uint64_t deadline, int interruptible, enum kernel_wait_wake_reason *reason)
{ (void)q; (void)interruptible; (void)reason; blocked_deadline = deadline; longjmp(stopped, 1); }
enum kernel_scheduler_status kernel_scheduler_yield_current(void) { if (++yields > 4) abort(); return KERNEL_SCHEDULER_STATUS_OK; }

/* RX boundary model: actual input decision, public pbuf refs and DMA loan API. */
struct stats_ lwip_stats;
static struct stats_mem pool_stats;
static struct pbuf *received;
static unsigned released, copied, allocation_fail;
static unsigned abandon_calls, drain_calls;
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
void pbuf_ref(struct pbuf *p) { p->ref++; }
unsigned virtio_net_tx_release(struct virtio_net_device *d,
    void (*release)(void *), int abandon)
{
    (void)release;
    unsigned released = d->tx_done_count;
    if (abandon) abandon_calls++;
    else {
        drain_calls++;
        if (worker_mode) { held -= d->tx_done_count; d->tx_capacity_generation += d->tx_done_count; d->tx_done_count = 0; }
    }
    return released;
}
int virtio_net_send_segments(struct virtio_net_device *d,
    const struct virtio_net_tx_segment *s, unsigned count, void *owner)
{ (void)d; (void)s; (void)count; (void)owner; return -KERNEL_ENOTSUP; }
int arch_dma_image_address(uint64_t v, uint64_t size,
    uint64_t *p)
{ (void)v; (void)size; (void)p; return 0; }
int virtio_net_lend(struct virtio_net_device *d, unsigned b)
{ if (d->loaned == 32) return -KERNEL_EAGAIN; lent[b] = 1; d->loaned++; return 0; }
void virtio_net_release(struct virtio_net_device *d, unsigned b)
{ released++; if (lent[b]) { lent[b] = 0; d->loaned--; } }
static err_t receive_input(struct pbuf *p, struct netif *n)
{ (void)n; received = p; return input_result; }
static int receive_contract(void)
{
    struct virtio_net_device device = {0};
    struct kernel_network owner = {.device = &device};
    unsigned char bytes[1514]; memset(bytes, 0x5a, sizeof(bytes));
    bytes[0] = 1; bytes[12] = 8; bytes[13] = 0; bytes[23] = 17;
    struct virtio_net_frame frame = {.data = bytes, .size = sizeof(bytes), .buffer = 0};
    owner.interface.input = receive_input;
    lwip_stats.memp[MEMP_PBUF_POOL] = &pool_stats;
    input_frame(&owner, &frame);
    if (received->payload != bytes || copied || released || owner.device->loaned != 1) return 1;
    received->ref++; if (pbuf_free(received) || released) return 1;
    if (!pbuf_free(received) || released != 1 || owner.device->loaned) return 1;
    owner.device->loaned = 32; released = 0;
    input_frame(&owner, &frame);
    if (received->payload == bytes || copied != sizeof(bytes) || released != 1 ||
        memcmp(received->payload, bytes, sizeof(bytes)) || owner.device->loaned != 32) return 1;
    if (!pbuf_free(received) || pool_stats.used) return 1;
    allocation_fail = 1; input_frame(&owner, &frame);
    if (released != 2 || owner.device->statistics.drops != 1) return 1;
    allocation_fail = 0; pool_stats.used = PBUF_POOL_SIZE - 16;
    input_frame(&owner, &frame);
    if (released != 3 || owner.device->statistics.drops != 2 || copied != sizeof(bytes)) return 1;
    pool_stats.used = 0; owner.device->loaned = 0; input_result = ERR_IF;
    input_frame(&owner, &frame);
    if (released != 4 || owner.device->loaned) return 1;
    printf("PASS same RX input: loan copies=0 fallback copies=%u; final reference, OOM, reserve and input error balanced\n", copied);
    return 0;
}

int main(void)
{
    struct virtio_net_device device = {0};
    struct kernel_network owner = {.device = &device};
    owner.device->frequency = owner.frequency = 100;
    owner.interface.flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    if (!setjmp(stopped)) worker(&owner);
    if (!expired || errors != 1 || blocked_deadline != 1200) {
        printf("FAIL failed-NIC protocol-expired=%u errors=%u deadline=%llu\n",
               expired, errors, (unsigned long long)blocked_deadline);
        return 1;
    }
    /* 失败只能归还已完成owner；在途DMA在stop()复位确认前不得abandon。 */
    if (abandon_calls != 0 || drain_calls == 0) {
        printf("FAIL failed-NIC tx-release abandon=%u drain=%u\n",
               abandon_calls, drain_calls);
        return 1;
    }
    printf("PASS failed-NIC preserves unrelated protocol timer progress, tx drain=%u abandon=%u\n",
           drain_calls, abandon_calls);
    for (worker_mode = 1; worker_mode <= 5; worker_mode++) {
        memset(&owner, 0, sizeof(owner)); memset(&device, 0, sizeof(device)); owner.device = &device; owner.device->frequency = owner.frequency = 100;
        owner.device->tx_done_count = worker_mode == 2 || worker_mode == 4 ? 0 : 64;
        held = 64; sent = service_calls = protocol_attempts = yields = 0;
        if (!setjmp(stopped)) worker(&owner);
        if ((worker_mode != 3 && sent != 1) || (worker_mode == 3 && (sent || protocol_attempts != 1 || yields))) {
            printf("FAIL TX completion mode=%u attempts=%u sent=%u held=%u yields=%u\n",
                worker_mode, protocol_attempts, sent, held, yields);
            return 1;
        }
        printf("PASS TX completion mode=%u attempts=%u sent=%u held=%u yields=%u\n",
            worker_mode, protocol_attempts, sent, held, yields);
    }
    worker_mode = 6; protocol_attempts = yields = sent = held = 0;
    memset(&owner, 0, sizeof(owner)); memset(&device, 0, sizeof(device)); owner.device = &device; owner.device->frequency = owner.frequency = 100;
    if (!setjmp(stopped)) timer_worker(&owner);
    if (protocol_attempts != 2 || yields != 1 || blocked_deadline != 1200) return 1;
    puts("PASS timer-only worker drains runnable software before sleeping");
    worker_mode = 0;
    return receive_contract();
}
