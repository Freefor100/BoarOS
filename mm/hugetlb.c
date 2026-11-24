#include <kernel/hugetlb.h>
#include <kernel/avl.h>
#include <kernel/physical_page.h>
#include <kernel/heap.h>
#include <kernel/page.h>
#include <kernel/sync.h>
#include <kernel/irq.h>
#include <string.h>
#include <stddef.h>

#define HSTATE_BUCKETS 64U
enum slot_state { SLOT_AVAILABLE, SLOT_PREPARING, SLOT_USED, SLOT_REMOVED, SLOT_REVOKED };
struct huge_block {
    struct huge_block *hash_next, *free_next;
    struct kernel_hugetlb_subpool *subpool;
    uint64_t address;
    unsigned free;
};
struct kernel_hugetlb_subpool {
    struct kernel_hstate *h;
    struct kernel_hugetlb_subpool *retired_next;
    uint64_t maximum, minimum, used;
    uint32_t references;
};
struct kernel_hugetlb_slot {
    uint64_t physical;
    uint32_t state, pending;
    uint8_t reserved, adopted, restore_reservation, keep;
};
struct kernel_hugetlb_file_slot {
    struct kernel_avl_node index;
    struct kernel_hugetlb_file_slot *next;
    struct kernel_hugetlb_slot slot;
};
struct kernel_hstate {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct huge_block *buckets[HSTATE_BUCKETS], *free_head, *retired;
    struct kernel_hugetlb_subpool *retired_subpools;
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
    struct kernel_hugetlb_subpool *subpool;
    uint64_t count, lineage, resident, file_index;
    struct kernel_avl_node *file_slots;
    uint32_t references, mode, noreserve, registrations, mapping_borrows, is_file;
    struct kernel_hugetlb_slot slots[];
};

static struct kernel_hugetlb_file_slot *file_slot_at_or_after(
    const struct kernel_hugetlb_map *map, uint64_t index)
{
    struct kernel_avl_node *found=NULL;
    for (struct kernel_avl_node *node=map->file_slots;node;) {
        if (node->key<index) node=node->right;
        else { found=node;node=node->left; }
    }
    return (struct kernel_hugetlb_file_slot *)found;
}

static struct kernel_hugetlb_file_slot *file_slot_at(
    const struct kernel_hugetlb_map *map, uint64_t index)
{
    struct kernel_hugetlb_file_slot *entry=file_slot_at_or_after(map,index);
    return entry && entry->index.key==index ? entry : NULL;
}

static struct kernel_hugetlb_slot *map_slot_at(struct kernel_hugetlb_map *map, uint64_t index)
{
    if (!map->is_file) return &map->slots[index];
    struct kernel_hugetlb_file_slot *entry=file_slot_at(map,index);
    return entry ? &entry->slot : NULL;
}

static struct kernel_hugetlb_file_slot *allocate_file_slot(struct kernel_hstate *h, uint64_t index)
{
    struct kernel_hugetlb_file_slot *slot;
    if (kernel_heap_allocate_zeroed_nosleep(h->heap,1,sizeof(*slot),(void **)&slot)) return NULL;
    slot->index.key=index;
    slot->index.height=1;
    return slot;
}
static unsigned bucket(struct kernel_hstate *h,uint64_t address)
{ return (unsigned)((address>>(BOAROS_PAGE_SHIFT+h->order))%HSTATE_BUCKETS); }
static struct huge_block *find(struct kernel_hstate *h,uint64_t address)
{
    for (struct huge_block *b=h->buckets[bucket(h,address)];b;b=b->hash_next)
        if (b->address==address) return b;
    return NULL;
}

static uint64_t subpool_idle(const struct kernel_hugetlb_subpool *pool)
{ return pool && pool->used<pool->minimum ? pool->minimum-pool->used : 0; }

static void retire_subpool_locked(struct kernel_hugetlb_subpool *pool)
{
    if (!pool || pool->references || pool->used) return;
    struct kernel_hstate *h=pool->h;
    h->reserved-=pool->minimum;
    h->references--;
    pool->retired_next=h->retired_subpools;
    h->retired_subpools=pool;
}

