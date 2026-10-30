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

/* 连续来源填一个目标页至多涉及两个用户页。散布来源也只保留这份
 * 有界前缀；pin固定在原地址，跨目标页或等待前归还，不保存载荷副本。 */
struct kernel_vfs_write_view {
    struct kernel_mm_user_pin pins[2];
    uint64_t pages[2];
    size_t size,first;
    unsigned count;
};

static inline int kernel_vfs_write_source_prepare(struct kernel_vfs_write_source *source,
    size_t limit,struct kernel_vfs_write_view *view)
{
    if (view->count) __builtin_trap();
    view->size=0;view->first=kernel_vfs_write_source_span(source,limit);
    if (!view->first) { source->access=KERNEL_UACCESS_STATUS_STATE;return 0; }
    if (!source->mm) { view->size=limit;return 1; }
    if (!kernel_vfs_write_source_pin(source,&view->pins[0])) return 0;
    uint64_t address=source->user.iov[source->user.index].base+source->user.offset;
    view->pages[0]=address&~BOAROS_PAGE_MASK;view->count=1;view->size=view->first;
    if (view->size==limit) return 1;
    struct kernel_uaccess_iov_cursor cursor=source->user;
    cursor.offset+=view->first;
    while (view->size<limit) {
        size_t part=kernel_uaccess_iov_span(&cursor,limit-view->size,&address);
        if (!part) __builtin_trap();
        uint64_t page=address&~BOAROS_PAGE_MASK;
        unsigned index=0;
        while (index<view->count && view->pages[index]!=page) index++;
        if (index==view->count) {
            if (index==2) break;
            /* MM编辑者可能正排空首pin；后续页只取得现成许可，
             * 先提交已有前缀，下一轮无pin后再处理真正缺页。 */
            COST_ADD(USER_RESOLUTIONS,1);COST_IO_ADD(4,1);
            enum kernel_mm_status status=kernel_mm_pin_user_page(source->mm,address,
                KERNEL_MM_READ,&view->pins[index]);
            if (status==KERNEL_MM_STATUS_NOT_MAPPED || status==KERNEL_MM_STATUS_ACCESS ||
                status==KERNEL_MM_STATUS_CONFLICT || status==KERNEL_MM_STATUS_ADDRESS_SPACE ||
                status==KERNEL_MM_STATUS_BUS_FAULT || status==KERNEL_MM_STATUS_NO_MEMORY) break;
            if (status) __builtin_trap();
            view->pages[index]=page;view->count++;
        }
        view->size+=part;cursor.offset+=part;
    }
    return 1;
}

static inline void kernel_vfs_write_source_copy_view(struct kernel_vfs_write_source *source,
    void *destination,const struct kernel_vfs_write_view *view)
{
    kernel_vfs_write_source_copy(source,destination,view->first);
    kernel_vfs_write_source_advance(source,view->first);
    for (size_t copied=view->first;copied<view->size;) {
        size_t part=kernel_vfs_write_source_span(source,view->size-copied);
        if (!part) __builtin_trap();
        uint64_t address=source->user.iov[source->user.index].base+source->user.offset;
        unsigned index=0;
        while (index<view->count && view->pages[index]!=(address&~BOAROS_PAGE_MASK)) index++;
        if (index==view->count) __builtin_trap();
        kernel_vfs_write_source_use_pin(source,&view->pins[index]);
        kernel_vfs_write_source_copy(source,(unsigned char *)destination+copied,part);
        kernel_vfs_write_source_advance(source,part);copied+=part;
    }
}

static inline void kernel_vfs_write_source_finish(struct kernel_vfs_write_source *source,
    struct kernel_vfs_write_view *view)
{
    while (view->count) kernel_mm_unpin_user_page(&view->pins[--view->count]);
    if (source->mm) source->data=NULL;
}

int kernel_page_cache_write_source(struct kernel_page_cache *, struct kernel_vfs_file *,
    uint64_t offset, struct kernel_vfs_write_source *, size_t size, size_t *written);

#endif
