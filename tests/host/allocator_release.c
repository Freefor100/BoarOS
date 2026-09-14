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
    }
}
int main(void)
{
    struct rlimit limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &limit) == 0);
    for (unsigned int i = 0; i < 10; i++) {
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
