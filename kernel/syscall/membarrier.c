#include "private.h"
#include <arch/smp.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/membarrier.h>
#include <kernel/mm.h>
#include <kernel/task.h>

enum {
    GLOBAL = 1 << 0,
    GLOBAL_EXPEDITED = 1 << 1,
    REGISTER_GLOBAL_EXPEDITED = 1 << 2,
    PRIVATE_EXPEDITED = 1 << 3,
    REGISTER_PRIVATE_EXPEDITED = 1 << 4,
    GET_REGISTRATIONS = 1 << 9,
    COMMANDS = GLOBAL | GLOBAL_EXPEDITED | REGISTER_GLOBAL_EXPEDITED |
               PRIVATE_EXPEDITED | REGISTER_PRIVATE_EXPEDITED | GET_REGISTRATIONS,
};

#if BOAROS_MAX_CPUS > 1
/* A sender remains pinned until every published target acknowledges. Receiver
 * bits are consumed once, even when an old hardware interrupt arrives late. */
static struct __attribute__((aligned(64))) barrier_mailbox {
    unsigned sources;
    uint64_t generation;
    uint64_t acknowledged[BOAROS_MAX_CPUS];
} mailboxes[BOAROS_MAX_CPUS];

void kernel_membarrier_receive(void)
{
    unsigned target = kernel_cpu_current()->logical_id;
    unsigned sources = __atomic_exchange_n(&mailboxes[target].sources, 0, __ATOMIC_ACQUIRE);
    if (!sources)
        return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (sources) {
        unsigned source = kernel_cpu_first_in_mask(sources);
        sources &= sources - 1U;
        struct barrier_mailbox *mailbox = &mailboxes[source];
        uint64_t generation = __atomic_load_n(&mailbox->generation, __ATOMIC_RELAXED);
        __atomic_store_n(&mailbox->acknowledged[target], generation, __ATOMIC_RELEASE);
    }
}

static void rendezvous(const struct kernel_mm *mm)
{
    kernel_preempt_disable();
    unsigned self = kernel_cpu_current()->logical_id;
    struct barrier_mailbox *mailbox = &mailboxes[self];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* MM activation/deactivation already publish sequenced CPU membership.
     * A join missed by this snapshot observes the publication on its switch. */
    unsigned targets = mm ? kernel_mm_membarrier_active(mm) : ~0U;
    targets &= (1U << kernel_cpu_get_boot_count()) - 1U;
    targets &= ~(1U << self);
    if (targets) {
        uint64_t generation = __atomic_load_n(&mailbox->generation, __ATOMIC_RELAXED) + 1U;
        if (!generation) __builtin_trap();
        __atomic_store_n(&mailbox->generation, generation, __ATOMIC_RELAXED);
        unsigned remaining = targets;
        while (remaining) {
            unsigned target = kernel_cpu_first_in_mask(remaining);
            remaining &= remaining - 1U;
            struct kernel_cpu *cpu = arch_smp_cpu(target);
            if (!cpu || !kernel_cpu_online(cpu)) {
                targets &= ~(1U << target);
                continue;
            }
            __atomic_fetch_or(&mailboxes[target].sources, 1U << self, __ATOMIC_RELEASE);
            kernel_cpu_notify(cpu, KERNEL_IPI_MEMBARRIER);
        }
        kernel_cpu_flush_notifications();
        while (targets) {
            /* Two simultaneous senders can make progress even when the trap
             * entered with local interrupts masked. */
            kernel_membarrier_receive();
            unsigned target = kernel_cpu_first_in_mask(targets);
            if (__atomic_load_n(&mailbox->acknowledged[target], __ATOMIC_ACQUIRE) == generation)
                targets &= targets - 1U;
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    kernel_preempt_enable();
}
#else
static void rendezvous(const struct kernel_mm *mm)
{
    (void)mm;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
#endif

enum kernel_syscall_status syscall_handle_membarrier(struct kernel_task *caller,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *decoded)
{
    unsigned command = (unsigned)request->arguments[0];
    unsigned flags = (unsigned)request->arguments[1];
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = 0;
    if (flags || (command && ((command & (command - 1U)) || !(command & COMMANDS)))) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (!command) {
        decoded->value = COMMANDS;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (command == GLOBAL || command == GLOBAL_EXPEDITED) {
        rendezvous(NULL);
        return KERNEL_SYSCALL_STATUS_OK;
    }
    struct kernel_mm *mm;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    unsigned registrations = kernel_mm_membarrier_registrations(mm);
    if (command == GET_REGISTRATIONS) {
        decoded->value = registrations;
    } else if (command == PRIVATE_EXPEDITED) {
        if (registrations & REGISTER_PRIVATE_EXPEDITED)
            rendezvous(mm);
        else
            decoded->value = -KERNEL_EPERM;
    } else if (!(registrations & command)) {
        rendezvous(mm);
        kernel_mm_membarrier_register(mm, command);
    }
    return KERNEL_SYSCALL_STATUS_OK;
}
