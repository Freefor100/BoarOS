#include "packet_ring.h"

#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/socket.h>

#include <stddef.h>
#include <string.h>

#define PACKET_RING_BYTES_MAX (64U * 1024 * 1024)
#define PACKET_RING_DEFAULT_TIMEOUT_MS 64U
#define PACKET_ALIGN 16U
#define PACKET_V3_ALIGN 8U
#define PACKET_STATUS_USER 1U
#define PACKET_STATUS_TIMEOUT 32U
#define PACKET_STATUS_SOFTWARE (1U << 29)

struct packet_timestamp { uint32_t seconds, nanoseconds; };
struct packet_block_desc {
    uint32_t version, private_offset;
    uint32_t status, count, first, length;
    uint64_t sequence;
    struct packet_timestamp opened, last;
};
struct packet_record {
    uint32_t next, seconds, nanoseconds, captured, length, status;
    uint16_t mac, network;
    uint32_t rx_hash, vlan_tci;
    uint16_t vlan_tpid, padding;
    uint8_t reserved[8];
};
struct packet_ll_address {
    uint16_t family, protocol;
    uint32_t index;
    uint16_t hardware;
    uint8_t type, length, address[8];
};
_Static_assert(sizeof(struct packet_block_desc) == 48, "TPACKET_V3 block header");
_Static_assert(offsetof(struct packet_block_desc, status) == 8, "TPACKET_V3 block ownership");
_Static_assert(sizeof(struct packet_record) == 48, "TPACKET_V3 packet header");
_Static_assert(sizeof(struct packet_ll_address) == 20, "sockaddr_ll layout");
_Static_assert(sizeof(struct packet_record) + sizeof(struct packet_ll_address) ==
    PACKET_V3_HEADER_LENGTH, "TPACKET_V3 packet header and address");

struct packet_ring_page { uint64_t physical; unsigned char *address; };
struct packet_ring {
    struct kernel_heap *heap;
    uint64_t mapped_bytes, sequence;
    uint32_t block_size, blocks, first, reserve, timeout_ms;
    uint32_t producer, cursor, last, packets, deadline;
    uint32_t latest; /* One-based block index, published for poll observers. */
    unsigned frozen;
    struct packet_timestamp last_stamp;
    uint32_t page_count;
    struct packet_ring_page pages[];
};

static uint64_t align_up(uint64_t value, uint64_t alignment)
{ return (value + alignment - 1) & ~(alignment - 1); }

int packet_ring_create(struct kernel_heap *heap,
    const struct kernel_socket_packet_ring_request *request, uint32_t reserve,
    struct packet_ring **owner)
{
    uint32_t size = request->block_size, blocks = request->block_count;
    uint32_t frame = request->frame_size;
    uint64_t first = sizeof(struct packet_block_desc) +
        align_up(request->private_bytes, PACKET_V3_ALIGN);
    uint64_t bytes = (uint64_t)size * blocks;
    if (!size || !blocks || (size & BOAROS_PAGE_MASK) ||
        (frame & (PACKET_ALIGN - 1)) || reserve>UINT16_MAX-96 ||
        frame < PACKET_V3_HEADER_LENGTH + reserve ||
        first + PACKET_V3_HEADER_LENGTH + reserve > size ||
        (uint64_t)(size / frame) * blocks != request->frame_count ||
        frame > size || request->timeout_ms > INT32_MAX)
        return -KERNEL_EINVAL;
    if (bytes > PACKET_RING_BYTES_MAX) return -KERNEL_ENOMEM;
    if (request->features) return -KERNEL_EOPNOTSUPP;

