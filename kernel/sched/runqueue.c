#include <kernel/sched_runqueue.h>
#include <kernel/sched_policy.h>

static struct kernel_sched_node *fair_node(struct kernel_avl_node *node)
{ return (void *)node; }
#if BOAROS_MAX_CPUS > 1
static unsigned fair_subtree_affinity(struct kernel_avl_node *node)
{ return node ? fair_node(node)->fair_subtree_affinity : 0; }

static void fair_update_affinity(struct kernel_avl_node *node)
{
    struct kernel_sched_node *task=fair_node(node);
    task->fair_subtree_affinity=task->fair_affinity |
        fair_subtree_affinity(node->left) | fair_subtree_affinity(node->right);
}

struct kernel_sched_node *kernel_sched_fair_pick_cpu(const struct kernel_sched_runqueue *queue,unsigned cpu)
{
    unsigned mask=1U<<cpu;
    struct kernel_avl_node *node=queue->fair_root;
    if (!(fair_subtree_affinity(node)&mask)) return 0;
    while (node) {
        if (fair_subtree_affinity(node->left)&mask) node=node->left;
        else if (fair_node(node)->fair_affinity&mask) return fair_node(node);
        else node=node->right;
    }
    return 0;
}
#endif

void kernel_sched_enqueue(struct kernel_sched_runqueue *queue,
    struct kernel_sched_node *node,void *owner,unsigned priority,int at_head)
{
    if(!queue||!node||node->queued||!owner||priority>99)__builtin_trap();
    node->owner=owner;node->priority=priority;node->queued=1;queue->count++;
    if(!priority) {
        node->fair.left=node->fair.right=0;node->fair.height=1;
#if BOAROS_MAX_CPUS > 1
        node->fair_subtree_affinity=node->fair_affinity;
        queue->fair_root=kernel_avl_insert_node_augmented(queue->fair_root,&node->fair,fair_update_affinity);
#else
        queue->fair_root=kernel_avl_insert_node(queue->fair_root,&node->fair);
#endif
        return;
    }
    struct kernel_sched_node *previous=0,*next=0;
    if(queue->heads[priority]) {
        if(at_head){next=queue->heads[priority];previous=next->previous;}
        else {previous=queue->tails[priority];next=previous->next;}
    } else if(!queue->rt_last || queue->rt_last->priority>priority) previous=queue->rt_last;
    else {
        for(unsigned p=priority+1;p<100;p++)if(queue->tails[p]){previous=queue->tails[p];break;}
        next=previous?previous->next:queue->rt_first;
    }
    node->next=next;node->previous=previous;
    if(previous)previous->next=node;else queue->rt_first=node;
    if(next)next->previous=node;else queue->rt_last=node;
    if(!queue->heads[priority]||at_head)queue->heads[priority]=node;
    if(!queue->tails[priority]||!at_head)queue->tails[priority]=node;
}
void kernel_sched_dequeue(struct kernel_sched_runqueue *queue,struct kernel_sched_node *node)
{
    if(!queue||!node||!node->queued||!queue->count)__builtin_trap();
    queue->count--;
    unsigned p=node->priority;
    if(!p) {
#if BOAROS_MAX_CPUS > 1
        queue->fair_root=kernel_avl_remove_node_augmented(queue->fair_root,&node->fair,fair_update_affinity);
#else
        queue->fair_root=kernel_avl_remove_node(queue->fair_root,&node->fair);
#endif
        node->fair.left=node->fair.right=0;node->fair.height=0;
    } else {
        if(queue->heads[p]==node)queue->heads[p]=node->next&&node->next->priority==p?node->next:0;
        if(queue->tails[p]==node)queue->tails[p]=node->previous&&node->previous->priority==p?node->previous:0;
        if(node->previous)node->previous->next=node->next;else queue->rt_first=node->next;
        if(node->next)node->next->previous=node->previous;else queue->rt_last=node->previous;
        node->next=node->previous=0;
    }
    node->queued=0;
}
struct kernel_sched_node *kernel_sched_pick(struct kernel_sched_runqueue *q,int rt)
{ return rt&&q->rt_first?q->rt_first:fair_node(kernel_avl_first(q->fair_root)); }
struct kernel_sched_node *kernel_sched_last(struct kernel_sched_runqueue *q)
{ struct kernel_avl_node *node=kernel_avl_last(q->fair_root);return node?fair_node(node):q->rt_last; }
struct kernel_sched_node *kernel_sched_next(struct kernel_sched_runqueue *q,struct kernel_sched_node *node)
{
    if(node->priority)return node->next?node->next:fair_node(kernel_avl_first(q->fair_root));
    return fair_node(kernel_avl_next(q->fair_root,&node->fair));
}
void kernel_sched_fair_place(struct kernel_sched_runqueue *q,struct kernel_sched_node *node,
    const struct kernel_sched_runqueue *previous)
{
    if(node->queued)__builtin_trap();
    __int128 value=previous?(__int128)node->fair.key-previous->min_vruntime+q->min_vruntime:q->min_vruntime;
    uint64_t floor=q->min_vruntime>KERNEL_SCHED_FAIR_NS?q->min_vruntime-KERNEL_SCHED_FAIR_NS:0;
    if(value<floor)value=floor;
    if(value>UINT64_MAX)__builtin_trap();
    node->fair.key=(uint64_t)value;
}
void kernel_sched_fair_advance(struct kernel_sched_runqueue *q,const struct kernel_sched_node *running)
{
    struct kernel_avl_node *first=kernel_avl_first(q->fair_root);
    uint64_t value=first?first->key:running?running->fair.key:q->min_vruntime;
    if(running&&running->fair.key<value)value=running->fair.key;
    if(value>q->min_vruntime)q->min_vruntime=value;
}
