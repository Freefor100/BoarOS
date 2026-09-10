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

int kernel_random_available(void);

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