    uint32_t count = (uint32_t)(bytes >> BOAROS_PAGE_SHIFT);
    struct packet_ring *ring;
    if (kernel_heap_allocate_zeroed(heap, 1, sizeof(*ring) +
            (size_t)count * sizeof(*ring->pages), (void **)&ring))
        return -KERNEL_ENOMEM;
    ring->heap = heap;
    ring->block_size = size;
    ring->blocks = blocks;
    ring->first = (uint32_t)first;
    ring->reserve = reserve;
    ring->timeout_ms = request->timeout_ms ? request->timeout_ms : PACKET_RING_DEFAULT_TIMEOUT_MS;
    while (ring->page_count < count) {
        struct packet_ring_page *page = &ring->pages[ring->page_count];
        if (physical_page_allocate(heap->page_allocator, &page->physical)) {
            packet_ring_destroy(ring);
            return -KERNEL_ENOMEM;
        }
        ring->page_count++;
        void *address;
        if (physical_page_resolve(heap->page_allocator, page->physical, &address))
            __builtin_trap();
        page->address = address;
        memset(address, 0, BOAROS_PAGE_SIZE);
    }
    *owner = ring;
    return 0;
}

void packet_ring_destroy(struct packet_ring *ring)
{
    if (!ring) return;
    if (__atomic_load_n(&ring->mapped_bytes, __ATOMIC_ACQUIRE)) __builtin_trap();
    while (ring->page_count) {
        if (physical_page_release(ring->heap->page_allocator,
                ring->pages[--ring->page_count].physical)) __builtin_trap();
    }
    kernel_heap_release(ring->heap, ring);
}

uint64_t *packet_ring_map_count(struct packet_ring *ring)
{ return &ring->mapped_bytes; }

uint64_t packet_ring_bytes(const struct packet_ring *ring)
{ return (uint64_t)ring->block_size * ring->blocks; }

int packet_ring_page(const struct packet_ring *ring, uint64_t offset, uint64_t *physical)
{
    if ((offset & BOAROS_PAGE_MASK) || offset >= packet_ring_bytes(ring))
        return -KERNEL_EINVAL;
    uint64_t page = ring->pages[offset >> BOAROS_PAGE_SHIFT].physical;
    if (physical_page_acquire(ring->heap->page_allocator, page))
        return -KERNEL_ENOMEM;
    *physical = page;
    return 0;
}

static unsigned char *ring_address(const struct packet_ring *ring, uint32_t offset)
{
    return ring->pages[offset >> BOAROS_PAGE_SHIFT].address + (offset & BOAROS_PAGE_MASK);
}

static void ring_store(struct packet_ring *ring, uint32_t offset,
    const void *source, uint32_t length)
{
    const unsigned char *bytes = source;
    while (length) {
        uint32_t chunk = BOAROS_PAGE_SIZE - (offset & BOAROS_PAGE_MASK);
        if (chunk > length) chunk = length;
        memcpy(ring_address(ring, offset), bytes, chunk);
        bytes += chunk;
        offset += chunk;
        length -= chunk;
    }
}

static void ring_copy_payload(struct packet_ring *ring, uint32_t offset, uint32_t length,
    int (*read)(void *, int64_t, void *, uint32_t), void *source)
{
    for (uint32_t done = 0; done < length;) {
        uint32_t chunk = BOAROS_PAGE_SIZE - (offset & BOAROS_PAGE_MASK);
        if (chunk > length - done) chunk = length - done;
        if (read(source, done, ring_address(ring, offset), chunk)) __builtin_trap();
        offset += chunk;
        done += chunk;
    }
}

static struct packet_block_desc *block_desc(const struct packet_ring *ring, uint32_t index)
{ return (void *)ring_address(ring, index * ring->block_size); }

static void publish_block(struct packet_ring *ring, uint32_t status)
{
    uint32_t index = ring->producer;
    struct packet_block_desc *block = block_desc(ring, index);
    const uint32_t end = 0;
    ring_store(ring, index * ring->block_size + ring->last, &end, sizeof(end));
    block->count = ring->packets;
    block->length = ring->cursor;
    block->last = ring->last_stamp;
    /* Payload and descriptor bytes belong to userspace only after this store. */
    __atomic_store_n(&block->status, PACKET_STATUS_USER | status, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->latest, index + 1, __ATOMIC_RELEASE);
    ring->producer = index + 1 == ring->blocks ? 0 : index + 1;
    ring->packets = 0;
    ring->cursor = ring->first;
}

