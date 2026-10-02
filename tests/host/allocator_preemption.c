#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/sync.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Instrument production function boundaries, not metadata layouts. At each
 * possible boundary, deliver one timer switch if IRQs permit it. The second
 * task runs a normal syscall (IRQs off), then returns to the interrupted task. */
uintptr_t allocator_test_irq_enabled = 1;
static unsigned armed, remaining, events, delivered, pending;
static void (*other_task)(void);
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static struct kernel_io_context contexts[2];
static unsigned current_task;
static unsigned char pool[64 * BOAROS_PAGE_SIZE]
    __attribute__((aligned(64 * BOAROS_PAGE_SIZE)));
static uint64_t other_page, shared_page;
static void *other_object;
static uint64_t initial_free;

struct kernel_io_context *kernel_io_context_current(void)
{ return &contexts[current_task]; }
void kernel_console_putc(char c) { fputc(c, stderr); }
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer, uint64_t *address)
{ *address = (uintptr_t)pointer; return 1; }

static void boundary(void)
{
    if (!armed) return;
    events++;
    if (remaining && !--remaining) pending = 1;
    if (!pending || !allocator_test_irq_enabled) return;
    armed = 0;
    pending = 0;
    delivered++;
    allocator_test_irq_enabled = 0;
    current_task = 1;
    other_task();
    current_task = 0;
    allocator_test_irq_enabled = 1;
}
void __cyg_profile_func_enter(void *fn, void *caller)
{ (void)fn; (void)caller; boundary(); }
void __cyg_profile_func_exit(void *fn, void *caller)
{ (void)fn; (void)caller; boundary(); }
static void setup(void)
{
    struct boot_memory_layout layout = {0};
    armed = pending = delivered = 0;
    allocator_test_irq_enabled = 1;
    current_task = 0;
    memset(contexts, 0, sizeof(contexts));
    layout.usable_count = 1;
    layout.usable[0].base = (uintptr_t)pool;
    layout.usable[0].size = sizeof(pool);
    assert(physical_page_allocator_init(&allocator, &layout) == 0);
    assert(physical_page_allocator_bind_access(&allocator, access_page) == 0);
    assert(physical_page_allocator_finalize(&allocator) == 0);
    assert(kernel_heap_init(&heap, &allocator, page_address) == 0);
    initial_free = physical_page_available(&allocator);
}
static void start(unsigned at, void (*task)(void))
{ events = 0; remaining = at; other_task = task; armed = 1; }
static unsigned finish(void)
{
    unsigned count = events;
    if (!delivered) { pending = 1; boundary(); }
    armed = 0;
    assert(delivered == 1 && allocator_test_irq_enabled == 1);
    return count;
}
static void allocate_page(void)
{ assert(physical_page_allocate(&allocator, &other_page) == 0); }
static void acquire_page(void)
{ assert(physical_page_acquire(&allocator, shared_page) == 0); }
static void allocate_object(void)
{ assert(kernel_heap_allocate(&heap, 64, &other_object) == 0); }
static void release_object(void)
{ assert(kernel_heap_release(&heap, other_object) == 0); }
static unsigned page_case(unsigned at)
{
    setup();
    uint64_t page;
    start(at, allocate_page);
    assert(physical_page_allocate(&allocator, &page) == 0);
    unsigned count = finish();
    assert(page != other_page);
    memset(access_page(page), 0x31, BOAROS_PAGE_SIZE);
    memset(access_page(other_page), 0x42, BOAROS_PAGE_SIZE);
    assert(*(unsigned char *)access_page(page) == 0x31);
    assert(physical_page_release(&allocator, page) == 0);
    assert(physical_page_release(&allocator, other_page) == 0);
    assert(physical_page_available(&allocator) == initial_free);
    return count;
}
static unsigned page_release_case(unsigned at)
{
    setup();
    uint64_t page, kept;
    assert(physical_page_allocate(&allocator, &page) == 0);
    assert(physical_page_allocate(&allocator, &kept) == 0);
    start(at, allocate_page);
    assert(physical_page_release(&allocator, page) == 0);
    unsigned count = finish();
    assert(other_page != kept);
    memset(access_page(kept), 0x31, BOAROS_PAGE_SIZE);
    memset(access_page(other_page), 0x42, BOAROS_PAGE_SIZE);
    assert(*(unsigned char *)access_page(kept) == 0x31);
    assert(physical_page_release(&allocator, kept) == 0);
    assert(physical_page_release(&allocator, other_page) == 0);
    assert(physical_page_available(&allocator) == initial_free);
    return count;
}
static unsigned reference_case(unsigned at)
{
    setup();
    assert(physical_page_allocate(&allocator, &shared_page) == 0);
    assert(physical_page_acquire(&allocator, shared_page) == 0);
    start(at, acquire_page);
    assert(physical_page_release(&allocator, shared_page) == 0);
    unsigned count = finish();
    uint32_t refs;
    assert(physical_page_reference_count(&allocator, shared_page, &refs) == 0 && refs == 2);
    assert(physical_page_release(&allocator, shared_page) == 0);
    assert(physical_page_release(&allocator, shared_page) == 0);
    assert(physical_page_available(&allocator) == initial_free);
    return count;
}
static unsigned heap_case(unsigned at, unsigned release, unsigned fresh)
{
    setup();
    void *kept, *object;
    kept = 0;
    if (!fresh) assert(kernel_heap_allocate(&heap, 64, &kept) == 0);
    if (release) assert(kernel_heap_allocate(&heap, 64, &other_object) == 0);
    start(at, release ? release_object : allocate_object);
    assert(kernel_heap_allocate(&heap, 64, &object) == 0);
    unsigned count = finish();
    assert(object != kept && (release || object != other_object));
    if (kept) memset(kept, 0x31, 64);
    memset(object, 0x42, 64);
    if (!release) memset(other_object, 0x53, 64);
    assert((!kept || *(unsigned char *)kept == 0x31) && *(unsigned char *)object == 0x42);
    assert(kernel_heap_release(&heap, kept) == 0);
    assert(kernel_heap_release(&heap, object) == 0);
    if (!release) assert(kernel_heap_release(&heap, other_object) == 0);
    struct kernel_heap_statistics stats;
    kernel_heap_get_statistics(&heap, &stats);
    assert(stats.live_allocations == 0 && stats.current_pages == 0);
    assert(physical_page_available(&allocator) == initial_free);
    return count;
}
static unsigned pressure_notified, pressure_reclaimed, pressure_waited;
static uint64_t pressure_page;
static void pressure_notify(void *context)
{
    assert(context == &allocator && !allocator_test_irq_enabled);
    pressure_notified++;
}
static uint64_t pressure_reclaim(void *context, uint64_t needed)
{
    assert(context == &allocator && needed == 1 && !allocator_test_irq_enabled);
    pressure_reclaimed++;
    uint64_t unused;
    /* 同任务递归必须返回真实 EMPTY，不能再次进入回收器。 */
    assert(physical_page_allocate(&allocator, &unused) == PHYSICAL_PAGE_STATUS_EMPTY);
    return 0;
}
static void pressure_wait(void *context)
{
    assert(context == &allocator && !allocator_test_irq_enabled);
    assert(contexts[0].reclaim_depth == 0);
    pressure_waited++;
    /* 模拟 callback 持有 group 引用后显式睡眠；其他任务可正常分配、
     * 释放并注销回收器。返回后分配器不再解引用旧 callback context。 */
    current_task = 1;
    assert(physical_page_release(&allocator, pressure_page) == 0);
    uint64_t temporary;
    assert(physical_page_allocate(&allocator, &temporary) == 0);
    assert(physical_page_release(&allocator, temporary) == 0);
    assert(physical_page_allocator_clear_reclaimer(&allocator) == 0);
    allocator.pressure_notify = 0;
    allocator.pressure_wait = 0;
    allocator.pressure_context = 0;
    current_task = 0;
}
static uint32_t *reclaim_depth(void) { return &contexts[current_task].reclaim_depth; }
static void pressure_case(uintptr_t enabled)
{
    setup();
    uint64_t pages[64], count = 0;
    while (physical_page_available(&allocator)) {
        assert(count < 64);
        assert(physical_page_allocate(&allocator, &pages[count++]) == 0);
    }
    pressure_page = pages[--count];
    assert(physical_page_allocator_set_reclaimer(&allocator, pressure_reclaim, &allocator) == 0);
    allocator.reclaim_depth = reclaim_depth;
    allocator.pressure_context = &allocator;
    allocator.pressure_notify = pressure_notify;
    allocator.pressure_wait = pressure_wait;
    pressure_notified = pressure_reclaimed = pressure_waited = 0;
    allocator_test_irq_enabled = enabled;
    uint64_t page;
    assert(physical_page_allocate(&allocator, &page) == 0);
    assert(allocator_test_irq_enabled == enabled);
    assert(pressure_reclaimed == 1 && pressure_waited == 1 && pressure_notified >= 2);
    assert(physical_page_release(&allocator, page) == 0);
    while (count) assert(physical_page_release(&allocator, pages[--count]) == 0);
    assert(physical_page_available(&allocator) == initial_free);
    allocator_test_irq_enabled = 1;
}
static void irq_restore_case(uintptr_t enabled)
{
    setup();
    allocator_test_irq_enabled = enabled;
    uint64_t page = UINT64_MAX;
    assert(physical_page_allocate_order(&allocator, PHYSICAL_PAGE_MAX_ORDER + 1, &page) == PHYSICAL_PAGE_STATUS_INVALID);
    assert(page == UINT64_MAX && allocator_test_irq_enabled == enabled);
    void *object = (void *)(uintptr_t)1;
    assert(kernel_heap_allocate_zeroed(&heap, SIZE_MAX, 2, &object) == KERNEL_HEAP_STATUS_OVERFLOW);
    assert(object == (void *)(uintptr_t)1 && allocator_test_irq_enabled == enabled);
    assert(kernel_heap_allocate(&heap, 64, &object) == 0);
    assert(allocator_test_irq_enabled == enabled);
    assert(kernel_heap_release(&heap, object) == 0);
    assert(allocator_test_irq_enabled == enabled);
    assert(physical_page_available(&allocator) == initial_free);
    allocator_test_irq_enabled = 1;
}
int main(void)
{
    unsigned n = page_case(0);
    for (unsigned i = 1; i <= n; i++) page_case(i);
    printf("page ownership: %u timer boundaries\n", n);
    n = page_release_case(0);
    for (unsigned i = 1; i <= n; i++) page_release_case(i);
    printf("page coalescing: %u timer boundaries\n", n);
    n = reference_case(0);
    for (unsigned i = 1; i <= n; i++) reference_case(i);
    printf("page references: %u timer boundaries\n", n);
    for (unsigned release = 0; release < 2; release++) {
        n = heap_case(0, release, 0);
        for (unsigned i = 1; i <= n; i++) heap_case(i, release, 0);
        printf("heap ownership (%u): %u timer boundaries\n", release, n);
    }
    n = heap_case(0, 0, 1);
    for (unsigned i = 1; i <= n; i++) heap_case(i, 0, 1);
    printf("private slab publication: %u timer boundaries\n", n);
    pressure_case(1);
    pressure_case(0);
    irq_restore_case(1);
    irq_restore_case(0);
    puts("allocator preemption contracts passed");
}
