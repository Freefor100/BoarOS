#ifndef BOAROS_FS_WRITE_SOURCE_H
#define BOAROS_FS_WRITE_SOURCE_H

#include "uaccess_iov_internal.h"
#include "../mm/uaccess_internal.h"
#include <kernel/cost.h>
#include <string.h>

struct kernel_page_cache;
struct kernel_vfs_file;

/* 两种实际写入来源共用最终页填入；data仅在当前片段pin存活时借用用户RAM。 */
struct kernel_vfs_write_source {
    const unsigned char *data;
    struct kernel_mm *mm;
    struct kernel_uaccess_iov_cursor user;
    enum kernel_uaccess_status access;
    size_t chunks;
};

static inline size_t kernel_vfs_write_source_span(
    struct kernel_vfs_write_source *source, size_t limit)
{
    if (!source->mm) return limit;
    uint64_t address;
    return kernel_uaccess_iov_span(&source->user, limit, &address);
}

/* 同一真实用户页内可含不连续iovec；先确定该pin能覆盖的逻辑前缀，
 * 目标页只准备一次，复制仍逐片段进行以保留共享映射别名的次序。 */
static inline size_t kernel_vfs_write_source_page_span(
    struct kernel_vfs_write_source *source,size_t limit,size_t *first)
{
    if (!source->mm) { *first=limit;return limit; }
    uint64_t address;
    *first=kernel_uaccess_iov_span(&source->user,limit,&address);
    if (!*first || *first==limit) return *first;
    uint64_t page=address&~BOAROS_PAGE_MASK;
    struct kernel_uaccess_iov_cursor cursor=source->user;
    size_t count=*first;cursor.offset+=*first;
    while (count<limit) {
        size_t part=kernel_uaccess_iov_span(&cursor,limit-count,&address);
        if (!part || (address&~BOAROS_PAGE_MASK)!=page) break;
        count+=part;cursor.offset+=part;
    }
    return count;
}

static inline void kernel_vfs_write_source_use_pin(
    struct kernel_vfs_write_source *source, const struct kernel_mm_user_pin *pin)
{
    uint64_t address=source->user.iov[source->user.index].base+source->user.offset;
    source->data=(const unsigned char *)pin->page+(address&BOAROS_PAGE_MASK);
}

static inline int kernel_vfs_write_source_pin(
    struct kernel_vfs_write_source *source, struct kernel_mm_user_pin *pin)
{
    if (!source->mm) return 1;
    uint64_t address = source->user.iov[source->user.index].base + source->user.offset;
    source->access = kernel_uaccess_pin_user_page(source->mm, address, KERNEL_MM_READ, pin);
    if (source->access) return 0;
    kernel_vfs_write_source_use_pin(source,pin);
    return 1;
}

static inline void kernel_vfs_write_source_copy(
    struct kernel_vfs_write_source *source, void *destination, size_t size)
{
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_tag tag = kernel_cost_capture();
    uint64_t start = source->mm ? kernel_cost_clock() : 0;
#endif
    /* MAP_SHARED来源可以就是目标页，按该实际片段的重叠方向复制。 */
    memmove(destination, source->data, size);
    if (source->mm) {
#if BOAROS_COST_DIAGNOSTICS
        kernel_cost_add_tag(tag, COST_UACCESS_COPY_TICKS, kernel_cost_clock() - start);
        kernel_cost_add_tag(tag, COST_UACCESS_COPY_BYTES, size);
#endif
        COST_ADD(COPY_FROM_USER, size); COST_IO_ADD(3, size);
    } else COST_ADD(CACHE_COPY, size);
}

static inline void kernel_vfs_write_source_advance(
    struct kernel_vfs_write_source *source, size_t size)
{
    if (source->mm) { source->user.offset += size; source->data = 0; source->chunks++; }
    else source->data += size;
}

int kernel_page_cache_write_source(struct kernel_page_cache *, struct kernel_vfs_file *,
    uint64_t offset, struct kernel_vfs_write_source *, size_t size, size_t *written);

#endif
