#include <kernel/random.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_random_state {
    uint32_t key[8];
    uint32_t nonce[3];
    uint32_t counter;
    uint8_t seeded;
};

static struct kernel_random_state random_state;

static uint32_t load32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void store32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t rotate_left(uint32_t value, uint32_t amount)
{
    return (value << amount) | (value >> (32U - amount));
}

static void quarter_round(uint32_t *a,
                          uint32_t *b,
                          uint32_t *c,
                          uint32_t *d)
{
    *a += *b;
    *d ^= *a;
    *d = rotate_left(*d, 16U);
    *c += *d;
    *b ^= *c;
    *b = rotate_left(*b, 12U);
    *a += *b;
    *d ^= *a;
    *d = rotate_left(*d, 8U);
    *c += *d;
    *b ^= *c;
    *b = rotate_left(*b, 7U);
}

void kernel_random_chacha20_block(
    const uint8_t key[32],
    const uint8_t nonce[12],
    uint32_t counter,
    uint8_t output[64])
{
    static const uint8_t constants[16] = {
        'e', 'x', 'p', 'a', 'n', 'd', ' ', '3',
        '2', '-', 'b', 'y', 't', 'e', ' ', 'k',
    };
    uint32_t state[16];
    uint32_t working[16];
    uint32_t index;

    if (key == 0 || nonce == 0 || output == 0) {
        return;
    }
    state[0] = load32(constants);
    state[1] = load32(constants + 4U);
    state[2] = load32(constants + 8U);
    state[3] = load32(constants + 12U);
    for (index = 0U; index < 8U; index++) {
        state[4U + index] = load32(key + index * 4U);
    }
    state[12] = counter;
    state[13] = load32(nonce);
    state[14] = load32(nonce + 4U);
    state[15] = load32(nonce + 8U);
    for (index = 0U; index < 16U; index++) {
        working[index] = state[index];
    }
    for (index = 0U; index < 10U; index++) {
        quarter_round(&working[0], &working[4], &working[8], &working[12]);
        quarter_round(&working[1], &working[5], &working[9], &working[13]);
        quarter_round(&working[2], &working[6], &working[10], &working[14]);
        quarter_round(&working[3], &working[7], &working[11], &working[15]);
        quarter_round(&working[0], &working[5], &working[10], &working[15]);
        quarter_round(&working[1], &working[6], &working[11], &working[12]);
        quarter_round(&working[2], &working[7], &working[8], &working[13]);
        quarter_round(&working[3], &working[4], &working[9], &working[14]);
    }
    for (index = 0U; index < 16U; index++) {
        store32(output + index * 4U, working[index] + state[index]);
    }
}

static void rekey(const uint8_t block[64])
{
    uint32_t index;

    for (index = 0U; index < 8U; index++) {
        random_state.key[index] = load32(block + index * 4U);
    }
    random_state.counter++;
    if (random_state.counter == 0U) {
        random_state.nonce[0]++;
        if (random_state.nonce[0] == 0U) {
            random_state.nonce[1]++;
            if (random_state.nonce[1] == 0U) {
                random_state.nonce[2]++;
            }
        }
    }
}

enum kernel_random_status kernel_random_initialize(
    const uint8_t *seed,
    size_t seed_size)
{
    uint32_t index;

    if (seed == 0 || seed_size < 32U) {
        random_state.seeded = 0U;
        return KERNEL_RANDOM_STATUS_UNAVAILABLE;
    }
    for (index = 0U; index < 8U; index++) {
        random_state.key[index] = load32(seed + index * 4U);
    }
    random_state.nonce[0] = load32(seed) ^ UINT32_C(0xa5a5a5a5);
    random_state.nonce[1] = load32(seed + 12U) ^ UINT32_C(0x3c6ef372);
    random_state.nonce[2] = load32(seed + 20U) ^ UINT32_C(0x9e3779b9);
    random_state.counter = 1U;
    random_state.seeded = 1U;
    return KERNEL_RANDOM_STATUS_OK;
}

int kernel_random_available(void)
{
    return random_state.seeded != 0U;
}

enum kernel_random_status kernel_random_fill(void *buffer, size_t size)
{
    uint8_t *destination = buffer;
    uint8_t nonce[12];
    uint8_t block[64];
    size_t offset = 0U;
    uint32_t index;

    if (buffer == 0 && size != 0U) {
        return KERNEL_RANDOM_STATUS_INVALID_ARGUMENT;
    }
    if (!random_state.seeded) {
        return size == 0U ? KERNEL_RANDOM_STATUS_OK
                          : KERNEL_RANDOM_STATUS_UNAVAILABLE;
    }
    while (offset < size) {
        store32(nonce, random_state.nonce[0]);
        store32(nonce + 4U, random_state.nonce[1]);
        store32(nonce + 8U, random_state.nonce[2]);
        kernel_random_chacha20_block((const uint8_t *)random_state.key,
                                     nonce,
                                     random_state.counter,
                                     block);
        for (index = 0U; index < 64U && offset < size; index++, offset++) {
            destination[offset] = block[index];
        }
        rekey(block);
    }
    return KERNEL_RANDOM_STATUS_OK;
}
