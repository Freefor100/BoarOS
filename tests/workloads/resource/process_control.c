#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/sendfile.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2,"PROCESS_CONTROL FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); exit(1); } } while (0)
static long ctl(int op, unsigned long arg) { return syscall(SYS_prctl,op,arg,0,0,0); }
static void sendint(int fd,int n) { CHECK(write(fd,&n,sizeof(n))==sizeof(n)); }
static int recvint(int fd) { int n;CHECK(read(fd,&n,sizeof(n))==sizeof(n));return n; }
static int waitsignal(const sigset_t *set,siginfo_t *si) { int n;do { n=sigwaitinfo(set,si); } while(n<0 && errno==EINTR);return n; }
static void joined(pid_t p) { int s;CHECK(waitpid(p,&s,0)==p && WIFEXITED(s) && WEXITSTATUS(s)==0); }
static int query(int op) { int n=-1;CHECK(!ctl(op,(unsigned long)&n));return n; }
static void name_is(const char *s) { char n[16];CHECK(!ctl(16,(unsigned long)n));CHECK(!strcmp(n,s)); }
static void setname(const char *s) { CHECK(!ctl(15,(unsigned long)s)); }
static void procname(pid_t tid,const char *name)
{
    char path[80],buf[64];snprintf(path,sizeof(path),"/proc/self/task/%d/comm",tid);
    int fd=open(path,O_RDWR);CHECK(fd>=0);size_t len=strlen(name);CHECK(write(fd,name,len)==(ssize_t)len);
    CHECK(lseek(fd,0,SEEK_SET)==0);ssize_t n=read(fd,buf,sizeof(buf));CHECK(n==(ssize_t)len+1 && !memcmp(buf,name,len) && buf[len]=='\n');CHECK(!close(fd));
}
/* clear_child_tid/join can precede final TID retirement. Observe the actual
 * identity removal before checking that an old proc FD cannot find it. */
