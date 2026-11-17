#include <kernel/hugetlb.h>
#include <kernel/physical_page.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/sync.h>
#include <kernel/irq.h>
#include <string.h>
#include <stddef.h>

#define HSTATE_BUCKETS 64U
enum slot_state { SLOT_AVAILABLE, SLOT_PREPARING, SLOT_USED, SLOT_REMOVED, SLOT_REVOKED };
struct huge_block { struct huge_block *hash_next, *free_next; uint64_t address; unsigned free; };
struct huge_slot { uint64_t physical; uint32_t state, pending, reserved, adopted; };
struct kernel_hstate {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct huge_block *buckets[HSTATE_BUCKETS], *free_head, *retired;
    struct kernel_mutex mutation;
    struct kernel_wait_queue changed;
    struct kernel_hugetlb_mapping *mappings;
    uint64_t next_lineage, epoch, closing_lineage, target;
    uint32_t mapping_borrows;
    uint64_t total, free, reserved, maps;
    uint32_t order, references, closing, resizing;
};
struct kernel_hugetlb_map {
    struct kernel_hstate *h;
    uint64_t count, lineage;
    uint32_t references, mode, noreserve, registrations, mapping_borrows;
    struct huge_slot slots[];
};
static unsigned bucket(struct kernel_hstate *h,uint64_t address)
{ return (unsigned)((address>>(BOAROS_PAGE_SHIFT+h->order))%HSTATE_BUCKETS); }
static struct huge_block *find(struct kernel_hstate *h,uint64_t address)
{
    for (struct huge_block *b=h->buckets[bucket(h,address)];b;b=b->hash_next)
        if (b->address==address) return b;
    return NULL;
}
static void put_locked(struct kernel_hstate *h,struct huge_block *b)
{
    if (!b || b->free || h->free>=h->total) __builtin_trap();
    b->free=1;b->free_next=h->free_head;h->free_head=b;h->free++;
}
static struct huge_block *take_locked(struct kernel_hstate *h)
{
    struct huge_block *b=h->free_head;
    if (!b || !b->free || !h->free) __builtin_trap();
    h->free_head=b->free_next;b->free_next=NULL;b->free=0;h->free--;
    return b;
}
static void forget_locked(struct kernel_hstate *h,struct huge_block *b)
{
    struct huge_block **link=&h->buckets[bucket(h,b->address)];
    while (*link!=b) { if (!*link) __builtin_trap();link=&(*link)->hash_next; }
    *link=b->hash_next;h->total--;
}
static void reap(struct kernel_hstate *h)
{
    struct huge_block *list;
    { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);list=h->retired;h->retired=NULL; }
    while (list) { struct huge_block *next=list->free_next;kernel_heap_release(h->heap,list);list=next; }
}
static int recycle(void *context,uint64_t address,uint32_t order)
{
    struct kernel_hstate *h=context;
    if (order!=h->order) return 0;
    struct huge_block *b=find(h,address);
    if (!b) return 0;
    if (h->total>h->target && h->free>=h->reserved) {
        forget_locked(h,b);b->free_next=h->retired;h->retired=b;
        return 0;
    }
    put_locked(h,b);
    return 1;
}
enum kernel_hugetlb_status kernel_hstate_create(struct kernel_heap *heap,
    struct physical_page_allocator *a,uint32_t order,struct kernel_hstate **out)
{
    if (!heap || !a || !out || order!=(BOAROS_PAGE_SHIFT==12?9U:11U)) return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*h),(void **)&h)) return KERNEL_HUGETLB_NO_MEMORY;
    h->heap=heap;h->allocator=a;h->order=order;h->references=1;
    h->epoch=1;
    kernel_mutex_init(&h->mutation,20,(uintptr_t)h);
    kernel_wait_queue_init(&h->changed);
    struct physical_page_compound_recycler hooks={.context=h,.retain_last=recycle};
    if (physical_page_allocator_set_compound_recycler(a,&hooks)) {
        kernel_heap_release(heap,h);return KERNEL_HUGETLB_INVALID;
    }
    *out=h;return KERNEL_HUGETLB_OK;
}
enum kernel_hugetlb_status kernel_hstate_get(struct physical_page_allocator *a,struct kernel_hstate **out)
{
    if (!a || !out || !physical_page_allocator_is_finalized(a)) return KERNEL_HUGETLB_INVALID;
    KERNEL_RAW_SCOPE(guard,&a->lock);
    if (!a->compound_recycler.retain_last) return KERNEL_HUGETLB_NOT_FOUND;
    if (a->compound_recycler.retain_last!=recycle) return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=a->compound_recycler.context;
    if (h->closing) return KERNEL_HUGETLB_BUSY;
    if (!h->references || h->references==UINT32_MAX) __builtin_trap();
    h->references++;*out=h;return KERNEL_HUGETLB_OK;
}
void kernel_hstate_put(struct kernel_hstate *h)
{
    KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
    if (h->references<=1) __builtin_trap();
    h->references--;
}
enum kernel_hugetlb_status kernel_hstate_get_or_create(struct kernel_heap *heap,
    struct physical_page_allocator *a,uint32_t order,struct kernel_hstate **out)
{
    enum kernel_hugetlb_status status=kernel_hstate_get(a,out);
    if (status!=KERNEL_HUGETLB_NOT_FOUND) {
        if (!status && (*out)->order!=order) { kernel_hstate_put(*out);*out=NULL;return KERNEL_HUGETLB_INVALID; }
        return status;
    }
    struct kernel_hstate *created;
    status=kernel_hstate_create(heap,a,order,&created);
    if (status && status!=KERNEL_HUGETLB_INVALID) return status;
    return kernel_hstate_get(a,out); /* also resolves a competing creator */
}
enum kernel_hugetlb_status kernel_hstate_resize(struct kernel_hstate *h,uint64_t target)
{
    uint64_t current;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (h->resizing || h->closing) return KERNEL_HUGETLB_BUSY;
        h->resizing=1;current=h->total;h->target=target;
    }
    enum kernel_hugetlb_status status=KERNEL_HUGETLB_OK;
    if (target>current) status=kernel_hstate_grow(h,target-current);
    else if (target<current) kernel_hstate_shrink(h,current-target);
    { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);h->resizing=0; }
    reap(h);
    return status;
}
enum kernel_hugetlb_status kernel_hstate_grow(struct kernel_hstate *h,uint64_t count)
{
    if (!h) return KERNEL_HUGETLB_INVALID;
    /* Partial progress is deliberately visible, as with a fragmented hugepage
     * pool resize. snapshot reports actual capacity; reservations see only it. */
    while (count--) {
        struct huge_block *b;
        if (kernel_heap_allocate_zeroed(h->heap,1,sizeof(*b),(void **)&b)) return KERNEL_HUGETLB_NO_MEMORY;
        if (physical_page_allocate_order(h->allocator,h->order,&b->address)) {
            kernel_heap_release(h->heap,b);return KERNEL_HUGETLB_NO_MEMORY;
        }
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (find(h,b->address) || h->total==UINT64_MAX) __builtin_trap();
        unsigned index=bucket(h,b->address);
        b->hash_next=h->buckets[index];h->buckets[index]=b;h->total++;
        if (!h->resizing && h->target<h->total) h->target=h->total;
        put_locked(h,b);
    }
    return KERNEL_HUGETLB_OK;
}
uint64_t kernel_hstate_shrink(struct kernel_hstate *h,uint64_t count)
{
    uint64_t removed=0;
    { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
      if (!h->resizing) h->target=count<h->total?h->total-count:0; }
    while (removed<count) {
        struct huge_block *b;
        {
            KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
            if (h->free<=h->reserved || h->total<=h->target) break;
            b=take_locked(h);
            forget_locked(h,b);
        }
        physical_page_release_order(h->allocator,b->address,h->order);
        kernel_heap_release(h->heap,b);removed++;
    }
    return removed;
}
enum kernel_hugetlb_status kernel_hstate_destroy(struct kernel_hstate **owner)
{
    if (!owner || !*owner) return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=*owner;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (h->maps || h->reserved || h->free!=h->total || h->references!=1 || h->resizing) return KERNEL_HUGETLB_BUSY;
        h->closing=1;
    }
    kernel_hstate_shrink(h,UINT64_MAX);
    reap(h);
    if (physical_page_allocator_clear_compound_recycler(h->allocator,h)) __builtin_trap();
    *owner=NULL;kernel_heap_release(h->heap,h);return KERNEL_HUGETLB_OK;
}
void kernel_hstate_snapshot(struct kernel_hstate *h,struct kernel_hstate_snapshot *out)
{
    KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
    uint64_t persistent=h->total<h->target?h->total:h->target;
    *out=(struct kernel_hstate_snapshot){h->total,h->free,h->reserved,h->maps,persistent,h->total-persistent,h->order};
}
enum kernel_hugetlb_status kernel_hugetlb_map_create(struct kernel_hstate *h,uint64_t pages,
    enum kernel_hugetlb_mode mode,int noreserve,struct kernel_hugetlb_map **out)
{
    if (!h || !out || !pages || mode>KERNEL_HUGETLB_CHILD ||
        pages>(SIZE_MAX-sizeof(struct kernel_hugetlb_map))/sizeof(struct huge_slot)) return KERNEL_HUGETLB_INVALID;
    struct kernel_hugetlb_map *m;
    if (kernel_heap_allocate_zeroed(h->heap,1,sizeof(*m)+pages*sizeof(struct huge_slot),(void **)&m))
        return KERNEL_HUGETLB_NO_MEMORY;
    m->h=h;m->count=pages;m->references=1;m->mode=mode;m->noreserve=!!noreserve || mode==KERNEL_HUGETLB_CHILD;
    if (!m->noreserve) for (uint64_t i=0;i<pages;i++) m->slots[i].reserved=1;
    int failed=0;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (!m->noreserve && pages>h->free-h->reserved) failed=1;
        else {
            if (!m->noreserve) {
                h->reserved+=pages;
            }
            if (mode==KERNEL_HUGETLB_PRIVATE) {
                if (h->next_lineage==UINT64_MAX) __builtin_trap();
                m->lineage=++h->next_lineage;
            }
            h->maps++;
        }
    }
    if (failed) { kernel_heap_release(h->heap,m);return KERNEL_HUGETLB_NO_MEMORY; }
    *out=m;return KERNEL_HUGETLB_OK;
}
static void acquire_locked(struct kernel_hugetlb_map *m)
{ if (!m->references || m->references==UINT32_MAX) __builtin_trap();m->references++; }
void kernel_hugetlb_map_acquire(struct kernel_hugetlb_map *m)
{ KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);acquire_locked(m); }
void kernel_hugetlb_map_release(struct kernel_hugetlb_map **owner)
{
    if (!owner || !*owner) __builtin_trap();
    struct kernel_hugetlb_map *m=*owner;struct kernel_hstate *h=m->h;*owner=NULL;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (!m->references) __builtin_trap();
        if (--m->references) return;
        if (m->registrations || m->mapping_borrows) __builtin_trap();
        for (uint64_t i=0;i<m->count;i++) {
            struct huge_slot *slot=&m->slots[i];
            if (slot->pending || slot->state==SLOT_PREPARING) __builtin_trap();
            if (slot->reserved) { if (!h->reserved) __builtin_trap();h->reserved--;slot->reserved=0; }
        }
    }
    if (m->mode==KERNEL_HUGETLB_SHARED)
        for (uint64_t i=0;i<m->count;i++) if (m->slots[i].state==SLOT_USED)
            physical_page_release_order(h->allocator,m->slots[i].physical,h->order);
    { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);if (!h->maps) __builtin_trap();h->maps--; }
    kernel_heap_release(h->heap,m);
}
enum kernel_hugetlb_status kernel_hugetlb_map_fork(struct kernel_hugetlb_map *source,
    struct kernel_hugetlb_map **out)
{
    if (!source || !out) return KERNEL_HUGETLB_INVALID;
    if (source->mode==KERNEL_HUGETLB_SHARED) { kernel_hugetlb_map_acquire(source);*out=source;return KERNEL_HUGETLB_OK; }
    struct kernel_hugetlb_map *child;
    enum kernel_hugetlb_status status=kernel_hugetlb_map_create(source->h,source->count,KERNEL_HUGETLB_CHILD,1,&child);
    if (status) return status;
    int busy=0;
    {
        KERNEL_RAW_SCOPE(guard,&source->h->allocator->lock);
        for (uint64_t i=0;i<source->count;i++) {
            if (source->slots[i].pending) { busy=1;break; }
            child->slots[i].state=source->slots[i].state;
        }
    }
    if (busy) { kernel_hugetlb_map_release(&child);return KERNEL_HUGETLB_BUSY; }
    child->lineage=source->lineage;
    *out=child;return KERNEL_HUGETLB_OK;
}
enum kernel_hugetlb_status kernel_hugetlb_map_unmap(struct kernel_hugetlb_map *m,uint64_t first,uint64_t end)
{
    if (!m || first>end || end>m->count) return KERNEL_HUGETLB_INVALID;
    if (m->mode==KERNEL_HUGETLB_SHARED) return KERNEL_HUGETLB_OK;
    KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
    for (uint64_t i=first;i<end;i++) if (m->slots[i].pending) return KERNEL_HUGETLB_BUSY;
    for (uint64_t i=first;i<end;i++) {
        if (m->slots[i].reserved) { m->h->reserved--;m->slots[i].reserved=0; }
        m->slots[i].state=SLOT_REMOVED;
    }
    return KERNEL_HUGETLB_OK;
}
enum kernel_hugetlb_status kernel_hugetlb_map_revoke(struct kernel_hugetlb_map *m,uint64_t index)
{
    if (!m || m->mode!=KERNEL_HUGETLB_CHILD || index>=m->count) return KERNEL_HUGETLB_INVALID;
    KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
    if (m->slots[index].pending) return KERNEL_HUGETLB_BUSY;
    m->slots[index].state=SLOT_REVOKED;return KERNEL_HUGETLB_OK;
}
enum kernel_hugetlb_status kernel_hugetlb_fault_begin(struct kernel_hugetlb_map *m,
    uint64_t index,int cow,struct kernel_hugetlb_claim *claim)
{
    if (!m || !claim || claim->map || index>=m->count || (cow && m->mode==KERNEL_HUGETLB_SHARED))
        return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=m->h;struct huge_slot *slot=&m->slots[index];
    uint64_t address;int shared=m->mode==KERNEL_HUGETLB_SHARED, fresh=0, reserved=0;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (m->lineage && h->closing_lineage==m->lineage) return KERNEL_HUGETLB_BUSY;
        if (slot->state==SLOT_REMOVED || slot->state==SLOT_REVOKED) return KERNEL_HUGETLB_BUS;
        if (cow && slot->state!=SLOT_USED) return KERNEL_HUGETLB_INVALID;
        if (slot->state==SLOT_PREPARING || (!shared && slot->pending)) return KERNEL_HUGETLB_BUSY;
        if (shared && slot->state==SLOT_USED) address=slot->physical;
        else {
            reserved=!cow && slot->reserved;
            if (!h->free || (!reserved && h->free<=h->reserved))
                return cow && m->mode==KERNEL_HUGETLB_PRIVATE && !m->noreserve ?
                    KERNEL_HUGETLB_REVOKE_CHILDREN : KERNEL_HUGETLB_BUS;
            if (!cow && !shared && slot->state==SLOT_USED) return KERNEL_HUGETLB_INVALID;
            struct huge_block *b=take_locked(h);address=b->address;fresh=1;
            if (reserved) { h->reserved--;slot->reserved=0; }
            if (!cow) slot->state=SLOT_PREPARING;
            if (shared) slot->physical=address;
        }
        if (slot->pending==UINT32_MAX) __builtin_trap();
        slot->pending++;acquire_locked(m);
    }
    if (fresh && !cow) {
        void *bytes;
        if (physical_page_map_owned(h->allocator,address,&bytes)) __builtin_trap();
        memset(bytes,0,(size_t)BOAROS_PAGE_SIZE<<h->order);
    }
    if (shared) {
        /* slot's cache owner cannot disappear while pending retains it. */
        physical_page_acquire_order(h->allocator,address,h->order);
        if (fresh) { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);slot->state=SLOT_USED; }
    }
    *claim=(struct kernel_hugetlb_claim){m,index,address,h->order,!!cow,!!reserved,!!shared};
    return KERNEL_HUGETLB_OK;
}
void kernel_hugetlb_fault_finish(struct kernel_hugetlb_claim *claim,int accepted)
{
    if (!claim || !claim->map) __builtin_trap();
    struct kernel_hugetlb_map *m=claim->map;struct kernel_hstate *h=m->h;
    struct huge_slot *slot=&m->slots[claim->index];
    if (!accepted && claim->shared) physical_page_release_order(h->allocator,claim->physical_address,h->order);
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (!slot->pending) __builtin_trap();
        slot->pending--;
        if (accepted) {
            slot->state=SLOT_USED;
            if (claim->shared) slot->adopted=1;
        } else if (!claim->shared) {
            put_locked(h,find(h,claim->physical_address));
            if (!claim->cow) slot->state=SLOT_AVAILABLE;
            if (claim->reserved) { slot->reserved=1;h->reserved++; }
        } else if (!slot->pending && !slot->adopted) {
            put_locked(h,find(h,slot->physical));
            slot->state=SLOT_AVAILABLE;
            if (!m->noreserve) { slot->reserved=1;h->reserved++; }
        }
    }
    *claim=(struct kernel_hugetlb_claim){0};kernel_hugetlb_map_release(&m);
}
enum kernel_hugetlb_status kernel_hugetlb_discard(struct kernel_hugetlb_map *m,
    uint64_t index,uint64_t address)
{
    if (!m || index>=m->count || m->mode==KERNEL_HUGETLB_SHARED) return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=m->h;uint32_t refs;
    physical_page_reference_count(h->allocator,address,&refs);
    int transferred=0;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        struct huge_slot *slot=&m->slots[index];struct huge_block *b=find(h,address);
        if (!b || b->free || slot->state!=SLOT_USED) return KERNEL_HUGETLB_INVALID;
        if (slot->pending) return KERNEL_HUGETLB_BUSY;
        if (!m->noreserve && refs!=1 && h->free<=h->reserved) return KERNEL_HUGETLB_BUSY;
        if (refs==1) { put_locked(h,b);transferred=1; }
        slot->state=SLOT_AVAILABLE;
        if (!m->noreserve) { slot->reserved=1;h->reserved++; }
    }
    if (!transferred) physical_page_release_order(h->allocator,address,h->order);
    return KERNEL_HUGETLB_OK;
}

