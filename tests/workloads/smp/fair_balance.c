#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct shared {
    atomic_uint background, ready, moved, observed, done;
};
static struct shared *s;
static void require(int ok,const char *what)
{ if(!ok){dprintf(2,"FAIR-BALANCE FAIL %s errno=%d\n",what,errno);_exit(1);} }
static unsigned cpu(void)
{ unsigned id=99;require(!syscall(SYS_getcpu,&id,0,0),"getcpu");return id; }
static void affinity(uint64_t mask)
{ require(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask),"affinity"); }
/* This competitor never yields, blocks or exits before a formerly CPU0
 * worker proves execution on CPU1. Idle-only stealing cannot satisfy it. */
__attribute__((noinline)) void fair_balance_background(void)
{
    atomic_store_explicit(&s->background,1,memory_order_release);
    while(!atomic_load_explicit(&s->moved,memory_order_acquire)) {}
    atomic_store_explicit(&s->observed,1,memory_order_release);
    while(!atomic_load_explicit(&s->done,memory_order_acquire)) {}
}
static void movable(void)
{
    require(cpu()==0,"worker starts CPU0");
    affinity(3);
    atomic_fetch_add_explicit(&s->ready,1,memory_order_release);
    while(!atomic_load_explicit(&s->done,memory_order_acquire)) {
        if(cpu()==1) atomic_store_explicit(&s->moved,1,memory_order_release);
        /* No yield, futex, I/O or timer polling. getcpu is locally dispatched;
         * the ordinary scheduling timer must redistribute the busy queues. */
        for(unsigned n=0;n<100000;n++) __asm__ volatile("" ::: "memory");
    }
    _exit(0);
}
int main(void)
{
    affinity(1);
    s=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    require(s!=MAP_FAILED,"shared mapping");
    extern const unsigned char _start[],_etext[];
    volatile unsigned char warm=0;
    for(uintptr_t p=(uintptr_t)_start;p<(uintptr_t)_etext;p+=4096)warm^=*(const unsigned char *)p;
    (void)warm;
    pid_t children[4];
    children[0]=fork();require(children[0]>=0,"background fork");
    if(!children[0]){affinity(2);require(cpu()==1,"background CPU1");fair_balance_background();_exit(0);}
    while(!atomic_load_explicit(&s->background,memory_order_acquire)) {}
    for(unsigned n=1;n<4;n++) {
        children[n]=fork();require(children[n]>=0,"worker fork");
        if(!children[n])movable();
    }
    while(atomic_load_explicit(&s->ready,memory_order_acquire)!=3) {}
    puts("FAIR-BALANCE READY busy-competitor=1 movable-workers=3");
    while(!atomic_load_explicit(&s->observed,memory_order_acquire)) {}
    atomic_store_explicit(&s->done,1,memory_order_release);
    for(unsigned n=0;n<4;n++) {
        int status;require(waitpid(children[n],&status,0)==children[n] && status==0,"child cleanup");
    }
    require(!munmap(s,4096),"shared cleanup");
    puts("FAIR-BALANCE PASS busy-source=0 busy-destination=1 children=4");
    return 42;
}
