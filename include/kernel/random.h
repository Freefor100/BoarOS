#ifndef BOAROS_KERNEL_RANDOM_H
#define BOAROS_KERNEL_RANDOM_H

#include <stddef.h>
#include <stdint.h>

enum kernel_random_status {
    KERNEL_RANDOM_STATUS_OK = 0,
    KERNEL_RANDOM_STATUS_INVALID_ARGUMENT,
    KERNEL_RANDOM_STATUS_UNAVAILABLE,
    KERNEL_RANDOM_STATUS_STATE,
};

enum kernel_random_status kernel_random_initialize(
    const uint8_t *seed,
    size_t seed_size);

/* Material availability is not a trust claim. fill is an insecure fallback
 * before ready, for early boot ASLR and GRND_INSECURE only. */
int kernel_random_available(void);
int kernel_random_ready(void);
unsigned kernel_random_credited_bits(void);
/* Only successful trusted hardware RNG bytes may be credited. IRQ-safe. */
void kernel_random_mix(const void *data, size_t size, int trusted);
void kernel_random_erase(void *buffer, size_t size);
struct kernel_wait_queue;
struct kernel_wait_queue *kernel_random_wait_queue(void);
int kernel_random_wait_ready(int nonblock);

enum kernel_random_status kernel_random_fill(
    void *buffer,
    size_t size);

/* RFC 8439 ChaCha20 block primitive, useful for architecture-free checks. */
void kernel_random_chacha20_block(
    const uint8_t key[32],
    const uint8_t nonce[12],
    uint32_t counter,
    uint8_t output[64]);

#endif
