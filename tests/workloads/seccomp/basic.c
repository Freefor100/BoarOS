#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ucontext.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <linux/futex.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"SECCOMP FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static const char *self;
static int initial_mode;
static void nnp(void){CHECK(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));}
static int install(struct sock_filter *f,unsigned n)
{struct sock_fprog p={.len=n,.filter=f};return syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&p);}
static void deny(long nr,unsigned action)
{
    struct sock_filter f[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,nr,0,1),BPF_STMT(BPF_RET|BPF_K,action),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    CHECK(!install(f,4));
}
static void joined(pid_t p,int signal)
{int s;CHECK(waitpid(p,&s,0)==p);CHECK(signal?WIFSIGNALED(s)&&WTERMSIG(s)==signal:WIFEXITED(s)&&WEXITSTATUS(s)==0);}
static void child_case(void (*fn)(void),int signal)
{pid_t p=fork();CHECK(p>=0);if(!p){fn();_exit(0);}joined(p,signal);}
static void errors(void)
{
    nnp();
    struct sock_filter good[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    struct sock_fprog p={.len=1,.filter=good};
    CHECK(syscall(SYS_seccomp,55,0,0)==-1&&errno==EINVAL);
    CHECK(syscall(SYS_seccomp,1,0x80000000,&p)==-1&&errno==EINVAL);
    CHECK(syscall(SYS_seccomp,1,0,(void *)1)==-1&&errno==EFAULT);
    p.len=0;CHECK(syscall(SYS_seccomp,1,0,&p)==-1&&errno==EINVAL);
    p.len=4097;CHECK(syscall(SYS_seccomp,1,0,&p)==-1&&errno==EINVAL);
    p.len=1;p.filter=(void *)1;CHECK(syscall(SYS_seccomp,1,0,&p)==-1&&errno==EFAULT);
    struct sock_filter bad[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    CHECK(install(bad,2)==-1&&errno==EINVAL);bad[0].k=64;
    CHECK(install(bad,2)==-1&&errno==EINVAL);bad[0].code=BPF_LD|BPF_W|BPF_MEM;bad[0].k=0;
    CHECK(install(bad,2)==-1&&errno==EINVAL);bad[0].code=BPF_JMP|BPF_JA;bad[0].k=1;
    CHECK(install(bad,2)==-1&&errno==EINVAL);bad[0].code=BPF_ALU|BPF_DIV|BPF_K;bad[0].k=0;
    CHECK(install(bad,2)==-1&&errno==EINVAL);
    uint32_t action=SECCOMP_RET_ALLOW;CHECK(!syscall(SYS_seccomp,2,0,&action));
    action=0x12340000;CHECK(syscall(SYS_seccomp,2,0,&action)==-1&&errno==EOPNOTSUPP);
    uint32_t optional[]={SECCOMP_RET_TRAP,SECCOMP_RET_TRACE,SECCOMP_RET_LOG,SECCOMP_RET_USER_NOTIF};
    for(unsigned i=0;i<sizeof(optional)/sizeof(optional[0]);i++) {
        if(syscall(SYS_seccomp,2,0,&optional[i])) {
            CHECK(errno==EOPNOTSUPP);struct sock_filter f[]={BPF_STMT(BPF_RET|BPF_K,optional[i])};
            CHECK(install(f,1)==-1&&errno==EOPNOTSUPP);
        }
    }
    p.len=1;p.filter=good;CHECK(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p));
    CHECK(prctl(PR_GET_SECCOMP)==2);
    CHECK(prctl(PR_SET_SECCOMP,0)==-1&&errno==EINVAL);
}
static void precedence(void)
{
    nnp();deny(SYS_getpid,SECCOMP_RET_ERRNO|EPERM);
    errno=0;CHECK(syscall(SYS_getpid)==-1&&errno==EPERM);
    deny(SYS_getpid,SECCOMP_RET_ERRNO|EACCES);
    CHECK(syscall(SYS_getpid)==-1&&errno==EACCES);
    deny(SYS_getpid,0x12340000); /* ERRNO precedes unknown action before fail-closed decoding. */
    deny(SYS_getpid,SECCOMP_RET_ALLOW);
    CHECK(syscall(SYS_getpid)==-1&&errno==EACCES);
    /* Filter errno is user policy, never an internal restart request. */
    const unsigned restart_codes[]={512,513,514,516};
    for(unsigned i=0;i<sizeof(restart_codes)/sizeof(restart_codes[0]);i++) {
        deny(SYS_getppid,SECCOMP_RET_ERRNO|restart_codes[i]);
        errno=0;CHECK(syscall(SYS_getppid)==-1&&errno==(int)restart_codes[i]);
    }
    deny(SYS_getppid,SECCOMP_RET_ERRNO|65535);
    CHECK(syscall(SYS_getppid)==-1&&errno==4095);
    deny(SYS_gettid,SECCOMP_RET_ERRNO);
    CHECK(syscall(SYS_gettid)==0);
    pid_t p=fork();CHECK(p>=0);if(!p){CHECK(syscall(SYS_getpid)==-1&&errno==EACCES);_exit(0);}joined(p,0);
    char *args[]={(char *)self,"exec",0};execv(self,args);CHECK(0);
}
static void arguments(void)
{
#if defined(__riscv)
    uint32_t arch=AUDIT_ARCH_RISCV64;
#elif defined(__loongarch__)
    uint32_t arch=AUDIT_ARCH_LOONGARCH64;
#else
    uint32_t arch=AUDIT_ARCH_X86_64;
#endif
    nnp();
    struct sock_filter f[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,arch,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpid,0,6),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,16),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x76543210,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,20),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0xfedcba98,0,2),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|E2BIG),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    CHECK(!install(f,sizeof(f)/sizeof(f[0])));
    CHECK(syscall(SYS_getpid,UINT64_C(0xfedcba9876543210))==-1&&errno==E2BIG);
    CHECK(syscall(SYS_getpid,1)>0);
}
static void kill_thread(void){nnp();deny(SYS_getpid,SECCOMP_RET_KILL_THREAD);syscall(SYS_getpid);CHECK(0);}
static void kill_process(void){nnp();deny(SYS_getpid,SECCOMP_RET_KILL_PROCESS);syscall(SYS_getpid);CHECK(0);}
static void unknown(void){nnp();deny(SYS_getpid,0x12340000);syscall(SYS_getpid);CHECK(0);}
static void strict_bad(void){CHECK(!syscall(SYS_seccomp,0,0,0));syscall(SYS_getpid);__builtin_trap();}
static void strict_ok(void){CHECK(!prctl(PR_SET_SECCOMP,1));syscall(SYS_write,1,"SECCOMP strict write\n",21);syscall(SYS_exit,0);__builtin_trap();}
static void permission(void)
{
    if(getuid()!=0)return;
    CHECK(!setuid(65534));
    if(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0))return;
    struct sock_filter f[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    CHECK(install(f,1)==-1&&errno==EACCES);nnp();CHECK(!install(f,1));
}
static int ready[2],release_thread[2];
static void *thread_filter(void *v)
{
    nnp();deny(SYS_getpid,SECCOMP_RET_ERRNO|EAGAIN);
    int tid=syscall(SYS_gettid);CHECK(write(ready[1],&tid,sizeof(tid))==sizeof(tid));char c;CHECK(read(release_thread[0],&c,1)==1);
    if(v==(void *)3){char *args[]={(char *)self,"exec-thread",0};execv(self,args);CHECK(0);}
    if(v){deny(SYS_getpid,v==(void *)2?SECCOMP_RET_KILL_PROCESS:SECCOMP_RET_KILL_THREAD);syscall(SYS_getpid);__builtin_trap();}
    CHECK(syscall(SYS_getpid)==-1&&errno==EAGAIN);return 0;
}
static void threads(void)
{
    CHECK(!pipe(ready)&&!pipe(release_thread));pthread_t t;
    CHECK(!pthread_create(&t,0,thread_filter,0));int tid;CHECK(read(ready[0],&tid,sizeof(tid))==sizeof(tid));
    CHECK(syscall(SYS_getpid)>0);CHECK(write(release_thread[1],"x",1)==1);CHECK(!pthread_join(t,0));
    CHECK(!pthread_create(&t,0,thread_filter,(void *)1));CHECK(read(ready[0],&tid,sizeof(tid))==sizeof(tid));
    CHECK(write(release_thread[1],"x",1)==1);
    /* A raw fatal thread exit bypasses musl's pthread exit bookkeeping, so
     * pthread_join is not a portable observer. Poll actual kernel TID lifetime. */
    while(!syscall(SYS_tgkill,getpid(),tid,0))sched_yield();
    CHECK(errno==ESRCH);CHECK(syscall(SYS_getpid)>0);
    close(ready[0]);close(ready[1]);close(release_thread[0]);close(release_thread[1]);
}
static void threaded_group_kill(void)
{
    CHECK(!pipe(ready)&&!pipe(release_thread));pthread_t t;int tid;
    CHECK(!pthread_create(&t,0,thread_filter,(void *)2));
    CHECK(read(ready[0],&tid,sizeof(tid))==sizeof(tid));
    CHECK(write(release_thread[1],"x",1)==1);for(;;)pause();
}
static void thread_exec(void)
{
    CHECK(!pipe(ready)&&!pipe(release_thread));pthread_t t;int tid;
    CHECK(!pthread_create(&t,0,thread_filter,(void *)3));
    CHECK(read(ready[0],&tid,sizeof(tid))==sizeof(tid));
    CHECK(write(release_thread[1],"x",1)==1);for(;;)pause();
}
static volatile sig_atomic_t caught;
static void filtered_handler(int n)
{(void)n;caught=syscall(SYS_getpid)==-1&&errno==EPERM;}
static void signals(void)
{
    pid_t me=getpid();struct sigaction action={.sa_handler=filtered_handler};
    CHECK(!sigaction(SIGUSR1,&action,0));nnp();deny(SYS_getpid,SECCOMP_RET_ERRNO|EPERM);
    CHECK(!syscall(SYS_kill,me,SIGUSR1));CHECK(caught);
}
static void strict_handler(int n)
{(void)n;if(syscall(SYS_seccomp,0,0,0))__builtin_trap();}
static void strict_return(void)
{
    pid_t me=getpid();struct sigaction action={.sa_handler=strict_handler};
    CHECK(!sigaction(SIGUSR1,&action,0));CHECK(!syscall(SYS_kill,me,SIGUSR1));
    syscall(SYS_exit,0);__builtin_trap();
}
static void ptrace_rewrite(void)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p){pid_t me=getpid();if(ptrace(PTRACE_TRACEME,0,0,0)){if(initial_mode&&errno==EPERM)_exit(77);CHECK(0);}nnp();deny(SYS_getpid,SECCOMP_RET_ERRNO|EACCES);
        CHECK(!syscall(SYS_kill,me,SIGSTOP));CHECK(syscall(SYS_getppid)==-1&&errno==EACCES);_exit(0);}
    int status;CHECK(waitpid(p,&status,0)==p);if(WIFEXITED(status)&&WEXITSTATUS(status)==77){puts("SECCOMP ptrace SKIP host policy");return;}CHECK(WIFSTOPPED(status));
    CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)PTRACE_O_TRACESYSGOOD));
    CHECK(!ptrace(PTRACE_SYSCALL,p,0,0));CHECK(waitpid(p,&status,0)==p&&WIFSTOPPED(status));
    uint64_t registers[64];struct iovec regs={registers,sizeof(registers)};
    CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)1,&regs));
