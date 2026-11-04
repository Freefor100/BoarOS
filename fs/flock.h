#ifndef BOAROS_FS_FLOCK_H
#define BOAROS_FS_FLOCK_H

struct kernel_heap;
struct kernel_flock_context;
/* The backing inode owns this lazy context until its last real reference. */
struct kernel_flock_identity { struct kernel_flock_context *context; };
struct kernel_flock_owner {
    struct kernel_flock_context *context;
    unsigned type;
};

int kernel_flock_apply(struct kernel_flock_identity *, struct kernel_flock_owner *,
    struct kernel_heap *, unsigned operation);
void kernel_flock_release(struct kernel_flock_owner *);
void kernel_flock_destroy(struct kernel_flock_identity *);
struct kernel_flock_identity *kernel_flock_anonymous_identity(void);
struct kernel_flock_identity *kernel_flock_bootstrap_console_identity(void);

#endif
