#ifndef BOAROS_KERNEL_SECRETMEM_H
#define BOAROS_KERNEL_SECRETMEM_H

#include <kernel/syscall.h>
#include <stdint.h>

struct kernel_open_file_description;
struct kernel_mm;

enum kernel_syscall_status syscall_handle_memfd_secret(struct kernel_task *,
    const struct kernel_syscall_request *, struct kernel_syscall_result *);
#if defined(__riscv) || defined(BOAROS_ARCH_LOONGARCH)
int kernel_secretmem_acquire_page(struct kernel_open_file_description *, uint64_t, int, uint64_t *);
int kernel_secretmem_page_protected(uint64_t);
#else
static inline int kernel_secretmem_page_protected(uint64_t physical)
{ (void)physical; return 0; }
static inline int kernel_secretmem_acquire_page(struct kernel_open_file_description *file,
    uint64_t offset, int writable, uint64_t *physical)
{ (void)file; (void)offset; (void)writable; (void)physical; __builtin_trap(); }
#endif

#endif
