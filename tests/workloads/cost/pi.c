/* Existing clocks, affinity and barriers; fixed work for PI mutex consumers. */
#ifndef COST_PI_EMBEDDED
#define main existing_timer_cost_main
#include "../../accounting/posix.c"
#undef main
#endif
struct pi_cost_lock {pthread_mutex_t mutex;uint64_t value,check;};
struct pi_cost_work {
    pthread_barrier_t gate;
    struct pi_cost_lock locks[4];
    unsigned cpus,shared;
    uint64_t samples[32768],cpu[8];
};
struct pi_cost_actor {struct pi_cost_work *work;unsigned id;};
static void *pi_cost_run(void *opaque)
{
    struct pi_cost_actor *a=opaque;struct pi_cost_work *w=a->work;unsigned id=a->id;
    queue_pin((id/2+id%2)%w->cpus);queue_gate(&w->gate);queue_gate(&w->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);
    struct pi_cost_lock *l=&w->locks[w->shared?0:id/2];
    for(unsigned i=0;i<4096;i++){
        uint64_t start=now();CHECK(!pthread_mutex_lock(&l->mutex));w->samples[id*4096+i]=now()-start;
        l->value++;l->check+=((uint64_t)(id+1)<<32)+i+1;
        CHECK(!pthread_mutex_unlock(&l->mutex));
    }
    w->cpu[id]=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;queue_gate(&w->gate);return 0;
}
static void pi_perf(unsigned cpus)
{
    for(unsigned shared=0;shared<2;shared++){
        struct pi_cost_work *w=calloc(1,sizeof(*w));CHECK(w);w->cpus=cpus;w->shared=shared;
        for(unsigned i=0;i<32768;i+=512)((volatile uint64_t *)w->samples)[i]=0;
        pthread_mutexattr_t attr;CHECK(!pthread_mutexattr_init(&attr)&&!pthread_mutexattr_setprotocol(&attr,PTHREAD_PRIO_INHERIT));
        for(unsigned i=0;i<4;i++)CHECK(!pthread_mutex_init(&w->locks[i].mutex,&attr));
        CHECK(!pthread_mutexattr_destroy(&attr));
        CHECK(!pthread_barrier_init(&w->gate,0,9));pthread_t threads[8];struct pi_cost_actor actors[8];
        for(unsigned i=0;i<8;i++){actors[i]=(struct pi_cost_actor){w,i};CHECK(!pthread_create(&threads[i],0,pi_cost_run,&actors[i]));}
        queue_gate(&w->gate);uint64_t begin=now();queue_gate(&w->gate);queue_gate(&w->gate);uint64_t elapsed=now()-begin,cpu=0,value=0,check=0;
        for(unsigned i=0;i<8;i++){CHECK(!pthread_join(threads[i],0));cpu+=w->cpu[i];}
        for(unsigned i=0;i<4;i++){value+=w->locks[i].value;check+=w->locks[i].check;CHECK(!pthread_mutex_destroy(&w->locks[i].mutex));}
        CHECK(value==32768 && check==UINT64_C(633318764724224));qsort(w->samples,32768,sizeof(uint64_t),queue_compare);
        struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
        printf("PI-PERF name=%s cpus=%u count=32768 ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld check=%llu\n",shared?"mutex1":"mutex4",cpus,(unsigned long long)elapsed,(unsigned long long)w->samples[16384],(unsigned long long)w->samples[31129],(unsigned long long)w->samples[32767],(unsigned long long)cpu,usage.ru_maxrss,(unsigned long long)check);
        CHECK(!pthread_barrier_destroy(&w->gate));free(w);
    }
    puts("PI-PERF END 2");
}
#ifndef COST_PI_EMBEDDED
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);uint64_t mask=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(mask),&mask)>0);
    unsigned cpus=__builtin_popcountll(mask);CHECK(cpus==1||cpus==2||cpus==4);pi_perf(cpus);return 0;
}
#endif
