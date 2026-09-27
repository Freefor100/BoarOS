#include <kernel/random.h>
#include <kernel/time.h>

#include "lwip/sys.h"

#include <stdint.h>

u32_t sys_now(void)
{
    return (u32_t)(kernel_time_monotonic_ns() / UINT64_C(1000000));
}

unsigned int boaros_lwip_random(void)
{
    uint32_t value;
    static uint32_t fallback = UINT32_C(0x9e3779b9);

    if (kernel_random_available() &&
        kernel_random_fill(&value, sizeof(value)) == KERNEL_RANDOM_STATUS_OK) {
        return value;
    }
    /* Early boot and host fixtures can lack a supplied entropy seed. */
    fallback ^= (uint32_t)kernel_time_monotonic_ns();
    fallback ^= fallback << 13;
    fallback ^= fallback >> 17;
    fallback ^= fallback << 5;
    return fallback;
}

int atoi(const char *text)
{
    unsigned int value = 0U;

    if (text == 0) {
        return 0;
    }
    while (*text >= '0' && *text <= '9') {
        unsigned int digit = (unsigned int)(*text - '0');
        if (value > (unsigned int)(__INT_MAX__ - (int)digit) / 10U) {
            return __INT_MAX__;
        }
        value = value * 10U + digit;
        text++;
    }
    return (int)value;
}
