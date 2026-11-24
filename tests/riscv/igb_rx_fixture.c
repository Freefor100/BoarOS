#include <arch/timer.h>
#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/igb.h>
#include <kernel/nvme.h>
#include <kernel/pci.h>
#include <kernel/physical_page.h>
#include <string.h>

struct receive_page {
    uint64_t physical;
    uint8_t *data;
};

static struct receive_page pages[KERNEL_IGB_RX_DEPTH];
static struct kernel_igb_rx_buffer batch[KERNEL_IGB_RX_DEPTH];

static void message(const char *s)
{
    while (*s) kernel_console_putc(*s++);
}

static int transmit_copy(const void *context, void *buffer, uint32_t length)
{
    memcpy(buffer, context, length);
    return 1;
}

int __real_kernel_nvme_bus_start(struct pci_host *, struct kernel_heap *,
    struct physical_page_allocator *, uint32_t);

int __wrap_kernel_nvme_bus_start(struct pci_host *host, struct kernel_heap *heap,
    struct physical_page_allocator *allocator, uint32_t timebase)
{
    enum { FRAMES = 40, BYTES = 512, HEADER = 42 };
    struct kernel_igb_rx *rx = NULL;
    unsigned prior_claims = pci_host_claimed(host);
    uint64_t prior_pages = allocator->available_pages;
    if (kernel_igb_rx_open(&rx, host, 4U << 3, heap, allocator, timebase)) {
        message("IGB OPEN FAIL\n");
        return -KERNEL_EIO;
    }
    for (unsigned i = 0; i < KERNEL_IGB_RX_DEPTH; i++) {
        if (physical_page_allocate(allocator, &pages[i].physical) ||
            physical_page_map_owned(allocator, pages[i].physical, (void **)&pages[i].data))
            return -KERNEL_ENOMEM;
        memset(pages[i].data, 0xa5, BOAROS_PAGE_SIZE);
        batch[i] = (struct kernel_igb_rx_buffer){
            pages[i].physical, pages[i].physical + KERNEL_IGB_RX_PAYLOAD, &pages[i],
        };
    }
    if (kernel_igb_rx_post(rx, batch, KERNEL_IGB_RX_DEPTH) != KERNEL_IGB_RX_DEPTH - 1 ||
        kernel_igb_rx_destroy(rx) != -KERNEL_EBUSY) return -KERNEL_EINVAL;
    uint64_t negotiating = arch_time_read_ordered();
    while (!kernel_igb_rx_link_up(rx)) {
        if (arch_time_read_ordered() - negotiating > 5ULL * timebase) {
            message("IGB LINK TIMEOUT\n");
            return -KERNEL_ETIMEDOUT;
        }
    }
    uint8_t frame[64];
    const uint8_t prefix[] = {0x52, 0x54, 0, 1, 2, 3, 0x52, 0x54, 0, 0x12, 0x34, 0x56, 0x88, 0xb5};
    memcpy(frame, prefix, sizeof(prefix));
    kernel_igb_mac_address(rx, frame + 6);
    memset(frame + sizeof(prefix), 0xab, sizeof(frame) - sizeof(prefix));
    if (kernel_igb_tx_copy(rx, sizeof(frame), transmit_copy, frame, 1)) return -KERNEL_EIO;
    memset(frame + sizeof(prefix), 0xcd, sizeof(frame) - sizeof(prefix));
    if (kernel_igb_tx_copy(rx, sizeof(frame), transmit_copy, frame, 1) ||
        kernel_igb_tx_reap(rx)) return -KERNEL_EIO;
    kernel_igb_tx_flush(rx);
    message("IGB TRANSMIT\n");
    unsigned sent = 0;
    uint64_t transmitting = arch_time_read_ordered();
    while (sent < 2) {
        sent += kernel_igb_tx_reap(rx);
        if (arch_time_read_ordered() - transmitting > 5ULL * timebase) {
            message("IGB TX TIMEOUT\n");
            return -KERNEL_ETIMEDOUT;
        }
    }
    for (unsigned i = 0; i < FRAMES; i++) {
        message("IGB SEND\n");
        struct kernel_igb_rx_packet packet;
        uint64_t started = arch_time_read_ordered();
        while (!kernel_igb_rx_reap(rx, &packet, 1)) {
            if (arch_time_read_ordered() - started > 5ULL * timebase) {
                message("IGB REAP TIMEOUT\n");
                return -KERNEL_ETIMEDOUT;
            }
        }
        struct receive_page *page = packet.owner;
        uint8_t *header = page->data + KERNEL_IGB_RX_PAYLOAD;
        if (packet.error || !packet.end || !packet.split || packet.header_bytes != HEADER ||
            packet.payload_bytes != BYTES || header[12] != 8 || header[23] != 17) {
            message("IGB SPLIT FAIL\n");
            return -KERNEL_EILSEQ;
        }
        for (unsigned offset = 0; offset < BYTES; offset++) {
            if (page->data[offset] != (uint8_t)(i + 5 * offset)) {
                message("IGB DMA DATA FAIL\n");
                return -KERNEL_EILSEQ;
            }
        }
        for (unsigned offset = BYTES; offset < KERNEL_IGB_RX_PAYLOAD; offset++) {
            if (page->data[offset] != 0xa5) {
                message("IGB DMA BOUND FAIL\n");
                return -KERNEL_EILSEQ;
            }
        }
        for (unsigned offset = HEADER; offset < KERNEL_IGB_RX_HEADER; offset++) {
            if (header[offset] != 0xa5) return -KERNEL_EILSEQ;
        }
        if (kernel_igb_rx_post(rx, &batch[page - pages], 1) != 1)
            return -KERNEL_EIO;
    }
    const uint8_t isolated_mac[] = {0x52, 0x54, 0, 0x12, 0x34, 0x57};
    unsigned separate = KERNEL_IGB_RX_DEPTH - 1;
    if (kernel_igb_zc_bind(rx, isolated_mac) ||
        kernel_igb_zc_post(rx, &batch[separate], 1) != 1) return -KERNEL_EIO;
    message("IGB SELECT\n");
    struct kernel_igb_rx_packet packet;
    uint64_t selecting = arch_time_read_ordered();
    while (!kernel_igb_zc_reap(rx, &packet, 1)) {
        if (arch_time_read_ordered() - selecting > 5ULL * timebase) {
            message("IGB ISOLATION TIMEOUT\n");
            return -KERNEL_ETIMEDOUT;
        }
    }
    if (packet.error || !packet.split || packet.owner != &pages[separate] ||
        pages[separate].data[0] != 43 || packet.payload_bytes != BYTES) {
        message("IGB ISOLATION FAIL\n");
        return -KERNEL_EILSEQ;
    }
    for (unsigned i = 40; i < 42; i++) {
        if (kernel_igb_rx_reap(rx, &packet, 1) != 1 || packet.error ||
            ((struct receive_page *)packet.owner)->data[0] != i) return -KERNEL_EILSEQ;
        if (kernel_igb_rx_post(rx, &batch[(struct receive_page *)packet.owner - pages], 1) != 1)
            return -KERNEL_EIO;
    }
    if (kernel_igb_rx_reap(rx, &packet, 1) || kernel_igb_zc_reap(rx, &packet, 1) ||
        kernel_igb_zc_post(rx, &batch[separate], 1) != 1 ||
        kernel_igb_zc_stop(rx) || kernel_igb_zc_reap(rx, &packet, 1) != 1 ||
        packet.error != -KERNEL_ECANCELED || packet.owner != &pages[separate])
        return -KERNEL_EIO;
    message("IGB ISOLATION PASS\n");
    if (kernel_igb_rx_stop(rx) || kernel_igb_rx_destroy(rx) != -KERNEL_EBUSY ||
        kernel_igb_rx_post(rx, batch, 1)) return -KERNEL_EIO;
    unsigned returned = 0;
    while (kernel_igb_rx_reap(rx, &packet, 1)) {
        if (!packet.owner || packet.error != -KERNEL_ECANCELED) return -KERNEL_EIO;
        returned++;
    }
    if (returned != KERNEL_IGB_RX_DEPTH - 1 || kernel_igb_rx_destroy(rx) ||
        pci_host_claimed(host) != prior_claims) return -KERNEL_EIO;
    for (unsigned i = 0; i < KERNEL_IGB_RX_DEPTH; i++) {
        if (physical_page_release(allocator, pages[i].physical)) return -KERNEL_EIO;
    }
    if (allocator->available_pages != prior_pages) return -KERNEL_EIO;
    message("IGB DMA PASS\n");
    return __real_kernel_nvme_bus_start(host, heap, allocator, timebase);
}
