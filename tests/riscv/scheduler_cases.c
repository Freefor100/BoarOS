#include <arch/riscv/context.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>

#include <stdint.h>

extern unsigned char __boot_stack_bottom[];
extern unsigned char __boot_stack_top[];

#define TEST_PHYSICAL_BASE UINT64_C(0x41000000)
#define TEST_PAGE_COUNT 2U

static unsigned char page_pool[BOAROS_PAGE_SIZE * TEST_PAGE_COUNT]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned long fail_next_access;
static uintptr_t entry_sp[TEST_PAGE_COUNT];
static void *entry_tp[TEST_PAGE_COUNT];
static uintptr_t entry_order[TEST_PAGE_COUNT];
static unsigned long entry_count;

_Static_assert(RISCV_THREAD_STATE_KERNEL_SP ==
                   offsetof(struct riscv_thread_state, kernel_sp),
               "RISC-V thread kernel-sp offset mismatch");

static void *scheduler_page_access(uint64_t physical_address)
{
    if (fail_next_access != 0U) {
        fail_next_access = 0U;
        return 0;
    }
    if (physical_address < TEST_PHYSICAL_BASE ||
        physical_address - TEST_PHYSICAL_BASE >= sizeof(page_pool)) {
        return 0;
    }

    return &page_pool[physical_address - TEST_PHYSICAL_BASE];
}

static void thread_entry(void *argument)
{
    uintptr_t stack_pointer;
    unsigned long index = entry_count;

    __asm__ volatile("mv %0, sp" : "=r"(stack_pointer));
    if (index < TEST_PAGE_COUNT) {
        entry_sp[index] = stack_pointer;
        entry_tp[index] = riscv_current_thread_get();
        entry_order[index] = (uintptr_t)argument;
    }
    entry_count++;
}

static unsigned long expect_status(
    enum kernel_scheduler_status expected,
    enum kernel_scheduler_status actual)
{
    return expected != actual;
}

static unsigned long run_preinit_cases(void)
{
    struct kernel_thread_completion completion = {
        .kind = (enum kernel_thread_kind)0x11,
        .reason = (enum kernel_thread_exit_reason)0x22,
        .status = UINT64_C(0x33445566778899aa),
        .detail = UINT64_C(0xbbccddeeff001122),
    };
    unsigned long failures = 0U;

    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_thread_create(thread_entry, 0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_scheduler_on_tick(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != (enum kernel_thread_kind)0x11 ||
        completion.reason != (enum kernel_thread_exit_reason)0x22 ||
        completion.status != UINT64_C(0x33445566778899aa) ||
        completion.detail != UINT64_C(0xbbccddeeff001122)) {
        failures++;
    }

    return failures;
}

static unsigned long run_init_cases(
    struct physical_page_allocator *allocator)
{
    uintptr_t stack_low = (uintptr_t)__boot_stack_bottom;
    uintptr_t stack_high = (uintptr_t)__boot_stack_top;
    void *idle_thread;
    unsigned long failures = 0U;

    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(0,
                                                    stack_low,
                                                    stack_high));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_high,
                                                    stack_low));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_low));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_low + 1U,
                                                    stack_high));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_init(allocator,
                                                    stack_high + 16U,
                                                    stack_high + 32U));

    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_high));
    __asm__ volatile("csrci sstatus, 2" ::: "memory");

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_high));
    idle_thread = riscv_current_thread_get();
    if (idle_thread == 0) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_ALREADY_INITIALIZED,
                              kernel_scheduler_init(allocator,
                                                    stack_low,
                                                    stack_high));
    if (riscv_current_thread_get() != idle_thread) {
        failures++;
    }

    return failures;
}

static unsigned long run_idle_cases(void)
{
    struct kernel_thread_completion completion = {
        .kind = (enum kernel_thread_kind)0x33,
        .reason = (enum kernel_thread_exit_reason)0x44,
        .status = UINT64_C(0x5566778899aabbcc),
        .detail = UINT64_C(0xddeeff0011223344),
    };
    unsigned long failures = 0U;

    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_thread_create(0, 0));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
                              kernel_scheduler_on_tick(0U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_EMPTY,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != (enum kernel_thread_kind)0x33 ||
        completion.reason != (enum kernel_thread_exit_reason)0x44 ||
        completion.status != UINT64_C(0x5566778899aabbcc) ||
        completion.detail != UINT64_C(0xddeeff0011223344)) {
        failures++;
    }

    __asm__ volatile("csrsi sstatus, 2" ::: "memory");
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_on_tick(1U));
    completion.status = UINT64_C(0x8877665544332211);
    failures += expect_status(KERNEL_SCHEDULER_STATUS_INVALID_STATE,
                              kernel_scheduler_reap_one(&completion));
    __asm__ volatile("csrci sstatus, 2" ::: "memory");
    if (completion.status != UINT64_C(0x8877665544332211)) {
        failures++;
    }

    return failures;
}

