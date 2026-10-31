#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CLONE-RES FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
/* musl's public clone wrapper rejects CLONE_THREAD; use the native ABI. */
extern int test_thread_clone(int (*)(void *), void *, unsigned long, void *);
extern long test_clone3_stack(void *,size_t);
#if defined(__riscv)
__asm__(".text\n.global test_thread_clone\ntest_thread_clone:\n"
        "addi a1,a1,-16\nsd a0,0(a1)\nsd a3,8(a1)\nmv a0,a2\n"
        "li a2,0\nli a3,0\nli a4,0\nli a7,220\necall\n"
        "bnez a0,1f\nld t0,0(sp)\nld a0,8(sp)\njalr t0\nli a7,93\necall\n1: ret\n");
__asm__(".text\n.global test_clone3_stack\ntest_clone3_stack:\n"
        "li a7,435\necall\nbnez a0,1f\nld t0,0(sp)\nld a0,8(sp)\n"
        "jalr t0\nli a7,93\necall\n1: ret\n");
#elif defined(__loongarch__)
__asm__(".text\n.global test_thread_clone\ntest_thread_clone:\n"
        "addi.d $a1,$a1,-16\nst.d $a0,$a1,0\nst.d $a3,$a1,8\n"
        "or $a0,$a2,$zero\nor $a2,$zero,$zero\nor $a3,$zero,$zero\n"
        "or $a4,$zero,$zero\nori $a7,$zero,220\nsyscall 0\n"
        "bnez $a0,1f\nld.d $t8,$sp,0\nld.d $a0,$sp,8\n"
        "jirl $ra,$t8,0\nori $a7,$zero,93\nsyscall 0\n1: jirl $zero,$ra,0\n");
__asm__(".text\n.global test_clone3_stack\ntest_clone3_stack:\n"
        "ori $a7,$zero,435\nsyscall 0\nbnez $a0,1f\nld.d $t8,$sp,0\n"
        "ld.d $a0,$sp,8\njirl $ra,$t8,0\nori $a7,$zero,93\nsyscall 0\n1: jirl $zero,$ra,0\n");
