#ifndef BOAROS_KERNEL_PROCESS_ACCT_H
#define BOAROS_KERNEL_PROCESS_ACCT_H

#include <stdint.h>

struct kernel_lock_guard;
struct kernel_task;
struct kernel_vfs_mount;

/* These mount hooks pair the accounting-file lifetime with the real view.
 * Standalone VFS tests need not link the scheduler accounting consumer. */
void kernel_process_acct_lock(struct kernel_lock_guard *);
int kernel_process_acct_has_mount(const struct kernel_vfs_mount *);
void kernel_process_acct_unmount(struct kernel_vfs_mount *);
void kernel_process_acct_shutdown(void);

int kernel_process_acct_set(struct kernel_task *, uint64_t user_name);
int kernel_process_acct_enabled(void);
void kernel_process_acct_collect_memory(struct kernel_task *);
void kernel_process_acct_exit(struct kernel_task *);

#endif