int kernel_hugetlb_map_is_child_of(const struct kernel_hugetlb_map *child,
    const struct kernel_hugetlb_map *owner)
{
    return child && owner && owner->mode==KERNEL_HUGETLB_PRIVATE &&
        child->mode==KERNEL_HUGETLB_CHILD && owner->lineage &&
        child->h==owner->h && child->lineage==owner->lineage;
}
static void changed(struct kernel_hstate *h)
{
    KERNEL_IRQ_SCOPE(irq);
    enum kernel_scheduler_status status=kernel_wait_queue_wake_all(&h->changed);
    if (status!=KERNEL_SCHEDULER_STATUS_OK &&
        !(status==KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED && !h->changed.registrations && !h->changed.borrows))
        __builtin_trap();
}
static void mapping_borrow(struct kernel_hstate *h,struct kernel_hugetlb_mapping *m)
{
    if (!m) return;
    if (m->map->h!=h || m->borrows==UINT32_MAX || h->mapping_borrows==UINT32_MAX || m->map->mapping_borrows==UINT32_MAX) __builtin_trap();
    m->borrows++;h->mapping_borrows++;m->map->mapping_borrows++;
}
static void mapping_put(struct kernel_hstate *h,struct kernel_hugetlb_mapping *m)
{
    do {
        { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
          for (unsigned n=0;m && n<16;n++) {
              if (m->map->h!=h || !m->borrows || !h->mapping_borrows || !m->map->mapping_borrows) __builtin_trap();
              h->mapping_borrows--;m->map->mapping_borrows--;
              if (--m->borrows || m->previous) { m=NULL;break; }
              struct kernel_hugetlb_mapping *next=m->retired_next;
              m->next=m->retired_next=NULL;m=next;
          } }
        changed(h);
    } while(m);
}
enum kernel_hugetlb_status kernel_hugetlb_map_epoch(struct kernel_hugetlb_map *m,uint64_t *epoch)
{
    KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
    *epoch=m->h->epoch;
    return m->lineage && m->h->closing_lineage==m->lineage ? KERNEL_HUGETLB_BUSY:KERNEL_HUGETLB_OK;
}
enum kernel_hugetlb_status kernel_hugetlb_mapping_register(struct kernel_hugetlb_mapping *m,uint64_t epoch)
{
    if (!m || !m->map || !m->owner || !m->revoke || m->previous || m->borrows || m->retired_next) __builtin_trap();
    struct kernel_hstate *h=m->map->h;
    KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
    if ((epoch && epoch!=h->epoch) || (m->map->lineage && h->closing_lineage==m->map->lineage))
        return KERNEL_HUGETLB_BUSY;
    if (m->map->registrations==UINT32_MAX) __builtin_trap();
    m->map->registrations++;
    m->next=h->mappings;m->previous=&h->mappings;
    if (m->next) m->next->previous=&m->next;
    h->mappings=m;return KERNEL_HUGETLB_OK;
}
void kernel_hugetlb_mapping_stop(struct kernel_hugetlb_mapping *m)
{
    struct kernel_hstate *h=m->map->h;
    KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
    if (!m->previous) return;
    if (*m->previous!=m || m->retired_next) __builtin_trap();
    *m->previous=m->next;
    if (m->next) m->next->previous=m->previous;
    m->previous=NULL;
    if (!m->map->registrations) __builtin_trap();
    m->map->registrations--;
    if (m->borrows) { m->retired_next=m->next;mapping_borrow(h,m->retired_next); }
    else m->next=NULL;
}
void kernel_hugetlb_mapping_drain(struct kernel_hugetlb_mapping *m)
{
    struct kernel_hstate *h=m->map->h;
    KERNEL_IRQ_SCOPE(irq);
    for (;;) {
        { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
          if (m->previous) __builtin_trap();
          if (!m->borrows) return; }
        struct kernel_wait_token token;enum kernel_wait_wake_reason reason;int busy;
        if (kernel_wait_prepare(&h->changed,0,0,&token)) __builtin_trap();
        { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);busy=m->borrows!=0; }
        if (busy && kernel_wait_park(&token,&reason)) __builtin_trap();
        if (kernel_wait_finish(&token)) __builtin_trap();
    }
}
enum kernel_hugetlb_status kernel_hugetlb_revoke_children(struct kernel_hugetlb_map *owner,uint64_t index,uint64_t old_head)
{
    if (!owner || owner->mode!=KERNEL_HUGETLB_PRIVATE || index>=owner->count) __builtin_trap();
    struct kernel_hstate *h=owner->h;
    KERNEL_LOCK_SCOPE(mutation);kernel_mutex_lock(&h->mutation,&mutation);
    struct kernel_hugetlb_mapping *m;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (h->closing_lineage || h->epoch==UINT64_MAX) __builtin_trap();
        h->closing_lineage=owner->lineage;h->epoch++;
        m=h->mappings;mapping_borrow(h,m);
    }
    enum kernel_hugetlb_status status=KERNEL_HUGETLB_OK;
    while (m) {
        int active;
        { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
          active=m->previous && kernel_hugetlb_map_is_child_of(m->map,owner); }
        if (active) status=m->revoke(m->owner,m->map,index,old_head);
        if (status) { mapping_put(h,m);break; }
        struct kernel_hugetlb_mapping *next;
        { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
          next=m->previous?m->next:m->retired_next;mapping_borrow(h,next); }
        mapping_put(h,m);m=next;
    }
    { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);h->closing_lineage=0; }
    changed(h);
    return status;
}
