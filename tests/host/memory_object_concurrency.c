#define _POSIX_C_SOURCE 200809L
#include <kernel/memory_object.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <kernel/cpu.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void mm_test_attach(unsigned);
static unsigned char pool[16 * 1024 * 1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static struct kernel_memory_object *objects[4];
static struct kernel_memory_budget budget;
static pthread_barrier_t start, prepared;
static unsigned charging, quota, actors, preparing_pages;
static uint64_t addresses[4];
static enum kernel_memory_object_status results[4];
static unsigned created[4];
static unsigned fail_heap;
static uint64_t allocations, data_allocations, resolutions;
static size_t object_bytes;
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t v, int i) { (void)v; (void)i; }
static void *access(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
static void barrier(pthread_barrier_t *b)
{ int r = pthread_barrier_wait(b); assert(!r || r == PTHREAD_BARRIER_SERIAL_THREAD); }
enum physical_page_status __real_physical_page_acquire(struct physical_page_allocator *, uint64_t);
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *p, uint64_t *a)
{
    __atomic_fetch_add(&data_allocations,1,__ATOMIC_RELAXED);
    return __real_physical_page_allocate(p,a);
}
enum physical_page_status __real_physical_page_resolve(const struct physical_page_allocator *, uint64_t, void **);
enum physical_page_status __wrap_physical_page_resolve(const struct physical_page_allocator *p, uint64_t a, void **out)
{
    __atomic_fetch_add(&resolutions,1,__ATOMIC_RELAXED);
    return __real_physical_page_resolve(p,a,out);
}
enum physical_page_status __wrap_physical_page_acquire(struct physical_page_allocator *p, uint64_t a)
{
    enum physical_page_status r = __real_physical_page_acquire(p, a);
    /* 每个对象都完成自己的页owner准备后才竞争共用预算；不观察私有布局。 */
    if (charging && p == &pages) barrier(&prepared);
    return r;
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *, size_t, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h, size_t n, size_t s, void **p)
{
    __atomic_fetch_add(&allocations, 1, __ATOMIC_RELAXED);
    if (!object_bytes) object_bytes=n*s;
    if (fail_heap && !--fail_heap) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(h,n,s,p);
}
static void *create_page(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    mm_test_attach(id + 1);
    barrier(&start);
    int new_page = 0;
    results[id] = preparing_pages
        ? kernel_memory_object_prepare_page(objects[0],7,&addresses[id],&new_page)
        : kernel_memory_object_get_page(objects[quota ? id : 0],7,&addresses[id],&new_page);
    created[id] = new_page;
    return 0;
}
static void parallel_pages(int limited)
{
    quota = limited; budget = (struct kernel_memory_budget){.limit = 1};
    unsigned count = limited ? actors : 1;
    for (unsigned i = 0; i < count; i++) {
        assert(!kernel_memory_object_create(&heap, &pages, &objects[i]));
        assert(!kernel_memory_object_check_domain(objects[i], &heap, &pages));
        if (limited) kernel_memory_object_set_budget(objects[i], &budget);
        if (limited) assert(kernel_memory_object_check_domain(objects[i], &heap, &pages) == KERNEL_MEMORY_OBJECT_STATE);
    }
    uint64_t before = physical_page_available(&pages), data_before=data_allocations;
    memset(results, 0xff, sizeof(results)); memset(created, 0, sizeof(created));
    assert(!pthread_barrier_init(&start,0,actors + 1));
    assert(!pthread_barrier_init(&prepared,0,actors));
    charging = limited;
    pthread_t workers[4];
    for (unsigned i=0;i<actors;i++) assert(!pthread_create(&workers[i],0,create_page,(void *)(uintptr_t)i));
    barrier(&start);
    for (unsigned i=0;i<actors;i++) assert(!pthread_join(workers[i],0));
    charging = 0;
    unsigned successes = 0, publishes = 0;
    for (unsigned i=0;i<actors;i++) {
        if (results[i] == KERNEL_MEMORY_OBJECT_OK) {
            successes++; publishes += created[i];
            if (!limited) assert(addresses[i] == addresses[0]);
            assert(!physical_page_release(&pages, addresses[i]));
        } else assert(limited && results[i] == KERNEL_MEMORY_OBJECT_NO_SPACE);
    }
    assert(successes == (limited ? 1U : actors) && publishes == 1);
    assert(data_allocations-data_before==(limited?actors:1U));
    assert(pages.shared_anon_pages == 1 && (!limited || budget.used == 1));
    assert(physical_page_available(&pages) <= before - 1);
    for (unsigned i=0;i<count;i++) kernel_memory_object_release(&objects[i]);
    assert(!pages.shared_anon_pages && !budget.used);
    assert(!pthread_barrier_destroy(&start) && !pthread_barrier_destroy(&prepared));
}
static void *reference_owner(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    mm_test_attach(id + 1);
    barrier(&start);
    for (unsigned i=0;i<10000;i++) {
        assert(!kernel_memory_object_acquire(objects[0]));
        struct kernel_memory_object *owner = objects[0];
        kernel_memory_object_release(&owner);
        assert(!owner);
    }
    assert(!kernel_memory_object_acquire(objects[0]));
    barrier(&prepared); /* 全部actor的实际引用已存在，主owner此时可结束。 */
    struct kernel_memory_object *owner = objects[0];
    barrier(&prepared);
    kernel_memory_object_release(&owner);
    return 0;
}
static void references(void)
{
    assert(!kernel_memory_object_create(&heap,&pages,&objects[0]));
    assert(!pthread_barrier_init(&start,0,actors+1));
    assert(!pthread_barrier_init(&prepared,0,actors+1));
    pthread_t workers[4];
    for(unsigned i=0;i<actors;i++) assert(!pthread_create(&workers[i],0,reference_owner,(void *)(uintptr_t)i));
    barrier(&start); barrier(&prepared);
    struct kernel_memory_object *root = objects[0];
    kernel_memory_object_release(&root);
    barrier(&prepared);
    for(unsigned i=0;i<actors;i++) assert(!pthread_join(workers[i],0));
    objects[0]=0;
    assert(!pthread_barrier_destroy(&start) && !pthread_barrier_destroy(&prepared));
}
static void preparations(int accepted)
{
    quota=0; preparing_pages=1;
    assert(!kernel_memory_object_create(&heap,&pages,&objects[0]));
    assert(!pthread_barrier_init(&start,0,actors+1));
    pthread_t workers[4];
    for(unsigned i=0;i<actors;i++) assert(!pthread_create(&workers[i],0,create_page,(void *)(uintptr_t)i));
    barrier(&start);
    unsigned publishes=0;
    for(unsigned i=0;i<actors;i++) {
        assert(!pthread_join(workers[i],0) && results[i]==KERNEL_MEMORY_OBJECT_OK);
        publishes+=created[i]; assert(addresses[i]==addresses[0]);
    }
    assert(publishes==1 && pages.shared_anon_pages==1);
    if(accepted) {
        kernel_memory_object_finish_page(objects[0],7,addresses[0],1);
        ((unsigned char *)access(addresses[0]))[0]=0x5b;
        assert(!physical_page_release(&pages,addresses[0])); /* adopter的PTE先结束。 */
    }
    for(unsigned i=accepted!=0;i<actors;i++)
        kernel_memory_object_finish_page(objects[0],7,addresses[i],0);
    assert(kernel_memory_object_resident_pages(objects[0])==(unsigned)(accepted!=0));
    assert(pages.shared_anon_pages==(unsigned)(accepted!=0));
    if(accepted) {
        uint64_t p; assert(!kernel_memory_object_find_page(objects[0],7,&p));
        assert(((unsigned char *)access(p))[0]==0x5b && !physical_page_release(&pages,p));
    }
    kernel_memory_object_release(&objects[0]); preparing_pages=0;
    assert(!pthread_barrier_destroy(&start) && !pages.shared_anon_pages);
}
static void sequential(void)
{
    for (unsigned phase=1;phase<=2;phase++) {
        fail_heap=phase;
        uint64_t before=physical_page_available(&pages);
        assert(kernel_memory_object_create(&heap,&pages,&objects[0]) == KERNEL_MEMORY_OBJECT_NO_MEMORY);
        assert(!objects[0] && physical_page_available(&pages)==before);
    }
    assert(!kernel_memory_object_create(&heap,&pages,&objects[0]));
    for (unsigned i=0;i<80;i++) {
        if (i==32) {
            uint64_t p=UINT64_MAX, before=physical_page_available(&pages); int made=9;
            fail_heap=1;
            assert(kernel_memory_object_get_page(objects[0],i,&p,&made)==KERNEL_MEMORY_OBJECT_NO_MEMORY);
            assert(p==UINT64_MAX && made==9 && physical_page_available(&pages)==before &&
                kernel_memory_object_resident_pages(objects[0])==i);
        }
        int made; uint64_t p;
        assert(!kernel_memory_object_get_page(objects[0],i,&p,&made) && made);
        assert(!physical_page_release(&pages,p));
    }
    uint64_t calls=allocations, before=physical_page_available(&pages);
    uint64_t data_before=data_allocations, resolve_before=resolutions;
    for (unsigned i=0;i<80;i++) {
        int made; uint64_t p;
        assert(!kernel_memory_object_get_page(objects[0],i,&p,&made) && !made);
        assert(!physical_page_release(&pages,p));
        assert(!kernel_memory_object_find_page(objects[0],i,&p));
        assert(!physical_page_release(&pages,p));
    }
    assert(allocations==calls && physical_page_available(&pages)==before &&
        data_allocations==data_before && resolutions==resolve_before);
    fail_heap=1; uint64_t p=UINT64_MAX; int made=9;
    assert(kernel_memory_object_get_page(objects[0],1000,&p,&made)==KERNEL_MEMORY_OBJECT_NO_MEMORY);
    assert(p==UINT64_MAX && made==9 && physical_page_available(&pages)==before);
    uint64_t pressure[4096]; unsigned held=0;
    while(held<4096 && !physical_page_allocate(&pages,&pressure[held])) held++;
    assert(kernel_memory_object_get_page(objects[0],1000,&p,&made)==KERNEL_MEMORY_OBJECT_NO_MEMORY);
    assert(p==UINT64_MAX && made==9 && !physical_page_available(&pages));
    for(unsigned i=0;i<held;i++) physical_page_release(&pages,pressure[i]);
    assert(!kernel_memory_object_get_page(objects[0],1000,&p,&made) && made);
    uint32_t refs; assert(!physical_page_reference_count(&pages,p,&refs) && refs==2);
    kernel_memory_object_discard_new_page(objects[0],1000,p); /* caller尚未归还，不能删 */
    assert(!physical_page_release(&pages,p));
    kernel_memory_object_discard_new_page(objects[0],1000,p);
    assert(kernel_memory_object_resident_pages(objects[0])==80);
    kernel_memory_object_truncate(objects[0],0);
    assert(!kernel_memory_object_resident_pages(objects[0]) && !pages.shared_anon_pages);
    kernel_memory_object_release(&objects[0]);
}
int main(void)
{
    mm_test_attach(0);
    struct boot_memory_layout layout={0}; layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool; layout.usable[0].size=sizeof(pool);
    assert(!physical_page_allocator_init(&pages,&layout));
    assert(!physical_page_allocator_bind_access(&pages,access));
    assert(!physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    uint64_t baseline=physical_page_available(&pages);
    for(actors=2;actors<=4;actors+=2) {
        parallel_pages(1); parallel_pages(0); references(); preparations(0); preparations(1);
    }
    sequential();
    struct kernel_heap_statistics s; kernel_heap_get_statistics(&heap,&s);
    assert(!s.live_allocations && !s.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    printf("memory object requested bytes=%zu; warm get/find metadata allocations=0\n",object_bytes);
    puts("memory object: 2/4 actors quota, one publication, references, OOM, hit costs and owners passed");
}
