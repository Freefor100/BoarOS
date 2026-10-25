/* The old completed work is shared verbatim with the resource-limit workload. */
#define main resource_embedded_main
#include "rlimits-pidfd.c"
#undef main
#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 1
#endif
static uint64_t memlock_bytes(void)
{
    char data[4096];int fd=open("/proc/self/status",O_RDONLY);CHECK(fd>=0);
    ssize_t n=read(fd,data,sizeof(data)-1);CHECK(n>0&&!close(fd));data[n]=0;
    char *field=strstr(data,"VmLck:");CHECK(field);return strtoull(field+6,0,10)*1024;
}
static unsigned memlock_resident(unsigned char *p,size_t bytes)
{
    unsigned char vector[1024];CHECK(bytes/4096<=sizeof(vector)&&!mincore(p,bytes,vector));
    unsigned result=0;for(unsigned i=0;i<bytes/4096;i++)result+=vector[i]&1;return result;
}
struct memlock_work {
    pthread_barrier_t gate;
    unsigned cpus;
    uint64_t begin[4],end[4],cpu[4];
};
struct memlock_actor {struct memlock_work *work;unsigned id;};
static void *memlock_run(void *argument)
{
    struct memlock_actor *a=argument;struct memlock_work *w=a->work;unsigned id=a->id;
    queue_pin(id%w->cpus);queue_gate(&w->gate);queue_gate(&w->gate);
    uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);w->begin[id]=now();
    for(unsigned i=0;i<256;i++) {
        uint64_t begin=now();unsigned char *p=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(p!=MAP_FAILED&&!mlock(p,65536));p[0]=(unsigned char)i;CHECK(p[0]==(unsigned char)i);
        CHECK(!munlock(p,65536)&&!munmap(p,65536));mm_msg_samples[id*256+i]=now()-begin;
    }
    w->end[id]=now();w->cpu[id]=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;return 0;
}
static void memlock_new(unsigned cpus)
{
    for(unsigned mode=0;mode<2;mode++) {
        size_t bytes=4*1024*1024;unsigned char *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
        CHECK(!memlock_resident(p,bytes));uint64_t cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID),begin=now();
        CHECK(!syscall(SYS_mlock2,p,bytes,mode?MLOCK_ONFAULT:0));
        uint64_t elapsed=now()-begin;cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;mm_msg_samples[0]=elapsed;
        CHECK(memlock_bytes()==bytes);unsigned resident=memlock_resident(p,bytes);CHECK(resident==(mode?0U:1024U));
        mm_msg_report(mode?"onfault-set-4m":"immediate-set-4m",1,bytes,elapsed,cpu);
        cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID);begin=now();
        for(unsigned i=0;i<1024;i++){p[i*4096]=(unsigned char)i;CHECK(p[i*4096]==(unsigned char)i);}
        elapsed=now()-begin;cpu=queue_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;mm_msg_samples[0]=elapsed;
        CHECK(memlock_resident(p,bytes)==1024);mm_msg_report(mode?"onfault-touch-4m":"immediate-touch-4m",1,bytes,elapsed,cpu);
        printf("MEMLOCK-PERF state=%s locked=%zu before=%u after=1024\n",mode?"onfault":"immediate",bytes,resident);
        CHECK(!munlock(p,bytes)&&!munmap(p,bytes)&&!memlock_bytes());
    }
    struct memlock_work work={.cpus=cpus};CHECK(!pthread_barrier_init(&work.gate,0,5));
    pthread_t threads[4];struct memlock_actor actors[4];
    for(unsigned i=0;i<4;i++){actors[i]=(struct memlock_actor){&work,i};CHECK(!pthread_create(&threads[i],0,memlock_run,&actors[i]));}
    queue_gate(&work.gate);queue_gate(&work.gate);uint64_t begin=UINT64_MAX,end=0,cpu=0;
    for(unsigned i=0;i<4;i++){CHECK(!pthread_join(threads[i],0));if(work.begin[i]<begin)begin=work.begin[i];if(work.end[i]>end)end=work.end[i];cpu+=work.cpu[i];}
    CHECK(!pthread_barrier_destroy(&work.gate)&&!memlock_bytes());
    mm_msg_report("lock-unlock-map-four",1024,1024*65536,end-begin,cpu);
}
int main(void)
{
    unsigned mode,cpus;setvbuf(stdout,0,_IONBF,0);FILE *config=fopen("/account-resource","r");
    CHECK(config&&fscanf(config,"%u %u",&mode,&cpus)==2&&!fclose(config)&&mode<=1&&(cpus==1||cpus==2||cpus==4));queue_pin(0);
    if(mode)memlock_new(cpus);else {mm_msg_old(cpus);mm_msg_memory();resource_file();}
    printf("MEMLOCK-PERF END %u\n",mm_msg_metrics);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
