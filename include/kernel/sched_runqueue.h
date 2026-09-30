#ifndef BOAROS_KERNEL_SCHED_RUNQUEUE_H
#define BOAROS_KERNEL_SCHED_RUNQUEUE_H
/* Intrusive, priority-ordered FIFO. All operations require scheduler IRQ lock. */
struct kernel_sched_node {
    struct kernel_sched_node *next, *previous;
    void *owner;
    unsigned priority, queued;
};
struct kernel_sched_runqueue {
    struct kernel_sched_node *first, *last;
    struct kernel_sched_node *heads[100], *tails[100];
};
void kernel_sched_enqueue(struct kernel_sched_runqueue *queue,
    struct kernel_sched_node *node, void *owner, unsigned priority, int at_head);
void kernel_sched_dequeue(struct kernel_sched_runqueue *queue,
                           struct kernel_sched_node *node);
struct kernel_sched_node *kernel_sched_pick(struct kernel_sched_runqueue *queue,
                                            int rt_eligible);
#endif