unsigned packet_ring_receive(struct packet_ring *ring,
    const struct kernel_socket_address *address, uint32_t original_length, uint32_t snaplen,
    uint32_t mac_length, int (*read)(void *, int64_t, void *, uint32_t), void *source,
    uint32_t now_ms, uint64_t timestamp_ns)
{
    uint32_t network = (uint32_t)align_up(PACKET_V3_HEADER_LENGTH +
        (mac_length > PACKET_ALIGN ? mac_length : PACKET_ALIGN), PACKET_ALIGN) + ring->reserve;
    uint32_t mac = network - mac_length;
    if (network > UINT16_MAX || mac > ring->block_size - ring->first) return 0;
    uint32_t cap = ring->block_size - ring->first - mac;
    if (snaplen > cap) snaplen = cap;
    uint32_t stride = (uint32_t)align_up(mac + snaplen, PACKET_V3_ALIGN);
    unsigned events = 0;
    if (ring->packets && stride > ring->block_size - ring->cursor) {
        publish_block(ring, 0);
        events |= PACKET_RING_PUBLISHED;
    }
    struct packet_timestamp stamp = {(uint32_t)(timestamp_ns / UINT64_C(1000000000)),
        (uint32_t)(timestamp_ns % UINT64_C(1000000000))};
    if (!ring->packets) {
        struct packet_block_desc *block = block_desc(ring, ring->producer);
        if (__atomic_load_n(&block->status, __ATOMIC_ACQUIRE)) {
            if (!ring->frozen) events |= PACKET_RING_FROZEN;
            ring->frozen = 1;
            return events;
        }
        ring->frozen = 0;
        ring->cursor = ring->first;
        ring->deadline = now_ms + ring->timeout_ms;
        block->version = PACKET_VERSION_V3;
        block->private_offset = sizeof(*block);
        block->first = ring->first;
        block->sequence = ++ring->sequence;
        block->opened = stamp;
    }
    uint32_t base = ring->producer * ring->block_size + ring->cursor;
    struct packet_record record = {.next = stride, .seconds = stamp.seconds,
        .nanoseconds = stamp.nanoseconds, .captured = snaplen, .length = original_length,
        .status = PACKET_STATUS_USER | PACKET_STATUS_SOFTWARE,
        .mac = (uint16_t)mac, .network = (uint16_t)network};
    struct packet_ll_address ll = {.family = address->family,
        .protocol = __builtin_bswap16(address->port), .index = address->scope,
        .hardware = address->link_type, .type = address->link_packet_type,
        .length = address->link_length};
    memcpy(ll.address, address->link_address, sizeof(ll.address));
    ring_store(ring, base, &record, sizeof(record));
    ring_store(ring, base + sizeof(record), &ll, sizeof(ll));
    ring_copy_payload(ring, base + mac, snaplen, read, source);
    ring->last = ring->cursor;
    ring->last_stamp = stamp;
    ring->cursor += stride;
    ring->packets++;
    return events | PACKET_RING_RECEIVED;
}

int packet_ring_retire(struct packet_ring *ring, uint32_t now_ms)
{
    if (!ring->packets || (int32_t)(now_ms - ring->deadline) < 0) return 0;
    publish_block(ring, PACKET_STATUS_TIMEOUT);
    return 1;
}

int packet_ring_deadline(const struct packet_ring *ring, uint32_t *deadline)
{
    if (!ring->packets) return 0;
    *deadline = ring->deadline;
    return 1;
}

int packet_ring_poll(const struct packet_ring *ring)
{
    uint32_t block = __atomic_load_n(&ring->latest, __ATOMIC_ACQUIRE);
    return block && (__atomic_load_n(&block_desc(ring, block - 1)->status,
        __ATOMIC_ACQUIRE) & PACKET_STATUS_USER);
}
