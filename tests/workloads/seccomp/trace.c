/* Share the raw-call helpers and baseline behavior checks, not a second ABI. */
#define main seccomp_basic_main
#include "basic.c"
#undef main

#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
static int guest_mode;
struct trace_info {uint8_t op,pad[3];uint32_t arch;uint64_t pc,sp,nr,args[6];uint32_t data;};
static void trace_no_tracer(void)
{ nnp();deny(SYS_getpid,SECCOMP_RET_TRACE|42);errno=0;CHECK(syscall(SYS_getpid)==-1&&errno==ENOSYS); }
static unsigned nr_index(void)
{
#if defined(__riscv)
 return 17;
#elif defined(__loongarch__)
 return 11;
#else
 return 15;
#endif
}
static unsigned return_index(void)
{
#if defined(__riscv)
 return 10;
#elif defined(__loongarch__)
 return 4;
#else
 return 10;
#endif
}
static void trace_trap(int sig,siginfo_t *info,void *context)
{
    CHECK(sig==SIGSYS&&info->si_code==1&&info->si_syscall==SYS_getppid&&info->si_errno==77);
    ucontext_t *u=context;
#if defined(__riscv)
    u->uc_mcontext.__gregs[10]=901;
#elif defined(__loongarch__)
    u->uc_mcontext.__gregs[4]=901;
#else
    u->uc_mcontext.gregs[REG_RAX]=901;
#endif
}
/* modes: unchanged; rewrite denied; explicit skip; second TRACE; no option;
 * higher-priority ERRNO; fatal stop; ignored event resume signal. */
static void trace_case(unsigned mode)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p) {
        pid_t me=getpid();
        if(ptrace(PTRACE_TRACEME,0,0,0)) {if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}
        struct sigaction sa={.sa_sigaction=restart_handler,.sa_flags=SA_SIGINFO};
        CHECK(!sigaction(SIGUSR1,&sa,0));
        nnp();deny(SYS_getpid,SECCOMP_RET_TRACE|42);
        deny(SYS_getppid,mode==3?(SECCOMP_RET_TRACE|77):mode==8?SECCOMP_RET_KILL_PROCESS:
            mode==9?(SECCOMP_RET_TRAP|77):(SECCOMP_RET_ERRNO|EACCES));
        if(mode==9){sa.sa_sigaction=trace_trap;CHECK(!sigaction(SIGSYS,&sa,0));}
        if(mode==12) {
            struct sock_filter f[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
                BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpid,0,3),BPF_STMT(BPF_LD|BPF_W|BPF_ABS,16),
                BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,9876,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EACCES),
                BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};CHECK(!install(f,6));
        }
        if(mode==10) {
            struct sock_filter f[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
                BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpid,0,3),
                BPF_STMT(BPF_LD|BPF_W|BPF_ABS,16),
                BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,1234,1,0),
                BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
                BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
            CHECK(!install(f,6));
        }
        if(mode==5)deny(SYS_getpid,SECCOMP_RET_ERRNO|EBUSY);
        CHECK(!syscall(SYS_kill,me,SIGSTOP));
        errno=0;long r=syscall(SYS_getpid,1234,5678);
        if(mode==1||mode==12)CHECK(r==-1&&errno==EACCES);
        else if(mode==2)CHECK(r==-1&&errno==512);
        else if(mode==4)CHECK(r==-1&&errno==ENOSYS);
        else if(mode==5)CHECK(r==-1&&errno==EBUSY);
        else if(mode==9)CHECK(r==901);
        else if(mode==3)CHECK(r>0&&r!=me);
        else CHECK(r==me);
        if(mode==7)CHECK(!restart_notified);
        if(mode==2)CHECK(!notify_call(me)&&restart_notified);
        if(mode==11) {
            pid_t child=fork();CHECK(child>=0);
            if(!child){errno=0;CHECK(syscall(SYS_getpid)==-1&&errno==ENOSYS);_exit(0);}
            joined(child,0);execl(self,self,"trace-exec",(char *)0);CHECK(0);
        }
        _exit(0);
    }
    int s;CHECK(waitpid(p,&s,0)==p);
    if(WIFEXITED(s)&&WEXITSTATUS(s)==77) {puts("SECCOMP TRACE ptrace SKIP host policy");return;}
    CHECK(WIFSTOPPED(s)&&WSTOPSIG(s)==SIGSTOP);
    if(mode!=4)CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)(uintptr_t)(PTRACE_O_TRACESECCOMP|(mode==11?PTRACE_O_TRACEEXEC:0))));
    CHECK(!ptrace(PTRACE_CONT,p,0,0));
    if(mode==4||mode==5){joined(p,0);return;}
    CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGTRAP&&(s>>16)==7);
    unsigned long msg=0;CHECK(!ptrace(PTRACE_GETEVENTMSG,p,0,&msg)&&msg==42);
    struct trace_info info={0};CHECK(ptrace(PTRACE_GET_SYSCALL_INFO,p,(void *)sizeof(info),&info)==84);
    unsigned char short_info[25];memset(short_info,0xcc,sizeof(short_info));
    CHECK(ptrace(PTRACE_GET_SYSCALL_INFO,p,(void *)24,short_info)==84&&short_info[24]==0xcc);
    CHECK(info.op==3&&info.nr==SYS_getpid&&info.args[0]==1234&&info.args[1]==5678&&info.data==42);
    uint64_t reg[64]={0};struct iovec iov={reg,sizeof(reg)};CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)1,&iov));
    if(mode==1||mode==3||mode==8||mode==9)reg[nr_index()]=SYS_getppid;
    if(mode==2){reg[nr_index()]=(uint64_t)-1;reg[return_index()]=(uint64_t)-512;}
    if(mode==10)reg[return_index()]=9876;
