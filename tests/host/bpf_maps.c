#define _POSIX_C_SOURCE 200809L
#include <kernel/bpf_map.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/scheduler.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void mm_test_attach(unsigned);
static unsigned char pool[32*1024*1024] __attribute__((aligned(16384)));
static struct physical_page_allocator pages;
static struct kernel_heap heap;
static unsigned fail_heap,fail_page;
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *,size_t,size_t,void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t s,void **p)
{
    if (fail_heap && !--fail_heap) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(h,n,s,p);
}
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *,uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a,uint64_t *p)
{
    if (fail_page && !--fail_page) return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(a,p);
}
void kernel_console_putc(char c) { fputc(c,stderr); }
void mm_test_unavailable(void) { abort(); }
void mm_test_fence(uint64_t v,int i) { (void)v;(void)i; }
static void *access_page(uint64_t p) { return (void *)(uintptr_t)p; }
static int address_of(const void *p,uint64_t *out) { *out=(uintptr_t)p; return 1; }
static void barrier(pthread_barrier_t *b)
{ int r=pthread_barrier_wait(b); assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD); }
static struct bpf_map *create(unsigned type,unsigned entries,unsigned value,unsigned flags)
{
    struct bpf_map *m=0;
    struct bpf_map_spec s={type,type==27?0:4,value,entries,flags};
    assert(!bpf_map_create(&heap,&pages,&s,&m)); return m;
}
static void contiguous(void)
{
    uint64_t before=physical_page_available(&pages),base=0;
    assert(!physical_page_allocate_contiguous(&pages,4,&base));
    assert(physical_page_available(&pages)==before-16);
    for (unsigned i=0;i<16;i++) {
        uint64_t p=base+i*BOAROS_PAGE_SIZE; uint32_t order,refs;
        assert(!physical_page_allocation_order(&pages,p,&order) && !order);
        assert(!physical_page_acquire(&pages,p));
        assert(!physical_page_reference_count(&pages,p,&refs) && refs==2);
        assert(!physical_page_release(&pages,p));
    }
    assert(!physical_page_allocator_audit(&pages));
    /* Freeing alternating owners must neither release nor merge live siblings. */
    for (unsigned parity=0;parity<2;parity++) {
        for (unsigned i=parity;i<16;i+=2) assert(!physical_page_release(&pages,base+i*BOAROS_PAGE_SIZE));
        assert(!physical_page_allocator_audit(&pages));
    }
    assert(physical_page_available(&pages)==before);
    base=123;
    assert(physical_page_allocate_contiguous(&pages,PHYSICAL_PAGE_MAX_ORDER+1,&base)==PHYSICAL_PAGE_STATUS_INVALID && base==123);
}
struct pop_race {struct bpf_map *map;pthread_barrier_t *start;unsigned cpu;int error;uint64_t value;};
static void *pop_one(void *pointer)
{
    struct pop_race *race=pointer;mm_test_attach(race->cpu);unsigned key=7;
    barrier(race->start);
    race->error=bpf_map_lookup_and_delete(race->map,&key,&race->value);
    return NULL;
}
static void lookup_delete(void)
{
    struct bpf_map *map=create(1,2,8,0);unsigned key=7;uint64_t value=1234;
    assert(!bpf_map_update(map,&key,&value,0,1,0));
    bpf_map_borrow_begin(map);const uint64_t *old=bpf_map_lookup_borrowed(map,&key,0);
    assert(old && *old==value);
    pthread_barrier_t start;assert(!pthread_barrier_init(&start,NULL,3));
    struct pop_race calls[2]={{map,&start,0,0,0},{map,&start,1,0,0}};
    pthread_t threads[2];for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,pop_one,&calls[i]));
    barrier(&start);
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert((calls[0].error==0 && calls[1].error==-KERNEL_ENOENT && calls[0].value==value) ||
        (calls[1].error==0 && calls[0].error==-KERNEL_ENOENT && calls[1].value==value));
    assert(*old==value);bpf_map_borrow_end(map);
    assert(!pthread_barrier_destroy(&start));bpf_map_put(map);
}
static void ordinary(void)
{
    for (unsigned type=1;type<=2;type++) {
        struct bpf_map *m=create(type,2,8,0);
        unsigned key=0,other=1,bad=2; uint64_t v=42,out=99;
        assert(!bpf_map_update(m,&key,&v,0,1,0));
        assert(bpf_map_update(m,&key,&v,1,1,0)==-KERNEL_EEXIST);
        assert(!bpf_map_lookup(m,&key,&out,1,0) && out==42);
        assert(bpf_map_update(m,&key,&v,3,1,0)==-KERNEL_EINVAL);
        assert(!bpf_map_update(m,&other,&v,0,1,0));
        assert(bpf_map_update(m,&bad,&v,0,1,0)==-KERNEL_E2BIG);
        if (type==1) {
            assert(!bpf_map_delete(m,&key,1));
            assert(bpf_map_lookup(m,&key,&out,1,0)==-KERNEL_ENOENT);
            assert(bpf_map_update(m,&key,&v,2,1,0)==-KERNEL_ENOENT);
            assert(!bpf_map_update(m,&key,&v,1,1,0));
        } else assert(bpf_map_delete(m,&key,1)==-KERNEL_EINVAL);
        assert(!bpf_map_freeze(m));
        assert(bpf_map_freeze(m)==-KERNEL_EBUSY);
        assert(bpf_map_update(m,&key,&v,0,1,0)==-KERNEL_EPERM);
        assert(!bpf_map_update(m,&key,&v,0,0,0));
        bpf_map_put(m);
    }
    unsigned flags[]={8,16,128,256};
    for(unsigned i=0;i<4;i++) {
        struct bpf_map *m=create(2,1,8,flags[i]);
        unsigned key=0;uint64_t v=1;
        assert(bpf_map_update(m,&key,&v,0,i<2,0)==((i%2)==0?-KERNEL_EPERM:0));
        assert(bpf_map_lookup(m,&key,&v,i<2,0)==((i%2)?-KERNEL_EPERM:0));
        unsigned next=99;
        assert(bpf_map_next_key(m,NULL,&next)==(flags[i]==16?-KERNEL_EPERM:0));
        bpf_map_put(m);
    }
    struct bpf_map *m=0;
    struct bpf_map_spec s={2,3,8,2,0};
    assert(bpf_map_create(&heap,&pages,&s,&m)==-KERNEL_EINVAL);
    s.key_size=4;s.map_flags=1;
    assert(bpf_map_create(&heap,&pages,&s,&m)==-KERNEL_EINVAL);
    s.map_flags=24;
    assert(bpf_map_create(&heap,&pages,&s,&m)==-KERNEL_EINVAL);
}
static void percpu(void)
{
    for(unsigned type=5;type<=6;type++) {
        struct bpf_map *m=create(type,2,3,0);
        assert(bpf_map_cpu_count(m)==2 && bpf_map_user_value_size(m)==16);
        unsigned char values[16]={1,2,3,0,0,0,0,0,4,5,6},out[16]; unsigned key=0;
        assert(!bpf_map_update(m,&key,values,0,1,0));
        assert(!bpf_map_lookup(m,&key,out,1,0) && !memcmp(values,out,16));
        bpf_map_borrow_begin(m);
        unsigned char *p=bpf_map_lookup_borrowed(m,&key,1);assert(p && !memcmp(p,values+8,3));p[0]=7;
        bpf_map_borrow_end(m);
        assert(!bpf_map_lookup(m,&key,out,1,0) && out[0]==1 && out[8]==7);
        unsigned char replacement[3]={9,10,11};
        assert(!bpf_map_update(m,&key,replacement,0,0,0));
        assert(!bpf_map_lookup(m,&key,out,1,0) && out[0]==9 && out[8]==7);
        bpf_map_put(m);
    }
}
static pthread_barrier_t borrowed,removed;
static struct bpf_map *shared;
static void *borrow_worker(void *arg)
{
    (void)arg;mm_test_attach(1); unsigned key=10;
    bpf_map_borrow_begin(shared);
    uint64_t *p=bpf_map_lookup_borrowed(shared,&key,0);assert(p && *p==123);
    barrier(&borrowed);barrier(&removed);
    assert(*p==123);*p=456;
    uint64_t current;assert(!bpf_map_lookup(shared,&key,&current,1,0) && current==789);
    bpf_map_borrow_end(shared);return 0;
}
static void concurrency(void)
{
    shared=create(1,1,8,0);unsigned key=10;uint64_t v=123;
    assert(!bpf_map_update(shared,&key,&v,0,1,0));
    assert(!pthread_barrier_init(&borrowed,0,2) && !pthread_barrier_init(&removed,0,2));
    pthread_t t;assert(!pthread_create(&t,0,borrow_worker,0));barrier(&borrowed);
    assert(!bpf_map_delete(shared,&key,1));v=789;assert(!bpf_map_update(shared,&key,&v,0,1,0));
    barrier(&removed);assert(!pthread_join(t,0));
    assert(!bpf_map_lookup(shared,&key,&v,1,0) && v==789);
    bpf_map_borrow_begin(shared);
    uint64_t *old=bpf_map_lookup_borrowed(shared,&key,0);
    v=999;assert(!bpf_map_update(shared,&key,&v,0,0,0));
    assert(*old==789 && *(uint64_t *)bpf_map_lookup_borrowed(shared,&key,0)==999);
    bpf_map_borrow_end(shared);
    bpf_map_put(shared);assert(!pthread_barrier_destroy(&borrowed) && !pthread_barrier_destroy(&removed));
}
static void batch(void)
{
    for(unsigned type=1;type<=2;type++) {
        struct bpf_map *m=create(type,4,8,0);unsigned keys[]={0,1,2,3},outkeys[4];
        uint64_t values[]={10,20,30,40},out[4];uint32_t count=4,cursor;
        assert(!bpf_map_update_batch(m,keys,values,&count,0) && count==4);
        count=4;
        assert(bpf_map_lookup_batch(m,0,&cursor,outkeys,out,&count,0)==-KERNEL_ENOENT && count==4);
        for(unsigned i=0;i<4;i++) assert(out[i]==values[outkeys[i]]);
        if(type==1) { count=4;assert(bpf_map_lookup_batch(m,0,&cursor,outkeys,out,&count,1)==-KERNEL_ENOENT && count==4);assert(bpf_map_next_key(m,0,outkeys)==-KERNEL_ENOENT); }
        bpf_map_put(m);
    }
}
static void array_mapping(void)
{
    uint64_t before=physical_page_available(&pages);
    struct bpf_map *m=create(2,1,2*BOAROS_PAGE_SIZE+17,1024);
    assert(bpf_map_mmap_size(m)==3*BOAROS_PAGE_SIZE);
    uint64_t first,second,third;unsigned key=0;
    assert(!bpf_map_page_get(m,0,1,&first));
    assert(!bpf_map_page_get(m,BOAROS_PAGE_SIZE,1,&second));
    assert(!bpf_map_page_get(m,2*BOAROS_PAGE_SIZE,1,&third));
    assert(second==first+BOAROS_PAGE_SIZE);
    assert(third==second+BOAROS_PAGE_SIZE);
    bpf_map_borrow_begin(m);unsigned char *p=bpf_map_lookup_borrowed(m,&key,0);
    assert(p==access_page(first));p[BOAROS_PAGE_SIZE+16]=0x37;
    assert(((unsigned char *)access_page(second))[16]==0x37);
    bpf_map_borrow_end(m);
    assert(bpf_map_page_get(m,3*BOAROS_PAGE_SIZE,0,&second)==-KERNEL_EINVAL);
    assert(!bpf_map_freeze(m));assert(bpf_map_mmap_check(m,0,BOAROS_PAGE_SIZE,1)==-KERNEL_EPERM);
    bpf_map_put(m);
    assert(((unsigned char *)access_page(second))[16]==0x37);
    assert(!physical_page_release(&pages,first) && !physical_page_release(&pages,second));
    assert(!physical_page_release(&pages,third));
    assert(physical_page_available(&pages)==before);
}
static uint32_t ring_header_at(uint64_t *physical,unsigned size,uint64_t pos)
{
    pos&=size-1;uint32_t *p=(uint32_t *)((unsigned char *)access_page(physical[2+(pos>>BOAROS_PAGE_SHIFT)])+(pos&(BOAROS_PAGE_SIZE-1)));
    return __atomic_load_n(p,__ATOMIC_ACQUIRE);
}
static unsigned wakes,pollfree;
static void ring_wake(struct kernel_wait_node *node,enum kernel_wait_wake_reason reason)
{
    assert(!kernel_cpu_current()->raw_locks);
    if (reason==KERNEL_WAIT_POLL_FREE) { pollfree++;kernel_wait_queue_remove(node); }
    else { assert(reason==KERNEL_WAIT_WOKEN);wakes++; }
}
static void ring(void)
{
    uint64_t before=physical_page_available(&pages);unsigned size=2*BOAROS_PAGE_SIZE;
    struct bpf_map *m=create(27,size,0,0);struct bpf_ring *r=bpf_map_ring(m);
    struct kernel_wait_node observer;
    kernel_wait_node_init_callback(&observer,ring_wake,0);
    kernel_wait_queue_add(bpf_map_wait_queue(m),&observer);
    uint64_t physical[6];for(unsigned i=0;i<6;i++) assert(!bpf_map_page_get(m,i*BOAROS_PAGE_SIZE,i==0,&physical[i]));
    assert(physical[2]==physical[4] && physical[3]==physical[5]);
    assert(bpf_map_mmap_check(m,BOAROS_PAGE_SIZE,BOAROS_PAGE_SIZE,1)==-KERNEL_EPERM);
    uint64_t *cons=access_page(physical[0]);uint64_t *prod=access_page(physical[1]);
    unsigned char data[64];memset(data,0x51,sizeof data);
    struct bpf_ring_reservation a,b;
    assert(!bpf_ring_reserve(r,17,0,data,sizeof data,&a));
    assert(!bpf_ring_reserve(r,7,0,data,sizeof data,&b));
    assert(*prod==48 && ring_header_at(physical,size,0)==(17U|(1U<<31)));
    assert(!bpf_ring_submit(&b,1,0));
    assert(!wakes);
    assert(ring_header_at(physical,size,0)&(1U<<31));
    assert(!bpf_ring_submit(&a,0,1));assert(ring_header_at(physical,size,0)==(17U|(1U<<30)));
    assert(wakes==1);
    assert(bpf_ring_query(r,0)==48 && bpf_ring_query(r,1)==size && bpf_ring_query(r,2)==0 && bpf_ring_query(r,3)==48);
    assert(bpf_map_poll_readable(m));*cons=48;
    /* Move producer near the end using real completed records; then wrap. */
    unsigned padding=size-48-16;
    unsigned char *large=malloc(size);assert(large);memset(large,0x63,size);
    assert(!bpf_ring_output(r,large,padding-8,1));*cons=*prod;
    assert(*prod==size-16);
    assert(!bpf_ring_output(r,data,32,0));
    assert(ring_header_at(physical,size,size-16)==32);
    for(unsigned i=0;i<32;i++) {
        uint64_t off=(size-8+i)&(size-1);
        assert(((unsigned char *)access_page(physical[2+(off>>BOAROS_PAGE_SHIFT)]))[off&(BOAROS_PAGE_SIZE-1)]==0x51);
    }
    *cons=*prod;
    assert(!bpf_ring_reserve(r,size-16,0,large,size,&a));
    assert(bpf_ring_reserve(r,1,0,data,sizeof data,&b)==-KERNEL_EAGAIN);
    /* Forging the consumer cannot overwrite an outstanding producer. */
    *cons=*prod;assert(bpf_ring_reserve(r,1,0,data,sizeof data,&b)==-KERNEL_EAGAIN);
    assert(!bpf_ring_submit(&a,0,1));
    assert(!bpf_ring_output(r,data,1,0));*cons=*prod;
    assert(!bpf_map_poll_readable(m));
    assert(bpf_ring_output(r,data,1,4)==-KERNEL_EINVAL);
    free(large);bpf_map_put(m);assert(pollfree==1 && !observer.queue);
    for(unsigned i=0;i<6;i++) assert(!physical_page_release(&pages,physical[i]));
    assert(physical_page_available(&pages)==before);
}
static void failed_construction(void)
{
    uint64_t before=physical_page_available(&pages);
    struct bpf_map_spec hash={1,4,8,2,0},ring={27,0,0,BOAROS_PAGE_SIZE,0};
    struct bpf_map *out=(void *)(uintptr_t)1;
    for(unsigned i=1;i<=3;i++) {
        fail_heap=i;
        assert(bpf_map_create(&heap,&pages,&hash,&out)==-KERNEL_ENOMEM);
        assert(out==(void *)(uintptr_t)1 && physical_page_available(&pages)==before);
    }
    for(unsigned i=1;i<=3;i++) {
        fail_page=i;
        assert(bpf_map_create(&heap,&pages,&ring,&out)==-KERNEL_ENOMEM);
        assert(out==(void *)(uintptr_t)1 && physical_page_available(&pages)==before);
    }
    assert(!fail_heap && !fail_page);
}
int main(void)
{
    mm_test_attach(0);struct boot_memory_layout layout={0};layout.usable_count=1;
    layout.usable[0].base=(uintptr_t)pool;layout.usable[0].size=sizeof pool;
    assert(!physical_page_allocator_init(&pages,&layout));
    assert(!physical_page_allocator_bind_access(&pages,access_page));
    assert(!physical_page_allocator_finalize(&pages));
    assert(!kernel_heap_init(&heap,&pages,address_of));
    uint64_t baseline=physical_page_available(&pages);
    contiguous();ordinary();percpu();concurrency();lookup_delete();batch();array_mapping();ring();failed_construction();
    struct kernel_heap_statistics s;kernel_heap_get_statistics(&heap,&s);
    assert(!s.live_allocations && !s.current_pages && physical_page_available(&pages)==baseline);
    assert(!physical_page_allocator_audit(&pages));
    puts("BPF maps: flags, percpu, borrow/delete handshake, batch, ring order/wrap/full and page ownership passed");
}