/* Reserved slots and allocated blocks both occupy the mount's quota. Idle
 * guaranteed pages stay in h->reserved until a slot or a block needs them. */
static int charge_locked(struct kernel_hstate *h,struct kernel_hugetlb_subpool *pool,
    uint64_t pages,int reserve)
{
    uint64_t idle=subpool_idle(pool);
    uint64_t covered=pages<idle ? pages : idle;
    if ((pool && pages>pool->maximum-pool->used) || pages-covered>h->free-h->reserved)
        return 0;
    if (pool) pool->used+=pages;
    h->reserved-=covered;
    if (reserve) h->reserved+=pages;
    return 1;
}

static void uncharge_locked(struct kernel_hstate *h,struct kernel_hugetlb_subpool *pool,
    uint64_t pages,int reserved)
{
    if (reserved) h->reserved-=pages;
    if (!pool) return;
    uint64_t idle=subpool_idle(pool);
    pool->used-=pages;
    h->reserved+=subpool_idle(pool)-idle;
    retire_subpool_locked(pool);
}

static void return_charge_locked(struct kernel_hstate *h,struct huge_block *block,int restore)
{
    if (restore) h->reserved++;
    else uncharge_locked(h,block->subpool,1,0);
    block->subpool=NULL;
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
    struct kernel_hugetlb_subpool *pools;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        list=h->retired;h->retired=NULL;
        pools=h->retired_subpools;h->retired_subpools=NULL;
    }
    while (list) { struct huge_block *next=list->free_next;kernel_heap_release(h->heap,list);list=next; }
    while (pools) {
        struct kernel_hugetlb_subpool *next=pools->retired_next;
        kernel_heap_release(h->heap,pools);
        pools=next;
    }
}
static int recycle(void *context,uint64_t address,uint32_t order)
{
    struct kernel_hstate *h=context;
    if (order!=h->order) return 0;
    struct huge_block *b=find(h,address);
    if (!b) return 0;
    return_charge_locked(h,b,0);
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
static uint64_t trim_surplus(struct kernel_hstate *h,uint64_t count)
{
    uint64_t removed=0;
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

uint64_t kernel_hstate_shrink(struct kernel_hstate *h,uint64_t count)
{
    { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
      if (!h->resizing) h->target=count<h->total?h->total-count:0; }
    return trim_surplus(h,count);
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
enum kernel_hugetlb_status kernel_hstate_shutdown(struct physical_page_allocator *a)
{
    struct kernel_hstate *h=NULL;
    enum kernel_hugetlb_status status=kernel_hstate_get(a,&h);
    if (status==KERNEL_HUGETLB_NOT_FOUND) return KERNEL_HUGETLB_OK;
    if (status) return status;
    /* Shutdown owns the registry exclusively after users and callbacks stop. */
    kernel_hstate_put(h);
    return kernel_hstate_destroy(&h);
}
void kernel_hstate_snapshot(struct kernel_hstate *h,struct kernel_hstate_snapshot *out)
{
    KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
    uint64_t persistent=h->total<h->target?h->total:h->target;
    *out=(struct kernel_hstate_snapshot){h->total,h->free,h->reserved,h->maps,persistent,h->total-persistent,h->order};
}

enum kernel_hugetlb_status kernel_hugetlb_subpool_create(struct kernel_hstate *h,
    uint64_t maximum,uint64_t minimum,struct kernel_hugetlb_subpool **out)
{
    if (!h || minimum>maximum || !out || *out) return KERNEL_HUGETLB_INVALID;
    struct kernel_hugetlb_subpool *pool;
    if (kernel_heap_allocate_zeroed(h->heap,1,sizeof(*pool),(void **)&pool))
        return KERNEL_HUGETLB_NO_MEMORY;
    pool->h=h;pool->maximum=maximum;pool->minimum=minimum;pool->references=1;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (minimum<=h->free-h->reserved) {
            h->reserved+=minimum;
            if (h->references==UINT32_MAX) __builtin_trap();
            h->references++;
            *out=pool;
            return KERNEL_HUGETLB_OK;
        }
    }
    kernel_heap_release(h->heap,pool);
    return KERNEL_HUGETLB_NO_MEMORY;
}

static void subpool_get_locked(struct kernel_hugetlb_subpool *pool)
{
    if (!pool) return;
    if (!pool->references || pool->references==UINT32_MAX) __builtin_trap();
    pool->references++;
}

static void subpool_put_locked(struct kernel_hugetlb_subpool *pool)
{
    if (!pool) return;
    pool->references--;
    retire_subpool_locked(pool);
}

void kernel_hugetlb_subpool_put(struct kernel_hugetlb_subpool *pool)
{
    if (!pool) return;
    struct kernel_hstate *h=pool->h;
    { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);subpool_put_locked(pool); }
    (void)trim_surplus(h,UINT64_MAX);
    reap(h);
}

void kernel_hugetlb_subpool_capacity(struct kernel_hugetlb_subpool *pool,
    uint64_t *maximum,uint64_t *available)
{
    if (!pool) { *maximum=*available=0;return; }
    KERNEL_RAW_SCOPE(guard,&pool->h->allocator->lock);
    *maximum=pool->maximum;
    *available=pool->maximum==UINT64_MAX ? UINT64_MAX : pool->maximum-pool->used;
}

static enum kernel_hugetlb_status create_map(struct kernel_hstate *h,
    struct kernel_hugetlb_subpool *pool,uint64_t pages,enum kernel_hugetlb_mode mode,
    int noreserve,struct kernel_hugetlb_map **out)
{
    if (!h || !out || !pages || mode>KERNEL_HUGETLB_CHILD ||
        pages>(SIZE_MAX-sizeof(struct kernel_hugetlb_map))/sizeof(struct kernel_hugetlb_slot)) return KERNEL_HUGETLB_INVALID;
    struct kernel_hugetlb_map *m;
    if (kernel_heap_allocate_zeroed(h->heap,1,sizeof(*m)+pages*sizeof(struct kernel_hugetlb_slot),(void **)&m))
        return KERNEL_HUGETLB_NO_MEMORY;
    m->h=h;m->subpool=pool;m->count=pages;m->references=1;
    m->mode=mode;m->noreserve=!!noreserve || mode==KERNEL_HUGETLB_CHILD;
    if (!m->noreserve) for (uint64_t i=0;i<pages;i++) m->slots[i].reserved=1;
    int failed=0;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (!m->noreserve && !charge_locked(h,pool,pages,1)) failed=1;
        else {
            subpool_get_locked(pool);
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

enum kernel_hugetlb_status kernel_hugetlb_map_create(struct kernel_hstate *h,uint64_t pages,
    enum kernel_hugetlb_mode mode,int noreserve,struct kernel_hugetlb_map **out)
{ return create_map(h,NULL,pages,mode,noreserve,out); }
static void acquire_locked(struct kernel_hugetlb_map *m)
{ if (!m->references || m->references==UINT32_MAX) __builtin_trap();m->references++; }

enum kernel_hugetlb_status kernel_hugetlb_file_create(struct kernel_hstate *h,
    struct kernel_hugetlb_subpool *pool,struct kernel_hugetlb_map **out)
{
    if (!h || !out || *out) return KERNEL_HUGETLB_INVALID;
    struct kernel_hugetlb_map *map;
    if (kernel_heap_allocate_zeroed(h->heap,1,sizeof(*map),(void **)&map))
        return KERNEL_HUGETLB_NO_MEMORY;
    map->h=h;
    map->subpool=pool;
    map->count=UINT64_MAX;
    map->references=1;
    map->mode=KERNEL_HUGETLB_SHARED;
    map->is_file=1;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        subpool_get_locked(pool);
        h->maps++;
    }
    *out=map;
    return KERNEL_HUGETLB_OK;
}

enum kernel_hugetlb_status kernel_hugetlb_file_private_create(struct kernel_hugetlb_map *file,
    uint64_t pages,int noreserve,struct kernel_hugetlb_map **out)
{ return create_map(file->h,file->subpool,pages,KERNEL_HUGETLB_PRIVATE,noreserve,out); }

static int file_reserved(const struct kernel_hugetlb_slot *slot)
{
    return slot && (slot->reserved || slot->restore_reservation || slot->adopted);
}

enum kernel_hugetlb_status kernel_hugetlb_file_reserve_begin(struct kernel_hugetlb_map *map,
    uint64_t first, uint64_t end, struct kernel_hugetlb_reservation *reservation)
{
    if (!map || !map->is_file || first>end || !reservation || reservation->map)
        return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=map->h;
    kernel_hugetlb_map_acquire(map);
    reservation->map=map;
    uint64_t index;
    for (index=first;index<end;index++) {
        {
            KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
            if (file_reserved(map_slot_at(map,index))) continue;
            if (!charge_locked(h,map->subpool,1,1)) break;
        }
        struct kernel_hugetlb_file_slot *slot=allocate_file_slot(h,index);
        if (!slot) {
            { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);uncharge_locked(h,map->subpool,1,1); }
            (void)trim_surplus(h,1);
            break;
        }
        slot->next=reservation->candidates;
        reservation->candidates=slot;
    }
    if (index==end) return KERNEL_HUGETLB_OK;
    kernel_hugetlb_file_reserve_finish(reservation,0);
    return KERNEL_HUGETLB_NO_MEMORY;
}

void kernel_hugetlb_file_reserve_finish(struct kernel_hugetlb_reservation *reservation,int accepted)
{
    struct kernel_hugetlb_map *map=reservation->map;
    if (!map) return;
    struct kernel_hstate *h=map->h;
    struct kernel_hugetlb_file_slot *candidate=reservation->candidates;
    uint64_t returned=0;
    *reservation=(struct kernel_hugetlb_reservation){0};
    while (candidate) {
        struct kernel_hugetlb_file_slot *next=candidate->next;
        {
            KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
            struct kernel_hugetlb_slot *slot=accepted ? map_slot_at(map,candidate->index.key) : NULL;
            if (!accepted || file_reserved(slot)) {
                uncharge_locked(h,map->subpool,1,1);returned++;
            }
            else if (!slot) {
                candidate->slot.reserved=1;
                candidate->next=NULL;
                map->file_slots=kernel_avl_insert_node(map->file_slots,&candidate->index);
                candidate=NULL;
            } else if (slot->state==SLOT_AVAILABLE) slot->reserved=1;
            else {
                /* A fault already took a page without using this charge. If
                 * every claimant rejects it, restore the now reserved slot. */
                slot->restore_reservation=1;
                uncharge_locked(h,map->subpool,1,1);
                returned++;
            }
        }
        if (candidate) kernel_heap_release(h->heap,candidate);
        candidate=next;
    }
    if (returned) (void)trim_surplus(h,returned);
    kernel_hugetlb_map_release(&map);
}

enum kernel_hugetlb_status kernel_hugetlb_file_find_page(struct kernel_hugetlb_map *map,
    uint64_t index, uint64_t *physical)
{
    if (!map || !map->is_file || !physical) return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=map->h;
    uint64_t address;
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        struct kernel_hugetlb_slot *slot=map_slot_at(map,index);
        if (!slot || !slot->adopted) return KERNEL_HUGETLB_NOT_FOUND;
        address=slot->physical;
    }
    /* The caller's inode reader or registered MM gate keeps the inode's
     * reference in place through this physical reference acquisition. */
    physical_page_acquire_order(h->allocator,address,h->order);
    *physical=address;
    return KERNEL_HUGETLB_OK;
}

