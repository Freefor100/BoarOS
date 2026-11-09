#include <kernel/bpf_map.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/scheduler.h>
#include <kernel/irq.h>
#include <string.h>

#define BUSY (1U<<31)
#define DISCARD (1U<<30)
#define NO_WAKEUP 1U
#define FORCE_WAKEUP 2U
struct ring_page { uint64_t physical; void *data; };
struct bpf_ring {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_raw_lock lock;
    struct kernel_wait_queue waiters;
    uint32_t size, page_count, allocated;
    uint64_t pending;
    struct ring_page pages[];
};
struct ring_header { uint32_t length, page_offset; };
static uint64_t *consumer(struct bpf_ring *r) { return r->pages[0].data; }
static uint64_t *producer(struct bpf_ring *r) { return r->pages[1].data; }
static struct ring_header *header(struct bpf_ring *r,uint64_t position)
{
    uint64_t offset=position&(r->size-1U);
    return (struct ring_header *)((unsigned char *)r->pages[2+(offset>>BOAROS_PAGE_SHIFT)].data+
        (offset&(BOAROS_PAGE_SIZE-1U)));
}
static uint64_t record_size(uint32_t n) { return ((uint64_t)n+15U)&~UINT64_C(7); }
int bpf_ring_create(struct kernel_heap *heap,struct physical_page_allocator *allocator,uint32_t size,struct bpf_ring **out)
{
    struct bpf_ring *r;
    uint32_t count=2+(size>>BOAROS_PAGE_SHIFT);
    if (kernel_heap_allocate_zeroed(heap,1,sizeof *r+count*sizeof(struct ring_page),(void **)&r)) return -KERNEL_ENOMEM;
    r->heap=heap; r->allocator=allocator; r->size=size; r->page_count=count;
    kernel_raw_lock_init(&r->lock,KERNEL_RAW_RANK_OBJECT);
    kernel_wait_queue_init(&r->waiters);
    for (unsigned i=0;i<count;i++) {
        if (physical_page_allocate(allocator,&r->pages[i].physical)) { bpf_ring_destroy(r); return -KERNEL_ENOMEM; }
        r->allocated++;
        if (physical_page_map_owned(allocator,r->pages[i].physical,&r->pages[i].data)) __builtin_trap();
        memset(r->pages[i].data,0,BOAROS_PAGE_SIZE);
    }
    *out=r; return 0;
}
void bpf_ring_destroy(struct bpf_ring *r)
{
    kernel_wait_queue_pollfree(&r->waiters);
    if (kernel_wait_queue_destroy(&r->waiters)) __builtin_trap();
    for (unsigned i=0;i<r->allocated;i++) physical_page_release(r->allocator,r->pages[i].physical);
    kernel_heap_release(r->heap,r);
}
int bpf_ring_page_get(struct bpf_ring *r,uint64_t offset,int writable,uint64_t *out)
{
    if ((offset&(BOAROS_PAGE_SIZE-1U)) || offset>=2*BOAROS_PAGE_SIZE+2*(uint64_t)r->size) return -KERNEL_EINVAL;
    if (writable && offset) return -KERNEL_EPERM;
    uint64_t index=offset>>BOAROS_PAGE_SHIFT;
    if (index>=r->page_count) index-=r->page_count-2;
    uint64_t p=r->pages[index].physical;
    if (physical_page_acquire(r->allocator,p)) __builtin_trap();
    *out=p; return 0;
}
int bpf_ring_reserve(struct bpf_ring *r,uint32_t size,uint64_t flags,void *storage,size_t capacity,struct bpf_ring_reservation *out)
{
    if (!r || !out || flags || size>(UINT32_MAX>>2) || size>capacity || (!storage && size)) return -KERNEL_EINVAL;
    uint64_t n=record_size(size);
    if (n>=r->size) return -KERNEL_E2BIG;
    KERNEL_RAW_SCOPE(g,&r->lock);
    uint64_t prod=__atomic_load_n(producer(r),__ATOMIC_RELAXED);
    uint64_t cons=__atomic_load_n(consumer(r),__ATOMIC_ACQUIRE);
    /* A consumer may advance maliciously. Pending busy records remain owned by
     * their producers and must never be overwritten by a later reservation. */
    while (r->pending!=prod) {
        uint32_t len=__atomic_load_n(&header(r,r->pending)->length,__ATOMIC_ACQUIRE);
        if (len&BUSY) break;
        r->pending+=record_size(len&~DISCARD);
    }
    uint64_t next=prod+n;
    if (next-cons>r->size-1U || next-r->pending>r->size-1U) return -KERNEL_EAGAIN;
    struct ring_header *h=header(r,prod);
    h->page_offset=0;
    __atomic_store_n(&h->length,size|BUSY,__ATOMIC_RELAXED);
    __atomic_store_n(producer(r),next,__ATOMIC_RELEASE);
    *out=(struct bpf_ring_reservation){r,storage,prod,size};
    return 0;
}
static void copy_record(struct bpf_ring *r,uint64_t position,const void *data,uint32_t size)
{
    const unsigned char *src=data;
    while (size) {
        uint64_t offset=position&(r->size-1U);
        unsigned within=offset&(BOAROS_PAGE_SIZE-1U);
        unsigned n=BOAROS_PAGE_SIZE-within; if (n>size) n=size;
        memcpy((unsigned char *)r->pages[2+(offset>>BOAROS_PAGE_SHIFT)].data+within,src,n);
        position+=n; src+=n; size-=n;
    }
}
int bpf_ring_submit(struct bpf_ring_reservation *reservation,uint64_t flags,int discard)
{
    if (!reservation || !reservation->ring) return -KERNEL_EINVAL;
    struct bpf_ring *r=reservation->ring;
    struct ring_header *h=header(r,reservation->position);
    if (__atomic_load_n(&h->length,__ATOMIC_ACQUIRE)!=(reservation->size|BUSY)) __builtin_trap();
    if (!discard) copy_record(r,reservation->position+8,reservation->data,reservation->size);
    __atomic_store_n(&h->length,reservation->size|(discard?DISCARD:0),__ATOMIC_RELEASE);
    int wake=(flags&FORCE_WAKEUP) || (!(flags&NO_WAKEUP) &&
        __atomic_load_n(consumer(r),__ATOMIC_ACQUIRE)==reservation->position);
    reservation->ring=0;
    /* Wait callbacks can acquire unrelated object locks; no ring lock is held. */
    if (wake) {
        KERNEL_IRQ_SCOPE(irq);
        kernel_wait_queue_wake_all(&r->waiters);
    }
    return 0;
}
int bpf_ring_output(struct bpf_ring *r,const void *data,uint32_t size,uint64_t flags)
{
    if (flags&~(uint64_t)(NO_WAKEUP|FORCE_WAKEUP)) return -KERNEL_EINVAL;
    struct bpf_ring_reservation reservation;
    int e=bpf_ring_reserve(r,size,0,(void *)data,size,&reservation);
    return e ? -KERNEL_EAGAIN : bpf_ring_submit(&reservation,flags,0);
}
uint64_t bpf_ring_query(struct bpf_ring *r,uint64_t flags)
{
    if (!r) return 0;
    switch (flags) {
    case 0: return __atomic_load_n(producer(r),__ATOMIC_ACQUIRE)-__atomic_load_n(consumer(r),__ATOMIC_ACQUIRE);
    case 1: return r->size;
    case 2: return __atomic_load_n(consumer(r),__ATOMIC_ACQUIRE);
    case 3: return __atomic_load_n(producer(r),__ATOMIC_ACQUIRE);
    default: return 0;
    }
}
struct kernel_wait_queue *bpf_ring_wait_queue(struct bpf_ring *r) { return &r->waiters; }
int bpf_ring_poll_readable(struct bpf_ring *r) { return bpf_ring_query(r,0)!=0; }
