#ifndef BOAROS_PACKET_RING_H
#define BOAROS_PACKET_RING_H

#include <stdint.h>

struct kernel_heap;
struct packet_ring;
struct kernel_socket_address;
struct kernel_socket_packet_ring_request;

/* Wire layout constants for V3 packet records. */
#define PACKET_VERSION_V3 2U
#define PACKET_V3_HEADER_LENGTH 68U

enum packet_ring_result {
    PACKET_RING_RECEIVED = 1,
    PACKET_RING_PUBLISHED = 2,
    PACKET_RING_FROZEN = 4,
};

int packet_ring_create(struct kernel_heap *,
    const struct kernel_socket_packet_ring_request *, uint32_t reserve,
    struct packet_ring **owner);
void packet_ring_destroy(struct packet_ring *);
uint64_t *packet_ring_map_count(struct packet_ring *);
uint64_t packet_ring_bytes(const struct packet_ring *);
int packet_ring_page(const struct packet_ring *, uint64_t offset, uint64_t *physical);

/* Producer operations are serialized by the IP qualification. Only the user
 * ownership word is read from mapped storage. The cursor and bounds are private. */
unsigned packet_ring_receive(struct packet_ring *,
    const struct kernel_socket_address *, uint32_t original_length, uint32_t snaplen,
    uint32_t mac_length, int (*read)(void *, int64_t, void *, uint32_t), void *,
    uint32_t now_ms, uint64_t timestamp_ns);
int packet_ring_retire(struct packet_ring *, uint32_t now_ms);
int packet_ring_deadline(const struct packet_ring *, uint32_t *deadline);
int packet_ring_poll(const struct packet_ring *);

#endif
