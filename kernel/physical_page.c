#include <kernel/cost.h>
#include <kernel/irq.h>
#include <kernel/console.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>
#include <string.h>

/* 宿主/无终端平台可没有该hook；已有同步sink仍可输出。 */
extern void kernel_console_emergency_begin(void) __attribute__((weak));

#define PHYSICAL_PAGE_ALLOCATOR_INITIALIZED UINT32_C(0x50414745)
#define PHYSICAL_PAGE_ALLOCATOR_FINALIZED UINT32_C(0x42554459)
#define PHYSICAL_PAGE_NONE UINT64_MAX
#define PHYSICAL_PAGE_INDEX_NONE UINT32_MAX

static void note_allocated_peak(struct physical_page_allocator *allocator)
{
#if BOAROS_COST_DIAGNOSTICS
    uint64_t current = allocator->total_pages - allocator->available_pages;
    if (current > allocator->allocated_peak_pages) allocator->allocated_peak_pages = current;
#else
    (void)allocator;
#endif
}

enum physical_page_state {
    PHYSICAL_PAGE_STATE_ALLOCATED_HEAD = 0,
    PHYSICAL_PAGE_STATE_ALLOCATED_TAIL,
    PHYSICAL_PAGE_STATE_CANDIDATE,
    PHYSICAL_PAGE_STATE_FREE_HEAD,
    PHYSICAL_PAGE_STATE_FREE_TAIL,
    PHYSICAL_PAGE_STATE_INTERNAL,
};

struct physical_page_metadata {
    uint32_t next;
    uint32_t previous;
    uint32_t reference_count;
    uint8_t order;
    uint8_t state;
    uint16_t reserved;
};

struct physical_page_root
{
    uint64_t address;
    uint64_t node_offset;
    uint32_t first_page_index;
    uint32_t order;
    uint32_t range_index;
    uint32_t reserved;
};

enum buddy_state
{
    BUDDY_INACTIVE = 0,
    BUDDY_FREE,
    BUDDY_SPLIT,
    BUDDY_ALLOCATED,
    BUDDY_INTERNAL
};

struct page_work
{
    uintptr_t interrupts;
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_tag tag;
    uint64_t start, checked, written;
#endif
};

static struct page_work page_work_begin(void)
{
    struct page_work work = {0};
    work.interrupts = arch_interrupt_save();
#if BOAROS_COST_DIAGNOSTICS
    work.tag = kernel_cost_capture();
    work.start = kernel_cost_clock();
#endif
    return work;
}

static void page_work_end(struct page_work *work)
{
#if BOAROS_COST_DIAGNOSTICS
    uint64_t elapsed = kernel_cost_clock() - work->start;
#endif
    /* 先结束元数据计时并恢复IRQ，集中发布不能扩张每条记录的临界区。 */
    arch_interrupt_restore(work->interrupts);
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_add_tag(work->tag, COST_PAGE_META_CHECKED, work->checked);
    kernel_cost_add_tag(work->tag, COST_PAGE_META_WRITTEN, work->written);
    kernel_cost_add_tag(work->tag, COST_ALLOCATOR_META_TICKS, elapsed);
#endif
}
#define PAGE_METADATA_SCOPE(name)                                                      \
    struct page_work name __attribute__((cleanup(page_work_end))) = page_work_begin()

static void page_checked(struct page_work *work)
{
#if BOAROS_COST_DIAGNOSTICS
    if (work)
        work->checked++;
#else
    (void)work;
#endif
}
static void page_written(struct page_work *work)
{
#if BOAROS_COST_DIAGNOSTICS
    if (work)
        work->written++;
#else
    (void)work;
#endif
}

_Static_assert(sizeof(struct physical_page_metadata) == 16U,
               "physical page metadata must remain compact");

static const struct physical_page_root *
root_for_page(const struct physical_page_allocator *, uint32_t, struct page_work *);
static const struct physical_page_root *
root_for_address(const struct physical_page_allocator *, uint64_t, struct page_work *);

static int allocator_initialized(
    const struct physical_page_allocator *allocator)
{
    return allocator != 0 &&
           allocator->initialized == PHYSICAL_PAGE_ALLOCATOR_INITIALIZED;
}

int physical_page_allocator_is_finalized(
    const struct physical_page_allocator *allocator)
{
    return allocator_initialized(allocator) &&
           allocator->finalized == PHYSICAL_PAGE_ALLOCATOR_FINALIZED;
}

static int range_end(uint64_t base, uint64_t size, uint64_t *end)
{
    if (size > UINT64_MAX - base) {
        return 0;
    }

    *end = base + size;
    return 1;
}

static int address_was_allocated(
    const struct physical_page_allocator *allocator,
    uint64_t address)
{
    uint32_t index;

    if ((address & BOAROS_PAGE_MASK) != 0U) {
        return 0;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        const struct physical_page_range *range = &allocator->ranges[index];

        if (address >= range->base && address < range->next) {
            return 1;
        }
    }

    return 0;
}

static int page_lookup_work(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t *range_index,
    uint32_t *page_index, struct page_work *work)
{
    uint32_t index;

    if ((address & BOAROS_PAGE_MASK) != 0U) {
        return 0;
    }

    if (physical_page_allocator_is_finalized(allocator)) {
        const struct physical_page_root *root = root_for_address(allocator, address, work);
        if (!root) return 0;
        if (range_index) *range_index = root->range_index;
        if (page_index) *page_index = root->first_page_index + (uint32_t)((address-root->address)>>BOAROS_PAGE_SHIFT);
        return 1;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        const struct physical_page_range *range = &allocator->ranges[index];

        if (address >= range->base && address < range->end) {
            uint64_t local = (address - range->base) >> BOAROS_PAGE_SHIFT;
            uint64_t flat = (uint64_t)range->first_page_index + local;

            if (flat >= allocator->total_pages || flat > UINT32_MAX) {
                return 0;
            }
            if (range_index != 0) {
                *range_index = index;
            }
            if (page_index != 0) {
                *page_index = (uint32_t)flat;
            }
            return 1;
        }
    }

    return 0;
}

static int page_lookup(const struct physical_page_allocator *a, uint64_t address,
                       uint32_t *range, uint32_t *page)
{
    return page_lookup_work(a,address,range,page,0);
}

static void release_diagnostic_text(const char *text)
{
    while (*text != '\0') {
        kernel_console_putc(*text++);
    }
}

static void release_diagnostic_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[sizeof(value) * 2U];
    uint32_t length = 0U;

    release_diagnostic_text("0x");
    do {
        buffer[length++] = digits[value & 0xfU];
        value >>= 4U;
    } while (value != 0U);
    while (length != 0U) {
        kernel_console_putc(buffer[--length]);
    }
}

static void physical_page_release_fatal(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t order,
    const char *reason,
    uint64_t metadata_address) __attribute__((cold, noreturn));

