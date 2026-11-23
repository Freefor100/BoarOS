#ifndef BOAROS_FS_LEASE_H
#define BOAROS_FS_LEASE_H

#include <stdbool.h>
#include <stdint.h>

struct kernel_vfs_file;
struct kernel_open_file_description;

/* Linux fcntl lease types. */
enum kernel_lease_type {
    KERNEL_LEASE_READ, KERNEL_LEASE_WRITE, KERNEL_LEASE_UNLOCK,
};

struct kernel_lease_notify {
    void (*install)(struct kernel_open_file_description *, int fd);
    void (*clear)(struct kernel_open_file_description *);
    void (*signal)(struct kernel_open_file_description *);
};

/* The open file description owns storage; the inode lease lock owns linkage
 * and break state while the record is active. */
struct kernel_file_lease {
    struct kernel_file_lease *next;
    struct kernel_vfs_file *file;
    struct kernel_open_file_description *description;
    const struct kernel_lease_notify *notify;
    uint64_t downgrade_deadline, unlock_deadline;
    enum kernel_lease_type type;
    bool downgrade_pending, unlock_pending;
};

int kernel_vfs_lease_open(struct kernel_vfs_file *, int mode, int nonblock);
int kernel_vfs_open_truncate(struct kernel_vfs_file *);
int kernel_vfs_lease_get(struct kernel_vfs_file *);
int kernel_vfs_lease_set(struct kernel_vfs_file *, enum kernel_lease_type, int fd,
    struct kernel_file_lease *);
void kernel_vfs_lease_release(struct kernel_vfs_file *);
int kernel_vfs_lease_break_time(void);
void kernel_vfs_set_lease_break_time(int seconds);

#endif
