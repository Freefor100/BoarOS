#include "private.h"

#include <arch/mmu.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/rseq.h>
#include <kernel/signal.h>
#include <kernel/task_state.h>
#include <kernel/uaccess.h>

enum { RSEQ_UNREGISTER = 1U };

struct rseq_abi {
    uint32_t cpu_start, cpu;
    uint64_t cs;
    uint32_t flags;
};

struct rseq_cs {
    uint32_t version, flags;
    uint64_t start, commit_offset, abort;
};

_Static_assert(offsetof(struct rseq_abi, flags) + sizeof(uint32_t) ==
    KERNEL_RSEQ_FEATURE_SIZE, "classic rseq active size");
_Static_assert(sizeof(struct rseq_cs) == 32, "rseq_cs ABI");

static int rseq_read(struct kernel_task *task, void *data, uint64_t address, size_t size)
{
    size_t copied;
    return !kernel_copy_from_user(&task->mm, data, address, size, &copied) &&
           copied == size;
}

static int rseq_write(struct kernel_task *task, uint64_t address, const void *data, size_t size)
{
    size_t copied;
    return !kernel_copy_to_user(&task->mm, address, data, size, &copied) &&
           copied == size;
}

static int reset_cs(struct kernel_task *task, uintptr_t *pc)
{
    const uint64_t address = task->rseq.address + offsetof(struct rseq_abi, cs);
    uint64_t user_cs, abort_pc = 0;
    struct rseq_cs cs;
    if (!rseq_read(task, &user_cs, address, sizeof(user_cs)))
        return 0;
    if (!user_cs)
        return 1;
    if (!rseq_read(task, &cs, user_cs, sizeof(cs)) ||
        cs.start >= ARCH_MMU_USER_LIMIT ||
        cs.commit_offset > ARCH_MMU_USER_LIMIT - cs.start)
        return 0;
    if (*pc - cs.start < cs.commit_offset) {
        uint32_t signature;
        if (cs.version || cs.abort >= ARCH_MMU_USER_LIMIT ||
            cs.abort < sizeof(signature) || cs.abort - cs.start < cs.commit_offset ||
            !rseq_read(task, &signature, cs.abort - sizeof(signature), sizeof(signature)) ||
            signature != task->rseq.signature)
            return 0;
        abort_pc = cs.abort;
    }
    const uint64_t zero = 0;
    if (!rseq_write(task, address, &zero, sizeof(zero)))
        return 0;
    if (abort_pc)
        *pc = abort_pc;
    return 1;
}

static unsigned rseq_cpu(void)
{
#if BOAROS_MAX_CPUS > 1
    return kernel_cpu_current()->logical_id;
#else
    return 0;
#endif
}

int kernel_rseq_resume(struct kernel_task *task, uintptr_t *pc)
{
    /* Handle an rseq fault queued during signal delivery before returning
     * to the selected handler. */
    if (task->signal_fault.signal)
        return 1;
    struct kernel_rseq *rseq = &task->rseq;
    for (;;) {
        /* A user copy may fault and sleep. Switch-in can republish the
         * event even on the same CPU, and the CPU number may change.
         * Local interrupts stay masked between the final check and return. */
        rseq->pending = 0;
        unsigned cpu = rseq_cpu();
        uint32_t ids[] = {cpu, cpu};
        int ok = rseq_write(task, rseq->address, ids, sizeof(ids)) && reset_cs(task, pc);
        if (rseq->pending || cpu != rseq_cpu())
            continue;
        if (ok)
            return 0;
        kernel_signal_force_fault(task, 11U, 128, 0); /* SIGSEGV, SI_KERNEL */
        return 1;
    }
}

void kernel_rseq_signal(struct kernel_task *task, uintptr_t *pc, unsigned signal)
{
    /* The saved signal context must point at the abort handler when the
     * interrupted instruction belongs to an active critical section. */
    if (!reset_cs(task, pc)) {
        if (signal == 11U)
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_SIGNAL, 11U, 1U);
        kernel_signal_force_fault(task, 11U, 128, 0);
    }
    kernel_rseq_mark(&task->rseq);
}

enum kernel_syscall_status syscall_handle_rseq(struct kernel_task *task,
    const struct kernel_syscall_request *request, struct kernel_syscall_result *result)
{
    uint64_t address = request->arguments[0];
    uint32_t length = (uint32_t)request->arguments[1];
    uint32_t flags = (uint32_t)request->arguments[2];
    uint32_t signature = (uint32_t)request->arguments[3];
    struct kernel_rseq *rseq = &task->rseq;

    result->action = KERNEL_SYSCALL_ACTION_RETURN;
    result->value = -KERNEL_EINVAL;
    if (length != KERNEL_RSEQ_SIZE || (address & (KERNEL_RSEQ_SIZE - 1U)) ||
        (flags & ~RSEQ_UNREGISTER))
        return KERNEL_SYSCALL_STATUS_OK;
    if (rseq->address) {
        if (address != rseq->address)
            return KERNEL_SYSCALL_STATUS_OK;
        if (signature != rseq->signature) {
            result->value = -KERNEL_EPERM;
        } else if (!flags) {
            result->value = -KERNEL_EBUSY;
        } else {
            const uint32_t ids[] = {0, UINT32_MAX};
            if (!rseq_write(task, address, ids, sizeof(ids))) {
                kernel_signal_force_fault(task, 11U, 128, 0);
                result->value = -KERNEL_EFAULT;
            } else {
                *rseq = (struct kernel_rseq){0};
                result->value = 0;
            }
        }
    } else if (!flags) {
        const struct rseq_abi initial = {.cpu_start = UINT32_MAX, .cpu = UINT32_MAX};
        if (!address || kernel_user_range_check(address, length) ||
            !rseq_write(task, address, &initial, KERNEL_RSEQ_FEATURE_SIZE)) {
            result->value = -KERNEL_EFAULT;
        } else {
            *rseq = (struct kernel_rseq){.address = address, .signature = signature, .pending = 1};
            result->value = 0;
        }
    }
    return KERNEL_SYSCALL_STATUS_OK;
}
