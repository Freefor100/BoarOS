#ifndef BOAROS_KERNEL_IGB_H
#define BOAROS_KERNEL_IGB_H

#include <stdint.h>

struct kernel_heap;
struct kernel_igb_rx;
struct pci_host;
struct physical_page_allocator;
struct kernel_wait_queue;

enum {
    KERNEL_IGB_RX_DEPTH = 32,
    KERNEL_IGB_RX_HEADER = 256,
    KERNEL_IGB_RX_PAYLOAD = 2048,
};

struct kernel_igb_rx_buffer {
    uint64_t physical, header_physical;
    void *owner;
};

struct kernel_igb_rx_packet {
    void *owner;
    uint16_t header_bytes, payload_bytes, vlan;
    uint8_t split, end;
    int error;
};

/* Takes exclusive custody of an otherwise unused 82576 PCI function.
 * The caller serializes open and destroy against other PCI configuration. */
int kernel_igb_rx_open(struct kernel_igb_rx **, struct pci_host *, uint16_t bdf,
    struct kernel_heap *, struct physical_page_allocator *, uint32_t timebase);
int kernel_igb_rx_link_up(const struct kernel_igb_rx *);
void kernel_igb_mac_address(const struct kernel_igb_rx *, uint8_t address[6]);
int kernel_igb_open_optional(struct kernel_igb_rx **, struct pci_host *,
    struct kernel_heap *, struct physical_page_allocator *, uint32_t timebase);
struct kernel_wait_queue *kernel_igb_progress(struct kernel_igb_rx *);
int kernel_igb_irq_available(const struct kernel_igb_rx *);
int kernel_igb_pending(struct kernel_igb_rx *, int may_refill);

/* Accepted buffers require stable, writable DMA references for a 2 KiB-aligned
 * payload and 256-byte-aligned header. The caller regains custody through reap. */
unsigned kernel_igb_rx_post(struct kernel_igb_rx *,
    const struct kernel_igb_rx_buffer *, unsigned count);
unsigned kernel_igb_rx_reap(struct kernel_igb_rx *,
    struct kernel_igb_rx_packet *, unsigned max);

/* A second queue is selected by its own exact unicast destination address.
 * Its address must be locally administered and distinct from the main port. */
int kernel_igb_zc_bind(struct kernel_igb_rx *, const uint8_t address[6]);
int kernel_igb_zc_stop(struct kernel_igb_rx *);
unsigned kernel_igb_zc_post(struct kernel_igb_rx *,
    const struct kernel_igb_rx_buffer *, unsigned count);
unsigned kernel_igb_zc_reap(struct kernel_igb_rx *,
    struct kernel_igb_rx_packet *, unsigned max);
unsigned kernel_igb_zc_space(struct kernel_igb_rx *);
int kernel_igb_zc_pending(struct kernel_igb_rx *);
int kernel_igb_tx_copy(struct kernel_igb_rx *, unsigned length,
    int (*copy)(const void *, void *, uint32_t), const void *, int more);
void kernel_igb_tx_flush(struct kernel_igb_rx *);
unsigned kernel_igb_tx_reap(struct kernel_igb_rx *);

/* Stop first, reap outstanding buffers (ECANCELED), then destroy. A failed
 * stop retains ownership of all unreaped DMA buffers and can be retried. */
int kernel_igb_rx_stop(struct kernel_igb_rx *);
int kernel_igb_rx_destroy(struct kernel_igb_rx *);

#endif
