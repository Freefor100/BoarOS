#ifndef BOAROS_KERNEL_EPOLL_H
#define BOAROS_KERNEL_EPOLL_H

struct kernel_epoll_wait_request;
/* A suspended wait owns its scan and OFD pins until return or forced exit. */
void kernel_epoll_abort_wait(struct kernel_epoll_wait_request *request);

#endif
