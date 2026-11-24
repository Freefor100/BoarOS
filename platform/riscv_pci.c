#include <platform/riscv_pci.h>
#include <arch/bus.h>
#include <arch/riscv/memory_layout.h>
#include <arch/riscv/plic.h>

static volatile uint8_t *config_address(uint16_t bdf, uint16_t offset, unsigned width)
{
    if (offset >= 4096 || width > 4096U - offset) return NULL;
    uint64_t address = RISCV_VIRT_PCI_ECAM_BASE + ((uint64_t)bdf << 12) + offset;
    return (void *)(uintptr_t)(RISCV_KERNEL_MMIO_BASE + address);
}

static uint32_t config_read(void *context, uint16_t bdf, uint16_t offset, unsigned width)
{
    (void)context;
    volatile uint8_t *address = config_address(bdf, offset, width);
    if (!address) return UINT32_MAX;
    if (width == 1) return *address;
    if (width == 2 && !(offset & 1)) return *(volatile uint16_t *)address;
    if (width == 4 && !(offset & 3)) return *(volatile uint32_t *)address;
    __builtin_trap();
}

static void config_write(void *context, uint16_t bdf, uint16_t offset,
    unsigned width, uint32_t value)
{
    (void)context;
    volatile uint8_t *address = config_address(bdf, offset, width);
    if (!address) __builtin_trap();
    if (width == 1) *address = (uint8_t)value;
    else if (width == 2 && !(offset & 1)) *(volatile uint16_t *)address = (uint16_t)value;
    else if (width == 4 && !(offset & 3)) *(volatile uint32_t *)address = value;
    else __builtin_trap();
    arch_io_barrier();
}

static volatile void *map_resource(void *context, uint64_t address, uint64_t size)
{
    (void)context;
    const uint64_t end = RISCV_VIRT_PCI_MEMORY_BASE + RISCV_VIRT_PCI_MEMORY_SIZE;
    if (!size || address < RISCV_VIRT_PCI_MEMORY_BASE || address >= end || size > end - address)
        return NULL;
    return (void *)(uintptr_t)(RISCV_KERNEL_MMIO_BASE + address);
}

static uint32_t interrupt_source(void *context, uint16_t bdf, uint8_t pin)
{
    (void)context;
    return bdf < 256 && pin >= 1 && pin <= 4 ? 32 + (((bdf >> 3) + pin - 1) & 3) : 0;
}

static int register_interrupt(void *context, uint32_t source, void (*handler)(void *), void *owner)
{
    (void)context;
    return riscv_plic_register(source, handler, owner);
}

static void unregister_interrupt(void *context, uint32_t source, void *owner)
{
    (void)context;
    riscv_plic_unregister(source, owner);
}

struct pci_host *riscv_virt_pci_host(void)
{
    static struct pci_host host = {
        .read = config_read, .write = config_write, .map = map_resource,
        .interrupt_source = interrupt_source, .register_irq = register_interrupt,
        .unregister_irq = unregister_interrupt, .memory_base = RISCV_VIRT_PCI_MEMORY_BASE,
        .memory_size = RISCV_VIRT_PCI_MEMORY_SIZE,
    };
    return &host;
}
