#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stdint.h>

#define PHYSICAL_PAGE_ALLOCATOR_INITIALIZED UINT32_C(0x50414745)
#define PHYSICAL_PAGE_ALLOCATOR_FINALIZED UINT32_C(0x42554459)
#define PHYSICAL_PAGE_NONE UINT64_MAX
#define PHYSICAL_PAGE_INDEX_NONE UINT32_MAX

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
    uint8_t order;
    uint8_t state;
    uint16_t reserved;
};

_Static_assert(sizeof(struct physical_page_metadata) == 12U,
               "physical page metadata must remain compact");

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

static int page_lookup(
    const struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t *range_index,
    uint32_t *page_index)
{
    uint32_t index;

    if ((address & BOAROS_PAGE_MASK) != 0U) {
        return 0;
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

static int index_lookup(
    const struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t *range_index,
    uint64_t *address)
{
    uint32_t index;

    if ((uint64_t)page_index >= allocator->total_pages) {
        return 0;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        const struct physical_page_range *range = &allocator->ranges[index];
        uint64_t pages = (range->end - range->base) >> BOAROS_PAGE_SHIFT;
        uint64_t first = range->first_page_index;

        if ((uint64_t)page_index >= first &&
            (uint64_t)page_index - first < pages) {
            uint64_t local = (uint64_t)page_index - first;

            if (range_index != 0) {
                *range_index = index;
            }
            if (address != 0) {
                *address = range->base +
                           (local << BOAROS_PAGE_SHIFT);
            }
            return 1;
        }
    }

    return 0;
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

    result.total_pages = 0U;
    result.available_pages = 0U;
    result.recycled_head = PHYSICAL_PAGE_NONE;
    result.metadata_address = PHYSICAL_PAGE_NONE;
    result.metadata_pages = 0U;
    result.range_count = 0U;
    result.initialized = 0U;
    result.finalized = 0U;
    result.access = 0;
    result.metadata = 0;
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
        *address = result;
        return PHYSICAL_PAGE_STATUS_OK;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        struct physical_page_range *range = &allocator->ranges[index];

        if (range->next < range->end) {
            uint64_t result = range->next;

            range->next += BOAROS_PAGE_SIZE;
            allocator->available_pages--;
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
    uint64_t current;
    uint64_t scanned = 0U;

    if (!address_was_allocated(allocator, address)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->access == 0) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    current = allocator->recycled_head;
    while (current != PHYSICAL_PAGE_NONE &&
           scanned < allocator->total_pages) {
        if (current == address) {
            return PHYSICAL_PAGE_STATUS_DOUBLE_FREE;
        }
        if (!address_was_allocated(allocator, current)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        if (!read_recycled_node(allocator, current, &current)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        scanned++;
    }
    if (current != PHYSICAL_PAGE_NONE ||
        allocator->available_pages >= allocator->total_pages) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    if (!write_recycled_node(allocator, address, allocator->recycled_head)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    allocator->recycled_head = address;
    allocator->available_pages++;
    return PHYSICAL_PAGE_STATUS_OK;
}

static uint64_t order_page_count(uint32_t order)
{
    return UINT64_C(1) << order;
}

static int block_geometry(
    const struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order,
    uint32_t *range_index,
    uint64_t *address)
{
    uint64_t block_pages;
    uint64_t block_size;
    uint64_t result_address;
    uint32_t result_range;
    const struct physical_page_range *range;

    if (order > PHYSICAL_PAGE_MAX_ORDER) {
        return 0;
    }
    block_pages = order_page_count(order);
    if (block_pages > allocator->total_pages - page_index ||
        block_pages > (UINT64_MAX >> BOAROS_PAGE_SHIFT)) {
        return 0;
    }
    block_size = block_pages << BOAROS_PAGE_SHIFT;
    if (!index_lookup(allocator,
                      page_index,
                      &result_range,
                      &result_address)) {
        return 0;
    }
    range = &allocator->ranges[result_range];
    if ((result_address & (block_size - 1U)) != 0U ||
        block_size > range->end - result_address) {
        return 0;
    }

    if (range_index != 0) {
        *range_index = result_range;
    }
    if (address != 0) {
        *address = result_address;
    }
    return 1;
}

static void mark_block(
    struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order,
    enum physical_page_state head_state,
    enum physical_page_state tail_state)
{
    uint64_t count = order_page_count(order);
    uint64_t offset;

    for (offset = 0U; offset < count; offset++) {
        struct physical_page_metadata *metadata =
            &allocator->metadata[page_index + (uint32_t)offset];

        metadata->next = PHYSICAL_PAGE_INDEX_NONE;
        metadata->previous = PHYSICAL_PAGE_INDEX_NONE;
        metadata->order = offset == 0U ? (uint8_t)order : 0U;
        metadata->state = (uint8_t)(offset == 0U ? head_state : tail_state);
        metadata->reserved = 0U;
    }
}

static int free_list_node_valid(
    const struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order);

static int free_list_insert(
    struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order)
{
    struct physical_page_metadata *metadata;
    uint32_t head;

    if (!block_geometry(allocator, page_index, order, 0, 0)) {
        return 0;
    }
    head = allocator->free_heads[order];
    if (head != PHYSICAL_PAGE_INDEX_NONE) {
        struct physical_page_metadata *old_head = &allocator->metadata[head];

        if (!free_list_node_valid(allocator, head, order) ||
            old_head->previous != PHYSICAL_PAGE_INDEX_NONE) {
            return 0;
        }
    }

    metadata = &allocator->metadata[page_index];
    metadata->next = head;
    metadata->previous = PHYSICAL_PAGE_INDEX_NONE;
    metadata->order = (uint8_t)order;
    metadata->state = PHYSICAL_PAGE_STATE_FREE_HEAD;
    metadata->reserved = 0U;
    if (head != PHYSICAL_PAGE_INDEX_NONE) {
        allocator->metadata[head].previous = page_index;
    }
    allocator->free_heads[order] = page_index;
    return 1;
}

static int free_block_valid(
    const struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order)
{
    uint64_t count;
    uint64_t offset;

    if (!free_list_node_valid(allocator, page_index, order)) {
        return 0;
    }
    count = order_page_count(order);
    for (offset = 1U; offset < count; offset++) {
        if (allocator->metadata[page_index + (uint32_t)offset].state !=
            PHYSICAL_PAGE_STATE_FREE_TAIL) {
            return 0;
        }
    }
    return 1;
}

static int free_list_node_valid(
    const struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order)
{
    const struct physical_page_metadata *metadata;

    if (!block_geometry(allocator, page_index, order, 0, 0)) {
        return 0;
    }
    metadata = &allocator->metadata[page_index];
    if (metadata->state != PHYSICAL_PAGE_STATE_FREE_HEAD ||
        metadata->order != order) {
        return 0;
    }
    if (metadata->previous == PHYSICAL_PAGE_INDEX_NONE) {
        if (allocator->free_heads[order] != page_index) {
            return 0;
        }
    } else if ((uint64_t)metadata->previous >= allocator->total_pages ||
               allocator->metadata[metadata->previous].next != page_index) {
        return 0;
    }
    if (metadata->next != PHYSICAL_PAGE_INDEX_NONE &&
        ((uint64_t)metadata->next >= allocator->total_pages ||
         allocator->metadata[metadata->next].previous != page_index)) {
        return 0;
    }

    return 1;
}

static int free_list_remove(
    struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order)
{
    struct physical_page_metadata *metadata;
    uint32_t next;
    uint32_t previous;

    if (!free_list_node_valid(allocator, page_index, order)) {
        return 0;
    }
    metadata = &allocator->metadata[page_index];
    next = metadata->next;
    previous = metadata->previous;

    if (previous == PHYSICAL_PAGE_INDEX_NONE) {
        allocator->free_heads[order] = next;
    } else {
        allocator->metadata[previous].next = next;
    }
    if (next != PHYSICAL_PAGE_INDEX_NONE) {
        allocator->metadata[next].previous = previous;
    }
    metadata->next = PHYSICAL_PAGE_INDEX_NONE;
    metadata->previous = PHYSICAL_PAGE_INDEX_NONE;
    return 1;
}

static uint32_t largest_fitting_order(uint64_t address,
                                      uint64_t page_count)
{
    uint32_t order = 0U;

    while (order < PHYSICAL_PAGE_MAX_ORDER &&
           order_page_count(order + 1U) <= page_count) {
        order++;
    }
    while (order > 0U) {
        uint64_t block_size = order_page_count(order) << BOAROS_PAGE_SHIFT;

        if ((address & (block_size - 1U)) == 0U) {
            break;
        }
        order--;
    }
    return order;
}

static int build_free_lists(struct physical_page_allocator *allocator,
                            uint64_t *free_pages)
{
    uint64_t result = 0U;
    uint32_t range_index;

    for (range_index = 0U;
         range_index < allocator->range_count;
         range_index++) {
        const struct physical_page_range *range =
            &allocator->ranges[range_index];
        uint64_t range_pages =
            (range->end - range->base) >> BOAROS_PAGE_SHIFT;
        uint64_t local = 0U;

        while (local < range_pages) {
            uint32_t page_index =
                range->first_page_index + (uint32_t)local;

            if (allocator->metadata[page_index].state !=
                PHYSICAL_PAGE_STATE_CANDIDATE) {
                local++;
                continue;
            }

            {
                uint64_t run_pages = 0U;
                uint64_t consumed = 0U;

                while (local + run_pages < range_pages &&
                       allocator->metadata[page_index +
                                           (uint32_t)run_pages].state ==
                           PHYSICAL_PAGE_STATE_CANDIDATE) {
                    run_pages++;
                }

                while (consumed < run_pages) {
                    uint64_t address = range->base +
                        ((local + consumed) << BOAROS_PAGE_SHIFT);
                    uint32_t order = largest_fitting_order(
                        address,
                        run_pages - consumed);
                    uint64_t block_pages = order_page_count(order);

                    mark_block(allocator,
                               page_index + (uint32_t)consumed,
                               order,
                               PHYSICAL_PAGE_STATE_FREE_HEAD,
                               PHYSICAL_PAGE_STATE_FREE_TAIL);
                    if (!free_list_insert(allocator,
                                          page_index +
                                              (uint32_t)consumed,
                                          order)) {
                        return 0;
                    }
                    consumed += block_pages;
                    result += block_pages;
                }
                local += run_pages;
            }
        }
    }

    *free_pages = result;
    return 1;
}

static int validate_free_lists(
    const struct physical_page_allocator *allocator,
    uint64_t expected_free_pages)
{
    uint64_t free_pages = 0U;
    uint32_t order;

    for (order = 0U; order <= PHYSICAL_PAGE_MAX_ORDER; order++) {
        uint32_t page_index = allocator->free_heads[order];
        uint32_t previous = PHYSICAL_PAGE_INDEX_NONE;
        uint64_t visited = 0U;

        while (page_index != PHYSICAL_PAGE_INDEX_NONE) {
            const struct physical_page_metadata *metadata;
            uint64_t block_pages = order_page_count(order);
            uint64_t offset;

            if ((uint64_t)page_index >= allocator->total_pages ||
                !free_list_node_valid(allocator, page_index, order)) {
                return 0;
            }
            metadata = &allocator->metadata[page_index];
            if (metadata->previous != previous ||
                block_pages > allocator->total_pages - free_pages) {
                return 0;
            }
            for (offset = 1U; offset < block_pages; offset++) {
                if (allocator->metadata[
                        page_index + (uint32_t)offset].state !=
                    PHYSICAL_PAGE_STATE_FREE_TAIL) {
                    return 0;
                }
            }
            free_pages += block_pages;
            previous = page_index;
            page_index = metadata->next;
            visited++;
            if (visited > allocator->total_pages) {
                return 0;
            }
        }
    }

    return free_pages == expected_free_pages;
}

enum physical_page_status physical_page_allocator_finalize(
    struct physical_page_allocator *allocator)
{
    struct physical_page_allocator result;
    uint64_t metadata_bytes;
    uint64_t metadata_pages;
    uint64_t metadata_span;
    uint64_t metadata_address = PHYSICAL_PAGE_NONE;
    uint64_t metadata_last_address;
    uint64_t recycled_count = 0U;
    uint64_t tail_count = 0U;
    uint64_t free_pages;
    uint64_t current;
    uint32_t metadata_range = UINT32_MAX;
    uint32_t index;
    unsigned char *metadata_first;
    unsigned char *metadata_last;

    if (!allocator_initialized(allocator)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (physical_page_allocator_is_finalized(allocator)) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }
    if (allocator->access == 0) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }
    if (allocator->total_pages > UINT32_MAX ||
        allocator->total_pages >
            UINT64_MAX / sizeof(struct physical_page_metadata)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    metadata_bytes = allocator->total_pages *
                     sizeof(struct physical_page_metadata);
    if (metadata_bytes > UINT64_MAX - BOAROS_PAGE_MASK) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    metadata_pages = (metadata_bytes + BOAROS_PAGE_MASK) >>
                     BOAROS_PAGE_SHIFT;
    if (metadata_pages == 0U ||
        metadata_pages > allocator->available_pages ||
        metadata_pages > (UINT64_MAX >> BOAROS_PAGE_SHIFT)) {
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }
    metadata_span = metadata_pages << BOAROS_PAGE_SHIFT;

    for (index = 0U; index < allocator->range_count; index++) {
        const struct physical_page_range *range = &allocator->ranges[index];

        if (range->next <= range->end &&
            metadata_span <= range->end - range->next) {
            metadata_address = range->next;
            metadata_range = index;
            break;
        }
    }
    if (metadata_range == UINT32_MAX) {
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }

    metadata_last_address = metadata_address +
        ((metadata_pages - 1U) << BOAROS_PAGE_SHIFT);
    metadata_first = allocator->access(metadata_address);
    metadata_last = allocator->access(metadata_last_address);
    if (metadata_first == 0 || metadata_last == 0 ||
        (uintptr_t)metadata_first >
            UINTPTR_MAX -
                ((metadata_pages - 1U) << BOAROS_PAGE_SHIFT) ||
        (uintptr_t)metadata_last !=
            (uintptr_t)metadata_first +
                ((metadata_pages - 1U) << BOAROS_PAGE_SHIFT)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    result = *allocator;
    result.metadata_address = metadata_address;
    result.metadata_pages = metadata_pages;
    result.metadata = (struct physical_page_metadata *)metadata_first;
    result.ranges[metadata_range].next += metadata_span;
    for (index = 0U; index <= PHYSICAL_PAGE_MAX_ORDER; index++) {
        result.free_heads[index] = PHYSICAL_PAGE_INDEX_NONE;
    }
    for (index = 0U; (uint64_t)index < result.total_pages; index++) {
        struct physical_page_metadata *metadata = &result.metadata[index];

        metadata->next = PHYSICAL_PAGE_INDEX_NONE;
        metadata->previous = PHYSICAL_PAGE_INDEX_NONE;
        metadata->order = 0U;
        metadata->state = PHYSICAL_PAGE_STATE_ALLOCATED_HEAD;
        metadata->reserved = 0U;
    }

    for (current = metadata_address;
         current < metadata_address + metadata_span;
         current += BOAROS_PAGE_SIZE) {
        uint32_t page_index;

        if (!page_lookup(&result, current, 0, &page_index)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        result.metadata[page_index].state = PHYSICAL_PAGE_STATE_INTERNAL;
    }

    current = allocator->recycled_head;
    while (current != PHYSICAL_PAGE_NONE) {
        uint64_t next;
        uint32_t page_index;

        if (recycled_count >= allocator->total_pages ||
            !address_was_allocated(allocator, current) ||
            !page_lookup(&result, current, 0, &page_index) ||
            result.metadata[page_index].state !=
                PHYSICAL_PAGE_STATE_ALLOCATED_HEAD ||
            !read_recycled_node(allocator, current, &next) ||
            (next != PHYSICAL_PAGE_NONE &&
             !address_was_allocated(allocator, next))) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        result.metadata[page_index].state = PHYSICAL_PAGE_STATE_CANDIDATE;
        recycled_count++;
        current = next;
    }

    for (index = 0U; index < allocator->range_count; index++) {
        const struct physical_page_range *range = &allocator->ranges[index];
        uint64_t address;

        for (address = range->next;
             address < range->end;
             address += BOAROS_PAGE_SIZE) {
            uint32_t page_index;
            struct physical_page_metadata *metadata;

            if (!page_lookup(&result, address, 0, &page_index)) {
                return PHYSICAL_PAGE_STATUS_INVALID;
            }
            metadata = &result.metadata[page_index];
            if (metadata->state == PHYSICAL_PAGE_STATE_INTERNAL) {
                continue;
            }
            if (metadata->state != PHYSICAL_PAGE_STATE_ALLOCATED_HEAD) {
                return PHYSICAL_PAGE_STATUS_INVALID;
            }
            metadata->state = PHYSICAL_PAGE_STATE_CANDIDATE;
            tail_count++;
        }
    }
    if (recycled_count > UINT64_MAX - tail_count ||
        recycled_count + tail_count !=
            allocator->available_pages - metadata_pages) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    if (!build_free_lists(&result, &free_pages) ||
        free_pages != allocator->available_pages - metadata_pages ||
        !validate_free_lists(&result, free_pages)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    result.available_pages = free_pages;
    result.recycled_head = PHYSICAL_PAGE_NONE;
    result.finalized = PHYSICAL_PAGE_ALLOCATOR_FINALIZED;
    *allocator = result;
    return PHYSICAL_PAGE_STATUS_OK;
}

enum physical_page_status physical_page_allocate_order(
    struct physical_page_allocator *allocator,
    uint32_t order,
    uint64_t *address)
{
    uint32_t found;
    uint32_t page_index;
    uint64_t block_pages;
    uint64_t result_address;

    if (!allocator_initialized(allocator) || address == 0 ||
        order > PHYSICAL_PAGE_MAX_ORDER) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (!physical_page_allocator_is_finalized(allocator)) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    for (found = order; found <= PHYSICAL_PAGE_MAX_ORDER; found++) {
        if (allocator->free_heads[found] != PHYSICAL_PAGE_INDEX_NONE) {
            break;
        }
    }
    if (found > PHYSICAL_PAGE_MAX_ORDER) {
        return PHYSICAL_PAGE_STATUS_EMPTY;
    }

    page_index = allocator->free_heads[found];
    if (!free_block_valid(allocator, page_index, found) ||
        !index_lookup(allocator, page_index, 0, &result_address)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    while (found > order) {
        uint32_t target_order;
        uint32_t target_head;

        found--;
        target_order = found;
        target_head = allocator->free_heads[target_order];
        if (target_head != PHYSICAL_PAGE_INDEX_NONE &&
            !free_list_node_valid(allocator,
                                  target_head,
                                  target_order)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        if (!block_geometry(allocator,
                            page_index + (uint32_t)order_page_count(found),
                            found,
                            0,
                            0)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
    }
    found = allocator->metadata[page_index].order;
    if (!free_list_remove(allocator, page_index, found)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    while (found > order) {
        uint32_t upper;

        found--;
        upper = page_index + (uint32_t)order_page_count(found);
        if (!free_list_insert(allocator, upper, found)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
    }

    block_pages = order_page_count(order);
    if (block_pages > allocator->available_pages) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    mark_block(allocator,
               page_index,
               order,
               PHYSICAL_PAGE_STATE_ALLOCATED_HEAD,
               PHYSICAL_PAGE_STATE_ALLOCATED_TAIL);
    allocator->available_pages -= block_pages;
    *address = result_address;
    return PHYSICAL_PAGE_STATUS_OK;
}

static int allocated_block_valid(
    const struct physical_page_allocator *allocator,
    uint32_t page_index,
    uint32_t order)
{
    uint64_t count;
    uint64_t offset;

    if (!block_geometry(allocator, page_index, order, 0, 0) ||
        allocator->metadata[page_index].state !=
            PHYSICAL_PAGE_STATE_ALLOCATED_HEAD ||
        allocator->metadata[page_index].order != order) {
        return 0;
    }
    count = order_page_count(order);
    for (offset = 1U; offset < count; offset++) {
        if (allocator->metadata[page_index + (uint32_t)offset].state !=
            PHYSICAL_PAGE_STATE_ALLOCATED_TAIL) {
            return 0;
        }
    }
    return 1;
}

enum physical_page_status physical_page_release_order(
    struct physical_page_allocator *allocator,
    uint64_t address,
    uint32_t order)
{
    uint32_t page_index;
    uint32_t original_range;
    uint32_t merge_buddies[PHYSICAL_PAGE_MAX_ORDER];
    uint32_t merge_count = 0U;
    uint32_t current_order;
    uint64_t current_address;
    uint64_t block_pages;
    const struct physical_page_metadata *metadata;

    if (!allocator_initialized(allocator) ||
        order > PHYSICAL_PAGE_MAX_ORDER ||
        !page_lookup(allocator, address, &original_range, &page_index)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (!physical_page_allocator_is_finalized(allocator)) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }

    metadata = &allocator->metadata[page_index];
    if (metadata->state == PHYSICAL_PAGE_STATE_FREE_HEAD ||
        metadata->state == PHYSICAL_PAGE_STATE_FREE_TAIL) {
        return PHYSICAL_PAGE_STATUS_DOUBLE_FREE;
    }
    if (!allocated_block_valid(allocator, page_index, order)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    block_pages = order_page_count(order);
    if (block_pages > allocator->total_pages - allocator->available_pages) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    current_address = address;
    current_order = order;
    while (current_order < PHYSICAL_PAGE_MAX_ORDER) {
        uint64_t block_size =
            order_page_count(current_order) << BOAROS_PAGE_SHIFT;
        uint64_t buddy_address = current_address ^ block_size;
        uint32_t buddy_index;
        uint32_t buddy_range;

        if (!page_lookup(allocator,
                         buddy_address,
                         &buddy_range,
                         &buddy_index) ||
            buddy_range != original_range ||
            !free_list_node_valid(allocator,
                                  buddy_index,
                                  current_order)) {
            break;
        }
        merge_buddies[merge_count++] = buddy_index;
        if (buddy_address < current_address) {
            current_address = buddy_address;
        }
        current_order++;
    }

    if (allocator->free_heads[current_order] != PHYSICAL_PAGE_INDEX_NONE &&
        !free_list_node_valid(allocator,
                              allocator->free_heads[current_order],
                              current_order)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    mark_block(allocator,
               page_index,
               order,
               PHYSICAL_PAGE_STATE_FREE_TAIL,
               PHYSICAL_PAGE_STATE_FREE_TAIL);

    for (page_index = 0U; page_index < merge_count; page_index++) {
        uint32_t buddy_order = order + page_index;
        struct physical_page_metadata *buddy_metadata;

        if (!free_list_remove(allocator,
                              merge_buddies[page_index],
                              buddy_order)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        buddy_metadata = &allocator->metadata[merge_buddies[page_index]];
        buddy_metadata->order = 0U;
        buddy_metadata->state = PHYSICAL_PAGE_STATE_FREE_TAIL;
    }
    if (!page_lookup(allocator, current_address, 0, &page_index) ||
        !free_list_insert(allocator, page_index, current_order)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    allocator->available_pages += block_pages;
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
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (physical_page_allocator_is_finalized(allocator)) {
        return physical_page_release_order(allocator, address, 0U);
    }
    return physical_page_release_bootstrap(allocator, address);
}

enum physical_page_status physical_page_resolve(
    const struct physical_page_allocator *allocator,
    uint64_t physical_address,
    void **pointer)
{
    void *result;

    if (!allocator_initialized(allocator) || pointer == 0) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }
    if (allocator->access == 0) {
        return PHYSICAL_PAGE_STATUS_STATE;
    }
    if (physical_page_allocator_is_finalized(allocator)) {
        uint32_t page_index;
        uint8_t state;

        if (!page_lookup(allocator, physical_address, 0, &page_index)) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
        state = allocator->metadata[page_index].state;
        if (state != PHYSICAL_PAGE_STATE_ALLOCATED_HEAD &&
            state != PHYSICAL_PAGE_STATE_ALLOCATED_TAIL) {
            return PHYSICAL_PAGE_STATUS_INVALID;
        }
    } else if (!address_was_allocated(allocator, physical_address)) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

    result = allocator->access(physical_address);
    if (result == 0) {
        return PHYSICAL_PAGE_STATUS_INVALID;
    }

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
