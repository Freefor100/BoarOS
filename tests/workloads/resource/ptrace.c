#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#include <unistd.h>
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
#endif
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
int main(void)
{
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
    final_wait_ownership();
    continue_before_listen();
    kill_before_write();
#if defined(__riscv) || defined(__loongarch__)
    syscall_original_argument();
#endif
    tracer_death(0);
    tracer_death(PTRACE_O_EXITKILL);
    puts("PTRACE traced-stop memory syscall-info exit PASS");
    return 0;
}
