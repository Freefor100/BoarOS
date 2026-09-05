#include <arch/riscv/sv39.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

enum kernel_uaccess_status kernel_user_range_check(
    uint64_t user_address,
    size_t size)
{
    if (size == 0U) {
        return KERNEL_UACCESS_STATUS_OK;
    }
    if (user_address >= RISCV_SV39_USER_LIMIT ||
        (uint64_t)size > RISCV_SV39_USER_LIMIT - user_address) {
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    return KERNEL_UACCESS_STATUS_OK;
}

static enum kernel_uaccess_status resolve_user_page(
    struct kernel_mm *mm,
    uint64_t user_address,
    uint32_t required_permissions,
    unsigned char **page)
{
    struct kernel_mm_mapping mapping;
    void *pointer;
    enum kernel_mm_status mm_status;

    mm_status = kernel_mm_lookup(mm, user_address, &mapping);
    if (mm_status == KERNEL_MM_STATUS_NOT_MAPPED ||
        (mm_status == KERNEL_MM_STATUS_OK &&
         (mapping.permissions &
          (required_permissions | KERNEL_MM_USER)) !=
             (required_permissions | KERNEL_MM_USER))) {
        mm_status = kernel_mm_resolve_user_fault(
            mm,
            user_address,
            required_permissions);
        if (mm_status == KERNEL_MM_STATUS_NOT_MAPPED ||
            mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE ||
            mm_status == KERNEL_MM_STATUS_BUS_FAULT ||
            mm_status == KERNEL_MM_STATUS_NO_MEMORY) {
            return KERNEL_UACCESS_STATUS_FAULT;
        }
        if (mm_status != KERNEL_MM_STATUS_OK ||
            kernel_mm_lookup(mm, user_address, &mapping) !=
                KERNEL_MM_STATUS_OK) {
            return KERNEL_UACCESS_STATUS_STATE;
        }
    }
    if (mm_status != KERNEL_MM_STATUS_OK) {
        return KERNEL_UACCESS_STATUS_STATE;
    }
    required_permissions |= KERNEL_MM_USER;
    if ((mapping.permissions & required_permissions) !=
        required_permissions) {
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    if (physical_page_resolve(mm->allocator,
                              mapping.physical_address &
                                  ~BOAROS_PAGE_MASK,
                              &pointer) != PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_UACCESS_STATUS_STATE;
    }
    *page = pointer;
    return KERNEL_UACCESS_STATUS_OK;
}

enum kernel_uaccess_status kernel_copy_to_user(
    struct kernel_mm *mm,
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
    if (kernel_user_range_check(user_destination, size) !=
        KERNEL_UACCESS_STATUS_OK) {
        *bytes_copied = 0U;
        return KERNEL_UACCESS_STATUS_FAULT;
    }

    while (copied < size) {
        unsigned char *page;
        uint64_t current = user_destination + (uint64_t)copied;
        size_t offset = (size_t)(current & BOAROS_PAGE_MASK);
        size_t chunk = (size_t)BOAROS_PAGE_SIZE - offset;
        size_t index;
        enum kernel_uaccess_status status;

        if (chunk > size - copied) {
            chunk = size - copied;
        }
        status = resolve_user_page(mm,
                                   current,
                                   KERNEL_MM_WRITE,
                                   &page);
        if (status != KERNEL_UACCESS_STATUS_OK) {
            *bytes_copied = copied;
            return status;
        }
        for (index = 0U; index < chunk; index++) {
            page[offset + index] = source[copied + index];
        }
        copied += chunk;
    }

    *bytes_copied = copied;
    return KERNEL_UACCESS_STATUS_OK;
}

enum kernel_uaccess_status kernel_copy_from_user(
    struct kernel_mm *mm,
    void *kernel_destination,
    uint64_t user_source,
    size_t size,
    size_t *bytes_copied)
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

    while (copied < size) {
        unsigned char *page;
        uint64_t current = user_source + (uint64_t)copied;
        size_t offset = (size_t)(current & BOAROS_PAGE_MASK);
        size_t chunk = (size_t)BOAROS_PAGE_SIZE - offset;
        size_t index;
        enum kernel_uaccess_status status;

        if (chunk > size - copied) {
            chunk = size - copied;
        }
        status = resolve_user_page(mm,
                                   current,
                                   KERNEL_MM_READ,
                                   &page);
        if (status != KERNEL_UACCESS_STATUS_OK) {
            *bytes_copied = copied;
            return status;
        }
        for (index = 0U; index < chunk; index++) {
            destination[copied + index] = page[offset + index];
        }
        copied += chunk;
    }

    *bytes_copied = copied;
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
    if (user_source >= RISCV_SV39_USER_LIMIT) {
        return KERNEL_UACCESS_STATUS_FAULT;
    }

    while (copied < capacity) {
        unsigned char *page;
        uint64_t current = user_source + (uint64_t)copied;
        size_t offset;
        size_t chunk;
        size_t index;
        enum kernel_uaccess_status status;

        if (current < user_source || current >= RISCV_SV39_USER_LIMIT) {
            *string_length = copied;
            return KERNEL_UACCESS_STATUS_FAULT;
        }
        offset = (size_t)(current & BOAROS_PAGE_MASK);
        chunk = (size_t)BOAROS_PAGE_SIZE - offset;
        if (chunk > capacity - copied) {
            chunk = capacity - copied;
        }
        if ((uint64_t)chunk > RISCV_SV39_USER_LIMIT - current) {
            chunk = (size_t)(RISCV_SV39_USER_LIMIT - current);
        }
        status = resolve_user_page(mm,
                                   current,
                                   KERNEL_MM_READ,
                                   &page);
        if (status != KERNEL_UACCESS_STATUS_OK) {
            *string_length = copied;
            return status;
        }
        for (index = 0U; index < chunk; index++) {
            char byte = (char)page[offset + index];

            kernel_destination[copied] = byte;
            if (byte == '\0') {
                *string_length = copied;
                return KERNEL_UACCESS_STATUS_OK;
            }
            copied++;
        }
    }

    *string_length = copied;
    return KERNEL_UACCESS_STATUS_TOO_LONG;
}
