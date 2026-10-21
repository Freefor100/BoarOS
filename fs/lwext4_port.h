#ifndef BOAROS_FS_LWEXT4_PORT_H
#define BOAROS_FS_LWEXT4_PORT_H

#include <kernel/heap.h>
#include <kernel/sync.h>

struct ext4_lock;
struct ext4_fs;
struct boaros_lwext4_resource;
struct boaros_lwext4_resource_tree {
    struct boaros_lwext4_resource *rbh_root;
};
struct boaros_lwext4_domain {
    struct kernel_rwlock serial;
    struct kernel_mutex state;
    struct kernel_raw_lock resource_index;
    struct boaros_lwext4_resource_tree resources;
    struct kernel_heap *heap;
    struct ext4_fs *fs;
    unsigned parallel;
};
void boaros_lwext4_lock_init(struct ext4_lock *callbacks,
    struct boaros_lwext4_domain *domain, struct kernel_heap *heap);
void boaros_lwext4_lock_fini(struct boaros_lwext4_domain *domain);
/* A live backend inode retains the shared object, not its mutex qualification. */
int boaros_lwext4_inode_get(struct boaros_lwext4_domain *, uint32_t,
    struct boaros_lwext4_resource **);
void boaros_lwext4_inode_put(struct boaros_lwext4_domain *,
    struct boaros_lwext4_resource **);
/* Caller holds startup namespace qualification; workers await publication.
 * Drains old backend owners before publishing the parallel domain. */
void boaros_lwext4_enable_parallel(struct boaros_lwext4_domain *domain, struct ext4_fs *fs);
void boaros_lwext4_state_lock(void *domain);
void boaros_lwext4_state_read_lock(void *domain);
void boaros_lwext4_state_unlock(void *domain);
void boaros_lwext4_wait(void *key);
void boaros_lwext4_wake(void *key);
int boaros_lwext4_read_context(void);
unsigned boaros_lwext4_pause(void);
void boaros_lwext4_resume(void *lock, unsigned depth);
void boaros_lwext4_unlock(void *lock);

int boaros_lwext4_heap_bind(struct kernel_heap *heap);
void boaros_lwext4_heap_unbind(struct kernel_heap *heap);
int boaros_lwext4_allocation_active(void);

#endif
