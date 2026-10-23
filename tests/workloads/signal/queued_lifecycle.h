static int queue_exec_fd=-1,queue_exec_ready=-1;
static pthread_t queue_exec_leader;
static void *queued_after_leader(void *unused)
{
    (void)unused;if(pthread_join(queue_exec_leader,0))_exit(51);
    if(write(queue_exec_ready,"R",1)!=1)_exit(52);
    for(unsigned i=0;i<3;i++){struct signalfd_siginfo out;if(read(queue_exec_fd,&out,128)!=128||out.ssi_ptr!=40+i)_exit(53);}
    struct signalfd_siginfo native;if(read(queue_exec_fd,&native,128)!=128||native.ssi_code!=SI_USER||native.ssi_pid!=(unsigned)getppid())_exit(60);
    char fd[16];snprintf(fd,sizeof(fd),"%d",queue_exec_fd);execl("/init","/init","--queued-exec",fd,"43","7","x",(char *)0);_exit(54);
}
static int queued_exec_check(int argc,char **argv)
{
    if(argc!=6)return 55;
    struct signalfd_siginfo out;struct rlimit limit;
    return read(atoi(argv[2]),&out,128)!=128 || out.ssi_ptr!=(uint64_t)strtoull(argv[3],0,0) ||
        getrlimit(RLIMIT_SIGPENDING,&limit) || limit.rlim_cur!=(rlim_t)strtoull(argv[4],0,0) ? 56:0;
}
static void queued_lifecycle(void)
{
    int signal=SIGRTMIN+6;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    struct rlimit original;CHECK(!getrlimit(RLIMIT_SIGPENDING,&original));struct rlimit limited={7,original.rlim_max};CHECK(!setrlimit(RLIMIT_SIGPENDING,&limited));
    int fd=signalfd(-1,&set,SFD_NONBLOCK);CHECK(fd>=0);CHECK(!queued(signal,10));
    pid_t child=fork();CHECK(child>=0);if(!child){struct signalfd_siginfo out;if(!error(read(fd,&out,128),EAGAIN)||queued(signal,11))_exit(57);
        char number[16];snprintf(number,sizeof(number),"%d",fd);execl("/init","/init","--queued-exec",number,"11","7","x",(char *)0);_exit(58);}
    int status;CHECK(waitpid(child,&status,0)==child&&!status);struct signalfd_siginfo out;CHECK(read(fd,&out,128)==128&&out.ssi_ptr==10);CHECK(!close(fd));
    int ready[2];CHECK(!pipe(ready));fd=signalfd(-1,&set,0);CHECK(fd>=0);
    child=fork();CHECK(child>=0);if(!child){close(ready[0]);queue_exec_fd=fd;queue_exec_ready=ready[1];queue_exec_leader=pthread_self();
        pthread_t thread;if(pthread_create(&thread,0,queued_after_leader,0))_exit(59);pthread_exit(0);}
    CHECK(!close(ready[1]));char byte;CHECK(read(ready[0],&byte,1)==1&&byte=='R');
    for(unsigned i=0;i<4;i++){if(i==3)CHECK(!kill(child,signal));siginfo_t info={0};info.si_code=SI_QUEUE;info.si_value.sival_ptr=(void *)(uintptr_t)(40+i);CHECK(!syscall(SYS_rt_sigqueueinfo,child,signal,&info));}
    CHECK(waitpid(child,&status,0)==child&&!status);CHECK(!close(ready[0])&&!close(fd));
    fd=signalfd(-1,&set,SFD_NONBLOCK);CHECK(fd>=0&&!queued(signal,20)&&!queued(signal,21));
    struct sigaction ignore={.sa_handler=SIG_IGN},previous;sigemptyset(&ignore.sa_mask);CHECK(!sigaction(signal,&ignore,&previous));CHECK(error(read(fd,&out,128),EAGAIN));CHECK(!sigaction(signal,&previous,0)&&!close(fd));
    CHECK(!setrlimit(RLIMIT_SIGPENDING,&original)&&!sigprocmask(SIG_SETMASK,&old,0));

}
static void queued_child_times(void)
{
    sigset_t set,old;sigemptyset(&set);sigaddset(&set,SIGCHLD);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    struct sigaction action={.sa_handler=SIG_DFL},previous;sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGCHLD,&action,&previous));
    int fd=signalfd(-1,&set,SFD_NONBLOCK),hold[2];CHECK(fd>=0&&!pipe(hold));
    struct signalfd_siginfo stale;while(read(fd,&stale,128)==128){}CHECK(errno==EAGAIN&&!fcntl(fd,F_SETFL,0));
    pid_t child=fork();CHECK(child>=0);
    if(!child){close(hold[1]);struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);uint64_t start=(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
        do{for(volatile unsigned i=0;i<10000;i++);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);}while((uint64_t)t.tv_sec*1000000000+t.tv_nsec-start<50000000);
        raise(SIGSTOP);char byte;if(read(hold[0],&byte,1)!=1)_exit(63);_exit(0);}
    CHECK(!close(hold[0]));struct signalfd_siginfo out;ssize_t n=read(fd,&out,128);if(n!=128||out.ssi_code!=CLD_STOPPED||out.ssi_pid!=(unsigned)child||(!out.ssi_utime&&!out.ssi_stime))printf("CHILD-TIME got=%ld pid=%u expected=%u code=%d user=%llu system=%llu\n",(long)n,out.ssi_pid,(unsigned)child,out.ssi_code,(unsigned long long)out.ssi_utime,(unsigned long long)out.ssi_stime);CHECK(n==128&&out.ssi_code==CLD_STOPPED&&out.ssi_pid==(unsigned)child&&out.ssi_utime+out.ssi_stime>0);
    CHECK(!kill(child,SIGCONT));CHECK(read(fd,&out,128)==128&&out.ssi_code==CLD_CONTINUED&&out.ssi_utime+out.ssi_stime>0);
    CHECK(write(hold[1],"X",1)==1);int status;CHECK(waitpid(child,&status,0)==child&&!status);CHECK(!close(hold[1])&&!close(fd));
    CHECK(!sigaction(SIGCHLD,&previous,0)&&!sigprocmask(SIG_SETMASK,&old,0));
}

