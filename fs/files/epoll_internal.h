#ifndef BOAROS_FS_FILES_EPOLL_INTERNAL_H
#define BOAROS_FS_FILES_EPOLL_INTERNAL_H

#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>

#include <stddef.h>
#include <stdint.h>

struct kernel_heap;
struct kernel_open_file_description;
struct kernel_files;
struct kernel_mm;
struct kernel_task;

#define KERNEL_EPOLL_CLOEXEC 0x80000U

#define KERNEL_EPOLL_CTL_ADD 1
#define KERNEL_EPOLL_CTL_DEL 2
#define KERNEL_EPOLL_CTL_MOD 3

#define KERNEL_EPOLLIN 0x0001U
#define KERNEL_EPOLLPRI 0x0002U
#define KERNEL_EPOLLOUT 0x0004U
#define KERNEL_EPOLLERR 0x0008U
#define KERNEL_EPOLLHUP 0x0010U
#define KERNEL_EPOLLNVAL 0x0020U
#define KERNEL_EPOLLRDNORM 0x0040U
#define KERNEL_EPOLLRDBAND 0x0080U
#define KERNEL_EPOLLWRNORM 0x0100U
#define KERNEL_EPOLLWRBAND 0x0200U
#define KERNEL_EPOLLMSG 0x0400U
#define KERNEL_EPOLLRDHUP 0x2000U
#define KERNEL_EPOLLEXCLUSIVE (1U << 28)
#define KERNEL_EPOLLWAKEUP (1U << 29)
#define KERNEL_EPOLLONESHOT (1U << 30)
#define KERNEL_EPOLLET (1U << 31)

struct linux_epoll_event {
    uint32_t events;
    uint32_t _pad;
    uint64_t data;
};

_Static_assert(sizeof(struct linux_epoll_event) == 16U,
               "Linux struct epoll_event ABI size must remain 16 bytes");
_Static_assert(offsetof(struct linux_epoll_event, events) == 0U,
               "Linux epoll_event.events must be at offset 0");
_Static_assert(offsetof(struct linux_epoll_event, data) == 8U,
               "Linux epoll_event.data must be at offset 8");

struct kernel_epoll;

struct kernel_epoll_item {
    int target_fd;
    struct kernel_open_file_description *target_file;
    struct kernel_epoll *epoll;
    uint32_t events;
    uint64_t data;
    struct kernel_wait_node wait_node;

    struct kernel_epoll_item *items_next;
    struct kernel_epoll_item *items_prev;

    struct kernel_epoll_item *ready_next;

    struct kernel_epoll_item *target_next;
    struct kernel_epoll_item *target_prev;

    uint8_t on_ready_list;
    uint8_t oneshot_disarmed;
};

struct kernel_epoll {
    struct kernel_heap *heap;
    struct kernel_wait_queue wait_queue;
    struct kernel_open_file_description *file;
    struct kernel_epoll_item *items_head;
    struct kernel_epoll_item *ready_head;
    struct kernel_epoll_item *ready_tail;
    uint32_t item_count;
};

int kernel_epoll_create(struct kernel_heap *heap,
                        struct kernel_epoll **out_epoll);

enum kernel_files_status kernel_epoll_destroy(struct kernel_epoll *epoll);

void kernel_epoll_notify_file_release(
    struct kernel_open_file_description *file);

uint32_t kernel_epoll_poll(
    struct kernel_epoll *epoll,
    uint32_t requested_events,
    struct kernel_wait_queue **out_queue);

#endif
