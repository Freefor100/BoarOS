#ifndef BOAROS_KERNEL_EPOLL_H
#define BOAROS_KERNEL_EPOLL_H

struct kernel_epoll_wait_request;
struct kernel_open_file_description;
struct kernel_wait_queue;
/* 已pin的epoll OFD：仅决定是否先finish再刷新，不能作为最终revents。 */
int kernel_epoll_wait_pending(struct kernel_open_file_description *, struct kernel_wait_queue **);
/* A suspended wait owns its scan and OFD pins until return or forced exit. */
void kernel_epoll_abort_wait(struct kernel_epoll_wait_request *request);

#endif