static void queued_fatal(void)
{
    for(unsigned i=0;i<4;i++){pid_t child=fork();CHECK(child>=0);if(!child){if(queued(SIGRTMIN+7,9))_exit(61);_exit(62);}int status;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGRTMIN+7);}
}
static pthread_barrier_t competing_gate;
static unsigned competing_seen[96],competing_total,competing_bad,competing_end;
static pid_t competing_tids[3];
static void competing_claim(uint64_t value)
{
    if(value>=96||__atomic_fetch_add(&competing_seen[value],1,__ATOMIC_RELAXED))__atomic_store_n(&competing_bad,1,__ATOMIC_RELEASE);
    __atomic_fetch_add(&competing_total,1,__ATOMIC_RELEASE);
}
static void competing_handler(int signal,siginfo_t *info,void *context)
{(void)signal;(void)context;competing_claim((uintptr_t)info->si_value.sival_ptr);}
static void *competing_consumer(void *argument)
{
    unsigned mode=(unsigned)(uintptr_t)argument;queue_pin(mode%queue_cpus);competing_tids[mode]=syscall(SYS_gettid);
    int signal=SIGRTMIN+16,stop=SIGRTMIN+17;sigset_t set;sigemptyset(&set);sigaddset(&set,signal);sigaddset(&set,stop);
    int fd=-1;if(mode==1){fd=signalfd(-1,&set,0);if(fd<0)_exit(81);}
    if(mode==2){sigset_t unblocked;sigemptyset(&unblocked);sigaddset(&unblocked,signal);if(pthread_sigmask(SIG_UNBLOCK,&unblocked,0))_exit(82);}
    queue_gate(&competing_gate);
    if(mode==2){while(!__atomic_load_n(&competing_end,__ATOMIC_ACQUIRE))sched_yield();return 0;}
    for(;;){uint64_t value;int got;if(!mode){siginfo_t info;got=sigwaitinfo(&set,&info);value=(uintptr_t)info.si_value.sival_ptr;}
        else {struct signalfd_siginfo info;if(read(fd,&info,128)!=128)_exit(83);got=info.ssi_signo;value=info.ssi_ptr;}
        if(got==stop)break;
        if(got!=signal)_exit(84);
        competing_claim(value);
    }
    if(fd>=0&&close(fd))_exit(85);
    return 0;
}
static void queued_competition(void)
{
    int signal=SIGRTMIN+16,stop=SIGRTMIN+17;sigset_t set,old;sigemptyset(&set);sigaddset(&set,signal);sigaddset(&set,stop);CHECK(!sigprocmask(SIG_BLOCK,&set,&old));
    struct sigaction action={.sa_sigaction=competing_handler,.sa_flags=SA_SIGINFO},previous;sigemptyset(&action.sa_mask);CHECK(!sigaction(signal,&action,&previous));
    pthread_t threads[3];CHECK(!pthread_barrier_init(&competing_gate,0,4));for(unsigned i=0;i<3;i++)CHECK(!pthread_create(&threads[i],0,competing_consumer,(void *)(uintptr_t)i));queue_gate(&competing_gate);
    for(unsigned i=0;i<96;i++){siginfo_t info={0};info.si_code=SI_QUEUE;info.si_value.sival_ptr=(void *)(uintptr_t)i;
        CHECK(!(i<3?syscall(SYS_rt_tgsigqueueinfo,getpid(),competing_tids[i],signal,&info):syscall(SYS_rt_sigqueueinfo,getpid(),signal,&info)));}
    while(__atomic_load_n(&competing_total,__ATOMIC_ACQUIRE)<96)sched_yield();
    for(unsigned i=0;i<2;i++){siginfo_t info={0};info.si_code=SI_QUEUE;CHECK(!syscall(SYS_rt_tgsigqueueinfo,getpid(),competing_tids[i],stop,&info));}
    __atomic_store_n(&competing_end,1,__ATOMIC_RELEASE);for(unsigned i=0;i<3;i++)CHECK(!pthread_join(threads[i],0));
    CHECK(!competing_bad&&competing_total==96);for(unsigned i=0;i<96;i++)CHECK(competing_seen[i]==1);
    CHECK(!pthread_barrier_destroy(&competing_gate)&&!sigaction(signal,&previous,0)&&!sigprocmask(SIG_SETMASK,&old,0));puts("QUEUED COMPETE OK");
}
