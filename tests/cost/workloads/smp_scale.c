/* One fixed-work ELF per architecture is used unchanged on every kernel/CPU
 * configuration. Setup, thread creation and cleanup are outside the spans. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define main existing_mqueue_pipe_main
#include "mqueue_pipe.c"
#undef main
#pragma GCC diagnostic pop
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include "file_window.h"

enum { SCALE_GETPIDS=1000000, SCALE_SIGNALS=4096, SCALE_COMPUTE=100000000,
       SCALE_ROUNDS=4096, SCALE_MESSAGE=64, SCALE_FILE_ROUNDS=1024,
       SCALE_CPU_SAMPLES=16 };

static void scale_report(const char *name,struct clock_span *s,unsigned workers,
    uint64_t *samples,unsigned count,uint64_t work,uint64_t bytes,uint64_t checks)
{
    uint64_t wall,cpu;spans(s,workers,&wall,&cpu);
    metric(name,wall,cpu,samples,count,work,bytes,checks,workers);
    for(unsigned i=0;i<workers;i++)
        printf("SMP-SCALE ACTOR name=%s actor=%u begin_ns=%llu end_ns=%llu cpu_ns=%llu\n",
            name,i,(unsigned long long)s[i].begin,(unsigned long long)s[i].end,
            (unsigned long long)s[i].cpu);
}
static void scale_samples(const char *name,unsigned actor,uint64_t *samples,unsigned count)
{
    CHECK(count);qsort(samples,count,sizeof(*samples),queue_compare);
    printf("SMP-SCALE SAMPLES name=%s actor=%u count=%u p50_ns=%llu p95_ns=%llu max_ns=%llu\n",
        name,actor,count,(unsigned long long)samples[count/2],
        (unsigned long long)samples[count*95/100],(unsigned long long)samples[count-1]);
}

static void scale_legacy(void)
{
    queue_unbound=0;
    unsigned getpids=smoke?4096:SCALE_GETPIDS,signals=smoke?64:SCALE_SIGNALS;
    struct sigaction action={.sa_handler=ordinary_signal},old_action;
    sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGUSR1,&action,&old_action));
    sigset_t set,old_set;sigemptyset(&set);sigaddset(&set,SIGUSR1);
    CHECK(!pthread_sigmask(SIG_UNBLOCK,&set,&old_set));
    struct legacy_actor *a=calloc(ACTORS,sizeof(*a));CHECK(a);
    pthread_t threads[ACTORS];pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,NULL,ACTORS+1));
    for(unsigned i=0;i<ACTORS;i++) {
        a[i].gate=&gate;a[i].id=i;a[i].getpids=getpids/ACTORS;a[i].signals=signals/ACTORS;
        for(unsigned j=0;j<4096;j+=512)((volatile uint64_t *)a[i].samples)[j]=0;
        CHECK(!pthread_create(&threads[i],NULL,legacy_run,&a[i]));
    }
    queue_gate(&gate);queue_gate(&gate);
    for(unsigned i=0;i<ACTORS;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[ACTORS];uint64_t samples[SCALE_SIGNALS];unsigned delivered=0;
    for(unsigned i=0;i<ACTORS;i++)s[i]=a[i].spans[0];
    scale_report("getpid-bound",s,ACTORS,NULL,0,getpids,0,getpids);
    for(unsigned i=0;i<ACTORS;i++) {
        s[i]=a[i].spans[1];delivered+=a[i].delivered;
        memcpy(samples+i*(signals/ACTORS),a[i].samples,(signals/ACTORS)*sizeof(*samples));
    }
    CHECK(delivered==signals);
    scale_report("signal-bound",s,ACTORS,samples,signals,signals,0,delivered);
    for(unsigned i=0;i<ACTORS;i++)scale_samples("signal-bound",i,a[i].samples,signals/ACTORS);
    printf("SMP-SCALE VALID name=signal-bound delivered=%u expected=%u\n",delivered,signals);
    CHECK(!pthread_barrier_destroy(&gate));free(a);
    CHECK(!pthread_sigmask(SIG_SETMASK,&old_set,NULL));CHECK(!sigaction(SIGUSR1,&old_action,NULL));
}

struct scale_compute_actor {
    pthread_barrier_t *gate;unsigned id,iterations,observed[SCALE_CPU_SAMPLES];
    struct clock_span span;uint64_t checksum,mask;
};
static void *scale_compute_run(void *opaque)
{
    struct scale_compute_actor *a=opaque;queue_pin(a->id);
    queue_gate(a->gate);uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);
    a->span.begin=now();uint64_t value=1;
    for(unsigned sample=0;sample<SCALE_CPU_SAMPLES;sample++) {
        unsigned actual=UINT32_MAX;CHECK(!syscall(SYS_getcpu,&actual,NULL,NULL));
        a->observed[sample]=actual;unsigned index=0;
        while(index<cpu_count && cpu_ids[index]!=actual)index++;
        CHECK(index<cpu_count);a->mask|=UINT64_C(1)<<index;
        for(unsigned i=0;i<a->iterations/SCALE_CPU_SAMPLES;i++) {
            value=(value<<3)^(value>>7)^17;__asm__ volatile("":"+r"(value));
        }
    }
    /* These constants are the recurrence evaluated for the fixed iteration
     * counts, independent of the parallel execution being checked. */
    CHECK(value==(smoke?UINT64_C(0xed8eb42899304480):UINT64_C(0x36de7ac67e71a943)));
    a->checksum=value;a->span.end=now();a->span.cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    return NULL;
}
static void scale_compute(void)
{
    queue_unbound=1;unsigned iterations=smoke?80000:SCALE_COMPUTE;
    struct scale_compute_actor a[ACTORS]={0};pthread_t threads[ACTORS];pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,NULL,ACTORS+1));
    for(unsigned i=0;i<ACTORS;i++) {
        a[i].gate=&gate;a[i].id=i;a[i].iterations=iterations/ACTORS;
        CHECK(!pthread_create(&threads[i],NULL,scale_compute_run,&a[i]));
    }
    queue_gate(&gate);for(unsigned i=0;i<ACTORS;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[ACTORS];uint64_t used=0,checksum=0;
    for(unsigned i=0;i<ACTORS;i++) {
        s[i]=a[i].span;used|=a[i].mask;checksum+=a[i].checksum;
        printf("SMP-SCALE CPU actor=%u samples=%u used_cpu_mask=%llx ids=",i,SCALE_CPU_SAMPLES,(unsigned long long)a[i].mask);
        for(unsigned j=0;j<SCALE_CPU_SAMPLES;j++)printf("%s%u",j?",":"",a[i].observed[j]);
        putchar('\n');
    }
    scale_report("compute-unbound",s,ACTORS,NULL,0,iterations,0,ACTORS);
    CHECK(used && !(used&~((UINT64_C(1)<<cpu_count)-1)));
    printf("SMP-SCALE VALID name=compute-unbound checksum=%llu used_cpu_mask=%llx allowed_cpu_mask=%llx\n",
        (unsigned long long)checksum,(unsigned long long)used,(unsigned long long)((UINT64_C(1)<<cpu_count)-1));
    CHECK(!pthread_barrier_destroy(&gate));
}

