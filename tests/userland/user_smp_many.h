/* Exercise each online executor with the same user-visible operations. */
#include <sys/time.h>
static _Thread_local unsigned many_signals;
static atomic_uint many_virtual, many_prof;
struct many_cpu_work {
    unsigned cpu;
    pthread_barrier_t *barrier;
    unsigned char *shared;
    uint64_t checksum, cpu_ns;
};
static uint64_t many_thread_time(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));
    return (uint64_t)t.tv_sec*1000000000U+t.tv_nsec;
}
static void many_signal(int signal)
{
    CHECK(tls_value==73);
    if(signal==SIGUSR1){many_signals++;CHECK(!raise(SIGUSR2));}
    else if(signal==SIGUSR2)many_signals++;
    else if(signal==SIGVTALRM)atomic_fetch_add_explicit(&many_virtual,1,memory_order_relaxed);
    else {CHECK(signal==SIGPROF);atomic_fetch_add_explicit(&many_prof,1,memory_order_relaxed);}
}
static void many_barrier(pthread_barrier_t *b)
{int r=pthread_barrier_wait(b);CHECK(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void *many_cpu_worker(void *argument)
{
    struct many_cpu_work *w=argument;pin(w->cpu);tls_value=73;many_signals=0;
    sigset_t timers;sigemptyset(&timers);sigaddset(&timers,SIGVTALRM);sigaddset(&timers,SIGPROF);
    CHECK(!pthread_sigmask(SIG_UNBLOCK,&timers,NULL));
    many_barrier(w->barrier);
    uint64_t before=many_thread_time(),value=1;
    CHECK(!user_smp_register_probe(1) && many_signals==2);
    for(unsigned i=0;i<5000000;i++) {value=(value<<3)^(value>>7)^17;__asm__ volatile("" : "+r"(value));}
    w->checksum=value;w->cpu_ns=many_thread_time()-before;CHECK(w->cpu_ns && tls_value==73);
    /* All threads enter the same initially absent shared page. */
    w->shared[w->cpu*64]=(unsigned char)(71+w->cpu);
    unsigned char *private=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(private!=MAP_FAILED);private[0]=91;
    CHECK(!mprotect(private,4096,PROT_READ) && private[0]==91 &&
        !mprotect(private,4096,PROT_READ|PROT_WRITE));
    private[0]=92;CHECK(!munmap(private,4096));
    char directory[48],name[80];snprintf(directory,sizeof(directory),"/many-cpu-%u",w->cpu);
    CHECK(!mkdir(directory,0700));
    for(unsigned i=0;i<16;i++) {
        snprintf(name,sizeof(name),"%s/file-%u",directory,i);
        int fd=open(name,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
        unsigned char byte=(unsigned char)(33+w->cpu);
        CHECK(pwrite(fd,&byte,1,0)==1 && !ftruncate(fd,4096));
        unsigned char *mapped=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0);
        CHECK(mapped!=MAP_FAILED && mapped[0]==byte && !unlink(name));
        CHECK(!ftruncate(fd,0));expected_fault=SIGBUS;
        if(!sigsetjmp(fault_return,1)){volatile unsigned char lost=mapped[0];(void)lost;CHECK(0);}
        expected_fault=0;
        CHECK(!ftruncate(fd,4096) && mapped[0]==0 && pwrite(fd,&byte,1,0)==1 &&
            mapped[0]==byte);
        unsigned char *cow=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
        CHECK(cow!=MAP_FAILED && cow[0]==byte);cow[0]=byte+1;
        CHECK(!fallocate(fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,0,4096) &&
            mapped[0]==0 && cow[0]==byte+1 && !munmap(cow,4096));
        CHECK(pwrite(fd,&byte,1,0)==1 && mapped[0]==byte && !fsync(fd) &&
            !munmap(mapped,4096) && !close(fd));
    }
    int parent=open(directory,O_RDONLY|O_DIRECTORY);CHECK(parent>=0 && !fsync(parent) && !close(parent));
    CHECK(!rmdir(directory));
    CHECK(!pthread_sigmask(SIG_BLOCK,&timers,NULL));return NULL;
}
static void many_cpu_consumers(void)
{
    unsigned count=(unsigned)__builtin_popcountll(online);CHECK(count && count<=4 && online==((UINT64_C(1)<<count)-1));
    const int signals[]={SIGUSR1,SIGUSR2,SIGVTALRM,SIGPROF};struct sigaction old[4];
    struct sigaction action={.sa_handler=many_signal,.sa_flags=SA_NODEFER};sigemptyset(&action.sa_mask);
    for(unsigned i=0;i<4;i++)CHECK(!sigaction(signals[i],&action,&old[i]));
    sigset_t timers,saved;sigemptyset(&timers);sigaddset(&timers,SIGVTALRM);sigaddset(&timers,SIGPROF);
    CHECK(!pthread_sigmask(SIG_BLOCK,&timers,&saved));
    atomic_store(&many_virtual,0);atomic_store(&many_prof,0);
    struct itimerval tick={.it_interval={0,10000},.it_value={0,10000}},zero={0};
    CHECK(!setitimer(ITIMER_VIRTUAL,&tick,NULL) && !setitimer(ITIMER_PROF,&tick,NULL));
    pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,count+1));
    unsigned char *shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(shared!=MAP_FAILED);
    struct many_cpu_work work[4]={0};pthread_t threads[4];
    for(unsigned i=0;i<count;i++) {work[i]=(struct many_cpu_work){.cpu=i,.barrier=&gate,.shared=shared};CHECK(!pthread_create(&threads[i],NULL,many_cpu_worker,&work[i]));}
    many_barrier(&gate);
    for(unsigned i=0;i<count;i++)CHECK(!pthread_join(threads[i],NULL) && work[i].checksum==work[0].checksum && work[i].checksum && shared[i*64]==71+i);
    CHECK(atomic_load(&many_virtual) && atomic_load(&many_prof));
    CHECK(!setitimer(ITIMER_VIRTUAL,&zero,NULL) && !setitimer(ITIMER_PROF,&zero,NULL));
    struct timespec timeout={0};
    for(;;) {int r=(int)syscall(SYS_rt_sigtimedwait,&timers,NULL,&timeout,8);if(r>=0 || errno==EINTR)continue;CHECK(errno==EAGAIN);break;}
    for(unsigned i=0;i<4;i++)CHECK(!sigaction(signals[i],&old[i],NULL));
    CHECK(!pthread_sigmask(SIG_SETMASK,&saved,NULL) && !pthread_barrier_destroy(&gate) && !munmap(shared,4096));
    record("all-cpu-tls-fpu-signals-timers");record("all-cpu-mm-files-orphans");

    pid_t children[4];int ready[2],release[2];CHECK(!pipe(ready) && !pipe(release));
    for(unsigned i=0;i<count;i++) {
        children[i]=fork();CHECK(children[i]>=0);
        if(!children[i]) {
            uint64_t inherited=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(inherited),&inherited)==sizeof(inherited) && inherited==1);
            pin(i);CHECK(!close(ready[0]) && !close(release[1]));
            char byte='r';CHECK(write(ready[1],&byte,1)==1 && read(release[0],&byte,1)==1);
            CHECK(!close(ready[1]) && !close(release[0]));
            char cpu[16];snprintf(cpu,sizeof(cpu),"%u",i);execl("/init","init","--many-exec",cpu,(char *)NULL);_exit(98);
        }
    }
    CHECK(!close(ready[1]) && !close(release[0]));
    for(unsigned i=0;i<count;i++){char byte;CHECK(read(ready[0],&byte,1)==1);}
    CHECK(write(release[1],"gggg",count)==(ssize_t)count && !close(ready[0]) && !close(release[1]));
    for(unsigned i=0;i<count;i++){int status;CHECK(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && WEXITSTATUS(status)==40+(int)i);}
    record("all-cpu-fork-affinity-exec");pin(0);
}