#if defined(__loongarch__)
    if(mode==12)reg[32]=9876;
#endif
    CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)1,&iov));
    if(mode==6){CHECK(!kill(p,SIGKILL));joined(p,SIGKILL);return;}
    if(mode==13){CHECK(!ptrace(PTRACE_DETACH,p,0,0));joined(p,0);return;}
    CHECK(!ptrace(PTRACE_CONT,p,0,(void *)(uintptr_t)(mode==7?SIGUSR1:0)));
    if(mode==11) {
        for(;;) {
            CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s));
            if((s>>16)==4)break;
            CHECK(WSTOPSIG(s)==SIGCHLD);CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGCHLD));
        }
        CHECK(!ptrace(PTRACE_CONT,p,0,0));
        CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&(s>>16)==7);
        CHECK(!ptrace(PTRACE_CONT,p,0,0));
    }
    if(mode==8){joined(p,SIGSYS);return;}
    if(mode==9){CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGSYS);CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGSYS));}
    if(mode==2){CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGUSR1);CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGUSR1));}
    joined(p,0);
}
static int sync_command[2],sync_ack[2];
static void *trace_sync_worker(void *unused)
{
    (void)unused;char byte;CHECK(read(sync_command[0],&byte,1)==1);
    struct sock_filter f[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpid,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EBUSY),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    struct sock_fprog p={4,f};CHECK(!syscall(SYS_seccomp,1,SECCOMP_FILTER_FLAG_TSYNC,&p));
    CHECK(write(sync_ack[1],"x",1)==1);return 0;
}
static void trace_sync(void)
{
    CHECK(!pipe(sync_command)&&!pipe(sync_ack));pid_t p=fork();CHECK(p>=0);
    if(!p) {
        pid_t me=getpid();if(ptrace(PTRACE_TRACEME,0,0,0)){if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}
        nnp();deny(SYS_getpid,SECCOMP_RET_TRACE|42);pthread_t worker;
        CHECK(!pthread_create(&worker,0,trace_sync_worker,0));CHECK(!kill(me,SIGSTOP));
        errno=0;CHECK(syscall(SYS_getpid)==-1&&errno==EBUSY);CHECK(!pthread_join(worker,0));_exit(0);
    }
    int s;CHECK(waitpid(p,&s,0)==p);
    if(WIFEXITED(s)&&WEXITSTATUS(s)==77){puts("SECCOMP TRACE TSYNC SKIP host policy");goto done;}
    CHECK(WIFSTOPPED(s));CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)PTRACE_O_TRACESECCOMP));
    CHECK(!ptrace(PTRACE_CONT,p,0,0));CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&(s>>16)==7);
    CHECK(write(sync_command[1],"x",1)==1);char byte;CHECK(read(sync_ack[0],&byte,1)==1);
    CHECK(!ptrace(PTRACE_CONT,p,0,0));joined(p,0);puts("SECCOMP TRACE stopped TSYNC recheck PASS");
