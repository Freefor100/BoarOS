#include "lwext4_port.h"

#include <kernel/heap.h>

#include <stddef.h>

static struct kernel_heap *lwext4_heap;

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

    if (lwext4_heap == 0 ||
        kernel_heap_allocate(lwext4_heap, size, &pointer) !=
            KERNEL_HEAP_STATUS_OK) {
        return 0;
    }
    return pointer;
}

void *ext4_user_calloc(size_t count, size_t size)
{
    void *pointer = 0;

    if (lwext4_heap == 0 ||
        kernel_heap_allocate_zeroed(lwext4_heap,
                                    count,
                                    size,
                                    &pointer) != KERNEL_HEAP_STATUS_OK) {
        return 0;
    }
    return pointer;
}

void *ext4_user_realloc(void *pointer, size_t size)
{
    void *result = 0;

    if (lwext4_heap == 0 ||
        kernel_heap_resize(lwext4_heap, pointer, size, &result) !=
            KERNEL_HEAP_STATUS_OK) {
        return 0;
    }
    return result;
}

void ext4_user_free(void *pointer)
{
    if (lwext4_heap != 0) {
        (void)kernel_heap_release(lwext4_heap, pointer);
    }
}
