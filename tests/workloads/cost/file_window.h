/* Shared four-file pwrite/pread window; zero rounds preserves the formal 4 MiB per file. */
struct file_cost {pthread_barrier_t *gate;unsigned cpu,rounds;int fd;uint64_t begin,end,cpu_ns,sync_ns,samples[1024];};
static void *file_cost_run(void *opaque)
{
    struct file_cost *w=opaque;queue_pin(w->cpu);unsigned char data[4096],output[4096];
    memset(data,0x53,sizeof(data));queue_gate(w->gate);
    w->cpu_ns=queue_clock(CLOCK_THREAD_CPUTIME_ID);w->begin=now();
    for(unsigned i=0;i<(w->rounds?w->rounds:1024);i++) {
        uint64_t begin=now();CHECK(pwrite(w->fd,data,sizeof(data),(off_t)i*4096)==4096);
        CHECK(pread(w->fd,output,sizeof(output),(off_t)i*4096)==4096 && !memcmp(data,output,sizeof(data)));
        w->samples[i]=now()-begin;
    }
    uint64_t before=now();CHECK(!fsync(w->fd));w->sync_ns=now()-before;w->end=now();
    w->cpu_ns=queue_clock(CLOCK_THREAD_CPUTIME_ID)-w->cpu_ns;return NULL;
}