done:
    for(unsigned i=0;i<2;i++){close(sync_command[i]);close(sync_ack[i]);}
}
static void trace_order(void)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p){pid_t me=getpid();if(ptrace(PTRACE_TRACEME,0,0,0)){if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}
        nnp();deny(SYS_getpid,SECCOMP_RET_TRACE|42);CHECK(!kill(me,SIGSTOP));CHECK(syscall(SYS_getpid)==me);_exit(0);}
    int s;CHECK(waitpid(p,&s,0)==p);
    if(WIFEXITED(s)&&WEXITSTATUS(s)==77){puts("SECCOMP TRACE ordering SKIP host policy");return;}
    CHECK(WIFSTOPPED(s));CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)(PTRACE_O_TRACESECCOMP|PTRACE_O_TRACESYSGOOD)));
    unsigned stage=0;
    while(stage<3){CHECK(!ptrace(PTRACE_SYSCALL,p,0,0));CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s));
        struct trace_info info={0};CHECK(ptrace(PTRACE_GET_SYSCALL_INFO,p,(void *)sizeof(info),&info)>0);
        if(!stage&&info.op==1&&info.nr==SYS_getpid){stage=1;continue;}
        if(!stage)continue;
        CHECK(info.op==(stage==1?3:2));CHECK(stage!=1||((s>>16)==7&&info.data==42));stage++;
    }
    CHECK(!ptrace(PTRACE_CONT,p,0,0));joined(p,0);puts("SECCOMP TRACE entry/event/exit order PASS");
}
/* Directly observe the dispatcher boundary moved by TRACE rewriting. */
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
struct trace_record {uint64_t count,arg0,arg1;};
static int trace_bpf(unsigned cmd,union bpf_attr *attr)
{return syscall(SYS_bpf,cmd,attr,sizeof(*attr));}
#define BI(c,d,s,o,i) ((struct bpf_insn){.code=c,.dst_reg=d,.src_reg=s,.off=o,.imm=i})
static int trace_enter_observer(int *map)
{
    union bpf_attr attr={.map_type=BPF_MAP_TYPE_ARRAY,.key_size=4,.value_size=sizeof(struct trace_record),.max_entries=1024};
    *map=trace_bpf(BPF_MAP_CREATE,&attr);
    if(*map<0&&!guest_mode&&(errno==EPERM||errno==EACCES)){puts("SECCOMP TRACE BPF observer SKIP host permission");return -1;}
    CHECK(*map>=0);
    struct bpf_insn code[]={BI(0xbf,6,1,0,0),BI(0x61,1,6,8,0),BI(0x63,10,1,-4,0),
        BI(0x18,1,1,0,*map),BI(0,0,0,0,0),BI(0xbf,2,10,0,0),BI(0x07,2,0,0,-4),
        BI(0x85,0,0,0,BPF_FUNC_map_lookup_elem),BI(0x15,0,0,6,0),BI(0xb7,1,0,0,1),BI(0xdb,0,1,0,0),
        BI(0x79,1,6,16,0),BI(0x7b,0,1,8,0),BI(0x79,1,6,24,0),BI(0x7b,0,1,16,0),
        BI(0xb7,0,0,0,0),BI(0x95,0,0,0,0)};
    attr=(union bpf_attr){.prog_type=BPF_PROG_TYPE_TRACEPOINT,.insn_cnt=sizeof(code)/sizeof(code[0]),
        .insns=(uintptr_t)code,.license=(uintptr_t)"GPL"};
    int prog=trace_bpf(BPF_PROG_LOAD,&attr);CHECK(prog>=0);
    FILE *file=fopen("/sys/kernel/tracing/events/raw_syscalls/sys_enter/id","r");CHECK(file);
    unsigned id;CHECK(fscanf(file,"%u",&id)==1);fclose(file);
    struct perf_event_attr perf={.type=PERF_TYPE_TRACEPOINT,.size=sizeof(perf),.config=id,
        .sample_period=1,.sample_type=PERF_SAMPLE_RAW,.disabled=1};
    int event=syscall(SYS_perf_event_open,&perf,0,-1,-1,0);CHECK(event>=0);
    CHECK(!ioctl(event,PERF_EVENT_IOC_SET_BPF,prog));CHECK(!ioctl(event,PERF_EVENT_IOC_ENABLE,0));close(prog);return event;
}
static struct trace_record trace_record_read(int map,unsigned nr)
{
    struct trace_record record={0};union bpf_attr attr={.map_fd=map,.key=(uintptr_t)&nr,.value=(uintptr_t)&record};
    CHECK(!trace_bpf(BPF_MAP_LOOKUP_ELEM,&attr));return record;
}
#undef BI

