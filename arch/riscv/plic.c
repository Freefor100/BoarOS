#include <arch/riscv/context.h>
#include <arch/riscv/plic.h>
#include <stddef.h>
#define PLIC_MAX_SOURCES 1023U
static int dispatching;
int riscv_plic_in_interrupt(void) { return dispatching; }
static struct {
    volatile unsigned char *base;
    uint64_t size, enable, claim;
    uint32_t sources;
    struct { void (*handle)(void *); void *owner; } handlers[PLIC_MAX_SOURCES + 1];
} plic;
static volatile uint32_t *reg(uint64_t offset)
{
    if (offset > plic.size || plic.size - offset < 4) __builtin_trap();
    return (volatile uint32_t *)(plic.base + offset);
}
int riscv_plic_init(volatile void *base, uint64_t size, uint32_t context, uint32_t sources)
{
    uint64_t enable = 0x2000ULL + (uint64_t)context * 0x80;
    uint64_t threshold = 0x200000ULL + (uint64_t)context * 0x1000;
    if (!base || plic.base || !sources || sources > PLIC_MAX_SOURCES ||
        size < threshold + 8 || size < enable + 0x80) return 0;
    plic.base = base; plic.size = size; plic.sources = sources;
    plic.enable = enable; plic.claim = threshold + 4;
    for (uint32_t i = 0; i <= sources / 32; i++) *reg(enable + i * 4) = 0;
    *reg(threshold) = 0;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    __asm__ volatile("csrs sie, %0" :: "r"((uintptr_t)1 << 9) : "memory");
    return 1;
}
int riscv_plic_register(uint32_t source, void (*handle)(void *), void *owner)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!plic.base || !source || source > plic.sources || !handle || !owner ||
        plic.handlers[source].handle) { riscv_interrupt_restore(irq); return 0; }
    plic.handlers[source].handle = handle; plic.handlers[source].owner = owner;
    *reg(source * 4) = 1;
    *reg(plic.enable + (source / 32) * 4) |= 1U << (source % 32);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    riscv_interrupt_restore(irq);
    return 1;
}
void riscv_plic_unregister(uint32_t source, void *owner)
{
    uintptr_t irq = riscv_interrupt_save();
    if (!source || source > plic.sources || plic.handlers[source].owner != owner) __builtin_trap();
    *reg(plic.enable + (source / 32) * 4) &= ~(1U << (source % 32));
    plic.handlers[source].handle = 0; plic.handlers[source].owner = 0;
    riscv_interrupt_restore(irq);
}
void riscv_plic_dispatch(void)
{
    if (!plic.base || dispatching) __builtin_trap();
    dispatching = 1;
    /* Every claimed source is completed even if it was disabled meanwhile. */
    for (uint32_t n = 0; n <= plic.sources; n++) {
        uint32_t source = *reg(plic.claim);
        if (!source) { dispatching = 0; return; }
        if (source > plic.sources) __builtin_trap();
        if (plic.handlers[source].handle)
            plic.handlers[source].handle(plic.handlers[source].owner);
        else *reg(plic.enable + (source / 32) * 4) &= ~(1U << (source % 32));
        __asm__ volatile("fence iorw, iorw" ::: "memory");
        *reg(plic.claim) = source;
    }
    dispatching = 0;
}
