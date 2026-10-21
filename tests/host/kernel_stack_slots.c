/* Exercise the actual private slot allocator without exporting a test API. */
#include <arch/task.h>
#if BOAROS_PAGE_SHIFT == 14
/* The host executes RV boundary stubs; these constants check LA geometry. */
#undef ARCH_KERNEL_STACK_WINDOW_BASE
#undef ARCH_KERNEL_STACK_WINDOW_SIZE
#define ARCH_KERNEL_STACK_WINDOW_BASE UINT64_C(0xffff800000000000)
#define ARCH_KERNEL_STACK_WINDOW_SIZE UINT64_C(0x06000000)
#endif
#include "../../kernel/sched/core.c"
#include <assert.h>
#include <stdio.h>

_Thread_local uintptr_t sync_test_irq = 1;
static struct kernel_cpu test_cpu;
static struct kernel_io_context bootstrap_io;
static struct physical_page_allocator allocator;
static unsigned char metadata_page[BOAROS_PAGE_SIZE] __attribute__((aligned(BOAROS_PAGE_SIZE)));
static unsigned char stack_pages[KERNEL_STACK_BYTES] __attribute__((aligned(KERNEL_STACK_BYTES)));
static uint64_t physical_stack, mapped_stack;
static unsigned metadata_owned, stack_owned, map_calls;
static enum riscv_sv39_status map_result = RISCV_SV39_STATUS_NO_MEMORY;

