#ifndef BOAROS_PLATFORM_LOONGARCH_VIRT_H
#define BOAROS_PLATFORM_LOONGARCH_VIRT_H
#include <kernel/boot_memory.h>
#define LA_DIRECT_BASE UINT64_C(0x9000000000000000)
#define LA_UNCACHED_BASE UINT64_C(0x8000000000000000)
void la_virt_puts(const char *);
void la_virt_hex(uint64_t);
void la_virt_shutdown(void) __attribute__((noreturn));
void la_virt_fatal(const char *) __attribute__((noreturn));
int la_virt_boot_memory(uint64_t, uint64_t, uint64_t, struct boot_memory_layout *);
void *la_virt_page_access(uint64_t);
int la_virt_physical_address(const void *, uint64_t *);
#endif
