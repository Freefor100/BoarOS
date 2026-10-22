#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <poll.h>
static unsigned queue_cpus;
static void queue_pin(unsigned cpu)
{ uint64_t mask=UINT64_C(1)<<cpu;if(syscall(SYS_sched_setaffinity,0,8,&mask))_exit(31); }
static void queue_gate(pthread_barrier_t *gate)
{ int result=pthread_barrier_wait(gate);if(result&&result!=PTHREAD_BARRIER_SERIAL_THREAD)_exit(32); }
struct queue_pair { pthread_barrier_t gate;int signal,mode;pid_t tid[2];unsigned id; };
struct queue_actor {struct queue_pair *pair;unsigned side;};
static int queue_send(struct queue_pair *p,unsigned to,uint64_t value)
{
    siginfo_t info={0};info.si_code=SI_QUEUE;info.si_pid=getpid();info.si_uid=getuid();info.si_value.sival_ptr=(void *)(uintptr_t)value;
    return syscall(SYS_rt_tgsigqueueinfo,getpid(),p->tid[to],p->signal,&info);
}
static uint64_t queue_receive(struct queue_pair *p,int fd,int ep)
{
    if(!p->mode){sigset_t set;sigemptyset(&set);sigaddset(&set,p->signal);siginfo_t info;
        if(sigwaitinfo(&set,&info)!=p->signal || info.si_code!=SI_QUEUE)_exit(33);
        return (uintptr_t)info.si_value.sival_ptr;}
    if(p->mode==2){struct epoll_event event;if(epoll_wait(ep,&event,1,1000)!=1 || !(event.events&EPOLLIN))_exit(34);}
    if(p->mode==3){struct pollfd pollfd={fd,POLLIN,0};if(poll(&pollfd,1,1000)!=1 || !(pollfd.revents&POLLIN))_exit(35);}
    struct signalfd_siginfo info;if(read(fd,&info,128)!=128 || info.ssi_signo!=(unsigned)p->signal || info.ssi_code!=SI_QUEUE)_exit(36);
    return info.ssi_ptr;
}
static void *queue_exchange(void *argument)
{
    struct queue_actor *actor=argument;struct queue_pair *p=actor->pair;unsigned side=actor->side;
    queue_pin((p->id*2+side)%queue_cpus);p->tid[side]=syscall(SYS_gettid);
    sigset_t set;sigemptyset(&set);sigaddset(&set,p->signal);int fd=-1,ep=-1;
    if(p->mode){fd=signalfd(-1,&set,0);if(fd<0)_exit(37);}
    if(p->mode==2){ep=epoll_create1(0);struct epoll_event event={.events=EPOLLIN|EPOLLONESHOT,.data.u64=1};if(ep<0||epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event))_exit(38);}
    queue_gate(&p->gate);
    for(unsigned i=0;i<32;i++) {
        uint64_t value=UINT64_C(0xabcdef1200000000)+(uint64_t)p->id*256+i;
        if(!side){if(queue_send(p,1,value)||queue_receive(p,fd,ep)!=value)_exit(39);}
        else {if(queue_receive(p,fd,ep)!=value||queue_send(p,0,value))_exit(40);}
        if(ep>=0){struct epoll_event event={.events=EPOLLIN|EPOLLONESHOT,.data.u64=1};if(epoll_ctl(ep,EPOLL_CTL_MOD,fd,&event))_exit(41);}
    }
    if(ep>=0&&close(ep))_exit(42);
    if(fd>=0&&close(fd))_exit(43);
    return 0;
}
static void queued_threads(void)
{
    uint64_t allowed=0;CHECK(syscall(SYS_sched_getaffinity,0,8,&allowed)>0);queue_cpus=0;
    while(queue_cpus<4&&(allowed&(UINT64_C(1)<<queue_cpus)))queue_cpus++;
    CHECK(queue_cpus>0);if(!queue_cpus)return;
    sigset_t set,old;sigemptyset(&set);for(unsigned i=0;i<4;i++)sigaddset(&set,SIGRTMIN+8+(int)i);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    for(unsigned mode=0;mode<4;mode++) {
        struct queue_pair pairs[4];struct queue_actor actors[8];pthread_t threads[8];
        for(unsigned i=0;i<4;i++) {
            pairs[i]=(struct queue_pair){.signal=SIGRTMIN+8+(int)i,.mode=(int)mode,.id=i};
            CHECK(!pthread_barrier_init(&pairs[i].gate,0,2));
            for(unsigned side=0;side<2;side++){unsigned n=i*2+side;actors[n]=(struct queue_actor){&pairs[i],side};CHECK(!pthread_create(&threads[n],0,queue_exchange,&actors[n]));}
        }
        for(unsigned i=0;i<8;i++)CHECK(!pthread_join(threads[i],0));
        for(unsigned i=0;i<4;i++)CHECK(!pthread_barrier_destroy(&pairs[i].gate));
        printf("QUEUED THREADS mode=%u cpus=%u OK\n",mode,queue_cpus);
    }
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));
}
