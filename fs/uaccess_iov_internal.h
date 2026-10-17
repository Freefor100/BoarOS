#ifndef BOAROS_FS_UACCESS_IOV_INTERNAL_H
#define BOAROS_FS_UACCESS_IOV_INTERNAL_H

#include <kernel/files.h>
#include <kernel/uaccess.h>
#include <kernel/page.h>
#include <kernel/errno.h>
#include "../mm/uaccess_internal.h"

#include <stddef.h>

struct kernel_uaccess_iov_cursor {
    const struct kernel_uaccess_iovec *iov;
    size_t count;
    size_t index;
    uint64_t offset;
};

/* The caller owns storage for the already bounded descriptor count. Linux
 * caps a sole vector before access_ok; multiple vectors check original ranges
 * and cap the aggregate without rejecting a sum larger than ssize_t. */
static inline int kernel_uaccess_iov_import(struct kernel_mm *mm,
    uint64_t user_iov,struct kernel_uaccess_iovec *iov,size_t count,uint64_t *total)
{
    *total=0;
    if (!count) return 0;
    size_t copied=0;
    enum kernel_uaccess_status status=kernel_copy_from_user(mm,iov,user_iov,
        count*sizeof(*iov),&copied);
    if (status==KERNEL_UACCESS_STATUS_FAULT) return -KERNEL_EFAULT;
    if (status || copied!=count*sizeof(*iov)) __builtin_trap();
    for (size_t i=0;i<count;i++)
        if (iov[i].length>INT64_MAX) return -KERNEL_EINVAL;
    for (size_t i=0;i<count;i++) {
        if (count==1 && iov[i].length>KERNEL_FILES_MAX_RW_COUNT)
            iov[i].length=KERNEL_FILES_MAX_RW_COUNT;
        if (kernel_user_range_check(iov[i].base,iov[i].length) ||
            (!iov[i].length && iov[i].base && kernel_user_range_check(iov[i].base-1,1)))
            return -KERNEL_EFAULT;
        if (iov[i].length>KERNEL_FILES_MAX_RW_COUNT-*total)
            iov[i].length=KERNEL_FILES_MAX_RW_COUNT-*total;
        *total+=iov[i].length;
    }
    return 0;
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
