/* 独立用户态适配：借用原libc的syscall及线程errno，不替换原libc。 */
struct sched_param;
extern long syscall(long number, ...);

int sched_getparam(int pid, struct sched_param *parameter)
{
    return (int)syscall(121L, (long)pid, parameter);
}

int sched_getscheduler(int pid)
{
    return (int)syscall(120L, (long)pid);
}

int sched_setparam(int pid, const struct sched_param *parameter)
{
    return (int)syscall(118L, (long)pid, parameter);
}

int sched_setscheduler(int pid, int policy, const struct sched_param *parameter)
{
    return (int)syscall(119L, (long)pid, (long)policy, parameter);
}
