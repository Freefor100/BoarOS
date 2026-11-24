#include <kernel/cost.h>
#include <kernel/cpu.h>
#include <arch/mmu.h>
#include <arch/uaccess.h>
#include <kernel/task.h>
#include <kernel/scheduler.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/uaccess.h>
#include "uaccess_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum kernel_uaccess_status kernel_user_range_check(
    uint64_t user_address,
    size_t size)
{
    /* 这里只做区间算术；实际权限由本核硬件访问或远端MM pin检查。 */
    if (size == 0U) {
        return KERNEL_UACCESS_STATUS_OK;
    }
    if (user_address >= ARCH_MMU_USER_LIMIT ||
        (uint64_t)size > ARCH_MMU_USER_LIMIT - user_address) {
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    return KERNEL_UACCESS_STATUS_OK;
}

static inline enum kernel_uaccess_status access_status(enum kernel_mm_status status)
{
    if (status==KERNEL_MM_STATUS_OK) return KERNEL_UACCESS_STATUS_OK;
    if (status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS ||
        status==KERNEL_MM_STATUS_ADDRESS_SPACE || status==KERNEL_MM_STATUS_BUS_FAULT ||
        status==KERNEL_MM_STATUS_NO_MEMORY) return KERNEL_UACCESS_STATUS_FAULT;
    return KERNEL_UACCESS_STATUS_STATE;
}

/* The same fault/retry rule applies to a single page and a bounded prefix.
 * Only the first page may fault; a second page is borrowed if already ready. */
static inline enum kernel_uaccess_status pin_user_prefix(
    struct kernel_mm *mm,uint64_t address,uint64_t second,uint32_t access,
    struct kernel_mm_user_pin *pin,unsigned *count)
{ return access_status(kernel_uaccess_pin_resolved(mm,address,second,access,pin,count)); }

#if ARCH_UACCESS_NATIVE
static inline int local_user_mm(const struct kernel_mm *mm)
{
    return mm && mm->state==KERNEL_MM_LIVE && mm->record &&
        kernel_cpu_current()->active_mm_record==mm->record &&
        kernel_task_has_user_context(kernel_task_current());
}

static enum kernel_uaccess_status resolve_copy_fault(struct kernel_mm *mm,
    uint64_t address,uint32_t access,uintptr_t cause)
{
    /* Access faults cannot be repaired by republishing an already valid PTE. */
    if (cause!=13 && cause!=15) return KERNEL_UACCESS_STATUS_FAULT;
    return access_status(kernel_mm_resolve_user_fault(mm,address,access));
}

static enum kernel_uaccess_status local_copy(struct kernel_mm *mm,uint64_t user,
    void *buffer,size_t size,size_t *copied,int writing)
{
    *copied=0;
    while (*copied<size) {
        size_t remaining=size-*copied;
#if BOAROS_COST_DIAGNOSTICS
        struct kernel_cost_tag tag=kernel_cost_capture();uint64_t start=kernel_cost_clock();
#endif
        struct arch_uaccess_result result=writing ?
            arch_copy_to_user((void *)(uintptr_t)(user+*copied),(char *)buffer+*copied,remaining) :
            arch_copy_from_user((char *)buffer+*copied,(void *)(uintptr_t)(user+*copied),remaining);
        if (result.remaining>remaining) __builtin_trap();
        size_t done=remaining-result.remaining;
#if BOAROS_COST_DIAGNOSTICS
        kernel_cost_add_tag(tag,COST_UACCESS_COPY_TICKS,kernel_cost_clock()-start);
        kernel_cost_add_tag(tag,COST_UACCESS_COPY_BYTES,done);
#endif
        if (writing) COST_ADD(COPY_TO_USER,done);
        else {COST_ADD(COPY_FROM_USER,done);COST_IO_ADD(3,done);}
        *copied+=done;
        if (!result.remaining) return KERNEL_UACCESS_STATUS_OK;
        /* The hardware fault already returned through its recovery PC. No
         * nested trap frame remains while the common resolver may sleep. */
        enum kernel_uaccess_status status=resolve_copy_fault(mm,user+*copied,
            writing?KERNEL_MM_WRITE:KERNEL_MM_READ,result.cause);
        if (status) return status;
    }
    return KERNEL_UACCESS_STATUS_OK;
}

static enum kernel_uaccess_status local_string(struct kernel_mm *mm,char *buffer,
    uint64_t user,size_t capacity,size_t *length)
{
    size_t copied=0;
    while (copied<capacity) {
        uint64_t current=user+copied;
        if (current>=ARCH_MMU_USER_LIMIT) break;
        size_t count=capacity-copied;
        if (count>ARCH_MMU_USER_LIMIT-current) count=ARCH_MMU_USER_LIMIT-current;
        struct arch_uaccess_result result=arch_copy_string_from_user(buffer+copied,
            (const char *)(uintptr_t)current,count);
        if (result.remaining>count) __builtin_trap();
        copied+=count-result.remaining;*length=copied;
        if (!result.cause) return result.remaining ? KERNEL_UACCESS_STATUS_OK :
            copied==capacity ? KERNEL_UACCESS_STATUS_TOO_LONG : KERNEL_UACCESS_STATUS_FAULT;
        enum kernel_uaccess_status status=resolve_copy_fault(mm,user+copied,KERNEL_MM_READ,result.cause);
        if (status) return status;
    }
    *length=copied;
    return KERNEL_UACCESS_STATUS_FAULT;
}
#endif

enum kernel_uaccess_status kernel_uaccess_pin_user_page(
    struct kernel_mm *mm,uint64_t address,uint32_t access,struct kernel_mm_user_pin *pin)
{ return pin_user_prefix(mm,address,0,access,pin,NULL); }

enum kernel_uaccess_status kernel_uaccess_pin_user_pair(
    struct kernel_mm *mm,uint64_t first,uint64_t second,uint32_t access,
    struct kernel_mm_user_pin pins[2],unsigned *count)
{ return pin_user_prefix(mm,first,second,access,pins,count); }


void kernel_uaccess_write_cursor_finish(struct kernel_uaccess_write_cursor *cursor)
{
    if (cursor->pin.registration) kernel_mm_unpin_user_page(&cursor->pin);
}

static inline void copy_to_pinned_page(void *destination,const void *source,size_t size)
{
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_tag tag=kernel_cost_capture();
    uint64_t start=kernel_cost_clock();
#endif
    memcpy(destination,source,size);
#if BOAROS_COST_DIAGNOSTICS
    kernel_cost_add_tag(tag,COST_UACCESS_COPY_TICKS,kernel_cost_clock()-start);
    kernel_cost_add_tag(tag,COST_UACCESS_COPY_BYTES,size);
#endif
    COST_ADD(COPY_TO_USER,size);
}

enum kernel_uaccess_status kernel_uaccess_write_cursor_copy(
    struct kernel_mm *mm,
    struct kernel_uaccess_write_cursor *cursor,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied)
{
    const unsigned char *source = kernel_source;
    size_t copied = 0U;

    if (mm == 0 || cursor == 0 || bytes_copied == 0 ||
        (size != 0U && kernel_source == 0)) {
        return KERNEL_UACCESS_STATUS_INVALID_ARGUMENT;
    }
    if (size == 0U) {
        *bytes_copied = 0U;
        return KERNEL_UACCESS_STATUS_OK;
    }
    if (kernel_user_range_check(user_destination, size) !=
        KERNEL_UACCESS_STATUS_OK) {
        kernel_uaccess_write_cursor_finish(cursor);
        *bytes_copied = 0U;
        return KERNEL_UACCESS_STATUS_FAULT;
    }

#if ARCH_UACCESS_NATIVE
    if (local_user_mm(mm)) {
        kernel_uaccess_write_cursor_finish(cursor);
#if BOAROS_MAX_CPUS == 1
        /* Whole-page copies can alternate between two translations at the
         * same index. QEMU's direct-mapped table uses at least six index bits. */
        const uint64_t index_mask=63U*BOAROS_PAGE_SIZE;
        const uintptr_t source_address=(uintptr_t)kernel_source;
        if (size==BOAROS_PAGE_SIZE &&
            !((user_destination | source_address)&BOAROS_PAGE_MASK) &&
            !((user_destination ^ source_address)&index_mask) &&
            !kernel_mm_try_pin_user_page(mm,user_destination,KERNEL_MM_WRITE,&cursor->pin)) {
            uintptr_t mapped=(uintptr_t)cursor->pin.page;
            bool separated=!cursor->pin.cache_write.entry &&
                ((mapped ^ source_address)&index_mask);
            if (separated) copy_to_pinned_page(cursor->pin.page,kernel_source,size);
            kernel_uaccess_write_cursor_finish(cursor);
            if (separated) {
                *bytes_copied=size;
                return KERNEL_UACCESS_STATUS_OK;
            }
        }
#endif
        return local_copy(mm,user_destination,(void *)kernel_source,size,bytes_copied,1);
    }
#endif

    while (copied < size) {
        unsigned char *page;
        uint64_t current = user_destination + (uint64_t)copied;
        size_t offset = (size_t)(current & BOAROS_PAGE_MASK);
        size_t chunk = (size_t)BOAROS_PAGE_SIZE - offset;
        enum kernel_uaccess_status status;

        if (chunk > size - copied) {
            chunk = size - copied;
        }
        uint64_t virtual_page = current & ~BOAROS_PAGE_MASK;
        if (!cursor->pin.registration || cursor->pin.mm != mm ||
            cursor->virtual_page != virtual_page) {
            kernel_uaccess_write_cursor_finish(cursor);
            status = kernel_uaccess_pin_user_page(mm,current,KERNEL_MM_WRITE,&cursor->pin);
#if defined(__loongarch__)
            if (status == KERNEL_UACCESS_STATUS_FAULT) {
                size_t done;
                status = access_status(kernel_mm_copy_local_secret(mm,current,(void *)(source+copied),
                    chunk,SECRET_COPY_OUT,&done));
                copied += done;
                COST_ADD(COPY_TO_USER,done);
                if (!status) continue;
            }
#endif
            if (status != KERNEL_UACCESS_STATUS_OK) {
                *bytes_copied = copied;
                return status;
            }
            cursor->virtual_page = virtual_page;
        }
        page = cursor->pin.page;
        copy_to_pinned_page(page+offset,source+copied,chunk);
        copied += chunk;
    }

    *bytes_copied = copied;
    return KERNEL_UACCESS_STATUS_OK;
}

static inline enum kernel_uaccess_status copy_to_user_cursor(
    struct kernel_mm *mm, uint64_t user_destination, const void *kernel_source,
    size_t size, size_t *bytes_copied)
{
    struct kernel_uaccess_write_cursor cursor = {0};
    enum kernel_uaccess_status status=kernel_uaccess_write_cursor_copy(
        mm,&cursor,user_destination,kernel_source,size,bytes_copied);
    kernel_uaccess_write_cursor_finish(&cursor);
    return status;
}

static __attribute__((noinline)) enum kernel_uaccess_status copy_to_user_pair(
    struct kernel_mm *mm,uint64_t destination,const unsigned char *source,
    size_t size,size_t first,size_t *copied)
{
    struct kernel_mm_user_pin pins[2]={{0}};
    unsigned count;
    *copied=0;
    enum kernel_uaccess_status access=kernel_uaccess_pin_user_pair(mm,destination,
        destination+first,KERNEL_MM_WRITE,pins,&count);
#if defined(__loongarch__)
    if (access==KERNEL_UACCESS_STATUS_FAULT)
        return copy_to_user_cursor(mm,destination,source,size,copied);
#endif
    if (access) return access;
    copy_to_pinned_page((unsigned char *)pins[0].page+(destination&BOAROS_PAGE_MASK),source,first);
    if (count==2) {
        copy_to_pinned_page(pins[1].page,source+first,size-first);
        kernel_mm_unpin_user_pair(pins);
        *copied=size;
        return KERNEL_UACCESS_STATUS_OK;
    }
    kernel_mm_unpin_user_page(pins);
    access=copy_to_user_cursor(mm,destination+first,source+first,size-first,copied);
    *copied+=first;
    return access;
}

enum kernel_uaccess_status kernel_copy_to_user(
    struct kernel_mm *mm, uint64_t user_destination, const void *kernel_source,
    size_t size, size_t *bytes_copied)
{
    size_t first=BOAROS_PAGE_SIZE-(user_destination&BOAROS_PAGE_MASK);
#if defined(__loongarch__)
    /* A one-shot copy within one destination page owns the pin until the
     * last byte. Batched callers keep using the reusable write cursor. */
    if (size && size <= first && mm && kernel_source && bytes_copied) {
        *bytes_copied = 0;
        if (kernel_user_range_check(user_destination, size))
            return KERNEL_UACCESS_STATUS_FAULT;
        struct kernel_mm_user_pin pin = {0};
        enum kernel_uaccess_status status =
            pin_user_prefix(mm, user_destination, 0, KERNEL_MM_WRITE, &pin, NULL);
        if (status == KERNEL_UACCESS_STATUS_FAULT) {
            status = access_status(kernel_mm_copy_local_secret(mm, user_destination,
                (void *)kernel_source, size, SECRET_COPY_OUT, bytes_copied));
            COST_ADD(COPY_TO_USER, *bytes_copied);
        } else if (status == KERNEL_UACCESS_STATUS_OK) {
            copy_to_pinned_page((unsigned char *)pin.page + (user_destination & BOAROS_PAGE_MASK),
                                kernel_source, size);
            kernel_mm_unpin_user_page(&pin);
            *bytes_copied = size;
        }
        return status;
    }
#endif
    if (size>first && size-first<=BOAROS_PAGE_SIZE && mm && kernel_source &&
        bytes_copied && !kernel_user_range_check(user_destination,size)
#if ARCH_UACCESS_NATIVE
        && !local_user_mm(mm)
#endif
        )
        return copy_to_user_pair(mm,user_destination,kernel_source,size,first,bytes_copied);
    return copy_to_user_cursor(mm,user_destination,kernel_source,size,bytes_copied);
}

enum kernel_uaccess_status kernel_copy_from_user(
    struct kernel_mm *mm,
    void *kernel_destination,
    uint64_t user_source,
    size_t size,
    size_t *bytes_copied
    )
{
    unsigned char *destination = kernel_destination;
    size_t copied = 0U;

    if (mm == 0 || bytes_copied == 0 ||
        (size != 0U && kernel_destination == 0)) {
        return KERNEL_UACCESS_STATUS_INVALID_ARGUMENT;
    }
    if (size == 0U) {
        *bytes_copied = 0U;
        return KERNEL_UACCESS_STATUS_OK;
    }
    if (kernel_user_range_check(user_source, size) !=
        KERNEL_UACCESS_STATUS_OK) {
        *bytes_copied = 0U;
        return KERNEL_UACCESS_STATUS_FAULT;
    }

#if ARCH_UACCESS_NATIVE
    if (local_user_mm(mm)) return local_copy(mm,user_source,kernel_destination,size,bytes_copied,0);
#endif

    while (copied < size) {
        struct kernel_mm_user_pin pin = {0};
        unsigned char *page;
        uint64_t current = user_source + (uint64_t)copied;
        size_t offset = (size_t)(current & BOAROS_PAGE_MASK);
        size_t chunk = (size_t)BOAROS_PAGE_SIZE - offset;
        enum kernel_uaccess_status status;

        if (chunk > size - copied) {
            chunk = size - copied;
        }
        status = kernel_uaccess_pin_user_page(mm,current,KERNEL_MM_READ,&pin);
#if defined(__loongarch__)
        if (status == KERNEL_UACCESS_STATUS_FAULT) {
            size_t done;
            status = access_status(kernel_mm_copy_local_secret(mm,current,destination+copied,
                chunk,SECRET_COPY_IN,&done));
            copied += done;
            COST_ADD(COPY_FROM_USER,done); COST_IO_ADD(3,done);
            if (!status) continue;
        }
#endif
        if (status != KERNEL_UACCESS_STATUS_OK) {
            *bytes_copied = copied;
            return status;
        }
#if BOAROS_COST_DIAGNOSTICS
        struct kernel_cost_tag memory_tag = kernel_cost_capture();
        uint64_t memory_start = kernel_cost_clock();
#endif
        page = pin.page;
        memcpy(destination + copied, page + offset, chunk);
#if BOAROS_COST_DIAGNOSTICS
        uint64_t memory_elapsed = kernel_cost_clock() - memory_start;
        kernel_cost_add_tag(memory_tag, COST_UACCESS_COPY_TICKS, memory_elapsed);
        kernel_cost_add_tag(memory_tag, COST_UACCESS_COPY_BYTES, chunk);
#endif
        kernel_mm_unpin_user_page(&pin);
        COST_ADD(COPY_FROM_USER, chunk); COST_IO_ADD(3, chunk);
        copied += chunk;
    }

    *bytes_copied = copied;
    return KERNEL_UACCESS_STATUS_OK;
}


enum kernel_uaccess_status kernel_user_cmpxchg_u32(
    struct kernel_mm *mm, uint64_t user_address,
    uint32_t expected, uint32_t desired, uint32_t *observed)
{
    struct kernel_mm_user_pin pin = {0};
    unsigned char *page;
    uint32_t current = expected;
    enum kernel_uaccess_status status;

    if (mm == 0 || observed == 0)
        return KERNEL_UACCESS_STATUS_INVALID_ARGUMENT;
    if ((user_address & 3U) != 0U ||
        kernel_user_range_check(user_address, sizeof(uint32_t)) !=
            KERNEL_UACCESS_STATUS_OK)
        return KERNEL_UACCESS_STATUS_FAULT;
    status = kernel_uaccess_pin_user_page(mm, user_address, KERNEL_MM_WRITE, &pin);
    if (status != KERNEL_UACCESS_STATUS_OK) return status;
    page = pin.page;
    (void)__atomic_compare_exchange_n(
        (uint32_t *)(void *)(page + (user_address & BOAROS_PAGE_MASK)),
        &current, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    kernel_mm_unpin_user_page(&pin);
    *observed = current;
    return KERNEL_UACCESS_STATUS_OK;
}

enum kernel_uaccess_status kernel_copy_string_from_user(
    struct kernel_mm *mm,
    char *kernel_destination,
    uint64_t user_source,
    size_t capacity,
    size_t *string_length)
{
    size_t copied = 0U;

    if (mm == 0 || kernel_destination == 0 || capacity == 0U ||
        string_length == 0) {
        return KERNEL_UACCESS_STATUS_INVALID_ARGUMENT;
    }
    *string_length = 0U;
    if (user_source >= ARCH_MMU_USER_LIMIT) {
        return KERNEL_UACCESS_STATUS_FAULT;
    }

#if ARCH_UACCESS_NATIVE
    if (local_user_mm(mm)) return local_string(mm,kernel_destination,user_source,capacity,string_length);
#endif

    while (copied < capacity) {
        struct kernel_mm_user_pin pin = {0};
        unsigned char *page;
        uint64_t current = user_source + (uint64_t)copied;
        size_t offset;
        size_t chunk;
        size_t index;
        enum kernel_uaccess_status status;

        if (current < user_source || current >= ARCH_MMU_USER_LIMIT) {
            *string_length = copied;
            return KERNEL_UACCESS_STATUS_FAULT;
        }
        offset = (size_t)(current & BOAROS_PAGE_MASK);
        chunk = (size_t)BOAROS_PAGE_SIZE - offset;
        if (chunk > capacity - copied) {
            chunk = capacity - copied;
        }
        if ((uint64_t)chunk > ARCH_MMU_USER_LIMIT - current) {
            chunk = (size_t)(ARCH_MMU_USER_LIMIT - current);
        }
        status = kernel_uaccess_pin_user_page(mm,current,KERNEL_MM_READ,&pin);
#if defined(__loongarch__)
        if (status == KERNEL_UACCESS_STATUS_FAULT) {
            size_t done;
            status = access_status(kernel_mm_copy_local_secret(mm,current,kernel_destination+copied,
                chunk,SECRET_COPY_STRING,&done));
            copied += done;
            if (!status) {
                if (done == chunk) continue;
                *string_length = copied;
                return KERNEL_UACCESS_STATUS_OK;
            }
        }
#endif
        if (status != KERNEL_UACCESS_STATUS_OK) {
            *string_length = copied;
            return status;
        }
        page = pin.page;
        for (index = 0U; index < chunk; index++) {
            char byte = (char)page[offset + index];

            kernel_destination[copied] = byte;
            if (byte == '\0') {
                kernel_mm_unpin_user_page(&pin);
                *string_length = copied;
                return KERNEL_UACCESS_STATUS_OK;
            }
            copied++;
        }
        kernel_mm_unpin_user_page(&pin);
    }

    *string_length = copied;
    return KERNEL_UACCESS_STATUS_TOO_LONG;
}