static void scale_futex(unsigned unbound)
{
    queue_unbound=unbound;unsigned total=smoke?64:SCALE_ROUNDS,rounds=total/PAIRS;
    struct futex_perf_work *w=calloc(1,sizeof(*w));CHECK(w);
    w->cpus=cpu_count;w->rounds=rounds;w->message_words=SCALE_MESSAGE/sizeof(uint64_t);w->linear_actors=1;
    for(unsigned i=0;i<32768;i+=512)((volatile uint64_t *)w->samples)[i]=0;
    CHECK(!pthread_barrier_init(&w->gate,NULL,ACTORS+1));
    pthread_t threads[ACTORS];struct futex_perf_actor actors[ACTORS];
    for(unsigned i=0;i<ACTORS;i++) {
        actors[i]=(struct futex_perf_actor){w,i};CHECK(!pthread_create(&threads[i],NULL,futex_perf_run,&actors[i]));
    }
    queue_gate(&w->gate);queue_gate(&w->gate);queue_gate(&w->gate);
    struct clock_span s[ACTORS];unsigned waits=0,wakes=0;uint64_t checksum=0;
    for(unsigned i=0;i<ACTORS;i++) {
        CHECK(!pthread_join(threads[i],NULL));CHECK(w->completed[i]==rounds);
        s[i]=(struct clock_span){w->begin[i],w->end[i],w->cpu[i]};
        waits+=w->wait_calls[i];wakes+=w->wake_calls[i];
    }
    for(unsigned i=0;i<PAIRS;i++) {
        uint64_t value=futex_perf_value(i,rounds-1);
        CHECK(w->pairs[i].sequence==2*rounds && w->pairs[i].request==value && w->pairs[i].reply==~value);
        for(unsigned j=0;j<7;j++) {
            CHECK(w->pairs[i].request_tail[j]==(value^((uint64_t)(j+1)<<48)));
            CHECK(w->pairs[i].reply_tail[j]==~w->pairs[i].request_tail[j]);
            checksum+=w->pairs[i].reply_tail[j];
        }
        checksum+=w->pairs[i].reply;
    }
    const char *name=unbound?"futex-unbound":"futex-bound";
    for(unsigned i=0;i<PAIRS;i++)scale_samples(name,2*i,w->samples+i*rounds,rounds);
    scale_report(name,s,ACTORS,w->samples,total,total,(uint64_t)total*SCALE_MESSAGE*2,(uint64_t)total*2);
    printf("SMP-SCALE VALID name=%s completed=%u checksum=%llu wait_calls=%u wake_calls=%u\n",
        name,ACTORS,(unsigned long long)checksum,waits,wakes);
    CHECK(!pthread_barrier_destroy(&w->gate));free(w);
}

