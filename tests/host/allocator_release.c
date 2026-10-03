#include <kernel/heap.h>
#include <kernel/sync.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/console.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char pool[256 * BOAROS_PAGE_SIZE]
    __attribute__((aligned(BOAROS_PAGE_SIZE)));
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
/* This standalone allocator fixture has one synchronous execution context. */
static struct kernel_io_context io_context;
struct kernel_io_context *kernel_io_context_current(void) { return &io_context; }

/* Replace only the hardware sink: the allocator emits the real diagnostic. */
static unsigned int console_characters;
static int emergency_selected;
void kernel_console_emergency_begin(void) { emergency_selected = 1; }
void kernel_console_putc(char character)
{
    assert(emergency_selected);
    console_characters++;
    assert(write(STDOUT_FILENO, &character, 1U) == 1);
}

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

static void invalid_release(unsigned int which, uint64_t page, void *object)
{
    uint32_t references;
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

static const char *diagnostic_field(const char *diagnostic, const char *field)
{
    const char *start = diagnostic;
    size_t length = strlen(field);

    for (; *diagnostic != '\0'; diagnostic++) {
        if ((diagnostic == start || diagnostic[-1] == ' ') &&
            strncmp(diagnostic, field, length) == 0) {
            return diagnostic;
        }
    }
    return 0;
}

static uint64_t diagnostic_number(const char *diagnostic, const char *field)
{
    const char *value = diagnostic_field(diagnostic, field);
    uint64_t result = 0U;
    unsigned int digits = 0U;

    assert(value != 0);
    value += strlen(field);
    assert(value[0] == '0' && value[1] == 'x');
    value += 2;
    while ((*value >= '0' && *value <= '9') ||
           (*value >= 'a' && *value <= 'f')) {
        unsigned int digit = *value <= '9' ? (unsigned int)(*value - '0') :
                                             (unsigned int)(*value - 'a' + 10);
        assert(digits++ < 16U);
        result = (result << 4U) | digit;
        value++;
    }
    assert(digits != 0U && (*value == ' ' || *value == '\n'));
    return result;
}

static void check_release_diagnostic(unsigned int which, const char *diagnostic,
                                    uint64_t allocated_page)
{
    static const struct {
        const char *reason;
        unsigned int order;
        int metadata;
    } cases[] = {
        {"reason=wrong-order ", 0U, 1},
        {"reason=not-allocated-head ", 1U, 1},
        {"reason=already-free ", 1U, 1},
        {"reason=unaligned-address ", 0U, 0},
        {0, 0U, 0},
        {0, 0U, 0},
        {"reason=allocator-state ", 0U, 0},
        {"reason=available-count ", 1U, 1},
        {0, 0U, 0},
        {0, 0U, 0},
        {0, 0U, 0},
        {"reason=buddy-free-block ", 0U, 1},
        {"reason=buddy-free-block ", 1U, 1},
    };
    uint64_t address;

    if (cases[which].reason == 0) {
        return;
    }
    if (diagnostic_field(diagnostic, cases[which].reason) == 0) {
        fprintf(stderr, "release case %u missing guard reason: %s\n",
                which, diagnostic);
        assert(0);
    }
    address = diagnostic_number(diagnostic, "address=");
    assert(address >= (uintptr_t)pool &&
           address < (uintptr_t)pool + sizeof(pool));
    if (which <= 7U) {
        uint64_t expected_address = allocated_page;

        if (which == 1U) expected_address += BOAROS_PAGE_SIZE;
        if (which == 3U) expected_address++;
        assert(address == expected_address);
    }
    assert(diagnostic_number(diagnostic, "requested_order=") ==
           cases[which].order);
    if (cases[which].metadata) {
        assert(diagnostic_number(diagnostic, "page_index=") < 256U);
        (void)diagnostic_number(diagnostic, "state=");
        (void)diagnostic_number(diagnostic, "stored_order=");
        (void)diagnostic_number(diagnostic, "references=");
        (void)diagnostic_number(diagnostic, "next=");
        (void)diagnostic_number(diagnostic, "previous=");
        (void)diagnostic_number(diagnostic, "reserved=");
    } else {
        assert(diagnostic_field(diagnostic, "page_index=") == 0);
        assert(diagnostic_field(diagnostic, "state=") == 0);
        assert(diagnostic_field(diagnostic, "references=") == 0);
    }
    if (which == 0U) {
        assert(diagnostic_number(diagnostic, "stored_order=") == 1U);
        assert(diagnostic_number(diagnostic, "references=") == 1U);
    }
    if (which == 7U) {
        assert(diagnostic_number(diagnostic, "available=") >
               diagnostic_number(diagnostic, "total="));
    }
    if (which == 11U || which == 12U) {
        uint64_t inspected = diagnostic_number(diagnostic, "metadata_address=");
        assert(inspected >= (uintptr_t)pool &&
               inspected < (uintptr_t)pool + sizeof(pool));
        assert(inspected != address);
        assert(inspected ==
               (address ^ ((UINT64_C(1) << cases[which].order) *
                            BOAROS_PAGE_SIZE)));
    }
}

static void test_legal_release_is_silent(void)
{
    uint64_t page;
    uint64_t available;

    setup();
    available = physical_page_available(&allocator);
    assert(physical_page_allocate(&allocator, &page) == 0);
    assert(physical_page_acquire(&allocator, page) == 0);
    assert(physical_page_release(&allocator, page) == 0);
    assert(physical_page_available(&allocator) == available - 1U);
    assert(physical_page_release(&allocator, page) == 0);
    assert(physical_page_allocate_order(&allocator, 1U, &page) == 0);
    assert(physical_page_release_order(&allocator, page, 1U) == 0);
    assert(physical_page_available(&allocator) == available);
    assert(console_characters == 0U);
}

int main(void)
{
    struct rlimit limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &limit) == 0);
    test_legal_release_is_silent();
    for (unsigned int i = 0; i < 13; i++) {
        int output[2];
        char diagnostic[2048];
        size_t length = 0U;
        ssize_t bytes;
        uint64_t page;
        void *object;

        setup();
        assert(physical_page_allocate_order(&allocator, 1U, &page) == 0);
        assert(kernel_heap_allocate(&heap, 32U, &object) == 0);
        assert(pipe(output) == 0);
        pid_t child = fork();
        int status;
        assert(child >= 0);
        if (child == 0) {
            close(output[0]);
            assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
            close(output[1]);
            invalid_release(i, page, object);
            _exit(0);
        }
        close(output[1]);
        while ((bytes = read(output[0], diagnostic + length,
                             sizeof(diagnostic) - 1U - length)) > 0) {
            length += (size_t)bytes;
            assert(length < sizeof(diagnostic) - 1U);
        }
        assert(bytes == 0);
        close(output[0]);
        diagnostic[length] = '\0';
        assert(waitpid(child, &status, 0) == child);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGILL) {
            fprintf(stderr, "invalid release case %u returned instead of trapping (status=%d)\n", i, status);
            return 1;
        }
        check_release_diagnostic(i, diagnostic, page);
    }
    puts("allocator fatal-release tests passed");
    return 0;
}