struct many_service { unsigned cpu; pthread_barrier_t *gate; atomic_uint *done; };
static void *many_service_worker(void *argument)
{
    struct many_service *w=argument;pin(w->cpu);
    set_policy(SCHED_FIFO,10);
    many_barrier(w->gate);
    int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    struct ifreq query={0};memcpy(query.ifr_name,"lo",3);
    CHECK(!ioctl(fd,SIOCGIFMTU,&query) && query.ifr_mtu>0 && !close(fd));
    set_policy(SCHED_OTHER,0);
    atomic_fetch_add_explicit(w->done,1,memory_order_release);return NULL;
}
static void many_service_progress(void)
{
    unsigned count=(unsigned)__builtin_popcountll(online);pin(0);
    if(count>1) {
        pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,count));
        atomic_uint done=0;pthread_t threads[3];struct many_service work[3];
        for(unsigned i=1;i<count;i++) {work[i-1]=(struct many_service){i,&gate,&done};CHECK(!pthread_create(&threads[i-1],NULL,many_service_worker,&work[i-1]));}
        set_policy(SCHED_FIFO,20);
        many_barrier(&gate);struct timespec start,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));
        while(atomic_load_explicit(&done,memory_order_acquire)!=count-1) {
            CHECK(!clock_gettime(CLOCK_MONOTONIC,&now) && now.tv_sec-start.tv_sec<5);
        }
        set_policy(SCHED_OTHER,0);
        for(unsigned i=1;i<count;i++)CHECK(!pthread_join(threads[i-1],NULL));
        CHECK(!pthread_barrier_destroy(&gate));
    }
    record("all-remote-realtime-services");
}
