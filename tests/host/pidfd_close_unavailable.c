/* Unopened consumers must stay unreachable in the focused PIDFD OFD case. */
#include <stdio.h>
#include <stdlib.h>
static void unopened(void){fputs("unopened pidfd-close consumer reached\n",stderr);abort();}
#define EXCLUDED(name) void name(void){unopened();}
EXCLUDED(kernel_socket_rights_changed)
EXCLUDED(kernel_page_cache_run_cpu0)
EXCLUDED(kernel_record_lock_release)
EXCLUDED(kernel_vfs_node_record_locks)
EXCLUDED(kernel_vfs_path_release)
EXCLUDED(kernel_pipe_release_endpoint)
EXCLUDED(kernel_vfs_close)
EXCLUDED(kernel_event_fd_close)
EXCLUDED(kernel_epoll_destroy)
EXCLUDED(kernel_socket_destroy)
EXCLUDED(kernel_signal_fd_poll)
EXCLUDED(kernel_event_fd_poll)
EXCLUDED(kernel_pipe_poll)
EXCLUDED(kernel_epoll_poll)
EXCLUDED(kernel_socket_poll)
