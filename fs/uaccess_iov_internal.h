#ifndef BOAROS_FS_UACCESS_IOV_INTERNAL_H
#define BOAROS_FS_UACCESS_IOV_INTERNAL_H

#include <kernel/files.h>
#include <kernel/uaccess.h>
#include <kernel/page.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include "../mm/uaccess_internal.h"

#include <stddef.h>

struct kernel_uaccess_iov_cursor {
    const struct kernel_uaccess_iovec *iov;
    size_t count;
    size_t index;
    uint64_t offset;
};

/* 导入后view/count/total不变；allocation单独交接给可取消的TTY请求。 */
struct kernel_uaccess_iov_snapshot {
    struct kernel_uaccess_iovec local[8];
    const struct kernel_uaccess_iovec *view;
    size_t count;
    uint64_t total;
    void *allocation;
};

/* count已由入口按各自errno限制。先检查所有有符号长度，再检查原始
 * 数值范围；即使前缀已达到MAX_RW_COUNT，非法尾向量仍须返回EFAULT。 */
static inline enum kernel_files_status kernel_uaccess_iov_import(
    struct kernel_uaccess_iov_snapshot *snapshot, struct kernel_heap *heap,
    struct kernel_mm *mm, uint64_t user_iov, size_t count, int64_t *result)
{
    snapshot->view=snapshot->local;
    snapshot->count=count;
    snapshot->total=0;
    *result=0;
    if (!count) return KERNEL_FILES_STATUS_OK;
    struct kernel_uaccess_iovec *iov=snapshot->local;
    if (count>sizeof(snapshot->local)/sizeof(snapshot->local[0])) {
        enum kernel_heap_status status=kernel_heap_allocate(heap,count*sizeof(*iov),&snapshot->allocation);
        if (status==KERNEL_HEAP_STATUS_EMPTY) { *result=-KERNEL_ENOMEM; return KERNEL_FILES_STATUS_OK; }
        if (status) return KERNEL_FILES_STATUS_STATE;
        iov=snapshot->allocation;
        snapshot->view=iov;
    }
    size_t copied=0;
    enum kernel_uaccess_status status=kernel_copy_from_user(mm,iov,user_iov,count*sizeof(*iov),&copied);
    if (status==KERNEL_UACCESS_STATUS_FAULT) { *result=-KERNEL_EFAULT; return KERNEL_FILES_STATUS_OK; }
    if (status || copied!=count*sizeof(*iov)) __builtin_trap();
    for (size_t i=0;i<count;i++)
        if (iov[i].length>INT64_MAX) { *result=-KERNEL_EINVAL; return KERNEL_FILES_STATUS_OK; }
    for (size_t i=0;i<count;i++) {
        if (count==1 && iov[i].length>KERNEL_FILES_MAX_RW_COUNT)
            iov[i].length=KERNEL_FILES_MAX_RW_COUNT;
        if (kernel_user_range_check(iov[i].base,iov[i].length) ||
            (!iov[i].length && iov[i].base && kernel_user_range_check(iov[i].base-1,1))) {
            *result=-KERNEL_EFAULT; return KERNEL_FILES_STATUS_OK;
        }
        if (iov[i].length>KERNEL_FILES_MAX_RW_COUNT-snapshot->total)
            iov[i].length=KERNEL_FILES_MAX_RW_COUNT-snapshot->total;
        snapshot->total+=iov[i].length;
    }
    return KERNEL_FILES_STATUS_OK;
}

static inline void kernel_uaccess_iov_cleanup(struct kernel_uaccess_iov_snapshot *snapshot,
    struct kernel_heap *heap)
{
    if (snapshot->allocation) kernel_heap_release(heap,snapshot->allocation);
    snapshot->allocation=0;
}

/* 描述下一段但不解析用户页；复制和后端进展分别由调用者提交。 */
static inline size_t kernel_uaccess_iov_span(struct kernel_uaccess_iov_cursor *cursor,
    size_t limit, uint64_t *address)
{
    while (cursor->index < cursor->count && cursor->offset == cursor->iov[cursor->index].length) {
        cursor->index++; cursor->offset = 0;
    }
    if (cursor->index == cursor->count) return 0;
    *address = cursor->iov[cursor->index].base + cursor->offset;
    uint64_t available = cursor->iov[cursor->index].length - cursor->offset;
    if (available < limit) limit = (size_t)available;
    size_t page_left = BOAROS_PAGE_SIZE - (*address & BOAROS_PAGE_MASK);
    return limit < page_left ? limit : page_left;
}

