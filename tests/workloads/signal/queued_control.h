#include "../../smp/composition_protocol.h"
#include <sys/mount.h>
static struct composition_control *queued_control;
static int queued_control_fd=-1;
static void queued_observe(void)
{
    if(access("/queued-observer",F_OK))return;
    CHECK(!mkdir("/dev/shm",0700)||errno==EEXIST);CHECK(!mount("tmpfs","/dev/shm","tmpfs",0,0)||errno==EBUSY);
    queued_control_fd=open("/dev/shm/smp-control",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(queued_control_fd>=0&&!ftruncate(queued_control_fd,4096));
    queued_control=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,queued_control_fd,0);CHECK(queued_control!=MAP_FAILED);
    composition_store(&queued_control->magic,COMPOSITION_MAGIC);while(!composition_load(&queued_control->observer_ready))sched_yield();
}
struct queued_producer {pid_t target,tid;int signal,result;unsigned ready,go;};
static void *controlled_producer(void *arg)
{
    struct queued_producer *p=arg;queue_pin(0);p->tid=syscall(SYS_gettid);__atomic_store_n(&p->ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&p->go,__ATOMIC_ACQUIRE))sched_yield();
    siginfo_t info={0};info.si_code=SI_QUEUE;info.si_value.sival_ptr=(void *)501;
    p->result=syscall(SYS_rt_sigqueueinfo,p->target,p->signal,&info);return 0;
}
static void controlled_start(struct queued_producer *p,pthread_t *thread,unsigned mode,pid_t child)
{
    *p=(struct queued_producer){.target=child,.signal=SIGRTMIN+5};CHECK(!pthread_create(thread,0,controlled_producer,p));
    while(!__atomic_load_n(&p->ready,__ATOMIC_ACQUIRE))sched_yield();
    composition_store(&queued_control->read_release,0);composition_store(&queued_control->socket_entered,0);composition_store(&queued_control->socket_waited,0);composition_store(&queued_control->socket_release,0);
    composition_store(&queued_control->socket_tid,p->tid);composition_store(&queued_control->socket_waiter_tid,child);composition_store(&queued_control->socket_mode,mode);
    __atomic_store_n(&p->go,1,__ATOMIC_RELEASE);while(!composition_load(&queued_control->socket_entered))sched_yield();
}
static pthread_t controlled_leader;
static int controlled_ready,controlled_fd;
static void *controlled_receiver(void *unused)
{
    (void)unused;if(pthread_join(controlled_leader,0)||write(controlled_ready,"J",1)!=1)_exit(71);
    struct signalfd_siginfo out;if(read(controlled_fd,&out,128)!=128||out.ssi_ptr!=501)_exit(72);_exit(0);
}
static void queued_control_cases(void)
{
    if(!queued_control||queue_cpus<2){puts("QUEUED CONTROL SKIP");return;}
    queue_pin(1);sigset_t set,old;sigemptyset(&set);sigaddset(&set,SIGRTMIN+5);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    for(unsigned mode=3;mode<=4;mode++) {
        int fd=signalfd(-1,&set,0),ep=epoll_create1(0),ready[2];CHECK(fd>=0&&ep>=0&&!pipe(ready));
        pid_t child=fork();CHECK(child>=0);if(!child){close(ready[0]);
            if(mode==3){struct epoll_event event={.events=EPOLLIN};if(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event)||write(ready[1],"R",1)!=1)_exit(73);
                while(!composition_load(&queued_control->read_release))sched_yield();
                _exit(0);}
            controlled_fd=fd;controlled_ready=ready[1];controlled_leader=pthread_self();pthread_t thread;
            if(pthread_create(&thread,0,controlled_receiver,0)||write(ready[1],"R",1)!=1)_exit(74);
            while(!composition_load(&queued_control->read_release))sched_yield();
            pthread_exit(0);
        }
        CHECK(!close(ready[1]));char byte;CHECK(read(ready[0],&byte,1)==1&&byte=='R');
        struct queued_producer producer;pthread_t thread;controlled_start(&producer,&thread,mode,child);
        composition_store(&queued_control->read_release,1);
        if(mode==3){while(!composition_load(&queued_control->socket_waited))sched_yield();}
        else CHECK(read(ready[0],&byte,1)==1&&byte=='J');
        composition_store(&queued_control->socket_release,1);CHECK(!pthread_join(thread,0)&&!producer.result);
        int status;CHECK(waitpid(child,&status,0)==child&&!status);composition_store(&queued_control->socket_mode,0);
        if(mode==3)CHECK(!epoll_ctl(ep,EPOLL_CTL_DEL,fd,0));
        CHECK(!close(ready[0])&&!close(ep)&&!close(fd));
        puts(mode==3?"QUEUED CONTROL callback-source-exit OK":"QUEUED CONTROL prepared-leader-exit OK");
    }
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));
}
static void queued_observe_finish(void)
{
    if(!queued_control)return;
    composition_store(&queued_control->command,COMPOSITION_STOP);composition_store(&queued_control->request,1);
    while(composition_load(&queued_control->reply)!=1)sched_yield();
    CHECK(!munmap(queued_control,4096)&&!close(queued_control_fd)&&!unlink("/dev/shm/smp-control"));queued_control=0;
}
