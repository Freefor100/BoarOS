#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#define CHECK(test) do { if (!(test)) { dprintf(2,"PTRACE FAIL line=%d errno=%d %s\n",__LINE__,errno,#test); _exit(1); } } while(0)
static volatile long word = 73;
#if defined(__riscv)
__asm__(".pushsection .text\n.option push\n.option norvc\n.balign 8\n"
        ".global ptrace_old,ptrace_new\nptrace_old: li a0,1\nret\nptrace_new: li a0,2\nret\n"
        ".option pop\n.popsection");
#elif defined(__loongarch__)
__asm__(".pushsection .text\n.balign 8\n.global ptrace_old,ptrace_new\n"
        "ptrace_old: li.w $a0,1\njr $ra\nptrace_new: li.w $a0,2\njr $ra\n.popsection");
#endif
#if defined(__riscv) || defined(__loongarch__)
extern int ptrace_old(void),ptrace_new(void);

static void syscall_original_argument(void)
{
    int fd = open("/tmp/ptrace-original-arg", O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && !unlink("/tmp/ptrace-original-arg"));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!ptrace(PTRACE_TRACEME, 0, 0, 0));
        CHECK(!syscall(SYS_kill, getpid(), SIGSTOP));
        _exit(syscall(SYS_write, fd, "x", 1) == 1 ? 0 : 10);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
    CHECK(!ptrace(PTRACE_SETOPTIONS, child, 0, (void *)PTRACE_O_TRACESYSGOOD));
    CHECK(!ptrace(PTRACE_SYSCALL, child, 0, 0));
    CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status) &&
          WSTOPSIG(status) == (SIGTRAP | 128));
    uint64_t info[10] = {0};
    CHECK(syscall(SYS_ptrace, 0x420e, child, sizeof(info), info) == 80 &&
          (uint8_t)info[0] == 1 && info[3] == SYS_write && info[4] == (uint64_t)fd);
    uint64_t registers[64];
    struct iovec regs = {registers, sizeof(registers)};
    CHECK(!ptrace(PTRACE_GETREGSET, child, (void *)1, &regs));
#if defined(__riscv)
    registers[10] = (uint64_t)-1;
#else
    registers[4] = (uint64_t)-1;
#endif
    CHECK(!ptrace(PTRACE_SETREGSET, child, (void *)1, &regs));
    CHECK(!ptrace(PTRACE_CONT, child, 0, 0));
    char saved;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(pread(fd, &saved, 1, 0) == 1 && saved == 'x' && !close(fd));
}

static void wait_for_sleep(pid_t child)
{
    char path[64], stat[512];
    CHECK(snprintf(path, sizeof(path), "/proc/%d/stat", child) > 0);
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t bytes = read(fd, stat, sizeof(stat) - 1);
        CHECK(bytes > 0 && !close(fd));
        stat[bytes] = 0;
        char *state = strrchr(stat, ')');
        CHECK(state && state[1] == ' ' && state[2] != 'Z');
        if (state[2] == 'S') return;
        sched_yield();
    }
    CHECK(0);
}

static void replaced_restart(void)
{
    pid_t parent = getpid(), child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!ptrace(PTRACE_TRACEME, 0, 0, 0));
        CHECK(!syscall(SYS_kill, getpid(), SIGSTOP));
        struct timespec duration = {.tv_sec = 3};
        CHECK(syscall(SYS_nanosleep, &duration, NULL) == parent);
        CHECK(syscall(SYS_restart_syscall) == -1 && (errno == ENOSYS || errno == EINTR));
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
    CHECK(!ptrace(PTRACE_SETOPTIONS, child, 0, (void *)PTRACE_O_TRACESYSGOOD));
    CHECK(!ptrace(PTRACE_CONT, child, 0, 0));
    wait_for_sleep(child);
    CHECK(!kill(child, SIGSTOP));
    CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    CHECK(!ptrace(PTRACE_CONT, child, 0, (void *)SIGSTOP));
    CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    CHECK(!kill(child, SIGCONT) && !ptrace(PTRACE_SYSCALL, child, 0, 0));
    for (;;) {
        CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
        if (WSTOPSIG(status) == (SIGTRAP | 128)) {
            uint64_t info[10] = {0};
            CHECK(syscall(SYS_ptrace, 0x420e, child, sizeof(info), info) > 0);
            if ((uint8_t)info[0] == 1 && info[3] == SYS_restart_syscall) break;
            CHECK(!ptrace(PTRACE_SYSCALL, child, 0, 0));
        } else {
            CHECK(WSTOPSIG(status) == SIGCONT);
            CHECK(!ptrace(PTRACE_SYSCALL, child, 0, (void *)SIGCONT));
        }
    }
    uint64_t registers[64];
    struct iovec regs = {registers, sizeof(registers)};
    CHECK(!ptrace(PTRACE_GETREGSET, child, (void *)1, &regs));