static unsigned long run_create_cases(
    struct physical_page_allocator *allocator)
{
    uint64_t initial_available = physical_page_available(allocator);
    struct kernel_thread_completion completion;
    unsigned long failures = 0U;

    fail_next_access = 1U;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_PAGE_ACCESS,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) != initial_available) {
        failures++;
    }

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(thread_entry,
                                                   (void *)(uintptr_t)1U));
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_thread_create(thread_entry,
                                                   (void *)(uintptr_t)2U));
    if (physical_page_available(allocator) != 0U) {
        failures++;
    }
    failures += expect_status(KERNEL_SCHEDULER_STATUS_NO_MEMORY,
                              kernel_thread_create(thread_entry, 0));
    if (physical_page_available(allocator) != 0U) {
        failures++;
    }

    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_on_tick(1U));
    if (entry_count != TEST_PAGE_COUNT ||
        entry_order[0] != 1U || entry_order[1] != 2U ||
        entry_tp[0] == 0 || entry_tp[1] == 0 ||
        entry_tp[0] == entry_tp[1] ||
        (entry_sp[0] & ~(uintptr_t)BOAROS_PAGE_MASK) ==
            (entry_sp[1] & ~(uintptr_t)BOAROS_PAGE_MASK)) {
        failures++;
    }
    completion.kind = (enum kernel_thread_kind)0x55;
    completion.reason = (enum kernel_thread_exit_reason)0x66;
    completion.status = UINT64_MAX;
    completion.detail = UINT64_MAX;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available - 1U) {
        failures++;
    }

    completion.kind = (enum kernel_thread_kind)0x77;
    completion.reason = (enum kernel_thread_exit_reason)0x88;
    completion.status = UINT64_MAX;
    completion.detail = UINT64_MAX;
    failures += expect_status(KERNEL_SCHEDULER_STATUS_OK,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != KERNEL_THREAD_KIND_KERNEL ||
        completion.reason != KERNEL_THREAD_EXIT_RETURNED ||
        completion.status != 0U || completion.detail != 0U ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    completion.kind = (enum kernel_thread_kind)0x99;
    completion.reason = (enum kernel_thread_exit_reason)0xaa;
    completion.status = UINT64_C(0xbbccddeeff001122);
    completion.detail = UINT64_C(0x33445566778899aa);
    failures += expect_status(KERNEL_SCHEDULER_STATUS_EMPTY,
                              kernel_scheduler_reap_one(&completion));
    if (completion.kind != (enum kernel_thread_kind)0x99 ||
        completion.reason != (enum kernel_thread_exit_reason)0xaa ||
        completion.status != UINT64_C(0xbbccddeeff001122) ||
        completion.detail != UINT64_C(0x33445566778899aa) ||
        physical_page_available(allocator) != initial_available) {
        failures++;
    }

    return failures;
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    struct boot_memory_layout layout;
    struct physical_page_allocator allocator;
    enum physical_page_status page_status;
    unsigned long failures;

    (void)hart_id;
    (void)dtb;

    layout.usable_count = 1U;
    layout.usable[0].base = TEST_PHYSICAL_BASE;
    layout.usable[0].size = sizeof(page_pool);
    page_status = physical_page_allocator_init(&allocator, &layout);
    if (page_status == PHYSICAL_PAGE_STATUS_OK) {
        page_status = physical_page_allocator_bind_access(
            &allocator,
            scheduler_page_access);
    }

    failures = page_status != PHYSICAL_PAGE_STATUS_OK;
    failures += run_preinit_cases();
    failures += run_init_cases(&allocator);
    failures += run_idle_cases();
    failures += run_create_cases(&allocator);

    virt_uart_puts("BoarOS: scheduler cases failures=");
    virt_uart_put_hex(failures);
    virt_uart_putc('\n');
    sbi_shutdown();
}
