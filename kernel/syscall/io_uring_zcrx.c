#include <kernel/io_uring_zcrx.h>
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/network.h>
#include <kernel/page.h>
#include <kernel/raw_lock.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { ZCRX_RQE_START = 64, ZCRX_RQ_MAX = 32768, ZCRX_REGION_USER = 1 };
struct zcrx_rqe { uint64_t offset; uint32_t length, reserved; };
struct zcrx_offsets { uint32_t head, tail, rqes, reserved; uint64_t reserved2[2]; };
struct zcrx_registration {
    uint32_t ifindex, rx_queue, entries, flags;
    uint64_t area_ptr, region_ptr;
    struct zcrx_offsets offsets;
    uint32_t id, buffer_size;
    uint64_t event_ptr, reserved[2];
};
struct zcrx_area_registration {
    uint64_t address, length, token;
    uint32_t flags, dmabuf_fd;
    uint64_t reserved[2];
};
struct zcrx_region {
    uint64_t address, length;
    uint32_t flags, id;
    uint64_t mmap_offset, reserved[4];
};
_Static_assert(sizeof(struct zcrx_registration) == 96, "zcrx registration ABI");
_Static_assert(sizeof(struct zcrx_area_registration) == 48, "zcrx area ABI");
_Static_assert(sizeof(struct zcrx_region) == 64, "zcrx region ABI");
_Static_assert(sizeof(struct zcrx_rqe) == 16, "zcrx refill entry ABI");

struct zcrx_slot {
    struct kernel_mm_long_pin pin;
    struct zcrx_slot *next;
    unsigned user_offset, user_length;
    bool device;
};
struct kernel_io_uring_zcrx {
    struct kernel_heap *heap;
    struct physical_page_allocator *allocator;
    struct kernel_uid_account *account;
    struct kernel_raw_lock lock;
    struct zcrx_slot *slots, *free;
    struct kernel_mm_long_pin *rq_pages;
    uint64_t *frame_check, charged;
    uint32_t ifindex, entries, head, acquired_slots, acquired_rq, device_loans;
    unsigned attached;
    bool rq_user, closing;
};

static int copy_in(struct kernel_mm *mm,void *buffer,uint64_t user,size_t size)
{
    size_t copied;
    return kernel_copy_from_user(mm,buffer,user,size,&copied) || copied!=size ? -KERNEL_EFAULT : 0;
}
static int copy_out(struct kernel_mm *mm,uint64_t user,const void *buffer,size_t size)
{
    size_t copied;
    return kernel_copy_to_user(mm,user,buffer,size,&copied) || copied!=size ? -KERNEL_EFAULT : 0;
}

static void recycle_slot(struct kernel_io_uring_zcrx *area,struct zcrx_slot *slot)
{
    slot->next = area->free;
    area->free = slot;
}

static int provider_take(void *context,struct kernel_network_igb_zc_buffer *buffer)
{
    struct kernel_io_uring_zcrx *area = context;
    KERNEL_RAW_SCOPE(guard,&area->lock);
    struct zcrx_slot *slot = area->free;
    if (area->closing || !slot) return -KERNEL_EAGAIN;
    area->free = slot->next;
    slot->device = true;
    area->device_loans++;
    *buffer = (struct kernel_network_igb_zc_buffer){slot->pin.physical,slot->pin.page,slot};
    return 0;
}
static void provider_put(void *context,void *cookie)
{
    struct kernel_io_uring_zcrx *area = context;
    struct zcrx_slot *slot = cookie;
    KERNEL_RAW_SCOPE(guard,&area->lock);
    slot->device = false;
    area->device_loans--;
    if (!slot->user_length && !area->closing) recycle_slot(area,slot);
}
static void provider_set_attached(void *context,int attached)
{
    struct kernel_io_uring_zcrx *area = context;
    __atomic_store_n(&area->attached,(unsigned)attached,__ATOMIC_RELEASE);
}
static const struct kernel_network_igb_zc_ops provider_ops = {
    .take = provider_take, .put = provider_put,
    .set_attached = provider_set_attached,
};

static int is_attached(const struct kernel_io_uring_zcrx *area)
{ return __atomic_load_n(&area->attached,__ATOMIC_ACQUIRE) != 0; }

