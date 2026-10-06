#ifndef BOAROS_KERNEL_VIRTIO_NET_H
#define BOAROS_KERNEL_VIRTIO_NET_H
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/virtio_split_queue.h>
#include <stdint.h>

#define VIRTIO_NET_QUEUE_SIZE 32U
#define VIRTIO_NET_BUFFERS 64U
#define VIRTIO_NET_BUFFER_SIZE 2048U
#define VIRTIO_NET_TX_SEGMENTS 2U
#define VIRTIO_NET_TX_TABLE_OFFSET 64U

struct virtio_net_frame { void *data; uint32_t size, buffer; };
struct virtio_net_tx_segment { uint64_t physical_address; uint32_t length; };
struct virtio_net_statistics {
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes, drops, errors, interrupts;
    uint64_t loan_packets, loan_bytes, loan_peak, copied_packets, copied_bytes;
    uint64_t tx_sg_packets, tx_copy_packets;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t tx_done_free_count, tx_done_free_ticks, tx_done_free_max;
    uint64_t tx_free_post_count, tx_free_post_ticks, tx_free_post_max;
    uint64_t tx_latency_overflow;
#endif
};
struct virtio_net_device {
    struct virtio_transport transport;
    struct virtio_split_queue split[2];
    struct physical_page_allocator *allocator;
    void *queues, *rx_memory, *tx_memory;
    uint64_t queue_phys, rx_phys, tx_phys, frequency;
    struct kernel_wait_queue progress;
    struct virtio_net_statistics statistics;
    uint64_t tx_time[VIRTIO_NET_BUFFERS];
#if BOAROS_COST_DIAGNOSTICS
    /* 只记录软件收割完成；未使用槽及 reset 撤销不能伪造 DMA 完成样本。 */
    uint64_t tx_done_time[VIRTIO_NET_BUFFERS], tx_free_time[VIRTIO_NET_BUFFERS];
    uint64_t tx_done_valid, tx_free_valid;
#endif
    uint64_t tx_capacity_generation;
    uint32_t version, queue_order, buffer_order, irq_source, feature_low;
    uint32_t rx_posted, tx_posted;
    uint16_t rx_free[VIRTIO_NET_BUFFERS], rx_ready[VIRTIO_NET_BUFFERS];
    uint16_t tx_pending[VIRTIO_NET_BUFFERS];
    uint16_t rx_head, rx_count, ready_head, ready_count, tx_head, tx_count;
    uint16_t rx_map[VIRTIO_NET_QUEUE_SIZE], tx_map[VIRTIO_NET_QUEUE_SIZE];
    uint16_t rx_length[VIRTIO_NET_BUFFERS];
    uint8_t rx_state[VIRTIO_NET_BUFFERS], tx_state[VIRTIO_NET_BUFFERS];
    uint8_t mac[6], link_up, configured, irq_registered, stopping, failed;
    uint16_t loaned;
    /* 零拷贝 TX：每槽持有调用方 owner 与 indirect 表长度；完成后经 tx_release 归还。 */
    void *tx_owner[VIRTIO_NET_BUFFERS];
    uint16_t tx_table[VIRTIO_NET_BUFFERS];
    uint16_t tx_done[VIRTIO_NET_BUFFERS];
    uint16_t tx_done_head, tx_done_count;
};

int virtio_net_init(struct virtio_net_device *device,
    const struct virtio_transport *transport, struct physical_page_allocator *allocator,
    uint64_t frequency, uint32_t source);
/* Platform owns device/transport. Network joins its worker and returns loans/
 * TX owners after quiesce, then platform stops device and destroys transport. */
int virtio_net_quiesce(struct virtio_net_device *device);
int virtio_net_stop(struct virtio_net_device *device);
int virtio_net_service(struct virtio_net_device *device);
int virtio_net_receive(struct virtio_net_device *device, struct virtio_net_frame *frame);
int virtio_net_lend(struct virtio_net_device *device, unsigned buffer);
void virtio_net_release(struct virtio_net_device *device, unsigned buffer);
int virtio_net_send(struct virtio_net_device *device, const void *data, uint32_t size);
int virtio_net_send_copy(struct virtio_net_device *device, uint32_t size,
    int (*copy)(const void *, void *, uint32_t), const void *context);
/* Zero-copy TX: post an indirect table of header plus segments, holding owner
 * until the caller releases it. Needs the negotiated indirect feature. */
int virtio_net_send_segments(struct virtio_net_device *device,
    const struct virtio_net_tx_segment *segments, unsigned count, void *owner);
/* Return the number of released owners; abandon also releases posted/pending
 * owners after DMA stopped. tx_capacity_generation also includes copied TX. */
unsigned virtio_net_tx_release(struct virtio_net_device *device,
    void (*release)(void *owner), int abandon);
#endif
