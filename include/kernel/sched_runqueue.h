#ifndef BOAROS_KERNEL_SCHED_RUNQUEUE_H
#define BOAROS_KERNEL_SCHED_RUNQUEUE_H
#include <kernel/avl.h>
/* One node belongs to either the RT FIFO or the fair tree, never both.
 * The owning CPU run lock (UP: scheduler domain) protects either index. */
struct kernel_sched_node {
    union {
        struct { struct kernel_sched_node *next,*previous; };
        struct kernel_avl_node fair;
    };
    void *owner;
    unsigned priority,queued;
};
struct kernel_sched_runqueue {
    struct kernel_sched_node *rt_first,*rt_last;
    struct kernel_sched_node *heads[100],*tails[100];
    struct kernel_avl_node *fair_root;
    uint64_t min_vruntime;
    unsigned boosted,count;
};
void kernel_sched_enqueue(struct kernel_sched_runqueue *,struct kernel_sched_node *,void *,unsigned,int);
void kernel_sched_dequeue(struct kernel_sched_runqueue *,struct kernel_sched_node *);
struct kernel_sched_node *kernel_sched_pick(struct kernel_sched_runqueue *,int rt_eligible);
struct kernel_sched_node *kernel_sched_last(struct kernel_sched_runqueue *);
struct kernel_sched_node *kernel_sched_next(struct kernel_sched_runqueue *,struct kernel_sched_node *);
void kernel_sched_fair_place(struct kernel_sched_runqueue *,struct kernel_sched_node *,
    const struct kernel_sched_runqueue *previous);
void kernel_sched_fair_advance(struct kernel_sched_runqueue *,const struct kernel_sched_node *running);
#endif
