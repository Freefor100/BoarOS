#include <arch/riscv/direct_map.h>
#include <arch/riscv/memory_layout.h>
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

void riscv_relocate_to_high(uint64_t offset);

static struct physical_page_allocator page_allocator;
static struct riscv_sv39_page_table kernel_page_table;

static void *direct_map_page_access(uint64_t physical_address)
{
    uint64_t virtual_address;

    if (riscv_direct_map_pa_to_va(physical_address,
                                  BOAROS_PAGE_SIZE,
                                  &virtual_address) !=
        RISCV_DIRECT_MAP_STATUS_OK) {
        return 0;
    }

    return (void *)(uintptr_t)virtual_address;
}

static void shutdown_for_dtb_error(enum dtb_status status)
    __attribute__((noreturn));

static void shutdown_for_dtb_error(enum dtb_status status)
{
    if (status == DTB_STATUS_INVALID) {
        virt_uart_puts("BoarOS: invalid DTB\n");
    } else if (status == DTB_STATUS_NOT_FOUND) {
        virt_uart_puts("BoarOS: DTB memory not found\n");
    } else if (status == DTB_STATUS_UNSUPPORTED) {
        virt_uart_puts("BoarOS: unsupported DTB memory format\n");
    } else {
        virt_uart_puts("BoarOS: unknown DTB error\n");
    }

    sbi_shutdown();
}

static void shutdown_for_boot_memory_error(enum boot_memory_status status)
    __attribute__((noreturn));

static void shutdown_for_boot_memory_error(enum boot_memory_status status)
{
    if (status == BOOT_MEMORY_STATUS_INVALID) {
        virt_uart_puts("BoarOS: invalid boot memory input\n");
    } else if (status == BOOT_MEMORY_STATUS_EMPTY) {
        virt_uart_puts("BoarOS: no usable physical memory\n");
    } else {
        virt_uart_puts("BoarOS: unknown boot memory error\n");
    }

    sbi_shutdown();
}

static void shutdown_for_physical_page_error(enum physical_page_status status)
    __attribute__((noreturn));

static void shutdown_for_physical_page_error(enum physical_page_status status)
{
    if (status == PHYSICAL_PAGE_STATUS_INVALID) {
        virt_uart_puts("BoarOS: invalid physical page layout\n");
    } else if (status == PHYSICAL_PAGE_STATUS_EMPTY) {
        virt_uart_puts("BoarOS: no complete physical pages\n");
    } else if (status == PHYSICAL_PAGE_STATUS_STATE) {
        virt_uart_puts("BoarOS: physical page access is not bound\n");
    } else {
        virt_uart_puts("BoarOS: unknown physical page error\n");
    }

    sbi_shutdown();
}

static void shutdown_for_sv39_error(enum riscv_sv39_status status)
    __attribute__((noreturn));

static void shutdown_for_sv39_error(enum riscv_sv39_status status)
{
    if (status == RISCV_SV39_STATUS_INVALID) {
        virt_uart_puts("BoarOS: invalid Sv39 mapping\n");
    } else if (status == RISCV_SV39_STATUS_NO_MEMORY) {
        virt_uart_puts("BoarOS: no memory for Sv39 page tables\n");
    } else if (status == RISCV_SV39_STATUS_CONFLICT) {
        virt_uart_puts("BoarOS: conflicting Sv39 mapping\n");
    } else if (status == RISCV_SV39_STATUS_STATE) {
        virt_uart_puts("BoarOS: invalid Sv39 page-table state\n");
    } else {
        virt_uart_puts("BoarOS: unknown Sv39 error\n");
    }

    sbi_shutdown();
}

static void shutdown_for_direct_map_error(void) __attribute__((noreturn));

static void shutdown_for_direct_map_error(void)
{
    virt_uart_puts("BoarOS: direct map verification failed\n");
    sbi_shutdown();
}

static enum riscv_sv39_status map_identity(
    uint64_t start,
    uint64_t end,
    uint32_t permissions)
{
    if (start > end) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (start == end) {
        return RISCV_SV39_STATUS_OK;
    }

    return riscv_sv39_map_range(&kernel_page_table,
                                start,
                                start,
                                end - start,
                                permissions);
}

static enum riscv_sv39_status map_kernel_alias(
    uint64_t kernel_start,
    uint64_t start,
    uint64_t end,
    uint32_t permissions)
{
    uint64_t offset;

    if (kernel_start > start || start > end) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (start == end) {
        return RISCV_SV39_STATUS_OK;
    }
    offset = start - kernel_start;
    if (offset > UINT64_MAX - RISCV_KERNEL_VIRTUAL_BASE ||
        end - start > UINT64_MAX - (RISCV_KERNEL_VIRTUAL_BASE + offset)) {
        return RISCV_SV39_STATUS_INVALID;
    }

    return riscv_sv39_map_range(&kernel_page_table,
                                RISCV_KERNEL_VIRTUAL_BASE + offset,
                                start,
                                end - start,
                                permissions);
}

