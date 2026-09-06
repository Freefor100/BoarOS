#ifndef BOAROS_ARCH_RISCV_VIRT_RTC_H
#define BOAROS_ARCH_RISCV_VIRT_RTC_H

#include <arch/riscv/memory_layout.h>

#include <stdint.h>

#define VIRT_RTC_MMIO_PHYSICAL_BASE 0x101000UL
#define VIRT_RTC_MMIO_KERNEL_BASE \
    (RISCV_KERNEL_MMIO_BASE + VIRT_RTC_MMIO_PHYSICAL_BASE)
#define VIRT_RTC_MMIO_SIZE 0x1000UL

enum riscv_virt_rtc_status {
    RISCV_VIRT_RTC_STATUS_OK = 0,
    RISCV_VIRT_RTC_STATUS_UNAVAILABLE,
};

/*
 * Reads the goldfish RTC wall clock (nanoseconds since the Unix epoch)
 * after sanity-checking two consecutive samples.  UNAVAILABLE means the
 * device is absent or reads implausible values (e.g. non-virt boards).
 */
enum riscv_virt_rtc_status riscv_virt_rtc_read_ns(uint64_t *nanoseconds);

#endif