#if defined(__riscv)
    registers[17] = SYS_getppid;
#else
    registers[11] = SYS_getppid;
#endif
    CHECK(!ptrace(PTRACE_SETREGSET, child, (void *)1, &regs));
    CHECK(!ptrace(PTRACE_CONT, child, 0, 0));
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}
#endif
static void proc_tracer(pid_t tracee)
{
    char path[64],text[4096];snprintf(path,sizeof(path),"/proc/%d/status",tracee);
    int fd=open(path,O_RDONLY);CHECK(fd>=0);
    ssize_t n=read(fd,text,sizeof(text)-1);CHECK(n>0);text[n]=0;CHECK(!close(fd));
    const char *line=strstr(text,"\nTracerPid:\t");CHECK(line);
    CHECK(strtol(line+12,NULL,10)==syscall(SYS_gettid));
}
static void kill_before_write(void)
{
    int fd=open("/tmp/ptrace-stop-write",O_RDWR|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0 && !unlink("/tmp/ptrace-stop-write"));
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        CHECK(!ptrace(PTRACE_TRACEME,0,0,0));
        CHECK(!syscall(SYS_kill,getpid(),SIGSTOP));
        (void)syscall(SYS_write,fd,"x",1);
        _exit(33);
    }
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status));
    proc_tracer(child);
    CHECK(!ptrace(PTRACE_SETOPTIONS,child,0,(void *)PTRACE_O_TRACESYSGOOD));
    CHECK(!ptrace(PTRACE_SYSCALL,child,0,0));
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) && WSTOPSIG(status)==(SIGTRAP|128));
    uint64_t info[10]={0};
    CHECK(syscall(SYS_ptrace,0x420e,child,sizeof(info),info)==80 && (uint8_t)info[0]==1 &&
        info[3]==SYS_write && info[4]==(uint64_t)fd);
    CHECK(!ptrace(PTRACE_KILL,child,0,0));
    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    struct stat st;
    CHECK(!fstat(fd,&st) && st.st_size==0);
    CHECK(!close(fd));
}

static void continue_before_listen(void)
{
    pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        CHECK(!syscall(SYS_kill,getpid(),SIGSTOP));
        CHECK(!syscall(SYS_kill,getpid(),SIGSTOP));
        _exit(32);
    }
    int status;
    CHECK(waitpid(child,&status,WUNTRACED)==child && WIFSTOPPED(status));
    CHECK(!ptrace(PTRACE_SEIZE,child,0,0));
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) &&
        ((unsigned)status>>16)==PTRACE_EVENT_STOP && WSTOPSIG(status)==SIGSTOP);
    CHECK(!kill(child,SIGCONT));
    CHECK(!ptrace(PTRACE_LISTEN,child,0,0));
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) &&
        ((unsigned)status>>16)==PTRACE_EVENT_STOP && WSTOPSIG(status)==SIGTRAP);
    CHECK(!ptrace(PTRACE_CONT,child,0,0));
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) && WSTOPSIG(status)==SIGCONT);
    CHECK(!ptrace(PTRACE_CONT,child,0,0));
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP &&
        ((unsigned)status>>16)==0);
    CHECK(!ptrace(PTRACE_CONT,child,0,(void *)(uintptr_t)SIGSTOP));
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP &&
        ((unsigned)status>>16)==PTRACE_EVENT_STOP);
    CHECK(waitpid(child,&status,WUNTRACED|WNOHANG)==0);
    CHECK(!ptrace(PTRACE_DETACH,child,0,0));
    CHECK(!kill(child,SIGCONT));
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==32);
}