int kernel_io_uring_zcrx_capture(struct kernel_io_uring_zcrx *area,const struct pbuf *p,
    unsigned skip,unsigned count,uint64_t *offset)
{
    void *cookie;
    unsigned start;
    if (!area || !count || !kernel_network_igb_zc_payload(p,area,&cookie,&start) ||
        start >= BOAROS_PAGE_SIZE || skip > BOAROS_PAGE_SIZE - start ||
        count > BOAROS_PAGE_SIZE - start - skip) return -KERNEL_EXDEV;
    struct zcrx_slot *slot = cookie;
    KERNEL_RAW_SCOPE(guard,&area->lock);
    if (area->closing || slot->user_length) return -KERNEL_EAGAIN;
    slot->user_offset = start + skip;
    slot->user_length = count;
    *offset = (uint64_t)(slot - area->slots) * BOAROS_PAGE_SIZE + slot->user_offset;
    return 0;
}

static void *rq_position(const struct kernel_io_uring_zcrx *area,uint64_t offset)
{
    return (unsigned char *)area->rq_pages[offset >> BOAROS_PAGE_SHIFT].page +
        (offset & BOAROS_PAGE_MASK);
}

unsigned kernel_io_uring_zcrx_refill(struct kernel_io_uring_zcrx *area)
{
    if (!area || !is_attached(area)) return 0;
    uint32_t *tail = rq_position(area,4);
    uint32_t current = __atomic_load_n(tail,__ATOMIC_ACQUIRE);
    unsigned recycled = 0, kick = 0;
    for (;;) {
        KERNEL_RAW_SCOPE(guard,&area->lock);
        uint32_t pending = current - area->head;
        if (!pending || pending > area->entries || area->closing) break;
        if (pending > 16) pending = 16;
        while (pending--) {
            uint32_t index = area->head++ & (area->entries - 1);
            const struct zcrx_rqe *rqe = rq_position(area,ZCRX_RQE_START + index * sizeof(*rqe));
            uint64_t offset = __atomic_load_n(&rqe->offset,__ATOMIC_RELAXED);
            uint64_t page = offset >> BOAROS_PAGE_SHIFT;
            if (__atomic_load_n(&rqe->reserved,__ATOMIC_RELAXED) || (offset >> 48) ||
                page >= area->acquired_slots) continue;
            struct zcrx_slot *slot = &area->slots[page];
            if (!slot->user_length || (offset & BOAROS_PAGE_MASK) != slot->user_offset ||
                __atomic_load_n(&rqe->length,__ATOMIC_RELAXED) != slot->user_length) continue;
            slot->user_length = 0;
            recycled++;
            if (!slot->device) {
                recycle_slot(area,slot);
                kick = 1;
            }
        }
        uint32_t *head = rq_position(area,0);
        __atomic_store_n(head,area->head,__ATOMIC_RELEASE);
    }
    if (kick) kernel_network_igb_zc_kick(area->ifindex,area);
    return recycled;
}

int kernel_io_uring_zcrx_release(struct kernel_io_uring_zcrx **owner)
{
    struct kernel_io_uring_zcrx *area = *owner;
    if (!area) return 0;
    {
        KERNEL_RAW_SCOPE(guard,&area->lock);
        area->closing = true;
    }
    if (is_attached(area)) {
        int error = kernel_network_igb_zc_detach(area->ifindex,area);
        if (error) return error;
    }
    {
        KERNEL_RAW_SCOPE(guard,&area->lock);
        if (area->device_loans) return -KERNEL_EAGAIN;
    }
    while (area->acquired_slots) {
        kernel_preempt_disable();
        kernel_mm_long_pin_release(&area->slots[--area->acquired_slots].pin);
        kernel_preempt_enable();
    }
    while (area->acquired_rq) {
        kernel_preempt_disable();
        struct kernel_mm_long_pin *page = &area->rq_pages[--area->acquired_rq];
        if (area->rq_user) kernel_mm_long_pin_release(page);
        else if (page->physical && physical_page_release(area->allocator,page->physical)) __builtin_trap();
        kernel_preempt_enable();
    }
    kernel_preempt_disable();
    if (area->frame_check) kernel_heap_release(area->heap,area->frame_check);
    if (area->rq_pages) kernel_heap_release(area->heap,area->rq_pages);
    if (area->slots) kernel_heap_release(area->heap,area->slots);
    if (area->charged) kernel_uid_account_pin_uncharge(area->account,area->charged);
    *owner = NULL;
    kernel_heap_release(area->heap,area);
    kernel_preempt_enable();
    return 0;
}

