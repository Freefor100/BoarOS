#ifndef BOAROS_FS_UACCESS_IOV_INTERNAL_H
#define BOAROS_FS_UACCESS_IOV_INTERNAL_H

#include <kernel/files.h>
#include <kernel/uaccess.h>

#include <stddef.h>

struct kernel_uaccess_iov_cursor {
    const struct kernel_uaccess_iovec *iov;
    size_t count;
    size_t index;
    uint64_t offset;
};

/* The caller commits backend progress separately. A FAULT can carry a
 * successfully copied prefix, including one spanning prior iovecs. */
static inline enum kernel_uaccess_status kernel_copy_to_user_iov(
    struct kernel_mm *mm,
    struct kernel_uaccess_iov_cursor *cursor,
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
        status = kernel_copy_to_user(mm,
                    cursor->iov[cursor->index].base + cursor->offset,
                    (const unsigned char *)source + *copied,
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