static void final_wait_ownership(void)
{
    int ids[2],release[2];
    CHECK(!pipe(ids) && !pipe(release));
    pid_t parent=fork(); CHECK(parent>=0);
    if (!parent) {
        pid_t child=fork(); CHECK(child>=0);
        if (!child) {
            char token;
            CHECK(read(release[0],&token,1)==1);
            _exit(41);
        }
        CHECK(write(ids[1],&child,sizeof(child))==sizeof(child));
        int status;
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==41);
        _exit(0);
    }
    pid_t child;
    CHECK(read(ids[0],&child,sizeof(child))==sizeof(child));
    CHECK(!ptrace(PTRACE_SEIZE,child,0,0) && !ptrace(PTRACE_INTERRUPT,child,0,0));
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) && ((unsigned)status>>16)==128);
    CHECK(!ptrace(PTRACE_LISTEN,child,0,0) && !ptrace(PTRACE_INTERRUPT,child,0,0));
    siginfo_t info;
    CHECK(!waitid(P_PID,child,&info,WSTOPPED|WNOWAIT) && info.si_pid==child && info.si_code==CLD_TRAPPED);
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status));
    CHECK(!ptrace(PTRACE_CONT,child,0,0));
    CHECK(write(release[1],"x",1)==1);
    CHECK(!waitid(P_PID,child,&info,WEXITED|WNOWAIT) && info.si_pid==child && info.si_status==41);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==41);
    CHECK(waitpid(parent,&status,0)==parent && WIFEXITED(status) && !WEXITSTATUS(status));
    close(ids[0]); close(ids[1]); close(release[0]); close(release[1]);
}
static void tracer_death(unsigned options)
{
    int fd=open("/tmp/ptrace-tracer-death",O_RDWR|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0 && !unlink("/tmp/ptrace-tracer-death"));
    int identity[2],stopped[2],release[2];
    CHECK(!pipe(identity) && !pipe(stopped) && !pipe(release));
    pid_t tracer=fork();
    CHECK(tracer>=0);
    if (!tracer) {
        pid_t target;
        close(identity[1]); close(stopped[0]); close(release[0]);
        CHECK(read(identity[0],&target,sizeof(target))==sizeof(target));
        CHECK(!ptrace(PTRACE_SEIZE,target,0,(void *)(uintptr_t)(options|PTRACE_O_TRACESYSGOOD)));
        CHECK(write(release[1],"r",1)==1);
        int status;
        CHECK(waitpid(target,&status,0)==target && WIFSTOPPED(status) && WSTOPSIG(status)==SIGUSR1);
        CHECK(!ptrace(PTRACE_SYSCALL,target,0,0));
        CHECK(waitpid(target,&status,0)==target && WIFSTOPPED(status) && WSTOPSIG(status)==(SIGTRAP|128));
        uint64_t info[10]={0};
        CHECK(syscall(SYS_ptrace,0x420e,target,sizeof(info),info)==80 &&
            (uint8_t)info[0]==1 && info[3]==SYS_write && info[4]==(uint64_t)fd);
        CHECK(write(stopped[1],"s",1)==1);
        for (;;) pause();
    }
    close(identity[0]); close(stopped[1]); close(release[1]);
    pid_t target=fork();
    CHECK(target>=0);
    if (!target) {
        /* Yama can require a sibling tracer to be named explicitly. Kernels
         * without that LSM reject this prctl; SEIZE must succeed in either case. */
        CHECK(!prctl(PR_SET_PTRACER,(unsigned long)tracer,0UL,0UL,0UL) || errno==EINVAL);
        pid_t me=getpid();
        CHECK(write(identity[1],&me,sizeof(me))==sizeof(me));
        char byte;
        CHECK(read(release[0],&byte,1)==1 && byte=='r');
        CHECK(!syscall(SYS_kill,me,SIGUSR1));
        CHECK(syscall(SYS_write,fd,"x",1)==1);
        _exit(43);
    }
    close(identity[1]); close(release[0]);
    char byte;
    CHECK(read(stopped[0],&byte,1)==1 && byte=='s');
    CHECK(!kill(tracer,SIGKILL));
    int status;
    CHECK(waitpid(tracer,&status,0)==tracer && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    CHECK(waitpid(target,&status,0)==target);
    if (options) CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    else CHECK(WIFEXITED(status) && WEXITSTATUS(status)==43);
    struct stat st;
    CHECK(!fstat(fd,&st) && st.st_size==(options?0:1));
    close(stopped[0]); close(fd);
}
static const char *self_path;
static void *event_exec_thread(void *unused)
{
    (void)unused;
    execl(self_path,self_path,"--event-exec-leaf",NULL);
    _exit(95);
}
static int event_vfork_exec(void *unused)
{
    (void)unused;
    execl(self_path,self_path,"--event-vfork-leaf",NULL);
    _exit(96);
}
static void event_identity(void)
{
    pid_t parent=fork();CHECK(parent>=0);
    if(!parent) {
        CHECK(!ptrace(PTRACE_TRACEME,0,0,0));
        CHECK(!raise(SIGSTOP));
        pid_t child=fork();CHECK(child>=0);
        if(!child)_exit(7);
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==7);
        void *stack=malloc(65536);CHECK(stack);
        child=clone(event_vfork_exec,(char *)stack+65536,CLONE_VM|CLONE_VFORK|SIGCHLD,NULL);CHECK(child>=0);
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==9);free(stack);
        pthread_t thread;CHECK(!pthread_create(&thread,NULL,event_exec_thread,NULL));
        for(;;)pause();
    }
    int status;CHECK(waitpid(parent,&status,0)==parent && WIFSTOPPED(status));
    CHECK(!ptrace(PTRACE_SETOPTIONS,parent,0,(void *)(PTRACE_O_TRACEFORK|PTRACE_O_TRACECLONE|PTRACE_O_TRACEEXEC|PTRACE_O_TRACEVFORK|PTRACE_O_TRACEVFORKDONE)));
    CHECK(!ptrace(PTRACE_CONT,parent,0,0));
    pid_t fork_id=0,thread_id=0,vfork_id=0;unsigned exec_seen=0,vfork_done=0,vfork_exec=0,exits=0,steps=0;
    for(;;) {
        pid_t pid=waitpid(-1,&status,__WALL);
        if(pid<0){CHECK(errno==ECHILD);break;}
        CHECK(++steps<64);
        if(WIFEXITED(status)) {
            CHECK((pid==fork_id && WEXITSTATUS(status)==7) || (pid==parent && WEXITSTATUS(status)==31) || (pid==vfork_id && WEXITSTATUS(status)==9));
            exits++;continue;
        }
        CHECK(WIFSTOPPED(status));
        unsigned event=(unsigned)status>>16;
        if(event) {
            unsigned long message=0;CHECK(!ptrace(PTRACE_GETEVENTMSG,pid,0,&message));
            if(event==PTRACE_EVENT_FORK){CHECK(pid==parent && !fork_id && message>0);fork_id=(pid_t)message;}
            else if(event==PTRACE_EVENT_CLONE){CHECK(pid==parent && !thread_id && message>0);thread_id=(pid_t)message;}
            else if(event==PTRACE_EVENT_VFORK){CHECK(pid==parent && !vfork_id && message>0);vfork_id=(pid_t)message;}
            else if(event==PTRACE_EVENT_VFORK_DONE){CHECK(pid==parent && message==(unsigned long)vfork_id);vfork_done++;}
            else if(event==PTRACE_EVENT_EXEC && pid==vfork_id){CHECK(message==(unsigned long)vfork_id);vfork_exec++;}
            else {CHECK(event==PTRACE_EVENT_EXEC && pid==parent && thread_id>0 && message==(unsigned long)thread_id);exec_seen++;}
        }
        CHECK(!ptrace(PTRACE_CONT,pid,0,0));
    }
    CHECK(fork_id>0 && thread_id>0 && vfork_id>0 && exec_seen==1 && vfork_done==1 && vfork_exec==1 && exits==3);
    puts("PTRACE events fork-clone-vfork-nonleader-exec PASS");
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--event-exec-leaf"))return 31;
    if(argc==2 && !strcmp(argv[1],"--event-vfork-leaf"))return 9;
    self_path=argv[0];
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        CHECK(!ptrace(PTRACE_TRACEME,0,0,0));
#if defined(__riscv) || defined(__loongarch__)
        CHECK(ptrace_old()==1);
