#include "private.h"
#include "../../fs/cgroup_internal.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/task.h>

static struct kernel_cgroup *membership(const struct kernel_task *task)
{ return task->cgroup ? task->cgroup : kernel_cgroup_root(); }

static int live(const struct kernel_task *task)
{
    return task->accounted && task->arch.user_mode && task->group_leader &&
        !task->proc_exiting && !task->retirement;
}

static struct kernel_task *representative(struct kernel_task *, int);

/* A domain hierarchy cannot split a thread group. The wait-domain transaction
 * also orders ordinary forks against migration, without adding a clone lock. */
int kernel_cgroup_move(struct kernel_cgroup *destination, int32_t tid, int thread,
    const struct kernel_credentials *credentials)
{
    struct kernel_cgroup *old = 0;
    unsigned released = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        if (!destination->linked) return -KERNEL_ENODEV;
        struct kernel_task *task = tid ? process_find_identity_in(tid,KERNEL_PID_TID,process_pid_active(kernel_task_current())) : kernel_task_current();
        if (!task || !task->accounted || task->retirement || !task->group_leader) return -KERNEL_ESRCH;
        if (!thread) task = task->group_leader;
        /* kernfs authorizes the selected leader even when only another
         * thread remains alive. Exiting tasks retain their original cgroup. */
        struct kernel_cgroup *source = membership(task);
        int error = kernel_cgroup_migration_permission(source,destination,credentials);
        if (error) return error;
        if (thread && source != destination) return -KERNEL_EOPNOTSUPP;
        task = representative(task,thread);
        if (!task || membership(task) == destination) return 0;
        old = task->cgroup;
        struct kernel_task *leader = task->group_leader, *member = leader;
        do {
            if (live(member)) {
                if (member->cgroup != old) __builtin_trap();
                if (old) { if (!old->members) __builtin_trap(); old->members--; released++; }
                member->cgroup = destination == kernel_cgroup_root() ? 0 : kernel_cgroup_get(destination);
                if (member->cgroup) member->cgroup->members++;
            }
            member = member->group_next;
        } while (member != leader);
    }
    while (released--) kernel_cgroup_put(old);
    return 0;
}

/* A process whose leader has exited is represented by its first live
 * member. Visit each group once, not once for every thread or printed PID. */
static struct kernel_task *representative(struct kernel_task *task, int threads)
{
    if (threads) return live(task) ? task : 0;
    if (!task->accounted || task->retirement || task->group_leader != task) return 0;
    struct kernel_task *member = task;
    do {
        if (live(member)) return member;
        member = member->group_next;
    } while (member != task);
    return 0;
}

#include <kernel/heap.h>
#include <string.h>
int kernel_cgroup_members_snapshot(struct kernel_cgroup *group, int threads,
    struct kernel_heap *heap, char **buffer, size_t *length)
{
    /* The executing reader retains its active view across heap preparation. */
    struct kernel_pid_registry *view=process_pid_active(kernel_task_current());
    size_t capacity = 0;
    for (;;) {
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (!group->linked) return -KERNEL_ENODEV;
            for (struct kernel_task *task=scheduler.all_tasks;task;task=task->all_next) {
                struct kernel_task *member=representative(task,threads);
                if (member && membership(member)==group) capacity++;
            }
        }
        if (capacity > (SIZE_MAX-1)/12) return -KERNEL_ENOMEM;
        char *data=0;
        if (kernel_heap_allocate(heap,capacity*12+1,(void **)&data)) return -KERNEL_ENOMEM;
        size_t used=0,rows=0;int error=0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (!group->linked) error=-KERNEL_ENODEV;
            else for (struct kernel_task *task=scheduler.all_tasks;task;task=task->all_next) {
                struct kernel_task *member=representative(task,threads);
                if (!member || membership(member)!=group) continue;
                if (rows++==capacity) {error=-KERNEL_EAGAIN;break;}
                /* cgroup2 seq_show uses the current reader PID view, not the
                 * opener or hierarchy mount. Invisible members emit zero. */
                uint32_t pid=(uint32_t)kernel_pid_number_in(process_identity(member,
                    threads?KERNEL_PID_TID:KERNEL_PID_TGID),view);
                char digits[10];size_t n=0;
                do {digits[n++]=(char)('0'+pid%10);pid/=10;} while(pid);
                while(n)data[used++]=digits[--n];
                data[used++]='\n';
            }
        }
        if (!error) {data[used]=0;*buffer=data;*length=used;return 0;}
        kernel_heap_release(heap,data);
        if (error!=-KERNEL_EAGAIN) return error;
        /* Geometric slack bounds retries under a concurrent fork burst. */
        if (capacity>SIZE_MAX/2) return -KERNEL_ENOMEM;
        capacity=capacity*2+1;
    }
}

int kernel_cgroup_populated(struct kernel_cgroup *group)
{
    KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
    for (struct kernel_task *task = scheduler.all_tasks; task; task = task->all_next) {
        if (!live(task)) continue;
        for (struct kernel_cgroup *ancestor = membership(task); ancestor; ancestor = ancestor->parent)
            if (ancestor == group) return 1;
    }
    return 0;
}

int kernel_proc_cgroup(int32_t pid, uint64_t identity, int thread,
    struct kernel_heap *heap, char **buffer, size_t *length)
{
    char *data = 0;
    if (kernel_heap_allocate(heap,4102,(void **)&data)) return -KERNEL_ENOMEM;
    size_t begin = 4100;
    data[4100] = '\n'; data[4101] = 0;
    int error = 0;
    {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        struct kernel_task *task = process_find_identity(pid,thread ? KERNEL_PID_TID : KERNEL_PID_TGID);
        if (!task || !task->accounted || task->retirement || (thread ? process_identity(task,KERNEL_PID_TID)->generation : process_identity_generation(task)) != identity)
            error = -KERNEL_ESRCH;
        else {
            struct kernel_cgroup *group = membership(task);
            if (!group->linked) {begin-=10;memcpy(data+begin," (deleted)",10);}
            if (!group->parent) data[--begin] = '/';
            for (; group->parent; group = group->parent) {
                size_t count = strlen(group->name);
                if (begin < count + 4) { error = -KERNEL_ENAMETOOLONG; break; }
                begin -= count; memcpy(data+begin,group->name,count); data[--begin] = '/';
            }
        }
    }
    if (error) { kernel_heap_release(heap,data); return error; }
    begin -= 3; memcpy(data+begin,"0::",3);
    *length = 4101-begin;
    memmove(data,data+begin,*length+1);
    *buffer = data;
    return 0;
}
