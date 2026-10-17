/* 未开放消费者只可拒绝；真正触达即abort，不能返回成功替代业务。socket的
 * NULL类型查询链接生产net/socket.c，普通文件走其真实false分支。 */
#include <stdlib.h>
#include <stdio.h>
#define EXCLUDED(name) void __wrap_##name(void) { fputs("unopened file I/O consumer: " #name "\n",stderr); abort(); }
EXCLUDED(kernel_socket_destroy)
EXCLUDED(kernel_socket_pair)
EXCLUDED(kernel_socket_create)
EXCLUDED(kernel_socket_accept)
EXCLUDED(kernel_epoll_create)
EXCLUDED(kernel_socket_copy_read)
#ifndef TCP_IO_CASES
EXCLUDED(kernel_socket_read_user)
#endif
EXCLUDED(kernel_socket_discard_receive)
#ifndef TCP_IO_CASES
EXCLUDED(kernel_socket_finish_read)
#endif
EXCLUDED(kernel_socket_poll)
EXCLUDED(kernel_socket_read_info)
EXCLUDED(kernel_socket_receive_ready)
#ifndef TCP_IO_CASES
EXCLUDED(kernel_socket_receive_timeout)
EXCLUDED(kernel_socket_reserve_read)
EXCLUDED(kernel_socket_send_timeout)
#endif
EXCLUDED(kernel_socket_stream_begin)
EXCLUDED(kernel_socket_stream_commit)
EXCLUDED(kernel_socket_stream_finish)
#ifndef TCP_IO_CASES
EXCLUDED(kernel_socket_stream_reserve)
#endif
EXCLUDED(kernel_socket_unix_wait_error)
EXCLUDED(kernel_socket_wait_queue)
EXCLUDED(kernel_socket_write_buffer)
#ifndef TCP_IO_CASES
EXCLUDED(kernel_socket_write_unix)
#endif
EXCLUDED(kernel_socket_write_datagram)
EXCLUDED(kernel_socket_write_datagram_buffer)
EXCLUDED(kernel_signal_send_task)
EXCLUDED(kernel_signal_note_syscall_restart)
EXCLUDED(kernel_signal_clear_syscall_restart)
EXCLUDED(kernel_signal_note_futex_timed_restart)
EXCLUDED(kernel_time_boot_realtime_offset)
EXCLUDED(kernel_signal_has_pending)
EXCLUDED(kernel_time_deadline_from_monotonic)
