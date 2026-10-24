/* Share the old windows and their completion checks with signal-futex.c. */
#define main unused_posix_cost_main
#include "../../accounting/posix.c"
#undef main
#include <sys/msg.h>

enum { MM_MSG_COUNT=4096, MM_MSG_SMALL=65536, MM_MSG_LARGE=131072 };
static uint64_t mm_msg_samples[MM_MSG_COUNT];
static unsigned mm_msg_host,mm_msg_metrics;

static void mm_msg_old(unsigned cpus)
{
    FILE *config=fopen("/account-altstack","r");
    CHECK(config && !stack_cost_main(config));
    futex_perf(cpus);puts("SIGNAL-FUTEX-PERF END 5");
}

static void mm_msg_report(const char *name,unsigned count,uint64_t bytes,
                          uint64_t elapsed,uint64_t cpu)
{
    struct rusage usage;CHECK(!getrusage(RUSAGE_SELF,&usage));
    qsort(mm_msg_samples,count,sizeof(*mm_msg_samples),queue_compare);
    CHECK(elapsed && mm_msg_samples[count/2] && cpu && usage.ru_maxrss>0);
    printf("MMMSG-PERF name=%s count=%u bytes=%llu ns=%llu p50=%llu p95=%llu max=%llu cpu_ns=%llu rss_kib=%ld ops_per_sec=%llu\n",
        name,count,(unsigned long long)bytes,(unsigned long long)elapsed,
        (unsigned long long)mm_msg_samples[count/2],(unsigned long long)mm_msg_samples[count*95/100],
        (unsigned long long)mm_msg_samples[count-1],(unsigned long long)cpu,usage.ru_maxrss,
        (unsigned long long)((uint64_t)count*1000000000/elapsed));
    mm_msg_metrics++;
}

