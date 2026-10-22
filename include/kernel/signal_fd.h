#ifndef BOAROS_KERNEL_SIGNAL_FD_H
#define BOAROS_KERNEL_SIGNAL_FD_H
#include <kernel/files.h>
struct kernel_open_file_description;
struct kernel_wait_queue;
struct kernel_task;
enum kernel_files_status kernel_files_signalfd(struct kernel_files *,struct kernel_task *,int32_t,uint64_t,uint32_t,int64_t *);
int64_t kernel_signal_fd_read(struct kernel_open_file_description *,struct kernel_mm *,const struct kernel_uaccess_iovec *,size_t,uint64_t);
uint32_t kernel_signal_fd_poll(struct kernel_open_file_description *,struct kernel_wait_queue **);
#endif
