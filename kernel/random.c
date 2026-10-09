#include <kernel/random.h>

#include <arch/context.h>
#include <kernel/blake2s.h>
#include <kernel/scheduler.h>
#include <kernel/errno.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

struct kernel_random_state {
    uint32_t key[8];
    uint8_t seeded;
    uint8_t credited;
};

static struct kernel_random_state random_state;
static struct kernel_wait_queue ready_queue;

void kernel_random_erase(void *buffer, size_t size)
{
    volatile uint8_t *p = buffer;
    while (size--) *p++ = 0;
}


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
    kernel_random_erase(state, sizeof(state));
    kernel_random_erase(working, sizeof(working));
}


struct kernel_wait_queue *kernel_random_wait_queue(void)
{
    if (ready_queue.initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        kernel_wait_queue_init(&ready_queue);
    return &ready_queue;
}

unsigned kernel_random_credited_bits(void) { return random_state.credited * 8U; }
int kernel_random_ready(void) { return random_state.credited == 32U; }
int kernel_random_available(void) { return random_state.seeded != 0U; }

void kernel_random_mix(const void *data, size_t size, int trusted)
{
    const uint8_t *p = data;
    uint8_t input[64];
    if (!data) return;
    while (size) {
        size_t n = size > 32U ? 32U : size;
        uintptr_t irq = arch_interrupt_save();
        int was_ready = kernel_random_ready();
        memcpy(input, random_state.key, 32U);
        memcpy(input + 32U, p, n);
        /* 有界压缩在关中断区完成，避免 IRQ 混种被旧快照覆盖。 */
        kernel_blake2s(input, 32U + n, (uint8_t *)random_state.key);
        random_state.seeded = 1U;
        if (trusted && !was_ready) {
            size_t remaining = 32U - random_state.credited;
            random_state.credited += (uint8_t)(n < remaining ? n : remaining);
        }
        if (!was_ready && kernel_random_ready())
            (void)kernel_wait_queue_wake_all(kernel_random_wait_queue());
        arch_interrupt_restore(irq);
        p += n;
        size -= n;
    }
    kernel_random_erase(input, sizeof(input));
}

enum kernel_random_status kernel_random_initialize(const uint8_t *seed,
                                                   size_t seed_size)
{
    if (!seed || seed_size < 32U) return KERNEL_RANDOM_STATUS_UNAVAILABLE;
    kernel_random_mix(seed, seed_size, 0);
    return KERNEL_RANDOM_STATUS_OK;
}

int kernel_random_wait_ready(int nonblock)
{
    uintptr_t irq = arch_interrupt_save();
    while (!kernel_random_ready()) {
        enum kernel_wait_wake_reason reason;
        if (nonblock) { arch_interrupt_restore(irq); return -KERNEL_EAGAIN; }
        if (KERNEL_WAIT_RECHECK(kernel_random_wait_queue(), 0, 1,
                                          &reason,
                (!kernel_random_ready())) != KERNEL_SCHEDULER_STATUS_OK) {
            arch_interrupt_restore(irq); return -KERNEL_EIO;
        }
        if (reason == KERNEL_WAIT_SIGNALLED) {
            arch_interrupt_restore(irq); return -KERNEL_EINTR;
        }
    }
    arch_interrupt_restore(irq);
    return 0;
}

enum kernel_random_status kernel_random_fill(void *buffer, size_t size)
{
    uint8_t key[32], block[64], nonce[12] = {0};
    uint8_t *p = buffer;
    uint32_t counter = 1U;
    if (!buffer && size) return KERNEL_RANDOM_STATUS_INVALID_ARGUMENT;
    if (!size) return KERNEL_RANDOM_STATUS_OK;
    uintptr_t irq = arch_interrupt_save();
    memcpy(key, random_state.key, sizeof(key));
    kernel_random_chacha20_block(key, nonce, 0U, block);
    /* 首半块仅作为新全局 key；调用者只能看到后半块和旧 key 的独立流。 */
    memcpy(random_state.key, block, 32U);
    arch_interrupt_restore(irq);
    size_t n = size > 32U ? 32U : size;
    memcpy(p, block + 32U, n);
    p += n; size -= n;
    while (size) {
        kernel_random_chacha20_block(key, nonce, counter++, block);
        if (!counter) {
            for (unsigned i = 0; i < sizeof(nonce); i++)
                if (++nonce[i]) break;
        }
        n = size > 64U ? 64U : size;
        memcpy(p, block, n); p += n; size -= n;
    }
    kernel_random_erase(key, sizeof(key));
    kernel_random_erase(block, sizeof(block));
    return KERNEL_RANDOM_STATUS_OK;
}