static unsigned char mm_msg_byte(unsigned page,unsigned generation)
{ return (unsigned char)(page*29+generation*13+7); }
static void mm_msg_pages(unsigned char *p,size_t length,unsigned generation,int write)
{
    CHECK(p!=MAP_FAILED && !((uintptr_t)p&4095));
    for(unsigned offset=0;offset<length;offset+=4096) {
        unsigned char value=mm_msg_byte(offset/4096,generation);
        if(write){p[offset]=value;p[offset+4095]=(unsigned char)~value;}
        else CHECK(p[offset]==value && p[offset+4095]==(unsigned char)~value);
    }
}
static void mm_msg_memory(void)
{
    const char *names[]={"mmap-munmap-64k","mprotect-64k","mremap-64k-128k"};
    for(unsigned mode=0;mode<3;mode++) {
        unsigned char *p=0;
        if(mode) {
            p=mmap(0,MM_MSG_SMALL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            CHECK(p!=MAP_FAILED);mm_msg_pages(p,MM_MSG_SMALL,0,1);
        }
        uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID),begin=now();
        for(unsigned i=0;i<MM_MSG_COUNT;i++) {
            uint64_t start=now();
            if(!mode) {
                p=mmap(0,MM_MSG_SMALL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                CHECK(p!=MAP_FAILED && !((uintptr_t)p&4095));
                for(unsigned offset=0;offset<MM_MSG_SMALL;offset+=4096)CHECK(!p[offset]&&!p[offset+4095]);
                mm_msg_pages(p,MM_MSG_SMALL,i,1);mm_msg_pages(p,MM_MSG_SMALL,i,0);
                CHECK(!munmap(p,MM_MSG_SMALL));
            } else if(mode==1) {
                CHECK(!mprotect(p,MM_MSG_SMALL,PROT_READ));mm_msg_pages(p,MM_MSG_SMALL,i,0);
                CHECK(!mprotect(p,MM_MSG_SMALL,PROT_READ|PROT_WRITE));
                mm_msg_pages(p,MM_MSG_SMALL,i+1,1);
            } else {
                p=mremap(p,MM_MSG_SMALL,MM_MSG_LARGE,MREMAP_MAYMOVE);
                CHECK(p!=MAP_FAILED);mm_msg_pages(p,MM_MSG_SMALL,i,0);
                for(unsigned offset=MM_MSG_SMALL;offset<MM_MSG_LARGE;offset+=4096)CHECK(!p[offset]&&!p[offset+4095]);
                mm_msg_pages(p,MM_MSG_LARGE,i+1,1);
                p=mremap(p,MM_MSG_LARGE,MM_MSG_SMALL,MREMAP_MAYMOVE);
                CHECK(p!=MAP_FAILED);mm_msg_pages(p,MM_MSG_SMALL,i+1,0);
            }
            mm_msg_samples[i]=now()-start;
        }
        uint64_t elapsed=now()-begin;cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
        if(mode)CHECK(!munmap(p,MM_MSG_SMALL));
        mm_msg_report(names[mode],MM_MSG_COUNT,0,elapsed,cpu);
    }
}

struct mm_msg_packet {long type;unsigned char body[4096];};
struct mm_msg_exchange {
    pthread_barrier_t gate;
    unsigned cpus,bytes;
    int queues[4][2];
    uint64_t begin[8],end[8],cpu[8];
    unsigned completed[8];
};
struct mm_msg_actor {struct mm_msg_exchange *work;unsigned id;};
static void mm_msg_packet_set(struct mm_msg_packet *p,unsigned bytes,unsigned pair,unsigned round,unsigned role)
{
    uint64_t id=((uint64_t)pair<<32)|round;
    p->type=(long)(11+pair*2+role);memcpy(p->body,&id,sizeof(id));
    for(unsigned i=sizeof(id);i<bytes;i++)p->body[i]=(unsigned char)(pair*17+round*13+i*29+role);
}
static void mm_msg_packet_check(const struct mm_msg_packet *p,unsigned bytes,unsigned pair,unsigned round,unsigned role)
{
    uint64_t id;memcpy(&id,p->body,sizeof(id));
    CHECK(p->type==(long)(11+pair*2+role) && id==(((uint64_t)pair<<32)|round));
    for(unsigned i=sizeof(id);i<bytes;i++)CHECK(p->body[i]==(unsigned char)(pair*17+round*13+i*29+role));
}
static void *mm_msg_exchange_run(void *argument)
{
    struct mm_msg_actor *actor=argument;struct mm_msg_exchange *w=actor->work;
    unsigned id=actor->id,pair=id/2,role=id%2;
    if(!mm_msg_host)queue_pin((pair+role)%w->cpus);
    struct mm_msg_packet packet;memset(&packet,0,sizeof(packet));
    queue_gate(&w->gate);queue_gate(&w->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);w->begin[id]=now();
    for(unsigned round=0;round<MM_MSG_COUNT/4;round++) {
        uint64_t start=0;
        if(!role) {
            start=now();mm_msg_packet_set(&packet,w->bytes,pair,round,0);
            CHECK(!msgsnd(w->queues[pair][0],&packet,w->bytes,0));
            CHECK(msgrcv(w->queues[pair][1],&packet,w->bytes,12+pair*2,0)==(ssize_t)w->bytes);
            mm_msg_packet_check(&packet,w->bytes,pair,round,1);
            mm_msg_samples[pair*(MM_MSG_COUNT/4)+round]=now()-start;
        } else {
            CHECK(msgrcv(w->queues[pair][0],&packet,w->bytes,11+pair*2,0)==(ssize_t)w->bytes);
            mm_msg_packet_check(&packet,w->bytes,pair,round,0);
            mm_msg_packet_set(&packet,w->bytes,pair,round,1);
            CHECK(!msgsnd(w->queues[pair][1],&packet,w->bytes,0));
        }
        w->completed[id]++;
    }
    w->end[id]=now();w->cpu[id]=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    return 0;
}
static void mm_msg_messages(unsigned cpus)
{
    for(unsigned bytes=64;bytes<=4096;bytes*=64) {
        struct mm_msg_exchange work={.cpus=cpus,.bytes=bytes};
        CHECK(!pthread_barrier_init(&work.gate,0,9));
        pthread_t threads[8];struct mm_msg_actor actors[8];
        for(unsigned pair=0;pair<4;pair++)for(unsigned side=0;side<2;side++) {
            work.queues[pair][side]=msgget(IPC_PRIVATE,IPC_CREAT|0600);CHECK(work.queues[pair][side]>=0);
        }
        for(unsigned id=0;id<8;id++) {
            actors[id]=(struct mm_msg_actor){&work,id};CHECK(!pthread_create(&threads[id],0,mm_msg_exchange_run,&actors[id]));
        }
        queue_gate(&work.gate);queue_gate(&work.gate);
        for(unsigned id=0;id<8;id++)CHECK(!pthread_join(threads[id],0));
        uint64_t begin=UINT64_MAX,end=0,cpu=0;
        for(unsigned id=0;id<8;id++) {
            CHECK(work.completed[id]==MM_MSG_COUNT/4);
            if(work.begin[id]<begin)begin=work.begin[id];
            if(work.end[id]>end)end=work.end[id];
            cpu+=work.cpu[id];
        }
        for(unsigned pair=0;pair<4;pair++)for(unsigned side=0;side<2;side++) {
            struct msqid_ds state;CHECK(!msgctl(work.queues[pair][side],IPC_STAT,&state)&&!state.msg_qnum);
            CHECK(!msgctl(work.queues[pair][side],IPC_RMID,0));
        }
        CHECK(!pthread_barrier_destroy(&work.gate));
        mm_msg_report(bytes==64?"msg-roundtrip-64":"msg-roundtrip-4096",MM_MSG_COUNT,
                      (uint64_t)bytes*MM_MSG_COUNT*2,end-begin,cpu);
    }
}
int main(int argc,char **argv)
{
    unsigned old=0,memory=0,messages=0,cpus=1;
    setvbuf(stdout,0,_IONBF,0);
    if(argc==1) {
        FILE *config=fopen("/account-mm-msg","r");
        CHECK(config && fscanf(config,"%u %u %u %u",&old,&memory,&messages,&cpus)==4 && !fclose(config));
        CHECK(old<=1 && memory<=1 && messages<=1);
    } else for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--old"))old=1;
        else if(!strcmp(argv[i],"--mm"))memory=1;
        else if(!strcmp(argv[i],"--newmsg"))messages=1;
        else if(!strcmp(argv[i],"--host"))mm_msg_host=1;
        else if(!strcmp(argv[i],"--cpus")){CHECK(++i<argc);cpus=(unsigned)strtoul(argv[i],0,10);}
        else CHECK(0);
    }
    CHECK((old||memory||messages) && (cpus==1||cpus==2||cpus==4) && !(old&&mm_msg_host));
    if(!mm_msg_host)queue_pin(0);
    for(unsigned i=0;i<MM_MSG_COUNT;i+=512)((volatile uint64_t *)mm_msg_samples)[i]=0;
    printf("MMMSG-PERF BEGIN old=%u mm=%u msg=%u cpus=%u\n",old,memory,messages,cpus);
    if(old)mm_msg_old(cpus);
    if(memory)mm_msg_memory();
    if(messages)mm_msg_messages(cpus);
    printf("MMMSG-PERF END %u\n",mm_msg_metrics);return 0;
}
