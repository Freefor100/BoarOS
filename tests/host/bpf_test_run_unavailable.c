/* These helpers and process/MM services are not exercised by the TEST_RUN core
 * fixture; accidental entry fails, never supplies a successful result. */
#define EXCLUDED(name) void name(void) { __builtin_trap(); }
char __kernel_start[1],__kernel_end[1];
EXCLUDED(bpf_map_borrow_begin)
EXCLUDED(bpf_map_borrow_end)
EXCLUDED(bpf_map_delete)
EXCLUDED(bpf_map_lookup_borrowed)
EXCLUDED(bpf_map_ring)
EXCLUDED(bpf_map_spec)
EXCLUDED(bpf_map_is_frozen)
EXCLUDED(bpf_map_update)
EXCLUDED(bpf_ring_output)
EXCLUDED(bpf_ring_query)
EXCLUDED(bpf_ring_reserve)
EXCLUDED(bpf_ring_submit)
EXCLUDED(kernel_credentials_current)
EXCLUDED(kernel_mm_try_pin_user_page)
EXCLUDED(kernel_mm_unpin_user_page)
EXCLUDED(kernel_random_fill)
EXCLUDED(kernel_task_mm_borrow_mutable)
EXCLUDED(kernel_task_tgid)
EXCLUDED(kernel_task_tid)
EXCLUDED(riscv_direct_map_va_to_pa)