static int compare_frames(const void *a,const void *b)
{
    uint64_t first = *(const uint64_t *)a, second = *(const uint64_t *)b;
    return (first > second) - (first < second);
}

static int prepare_memory(struct kernel_io_uring_zcrx *area,struct kernel_mm *mm,
    const struct zcrx_area_registration *registration,const struct zcrx_region *region)
{
    size_t slots = registration->length >> BOAROS_PAGE_SHIFT;
    size_t rq_count = region->length >> BOAROS_PAGE_SHIFT;
    if (slots > UINT32_MAX || rq_count > INT32_MAX ||
        slots > SIZE_MAX / sizeof(*area->slots) || rq_count > SIZE_MAX / sizeof(*area->rq_pages))
        return -KERNEL_E2BIG;
    size_t pages = slots + rq_count;
    if (area->account) {
        uint64_t limit = kernel_task_current_memlock_limit() >> BOAROS_PAGE_SHIFT;
        kernel_preempt_disable();
        int error = kernel_uid_account_pin_charge(area->account,pages,limit);
        if (!error) area->charged = pages;
        kernel_preempt_enable();
        if (error) return error;
    }
    if (kernel_heap_allocate_zeroed(area->heap,slots,sizeof(*area->slots),(void **)&area->slots) ||
        kernel_heap_allocate_zeroed(area->heap,rq_count,sizeof(*area->rq_pages),(void **)&area->rq_pages) ||
        kernel_heap_allocate(area->heap,pages * sizeof(uint64_t),(void **)&area->frame_check))
        return -KERNEL_ENOMEM;
    for (size_t i = 0; i < slots; i++) {
        struct zcrx_slot *slot = &area->slots[i];
        area->acquired_slots++;
        int error = kernel_mm_long_pin_acquire(mm,registration->address + i * BOAROS_PAGE_SIZE,&slot->pin);
        if (error) return error;
        area->frame_check[i] = slot->pin.physical;
        recycle_slot(area,slot);
    }
    for (size_t i = 0; i < rq_count; i++) {
        struct kernel_mm_long_pin *page = &area->rq_pages[i];
        area->acquired_rq++;
        if (area->rq_user) {
            int error = kernel_mm_long_pin_acquire(mm,region->address + i * BOAROS_PAGE_SIZE,page);
            if (error) return error;
        } else {
            if (physical_page_allocate(area->allocator,&page->physical)) return -KERNEL_ENOMEM;
            if (physical_page_map_owned(area->allocator,page->physical,&page->page)) __builtin_trap();
            memset(page->page,0,BOAROS_PAGE_SIZE);
        }
        area->frame_check[slots + i] = page->physical;
    }
    /* Two independently posted buffers must never share a DMA frame. The
     * refill producer ring also cannot occupy a frame handed to the NIC. */
    qsort(area->frame_check,pages,sizeof(*area->frame_check),compare_frames);
    for (size_t i = 1; i < pages; i++)
        if (area->frame_check[i] == area->frame_check[i-1]) return -KERNEL_EINVAL;
    kernel_preempt_disable();
    kernel_heap_release(area->heap,area->frame_check);
    area->frame_check = NULL;
    kernel_preempt_enable();
    memset(area->rq_pages[0].page,0,2 * sizeof(uint32_t));
    return 0;
}

