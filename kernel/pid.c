#include <kernel/pid.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_PID_ALLOCATOR_INITIALIZED UINT32_C(0x50494441)

static int valid_allocator(const struct kernel_pid_allocator *allocator)
{
    return allocator != 0 &&
           allocator->initialized == KERNEL_PID_ALLOCATOR_INITIALIZED &&
           allocator->bitmap != 0 && allocator->limit != 0U &&
           allocator->limit <= (uint32_t)INT32_MAX &&
           allocator->cursor < allocator->limit &&
           allocator->allocated <= allocator->limit;
}

static uint64_t pid_mask(uint32_t index)
{
    return UINT64_C(1) << (index & 63U);
}

enum kernel_pid_status kernel_pid_allocator_init(
    struct kernel_pid_allocator *allocator,
    uint64_t *bitmap,
    uint32_t limit)
{
    uint32_t index;

    if (allocator == 0 || bitmap == 0 || limit == 0U ||
        limit > (uint32_t)INT32_MAX) {
        return KERNEL_PID_STATUS_INVALID;
    }
    if (allocator->initialized != 0U || allocator->bitmap != 0 ||
        allocator->limit != 0U || allocator->cursor != 0U ||
        allocator->allocated != 0U) {
        return KERNEL_PID_STATUS_STATE;
    }
    for (index = 0U; index < KERNEL_PID_BITMAP_WORDS(limit); index++) {
        bitmap[index] = 0U;
    }
    allocator->bitmap = bitmap;
    allocator->limit = limit;
    allocator->cursor = 0U;
    allocator->allocated = 0U;
    allocator->initialized = KERNEL_PID_ALLOCATOR_INITIALIZED;
    return KERNEL_PID_STATUS_OK;
}

enum kernel_pid_status kernel_pid_allocate(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t *pid)
{
    uint32_t scanned;

    if (pid == 0) {
        return KERNEL_PID_STATUS_INVALID;
    }
    if (!valid_allocator(allocator)) {
        return KERNEL_PID_STATUS_STATE;
    }
    if (allocator->allocated == allocator->limit) {
        return KERNEL_PID_STATUS_EXHAUSTED;
    }
    for (scanned = 0U; scanned < allocator->limit; scanned++) {
        uint32_t index = allocator->cursor + scanned;
        uint32_t word;
        uint64_t mask;

        if (index >= allocator->limit) {
            index -= allocator->limit;
        }
        word = index >> 6U;
        mask = pid_mask(index);
        if ((allocator->bitmap[word] & mask) != 0U) {
            continue;
        }
        allocator->bitmap[word] |= mask;
        allocator->allocated++;
        allocator->cursor = index + 1U;
        if (allocator->cursor == allocator->limit) {
            allocator->cursor = 0U;
        }
        *pid = (kernel_pid_t)(index + 1U);
        return KERNEL_PID_STATUS_OK;
    }
    return KERNEL_PID_STATUS_STATE;
}

enum kernel_pid_status kernel_pid_release(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t pid)
{
    uint32_t index;
    uint32_t word;
    uint64_t mask;

    if (pid <= 0) {
        return KERNEL_PID_STATUS_INVALID;
    }
    if (!valid_allocator(allocator)) {
        return KERNEL_PID_STATUS_STATE;
    }
    index = (uint32_t)pid - 1U;
    if (index >= allocator->limit) {
        return KERNEL_PID_STATUS_INVALID;
    }
    word = index >> 6U;
    mask = pid_mask(index);
    if ((allocator->bitmap[word] & mask) == 0U) {
        return KERNEL_PID_STATUS_NOT_ALLOCATED;
    }
    if (allocator->allocated == 0U) {
        return KERNEL_PID_STATUS_STATE;
    }
    allocator->bitmap[word] &= ~mask;
    allocator->allocated--;
    if (index < allocator->cursor) {
        allocator->cursor = index;
    }
    return KERNEL_PID_STATUS_OK;
}
