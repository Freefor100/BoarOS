#ifndef BOAROS_KERNEL_PIDFD_H
#define BOAROS_KERNEL_PIDFD_H
#include <stdint.h>
struct kernel_task;
struct kernel_pid;
struct kernel_files;
struct kernel_open_file_description;
struct kernel_signal_wait_info;
struct kernel_wait_queue;
int64_t kernel_pidfd_open(struct kernel_task *, int32_t, uint32_t);
int64_t kernel_pidfd_getfd(struct kernel_task *, int32_t, int32_t, uint32_t);
int kernel_pidfd_signal_target(struct kernel_task *,int32_t,uint32_t,struct kernel_pid **,unsigned *);
int64_t kernel_pidfd_send(struct kernel_task *, struct kernel_pid *,unsigned,uint32_t,
                          const struct kernel_signal_wait_info *);
int kernel_pidfd_wait_target(struct kernel_files *, int32_t, struct kernel_pid **, int *);
uint32_t kernel_pidfd_poll(struct kernel_open_file_description *, struct kernel_wait_queue **);
int32_t kernel_pidfd_number(const struct kernel_open_file_description *);
uint64_t kernel_pidfd_inode(const struct kernel_open_file_description *);
void kernel_pidfd_close(struct kernel_open_file_description *);
/* Caller owns the stopped task; capture the identity before role changes,
 * then notify outside the scheduler lock before returning from cleanup. */
struct kernel_pid *kernel_pidfd_exit_identity(struct kernel_task *);
void kernel_pidfd_notification_end(struct kernel_pid **);
int64_t kernel_signal_send_identity(struct kernel_pid *, unsigned,
    uint32_t, const struct kernel_signal_wait_info *);
#endif