static void remove_file_slots(struct kernel_hugetlb_map *map,uint64_t first,uint64_t end,int punch)
{
    struct kernel_hstate *h=map->h;
    uint64_t returned=0;
    for (;;) {
        struct kernel_hugetlb_file_slot *entry;
        {
            KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
            entry=file_slot_at_or_after(map,first);
            if (!entry || entry->index.key>=end) break;
            struct kernel_hugetlb_slot *slot=&entry->slot;
            if (slot->pending || slot->state==SLOT_PREPARING || slot->restore_reservation)
                __builtin_trap();
            if (punch && !slot->adopted) {
                first=entry->index.key+1;
                continue;
            }
            map->file_slots=kernel_avl_remove_node(map->file_slots,&entry->index);
            if (slot->reserved) { uncharge_locked(h,map->subpool,1,1);returned++; }
            if (slot->adopted) map->resident--;
        }
        if (entry->slot.adopted)
            physical_page_release_order(h->allocator,entry->slot.physical,h->order);
        kernel_heap_release(h->heap,entry);
    }
    if (returned) (void)trim_surplus(h,returned);
}

void kernel_hugetlb_file_remove(struct kernel_hugetlb_map *map,uint64_t first,uint64_t end)
{ remove_file_slots(map,first,end,0); }