int kernel_io_uring_zcrx_register(struct kernel_heap *heap,struct kernel_mm *mm,
    struct kernel_uid_account *account,uint64_t request,struct kernel_io_uring_zcrx **owner)
{
    if (!kernel_credentials_capable(kernel_credentials_current(),KERNEL_CAP_NET_ADMIN))
        return -KERNEL_EPERM;
    if (*owner) return -KERNEL_EBUSY;
    struct zcrx_registration reg;
    struct zcrx_region region;
    struct zcrx_area_registration memory;
    int error = copy_in(mm,&reg,request,sizeof(reg));
    if (error) return error;
    if (reg.id || reg.reserved[0] || reg.reserved[1] || (reg.flags & ~3U)) return -KERNEL_EINVAL;
    if (reg.flags || reg.event_ptr) return -KERNEL_EOPNOTSUPP;
    error = copy_in(mm,&region,reg.region_ptr,sizeof(region));
    if (error) return error;
    if (reg.rx_queue == UINT32_MAX || !reg.entries || reg.entries > ZCRX_RQ_MAX) return -KERNEL_EINVAL;
    if (reg.rx_queue != 1) return -KERNEL_EOPNOTSUPP;
    if (reg.buffer_size && reg.buffer_size != BOAROS_PAGE_SIZE) return -KERNEL_EOPNOTSUPP;
    unsigned rounded = 1;
    while (rounded < reg.entries) rounded <<= 1;
    reg.entries = rounded;
    error = copy_in(mm,&memory,reg.area_ptr,sizeof(memory));
    if (error) return error;
    if (!memory.length || ((memory.address | memory.length) & BOAROS_PAGE_MASK) || memory.token ||
        memory.reserved[0] || memory.reserved[1] || (memory.flags & ~1U)) return -KERNEL_EINVAL;
    if (memory.flags) return -KERNEL_EOPNOTSUPP;
    if (memory.dmabuf_fd) return -KERNEL_EINVAL;
    if (!memory.address || kernel_user_range_check(memory.address,memory.length)) return -KERNEL_EFAULT;
    if (region.flags & ~ZCRX_REGION_USER) return -KERNEL_EINVAL;
    if (!!region.flags != !!region.address) return -KERNEL_EFAULT;
    if (!region.length || ((region.length | region.address) & BOAROS_PAGE_MASK) ||
        region.length < ZCRX_RQE_START + reg.entries * sizeof(struct zcrx_rqe) ||
        region.id || region.mmap_offset || region.reserved[0] || region.reserved[1] ||
        region.reserved[2] || region.reserved[3]) return -KERNEL_EINVAL;
    if (region.address && kernel_user_range_check(region.address,region.length)) return -KERNEL_EFAULT;

    struct kernel_io_uring_zcrx *area;
    if (kernel_heap_allocate_zeroed(heap,1,sizeof(*area),(void **)&area)) return -KERNEL_ENOMEM;
    area->heap = heap;
    area->allocator = mm->allocator;
    area->account = account;
    area->ifindex = reg.ifindex;
    area->entries = reg.entries;
    area->rq_user = !!region.flags;
    kernel_raw_lock_init(&area->lock,KERNEL_RAW_RANK_OBJECT + 2);
    *owner = area;
    error = prepare_memory(area,mm,&memory,&region);
    if (!error) {
        reg.offsets = (struct zcrx_offsets){.head=0,.tail=4,.rqes=ZCRX_RQE_START};
        reg.buffer_size = BOAROS_PAGE_SIZE;
        if (!area->rq_user) region.mmap_offset = KERNEL_IO_URING_ZCRX_MMAP_OFFSET;
        error = copy_out(mm,request,&reg,sizeof(reg));
        if (!error) error = copy_out(mm,reg.region_ptr,&region,sizeof(region));
        if (!error) error = copy_out(mm,reg.area_ptr,&memory,sizeof(memory));
    }
    if (!error) error = kernel_network_igb_zc_attach(area->ifindex,reg.rx_queue,&provider_ops,area);
    if (error) (void)kernel_io_uring_zcrx_release(owner);
    return error;
}

int kernel_io_uring_zcrx_map(struct kernel_io_uring_zcrx *area,uint64_t offset,uint64_t length)
{
    return !area || !is_attached(area) || area->rq_user || offset != KERNEL_IO_URING_ZCRX_MMAP_OFFSET ||
        !length || length > (uint64_t)area->acquired_rq * BOAROS_PAGE_SIZE ? -KERNEL_EINVAL : 0;
}

int kernel_io_uring_zcrx_map_page(struct kernel_io_uring_zcrx *area,uint64_t offset,uint64_t *physical)
{
    uint64_t page = (offset - KERNEL_IO_URING_ZCRX_MMAP_OFFSET) >> BOAROS_PAGE_SHIFT;
    if (!area || !is_attached(area) || area->rq_user || page >= area->acquired_rq) return -KERNEL_EINVAL;
    *physical = area->rq_pages[page].physical;
    return physical_page_acquire(area->allocator,*physical) ? -KERNEL_EIO : 0;
}