static void scale_mqueue(unsigned unbound)
{
    queue_unbound=unbound;unsigned total=smoke?64:SCALE_ROUNDS,rounds=total/PAIRS;
    struct mq_pair *p=calloc(PAIRS,sizeof(*p));struct mq_actor *a=calloc(ACTORS,sizeof(*a));CHECK(p && a);
    pthread_t threads[ACTORS];pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,ACTORS+1));
    for(unsigned i=0;i<PAIRS;i++) {
        p[i].id=i;p[i].size=SCALE_MESSAGE;p[i].rounds=rounds;p[i].gate=&gate;
        for(unsigned j=0;j<sizeof(p[i].pattern);j++)p[i].pattern[j]=(unsigned char)(0x51+i*37+j*13);
        for(unsigned side=0;side<2;side++) {
            char name[96];CHECK(snprintf(name,sizeof(name),"/smp-scale-%ld-%u-%u-%u",(long)getpid(),unbound,i,side)>0);
            struct mq_attr attr={.mq_maxmsg=8,.mq_msgsize=SCALE_MESSAGE};
            p[i].fd[side]=mq_open(name,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600,&attr);
            CHECK(p[i].fd[side]!=(mqd_t)-1 && !mq_unlink(name));
        }
    }
    for(unsigned i=0;i<ACTORS;i++) {
        a[i].pair=&p[i/2];a[i].side=i%2;
        for(unsigned j=0;j<1024;j+=512)((volatile uint64_t *)a[i].samples)[j]=0;
        CHECK(!pthread_create(&threads[i],NULL,mq_run,&a[i]));
    }
    queue_gate(&gate);for(unsigned i=0;i<ACTORS;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[ACTORS];uint64_t samples[SCALE_ROUNDS];
    for(unsigned i=0;i<ACTORS;i++)s[i]=a[i].span;
    for(unsigned i=0;i<PAIRS;i++)memcpy(samples+i*rounds,a[2*i].samples,rounds*sizeof(*samples));
    const char *name=unbound?"mq64-unbound":"mq64-bound";
    scale_report(name,s,ACTORS,samples,total,total,(uint64_t)total*SCALE_MESSAGE*2,(uint64_t)total*2);
    for(unsigned i=0;i<PAIRS;i++)scale_samples(name,2*i,a[2*i].samples,rounds);
    for(unsigned i=0;i<PAIRS;i++)for(unsigned side=0;side<2;side++) {
        struct mq_attr attr;CHECK(!mq_getattr(p[i].fd[side],&attr) && !attr.mq_curmsgs);
        CHECK(!mq_close(p[i].fd[side]));
    }
    printf("SMP-SCALE VALID name=%s completed=%u messages=%u queues_closed=%u\n",name,ACTORS,total*2,PAIRS*2);
    CHECK(!pthread_barrier_destroy(&gate));free(a);free(p);
}

