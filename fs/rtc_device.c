#include "char_device_internal.h"
#include <arch/riscv/context.h>
#include <arch/riscv/virt_rtc.h>
#include <kernel/errno.h>
#include <kernel/uaccess.h>

static unsigned opened;
static int rtc_open(struct kernel_heap *heap, struct kernel_task *caller, uint32_t flags, void **instance)
{
    (void)heap; (void)caller; (void)flags; (void)instance;
    uint64_t now;
    if (riscv_virt_rtc_read_ns(&now) != RISCV_VIRT_RTC_STATUS_OK) return -KERNEL_ENODEV;
    uintptr_t irq = riscv_interrupt_save();
    int result = opened ? -KERNEL_EBUSY : 0;
    if (!result) opened = 1;
    riscv_interrupt_restore(irq); return result;
}
static void rtc_release(void *instance)
{
    (void)instance;
    uintptr_t irq = riscv_interrupt_save();
    if (!opened) __builtin_trap();
    opened = 0; riscv_interrupt_restore(irq);
}
static unsigned leap(unsigned year)
{ return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
struct rtc_time { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
static int rtc_ioctl(void *instance, struct kernel_task *caller, struct kernel_files *files, struct kernel_open_file_description **owner, struct kernel_mm *mm, uint64_t command, uint64_t argument)
{
    (void)instance; (void)caller; (void)files; (void)owner;
    if (command != UINT64_C(0x80247009)) {
        switch (command) {
        case 0x4004700c: case 0x4008700c: case 0x4004700e: case 0x4008700e:
        case 0x4024700a: case 0x40247007: case 0x80247008:
        case 0x4028700f: case 0x80287010:
        case 0x7001: case 0x7002: case 0x7003: case 0x7004:
        case 0x7005: case 0x7006: case 0x700f: case 0x7010:
        case 0x8008700b: case 0x8008700d: return -KERNEL_ENOTSUP;
        default: return -KERNEL_ENOTTY;
        }
    }
    uint64_t ns;
    if (riscv_virt_rtc_read_ns(&ns) != RISCV_VIRT_RTC_STATUS_OK) return -KERNEL_ENODEV;
    uint64_t seconds = ns / UINT64_C(1000000000), days = seconds / 86400;
    struct rtc_time value = {.sec = seconds % 60, .min = seconds / 60 % 60,
        .hour = seconds / 3600 % 24, .wday = (days + 4) % 7};
    unsigned year = 1970, month = 0;
    while (days >= 365 + leap(year)) { days -= 365 + leap(year); year++; }
    value.year = year - 1900; value.yday = days;
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    while (days >= month_days[month] + (month == 1 && leap(year))) {
        days -= month_days[month] + (month == 1 && leap(year)); month++;
    }
    value.mon = month; value.mday = days + 1;
    size_t copied = 0;
    return kernel_copy_to_user(mm, argument, &value, sizeof(value), &copied) == KERNEL_UACCESS_STATUS_OK &&
           copied == sizeof(value) ? 0 : -KERNEL_EFAULT;
}
static int rtc_read(void *instance, struct kernel_task *caller, uint32_t flags, void *buffer, size_t size, size_t *bytes)
{
    (void)instance; (void)caller; (void)flags; (void)buffer; (void)size; *bytes = 0; return -KERNEL_ENOTSUP; }
static int rtc_write(void *instance, struct kernel_task *caller, uint32_t flags, const void *buffer, size_t size, size_t *bytes)
{
    (void)instance; (void)caller; (void)flags; (void)buffer; (void)size; *bytes = 0; return -KERNEL_ENOTSUP; }
const struct kernel_char_device kernel_rtc_device = {
    .rdev = 0xa87, .kind = KERNEL_OPEN_FILE_KIND_RTC, .read_once = 1,
    .open = rtc_open, .release = rtc_release,
    .ioctl = rtc_ioctl, .read = rtc_read, .write = rtc_write,
};