#if defined(__riscv)
    CHECK(registers[17]==SYS_getppid);registers[17]=SYS_getpid;
#elif defined(__loongarch__)
    CHECK(registers[11]==SYS_getppid);registers[11]=SYS_getpid;
#else
    CHECK(registers[15]==SYS_getppid);registers[15]=SYS_getpid;
#endif
    CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)1,&regs));CHECK(!ptrace(PTRACE_CONT,p,0,0));joined(p,0);
}
extern char seccomp_call_after[];
static __attribute__((noinline)) long ip_call(long input)
{
#if defined(__riscv)
    register long a __asm__("a0")=input;register long n __asm__("a7")=SYS_getpid;
    __asm__ volatile("ecall\n.global seccomp_call_after\nseccomp_call_after:" : "+r"(a) : "r"(n) : "memory");
#elif defined(__loongarch__)
    register long a __asm__("$a0")=input;register long n __asm__("$a7")=SYS_getpid;
    __asm__ volatile("syscall 0\n.global seccomp_call_after\nseccomp_call_after:" : "+r"(a) : "r"(n) : "memory");
#else
    (void)input;long a;
    __asm__ volatile("syscall\n.global seccomp_call_after\nseccomp_call_after:" : "=a"(a) : "a"((long)SYS_getpid) : "memory","rcx","r11");
#endif
    return a;
}
static void instruction_pointer(void)
{
    uintptr_t ip=(uintptr_t)seccomp_call_after;nnp();
    struct sock_filter f[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpid,0,6),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,8),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(uint32_t)ip,0,3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,12),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,ip>>32,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|E2BIG),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
    CHECK(!install(f,sizeof(f)/sizeof(f[0])));CHECK(ip_call(0)==-E2BIG);
}

