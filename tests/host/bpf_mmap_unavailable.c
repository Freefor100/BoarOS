/* This fixture exercises actual BPF maps/OFDs/MM, not perf attachment, disk
 * cache mutation or task lookup. Accidentally entering those paths fails. */
void mm_file_unavailable(void);
#define EXCLUDED(name) void name(void) { mm_file_unavailable(); }
EXCLUDED(kernel_page_cache_run_cpu0_borrowing)
EXCLUDED(kernel_page_cache_invalidate_range)
EXCLUDED(kernel_files_hold_fd)
EXCLUDED(kernel_files_pin_guard_release)
EXCLUDED(kernel_task_lookup_put)
EXCLUDED(kernel_task_mm_borrow_mutable)
int kernel_scheduler_can_sleep(void) { return 1; }
int riscv_plic_in_interrupt(void) { return 0; }
EXCLUDED(kernel_elf64_source_program_header)
EXCLUDED(kernel_elf64_source_program_header_count)
EXCLUDED(kernel_page_cache_resident)
EXCLUDED(kernel_event_fd_poll)
EXCLUDED(kernel_pidfd_poll)
EXCLUDED(kernel_signal_fd_poll)
EXCLUDED(kernel_socket_poll)
EXCLUDED(kernel_epoll_poll)
EXCLUDED(kernel_pipe_poll)
