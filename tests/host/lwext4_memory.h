#ifndef BOAROS_TEST_LWEXT4_MEMORY_H
#define BOAROS_TEST_LWEXT4_MEMORY_H
#include <stddef.h>
void *ext4_user_malloc(size_t size);
void *ext4_user_calloc(size_t count, size_t size);
void *ext4_user_realloc(void *pointer, size_t size);
void ext4_user_free(void *pointer);
#endif