static volatile sig_atomic_t trapped;
static long trap_input;
static void trap_handler(int sig,siginfo_t *info,void *context)
{
    ucontext_t *uc=context;
    CHECK(sig==SIGSYS&&info->si_signo==SIGSYS&&info->si_code==1);
    CHECK(info->si_errno==0xabcd&&info->si_syscall==SYS_getpid);
    CHECK(info->si_call_addr==(void *)seccomp_call_after);
#if defined(__riscv)
    CHECK(info->si_arch==AUDIT_ARCH_RISCV64);
    CHECK(uc->uc_mcontext.__gregs[0]==(uintptr_t)seccomp_call_after);
    CHECK((long)uc->uc_mcontext.__gregs[10]==trap_input);
    uc->uc_mcontext.__gregs[10]=7654;
#elif defined(__loongarch__)
    CHECK(info->si_arch==AUDIT_ARCH_LOONGARCH64);
    CHECK(uc->uc_mcontext.__pc==(uintptr_t)seccomp_call_after);
    CHECK((long)uc->uc_mcontext.__gregs[4]==trap_input);
    uc->uc_mcontext.__gregs[4]=7654;
#else
    CHECK(info->si_arch==AUDIT_ARCH_X86_64);
    CHECK(uc->uc_mcontext.gregs[REG_RIP]==(greg_t)(uintptr_t)seccomp_call_after);
    CHECK(uc->uc_mcontext.gregs[REG_RAX]==SYS_getpid);
    uc->uc_mcontext.gregs[REG_RAX]=7654;
#endif
    trapped++;
}
static void trap_setup(void)
{
    nnp();struct sigaction sa={.sa_sigaction=trap_handler,.sa_flags=SA_SIGINFO|SA_RESTART};
    CHECK(!sigaction(SIGSYS,&sa,0));
    deny(SYS_getpid,SECCOMP_RET_TRAP|0xabcd);
}
static void trap_return(void)
{
    trap_setup();
    /* TRAP wins over ERRNO. Neither orig_a0 nor filter errno is a kernel restart. */
    deny(SYS_getpid,SECCOMP_RET_ERRNO|EPERM);
    trap_input=1234;CHECK(ip_call(trap_input)==7654&&trapped==1);
    trap_input=-512;CHECK(ip_call(trap_input)==7654&&trapped==2);
    deny(SYS_getppid,SECCOMP_RET_ERRNO|512);
    errno=0;CHECK(syscall(SYS_getppid)==-1&&errno==512);
}
static void trap_blocked(void)
{
    trap_setup();sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGSYS);
    CHECK(!sigprocmask(SIG_BLOCK,&mask,0));ip_call(0);CHECK(0);
}
static void trap_ignored(void)
{trap_setup();CHECK(signal(SIGSYS,SIG_IGN)!=SIG_ERR);ip_call(0);CHECK(0);}