static const char replacement[]="changed";
static unsigned arg1_index(void)
{
#if defined(__riscv)
 return 11;
#elif defined(__loongarch__)
 return 5;
#else
 return 13;
#endif
}
static void trace_write(int fatal)
{
    int pipes[2];CHECK(!pipe(pipes));pid_t p=fork();CHECK(p>=0);
    if(!p){pid_t me=getpid();if(ptrace(PTRACE_TRACEME,0,0,0)){if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}
        int map=-1,event=-1;
        event=trace_enter_observer(&map);
        nnp();deny(SYS_getpid,SECCOMP_RET_TRACE|42);CHECK(!kill(me,SIGSTOP));
        CHECK(syscall(SYS_getpid,pipes[1],"initial",7)==7);
        if(event>=0){struct trace_record r=trace_record_read(map,SYS_write);
            CHECK(r.count==1&&r.arg0==(uint64_t)pipes[1]&&r.arg1==(uintptr_t)replacement);
            CHECK(trace_record_read(map,SYS_getpid).count==0);
            deny(SYS_getppid,SECCOMP_RET_ERRNO|EACCES);CHECK(syscall(SYS_getppid)==-1&&errno==EACCES);
            CHECK(trace_record_read(map,SYS_getppid).count==0);close(event);close(map);}
        _exit(0);}
    int s;CHECK(waitpid(p,&s,0)==p);
    if(WIFEXITED(s)&&WEXITSTATUS(s)==77){puts("SECCOMP TRACE rewrite write SKIP host policy");goto done;}
    CHECK(WIFSTOPPED(s));CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)PTRACE_O_TRACESECCOMP));
    CHECK(!ptrace(PTRACE_CONT,p,0,0));CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&(s>>16)==7);
    uint64_t reg[64]={0};struct iovec iov={reg,sizeof(reg)};CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)1,&iov));
    reg[nr_index()]=SYS_write;reg[arg1_index()]=(uintptr_t)replacement;
    CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)1,&iov));
    if(fatal){CHECK(!kill(p,SIGKILL));joined(p,SIGKILL);close(pipes[1]);pipes[1]=-1;
        char byte;CHECK(read(pipes[0],&byte,1)==0);puts("SECCOMP TRACE fatal zero write effect PASS");goto done;}
    CHECK(!ptrace(PTRACE_CONT,p,0,0));joined(p,0);
    char data[7];CHECK(read(pipes[0],data,7)==7&&!memcmp(data,replacement,7));puts("SECCOMP TRACE modified syscall arguments PASS");
done:close(pipes[0]);close(pipes[1]);
}

