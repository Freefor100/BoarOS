#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/boot_memory.h>
#include <kernel/dtb.h>

static void fail_layout(unsigned long case_id,
                        enum boot_memory_status expected,
                        enum boot_memory_status actual)
    __attribute__((noreturn));

static void fail_layout(unsigned long case_id,
                        enum boot_memory_status expected,
                        enum boot_memory_status actual)
{
    virt_uart_puts("BoarOS: boot memory test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex((unsigned long)expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex((unsigned long)actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static void poison_layout(struct boot_memory_layout *layout)
{
    uint32_t index;

    layout->reserved_count = 0xa5a5a5a5U;
    layout->usable_count = 0x5a5a5a5aU;
    for (index = 0U; index < BOOT_MEMORY_MAX_RESERVED_RANGES; index++) {
        layout->reserved[index].base = 0x1100000000000000ULL + index;
        layout->reserved[index].size = 0x2200000000000000ULL + index;
    }
    for (index = 0U; index < BOOT_MEMORY_MAX_USABLE_RANGES; index++) {
        layout->usable[index].base = 0x3300000000000000ULL + index;
        layout->usable[index].size = 0x4400000000000000ULL + index;
    }
}

static int layout_is_poisoned(const struct boot_memory_layout *layout)
{
    uint32_t index;

    if (layout->reserved_count != 0xa5a5a5a5U ||
        layout->usable_count != 0x5a5a5a5aU) {
        return 0;
    }
    for (index = 0U; index < BOOT_MEMORY_MAX_RESERVED_RANGES; index++) {
        if (layout->reserved[index].base !=
                0x1100000000000000ULL + index ||
            layout->reserved[index].size !=
                0x2200000000000000ULL + index) {
            return 0;
        }
    }
    for (index = 0U; index < BOOT_MEMORY_MAX_USABLE_RANGES; index++) {
        if (layout->usable[index].base !=
                0x3300000000000000ULL + index ||
            layout->usable[index].size !=
                0x4400000000000000ULL + index) {
            return 0;
        }
    }

    return 1;
}

static void expect_invalid(unsigned long case_id,
                           const struct dtb_boot_info *info,
                           uint64_t kernel_start,
                           uint64_t kernel_end,
                           uint64_t dtb_address)
{
    struct boot_memory_layout layout;
    enum boot_memory_status actual;

    poison_layout(&layout);
    actual = boot_memory_build(info,
                               kernel_start,
                               kernel_end,
                               dtb_address,
                               &layout);
    if (actual != BOOT_MEMORY_STATUS_INVALID ||
        !layout_is_poisoned(&layout)) {
        fail_layout(case_id, BOOT_MEMORY_STATUS_INVALID, actual);
    }
}

static void test_excludes_firmware_kernel_and_dtb(void)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    enum boot_memory_status actual;

    info.memory.base = 0x1000ULL;
    info.memory.size = 0x8000ULL;
    info.dtb_size = 0x1000U;
    info.reserved_count = 1U;
    info.reserved[0].base = 0x1000ULL;
    info.reserved[0].size = 0x1000ULL;

    actual = boot_memory_build(&info,
                               0x3000ULL,
                               0x4000ULL,
                               0x7000ULL,
                               &layout);
    if (actual != BOOT_MEMORY_STATUS_OK ||
        layout.reserved_count != 3U || layout.usable_count != 3U ||
        layout.reserved[0].base != 0x1000ULL ||
        layout.reserved[0].size != 0x1000ULL ||
        layout.reserved[1].base != 0x3000ULL ||
        layout.reserved[1].size != 0x1000ULL ||
        layout.reserved[2].base != 0x7000ULL ||
        layout.reserved[2].size != 0x1000ULL ||
        layout.usable[0].base != 0x2000ULL ||
        layout.usable[0].size != 0x1000ULL ||
        layout.usable[1].base != 0x4000ULL ||
        layout.usable[1].size != 0x3000ULL ||
        layout.usable[2].base != 0x8000ULL ||
        layout.usable[2].size != 0x1000ULL) {
        fail_layout(1U, BOOT_MEMORY_STATUS_OK, actual);
    }
}

static void test_sorts_clips_and_merges_reservations(void)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    enum boot_memory_status actual;

