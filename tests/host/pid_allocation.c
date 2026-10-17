#include <kernel/heap.h>
#include <kernel/pid.h>
#include <kernel/sync.h>
#include <assert.h>
#include <stdio.h>

static unsigned char pool[256 * BOAROS_PAGE_SIZE] __attribute__((aligned(BOAROS_PAGE_SIZE)));
static struct kernel_io_context io;
struct kernel_io_context *kernel_io_context_current(void) { return &io; }
void kernel_console_putc(char character) { fputc(character, stderr); }
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer, uint64_t *address)
{ *address = (uintptr_t)pointer; return 1; }

int main(void)
{
    struct physical_page_allocator allocator = {0};
    struct kernel_heap heap = {0};
    struct boot_memory_layout layout = {.usable_count = 1};
    layout.usable[0].base = (uintptr_t)pool;
    layout.usable[0].size = sizeof(pool);
    assert(physical_page_allocator_init(&allocator, &layout) == PHYSICAL_PAGE_STATUS_OK);
    assert(physical_page_allocator_bind_access(&allocator, access_page) == PHYSICAL_PAGE_STATUS_OK);
    assert(physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_OK);
    uint64_t baseline = physical_page_available(&allocator);
    assert(kernel_heap_init(&heap, &allocator, page_address) == KERNEL_HEAP_STATUS_OK);
    enum { COUNT = BOAROS_PAGE_SIZE / 128 };
    uint64_t bitmap[KERNEL_PID_BITMAP_WORDS(COUNT)];
    struct kernel_pid_allocator numbers = {0};
    struct kernel_pid_registry registry = {.numbers = &numbers, .next_generation = 1};
    struct kernel_pid *owners[COUNT];
    assert(kernel_pid_allocator_init(&numbers, bitmap, COUNT) == KERNEL_PID_STATUS_OK);
    for (unsigned i = 0; i < COUNT; i++) {
        assert(kernel_heap_allocate_zeroed(&heap, 1, sizeof(*owners[i]),
                                          (void **)&owners[i]) == KERNEL_HEAP_STATUS_OK);
        assert(kernel_pid_publish(&registry, owners[i]) == KERNEL_PID_STATUS_OK);
        assert(kernel_pid_find(&registry, owners[i]->number) == owners[i]);
    }
    struct kernel_heap_statistics stats;
    kernel_heap_get_statistics(&heap, &stats);
    fprintf(stderr, "PID records=%zu page=%u owners=%u live-pages=%llu\n",
        sizeof(struct kernel_pid), (unsigned)BOAROS_PAGE_SIZE, COUNT,
        (unsigned long long)stats.current_pages);
    /* Public allocation budget: these owners must fit in one slab page. */
    assert(stats.current_pages == 1 && stats.live_allocations == COUNT);
    for (unsigned i = 0; i < COUNT; i++) kernel_pid_put(owners[i]);
    struct kernel_pid *retired;
    unsigned released = 0;
    while ((retired = kernel_pid_take_retired(&registry))) {
        kernel_heap_release(&heap, retired);
        released++;
    }
    kernel_heap_get_statistics(&heap, &stats);
    assert(released == COUNT && !numbers.allocated && !stats.live_allocations && !stats.current_pages);
    assert(physical_page_available(&allocator) == baseline);
    assert(physical_page_allocator_audit(&allocator) == PHYSICAL_PAGE_STATUS_OK);
    puts("PID heap budget and final owner baseline passed");
}
