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
uint64_t kernel_task_current_stack_limit(void) { return 0; }
#define EXCLUDED(name) void name(void) { mm_file_unavailable(); }
EXCLUDED(kernel_shm_attachment_close)
EXCLUDED(kernel_shm_attachment_open)
EXCLUDED(kernel_shm_attachment_acquire)
EXCLUDED(kernel_shm_attachment_release)
EXCLUDED(kernel_elf64_source_release)
EXCLUDED(kernel_elf64_source_acquire)
EXCLUDED(kernel_elf64_source_page)
#if BOAROS_MAX_CPUS == 1
EXCLUDED(kernel_task_mm_borrow)
#endif
EXCLUDED(kernel_proc_task_block_reads)
EXCLUDED(kernel_proc_task_note_fault)
EXCLUDED(kernel_epoll_notify_file_release)
EXCLUDED(kernel_epoll_destroy)
EXCLUDED(kernel_socket_destroy)
EXCLUDED(kernel_pipe_release_endpoint)
EXCLUDED(kernel_pipe_destroy)
EXCLUDED(kernel_page_cache_extend)
EXCLUDED(kernel_page_cache_truncate)
EXCLUDED(kernel_page_cache_get)
EXCLUDED(kernel_page_cache_get_batch)
EXCLUDED(kernel_page_cache_writeback_before)
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
EXCLUDED(kernel_page_cache_is_controlled)
EXCLUDED(kernel_ext4_check_mapping_domain)

EXCLUDED(kernel_page_cache_write_source)

EXCLUDED(kernel_page_cache_resize_prepare)
EXCLUDED(kernel_page_cache_resize_finish)
