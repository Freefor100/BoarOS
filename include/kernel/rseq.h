#ifndef BOAROS_KERNEL_RSEQ_H
#define BOAROS_KERNEL_RSEQ_H

#include <stdint.h>

#define KERNEL_RSEQ_SIZE           32U
#define KERNEL_RSEQ_FEATURE_SIZE   20U
#define KERNEL_AT_RSEQ_FEATURE_SIZE 27U
#define KERNEL_AT_RSEQ_ALIGN        28U

struct kernel_task;

/* The executing CPU owns the registration. The scheduler sets pending for
 * the next owner before publishing it, and a rendezvous marks the recipient's
 * current task with local interrupts masked. A user copy can reschedule. */
struct kernel_rseq {
    uint64_t address;
    uint32_t signature;
    volatile unsigned pending;
};

static inline void kernel_rseq_mark(struct kernel_rseq *rseq)
{
    if (rseq->address)
        rseq->pending = 1U;
}

int kernel_rseq_resume(struct kernel_task *, uintptr_t *pc);
void kernel_rseq_signal(struct kernel_task *, uintptr_t *pc, unsigned signal);

#endif
