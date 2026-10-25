#define main existing_pi_main
#include "pi.c"
#undef main
struct waitv_item {uint64_t value,address;uint32_t flags,reserved;};
static long waitv_call(struct waitv_item *v,unsigned n,unsigned flags,const struct timespec *t,int clock)
{return syscall(449,v,n,flags,t,clock);}
struct vector_job {struct waitv_item *v;unsigned count,cpu;pid_t tid;int ready[2];long result;int error;};
static void *vector_run(void *p)
{
    struct vector_job *j=p;affinity(j->cpu);j->tid=(pid_t)syscall(SYS_gettid);byte_send(j->ready[1]);
    errno=0;j->result=waitv_call(j->v,j->count,0,0,-1);j->error=errno;return 0;
}
static pthread_t vector_start(struct vector_job *j)
{pthread_t t;CHECK(!pipe(j->ready)&&!pthread_create(&t,0,vector_run,j));byte_read(j->ready[0]);blocked(j->tid);return t;}
static void vector_join(pthread_t t,struct vector_job *j)
{CHECK(!pthread_join(t,0)&&!close(j->ready[0])&&!close(j->ready[1]));}
static struct waitv_item item(uint32_t *p,uint32_t value,int private)
{return (struct waitv_item){.value=value,.address=(uintptr_t)p,.flags=2U|(private?128U:0)};}
static void vector_abi(void)
{
    uint32_t words[2]={1,2};struct waitv_item v[128];
    for(unsigned i=0;i<128;i++)v[i]=item(&words[0],0,1);
    ERROR(waitv_call(v,0,0,0,0),EINVAL);ERROR(waitv_call(v,129,0,0,0),EINVAL);
    ERROR(waitv_call(0,1,0,0,0),EINVAL);ERROR(waitv_call(v,1,1,0,0),EINVAL);
    ERROR(waitv_call((void *)1,1,0,0,0),EFAULT);
    ERROR(waitv_call(v,1,0,0,-1),EAGAIN);ERROR(waitv_call(v,128,0,0,1),EAGAIN);
    v[1].address=1;ERROR(waitv_call(v,2,0,0,1),EINVAL);
    v[1]=item((void *)0,0,0);ERROR(waitv_call(v,2,0,0,1),EFAULT);
    v[0].value=UINT64_C(1)<<32;ERROR(waitv_call(v,1,0,0,1),EINVAL);
    v[0]=item(words,1,1);v[0].reserved=1;ERROR(waitv_call(v,1,0,0,1),EINVAL);
    v[0].reserved=0;struct timespec past={0};ERROR(waitv_call(v,1,0,&past,99),EINVAL);
    ERROR(waitv_call(v,1,0,(void *)1,1),EFAULT);
    ERROR(waitv_call(v,1,0,&past,1),ETIMEDOUT);
    struct timespec bad={.tv_nsec=1000000000};ERROR(waitv_call(v,1,0,&bad,1),EINVAL);
    for(unsigned c=0;c<2;c++) {struct timespec t=after((clockid_t)c,2000000);ERROR(waitv_call(v,1,0,&t,(int)c),ETIMEDOUT);}
    puts("FUTEX-VECTOR CASE abi-deadline-precedence PASS");
}
static void vector_wakes(void)
{
    uint64_t mask=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(mask),&mask)>0);unsigned cpus=__builtin_popcountll(mask);
    for(unsigned cpu=0;cpu<cpus;cpu++)for(unsigned mode=0;mode<3;mode++) {
        uint32_t words[2]={0,0};struct waitv_item v[128];unsigned n=mode==2?128:8;
        for(unsigned i=0;i<n;i++)v[i]=item(&words[0],0,1);
        if(mode==0)v[n-1]=item(&words[1],0,1);
        struct vector_job j={.v=v,.count=n,.cpu=cpu};pthread_t t=vector_start(&j);
        if(mode==1) {
            CHECK(syscall(SYS_futex,&words[0],4|128,0,n,&words[1],0)==n);
            CHECK(syscall(SYS_futex,&words[0],1|128,n,0,0,0)==0);
        }
        long w=syscall(SYS_futex,&words[mode==2?0:1],1|128,n,0,0,0);CHECK(w>=1&&w<=n);
        vector_join(t,&j);CHECK(j.result>=0&&j.result<n);if(mode==0)CHECK(j.result==n-1);
    }
    puts("FUTEX-VECTOR CASE duplicates-requeue-caller-cpus PASS");
}
static void blocked_process(pid_t pid)
{
    char path[64],buf[512];snprintf(path,sizeof(path),"/proc/%d/stat",pid);uint64_t end=stamp()+2000000000;
    for(;;){int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,buf,sizeof(buf)-1);CHECK(n>0&&!close(fd));buf[n]=0;
        char *p=strrchr(buf,')');CHECK(p);if(p[2]=='S')return;CHECK(stamp()<end);sched_yield();}
}
static void vector_shared_exit(void)
{
    uint32_t *words=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(words!=MAP_FAILED);
    for(unsigned kill_child=0;kill_child<2;kill_child++) {
        int ready[2];CHECK(!pipe(ready));pid_t child=fork();CHECK(child>=0);
        if(!child) {struct waitv_item v[2]={item(words,0,0),item(words+1,0,0)};byte_send(ready[1]);CHECK(waitv_call(v,2,0,0,1)==1);_exit(0);}
        byte_read(ready[0]);blocked_process(child);
        if(kill_child) {CHECK(!kill(child,SIGKILL));int st;CHECK(waitpid(child,&st,0)==child&&WIFSIGNALED(st)&&WTERMSIG(st)==SIGKILL);}
        else {CHECK(syscall(SYS_futex,words+1,1,1,0,0,0)==1);wait_ok(child);}
        CHECK(!close(ready[0])&&!close(ready[1]));
        CHECK(syscall(SYS_futex,words,1,128,0,0,0)==0&&syscall(SYS_futex,words+1,1,128,0,0,0)==0);
    }
    CHECK(!munmap(words,4096));puts("FUTEX-VECTOR CASE shared-fork-forced-exit PASS");
}
static void vector_signal(void)
{
    uint32_t word=0;struct waitv_item v=item(&word,0,1);
    struct sigaction sa={.sa_handler=handler};CHECK(!sigaction(SIGUSR1,&sa,0));
    struct vector_job j={.v=&v,.count=1};pthread_t t=vector_start(&j);
    CHECK(!syscall(SYS_tgkill,getpid(),j.tid,SIGUSR1));vector_join(t,&j);CHECK(j.result==-1&&j.error==EINTR);
    sa.sa_flags=SA_RESTART;CHECK(!sigaction(SIGUSR1,&sa,0));
    j=(struct vector_job){.v=&v,.count=1};t=vector_start(&j);sig_atomic_t before=handled;
    CHECK(!syscall(SYS_tgkill,getpid(),j.tid,SIGUSR1));while(handled==before)sched_yield();blocked(j.tid);
    CHECK(syscall(SYS_futex,&word,1|128,1,0,0,0)==1);vector_join(t,&j);CHECK(j.result==0);
    puts("FUTEX-VECTOR CASE signal-and-restart PASS");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);alarm(20);mkdir("/proc",0755);CHECK(!mount("proc","/proc","proc",0,0)||errno==EBUSY);
    vector_abi();vector_wakes();vector_shared_exit();vector_signal();
    puts("FUTEX-VECTOR END 4");alarm(0);if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
