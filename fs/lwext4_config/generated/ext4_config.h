#ifndef BOAROS_LWEXT4_GENERATED_CONFIG_H
#define BOAROS_LWEXT4_GENERATED_CONFIG_H

#include <stddef.h>

#define CONFIG_JOURNALING_ENABLE 1
#define CONFIG_XATTR_ENABLE 0
#define CONFIG_HAVE_OWN_ERRNO 1
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0
#define CONFIG_HAVE_OWN_ASSERT 1
#define CONFIG_BLOCK_DEV_ENABLE_STATS 1
#define CONFIG_BLOCK_DEV_CACHE_SIZE 8
#define CONFIG_EXT4_BLOCKDEVS_COUNT 1
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 1
#define CONFIG_HAVE_OWN_OFLAGS 1
#define CONFIG_UNALIGNED_ACCESS 0
#define CONFIG_USE_USER_MALLOC 1

void *ext4_user_malloc(size_t size);
void *ext4_user_calloc(size_t count, size_t size);
void *ext4_user_realloc(void *pointer, size_t size);
void ext4_user_free(void *pointer);

#endif