static void *trap_worker(void *unused)
{(void)unused;trap_return();return 0;}
static void trap_thread(void)
{
    pthread_t t;CHECK(!pthread_create(&t,0,trap_worker,0));
    CHECK(!pthread_join(t,0));CHECK(syscall(SYS_getpid)>0&&trapped==2);
}
static void trap_ptrace(void)
{
    pid_t p=fork();CHECK(p>=0);
    if(!p) {
        pid_t me=getpid();
        if(ptrace(PTRACE_TRACEME,0,0,0)) {
            if(initial_mode&&errno==EPERM)_exit(77);
            CHECK(0);
        }
        CHECK(!syscall(SYS_kill,me,SIGSTOP));trap_setup();
        struct sock_filter original[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpid,0,3),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,16),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,1234,1,0),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
#if defined(__riscv) || defined(__loongarch__)
        CHECK(!install(original,sizeof(original)/sizeof(original[0])));
#else
        (void)original;
#endif
        long ret=ip_call(1234);
#if defined(__riscv) || defined(__loongarch__)
        CHECK(ret==1234);
#else
        CHECK(ret==SYS_getpid);
#endif
        CHECK(!trapped);_exit(0);
    }
    int status;CHECK(waitpid(p,&status,0)==p);
    if(WIFEXITED(status)&&WEXITSTATUS(status)==77) {
        puts("SECCOMP TRAP ptrace SKIP host policy");return;
    }
    CHECK(WIFSTOPPED(status)&&WSTOPSIG(status)==SIGSTOP);
    CHECK(!ptrace(PTRACE_SETOPTIONS,p,0,(void *)PTRACE_O_TRACESYSGOOD));
    int entry_seen=0;
    for(unsigned i=0;i<32;i++) {
        CHECK(!ptrace(PTRACE_SYSCALL,p,0,0));
        CHECK(waitpid(p,&status,0)==p&&WIFSTOPPED(status)&&WSTOPSIG(status)==(SIGTRAP|0x80));
        uint64_t registers[64]={0};struct iovec regs={registers,sizeof(registers)};
        CHECK(!ptrace(PTRACE_GETREGSET,p,(void *)1,&regs));
#if defined(__riscv)
        if(registers[17]!=SYS_getpid)continue;
        CHECK(registers[0]==(uintptr_t)seccomp_call_after);
        registers[10]=9876; /* Linux syscall arguments/rollback use orig_a0. */
#elif defined(__loongarch__)
        if(registers[11]!=SYS_getpid)continue;
        CHECK(registers[33]==(uintptr_t)seccomp_call_after&&registers[32]==1234);
        registers[4]=9876; /* Keep the separately writable orig_a0 unchanged. */
#else
        if(registers[15]!=SYS_getpid)continue;
#endif
        CHECK(!ptrace(PTRACE_SETREGSET,p,(void *)1,&regs));entry_seen=1;break;
    }
    CHECK(entry_seen);
    CHECK(!ptrace(PTRACE_CONT,p,0,0));
    CHECK(waitpid(p,&status,0)==p&&WIFSTOPPED(status)&&WSTOPSIG(status)==SIGSYS);
    siginfo_t info;CHECK(!ptrace(PTRACE_GETSIGINFO,p,0,&info));
    CHECK(info.si_signo==SIGSYS&&info.si_code==1&&info.si_errno==0xabcd);
    CHECK(info.si_syscall==SYS_getpid&&info.si_call_addr==(void *)seccomp_call_after);
    CHECK(!ptrace(PTRACE_CONT,p,0,0));joined(p,0);
}