void kernel_hugetlb_file_punch(struct kernel_hugetlb_map *map,uint64_t first,uint64_t end)
{ remove_file_slots(map,first,end,1); }

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
        for (uint64_t i=0;!m->is_file && i<m->count;i++) {
            struct kernel_hugetlb_slot *slot=&m->slots[i];
            if (slot->pending || slot->state==SLOT_PREPARING) __builtin_trap();
            if (slot->reserved) uncharge_locked(h,m->subpool,1,1);
        }
    }
    if (m->is_file) kernel_hugetlb_file_remove(m,0,UINT64_MAX);
    else if (m->mode==KERNEL_HUGETLB_SHARED)
        for (uint64_t i=0;i<m->count;i++) if (m->slots[i].state==SLOT_USED)
            physical_page_release_order(h->allocator,m->slots[i].physical,h->order);
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (!h->maps) __builtin_trap();
        h->maps--;
        subpool_put_locked(m->subpool);
    }
    kernel_heap_release(h->heap,m);
}
enum kernel_hugetlb_status kernel_hugetlb_map_fork(struct kernel_hugetlb_map *source,
    struct kernel_hugetlb_map **out)
{
    if (!source || !out) return KERNEL_HUGETLB_INVALID;
    if (source->mode==KERNEL_HUGETLB_SHARED) { kernel_hugetlb_map_acquire(source);*out=source;return KERNEL_HUGETLB_OK; }
    struct kernel_hugetlb_map *child;
    enum kernel_hugetlb_status status=create_map(source->h,source->subpool,
        source->count,KERNEL_HUGETLB_CHILD,1,&child);
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
    child->file_index=source->file_index;
    *out=child;return KERNEL_HUGETLB_OK;
}

