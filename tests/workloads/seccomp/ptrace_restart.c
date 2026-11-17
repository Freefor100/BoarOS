/* No TRACE policy is installed: this exercises ordinary PTRACE_SYSCALL. */
#define main seccomp_basic_main
#include "basic.c"
#undef main
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
struct trace_info {uint8_t op,pad[3];uint32_t arch;uint64_t pc,sp,nr,args[6];};
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
static void entry_restart(void)
{
    mkdir("/proc",0755);if(access("/proc/self/stat",R_OK))CHECK(!mount("proc","/proc","proc",0,0));
    for(unsigned kind=0;kind<6;kind++) {
        pid_t parent=getpid(),p=fork();CHECK(p>=0);
        if(!p) {
            pid_t me=getpid();if(ptrace(PTRACE_TRACEME,0,0,0)){if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}
            struct sigaction sa={.sa_sigaction=restart_handler,.sa_flags=SA_SIGINFO};CHECK(!sigaction(SIGUSR1,&sa,0));
            CHECK(!kill(me,SIGSTOP));
            struct timespec duration={(kind<2||kind>=4)?60:1,0};int word=0;
            long r=(kind&1)?syscall(SYS_futex,&word,FUTEX_WAIT,0,&duration,0,0):syscall(SYS_nanosleep,&duration,0);
            if(kind<2)CHECK(r==parent);
            else if(kind>=4)CHECK(r==-1&&errno==ENOSYS);
            else if(kind&1)CHECK(r==-1&&errno==ETIMEDOUT);
            else CHECK(r==0);
            CHECK(!notify_call(me)&&restart_notified);_exit(0);
        }
        int s;CHECK(waitpid(p,&s,0)==p);
        if(WIFEXITED(s)&&WEXITSTATUS(s)==77){puts("SECCOMP ptrace-entry restart rewrite SKIP host policy");continue;}
        CHECK(WIFSTOPPED(s));CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)(uintptr_t)PTRACE_O_TRACESYSGOOD));
        CHECK(!ptrace(PTRACE_CONT,p,0,0));wait_sleeping(p);CHECK(!kill(p,SIGSTOP));
        CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGSTOP);
        CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGSTOP));
        CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGSTOP);
        CHECK(!kill(p,SIGCONT));CHECK(!ptrace(PTRACE_SYSCALL,p,0,0));
        for(;;) {
            CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s));
            if(WSTOPSIG(s)==(SIGTRAP|0x80)) {
                struct trace_info current={0};CHECK(ptrace(PTRACE_GET_SYSCALL_INFO,p,(void *)sizeof(current),&current)>0);
                if(current.op==1&&current.nr==SYS_restart_syscall)break;
                CHECK(!ptrace(PTRACE_SYSCALL,p,0,0));continue;
            }
            CHECK(WSTOPSIG(s)==SIGCONT);CHECK(!ptrace(PTRACE_SYSCALL,p,0,(void *)SIGCONT));
        }
        struct trace_info info={0};CHECK(ptrace(PTRACE_GET_SYSCALL_INFO,p,(void *)sizeof(info),&info)==80);
        CHECK(info.nr==SYS_restart_syscall);
        uint64_t reg[64]={0};struct iovec iov={reg,sizeof(reg)};CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)1,&iov));
        if(kind<2)reg[nr_index()]=SYS_getppid;
        if(kind>=4)reg[nr_index()]=UINT64_C(0x100000000)+SYS_restart_syscall;
        CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)1,&iov));
        CHECK(!ptrace(PTRACE_CONT,p,0,0));
        CHECK(waitpid(p,&s,0)==p&&WIFSTOPPED(s)&&WSTOPSIG(s)==SIGUSR1);
        CHECK(!ptrace(PTRACE_CONT,p,0,(void *)SIGUSR1));joined(p,0);
    }
    puts("SECCOMP ptrace-entry restart rewrite checks complete");
}

int main(int argc,char **argv)
{
    (void)argc;setvbuf(stdout,0,_IONBF,0);alarm(40);self=argv[0];initial_mode=prctl(PR_GET_SECCOMP);
    entry_restart();puts("SECCOMP ALL PASS");if(getpid()==1)reboot(RB_POWER_OFF);return 0;
}
