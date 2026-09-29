#ifndef BOAROS_ARCH_RISCV_VIRTIO_MMIO_RNG_H
#define BOAROS_ARCH_RISCV_VIRTIO_MMIO_RNG_H
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <stdint.h>
struct riscv_virtio_mmio_rng {
    volatile uint8_t *mmio;
    struct physical_page_allocator *allocator;
    void *queue_memory;
    uint64_t queue_phys, frequency;
    struct kernel_thread_join worker;
    struct kernel_wait_queue progress;
    uint32_t version, order, irq_source;
    uint16_t available, consumed;
    uint8_t irq_registered, started, stopping, active, configured;
    uint8_t completed, invalid;
    uint32_t response_length;
    uint64_t requests, bytes, zero_responses, errors, timeouts;
};
/* Start failures leave mmio non-NULL only when reset did not reclaim DMA.
 * That object remains owned and stop must succeed before its storage is freed.
 */
int riscv_virtio_mmio_rng_start(struct riscv_virtio_mmio_rng *device,
                                volatile void *mmio, uint64_t size,
                                struct physical_page_allocator *allocator,
                                uint64_t frequency, uint32_t irq_source);
int riscv_virtio_mmio_rng_stop(struct riscv_virtio_mmio_rng *device);
#endif