void kernel_hugetlb_map_set_file_index(struct kernel_hugetlb_map *map,uint64_t first)
{ map->file_index=first; }

uint64_t kernel_hugetlb_map_file_index(const struct kernel_hugetlb_map *map)
{ return map->file_index; }

void kernel_hugetlb_private_reset(struct kernel_hugetlb_map *map,uint64_t first,uint64_t end)
{
    if (map->mode==KERNEL_HUGETLB_SHARED || first>end || end>map->count) __builtin_trap();
    struct kernel_hstate *h=map->h;
    while (first<end) {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        uint64_t batch=end-first>16 ? first+16 : end;
        for (;first<batch;first++) {
            struct kernel_hugetlb_slot *slot=&map->slots[first];
            if (slot->pending || slot->state==SLOT_PREPARING) __builtin_trap();
            if (slot->state==SLOT_USED) slot->state=SLOT_AVAILABLE;
        }
    }
}

enum kernel_hugetlb_status kernel_hugetlb_map_unmap(struct kernel_hugetlb_map *m,uint64_t first,uint64_t end)
{
    if (!m || first>end || end>m->count) return KERNEL_HUGETLB_INVALID;
    if (m->mode==KERNEL_HUGETLB_SHARED) return KERNEL_HUGETLB_OK;
    KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
    for (uint64_t i=first;i<end;i++) if (m->slots[i].pending) return KERNEL_HUGETLB_BUSY;
    for (uint64_t i=first;i<end;i++) {
        if (m->slots[i].reserved) {
            uncharge_locked(m->h,m->subpool,1,1);
            m->slots[i].reserved=0;
        }
        m->slots[i].state=SLOT_REMOVED;
    }
    return KERNEL_HUGETLB_OK;
}

