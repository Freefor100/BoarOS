/* Reuse the established standard-signal/futex windows and compute work. */
#define main existing_posix_cost_main
#include "../../accounting/posix.c"
#undef main
struct file_cost {pthread_barrier_t *gate;unsigned cpu;int fd;uint64_t begin,end,cpu_ns,sync_ns,samples[1024];};
static void *file_cost_run(void *opaque)
{
    struct file_cost *w=opaque;queue_pin(w->cpu);unsigned char data[4096],output[4096];
    memset(data,0x53,sizeof(data));queue_gate(w->gate);
    w->cpu_ns=queue_clock(CLOCK_THREAD_CPUTIME_ID);w->begin=now();
    for(unsigned i=0;i<1024;i++) {
        uint64_t begin=now();CHECK(pwrite(w->fd,data,sizeof(data),(off_t)i*4096)==4096);
        CHECK(pread(w->fd,output,sizeof(output),(off_t)i*4096)==4096 && !memcmp(data,output,sizeof(data)));
        w->samples[i]=now()-begin;
    }
    uint64_t before=now();CHECK(!fsync(w->fd));w->sync_ns=now()-before;w->end=now();
    w->cpu_ns=queue_clock(CLOCK_THREAD_CPUTIME_ID)-w->cpu_ns;return NULL;
}
static void filesystem_cost(unsigned cpus)
{
    CHECK(!mkdir("/la-cost",0700));struct file_cost workers[4];pthread_t threads[4];pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,NULL,5));
    for(unsigned i=0;i<4;i++) {
        char path[32];snprintf(path,sizeof(path),"/la-cost/%u",i);int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
        workers[i]=(struct file_cost){.gate=&gate,.cpu=i%cpus,.fd=fd};
    }
    int dir=open("/la-cost",O_RDONLY|O_DIRECTORY);CHECK(dir>=0&&!fsync(dir));
    for(unsigned i=0;i<4;i++)CHECK(!pthread_create(&threads[i],NULL,file_cost_run,&workers[i]));
    queue_gate(&gate);
    uint64_t start=UINT64_MAX,end=0,cpu=0,sync_max=0,samples[4096];
    for(unsigned i=0;i<4;i++) {
        CHECK(!pthread_join(threads[i],NULL));
        if(workers[i].begin<start)start=workers[i].begin;
        if(workers[i].end>end)end=workers[i].end;
        if(workers[i].sync_ns>sync_max)sync_max=workers[i].sync_ns;
        cpu+=workers[i].cpu_ns;memcpy(samples+i*1024,workers[i].samples,sizeof(workers[i].samples));
    }
    qsort(samples,4096,sizeof(*samples),queue_compare);struct rusage u;CHECK(!getrusage(RUSAGE_SELF,&u));
    printf("LA-SMP-PERF name=files cpus=%u count=4096 bytes=33554432 ns=%llu cpu_ns=%llu p50=%llu p95=%llu max=%llu sync_max=%llu rss_kib=%ld\n",cpus,
        (unsigned long long)(end-start),(unsigned long long)cpu,(unsigned long long)samples[2048],(unsigned long long)samples[3891],(unsigned long long)samples[4095],(unsigned long long)sync_max,u.ru_maxrss);
    CHECK(!close(dir)&&!pthread_barrier_destroy(&gate));
    for(unsigned i=0;i<4;i++){char path[32];snprintf(path,sizeof(path),"/la-cost/%u",i);CHECK(!close(workers[i].fd)&&!unlink(path));}
    CHECK(!rmdir("/la-cost"));
}
static void compute_cost(unsigned cpus)
{
    pthread_barrier_t gate;CHECK(!pthread_barrier_init(&gate,NULL,5));struct posix_worker workers[4];pthread_t threads[4];
    for(unsigned i=0;i<4;i++){workers[i]=(struct posix_worker){.gate=&gate,.id=i,.cpus=cpus,.clock=1};CHECK(!pthread_create(&threads[i],NULL,compute,&workers[i]));}
    queue_gate(&gate);uint64_t start=UINT64_MAX,end=0,cpu=0;
    for(unsigned i=0;i<4;i++) {
        CHECK(!pthread_join(threads[i],NULL)&&workers[i].check==workers[0].check);
        if(workers[i].begin<start)start=workers[i].begin;
        if(workers[i].end>end)end=workers[i].end;
        cpu+=workers[i].cpu;
    }
    CHECK(!pthread_barrier_destroy(&gate));struct rusage u;CHECK(!getrusage(RUSAGE_SELF,&u));
    printf("LA-SMP-PERF name=compute cpus=%u count=100000000 bytes=0 ns=%llu cpu_ns=%llu p50=0 p95=0 max=0 sync_max=0 rss_kib=%ld\n",cpus,(unsigned long long)(end-start),(unsigned long long)cpu,u.ru_maxrss);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);FILE *config=fopen("/la-smp-cost","r");unsigned cpus;
    CHECK(config&&fscanf(config,"%u",&cpus)==1&&!fclose(config)&&(cpus==1||cpus==2));
    config=fopen("/account-altstack","r");CHECK(config&&!stack_cost_main(config));
    futex_perf(cpus);compute_cost(cpus);filesystem_cost(cpus);puts("LA-SMP-PERF END 7");return 0;
}
