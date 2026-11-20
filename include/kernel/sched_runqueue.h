#ifndef BOAROS_KERNEL_SCHED_RUNQUEUE_H
#define BOAROS_KERNEL_SCHED_RUNQUEUE_H
#include <kernel/avl.h>
#include <kernel/cpu_config.h>
#include <kernel/sched_policy.h>
struct kernel_task;
#if BOAROS_USER_SMP
struct kernel_fair_link {
    struct kernel_fair_link *next,*previous;
};
#endif
#if BOAROS_MAX_CPUS > 1
struct kernel_sched_rt_candidate {
    struct kernel_avl_node node;
    uint32_t eligibility,subtree_eligibility;
};
#endif
/* One node belongs to either the RT FIFO or the fair tree, never both.
 * The owning CPU run lock (UP: scheduler domain) protects either index. */
struct kernel_sched_node {
    union {
        struct { struct kernel_sched_node *next,*previous; };
        struct kernel_avl_node fair;
    };
    void *owner;
    uint8_t priority,queued;
#if BOAROS_MAX_CPUS > 1
    uint8_t fair_affinity, fair_subtree_affinity;
#endif
#if BOAROS_USER_SMP
    unsigned fair_weight; /* Snapshot while queued, including a later nice/PI change. */
#endif
};
struct kernel_sched_runqueue {
    struct kernel_sched_node *rt_first,*rt_last;
    struct kernel_sched_node *heads[100],*tails[100];
    struct kernel_avl_node *fair_root;
    struct kernel_task *ready_wait_first[KERNEL_SCHED_NICE_LEVELS];
    uint64_t min_vruntime;
#if BOAROS_USER_SMP
    uint64_t fair_weight;
#endif
#if BOAROS_MAX_CPUS > 1
    struct kernel_avl_node *rt_remote;
    uint64_t rt_sequence;
#endif
    unsigned boosted,count;
};
void kernel_sched_enqueue(struct kernel_sched_runqueue *,struct kernel_sched_node *,void *,unsigned,int);
void kernel_sched_dequeue(struct kernel_sched_runqueue *,struct kernel_sched_node *);
struct kernel_sched_node *kernel_sched_pick(struct kernel_sched_runqueue *,int rt_eligible);
#if BOAROS_MAX_CPUS > 1
struct kernel_sched_node *kernel_sched_fair_pick_cpu(const struct kernel_sched_runqueue *,unsigned cpu);
#endif
struct kernel_sched_node *kernel_sched_last(struct kernel_sched_runqueue *);
struct kernel_sched_node *kernel_sched_next(struct kernel_sched_runqueue *,struct kernel_sched_node *);
void kernel_sched_fair_place(struct kernel_sched_runqueue *,struct kernel_sched_node *,
    const struct kernel_sched_runqueue *previous);
void kernel_sched_fair_advance(struct kernel_sched_runqueue *,const struct kernel_sched_node *running);
#endif