void kernel_hugetlb_private_keep(struct kernel_hugetlb_map *m,uint64_t first,uint64_t end)
{
    if (m->mode==KERNEL_HUGETLB_SHARED || end>m->count || first>end) __builtin_trap();
    while (first<end) {
        KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
        uint64_t batch=end-first>16 ? first+16 : end;
        for (;first<batch;first++) m->slots[first].keep=1;
    }
}

void kernel_hugetlb_private_prune(struct kernel_hugetlb_map *m)
{
    if (m->mode==KERNEL_HUGETLB_SHARED) __builtin_trap();
    uint64_t returned=0;
    for (uint64_t i=0;i<m->count;) {
        KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
        uint64_t end=m->count-i>16 ? i+16 : m->count;
        for (;i<end;i++) {
            struct kernel_hugetlb_slot *slot=&m->slots[i];
            if (slot->keep) { slot->keep=0;continue; }
            if (slot->pending) __builtin_trap();
            if (slot->reserved) {
                uncharge_locked(m->h,m->subpool,1,1);
                slot->reserved=0;returned++;
            }
            slot->state=SLOT_REMOVED;
        }
    }
    if (returned) (void)trim_surplus(m->h,returned);
}
uint64_t kernel_hugetlb_map_resident_pages(struct kernel_hugetlb_map *m)
{
    KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
    return m->resident << m->h->order;
}
int kernel_hugetlb_map_resident(struct kernel_hugetlb_map *m,uint64_t index)
{
    if (!m || index>=m->count || m->mode!=KERNEL_HUGETLB_SHARED) return 0;
    KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
    struct kernel_hugetlb_slot *slot=map_slot_at(m,index);
    return slot && slot->adopted;
}
enum kernel_hugetlb_status kernel_hugetlb_map_revoke(struct kernel_hugetlb_map *m,uint64_t index)
{
    if (!m || m->mode!=KERNEL_HUGETLB_CHILD || index>=m->count) return KERNEL_HUGETLB_INVALID;
    KERNEL_RAW_SCOPE(guard,&m->h->allocator->lock);
    if (m->slots[index].pending) return KERNEL_HUGETLB_BUSY;
    m->slots[index].state=SLOT_REVOKED;return KERNEL_HUGETLB_OK;
}

/* A missing sparse slot needs a heap candidate before taking a physical
 * page. Recheck the tree after allocation because another fault may win. */
static enum kernel_hugetlb_status prepare_fault(struct kernel_hugetlb_map *m,
    uint64_t index,int cow,struct kernel_hugetlb_claim *claim,
    struct kernel_hugetlb_file_slot **candidate,int *fresh)
{
    struct kernel_hstate *h=m->h;
    uint64_t address;
    int shared=m->mode==KERNEL_HUGETLB_SHARED,reserved=0;
    KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
    struct kernel_hugetlb_slot *slot=map_slot_at(m,index);
    struct kernel_hugetlb_file_slot *entry=NULL;
    if (!slot) {
        entry=*candidate;
        if (!entry) return KERNEL_HUGETLB_NOT_FOUND;
        slot=&entry->slot;
    }
    if (m->lineage && h->closing_lineage==m->lineage) return KERNEL_HUGETLB_BUSY;
    if (slot->state==SLOT_REMOVED || slot->state==SLOT_REVOKED) return KERNEL_HUGETLB_BUS;
    if (cow && slot->state!=SLOT_USED) return KERNEL_HUGETLB_INVALID;
    if (slot->state==SLOT_PREPARING || (!shared && slot->pending)) return KERNEL_HUGETLB_BUSY;
    if (shared && slot->state==SLOT_USED) address=slot->physical;
    else {
        reserved=!cow && slot->reserved;
        if (!cow && !shared && slot->state==SLOT_USED) return KERNEL_HUGETLB_INVALID;
        if (!reserved && !charge_locked(h,m->subpool,1,0))
            return cow && m->mode==KERNEL_HUGETLB_PRIVATE && !m->noreserve ?
                KERNEL_HUGETLB_REVOKE_CHILDREN : KERNEL_HUGETLB_BUS;
        if (entry) {
            m->file_slots=kernel_avl_insert_node(m->file_slots,&entry->index);
            *candidate=NULL;
        }
        struct huge_block *b=take_locked(h);address=b->address;*fresh=1;
        b->subpool=m->subpool;
        if (reserved) { h->reserved--;slot->reserved=0; }
        if (!cow) slot->state=SLOT_PREPARING;
        if (shared) {
            slot->physical=address;
            slot->restore_reservation=reserved;
        }
    }
    if (slot->pending==UINT32_MAX) __builtin_trap();
    slot->pending++;acquire_locked(m);
    *claim=(struct kernel_hugetlb_claim){m,slot,index,address,h->order,!!cow,!!reserved,!!shared};
    return KERNEL_HUGETLB_OK;
}

