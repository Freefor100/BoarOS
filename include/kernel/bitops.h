#ifndef BOAROS_KERNEL_BITOPS_H
#define BOAROS_KERNEL_BITOPS_H

#include <stdint.h>

/* bits must be nonzero. */
static inline uint32_t kernel_first_set_u64(uint64_t bits)
{
#if defined(__riscv) && !defined(__riscv_zbb)
    /* 基础 RV64 的编译器内建操作会调用外部运行库。 */
    uint32_t bit = 0;
    if (!(bits & UINT32_MAX)) { bits >>= 32U; bit += 32U; }
    if (!(bits & UINT16_MAX)) { bits >>= 16U; bit += 16U; }
    if (!(bits & UINT8_MAX)) { bits >>= 8U; bit += 8U; }
    if (!(bits & 0xFU)) { bits >>= 4U; bit += 4U; }
    if (!(bits & 0x3U)) { bits >>= 2U; bit += 2U; }
    return bit + !(bits & 1U);
#else
    return (uint32_t)__builtin_ctzll(bits);
#endif
}

/* bits must be nonzero. */
static inline uint32_t kernel_last_set_u64(uint64_t bits)
{
#if defined(__riscv) && !defined(__riscv_zbb)
    uint32_t bit = 0;
    if (bits >> 32U) { bits >>= 32U; bit += 32U; }
    if (bits >> 16U) { bits >>= 16U; bit += 16U; }
    if (bits >> 8U) { bits >>= 8U; bit += 8U; }
    if (bits >> 4U) { bits >>= 4U; bit += 4U; }
    if (bits >> 2U) { bits >>= 2U; bit += 2U; }
    return bit + (uint32_t)(bits >> 1U);
#else
    return 63U - (uint32_t)__builtin_clzll(bits);
#endif
}

#endif
