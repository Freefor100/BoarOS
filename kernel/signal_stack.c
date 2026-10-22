#include <kernel/signal.h>
#include <kernel/task_state.h>
#include <kernel/errno.h>

_Static_assert(sizeof(struct kernel_signal_stack) == 24, "Linux stack_t");

/* Only the executing thread changes its registration. Clone reads its own
 * parent before child publication; no shared disposition lock is needed. */
static int on_stack(const struct kernel_signal_stack *s, uint64_t sp)
{
    return !(s->flags & LINUX_SS_AUTODISARM) &&
           sp > s->pointer && sp - s->pointer <= s->size;
}

void kernel_signal_stack_reset(struct kernel_task *task)
{
    task->signal_stack = (struct kernel_signal_stack){.flags=LINUX_SS_DISABLE};
}

int kernel_signal_stack_update(struct kernel_task *task,
    const struct kernel_signal_stack *input, struct kernel_signal_stack *old,
    uint64_t sp)
{
    struct kernel_signal_stack *s = &task->signal_stack;
    if (old) {
        *old = *s;
        old->padding = 0;
        old->flags = (s->flags & LINUX_SS_AUTODISARM) |
            (!s->size ? LINUX_SS_DISABLE : on_stack(s, sp) ? LINUX_SS_ONSTACK : 0);
    }
    if (!input) return 0;
    if (on_stack(s, sp)) return -KERNEL_EPERM;
    uint32_t mode = input->flags & ~LINUX_SS_AUTODISARM;
    if (mode != 0 && mode != LINUX_SS_DISABLE && mode != LINUX_SS_ONSTACK)
        return -KERNEL_EINVAL;
    if (s->pointer == input->pointer && s->size == input->size &&
        s->flags == input->flags) return 0;
    struct kernel_signal_stack next = *input;
    next.padding = 0;
    if (mode == LINUX_SS_DISABLE) {
        next.pointer = next.size = 0;
    } else {
#if defined(__loongarch__)
        const uint64_t minimum = 4096;
#else
        const uint64_t minimum = 2048;
#endif
        if (next.size < minimum) return -KERNEL_ENOMEM;
    }
    *s = next;
    return 0;
}

uint64_t kernel_signal_stack_prepare(struct kernel_task *task, uint64_t sp,
    uint64_t flags, struct kernel_signal_stack *saved)
{
    *saved = task->signal_stack;
    if ((flags & LINUX_SA_ONSTACK) && saved->size && !on_stack(saved, sp))
        return saved->pointer + saved->size;
    return sp;
}

int kernel_signal_stack_frame_valid(const struct kernel_signal_stack *s,
    uint64_t sp, uint64_t flags, uint64_t frame_sp, uint64_t top)
{
    if (frame_sp > top) return 0;
    if (on_stack(s, sp) || ((flags & LINUX_SA_ONSTACK) && s->size)) {
        /* Check the actual aligned architecture frame, including FP/SIMD,
         * before copying. Wrapped registration addresses fail delivery. */
        return top >= s->pointer && frame_sp > s->pointer &&
               top - s->pointer <= s->size;
    }
    return 1;
}

void kernel_signal_stack_delivered(struct kernel_task *task)
{
    if (task->signal_stack.flags & LINUX_SS_AUTODISARM)
        kernel_signal_stack_reset(task);
}
