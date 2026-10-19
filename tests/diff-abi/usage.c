#include "abi.h"

static unsigned long monotonic_ns(void)
{
    long t[2];abi_require(SC2(113,1,t)==0);
    return (unsigned long)t[0]*1000000000UL+(unsigned long)t[1];
}
static void benchmark(void)
{
    unsigned long start=monotonic_ns();
    for (int i=0;i<10000;i++) abi_require(SC0(172)>0);
    unsigned long calls=monotonic_ns()-start;start=monotonic_ns();
    for (int i=0;i<1000;i++) abi_require(SC0(124)==0);
    unsigned long yields=monotonic_ns()-start;
    char line[120];usize n=0;
    const char *prefix="USAGE BENCH getpid10000_ns=";while(*prefix)line[n++]=*prefix++;
    for (int part=0;part<2;part++) {
        unsigned long v=part ? yields : calls;char digits[24];usize count=0;
        do {digits[count++]='0'+v%10;v/=10;}while(v);
        while(count)line[n++]=digits[--count];
        if(!part) {prefix=" yield1000_ns=";while(*prefix)line[n++]=*prefix++;}
    }
    line[n++]='\n';abi_require(SC3(64,1,line,n)==(long)n);
}

struct usage {
    long utime_sec,utime_usec,stime_sec,stime_usec,maxrss,ixrss,idrss,isrss;
    long minflt,majflt,nswap,inblock,oublock,msgsnd,msgrcv,nsignals,nvcsw,nivcsw;
};
static long get_usage(int who,struct usage *r) {return SC2(165,who,r);}
static void fault_pages(unsigned count)
{
    long map=SC6(222,0,count*ABI_PAGE_SIZE,3,0x22,-1,0);abi_require(map>0);
    for(unsigned i=0;i<count;i++)*(volatile unsigned char *)(map+i*ABI_PAGE_SIZE)=17;
    abi_require(!SC2(215,map,count*ABI_PAGE_SIZE));
}
extern long abi_clone_entry(long,void *,int *,void (*)(void *),void *);
static unsigned char usage_stack[16384] __attribute__((aligned(16)));
struct usage_thread {struct usage result;int done;};
static unsigned long usage_online;
static void usage_worker(void *opaque)
{
    struct usage_thread *t=opaque;
    unsigned long mask=(usage_online&2) ? 2 : 1;
    abi_require(!SC3(122,0,sizeof(mask),&mask));
    fault_pages(32);
    unsigned long end=monotonic_ns()+100000000UL,value=1;
    do {for(unsigned i=0;i<100000;i++) {value=(value<<3)^(value>>7)^17;__asm__ volatile("" : "+r"(value));}}
    while(monotonic_ns()<end);
    abi_require(!get_usage(1,&t->result));__atomic_store_n(&t->done,1,__ATOMIC_RELEASE);
}
void abi_usage_exec_probe(const char *text)
{
    long minimum=0;while(*text) {abi_require(*text>='0' && *text<='9');minimum=minimum*10+*text++-'0';}
    struct usage r={0};abi_exit(!get_usage(0,&r) && r.maxrss>=minimum ? 57 : 1);
}