static enum kernel_hugetlb_status begin_fault(struct kernel_hugetlb_map *m,
    uint64_t index,int cow,int clear,struct kernel_hugetlb_claim *claim)
{
    if (!m || !claim || claim->map || index>=m->count ||
        ((cow || !clear) && m->mode==KERNEL_HUGETLB_SHARED))
        return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=m->h;
    struct kernel_hugetlb_file_slot *candidate=NULL;
    int fresh=0;
    enum kernel_hugetlb_status status=prepare_fault(m,index,cow,claim,&candidate,&fresh);
    if (status==KERNEL_HUGETLB_NOT_FOUND) {
        candidate=allocate_file_slot(h,index);
        status=prepare_fault(m,index,cow,claim,&candidate,&fresh);
        if (status==KERNEL_HUGETLB_NOT_FOUND) return KERNEL_HUGETLB_NO_MEMORY;
    }
    if (candidate) kernel_heap_release(h->heap,candidate);
    if (status) return status;
    uint64_t address=claim->physical_address;
    if (fresh && !cow && clear) {
        void *bytes;
        if (physical_page_map_owned(h->allocator,address,&bytes)) __builtin_trap();
        memset(bytes,0,(size_t)BOAROS_PAGE_SIZE<<h->order);
    }
    if (claim->shared) {
        /* slot's cache owner cannot disappear while pending retains it. */
        physical_page_acquire_order(h->allocator,address,h->order);
        if (fresh) { KERNEL_RAW_SCOPE(guard,&h->allocator->lock);claim->slot->state=SLOT_USED; }
    }
    return KERNEL_HUGETLB_OK;
}

enum kernel_hugetlb_status kernel_hugetlb_fault_begin(struct kernel_hugetlb_map *m,
    uint64_t index,int cow,struct kernel_hugetlb_claim *claim)
{ return begin_fault(m,index,cow,1,claim); }

enum kernel_hugetlb_status kernel_hugetlb_private_copy_begin(struct kernel_hugetlb_map *m,
    uint64_t index,struct kernel_hugetlb_claim *claim)
{ return begin_fault(m,index,0,0,claim); }