#else
#error Unsupported clone ABI
#endif
struct thread_args { int watched, ready, release, cpu; const char *directory; };
static int thread_child(void *opaque)
{
    struct thread_args *a=opaque; char c='O';
    cpu_set_t wanted;CPU_ZERO(&wanted);CPU_SET(a->cpu,&wanted);
    if (sched_setaffinity(0,sizeof(wanted),&wanted) ||
        close(a->watched) || chdir(a->directory)) c='E';
    if (write(a->ready,&c,1)!=1 || read(a->release,&c,1)!=1) syscall(SYS_exit,2);
    syscall(SYS_exit,0); return 0;
}
static void wait_tid(pid_t tid)
{
    for (unsigned i=0;i<100000;i++) {
        if (syscall(SYS_tgkill,getpid(),tid,0)==-1 && errno==ESRCH) return;
        sched_yield();
    }
    CHECK(0);
}
static void thread_case(int shared_files,int shared_fs,int cpu)
{
    char before[512],after[512],path[128],link[512];
    CHECK(getcwd(before,sizeof(before)));
    int watched=open("/clone-resource-file",O_CREAT|O_RDWR,0600),ready[2],release[2];
    CHECK(watched>=0 && !pipe(ready) && !pipe(release));
    struct thread_args args={watched,ready[1],release[0],cpu,"/clone-resource-dir"};
    char *stack=malloc(131072); CHECK(stack);
    int flags=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|
        (shared_files?CLONE_FILES:0)|(shared_fs?CLONE_FS:0);
    pid_t tid=test_thread_clone(thread_child,stack+131072,(unsigned long)flags,&args); CHECK(tid>0);
    char c; CHECK(read(ready[0],&c,1)==1 && c=='O');
    errno=0; int result=fcntl(watched,F_GETFD);
    CHECK(shared_files ? result==-1 && errno==EBADF : result>=0);
    CHECK(getcwd(after,sizeof(after)));
    CHECK(shared_fs ? strcmp(before,after)!=0 : !strcmp(before,after));
    snprintf(path,sizeof(path),"/proc/%ld/task/%ld/cwd",(long)getpid(),(long)tid);
    ssize_t n=readlink(path,link,sizeof(link)-1); CHECK(n>0); link[n]=0;
    const char *suffix="/clone-resource-dir"; size_t a=strlen(link),b=strlen(suffix);
    CHECK(a>=b && !strcmp(link+a-b,suffix));
    snprintf(path,sizeof(path),"/proc/%ld/task/%ld/fd/%d",(long)getpid(),(long)tid,watched);
    errno=0; CHECK(readlink(path,link,sizeof(link))==-1 && errno==ENOENT);
    CHECK(write(release[1],"x",1)==1);wait_tid(tid);
    if(!shared_files)CHECK(!close(watched));
    CHECK(!close(ready[0])&&!close(ready[1])&&!close(release[0])&&!close(release[1]));
    CHECK(!chdir(before));free(stack);
}
struct exec_args { int report,cloexec,ordinary; };
static int exec_child(void *opaque)
{
    struct exec_args *a=opaque; char report[24],clo[24],normal[24];
    snprintf(report,sizeof(report),"%d",a->report);snprintf(clo,sizeof(clo),"%d",a->cloexec);
    snprintf(normal,sizeof(normal),"%d",a->ordinary);
    char *argv[]={"clone-resources","--exec-child",report,clo,normal,0};
    execv("/proc/self/exe",argv); (void)write(a->report,"E",1); _exit(99);
}
static volatile int shared_value;
static int vm_child(void *opaque)
{
    int fd=*(int *)opaque;shared_value=73;
    if(write(fd,"v",1)!=1) _exit(1);
    _exit(0);
}
static void first_handler(int signal_number) { (void)signal_number; }
static void second_handler(int signal_number) { (void)signal_number; }
struct sighand_args { int ready,release,report; };
static char read_one(int fd)
{
    struct pollfd p={.fd=fd,.events=POLLIN};char c;
    CHECK(poll(&p,1,5000)==1 && (p.revents&POLLIN));CHECK(read(fd,&c,1)==1);return c;
}
static int sighand_child(void *opaque)
{
    struct sighand_args *a=opaque; char c,fd[24];
    if(signal(SIGUSR1,second_handler)==SIG_ERR || write(a->ready,"s",1)!=1 ||
       read(a->release,&c,1)!=1) _exit(91);
    snprintf(fd,sizeof(fd),"%d",a->report);
    char *argv[]={"clone-resources","--signal-exec",fd,0};
    execv("/proc/self/exe",argv);_exit(92);
}
static int vfork_sighand_child(void *opaque)
{
    char fd[24];snprintf(fd,sizeof(fd),"%d",*(int *)opaque);
    char *argv[]={"clone-resources","--signal-exec",fd,0};
    execv("/proc/self/exe",argv);_exit(92);
}
struct owner_args { int release,report,watched; };
struct thread_exec_args { int report,cloexec,ordinary,removed; };
static int thread_exec_child(void *opaque)
{
    struct thread_exec_args *a=opaque;char report[24],clo[24],normal[24],removed[24];
    if(close(a->removed)||chdir("/clone-resource-dir"))syscall(SYS_exit,98);
    snprintf(report,sizeof(report),"%d",a->report);snprintf(clo,sizeof(clo),"%d",a->cloexec);
    snprintf(normal,sizeof(normal),"%d",a->ordinary);snprintf(removed,sizeof(removed),"%d",a->removed);
    char *argv[]={"clone-resources","--thread-exec",report,clo,normal,removed,0};
    execv("/proc/self/exe",argv);syscall(SYS_exit,99);return 0;
}
static void thread_exec_case(void)
{
    int report[2],block[2];CHECK(!pipe(report)&&!pipe(block));
    pid_t owner=fork();CHECK(owner>=0);
    if(!owner) {
        struct thread_exec_args a={.report=report[1]};
        a.cloexec=open("/clone-resource-file",O_RDWR|O_CLOEXEC);
        a.ordinary=open("/clone-resource-file",O_RDWR);
        a.removed=open("/clone-resource-file",O_RDWR);
        char *stack=malloc(131072);if(a.cloexec<0||a.ordinary<0||a.removed<0||!stack)_exit(94);
        if(test_thread_clone(thread_exec_child,stack+131072,CLONE_VM|CLONE_SIGHAND|CLONE_THREAD,&a)<0)_exit(95);
        char c;(void)read(block[0],&c,1);_exit(96);
    }
    CHECK(read_one(report[0])=='T');int st;
    CHECK(waitpid(owner,&st,0)==owner&&WIFEXITED(st)&&!WEXITSTATUS(st));
    CHECK(!close(report[0])&&!close(report[1])&&!close(block[0])&&!close(block[1]));
}
static int owner_clone_child(void *opaque)
{
    struct owner_args *a=opaque;char c;struct sigaction action;
    if(read(a->release,&c,1)!=1 || fcntl(a->watched,F_GETFD)<0 ||
       sigaction(SIGUSR1,0,&action) || action.sa_handler!=second_handler ||
       write(a->report,"I",1)!=1) _exit(93);
    _exit(0);
}
static void owner_exec_case(void)
{
    int report[2],release[2];CHECK(!pipe(report)&&!pipe(release));
    int watched=open("/clone-resource-file",O_RDWR|O_CLOEXEC);CHECK(watched>=0);
    pid_t owner=fork();CHECK(owner>=0);
    if(!owner) {
        struct owner_args a={release[0],report[1],watched};
        char *stack=malloc(131072);if(!stack || signal(SIGUSR1,second_handler)==SIG_ERR)_exit(94);
        pid_t child=clone(owner_clone_child,stack+131072,CLONE_VM|CLONE_FILES|CLONE_SIGHAND|SIGCHLD,&a);
        if(child<0 || syscall(SYS_kcmp,getpid(),child,4,0,0)!=0)_exit(95);
        char fd[24],pid[24],watch[24];snprintf(fd,sizeof(fd),"%d",report[1]);
        snprintf(pid,sizeof(pid),"%ld",(long)child);snprintf(watch,sizeof(watch),"%d",watched);
        char *argv[]={"clone-resources","--owner-exec",fd,pid,watch,0};
        execv("/proc/self/exe",argv);_exit(96);
    }
    CHECK(read_one(report[0])=='P');CHECK(write(release[1],"x",1)==1);
    CHECK(read_one(report[0])=='I');int st;
    CHECK(waitpid(owner,&st,0)==owner && WIFEXITED(st) && !WEXITSTATUS(st));
    CHECK(!close(watched)&&!close(report[0])&&!close(report[1])&&!close(release[0])&&!close(release[1]));
}
static void process_cases(void)
{
    char before[512],after[512];CHECK(getcwd(before,sizeof(before)));
    int watched=open("/clone-resource-file",O_RDWR),created[2],released[2];
    CHECK(watched>=0&&!pipe(created)&&!pipe(released));
    struct thread_args combined={watched,created[1],released[0],0,"/clone-resource-dir"};
    char *combined_stack=malloc(131072);CHECK(combined_stack);
    pid_t combined_pid=clone(thread_child,combined_stack+131072,CLONE_FILES|CLONE_FS|SIGCHLD,&combined);
    CHECK(combined_pid>0&&read_one(created[0])=='O');
    errno=0;CHECK(fcntl(watched,F_GETFD)==-1&&errno==EBADF);
    CHECK(getcwd(after,sizeof(after))&&strcmp(before,after));
    CHECK(write(released[1],"x",1)==1);int combined_status;
    CHECK(waitpid(combined_pid,&combined_status,0)==combined_pid&&WIFEXITED(combined_status)&&!WEXITSTATUS(combined_status));
    CHECK(!close(created[0])&&!close(created[1])&&!close(released[0])&&!close(released[1])&&!chdir(before));
    free(combined_stack);
    int ready[2];CHECK(!pipe(ready));char *stack=malloc(131072);CHECK(stack);
    shared_value=0;pid_t pid=clone(vm_child,stack+131072,CLONE_VM|SIGCHLD,&ready[1]);CHECK(pid>0);
    char c;CHECK(read(ready[0],&c,1)==1&&c=='v');int st;CHECK(waitpid(pid,&st,0)==pid&&WIFEXITED(st)&&!WEXITSTATUS(st));
    CHECK(shared_value==73);CHECK(!close(ready[0])&&!close(ready[1]));free(stack);
    int normal=open("/clone-resource-file",O_RDWR),clo=open("/clone-resource-file",O_RDWR|O_CLOEXEC),p[2];
    CHECK(normal>=0&&clo>=0&&!pipe(p));struct exec_args a={p[1],clo,normal};stack=malloc(131072);CHECK(stack);
    pid=clone(exec_child,stack+131072,CLONE_FILES|SIGCHLD,&a);CHECK(pid>0);
    CHECK(read_one(p[0])=='O');CHECK(waitpid(pid,&st,0)==pid&&WIFEXITED(st)&&!WEXITSTATUS(st));
    CHECK(fcntl(clo,F_GETFD)>=0&&fcntl(normal,F_GETFD)>=0);
    CHECK(!close(normal)&&!close(clo)&&!close(p[0])&&!close(p[1]));free(stack);

    int signal_ready[2],signal_release[2],signal_report[2];
    CHECK(!pipe(signal_ready)&&!pipe(signal_release)&&!pipe(signal_report));
    CHECK(signal(SIGUSR1,first_handler)!=SIG_ERR);
    struct sighand_args sa={signal_ready[1],signal_release[0],signal_report[1]};
    stack=malloc(131072);CHECK(stack);
    pid=clone(sighand_child,stack+131072,CLONE_VM|CLONE_SIGHAND|SIGCHLD,&sa);CHECK(pid>0);
    CHECK(read(signal_ready[0],&c,1)==1&&c=='s');struct sigaction action;
    CHECK(!sigaction(SIGUSR1,0,&action)&&action.sa_handler==second_handler);
    CHECK(write(signal_release[1],"x",1)==1&&read(signal_report[0],&c,1)==1&&c=='s');
    CHECK(waitpid(pid,&st,0)==pid&&WIFEXITED(st)&&!WEXITSTATUS(st));
    CHECK(!sigaction(SIGUSR1,0,&action)&&action.sa_handler==second_handler);
    CHECK(signal(SIGUSR1,SIG_DFL)!=SIG_ERR);
    CHECK(!close(signal_ready[0])&&!close(signal_ready[1])&&!close(signal_release[0])&&
          !close(signal_release[1])&&!close(signal_report[0])&&!close(signal_report[1]));free(stack);
    CHECK(!pipe(signal_report));CHECK(signal(SIGUSR1,second_handler)!=SIG_ERR);
    stack=malloc(131072);CHECK(stack);
    pid=clone(vfork_sighand_child,stack+131072,CLONE_VM|CLONE_VFORK|CLONE_SIGHAND|SIGCHLD,&signal_report[1]);
    CHECK(pid>0 && read_one(signal_report[0])=='s');
    CHECK(waitpid(pid,&st,0)==pid&&WIFEXITED(st)&&!WEXITSTATUS(st));
    CHECK(!sigaction(SIGUSR1,0,&action)&&action.sa_handler==second_handler);
    CHECK(signal(SIGUSR1,SIG_DFL)!=SIG_ERR && !close(signal_report[0])&&!close(signal_report[1]));free(stack);
    owner_exec_case();
    thread_exec_case();
}
struct test_clone_args {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
    uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};