static void functional_usage(void)
{
    struct usage before,after,thread_before,thread_after;
    abi_require(SC3(123,0,sizeof(usage_online),&usage_online)==sizeof(usage_online));
    long ret=get_usage(0,&before);abi_record("usage.available",ret,-1,-1,0,0,0);abi_require(!ret);
    abi_record("usage.timeval-units",before.utime_sec>=0 && before.stime_sec>=0 &&
        before.utime_usec>=0 && before.utime_usec<1000000 && before.stime_usec>=0 && before.stime_usec<1000000,-1,-1,0,0,0);
    abi_record("usage.linux-unused-fields",!before.ixrss && !before.idrss && !before.isrss && !before.nswap &&
        !before.msgsnd && !before.msgrcv && !before.nsignals,-1,-1,0,0,0);
    abi_require(!get_usage(1,&before));unsigned long start=monotonic_ns();
    long delay[2]={0,200000000};abi_require(!SC2(101,delay,0) && !get_usage(1,&after));
    unsigned long elapsed=monotonic_ns()-start;
    long cpu=(after.utime_sec-before.utime_sec+after.stime_sec-before.stime_sec)*1000000L+
        after.utime_usec-before.utime_usec+after.stime_usec-before.stime_usec;
    abi_record("usage.sleep-excludes-cpu",cpu>=0 && (unsigned long)cpu*1000UL<elapsed/2,-1,-1,0,0,0);
    long map=SC6(222,0,64*ABI_PAGE_SIZE,3,0x22,-1,0);abi_require(map>0);
    abi_require(!get_usage(0,&before));
    for(unsigned i=0;i<64;i++)*(volatile unsigned char *)(map+i*ABI_PAGE_SIZE)=17;
    abi_require(!get_usage(0,&after));
    abi_record("usage.minor-faults",after.minflt-before.minflt>=64,-1,-1,0,0,0);
    abi_record("usage.rss-page-units",after.maxrss>=64*(ABI_PAGE_SIZE/1024),-1,-1,0,0,0);
    long peak=after.maxrss;
    abi_require(!SC3(226,map,64*ABI_PAGE_SIZE,0) && !get_usage(0,&after));
    abi_record("usage.rss-protection",after.maxrss==peak,-1,-1,0,0,0);
    abi_require(!SC2(215,map,64*ABI_PAGE_SIZE) && !get_usage(0,&after));
    abi_record("usage.rss-high-water",after.maxrss>=peak,-1,-1,0,0,0);
    abi_require(!get_usage(0,&before) && !get_usage(1,&thread_before));
    struct usage_thread work={0};int tid_address=0;
    long tid=abi_clone_entry(0x10f00,usage_stack+sizeof(usage_stack),&tid_address,usage_worker,&work);abi_require(tid>0);
    while(!__atomic_load_n(&work.done,__ATOMIC_ACQUIRE)) SC0(124);
    while(SC2(130,tid,0)==0) SC0(124);
    abi_require(!get_usage(0,&after) && !get_usage(1,&thread_after));
    abi_record("usage.thread-user-time",work.result.utime_sec>0 || work.result.utime_usec>0,-1,-1,0,0,0);
    abi_record("usage.dead-thread-self",after.minflt-before.minflt>=32,-1,-1,0,0,0);
    abi_record("usage.dead-thread-isolation",thread_after.minflt-thread_before.minflt<32 && work.result.minflt>=32,-1,-1,0,0,0);
    static unsigned char io_buffer[ABI_PAGE_SIZE];
    for(unsigned i=0;i<sizeof(io_buffer);i++)io_buffer[i]=19;
    long fd=abi_open("/data",0);abi_require(fd>=0 && !get_usage(1,&before));
    abi_require(SC4(67,fd,io_buffer,1,0)==1 && !get_usage(1,&after));
    abi_record("usage.cold-read",after.inblock>before.inblock,-1,-1,0,0,0);
    before=after;abi_require(SC4(67,fd,io_buffer,1,0)==1 && !get_usage(1,&after));
    abi_record("usage.hot-read",after.inblock==before.inblock,-1,-1,0,0,0);
    abi_require(!SC1(57,fd));fd=abi_open("/usage-io",2|64|512);abi_require(fd>=0);
    abi_require(!get_usage(1,&before) && SC3(64,fd,io_buffer,1)==1 && !get_usage(1,&after));
    abi_record("usage.dirty-page-units",after.oublock-before.oublock>=ABI_PAGE_SIZE/512,-1,-1,0,0,0);
    before=after;abi_require(SC4(68,fd,io_buffer,1,0)==1 && !get_usage(1,&after));
    abi_record("usage.dirty-page-once",after.oublock==before.oublock,-1,-1,0,0,0);
    abi_require(!SC1(82,fd) && !SC1(57,fd));
    long exec_child=SC5(220,17,0,0,0,0);abi_require(exec_child>=0);
    if(!exec_child) {
        fault_pages(1024);char number[24],digits[24];usize n=0,k=0;
        unsigned long value=1024*(ABI_PAGE_SIZE/1024);
        do {digits[k++]='0'+value%10;value/=10;}while(value);
        while(k) number[n++]=digits[--k];
        number[n]=0;
        const char *args[]={"/init","usage-exec",number,0};const char *env[]={0};
        SC3(221,"/init",args,env);abi_exit(97);
    }
    int exec_status=0;struct usage exec_usage;
    abi_require(SC4(260,exec_child,&exec_status,0,&exec_usage)==exec_child);
    abi_record("usage.exec-high-water",exec_status==(57<<8) && exec_usage.maxrss>=1024*(ABI_PAGE_SIZE/1024),-1,-1,0,0,0);
    struct usage children_before,children_after,peek,reaped;
    abi_require(!get_usage(-1,&children_before));
    long child=SC5(220,17,0,0,0,0);abi_require(child>=0);
    if(!child) {
        fault_pages(16);long grandchild=SC5(220,17,0,0,0,0);abi_require(grandchild>=0);
        if(!grandchild) {fault_pages(32);abi_exit(0);}
        abi_require(SC4(260,grandchild,0,0,0)==grandchild);abi_exit(57);
    }
    unsigned char info[128];abi_require(!SC5(95,1,child,info,4|0x1000000,&peek));
    abi_require(!get_usage(-1,&children_after));
    abi_record("usage.wnowait-not-children",children_before.minflt==children_after.minflt &&
        children_before.maxrss==children_after.maxrss,-1,-1,0,0,0);
    abi_require(SC4(260,child,0,0,&reaped)==child && !get_usage(-1,&children_after));
    /* Linux可先通知zombie，再完成最后一次切出；后一次快照允许继续增长。 */
    abi_record("usage.wait-descendants",peek.minflt>=48 && reaped.minflt==peek.minflt &&
        reaped.maxrss==peek.maxrss && reaped.nvcsw>=peek.nvcsw && reaped.nivcsw>=peek.nivcsw,-1,-1,0,0,0);
    abi_record("usage.children-consume-once",children_after.minflt-children_before.minflt==reaped.minflt,-1,-1,0,0,0);
    abi_record("usage.children-maxrss",children_after.maxrss==
        (children_before.maxrss>reaped.maxrss ? children_before.maxrss : reaped.maxrss),-1,-1,0,0,0);
    abi_require(SC4(260,child,0,1,0)==-10 && !get_usage(-1,&after));
    abi_record("usage.children-after-echild",after.minflt==children_after.minflt && after.maxrss==children_after.maxrss,-1,-1,0,0,0);
    unsigned char *partial=(void *)SC6(222,0,2*ABI_PAGE_SIZE,3,0x22,-1,0);abi_require((long)partial>0);
    for(int i=0;i<64;i++)partial[ABI_PAGE_SIZE-64+i]=0x5a;
    abi_require(!SC3(226,partial+ABI_PAGE_SIZE,ABI_PAGE_SIZE,0));
    ret=SC2(165,0,partial+ABI_PAGE_SIZE-64);
    abi_record("usage.output-fault-prefix",ret,-1,-1,partial[ABI_PAGE_SIZE-24]!=0x5a,0,0);
    abi_require(!SC2(215,partial,2*ABI_PAGE_SIZE));
}

void abi_usage_cases(void)
{
    benchmark();
    functional_usage();
    abi_record("usage.invalid-who",SC2(165,2,(void *)-1),-1,-1,0,0,0);
    abi_record("usage.invalid-both",SC2(165,-2,(void *)-1),-1,-1,0,0,0);
    abi_record("usage.who-signed32",SC2(165,0x100000000UL,(void *)-1),-1,-1,0,0,0);
    abi_record("usage.self-badptr",SC2(165,0,(void *)-1),-1,-1,0,0,0);
    abi_record("usage.thread-badptr",SC2(165,1,(void *)-1),-1,-1,0,0,0);
    abi_record("usage.children-badptr",SC2(165,-1,(void *)-1),-1,-1,0,0,0);
    abi_record("usage.self-null",SC2(165,0,0),-1,-1,0,0,0);
#ifndef ABI_RESOURCE_ONLY
    abi_cpu_clock_cases();
#endif
}
