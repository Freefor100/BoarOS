#include <platform/loongarch_virt.h>
#include <kernel/page.h>
#include <stddef.h>
#include <kernel/console.h>

/* QEMU v11.1.0 virt.h: ns16550 and ACPI GED, accessed uncached. */
static volatile unsigned char *const uart = (void *)(LA_UNCACHED_BASE + 0x1fe001e0);
void kernel_console_putc(char c)
{ while (!(uart[5] & 0x20)) { } uart[0] = (unsigned char)c; }
void kernel_console_emergency_begin(void) { }
void la_virt_puts(const char *text)
{
    while (*text) {
        while (!(uart[5] & 0x20)) { }
        uart[0] = (unsigned char)*text++;
    }
}
void la_virt_hex(uint64_t value)
{
    char text[19] = "0x0000000000000000";
    for (unsigned i = 0; i < 16; i++) text[17-i] = "0123456789abcdef"[(value >> (i*4)) & 15];
    la_virt_puts(text);
}
void la_virt_shutdown(void)
{
    *(volatile unsigned char *)(LA_UNCACHED_BASE + 0x100e001c) = 0x34;
    for (;;) __asm__ volatile("idle 0");
}
void la_virt_fatal(const char *message)
{ la_virt_puts("BoarOS: fatal LA "); la_virt_puts(message); la_virt_puts("\n"); la_virt_shutdown(); }
void *la_virt_page_access(uint64_t address)
{ return (void *)(uintptr_t)(LA_DIRECT_BASE + address); }
int la_virt_physical_address(const void *pointer, uint64_t *address)
{
    uint64_t v = (uintptr_t)pointer;
    if (!address || v < LA_DIRECT_BASE || v - LA_DIRECT_BASE >= (UINT64_C(1) << 48)) return 0;
    *address = v - LA_DIRECT_BASE; return 1;
}
struct efi_header { uint64_t signature; uint32_t revision, size, crc, reserved; };
struct efi_table {
    struct efi_header header;
    uint64_t vendor; uint32_t revision;
    uint64_t handles[8];
    uint64_t count, tables;
};
struct efi_config { uint32_t guid[4]; uint64_t address; };
int la_virt_boot_memory(uint64_t systab, uint64_t kernel_start, uint64_t kernel_end,
                        struct boot_memory_layout *layout)
{
    if (systab > 0x100000 - sizeof(struct efi_table)) return 0;
    const struct efi_table *table = la_virt_page_access(systab);
    if (table->header.signature != UINT64_C(0x5453595320494249) || table->count > 16 ||
        table->tables > 0x100000 - table->count*sizeof(struct efi_config)) return 0;
    const struct efi_config *config = la_virt_page_access(table->tables);
    uint64_t fdt = 0;
    for (uint64_t i = 0; i < table->count; i++)
        if (config[i].guid[0] == 0xb1b621d5 && config[i].guid[1] == 0x41a5f19c &&
            config[i].guid[2] == 0x15d90b83 && config[i].guid[3] == 0xe0aa692c) fdt = config[i].address;
    struct dtb_boot_info info;
    if (!fdt || fdt >= 0x10000000 || dtb_read_boot_info(la_virt_page_access(fdt), &info) != DTB_STATUS_OK)
        return 0;
    *layout = (struct boot_memory_layout){0};
    struct dtb_memory_range reservations[DTB_MAX_RESERVED_RANGES + 3];
    unsigned count = info.reserved_count;
    for (unsigned i = 0; i < count; i++) reservations[i] = info.reserved[i];
    reservations[count++] = (struct dtb_memory_range){0, 0x100000};
    reservations[count++] = (struct dtb_memory_range){kernel_start, kernel_end-kernel_start};
    reservations[count++] = (struct dtb_memory_range){fdt, info.dtb_size};
    for (unsigned i = 1; i < count; i++) {
        struct dtb_memory_range r = reservations[i]; unsigned j = i;
        while (j && reservations[j-1].base > r.base) { reservations[j] = reservations[j-1]; j--; }
        reservations[j] = r;
    }
    for (unsigned i = 1; i < info.memory_count; i++) {
        struct dtb_memory_range r = info.memories[i]; unsigned j = i;
        while (j && info.memories[j-1].base > r.base) { info.memories[j] = info.memories[j-1]; j--; }
        info.memories[j] = r;
    }
    for (unsigned i = 0; i < info.memory_count; i++) {
        struct dtb_memory_range r = info.memories[i];
        if (!r.size || r.base > UINT64_MAX-r.size ||
            (i && info.memories[i-1].base+info.memories[i-1].size > r.base)) return 0;
        uint64_t end = (r.base+r.size) & ~BOAROS_PAGE_MASK;
        uint64_t cursor = (r.base+BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
        for (unsigned j = 0; j <= count && cursor < end; j++) {
            uint64_t first = end, last = end;
            if (j < count) {
                if (reservations[j].base > UINT64_MAX-reservations[j].size) return 0;
                first = reservations[j].base & ~BOAROS_PAGE_MASK;
                last = reservations[j].base+reservations[j].size;
                if (last > UINT64_MAX-BOAROS_PAGE_MASK) return 0;
                last = (last+BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
                if (last <= cursor) continue;
                if (first > end) first = end;
            }
            if (first > cursor) {
                if (layout->usable_count == BOOT_MEMORY_MAX_USABLE_RANGES) return 0;
                layout->usable[layout->usable_count++] = (struct dtb_memory_range){cursor, first-cursor};
            }
            if (last > cursor) cursor = last;
        }
    }
    return layout->usable_count != 0;
}
