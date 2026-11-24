#ifndef BOAROS_RISCV_UACCESS_H
#define BOAROS_RISCV_UACCESS_H
#include <stddef.h>
#include <stdint.h>
struct riscv_trap_frame;
struct riscv_uaccess_result { size_t remaining; uintptr_t cause; };
struct riscv_uaccess_result riscv_copy_to_user(void *,const void *,size_t);
struct riscv_uaccess_result riscv_copy_from_user(void *,const void *,size_t);
#if BOAROS_MAX_CPUS == 1
/* Copy one full page between distinct, page-aligned kernel mappings. */
void riscv_copy_page(void *,const void *);
#endif
/* A successful string keeps the terminating NUL in remaining. */
struct riscv_uaccess_result riscv_copy_string_from_user(char *,const char *,size_t);
int riscv_uaccess_fault(struct riscv_trap_frame *);
#endif
