#define _GNU_SOURCE
#include <kernel/cost.h>
#include <kernel/console.h>
#include <kernel/physical_page.h>
#include <kernel/page.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static const uint64_t physical_base = UINT64_C(0x40000000);
static unsigned char *pool;
static size_t pool_bytes;
static struct kernel_cost_task task;
static uint64_t ticks;
static int inaccessible;
void kernel_console_putc(char c) { fputc(c, stderr); }
uint64_t kernel_cost_clock(void) { return ++ticks; }
uint64_t kernel_cost_lock(void) { return 0; }
void kernel_cost_unlock(uint64_t saved) { (void)saved; }
struct kernel_cost_task *kernel_cost_current(void) { return &task; }
static void *access_page(uint64_t address)
{
    if (inaccessible || address < physical_base || address - physical_base >= pool_bytes) return 0;
    return pool + (address - physical_base);
}
static void setup(struct physical_page_allocator *allocator, unsigned pages)
{
    struct boot_memory_layout layout = {0};
    pool_bytes = (size_t)pages * BOAROS_PAGE_SIZE;
    pool = mmap(0, pool_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pool != MAP_FAILED);
    layout.usable_count = 1;
    layout.usable[0].base = physical_base;
    layout.usable[0].size = pool_bytes;
    assert(physical_page_allocator_init(allocator, &layout) == PHYSICAL_PAGE_STATUS_OK);
    assert(physical_page_allocator_bind_access(allocator, access_page) == PHYSICAL_PAGE_STATUS_OK);
    assert(physical_page_allocator_finalize(allocator) == PHYSICAL_PAGE_STATUS_OK);
    assert(physical_page_allocator_audit(allocator) == PHYSICAL_PAGE_STATUS_OK);
}
static void end_measure(unsigned height, unsigned pages)
{
    uint64_t checked, written, elapsed;
    assert(kernel_cost_end(1, 0) == 0);
    assert(kernel_cost_read(0, COST_PAGE_META_CHECKED, &checked) == 0);
    assert(kernel_cost_read(0, COST_PAGE_META_WRITTEN, &written) == 0);
    assert(kernel_cost_read(0, COST_ALLOCATOR_META_TICKS, &elapsed) == 0);
    assert(checked && written && elapsed);
    /* 对齐的2B RAM形成单根；H/L来自公开几何，不读私有树。 */
    assert(checked <= 32ULL*(height+1)*(height+1) + 16ULL*pages + 64ULL*2);
    assert(written <= 8ULL*(height+1) + 4ULL*pages);
    printf("page=%u H=%u checked=%llu written=%llu\n", (unsigned)BOAROS_PAGE_SIZE, height,
           (unsigned long long)checked, (unsigned long long)written);
}
static void force_split(unsigned order)
{
    struct physical_page_allocator allocator;
    unsigned block_pages = 1U << order;
    setup(&allocator, 2U * block_pages);
    uint64_t initial = physical_page_available(&allocator), large, page;
    uint64_t *other = calloc((size_t)initial, sizeof(*other));
    assert(other);
    assert(physical_page_allocate_order(&allocator, order, &large) == 0);
    size_t count = 0;
    while (physical_page_available(&allocator))
        assert(physical_page_allocate(&allocator, &other[count++]) == 0);
    assert(physical_page_release_order(&allocator, large, order) == 0);
    assert(physical_page_allocator_audit(&allocator) == 0);
    assert(kernel_cost_begin(1, 10000000, 1, 0) == 0);
    assert(physical_page_allocate(&allocator, &page) == 0);
    end_measure(order+1, 1);
    assert(page >= large && page - large < (uint64_t)block_pages * BOAROS_PAGE_SIZE);
    assert(kernel_cost_begin(1, 10000000, 1, 0) == 0);
    assert(physical_page_release(&allocator, page) == 0);
    end_measure(order+1, 1);
    assert(physical_page_allocator_audit(&allocator) == 0);
    while (count) assert(physical_page_release(&allocator, other[--count]) == 0);
    assert(physical_page_available(&allocator) == initial);
    assert(physical_page_allocator_audit(&allocator) == 0);
    free(other);
    assert(munmap(pool, pool_bytes) == 0);
}
static void random_owners(void)
{
    struct physical_page_allocator allocator;
    struct { uint64_t address; unsigned order, references; } owners[256] = {0};
    unsigned char owned[256] = {0};
    unsigned rng = 0x56ac92e1U;
    setup(&allocator, 256);
    uint64_t initial = physical_page_available(&allocator);
    for (unsigned step = 0; step < 10000; step++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        unsigned slot = rng & 255U;
        if (owners[slot].references) {
            if (owners[slot].order == 0 && (rng & 0x400U) && owners[slot].references < 4) {
                assert(physical_page_acquire(&allocator, owners[slot].address) == 0);
                owners[slot].references++;
            } else {
                assert(physical_page_release_order(&allocator, owners[slot].address, owners[slot].order) == 0);
                if (!--owners[slot].references) {
                    unsigned index = (owners[slot].address - physical_base) / BOAROS_PAGE_SIZE;
                    for (unsigned p = 0; p < (1U << owners[slot].order); p++) {
                        assert(owned[index+p]); owned[index+p] = 0;
                    }
                }
            }
        } else {
            uint64_t address = UINT64_MAX;
            unsigned order = (rng >> 8) % 5;
            enum physical_page_status status = physical_page_allocate_order(&allocator, order, &address);
            assert(status == PHYSICAL_PAGE_STATUS_OK || status == PHYSICAL_PAGE_STATUS_EMPTY);
            if (status == PHYSICAL_PAGE_STATUS_EMPTY) assert(address == UINT64_MAX);
            else {
                unsigned index = (address - physical_base) / BOAROS_PAGE_SIZE;
                assert(index < 256 && index + (1U << order) <= 256);
                assert((address & (((uint64_t)BOAROS_PAGE_SIZE << order)-1)) == 0);
                for (unsigned p = 0; p < (1U << order); p++) { assert(!owned[index+p]); owned[index+p] = 1; }
                owners[slot].address = address; owners[slot].order = order; owners[slot].references = 1;
                void *pointer;
                uint64_t tail = address + ((uint64_t)BOAROS_PAGE_SIZE << order) - BOAROS_PAGE_SIZE;
                assert(physical_page_resolve(&allocator, tail, &pointer) == 0);
                assert(pointer == pool + (tail - physical_base));
                pointer = (void *)1;
                assert(physical_page_resolve(&allocator, address+1, &pointer) == PHYSICAL_PAGE_STATUS_INVALID);
                assert(pointer == (void *)1);
            }
        }
        unsigned active = 0;
        for (unsigned p = 0; p < 256; p++) active += owned[p];
        assert(physical_page_available(&allocator) + active == initial);
        assert(physical_page_allocator_audit(&allocator) == 0);
    }
    for (unsigned s = 0; s < 256; s++)
        while (owners[s].references) {
            assert(physical_page_release_order(&allocator, owners[s].address, owners[s].order) == 0);
            owners[s].references--;
        }
    assert(physical_page_available(&allocator) == initial);
    assert(physical_page_allocator_audit(&allocator) == 0);
    assert(munmap(pool, pool_bytes) == 0);
}

