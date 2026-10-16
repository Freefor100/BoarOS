#include <kernel/cost.h>
#include <kernel/cpu.h>
#include <arch/mmu.h>
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
    /* 这里只做区间算术；真正访问由MM pin检查绑定域及权限。 */
    if (size == 0U) {
        return KERNEL_UACCESS_STATUS_OK;
    }
    if (user_address >= ARCH_MMU_USER_LIMIT ||
        (uint64_t)size > ARCH_MMU_USER_LIMIT - user_address) {
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    return KERNEL_UACCESS_STATUS_OK;
}

/* 每个基页片段持有实际物理owner；fault可能睡眠，成功后重新pin。 */
enum kernel_uaccess_status kernel_uaccess_pin_user_page(
    struct kernel_mm *mm, uint64_t address, uint32_t access,
    struct kernel_mm_user_pin *pin)
{
    COST_ADD(USER_RESOLUTIONS, 1); COST_IO_ADD(4, 1);
    enum kernel_mm_status status = kernel_mm_pin_user_page(mm, address, access, pin);
    while (status == KERNEL_MM_STATUS_NOT_MAPPED || status == KERNEL_MM_STATUS_ACCESS) {
        status = kernel_mm_resolve_user_fault(mm, address, access);
        if (status != KERNEL_MM_STATUS_OK) break;
        /* fault归还临时写许可后，快照可再次撤W。最终pin才接纳复制；
         * 重做fault会复查真实VMA/EOF，CONFLICT与真实撤权都不忙等。 */
        status = kernel_mm_pin_user_page(mm, address, access, pin);
    }
    if (status == KERNEL_MM_STATUS_OK) return KERNEL_UACCESS_STATUS_OK;
    if (status == KERNEL_MM_STATUS_NOT_MAPPED || status == KERNEL_MM_STATUS_ACCESS ||
        status == KERNEL_MM_STATUS_ADDRESS_SPACE || status == KERNEL_MM_STATUS_BUS_FAULT ||
        status == KERNEL_MM_STATUS_NO_MEMORY) return KERNEL_UACCESS_STATUS_FAULT;
    return KERNEL_UACCESS_STATUS_STATE;
}


void kernel_uaccess_write_cursor_finish(struct kernel_uaccess_write_cursor *cursor)
{
    if (cursor->pin.registration) kernel_mm_unpin_user_page(&cursor->pin);
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
            status = kernel_uaccess_pin_user_page(mm, current,
                                                 KERNEL_MM_WRITE, &cursor->pin);
            if (status != KERNEL_UACCESS_STATUS_OK) {
                *bytes_copied = copied;
                return status;
            }
            cursor->virtual_page = virtual_page;
        }
#if BOAROS_COST_DIAGNOSTICS
        struct kernel_cost_tag memory_tag = kernel_cost_capture();
        uint64_t memory_start = kernel_cost_clock();
#endif
        page = cursor->pin.page;
        memcpy(page + offset, source + copied, chunk);
#if BOAROS_COST_DIAGNOSTICS
        uint64_t memory_elapsed = kernel_cost_clock() - memory_start;
        kernel_cost_add_tag(memory_tag, COST_UACCESS_COPY_TICKS, memory_elapsed);
        kernel_cost_add_tag(memory_tag, COST_UACCESS_COPY_BYTES, chunk);
#endif
        COST_ADD(COPY_TO_USER, chunk);
        copied += chunk;
    }

    *bytes_copied = copied;
    return KERNEL_UACCESS_STATUS_OK;
}

enum kernel_uaccess_status kernel_copy_to_user(
    struct kernel_mm *mm, uint64_t user_destination, const void *kernel_source,
    size_t size, size_t *bytes_copied)
{
    struct kernel_uaccess_write_cursor cursor = {0};
    enum kernel_uaccess_status status = kernel_uaccess_write_cursor_copy(
        mm, &cursor, user_destination, kernel_source, size, bytes_copied);
    kernel_uaccess_write_cursor_finish(&cursor);
    return status;
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
        status = kernel_uaccess_pin_user_page(mm,
                                   current,
                                   KERNEL_MM_READ,
                                   &pin);
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
        status = kernel_uaccess_pin_user_page(mm,
                                   current,
                                   KERNEL_MM_READ,
                                   &pin);
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
