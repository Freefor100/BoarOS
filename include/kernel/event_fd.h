#ifndef BOAROS_KERNEL_EVENT_FD_H
#define BOAROS_KERNEL_EVENT_FD_H
#include <kernel/files.h>
#include <stdint.h>
struct kernel_open_file_description;
struct kernel_wait_queue;
struct kernel_mm;
/* FD installation consumes the newly allocated OFD only after publication. */
enum kernel_files_status kernel_files_eventfd_create(struct kernel_files *, uint32_t, uint32_t, int64_t *);
int64_t kernel_event_fd_io(struct kernel_open_file_description *, struct kernel_mm *,
    const struct kernel_uaccess_iovec *, size_t, uint64_t, int);
uint32_t kernel_event_fd_poll(struct kernel_open_file_description *, struct kernel_wait_queue **);
void kernel_event_fd_close(struct kernel_open_file_description *);
#endif
