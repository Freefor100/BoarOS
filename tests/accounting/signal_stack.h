/* Fixed work for the alternate-stack cost comparison. Included by cost.c. */
struct stack_cost_worker {
    unsigned cpu, mode, workers;
    pthread_barrier_t *gate;
    uint64_t begin[2], end[2], cpu_ns[2], latency[4096];
    unsigned signals;
};
static _Thread_local volatile sig_atomic_t stack_cost_signals;
static void stack_cost_handler(int signal) { (void)signal;stack_cost_signals++; }
static uint64_t stack_cost_cpu(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));
    return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec;
}
static void stack_cost_gate(pthread_barrier_t *gate)
{ int n=pthread_barrier_wait(gate);CHECK(!n || n==PTHREAD_BARRIER_SERIAL_THREAD); }
static void *stack_cost_run(void *argument)
{
    struct stack_cost_worker *w=argument;
    uint64_t affinity=UINT64_C(1)<<w->cpu;
    CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(affinity),&affinity));
    char *memory=0;
    if(w->mode) {
        memory=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(memory!=MAP_FAILED);memset(memory,0,65536);
        stack_t s={.ss_sp=memory,.ss_size=65536,.ss_flags=w->mode==3?(int)(1U<<31):0};
        CHECK(!syscall(SYS_sigaltstack,&s,0));
    }
    pid_t pid=getpid(),tid=(pid_t)syscall(SYS_gettid);uint64_t cpu;
    stack_cost_signals=0;
    stack_cost_gate(w->gate);
    cpu=stack_cost_cpu();w->begin[0]=now();
    for(unsigned i=0;i<1000000U/w->workers;i++)CHECK(syscall(SYS_getpid)==pid);
    w->end[0]=now();w->cpu_ns[0]=stack_cost_cpu()-cpu;
    stack_cost_gate(w->gate);
    cpu=stack_cost_cpu();w->begin[1]=now();
    for(unsigned i=0;i<4096U/w->workers;i++) {
        uint64_t start=now();CHECK(!syscall(SYS_tgkill,pid,tid,SIGUSR1));
        w->latency[i]=now()-start;CHECK((unsigned)stack_cost_signals==i+1);
    }
    w->end[1]=now();w->cpu_ns[1]=stack_cost_cpu()-cpu;
    w->signals=(unsigned)stack_cost_signals;
    if(memory) {
        stack_t s={.ss_flags=SS_DISABLE};CHECK(!syscall(SYS_sigaltstack,&s,0));
        CHECK(!munmap(memory,65536));
    }
    return 0;
}
static int stack_cost_compare(const void *a,const void *b)
{ uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y); }
static int stack_cost_main(FILE *config)
{
    unsigned cpus,only=0;CHECK(fscanf(config,"%u",&cpus)==1 && (cpus==1 || cpus==4));
    int extra=fscanf(config,"%u",&only);CHECK(extra==EOF || (extra==1 && only==1));CHECK(!fclose(config));
    uint64_t affinity=1;CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(affinity),&affinity));
    stack_t old;errno=0;int available=!syscall(SYS_sigaltstack,0,&old);
    CHECK(available || errno==ENOSYS);
    const char *names[]={"unregistered","registered","onstack","autodisarm"};
    for(unsigned mode=0;mode<(available&&!only?4U:1U);mode++) {
        struct sigaction action={.sa_handler=stack_cost_handler,.sa_flags=mode>=2?SA_ONSTACK:0};
        sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGUSR1,&action,0));
        pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,0,cpus+1));
        struct stack_cost_worker *workers=calloc(cpus,sizeof(*workers));CHECK(workers);
        pthread_t tasks[4];
        for(unsigned i=0;i<cpus;i++) {
            workers[i].cpu=i;workers[i].mode=mode;workers[i].workers=cpus;workers[i].gate=&gate;
            CHECK(!pthread_create(&tasks[i],0,stack_cost_run,&workers[i]));
        }
        stack_cost_gate(&gate);stack_cost_gate(&gate);
        for(unsigned i=0;i<cpus;i++)CHECK(!pthread_join(tasks[i],0));
        uint64_t samples[4096];unsigned count=0;
        for(unsigned i=0;i<cpus;i++) {
            CHECK(workers[i].signals==4096U/cpus);
            for(unsigned j=0;j<4096U/cpus;j++)samples[count++]=workers[i].latency[j];
        }
        qsort(samples,count,sizeof(samples[0]),stack_cost_compare);
        struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
        for(unsigned load=0;load<2;load++) {
            uint64_t begin=UINT64_MAX,end=0,cpu=0;
            for(unsigned i=0;i<cpus;i++) {
                if(workers[i].begin[load]<begin)begin=workers[i].begin[load];
                if(workers[i].end[load]>end)end=workers[i].end[load];
                cpu+=workers[i].cpu_ns[load];
            }
            printf("ALTSTACK-PERF mode=%s name=%s count=%u ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld\n",
                names[mode],load?"signal":"getpid",load?4096:1000000,
                (unsigned long long)(end-begin),(unsigned long long)(load?samples[count/2]:0),
                (unsigned long long)(load?samples[count*95/100]:0),(unsigned long long)(load?samples[count-1]:0),
                (unsigned long long)cpu,usage.ru_maxrss);
        }
        free(workers);CHECK(!pthread_barrier_destroy(&gate));
    }
    printf("ALTSTACK-PERF END %u\n",available&&!only?8:2);fflush(stdout);return 0;
}
