#include <arch/riscv/context.h>
#include <arch/riscv/direct_map.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/root_boot.h>
#include <arch/riscv/plic.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/timer.h>
#include <arch/riscv/virt_rtc.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/boot_memory.h>
#include <kernel/dtb.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/random.h>
#include <kernel/scheduler.h>
#include <kernel/tick.h>
#include <kernel/time.h>

#include <stdint.h>

extern unsigned char __kernel_start[];
extern unsigned char __kernel_end[];
extern unsigned char __text_start[];
extern unsigned char __text_end[];
extern unsigned char __rodata_start[];
extern unsigned char __rodata_end[];
extern unsigned char __data_start[];
extern unsigned char __data_end[];
extern unsigned char __boot_stack_bottom[];
extern unsigned char __boot_stack_top[];

void riscv_relocate_to_high(uint64_t offset);

#define RISCV_TRANSITION_TABLE_PAGE_COUNT 5U
#define RISCV_ROOT_BOOT_CLEANUP_ATTEMPTS 3U

static struct physical_page_allocator page_allocator;
static struct riscv_sv39_page_table kernel_page_table;
static struct physical_page_allocator transition_page_allocator;
static struct riscv_sv39_page_table transition_page_table;
static struct riscv_root_boot root_boot;
static struct dtb_irq_info boot_irq;
static struct dtb_boot_info boot_info;
static struct boot_memory_layout boot_layout;
static unsigned long boot_hart_id;
static uintptr_t boot_dtb_address;
static int root_started;
static uint64_t cleanup_retry_ticks;
static unsigned char
    transition_table_pages[RISCV_TRANSITION_TABLE_PAGE_COUNT *
                           BOAROS_PAGE_SIZE]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));

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
    virt_uart_emergency_begin();
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
    virt_uart_emergency_begin();
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
    virt_uart_emergency_begin();
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
    virt_uart_emergency_begin();
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
    virt_uart_emergency_begin();
    virt_uart_puts("BoarOS: direct map verification failed\n");
    sbi_shutdown();
}

static void shutdown_for_time_error(enum kernel_time_status status)
    __attribute__((noreturn));

static void shutdown_for_time_error(enum kernel_time_status status)
{
    virt_uart_emergency_begin();
    if (status == KERNEL_TIME_STATUS_INVALID_ARGUMENT) {
        virt_uart_puts("BoarOS: invalid time argument\n");
    } else if (status == KERNEL_TIME_STATUS_ALREADY_INITIALIZED) {
        virt_uart_puts("BoarOS: time already initialized\n");
    } else {
        virt_uart_puts("BoarOS: unknown time startup error\n");
    }
    sbi_shutdown();
}

static void shutdown_for_timer_error(enum riscv_timer_status status)
    __attribute__((noreturn));

static void shutdown_for_timer_error(enum riscv_timer_status status)
{
    virt_uart_emergency_begin();
    if (status == RISCV_TIMER_STATUS_INVALID_ARGUMENT) {
        virt_uart_puts("BoarOS: invalid timer argument\n");
    } else if (status == RISCV_TIMER_STATUS_INVALID_FREQUENCY) {
        virt_uart_puts("BoarOS: invalid timer frequency\n");
    } else if (status == RISCV_TIMER_STATUS_ALREADY_STARTED) {
        virt_uart_puts("BoarOS: timer already started\n");
    } else if (status == RISCV_TIMER_STATUS_SBI_PROBE_FAILED) {
        virt_uart_puts("BoarOS: SBI TIME probe failed\n");
    } else if (status == RISCV_TIMER_STATUS_SBI_TIME_UNAVAILABLE) {
        virt_uart_puts("BoarOS: SBI TIME unavailable\n");
    } else if (status == RISCV_TIMER_STATUS_SBI_SET_FAILED) {
        virt_uart_puts("BoarOS: SBI timer setup failed\n");
    } else {
        virt_uart_puts("BoarOS: unknown timer startup error\n");
    }

    sbi_shutdown();
}

