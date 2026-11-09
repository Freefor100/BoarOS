#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"BPF TRACE FAIL %d errno=%d: %s\n",__LINE__,errno,#c);exit(1);}}while(0)
#define I(c,d,s,o,i) ((struct bpf_insn){.code=c,.dst_reg=d,.src_reg=s,.off=o,.imm=i})
struct record {uint64_t count;int64_t first,second;};
static int bpf(unsigned cmd,union bpf_attr *a){return syscall(__NR_bpf,cmd,a,sizeof(*a));}
static int make_map(void)
{
    union bpf_attr a={.map_type=BPF_MAP_TYPE_ARRAY,.key_size=4,.value_size=sizeof(struct record),.max_entries=1024};
    int fd=bpf(BPF_MAP_CREATE,&a);CHECK(fd>=0);return fd;
}
static struct record get(int map,uint32_t key)
{
    struct record r={0};union bpf_attr a={.map_fd=map,.key=(uintptr_t)&key,.value=(uintptr_t)&r};
    CHECK(!bpf(BPF_MAP_LOOKUP_ELEM,&a));return r;
}
static int program(int map,int enter)
{
    struct bpf_insn code[]={
        I(0xbf,6,1,0,0),I(0x85,0,0,0,14),I(0xbc,0,0,0,0),
        I(0x55,0,0,16,syscall(__NR_gettid)),
        I(0x61,1,6,8,0),I(0x63,10,1,-4,0),
        I(0x18,1,1,0,map),I(0,0,0,0,0),
        I(0xbf,2,10,0,0),I(0x07,2,0,0,-4),I(0x85,0,0,0,BPF_FUNC_map_lookup_elem),
        I(0x15,0,0,8,0),I(0xb7,1,0,0,1),I(0xdb,0,1,0,0),
        I(0x79,1,6,16,0),I(0x7b,0,1,8,0),
        I(0x79,1,6,24,0),I(0x7b,0,1,16,0),
        I(0xb7,0,0,0,0),I(0xb7,0,0,0,0),I(0x95,0,0,0,0)
    };
    if(enter!=1){code[16]=I(0x55,1,0,2,enter==2?0:-512);code[17]=I(0xb7,1,0,0,1);code[18]=I(0xdb,0,1,16,0);}
    if(enter==2)code[3]=I(0xb7,0,0,0,0);
    char log[8192]={0};union bpf_attr a={.prog_type=BPF_PROG_TYPE_TRACEPOINT,
        .insn_cnt=sizeof(code)/sizeof(code[0]),.insns=(uintptr_t)code,.license=(uintptr_t)"GPL",
        .log_level=1,.log_size=sizeof(log),.log_buf=(uintptr_t)log};
    int fd=bpf(BPF_PROG_LOAD,&a);if(fd<0)fprintf(stderr,"%s\n",log);CHECK(fd>=0);return fd;
}
static unsigned trace_id(const char *which)
{
    char path[160];snprintf(path,sizeof(path),"/sys/kernel/tracing/events/raw_syscalls/%s/id",which);
    FILE *f=fopen(path,"r");CHECK(f);unsigned id;CHECK(fscanf(f,"%u",&id)==1);fclose(f);return id;
}
static int perf(unsigned id,int pid,int cpu,int flags)
{
    struct perf_event_attr attr={.type=PERF_TYPE_TRACEPOINT,.size=sizeof(attr),.config=id,
        .sample_period=1,.sample_type=PERF_SAMPLE_RAW,.disabled=1,.wakeup_events=1};
    return syscall(__NR_perf_event_open,&attr,pid,cpu,-1,flags);
}
static int link_to(int prog,int event)
{
    union bpf_attr a={0};a.link_create.prog_fd=prog;a.link_create.target_fd=event;
    a.link_create.attach_type=BPF_PERF_EVENT;a.link_create.perf_event.bpf_cookie=0x12345678;
    int fd=bpf(BPF_LINK_CREATE,&a);CHECK(fd>=0);return fd;
}
static void cookie_test(unsigned event_id)
{
    int map=make_map();
    struct bpf_insn code[]={
        I(0xbf,6,1,0,0),I(0x62,10,0,-4,0),I(0x18,1,1,0,map),I(0,0,0,0,0),
        I(0xbf,2,10,0,0),I(0x07,2,0,0,-4),I(0x85,0,0,0,1),I(0x15,0,0,5,0),
        I(0xbf,7,0,0,0),I(0xbf,1,6,0,0),I(0x85,0,0,0,174),I(0x7b,7,0,0,0),
        I(0xb7,0,0,0,0),I(0x95,0,0,0,0)
    };
    union bpf_attr a={.prog_type=BPF_PROG_TYPE_TRACEPOINT,.insn_cnt=sizeof(code)/sizeof(code[0]),
        .insns=(uintptr_t)code,.license=(uintptr_t)"GPL"};
    int prog=bpf(BPF_PROG_LOAD,&a);CHECK(prog>=0);
    int event=perf(event_id,0,-1,PERF_FLAG_FD_CLOEXEC);CHECK(event>=0);
    int link=link_to(prog,event);CHECK(get(map,0).count==0x12345678);
    close(link);close(event);event=perf(event_id,0,-1,0);CHECK(event>=0);
    CHECK(!ioctl(event,PERF_EVENT_IOC_SET_BPF,prog));CHECK(get(map,0).count==0);
    close(event);close(prog);close(map);
}
static void enable(int fd){CHECK(!ioctl(fd,PERF_EVENT_IOC_ENABLE,0));}
static void disable(int fd){CHECK(!ioctl(fd,PERF_EVENT_IOC_DISABLE,0));}
static volatile sig_atomic_t interrupted;
static void handler(int sig){(void)sig;interrupted=1;}
int main(int argc,char **argv)
{
    if(argc>1)return 0;
    unsigned enter_id=trace_id("sys_enter"),exit_id=trace_id("sys_exit");
    int imap=make_map(),omap=make_map(),iprog=program(imap,1),oprog=program(omap,0);
    int in=perf(enter_id,0,-1,0),out=perf(exit_id,0,-1,PERF_FLAG_FD_CLOEXEC);
    CHECK(in>=0 && out>=0);CHECK(!(fcntl(in,F_GETFD)&FD_CLOEXEC));CHECK(fcntl(out,F_GETFD)&FD_CLOEXEC);
    CHECK(!ioctl(in,PERF_EVENT_IOC_SET_BPF,iprog));int link=link_to(oprog,out);
    enable(in);enable(out);
    for(int i=0;i<64;i++)CHECK(syscall(__NR_getpgid,0)>=0);
    for(int i=0;i<7;i++)CHECK(syscall(__NR_getpid)>0);
    CHECK(syscall(1000,123,456)==-1 && errno==ENOSYS);
    disable(in);disable(out);
    struct record a=get(imap,__NR_getpgid),b=get(omap,__NR_getpgid);
    CHECK(a.count==64 && a.first==0 && b.count==64 && b.first>=0);
    CHECK(get(imap,__NR_getpid).count==7 && get(omap,__NR_getpid).count==7);
    a=get(imap,1000);b=get(omap,1000);CHECK(a.count==1 && a.first==123 && a.second==456);
    CHECK(b.count==1 && b.first==-ENOSYS);
    CHECK(syscall(__NR_getpgid,0)>=0);CHECK(get(imap,__NR_getpgid).count==65);
    enable(in);enable(out);
    /* The child waits for the actual read entry observed by BPF before it
     * sends the signal. A pending signal before sleep is also a valid edge. */
    int pipefd[2];CHECK(!pipe(pipefd));
    struct sigaction sa={.sa_handler=handler};sigemptyset(&sa.sa_mask);CHECK(!sigaction(SIGUSR1,&sa,NULL));
    pid_t parent=getpid(),child=fork();CHECK(child>=0);
    if(!child){
        while(!get(imap,__NR_read).count)sched_yield();
        CHECK(!kill(parent,SIGUSR1));_exit(0);
    }
    char byte;CHECK(read(pipefd[0],&byte,1)==-1 && errno==EINTR && interrupted);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    b=get(omap,__NR_read);
    CHECK(b.count==2 && b.first==-EINTR && b.second==1);
    close(pipefd[0]);close(pipefd[1]);
    /* Closing the perf FD preserves a modern link, but closing the legacy
     * perf FD stops its program even while the program FD stays open. */
    close(in);close(out);CHECK(syscall(__NR_getpgid,0)>=0);
    CHECK(get(imap,__NR_getpgid).count==65 && get(omap,__NR_getpgid).count==66);
    union bpf_attr detach={0};detach.link_detach.link_fd=link;
    CHECK(bpf(BPF_LINK_DETACH,&detach)==-1 && errno==EOPNOTSUPP);close(link);
    CHECK(syscall(__NR_getpgid,0)>=0);CHECK(get(omap,__NR_getpgid).count==66);
    close(iprog);close(oprog);close(imap);close(omap);
    /* A shared tracepoint program sees the real child clone return and the
     * successful exec return. Neither terminating exit call has an exit event. */
    int fmap=make_map(),fprog=program(fmap,2),fevent=perf(exit_id,0,-1,PERF_FLAG_FD_CLOEXEC);
    CHECK(fevent>=0 && !ioctl(fevent,PERF_EVENT_IOC_SET_BPF,fprog));enable(fevent);
    child=syscall(__NR_clone,SIGCHLD,0,0,0,0);CHECK(child>=0);
    if(!child){execl(argv[0],argv[0],"exec-child",NULL);_exit(127);}
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    b=get(fmap,__NR_clone);CHECK(b.count==2 && b.second==1);
    b=get(fmap,__NR_execve);CHECK(b.count==1 && b.first==0 && b.second==1);
    CHECK(!get(fmap,__NR_exit).count && !get(fmap,__NR_exit_group).count);
    close(fevent);close(fprog);close(fmap);
    cookie_test(enter_id);
    puts("BPF TRACE PASS counts args results fast unknown restart disable close link clone exec cookie");
    return 0;
}