static void wait_tid_gone(pid_t tid)
{
    struct timespec start, now;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &start));
    for (;;) {
        errno = 0;
        long result = syscall(SYS_tgkill, getpid(), tid, 0);
        if (result == -1 && errno == ESRCH) return;
        CHECK(result == 0 && !clock_gettime(CLOCK_MONOTONIC, &now));
        CHECK(now.tv_sec - start.tv_sec < 10);
        CHECK(!sched_yield());
    }
}
static void seq_edges(void)
{
    char buf[32];
    int fd = open("/proc/self/comm", O_RDWR);
    CHECK(fd >= 0);
    setname("vector-name");
    struct iovec iov = {buf, sizeof(buf)};
    CHECK(preadv(fd, &iov, 1, 0) == 12 && !memcmp(buf, "vector-name\n", 12));
    errno = 0; CHECK(pwritev(fd, &iov, 1, 0) == -1 && errno == ESPIPE);
    struct iovec zero = {buf, 0};
    errno = 0; CHECK(pwritev(fd, &zero, 1, 0) == -1 && errno == ESPIPE);
    setname("abcd"); CHECK(lseek(fd, 1, SEEK_SET) == 1); setname("wxyz");
    CHECK(read(fd, buf, 3) == 3 && !memcmp(buf, "bcd", 3));
    CHECK(read(fd, buf, 1) == 1 && buf[0] == '\n');
    int pipefd[2]; CHECK(!pipe(pipefd));
    errno = 0; CHECK(splice(fd, 0, pipefd[1], 0, 1, 0) == -1 && errno == EINVAL);
    CHECK(splice(fd, 0, pipefd[1], 0, 0, 0) == 0);
    errno = 0; CHECK(sendfile(pipefd[1], fd, 0, 1) == -1 && errno == EINVAL);
    CHECK(!close(pipefd[0]) && !close(pipefd[1]) && !close(fd));
    setname("proc-name");
}
static int thread_ready[2],thread_go[2];
static void *name_thread(void *unused)
{
    (void)unused;CHECK(!pthread_setname_np(pthread_self(),"thread-own"));name_is("thread-own");
    CHECK(query(2)==0);CHECK(!ctl(1,SIGUSR2));CHECK(query(37)==1);
    sendint(thread_ready[1],(int)syscall(SYS_gettid));CHECK(recvint(thread_go[0])==1);
    name_is("thread-other");CHECK(query(2)==SIGUSR2);CHECK(!ctl(36,0));return 0;
}
static void names(void)
{
    char buf[16];setname("");name_is("");setname("123456789012345");name_is("123456789012345");
    setname("1234567890123456");name_is("123456789012345");
    CHECK(!ctl(16,(unsigned long)buf));CHECK(!ctl(15,(unsigned long)buf));name_is("123456789012345");
    errno=0;CHECK(ctl(15,1)==-1 && errno==EFAULT);name_is("123456789012345");
    errno=0;CHECK(ctl(16,1)==-1 && errno==EFAULT);
    long ps=sysconf(_SC_PAGESIZE);char *p=mmap(0,(size_t)ps*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
    CHECK(!mprotect(p+ps,(size_t)ps,PROT_NONE));memcpy(p+ps-15,"abcdefghijklmno",15);CHECK(!ctl(15,(unsigned long)(p+ps-15)));name_is("abcdefghijklmno");
    CHECK(!ctl(16,(unsigned long)(p+ps-16)));CHECK(!memcmp(p+ps-16,"abcdefghijklmno",16));
    memset(p+ps-14,'x',14);errno=0;CHECK(ctl(15,(unsigned long)(p+ps-14))==-1 && errno==EFAULT);name_is("abcdefghijklmno");
    uint32_t *word=(uint32_t *)(p+ps-4);CHECK(!ctl(2,(unsigned long)word) && !*word);CHECK(!ctl(37,(unsigned long)word) && !*word);
    CHECK(!munmap(p,(size_t)ps*2));
    CHECK(!pthread_setname_np(pthread_self(),"libc-own"));CHECK(!pthread_getname_np(pthread_self(),buf,sizeof(buf)) && !strcmp(buf,"libc-own"));
    CHECK(pthread_setname_np(pthread_self(),"1234567890123456")==ERANGE);CHECK(pthread_getname_np(pthread_self(),buf,2)==ERANGE);
    procname((pid_t)syscall(SYS_gettid),"proc-name");name_is("proc-name");
    int comm=open("/proc/self/comm",O_RDWR);CHECK(comm>=0);struct iovec parts[2]={{.iov_base="left",.iov_len=4},{.iov_base="right",.iov_len=5}};CHECK(writev(comm,parts,2)==9);name_is("right");struct iovec empty={.iov_base="ignored",.iov_len=0};CHECK(writev(comm,&empty,1)==0);name_is("right");struct iovec faulty[2]={{.iov_base="prefix",.iov_len=6},{.iov_base=(void *)1,.iov_len=2}};CHECK(writev(comm,faulty,2)==6);name_is("prefix");errno=0;CHECK(pwrite(comm,"positioned",10,0)==-1 && errno==ESPIPE);CHECK(write(comm,"",0)==0);name_is("");CHECK(!close(comm));setname("proc-name");
    {
        int fd=open("/proc/self/comm",O_RDWR);CHECK(fd>=0);const char embedded[3]={'A',0,'B'};CHECK(write(fd,embedded,sizeof(embedded))==sizeof(embedded));memset(buf,0xcc,sizeof(buf));CHECK(!ctl(16,(unsigned long)buf) && buf[0]=='A');for(size_t i=1;i<sizeof(buf);i++)CHECK(buf[i]==0);
        setname("N\nB\\C");char bytes[512];CHECK(pread(fd,bytes,sizeof(bytes),0)==6 && !memcmp(bytes,"N\nB\\C\n",6));int status=open("/proc/self/status",O_RDONLY);CHECK(status>=0);ssize_t n=read(status,bytes,sizeof(bytes));const char escaped[]="Name:\tN\\nB\\\\C\n";CHECK(n>=(ssize_t)strlen(escaped) && !memcmp(bytes,escaped,strlen(escaped)));CHECK(!close(status));int statfd=open("/proc/self/stat",O_RDONLY);CHECK(statfd>=0);n=read(statfd,bytes,sizeof(bytes));const char raw[]="(N\nB\\C) ";CHECK(n>0 && memmem(bytes,(size_t)n,raw,strlen(raw)));CHECK(!close(statfd));
        setname("first-name");CHECK(pread(fd,bytes,4,0)==4 && !memcmp(bytes,"firs",4));setname("fresh-name");CHECK(pread(fd,bytes,4,0)==4 && !memcmp(bytes,"fres",4));setname("noncontiguous");CHECK(pread(fd,bytes,4,3)==4 && !memcmp(bytes,"cont",4));
        setname("buffer-old");CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(read(fd,bytes,4)==4 && !memcmp(bytes,"buff",4));setname("buffer-new");CHECK(read(fd,bytes,sizeof(bytes))==7 && !memcmp(bytes,"er-old\n",7));CHECK(pread(fd,bytes,sizeof(bytes),0)==11 && !memcmp(bytes,"buffer-new\n",11));CHECK(!close(fd));setname("proc-name");
    }
    FILE *f=fopen("/proc/self/status","r");CHECK(f);char line[512];int found=0;while(fgets(line,sizeof(line),f))if(!strcmp(line,"Name:\tproc-name\n"))found=1;CHECK(!fclose(f) && found);
    f=fopen("/proc/self/stat","r");CHECK(f && fgets(line,sizeof(line),f));CHECK(strstr(line,"(proc-name) "));CHECK(!fclose(f));
    CHECK(!ctl(36,1) && !ctl(1,SIGUSR1));CHECK(!pipe(thread_ready) && !pipe(thread_go));pthread_t t;CHECK(!pthread_create(&t,0,name_thread,0));
    pid_t tid=recvint(thread_ready[0]);CHECK(!pthread_getname_np(t,buf,sizeof(buf)) && !strcmp(buf,"thread-own"));
    char stale_path[80];snprintf(stale_path,sizeof(stale_path),"/proc/self/task/%d/comm",tid);int stale=open(stale_path,O_RDONLY);CHECK(stale>=0);procname(tid,"proc-other");CHECK(!pthread_getname_np(t,buf,sizeof(buf)) && !strcmp(buf,"proc-other"));CHECK(!pthread_setname_np(t,"thread-other"));sendint(thread_go[1],1);CHECK(!pthread_join(t,0));wait_tid_gone(tid);name_is("proc-name");CHECK(query(2)==SIGUSR1 && query(37)==0);errno=0;CHECK(read(stale,buf,sizeof(buf))==-1 && errno==ESRCH);errno=0;CHECK(lseek(stale,1,SEEK_SET)==-1 && errno==ESRCH);CHECK(lseek(stale,0,SEEK_SET)==0);CHECK(!close(stale));
    CHECK(!close(thread_ready[0]) && !close(thread_ready[1]) && !close(thread_go[0]) && !close(thread_go[1]));
    pid_t child=fork();CHECK(child>=0);if(!child){name_is("proc-name");CHECK(query(2)==0 && query(37)==0);_exit(0);}joined(child);
    char path[64];snprintf(path,sizeof(path),"/proc/%d/comm",getpid());child=fork();CHECK(child>=0);
    if(!child){int fd=open(path,O_WRONLY);CHECK(fd>=0);errno=0;CHECK(write(fd,"bad",3)==-1 && errno==EINVAL);CHECK(!close(fd));_exit(0);}joined(child);name_is("proc-name");
    char *av[]={"missing",0};errno=0;CHECK(execv("/process-control-does-not-exist",av)==-1 && errno==ENOENT);name_is("proc-name");CHECK(query(2)==SIGUSR1);CHECK(!ctl(1,0));
}
static void exec_checks(void)
{
    int fds[2];CHECK(!pipe(fds));pid_t p=fork();CHECK(p>=0);
    if(!p){CHECK(!close(fds[0]));CHECK(!ctl(1,SIGUSR1) && !ctl(36,1));setname("before-exec");char *missing[]={"missing",0};errno=0;CHECK(execv("/process-control-no-exec",missing)==-1 && errno==ENOENT);name_is("before-exec");CHECK(query(2)==SIGUSR1 && query(37)==1);char fd[24];snprintf(fd,sizeof(fd),"%d",fds[1]);char *av[]={"ignored-argv0","exec-child",fd,0};execv("/proc/self/exe",av);_exit(99);}
    CHECK(!close(fds[1]));CHECK(recvint(fds[0])==1);CHECK(!close(fds[0]));joined(p);
    CHECK(!pipe(fds));p=fork();CHECK(p>=0);if(!p){CHECK(!close(fds[0]));int executable=open("/proc/self/exe",O_RDONLY);CHECK(executable>=0);char path[512];ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);CHECK(n>0);path[n]=0;char *base=strrchr(path,'/');CHECK(base);char fd[24];snprintf(fd,sizeof(fd),"%d",fds[1]);char *av[]={"ignored-argv0","empty-exec",fd,base+1,0};char *env[]={0};syscall(SYS_execveat,executable,"",av,env,AT_EMPTY_PATH);_exit(99);}
    CHECK(!close(fds[1]));CHECK(recvint(fds[0])==1);CHECK(!close(fds[0]));joined(p);

}
static void empty_script_exec(void)
{
    char path[96], actual[512];
    snprintf(path, sizeof(path), "/tmp/pc-empty-script-%d", getpid());
    ssize_t n = readlink("/proc/self/exe", actual, sizeof(actual) - 1);
    CHECK(n > 0); actual[n] = 0;
    char *base = strrchr(actual, '/'); CHECK(base);
    int file = open(path, O_CREAT|O_EXCL|O_WRONLY, 0700);
    const char body[] = "#!/proc/self/exe script-child\n";
    CHECK(file >= 0 && write(file, body, sizeof(body)-1) == (ssize_t)sizeof(body)-1 && !close(file));
    int report[2]; CHECK(!pipe(report)); pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(!close(report[0]));
        int fd = open(path, O_RDONLY); CHECK(fd >= 0);
        char number[24]; snprintf(number, sizeof(number), "%d", report[1]);
        char *args[] = {"ignored", number, base+1, 0}; char *env[] = {0};
        syscall(SYS_execveat, fd, "", args, env, AT_EMPTY_PATH); _exit(99);
    }
    CHECK(!close(report[1]) && recvint(report[0]) == 1 && !close(report[0]));
    joined(child); CHECK(!unlink(path));
}
static void subreaper(void)
{
    CHECK(!ctl(36,1) && query(37)==1);int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));pid_t middle=fork();CHECK(middle>=0);
    if(!middle){CHECK(query(37)==0);pid_t child=fork();CHECK(child>=0);if(!child){CHECK(recvint(go[0])==1);CHECK(getppid()!=middle);CHECK(getppid()==recvint(go[0]));_exit(23);}sendint(ready[1],child);_exit(0);}
    pid_t child=recvint(ready[0]);joined(middle);sendint(go[1],1);sendint(go[1],getpid());siginfo_t si={0};CHECK(!waitid(P_PID,(id_t)child,&si,WEXITED|WNOWAIT));CHECK(si.si_pid==child && si.si_code==CLD_EXITED && si.si_status==23);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==23);errno=0;CHECK(waitpid(child,&status,WNOHANG)==-1 && errno==ECHILD);
    CHECK(!close(ready[0]) && !close(ready[1]) && !close(go[0]) && !close(go[1]));CHECK(!ctl(36,0));
}
static void pdeath(void)
{
    errno=0;CHECK(ctl(1,UINT64_C(0x100000001))==-1 && errno==EINVAL);CHECK(query(2)==0);errno=0;CHECK(ctl(2,1)==-1 && errno==EFAULT);errno=0;CHECK(ctl(37,1)==-1 && errno==EFAULT);
    CHECK(!ctl(36,1));int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));pid_t parent=fork();CHECK(parent>=0);
    if(!parent){pid_t expected_parent=getpid();pid_t child=fork();CHECK(child>=0);if(!child){sigset_t set;sigemptyset(&set);sigaddset(&set,SIGUSR1);CHECK(!sigprocmask(SIG_BLOCK,&set,0));CHECK(!ctl(1,SIGUSR1));sendint(ready[1],getpid());siginfo_t si={0};CHECK(waitsignal(&set,&si)==SIGUSR1);CHECK(si.si_pid==expected_parent);CHECK(si.si_code==SI_USER);_exit(0);}CHECK(recvint(go[0])==1);_exit(0);}
    pid_t child=recvint(ready[0]);sendint(go[1],1);joined(parent);joined(child);CHECK(!ctl(36,0));CHECK(!close(ready[0]) && !close(ready[1]) && !close(go[0]) && !close(go[1]));
}
static void nested_subreaper(int cancel)
{
    CHECK(!ctl(36,1));int ids[2],a_control[2],b_control[2],c_control[2],result[2];CHECK(!pipe(ids) && !pipe(a_control) && !pipe(b_control) && !pipe(c_control) && !pipe(result));pid_t a=fork();CHECK(a>=0);
    if(!a){CHECK(!ctl(36,1));pid_t b=fork();CHECK(b>=0);if(!b){CHECK(!ctl(36,1));pid_t c=fork();CHECK(c>=0);if(!c){CHECK(query(37)==0);CHECK(recvint(c_control[0])==1);sendint(result[1],getppid());CHECK(recvint(c_control[0])==1);sendint(result[1],getppid());_exit(0);}sendint(ids[1],c);CHECK(recvint(b_control[0])==1);_exit(0);}CHECK(recvint(a_control[0])==1);if(cancel)CHECK(!ctl(36,0));sendint(result[1],1);joined(b);sendint(result[1],2);CHECK(recvint(a_control[0])==1);_exit(0);}
    pid_t c=recvint(ids[0]);sendint(a_control[1],1);CHECK(recvint(result[0])==1);sendint(b_control[1],1);CHECK(recvint(result[0])==2);sendint(c_control[1],1);CHECK(recvint(result[0])==(cancel?getpid():a));sendint(a_control[1],1);joined(a);sendint(c_control[1],1);CHECK(recvint(result[0])==getpid());joined(c);CHECK(!ctl(36,0));
    CHECK(!close(ids[0]) && !close(ids[1]) && !close(a_control[0]) && !close(a_control[1]) && !close(b_control[0]) && !close(b_control[1]) && !close(c_control[0]) && !close(c_control[1]) && !close(result[0]) && !close(result[1]));
}
static int creator_ready[2],creator_exit[2],group_exit[2],creator_result[2];
static void *creator_thread(void *unused)
{
    (void)unused;pid_t expected=getpid();pid_t child=fork();CHECK(child>=0);
    if(!child){sigset_t set;sigemptyset(&set);sigaddset(&set,SIGUSR2);CHECK(!sigprocmask(SIG_BLOCK,&set,0));CHECK(!ctl(1,SIGUSR2));sendint(creator_ready[1],getpid());siginfo_t si={0};CHECK(waitsignal(&set,&si)==SIGUSR2);CHECK(si.si_pid==expected && si.si_uid==getuid() && si.si_code==SI_USER);CHECK(getppid()==expected);sendint(creator_result[1],1);_exit(0);}
    CHECK(recvint(creator_exit[0])==1);return 0;
}
static void parent_thread_exit(void)
{
    CHECK(!ctl(36,1));CHECK(!pipe(creator_ready) && !pipe(creator_exit) && !pipe(group_exit) && !pipe(creator_result));pid_t parent=fork();CHECK(parent>=0);
    if(!parent){pthread_t t;CHECK(!pthread_create(&t,0,creator_thread,0));CHECK(!pthread_join(t,0));CHECK(recvint(group_exit[0])==1);_exit(0);}
    pid_t child=recvint(creator_ready[0]);sendint(creator_exit[1],1);CHECK(recvint(creator_result[0])==1);int status;CHECK(waitpid(parent,&status,WNOHANG)==0);sendint(group_exit[1],1);joined(parent);joined(child);CHECK(!ctl(36,0));
    CHECK(!close(creator_ready[0]) && !close(creator_ready[1]) && !close(creator_exit[0]) && !close(creator_exit[1]) && !close(group_exit[0]) && !close(group_exit[1]) && !close(creator_result[0]) && !close(creator_result[1]));
}
static void parent_already_exited(void)
{
    CHECK(!ctl(36,1));int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));pid_t parent=fork();CHECK(parent>=0);
    if(!parent){pid_t child=fork();CHECK(child>=0);if(!child){sigset_t set,pending;sigemptyset(&set);sigaddset(&set,SIGUSR1);CHECK(!sigprocmask(SIG_BLOCK,&set,0));CHECK(recvint(go[0])==1);CHECK(!ctl(1,SIGUSR1));CHECK(!sigpending(&pending) && !sigismember(&pending,SIGUSR1));_exit(0);}sendint(ready[1],child);_exit(0);}
    pid_t child=recvint(ready[0]);joined(parent);sendint(go[1],1);joined(child);CHECK(!ctl(36,0));CHECK(!close(ready[0]) && !close(ready[1]) && !close(go[0]) && !close(go[1]));
}
static void *exec_thread(void *arg)
{
    int fd=*(int *)arg;CHECK(query(37)==1 && !ctl(1,SIGUSR1));char number[24];snprintf(number,sizeof(number),"%d",fd);char *av[]={"other-argv0","exec-child",number,0};execv("/proc/self/exe",av);_exit(99);
}
static void nonleader_exec(void)
{
    int fds[2];CHECK(!pipe(fds));pid_t p=fork();CHECK(p>=0);if(!p){CHECK(!close(fds[0]) && !ctl(36,1));pthread_t t;CHECK(!pthread_create(&t,0,exec_thread,&fds[1]));for(;;)pause();}
    CHECK(!close(fds[1]));CHECK(recvint(fds[0])==1);CHECK(!close(fds[0]));joined(p);
}
static void wait_signal_sleep(pid_t pid)
{
    struct timespec start, now; CHECK(!clock_gettime(CLOCK_MONOTONIC, &start));
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    for (;;) {
        char text[1024]; int fd = open(path, O_RDONLY); CHECK(fd >= 0);
        ssize_t size = read(fd, text, sizeof(text)-1); CHECK(size > 0 && !close(fd)); text[size] = 0;
        char *end = strrchr(text, ')'); CHECK(end && end[1] == ' ');
        if (end[2] == 'S') return;
        CHECK(!clock_gettime(CLOCK_MONOTONIC, &now) && now.tv_sec - start.tv_sec < 10);
        CHECK(!sched_yield());
    }
}
static void tracer_relationship(void)
{
    CHECK(!ctl(36,1));int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));pid_t parent=fork();CHECK(parent>=0);
    if(!parent){pid_t source=getpid();pid_t child=fork();CHECK(child>=0);if(!child){sigset_t set;sigemptyset(&set);sigaddset(&set,SIGUSR1);CHECK(!sigprocmask(SIG_BLOCK,&set,0) && !ctl(1,SIGUSR1));sendint(ready[1],getpid());siginfo_t si={0};CHECK(waitsignal(&set,&si)==SIGUSR1 && si.si_pid==source && si.si_code==SI_USER);_exit(0);}CHECK(recvint(go[0])==1);_exit(0);}
    pid_t child=recvint(ready[0]);wait_signal_sleep(child);CHECK(!ptrace(PTRACE_ATTACH,child,0,0));int status;CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP);sendint(go[1],1);joined(parent);CHECK(!ptrace(PTRACE_CONT,child,0,0));joined(child);CHECK(!ctl(36,0));CHECK(!close(ready[0]) && !close(ready[1]) && !close(go[0]) && !close(go[1]));
}
struct death_thread { int armed[2],release[2]; };
static void *repeated_death_thread(void *arg)
{
    struct death_thread *d=arg;CHECK(!ctl(1,40));sendint(d->armed[1],1);CHECK(recvint(d->release[0])==1);return 0;
}
static void repeated_thread_pdeath(void)
{
    pid_t root=getpid();CHECK(!ctl(36,1));int records[2],parent_exit[2],adopter_exit[2];CHECK(!pipe(records) && !pipe(parent_exit) && !pipe(adopter_exit));pid_t adopter=fork();CHECK(adopter>=0);
    if(!adopter){CHECK(!ctl(36,1));pid_t adopter_pid=getpid();pid_t parent=fork();CHECK(parent>=0);
        if(!parent){pid_t source=getpid();pid_t child=fork();CHECK(child>=0);
            if(!child){sigset_t set,pending;sigemptyset(&set);sigaddset(&set,40);CHECK(!sigprocmask(SIG_BLOCK,&set,0) && !ctl(1,40));struct death_thread d;CHECK(!pipe(d.armed) && !pipe(d.release));pthread_t t;CHECK(!pthread_create(&t,0,repeated_death_thread,&d));CHECK(recvint(d.armed[0])==1);sendint(records[1],getpid());
                for(int round=0;round<2;round++){for(int i=0;i<2;i++){siginfo_t si={0};CHECK(waitsignal(&set,&si)==40 && si.si_code==SI_USER && si.si_pid==(round?adopter_pid:source) && si.si_uid==getuid());}CHECK(getppid()==(round?root:adopter_pid));CHECK(!sigpending(&pending) && !sigismember(&pending,40));sendint(records[1],(round+1)*2);}
                sendint(d.release[1],1);CHECK(!pthread_join(t,0));CHECK(!close(d.armed[0]) && !close(d.armed[1]) && !close(d.release[0]) && !close(d.release[1]));_exit(0);}
            CHECK(recvint(parent_exit[0])==1);_exit(0);}
        joined(parent);CHECK(recvint(adopter_exit[0])==1);_exit(0);}
    pid_t child=recvint(records[0]);sendint(parent_exit[1],1);CHECK(recvint(records[0])==2);sendint(adopter_exit[1],1);joined(adopter);CHECK(recvint(records[0])==4);joined(child);CHECK(!ctl(36,0));CHECK(!close(records[0]) && !close(records[1]) && !close(parent_exit[0]) && !close(parent_exit[1]) && !close(adopter_exit[0]) && !close(adopter_exit[1]));
}
static void safe_exec(void)
{
    char path[96];snprintf(path,sizeof(path),"/tmp/pc-safe-%d",getpid());int in=open("/proc/self/exe",O_RDONLY),out=open(path,O_CREAT|O_EXCL|O_WRONLY,0700);CHECK(in>=0 && out>=0);char buffer[8192];ssize_t n;while((n=read(in,buffer,sizeof(buffer)))>0)CHECK(write(out,buffer,(size_t)n)==n);CHECK(!n && !close(in) && !fchown(out,65534,65534) && !fchmod(out,04755) && !close(out));
    int fds[2];CHECK(!pipe(fds));pid_t p=fork();CHECK(p>=0);if(!p){CHECK(!close(fds[0]) && !ctl(36,1) && !ctl(1,SIGUSR1));char number[24];snprintf(number,sizeof(number),"%d",fds[1]);char *av[]={"ignored","safe-exec",number,path+5,0};execv(path,av);_exit(99);}CHECK(!close(fds[1]));CHECK(recvint(fds[0])==1);CHECK(!close(fds[0]));joined(p);CHECK(!unlink(path));
}
static void credentials(void)
{
    CHECK(geteuid()==0);pid_t p=fork();CHECK(p>=0);if(!p){CHECK(!ctl(1,SIGUSR1));CHECK(!setresuid(65534,0,0));CHECK(query(2)==SIGUSR1);CHECK(!setresuid(-1,65534,-1));CHECK(query(2)==0);_exit(0);}joined(p);
    p=fork();CHECK(p>=0);if(!p){CHECK(!ctl(1,SIGUSR1));CHECK(!setresgid(65534,0,0));CHECK(query(2)==SIGUSR1);CHECK(!setresgid(-1,65534,-1));CHECK(query(2)==0);_exit(0);}joined(p);
    p=fork();CHECK(p>=0);if(!p){CHECK(!ctl(1,SIGUSR1));CHECK(syscall(SYS_setfsuid,65534)==0);CHECK(query(2)==0);_exit(0);}joined(p);
    p=fork();CHECK(p>=0);if(!p){CHECK(!ctl(1,SIGUSR1));CHECK(syscall(SYS_setfsgid,65534)==0);CHECK(query(2)==0);_exit(0);}joined(p);
    safe_exec();
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);alarm(40);
    if(argc==5 && !strcmp(argv[1],"script-child")){char expected[16];snprintf(expected,sizeof(expected),"%.15s",argv[4]);name_is(expected);sendint(atoi(argv[3]),1);return 0;}
    if(argc==4 && !strcmp(argv[1],"empty-exec")){char expected[16];snprintf(expected,sizeof(expected),"%.15s",argv[3]);name_is(expected);sendint(atoi(argv[2]),1);return 0;}
    if(argc==4 && !strcmp(argv[1],"safe-exec")){char expected[16];snprintf(expected,sizeof(expected),"%.15s",argv[3]);name_is(expected);CHECK(query(2)==0 && query(37)==1 && geteuid()==65534);sendint(atoi(argv[2]),1);return 0;}
    if(argc==3 && !strcmp(argv[1],"exec-child")){name_is("exe");CHECK(query(2)==SIGUSR1 && query(37)==1);sendint(atoi(argv[2]),1);return 0;}
    names();seq_edges();puts("PROCESS_CONTROL names ok");exec_checks();empty_script_exec();nonleader_exec();puts("PROCESS_CONTROL exec ok");subreaper();nested_subreaper(0);nested_subreaper(1);puts("PROCESS_CONTROL subreaper ok");pdeath();puts("PROCESS_CONTROL death-signal ok");parent_thread_exit();puts("PROCESS_CONTROL creator-thread ok");parent_already_exited();puts("PROCESS_CONTROL late-registration ok");tracer_relationship();puts("PROCESS_CONTROL traced-adoption ok");repeated_thread_pdeath();puts("PROCESS_CONTROL repeated-notices ok");puts("PROCESS_CONTROL parent-thread ok");if(argc!=2 || strcmp(argv[1],"unprivileged"))credentials();puts("PROCESS_CONTROL PASS");return 0;
}