static void physical_page_release_fatal(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t order,
    const char *reason,
    uint64_t metadata_address)
{
    uint32_t page_index;

    /* Fatal 输出只走同步硬件 sink，不分配、回收或唤醒日志等待者。 */
    if (kernel_console_emergency_begin) kernel_console_emergency_begin();
    release_diagnostic_text("BoarOS: physical page release fatal reason=");
    release_diagnostic_text(reason);
    release_diagnostic_text(" address=");
    release_diagnostic_hex(address);
    release_diagnostic_text(" requested_order=");
    release_diagnostic_hex(order);
    if (allocator_initialized(allocator)) {
        release_diagnostic_text(" available=");
        release_diagnostic_hex(allocator->available_pages);
        release_diagnostic_text(" total=");
        release_diagnostic_hex(allocator->total_pages);
    }
    /* 非法地址及 bootstrap 没有可信 metadata；不追踪 next/previous 指针。 */
    if (physical_page_allocator_is_finalized(allocator) &&
        allocator->metadata != 0 &&
        page_lookup(allocator, metadata_address, 0, &page_index)) {
        const struct physical_page_metadata *metadata =
            &allocator->metadata[page_index];

        release_diagnostic_text(" metadata_address=");
        release_diagnostic_hex(metadata_address);
        release_diagnostic_text(" page_index=");
        release_diagnostic_hex(page_index);
        release_diagnostic_text(" state=");
        release_diagnostic_hex(metadata->state);
        release_diagnostic_text(" stored_order=");
        release_diagnostic_hex(metadata->order);
        release_diagnostic_text(" references=");
        release_diagnostic_hex(metadata->reference_count);
        release_diagnostic_text(" next=");
        release_diagnostic_hex(metadata->next);
        release_diagnostic_text(" previous=");
        release_diagnostic_hex(metadata->previous);
        release_diagnostic_text(" reserved=");
        release_diagnostic_hex(metadata->reserved);
    }
    kernel_console_putc('\n');
    __builtin_trap();
}

static int read_recycled_node(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint64_t *value)
{
    const unsigned char *bytes = allocator->access(address);
    uint64_t result = 0U;
    uint32_t index;

    if (bytes == 0) {
        return 0;
    }
    for (index = 0U; index < sizeof(result); index++) {
        result |= (uint64_t)bytes[index] << (index * 8U);
    }

    *value = result;
    return 1;
}

static int write_recycled_node(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint64_t value)
{
    unsigned char *bytes = allocator->access(address);
    uint32_t index;

    if (bytes == 0) {
        return 0;
    }
    for (index = 0U; index < sizeof(value); index++) {
        bytes[index] = (unsigned char)(value >> (index * 8U));
    }

    return 1;
}

