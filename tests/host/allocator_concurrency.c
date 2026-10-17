#define _POSIX_C_SOURCE 200809L
#include <kernel/cpu.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

_Thread_local uintptr_t sync_test_irq = 1;
static _Thread_local struct kernel_cpu cpu;
static _Thread_local struct kernel_io_context io;
void *sync_test_cpu(void) { return &cpu; }
struct kernel_io_context *kernel_io_context_current(void) { return &io; }
void kernel_console_putc(char c) { fputc(c, stderr); }
static struct physical_page_allocator allocator;
static struct kernel_heap heap;
static unsigned char *pool;
static size_t pool_bytes;
static unsigned *owners;
static pthread_mutex_t model = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t gate;
static unsigned thread_count;
static uint64_t shared;
struct allocation { void *pointer; size_t bytes; unsigned order, page; unsigned char pattern; };
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int page_address(const void *pointer, uint64_t *address)
{ *address = (uintptr_t)pointer; return 1; }
static void barrier(void)
{ int s = pthread_barrier_wait(&gate); assert(!s || s == PTHREAD_BARRIER_SERIAL_THREAD); }
static void track_locked(struct allocation *a, unsigned id, int acquire)
{
    uintptr_t offset = (unsigned char *)a->pointer - pool;
    assert(offset < pool_bytes && a->bytes <= pool_bytes-offset && !(offset % 16));
    size_t first = offset / 16, count = (a->bytes + 15) / 16;
    for (size_t i = first; i < first+count; i++) {
        if (acquire) { assert(!owners[i]); owners[i] = id + 1; }
        else { assert(owners[i] == id + 1); owners[i] = 0; }
    }
}
static void track(struct allocation *a, unsigned id, int acquire)
{
    assert(pthread_mutex_lock(&model) == 0);
    track_locked(a, id, acquire);
    assert(pthread_mutex_unlock(&model) == 0);
}
static void discard(struct allocation *a, unsigned id)
{
    for (size_t i = 0; i < a->bytes; i++) assert(((unsigned char *)a->pointer)[i] == a->pattern);
    track(a, id, 0);
    if (a->page) assert(physical_page_release_order(&allocator, (uintptr_t)a->pointer, a->order) == 0);
    else kernel_heap_release(&heap, a->pointer);
    *a = (struct allocation){0};
}
static unsigned random_next(unsigned *state)
{ *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5; return *state; }
static void *worker(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument, random = 0x9173U + id*4711U;
    kernel_cpu_initialize(&cpu, id, &io);
    struct allocation held[32] = {0};
    barrier();
    for (unsigned iteration = 0; iteration < 10000; iteration++) {
        unsigned slot = random_next(&random) % 32;
        struct allocation *a = &held[slot];
        if (a->pointer) {
            if (!a->page && a->bytes < BOAROS_PAGE_SIZE*4 && !(iteration % 8)) {
                assert(pthread_mutex_lock(&model) == 0);
                void *grown = 0;
                int resized = kernel_heap_resize(&heap, a->pointer, a->bytes*2, &grown);
                assert(!resized || resized == KERNEL_HEAP_STATUS_EMPTY);
                if (!resized) {
                    for (size_t i = 0; i < a->bytes; i++) assert(((unsigned char *)grown)[i] == a->pattern);
                    /* 仅resize的账簿交接串行；生产分配/释放仍和其他线程并发。 */
                    track_locked(a, id, 0); a->pointer = grown; a->bytes *= 2;
                    track_locked(a, id, 1); memset(a->pointer, a->pattern, a->bytes);
                }
                assert(pthread_mutex_unlock(&model) == 0);
            } else discard(a, id);
            continue;
        }
        unsigned kind = random_next(&random) % 5;
        int status;
        if (kind < 2) {
            a->page = 1; a->order = kind ? random_next(&random)%4 : 0;
            uint64_t address;
            status = physical_page_allocate_order(&allocator, a->order, &address);
            if (!status) a->pointer = access_page(address);
            a->bytes = (size_t)BOAROS_PAGE_SIZE << a->order;
        } else {
            unsigned shift = KERNEL_HEAP_MIN_CLASS_SHIFT + random_next(&random) % (KERNEL_HEAP_SIZE_CLASS_COUNT+2);
            a->bytes = (size_t)1 << shift;
            status = kind == 4 ? kernel_heap_allocate_zeroed(&heap, 1, a->bytes, &a->pointer)
                               : kernel_heap_allocate(&heap, a->bytes, &a->pointer);
            if (!status && kind == 4)
                for (size_t i = 0; i < a->bytes; i++) assert(!((unsigned char *)a->pointer)[i]);
        }
        assert(!status || status == PHYSICAL_PAGE_STATUS_EMPTY);
        if (status) { *a = (struct allocation){0}; continue; }
        track(a, id, 1); a->pattern = (unsigned char)(id*37+iteration%201+1);
        memset(a->pointer, a->pattern, a->bytes);
        assert(physical_page_acquire(&allocator, shared) == 0);
        uint32_t refs;
        assert(physical_page_reference_count(&allocator, shared, &refs) == 0 && refs > 1);
        assert(physical_page_release(&allocator, shared) == 0);
    }
    for (unsigned i = 0; i < 32; i++) if (held[i].pointer) discard(&held[i], id);
    barrier();
    /* 所有线程同时逼近耗尽；在所有owner登记完成前不释放任何页。 */
    struct allocation exhausted[1024]; unsigned used = 0;
    while (used < 1024) {
        uint64_t address; int status = physical_page_allocate(&allocator, &address);
        assert(!status || status == PHYSICAL_PAGE_STATUS_EMPTY);
        if (status) break;
        struct allocation *a = &exhausted[used++];
        *a = (struct allocation){ .pointer = access_page(address), .bytes = BOAROS_PAGE_SIZE,
                                  .page = 1, .pattern = (unsigned char)(id+1) };
        track(a, id, 1); memset(a->pointer, a->pattern, a->bytes);
    }
    barrier();
    assert(physical_page_available(&allocator) == 0);
    barrier();
    for (unsigned i = 0; i < used; i++) discard(&exhausted[i], id);
    assert(!cpu.raw_locks && !cpu.preempt_depth && sync_test_irq);
    return 0;
}
struct hook_owner { unsigned references; uint64_t value; };
static pthread_barrier_t hook_entered, hook_continue;
static unsigned hook_freed;
static void hook_retain(void *context)
{
    struct hook_owner *owner=context;
    assert(__atomic_fetch_add(&owner->references,1,__ATOMIC_RELAXED));
}
static void hook_release(void *context)
{
    struct hook_owner *owner=context;
    unsigned references=__atomic_fetch_sub(&owner->references,1,__ATOMIC_ACQ_REL);
    assert(references);
    if (references==1) { free(owner);__atomic_store_n(&hook_freed,1,__ATOMIC_RELEASE); }
}
static uint64_t hook_reclaim(void *context,uint64_t count)
{ (void)context;(void)count;assert(0);return 0; }
static void hook_notify(void *context)
{
    struct hook_owner *owner=context;
    assert(cpu.logical_id==1 && !cpu.raw_locks && !cpu.preempt_depth && sync_test_irq);
    assert(owner->value==UINT64_C(0x713791ca));
    pthread_barrier_wait(&hook_entered);
    pthread_barrier_wait(&hook_continue);
    assert(!__atomic_load_n(&hook_freed,__ATOMIC_ACQUIRE) && owner->value==UINT64_C(0x713791ca));
}
static void *hook_allocate(void *unused)
{
    (void)unused;kernel_cpu_initialize(&cpu,1,&io);cpu.logical_id=1;
    uint64_t page;assert(!physical_page_allocate(&allocator,&page));
    assert(!physical_page_release(&allocator,page));return NULL;
}
static void hook_lifetime(void)
{
    struct hook_owner *owner=malloc(sizeof(*owner));assert(owner);
    *owner=(struct hook_owner){.references=1,.value=UINT64_C(0x713791ca)};
    hook_freed=0;
    assert(!pthread_barrier_init(&hook_entered,NULL,2) && !pthread_barrier_init(&hook_continue,NULL,2));
    assert(!physical_page_allocator_set_reclaimer(&allocator,&(struct physical_page_reclaim_hooks){
        .reclaim=hook_reclaim,.notify=hook_notify,.retain=hook_retain,.release=hook_release,.context=owner}));
    pthread_t thread;assert(!pthread_create(&thread,NULL,hook_allocate,NULL));
    pthread_barrier_wait(&hook_entered);
    assert(!physical_page_allocator_clear_reclaimer(&allocator));
    hook_release(owner);
    assert(!__atomic_load_n(&hook_freed,__ATOMIC_ACQUIRE));
    pthread_barrier_wait(&hook_continue);
    assert(!pthread_join(thread,NULL) && __atomic_load_n(&hook_freed,__ATOMIC_ACQUIRE));
    assert(!pthread_barrier_destroy(&hook_entered) && !pthread_barrier_destroy(&hook_continue));
    puts("allocator remote notification survives concurrent hook detach; final context owner returned");
}
static void run(unsigned threads)
{
    thread_count = threads;
    kernel_cpu_initialize(&cpu, 99, &io);
    pool_bytes = 1024U * BOAROS_PAGE_SIZE;
    assert(posix_memalign((void **)&pool, BOAROS_PAGE_SIZE, pool_bytes) == 0);
    owners = calloc(pool_bytes/16, sizeof(*owners)); assert(owners);
    struct boot_memory_layout layout = {0};
    layout.usable_count = 1; layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = pool_bytes;
    assert(physical_page_allocator_init(&allocator, &layout) == 0);
    assert(physical_page_allocator_bind_access(&allocator, access_page) == 0);
    assert(physical_page_allocator_finalize(&allocator) == 0);
    assert(kernel_heap_init(&heap, &allocator, page_address) == 0);
    uint64_t baseline = physical_page_available(&allocator);
    assert(physical_page_allocate(&allocator, &shared) == 0);
    assert(pthread_barrier_init(&gate, 0, thread_count) == 0);
    pthread_t ids[4];
    for (unsigned i = 0; i < threads; i++) assert(pthread_create(&ids[i], 0, worker, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < threads; i++) assert(pthread_join(ids[i], 0) == 0);
    assert(pthread_barrier_destroy(&gate) == 0);
    assert(physical_page_release(&allocator, shared) == 0);
    hook_lifetime();
    assert(physical_page_allocator_audit(&allocator) == 0);
    struct kernel_heap_statistics stats; kernel_heap_get_statistics(&heap, &stats);
    assert(!stats.live_allocations && !stats.current_pages);
    assert(physical_page_available(&allocator) == baseline);
    for (size_t i = 0; i < pool_bytes/16; i++) assert(!owners[i]);
    free(owners); free(pool);
    printf("allocator concurrent owners: page=%u threads=%u passed\n", (unsigned)BOAROS_PAGE_SIZE, threads);
}
int main(void)
{ struct rlimit limit = {0}; assert(setrlimit(RLIMIT_CORE, &limit) == 0); run(2); run(4); }