void *sync_test_cpu(void) { return &test_cpu; }
void *sync_test_current_task(void) { return NULL; }
void sync_test_current_task_set(void *task) { (void)task; __builtin_trap(); }
uintptr_t sync_test_stack(void) { return 0; }
enum physical_page_status physical_page_allocate(struct physical_page_allocator *owner, uint64_t *address)
{
    assert(owner == &allocator && !metadata_owned && !stack_owned);
    metadata_owned = 1; *address = UINT64_C(0x80000000);
    return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_allocate_order(struct physical_page_allocator *owner,
    uint32_t order, uint64_t *address)
{
    assert(owner == &allocator && metadata_owned && !stack_owned && order == KERNEL_STACK_ORDER);
    stack_owned = 1; *address = physical_stack;
    return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_resolve(const struct physical_page_allocator *owner,
    uint64_t address, void **pointer)
{
    assert(owner == &allocator && metadata_owned);
    if (address == UINT64_C(0x80000000)) *pointer = metadata_page;
    else { assert(address == physical_stack && stack_owned); *pointer = stack_pages; }
    return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_release_order(struct physical_page_allocator *owner,
    uint64_t address, uint32_t order)
{
    assert(owner == &allocator && address == physical_stack && order == KERNEL_STACK_ORDER);
    assert(stack_owned && metadata_owned && !test_cpu.raw_locks);
    stack_owned = 0; return PHYSICAL_PAGE_STATUS_OK;
}
enum physical_page_status physical_page_release(struct physical_page_allocator *owner, uint64_t address)
{
    assert(owner == &allocator && address == UINT64_C(0x80000000));
    assert(metadata_owned && !stack_owned && !test_cpu.raw_locks);
    metadata_owned = 0; return PHYSICAL_PAGE_STATUS_OK;
}
int riscv_sv39_kernel_window_active(void) { return 1; }
enum riscv_sv39_status riscv_sv39_kernel_window_map_pages(struct physical_page_allocator *owner,
    uint64_t va, uint64_t pa, unsigned pages)
{
    assert(owner == &allocator && pa == physical_stack && pages == KERNEL_STACK_PAGES);
    assert(metadata_owned && stack_owned && !test_cpu.raw_locks);
    assert(va >= ARCH_KERNEL_STACK_WINDOW_BASE + BOAROS_PAGE_SIZE &&
        va + KERNEL_STACK_BYTES <= ARCH_KERNEL_STACK_WINDOW_BASE + ARCH_KERNEL_STACK_WINDOW_SIZE);
    assert((va - ARCH_KERNEL_STACK_WINDOW_BASE) % ARCH_KERNEL_STACK_SLOT_SIZE == BOAROS_PAGE_SIZE);
    mapped_stack = va; map_calls++;
    return map_result;
}

static uint32_t first_same_index(uint64_t physical)
{
    const uint64_t pages = BOAROS_PAGE_SIZE / sizeof(uint64_t);
    for (uint32_t slot = 0; slot < KERNEL_STACK_SLOT_COUNT; slot++) {
        uint64_t first = ARCH_KERNEL_STACK_WINDOW_BASE + slot * ARCH_KERNEL_STACK_SLOT_SIZE + BOAROS_PAGE_SIZE;
        if ((first / BOAROS_PAGE_SIZE) % pages == (physical / BOAROS_PAGE_SIZE) % pages)
            return slot;
    }
    __builtin_trap();
}

static void failed_mapping(void)
{
    physical_stack = (UINT64_C(0x100000) + 12) * BOAROS_PAGE_SIZE;
    kernel_stack_slots_reset();
    struct kernel_task *task = NULL;
    assert(allocate_task_storage(&task) == KERNEL_SCHEDULER_STATUS_NO_MEMORY);
    assert(!task && !metadata_owned && !stack_owned && map_calls == 1);
    uint64_t expected = ARCH_KERNEL_STACK_WINDOW_BASE +
        first_same_index(physical_stack) * ARCH_KERNEL_STACK_SLOT_SIZE + BOAROS_PAGE_SIZE;
    assert(mapped_stack == expected);
    for (unsigned word = 0; word < KERNEL_STACK_SLOT_WORDS; word++)
        assert(!kernel_stack_slot_bitmap[word]);
    map_result = RISCV_SV39_STATUS_INVALID;
    assert(allocate_task_storage(&task) == KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    assert(!task && !metadata_owned && !stack_owned && map_calls == 2 && mapped_stack == expected);
    for (unsigned word = 0; word < KERNEL_STACK_SLOT_WORDS; word++)
        assert(!kernel_stack_slot_bitmap[word]);
}

static uint32_t window_slot(uint64_t window)
{
    assert(window >= ARCH_KERNEL_STACK_WINDOW_BASE &&
        window + ARCH_KERNEL_STACK_SLOT_SIZE <= ARCH_KERNEL_STACK_WINDOW_BASE + ARCH_KERNEL_STACK_WINDOW_SIZE);
    assert((window - ARCH_KERNEL_STACK_WINDOW_BASE) % ARCH_KERNEL_STACK_SLOT_SIZE == 0);
    return (uint32_t)((window - ARCH_KERNEL_STACK_WINDOW_BASE) / ARCH_KERNEL_STACK_SLOT_SIZE);
}

static void preference_cases(void)
{
    const uint64_t pages = KERNEL_STACK_ALIAS_PAGES;
    /* Enumerate the physical order-1 colors independently of the inverse. */
    for (uint64_t page = 0; page < pages; page += KERNEL_STACK_PAGES) {
        uint64_t first, next;
        kernel_stack_slots_reset();
        assert(!kernel_stack_slot_acquire(page * BOAROS_PAGE_SIZE, &first));
        uint32_t preferred = first_same_index(page * BOAROS_PAGE_SIZE);
        assert(window_slot(first) == preferred);
        assert(!kernel_stack_slot_acquire(page * BOAROS_PAGE_SIZE, &next));
        if (preferred + pages < KERNEL_STACK_SLOT_COUNT)
            assert(window_slot(next) == preferred + pages);
        else assert(window_slot(next) == (preferred ? 0U : 1U));
        kernel_stack_slot_release(first); kernel_stack_slot_release(next);
        assert(!kernel_stack_slot_acquire(page * BOAROS_PAGE_SIZE, &next) && next == first);
        kernel_stack_slot_release(next);
    }
    uint64_t occupied[KERNEL_STACK_SLOT_COUNT], window;
    unsigned count = 0;
    uint32_t preferred = first_same_index(physical_stack);
    kernel_stack_slots_reset();
    for (uint32_t slot = preferred; slot < KERNEL_STACK_SLOT_COUNT; slot += pages) {
        assert(!kernel_stack_slot_acquire(physical_stack, &window) && window_slot(window) == slot);
        occupied[count++] = window;
    }
    assert(!kernel_stack_slot_acquire(physical_stack, &window));
    assert(window_slot(window) == (preferred ? 0U : 1U));
    kernel_stack_slot_release(window);
    while (count) kernel_stack_slot_release(occupied[--count]);
}

static void capacity_cases(void)
{
    static uint64_t windows[KERNEL_STACK_SLOT_COUNT];
    static unsigned char occupied[KERNEL_STACK_SLOT_COUNT];
    kernel_stack_slots_reset();
    for (uint32_t i = 0; i < KERNEL_STACK_SLOT_COUNT; i++) {
        assert(!kernel_stack_slot_acquire(physical_stack, &windows[i]));
        uint32_t slot = window_slot(windows[i]);
        assert(!occupied[slot]); occupied[slot] = 1;
    }
    uint64_t untouched = UINT64_MAX;
    assert(kernel_stack_slot_acquire(physical_stack, &untouched) == -1 && untouched == UINT64_MAX);
    /* Padding bits in the final RV bitmap word remain clear, never usable. */
    if (KERNEL_STACK_SLOT_COUNT % 64U)
        assert(!(kernel_stack_slot_bitmap[KERNEL_STACK_SLOT_WORDS - 1] >> (KERNEL_STACK_SLOT_COUNT % 64U)));
    struct kernel_task *task = NULL;
    unsigned before = map_calls;
    assert(allocate_task_storage(&task) == KERNEL_SCHEDULER_STATUS_NO_MEMORY);
    assert(!task && !metadata_owned && !stack_owned && map_calls == before);
    /* Free the lowest nonpreferred slot while every same-color slot stays
     * occupied; capacity must win over the placement preference. */
    uint64_t fallback = ARCH_KERNEL_STACK_WINDOW_BASE;
    assert(first_same_index(physical_stack) != 0);
    kernel_stack_slot_release(fallback);
    assert(!kernel_stack_slot_acquire(physical_stack, &untouched) && untouched == fallback);
    for (uint32_t i = KERNEL_STACK_SLOT_COUNT; i; i--) kernel_stack_slot_release(windows[i - 1]);
    for (unsigned word = 0; word < KERNEL_STACK_SLOT_WORDS; word++)
        assert(!kernel_stack_slot_bitmap[word]);
}

int main(void)
{
    kernel_cpu_initialize(&test_cpu, 0, &bootstrap_io);
    kernel_raw_lock_init(&kernel_wait_domain, KERNEL_RAW_RANK_SCHEDULER);
    scheduler.allocator = &allocator;
    failed_mapping();
    preference_cases();
    capacity_cases();
    printf("PASS stack slot page=%llu count=%u: all physical colors, next/fallback, full/release, padding and failed-map owners\n",
        (unsigned long long)BOAROS_PAGE_SIZE, KERNEL_STACK_SLOT_COUNT);
    return 0;
}