#endif
        CHECK(!syscall(SYS_kill,getpid(),SIGSTOP));
        CHECK(word == 86);
#if defined(__riscv) || defined(__loongarch__)
        CHECK(ptrace_old()==2);
        CHECK(syscall(SYS_getpid)==321);
#else
        CHECK(syscall(SYS_getpid)>0);
#endif
        _exit(23);
    }
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP);
    errno=0;
    CHECK(ptrace(PTRACE_SETOPTIONS,pid,0,(void *)(1UL<<30))==-1 && errno==EINVAL);
    uint64_t registers[64];
    struct iovec regs={registers,sizeof(registers)};
    CHECK(!ptrace(PTRACE_GETREGSET,pid,(void *)1,&regs) && regs.iov_len<=sizeof(registers));
    CHECK(!ptrace(PTRACE_SETREGSET,pid,(void *)1,&regs));
#if defined(__riscv)
    CHECK(regs.iov_len==32*8);
    errno=0; CHECK(ptrace(PTRACE_PEEKUSER,pid,(void *)0,0)==-1 && errno==EIO);
#elif defined(__loongarch__)
    CHECK(regs.iov_len==45*8);
    CHECK((uint64_t)ptrace(PTRACE_PEEKUSER,pid,(void *)33,0)==registers[33]);
