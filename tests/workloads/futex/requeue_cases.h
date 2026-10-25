struct requeue_job {
    uint32_t *source,*target;unsigned private,cpu,priority;
    pid_t tid;int ready[2];struct timespec timeout;
    long result;int error;unsigned owned,order;unsigned *sequence;
};
static void *requeue_actor(void *opaque)
{
    struct requeue_job *j=opaque;affinity(j->cpu);j->tid=(pid_t)syscall(SYS_gettid);
    if(j->priority)policy(SCHED_FIFO,(int)j->priority);
    byte_send(j->ready[1]);errno=0;
    j->result=syscall(SYS_futex,j->source,11|(j->private?128:0),0,
        j->timeout.tv_sec||j->timeout.tv_nsec?&j->timeout:0,j->target,0);j->error=errno;
    if(!j->result){j->owned=(*j->target&0x3fffffffU)==(uint32_t)j->tid;CHECK(j->owned);
        if(j->sequence)j->order=__atomic_fetch_add(j->sequence,1,__ATOMIC_SEQ_CST);
        CHECK(!pi(j->target,7|(j->private?128:0),0));}
    if(j->priority)policy(SCHED_OTHER,0);
    return 0;
}
static pthread_t requeue_start(struct requeue_job *j)
{pthread_t t;CHECK(!pipe(j->ready)&&!pthread_create(&t,0,requeue_actor,j));byte_read(j->ready[0]);blocked(j->tid);return t;}
static void requeue_join(pthread_t t,struct requeue_job *j)
{CHECK(!pthread_join(t,0)&&!close(j->ready[0])&&!close(j->ready[1]));}
static long requeue(uint32_t *source,uint32_t *target,unsigned n,int private)
{return syscall(SYS_futex,source,12|(private?128:0),1,n,target,0);}
static void requeue_handoffs(void)
{
    uint64_t mask=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(mask),&mask)>0);unsigned cpus=__builtin_popcountll(mask);
    for(unsigned shared=0;shared<2;shared++)for(unsigned held=0;held<2;held++) {
        uint32_t *words=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(words!=MAP_FAILED);
        if(held)CHECK(!pi(words+1,6|(shared?0:128),0));
        struct requeue_job jobs[3]={0};pthread_t threads[3];unsigned sequence=0;
        for(unsigned i=0;i<3;i++) {jobs[i]=(struct requeue_job){.source=words,.target=words+1,.private=!shared,.cpu=i%cpus,.priority=(i+1)*10,.sequence=&sequence};threads[i]=requeue_start(&jobs[i]);}
        CHECK(requeue(words,words+1,2,!shared)==3);
        if(held){char state;CHECK(task_field((pid_t)syscall(SYS_gettid),18,&state)==-31);CHECK(!pi(words+1,7|(shared?0:128),0));}
        for(unsigned i=0;i<3;i++){requeue_join(threads[i],&jobs[i]);CHECK(!jobs[i].result&&jobs[i].owned&&jobs[i].order==2-i);}
        CHECK(!words[1]&&sequence==3&&!munmap(words,4096));
    }
    puts("FUTEX-VECTOR CASE pi-proxy-broadcast-priority-shared PASS");
}
static void requeue_mixed(void)
{
    uint32_t words[3]={0};struct requeue_job j={.source=words,.target=words+1,.private=1};pthread_t t=requeue_start(&j);
    ERROR(syscall(SYS_futex,words,1|128,1,0,0,0),EINVAL);
    ERROR(syscall(SYS_futex,words,4|128,0,1,words+1,0),EINVAL);
    ERROR(requeue(words,words+2,0,1),EINVAL);
    CHECK(requeue(words,words+1,0,1)==1);requeue_join(t,&j);CHECK(j.result==0&&j.owned);
    struct waitv_item v=item(words,0,1);struct vector_job normal={.v=&v,.count=1};t=vector_start(&normal);
    ERROR(requeue(words,words+1,0,1),EINVAL);
    CHECK(syscall(SYS_futex,words,1|128,1,0,0,0)==1);vector_join(t,&normal);CHECK(normal.result==0);
    puts("FUTEX-VECTOR CASE pi-mismatched-wake-requeue PASS");
}
static void requeue_signal_timeout(void)
{
    uint32_t words[2]={0};struct sigaction sa={.sa_handler=handler};CHECK(!sigaction(SIGUSR1,&sa,0));
    for(unsigned migrated=0;migrated<2;migrated++) {
        CHECK(!pi(words+1,6|128,0));
        struct requeue_job j={.source=words,.target=words+1,.private=1};pthread_t t=requeue_start(&j);
        if(migrated)CHECK(requeue(words,words+1,0,1)==1);
        sig_atomic_t before=handled;CHECK(!syscall(SYS_tgkill,getpid(),j.tid,SIGUSR1));while(before==handled)sched_yield();
        if(!migrated){blocked(j.tid);CHECK(requeue(words,words+1,0,1)==1);CHECK(!pi(words+1,7|128,0));}
        requeue_join(t,&j);
        if(migrated){CHECK(j.result==-1&&j.error==EAGAIN&&!j.owned);CHECK(!pi(words+1,7|128,0));}
        else CHECK(!j.result&&j.owned);
    }
    for(unsigned migrated=0;migrated<2;migrated++) {
        CHECK(!pi(words+1,6|128,0));
        struct requeue_job j={.source=words,.target=words+1,.private=1,.timeout=after(CLOCK_MONOTONIC,30000000)};
        pthread_t t=requeue_start(&j);if(migrated)CHECK(requeue(words,words+1,0,1)==1);
        requeue_join(t,&j);CHECK(j.result==-1&&j.error==ETIMEDOUT);CHECK(!pi(words+1,7|128,0));
    }
    puts("FUTEX-VECTOR CASE pi-phase-signal-timeout PASS");
}
static void requeue_faults_and_exit(void)
{
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint32_t *words=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(words!=MAP_FAILED);
    uint32_t *target=(void *)((char *)words+page);*target=0;
    CHECK(!mprotect(target,page,PROT_READ));
    ERROR(syscall(SYS_futex,words,12,1,0,target,1),EFAULT);
    ERROR(syscall(SYS_futex,words,11,1,0,target,0),EFAULT);
    ERROR(syscall(SYS_futex,words,12|128,1,0,target,1),EAGAIN);
    CHECK(!mprotect(target,page,PROT_READ|PROT_WRITE));
    struct requeue_job j={.source=words,.target=target,.private=1};pthread_t t=requeue_start(&j);
    CHECK(!mprotect(target,page,PROT_READ));ERROR(requeue(words,target,0,1),EFAULT);
    CHECK(!mprotect(target,page,PROT_READ|PROT_WRITE));CHECK(requeue(words,target,0,1)==1);requeue_join(t,&j);CHECK(!j.result&&j.owned);
    for(unsigned migrated=0;migrated<2;migrated++) {
        CHECK(!pi(target,6,0));int ready[2];CHECK(!pipe(ready));pid_t child=fork();CHECK(child>=0);
        if(!child){byte_send(ready[1]);(void)syscall(SYS_futex,words,11,0,0,target,0);_exit(7);}
        byte_read(ready[0]);blocked_process(child);if(migrated)CHECK(requeue(words,target,0,0)==1);
        CHECK(!kill(child,SIGKILL));int st;CHECK(waitpid(child,&st,0)==child&&WIFSIGNALED(st)&&WTERMSIG(st)==SIGKILL);
        CHECK(!pi(target,7,0)&&syscall(SYS_futex,words,1,128,0,0,0)==0);
        CHECK(!close(ready[0])&&!close(ready[1]));
    }
    struct dying d={.target=target,.shared=1};CHECK(!pipe(d.ready)&&!pipe(d.release));pthread_t owner;
    CHECK(!pthread_create(&owner,0,dying_owner,&d));byte_read(d.ready[0]);
    j=(struct requeue_job){.source=words,.target=target};t=requeue_start(&j);CHECK(requeue(words,target,0,0)==1);
    byte_send(d.release[1]);CHECK(!pthread_join(owner,0));requeue_join(t,&j);CHECK(!j.result&&j.owned&&! *target);
    CHECK(!close(d.ready[0])&&!close(d.ready[1])&&!close(d.release[0])&&!close(d.release[1]));
    CHECK(!munmap(words,2*page));puts("FUTEX-VECTOR CASE pi-access-fault-owner-waiter-exit PASS");
}
static void requeue_partial(void)
{
    uint32_t words[3]={0};CHECK(!pi(words+1,6|128,0));
    struct requeue_job a={.source=words,.target=words+1,.private=1},b={.source=words,.target=words+2,.private=1};
    pthread_t ta=requeue_start(&a),tb=requeue_start(&b);
    ERROR(requeue(words,words+1,1,1),EINVAL);CHECK(!pi(words+1,7|128,0));
    requeue_join(ta,&a);CHECK(!a.result&&a.owned);
    CHECK(requeue(words,words+2,0,1)==1);requeue_join(tb,&b);CHECK(!b.result&&b.owned);
    puts("FUTEX-VECTOR CASE pi-error-completed-prefix PASS");
}
