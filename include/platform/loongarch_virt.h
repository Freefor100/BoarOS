#ifndef BOAROS_PLATFORM_LOONGARCH_VIRT_H
#define BOAROS_PLATFORM_LOONGARCH_VIRT_H
#include <arch/loongarch/boot_map.h>
#include <kernel/boot_memory.h>
struct ns16550_port;
struct kernel_heap;
int la_virt_uart_start(struct ns16550_port **,struct kernel_heap *,uint64_t);
void la_virt_puts(const char *);
void la_virt_hex(uint64_t);
void la_virt_shutdown(void) __attribute__((noreturn));
void la_virt_restart(void) __attribute__((noreturn));
void la_virt_halt(void) __attribute__((noreturn));
void la_virt_fatal(const char *) __attribute__((noreturn));
int la_virt_boot_memory(uint64_t, uint64_t, uint64_t, struct boot_memory_layout *);
void *la_virt_page_access(uint64_t);
void la_virt_use_kernel_mapping(void);
int la_virt_physical_address(const void *, uint64_t *);
#endif