enum physical_page_status physical_page_allocator_init(
    struct physical_page_allocator *allocator,
    const struct boot_memory_layout *layout)
{
    struct physical_page_allocator result;
    uint64_t previous_end = 0U;
    uint32_t index;

    if (allocator == 0 || layout == 0 || layout->usable_count == 0U ||
        layout->usable_count > BOOT_MEMORY_MAX_USABLE_RANGES) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    result.pressure_notify = 0;
    result.pressure_wait = 0;
    result.pressure_context = 0;
    result.shared_anon_pages = 0;
    result.cache_snapshot = 0;
    result.cache_context = 0;
    result.buffer_bytes = 0;
    result.buffer_context = 0;
    result.total_pages = 0U;
    result.available_pages = 0U;
#if BOAROS_COST_DIAGNOSTICS
    result.allocated_peak_pages = 0;
#endif
    result.recycled_head = PHYSICAL_PAGE_NONE;
    result.metadata_address = PHYSICAL_PAGE_NONE;
    result.metadata_pages = 0U;
    result.range_count = 0U;
    result.initialized = 0U;
    result.finalized = 0U;
    result.reclaiming = 0U;
    result.reclaim_depth = 0;
    result.access = 0;
    result.reclaimer = 0;
    result.reclaimer_context = 0;
    result.metadata = 0;
    result.roots = 0;
    result.tree = 0;
    result.tree_nodes = 0;
    result.root_count = 0;
    for (index = 0U; index <= PHYSICAL_PAGE_MAX_ORDER; index++) {
        result.free_heads[index] = PHYSICAL_PAGE_INDEX_NONE;
    }
    for (index = 0U; index < BOOT_MEMORY_MAX_USABLE_RANGES; index++) {
        result.ranges[index].base = 0U;
        result.ranges[index].next = 0U;
        result.ranges[index].end = 0U;
        result.ranges[index].first_page_index = 0U;
    }

    for (index = 0U; index < layout->usable_count; index++) {
        uint64_t base = layout->usable[index].base;
        uint64_t size = layout->usable[index].size;
        uint64_t end;
        uint64_t aligned_base;
        uint64_t aligned_end;
        uint64_t page_count;

        if (size == 0U || !range_end(base, size, &end) ||
            (index > 0U && base < previous_end)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        previous_end = end;

        if (base > UINT64_MAX - BOAROS_PAGE_MASK) {
            continue;
        }
        aligned_base = (base + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
        aligned_end = end & ~BOAROS_PAGE_MASK;
        if (aligned_base >= aligned_end) {
            continue;
        }

        page_count = (aligned_end - aligned_base) >> BOAROS_PAGE_SHIFT;
        if (page_count > UINT32_MAX - result.total_pages) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }

        result.ranges[result.range_count].base = aligned_base;
        result.ranges[result.range_count].next = aligned_base;
        result.ranges[result.range_count].end = aligned_end;
        result.ranges[result.range_count].first_page_index =
            (uint32_t)result.total_pages;
        result.range_count++;
        result.total_pages += page_count;
    }

    if (result.total_pages == 0U) {
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }

    result.available_pages = result.total_pages;
    result.initialized = PHYSICAL_PAGE_ALLOCATOR_INITIALIZED;
    *allocator = result;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocator_bind_access(
    struct physical_page_allocator *allocator,
    physical_page_access_fn access)
{
    if (!allocator_initialized(allocator) || access == 0) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->access != 0) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    allocator->access = access;
    return PHYSICAL_PAGE_STATUS_OK;
}

static enum physical_page_status physical_page_allocate_bootstrap(
    struct physical_page_allocator *allocator,
    uint64_t *address)
{
    KERNEL_IRQ_SCOPE(irq);
    uint32_t index;

    if (allocator->available_pages == 0U) {
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }

    if (allocator->recycled_head != PHYSICAL_PAGE_NONE) {
        uint64_t result = allocator->recycled_head;
        uint64_t next;

        if (allocator->access == 0) {
            return PHYSICAL_PAGE_STATUS_STATE;
        }
        if (!address_was_allocated(allocator, result)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        if (!read_recycled_node(allocator, result, &next)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        if (next != PHYSICAL_PAGE_NONE &&
            !address_was_allocated(allocator, next)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }

        allocator->recycled_head = next;
        allocator->available_pages--;
        note_allocated_peak(allocator);
        *address = result;
        return PHYSICAL_PAGE_STATUS_OK;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        struct physical_page_range *range = &allocator->ranges[index];

        if (range->next < range->end) {
            uint64_t result = range->next;

            range->next += BOAROS_PAGE_SIZE;
            allocator->available_pages--;
            note_allocated_peak(allocator);
            *address = result;
            return PHYSICAL_PAGE_STATUS_OK;
        }
    }

    return PHYSICAL_PAGE_STATUS_INVALID;
}

static enum physical_page_status physical_page_release_bootstrap(
    struct physical_page_allocator *allocator,
    uint64_t address)
{
    KERNEL_IRQ_SCOPE(irq);
    uint64_t current;
    uint64_t scanned = 0U;

    if (!address_was_allocated(allocator, address)) {
        physical_page_release_fatal(allocator, address, 0U,
                                    "bootstrap-address", address);
    }
    if (allocator->access == 0) {
        physical_page_release_fatal(allocator, address, 0U,
                                    "bootstrap-access", address);
    }

    current = allocator->recycled_head;
    while (current != PHYSICAL_PAGE_NONE &&
           scanned < allocator->total_pages) {
        if (current == address) {
            physical_page_release_fatal(allocator, address, 0U,
                                        "bootstrap-already-free", current);
        }
        if (!address_was_allocated(allocator, current)) {
            physical_page_release_fatal(allocator, address, 0U,
                                        "bootstrap-chain-address", current);
        }
        if (!read_recycled_node(allocator, current, &current)) {
            physical_page_release_fatal(allocator, address, 0U,
                                        "bootstrap-chain-access", current);
        }
        scanned++;
    }
    if (current != PHYSICAL_PAGE_NONE) {
        physical_page_release_fatal(allocator, address, 0U,
                                    "bootstrap-chain-cycle", current);
    }
    if (allocator->available_pages >= allocator->total_pages) {
        physical_page_release_fatal(allocator, address, 0U,
                                    "bootstrap-available-count", address);
    }

    if (!write_recycled_node(allocator, address, allocator->recycled_head)) {
        physical_page_release_fatal(allocator, address, 0U,
                                    "bootstrap-release-access", address);
    }
    allocator->recycled_head = address;
    allocator->available_pages++;
    return PHYSICAL_PAGE_STATUS_OK;
}

static uint64_t order_page_count(uint32_t order)
{
    return UINT64_C(1) << order;
}

static uint32_t largest_fitting_order(uint64_t address, uint64_t pages)
{
    uint32_t order = PHYSICAL_PAGE_MAX_ORDER;
    while (order && ((address & ((order_page_count(order) << BOAROS_PAGE_SHIFT) - 1)) ||
                     order_page_count(order) > pages))
        order--;
    return order;
}

static int root_valid(const struct physical_page_allocator *a,
                      const struct physical_page_root *r)
{
    if (r->order > PHYSICAL_PAGE_MAX_ORDER || r->reserved ||
        r->range_index >= a->range_count)
        return 0;
    const struct physical_page_range *range = &a->ranges[r->range_index];
    uint64_t n = order_page_count(r->order), bytes = n << BOAROS_PAGE_SHIFT;
    uint64_t range_pages = (range->end - range->base) >> BOAROS_PAGE_SHIFT;
    if (r->first_page_index < range->first_page_index || n > a->total_pages ||
        n > range_pages ||
        (uint64_t)r->first_page_index - range->first_page_index > range_pages - n ||
        r->first_page_index > a->total_pages - n ||
        r->address !=
            range->base + (((uint64_t)r->first_page_index - range->first_page_index)
                           << BOAROS_PAGE_SHIFT) ||
        (r->address & (bytes - 1)) || r->address >= range->end ||
        bytes > range->end - r->address || r->node_offset > a->tree_nodes ||
        2 * n - 1 > a->tree_nodes - r->node_offset)
        return 0;
    return 1;
}

static const struct physical_page_root *
root_for_page(const struct physical_page_allocator *a, uint32_t page,
              struct page_work *work)
{
    uint32_t lo = 0, hi = a->root_count;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct physical_page_root *r = &a->roots[mid];
        page_checked(work);
        if (!root_valid(a, r))
            __builtin_trap();
        if (page < r->first_page_index)
            hi = mid;
        else if ((uint64_t)page - r->first_page_index >= order_page_count(r->order))
            lo = mid + 1;
        else
            return r;
    }
    return 0;
}

static const struct physical_page_root *
root_for_address(const struct physical_page_allocator *a, uint64_t address,
                 struct page_work *work)
{
    uint32_t lo = 0, hi = a->root_count;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct physical_page_root *r = &a->roots[mid];
        page_checked(work);
        if (!root_valid(a, r))
            __builtin_trap();
        if (address < r->address)
            hi = mid;
        else if (address - r->address >=
                 (order_page_count(r->order) << BOAROS_PAGE_SHIFT))
            lo = mid + 1;
        else
            return r;
    }
    return 0;
}

static unsigned tree_get(const struct physical_page_allocator *a,
                         const struct physical_page_root *root, uint64_t node,
                         struct page_work *work)
{
    uint64_t index = root->node_offset + node;
    if (node >= 2 * order_page_count(root->order) - 1 || index >= a->tree_nodes)
        __builtin_trap();
    uint64_t bit = index * 3, byte = bit / 8;
    unsigned shift = bit % 8, value = a->tree[byte];
    if (shift > 5)
        value |= (unsigned)a->tree[byte + 1] << 8;
    page_checked(work);
    return (value >> shift) & 7U;
}

static void tree_set(struct physical_page_allocator *a,
                     const struct physical_page_root *root, uint64_t node,
                     unsigned state, struct page_work *work)
{
    uint64_t index = root->node_offset + node;
    if (state > BUDDY_INTERNAL || node >= 2 * order_page_count(root->order) - 1 ||
        index >= a->tree_nodes)
        __builtin_trap();
    uint64_t bit = index * 3, byte = bit / 8;
    unsigned shift = bit % 8, value = a->tree[byte];
    if (shift > 5)
        value |= (unsigned)a->tree[byte + 1] << 8;
    value = (value & ~(7U << shift)) | state << shift;
    a->tree[byte] = value;
    if (shift > 5)
        a->tree[byte + 1] = value >> 8;
    page_written(work);
}

struct buddy_owner
{
    const struct physical_page_root *root;
    uint64_t node;
    uint32_t first, order;
    unsigned state;
};

static int owner_find(const struct physical_page_allocator *a, uint32_t page,
                      struct buddy_owner *owner, struct page_work *work)
{
    const struct physical_page_root *r = root_for_page(a, page, work);
    if (!r)
        return 0;
    *owner = (struct buddy_owner){r, 0, r->first_page_index, r->order, 0};
    for (;;)
    {
        owner->state = tree_get(a, r, owner->node, work);
        if (owner->state == BUDDY_SPLIT)
        {
            if (!owner->order)
                return 0;
            owner->order--;
            unsigned right =
                (uint64_t)page - owner->first >= order_page_count(owner->order);
            owner->node = 2 * owner->node + 1 + right;
            if (right)
                owner->first += (uint32_t)order_page_count(owner->order);
        }
        else
            return owner->state >= BUDDY_FREE && owner->state <= BUDDY_INTERNAL;
    }
}

static uint64_t owner_address(const struct buddy_owner *owner)
{
    return owner->root->address +
           (((uint64_t)owner->first - owner->root->first_page_index)
            << BOAROS_PAGE_SHIFT);
}

static void mark_head(struct physical_page_allocator *a, uint32_t page, unsigned order,
                      enum physical_page_state state, struct page_work *work)
{
    a->metadata[page] = (struct physical_page_metadata){
        PHYSICAL_PAGE_INDEX_NONE,
        PHYSICAL_PAGE_INDEX_NONE,
        state == PHYSICAL_PAGE_STATE_ALLOCATED_HEAD ? 1U : 0U,
        order,
        state,
        0};
    page_written(work);
}

static int head_valid(const struct physical_page_allocator *a,
                      const struct buddy_owner *owner, struct page_work *work)
{
    const struct physical_page_metadata *m = &a->metadata[owner->first];
    page_checked(work);
    if (m->order != owner->order || m->reserved)
        return 0;
    if (owner->state == BUDDY_FREE)
        return m->state == PHYSICAL_PAGE_STATE_FREE_HEAD && !m->reference_count;
    if (m->next != PHYSICAL_PAGE_INDEX_NONE || m->previous != PHYSICAL_PAGE_INDEX_NONE)
        return 0;
    if (owner->state == BUDDY_INTERNAL)
        return m->state == PHYSICAL_PAGE_STATE_INTERNAL && !m->reference_count;
    return owner->state == BUDDY_ALLOCATED &&
           m->state == PHYSICAL_PAGE_STATE_ALLOCATED_HEAD && m->reference_count &&
           (!owner->order || m->reference_count == 1);
}

static int free_head_plain(const struct physical_page_allocator *a, uint32_t page,
                           uint32_t order, struct page_work *work)
{
    struct buddy_owner owner;
    return owner_find(a, page, &owner, work) && owner.first == page &&
           owner.order == order && owner.state == BUDDY_FREE &&
           head_valid(a, &owner, work);
}

static int free_head_valid(const struct physical_page_allocator *a,
                           const struct buddy_owner *owner, struct page_work *work)
{
    if (owner->state != BUDDY_FREE || !head_valid(a, owner, work))
        return 0;
    uint32_t page = owner->first, order = owner->order;
    const struct physical_page_metadata *m = &a->metadata[page];
    if (m->previous == PHYSICAL_PAGE_INDEX_NONE)
    {
        page_checked(work);
        if (a->free_heads[order] != page)
            return 0;
    }
    else if (m->previous == page || !free_head_plain(a, m->previous, order, work) ||
             a->metadata[m->previous].next != page)
        return 0;
    if (m->next != PHYSICAL_PAGE_INDEX_NONE &&
        (m->next == page || !free_head_plain(a, m->next, order, work) ||
         a->metadata[m->next].previous != page))
        return 0;
    return 1;
}

/* 调用者持有IRQ元数据区且已检查该head/两邻居，摘链不重复走owner路径。 */
static void free_remove(struct physical_page_allocator *a, uint32_t page,
                        struct page_work *work)
{
    struct physical_page_metadata *m = &a->metadata[page];
    uint32_t previous = m->previous, next = m->next;
    if (previous == PHYSICAL_PAGE_INDEX_NONE)
    {
        a->free_heads[m->order] = next;
        page_written(work);
    }
    else
    {
        a->metadata[previous].next = next;
        page_written(work);
    }
    if (next != PHYSICAL_PAGE_INDEX_NONE)
    {
        a->metadata[next].previous = previous;
        page_written(work);
    }
    m->next = m->previous = PHYSICAL_PAGE_INDEX_NONE;
    page_written(work);
}

static int free_insert(struct physical_page_allocator *a, uint32_t page, uint32_t order,
                       struct page_work *work)
{
    page_checked(work);
    uint32_t old = a->free_heads[order];
    if (old != PHYSICAL_PAGE_INDEX_NONE)
    {
        struct buddy_owner owner;
        if (!owner_find(a, old, &owner, work) || owner.first != old ||
            owner.order != order || !free_head_valid(a, &owner, work) ||
            a->metadata[old].previous != PHYSICAL_PAGE_INDEX_NONE)
            return 0;
    }
    struct physical_page_metadata *m = &a->metadata[page];
    m->next = old;
    m->previous = PHYSICAL_PAGE_INDEX_NONE;
    page_written(work);
    if (old != PHYSICAL_PAGE_INDEX_NONE)
    {
        a->metadata[old].previous = page;
        page_written(work);
    }
    a->free_heads[order] = page;
    page_written(work);
    return 1;
}

/* 仅一次finalize私有构建可以读取逐页导入状态，运行期不扫描旧尾载荷。 */
static unsigned build_tree(struct physical_page_allocator *a,
                           const struct physical_page_root *r, uint64_t node,
                           uint32_t page, unsigned order)
{
    unsigned state;
    if (!order)
    {
        switch (a->metadata[page].state)
        {
        case PHYSICAL_PAGE_STATE_CANDIDATE:
            state = BUDDY_FREE;
            break;
        case PHYSICAL_PAGE_STATE_INTERNAL:
            state = BUDDY_INTERNAL;
            break;
        default:
            state = BUDDY_ALLOCATED;
            break;
        }
    }
    else
    {
        unsigned left = build_tree(a, r, 2 * node + 1, page, order - 1);
        unsigned right =
            build_tree(a, r, 2 * node + 2, page + (uint32_t)order_page_count(order - 1),
                       order - 1);
        if (left == right && (left == BUDDY_FREE || left == BUDDY_INTERNAL))
        {
            tree_set(a, r, 2 * node + 1, BUDDY_INACTIVE, 0);
            tree_set(a, r, 2 * node + 2, BUDDY_INACTIVE, 0);
            state = left;
        }
        else
            state = BUDDY_SPLIT;
    }
    tree_set(a, r, node, state, 0);
    if (state != BUDDY_SPLIT)
        mark_head(a, page, order,
                  state == BUDDY_FREE       ? PHYSICAL_PAGE_STATE_FREE_HEAD
                  : state == BUDDY_INTERNAL ? PHYSICAL_PAGE_STATE_INTERNAL
                                            : PHYSICAL_PAGE_STATE_ALLOCATED_HEAD,
                  0);
    return state;
}

static int publish_free_heads(struct physical_page_allocator *a,
                              const struct physical_page_root *r, uint64_t node,
                              uint32_t page, unsigned order, uint64_t *free_pages)
{
    unsigned state = tree_get(a, r, node, 0);
    if (state == BUDDY_SPLIT)
        return order &&
               publish_free_heads(a, r, 2 * node + 1, page, order - 1, free_pages) &&
               publish_free_heads(a, r, 2 * node + 2,
                                  page + (uint32_t)order_page_count(order - 1),
                                  order - 1, free_pages);
    if (state == BUDDY_FREE)
    {
        if (!free_insert(a, page, order, 0))
            return 0;
        *free_pages += order_page_count(order);
    }
    return state >= BUDDY_FREE && state <= BUDDY_INTERNAL;
}

enum physical_page_status
physical_page_allocator_audit(const struct physical_page_allocator *a)
{
    if (!allocator_initialized(a))
        return PHYSICAL_PAGE_STATUS_INVALID;
    if (!physical_page_allocator_is_finalized(a))
        return PHYSICAL_PAGE_STATUS_STATE;
    if (!a->roots || !a->tree || !a->metadata || !a->root_count)
        __builtin_trap();
    uint64_t free_pages = 0, covered = 0, nodes = 0, internal = 0;
    uint64_t free_by_order[PHYSICAL_PAGE_MAX_ORDER + 1] = {0};
    uint32_t expected_range = 0;
    uint64_t expected_address = a->ranges[0].base;
    struct audit_frame
    {
        uint64_t node;
        uint32_t page, order;
        unsigned active;
    };
    struct audit_frame stack[PHYSICAL_PAGE_MAX_ORDER + 1];
    for (uint32_t i = 0; i < a->root_count; i++)
    {
        const struct physical_page_root *r = &a->roots[i];
        if (!root_valid(a, r) || r->first_page_index != covered ||
            r->node_offset != nodes || r->range_index != expected_range ||
            r->address != expected_address)
            __builtin_trap();
        unsigned depth = 1;
        stack[0] = (struct audit_frame){0, r->first_page_index, r->order, 1};
        while (depth)
        {
            struct audit_frame f = stack[--depth];
            unsigned state = tree_get(a, r, f.node, 0);
            if ((!f.active && state != BUDDY_INACTIVE) ||
                (f.active && (state < BUDDY_FREE || state > BUDDY_INTERNAL)) ||
                (state == BUDDY_SPLIT && !f.order))
                __builtin_trap();
            if (f.active && state != BUDDY_SPLIT)
            {
                struct buddy_owner o = {r, f.node, f.page, f.order, state};
                if (!head_valid(a, &o, 0))
                    __builtin_trap();
                if (state == BUDDY_FREE)
                {
                    if (!free_head_valid(a, &o, 0))
                        __builtin_trap();
                    free_pages += order_page_count(f.order);
                    free_by_order[f.order]++;
                }
                else if (state == BUDDY_INTERNAL)
                {
                    uint64_t address = owner_address(&o),
                             bytes = order_page_count(f.order) << BOAROS_PAGE_SHIFT;
                    if (address < a->metadata_address ||
                        address - a->metadata_address >
                            (a->metadata_pages << BOAROS_PAGE_SHIFT) ||
                        bytes > (a->metadata_pages << BOAROS_PAGE_SHIFT) -
                                    (address - a->metadata_address))
                        __builtin_trap();
                    internal += order_page_count(f.order);
                }
                else
                {
                    uint64_t address = owner_address(&o),
                             bytes = order_page_count(f.order) << BOAROS_PAGE_SHIFT;
                    if (address < a->metadata_address +
                                      (a->metadata_pages << BOAROS_PAGE_SHIFT) &&
                        address + bytes > a->metadata_address)
                        __builtin_trap();
                }
            }
            if (f.order)
            {
                if (depth + 2 > PHYSICAL_PAGE_MAX_ORDER + 1)
                    __builtin_trap();
                unsigned active = f.active && state == BUDDY_SPLIT;
                stack[depth++] = (struct audit_frame){
                    2 * f.node + 2, f.page + (uint32_t)order_page_count(f.order - 1),
                    f.order - 1, active};
                stack[depth++] =
                    (struct audit_frame){2 * f.node + 1, f.page, f.order - 1, active};
            }
        }
        covered += order_page_count(r->order);
        nodes += 2 * order_page_count(r->order) - 1;
        expected_address += order_page_count(r->order) << BOAROS_PAGE_SHIFT;
        if (expected_address == a->ranges[expected_range].end && i + 1 < a->root_count)
        {
            if (++expected_range >= a->range_count)
                __builtin_trap();
            expected_address = a->ranges[expected_range].base;
        }
    }
    if (covered != a->total_pages || nodes != a->tree_nodes ||
        free_pages != a->available_pages || internal != a->metadata_pages ||
        expected_range + 1 != a->range_count ||
        expected_address != a->ranges[expected_range].end)
        __builtin_trap();
    for (unsigned order = 0; order <= PHYSICAL_PAGE_MAX_ORDER; order++)
    {
        uint32_t page = a->free_heads[order], previous = PHYSICAL_PAGE_INDEX_NONE;
        uint64_t visited = 0;
        while (page != PHYSICAL_PAGE_INDEX_NONE)
        {
            if (++visited > free_by_order[order] ||
                !free_head_plain(a, page, order, 0) ||
                a->metadata[page].previous != previous)
                __builtin_trap();
            previous = page;
            page = a->metadata[page].next;
        }
        if (visited != free_by_order[order])
            __builtin_trap();
    }
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status
physical_page_allocator_finalize(struct physical_page_allocator *allocator)
{
    if (!allocator_initialized(allocator))
        return PHYSICAL_PAGE_STATUS_INVALID;
    if (physical_page_allocator_is_finalized(allocator) || !allocator->access)
        return PHYSICAL_PAGE_STATUS_STATE;
    if (allocator->total_pages > UINT32_MAX)
        return PHYSICAL_PAGE_STATUS_INVALID;
    uint64_t nodes = 0, root_count = 0;
    for (uint32_t i = 0; i < allocator->range_count; i++)
    {
        const struct physical_page_range *r = &allocator->ranges[i];
        for (uint64_t address = r->base; address < r->end;)
        {
            unsigned order =
                largest_fitting_order(address, (r->end - address) >> BOAROS_PAGE_SHIFT);
            nodes += 2 * order_page_count(order) - 1;
            root_count++;
            address += order_page_count(order) << BOAROS_PAGE_SHIFT;
        }
    }
    if (root_count > UINT32_MAX || nodes > (UINT64_MAX - 7) / 3 ||
        root_count > UINT64_MAX / sizeof(struct physical_page_root) ||
        allocator->total_pages > UINT64_MAX / sizeof(struct physical_page_metadata))
        return PHYSICAL_PAGE_STATUS_INVALID;
    uint64_t payload = allocator->total_pages * sizeof(struct physical_page_metadata);
    uint64_t roots = root_count * sizeof(struct physical_page_root),
             tree = (nodes * 3 + 7) / 8;
    if (roots > UINT64_MAX - payload || tree > UINT64_MAX - payload - roots ||
        payload + roots + tree > UINT64_MAX - BOAROS_PAGE_MASK)
        return PHYSICAL_PAGE_STATUS_INVALID;
    uint64_t metadata_pages =
        (payload + roots + tree + BOAROS_PAGE_MASK) >> BOAROS_PAGE_SHIFT;
    if (!metadata_pages || metadata_pages > allocator->available_pages ||
        metadata_pages > UINT64_MAX >> BOAROS_PAGE_SHIFT)
        return PHYSICAL_PAGE_STATUS_EMPTY;
    uint64_t span = metadata_pages << BOAROS_PAGE_SHIFT, address = 0;
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < allocator->range_count; i++)
    {
        const struct physical_page_range *r = &allocator->ranges[i];
        if (r->next <= r->end && span <= r->end - r->next)
        {
            selected = i;
            address = r->next;
            break;
        }
    }
    if (selected == UINT32_MAX)
        return PHYSICAL_PAGE_STATUS_EMPTY;
    unsigned char *first = allocator->access(address),
                  *last = allocator->access(address + span - BOAROS_PAGE_SIZE);
    if (!first || !last || (uintptr_t)first > UINTPTR_MAX - (span - BOAROS_PAGE_SIZE) ||
        (uintptr_t)last != (uintptr_t)first + span - BOAROS_PAGE_SIZE)
        return PHYSICAL_PAGE_STATUS_INVALID;
    struct physical_page_allocator result = *allocator;
    result.metadata_address = address;
    result.metadata_pages = metadata_pages;
    result.metadata = (struct physical_page_metadata *)first;
    result.roots = (struct physical_page_root *)(first + payload);
    result.root_count = (uint32_t)root_count;
    result.tree = first + payload + roots;
    result.tree_nodes = nodes;
    result.ranges[selected].next += span;
    memset(result.tree, 0, (size_t)tree);
    for (uint32_t i = 0; i <= PHYSICAL_PAGE_MAX_ORDER; i++)
        result.free_heads[i] = PHYSICAL_PAGE_INDEX_NONE;
    for (uint32_t i = 0; (uint64_t)i < result.total_pages; i++)
        mark_head(&result, i, 0, PHYSICAL_PAGE_STATE_ALLOCATED_HEAD, 0);
    for (uint64_t pa = address; pa < address + span; pa += BOAROS_PAGE_SIZE)
    {
        uint32_t index;
        if (!page_lookup(&result, pa, 0, &index))
            return PHYSICAL_PAGE_STATUS_INVALID;
        mark_head(&result, index, 0, PHYSICAL_PAGE_STATE_INTERNAL, 0);
    }
    uint64_t recycled = 0, virgin = 0, current = allocator->recycled_head;
    while (current != PHYSICAL_PAGE_NONE)
    {
        uint32_t index;
        uint64_t next;
        if (recycled >= allocator->total_pages ||
            !address_was_allocated(allocator, current) ||
            !page_lookup(&result, current, 0, &index) ||
            result.metadata[index].state != PHYSICAL_PAGE_STATE_ALLOCATED_HEAD ||
            !read_recycled_node(allocator, current, &next) ||
            (next != PHYSICAL_PAGE_NONE && !address_was_allocated(allocator, next)))
            return PHYSICAL_PAGE_STATUS_INVALID;
        mark_head(&result, index, 0, PHYSICAL_PAGE_STATE_CANDIDATE, 0);
        recycled++;
        current = next;
    }
    for (uint32_t i = 0; i < result.range_count; i++)
    {
        const struct physical_page_range *r = &result.ranges[i];
        for (uint64_t pa = r->next; pa < r->end; pa += BOAROS_PAGE_SIZE)
        {
            uint32_t index;
            if (!page_lookup(&result, pa, 0, &index) ||
                result.metadata[index].state != PHYSICAL_PAGE_STATE_ALLOCATED_HEAD)
                return PHYSICAL_PAGE_STATUS_INVALID;
            mark_head(&result, index, 0, PHYSICAL_PAGE_STATE_CANDIDATE, 0);
            virgin++;
        }
    }
    if (recycled + virgin != allocator->available_pages - metadata_pages)
        return PHYSICAL_PAGE_STATUS_INVALID;
    uint32_t root = 0;
    uint64_t offset = 0;
    for (uint32_t i = 0; i < result.range_count; i++)
    {
        const struct physical_page_range *r = &result.ranges[i];
        for (uint64_t pa = r->base; pa < r->end;)
        {
            unsigned order =
                largest_fitting_order(pa, (r->end - pa) >> BOAROS_PAGE_SHIFT);
            result.roots[root] = (struct physical_page_root){
                pa,
                offset,
                r->first_page_index + (uint32_t)((pa - r->base) >> BOAROS_PAGE_SHIFT),
                order,
                i,
                0};
            offset += 2 * order_page_count(order) - 1;
            pa += order_page_count(order) << BOAROS_PAGE_SHIFT;
            root++;
        }
    }
    for (uint32_t i = 0; i < result.root_count; i++)
    {
        const struct physical_page_root *r = &result.roots[i];
        build_tree(&result, r, 0, r->first_page_index, r->order);
    }
    uint64_t free_pages = 0;
    for (uint32_t i = 0; i < result.root_count; i++)
    {
        const struct physical_page_root *r = &result.roots[i];
        if (!publish_free_heads(&result, r, 0, r->first_page_index, r->order,
                                &free_pages))
            return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (free_pages != allocator->available_pages - metadata_pages)
        return PHYSICAL_PAGE_STATUS_INVALID;
    result.available_pages = free_pages;
    result.recycled_head = PHYSICAL_PAGE_NONE;
    result.finalized = PHYSICAL_PAGE_ALLOCATOR_FINALIZED;
    note_allocated_peak(&result);
    (void)physical_page_allocator_audit(&result);
    *allocator = result;
    return PHYSICAL_PAGE_STATUS_OK;
}

static enum physical_page_status
physical_page_allocate_order_once(struct physical_page_allocator *a, uint32_t order,
                                  uint64_t *address)
{
    PAGE_METADATA_SCOPE(work);
    if (!allocator_initialized(a) || !address || order > PHYSICAL_PAGE_MAX_ORDER)
        return PHYSICAL_PAGE_STATUS_INVALID;
    if (!physical_page_allocator_is_finalized(a))
        return PHYSICAL_PAGE_STATUS_STATE;
    uint32_t found = order;
    while (found <= PHYSICAL_PAGE_MAX_ORDER)
    {
        page_checked(&work);
        if (a->free_heads[found] != PHYSICAL_PAGE_INDEX_NONE)
            break;
        found++;
    }
    if (found > PHYSICAL_PAGE_MAX_ORDER)
        return PHYSICAL_PAGE_STATUS_EMPTY;
    uint32_t page = a->free_heads[found];
    struct buddy_owner owner;
    if (!owner_find(a, page, &owner, &work) || owner.first != page ||
        owner.order != found || !free_head_valid(a, &owner, &work))
        __builtin_trap();
    uint64_t pa = owner_address(&owner);
    if (order_page_count(order) > a->available_pages ||
        a->available_pages > a->total_pages)
        __builtin_trap();
    free_remove(a, page, &work);
    while (found > order)
    {
        if (tree_get(a, owner.root, 2 * owner.node + 1, &work) != BUDDY_INACTIVE ||
            tree_get(a, owner.root, 2 * owner.node + 2, &work) != BUDDY_INACTIVE)
            __builtin_trap();
        found--;
        uint32_t upper = page + (uint32_t)order_page_count(found);
        tree_set(a, owner.root, 2 * owner.node + 1, BUDDY_FREE, &work);
        tree_set(a, owner.root, 2 * owner.node + 2, BUDDY_FREE, &work);
        tree_set(a, owner.root, owner.node, BUDDY_SPLIT, &work);
        mark_head(a, upper, found, PHYSICAL_PAGE_STATE_FREE_HEAD, &work);
        if (!free_insert(a, upper, found, &work))
            __builtin_trap();
        owner.node = 2 * owner.node + 1;
    }
    tree_set(a, owner.root, owner.node, BUDDY_ALLOCATED, &work);
    mark_head(a, page, order, PHYSICAL_PAGE_STATE_ALLOCATED_HEAD, &work);
    a->available_pages -= order_page_count(order);
    note_allocated_peak(a);
    *address = pa;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocate_order(
    struct physical_page_allocator *allocator,
    uint32_t order,
    uint64_t *address)
{
    COST_ADD(PAGE_CALLS, 1);
    enum physical_page_status status =
        physical_page_allocate_order_once(allocator, order, address);

    {
        /* 分发与 callback 取得自身 owner 之间不得被卸载。干净回收不睡眠；
         * pressure_wait 在显式睡眠前自行 pin 组，不持有 buddy 修改状态。 */
        KERNEL_IRQ_SCOPE(irq);
        if (allocator_initialized(allocator) && allocator->pressure_notify)
            allocator->pressure_notify(allocator->pressure_context);
        if (status == PHYSICAL_PAGE_STATUS_EMPTY &&
            allocator_initialized(allocator) &&
            physical_page_allocator_is_finalized(allocator) &&
            allocator->reclaimer != 0) {
            uint32_t *depth = allocator->reclaim_depth ? allocator->reclaim_depth() : &allocator->reclaiming;
            if (*depth) { COST_ADD(PAGE_FAILURES, 1); return status; }
            if (depth != &allocator->reclaiming) *depth = 1U;
            if (allocator->reclaiming == UINT32_MAX) __builtin_trap();
            allocator->reclaiming++;
            (void)allocator->reclaimer(allocator->reclaimer_context,
                                       order_page_count(order));
            allocator->reclaiming--;
            if (depth != &allocator->reclaiming) *depth = 0U;
            if (allocator->available_pages < order_page_count(order) && allocator->pressure_wait)
                allocator->pressure_wait(allocator->pressure_context);
            status = physical_page_allocate_order_once(allocator,
                                                       order,
                                                       address);
        }
    }

    if (status == PHYSICAL_PAGE_STATUS_OK) {
        COST_ADD(PAGE_ACCEPTED, order_page_count(order)); COST_IO_ADD(7, order_page_count(order));
    } else COST_ADD(PAGE_FAILURES, 1);
    return status;
}

enum physical_page_status
physical_page_allocation_order(const struct physical_page_allocator *a,
                               uint64_t address, uint32_t *order)
{
    PAGE_METADATA_SCOPE(work);
    uint32_t page;
    if (!allocator_initialized(a) || !order ||
        !page_lookup_work(a, address, 0, &page, &work))
        return PHYSICAL_PAGE_STATUS_INVALID;
    if (!physical_page_allocator_is_finalized(a))
        return PHYSICAL_PAGE_STATUS_STATE;
    struct buddy_owner owner;
    if (!owner_find(a, page, &owner, &work))
        __builtin_trap();
    if (owner.state == BUDDY_FREE)
        return PHYSICAL_PAGE_STATUS_DOUBLE_FREE;
    if (owner.state != BUDDY_ALLOCATED || owner.first != page)
        return PHYSICAL_PAGE_STATUS_INVALID;
    if (!head_valid(a, &owner, &work))
        __builtin_trap();
    *order = owner.order;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_release_order(struct physical_page_allocator *a,
                                                      uint64_t address, uint32_t order)
{
    PAGE_METADATA_SCOPE(work);
    uint32_t page;
    if (!allocator_initialized(a))
        physical_page_release_fatal(a, address, order, "allocator-state", address);
    if (order > PHYSICAL_PAGE_MAX_ORDER)
        physical_page_release_fatal(a, address, order, "order-range", address);
    if (address & BOAROS_PAGE_MASK)
        physical_page_release_fatal(a, address, order, "unaligned-address", address);
    if (!page_lookup_work(a, address, 0, &page, &work))
        physical_page_release_fatal(a, address, order, "address-range", address);
    if (!physical_page_allocator_is_finalized(a))
        physical_page_release_fatal(a, address, order, "not-finalized", address);
    struct buddy_owner owner;
    if (!owner_find(a, page, &owner, &work))
        physical_page_release_fatal(a, address, order, "tree-owner", address);
    if (owner.state == BUDDY_FREE)
        physical_page_release_fatal(a, address, order, "already-free", address);
    if (owner.state != BUDDY_ALLOCATED || owner.first != page)
        physical_page_release_fatal(a, address, order, "not-allocated-head", address);
    if (owner.order != order)
        physical_page_release_fatal(a, address, order, "wrong-order", address);
    if (!a->metadata[page].reference_count)
        physical_page_release_fatal(a, address, order, "reference-count", address);
    if (!head_valid(a, &owner, &work))
        physical_page_release_fatal(a, address, order, "allocated-block", address);
    if (a->metadata[page].reference_count > 1)
    {
        a->metadata[page].reference_count--;
        page_written(&work);
        return PHYSICAL_PAGE_STATUS_OK;
    }
    uint64_t pages = order_page_count(order);
    if (a->available_pages > a->total_pages ||
        pages > a->total_pages - a->available_pages)
        physical_page_release_fatal(a, address, order, "available-count", address);
    tree_set(a, owner.root, owner.node, BUDDY_FREE, &work);
    mark_head(a, page, order, PHYSICAL_PAGE_STATE_FREE_HEAD, &work);
    while (owner.order < owner.root->order)
    {
        uint64_t sibling = (owner.node & 1U) ? owner.node + 1 : owner.node - 1;
        uint32_t buddy;
        /* 平坦索引不一定自然对齐：兄弟位置由同一物理根的局部偏移推导。 */
        buddy = owner.root->first_page_index +
                ((owner.first - owner.root->first_page_index) ^
                 (uint32_t)order_page_count(owner.order));
        unsigned state = tree_get(a, owner.root, sibling, &work);
        if (state == BUDDY_FREE)
        {
            struct buddy_owner other = {owner.root, sibling, buddy, owner.order, state};
            if (!free_head_valid(a, &other, &work))
                physical_page_release_fatal(a, address, order, "buddy-free-block",
                                            owner_address(&other));
            free_remove(a, buddy, &work);
            tree_set(a, owner.root, owner.node, BUDDY_INACTIVE, &work);
            tree_set(a, owner.root, sibling, BUDDY_INACTIVE, &work);
            owner.node = (owner.node - 1) / 2;
            if (buddy < owner.first)
                owner.first = buddy;
            owner.order++;
            tree_set(a, owner.root, owner.node, BUDDY_FREE, &work);
            mark_head(a, owner.first, owner.order, PHYSICAL_PAGE_STATE_FREE_HEAD,
                      &work);
        }
        else
        {
            if (state != BUDDY_ALLOCATED && state != BUDDY_INTERNAL &&
                state != BUDDY_SPLIT)
            {
                struct buddy_owner other = {owner.root, sibling, buddy, owner.order,
                                            state};
                physical_page_release_fatal(a, address, order, "buddy-free-block",
                                            owner_address(&other));
            }
            break;
        }
    }
    if (!free_insert(a, owner.first, owner.order, &work))
        physical_page_release_fatal(a, address, order, "free-list-insert",
                                    owner_address(&owner));
    a->available_pages += pages;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocate(
    struct physical_page_allocator *allocator,
    uint64_t *address)
{
    if (!allocator_initialized(allocator) || address == 0) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (physical_page_allocator_is_finalized(allocator)) {
        return physical_page_allocate_order(allocator, 0U, address);
    }
    return physical_page_allocate_bootstrap(allocator, address);
}

enum physical_page_status physical_page_release(
    struct physical_page_allocator *allocator,
    uint64_t address)
{
    if (!allocator_initialized(allocator)) {
        physical_page_release_fatal(allocator, address, 0U,
                                    "allocator-state", address);
    }
    if (physical_page_allocator_is_finalized(allocator)) {
        return physical_page_release_order(allocator, address, 0U);
    }
    return physical_page_release_bootstrap(allocator, address);
}

enum physical_page_status physical_page_acquire(struct physical_page_allocator *a,
                                                uint64_t address)
{
    PAGE_METADATA_SCOPE(work);
    uint32_t page;
    struct buddy_owner owner;
    if (!physical_page_allocator_is_finalized(a) || (address & BOAROS_PAGE_MASK) ||
        !page_lookup_work(a, address, 0, &page, &work) ||
        !owner_find(a, page, &owner, &work) || owner.first != page || owner.order ||
        owner.state != BUDDY_ALLOCATED || !head_valid(a, &owner, &work) ||
        a->metadata[page].reference_count == UINT32_MAX)
        __builtin_trap();
    a->metadata[page].reference_count++;
    page_written(&work);
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status
physical_page_reference_count(const struct physical_page_allocator *a, uint64_t address,
                              uint32_t *references)
{
    PAGE_METADATA_SCOPE(work);
    uint32_t page;
    struct buddy_owner owner;
    if (!physical_page_allocator_is_finalized(a) || !references ||
        !page_lookup_work(a, address, 0, &page, &work) ||
        !owner_find(a, page, &owner, &work) || owner.first != page ||
        owner.state != BUDDY_ALLOCATED || !head_valid(a, &owner, &work))
        __builtin_trap();
    *references = a->metadata[page].reference_count;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocator_set_reclaimer(
    struct physical_page_allocator *allocator,
    physical_page_reclaim_fn reclaimer,
    void *context)
{
    KERNEL_IRQ_SCOPE(irq);
    if (!physical_page_allocator_is_finalized(allocator) ||
        reclaimer == 0) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->reclaimer != 0 || allocator->reclaiming != 0U) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    allocator->reclaimer = reclaimer;
    allocator->reclaimer_context = context;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocator_clear_reclaimer(
    struct physical_page_allocator *allocator)
{
    KERNEL_IRQ_SCOPE(irq);
    if (!physical_page_allocator_is_finalized(allocator)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->reclaimer == 0 || allocator->reclaiming != 0U) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    allocator->reclaimer = 0;
    allocator->reclaim_depth = 0;
    allocator->reclaimer_context = 0;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status
physical_page_resolve(const struct physical_page_allocator *allocator,
                      uint64_t physical_address, void **pointer)
{
    PAGE_METADATA_SCOPE(work);
    if (!allocator_initialized(allocator) || !pointer)
        return PHYSICAL_PAGE_STATUS_INVALID;
    if (!allocator->access)
        return PHYSICAL_PAGE_STATUS_STATE;
    if (physical_page_allocator_is_finalized(allocator))
    {
        uint32_t page;
        struct buddy_owner owner;
        if (!page_lookup_work(allocator, physical_address, 0, &page, &work))
            return PHYSICAL_PAGE_STATUS_INVALID;
        if (!owner_find(allocator, page, &owner, &work))
            __builtin_trap();
        if (owner.state != BUDDY_ALLOCATED)
            return PHYSICAL_PAGE_STATUS_INVALID;
        if (!head_valid(allocator, &owner, &work))
            __builtin_trap();
    }
    else if (!address_was_allocated(allocator, physical_address))
        return PHYSICAL_PAGE_STATUS_INVALID;
    void *result = allocator->access(physical_address);
    if (!result)
        return PHYSICAL_PAGE_STATUS_INVALID;
    *pointer = result;
    return PHYSICAL_PAGE_STATUS_OK;
}

uint64_t physical_page_total(
    const struct physical_page_allocator *allocator)
{
    if (!allocator_initialized(allocator)) {
        return 0U;
    }

    return allocator->total_pages;
}

uint64_t physical_page_available(
    const struct physical_page_allocator *allocator)
{
    if (!allocator_initialized(allocator)) {
        return 0U;
    }

    return allocator->available_pages;
}

uint64_t physical_page_metadata_pages(
    const struct physical_page_allocator *allocator)
{
    if (!physical_page_allocator_is_finalized(allocator)) {
        return 0U;
    }

    return allocator->metadata_pages;
}

void kernel_memory_snapshot(const struct physical_page_allocator *allocator,
                            struct kernel_memory_statistics *out)
{
    *out = (struct kernel_memory_statistics){0};
    out->total = physical_page_total(allocator) * BOAROS_PAGE_SIZE;
    out->free = physical_page_available(allocator) * BOAROS_PAGE_SIZE;
    out->available = out->free;
    out->shared = allocator->shared_anon_pages * BOAROS_PAGE_SIZE;
    out->cached = out->shared;
    if (allocator->buffer_bytes)
        out->buffers = allocator->buffer_bytes(allocator->buffer_context);
    if (allocator->cache_snapshot)
        allocator->cache_snapshot(allocator->cache_context, out);
}