/* The caller commits backend progress separately. A FAULT can carry a
 * successfully copied prefix, including one spanning prior iovecs. */
static inline enum kernel_uaccess_status kernel_copy_to_user_iov_impl(
    struct kernel_mm *mm,
    struct kernel_uaccess_iov_cursor *cursor,
    struct kernel_uaccess_write_cursor *pages,
    const void *source,
    size_t length,
    size_t *copied)
{
    *copied = 0U;
    while (*copied < length) {
        size_t part = 0U;
        size_t chunk;
        enum kernel_uaccess_status status;

        while (cursor->index < cursor->count &&
               cursor->offset == cursor->iov[cursor->index].length) {
            cursor->index++;
            cursor->offset = 0U;
        }
        if (cursor->index == cursor->count) {
            return KERNEL_UACCESS_STATUS_STATE;
        }
        chunk = length - *copied;
        if ((uint64_t)chunk > cursor->iov[cursor->index].length -
                              cursor->offset) {
            chunk = (size_t)(cursor->iov[cursor->index].length -
                             cursor->offset);
        }
        uint64_t address = cursor->iov[cursor->index].base + cursor->offset;
        const void *data = (const unsigned char *)source + *copied;
        status = pages ? kernel_uaccess_write_cursor_copy(mm, pages, address, data, chunk, &part)
                       : kernel_copy_to_user(mm, address, data, chunk, &part);
        if (part > chunk) {
            return KERNEL_UACCESS_STATUS_STATE;
        }
        cursor->offset += part;
        *copied += part;
        if (status != KERNEL_UACCESS_STATUS_OK || part != chunk) {
            return status == KERNEL_UACCESS_STATUS_OK
                       ? KERNEL_UACCESS_STATUS_STATE : status;
        }
    }
    return KERNEL_UACCESS_STATUS_OK;
}

static inline enum kernel_uaccess_status kernel_copy_to_user_iov(
    struct kernel_mm *mm, struct kernel_uaccess_iov_cursor *cursor,
    const void *source, size_t length, size_t *copied)
{
    return kernel_copy_to_user_iov_impl(mm, cursor, 0, source, length, copied);
}

static inline enum kernel_uaccess_status kernel_copy_to_user_iov_cursor(
    struct kernel_mm *mm, struct kernel_uaccess_iov_cursor *cursor,
    struct kernel_uaccess_write_cursor *pages, const void *source,
    size_t length, size_t *copied)
{
    return kernel_copy_to_user_iov_impl(mm, cursor, pages, source, length, copied);
}

/* A pipe writer publishes a fragment only after this copies the complete
 * requested slice. Bytes already placed in uncommitted ring space on FAULT
 * remain invisible to readers. */
static inline enum kernel_uaccess_status kernel_copy_from_user_iov(
    struct kernel_mm *mm,
    struct kernel_uaccess_iov_cursor *cursor,
    void *destination,
    size_t length,
    size_t *copied)
{
    *copied = 0U;
    while (*copied < length) {
        size_t part = 0U;
        size_t chunk;
        enum kernel_uaccess_status status;

        while (cursor->index < cursor->count &&
               cursor->offset == cursor->iov[cursor->index].length) {
            cursor->index++;
            cursor->offset = 0U;
        }
        if (cursor->index == cursor->count) {
            return KERNEL_UACCESS_STATUS_STATE;
        }
        chunk = length - *copied;
        if ((uint64_t)chunk > cursor->iov[cursor->index].length -
                              cursor->offset) {
            chunk = (size_t)(cursor->iov[cursor->index].length -
                             cursor->offset);
        }
        status = kernel_copy_from_user(mm,
                    (unsigned char *)destination + *copied,
                    cursor->iov[cursor->index].base + cursor->offset,
                    chunk, &part);
        if (part > chunk) {
            return KERNEL_UACCESS_STATUS_STATE;
        }
        cursor->offset += part;
        *copied += part;
        if (status != KERNEL_UACCESS_STATUS_OK || part != chunk) {
            return status == KERNEL_UACCESS_STATUS_OK
                       ? KERNEL_UACCESS_STATUS_STATE : status;
        }
    }
    return KERNEL_UACCESS_STATUS_OK;
}

#endif
