#ifndef BOAROS_RISCV_VIRTIO_MMIO_NET_H
#define BOAROS_RISCV_VIRTIO_MMIO_NET_H
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <stdint.h>

#define RISCV_NET_QUEUE_SIZE 32U
#define RISCV_NET_BUFFERS 64U
#define RISCV_NET_BUFFER_SIZE 2048U
#define RISCV_NET_TX_SEGMENTS 2U
#define RISCV_NET_TX_TABLE_OFFSET 64U

struct riscv_net_frame { void *data; uint32_t size, buffer; };
struct riscv_net_tx_segment { uint64_t physical_address; uint32_t length; };
struct riscv_net_statistics {
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes, drops, errors, interrupts;
    uint64_t loan_packets, loan_bytes, loan_peak, copied_packets, copied_bytes;
    uint64_t tx_sg_packets, tx_copy_packets;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t tx_done_free_count, tx_done_free_ticks, tx_done_free_max;
    uint64_t tx_free_post_count, tx_free_post_ticks, tx_free_post_max;
    uint64_t tx_latency_overflow;
#endif
};
struct riscv_virtio_mmio_net {
    volatile uint8_t *mmio;
    struct physical_page_allocator *allocator;
    void *queues, *rx_memory, *tx_memory;
    uint64_t queue_phys, rx_phys, tx_phys, frequency;
    struct kernel_wait_queue progress;
    struct kernel_thread_join worker;
    struct riscv_net_statistics statistics;
    uint64_t tx_time[RISCV_NET_BUFFERS];
#if BOAROS_COST_DIAGNOSTICS
    /* 只记录软件收割完成；未使用槽及 reset 撤销不能伪造 DMA 完成样本。 */
    uint64_t tx_done_time[RISCV_NET_BUFFERS], tx_free_time[RISCV_NET_BUFFERS];
    uint64_t tx_done_valid, tx_free_valid;
#endif
    uint64_t tx_capacity_generation;
    uint32_t version, queue_order, irq_source, feature_low;
    uint32_t rx_posted, tx_posted;
    uint16_t available[2], consumed[2];
    uint16_t rx_free[RISCV_NET_BUFFERS], rx_ready[RISCV_NET_BUFFERS];
    uint16_t tx_pending[RISCV_NET_BUFFERS];
    uint16_t rx_head, rx_count, ready_head, ready_count, tx_head, tx_count;
    uint16_t rx_map[RISCV_NET_QUEUE_SIZE], tx_map[RISCV_NET_QUEUE_SIZE];
    uint16_t rx_length[RISCV_NET_BUFFERS];
    uint8_t rx_state[RISCV_NET_BUFFERS], tx_state[RISCV_NET_BUFFERS];
    uint8_t mac[6], link_up, configured, irq_registered, stopping, failed;
    uint16_t loaned;
    /* 零拷贝 TX：每槽持有调用方 owner 与 indirect 表长度；完成后经 tx_release 归还。 */
    void *tx_owner[RISCV_NET_BUFFERS];
    uint16_t tx_table[RISCV_NET_BUFFERS];
    uint16_t tx_done[RISCV_NET_BUFFERS];
    uint16_t tx_done_head, tx_done_count;
};

int riscv_virtio_mmio_net_init(struct riscv_virtio_mmio_net *device,
    volatile void *mmio, uint64_t size, struct physical_page_allocator *allocator,
    uint64_t frequency, uint32_t source);
/* Caller must join the worker; outstanding custom pbufs keep this owner live. */
int riscv_virtio_mmio_net_stop(struct riscv_virtio_mmio_net *device);
int riscv_virtio_mmio_net_service(struct riscv_virtio_mmio_net *device);
int riscv_virtio_mmio_net_receive(struct riscv_virtio_mmio_net *device, struct riscv_net_frame *frame);
int riscv_virtio_mmio_net_lend(struct riscv_virtio_mmio_net *device, unsigned buffer);
void riscv_virtio_mmio_net_release(struct riscv_virtio_mmio_net *device, unsigned buffer);
int riscv_virtio_mmio_net_send(struct riscv_virtio_mmio_net *device, const void *data, uint32_t size);
int riscv_virtio_mmio_net_send_copy(struct riscv_virtio_mmio_net *device, uint32_t size,
    int (*copy)(const void *, void *, uint32_t), const void *context);
/* Zero-copy TX: post an indirect table of header plus segments, holding owner
 * until the caller releases it. Needs the negotiated indirect feature. */
int riscv_virtio_mmio_net_send_segments(struct riscv_virtio_mmio_net *device,
    const struct riscv_net_tx_segment *segments, unsigned count, void *owner);
/* Return the number of released owners; abandon also releases posted/pending
 * owners after DMA stopped. tx_capacity_generation also includes copied TX. */
unsigned riscv_virtio_mmio_net_tx_release(struct riscv_virtio_mmio_net *device,
    void (*release)(void *owner), int abandon);
#endif