static void scale_files(void)
{
    queue_unbound=0;unsigned rounds=smoke?16:SCALE_FILE_ROUNDS;
    CHECK(!mkdir("/la-cost",0700));struct file_cost a[PAIRS]={0};pthread_t threads[PAIRS];pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,NULL,PAIRS+1));
    for(unsigned i=0;i<PAIRS;i++) {
        char path[32];CHECK(snprintf(path,sizeof(path),"/la-cost/%u",i)>0);
        int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
        a[i]=(struct file_cost){.gate=&gate,.cpu=i%cpu_count,.rounds=rounds,.fd=fd};
        memset(a[i].samples,0,sizeof(a[i].samples));
    }
    int dir=open("/la-cost",O_RDONLY|O_DIRECTORY);CHECK(dir>=0 && !fsync(dir));
    for(unsigned i=0;i<PAIRS;i++)CHECK(!pthread_create(&threads[i],NULL,file_cost_run,&a[i]));
    queue_gate(&gate);for(unsigned i=0;i<PAIRS;i++)CHECK(!pthread_join(threads[i],NULL));
    struct clock_span s[PAIRS];uint64_t samples[PAIRS*SCALE_FILE_ROUNDS],sync_max=0;
    for(unsigned i=0;i<PAIRS;i++) {
        s[i]=(struct clock_span){a[i].begin,a[i].end,a[i].cpu_ns};
        memcpy(samples+i*rounds,a[i].samples,rounds*sizeof(*samples));
        if(a[i].sync_ns>sync_max)sync_max=a[i].sync_ns;
        struct stat st;CHECK(!fstat(a[i].fd,&st) && st.st_size==(off_t)rounds*4096);
    }
    scale_report("files-bound",s,PAIRS,samples,PAIRS*rounds,PAIRS*rounds,(uint64_t)PAIRS*rounds*4096*2,PAIRS*rounds);
    for(unsigned i=0;i<PAIRS;i++)scale_samples("files-bound",i,a[i].samples,rounds);
    printf("SMP-SCALE VALID name=files-bound final_fsyncs=%u sync_max_ns=%llu\n",PAIRS,(unsigned long long)sync_max);
    CHECK(!close(dir) && !pthread_barrier_destroy(&gate));
    for(unsigned i=0;i<PAIRS;i++) {
        char path[32];CHECK(snprintf(path,sizeof(path),"/la-cost/%u",i)>0);
        CHECK(!close(a[i].fd) && !unlink(path));
    }
    CHECK(!rmdir("/la-cost"));
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);unsigned requested=0;char profile[16]="formal";
    if(argc==1) {
        FILE *config=fopen("/smp-scale-cost","r");
        if(config) {int n=fscanf(config,"%u %15s",&requested,profile);CHECK(n==1 || n==2);CHECK(!fclose(config));}
        else CHECK(errno==ENOENT);
        CHECK(!strcmp(profile,"formal") || !strcmp(profile,"smoke"));smoke=!strcmp(profile,"smoke");
    }
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--smoke"))smoke=1;
        else {CHECK(!strcmp(argv[i],"--cpus") && ++i<argc);char *end;unsigned long n=strtoul(argv[i],&end,10);
            CHECK(*argv[i] && !*end && n<=ACTORS);requested=(unsigned)n;}
    }
    cpu_set_t allowed;CPU_ZERO(&allowed);CHECK(!sched_getaffinity(0,sizeof(allowed),&allowed));
    unsigned found=0;for(unsigned i=0;i<CPU_SETSIZE && found<ACTORS;i++)if(CPU_ISSET(i,&allowed))cpu_ids[found++]=i;
    cpu_count=requested?requested:found;
    CHECK((cpu_count==1 || cpu_count==2 || cpu_count==4 || cpu_count==8) && cpu_count<=found);
    struct utsname uts;CHECK(!uname(&uts));
    int linux_pid1=getpid()==1 && !strcmp(uts.sysname,"Linux") && !strstr(uts.release,"boaros"),own_proc=0;
    if(getpid()==1) {
        CHECK(!mkdir("/proc",0755) || errno==EEXIST);
        own_proc=!mount("proc","/proc","proc",0,NULL);CHECK(own_proc || errno==EBUSY);
    }
    queue_fixed_actors=1;queue_result_prefix="SMP-SCALE";
    printf("SMP-SCALE BEGIN version=1 profile=%s cpus=%u actors=%u file_workers=%u cpu_ids=",smoke?"smoke":"formal",cpu_count,ACTORS,PAIRS);
    for(unsigned i=0;i<cpu_count;i++)printf("%s%u",i?",":"",cpu_ids[i]);
    putchar('\n');
    scale_legacy();scale_compute();scale_futex(0);scale_futex(1);scale_mqueue(0);scale_mqueue(1);scale_files();
    CHECK(windows==8);if(own_proc)CHECK(!umount("/proc"));
    printf("SMP-SCALE END windows=%u checks=%llu profile=%s\n",windows,(unsigned long long)all_checks,smoke?"smoke":"formal");
    if(linux_pid1) {sync();CHECK(!reboot(RB_POWER_OFF));}
    return 0;
}