#if defined(__riscv) || defined(__loongarch__)
extern char trace_pc_before[],trace_pc_after[];
static __attribute__((noinline)) long trace_pc_call(void)
{
#if defined(__riscv)
    register long a __asm__("a0")=1234,n __asm__("a7")=SYS_getpid;
    __asm__ volatile(".option push\n.option norvc\necall\n.global trace_pc_before\ntrace_pc_before:\nli a0,999\n.global trace_pc_after\ntrace_pc_after:\n.option pop" : "+r"(a):"r"(n):"memory");
#else
    register long a __asm__("$a0")=1234,n __asm__("$a7")=SYS_getpid;
    __asm__ volatile("syscall 0\n.global trace_pc_before\ntrace_pc_before:\naddi.d $a0,$zero,999\n.global trace_pc_after\ntrace_pc_after:" : "+r"(a):"r"(n):"memory");
#endif
    return a;
}
static void trace_pc(void)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p){pid_t me=getpid();CHECK(!ptrace(PTRACE_TRACEME,0,0,0));nnp();
        deny(SYS_getpid,SECCOMP_RET_TRACE|42);
        struct sock_filter f[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,8),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(uint32_t)(uintptr_t)trace_pc_after,0,1),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|E2BIG),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
        CHECK(!install(f,4));CHECK(!kill(me,SIGSTOP));CHECK(trace_pc_call()==-E2BIG);_exit(0);}
    int s;CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s));CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)PTRACE_O_TRACESECCOMP));
    CHECK(!ptrace(PTRACE_CONT,p,0,0));CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&(s>>16)==7);
    uint64_t reg[64]={0};struct iovec iov={reg,sizeof(reg)};CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)1,&iov));
#if defined(__riscv)
    unsigned pc=0;
#else
    unsigned pc=33;
#endif
    CHECK(reg[pc]==(uintptr_t)trace_pc_before);reg[pc]=(uintptr_t)trace_pc_after;
    CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)1,&iov));CHECK(!ptrace(PTRACE_CONT,p,0,0));joined(p,0);
    puts("SECCOMP TRACE changed PC recheck/return PASS");
}
#else
static void trace_pc(void){puts("SECCOMP TRACE target PC SKIP host architecture");}
#endif

static void trace_restart(void)
{
    mkdir("/proc",0755);if(access("/proc/self/stat",R_OK))CHECK(!mount("proc","/proc","proc",0,0));
    for(unsigned kind=0;kind<6;kind++) {
        pid_t parent=getpid(),p=fork();CHECK(p>=0);
        if(!p) {
            pid_t me=getpid();if(ptrace(PTRACE_TRACEME,0,0,0)){if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}
            struct sigaction sa={.sa_sigaction=restart_handler,.sa_flags=SA_SIGINFO};CHECK(!sigaction(SIGUSR1,&sa,0));
            nnp();deny(SYS_restart_syscall,SECCOMP_RET_TRACE|42);CHECK(!kill(me,SIGSTOP));
            struct timespec duration={(kind<2||kind>=4)?60:1,0};int word=0;
            long r=(kind&1)?syscall(SYS_futex,&word,FUTEX_WAIT,0,&duration,0,0):syscall(SYS_nanosleep,&duration,0);
            if(kind<2)CHECK(r==parent);
            else if(kind>=4)CHECK(r==-1&&errno==ENOSYS);
            else if(kind&1)CHECK(r==-1&&errno==ETIMEDOUT);
            else CHECK(r==0);
            CHECK(!notify_call(me)&&restart_notified);_exit(0);
        }
        int s;CHECK(waitpid(p,&s,0)==p);
        if(WIFEXITED(s)&&WEXITSTATUS(s)==77){puts("SECCOMP TRACE restart rewrite SKIP host policy");continue;}
        CHECK(WIFSTOPPED(s));CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)PTRACE_O_TRACESECCOMP));
        CHECK(!ptrace(PTRACE_CONT,p,0,0));wait_sleeping(p);CHECK(!kill(p,SIGSTOP));
        CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGSTOP);
        CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGSTOP));
        CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGSTOP);
        CHECK(!kill(p,SIGCONT));CHECK(!ptrace(PTRACE_CONT,p,0,0));
        for(;;) {
            CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s));
            if((s>>16)==7)break;
            CHECK(WSTOPSIG(s)==SIGCONT);CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGCONT));
        }
        struct trace_info info={0};CHECK(ptrace(PTRACE_GET_SYSCALL_INFO,p,(void *)sizeof(info),&info)==84);
        CHECK(info.nr==SYS_restart_syscall);
        uint64_t reg[64]={0};struct iovec iov={reg,sizeof(reg)};CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)1,&iov));
        if(kind<2)reg[nr_index()]=SYS_getppid;
        if(kind>=4)reg[nr_index()]=UINT64_C(0x100000000)+SYS_restart_syscall;
        CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)1,&iov));
        CHECK(!ptrace(PTRACE_CONT,p,0,0));
        CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGUSR1);
        CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGUSR1));joined(p,0);
    }
    puts("SECCOMP TRACE restart rewritten success/later signal checks complete");
}

