#include "lwext4_port.h"

#include <kernel/heap.h>

#include <stddef.h>

static struct kernel_heap *lwext4_heap;
static unsigned int allocation_depth;

int boaros_lwext4_allocation_active(void)
{
    return allocation_depth != 0U;
}

int boaros_lwext4_heap_bind(struct kernel_heap *heap)
{
    if (heap == 0 || (lwext4_heap != 0 && lwext4_heap != heap)) {
        return 0;
    }

    lwext4_heap = heap;
    return 1;
}

void boaros_lwext4_heap_unbind(struct kernel_heap *heap)
{
    if (lwext4_heap == heap) {
        lwext4_heap = 0;
    }
}

void *ext4_user_malloc(size_t size)
{
    void *pointer = 0;
    if (lwext4_heap == 0) return 0;
    allocation_depth++;
    enum kernel_heap_status status = kernel_heap_allocate(lwext4_heap, size, &pointer);
    allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? pointer : 0;
}

void *ext4_user_calloc(size_t count, size_t size)
{
    void *pointer = 0;

    if (lwext4_heap == 0) return 0;
    allocation_depth++;
    enum kernel_heap_status status = kernel_heap_allocate_zeroed(lwext4_heap,
                                    count,
                                    size,
                                    &pointer);
    allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? pointer : 0;
}

void *ext4_user_realloc(void *pointer, size_t size)
{
    void *result = 0;

    if (lwext4_heap == 0) return 0;
    allocation_depth++;
    enum kernel_heap_status status = kernel_heap_resize(lwext4_heap, pointer, size, &result);
    allocation_depth--;
    return status == KERNEL_HEAP_STATUS_OK ? result : 0;
}

void ext4_user_free(void *pointer)
{
    if (lwext4_heap != 0) {
        (void)kernel_heap_release(lwext4_heap, pointer);
    }
}
