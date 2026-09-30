#include <kernel/sched_runqueue.h>

void kernel_sched_enqueue(struct kernel_sched_runqueue *queue,
    struct kernel_sched_node *node, void *owner, unsigned priority, int at_head)
{
    if (!queue || !node || node->queued || !owner || priority > 99) __builtin_trap();
    struct kernel_sched_node *previous = 0, *next = 0;
    if (queue->heads[priority]) {
        if (at_head) {
            next = queue->heads[priority];
            previous = next->previous;
        } else {
            previous = queue->tails[priority];
            next = previous->next;
        }
    } else if (!queue->last || queue->last->priority > priority) {
        previous = queue->last;
    } else {
        /* 最多检查 99 个优先级；已有桶（包括 OTHER）入队不扫描任务。 */
        for (unsigned p = priority + 1; p < 100; p++) {
            if (queue->tails[p]) { previous = queue->tails[p]; break; }
        }
        next = previous ? previous->next : queue->first;
    }
    node->next = next; node->previous = previous;
    node->owner = owner; node->priority = priority; node->queued = 1;
    if (previous) previous->next = node; else queue->first = node;
    if (next) next->previous = node; else queue->last = node;
    if (!queue->heads[priority] || at_head) queue->heads[priority] = node;
    if (!queue->tails[priority] || !at_head) queue->tails[priority] = node;
}

void kernel_sched_dequeue(struct kernel_sched_runqueue *queue,
                           struct kernel_sched_node *node)
{
    if (!queue || !node || !node->queued) __builtin_trap();
    unsigned p = node->priority;
    if (queue->heads[p] == node)
        queue->heads[p] = node->next && node->next->priority == p ? node->next : 0;
    if (queue->tails[p] == node)
        queue->tails[p] = node->previous && node->previous->priority == p
                             ? node->previous : 0;
    if (node->previous) node->previous->next = node->next;
    else queue->first = node->next;
    if (node->next) node->next->previous = node->previous;
    else queue->last = node->previous;
    node->next = node->previous = 0;
    node->queued = 0;
}

struct kernel_sched_node *kernel_sched_pick(struct kernel_sched_runqueue *queue,
                                            int rt_eligible)
{
    return rt_eligible ? queue->first : queue->heads[0];
}