static void ranges_and_finalize(void)
{
    struct physical_page_allocator allocator, saved;
    struct boot_memory_layout layout = {0};
    pool_bytes = 256U * BOAROS_PAGE_SIZE;
    pool = mmap(0,pool_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(pool != MAP_FAILED);
    layout.usable_count = 2;
    layout.usable[0].base = physical_base + BOAROS_PAGE_SIZE;
    layout.usable[0].size = 59U * BOAROS_PAGE_SIZE;
    layout.usable[1].base = physical_base + 80U * BOAROS_PAGE_SIZE;
    layout.usable[1].size = 71U * BOAROS_PAGE_SIZE;
    assert(physical_page_allocator_audit(0) == PHYSICAL_PAGE_STATUS_INVALID);
    assert(physical_page_allocator_init(&allocator,&layout) == 0);
    assert(physical_page_allocator_audit(&allocator) == PHYSICAL_PAGE_STATUS_STATE);
    assert(physical_page_allocator_bind_access(&allocator,access_page) == 0);
    uint64_t boot[3];
    for (unsigned i=0;i<3;i++) assert(physical_page_allocate(&allocator,&boot[i]) == 0);
    memset(access_page(boot[0]),0xa5,BOAROS_PAGE_SIZE);
    assert(physical_page_release(&allocator,boot[1]) == 0);
    saved = allocator;
    inaccessible = 1;
    assert(physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_INVALID);
    assert(memcmp(&saved,&allocator,sizeof(saved)) == 0);
    inaccessible = 0;
    assert(physical_page_allocator_finalize(&allocator) == 0);
    assert(physical_page_allocator_audit(&allocator) == 0);
    assert(physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_STATE);
    for (size_t i=0;i<BOAROS_PAGE_SIZE;i++) assert(((unsigned char *)access_page(boot[0]))[i] == 0xa5);
    uint64_t initial = physical_page_available(&allocator), pages[256], count=0, run;
    assert(physical_page_allocate_order(&allocator,2,&run) == 0);
    uint32_t order = 99;
    assert(physical_page_allocation_order(&allocator,run+BOAROS_PAGE_SIZE,&order) == PHYSICAL_PAGE_STATUS_INVALID);
    assert(order == 99);
    assert(physical_page_release_order(&allocator,run,2) == 0);
    void *pointer=(void *)1;
    assert(physical_page_resolve(&allocator,allocator.metadata_address,&pointer) == PHYSICAL_PAGE_STATUS_INVALID);
    assert(pointer == (void *)1);
    while (physical_page_available(&allocator)) {
        assert(physical_page_allocate(&allocator,&pages[count]) == 0);
        uint64_t n=(pages[count]-physical_base)/BOAROS_PAGE_SIZE;
        assert((n>=1 && n<60) || (n>=80 && n<151));
        count++;
    }
    assert(physical_page_allocator_audit(&allocator) == 0);
    while (count) assert(physical_page_release(&allocator,pages[--count]) == 0);
    assert(physical_page_available(&allocator) == initial);
    assert(physical_page_release(&allocator,boot[0]) == 0);
    assert(physical_page_release(&allocator,boot[2]) == 0);
    assert(physical_page_available(&allocator)+physical_page_metadata_pages(&allocator) == 130);
    assert(physical_page_allocator_audit(&allocator) == 0);
    struct physical_page_allocator tiny;
    layout.usable_count = 1;
    layout.usable[0].base = physical_base+200U*BOAROS_PAGE_SIZE;
    layout.usable[0].size = BOAROS_PAGE_SIZE;
    assert(physical_page_allocator_init(&tiny,&layout) == 0);
    assert(physical_page_allocator_bind_access(&tiny,access_page) == 0);
    assert(physical_page_allocate(&tiny,&run) == 0);
    saved = tiny;
    assert(physical_page_allocator_finalize(&tiny) == PHYSICAL_PAGE_STATUS_EMPTY);
    assert(memcmp(&saved,&tiny,sizeof(saved)) == 0);
    assert(physical_page_release(&tiny,run) == 0);
    assert(munmap(pool,pool_bytes) == 0);
}
int main(void)
{
    const unsigned orders[] = {6,9,12,15};
    for (unsigned i = 0; i < sizeof(orders)/sizeof(orders[0]); i++) force_split(orders[i]);
    random_owners();
    ranges_and_finalize();
    puts("allocator bounded work and independent owners passed");
}