#endif
    regs.iov_len=7; errno=0;
    CHECK(ptrace(PTRACE_GETREGSET,pid,(void *)1,&regs)==-1 && errno==EINVAL);
    regs.iov_len=sizeof(registers);
    CHECK(!ptrace(PTRACE_GETREGSET,pid,(void *)2,&regs));
    CHECK(!ptrace(PTRACE_SETREGSET,pid,(void *)2,&regs));
    long value=0;
    CHECK(!syscall(SYS_ptrace,PTRACE_PEEKDATA,pid,&word,&value) && value==73);
    CHECK(ptrace(PTRACE_PEEKDATA,pid,&word,0)==73);
    CHECK(!ptrace(PTRACE_POKEDATA,pid,&word,(void *)86L));
#if defined(__riscv) || defined(__loongarch__)
    uint64_t code;
    memcpy(&code,(const void *)ptrace_new,sizeof(code));
    CHECK(!ptrace(PTRACE_POKETEXT,pid,ptrace_old,(void *)code));
    CHECK(ptrace_old()==1);
#endif
    CHECK(!ptrace(PTRACE_SETOPTIONS,pid,0,(void *)(PTRACE_O_TRACESYSGOOD|PTRACE_O_TRACEEXIT)));
    CHECK(!ptrace(PTRACE_SYSCALL,pid,0,0));
    CHECK(waitpid(pid,&status,0)==pid && WIFSTOPPED(status) && WSTOPSIG(status)==(SIGTRAP|128));
    _Alignas(8) uint8_t info[128]={0};
    CHECK(syscall(SYS_ptrace,0x420e,pid,sizeof(info),info)>=24 && info[0]==1);
    CHECK(*(uint64_t *)(void *)(info+24)==SYS_getpid);
#if defined(__riscv) || defined(__loongarch__)
    regs.iov_len=sizeof(registers);
    CHECK(!ptrace(PTRACE_GETREGSET,pid,(void *)1,&regs));
#if defined(__riscv)
    registers[17]=SYS_getppid;
#else
    registers[11]=SYS_getppid;
#endif
    CHECK(!ptrace(PTRACE_SETREGSET,pid,(void *)1,&regs));
#endif
    CHECK(!ptrace(PTRACE_SYSCALL,pid,0,0));
    CHECK(waitpid(pid,&status,0)==pid && WIFSTOPPED(status) && WSTOPSIG(status)==(SIGTRAP|128));
    CHECK(syscall(SYS_ptrace,0x420e,pid,sizeof(info),info)==33 && info[0]==2);
#if defined(__riscv) || defined(__loongarch__)
    CHECK(*(uint64_t *)(void *)(info+24)==(uint64_t)getpid());
    CHECK(!ptrace(PTRACE_GETREGSET,pid,(void *)1,&regs));
#if defined(__riscv)
    registers[10]=321;
#else
    registers[4]=321;
#endif
    CHECK(!ptrace(PTRACE_SETREGSET,pid,(void *)1,&regs));
#else
    CHECK(*(uint64_t *)(void *)(info+24)==(uint64_t)pid);
#endif
    CHECK(!ptrace(PTRACE_CONT,pid,0,(void *)(uintptr_t)SIGUSR1));
    CHECK(waitpid(pid,&status,0)==pid && WIFSTOPPED(status) && WSTOPSIG(status)==SIGUSR1);
    siginfo_t delivered;
    CHECK(!ptrace(PTRACE_GETSIGINFO,pid,0,&delivered) && delivered.si_signo==SIGUSR1 && delivered.si_code==SI_KERNEL);
    CHECK(!ptrace(PTRACE_CONT,pid,0,0));
    CHECK(waitpid(pid,&status,0)==pid && WIFSTOPPED(status) && ((unsigned)status>>16)==PTRACE_EVENT_EXIT);
    CHECK(!ptrace(PTRACE_CONT,pid,0,(void *)(uintptr_t)SIGUSR1));
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==23);
    CHECK(waitpid(pid,&status,WNOHANG)==-1 && errno==ECHILD);
    event_identity();
    final_wait_ownership();
    continue_before_listen();
    kill_before_write();
#if defined(__riscv) || defined(__loongarch__)
    syscall_original_argument();
    replaced_restart();
#endif
    tracer_death(0);
    tracer_death(PTRACE_O_EXITKILL);
    puts("PTRACE traced-stop memory syscall-info exit PASS");
    return 0;
}