void kernel_hugetlb_fault_finish(struct kernel_hugetlb_claim *claim,int accepted)
{
    if (!claim || !claim->map) __builtin_trap();
    struct kernel_hugetlb_map *m=claim->map;struct kernel_hstate *h=m->h;
    struct kernel_hugetlb_slot *slot=claim->slot;
    struct kernel_hugetlb_file_slot *empty=NULL;
    if (!accepted && claim->shared) physical_page_release_order(h->allocator,claim->physical_address,h->order);
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        if (!slot->pending) __builtin_trap();
        slot->pending--;
        if (accepted) {
            slot->state=SLOT_USED;
            if (claim->shared && !slot->adopted) {
                slot->adopted=1;
                slot->restore_reservation=0;
                m->resident++;
            }
        } else if (!claim->shared) {
            struct huge_block *block=find(h,claim->physical_address);
            return_charge_locked(h,block,claim->reserved);
            put_locked(h,block);
            if (!claim->cow) slot->state=SLOT_AVAILABLE;
            if (claim->reserved) slot->reserved=1;
        } else if (!slot->pending && !slot->adopted) {
            struct huge_block *block=find(h,slot->physical);
            return_charge_locked(h,block,slot->restore_reservation);
            put_locked(h,block);
            slot->state=SLOT_AVAILABLE;
            if (slot->restore_reservation) {
                slot->reserved=1;
                slot->restore_reservation=0;
            } else if (m->is_file) {
                empty=(struct kernel_hugetlb_file_slot *)((char *)slot-
                    offsetof(struct kernel_hugetlb_file_slot,slot));
                m->file_slots=kernel_avl_remove_node(m->file_slots,&empty->index);
            }
        }
    }
    if (empty) {
        kernel_heap_release(h->heap,empty);
        (void)trim_surplus(h,1);
    }
    *claim=(struct kernel_hugetlb_claim){0};kernel_hugetlb_map_release(&m);
}
enum kernel_hugetlb_status kernel_hugetlb_discard_begin(struct kernel_hugetlb_map *m,
    uint64_t index,uint64_t address,int exclusive,struct kernel_hugetlb_discard_claim *claim)
{
    if (!m || !claim || claim->map || index>=m->count || m->mode==KERNEL_HUGETLB_SHARED)
        return KERNEL_HUGETLB_INVALID;
    struct kernel_hstate *h=m->h;uint32_t refs;
    /* Caller owns the still-published PTE and forbids concurrent acquisition in
     * exclusive mode, so the ref1 result cannot grow before custody transfer. */
    if (physical_page_reference_count(h->allocator,address,&refs)) return KERNEL_HUGETLB_INVALID;
    exclusive=exclusive && refs==1;
    KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
    struct kernel_hugetlb_slot *slot=&m->slots[index];struct huge_block *b=find(h,address);
    if (!b || b->free || slot->state!=SLOT_USED) return KERNEL_HUGETLB_INVALID;
    if (slot->pending || (m->lineage && h->closing_lineage==m->lineage)) return KERNEL_HUGETLB_BUSY;
    int reserve=!m->noreserve && !exclusive;
    if (reserve && !charge_locked(h,m->subpool,1,1)) return KERNEL_HUGETLB_BUSY;
    slot->pending++;acquire_locked(m);
    *claim=(struct kernel_hugetlb_discard_claim){m,index,address,(uint32_t)exclusive,(uint32_t)reserve};
    return KERNEL_HUGETLB_OK;
}
void kernel_hugetlb_discard_finish(struct kernel_hugetlb_discard_claim *claim,int accepted)
{
    if (!claim || !claim->map) __builtin_trap();
    struct kernel_hugetlb_map *m=claim->map;struct kernel_hstate *h=m->h;
    int transferred=0;
    if (accepted && claim->exclusive) {
        uint32_t refs;
        if (physical_page_reference_count(h->allocator,claim->physical_address,&refs) || refs!=1) __builtin_trap();
    }
    {
        KERNEL_RAW_SCOPE(guard,&h->allocator->lock);
        struct kernel_hugetlb_slot *slot=&m->slots[claim->index];
        if (slot->state!=SLOT_USED || slot->pending!=1 || slot->reserved) __builtin_trap();
        slot->pending--;
        if (accepted) {
            if (claim->exclusive) {
                struct huge_block *block=find(h,claim->physical_address);
                return_charge_locked(h,block,!m->noreserve);
                put_locked(h,block);transferred=1;
            }
            slot->state=SLOT_AVAILABLE;
            if (!m->noreserve) slot->reserved=1;
        } else if (claim->reserved) uncharge_locked(h,m->subpool,1,1);
    }
    if (accepted && !transferred) physical_page_release_order(h->allocator,claim->physical_address,h->order);
    *claim=(struct kernel_hugetlb_discard_claim){0};kernel_hugetlb_map_release(&m);
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
        struct kernel_hugetlb_slot *slot=&m->slots[index];struct huge_block *b=find(h,address);
        if (!b || b->free || slot->state!=SLOT_USED) return KERNEL_HUGETLB_INVALID;
        if (slot->pending) return KERNEL_HUGETLB_BUSY;
        if (!m->noreserve && refs!=1 && !charge_locked(h,m->subpool,1,1))
            return KERNEL_HUGETLB_BUSY;
        if (refs==1) {
            return_charge_locked(h,b,!m->noreserve);
            put_locked(h,b);transferred=1;
        }
        slot->state=SLOT_AVAILABLE;
        if (!m->noreserve) slot->reserved=1;
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
