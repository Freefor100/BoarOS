/* tmpfs文件模型未开放的磁盘/设备/进程业务；意外进入即失败。
 * 不替代VFS、OFD、tmpfs、对象、登记或MM实现。 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <kernel/cpu.h>
uint64_t kernel_time_realtime_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) abort();
    return (uint64_t)now.tv_sec * 1000000000U + now.tv_nsec;
}
void kernel_console_putc(char c) { fputc(c, stderr); }
void mm_file_unavailable(void) { fputs("excluded disk/device/process business reached\n", stderr); abort(); }
__attribute__((weak)) uint64_t kernel_task_current_stack_limit(void) { return UINT64_MAX; }
#include <kernel/rlimit.h>
__attribute__((weak)) void kernel_task_current_memory_limits(struct kernel_memory_limits *out)
{ *out = (struct kernel_memory_limits){UINT64_MAX, UINT64_MAX, UINT64_MAX}; }
#define EXCLUDED(name) void name(void) { mm_file_unavailable(); }
EXCLUDED(kernel_shm_attachment_close)
EXCLUDED(kernel_shm_attachment_open)
EXCLUDED(kernel_shm_attachment_acquire)
struct kernel_shm_attachment;
void kernel_shm_attachment_release(struct kernel_shm_attachment **owner)
{ if (owner && *owner) mm_file_unavailable(); }
#ifndef MM_TEST_ELF_SOURCE
EXCLUDED(kernel_elf64_source_release)
EXCLUDED(kernel_elf64_source_acquire)
EXCLUDED(kernel_elf64_source_page)
EXCLUDED(kernel_elf64_source_file)
EXCLUDED(kernel_elf64_source_run_count)
EXCLUDED(kernel_elf64_source_run_at)
#endif
#if BOAROS_MAX_CPUS == 1
EXCLUDED(kernel_task_mm_borrow)
#endif
EXCLUDED(kernel_task_read_bytes)
EXCLUDED(kernel_task_account_fault)
#ifndef MM_TEST_EPOLL
EXCLUDED(kernel_epoll_notify_file_release)
EXCLUDED(kernel_epoll_destroy)
#endif
EXCLUDED(kernel_socket_destroy)
EXCLUDED(kernel_socket_is_unix)
EXCLUDED(kernel_socket_rights_changed)
EXCLUDED(kernel_pidfd_close)
EXCLUDED(kernel_pipe_release_endpoint)
EXCLUDED(kernel_pipe_destroy)
EXCLUDED(kernel_page_cache_truncate)
EXCLUDED(kernel_page_cache_writeback_range_borrowing)
EXCLUDED(kernel_page_cache_get)
EXCLUDED(kernel_page_cache_get_batch)
EXCLUDED(kernel_page_cache_lookup)
EXCLUDED(kernel_page_cache_alias_attach)
EXCLUDED(kernel_page_cache_alias_detach)
EXCLUDED(kernel_page_cache_alias_clone)
EXCLUDED(kernel_page_cache_alias_mark_dirty)
EXCLUDED(kernel_page_cache_alias_drain)
EXCLUDED(kernel_page_cache_alias_write_begin)
EXCLUDED(kernel_page_cache_write_end)
EXCLUDED(kernel_page_cache_wait_writable)
EXCLUDED(kernel_page_cache_alias_hold)
EXCLUDED(kernel_page_cache_alias_put)
EXCLUDED(kernel_page_cache_retry_alias)

EXCLUDED(kernel_page_cache_write_source)

EXCLUDED(kernel_page_cache_resize_prepare)
EXCLUDED(kernel_page_cache_resize_finish)

EXCLUDED(kernel_page_cache_needs_cpu0)
EXCLUDED(kernel_page_cache_run_cpu0)

EXCLUDED(kernel_page_cache_prepare_mapping_write)
/* This MM fixture never constructs eventfds; an accidental close is a failure. */
void kernel_event_fd_close(void *context) { (void)context; mm_file_unavailable(); }
EXCLUDED(kernel_page_cache_cancel_readahead)
EXCLUDED(kernel_page_cache_accept_mapping_write)
EXCLUDED(kernel_page_cache_finish_mapping_write)
EXCLUDED(kernel_page_cache_writeback)
