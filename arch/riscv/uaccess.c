#include <arch/riscv/sv39.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

enum kernel_uaccess_status kernel_copy_to_user(
    const struct kernel_mm *mm,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied)
{
    const unsigned char *source = kernel_source;
    size_t copied = 0U;

    if (mm == 0 || bytes_copied == 0 ||
        (size != 0U && kernel_source == 0)) {
        return KERNEL_UACCESS_STATUS_INVALID_ARGUMENT;
    }
    if (size == 0U) {
        *bytes_copied = 0U;
        return KERNEL_UACCESS_STATUS_OK;
    }
    if (user_destination >= RISCV_SV39_USER_LIMIT ||
        (uint64_t)size > RISCV_SV39_USER_LIMIT - user_destination) {
        *bytes_copied = 0U;
        return KERNEL_UACCESS_STATUS_FAULT;
    }

    while (copied < size) {
        struct kernel_mm_mapping mapping;
        enum kernel_mm_status mm_status;
        unsigned char *page;
        uint64_t current = user_destination + (uint64_t)copied;
        size_t offset = (size_t)(current & BOAROS_PAGE_MASK);
        size_t chunk = (size_t)BOAROS_PAGE_SIZE - offset;
        void *pointer;
        size_t index;

        if (chunk > size - copied) {
            chunk = size - copied;
        }
        mm_status = kernel_mm_lookup(mm, current, &mapping);
        if (mm_status == KERNEL_MM_STATUS_NOT_MAPPED) {
            *bytes_copied = copied;
            return KERNEL_UACCESS_STATUS_FAULT;
        }
        if (mm_status != KERNEL_MM_STATUS_OK) {
            *bytes_copied = copied;
            return KERNEL_UACCESS_STATUS_STATE;
        }
        if ((mapping.permissions &
             (KERNEL_MM_USER | KERNEL_MM_WRITE)) !=
            (KERNEL_MM_USER | KERNEL_MM_WRITE)) {
            *bytes_copied = copied;
            return KERNEL_UACCESS_STATUS_FAULT;
        }
        if (physical_page_resolve(mm->allocator,
                                  mapping.physical_address &
                                      ~BOAROS_PAGE_MASK,
                                  &pointer) != PHYSICAL_PAGE_STATUS_OK) {
            *bytes_copied = copied;
            return KERNEL_UACCESS_STATUS_STATE;
        }
        page = pointer;
        for (index = 0U; index < chunk; index++) {
            page[offset + index] = source[copied + index];
        }
        copied += chunk;
    }

    *bytes_copied = copied;
    return KERNEL_UACCESS_STATUS_OK;
}