static volatile int shared_pidfd_result;
static int clone_tls_child(void *opaque)
{
    uintptr_t tls;
#if defined(__riscv)
    __asm__ volatile("mv %0,tp" : "=r"(tls));
#else
    __asm__ volatile("or %0,$tp,$zero" : "=r"(tls));
#endif
    return tls==(uintptr_t)opaque ? 48 : 97;
}
static int child_tid_child(void *opaque)
{
    return *(volatile int *)opaque==(int)syscall(SYS_gettid) ? 49 : 98;
}
static int shared_pidfd_child(void *opaque)
{
    int fd=*(volatile int *)opaque;
    int result=fd>=0 && (fcntl(fd,F_GETFD)&FD_CLOEXEC) ? 45 : 96;
    shared_pidfd_result=result;
    return result;
}
static void clone3_case(void)
{
    struct test_clone_args args={.exit_signal=SIGCHLD};
    errno=0; CHECK(syscall(435,&args,63)==-1 && errno==EINVAL);
    errno=0; CHECK(syscall(435,(void *)1,sizeof(args))==-1 && errno==EFAULT);
    args.stack_size=1;errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EINVAL);
    args.stack_size=0;
    struct { struct test_clone_args args; uint64_t extra; } extended={.args=args,.extra=1};
    errno=0;CHECK(syscall(435,&extended,sizeof(extended))==-1 && errno==E2BIG);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char *mapping=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED);mapping[page]=1;CHECK(!mprotect(mapping,page,PROT_NONE));
    errno=0;CHECK(syscall(435,mapping+page-sizeof(args),sizeof(args)+1)==-1 && errno==E2BIG);
    CHECK(!munmap(mapping,page*2));
    args.flags=0x1000|0x100000;args.pidfd=args.parent_tid=(uintptr_t)&extended;
    errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EINVAL);
    args.flags=args.pidfd=args.parent_tid=0;
    args.flags=0x400000;errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EINVAL);
    args.flags=0x8000;errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EINVAL);
    args.flags=0;
    args.set_tid_size=1;errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EINVAL);
    args.set_tid=1;errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EFAULT);
    args.set_tid=args.set_tid_size=0;args.flags=UINT64_C(0x200000000);args.cgroup=UINT64_MAX;
    errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EINVAL);
    args.flags=0;
    pid_t child=(pid_t)syscall(435,&args,sizeof(args));
    CHECK(child>=0);
    if(!child) _exit(37);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==37);
    args.cgroup=0;
    args.exit_signal=0;
    child=(pid_t)syscall(435,&args,sizeof(args));CHECK(child>=0);
    if(!child)_exit(38);
    errno=0;CHECK(waitpid(child,&status,WNOHANG)==-1 && errno==ECHILD);
    CHECK(waitpid(child,&status,(int)0x80000000U)==child && WIFEXITED(status) && WEXITSTATUS(status)==38);
    int ready[2];CHECK(!pipe(ready));
    child=(pid_t)syscall(435,&args,sizeof(args));CHECK(child>=0);
    if(!child) {
        char fd[24];snprintf(fd,sizeof(fd),"%d",ready[1]);
        char *argv[]={"clone-resources","--exec-signal",fd,0};execv("/proc/self/exe",argv);_exit(97);
    }
    CHECK(read_one(ready[0])=='X');
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==47);
    CHECK(!close(ready[0])&&!close(ready[1]));
    sigset_t set,old;sigemptyset(&set);sigaddset(&set,SIGUSR1);
    CHECK(!sigprocmask(SIG_BLOCK,&set,&old));args.exit_signal=SIGUSR1;
    child=(pid_t)syscall(435,&args,sizeof(args));CHECK(child>=0);
    if(!child)_exit(39);
    siginfo_t info;CHECK(sigwaitinfo(&set,&info)==SIGUSR1 && info.si_pid==child &&
        info.si_code==CLD_EXITED && info.si_status==39);
    CHECK(waitpid(child,&status,(int)0x80000000U)==child && WIFEXITED(status) && WEXITSTATUS(status)==39);
    CHECK(!sigprocmask(SIG_SETMASK,&old,0));
}
static void clone_pidfd_case(void)
{
    struct test_clone_args args={.flags=0x1000,.exit_signal=SIGCHLD,.pidfd=1};
    errno=0;CHECK(syscall(435,&args,sizeof(args))==-1 && errno==EFAULT);
    errno=0;CHECK(waitpid(-1,0,WNOHANG)==-1 && errno==ECHILD);
    pid_t probe=fork();CHECK(probe>=0);
    if(!probe) {
        struct rlimit limit={.rlim_cur=0,.rlim_max=0};
        if(setrlimit(RLIMIT_NOFILE,&limit))_exit(90);
        int value=-1;struct test_clone_args a={.flags=0x1000,.exit_signal=SIGCHLD,.pidfd=(uintptr_t)&value};
        errno=0;_exit(syscall(435,&a,sizeof(a))==-1 && errno==EMFILE ? 0 : 91);
    }
    int probe_status;CHECK(waitpid(probe,&probe_status,0)==probe && WIFEXITED(probe_status) && !WEXITSTATUS(probe_status));
    int fd=-1;args.pidfd=(uintptr_t)&fd;
    pid_t child=(pid_t)syscall(435,&args,sizeof(args));CHECK(child>=0);
    if(!child)_exit(43);
    CHECK(fd>=0 && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
    struct pollfd p={.fd=fd,.events=POLLIN};CHECK(poll(&p,1,5000)==1 && (p.revents&POLLIN));
    siginfo_t info={0};CHECK(!waitid((idtype_t)3,(id_t)fd,&info,WEXITED));
    CHECK(info.si_pid==child && info.si_status==43 && !close(fd));
    fd=-1;child=(pid_t)syscall(SYS_clone,0x1000|SIGCHLD,0,&fd,0,0);
    CHECK(child>=0);if(!child)_exit(44);
    int status;CHECK(fd>=0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==44);
    CHECK(!close(fd));
    errno=0;CHECK(syscall(SYS_clone,0x1000|0x100000|SIGCHLD,0,&fd,0,0)==-1 && errno==EINVAL);
    errno=0;CHECK(syscall(SYS_clone,0x1000|0x400000|SIGCHLD,0,&fd,0,0)==-1 && errno==EINVAL);
    int tid_out=-1;fd=-1;
    args=(struct test_clone_args){.flags=0x1000|0x100000,.pidfd=(uintptr_t)&fd,
        .parent_tid=(uintptr_t)&tid_out,.exit_signal=SIGCHLD};
    child=(pid_t)syscall(435,&args,sizeof(args));CHECK(child>=0);if(!child)_exit(46);
    CHECK(tid_out==child && fd>=0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==46);
    CHECK(!close(fd));
    char *stack=malloc(131072);CHECK(stack);
    for(int i=0;i<32;i++) {
        fd=-1;
        uintptr_t *top=(uintptr_t *)(stack+131072-16);
        top[0]=(uintptr_t)shared_pidfd_child;top[1]=(uintptr_t)&fd;
        args=(struct test_clone_args){.flags=0x1000|CLONE_VM|CLONE_FILES,.pidfd=(uintptr_t)&fd,
            .exit_signal=SIGCHLD,.stack=(uintptr_t)stack,.stack_size=131072-16};
        child=(pid_t)test_clone3_stack(&args,sizeof(args));CHECK(child>0);
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==45);
        CHECK(!close(fd));
    }
    fd=-1;shared_pidfd_result=0;
    uintptr_t *top=(uintptr_t *)(stack+131072-16);
    top[0]=(uintptr_t)shared_pidfd_child;top[1]=(uintptr_t)&fd;
    args=(struct test_clone_args){.flags=0x1000|CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD,
        .pidfd=(uintptr_t)&fd,.stack=(uintptr_t)stack,.stack_size=131072-16};
    pid_t tid=(pid_t)test_clone3_stack(&args,sizeof(args));CHECK(tid>0);
    p=(struct pollfd){.fd=fd,.events=POLLIN};CHECK(poll(&p,1,5000)==1 && (p.revents&POLLIN));
    CHECK(shared_pidfd_result==45);wait_tid(tid);CHECK(!close(fd));
    top[0]=(uintptr_t)clone_tls_child;top[1]=(uintptr_t)stack;
    args=(struct test_clone_args){.flags=0x80000,.exit_signal=SIGCHLD,
        .stack=(uintptr_t)stack,.stack_size=131072-16,.tls=(uintptr_t)stack};
    child=(pid_t)test_clone3_stack(&args,sizeof(args));CHECK(child>0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==48);
    int child_tid=-1;top[0]=(uintptr_t)child_tid_child;top[1]=(uintptr_t)&child_tid;
    args=(struct test_clone_args){.flags=CLONE_VM|0x1000000|0x200000,.exit_signal=SIGCHLD,
        .stack=(uintptr_t)stack,.stack_size=131072-16,.child_tid=(uintptr_t)&child_tid};
    child=(pid_t)test_clone3_stack(&args,sizeof(args));CHECK(child>0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==49 && child_tid==0);
    free(stack);
}
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"--exec-signal"))
        return write(atoi(argv[2]),"X",1)==1?47:1;
    if(argc==6 && !strcmp(argv[1],"--thread-exec")) {
        int report=atoi(argv[2]),clo=atoi(argv[3]),normal=atoi(argv[4]),removed=atoi(argv[5]);
        char cwd[512];errno=0;int a=fcntl(clo,F_GETFD),aerr=errno;
        errno=0;int b=fcntl(removed,F_GETFD),berr=errno;
        if(a!=-1 || aerr!=EBADF || b!=-1 || berr!=EBADF || fcntl(normal,F_GETFD)<0 ||
           !getcwd(cwd,sizeof(cwd)) || !strstr(cwd,"/clone-resource-dir") || write(report,"T",1)!=1) return 1;
        return 0;
    }
    if(argc==5 && !strcmp(argv[1],"--owner-exec")) {
        int report=atoi(argv[2]),watched=atoi(argv[4]),st;pid_t child=(pid_t)atoi(argv[3]);
        errno=0;if(fcntl(watched,F_GETFD)!=-1 || errno!=EBADF ||
            syscall(SYS_kcmp,getpid(),child,4,0,0)<=0 || write(report,"P",1)!=1 ||
            waitpid(child,&st,0)!=child || !WIFEXITED(st) || WEXITSTATUS(st)) return 1;
        return 0;
    }
    if(argc==3 && !strcmp(argv[1],"--signal-exec"))
        return write(atoi(argv[2]),"s",1)==1?0:1;
    if(argc==5 && !strcmp(argv[1],"--exec-child")) {
        int report=atoi(argv[2]),clo=atoi(argv[3]),normal=atoi(argv[4]);errno=0;
        char c=(fcntl(clo,F_GETFD)==-1&&errno==EBADF&&fcntl(normal,F_GETFD)>=0)?'O':'E';
        return write(report,&c,1)==1 && c=='O'?0:1;
    }
    if(access("/proc/self",F_OK)) { CHECK(!mkdir("/proc",0555)||errno==EEXIST); CHECK(!mount("proc","/proc","proc",0,0)); }
    CHECK(!mkdir("/clone-resource-dir",0700)||errno==EEXIST);
    cpu_set_t original,wanted;CHECK(!sched_getaffinity(0,sizeof(original),&original));
    int first=-1,second=-1;for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&original)) {
        if(first<0)first=i;else if(second<0)second=i;
    }
    CHECK(first>=0);if(second<0)second=first;
    CPU_ZERO(&wanted);CPU_SET(first,&wanted);CHECK(!sched_setaffinity(0,sizeof(wanted),&wanted));
    for(int files=0;files<2;files++)for(int fs=0;fs<2;fs++)thread_case(files,fs,second);
    CHECK(!sched_setaffinity(0,sizeof(original),&original));
    puts("CLONE-RES CASE thread-files-fs PASS");
    process_cases();puts("CLONE-RES CASE process-vm-files-exec PASS");
    clone3_case();puts("CLONE-RES CASE clone3 PASS");
    clone_pidfd_case();puts("CLONE-RES CASE pidfd PASS");
    CHECK(!unlink("/clone-resource-file")&&!rmdir("/clone-resource-dir"));
    puts("CLONE-RES END 4");return 0;
}