extern char seccomp_notify_after[];
static volatile sig_atomic_t restart_notified;
static void restart_handler(int sig,siginfo_t *info,void *context)
{
    (void)info;ucontext_t *uc=context;CHECK(sig==SIGUSR1);
#if defined(__riscv)
    CHECK(uc->uc_mcontext.__gregs[0]==(uintptr_t)seccomp_notify_after);
    CHECK(!uc->uc_mcontext.__gregs[10]);
#elif defined(__loongarch__)
    CHECK(uc->uc_mcontext.__pc==(uintptr_t)seccomp_notify_after);
    CHECK(!uc->uc_mcontext.__gregs[4]);
#else
    CHECK(uc->uc_mcontext.gregs[REG_RIP]==(greg_t)(uintptr_t)seccomp_notify_after);
    CHECK(!uc->uc_mcontext.gregs[REG_RAX]);
#endif
    restart_notified=1;
}
static __attribute__((noinline)) long notify_call(long pid)
{
#if defined(__riscv)
    register long a __asm__("a0")=pid,b __asm__("a1")=SIGUSR1,n __asm__("a7")=SYS_kill;
    __asm__ volatile("ecall\n.global seccomp_notify_after\nseccomp_notify_after:" : "+r"(a) : "r"(b),"r"(n) : "memory");
#elif defined(__loongarch__)
    register long a __asm__("$a0")=pid,b __asm__("$a1")=SIGUSR1,n __asm__("$a7")=SYS_kill;
    __asm__ volatile("syscall 0\n.global seccomp_notify_after\nseccomp_notify_after:" : "+r"(a) : "r"(b),"r"(n) : "memory");
#else
    long a;
    __asm__ volatile("syscall\n.global seccomp_notify_after\nseccomp_notify_after:" : "=a"(a) : "a"((long)SYS_kill),"D"(pid),"S"((long)SIGUSR1) : "memory","rcx","r11");
#endif
    return a;
}
static void wait_sleeping(pid_t p)
{
    char path[64],text[512];snprintf(path,sizeof(path),"/proc/%d/stat",p);
    for(unsigned i=0;i<100000;i++) {
        int fd=open(path,O_RDONLY);CHECK(fd>=0);
        ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0);close(fd);text[n]=0;
        char *end=strrchr(text,')');CHECK(end);
        if(end[2]=='S')return;
        CHECK(end[2]!='Z');sched_yield();
    }
    CHECK(0);
}

