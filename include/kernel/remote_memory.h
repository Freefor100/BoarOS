#ifndef BOAROS_KERNEL_REMOTE_MEMORY_H
#define BOAROS_KERNEL_REMOTE_MEMORY_H
struct kernel_task;
/* Release a process_vm request's MM, vectors and scratch. On real MM close
 * failure returns -EIO and leaves the heap-owned request for the next retry;
 * both normal completion and task exit use this same owner. */
int kernel_remote_memory_abort(struct kernel_task *);
#endif
