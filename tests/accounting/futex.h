/* One ELF compares fixed WAIT/WAKE work on both the old and new kernels. */
enum { FUTEX_PERF_WAIT_PRIVATE=128,FUTEX_PERF_WAKE_PRIVATE=129 };
struct futex_perf_pair { uint32_t sequence;uint64_t request,reply; };
struct futex_perf_mutex { uint32_t state;uint64_t value,check; };
struct futex_perf_work {
    unsigned cpus,mode,completed[8],wait_calls[8],wake_calls[8];
    pthread_barrier_t gate;
    struct futex_perf_pair pairs[4];
    struct futex_perf_mutex locks[4];
    uint64_t cpu[8],samples[32768];
};
struct futex_perf_actor { struct futex_perf_work *work;unsigned id; };
static _Thread_local unsigned futex_perf_wait_calls,futex_perf_wake_calls;
static void futex_perf_wait(uint32_t *word,uint32_t value)
{
    futex_perf_wait_calls++;
    int result=(int)syscall(SYS_futex,word,FUTEX_PERF_WAIT_PRIVATE,value,0,0,0);
    CHECK(!result || (result==-1 && (errno==EAGAIN || errno==EINTR)));
}
static void futex_perf_wake(uint32_t *word)
{ futex_perf_wake_calls++;CHECK(syscall(SYS_futex,word,FUTEX_PERF_WAKE_PRIVATE,1,0,0,0)>=0); }
static void futex_perf_until(uint32_t *word,uint32_t expected)
{
    for(;;) {
        uint32_t value=__atomic_load_n(word,__ATOMIC_ACQUIRE);
        if(value==expected)return;
        CHECK(value<expected);futex_perf_wait(word,value);
    }
}
static void futex_perf_lock(struct futex_perf_mutex *lock)
{
    uint32_t zero=0;
    if(__atomic_compare_exchange_n(&lock->state,&zero,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    while(__atomic_exchange_n(&lock->state,2,__ATOMIC_ACQUIRE))futex_perf_wait(&lock->state,2);
}
static void futex_perf_unlock(struct futex_perf_mutex *lock)
{
    uint32_t state=__atomic_exchange_n(&lock->state,0,__ATOMIC_RELEASE);
    CHECK(state==1 || state==2);if(state==2)futex_perf_wake(&lock->state);
}
static uint64_t futex_perf_value(unsigned pair,unsigned round)
{ return UINT64_C(0xabcdef1200000000)+(uint64_t)pair*1024+round; }
static void *futex_perf_run(void *argument)
{
    struct futex_perf_actor *actor=argument;struct futex_perf_work *w=actor->work;
    unsigned id=actor->id,pair=id/2,role=id%2;
    futex_perf_wait_calls=futex_perf_wake_calls=0;
    queue_pin((pair+role)%w->cpus);queue_gate(&w->gate);queue_gate(&w->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);
    if(!w->mode) {
        struct futex_perf_pair *p=&w->pairs[pair];
        for(unsigned i=0;i<1024;i++) {
            uint64_t value=futex_perf_value(pair,i),start=0;
            if(!role) {
                start=now();p->request=value;
                __atomic_store_n(&p->sequence,2*i+1,__ATOMIC_RELEASE);futex_perf_wake(&p->sequence);
                futex_perf_until(&p->sequence,2*i+2);CHECK(p->reply==~value);
                w->samples[pair*1024+i]=now()-start;
            } else {
                futex_perf_until(&p->sequence,2*i+1);CHECK(p->request==value);p->reply=~value;
                __atomic_store_n(&p->sequence,2*i+2,__ATOMIC_RELEASE);futex_perf_wake(&p->sequence);
            }
        }
        w->completed[id]=1024;
    } else {
        struct futex_perf_mutex *lock=&w->locks[w->mode==1?pair:0];
        for(unsigned i=0;i<4096;i++) {
            uint64_t start=now();futex_perf_lock(lock);
            w->samples[id*4096+i]=now()-start;
            lock->value++;lock->check+=((uint64_t)(id+1)<<32)+i+1;
            futex_perf_unlock(lock);
        }
        w->completed[id]=4096;
    }
    w->cpu[id]=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    w->wait_calls[id]=futex_perf_wait_calls;w->wake_calls[id]=futex_perf_wake_calls;
    queue_gate(&w->gate);return 0;
}
static void futex_perf(unsigned cpus)
{
    queue_pin(0);
    const char *names[]={"roundtrip","mutex4","mutex1"};
    for(unsigned mode=0;mode<3;mode++) {
        struct futex_perf_work *w=calloc(1,sizeof(*w));CHECK(w);w->cpus=cpus;w->mode=mode;
        /* calloc can leave demand-zero sample pages untouched until the timed work. */
        for(unsigned i=0;i<32768;i+=512)((volatile uint64_t *)w->samples)[i]=0;
        CHECK(!pthread_barrier_init(&w->gate,0,9));pthread_t threads[8];struct futex_perf_actor actors[8];
        for(unsigned i=0;i<8;i++) {
            actors[i]=(struct futex_perf_actor){w,i};CHECK(!pthread_create(&threads[i],0,futex_perf_run,&actors[i]));
        }
        queue_gate(&w->gate);uint64_t begin=now();queue_gate(&w->gate);queue_gate(&w->gate);
        uint64_t cpu=0,check=0;unsigned completed=0,wait_calls=0,wake_calls=0;
        for(unsigned i=0;i<8;i++) {
            CHECK(w->completed[i]==(mode?4096U:1024U));completed++;cpu+=w->cpu[i];
            wait_calls+=w->wait_calls[i];wake_calls+=w->wake_calls[i];
        }
        if(!mode) {
            for(unsigned i=0;i<4;i++) {
                CHECK(w->pairs[i].sequence==2048 && w->pairs[i].request==futex_perf_value(i,1023) &&
                      w->pairs[i].reply==~futex_perf_value(i,1023));check+=w->pairs[i].reply;
            }
        } else {
            for(unsigned i=0;i<(mode==1?4U:1U);i++) {
                uint64_t expected=0;
                for(unsigned id=0;id<8;id++)if(mode==2 || id/2==i)
                    expected+=((uint64_t)(id+1)<<32)*4096+UINT64_C(4096)*4097/2;
                CHECK(!w->locks[i].state && w->locks[i].value==(mode==1?8192U:32768U) &&
                      w->locks[i].check==expected);check+=w->locks[i].check;
            }
        }
        uint64_t elapsed=now()-begin;
        for(unsigned i=0;i<8;i++)CHECK(!pthread_join(threads[i],0));
        unsigned count=mode?32768:4096;qsort(w->samples,count,sizeof(*w->samples),queue_compare);
        struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
        printf("FUTEX-PERF name=%s cpus=%u workers=8 count=%u ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld completed=%u check=%llu wait_calls=%u wake_calls=%u\n",
            names[mode],cpus,count,(unsigned long long)elapsed,(unsigned long long)w->samples[count/2],
            (unsigned long long)w->samples[count*95/100],(unsigned long long)w->samples[count-1],
            (unsigned long long)cpu,usage.ru_maxrss,completed,(unsigned long long)check,wait_calls,wake_calls);
        CHECK(!pthread_barrier_destroy(&w->gate));free(w);
    }
    puts("FUTEX-PERF END 3");
}