    info.memory.base = 0x1000ULL;
    info.memory.size = 0x9000ULL;
    info.dtb_size = 0x1000U;
    info.reserved_count = 5U;
    info.reserved[0].base = 0x8000ULL;
    info.reserved[0].size = 0x1000ULL;
    info.reserved[1].base = 0x0ULL;
    info.reserved[1].size = 0x1800ULL;
    info.reserved[2].base = 0x1800ULL;
    info.reserved[2].size = 0x800ULL;
    info.reserved[3].base = 0x5000ULL;
    info.reserved[3].size = 0x1000ULL;
    info.reserved[4].base = 0x5800ULL;
    info.reserved[4].size = 0x2000ULL;

    actual = boot_memory_build(&info,
                               0x2000ULL,
                               0x3000ULL,
                               0xb000ULL,
                               &layout);
    if (actual != BOOT_MEMORY_STATUS_OK ||
        layout.reserved_count != 3U || layout.usable_count != 3U ||
        layout.reserved[0].base != 0x1000ULL ||
        layout.reserved[0].size != 0x2000ULL ||
        layout.reserved[1].base != 0x5000ULL ||
        layout.reserved[1].size != 0x2800ULL ||
        layout.reserved[2].base != 0x8000ULL ||
        layout.reserved[2].size != 0x1000ULL ||
        layout.usable[0].base != 0x3000ULL ||
        layout.usable[0].size != 0x2000ULL ||
        layout.usable[1].base != 0x7800ULL ||
        layout.usable[1].size != 0x800ULL ||
        layout.usable[2].base != 0x9000ULL ||
        layout.usable[2].size != 0x1000ULL) {
        fail_layout(2U, BOOT_MEMORY_STATUS_OK, actual);
    }
}

static void test_reports_empty_memory_without_changing_output(void)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    enum boot_memory_status actual;

    info.memory.base = 0x1000ULL;
    info.memory.size = 0x1000ULL;
    info.dtb_size = 0x1000U;
    info.reserved_count = 1U;
    info.reserved[0].base = 0x0ULL;
    info.reserved[0].size = 0x3000ULL;
    poison_layout(&layout);

    actual = boot_memory_build(&info,
                               0x1000ULL,
                               0x1800ULL,
                               0x4000ULL,
                               &layout);
    if (actual != BOOT_MEMORY_STATUS_EMPTY ||
        !layout_is_poisoned(&layout)) {
        fail_layout(3U, BOOT_MEMORY_STATUS_EMPTY, actual);
    }
}

static void test_rejects_overflow_without_changing_output(void)
{
    struct dtb_boot_info info;

    info.memory.base = 0x1000ULL;
    info.memory.size = 0x8000ULL;
    info.dtb_size = 0x1000U;
    info.reserved_count = 1U;
    info.reserved[0].base = UINT64_MAX - 0x100ULL;
    info.reserved[0].size = 0x200ULL;
    expect_invalid(4U, &info, 0x3000ULL, 0x4000ULL, 0xa000ULL);
}

static void test_rejects_invalid_memory_dtb_and_kernel_ranges(void)
{
    struct dtb_boot_info info;

    info.memory.base = UINT64_MAX - 0x100ULL;
    info.memory.size = 0x200ULL;
    info.dtb_size = 0x1000U;
    info.reserved_count = 0U;
    expect_invalid(5U, &info, 0x1000ULL, 0x2000ULL, 0x3000ULL);

    info.memory.base = 0x1000ULL;
    info.memory.size = 0x8000ULL;
    info.dtb_size = 0x200U;
    expect_invalid(6U,
                   &info,
                   0x3000ULL,
                   0x4000ULL,
                   UINT64_MAX - 0x100ULL);

    info.dtb_size = 0x1000U;
    expect_invalid(7U, &info, 0x0ULL, 0x1000ULL, 0xa000ULL);
    expect_invalid(8U, &info, 0x4000ULL, 0x3000ULL, 0xa000ULL);
}

void run_boot_memory_tests(void)
{
    test_excludes_firmware_kernel_and_dtb();
    test_sorts_clips_and_merges_reservations();
    test_reports_empty_memory_without_changing_output();
    test_rejects_overflow_without_changing_output();
    test_rejects_invalid_memory_dtb_and_kernel_ranges();
}
