#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct shared {
    atomic_uint low_ready, blocker_ready, go, blocker_running;
    atomic_uint moved_cpu, progress, observed, release, blocker_exiting, done;
};
static struct shared *s;
static void require(int ok,const char *what)
{ if (!ok) {dprintf(2,"RT-BALANCE FAIL %s errno=%d\n",what,errno);_exit(1);} }
static unsigned cpu(void)
{ unsigned id=99;require(!syscall(SYS_getcpu,&id,0,0),"getcpu");return id; }
static void affinity(uint64_t mask)
{ require(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask),"affinity"); }
static void fifo(unsigned priority)
{
    struct sched_param p={.sched_priority=priority};
    require(!syscall(SYS_sched_setscheduler,0,SCHED_FIFO,&p),"FIFO");
}
static void runtime(const char *value)
{
    int fd=open("/proc/sys/kernel/sched_rt_runtime_us",O_WRONLY);
    require(fd>=0,"runtime open");
    size_t n=0;while(value[n])n++;
    require(write(fd,value,n)==(ssize_t)n && !close(fd),"runtime write");
}
static void wait_flag(atomic_uint *flag)
{ while(!atomic_load_explicit(flag,memory_order_acquire)) {} }
/* Keep a named, syscall-free low-priority loop so the runner can verify a
 * real supervisor software interrupt at its user PC, not merely a timer. */
__attribute__((noinline)) void rt_balance_low_busy(void)
{
    atomic_store_explicit(&s->low_ready,1,memory_order_release);
    while(!atomic_load_explicit(&s->done,memory_order_acquire)) {}
}
/* A dedicated RISC-V trap site lets the runner distinguish migration
 * progress from a later FIFO10 exit, independent of libc syscall wrappers. */
__attribute__((noinline)) long rt_balance_moved_getcpu(unsigned *id)
{
    register long a0 __asm__("a0")=(long)id;
    register long a1 __asm__("a1")=0;
    register long a2 __asm__("a2")=0;
    register long a7 __asm__("a7")=SYS_getcpu;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1),"r"(a2),"r"(a7) : "memory");
    return a0;
}
static void blocker(void)
{
    affinity(1);fifo(90);require(cpu()==0,"blocker CPU0");
    atomic_store_explicit(&s->blocker_ready,1,memory_order_release);
    while(!atomic_load_explicit(&s->go,memory_order_acquire)) {
        long result=syscall(SYS_futex,&s->go,0,0,0,0,0);
        require(!result || errno==EAGAIN || errno==EINTR,"blocker wait");
    }
    /* No yield, blocking call, clock or output until the movable task proves
     * progress on CPU1. RT bandwidth is disabled for this dependency window. */
    atomic_store_explicit(&s->blocker_running,1,memory_order_release);
    wait_flag(&s->progress);
    require(atomic_load_explicit(&s->moved_cpu,memory_order_relaxed)==1,"observed CPU1");
    atomic_store_explicit(&s->observed,1,memory_order_release);
    wait_flag(&s->release);
    atomic_store_explicit(&s->blocker_exiting,1,memory_order_release);
    _exit(0);
}
static void movable(void)
{
    /* Widen affinity while OTHER, before obtaining RT qualification. A later
     * RT service call would intentionally retain its source reservation. */
    affinity(3);require(cpu()==0,"movable starts CPU0");fifo(80);
    require(cpu()==0,"FIFO80 still CPU0");
    atomic_store_explicit(&s->go,1,memory_order_release);
    require(syscall(SYS_futex,&s->go,1,1,0,0,0)==1,"wake blocked FIFO90");
    wait_flag(&s->blocker_running);
    unsigned destination=99;
    require(!rt_balance_moved_getcpu(&destination),"moved getcpu");
    require(destination==1,"FIFO80 migrated to busy CPU1");
    require(!atomic_load_explicit(&s->blocker_exiting,memory_order_acquire),"blocker still active");
    atomic_store_explicit(&s->moved_cpu,destination,memory_order_relaxed);
    atomic_store_explicit(&s->progress,1,memory_order_release);
    wait_flag(&s->observed);
    require(!atomic_load_explicit(&s->blocker_exiting,memory_order_acquire),"progress precedes blocker exit");
    atomic_store_explicit(&s->release,1,memory_order_release);
    atomic_store_explicit(&s->done,1,memory_order_release);
    _exit(0);
}
static void reap(pid_t child)
{ int status;require(waitpid(child,&status,0)==child && status==0,"child cleanup"); }
int main(void)
{
    require(mkdir("/proc",0755)==0 || errno==EEXIST,"proc directory");
    require(!mount("proc","/proc","proc",0,0),"proc mount");
    affinity(1);runtime("-1\n");
    s=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    require(s!=MAP_FAILED,"shared mapping");
    /* Prefault the full static text before fork, including the spin windows.
     * A cold executable fault must not accidentally make FIFO90 block. */
    extern const unsigned char _start[],_etext[];
    volatile unsigned char warm=0;
    for(uintptr_t p=(uintptr_t)_start;p<(uintptr_t)_etext;p+=4096)warm^=*(const unsigned char *)p;
    (void)warm;require(cpu()==0,"controller CPU0");
    pid_t low=fork();require(low>=0,"low fork");
    if(!low) {affinity(2);fifo(10);require(cpu()==1,"low CPU1");rt_balance_low_busy();_exit(0);}
    wait_flag(&s->low_ready);
    pid_t high=fork();require(high>=0,"blocker fork");if(!high)blocker();
    /* Both controller and FIFO90 are pinned to CPU0. The controller can
     * observe ready only after FIFO90 really blocked in its futex wait. */
    wait_flag(&s->blocker_ready);
    pid_t moving=fork();require(moving>=0,"movable fork");if(!moving)movable();
    reap(moving);reap(high);reap(low);
    require(atomic_load(&s->progress) && atomic_load(&s->observed) &&
        atomic_load(&s->blocker_exiting) && atomic_load(&s->moved_cpu)==1,"completed handshake");
    require(!munmap(s,4096),"shared cleanup");runtime("950000\n");
    puts("RT-BALANCE PASS source=0 destination=1 progress-before-blocker-exit children=3");
    return 42;
}
