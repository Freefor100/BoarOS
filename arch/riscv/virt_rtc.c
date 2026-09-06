#include <arch/riscv/virt_rtc.h>

#include <stdint.h>

/* Goldfish RTC registers: reading TIME_LOW latches TIME_HIGH. */
#define VIRT_RTC_TIME_LOW 0x00U
#define VIRT_RTC_TIME_HIGH 0x04U

/* Samples must be plausible wall clock: after 2020-01-01, within 1s apart. */
#define VIRT_RTC_MINIMUM_NS UINT64_C(1577836800000000000)
#define VIRT_RTC_MAXIMUM_DELTA_NS UINT64_C(1000000000)

static uint64_t virt_rtc_sample(void)
{
    volatile uint32_t *base = (volatile uint32_t *)VIRT_RTC_MMIO_KERNEL_BASE;
    uint64_t time_low = base[VIRT_RTC_TIME_LOW / 4U];
    uint64_t time_high = base[VIRT_RTC_TIME_HIGH / 4U];

    return (time_high << 32) | time_low;
}

enum riscv_virt_rtc_status riscv_virt_rtc_read_ns(uint64_t *nanoseconds)
{
    uint64_t first;
    uint64_t second;

    if (nanoseconds == 0) {
        return RISCV_VIRT_RTC_STATUS_UNAVAILABLE;
    }

    first = virt_rtc_sample();
    second = virt_rtc_sample();
    if (first < VIRT_RTC_MINIMUM_NS || second < first ||
        second - first > VIRT_RTC_MAXIMUM_DELTA_NS) {
        return RISCV_VIRT_RTC_STATUS_UNAVAILABLE;
    }

    *nanoseconds = second;
    return RISCV_VIRT_RTC_STATUS_OK;
}
