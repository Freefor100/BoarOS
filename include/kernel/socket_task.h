#ifndef BOAROS_KERNEL_SOCKET_TASK_H
#define BOAROS_KERNEL_SOCKET_TASK_H

#include <stdint.h>
struct kernel_pid;

/* pid 0 owns the caller's TGID; explicit numbers own any published PID identity,
 * including TIDs. Missing identities return -ESRCH and clear owner. Release it
 * with kernel_task_identity_put, outside raw locks. */
int kernel_task_socket_identity(int32_t pid, struct kernel_pid **owner);
void kernel_task_identity_put(struct kernel_pid *identity);

#endif
