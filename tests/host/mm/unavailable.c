/* 匿名MM模型链接真实memory_object，不执行文件/SysV或进程业务。意外进入时立即失败，
 * 这些入口不是成功存根，也不作为对应子系统的验收证据。 */
#include <stdint.h>
struct kernel_task;
void mm_test_unavailable(void);
uint64_t kernel_task_current_stack_limit(void) { return 0; }
#define EXCLUDED(name) void name(void) { mm_test_unavailable(); }
EXCLUDED(kernel_shm_attachment_close)
EXCLUDED(kernel_shm_attachment_open)
EXCLUDED(kernel_elf64_source_release)
EXCLUDED(kernel_elf64_source_acquire)
EXCLUDED(kernel_elf64_source_page)
EXCLUDED(kernel_open_file_node)
EXCLUDED(kernel_file_mapping_unregister)
EXCLUDED(kernel_open_file_release)
EXCLUDED(kernel_page_cache_alias_detach)
#if !defined(BOAROS_MAX_CPUS) || BOAROS_MAX_CPUS == 1
EXCLUDED(kernel_task_mm_borrow)
#endif
#if !BOAROS_USER_SMP
EXCLUDED(kernel_proc_task_block_reads)
EXCLUDED(kernel_proc_task_note_fault)
#endif
EXCLUDED(kernel_vfs_node_lock)
EXCLUDED(kernel_vfs_node_try_read)
EXCLUDED(kernel_open_file_size)
EXCLUDED(kernel_open_file_memory_backed)
EXCLUDED(kernel_open_file_get_page_for_fault)
EXCLUDED(kernel_open_file_memory_modified)
EXCLUDED(kernel_open_file_discard_new_page)
EXCLUDED(kernel_open_file_get_page)
EXCLUDED(kernel_open_file_lookup_page)
EXCLUDED(kernel_open_file_pread)
EXCLUDED(kernel_open_file_alias_attach)
EXCLUDED(kernel_page_cache_alias_mark_dirty)
EXCLUDED(kernel_open_file_acquire)
EXCLUDED(kernel_page_cache_alias_clone)
EXCLUDED(kernel_file_mapping_register)
EXCLUDED(kernel_open_file_sync_range)
EXCLUDED(kernel_open_file_prepare_memory_page)
EXCLUDED(kernel_open_file_finish_memory_page)
EXCLUDED(kernel_file_mapping_epoch)
EXCLUDED(kernel_file_mapping_wait)
EXCLUDED(kernel_file_mapping_stop)
EXCLUDED(kernel_file_mapping_drain)
EXCLUDED(kernel_tmpfs_check_domain)
EXCLUDED(kernel_shm_attachment_acquire)
struct kernel_shm_attachment;
void kernel_shm_attachment_release(struct kernel_shm_attachment **owner)
{ if (owner && *owner) mm_test_unavailable(); }

EXCLUDED(kernel_file_mapping_prepare)

EXCLUDED(kernel_open_file_vfs_borrow)
EXCLUDED(kernel_open_file_page_cache)
EXCLUDED(kernel_open_file_prepare_mapping_page)
EXCLUDED(kernel_open_file_finish_mapping_page)
EXCLUDED(kernel_open_file_check_mapping_domain)
EXCLUDED(kernel_page_cache_alias_drain)
EXCLUDED(kernel_page_cache_alias_write_begin)
EXCLUDED(kernel_page_cache_write_end)
EXCLUDED(kernel_page_cache_wait_writable)
EXCLUDED(kernel_page_cache_alias_hold)
EXCLUDED(kernel_page_cache_alias_put)
EXCLUDED(kernel_page_cache_retry_alias)

EXCLUDED(kernel_page_cache_prepare_mapping_write)
