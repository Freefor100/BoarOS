#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/boot_memory.h>
#include <kernel/dtb.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>

extern unsigned char __kernel_start[];
extern unsigned char __kernel_end[];
extern unsigned char __text_start[];
extern unsigned char __text_end[];
extern unsigned char __rodata_start[];
extern unsigned char __rodata_end[];
extern unsigned char __data_start[];
extern unsigned char __data_end[];

static const uint64_t readonly_target = UINT64_C(0x1122334455667788);
static struct physical_page_allocator allocator;
static struct riscv_sv39_page_table page_table;

static void *identity_page_access(uint64_t address)
{
    return (void *)(uintptr_t)address;
}

static void fail_setup(void) __attribute__((noreturn));

static uint64_t load_readonly_target(const uint64_t *address)
{
    uint64_t value;

    __asm__ volatile("ld %0, 0(%1)"
                     : "=r"(value)
                     : "r"(address)
                     : "memory");
    return value;
}

static void store_readonly_target(const uint64_t *address, uint64_t value)
{
    __asm__ volatile("sd %0, 0(%1)"
                     :
                     : "r"(value), "r"(address)
                     : "memory");
}

static void fail_setup(void)
{
    virt_uart_puts("BoarOS: Sv39 permission setup failed\n");
    sbi_shutdown();
}

static enum riscv_sv39_status map_identity(uint64_t start,
                                           uint64_t end,
                                           uint32_t permissions)
{
    if (start > end) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (start == end) {
        return RISCV_SV39_STATUS_OK;
    }

    return riscv_sv39_map_range(&page_table,
                                start,
                                start,
                                end - start,
                                permissions);
}

static int build_page_table(const struct dtb_boot_info *info)
{
    uint64_t memory_start = info->memory.base & ~BOAROS_PAGE_MASK;
    uint64_t memory_end;
    uint64_t text_start = (uint64_t)(uintptr_t)__text_start;
    uint64_t text_end = (uint64_t)(uintptr_t)__text_end;
    uint64_t rodata_start = (uint64_t)(uintptr_t)__rodata_start;
    uint64_t rodata_end = (uint64_t)(uintptr_t)__rodata_end;
    uint64_t data_start = (uint64_t)(uintptr_t)__data_start;
    enum riscv_sv39_status status;

    if (info->memory.size > UINT64_MAX - info->memory.base) {
        return 0;
    }
    memory_end = info->memory.base + info->memory.size;
    if (memory_end > UINT64_MAX - BOAROS_PAGE_MASK) {
        return 0;
    }
    memory_end = (memory_end + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;

    status = riscv_sv39_page_table_init(&page_table, &allocator);
    if (status != RISCV_SV39_STATUS_OK || memory_start > text_start ||
        text_start > text_end || text_end != rodata_start ||
        rodata_start > rodata_end || rodata_end != data_start ||
        (uint64_t)(uintptr_t)__data_end > memory_end) {
        return 0;
    }
    status = map_identity(memory_start,
                          text_start,
                          RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status == RISCV_SV39_STATUS_OK) {
        status = map_identity(text_start,
                              text_end,
                              RISCV_SV39_READ | RISCV_SV39_EXECUTE);
    }
    if (status == RISCV_SV39_STATUS_OK) {
        status = map_identity(rodata_start, rodata_end, RISCV_SV39_READ);
    }
    if (status == RISCV_SV39_STATUS_OK) {
        status = map_identity(data_start,
                              memory_end,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    }
    if (status == RISCV_SV39_STATUS_OK) {
        status = riscv_sv39_map_range(&page_table,
                                      VIRT_UART_MMIO_PHYSICAL_BASE,
                                      VIRT_UART_MMIO_PHYSICAL_BASE,
                                      VIRT_UART_MMIO_SIZE,
                                      RISCV_SV39_READ | RISCV_SV39_WRITE);
    }

    return status == RISCV_SV39_STATUS_OK;
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    uint64_t available;
    uint64_t value;

    (void)hart_id;

    if (dtb_read_boot_info(dtb, &info) != DTB_STATUS_OK ||
        boot_memory_build(&info,
                          (uint64_t)(uintptr_t)__kernel_start,
                          (uint64_t)(uintptr_t)__kernel_end,
                          (uint64_t)(uintptr_t)dtb,
                          &layout) != BOOT_MEMORY_STATUS_OK ||
        physical_page_allocator_init(&allocator, &layout) !=
            PHYSICAL_PAGE_STATUS_OK ||
        physical_page_allocator_bind_access(&allocator,
                                            identity_page_access) !=
            PHYSICAL_PAGE_STATUS_OK ||
        !build_page_table(&info)) {
        fail_setup();
    }

    if (riscv_sv39_activate(&page_table) != RISCV_SV39_STATUS_OK ||
        page_table.state != RISCV_SV39_STATE_ACTIVE) {
        fail_setup();
    }
    available = physical_page_available(&allocator);
    if (riscv_sv39_map_range(&page_table,
                             VIRT_UART_MMIO_PHYSICAL_BASE,
                             VIRT_UART_MMIO_PHYSICAL_BASE,
                             VIRT_UART_MMIO_SIZE,
                             RISCV_SV39_READ | RISCV_SV39_WRITE) !=
            RISCV_SV39_STATUS_STATE ||
        riscv_sv39_activate(&page_table) != RISCV_SV39_STATUS_STATE ||
        riscv_sv39_page_table_init(&page_table, &allocator) !=
            RISCV_SV39_STATUS_STATE ||
        riscv_sv39_page_table_init(&page_table, 0) !=
            RISCV_SV39_STATUS_STATE ||
        page_table.state != RISCV_SV39_STATE_ACTIVE ||
        physical_page_available(&allocator) != available) {
        fail_setup();
    }

    virt_uart_puts("BoarOS: Sv39 readonly target=");
    virt_uart_put_hex((unsigned long)(uintptr_t)&readonly_target);
    virt_uart_putc('\n');

    value = load_readonly_target(&readonly_target);
    virt_uart_puts("BoarOS: Sv39 readonly read=");
    virt_uart_put_hex((unsigned long)value);
    virt_uart_putc('\n');

    store_readonly_target(&readonly_target, UINT64_C(0xa5a5a5a5a5a5a5a5));

    virt_uart_puts("BoarOS: Sv39 readonly write returned\n");
    sbi_shutdown();
}
