#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char pool[256 * BOAROS_PAGE_SIZE]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;

#define TEST_SLAB_BITMAP_WORDS 4U
#define TEST_PAGE_STATE_FREE_HEAD 3U
#define TEST_PAGE_STATE_FREE_TAIL 4U

/* These mirrors are used only to inject corruption into the metadata paths
 * whose invariants are part of the allocator contract. */
struct test_slab_header {
    uint64_t magic;
    struct kernel_heap *heap;
    struct test_slab_header *next;
    struct test_slab_header *previous;
    uint64_t allocated[TEST_SLAB_BITMAP_WORDS];
    uint32_t free_head;
    uint16_t class_index;
    uint16_t slot_count;
    uint16_t free_count;
    uint16_t slot_offset;
    uint32_t reserved;
};

struct test_page_metadata {
    uint32_t next;
    uint32_t previous;
    uint32_t reference_count;
    uint8_t order;
    uint8_t state;
    uint16_t reserved;
};

static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer, uint64_t *address)
{
    uintptr_t value = (uintptr_t)pointer;
    if (value < (uintptr_t)pool || value >= (uintptr_t)pool + sizeof(pool))
        return 0;
    *address = value;
    return 1;
}
static void setup(void)
{
    struct boot_memory_layout layout = {0};
    layout.usable_count = 1;
    layout.usable[0].base = (uintptr_t)pool;
    layout.usable[0].size = sizeof(pool);
    assert(physical_page_allocator_init(&allocator, &layout) == 0);
    assert(physical_page_allocator_bind_access(&allocator, access_page) == 0);
    assert(physical_page_allocator_finalize(&allocator) == 0);
    assert(kernel_heap_init(&heap, &allocator, page_address) == 0);
}
static uint32_t test_page_index(uint64_t address)
{
    uint32_t index;

    for (index = 0U; index < allocator.range_count; index++) {
        const struct physical_page_range *range = &allocator.ranges[index];

        if (address >= range->base && address < range->end) {
            return range->first_page_index +
                   (uint32_t)((address - range->base) >> BOAROS_PAGE_SHIFT);
        }
    }
    assert(0);
    return 0U;
}

static void invalid_release(unsigned int which)
{
    uint64_t page;
    void *object;
    uint32_t references;
    setup();
    assert(physical_page_allocate_order(&allocator, 1, &page) == 0);
    assert(kernel_heap_allocate(&heap, 32, &object) == 0);
    switch (which) {
    case 0: physical_page_release_order(&allocator, page, 0); break;
    case 1: physical_page_release_order(&allocator, page + BOAROS_PAGE_SIZE, 1); break;
    case 2: physical_page_release_order(&allocator, page, 1);
            physical_page_release_order(&allocator, page, 1); break;
    case 3: physical_page_release(&allocator, page + 1); break;
    case 4: kernel_heap_release(&heap, (char *)object + 1); break;
    case 5: kernel_heap_release(&heap, object);
            kernel_heap_release(&heap, object); break;
    case 6: physical_page_release(0, page); break;
    case 7: allocator.available_pages = allocator.total_pages + 1;
            physical_page_release_order(&allocator, page, 1); break;
    case 8: physical_page_release_order(&allocator, page, 1);
            physical_page_acquire(&allocator, page); break;
    case 9: physical_page_release_order(&allocator, page, 1);
            physical_page_reference_count(&allocator, page, &references); break;
    case 10: {
        void *other;
        struct test_slab_header *slab;

        assert(kernel_heap_allocate(&heap, 32, &other) == 0);
        slab = (struct test_slab_header *)heap.partial_slabs[1];
        assert(slab != 0 && slab->free_count > 0U);
        slab->free_count++;
        kernel_heap_release(&heap, object);
        break;
    }
    case 11: {
        uint64_t pages[128];
        uint32_t page_count = 0U;
        uint32_t first;
        uint32_t second;
        struct test_page_metadata *metadata;

        while (page_count < 128U &&
               physical_page_allocate_order(&allocator,
                                             0U,
                                             &pages[page_count]) == 0) {
            page_count++;
        }
        for (first = 0U; first < page_count; first++) {
            for (second = first + 1U; second < page_count; second++) {
                if ((pages[first] ^ BOAROS_PAGE_SIZE) == pages[second]) {
                    metadata = (struct test_page_metadata *)allocator.metadata;
                    physical_page_release_order(&allocator, pages[first], 0U);
                    assert(metadata[test_page_index(pages[first])].state ==
                           TEST_PAGE_STATE_FREE_HEAD);
                    metadata[test_page_index(pages[first])].order = 1U;
                    physical_page_release_order(&allocator, pages[second], 0U);
                    break;
                }
            }
            if (second < page_count) {
                break;
            }
        }
        assert(0);
        break;
    }
    case 12: {
        uint64_t pages[64];
        uint32_t page_count = 0U;
        uint32_t first;
        uint32_t second;
        struct test_page_metadata *metadata;

        while (page_count < 64U &&
               physical_page_allocate_order(&allocator,
                                             1U,
                                             &pages[page_count]) == 0) {
            page_count++;
        }
        for (first = 0U; first < page_count; first++) {
            for (second = first + 1U; second < page_count; second++) {
                if ((pages[first] ^
                     (2U * BOAROS_PAGE_SIZE)) == pages[second]) {
                    metadata = (struct test_page_metadata *)allocator.metadata;
                    physical_page_release_order(&allocator, pages[first], 1U);
                    assert(metadata[test_page_index(pages[first]) + 1U].state ==
                           TEST_PAGE_STATE_FREE_TAIL);
                    metadata[test_page_index(pages[first]) + 1U].reserved = 1U;
                    physical_page_release_order(&allocator, pages[second], 1U);
                    break;
                }
            }
            if (second < page_count) {
                break;
            }
        }
        assert(0);
        break;
    }
    }
}
int main(void)
{
    struct rlimit limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &limit) == 0);
    for (unsigned int i = 0; i < 13; i++) {
        pid_t child = fork();
        int status;
        assert(child >= 0);
        if (child == 0) { invalid_release(i); _exit(0); }
        assert(waitpid(child, &status, 0) == child);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGILL) {
            fprintf(stderr, "invalid release case %u returned instead of trapping (status=%d)\n", i, status);
            return 1;
        }
    }
    puts("allocator fatal-release tests passed");
    return 0;
}
