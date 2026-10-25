#define main previous_cost_main
#include "../../accounting/posix.c"
#undef main
#define COST_PI_EMBEDDED 1
#include "pi.c"
struct next_pair {uint32_t word[8],mutex,turn,request,response;};
struct next_work {pthread_barrier_t gate;struct next_pair pairs[4];unsigned cpus,mode;uint64_t samples[4096],cpu[8],check[8];};
struct next_actor {struct next_work *work;unsigned id;};
static long next_futex(uint32_t *p,unsigned op,unsigned v,uint64_t n,uint32_t *q,unsigned expect)
{return syscall(SYS_futex,p,op|128,v,n,q,expect);}
static void next_lock(struct next_pair *p)
{CHECK(!next_futex(&p->mutex,13,0,0,0,0));CHECK((p->mutex&0x3fffffffU)==(uint32_t)syscall(SYS_gettid));}
static void next_unlock(struct next_pair *p)
{CHECK(!next_futex(&p->mutex,7,0,0,0,0));}
static void next_condition(struct next_pair *p,unsigned role)
{
    while(p->turn!=role) {
        uint32_t previous=__atomic_load_n(&p->word[role],__ATOMIC_ACQUIRE);next_unlock(p);
        long result=next_futex(&p->word[role],11,previous,0,&p->mutex,0);
        if(result){CHECK(errno==EAGAIN);next_lock(p);}
        else CHECK((p->mutex&0x3fffffffU)==(uint32_t)syscall(SYS_gettid));
    }
}
static void next_notify(struct next_pair *p,unsigned role)
{
    uint32_t value=__atomic_add_fetch(&p->word[role],1,__ATOMIC_RELEASE);
    CHECK(next_futex(&p->word[role],12,1,0,&p->mutex,value)>=0);
}
static void next_vector(struct next_pair *p,unsigned role,uint32_t sequence)
{
    struct {uint64_t value,address;uint32_t flags,reserved;} vector[8];
    for(unsigned i=0;i<8;i++)vector[i]=(typeof(*vector)){.address=(uintptr_t)(i==7?&p->turn:&p->word[i?i+1:role]),.flags=130};
    /* Seven distinct inert words plus the actual per-role sequence. */
    for(;;) {
        uint32_t actual=__atomic_load_n(&p->word[role],__ATOMIC_ACQUIRE);
        if(actual==sequence)return;
        CHECK(actual<sequence);vector[0].value=actual;
        long result=syscall(449,vector,8,0,0,CLOCK_MONOTONIC);
        CHECK(result==0||(result==-1&&errno==EAGAIN));
    }
}
static void *next_run(void *opaque)
{
    struct next_actor *a=opaque;struct next_work *w=a->work;unsigned role=a->id&1,pair=a->id/2;
    struct next_pair *p=&w->pairs[pair];queue_pin((pair+role)%w->cpus);queue_gate(&w->gate);queue_gate(&w->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID),check=0;
    for(unsigned i=1;i<=1024;i++) {
        uint64_t begin=role?0:now();
        if(!w->mode) {
            if(!role){__atomic_store_n(&p->word[0],i,__ATOMIC_RELEASE);CHECK(next_futex(&p->word[0],1,1,0,0,0)>=0);next_vector(p,1,i);}
            else {next_vector(p,0,i);__atomic_store_n(&p->word[1],i,__ATOMIC_RELEASE);CHECK(next_futex(&p->word[1],1,1,0,0,0)>=0);}
        } else {
            next_lock(p);next_condition(p,role);
            if(!role) {
                p->request=i;p->turn=1;next_notify(p,1);next_condition(p,0);CHECK(p->response==i);
            } else {CHECK(p->request==i);p->response=i;p->turn=0;next_notify(p,0);}
            next_unlock(p);
        }
        check+=i;if(!role)w->samples[pair*1024+i-1]=now()-begin;
    }
    w->cpu[a->id]=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;w->check[a->id]=check;queue_gate(&w->gate);return 0;
}
static void next_perf(unsigned cpus)
{
    for(unsigned mode=0;mode<2;mode++) {
        struct next_work *w=calloc(1,sizeof(*w));CHECK(w);w->cpus=cpus;w->mode=mode;
        CHECK(!pthread_barrier_init(&w->gate,0,9));pthread_t threads[8];struct next_actor actors[8];
        for(unsigned i=0;i<8;i++){actors[i]=(struct next_actor){w,i};CHECK(!pthread_create(&threads[i],0,next_run,&actors[i]));}
        queue_gate(&w->gate);uint64_t begin=now();queue_gate(&w->gate);queue_gate(&w->gate);uint64_t elapsed=now()-begin,cpu=0,check=0;
        for(unsigned i=0;i<8;i++){CHECK(!pthread_join(threads[i],0));CHECK(w->check[i]==524800);cpu+=w->cpu[i];check+=w->check[i];}
        qsort(w->samples,4096,sizeof(uint64_t),queue_compare);struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
        printf("FUTEX-NEXT-PERF name=%s cpus=%u count=4096 ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld check=%llu\n",
            mode?"requeue-pi":"waitv8",cpus,(unsigned long long)elapsed,(unsigned long long)w->samples[2048],
            (unsigned long long)w->samples[3891],(unsigned long long)w->samples[4095],(unsigned long long)cpu,usage.ru_maxrss,(unsigned long long)check);
        CHECK(!pthread_barrier_destroy(&w->gate));free(w);
    }
    puts("FUTEX-NEXT-PERF END 2");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);FILE *config=fopen("/account-posix","r");unsigned kind,cpus;
    CHECK(config&&fscanf(config,"%u %u",&kind,&cpus)==2&&!fclose(config));CHECK(!kind&&(cpus==1||cpus==2||cpus==4));
    if(!access("/futex-next",F_OK))next_perf(cpus);
    else {CHECK(!previous_cost_main());uint64_t mask=(UINT64_C(1)<<cpus)-1;CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask));pi_perf(cpus);puts("FUTEX-COST END 8");}
    return 0;
}
