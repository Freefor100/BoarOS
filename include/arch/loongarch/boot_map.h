#ifndef BOAROS_LOONGARCH_BOOT_MAP_H
#define BOAROS_LOONGARCH_BOOT_MAP_H

/* Low firmware entry starts in direct windows. The runtime bases are
 * canonical paged addresses, with room for 16 TiB in each window. */
#define LA_BOOT_CACHED_BASE 0x9000000000000000
#define LA_BOOT_UNCACHED_BASE 0x8000000000000000
#define LA_DIRECT_BASE 0xffff900000000000
#define LA_UNCACHED_BASE 0xffffa00000000000
#define LA_DIRECT_BITS 44
#define LA_DIRECT_SIZE (1ULL << LA_DIRECT_BITS)
#define LA_BOOT_DIRECT_LIMIT 0x02000000
#define LA_BOOT_RAM_ROOT_OFFSET 0x800
#define LA_BOOT_RAM_LEAF 0x11d3

/* PT/PTE widths are 11 bits. LDDIR walks root level 3 and middle level 1. */
#define LA_PWCL (14 | (11 << 5) | (25 << 10) | (11 << 15))
#define LA_PWCH (36 | (11 << 6))

#endif