static volatile sig_atomic_t restart_trapped;
static void restart_trap_handler(int sig,siginfo_t *info,void *context)
{
    ucontext_t *uc=context;
    CHECK(sig==SIGSYS&&info->si_code==1&&info->si_errno==0x55a);
    CHECK(info->si_syscall==SYS_restart_syscall);
#if defined(__riscv)
    uc->uc_mcontext.__gregs[10]=(unsigned long)-512;
#elif defined(__loongarch__)
    uc->uc_mcontext.__gregs[4]=(unsigned long)-512;
#else
    uc->uc_mcontext.gregs[REG_RAX]=-512;
#endif
    restart_trapped=1;
}
static void restart_rejected(void)
{
    mkdir("/proc",0755);
    /* Do not mount over Linux's existing procfs, including unprivileged runs. */
    if(access("/proc/self/stat",R_OK))CHECK(!mount("proc","/proc","proc",0,0));
    for(unsigned mode=0;mode<4;mode++) {
        pid_t p=fork();CHECK(p>=0);
        if(!p) {
            pid_t me=getpid();struct sigaction sa={.sa_sigaction=restart_handler,.sa_flags=SA_SIGINFO};
            CHECK(!sigaction(SIGUSR1,&sa,0));nnp();
            if(mode>=2) {
                sa.sa_sigaction=restart_trap_handler;CHECK(!sigaction(SIGSYS,&sa,0));
            }
            deny(SYS_restart_syscall,mode>=2?(SECCOMP_RET_TRAP|0x55a):(SECCOMP_RET_ERRNO|512));
            struct timespec duration={60,0};int word=0;errno=0;
            long ret=(mode&1)?syscall(SYS_futex,&word,FUTEX_WAIT,0,&duration,0,0):
                syscall(SYS_nanosleep,&duration,0);
            CHECK(ret==-1&&errno==512&&restart_trapped==(mode>=2));
            /* A rejected restart must not corrupt a later signal's PC or a0. */
            CHECK(!notify_call(me)&&restart_notified);_exit(0);
        }
        wait_sleeping(p);CHECK(!kill(p,SIGSTOP));int status;
        CHECK(waitpid(p,&status,WUNTRACED)==p&&WIFSTOPPED(status));
        CHECK(!kill(p,SIGCONT));joined(p,0);
    }
}
int main(int argc,char **argv)
{
    if(argc>1&&!strcmp(argv[1],"exec-thread")){CHECK(prctl(PR_GET_SECCOMP)==2);CHECK(syscall(SYS_getpid)==-1&&errno==EAGAIN);return 0;}
    if(argc>1&&!strcmp(argv[1],"exec")){CHECK(prctl(PR_GET_SECCOMP)==2);CHECK(syscall(SYS_getpid)==-1&&errno==EACCES);return 0;}
    setvbuf(stdout,0,_IONBF,0);alarm(40);self=argv[0];initial_mode=prctl(PR_GET_SECCOMP);
    child_case(restart_rejected,0);puts("SECCOMP rejected restart state/PC PASS");
    child_case(trap_return,0);child_case(trap_thread,0);child_case(trap_ptrace,0);child_case(trap_blocked,SIGSYS);child_case(trap_ignored,SIGSYS);puts("SECCOMP TRAP metadata/return/forced PASS");
    child_case(errors,0);puts("SECCOMP errors PASS");
    child_case(permission,0);child_case(precedence,0);puts("SECCOMP inheritance/precedence PASS");
    child_case(instruction_pointer,0);child_case(ptrace_rewrite,0);puts("SECCOMP PC/ptrace checks complete");child_case(arguments,0);puts("SECCOMP arguments PASS");child_case(kill_thread,SIGSYS);puts("SECCOMP kill-thread PASS");child_case(kill_process,SIGSYS);puts("SECCOMP kill-process PASS");child_case(unknown,SIGSYS);puts("SECCOMP unknown PASS");
    child_case(threads,0);child_case(signals,0);child_case(thread_exec,0);child_case(threaded_group_kill,SIGSYS);puts("SECCOMP threads/actions PASS");
    if(!initial_mode){child_case(strict_ok,0);child_case(strict_return,0);child_case(strict_bad,SIGKILL);}
    else puts("SECCOMP STRICT SKIP inherited host filter");
    puts("SECCOMP ALL PASS");if(getpid()==1)reboot(RB_POWER_OFF);return 0;
}