static enum riscv_sv39_status map_direct_alias(
    uint64_t start,
    uint64_t end,
    uint32_t permissions)
{
    uint64_t virtual_address;

    if (start > end) {
        return RISCV_SV39_STATUS_INVALID;
    }
    if (start == end) {
        return RISCV_SV39_STATUS_OK;
    }
    if (riscv_direct_map_pa_to_va(start,
                                  end - start,
                                  &virtual_address) !=
        RISCV_DIRECT_MAP_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }

    return riscv_sv39_map_range(&kernel_page_table,
                                virtual_address,
                                start,
                                end - start,
                                permissions);
}

static enum riscv_sv39_status build_kernel_page_table(
    const struct dtb_boot_info *info)
{
    uint64_t memory_start;
    uint64_t memory_end;
    uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    uint64_t text_start = (uint64_t)(uintptr_t)__text_start;
    uint64_t text_end = (uint64_t)(uintptr_t)__text_end;
    uint64_t rodata_start = (uint64_t)(uintptr_t)__rodata_start;
    uint64_t rodata_end = (uint64_t)(uintptr_t)__rodata_end;
    uint64_t data_start = (uint64_t)(uintptr_t)__data_start;
    uint64_t data_end = (uint64_t)(uintptr_t)__data_end;
    enum riscv_sv39_status status;

    if (info == 0 || info->memory.size == 0U ||
        info->memory.size > UINT64_MAX - info->memory.base) {
        return RISCV_SV39_STATUS_INVALID;
    }
    memory_start = info->memory.base & ~BOAROS_PAGE_MASK;
    memory_end = info->memory.base + info->memory.size;
    if (memory_end > UINT64_MAX - BOAROS_PAGE_MASK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    memory_end = (memory_end + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;

    if (memory_start > text_start || text_start > text_end ||
        text_end != rodata_start || rodata_start > rodata_end ||
        rodata_end != data_start || data_start > data_end ||
        data_end > memory_end) {
        return RISCV_SV39_STATUS_INVALID;
    }

    status = riscv_sv39_page_table_init(&kernel_page_table,
                                        &page_allocator);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    status = map_identity(memory_start,
                          text_start,
                          RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_identity(text_start,
                          text_end,
                          RISCV_SV39_READ | RISCV_SV39_EXECUTE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_identity(rodata_start,
                          rodata_end,
                          RISCV_SV39_READ);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_identity(data_start,
                          memory_end,
                          RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    status = map_kernel_alias(kernel_start,
                              text_start,
                              text_end,
                              RISCV_SV39_READ | RISCV_SV39_EXECUTE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_kernel_alias(kernel_start,
                              rodata_start,
                              rodata_end,
                              RISCV_SV39_READ);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_kernel_alias(kernel_start,
                              data_start,
                              data_end,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    status = map_direct_alias(memory_start,
                              text_start,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_direct_alias(text_start,
                              text_end,
                              RISCV_SV39_READ);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_direct_alias(rodata_start,
                              rodata_end,
                              RISCV_SV39_READ);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_direct_alias(data_start,
                              memory_end,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    return riscv_sv39_map_range(&kernel_page_table,
                                VIRT_UART_MMIO_BASE,
                                VIRT_UART_MMIO_BASE,
                                VIRT_UART_MMIO_SIZE,
                                RISCV_SV39_READ | RISCV_SV39_WRITE);
}

static void verify_direct_map_runtime(void)
{
    const uint64_t pattern = UINT64_C(0x1122334455667788);
    uint64_t physical_address;
    uint64_t virtual_address;
    uint64_t recycled_address;
    volatile uint64_t *words;
    enum physical_page_status status;

    if (page_allocator.access != direct_map_page_access ||
        kernel_page_table.allocator != &page_allocator) {
        shutdown_for_direct_map_error();
    }

    status = physical_page_allocate(&page_allocator, &physical_address);
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(status);
    }
    if (riscv_direct_map_pa_to_va(physical_address,
                                  BOAROS_PAGE_SIZE,
                                  &virtual_address) !=
        RISCV_DIRECT_MAP_STATUS_OK) {
        shutdown_for_direct_map_error();
    }

    words = (volatile uint64_t *)(uintptr_t)virtual_address;
    words[1] = pattern;
    if (words[1] != pattern) {
        shutdown_for_direct_map_error();
    }

    status = physical_page_release(&page_allocator, physical_address);
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(status);
    }
    status = physical_page_allocate(&page_allocator, &recycled_address);
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(status);
    }
    if (recycled_address != physical_address || words[1] != pattern) {
        shutdown_for_direct_map_error();
    }
    status = physical_page_release(&page_allocator, recycled_address);
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(status);
    }

    virt_uart_puts("BoarOS: direct map pa=");
    virt_uart_put_hex((unsigned long)physical_address);
    virt_uart_puts(" va=");
    virt_uart_put_hex((unsigned long)virtual_address);
    virt_uart_puts(" offset=");
    virt_uart_put_hex((unsigned long)(virtual_address - physical_address));
    virt_uart_puts(" access=");
    virt_uart_put_hex((unsigned long)(uintptr_t)page_allocator.access);
    virt_uart_puts(" value=");
    virt_uart_put_hex((unsigned long)pattern);
    virt_uart_puts(" reused=");
    virt_uart_put_hex((unsigned long)recycled_address);
    virt_uart_putc('\n');
}

static uint64_t current_pc(void)
{
    uint64_t value;

    __asm__ volatile("auipc %0, 0" : "=r"(value));
    return value;
}

static uint64_t current_sp(void)
{
    uint64_t value;

    __asm__ volatile("mv %0, sp" : "=r"(value));
    return value;
}

static uint64_t current_gp(void)
{
    uint64_t value;

    __asm__ volatile("mv %0, gp" : "=r"(value));
    return value;
}

static uint64_t current_stvec(void)
{
    uint64_t value;

    __asm__ volatile("csrr %0, stvec" : "=r"(value));
    return value;
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct dtb_boot_info info;
    struct boot_memory_layout layout;
    enum dtb_status dtb_status = dtb_read_boot_info(dtb, &info);
    enum boot_memory_status memory_status;
    enum physical_page_status page_status;
    enum riscv_sv39_status sv39_status;

    if (dtb_status != DTB_STATUS_OK) {
        shutdown_for_dtb_error(dtb_status);
    }

    memory_status = boot_memory_build(
        &info,
        (uint64_t)(uintptr_t)__kernel_start,
        (uint64_t)(uintptr_t)__kernel_end,
        (uint64_t)(uintptr_t)dtb,
        &layout);
    if (memory_status != BOOT_MEMORY_STATUS_OK) {
        shutdown_for_boot_memory_error(memory_status);
    }

    page_status = physical_page_allocator_init(&page_allocator, &layout);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(page_status);
    }

    sv39_status = build_kernel_page_table(&info);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        shutdown_for_sv39_error(sv39_status);
    }
    sv39_status = riscv_sv39_activate(&kernel_page_table);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        shutdown_for_sv39_error(sv39_status);
    }
    riscv_relocate_to_high(RISCV_KERNEL_VIRTUAL_BASE -
                           (uint64_t)(uintptr_t)__kernel_start);

    page_status = physical_page_allocator_bind_access(
        &page_allocator,
        direct_map_page_access);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(page_status);
    }
    kernel_page_table.allocator = &page_allocator;
    verify_direct_map_runtime();

    virt_uart_puts("BoarOS: high-half pc=");
    virt_uart_put_hex((unsigned long)current_pc());
    virt_uart_puts(" sp=");
    virt_uart_put_hex((unsigned long)current_sp());
    virt_uart_puts(" gp=");
    virt_uart_put_hex((unsigned long)current_gp());
    virt_uart_puts(" stvec=");
    virt_uart_put_hex((unsigned long)current_stvec());
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: booted hart=");
    virt_uart_put_hex(hart_id);
    virt_uart_puts(" dtb=");
    virt_uart_put_hex((unsigned long)dtb);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: memory base=");
    virt_uart_put_hex((unsigned long)info.memory.base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)info.memory.size);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: memory layout reserved=");
    virt_uart_put_hex((unsigned long)layout.reserved_count);
    virt_uart_puts(" usable=");
    virt_uart_put_hex((unsigned long)layout.usable_count);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: first reserved base=");
    virt_uart_put_hex((unsigned long)layout.reserved[0].base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)layout.reserved[0].size);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: first usable base=");
    virt_uart_put_hex((unsigned long)layout.usable[0].base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)layout.usable[0].size);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: physical pages total=");
    virt_uart_put_hex((unsigned long)physical_page_total(&page_allocator));
    virt_uart_puts(" available=");
    virt_uart_put_hex(
        (unsigned long)physical_page_available(&page_allocator));
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: Sv39 root=");
    virt_uart_put_hex((unsigned long)kernel_page_table.root_address);
    virt_uart_puts(" tables=");
    virt_uart_put_hex((unsigned long)kernel_page_table.table_pages);
    virt_uart_puts(" leaf4k=");
    virt_uart_put_hex((unsigned long)kernel_page_table.leaf_4k);
    virt_uart_puts(" leaf2m=");
    virt_uart_put_hex((unsigned long)kernel_page_table.leaf_2m);
    virt_uart_puts(" satp=");
    virt_uart_put_hex((unsigned long)riscv_sv39_current_satp());
    virt_uart_putc('\n');

    sbi_shutdown();
}