static void shutdown_for_scheduler_error(
    enum kernel_scheduler_status status) __attribute__((noreturn));

static void shutdown_for_scheduler_error(
    enum kernel_scheduler_status status)
{
    virt_uart_emergency_begin();
    virt_uart_puts("BoarOS: scheduler startup/idle error status=");
    virt_uart_put_hex((unsigned long)status);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static void shutdown_for_root_boot_error(
    enum riscv_root_boot_status status) __attribute__((noreturn));

static void shutdown_for_root_boot_error(
    enum riscv_root_boot_status status)
{
    virt_uart_emergency_begin();
    virt_uart_puts("BoarOS: root boot error status=");
    virt_uart_put_hex((unsigned long)status);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static enum riscv_sv39_status map_identity(
    struct riscv_sv39_page_table *table,
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

    return riscv_sv39_map_range(table,
                                start,
                                start,
                                end - start,
                                permissions);
}

static enum riscv_sv39_status map_kernel_alias(
    struct riscv_sv39_page_table *table,
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

    return riscv_sv39_map_range(table,
                                RISCV_KERNEL_VIRTUAL_BASE + offset,
                                start,
                                end - start,
                                permissions);
}

static enum riscv_sv39_status map_direct_alias(
    struct riscv_sv39_page_table *table,
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

    return riscv_sv39_map_range(table,
                                virtual_address,
                                start,
                                end - start,
                                permissions);
}

static enum riscv_sv39_status map_mmio_alias(
    struct riscv_sv39_page_table *table,
    uint64_t physical_address,
    uint64_t size)
{
    uint64_t mapping_start;
    uint64_t mapping_end;

    if (size == 0U || size > UINT64_MAX - physical_address) {
        return RISCV_SV39_STATUS_INVALID;
    }
    mapping_start = physical_address & ~BOAROS_PAGE_MASK;
    mapping_end = physical_address + size;
    if (mapping_end > UINT64_MAX - BOAROS_PAGE_MASK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    mapping_end = (mapping_end + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    if (mapping_start >= RISCV_KERNEL_MMIO_SIZE ||
        mapping_end > RISCV_KERNEL_MMIO_SIZE) {
        return RISCV_SV39_STATUS_INVALID;
    }

    return riscv_sv39_map_range(table,
                                RISCV_KERNEL_MMIO_BASE + mapping_start,
                                mapping_start,
                                mapping_end - mapping_start,
                                RISCV_SV39_READ | RISCV_SV39_WRITE);
}

static enum riscv_sv39_status build_transition_page_table(void)
{
    struct boot_memory_layout layout;
    uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_end;
    uint64_t mapping_start;
    uint64_t mapping_end;
    uint64_t mapping_size;
    uint64_t virtual_start;
    enum physical_page_status page_status;
    enum riscv_sv39_status status;

    if (kernel_start > kernel_end ||
        kernel_end > UINT64_MAX - (RISCV_SV39_PAGE_SIZE_2M - 1U)) {
        return RISCV_SV39_STATUS_INVALID;
    }
    mapping_start = kernel_start & ~(RISCV_SV39_PAGE_SIZE_2M - 1U);
    mapping_end = (kernel_end + RISCV_SV39_PAGE_SIZE_2M - 1U) &
                  ~(RISCV_SV39_PAGE_SIZE_2M - 1U);
    mapping_size = mapping_end - mapping_start;
    if (kernel_start - mapping_start > RISCV_KERNEL_VIRTUAL_BASE) {
        return RISCV_SV39_STATUS_INVALID;
    }
    virtual_start = RISCV_KERNEL_VIRTUAL_BASE -
                    (kernel_start - mapping_start);

    layout.reserved_count = 0U;
    layout.usable_count = 1U;
    layout.usable[0].base =
        (uint64_t)(uintptr_t)transition_table_pages;
    layout.usable[0].size = sizeof(transition_table_pages);
    page_status = physical_page_allocator_init(&transition_page_allocator,
                                               &layout);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return RISCV_SV39_STATUS_INVALID;
    }
    status = riscv_sv39_page_table_init(&transition_page_table,
                                        &transition_page_allocator);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    status = map_identity(&transition_page_table,
                          mapping_start,
                          mapping_end,
                          RISCV_SV39_READ |
                              RISCV_SV39_WRITE |
                              RISCV_SV39_EXECUTE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = riscv_sv39_map_range(&transition_page_table,
                                  virtual_start,
                                  mapping_start,
                                  mapping_size,
                                  RISCV_SV39_READ |
                                      RISCV_SV39_WRITE |
                                      RISCV_SV39_EXECUTE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = riscv_sv39_map_range(&transition_page_table,
                                  VIRT_UART_MMIO_PHYSICAL_BASE,
                                  VIRT_UART_MMIO_PHYSICAL_BASE,
                                  VIRT_UART_MMIO_SIZE,
                                  RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    if (physical_page_available(&transition_page_allocator) != 0U ||
        transition_page_table.table_pages !=
            RISCV_TRANSITION_TABLE_PAGE_COUNT) {
        return RISCV_SV39_STATUS_INVALID;
    }

    return RISCV_SV39_STATUS_OK;
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
    uint32_t index;

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

    status = map_kernel_alias(&kernel_page_table,
                              kernel_start,
                              text_start,
                              text_end,
                              RISCV_SV39_READ | RISCV_SV39_EXECUTE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_kernel_alias(&kernel_page_table,
                              kernel_start,
                              rodata_start,
                              rodata_end,
                              RISCV_SV39_READ);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_kernel_alias(&kernel_page_table,
                              kernel_start,
                              data_start,
                              data_end,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    status = map_direct_alias(&kernel_page_table,
                              memory_start,
                              text_start,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_direct_alias(&kernel_page_table,
                              text_start,
                              text_end,
                              RISCV_SV39_READ);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_direct_alias(&kernel_page_table,
                              rodata_start,
                              rodata_end,
                              RISCV_SV39_READ);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_direct_alias(&kernel_page_table,
                              data_start,
                              memory_end,
                              RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    struct dtb_memory_range uart_ranges[2];
    unsigned uart_count = riscv_uart_tty_mapping_ranges(
        (struct dtb_memory_range){VIRT_UART_MMIO_PHYSICAL_BASE, VIRT_UART_MMIO_SIZE},
        &boot_irq.uart, uart_ranges);
    if (!uart_count) return RISCV_SV39_STATUS_INVALID;
    /* 固定早期sink与发现的UART可能共用页；同一PTE只能建立一次。 */
    for (unsigned i = 0; i < uart_count; i++) {
        status = map_mmio_alias(&kernel_page_table, uart_ranges[i].base, uart_ranges[i].size);
        if (status != RISCV_SV39_STATUS_OK) return status;
    }
    status = map_mmio_alias(&kernel_page_table,
                            VIRT_RTC_MMIO_PHYSICAL_BASE,
                            VIRT_RTC_MMIO_SIZE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }
    status = map_mmio_alias(&kernel_page_table, boot_irq.plic.base, boot_irq.plic.size);
    if (status != RISCV_SV39_STATUS_OK) return status;
    for (index = 0U; index < info->virtio_mmio_count; index++) {
        status = map_mmio_alias(&kernel_page_table,
                                info->virtio_mmio[index].base,
                                info->virtio_mmio[index].size);
        if (status != RISCV_SV39_STATUS_OK) {
            return status;
        }
    }

    /* 空栈窗口必须在任何 mm 复制 root 项之前建立。 */
    status = riscv_sv39_kernel_window_reserve(
        &kernel_page_table,
        RISCV_KERNEL_STACK_WINDOW_BASE,
        RISCV_KERNEL_STACK_WINDOW_SIZE);
    if (status != RISCV_SV39_STATUS_OK) {
        return status;
    }

    return RISCV_SV39_STATUS_OK;
}

static void verify_direct_map_runtime(void)
{
    const uint64_t pattern = UINT64_C(0x1122334455667788);
    uint64_t physical_address;
    uint64_t virtual_address;
    uint64_t recycled_address;
    void *page_pointer;
    void *recycled_pointer;
    volatile uint64_t *words;
    volatile uint64_t *recycled_words;
    enum physical_page_status status;

    if (kernel_page_table.allocator != &page_allocator) {
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
    status = physical_page_resolve(&page_allocator,
                                   physical_address,
                                   &page_pointer);
    if (status != PHYSICAL_PAGE_STATUS_OK ||
        (uint64_t)(uintptr_t)page_pointer != virtual_address) {
        shutdown_for_direct_map_error();
    }

    words = page_pointer;
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
    status = physical_page_resolve(&page_allocator,
                                   recycled_address,
                                   &recycled_pointer);
    if (status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_direct_map_error();
    }
    recycled_words = recycled_pointer;
    recycled_words[1] = pattern;
    if (recycled_words[1] != pattern) {
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
    virt_uart_put_hex((unsigned long)(uintptr_t)direct_map_page_access);
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

static int __attribute__((noinline)) boot_storage_present(const struct dtb_boot_info *info)
{
    for (uint32_t i = 0; i < info->virtio_mmio_count; i++) {
        volatile uint32_t *registers = (void *)(uintptr_t)(RISCV_KERNEL_MMIO_BASE + info->virtio_mmio[i].base);
        if (info->virtio_mmio[i].size >= 12 && registers[0] == UINT32_C(0x74726976) && registers[2] == 2)
            return 1;
    }
    return 0;
}

static void storage_cleanup_worker(void *argument)
{
    (void)argument;
    (void)riscv_interrupt_save();
    kernel_scheduler_register_cleanup();
    enum kernel_scheduler_status scheduler_status;
    enum riscv_root_boot_status root_status;
    for (;;) {
        uintptr_t interrupt_status = riscv_interrupt_save();
        struct kernel_thread_completion completion;
        struct kernel_thread_completion init_completion;
        int init_reaped = 0;

        do {
            scheduler_status = kernel_scheduler_reap_one(&completion);
            if (scheduler_status == KERNEL_SCHEDULER_STATUS_OK &&
                root_started && completion.kind == KERNEL_THREAD_KIND_USER &&
                completion.tgid == 1) {
                init_completion = completion;
                init_reaped = 1;
            }
        } while (scheduler_status == KERNEL_SCHEDULER_STATUS_OK ||
                 (scheduler_status == KERNEL_SCHEDULER_STATUS_EMPTY &&
                  kernel_scheduler_reap_pending()));
        riscv_interrupt_restore(interrupt_status);
        if (init_reaped) {
            struct kernel_heap_statistics heap_statistics;
            uint64_t available_pages;

            root_status = riscv_root_boot_finish(&root_boot,
                                                  &init_completion,
                                                  &heap_statistics,
                                                  &available_pages);
            if (root_status != RISCV_ROOT_BOOT_STATUS_OK) {
                if (root_status == RISCV_ROOT_BOOT_STATUS_CLEANUP) {
                    virt_uart_puts("BoarOS: root finish failure stage=");
                    virt_uart_put_hex(root_boot.finish_failure);
                    virt_uart_puts(" error=");
                    virt_uart_put_hex((unsigned long)(uint32_t)
                                      root_boot.finish_error);
                    if ((root_boot.finish_failure &
                         (RISCV_ROOT_FINISH_HEAP_BASELINE |
                          RISCV_ROOT_FINISH_PAGE_BASELINE)) != 0U) {
                        virt_uart_puts(" heap-live=");
                        virt_uart_put_hex(heap_statistics.live_allocations);
                        virt_uart_puts(" heap-pages=");
                        virt_uart_put_hex(heap_statistics.current_pages);
                        virt_uart_puts(" available=");
                        virt_uart_put_hex(available_pages);
                        virt_uart_puts(" baseline=");
                        virt_uart_put_hex(root_boot.baseline_pages);
                    }
                    virt_uart_putc('\n');
                }
                shutdown_for_root_boot_error(root_status);
            }
            struct kernel_stack_statistics stack_statistics;
            kernel_scheduler_stack_statistics(&stack_statistics);
            virt_uart_puts("BoarOS: task stacks released=");
            virt_uart_put_hex((unsigned long)stack_statistics.stacks_released);
            virt_uart_puts(" min-free=");
            virt_uart_put_hex((unsigned long)stack_statistics.minimum_free_bytes);
            virt_uart_puts(" max-used=");
            virt_uart_put_hex((unsigned long)stack_statistics.maximum_used_bytes);
            virt_uart_putc('\n');
            virt_uart_puts("BoarOS: PID 1 exited status=");
            virt_uart_put_hex((unsigned long)completion.status);
            virt_uart_puts(" pages=");
            virt_uart_put_hex((unsigned long)available_pages);
            virt_uart_puts(" heap-live=");
            virt_uart_put_hex(
                (unsigned long)heap_statistics.live_allocations);
            virt_uart_puts("; shutting down\n");
            sbi_shutdown();
        }
        if (scheduler_status != KERNEL_SCHEDULER_STATUS_EMPTY &&
            scheduler_status != KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP) {
            shutdown_for_scheduler_error(scheduler_status);
        }
        int cleanup_retry =
            scheduler_status == KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP;
        interrupt_status = riscv_interrupt_save();
        kernel_scheduler_wait_cleanup(cleanup_retry ? riscv_time_read() + cleanup_retry_ticks : 0);
        riscv_interrupt_restore(interrupt_status);
    }
}

static void kernel_main_high(void) __attribute__((noinline, noreturn));

void kernel_main(unsigned long hart_id, const void *dtb)
{
    enum dtb_status dtb_status = dtb_read_boot_info(dtb, &boot_info);
    enum boot_memory_status memory_status;
    enum physical_page_status page_status;
    enum riscv_sv39_status sv39_status;

    if (dtb_status != DTB_STATUS_OK) {
        shutdown_for_dtb_error(dtb_status);
    }
    if (dtb_read_irq_info(dtb, hart_id, &boot_irq) != DTB_STATUS_OK)
        shutdown_for_dtb_error(DTB_STATUS_UNSUPPORTED);
    boot_hart_id = hart_id;
    boot_dtb_address = (uintptr_t)dtb;
    /* 重定位前 __kernel_start 即物理加载地址，绑定镜像 VA->PA 偏移。 */
    riscv_image_bind_load_offset(RISCV_KERNEL_VIRTUAL_BASE -
                                 (uint64_t)(uintptr_t)__kernel_start);
    cleanup_retry_ticks = boot_info.timebase_frequency / KERNEL_TICKS_PER_SECOND;
    (void)kernel_random_initialize(boot_info.rng_seed, boot_info.rng_seed_size);
    kernel_random_erase(boot_info.rng_seed, sizeof(boot_info.rng_seed));
    boot_info.rng_seed_size = 0U;

    memory_status = boot_memory_build(
        &boot_info,
        (uint64_t)(uintptr_t)__kernel_start,
        (uint64_t)(uintptr_t)__kernel_end,
        (uint64_t)(uintptr_t)dtb,
        &boot_layout);
    if (memory_status != BOOT_MEMORY_STATUS_OK) {
        shutdown_for_boot_memory_error(memory_status);
    }

    page_status = physical_page_allocator_init(&page_allocator, &boot_layout);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(page_status);
    }

    sv39_status = build_transition_page_table();
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        shutdown_for_sv39_error(sv39_status);
    }
    sv39_status = build_kernel_page_table(&boot_info);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        shutdown_for_sv39_error(sv39_status);
    }
    sv39_status = riscv_sv39_activate(&transition_page_table);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        shutdown_for_sv39_error(sv39_status);
    }
    riscv_relocate_to_high(RISCV_KERNEL_VIRTUAL_BASE -
                           (uint64_t)(uintptr_t)__kernel_start);
    /* 切换前算出的低地址指针不能带入仅保留高地址的最终页表。 */
    kernel_main_high();
}

static void kernel_main_high(void)
{
    enum riscv_sv39_status sv39_status;
    enum physical_page_status page_status;
    enum kernel_scheduler_status scheduler_status;
    enum riscv_root_boot_status root_status;
    enum riscv_timer_status timer_status;
    enum kernel_time_status time_status;
    uint64_t boot_realtime_ns = 0;

    sv39_status = riscv_sv39_activate(&kernel_page_table);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        shutdown_for_sv39_error(sv39_status);
    }
    virt_uart_use_kernel_mapping();

    page_status = physical_page_allocator_bind_access(
        &page_allocator,
        direct_map_page_access);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(page_status);
    }
    page_status = physical_page_allocator_finalize(&page_allocator);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        shutdown_for_physical_page_error(page_status);
    }
    virt_uart_puts("BoarOS: physical allocator mode=buddy metadata=");
    virt_uart_put_hex(
        (unsigned long)physical_page_metadata_pages(&page_allocator));
    virt_uart_putc('\n');
    kernel_page_table.allocator = &page_allocator;
    verify_direct_map_runtime();

    scheduler_status = kernel_scheduler_init(
        &page_allocator,
        (uintptr_t)__boot_stack_bottom,
        (uintptr_t)__boot_stack_top);
    if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
        shutdown_for_scheduler_error(scheduler_status);
    }

    if (boot_storage_present(&boot_info)) {
        scheduler_status = kernel_thread_create(storage_cleanup_worker, 0);
        if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) shutdown_for_scheduler_error(scheduler_status);
    }
    if (!riscv_plic_init((void *)(uintptr_t)(RISCV_KERNEL_MMIO_BASE + boot_irq.plic.base),
                         boot_irq.plic.size, boot_irq.context, boot_irq.source_count)) __builtin_trap();

    root_status = riscv_root_boot_start_with_irq(&root_boot,
                                        &boot_info,
                                        &boot_irq,
                                        &page_allocator,
                                        &kernel_page_table);
    if (root_status == RISCV_ROOT_BOOT_STATUS_OK) {
        root_started = 1;
        if (kernel_page_cache_start_worker(&root_boot.page_cache) != 0)
            shutdown_for_root_boot_error(RISCV_ROOT_BOOT_STATUS_RESOURCES);
        if (kernel_vfs_start_journal_worker(&root_boot.mount) != 0)
            shutdown_for_root_boot_error(RISCV_ROOT_BOOT_STATUS_RESOURCES);
        for (uint32_t disk = 0U; disk < root_boot.device_count; disk++) {
            struct riscv_virtio_mmio_block *device =
                riscv_root_boot_device(&root_boot, disk);
            uint32_t source = 0;
            uint64_t base = (uintptr_t)device->mmio - RISCV_KERNEL_MMIO_BASE;
            for (uint32_t i = 0; i < boot_irq.route_count; i++)
                if (boot_irq.routes[i].base == base) source = boot_irq.routes[i].source;
            if (!riscv_virtio_mmio_block_enable_irq(device, source)) __builtin_trap();
            virt_uart_puts("BoarOS: block device=");
            virt_uart_put_hex(device->block.device_number);
            virt_uart_puts(" irq=");
            virt_uart_put_hex(source);
            virt_uart_puts("\n");
        }
        if (riscv_root_boot_start_rng(&root_boot, &boot_info, &boot_irq))
            shutdown_for_root_boot_error(RISCV_ROOT_BOOT_STATUS_CLEANUP);
        if (kernel_network_start(&root_boot.network, &root_boot.heap,
                                 &boot_info, &boot_irq))
            shutdown_for_root_boot_error(RISCV_ROOT_BOOT_STATUS_CLEANUP);
        virt_uart_puts("BoarOS: root /init started pid=0x1\n");
    } else if (root_status != RISCV_ROOT_BOOT_STATUS_NO_DEVICE) {
        uint32_t cleanup_attempts = 0U;

        while (root_boot.state == RISCV_ROOT_BOOT_CLEANUP &&
               cleanup_attempts < RISCV_ROOT_BOOT_CLEANUP_ATTEMPTS) {
            cleanup_attempts++;
            root_status = riscv_root_boot_cleanup(&root_boot);
        }
        shutdown_for_root_boot_error(root_status);
    }

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
    virt_uart_put_hex(boot_hart_id);
    virt_uart_puts(" dtb=");
    virt_uart_put_hex((unsigned long)boot_dtb_address);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: memory base=");
    virt_uart_put_hex((unsigned long)boot_info.memory.base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)boot_info.memory.size);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: memory layout reserved=");
    virt_uart_put_hex((unsigned long)boot_layout.reserved_count);
    virt_uart_puts(" usable=");
    virt_uart_put_hex((unsigned long)boot_layout.usable_count);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: first reserved base=");
    virt_uart_put_hex((unsigned long)boot_layout.reserved[0].base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)boot_layout.reserved[0].size);
    virt_uart_putc('\n');

    virt_uart_puts("BoarOS: first usable base=");
    virt_uart_put_hex((unsigned long)boot_layout.usable[0].base);
    virt_uart_puts(" size=");
    virt_uart_put_hex((unsigned long)boot_layout.usable[0].size);
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

    if (riscv_virt_rtc_read_ns(&boot_realtime_ns) !=
        RISCV_VIRT_RTC_STATUS_OK) {
        boot_realtime_ns = 0;
    }
    time_status = kernel_time_init(boot_info.timebase_frequency,
                                   boot_realtime_ns);
    if (time_status != KERNEL_TIME_STATUS_OK) {
        shutdown_for_time_error(time_status);
    }

    /* OpenSBI hands the hart over with sstatus.FS=Dirty; start Clean so
     * kernel-only execution never produces an FP save. */
    {
        uintptr_t fs_mask = 0x6000U;
        uintptr_t fs_clean = 0x4000U;

        __asm__ volatile("csrc sstatus, %0" ::"r"(fs_mask) : "memory");
        __asm__ volatile("csrs sstatus, %0" ::"r"(fs_clean) : "memory");
    }

    timer_status = riscv_timer_start(boot_info.timebase_frequency,
                                     KERNEL_TICKS_PER_SECOND);
    if (timer_status != RISCV_TIMER_STATUS_OK) {
        shutdown_for_timer_error(timer_status);
    }

    virt_uart_puts("BoarOS: timer frequency=");
    virt_uart_put_hex((unsigned long)boot_info.timebase_frequency);
    virt_uart_puts(" tick-hz=");
    virt_uart_put_hex(KERNEL_TICKS_PER_SECOND);
    virt_uart_puts(" period=");
    virt_uart_put_hex((unsigned long)(boot_info.timebase_frequency /
                                      KERNEL_TICKS_PER_SECOND));
    virt_uart_putc('\n');

    for (;;) {
        uintptr_t irq = riscv_interrupt_save();
        if (!root_started) {
            struct kernel_thread_completion completion;
            while (kernel_scheduler_reap_one(&completion) == KERNEL_SCHEDULER_STATUS_OK) { }
        }
        scheduler_status = kernel_scheduler_yield_current();
        if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) shutdown_for_scheduler_error(scheduler_status);
        riscv_interrupt_restore(irq | RISCV_SSTATUS_SIE);
        asm volatile("wfi");
    }

}
