#ifndef BOAROS_ARCH_BUS_H
#define BOAROS_ARCH_BUS_H
/* Publication/completion barriers are CPU facts; transport supplies bus accesses. */
static inline void arch_dma_barrier(void)
{
#if defined(BOAROS_ARCH_LOONGARCH)
    __asm__ volatile("dbar 0" ::: "memory");
#else
    __asm__ volatile("fence rw, rw" ::: "memory");
#endif
}
static inline void arch_io_barrier(void)
{
#if defined(BOAROS_ARCH_LOONGARCH)
    __asm__ volatile("dbar 0" ::: "memory");
#else
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#endif
}
#endif