static void trace_tracer_exit(int exitkill)
{
    int bytes[2],identity[2];CHECK(!pipe(bytes)&&!pipe(identity));
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER,1));pid_t tracer=fork();CHECK(tracer>=0);
    if(!tracer) {
        close(bytes[0]);close(identity[0]);pid_t target=fork();CHECK(target>=0);
        if(!target) {
            close(identity[1]);pid_t me=getpid();
            if(ptrace(PTRACE_TRACEME,0,0,0)){if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}
            nnp();deny(SYS_getpid,SECCOMP_RET_TRACE|42);CHECK(!kill(me,SIGSTOP));
            CHECK(syscall(SYS_getpid,bytes[1],"x",1)==1);_exit(0);
        }
        CHECK(write(identity[1],&target,sizeof(target))==sizeof(target));close(identity[1]);
        int status;CHECK(waitpid(target,&status,0)==target);
        if(WIFEXITED(status)&&WEXITSTATUS(status)==77)_exit(77);
        CHECK(WIFSTOPPED(status));CHECK(!ptrace(PTRACE_SETOPTIONS,target,0,
            (void *)(uintptr_t)(PTRACE_O_TRACESECCOMP|(exitkill?PTRACE_O_EXITKILL:0))));
        CHECK(!ptrace(PTRACE_CONT,target,0,0));
        CHECK(waitpid(target,&status,0)==target&&WIFSTOPPED(status)&&(status>>16)==7);
        uint64_t reg[64]={0};struct iovec iov={reg,sizeof(reg)};CHECK(!ptrace(PTRACE_GETREGSET,target,(void *)1,&iov));
        reg[nr_index()]=SYS_write;CHECK(!ptrace(PTRACE_SETREGSET,target,(void *)1,&iov));
        _exit(0); /* Exit itself releases the event; no CONT or DETACH. */
    }
    close(bytes[1]);close(identity[1]);pid_t target;CHECK(read(identity[0],&target,sizeof(target))==sizeof(target));close(identity[0]);
    int status;CHECK(waitpid(tracer,&status,0)==tracer&&WIFEXITED(status));
    if(WEXITSTATUS(status)==77)puts("SECCOMP TRACE tracer exit SKIP host policy");
    else {CHECK(WEXITSTATUS(status)==0);joined(target,exitkill?SIGKILL:0);
        char byte;CHECK(read(bytes[0],&byte,1)==(exitkill?0:1));
        puts(exitkill?"SECCOMP TRACE EXITKILL zero write effect PASS":"SECCOMP TRACE tracer exit resumes selected syscall PASS");}
    close(bytes[0]);CHECK(!prctl(PR_SET_CHILD_SUBREAPER,0));
}

int main(int argc,char **argv)
{
    if(argc>1&&!strcmp(argv[1],"trace-exec")){CHECK(syscall(SYS_getpid)>0);return 0;}
    setvbuf(stdout,0,_IONBF,0);alarm(40);self=argv[0];initial_mode=prctl(PR_GET_SECCOMP);
    guest_mode=getpid()==1;
    if(guest_mode){mkdir("/sys",0755);CHECK(!mount("sysfs","/sys","sysfs",0,0));
        mkdir("/sys/kernel",0755);mkdir("/sys/kernel/tracing",0755);
        CHECK(!mount("tracefs","/sys/kernel/tracing","tracefs",0,0));}
    child_case(trace_no_tracer,0);puts("SECCOMP TRACE no-tracer PASS");
    for(unsigned mode=0;mode<12;mode++)trace_case(mode);
#if defined(__loongarch__)
    trace_case(12);
#endif
    puts("SECCOMP TRACE event/recheck/skip/priority/fatal PASS");
    trace_sync();trace_order();trace_write(0);trace_write(1);trace_pc();trace_restart();trace_case(13);trace_tracer_exit(0);trace_tracer_exit(1);
    puts("SECCOMP ALL PASS");if(getpid()==1)reboot(RB_POWER_OFF);return 0;
}
