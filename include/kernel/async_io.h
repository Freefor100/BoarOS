#ifndef BOAROS_KERNEL_ASYNC_IO_H
#define BOAROS_KERNEL_ASYNC_IO_H
#include <stdint.h>

/* Linux positive si_code values used by signal-driven I/O. These events have
 * their own source; ordinary poll wakeups are not asynchronous I/O events. */
#define KERNEL_ASYNC_POLL_IN 1U
#define KERNEL_ASYNC_POLL_OUT 2U
#define KERNEL_ASYNC_POLL_MSG 3U
#define KERNEL_ASYNC_POLL_ERR 4U
#define KERNEL_ASYNC_POLL_PRI 5U
#define KERNEL_ASYNC_POLL_HUP 6U
#define KERNEL_ASYNC_INVALIDATE UINT64_MAX

struct kernel_open_file_description;
struct kernel_mm;
int kernel_open_file_async_flags(struct kernel_open_file_description *,int fd,uint32_t flags);
int64_t kernel_open_file_async_fcntl(struct kernel_open_file_description *,unsigned command,
    uint64_t argument,struct kernel_mm *);
struct kernel_file_lease;
struct kernel_file_lease *kernel_open_file_async_lease_prepare(struct kernel_open_file_description *);
void kernel_open_file_async_close(struct kernel_open_file_description *);
#endif
